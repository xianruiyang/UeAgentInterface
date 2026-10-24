#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationExecutionChains.h"

#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/Verification/UeAgentVerification.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	using namespace UE::UAI::BindingGateway::Operation;
	using namespace UE::UAI::BindingGateway::Query;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;

	namespace
	{
		

		FDiagnostic ChainFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& PrivateSubject = FString())
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("native_execution"), FString(), PrivateSubject);
			Error.Schema = TEXT("uai.runtime.diagnostic/1");
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FDiagnostic ChainDescriptorMismatch(const FString& Code, const FUeAgentDescriptorIdentity& Actual,
			const FUeAgentDescriptorIdentity& Expected, const FString& Path)
		{
			if (Actual.GetDescriptorId() != Expected.GetDescriptorId())
				return ChainFact(Code, TEXT("Descriptor id differs from the descriptor captured for this stage."), Path + TEXT("/descriptor_id"),
					MakeShared<FJsonValueString>(Actual.GetDescriptorId()), MakeShared<FJsonValueString>(Expected.GetDescriptorId()));
			if (Actual.GetVersion() != Expected.GetVersion())
				return ChainFact(Code, TEXT("Descriptor version differs from the version captured for this stage."), Path + TEXT("/version"),
					MakeShared<FJsonValueNumber>(Actual.GetVersion()), MakeShared<FJsonValueNumber>(Expected.GetVersion()));
			auto Error = ChainFact(Code, TEXT("Descriptor binding differs from the binding captured for this stage."), Path + TEXT("/binding_matches"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			Error.ActualEvidence = MakeShared<FJsonValueString>(Actual.GetDescriptorHash());
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.GetDescriptorHash());
			return Error;
		}

		void SupplementChainTarget(FDiagnostic& Error, const FString& Target)
		{
			if (Error.IsEmpty()) return;
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
			if (Error.SourceKind.IsEmpty() && Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
			for (auto& Child : Error.Diagnostics) SupplementChainTarget(Child, Target);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<FDiagnostic>(*Error.PublicCause);
				SupplementChainTarget(*Cause, Target);
				Error.PublicCause = MoveTemp(Cause);
			}
		}

		void SupplementChainTargets(FDiagnostic& Error, const TArray<FUeAgentOperationTargetBinding>& Targets)
		{
			if (!Targets.IsEmpty()) SupplementChainTarget(Error, Targets[0].GetBinding().GetPublicRef());
		}

		void ForwardDiagnostic(const UE::UAI::Contract::FDiagnostic& Error, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError = Error;
		}

		

		

		bool IsTypedRequestExact(
			const FUeAgentTimelineAnimationExecutionChainContract& Contract,
			const FUeAgentTimelineAnimationTypedRequestPtr& Request,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			if (!Request.IsValid())
			{
				OutError = ChainFact(TEXT("timeline_animation_typed_request_mismatch"),
					TEXT("Decoder did not return a typed timeline request."), TEXT("/typed_request"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetCapabilityId());
				return false;
			}
			if (Request->GetTypeId() != Contract.GetTypedRequestId())
			{
				OutError = ChainFact(TEXT("timeline_animation_typed_request_mismatch"),
					TEXT("Decoded timeline request has a different type from the execution-chain contract."), TEXT("/typed_request/type"),
					MakeShared<FJsonValueString>(Request->GetTypeId()), MakeShared<FJsonValueString>(Contract.GetTypedRequestId()), Contract.GetCapabilityId());
				return false;
			}
			if (Request->GetFamilyId() != Contract.GetFamilyId())
			{
				OutError = ChainFact(TEXT("timeline_animation_typed_request_mismatch"),
					TEXT("Decoded timeline request belongs to a different port family."), TEXT("/typed_request/family"),
					MakeShared<FJsonValueString>(Request->GetFamilyId()), MakeShared<FJsonValueString>(Contract.GetFamilyId()), Contract.GetCapabilityId());
				return false;
			}
			return true;
		}

		TOptional<FUeAgentCandidateBinding> FirstReadTarget(
			const TArray<FUeAgentOperationTargetBinding>& Targets)
		{
			if (Targets.IsEmpty())
			{
				return {};
			}
			return Targets[0].GetBinding();
		}

		bool ValidateChainTargets(const TArray<FUeAgentOperationTargetBinding>& Left,
			const TArray<FUeAgentOperationTargetBinding>& Right, FDiagnostic& OutError, const FString& Code)
		{
			const auto IsPreparedByC3O = [](const FUeAgentOperationTargetBinding& Target)
			{
				return Target.GetBinding().GetBindingEvidence().EndsWith(TEXT(":c3o-prepared"))
					|| Target.GetBinding().GetBindingEvidence() == TEXT("c3o-operation-preparation");
			};
			if (Left.IsEmpty())
			{
				for (int32 Index = 0; Index < Right.Num(); ++Index)
				{
					if (!IsPreparedByC3O(Right[Index]))
					{
						OutError = ChainFact(Code, TEXT("A target added after empty preflight targets lacks a C3O preparation binding."),
							TEXT("/targets/") + FString::FromInt(Index) + TEXT("/preparation_binding"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						OutError.TargetPath = Right[Index].GetBinding().GetPublicRef();
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Right[Index].GetBinding().GetBindingEvidence());
						return false;
					}
				}
				return true;
			}
			if (Left.Num() != Right.Num())
			{
				OutError = ChainFact(Code, TEXT("Target count differs from the targets captured during preflight."), TEXT("/targets/count"),
					MakeShared<FJsonValueNumber>(Right.Num()), MakeShared<FJsonValueNumber>(Left.Num()));
				return false;
			}
			for (int32 Index = 0; Index < Left.Num(); ++Index)
			{
				const auto& Expected = Left[Index].GetBinding();
				const auto& Actual = Right[Index].GetBinding();
				const FString Path = TEXT("/targets/") + FString::FromInt(Index);
				if (!IsPreparedByC3O(Right[Index]) && Left[Index].GetTicket().GetTicketId() != Right[Index].GetTicket().GetTicketId())
				{
					OutError = ChainFact(Code, TEXT("Target ticket differs from the ticket captured during preflight."), Path + TEXT("/ticket_binding_matches"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Right[Index].GetTicket().GetTicketId());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Left[Index].GetTicket().GetTicketId());
				}
				else
				{
					const bool bPrepared = IsPreparedByC3O(Right[Index]);
					const bool bDifferent = bPrepared
						? Expected.GetPublicRef() != Actual.GetPublicRef() || Expected.GetSummary().GetDescriptorIdentity() != Actual.GetSummary().GetDescriptorIdentity()
						: Expected != Actual;
					if (!bDifferent) continue;
					if (Expected.GetPublicRef() != Actual.GetPublicRef())
						OutError = ChainFact(Code, TEXT("Target reference differs from the reference captured during preflight."), Path + TEXT("/ref"),
							MakeShared<FJsonValueString>(Actual.GetPublicRef()), MakeShared<FJsonValueString>(Expected.GetPublicRef()));
					else if (Expected.GetSummary().GetDescriptorIdentity() != Actual.GetSummary().GetDescriptorIdentity())
						OutError = ChainDescriptorMismatch(Code, Actual.GetSummary().GetDescriptorIdentity(), Expected.GetSummary().GetDescriptorIdentity(), Path + TEXT("/descriptor"));
					else
					{
						OutError = ChainFact(Code, TEXT("Native candidate binding differs from the binding captured during preflight."), Path + TEXT("/binding_matches"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						if (Actual.GetBindingEvidence() != Expected.GetBindingEvidence())
						{
							OutError.ActualEvidence = MakeShared<FJsonValueString>(Actual.GetBindingEvidence());
							OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.GetBindingEvidence());
						}
					}
				}
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Actual.GetPublicRef();
				return false;
			}
			return true;
		}
	}

	FUeAgentTimelineAnimationExecutionChainContract::
	FUeAgentTimelineAnimationExecutionChainContract(
		FString InCapabilityId,
		FString InDescriptorId,
		FString InOperationKind,
		FString InC3Lane,
		FString InFamilyId,
		FString InBindingId,
		FString InTypedRequestId,
		FString InTypedResultId,
		FString InC6ChangeKind,
		FString InNativeActionId,
		FString InReadbackId,
		FString InReadbackOwner,
		FString InSettleBarrier,
		FString InOracleKind,
		const bool bInRequiresNativeMutation,
		const bool bInReadsBackAfterMutation,
		TArray<CommandContract::FUeAgentModelResultFieldDeclaration>
			InModelResultFields)
		: CapabilityId(MoveTemp(InCapabilityId))
		, DescriptorId(MoveTemp(InDescriptorId))
		, OperationKind(MoveTemp(InOperationKind))
		, C3Lane(MoveTemp(InC3Lane))
		, FamilyId(MoveTemp(InFamilyId))
		, BindingId(MoveTemp(InBindingId))
		, TypedRequestId(MoveTemp(InTypedRequestId))
		, TypedResultId(MoveTemp(InTypedResultId))
		, C6ChangeKind(MoveTemp(InC6ChangeKind))
		, NativeActionId(MoveTemp(InNativeActionId))
		, ReadbackId(MoveTemp(InReadbackId))
		, ReadbackOwner(MoveTemp(InReadbackOwner))
		, SettleBarrier(MoveTemp(InSettleBarrier))
		, OracleKind(MoveTemp(InOracleKind))
		, bRequiresNativeMutation(bInRequiresNativeMutation)
		, bReadsBackAfterMutation(bInReadsBackAfterMutation)
		, ModelResultFields(MoveTemp(InModelResultFields))
	{
	}

	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetCapabilityId() const { return CapabilityId; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetDescriptorId() const { return DescriptorId; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetOperationKind() const { return OperationKind; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetC3Lane() const { return C3Lane; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetFamilyId() const { return FamilyId; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetBindingId() const { return BindingId; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetTypedOperationId() const { return BindingId; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetTemporalContractId() const { return TemporalContractId; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetTypedRequestId() const { return TypedRequestId; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetTypedResultId() const { return TypedResultId; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetC6ChangeKind() const { return C6ChangeKind; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetNativeActionId() const { return NativeActionId; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetReadbackId() const { return ReadbackId; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetReadbackOwner() const { return ReadbackOwner; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetSettleBarrier() const { return SettleBarrier; }
	const FString& FUeAgentTimelineAnimationExecutionChainContract::GetOracleKind() const { return OracleKind; }
	bool FUeAgentTimelineAnimationExecutionChainContract::RequiresNativeMutation() const { return bRequiresNativeMutation; }
	bool FUeAgentTimelineAnimationExecutionChainContract::ReadsBackAfterMutation() const { return bReadsBackAfterMutation; }
	const TArray<CommandContract::FUeAgentModelResultFieldDeclaration>&
	FUeAgentTimelineAnimationExecutionChainContract::GetModelResultFields() const
	{
		return ModelResultFields;
	}

	const TArray<FUeAgentTimelineAnimationExecutionChainContract>&
	GetTimelineAnimationExecutionChainContracts()
	{
		static const TArray<FUeAgentTimelineAnimationExecutionChainContract> Contracts = {
			{TEXT("animation.sequence.read"), TEXT("uai.query.animation.sequence.read"), TEXT("query"), TEXT("c3q"), TEXT("temporal_read"), TEXT("timeline_animation.sequence.read"), TEXT("timeline_animation.sequence.read.request.v1"), TEXT("timeline_animation.sequence.read.result.v1"), TEXT(""), TEXT(""), TEXT("timeline_animation.sequence.readback"), TEXT("independent_timeline_animation_reader"), TEXT("none"), TEXT("independent_asset_or_sample_readback"), false, false},
			{TEXT("animation.sequence.root_motion.analyze"), TEXT("uai.query.animation.sequence.root_motion.analyze"), TEXT("query"), TEXT("c3q"), TEXT("temporal_read"), TEXT("timeline_animation.sequence.analyze_root_motion"), TEXT("timeline_animation.sequence.root_motion.request.v1"), TEXT("timeline_animation.sequence.root_motion.result.v1"), TEXT(""), TEXT(""), TEXT("timeline_animation.sequence.root_motion.readback"), TEXT("independent_timeline_animation_reader"), TEXT("none"), TEXT("independent_asset_or_sample_readback"), false, false},
			{TEXT("animation.sequence.pose.sample"), TEXT("uai.query.animation.sequence.pose.sample"), TEXT("query"), TEXT("c3q"), TEXT("temporal_read"), TEXT("timeline_animation.sequence.sample_pose"), TEXT("timeline_animation.sequence.pose_sample.request.v1"), TEXT("timeline_animation.sequence.pose_sample.result.v1"), TEXT(""), TEXT(""), TEXT("timeline_animation.sequence.pose_sample.readback"), TEXT("independent_timeline_animation_reader"), TEXT("none"), TEXT("independent_asset_or_sample_readback"), false, false},
			{TEXT("animation.blend_space.sample"), TEXT("uai.query.animation.blend_space.sample"), TEXT("query"), TEXT("c3q"), TEXT("temporal_read"), TEXT("timeline_animation.blend_space.sample"), TEXT("timeline_animation.blend_space.sample.request.v1"), TEXT("timeline_animation.blend_space.sample.result.v1"), TEXT(""), TEXT(""), TEXT("timeline_animation.blend_space.sample.readback"), TEXT("independent_timeline_animation_reader"), TEXT("none"), TEXT("independent_asset_or_sample_readback"), false, false},
			{TEXT("animation.sequence.bone_tracks.replace"), TEXT("uai.operation.animation.sequence.bone_tracks.replace"), TEXT("operation"), TEXT("c3o"), TEXT("animation_keyed_data"), TEXT("timeline_animation.sequence.replace_bone_tracks"), TEXT("timeline_animation.sequence.bone_tracks_replace.request.v1"), TEXT("timeline_animation.sequence.bone_tracks_replace.result.v1"), TEXT("timeline_animation.sequence.replace_bone_tracks"), TEXT("timeline_animation.sequence.replace_bone_tracks.mutate"), TEXT("timeline_animation.sequence.bone_tracks.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("animation_sequence_rebuild_notify_compress_settle"), TEXT("independent_tracks_curves_events_readback"), true, true},
			{TEXT("animation.sequence.curves.update"), TEXT("uai.operation.animation.sequence.curves.update"), TEXT("operation"), TEXT("c3o"), TEXT("animation_keyed_data"), TEXT("timeline_animation.sequence.update_curves"), TEXT("timeline_animation.sequence.curves_update.request.v1"), TEXT("timeline_animation.sequence.curves_update.result.v1"), TEXT("timeline_animation.sequence.update_curves"), TEXT("timeline_animation.sequence.update_curves.mutate"), TEXT("timeline_animation.sequence.curves.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("animation_sequence_rebuild_notify_compress_settle"), TEXT("independent_tracks_curves_events_readback"), true, true},
			{TEXT("animation.sequence.metadata.update"), TEXT("uai.operation.animation.sequence.metadata.update"), TEXT("operation"), TEXT("c3o"), TEXT("animation_keyed_data"), TEXT("timeline_animation.sequence.update_metadata"), TEXT("timeline_animation.sequence.metadata_update.request.v1"), TEXT("timeline_animation.sequence.metadata_update.result.v1"), TEXT("timeline_animation.sequence.update_metadata"), TEXT("timeline_animation.sequence.update_metadata.mutate"), TEXT("timeline_animation.sequence.metadata.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("animation_sequence_rebuild_notify_compress_settle"), TEXT("independent_tracks_curves_events_readback"), true, true},
			{TEXT("animation.sequence.events.update"), TEXT("uai.operation.animation.sequence.events.update"), TEXT("operation"), TEXT("c3o"), TEXT("animation_keyed_data"), TEXT("timeline_animation.sequence.update_events"), TEXT("timeline_animation.sequence.events_update.request.v1"), TEXT("timeline_animation.sequence.events_update.result.v1"), TEXT("timeline_animation.sequence.update_events"), TEXT("timeline_animation.sequence.update_events.mutate"), TEXT("timeline_animation.sequence.events.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("animation_sequence_rebuild_notify_compress_settle"), TEXT("independent_tracks_curves_events_readback"), true, true},
			{TEXT("animation.montage.read"), TEXT("uai.query.animation.montage.read"), TEXT("query"), TEXT("c3q"), TEXT("temporal_read"), TEXT("timeline_animation.montage.read"), TEXT("timeline_animation.montage.read.request.v1"), TEXT("timeline_animation.montage.read.result.v1"), TEXT(""), TEXT(""), TEXT("timeline_animation.montage.readback"), TEXT("independent_timeline_animation_reader"), TEXT("none"), TEXT("independent_asset_or_sample_readback"), false, false},
			{TEXT("animation.montage.create"), TEXT("uai.operation.animation.montage.create"), TEXT("operation"), TEXT("c3o"), TEXT("timeline_structure"), TEXT("timeline_animation.montage.create"), TEXT("timeline_animation.montage.create.request.v1"), TEXT("timeline_animation.montage.create.result.v1"), TEXT("timeline_animation.montage.create"), TEXT("timeline_animation.montage.create.mutate"), TEXT("timeline_animation.montage.structure.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("montage_refresh_and_relink"), TEXT("independent_structure_identity_and_range_readback"), true, true, {{TEXT("target_ref"), {TEXT("projection"), TEXT("payload"), TEXT("result"), TEXT("target_ref")}}}},
			{TEXT("animation.montage.structure.replace"), TEXT("uai.operation.animation.montage.structure.replace"), TEXT("operation"), TEXT("c3o"), TEXT("timeline_structure"), TEXT("timeline_animation.montage.replace_structure"), TEXT("timeline_animation.montage.structure_replace.request.v1"), TEXT("timeline_animation.montage.structure_replace.result.v1"), TEXT("timeline_animation.montage.replace_structure"), TEXT("timeline_animation.montage.replace_structure.mutate"), TEXT("timeline_animation.montage.structure.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("montage_refresh_and_relink"), TEXT("independent_structure_identity_and_range_readback"), true, true},
			{TEXT("animation.skeleton.slots.read"), TEXT("uai.query.animation.skeleton.slots.read"), TEXT("query"), TEXT("c3q"), TEXT("temporal_read"), TEXT("timeline_animation.skeleton.read_slots"), TEXT("timeline_animation.skeleton.slots_read.request.v1"), TEXT("timeline_animation.skeleton.slots_read.result.v1"), TEXT(""), TEXT(""), TEXT("timeline_animation.skeleton.slots.readback"), TEXT("independent_timeline_animation_reader"), TEXT("none"), TEXT("independent_asset_or_sample_readback"), false, false},
			{TEXT("timeline.sequence.read"), TEXT("uai.query.timeline.sequence.read"), TEXT("query"), TEXT("c3q"), TEXT("temporal_read"), TEXT("timeline_animation.level_sequence.read"), TEXT("timeline_animation.level_sequence.read.request.v1"), TEXT("timeline_animation.level_sequence.read.result.v1"), TEXT(""), TEXT(""), TEXT("timeline_animation.level_sequence.readback"), TEXT("independent_timeline_animation_reader"), TEXT("none"), TEXT("independent_asset_or_sample_readback"), false, false},
			{TEXT("timeline.sequence.create"), TEXT("uai.operation.timeline.sequence.create"), TEXT("operation"), TEXT("c3o"), TEXT("timeline_structure"), TEXT("timeline_animation.level_sequence.create"), TEXT("timeline_animation.level_sequence.create.request.v1"), TEXT("timeline_animation.level_sequence.create.result.v1"), TEXT("timeline_animation.level_sequence.create"), TEXT("timeline_animation.level_sequence.create.mutate"), TEXT("timeline_animation.level_sequence.structure.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("movie_scene_compile_and_evaluation_template_settle"), TEXT("independent_structure_identity_and_range_readback"), true, true, {{TEXT("target_ref"), {TEXT("projection"), TEXT("payload"), TEXT("result"), TEXT("target_ref")}}}},
			{TEXT("timeline.sequence.settings.update"), TEXT("uai.operation.timeline.sequence.settings.update"), TEXT("operation"), TEXT("c3o"), TEXT("timeline_structure"), TEXT("timeline_animation.level_sequence.update_settings"), TEXT("timeline_animation.level_sequence.settings_update.request.v1"), TEXT("timeline_animation.level_sequence.settings_update.result.v1"), TEXT("timeline_animation.level_sequence.update_settings"), TEXT("timeline_animation.level_sequence.update_settings.mutate"), TEXT("timeline_animation.level_sequence.structure.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("movie_scene_compile_and_evaluation_template_settle"), TEXT("independent_settings_and_structure_preservation_readback"), true, true},
			{TEXT("timeline.sequence.structure.replace"), TEXT("uai.operation.timeline.sequence.structure.replace"), TEXT("operation"), TEXT("c3o"), TEXT("timeline_structure"), TEXT("timeline_animation.level_sequence.replace_structure"), TEXT("timeline_animation.level_sequence.structure_replace.request.v1"), TEXT("timeline_animation.level_sequence.structure_replace.result.v1"), TEXT("timeline_animation.level_sequence.replace_structure"), TEXT("timeline_animation.level_sequence.replace_structure.mutate"), TEXT("timeline_animation.level_sequence.structure.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("movie_scene_compile_and_evaluation_template_settle"), TEXT("independent_structure_identity_and_range_readback"), true, true},
			{TEXT("timeline.widget_animation.read"), TEXT("uai.query.timeline.widget_animation.read"), TEXT("query"), TEXT("c3q"), TEXT("temporal_read"), TEXT("timeline_animation.widget_animation.read"), TEXT("timeline_animation.widget_animation.read.request.v1"), TEXT("timeline_animation.widget_animation.read.result.v1"), TEXT(""), TEXT(""), TEXT("timeline_animation.widget_animation.readback"), TEXT("independent_timeline_animation_reader"), TEXT("none"), TEXT("independent_asset_or_sample_readback"), false, false},
			{TEXT("animation.subject.pose.update"), TEXT("uai.operation.animation.subject.pose.update"), TEXT("operation"), TEXT("c3o"), TEXT("subject_animation_state"), TEXT("timeline_animation.subject.update_pose"), TEXT("timeline_animation.subject.pose_update.request.v1"), TEXT("timeline_animation.subject.pose_update.result.v1"), TEXT("timeline_animation.subject.update_pose"), TEXT("timeline_animation.subject.update_pose.mutate"), TEXT("timeline_animation.subject.pose.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("skeletal_component_pose_refresh"), TEXT("same_component_pose_or_morph_readback"), true, true},
			{TEXT("animation.subject.morph_weights.update"), TEXT("uai.operation.animation.subject.morph_weights.update"), TEXT("operation"), TEXT("c3o"), TEXT("subject_animation_state"), TEXT("timeline_animation.subject.update_morph_weights"), TEXT("timeline_animation.subject.morph_weights_update.request.v1"), TEXT("timeline_animation.subject.morph_weights_update.result.v1"), TEXT("timeline_animation.subject.update_morph_weights"), TEXT("timeline_animation.subject.update_morph_weights.mutate"), TEXT("timeline_animation.subject.morph_weights.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("skeletal_component_morph_refresh"), TEXT("same_component_pose_or_morph_readback"), true, true},
			{TEXT("animation.ik_probe_candidate.apply"), TEXT("uai.operation.animation.ik_probe_candidate.apply"), TEXT("operation"), TEXT("c3o"), TEXT("timeline_rig_candidate"), TEXT("timeline_animation.ik_probe.apply_candidate"), TEXT("timeline_animation.ik_probe.candidate_apply.request.v1"), TEXT("timeline_animation.ik_probe.candidate_apply.result.v1"), TEXT("timeline_animation.ik_probe.apply_candidate"), TEXT("timeline_animation.ik_probe.apply_candidate.mutate"), TEXT("timeline_animation.ik_probe.candidate.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("control_rig_sequence_evaluation_settle"), TEXT("independent_sequence_and_rig_key_readback"), true, true},
		};
		return Contracts;
	}

	FUeAgentTimelineAnimationMutationResult::FUeAgentTimelineAnimationMutationResult()
		: FUeAgentTimelineAnimationMutationResult(FString(), FString())
	{
	}

	FUeAgentTimelineAnimationMutationResult::FUeAgentTimelineAnimationMutationResult(FString InOperationId, FString InMutationEvidenceId)
		: OperationId(MoveTemp(InOperationId)), MutationEvidenceId(MoveTemp(InMutationEvidenceId))
	{
		OperationId.TrimStartAndEndInline();
		MutationEvidenceId.TrimStartAndEndInline();
		if (OperationId.IsEmpty())
			ValidationDiagnostic = ChainFact(TEXT("timeline_animation_mutation_result_invalid"),
				TEXT("The mutation result has no operation id."), TEXT("/mutation_result/operation_id_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		else if (MutationEvidenceId.IsEmpty())
			ValidationDiagnostic = ChainFact(TEXT("timeline_animation_mutation_result_invalid"),
				TEXT("The mutation result has no mutation evidence id."), TEXT("/mutation_result/evidence_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), OperationId);
	}

	const FString& FUeAgentTimelineAnimationMutationResult::GetOperationId() const { return OperationId; }
	const FString& FUeAgentTimelineAnimationMutationResult::GetMutationEvidenceId() const { return MutationEvidenceId; }
	bool FUeAgentTimelineAnimationMutationResult::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FUeAgentTimelineAnimationExecutionError& FUeAgentTimelineAnimationMutationResult::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	class FTimelineAnimationQueryProvider final : public IUeAgentDomainQueryProvider
	{
	public:
		FTimelineAnimationQueryProvider(
			FUeAgentTimelineAnimationExecutionChainContract InContract,
			TSharedRef<IUeAgentTimelineAnimationRequestDecoder> InDecoder,
			TSharedRef<IUeAgentTimelineAnimationReadbackPort> InReadback)
			: Contract(MoveTemp(InContract))
			, Decoder(MoveTemp(InDecoder))
			, Readback(MoveTemp(InReadback))
		{
		}

		bool TryQuery(
			const FUeAgentQueryDescriptor& Descriptor,
			const FUeAgentContractValue& CanonicalInput,
			const TOptional<FUeAgentCandidateBinding>& Target,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentProjectionSourceValue>& OutSource,
			FUeAgentQueryBindingError& OutError) override
		{
			OutSource.Reset();
			OutError = {};
			ON_SCOPE_EXIT { SupplementChainTarget(OutError, Target.IsSet() ? Target->GetPublicRef() : FString()); };
			if (Descriptor.GetCapabilityId() != Contract.GetCapabilityId())
			{
				OutError = ChainFact(TEXT("timeline_animation_query_binding_mismatch"),
					TEXT("Query capability differs from the execution-chain capability."), TEXT("/descriptor/capability_id"),
					MakeShared<FJsonValueString>(Descriptor.GetCapabilityId()), MakeShared<FJsonValueString>(Contract.GetCapabilityId()));
				return false;
			}
			if (Descriptor.GetIdentity().GetDescriptorId() != Contract.GetDescriptorId())
			{
				OutError = ChainFact(TEXT("timeline_animation_query_binding_mismatch"),
					TEXT("Query descriptor id differs from the execution-chain descriptor."), TEXT("/descriptor/descriptor_id"),
					MakeShared<FJsonValueString>(Descriptor.GetIdentity().GetDescriptorId()), MakeShared<FJsonValueString>(Contract.GetDescriptorId()));
				return false;
			}
			if (Descriptor.GetProviderBinding() != Contract.GetBindingId())
			{
				OutError = ChainFact(TEXT("timeline_animation_query_binding_mismatch"),
					TEXT("Query provider binding differs from the execution-chain binding."), TEXT("/descriptor/provider_binding_matches"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetBindingId());
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Descriptor.GetProviderBinding());
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Contract.GetBindingId());
				return false;
			}
			if (Context.GetRequestKind() != EUeAgentRequestKind::Query)
			{
				OutError = ChainFact(TEXT("timeline_animation_context_descriptor_mismatch"),
					TEXT("A timeline query requires a query request context."), TEXT("/context/request_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Context.GetRequestKind())),
					MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentRequestKind::Query)));
				return false;
			}
			if (Context.GetDescriptorIdentity() != Descriptor.GetIdentity())
			{
				OutError = ChainDescriptorMismatch(TEXT("timeline_animation_context_descriptor_mismatch"),
					Context.GetDescriptorIdentity(), Descriptor.GetIdentity(), TEXT("/context/descriptor"));
				return false;
			}

			FUeAgentTimelineAnimationTypedRequestPtr Request;
			FUeAgentTimelineAnimationExecutionError Error;
			if (!Decoder->TryDecode(
					Contract, CanonicalInput, Context.GetResourceTracker(), Request, Error)
				|| !IsTypedRequestExact(Contract, Request, Error))
			{
				OutError = Error.IsEmpty() ? ChainFact(TEXT("timeline_animation_request_decode_failed"),
					TEXT("The timeline request decoder failed without returning a diagnostic."), TEXT("/decoder/succeeded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetTypedRequestId()) : MoveTemp(Error);
				return false;
			}

			TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
			const bool bRead = Readback->TryRead(
				Contract, Request.ToSharedRef(), Target, EUeAgentTimelineAnimationReadPhase::Query, Context, Snapshot, Error);
			if (!bRead || !Snapshot.IsValid())
			{
				if (Error.IsEmpty())
					Error = ChainFact(TEXT("timeline_animation_query_readback_missing"),
						bRead ? TEXT("Query readback succeeded without returning a snapshot.") : TEXT("Query readback failed without returning a diagnostic."),
						bRead ? TEXT("/readback/snapshot") : TEXT("/readback/succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetReadbackId());
				ForwardDiagnostic(Error, OutError);
				return false;
			}
			OutSource = MakeShared<FUeAgentProjectionSourceValue>(Snapshot.ToSharedRef());
			return true;
		}

	private:
		FUeAgentTimelineAnimationExecutionChainContract Contract;
		TSharedRef<IUeAgentTimelineAnimationRequestDecoder> Decoder;
		TSharedRef<IUeAgentTimelineAnimationReadbackPort> Readback;
	};

	class FUeAgentTimelineAnimationOperationChain::FState final
	{
	public:
		struct FPrepared
		{
			FUeAgentTimelineAnimationTypedRequestPtr Request;
			TSharedPtr<const FUeAgentCanonicalSnapshot> Before;
			TArray<FUeAgentOperationTargetBinding> Targets;
			const FUeAgentOperationContext* Context = nullptr;
		};

		struct FExecuted
		{
			FPrepared Prepared;
			FUeAgentTimelineAnimationMutationResult Mutation;
		};

		FCriticalSection Mutex;
		TMap<FString, FPrepared> PreparedByRequest;
		TMap<FString, FExecuted> ExecutedByOperation;
	};

	namespace
	{
		class FTimelineAnimationPlanner final : public IUeAgentOperationPlanner
		{
		public:
			FTimelineAnimationPlanner(
				FUeAgentTimelineAnimationExecutionChainContract InContract,
				TSharedRef<FUeAgentTimelineAnimationOperationChain::FState, ESPMode::ThreadSafe> InState)
				: Contract(MoveTemp(InContract)), State(MoveTemp(InState))
			{
			}

			bool TryPlan(
				const FPlanningRequest& Request,
				const FUeAgentOperationContext& Context,
				FPlanningResult& OutResult,
				FUeAgentOperationBindingError& OutError) override
			{
				OutResult = {};
				OutError = {};
				ON_SCOPE_EXIT { SupplementChainTarget(OutError, FString()); };
				if (!Request.IsValid())
				{
					OutError = Request.GetValidationDiagnostic();
					return false;
				}
				if (Request.GetDraft().GetDescriptorIdentity() != Context.GetDescriptorIdentity())
				{
					OutError = ChainDescriptorMismatch(TEXT("timeline_animation_planning_context_mismatch"),
						Request.GetDraft().GetDescriptorIdentity(), Context.GetDescriptorIdentity(), TEXT("/draft/descriptor"));
					return false;
				}
				if (Context.GetDescriptorIdentity().GetDescriptorId() != Contract.GetDescriptorId())
				{
					OutError = ChainFact(TEXT("timeline_animation_planning_context_mismatch"),
						TEXT("Planning context descriptor differs from the execution-chain descriptor."), TEXT("/context/descriptor/descriptor_id"),
						MakeShared<FJsonValueString>(Context.GetDescriptorIdentity().GetDescriptorId()), MakeShared<FJsonValueString>(Contract.GetDescriptorId()));
					return false;
				}
				{
					FScopeLock Lock(&State->Mutex);
					const auto* Prepared = State->PreparedByRequest.Find(Request.GetDraft().GetRequestId());
					if (!Prepared || Prepared->Context != &Context)
					{
						OutError = ChainFact(TEXT("timeline_animation_planning_request_not_prepared"),
							Prepared ? TEXT("The prepared request belongs to a different operation context.") : TEXT("No preflight preparation exists for this planning request."),
							Prepared ? TEXT("/prepared_request/context_matches") : TEXT("/prepared_request/present"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Request.GetDraft().GetRequestId());
						return false;
					}
				}
				FPlanningPreflightError Error;
				if (!TryPlanChanges(Request, Context.GetResourceTracker(), OutResult, Error))
				{
					OutError = Error;
					return false;
				}
				return true;
			}

		private:
			FUeAgentTimelineAnimationExecutionChainContract Contract;
			TSharedRef<FUeAgentTimelineAnimationOperationChain::FState, ESPMode::ThreadSafe> State;
		};

		class FTimelineAnimationExecutor final : public IUeAgentOperationExecutor
		{
		public:
			FTimelineAnimationExecutor(
				FUeAgentTimelineAnimationExecutionChainContract InContract,
				TSharedRef<FUeAgentTimelineAnimationOperationChain::FState, ESPMode::ThreadSafe> InState,
				TSharedRef<IUeAgentTimelineAnimationMutationPort> InMutation)
				: Contract(MoveTemp(InContract))
				, State(MoveTemp(InState))
				, Mutation(MoveTemp(InMutation))
			{
			}

			bool TryExecute(
				const FPlanningResult& Planning,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FExecutionReceipt& OutReceipt,
				FUeAgentOperationBindingError& OutError) override
			{
				OutReceipt = {};
				OutError = {};
				ON_SCOPE_EXIT { SupplementChainTargets(OutError, Targets); };
				if (!Planning.IsReady())
				{
					OutError = ChainFact(TEXT("timeline_animation_execution_plan_not_ready"),
						TEXT("The planning result is not ready for execution."), TEXT("/planning/ready"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetCapabilityId());
					return false;
				}
				if (!Planning.IsMutationPermitted())
				{
					OutError = ChainFact(TEXT("timeline_animation_execution_plan_not_ready"),
						TEXT("The planning result does not permit mutation."), TEXT("/planning/mutation_permitted"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetCapabilityId());
					return false;
				}
				if (!Planning.GetPlan().IsValid())
				{
					OutError = ChainFact(TEXT("timeline_animation_execution_plan_not_ready"),
						TEXT("The planning result has no executable plan."), TEXT("/planning/plan"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetCapabilityId());
					return false;
				}
				const FString RequestId = Planning.GetPlan()->GetRequestId();
				FUeAgentTimelineAnimationOperationChain::FState::FPrepared Prepared;
				{
					FScopeLock Lock(&State->Mutex);
					auto* Found = State->PreparedByRequest.Find(RequestId);
					if (!Found || Found->Context != &Context)
					{
						OutError = ChainFact(TEXT("timeline_animation_execution_request_mismatch"),
							Found ? TEXT("The prepared request belongs to a different execution context.") : TEXT("No preflight preparation exists for this execution request."),
							Found ? TEXT("/prepared_request/context_matches") : TEXT("/prepared_request/present"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), RequestId);
						return false;
					}
					if (!ValidateChainTargets(Found->Targets, Targets, OutError, TEXT("timeline_animation_execution_request_mismatch"))) return false;
					Prepared = MoveTemp(*Found);
					State->PreparedByRequest.Remove(RequestId);
				}

				FUeAgentTimelineAnimationMutationResult MutationResult;
				FUeAgentTimelineAnimationExecutionError Error;
				const bool bMutated = Mutation->TryMutate(
					Contract, Prepared.Request.ToSharedRef(), Targets, Context, MutationResult, Error);
				if (!bMutated || !MutationResult.IsValid())
				{
					if (Error.IsEmpty())
					{
						Error = bMutated ? MutationResult.GetValidationDiagnostic() : ChainFact(TEXT("timeline_animation_mutation_result_invalid"),
							TEXT("The native mutation provider failed without returning a diagnostic."), TEXT("/mutation/succeeded"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetNativeActionId());
					}
					ForwardDiagnostic(Error, OutError);
					return false;
				}

				{
					FScopeLock Lock(&State->Mutex);
					if (State->ExecutedByOperation.Contains(MutationResult.GetOperationId()))
					{
						OutError = ChainFact(TEXT("timeline_animation_operation_id_duplicate"),
							TEXT("The mutation returned an operation id that already has an execution record."), TEXT("/executed_operation/already_registered"),
							MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), MutationResult.GetOperationId());
						return false;
					}
					State->ExecutedByOperation.Add(MutationResult.GetOperationId(), {MoveTemp(Prepared), MutationResult});
				}
				OutReceipt = FExecutionReceipt(MutationResult.GetOperationId(),
					EExecutionPhase::Terminal, EExecutionStatus::AppliedInMemory, FExecutionMutationError(),
					EExecutionCancellationMode::NotRequested, true, {}, false, {}, {TEXT("executing"), TEXT("terminal")});
				if (!OutReceipt.IsValid())
				{
					OutError = OutReceipt.GetValidationDiagnostic();
					return false;
				}
				return true;
			}

		private:
			FUeAgentTimelineAnimationExecutionChainContract Contract;
			TSharedRef<FUeAgentTimelineAnimationOperationChain::FState, ESPMode::ThreadSafe> State;
			TSharedRef<IUeAgentTimelineAnimationMutationPort> Mutation;
		};

		class FTimelineAnimationVerifier final : public IUeAgentOperationVerifier
		{
		public:
			FTimelineAnimationVerifier(
				FUeAgentTimelineAnimationExecutionChainContract InContract,
				TSharedRef<FUeAgentTimelineAnimationOperationChain::FState, ESPMode::ThreadSafe> InState,
				TSharedRef<IUeAgentTimelineAnimationReadbackPort> InReadback,
				TSharedRef<IUeAgentTimelineAnimationSettlePort> InSettle,
				TSharedRef<IUeAgentTimelineAnimationResultComparator> InComparator)
				: Contract(MoveTemp(InContract))
				, State(MoveTemp(InState))
				, Readback(MoveTemp(InReadback))
				, Settle(MoveTemp(InSettle))
				, Comparator(MoveTemp(InComparator))
			{
			}

			bool TryVerify(
				const FExecutionReceipt& Receipt,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentOperationBindingError& OutError) override
			{
				OutOutcome = {};
				OutError = {};
				ON_SCOPE_EXIT { SupplementChainTargets(OutError, Targets); };
				if (!Receipt.IsValid())
				{
					OutError = Receipt.GetValidationDiagnostic();
					return false;
				}
				if (Receipt.GetStatus() != EExecutionStatus::AppliedInMemory)
				{
					OutError = ChainFact(TEXT("timeline_animation_verification_receipt_invalid"),
						TEXT("Timeline verification requires a receipt with AppliedInMemory status."), TEXT("/receipt/status"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Receipt.GetStatus())),
						MakeShared<FJsonValueNumber>(static_cast<int32>(EExecutionStatus::AppliedInMemory)), Receipt.GetOperationId());
					return false;
				}

				FUeAgentTimelineAnimationOperationChain::FState::FExecuted Executed;
				{
					FScopeLock Lock(&State->Mutex);
					auto* Found = State->ExecutedByOperation.Find(Receipt.GetOperationId());
					if (!Found || Found->Prepared.Context != &Context)
					{
						OutError = ChainFact(TEXT("timeline_animation_verification_request_mismatch"),
							Found ? TEXT("The execution record belongs to a different verification context.") : TEXT("No execution record exists for this verification receipt."),
							Found ? TEXT("/executed_operation/context_matches") : TEXT("/executed_operation/present"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Receipt.GetOperationId());
						return false;
					}
					if (!ValidateChainTargets(Found->Prepared.Targets, Targets, OutError, TEXT("timeline_animation_verification_request_mismatch"))) return false;
					Executed = MoveTemp(*Found);
					State->ExecutedByOperation.Remove(Receipt.GetOperationId());
				}

				FUeAgentTimelineAnimationExecutionError Error;
				if (!Settle->TrySettle(Contract, Executed.Mutation, Context, Error))
				{
					OutError = Error.IsEmpty() ? ChainFact(TEXT("timeline_animation_settle_failed"),
						TEXT("The settle provider failed without returning a diagnostic."), TEXT("/settle/succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetSettleBarrier()) : MoveTemp(Error);
					return false;
				}
				TSharedPtr<const FUeAgentCanonicalSnapshot> After;
				const bool bRead = Readback->TryRead(
					Contract, Executed.Prepared.Request.ToSharedRef(), FirstReadTarget(Targets),
					EUeAgentTimelineAnimationReadPhase::Verification, Context, After, Error);
				if (!bRead || !After.IsValid())
				{
					if (Error.IsEmpty())
						Error = ChainFact(TEXT("timeline_animation_verification_readback_missing"),
							bRead ? TEXT("Verification readback succeeded without returning a snapshot.") : TEXT("Verification readback failed without returning a diagnostic."),
							bRead ? TEXT("/readback/snapshot") : TEXT("/readback/succeeded"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetReadbackId());
					ForwardDiagnostic(Error, OutError);
					return false;
				}
				if (!Comparator->TryCompare(
					Contract, Executed.Prepared.Request.ToSharedRef(), *Executed.Prepared.Before, *After, Context, OutOutcome, Error))
				{
					OutError = Error.IsEmpty() ? ChainFact(TEXT("timeline_animation_comparison_failed"),
						TEXT("The timeline comparator failed without returning a diagnostic."), TEXT("/comparison/succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetOracleKind()) : MoveTemp(Error);
					return false;
				}
				return true;
			}

		private:
			FUeAgentTimelineAnimationExecutionChainContract Contract;
			TSharedRef<FUeAgentTimelineAnimationOperationChain::FState, ESPMode::ThreadSafe> State;
			TSharedRef<IUeAgentTimelineAnimationReadbackPort> Readback;
			TSharedRef<IUeAgentTimelineAnimationSettlePort> Settle;
			TSharedRef<IUeAgentTimelineAnimationResultComparator> Comparator;
		};
	}

	FUeAgentTimelineAnimationOperationChain::FUeAgentTimelineAnimationOperationChain(
		FUeAgentTimelineAnimationExecutionChainContract InContract,
		TSharedRef<IUeAgentTimelineAnimationRequestDecoder> InDecoder,
		TSharedRef<IUeAgentTimelineAnimationReadbackPort> InReadback,
		TSharedRef<FState, ESPMode::ThreadSafe> InState,
		TSharedRef<IUeAgentOperationPlanner> InPlanner,
		TSharedRef<IUeAgentOperationExecutor> InExecutor,
		TSharedRef<IUeAgentOperationVerifier> InVerifier)
		: Contract(MoveTemp(InContract))
		, Decoder(MoveTemp(InDecoder))
		, Readback(MoveTemp(InReadback))
		, State(MoveTemp(InState))
		, Planner(MoveTemp(InPlanner))
		, Executor(MoveTemp(InExecutor))
		, Verifier(MoveTemp(InVerifier))
	{
	}

	const FUeAgentTimelineAnimationExecutionChainContract&
	FUeAgentTimelineAnimationOperationChain::GetContract() const
	{
		return Contract;
	}

	bool FUeAgentTimelineAnimationOperationChain::TryBuildPlanningRequest(
		const FUeAgentContractValue& CanonicalInput,
		const TArray<FUeAgentOperationTargetBinding>& Targets,
		const FUeAgentOperationContext& Context,
		TOptional<FPlanningRequest>& OutRequest,
		FUeAgentOperationBindingError& OutError)
	{
		OutRequest.Reset();
		OutError = {};
		ON_SCOPE_EXIT { SupplementChainTargets(OutError, Targets); };
		if (Context.GetRequestKind() != EUeAgentRequestKind::Operation)
		{
			OutError = ChainFact(TEXT("timeline_animation_context_descriptor_mismatch"),
				TEXT("Timeline operation planning requires an operation request context."), TEXT("/context/request_kind"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(Context.GetRequestKind())),
				MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentRequestKind::Operation)));
			return false;
		}
		if (Context.GetDescriptorIdentity().GetDescriptorId() != Contract.GetDescriptorId())
		{
			OutError = ChainFact(TEXT("timeline_animation_context_descriptor_mismatch"),
				TEXT("Operation context descriptor differs from the execution-chain descriptor."), TEXT("/context/descriptor/descriptor_id"),
				MakeShared<FJsonValueString>(Context.GetDescriptorIdentity().GetDescriptorId()), MakeShared<FJsonValueString>(Contract.GetDescriptorId()));
			return false;
		}

		FUeAgentTimelineAnimationTypedRequestPtr TypedRequest;
		FUeAgentTimelineAnimationExecutionError Error;
		if (!Decoder->TryDecode(
				Contract, CanonicalInput, Context.GetResourceTracker(), TypedRequest, Error)
			|| !IsTypedRequestExact(Contract, TypedRequest, Error))
		{
			OutError = Error.IsEmpty() ? ChainFact(TEXT("timeline_animation_request_decode_failed"),
				TEXT("The timeline request decoder failed without returning a diagnostic."), TEXT("/decoder/succeeded"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetTypedRequestId()) : MoveTemp(Error);
			return false;
		}

		TSharedPtr<const FUeAgentCanonicalSnapshot> Before;
		const bool bRead = Readback->TryRead(
			Contract, TypedRequest.ToSharedRef(), FirstReadTarget(Targets), EUeAgentTimelineAnimationReadPhase::Preflight, Context, Before, Error);
		if (!bRead || !Before.IsValid())
		{
			if (Error.IsEmpty())
				Error = ChainFact(TEXT("timeline_animation_preflight_readback_missing"),
					bRead ? TEXT("Preflight readback succeeded without returning a snapshot.") : TEXT("Preflight readback failed without returning a diagnostic."),
					bRead ? TEXT("/readback/snapshot") : TEXT("/readback/succeeded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetReadbackId());
			ForwardDiagnostic(Error, OutError);
			return false;
		}

		TOptional<FChangeDraft> Change;
		const bool bBuiltChange = TypedRequest->TryBuildChangeDraft(*Before, Context, Change, Error);
		if (!bBuiltChange || !Change.IsSet() || !Change->IsValid())
		{
			if (Error.IsEmpty())
			{
				if (Change.IsSet() && !Change->IsValid()) Error = Change->GetValidationDiagnostic();
				else Error = ChainFact(TEXT("timeline_animation_change_draft_invalid"),
					!Change.IsSet() ? TEXT("The typed request did not return a change draft.") : TEXT("The change-draft provider failed without returning a diagnostic."),
					!Change.IsSet() ? TEXT("/change_draft") : TEXT("/change_draft/build_succeeded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetC6ChangeKind());
			}
			ForwardDiagnostic(Error, OutError);
			return false;
		}

		FChangeSetDraft Draft(Context.GetDescriptorIdentity(), Context.GetRequestId(), Context.GetIdempotencyKey(),
			Before.ToSharedRef(), {MoveTemp(Change.GetValue())});
		OutRequest.Emplace(Draft, Context.GetCallerContext(), false);
		if (!OutRequest->IsValid())
		{
			OutError = OutRequest->GetValidationDiagnostic();
			OutRequest.Reset();
			return false;
		}

		{
			FScopeLock Lock(&State->Mutex);
			if (State->PreparedByRequest.Contains(Context.GetRequestId()))
			{
				OutRequest.Reset();
				OutError = ChainFact(TEXT("timeline_animation_request_already_prepared"),
					TEXT("A preflight preparation already exists for this request."), TEXT("/prepared_request/already_registered"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Context.GetRequestId());
				return false;
			}
			State->PreparedByRequest.Add(Context.GetRequestId(), {TypedRequest, Before, Targets, &Context});
		}
		return true;
	}

	IUeAgentOperationPlanner& FUeAgentTimelineAnimationOperationChain::GetPlanner() const { return Planner.Get(); }
	IUeAgentOperationExecutor& FUeAgentTimelineAnimationOperationChain::GetExecutor() const { return Executor.Get(); }
	IUeAgentOperationVerifier& FUeAgentTimelineAnimationOperationChain::GetVerifier() const { return Verifier.Get(); }

	bool TryCreateTimelineAnimationQueryProviderRegistry(
		TSharedRef<IUeAgentTimelineAnimationRequestDecoder> Decoder,
		TSharedRef<IUeAgentTimelineAnimationReadbackPort> Readback,
		TSharedPtr<const FUeAgentDomainQueryProviderRegistry>& OutRegistry,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		OutRegistry.Reset();
		OutError = {};
		TArray<FUeAgentDomainQueryProviderRegistration> Registrations;
		for (const FUeAgentTimelineAnimationExecutionChainContract& Contract
			: GetTimelineAnimationExecutionChainContracts())
		{
			if (Contract.GetC3Lane() != TEXT("c3q"))
			{
				continue;
			}
			Registrations.Emplace(
				Contract.GetBindingId(),
				MakeShared<FTimelineAnimationQueryProvider>(
					Contract, Decoder, Readback));
		}
		FUeAgentQueryBindingError Error;
		if (!FUeAgentDomainQueryProviderRegistry::TryCreate(
				MoveTemp(Registrations), OutRegistry, Error))
		{
			OutError = Error;
			return false;
		}
		return true;
	}

	bool TryCreateTimelineAnimationOperationChains(
		TSharedRef<IUeAgentTimelineAnimationRequestDecoder> Decoder,
		TSharedRef<IUeAgentTimelineAnimationReadbackPort> Readback,
		TSharedRef<IUeAgentTimelineAnimationMutationPort> Mutation,
		TSharedRef<IUeAgentTimelineAnimationSettlePort> Settle,
		TSharedRef<IUeAgentTimelineAnimationResultComparator> Comparator,
		TArray<TSharedRef<FUeAgentTimelineAnimationOperationChain>>& OutChains,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		OutChains.Reset();
		OutError = {};
		for (const FUeAgentTimelineAnimationExecutionChainContract& Contract
			: GetTimelineAnimationExecutionChainContracts())
		{
			if (Contract.GetC3Lane() != TEXT("c3o"))
			{
				continue;
			}
			const TSharedRef<FUeAgentTimelineAnimationOperationChain::FState, ESPMode::ThreadSafe>
				State = MakeShared<FUeAgentTimelineAnimationOperationChain::FState, ESPMode::ThreadSafe>();
			const TSharedRef<IUeAgentOperationPlanner> Planner =
				MakeShared<FTimelineAnimationPlanner>(Contract, State);
			const TSharedRef<IUeAgentOperationExecutor> Executor =
				MakeShared<FTimelineAnimationExecutor>(Contract, State, Mutation);
			const TSharedRef<IUeAgentOperationVerifier> Verifier =
				MakeShared<FTimelineAnimationVerifier>(
					Contract, State, Readback, Settle, Comparator);
			OutChains.Add(MakeShareable(
				new FUeAgentTimelineAnimationOperationChain(
					Contract,
					Decoder,
					Readback,
					State,
					Planner,
					Executor,
					Verifier)));
		}
		return true;
	}
}
