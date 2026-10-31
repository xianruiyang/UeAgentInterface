#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialExecutionChains.h"

#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/Verification/UeAgentVerification.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
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
		

		FDiagnostic NativeFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Subject = FString())
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("native_execution"), FString(), Subject);
			Error.Schema = TEXT("uai.runtime.diagnostic/1");
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FDiagnostic InternalDifference(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Subject = FString())
		{
			auto Error = NativeFact(Code, Reason, Path, MakeShared<FJsonValueBoolean>(false),
				MakeShared<FJsonValueBoolean>(true), Subject);
			Error.ActualEvidence = MoveTemp(Actual);
			Error.ExpectedEvidence = MoveTemp(Expected);
			return Error;
		}

		FDiagnostic DescriptorDifference(const FUeAgentDescriptorIdentity& Expected,
			const FUeAgentDescriptorIdentity& Actual, const FString& Code, const FString& Path, const FString& Subject)
		{
			if (Actual.GetDescriptorId() != Expected.GetDescriptorId())
				return NativeFact(Code, TEXT("The descriptor identifier differs from the required descriptor."),
					Path + TEXT("/descriptor_id"), MakeShared<FJsonValueString>(Actual.GetDescriptorId()),
					MakeShared<FJsonValueString>(Expected.GetDescriptorId()), Subject);
			if (Actual.GetVersion() != Expected.GetVersion())
				return NativeFact(Code, TEXT("The descriptor version differs from the required descriptor."),
					Path + TEXT("/version"), MakeShared<FJsonValueNumber>(Actual.GetVersion()),
					MakeShared<FJsonValueNumber>(Expected.GetVersion()), Subject);
			return InternalDifference(Code, TEXT("The descriptor fingerprint does not match the required descriptor."),
				Path + TEXT("/descriptor_hash_matches"), MakeShared<FJsonValueString>(Actual.GetDescriptorHash()),
				MakeShared<FJsonValueString>(Expected.GetDescriptorHash()), Subject);
		}

		FDiagnostic StringArrayDifference(const TArray<FString>& Expected, const TArray<FString>& Actual,
			const FString& Code, const FString& Path)
		{
			if (Actual.Num() != Expected.Num())
				return NativeFact(Code, TEXT("The target binding list length differs from its prepared value."),
					Path, MakeShared<FJsonValueNumber>(Actual.Num()), MakeShared<FJsonValueNumber>(Expected.Num()));
			for (int32 Index = 0; Index < Actual.Num(); ++Index)
				if (Actual[Index] != Expected[Index])
					return NativeFact(Code, TEXT("A target binding list entry differs from its prepared value."),
						Path + TEXT("/") + FString::FromInt(Index),
						MakeShared<FJsonValueString>(Actual[Index]), MakeShared<FJsonValueString>(Expected[Index]));
			return {};
		}

		FDiagnostic BindingDifference(const FUeAgentCandidateBinding& Expected, const FUeAgentCandidateBinding& Actual,
			const FString& Code, const FString& Path)
		{
			if (Actual.GetBindingId() != Expected.GetBindingId())
				return InternalDifference(Code, TEXT("The target binding identity does not match its prepared identity."),
					Path + TEXT("/binding_id_matches"), MakeShared<FJsonValueString>(Actual.GetBindingId()),
					MakeShared<FJsonValueString>(Expected.GetBindingId()));
			const auto& A = Actual.GetSummary();
			const auto& E = Expected.GetSummary();
			if (A != E)
			{
				const FString SummaryPath = Path + TEXT("/summary");
				if (A.GetIdentity() != E.GetIdentity())
					return InternalDifference(Code, TEXT("The candidate canonical identity differs from its prepared identity."),
						SummaryPath + TEXT("/identity_matches"), MakeShared<FJsonValueString>(A.GetIdentity().GetCanonicalKey()),
						MakeShared<FJsonValueString>(E.GetIdentity().GetCanonicalKey()));
				if (A.GetDescriptorIdentity() != E.GetDescriptorIdentity())
					return DescriptorDifference(E.GetDescriptorIdentity(), A.GetDescriptorIdentity(), Code,
						SummaryPath + TEXT("/descriptor"), FString());
				if (A.GetCatalogKind() != E.GetCatalogKind())
					return NativeFact(Code, TEXT("The candidate catalog kind differs from its prepared value."),
						SummaryPath + TEXT("/catalog_kind"), MakeShared<FJsonValueString>(A.GetCatalogKind()), MakeShared<FJsonValueString>(E.GetCatalogKind()));
				if (A.GetDisplayName() != E.GetDisplayName())
					return NativeFact(Code, TEXT("The candidate display name differs from its prepared value."),
						SummaryPath + TEXT("/display_name"), MakeShared<FJsonValueString>(A.GetDisplayName()), MakeShared<FJsonValueString>(E.GetDisplayName()));
				if (A.GetSummary() != E.GetSummary())
					return NativeFact(Code, TEXT("The candidate summary differs from its prepared value."),
						SummaryPath + TEXT("/summary"), MakeShared<FJsonValueString>(A.GetSummary()), MakeShared<FJsonValueString>(E.GetSummary()));
				if (A.GetCategoryPath() != E.GetCategoryPath())
					return StringArrayDifference(E.GetCategoryPath(), A.GetCategoryPath(), Code, SummaryPath + TEXT("/category_path"));
				return StringArrayDifference(E.GetDiscoveryScopes(), A.GetDiscoveryScopes(), Code, SummaryPath + TEXT("/discovery_scopes"));
			}
			const auto& AR = Actual.GetCatalogRevision();
			const auto& ER = Expected.GetCatalogRevision();
			if (AR != ER)
			{
				const FString Field = AR.GetSourceId() != ER.GetSourceId() ? TEXT("source_id")
					: AR.GetGeneration() != ER.GetGeneration() ? TEXT("generation") : TEXT("sequence");
				const FString AV = Field == TEXT("source_id") ? AR.GetSourceId()
					: Field == TEXT("generation") ? AR.GetGeneration() : ::LexToString(AR.GetSequence());
				const FString EV = Field == TEXT("source_id") ? ER.GetSourceId()
					: Field == TEXT("generation") ? ER.GetGeneration() : ::LexToString(ER.GetSequence());
				return InternalDifference(Code, TEXT("The target catalog revision differs from its prepared revision."),
					Path + TEXT("/catalog_revision/") + Field + TEXT("_matches"),
					MakeShared<FJsonValueString>(AV), MakeShared<FJsonValueString>(EV));
			}
			if (Actual.GetPublicRef() != Expected.GetPublicRef())
				return NativeFact(Code, TEXT("The target reference differs from its prepared reference."),
					Path + TEXT("/public_ref"), MakeShared<FJsonValueString>(Actual.GetPublicRef()), MakeShared<FJsonValueString>(Expected.GetPublicRef()));
			if (Actual.GetReadPortId() != Expected.GetReadPortId())
				return InternalDifference(Code, TEXT("The target read port differs from its prepared read port."),
					Path + TEXT("/read_port_matches"), MakeShared<FJsonValueString>(Actual.GetReadPortId()), MakeShared<FJsonValueString>(Expected.GetReadPortId()));
			const auto& AO = Actual.GetSupportedOperations();
			const auto& EO = Expected.GetSupportedOperations();
			if (AO != EO)
			{
				if (AO.Num() != EO.Num())
					return NativeFact(Code, TEXT("The supported operation count differs from the prepared binding."),
						Path + TEXT("/supported_operations"), MakeShared<FJsonValueNumber>(AO.Num()), MakeShared<FJsonValueNumber>(EO.Num()));
				for (int32 Index = 0; Index < AO.Num(); ++Index)
					if (AO[Index] != EO[Index])
						return NativeFact(Code, TEXT("A supported candidate operation differs from the prepared binding."),
							Path + TEXT("/supported_operations/") + FString::FromInt(Index),
							MakeShared<FJsonValueNumber>(static_cast<int32>(AO[Index])), MakeShared<FJsonValueNumber>(static_cast<int32>(EO[Index])));
			}
			return InternalDifference(Code, TEXT("The target binding evidence differs from its prepared evidence."),
				Path + TEXT("/binding_evidence_matches"), MakeShared<FJsonValueString>(Actual.GetBindingEvidence()),
				MakeShared<FJsonValueString>(Expected.GetBindingEvidence()));
		}


		FDiagnostic ReadbackFailure(const FString& Code, const FString& ReadbackId, bool bSucceeded, bool bSnapshotPresent)
		{
			const auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("reported_success"), bSucceeded);
			Actual->SetBoolField(TEXT("snapshot_present"), bSnapshotPresent);
			const auto Expected = MakeShared<FJsonObject>();
			Expected->SetBoolField(TEXT("reported_success"), true);
			Expected->SetBoolField(TEXT("snapshot_present"), true);
			return NativeFact(Code, bSucceeded ? TEXT("Native readback reported success without a snapshot.")
				: TEXT("Native readback failed without reporting a diagnostic."), TEXT("/readback"),
				MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), ReadbackId);
		}

		FDiagnostic NativeValidation(FDiagnostic Error)
		{
			Error.SourceKind = TEXT("native_result");
			for (auto& Child : Error.Diagnostics) Child = NativeValidation(MoveTemp(Child));
			if (Error.PublicCause) Error.PublicCause = MakeShared<FDiagnostic>(NativeValidation(*Error.PublicCause));
			return Error;
		}

		

		

		bool TryPortFamily(
			const FString& FamilyId,
			EUeAgentRigSimulationSpatialPortFamily& OutFamily)
		{
			if (FamilyId == TEXT("skeletal_attachment"))
			{
				OutFamily =
					EUeAgentRigSimulationSpatialPortFamily::SkeletalAttachment;
				return true;
			}
			if (FamilyId == TEXT("rig_bake"))
			{
				OutFamily = EUeAgentRigSimulationSpatialPortFamily::RigBake;
				return true;
			}
			if (FamilyId == TEXT("ik_retarget"))
			{
				OutFamily = EUeAgentRigSimulationSpatialPortFamily::IkRetarget;
				return true;
			}
			if (FamilyId == TEXT("simulation_definition"))
			{
				OutFamily =
					EUeAgentRigSimulationSpatialPortFamily::SimulationDefinition;
				return true;
			}
			if (FamilyId == TEXT("simulation_observation"))
			{
				OutFamily =
					EUeAgentRigSimulationSpatialPortFamily::SimulationObservation;
				return true;
			}
			return false;
		}

		bool IsTypedRequestExact(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialTypedRequestPtr& Request,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			const FString Code = TEXT("rig_simulation_spatial_typed_request_mismatch");
			if (!Request.IsValid())
			{
				OutError = NativeFact(Code, TEXT("The decoder did not provide a typed request."), TEXT("/typed_request"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetCapabilityId());
				return false;
			}
			if (Request->GetTypeId() != Contract.GetTypedRequestId())
			{
				OutError = NativeFact(Code, TEXT("The decoded request type differs from the execution chain contract."),
					TEXT("/type_id"), MakeShared<FJsonValueString>(Request->GetTypeId()),
					MakeShared<FJsonValueString>(Contract.GetTypedRequestId()), Contract.GetCapabilityId());
				return false;
			}
			if (Request->GetFamilyId() != Contract.GetFamilyId())
			{
				OutError = NativeFact(Code, TEXT("The decoded request family differs from the execution chain contract."),
					TEXT("/family_id"), MakeShared<FJsonValueString>(Request->GetFamilyId()),
					MakeShared<FJsonValueString>(Contract.GetFamilyId()), Contract.GetCapabilityId());
				return false;
			}
			EUeAgentRigSimulationSpatialPortFamily ExpectedFamily = EUeAgentRigSimulationSpatialPortFamily::SimulationObservation;
			if (!TryPortFamily(Contract.GetFamilyId(), ExpectedFamily))
			{
				OutError = NativeFact(TEXT("rig_simulation_spatial_formal_port_family_mismatch"),
					TEXT("The execution chain contract names an unsupported native port family."), TEXT("/family_id"),
					MakeShared<FJsonValueString>(Contract.GetFamilyId()),
					MakeShared<FJsonValueString>(TEXT("skeletal_attachment, rig_bake, ik_retarget, simulation_definition or simulation_observation")));
				return false;
			}
			if (Request->GetFormalPortRequest().GetFamily() != ExpectedFamily)
			{
				OutError = NativeFact(TEXT("rig_simulation_spatial_formal_port_family_mismatch"),
					TEXT("The formal request port family differs from the execution chain family."),
					TEXT("/formal_request/family"), MakeShared<FJsonValueNumber>(static_cast<int32>(Request->GetFormalPortRequest().GetFamily())),
					MakeShared<FJsonValueNumber>(static_cast<int32>(ExpectedFamily)), Contract.GetCapabilityId());
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

		bool TryMatchTargets(const TArray<FUeAgentOperationTargetBinding>& Left,
			const TArray<FUeAgentOperationTargetBinding>& Right, const FString& Code, FDiagnostic& OutError)
		{
			const auto IsPreparedByC3O = [](const FUeAgentOperationTargetBinding& Target)
			{
				return Target.GetBinding().GetBindingEvidence().EndsWith(TEXT(":c3o-prepared"))
					|| Target.GetBinding().GetBindingEvidence() == TEXT("c3o-operation-preparation");
			};
			auto Mismatch = [&OutError](FDiagnostic Error, const FString& Target)
			{
				Error.TargetPath = Target;
				OutError = MoveTemp(Error);
				return false;
			};
			if (Left.IsEmpty())
			{
				for (int32 Index = 0; Index < Right.Num(); ++Index)
					if (!IsPreparedByC3O(Right[Index]))
						return Mismatch(InternalDifference(Code,
							TEXT("An execution target was not prepared by the operation gateway."),
							TEXT("/targets/") + FString::FromInt(Index) + TEXT("/gateway_prepared"),
							MakeShared<FJsonValueString>(Right[Index].GetBinding().GetBindingEvidence()),
							MakeShared<FJsonValueString>(TEXT("c3o-prepared binding evidence"))), Right[Index].GetBinding().GetPublicRef());
				return true;
			}
			if (Left.Num() != Right.Num())
				return Mismatch(NativeFact(Code, TEXT("The execution target count differs from the prepared target count."),
					TEXT("/targets"), MakeShared<FJsonValueNumber>(Right.Num()), MakeShared<FJsonValueNumber>(Left.Num())), FString());
			for (int32 Index = 0; Index < Left.Num(); ++Index)
			{
				const FString Path = TEXT("/targets/") + FString::FromInt(Index);
				const auto& Expected = Left[Index].GetBinding();
				const auto& Actual = Right[Index].GetBinding();
				if (IsPreparedByC3O(Right[Index]))
				{
					if (Expected.GetPublicRef() != Actual.GetPublicRef())
						return Mismatch(NativeFact(Code, TEXT("The execution target reference differs from its prepared reference."),
							Path + TEXT("/public_ref"), MakeShared<FJsonValueString>(Actual.GetPublicRef()),
							MakeShared<FJsonValueString>(Expected.GetPublicRef())), Actual.GetPublicRef());
					if (Expected.GetSummary().GetDescriptorIdentity() != Actual.GetSummary().GetDescriptorIdentity())
						return Mismatch(DescriptorDifference(Expected.GetSummary().GetDescriptorIdentity(),
							Actual.GetSummary().GetDescriptorIdentity(), Code, Path + TEXT("/descriptor"), FString()), Actual.GetPublicRef());
				}
				else
				{
					if (Left[Index].GetTicket().GetTicketId() != Right[Index].GetTicket().GetTicketId())
						return Mismatch(InternalDifference(Code, TEXT("The execution ticket differs from the prepared target ticket."),
							Path + TEXT("/ticket_matches"), MakeShared<FJsonValueString>(Right[Index].GetTicket().GetTicketId()),
							MakeShared<FJsonValueString>(Left[Index].GetTicket().GetTicketId())), Actual.GetPublicRef());
					if (Expected != Actual)
						return Mismatch(BindingDifference(Expected, Actual, Code, Path + TEXT("/binding")), Actual.GetPublicRef());
				}
			}
			return true;
		}
	}

	FUeAgentRigSimulationSpatialExecutionChainContract::
	FUeAgentRigSimulationSpatialExecutionChainContract(
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

	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetCapabilityId() const { return CapabilityId; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetDescriptorId() const { return DescriptorId; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetOperationKind() const { return OperationKind; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetC3Lane() const { return C3Lane; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetFamilyId() const { return FamilyId; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetBindingId() const { return BindingId; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetTypedOperationId() const { return BindingId; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetTemporalContractId() const { return TemporalContractId; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetSpaceContractId() const { return SpaceContractId; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetTypedPortId() const { return TypedPortId; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetTypedRequestId() const { return TypedRequestId; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetTypedResultId() const { return TypedResultId; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetC6ChangeKind() const { return C6ChangeKind; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetNativeActionId() const { return NativeActionId; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetReadbackId() const { return ReadbackId; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetReadbackOwner() const { return ReadbackOwner; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetSettleBarrier() const { return SettleBarrier; }
	const FString& FUeAgentRigSimulationSpatialExecutionChainContract::GetOracleKind() const { return OracleKind; }
	bool FUeAgentRigSimulationSpatialExecutionChainContract::RequiresNativeMutation() const { return bRequiresNativeMutation; }
	bool FUeAgentRigSimulationSpatialExecutionChainContract::ReadsBackAfterMutation() const { return bReadsBackAfterMutation; }
	const TArray<CommandContract::FUeAgentModelResultFieldDeclaration>&
	FUeAgentRigSimulationSpatialExecutionChainContract::GetModelResultFields() const
	{
		return ModelResultFields;
	}

	const TArray<FUeAgentRigSimulationSpatialExecutionChainContract>&
	GetRigSimulationSpatialExecutionChainContracts()
	{
		static const TArray<FUeAgentRigSimulationSpatialExecutionChainContract> Contracts = {
			{TEXT("rig.control.bake.context"), TEXT("uai.query.rig.control.bake.context"), TEXT("query"), TEXT("c3q"), TEXT("rig_bake"), TEXT("rig_simulation_spatial.control_rig.bake_context"), TEXT("rig_simulation_spatial.control_rig.bake_context.request.v1"), TEXT("rig_simulation_spatial.control_rig.bake_context.result.v1"), TEXT(""), TEXT("rig_simulation_spatial.control_rig.bake_context.observe"), TEXT("rig_simulation_spatial.control_rig.bake_context.readback"), TEXT("independent_rig_simulation_spatial_reader"), TEXT("none"), TEXT("native_sequence_binding_and_revision"), false, false, {{TEXT("bake_context"), {TEXT("bake_context")}}}},
			{TEXT("rig.skeleton.preview_mesh.bind"), TEXT("uai.operation.rig.skeleton.preview_mesh.bind"), TEXT("operation"), TEXT("c3o"), TEXT("skeletal_attachment"), TEXT("rig_simulation_spatial.skeleton.bind_preview_mesh"), TEXT("rig_simulation_spatial.skeleton.bind_preview_mesh.request.v1"), TEXT("rig_simulation_spatial.skeleton.bind_preview_mesh.result.v1"), TEXT("rig_simulation_spatial.skeleton.bind_preview_mesh"), TEXT("rig_simulation_spatial.skeleton.bind_preview_mesh.mutate"), TEXT("rig_simulation_spatial.skeleton.bind_preview_mesh.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("skeleton_post_edit_settle"), TEXT("independent_skeleton_socket_and_parent_bone_readback"), true, true},
			{TEXT("rig.skeleton.socket.update"), TEXT("uai.operation.rig.skeleton.socket.update"), TEXT("operation"), TEXT("c3o"), TEXT("skeletal_attachment"), TEXT("rig_simulation_spatial.skeleton.update_socket"), TEXT("rig_simulation_spatial.skeleton.update_socket.request.v1"), TEXT("rig_simulation_spatial.skeleton.update_socket.result.v1"), TEXT("rig_simulation_spatial.skeleton.update_socket"), TEXT("rig_simulation_spatial.skeleton.update_socket.mutate"), TEXT("rig_simulation_spatial.skeleton.update_socket.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("skeleton_post_edit_settle"), TEXT("independent_skeleton_socket_and_parent_bone_readback"), true, true},
			{TEXT("rig.control.bake.animation"), TEXT("uai.operation.rig.control.bake.animation"), TEXT("operation"), TEXT("c3o"), TEXT("rig_bake"), TEXT("rig_simulation_spatial.control_rig.bake_animation"), TEXT("rig_simulation_spatial.control_rig.bake_animation.request.v1"), TEXT("rig_simulation_spatial.control_rig.bake_animation.result.v1"), TEXT("rig_simulation_spatial.control_rig.bake_animation"), TEXT("rig_simulation_spatial.control_rig.bake_animation.mutate"), TEXT("rig_simulation_spatial.control_rig.bake_animation.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("rig_bake_evaluation_and_asset_settle"), TEXT("independent_target_animation_or_control_key_readback"), true, true},
			{TEXT("rig.control.bake.controls"), TEXT("uai.operation.rig.control.bake.controls"), TEXT("operation"), TEXT("c3o"), TEXT("rig_bake"), TEXT("rig_simulation_spatial.control_rig.bake_controls"), TEXT("rig_simulation_spatial.control_rig.bake_controls.request.v1"), TEXT("rig_simulation_spatial.control_rig.bake_controls.result.v1"), TEXT("rig_simulation_spatial.control_rig.bake_controls"), TEXT("rig_simulation_spatial.control_rig.bake_controls.mutate"), TEXT("rig_simulation_spatial.control_rig.bake_controls.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("rig_bake_evaluation_and_asset_settle"), TEXT("independent_target_animation_or_control_key_readback"), true, true},
			{TEXT("rig.ik.definition.update"), TEXT("uai.operation.rig.ik.definition.update"), TEXT("operation"), TEXT("c3o"), TEXT("ik_retarget"), TEXT("rig_simulation_spatial.ik.update_definition"), TEXT("rig_simulation_spatial.ik.update_definition.request.v1"), TEXT("rig_simulation_spatial.ik.update_definition.result.v1"), TEXT("rig_simulation_spatial.ik.update_definition"), TEXT("rig_simulation_spatial.ik.update_definition.mutate"), TEXT("rig_simulation_spatial.ik.update_definition.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("ik_retarget_compile_and_asset_settle"), TEXT("independent_source_target_definition_pose_and_asset_readback"), true, true},
			{TEXT("rig.retarget.definition.update"), TEXT("uai.operation.rig.retarget.definition.update"), TEXT("operation"), TEXT("c3o"), TEXT("ik_retarget"), TEXT("rig_simulation_spatial.retarget.update_definition"), TEXT("rig_simulation_spatial.retarget.update_definition.request.v1"), TEXT("rig_simulation_spatial.retarget.update_definition.result.v1"), TEXT("rig_simulation_spatial.retarget.update_definition"), TEXT("rig_simulation_spatial.retarget.update_definition.mutate"), TEXT("rig_simulation_spatial.retarget.update_definition.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("ik_retarget_compile_and_asset_settle"), TEXT("independent_source_target_definition_pose_and_asset_readback"), true, true},
			{TEXT("rig.retarget.pose.update"), TEXT("uai.operation.rig.retarget.pose.update"), TEXT("operation"), TEXT("c3o"), TEXT("ik_retarget"), TEXT("rig_simulation_spatial.retarget.update_pose"), TEXT("rig_simulation_spatial.retarget.update_pose.request.v1"), TEXT("rig_simulation_spatial.retarget.update_pose.result.v1"), TEXT("rig_simulation_spatial.retarget.update_pose"), TEXT("rig_simulation_spatial.retarget.update_pose.mutate"), TEXT("rig_simulation_spatial.retarget.update_pose.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("ik_retarget_compile_and_asset_settle"), TEXT("independent_source_target_definition_pose_and_asset_readback"), true, true},
			{TEXT("rig.retarget.pose.auto_align"), TEXT("uai.operation.rig.retarget.pose.auto_align"), TEXT("operation"), TEXT("c3o"), TEXT("ik_retarget"), TEXT("rig_simulation_spatial.retarget.auto_align_pose"), TEXT("rig_simulation_spatial.retarget.auto_align_pose.request.v1"), TEXT("rig_simulation_spatial.retarget.auto_align_pose.result.v1"), TEXT("rig_simulation_spatial.retarget.auto_align_pose"), TEXT("rig_simulation_spatial.retarget.auto_align_pose.mutate"), TEXT("rig_simulation_spatial.retarget.auto_align_pose.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("ik_retarget_compile_and_asset_settle"), TEXT("independent_source_target_definition_pose_and_asset_readback"), true, true},
			{TEXT("rig.retarget.assets.generate"), TEXT("uai.operation.rig.retarget.assets.generate"), TEXT("operation"), TEXT("c3o"), TEXT("ik_retarget"), TEXT("rig_simulation_spatial.retarget.generate_assets"), TEXT("rig_simulation_spatial.retarget.generate_assets.request.v1"), TEXT("rig_simulation_spatial.retarget.generate_assets.result.v1"), TEXT("rig_simulation_spatial.retarget.generate_assets"), TEXT("rig_simulation_spatial.retarget.generate_assets.mutate"), TEXT("rig_simulation_spatial.retarget.generate_assets.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("ik_retarget_compile_and_asset_settle"), TEXT("independent_source_target_definition_pose_and_asset_readback"), true, true, {{TEXT("generated_asset_refs"), {TEXT("projection"), TEXT("payload"), TEXT("result"), TEXT("generated_asset_refs")}}}},
			{TEXT("physics.material.update"), TEXT("uai.operation.physics.material.update"), TEXT("operation"), TEXT("c3o"), TEXT("simulation_definition"), TEXT("rig_simulation_spatial.physics.update_material"), TEXT("rig_simulation_spatial.physics.update_material.request.v1"), TEXT("rig_simulation_spatial.physics.update_material.result.v1"), TEXT("rig_simulation_spatial.physics.update_material"), TEXT("rig_simulation_spatial.physics.update_material.mutate"), TEXT("rig_simulation_spatial.physics.update_material.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("physics_asset_rebuild_and_constraint_settle"), TEXT("independent_physics_asset_or_constraint_definition_readback"), true, true},
			{TEXT("physics.asset.structure.replace"), TEXT("uai.operation.physics.asset.structure.replace"), TEXT("operation"), TEXT("c3o"), TEXT("simulation_definition"), TEXT("rig_simulation_spatial.physics.replace_asset_structure"), TEXT("rig_simulation_spatial.physics.replace_asset_structure.request.v1"), TEXT("rig_simulation_spatial.physics.replace_asset_structure.result.v1"), TEXT("rig_simulation_spatial.physics.replace_asset_structure"), TEXT("rig_simulation_spatial.physics.replace_asset_structure.mutate"), TEXT("rig_simulation_spatial.physics.replace_asset_structure.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("physics_asset_rebuild_and_constraint_settle"), TEXT("independent_physics_asset_or_constraint_definition_readback"), true, true},
			{TEXT("physics.constraint.update"), TEXT("uai.operation.physics.constraint.update"), TEXT("operation"), TEXT("c3o"), TEXT("simulation_definition"), TEXT("rig_simulation_spatial.physics.update_constraint"), TEXT("rig_simulation_spatial.physics.update_constraint.request.v1"), TEXT("rig_simulation_spatial.physics.update_constraint.result.v1"), TEXT("rig_simulation_spatial.physics.update_constraint"), TEXT("rig_simulation_spatial.physics.update_constraint.mutate"), TEXT("rig_simulation_spatial.physics.update_constraint.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("physics_asset_rebuild_and_constraint_settle"), TEXT("independent_physics_asset_or_constraint_definition_readback"), true, true},
			{TEXT("physics.simulation.probe"), TEXT("uai.operation.physics.simulation.probe"), TEXT("operation"), TEXT("c3o"), TEXT("simulation_observation"), TEXT("rig_simulation_spatial.physics.probe_simulation"), TEXT("rig_simulation_spatial.physics.probe_simulation.request.v1"), TEXT("rig_simulation_spatial.physics.probe_simulation.result.v1"), TEXT("rig_simulation_spatial.physics.probe_simulation"), TEXT("rig_simulation_spatial.physics.probe_simulation.mutate"), TEXT("rig_simulation_spatial.physics.probe_simulation.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("owned_simulation_session_settle"), TEXT("independent_component_asset_or_generated_data_readback"), true, true},
			{TEXT("deformer.geometry.sample"), TEXT("uai.query.deformer.geometry.sample"), TEXT("query"), TEXT("c3q"), TEXT("simulation_observation"), TEXT("rig_simulation_spatial.deformer.sample_geometry"), TEXT("rig_simulation_spatial.deformer.sample_geometry.request.v1"), TEXT("rig_simulation_spatial.deformer.sample_geometry.result.v1"), TEXT(""), TEXT("rig_simulation_spatial.deformer.sample_geometry.observe"), TEXT("rig_simulation_spatial.deformer.sample_geometry.readback"), TEXT("independent_rig_simulation_spatial_reader"), TEXT("none"), TEXT("independent_component_asset_or_generated_data_readback"), false, false},
			{TEXT("deformer.training_inputs.validate"), TEXT("uai.query.deformer.training_inputs.validate"), TEXT("query"), TEXT("c3q"), TEXT("simulation_observation"), TEXT("rig_simulation_spatial.deformer.validate_training_inputs"), TEXT("rig_simulation_spatial.deformer.validate_training_inputs.request.v1"), TEXT("rig_simulation_spatial.deformer.validate_training_inputs.result.v1"), TEXT(""), TEXT("rig_simulation_spatial.deformer.validate_training_inputs.observe"), TEXT("rig_simulation_spatial.deformer.validate_training_inputs.readback"), TEXT("independent_rig_simulation_spatial_reader"), TEXT("none"), TEXT("independent_component_asset_or_generated_data_readback"), false, false},
			{TEXT("deformer.evaluation.sample"), TEXT("uai.query.deformer.evaluation.sample"), TEXT("query"), TEXT("c3q"), TEXT("simulation_observation"), TEXT("rig_simulation_spatial.deformer.sample_evaluation"), TEXT("rig_simulation_spatial.deformer.sample_evaluation.request.v1"), TEXT("rig_simulation_spatial.deformer.sample_evaluation.result.v1"), TEXT(""), TEXT("rig_simulation_spatial.deformer.sample_evaluation.observe"), TEXT("rig_simulation_spatial.deformer.sample_evaluation.readback"), TEXT("independent_rig_simulation_spatial_reader"), TEXT("none"), TEXT("independent_component_asset_or_generated_data_readback"), false, false},
		};
		return Contracts;
	}

	FUeAgentRigSimulationSpatialMutationResult::FUeAgentRigSimulationSpatialMutationResult(
		FString InOperationId,
		FString InMutationEvidenceId)
		: OperationId(MoveTemp(InOperationId))
		, MutationEvidenceId(MoveTemp(InMutationEvidenceId))
	{
		OperationId.TrimStartAndEndInline();
		MutationEvidenceId.TrimStartAndEndInline();
		bValid = !OperationId.IsEmpty() && !MutationEvidenceId.IsEmpty();
	}

	const FString& FUeAgentRigSimulationSpatialMutationResult::GetOperationId() const { return OperationId; }
	const FString& FUeAgentRigSimulationSpatialMutationResult::GetMutationEvidenceId() const { return MutationEvidenceId; }
	bool FUeAgentRigSimulationSpatialMutationResult::IsValid() const { return bValid; }

	class FRigSimulationSpatialQueryProvider final : public IUeAgentDomainQueryProvider
	{
	public:
		FRigSimulationSpatialQueryProvider(
			FUeAgentRigSimulationSpatialExecutionChainContract InContract,
			TSharedRef<IUeAgentRigSimulationSpatialRequestDecoder> InDecoder,
			TSharedRef<IUeAgentRigSimulationSpatialReadbackPort> InReadback)
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
			const FString BindingField = Descriptor.GetCapabilityId() != Contract.GetCapabilityId() ? TEXT("capability_id")
				: Descriptor.GetIdentity().GetDescriptorId() != Contract.GetDescriptorId() ? TEXT("descriptor_id")
				: Descriptor.GetProviderBinding() != Contract.GetBindingId() ? TEXT("provider_binding") : FString();
			if (!BindingField.IsEmpty())
			{
				const FString Actual = BindingField == TEXT("capability_id") ? Descriptor.GetCapabilityId()
					: BindingField == TEXT("descriptor_id") ? Descriptor.GetIdentity().GetDescriptorId() : Descriptor.GetProviderBinding();
				const FString Expected = BindingField == TEXT("capability_id") ? Contract.GetCapabilityId()
					: BindingField == TEXT("descriptor_id") ? Contract.GetDescriptorId() : Contract.GetBindingId();
				OutError = NativeFact(TEXT("rig_simulation_spatial_query_binding_mismatch"),
					TEXT("The query descriptor does not match the registered execution chain."),
					TEXT("/") + BindingField, MakeShared<FJsonValueString>(Actual), MakeShared<FJsonValueString>(Expected), Contract.GetBindingId());
				return false;
			}
			if (Context.GetRequestKind() != EUeAgentRequestKind::Query)
			{
				OutError = NativeFact(TEXT("rig_simulation_spatial_context_descriptor_mismatch"),
					TEXT("The query provider requires a query operation context."), TEXT("/request_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Context.GetRequestKind())),
					MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentRequestKind::Query)), Contract.GetDescriptorId());
				return false;
			}
			if (Context.GetDescriptorIdentity() != Descriptor.GetIdentity())
			{
				OutError = DescriptorDifference(Descriptor.GetIdentity(), Context.GetDescriptorIdentity(),
					TEXT("rig_simulation_spatial_context_descriptor_mismatch"), TEXT("/descriptor"), Contract.GetDescriptorId());
				return false;
			}

			FUeAgentRigSimulationSpatialTypedRequestPtr Request;
			FUeAgentRigSimulationSpatialExecutionError Error;
			if (!Decoder->TryDecode(
					Contract,
					CanonicalInput,
					Context.GetResourceTracker(),
					Request,
					Error)
				|| !IsTypedRequestExact(Contract, Request, Error))
			{
				OutError = Error;
				return false;
			}

			TOptional<FUeAgentRigSimulationSpatialReadbackResult> ReadbackResult;
			const bool bReadbackSucceeded = Readback->TryRead(
					Contract,
					Request.ToSharedRef(),
					Target,
					EUeAgentRigSimulationSpatialReadPhase::Query,
					Context,
					ReadbackResult,
					Error);
			if (!bReadbackSucceeded || !ReadbackResult.IsSet())
			{
				if (!Error.IsValid())
				{
					Error = ReadbackFailure(TEXT("rig_simulation_spatial_query_readback_missing"), Contract.GetReadbackId(), bReadbackSucceeded, ReadbackResult.IsSet());
				}
				OutError = Error;
				return false;
			}
			OutSource = MakeShared<FUeAgentProjectionSourceValue>(
				ReadbackResult->Snapshot);
			return true;
		}

	private:
		FUeAgentRigSimulationSpatialExecutionChainContract Contract;
		TSharedRef<IUeAgentRigSimulationSpatialRequestDecoder> Decoder;
		TSharedRef<IUeAgentRigSimulationSpatialReadbackPort> Readback;
	};

	class FUeAgentRigSimulationSpatialOperationChain::FState final
	{
	public:
		struct FPrepared
		{
			FUeAgentRigSimulationSpatialTypedRequestPtr Request;
			TSharedPtr<const FUeAgentCanonicalSnapshot> Before;
			TArray<FUeAgentOperationTargetBinding> Targets;
			const FUeAgentOperationContext* Context = nullptr;
		};

		struct FExecuted
		{
			FPrepared Prepared;
			FUeAgentRigSimulationSpatialMutationResult Mutation;
		};

		FCriticalSection Mutex;
		TMap<FString, FPrepared> PreparedByRequest;
		TMap<FString, FExecuted> ExecutedByOperation;
	};

	namespace
	{
		class FRigSimulationSpatialPlanner final : public IUeAgentOperationPlanner
		{
		public:
			FRigSimulationSpatialPlanner(
				FUeAgentRigSimulationSpatialExecutionChainContract InContract,
				TSharedRef<FUeAgentRigSimulationSpatialOperationChain::FState, ESPMode::ThreadSafe> InState)
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
				if (!Request.IsValid())
				{
					OutError = NativeValidation(Request.GetValidationDiagnostic());
					return false;
				}
				if (Request.GetDraft().GetDescriptorIdentity() != Context.GetDescriptorIdentity())
				{
					OutError = DescriptorDifference(Context.GetDescriptorIdentity(), Request.GetDraft().GetDescriptorIdentity(),
						TEXT("rig_simulation_spatial_planning_context_mismatch"), TEXT("/draft/descriptor"), Contract.GetDescriptorId());
					return false;
				}
				if (Context.GetDescriptorIdentity().GetDescriptorId() != Contract.GetDescriptorId())
				{
					OutError = NativeFact(TEXT("rig_simulation_spatial_planning_context_mismatch"),
						TEXT("The planning context descriptor differs from the execution chain descriptor."), TEXT("/descriptor_id"),
						MakeShared<FJsonValueString>(Context.GetDescriptorIdentity().GetDescriptorId()),
						MakeShared<FJsonValueString>(Contract.GetDescriptorId()), Contract.GetDescriptorId());
					return false;
				}
				{
					FScopeLock Lock(&State->Mutex);
					const auto* Prepared = State->PreparedByRequest.Find(
						Request.GetDraft().GetRequestId());
					if (!Prepared || Prepared->Context != &Context)
					{
						OutError = NativeFact(TEXT("rig_simulation_spatial_planning_request_not_prepared"),
							Prepared ? TEXT("The prepared planning request belongs to a different operation context.")
							: TEXT("No prepared request exists for this planning request."),
							Prepared ? TEXT("/prepared_request/context_matches") : TEXT("/prepared_request"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Request.GetDraft().GetRequestId());
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
					OutError = Error;
					return false;
				}
				return true;
			}

		private:
			FUeAgentRigSimulationSpatialExecutionChainContract Contract;
			TSharedRef<FUeAgentRigSimulationSpatialOperationChain::FState, ESPMode::ThreadSafe> State;
		};

		class FRigSimulationSpatialExecutor final : public IUeAgentOperationExecutor
		{
		public:
			FRigSimulationSpatialExecutor(
				FUeAgentRigSimulationSpatialExecutionChainContract InContract,
				TSharedRef<FUeAgentRigSimulationSpatialOperationChain::FState, ESPMode::ThreadSafe> InState,
				TSharedRef<IUeAgentRigSimulationSpatialMutationPort> InMutation)
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
				if (!Planning.IsReady())
				{
					OutError = Planning.GetPlan().IsValid()
						? NativeFact(TEXT("rig_simulation_spatial_execution_plan_not_ready"),
							TEXT("The planning result status does not permit execution."), TEXT("/planning/status"),
							MakeShared<FJsonValueString>(Planning.GetStatus()), MakeShared<FJsonValueString>(TEXT("ready or no_change")), Contract.GetCapabilityId())
						: NativeFact(TEXT("rig_simulation_spatial_execution_plan_not_ready"),
							TEXT("The planning result has no execution plan."), TEXT("/planning/plan"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetCapabilityId());
					return false;
				}
				if (!Planning.IsMutationPermitted())
				{
					OutError = NativeFact(TEXT("rig_simulation_spatial_execution_plan_not_ready"),
						Planning.IsDryRun() ? TEXT("A dry-run plan does not permit mutation.")
						: TEXT("The planning result does not permit mutation."),
						TEXT("/planning/mutation_permitted"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), Contract.GetCapabilityId());
					return false;
				}
				const FString RequestId = Planning.GetPlan()->GetRequestId();
				FUeAgentRigSimulationSpatialOperationChain::FState::FPrepared Prepared;
				{
					FScopeLock Lock(&State->Mutex);
					auto* Found = State->PreparedByRequest.Find(RequestId);
					if (!Found || Found->Context != &Context)
					{
						OutError = NativeFact(TEXT("rig_simulation_spatial_execution_request_mismatch"),
							Found ? TEXT("The prepared request belongs to a different execution context.")
							: TEXT("The execution plan has no prepared request."),
							Found ? TEXT("/prepared_request/context_matches") : TEXT("/prepared_request"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), RequestId);
						return false;
					}
					if (!TryMatchTargets(Found->Targets, Targets, TEXT("rig_simulation_spatial_execution_request_mismatch"), OutError)) return false;
					Prepared = MoveTemp(*Found);
					State->PreparedByRequest.Remove(RequestId);
				}

				FUeAgentRigSimulationSpatialMutationResult MutationResult;
				FUeAgentRigSimulationSpatialExecutionError Error;
				const bool bMutationSucceeded = Mutation->TryMutate(Contract, Prepared.Request.ToSharedRef(),
					Targets, Context, MutationResult, Error);
				if (!bMutationSucceeded || !MutationResult.IsValid())
				{
					if (!Error.IsValid())
					{
						if (!bMutationSucceeded)
							Error = NativeFact(TEXT("rig_simulation_spatial_mutation_result_invalid"),
								TEXT("The mutation port failed without reporting a diagnostic."), TEXT("/mutation/reported_success"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetNativeActionId());
						else
							Error = NativeFact(TEXT("rig_simulation_spatial_mutation_result_invalid"),
								MutationResult.GetOperationId().IsEmpty() ? TEXT("The mutation result has no operation identifier.")
								: TEXT("The mutation result has no mutation evidence identifier."),
								MutationResult.GetOperationId().IsEmpty() ? TEXT("/mutation/operation_id_present") : TEXT("/mutation/evidence_present"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetNativeActionId());
					}
					OutError = Error;
					return false;
				}

				{
					FScopeLock Lock(&State->Mutex);
					if (State->ExecutedByOperation.Contains(
							MutationResult.GetOperationId()))
					{
						OutError = NativeFact(TEXT("rig_simulation_spatial_operation_id_duplicate"),
							TEXT("The mutation operation identifier is already registered as executed."), TEXT("/operation_id_already_executed"),
							MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), MutationResult.GetOperationId());
						return false;
					}
					State->ExecutedByOperation.Add(
						MutationResult.GetOperationId(),
						{MoveTemp(Prepared), MutationResult});
				}
				OutReceipt = FExecutionReceipt(MutationResult.GetOperationId(),
			EExecutionPhase::Terminal,
			EExecutionStatus::AppliedInMemory,
			FExecutionMutationError(),
			EExecutionCancellationMode::NotRequested,
			true,
			{},
			false,
			{},
			{TEXT("executing"), TEXT("terminal")});
				if (!OutReceipt.IsValid())
				{
					OutError = NativeValidation(OutReceipt.GetValidationDiagnostic());
					return false;
				}
				return true;
			}

		private:
			FUeAgentRigSimulationSpatialExecutionChainContract Contract;
			TSharedRef<FUeAgentRigSimulationSpatialOperationChain::FState, ESPMode::ThreadSafe> State;
			TSharedRef<IUeAgentRigSimulationSpatialMutationPort> Mutation;
		};

		class FRigSimulationSpatialVerifier final : public IUeAgentOperationVerifier
		{
		public:
			FRigSimulationSpatialVerifier(
				FUeAgentRigSimulationSpatialExecutionChainContract InContract,
				TSharedRef<FUeAgentRigSimulationSpatialOperationChain::FState, ESPMode::ThreadSafe> InState,
				TSharedRef<IUeAgentRigSimulationSpatialReadbackPort> InReadback,
				TSharedRef<IUeAgentRigSimulationSpatialSettlePort> InSettle,
				TSharedRef<IUeAgentRigSimulationSpatialResultComparator> InComparator)
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
				if (!Receipt.IsValid())
				{
					OutError = NativeValidation(Receipt.GetValidationDiagnostic());
					return false;
				}
				if (Receipt.GetStatus() != EExecutionStatus::AppliedInMemory)
				{
					OutError = NativeFact(TEXT("rig_simulation_spatial_verification_receipt_invalid"),
						TEXT("Verification requires an execution receipt with AppliedInMemory status."), TEXT("/receipt/status"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Receipt.GetStatus())),
						MakeShared<FJsonValueNumber>(static_cast<int32>(EExecutionStatus::AppliedInMemory)), Receipt.GetOperationId());
					return false;
				}

				FUeAgentRigSimulationSpatialOperationChain::FState::FExecuted Executed;
				{
					FScopeLock Lock(&State->Mutex);
					auto* Found = State->ExecutedByOperation.Find(
						Receipt.GetOperationId());
					if (!Found || Found->Prepared.Context != &Context)
					{
						OutError = NativeFact(TEXT("rig_simulation_spatial_verification_request_mismatch"),
							Found ? TEXT("The executed operation belongs to a different verification context.")
							: TEXT("No executed operation exists for the verification receipt."),
							Found ? TEXT("/executed_operation/context_matches") : TEXT("/executed_operation"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Receipt.GetOperationId());
						return false;
					}
					if (!TryMatchTargets(Found->Prepared.Targets, Targets, TEXT("rig_simulation_spatial_verification_request_mismatch"), OutError)) return false;
					Executed = MoveTemp(*Found);
					State->ExecutedByOperation.Remove(Receipt.GetOperationId());
				}

				FUeAgentRigSimulationSpatialExecutionError Error;
				if (!Settle->TrySettle(
						Contract, Executed.Mutation, Context, Error))
				{
					OutError = Error;
					return false;
				}
				TOptional<FUeAgentRigSimulationSpatialReadbackResult> After;
				const bool bReadbackSucceeded = Readback->TryRead(
						Contract,
						Executed.Prepared.Request.ToSharedRef(),
						FirstReadTarget(Targets),
						EUeAgentRigSimulationSpatialReadPhase::Verification,
						Context,
						After,
						Error);
				if (!bReadbackSucceeded || !After.IsSet())
				{
					if (!Error.IsValid())
					{
						Error = ReadbackFailure(TEXT("rig_simulation_spatial_verification_readback_missing"), Contract.GetReadbackId(), bReadbackSucceeded, After.IsSet());
					}
					OutError = Error;
					return false;
				}
				if (!Comparator->TryCompare(
						Contract,
						Executed.Prepared.Request.ToSharedRef(),
						*Executed.Prepared.Before,
						After.GetValue(),
						Context,
						OutOutcome,
						Error))
				{
					OutError = Error;
					return false;
				}
				return true;
			}

		private:
			FUeAgentRigSimulationSpatialExecutionChainContract Contract;
			TSharedRef<FUeAgentRigSimulationSpatialOperationChain::FState, ESPMode::ThreadSafe> State;
			TSharedRef<IUeAgentRigSimulationSpatialReadbackPort> Readback;
			TSharedRef<IUeAgentRigSimulationSpatialSettlePort> Settle;
			TSharedRef<IUeAgentRigSimulationSpatialResultComparator> Comparator;
		};
	}

	FUeAgentRigSimulationSpatialOperationChain::FUeAgentRigSimulationSpatialOperationChain(
		FUeAgentRigSimulationSpatialExecutionChainContract InContract,
		TSharedRef<IUeAgentRigSimulationSpatialRequestDecoder> InDecoder,
		TSharedRef<IUeAgentRigSimulationSpatialReadbackPort> InReadback,
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

	const FUeAgentRigSimulationSpatialExecutionChainContract&
	FUeAgentRigSimulationSpatialOperationChain::GetContract() const
	{
		return Contract;
	}

	bool FUeAgentRigSimulationSpatialOperationChain::TryBuildPlanningRequest(
		const FUeAgentContractValue& CanonicalInput,
		const TArray<FUeAgentOperationTargetBinding>& Targets,
		const FUeAgentOperationContext& Context,
		TOptional<FPlanningRequest>& OutRequest,
		FUeAgentOperationBindingError& OutError)
	{
		OutRequest.Reset();
		OutError = {};
		if (Context.GetRequestKind() != EUeAgentRequestKind::Operation)
		{
			OutError = NativeFact(TEXT("rig_simulation_spatial_context_descriptor_mismatch"),
				TEXT("Planning requires an operation request context."), TEXT("/request_kind"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(Context.GetRequestKind())),
				MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentRequestKind::Operation)), Contract.GetDescriptorId());
			return false;
		}
		if (Context.GetDescriptorIdentity().GetDescriptorId() != Contract.GetDescriptorId())
		{
			OutError = NativeFact(TEXT("rig_simulation_spatial_context_descriptor_mismatch"),
				TEXT("The request context descriptor differs from the execution chain descriptor."), TEXT("/descriptor_id"),
				MakeShared<FJsonValueString>(Context.GetDescriptorIdentity().GetDescriptorId()),
				MakeShared<FJsonValueString>(Contract.GetDescriptorId()), Contract.GetDescriptorId());
			return false;
		}

		FUeAgentRigSimulationSpatialTypedRequestPtr TypedRequest;
		FUeAgentRigSimulationSpatialExecutionError Error;
		if (!Decoder->TryDecode(
				Contract,
				CanonicalInput,
				Context.GetResourceTracker(),
				TypedRequest,
				Error)
			|| !IsTypedRequestExact(Contract, TypedRequest, Error))
		{
			OutError = Error;
			return false;
		}

		TOptional<FUeAgentRigSimulationSpatialReadbackResult> ReadbackResult;
		const bool bReadbackSucceeded = Readback->TryRead(
				Contract,
				TypedRequest.ToSharedRef(),
				FirstReadTarget(Targets),
				EUeAgentRigSimulationSpatialReadPhase::Preflight,
				Context,
				ReadbackResult,
				Error);
		if (!bReadbackSucceeded || !ReadbackResult.IsSet())
		{
			if (!Error.IsValid())
			{
				Error = ReadbackFailure(TEXT("rig_simulation_spatial_preflight_readback_missing"), Contract.GetReadbackId(), bReadbackSucceeded, ReadbackResult.IsSet());
			}
			OutError = Error;
			return false;
		}

		const auto& Before = ReadbackResult->Snapshot;
		TOptional<FChangeDraft> Change;
		const bool bDraftBuilt = TypedRequest->TryBuildChangeDraft(*Before, Context, Change, Error);
		if (!bDraftBuilt || !Change.IsSet() || !Change->IsValid())
		{
			if (!Error.IsValid())
			{
				if (!bDraftBuilt)
					Error = NativeFact(TEXT("rig_simulation_spatial_change_draft_invalid"),
						TEXT("The request failed to build a change draft without reporting a diagnostic."), TEXT("/change_draft/reported_success"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetC6ChangeKind());
				else if (!Change.IsSet())
					Error = NativeFact(TEXT("rig_simulation_spatial_change_draft_invalid"),
						TEXT("The request reported success without a change draft."), TEXT("/change_draft"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetC6ChangeKind());
				else Error = NativeValidation(Change->GetValidationDiagnostic());
			}
			OutError = Error;
			return false;
		}

		FChangeSetDraft Draft(
			Context.GetDescriptorIdentity(),
			Context.GetRequestId(),
			Context.GetIdempotencyKey(),
			Before,
			{MoveTemp(Change.GetValue())});
		OutRequest.Emplace(Draft, Context.GetCallerContext(), false);
		if (!OutRequest->IsValid())
		{
			const auto ValidationError = NativeValidation(OutRequest->GetValidationDiagnostic());
			OutRequest.Reset();
			OutError = ValidationError;
			return false;
		}

		{
			FScopeLock Lock(&State->Mutex);
			if (State->PreparedByRequest.Contains(Context.GetRequestId()))
			{
				OutRequest.Reset();
				OutError = NativeFact(TEXT("rig_simulation_spatial_request_already_prepared"),
					TEXT("This request already has a prepared operation entry."), TEXT("/request_already_prepared"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Context.GetRequestId());
				return false;
			}
			State->PreparedByRequest.Add(
				Context.GetRequestId(),
				{TypedRequest, Before, Targets, &Context});
		}
		return true;
	}

	IUeAgentOperationPlanner& FUeAgentRigSimulationSpatialOperationChain::GetPlanner() const { return Planner.Get(); }
	IUeAgentOperationExecutor& FUeAgentRigSimulationSpatialOperationChain::GetExecutor() const { return Executor.Get(); }
	IUeAgentOperationVerifier& FUeAgentRigSimulationSpatialOperationChain::GetVerifier() const { return Verifier.Get(); }

	bool TryCreateRigSimulationSpatialQueryProviderRegistry(
		TSharedRef<IUeAgentRigSimulationSpatialRequestDecoder> Decoder,
		TSharedRef<IUeAgentRigSimulationSpatialReadbackPort> Readback,
		TSharedPtr<const FUeAgentDomainQueryProviderRegistry>& OutRegistry,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutRegistry.Reset();
		OutError = {};
		TArray<FUeAgentDomainQueryProviderRegistration> Registrations;
		for (const FUeAgentRigSimulationSpatialExecutionChainContract& Contract
			: GetRigSimulationSpatialExecutionChainContracts())
		{
			if (Contract.GetC3Lane() != TEXT("c3q"))
			{
				continue;
			}
			Registrations.Emplace(
				Contract.GetBindingId(),
				MakeShared<FRigSimulationSpatialQueryProvider>(
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

	bool TryCreateRigSimulationSpatialOperationChains(
		TSharedRef<IUeAgentRigSimulationSpatialRequestDecoder> Decoder,
		TSharedRef<IUeAgentRigSimulationSpatialReadbackPort> Readback,
		TSharedRef<IUeAgentRigSimulationSpatialMutationPort> Mutation,
		TSharedRef<IUeAgentRigSimulationSpatialSettlePort> Settle,
		TSharedRef<IUeAgentRigSimulationSpatialResultComparator> Comparator,
		TArray<TSharedRef<FUeAgentRigSimulationSpatialOperationChain>>& OutChains,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutChains.Reset();
		OutError = {};
		for (const FUeAgentRigSimulationSpatialExecutionChainContract& Contract
			: GetRigSimulationSpatialExecutionChainContracts())
		{
			if (Contract.GetC3Lane() != TEXT("c3o"))
			{
				continue;
			}
			const TSharedRef<FUeAgentRigSimulationSpatialOperationChain::FState, ESPMode::ThreadSafe>
				State = MakeShared<FUeAgentRigSimulationSpatialOperationChain::FState, ESPMode::ThreadSafe>();
			const TSharedRef<IUeAgentOperationPlanner> Planner =
				MakeShared<FRigSimulationSpatialPlanner>(Contract, State);
			const TSharedRef<IUeAgentOperationExecutor> Executor =
				MakeShared<FRigSimulationSpatialExecutor>(Contract, State, Mutation);
			const TSharedRef<IUeAgentOperationVerifier> Verifier =
				MakeShared<FRigSimulationSpatialVerifier>(
					Contract, State, Readback, Settle, Comparator);
			OutChains.Add(MakeShareable(
				new FUeAgentRigSimulationSpatialOperationChain(
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
