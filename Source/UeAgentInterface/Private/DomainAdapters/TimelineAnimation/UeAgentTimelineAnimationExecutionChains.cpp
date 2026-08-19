#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationExecutionChains.h"

#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/Verification/UeAgentVerification.h"
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
		void SetExecutionError(
			FUeAgentTimelineAnimationExecutionError& OutError,
			const FString& Code,
			const FString& SubjectId = FString(),
			const FString& Detail = FString())
		{
			OutError = {};
			OutError.Code = Code;
			OutError.SubjectId = SubjectId;
			OutError.Detail = Detail;
		}

		void CopyError(
			const FUeAgentTimelineAnimationExecutionError& Error,
			FUeAgentQueryBindingError& OutError)
		{
			OutError = {};
			OutError.Code = Error.Code;
			OutError.SubjectId = Error.SubjectId;
			OutError.Detail = Error.Detail;
		}

		void CopyError(
			const FUeAgentTimelineAnimationExecutionError& Error,
			FUeAgentOperationBindingError& OutError)
		{
			OutError = {};
			OutError.Code = Error.Code;
			OutError.SubjectId = Error.SubjectId;
			OutError.Detail = Error.Detail;
		}

		void SetBindingError(
			FUeAgentOperationBindingError& OutError,
			const FString& Code,
			const FString& SubjectId = FString(),
			const FString& Detail = FString())
		{
			OutError = {};
			OutError.Code = Code;
			OutError.SubjectId = SubjectId;
			OutError.Detail = Detail;
		}

		bool IsTypedRequestExact(
			const FUeAgentTimelineAnimationExecutionChainContract& Contract,
			const FUeAgentTimelineAnimationTypedRequestPtr& Request,
			FUeAgentTimelineAnimationExecutionError& OutError)
		{
			if (!Request.IsValid()
				|| Request->GetTypeId() != Contract.GetTypedRequestId()
				|| Request->GetFamilyId() != Contract.GetFamilyId())
			{
				SetExecutionError(
					OutError,
					TEXT("timeline_animation_typed_request_mismatch"),
					Contract.GetCapabilityId());
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

		bool TargetsEqual(
			const TArray<FUeAgentOperationTargetBinding>& Left,
			const TArray<FUeAgentOperationTargetBinding>& Right)
		{
			const auto IsPreparedByC3O = [](
				const FUeAgentOperationTargetBinding& Target)
			{
				return Target.GetBinding().GetBindingEvidence()
					.EndsWith(TEXT(":c3o-prepared"))
					|| Target.GetBinding().GetBindingEvidence()
						== TEXT("c3o-operation-preparation");
			};
			if (Left.IsEmpty())
			{
				if (Right.IsEmpty()) return true;
				for (const FUeAgentOperationTargetBinding& Target : Right)
				{
					if (!IsPreparedByC3O(Target)) return false;
				}
				return true;
			}
			if (Left.Num() != Right.Num())
			{
				return false;
			}
			for (int32 Index = 0; Index < Left.Num(); ++Index)
			{
				if (IsPreparedByC3O(Right[Index]))
				{
					if (Left[Index].GetBinding().GetPublicRef()
							!= Right[Index].GetBinding().GetPublicRef()
						|| Left[Index].GetBinding().GetSummary()
							.GetDescriptorIdentity()
							!= Right[Index].GetBinding().GetSummary()
								.GetDescriptorIdentity())
					{
						return false;
					}
				}
				else if (Left[Index].GetTicket().GetTicketId()
						!= Right[Index].GetTicket().GetTicketId()
					|| Left[Index].GetBinding() != Right[Index].GetBinding())
				{
					return false;
				}
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
		const bool bInReadsBackAfterMutation)
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
			{TEXT("animation.montage.create"), TEXT("uai.operation.animation.montage.create"), TEXT("operation"), TEXT("c3o"), TEXT("timeline_structure"), TEXT("timeline_animation.montage.create"), TEXT("timeline_animation.montage.create.request.v1"), TEXT("timeline_animation.montage.create.result.v1"), TEXT("timeline_animation.montage.create"), TEXT("timeline_animation.montage.create.mutate"), TEXT("timeline_animation.montage.structure.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("montage_refresh_and_relink"), TEXT("independent_structure_identity_and_range_readback"), true, true},
			{TEXT("animation.montage.structure.replace"), TEXT("uai.operation.animation.montage.structure.replace"), TEXT("operation"), TEXT("c3o"), TEXT("timeline_structure"), TEXT("timeline_animation.montage.replace_structure"), TEXT("timeline_animation.montage.structure_replace.request.v1"), TEXT("timeline_animation.montage.structure_replace.result.v1"), TEXT("timeline_animation.montage.replace_structure"), TEXT("timeline_animation.montage.replace_structure.mutate"), TEXT("timeline_animation.montage.structure.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("montage_refresh_and_relink"), TEXT("independent_structure_identity_and_range_readback"), true, true},
			{TEXT("animation.skeleton.slots.read"), TEXT("uai.query.animation.skeleton.slots.read"), TEXT("query"), TEXT("c3q"), TEXT("temporal_read"), TEXT("timeline_animation.skeleton.read_slots"), TEXT("timeline_animation.skeleton.slots_read.request.v1"), TEXT("timeline_animation.skeleton.slots_read.result.v1"), TEXT(""), TEXT(""), TEXT("timeline_animation.skeleton.slots.readback"), TEXT("independent_timeline_animation_reader"), TEXT("none"), TEXT("independent_asset_or_sample_readback"), false, false},
			{TEXT("timeline.sequence.read"), TEXT("uai.query.timeline.sequence.read"), TEXT("query"), TEXT("c3q"), TEXT("temporal_read"), TEXT("timeline_animation.level_sequence.read"), TEXT("timeline_animation.level_sequence.read.request.v1"), TEXT("timeline_animation.level_sequence.read.result.v1"), TEXT(""), TEXT(""), TEXT("timeline_animation.level_sequence.readback"), TEXT("independent_timeline_animation_reader"), TEXT("none"), TEXT("independent_asset_or_sample_readback"), false, false},
			{TEXT("timeline.sequence.create"), TEXT("uai.operation.timeline.sequence.create"), TEXT("operation"), TEXT("c3o"), TEXT("timeline_structure"), TEXT("timeline_animation.level_sequence.create"), TEXT("timeline_animation.level_sequence.create.request.v1"), TEXT("timeline_animation.level_sequence.create.result.v1"), TEXT("timeline_animation.level_sequence.create"), TEXT("timeline_animation.level_sequence.create.mutate"), TEXT("timeline_animation.level_sequence.structure.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("movie_scene_compile_and_evaluation_template_settle"), TEXT("independent_structure_identity_and_range_readback"), true, true},
			{TEXT("timeline.sequence.structure.replace"), TEXT("uai.operation.timeline.sequence.structure.replace"), TEXT("operation"), TEXT("c3o"), TEXT("timeline_structure"), TEXT("timeline_animation.level_sequence.replace_structure"), TEXT("timeline_animation.level_sequence.structure_replace.request.v1"), TEXT("timeline_animation.level_sequence.structure_replace.result.v1"), TEXT("timeline_animation.level_sequence.replace_structure"), TEXT("timeline_animation.level_sequence.replace_structure.mutate"), TEXT("timeline_animation.level_sequence.structure.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("movie_scene_compile_and_evaluation_template_settle"), TEXT("independent_structure_identity_and_range_readback"), true, true},
			{TEXT("timeline.widget_animation.read"), TEXT("uai.query.timeline.widget_animation.read"), TEXT("query"), TEXT("c3q"), TEXT("temporal_read"), TEXT("timeline_animation.widget_animation.read"), TEXT("timeline_animation.widget_animation.read.request.v1"), TEXT("timeline_animation.widget_animation.read.result.v1"), TEXT(""), TEXT(""), TEXT("timeline_animation.widget_animation.readback"), TEXT("independent_timeline_animation_reader"), TEXT("none"), TEXT("independent_asset_or_sample_readback"), false, false},
			{TEXT("animation.subject.pose.update"), TEXT("uai.operation.animation.subject.pose.update"), TEXT("operation"), TEXT("c3o"), TEXT("subject_animation_state"), TEXT("timeline_animation.subject.update_pose"), TEXT("timeline_animation.subject.pose_update.request.v1"), TEXT("timeline_animation.subject.pose_update.result.v1"), TEXT("timeline_animation.subject.update_pose"), TEXT("timeline_animation.subject.update_pose.mutate"), TEXT("timeline_animation.subject.pose.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("skeletal_component_pose_refresh"), TEXT("same_component_pose_or_morph_readback"), true, true},
			{TEXT("animation.subject.morph_weights.update"), TEXT("uai.operation.animation.subject.morph_weights.update"), TEXT("operation"), TEXT("c3o"), TEXT("subject_animation_state"), TEXT("timeline_animation.subject.update_morph_weights"), TEXT("timeline_animation.subject.morph_weights_update.request.v1"), TEXT("timeline_animation.subject.morph_weights_update.result.v1"), TEXT("timeline_animation.subject.update_morph_weights"), TEXT("timeline_animation.subject.update_morph_weights.mutate"), TEXT("timeline_animation.subject.morph_weights.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("skeletal_component_morph_refresh"), TEXT("same_component_pose_or_morph_readback"), true, true},
			{TEXT("animation.ik_probe_candidate.apply"), TEXT("uai.operation.animation.ik_probe_candidate.apply"), TEXT("operation"), TEXT("c3o"), TEXT("timeline_rig_candidate"), TEXT("timeline_animation.ik_probe.apply_candidate"), TEXT("timeline_animation.ik_probe.candidate_apply.request.v1"), TEXT("timeline_animation.ik_probe.candidate_apply.result.v1"), TEXT("timeline_animation.ik_probe.apply_candidate"), TEXT("timeline_animation.ik_probe.apply_candidate.mutate"), TEXT("timeline_animation.ik_probe.candidate.readback"), TEXT("independent_post_mutation_timeline_animation_reader"), TEXT("control_rig_sequence_evaluation_settle"), TEXT("independent_sequence_and_rig_key_readback"), true, true},
		};
		return Contracts;
	}

	FUeAgentTimelineAnimationMutationResult::FUeAgentTimelineAnimationMutationResult(
		FString InOperationId,
		FString InMutationEvidenceId)
		: OperationId(MoveTemp(InOperationId))
		, MutationEvidenceId(MoveTemp(InMutationEvidenceId))
	{
		OperationId.TrimStartAndEndInline();
		MutationEvidenceId.TrimStartAndEndInline();
		bValid = !OperationId.IsEmpty() && !MutationEvidenceId.IsEmpty();
	}

	const FString& FUeAgentTimelineAnimationMutationResult::GetOperationId() const { return OperationId; }
	const FString& FUeAgentTimelineAnimationMutationResult::GetMutationEvidenceId() const { return MutationEvidenceId; }
	bool FUeAgentTimelineAnimationMutationResult::IsValid() const { return bValid; }

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
			if (Descriptor.GetCapabilityId() != Contract.GetCapabilityId()
				|| Descriptor.GetIdentity().GetDescriptorId() != Contract.GetDescriptorId()
				|| Descriptor.GetProviderBinding() != Contract.GetBindingId())
			{
				OutError.Code = TEXT("timeline_animation_query_binding_mismatch");
				OutError.SubjectId = Contract.GetBindingId();
				return false;
			}
			if (Context.GetRequestKind() != EUeAgentRequestKind::Query
				|| Context.GetDescriptorIdentity() != Descriptor.GetIdentity())
			{
				OutError.Code = TEXT("timeline_animation_context_descriptor_mismatch");
				OutError.SubjectId = Contract.GetDescriptorId();
				return false;
			}

			FUeAgentTimelineAnimationTypedRequestPtr Request;
			FUeAgentTimelineAnimationExecutionError Error;
			if (!Decoder->TryDecode(
					Contract,
					CanonicalInput,
					Context.GetResourceTracker(),
					Request,
					Error)
				|| !IsTypedRequestExact(Contract, Request, Error))
			{
				CopyError(Error, OutError);
				return false;
			}

			TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
			if (!Readback->TryRead(
					Contract,
					Request.ToSharedRef(),
					Target,
					EUeAgentTimelineAnimationReadPhase::Query,
					Context,
					Snapshot,
					Error)
				|| !Snapshot.IsValid())
			{
				if (!Error.IsValid())
				{
					SetExecutionError(
						Error,
						TEXT("timeline_animation_query_readback_missing"),
						Contract.GetReadbackId());
				}
				CopyError(Error, OutError);
				return false;
			}
			OutSource = MakeShared<FUeAgentProjectionSourceValue>(
				Snapshot.ToSharedRef());
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
				if (!Request.IsValid()
					|| Request.GetDraft().GetDescriptorIdentity()
						!= Context.GetDescriptorIdentity()
					|| Context.GetDescriptorIdentity().GetDescriptorId()
						!= Contract.GetDescriptorId())
				{
					SetBindingError(
						OutError,
						TEXT("timeline_animation_planning_context_mismatch"),
						Contract.GetDescriptorId());
					return false;
				}
				{
					FScopeLock Lock(&State->Mutex);
					const auto* Prepared = State->PreparedByRequest.Find(
						Request.GetDraft().GetRequestId());
					if (!Prepared || Prepared->Context != &Context)
					{
						SetBindingError(
							OutError,
							TEXT("timeline_animation_planning_request_not_prepared"),
							Request.GetDraft().GetRequestId());
						return false;
					}
				}
				FPlanningPreflightError Error;
				if (!TryPlanChanges(
						Request,
						Context.GetResourceTracker(),
						OutResult,
						Error))
				{
					SetBindingError(
						OutError,
						Error.GetCode(),
						Error.GetIdentityKey(),
						Error.GetDetail());
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
				if (!Planning.IsReady() || !Planning.IsMutationPermitted()
					|| !Planning.GetPlan().IsValid())
				{
					SetBindingError(
						OutError,
						TEXT("timeline_animation_execution_plan_not_ready"),
						Contract.GetCapabilityId());
					return false;
				}
				const FString RequestId = Planning.GetPlan()->GetRequestId();
				FUeAgentTimelineAnimationOperationChain::FState::FPrepared Prepared;
				{
					FScopeLock Lock(&State->Mutex);
					auto* Found = State->PreparedByRequest.Find(RequestId);
					if (!Found || Found->Context != &Context
						|| !TargetsEqual(Found->Targets, Targets))
					{
						SetBindingError(
							OutError,
							TEXT("timeline_animation_execution_request_mismatch"),
							RequestId);
						return false;
					}
					Prepared = MoveTemp(*Found);
					State->PreparedByRequest.Remove(RequestId);
				}

				FUeAgentTimelineAnimationMutationResult MutationResult;
				FUeAgentTimelineAnimationExecutionError Error;
				if (!Mutation->TryMutate(
						Contract,
						Prepared.Request.ToSharedRef(),
						Targets,
						Context,
						MutationResult,
						Error)
					|| !MutationResult.IsValid())
				{
					if (!Error.IsValid())
					{
						SetExecutionError(
							Error,
							TEXT("timeline_animation_mutation_result_invalid"),
							Contract.GetNativeActionId());
					}
					CopyError(Error, OutError);
					return false;
				}

				{
					FScopeLock Lock(&State->Mutex);
					if (State->ExecutedByOperation.Contains(
							MutationResult.GetOperationId()))
					{
						SetBindingError(
							OutError,
							TEXT("timeline_animation_operation_id_duplicate"),
							MutationResult.GetOperationId());
						return false;
					}
					State->ExecutedByOperation.Add(
						MutationResult.GetOperationId(),
						{MoveTemp(Prepared), MutationResult});
				}
				OutReceipt = FExecutionReceipt(
					MutationResult.GetOperationId(),
					EExecutionPhase::Terminal,
					EExecutionStatus::AppliedInMemory,
					FString(),
					EExecutionCancellationMode::NotRequested,
					true,
					{},
					false,
					{},
					{TEXT("executing"), TEXT("terminal")});
				return OutReceipt.IsValid();
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
				if (!Receipt.IsValid()
					|| Receipt.GetStatus() != EExecutionStatus::AppliedInMemory)
				{
					SetBindingError(
						OutError,
						TEXT("timeline_animation_verification_receipt_invalid"),
						Receipt.GetOperationId());
					return false;
				}

				FUeAgentTimelineAnimationOperationChain::FState::FExecuted Executed;
				{
					FScopeLock Lock(&State->Mutex);
					auto* Found = State->ExecutedByOperation.Find(
						Receipt.GetOperationId());
					if (!Found || Found->Prepared.Context != &Context
						|| !TargetsEqual(Found->Prepared.Targets, Targets))
					{
						SetBindingError(
							OutError,
							TEXT("timeline_animation_verification_request_mismatch"),
							Receipt.GetOperationId());
						return false;
					}
					Executed = MoveTemp(*Found);
					State->ExecutedByOperation.Remove(Receipt.GetOperationId());
				}

				FUeAgentTimelineAnimationExecutionError Error;
				if (!Settle->TrySettle(
						Contract, Executed.Mutation, Context, Error))
				{
					CopyError(Error, OutError);
					return false;
				}
				TSharedPtr<const FUeAgentCanonicalSnapshot> After;
				if (!Readback->TryRead(
						Contract,
						Executed.Prepared.Request.ToSharedRef(),
						FirstReadTarget(Targets),
						EUeAgentTimelineAnimationReadPhase::Verification,
						Context,
						After,
						Error)
					|| !After.IsValid())
				{
					if (!Error.IsValid())
					{
						SetExecutionError(
							Error,
							TEXT("timeline_animation_verification_readback_missing"),
							Contract.GetReadbackId());
					}
					CopyError(Error, OutError);
					return false;
				}
				if (!Comparator->TryCompare(
						Contract,
						Executed.Prepared.Request.ToSharedRef(),
						*Executed.Prepared.Before,
						*After,
						Context,
						OutOutcome,
						Error))
				{
					CopyError(Error, OutError);
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
		if (Context.GetRequestKind() != EUeAgentRequestKind::Operation
			|| Context.GetDescriptorIdentity().GetDescriptorId()
				!= Contract.GetDescriptorId())
		{
			SetBindingError(
				OutError,
				TEXT("timeline_animation_context_descriptor_mismatch"),
				Contract.GetDescriptorId());
			return false;
		}

		FUeAgentTimelineAnimationTypedRequestPtr TypedRequest;
		FUeAgentTimelineAnimationExecutionError Error;
		if (!Decoder->TryDecode(
				Contract,
				CanonicalInput,
				Context.GetResourceTracker(),
				TypedRequest,
				Error)
			|| !IsTypedRequestExact(Contract, TypedRequest, Error))
		{
			CopyError(Error, OutError);
			return false;
		}

		TSharedPtr<const FUeAgentCanonicalSnapshot> Before;
		if (!Readback->TryRead(
				Contract,
				TypedRequest.ToSharedRef(),
				FirstReadTarget(Targets),
				EUeAgentTimelineAnimationReadPhase::Preflight,
				Context,
				Before,
				Error)
			|| !Before.IsValid())
		{
			if (!Error.IsValid())
			{
				SetExecutionError(
					Error,
					TEXT("timeline_animation_preflight_readback_missing"),
					Contract.GetReadbackId());
			}
			CopyError(Error, OutError);
			return false;
		}

		TOptional<FChangeDraft> Change;
		if (!TypedRequest->TryBuildChangeDraft(
				*Before, Context, Change, Error)
			|| !Change.IsSet() || !Change->IsValid())
		{
			if (!Error.IsValid())
			{
				SetExecutionError(
					Error,
					TEXT("timeline_animation_change_draft_invalid"),
					Contract.GetC6ChangeKind());
			}
			CopyError(Error, OutError);
			return false;
		}

		FChangeSetDraft Draft(
			Context.GetDescriptorIdentity(),
			Context.GetRequestId(),
			Context.GetIdempotencyKey(),
			Before.ToSharedRef(),
			{MoveTemp(Change.GetValue())});
		OutRequest.Emplace(Draft, Context.GetCallerContext(), false);
		if (!OutRequest->IsValid())
		{
			OutRequest.Reset();
			SetBindingError(
				OutError,
				TEXT("timeline_animation_planning_request_invalid"),
				Contract.GetCapabilityId());
			return false;
		}

		{
			FScopeLock Lock(&State->Mutex);
			if (State->PreparedByRequest.Contains(Context.GetRequestId()))
			{
				OutRequest.Reset();
				SetBindingError(
					OutError,
					TEXT("timeline_animation_request_already_prepared"),
					Context.GetRequestId());
				return false;
			}
			State->PreparedByRequest.Add(
				Context.GetRequestId(),
				{TypedRequest, Before, Targets, &Context});
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
		if (Registrations.Num() != 8)
		{
			SetExecutionError(
				OutError,
				TEXT("timeline_animation_query_contract_count_invalid"));
			return false;
		}
		FUeAgentQueryBindingError Error;
		if (!FUeAgentDomainQueryProviderRegistry::TryCreate(
				MoveTemp(Registrations), OutRegistry, Error))
		{
			SetExecutionError(
				OutError, Error.Code, Error.SubjectId, Error.Detail);
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
		if (OutChains.Num() != 11)
		{
			OutChains.Reset();
			SetExecutionError(
				OutError,
				TEXT("timeline_animation_operation_contract_count_invalid"));
			return false;
		}
		return true;
	}
}
