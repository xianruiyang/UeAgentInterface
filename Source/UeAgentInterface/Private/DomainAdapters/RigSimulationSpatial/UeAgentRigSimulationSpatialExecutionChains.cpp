#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialExecutionChains.h"

#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/Verification/UeAgentVerification.h"
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
		void SetExecutionError(
			FUeAgentRigSimulationSpatialExecutionError& OutError,
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
			const FUeAgentRigSimulationSpatialExecutionError& Error,
			FUeAgentQueryBindingError& OutError)
		{
			OutError = {};
			OutError.Code = Error.Code;
			OutError.SubjectId = Error.SubjectId;
			OutError.Detail = Error.Detail;
		}

		void CopyError(
			const FUeAgentRigSimulationSpatialExecutionError& Error,
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
			if (!Request.IsValid()
				|| Request->GetTypeId() != Contract.GetTypedRequestId()
				|| Request->GetFamilyId() != Contract.GetFamilyId())
			{
				SetExecutionError(
					OutError,
					TEXT("rig_simulation_spatial_typed_request_mismatch"),
					Contract.GetCapabilityId());
				return false;
			}
			EUeAgentRigSimulationSpatialPortFamily ExpectedFamily =
				EUeAgentRigSimulationSpatialPortFamily::SimulationObservation;
			if (!TryPortFamily(Contract.GetFamilyId(), ExpectedFamily)
				|| Request->GetFormalPortRequest().GetFamily()
					!= ExpectedFamily)
			{
				SetExecutionError(
					OutError,
					TEXT("rig_simulation_spatial_formal_port_family_mismatch"),
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

	const TArray<FUeAgentRigSimulationSpatialExecutionChainContract>&
	GetRigSimulationSpatialExecutionChainContracts()
	{
		static const TArray<FUeAgentRigSimulationSpatialExecutionChainContract> Contracts = {
			{TEXT("rig.skeleton.preview_mesh.bind"), TEXT("uai.operation.rig.skeleton.preview_mesh.bind"), TEXT("operation"), TEXT("c3o"), TEXT("skeletal_attachment"), TEXT("rig_simulation_spatial.skeleton.bind_preview_mesh"), TEXT("rig_simulation_spatial.skeleton.bind_preview_mesh.request.v1"), TEXT("rig_simulation_spatial.skeleton.bind_preview_mesh.result.v1"), TEXT("rig_simulation_spatial.skeleton.bind_preview_mesh"), TEXT("rig_simulation_spatial.skeleton.bind_preview_mesh.mutate"), TEXT("rig_simulation_spatial.skeleton.bind_preview_mesh.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("skeleton_post_edit_settle"), TEXT("independent_skeleton_socket_and_parent_bone_readback"), true, true},
			{TEXT("rig.skeleton.socket.update"), TEXT("uai.operation.rig.skeleton.socket.update"), TEXT("operation"), TEXT("c3o"), TEXT("skeletal_attachment"), TEXT("rig_simulation_spatial.skeleton.update_socket"), TEXT("rig_simulation_spatial.skeleton.update_socket.request.v1"), TEXT("rig_simulation_spatial.skeleton.update_socket.result.v1"), TEXT("rig_simulation_spatial.skeleton.update_socket"), TEXT("rig_simulation_spatial.skeleton.update_socket.mutate"), TEXT("rig_simulation_spatial.skeleton.update_socket.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("skeleton_post_edit_settle"), TEXT("independent_skeleton_socket_and_parent_bone_readback"), true, true},
			{TEXT("rig.control.bake.animation"), TEXT("uai.operation.rig.control.bake.animation"), TEXT("operation"), TEXT("c3o"), TEXT("rig_bake"), TEXT("rig_simulation_spatial.control_rig.bake_animation"), TEXT("rig_simulation_spatial.control_rig.bake_animation.request.v1"), TEXT("rig_simulation_spatial.control_rig.bake_animation.result.v1"), TEXT("rig_simulation_spatial.control_rig.bake_animation"), TEXT("rig_simulation_spatial.control_rig.bake_animation.mutate"), TEXT("rig_simulation_spatial.control_rig.bake_animation.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("rig_bake_evaluation_and_asset_settle"), TEXT("independent_target_animation_or_control_key_readback"), true, true},
			{TEXT("rig.control.bake.controls"), TEXT("uai.operation.rig.control.bake.controls"), TEXT("operation"), TEXT("c3o"), TEXT("rig_bake"), TEXT("rig_simulation_spatial.control_rig.bake_controls"), TEXT("rig_simulation_spatial.control_rig.bake_controls.request.v1"), TEXT("rig_simulation_spatial.control_rig.bake_controls.result.v1"), TEXT("rig_simulation_spatial.control_rig.bake_controls"), TEXT("rig_simulation_spatial.control_rig.bake_controls.mutate"), TEXT("rig_simulation_spatial.control_rig.bake_controls.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("rig_bake_evaluation_and_asset_settle"), TEXT("independent_target_animation_or_control_key_readback"), true, true},
			{TEXT("rig.ik.definition.update"), TEXT("uai.operation.rig.ik.definition.update"), TEXT("operation"), TEXT("c3o"), TEXT("ik_retarget"), TEXT("rig_simulation_spatial.ik.update_definition"), TEXT("rig_simulation_spatial.ik.update_definition.request.v1"), TEXT("rig_simulation_spatial.ik.update_definition.result.v1"), TEXT("rig_simulation_spatial.ik.update_definition"), TEXT("rig_simulation_spatial.ik.update_definition.mutate"), TEXT("rig_simulation_spatial.ik.update_definition.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("ik_retarget_compile_and_asset_settle"), TEXT("independent_source_target_definition_pose_and_asset_readback"), true, true},
			{TEXT("rig.retarget.definition.update"), TEXT("uai.operation.rig.retarget.definition.update"), TEXT("operation"), TEXT("c3o"), TEXT("ik_retarget"), TEXT("rig_simulation_spatial.retarget.update_definition"), TEXT("rig_simulation_spatial.retarget.update_definition.request.v1"), TEXT("rig_simulation_spatial.retarget.update_definition.result.v1"), TEXT("rig_simulation_spatial.retarget.update_definition"), TEXT("rig_simulation_spatial.retarget.update_definition.mutate"), TEXT("rig_simulation_spatial.retarget.update_definition.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("ik_retarget_compile_and_asset_settle"), TEXT("independent_source_target_definition_pose_and_asset_readback"), true, true},
			{TEXT("rig.retarget.pose.update"), TEXT("uai.operation.rig.retarget.pose.update"), TEXT("operation"), TEXT("c3o"), TEXT("ik_retarget"), TEXT("rig_simulation_spatial.retarget.update_pose"), TEXT("rig_simulation_spatial.retarget.update_pose.request.v1"), TEXT("rig_simulation_spatial.retarget.update_pose.result.v1"), TEXT("rig_simulation_spatial.retarget.update_pose"), TEXT("rig_simulation_spatial.retarget.update_pose.mutate"), TEXT("rig_simulation_spatial.retarget.update_pose.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("ik_retarget_compile_and_asset_settle"), TEXT("independent_source_target_definition_pose_and_asset_readback"), true, true},
			{TEXT("rig.retarget.pose.auto_align"), TEXT("uai.operation.rig.retarget.pose.auto_align"), TEXT("operation"), TEXT("c3o"), TEXT("ik_retarget"), TEXT("rig_simulation_spatial.retarget.auto_align_pose"), TEXT("rig_simulation_spatial.retarget.auto_align_pose.request.v1"), TEXT("rig_simulation_spatial.retarget.auto_align_pose.result.v1"), TEXT("rig_simulation_spatial.retarget.auto_align_pose"), TEXT("rig_simulation_spatial.retarget.auto_align_pose.mutate"), TEXT("rig_simulation_spatial.retarget.auto_align_pose.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("ik_retarget_compile_and_asset_settle"), TEXT("independent_source_target_definition_pose_and_asset_readback"), true, true},
			{TEXT("rig.retarget.assets.generate"), TEXT("uai.operation.rig.retarget.assets.generate"), TEXT("operation"), TEXT("c3o"), TEXT("ik_retarget"), TEXT("rig_simulation_spatial.retarget.generate_assets"), TEXT("rig_simulation_spatial.retarget.generate_assets.request.v1"), TEXT("rig_simulation_spatial.retarget.generate_assets.result.v1"), TEXT("rig_simulation_spatial.retarget.generate_assets"), TEXT("rig_simulation_spatial.retarget.generate_assets.mutate"), TEXT("rig_simulation_spatial.retarget.generate_assets.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("ik_retarget_compile_and_asset_settle"), TEXT("independent_source_target_definition_pose_and_asset_readback"), true, true},
			{TEXT("physics.material.update"), TEXT("uai.operation.physics.material.update"), TEXT("operation"), TEXT("c3o"), TEXT("simulation_definition"), TEXT("rig_simulation_spatial.physics.update_material"), TEXT("rig_simulation_spatial.physics.update_material.request.v1"), TEXT("rig_simulation_spatial.physics.update_material.result.v1"), TEXT("rig_simulation_spatial.physics.update_material"), TEXT("rig_simulation_spatial.physics.update_material.mutate"), TEXT("rig_simulation_spatial.physics.update_material.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("physics_asset_rebuild_and_constraint_settle"), TEXT("independent_physics_asset_or_constraint_definition_readback"), true, true},
			{TEXT("physics.asset.structure.replace"), TEXT("uai.operation.physics.asset.structure.replace"), TEXT("operation"), TEXT("c3o"), TEXT("simulation_definition"), TEXT("rig_simulation_spatial.physics.replace_asset_structure"), TEXT("rig_simulation_spatial.physics.replace_asset_structure.request.v1"), TEXT("rig_simulation_spatial.physics.replace_asset_structure.result.v1"), TEXT("rig_simulation_spatial.physics.replace_asset_structure"), TEXT("rig_simulation_spatial.physics.replace_asset_structure.mutate"), TEXT("rig_simulation_spatial.physics.replace_asset_structure.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("physics_asset_rebuild_and_constraint_settle"), TEXT("independent_physics_asset_or_constraint_definition_readback"), true, true},
			{TEXT("physics.constraint.update"), TEXT("uai.operation.physics.constraint.update"), TEXT("operation"), TEXT("c3o"), TEXT("simulation_definition"), TEXT("rig_simulation_spatial.physics.update_constraint"), TEXT("rig_simulation_spatial.physics.update_constraint.request.v1"), TEXT("rig_simulation_spatial.physics.update_constraint.result.v1"), TEXT("rig_simulation_spatial.physics.update_constraint"), TEXT("rig_simulation_spatial.physics.update_constraint.mutate"), TEXT("rig_simulation_spatial.physics.update_constraint.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("physics_asset_rebuild_and_constraint_settle"), TEXT("independent_physics_asset_or_constraint_definition_readback"), true, true},
			{TEXT("physics.simulation.probe"), TEXT("uai.operation.physics.simulation.probe"), TEXT("operation"), TEXT("c3o"), TEXT("simulation_observation"), TEXT("rig_simulation_spatial.physics.probe_simulation"), TEXT("rig_simulation_spatial.physics.probe_simulation.request.v1"), TEXT("rig_simulation_spatial.physics.probe_simulation.result.v1"), TEXT("rig_simulation_spatial.physics.probe_simulation"), TEXT("rig_simulation_spatial.physics.probe_simulation.mutate"), TEXT("rig_simulation_spatial.physics.probe_simulation.readback"), TEXT("independent_post_mutation_rig_simulation_spatial_reader"), TEXT("owned_simulation_session_settle"), TEXT("independent_component_asset_or_generated_data_readback"), true, true},
			{TEXT("deformer.geometry.sample"), TEXT("uai.query.deformer.geometry.sample"), TEXT("query"), TEXT("c3q"), TEXT("simulation_observation"), TEXT("rig_simulation_spatial.deformer.sample_geometry"), TEXT("rig_simulation_spatial.deformer.sample_geometry.request.v1"), TEXT("rig_simulation_spatial.deformer.sample_geometry.result.v1"), TEXT(""), TEXT("rig_simulation_spatial.deformer.sample_geometry.observe"), TEXT("rig_simulation_spatial.deformer.sample_geometry.readback"), TEXT("independent_rig_simulation_spatial_reader"), TEXT("none"), TEXT("independent_component_asset_or_generated_data_readback"), false, false},
			{TEXT("deformer.training_inputs.validate"), TEXT("uai.query.deformer.training_inputs.validate"), TEXT("query"), TEXT("c3q"), TEXT("simulation_observation"), TEXT("rig_simulation_spatial.deformer.validate_training_inputs"), TEXT("rig_simulation_spatial.deformer.validate_training_inputs.request.v1"), TEXT("rig_simulation_spatial.deformer.validate_training_inputs.result.v1"), TEXT(""), TEXT("rig_simulation_spatial.deformer.validate_training_inputs.observe"), TEXT("rig_simulation_spatial.deformer.validate_training_inputs.readback"), TEXT("independent_rig_simulation_spatial_reader"), TEXT("none"), TEXT("independent_component_asset_or_generated_data_readback"), false, false},
			{TEXT("deformer.evaluation.sample"), TEXT("uai.query.deformer.evaluation.sample"), TEXT("query"), TEXT("c3q"), TEXT("simulation_observation"), TEXT("rig_simulation_spatial.deformer.sample_evaluation"), TEXT("rig_simulation_spatial.deformer.sample_evaluation.request.v1"), TEXT("rig_simulation_spatial.deformer.sample_evaluation.result.v1"), TEXT(""), TEXT("rig_simulation_spatial.deformer.sample_evaluation.observe"), TEXT("rig_simulation_spatial.deformer.sample_evaluation.readback"), TEXT("independent_rig_simulation_spatial_reader"), TEXT("none"), TEXT("independent_component_asset_or_generated_data_readback"), false, false},
			{TEXT("pcg.generated_spatial_data.read"), TEXT("uai.query.pcg.generated_spatial_data.read"), TEXT("query"), TEXT("c3q"), TEXT("simulation_observation"), TEXT("rig_simulation_spatial.pcg.read_generated_data"), TEXT("rig_simulation_spatial.pcg.read_generated_data.request.v1"), TEXT("rig_simulation_spatial.pcg.read_generated_data.result.v1"), TEXT(""), TEXT("rig_simulation_spatial.pcg.read_generated_data.observe"), TEXT("rig_simulation_spatial.pcg.read_generated_data.readback"), TEXT("independent_rig_simulation_spatial_reader"), TEXT("none"), TEXT("independent_component_asset_or_generated_data_readback"), false, false},
			{TEXT("pcg.partition.spatial_state.read"), TEXT("uai.query.pcg.partition.spatial_state.read"), TEXT("query"), TEXT("c3q"), TEXT("simulation_observation"), TEXT("rig_simulation_spatial.pcg.read_partition_state"), TEXT("rig_simulation_spatial.pcg.read_partition_state.request.v1"), TEXT("rig_simulation_spatial.pcg.read_partition_state.result.v1"), TEXT(""), TEXT("rig_simulation_spatial.pcg.read_partition_state.observe"), TEXT("rig_simulation_spatial.pcg.read_partition_state.readback"), TEXT("independent_rig_simulation_spatial_reader"), TEXT("none"), TEXT("independent_component_asset_or_generated_data_readback"), false, false},
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
			if (Descriptor.GetCapabilityId() != Contract.GetCapabilityId()
				|| Descriptor.GetIdentity().GetDescriptorId() != Contract.GetDescriptorId()
				|| Descriptor.GetProviderBinding() != Contract.GetBindingId())
			{
				OutError.Code = TEXT("rig_simulation_spatial_query_binding_mismatch");
				OutError.SubjectId = Contract.GetBindingId();
				return false;
			}
			if (Context.GetRequestKind() != EUeAgentRequestKind::Query
				|| Context.GetDescriptorIdentity() != Descriptor.GetIdentity())
			{
				OutError.Code = TEXT("rig_simulation_spatial_context_descriptor_mismatch");
				OutError.SubjectId = Contract.GetDescriptorId();
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
				CopyError(Error, OutError);
				return false;
			}

			TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
			if (!Readback->TryRead(
					Contract,
					Request.ToSharedRef(),
					Target,
					EUeAgentRigSimulationSpatialReadPhase::Query,
					Context,
					Snapshot,
					Error)
				|| !Snapshot.IsValid())
			{
				if (!Error.IsValid())
				{
					SetExecutionError(
						Error,
						TEXT("rig_simulation_spatial_query_readback_missing"),
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
				if (!Request.IsValid()
					|| Request.GetDraft().GetDescriptorIdentity()
						!= Context.GetDescriptorIdentity()
					|| Context.GetDescriptorIdentity().GetDescriptorId()
						!= Contract.GetDescriptorId())
				{
					SetBindingError(
						OutError,
						TEXT("rig_simulation_spatial_planning_context_mismatch"),
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
							TEXT("rig_simulation_spatial_planning_request_not_prepared"),
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
				if (!Planning.IsReady() || !Planning.IsMutationPermitted()
					|| !Planning.GetPlan().IsValid())
				{
					SetBindingError(
						OutError,
						TEXT("rig_simulation_spatial_execution_plan_not_ready"),
						Contract.GetCapabilityId());
					return false;
				}
				const FString RequestId = Planning.GetPlan()->GetRequestId();
				FUeAgentRigSimulationSpatialOperationChain::FState::FPrepared Prepared;
				{
					FScopeLock Lock(&State->Mutex);
					auto* Found = State->PreparedByRequest.Find(RequestId);
					if (!Found || Found->Context != &Context
						|| !TargetsEqual(Found->Targets, Targets))
					{
						SetBindingError(
							OutError,
							TEXT("rig_simulation_spatial_execution_request_mismatch"),
							RequestId);
						return false;
					}
					Prepared = MoveTemp(*Found);
					State->PreparedByRequest.Remove(RequestId);
				}

				FUeAgentRigSimulationSpatialMutationResult MutationResult;
				FUeAgentRigSimulationSpatialExecutionError Error;
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
							TEXT("rig_simulation_spatial_mutation_result_invalid"),
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
							TEXT("rig_simulation_spatial_operation_id_duplicate"),
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
				if (!Receipt.IsValid()
					|| Receipt.GetStatus() != EExecutionStatus::AppliedInMemory)
				{
					SetBindingError(
						OutError,
						TEXT("rig_simulation_spatial_verification_receipt_invalid"),
						Receipt.GetOperationId());
					return false;
				}

				FUeAgentRigSimulationSpatialOperationChain::FState::FExecuted Executed;
				{
					FScopeLock Lock(&State->Mutex);
					auto* Found = State->ExecutedByOperation.Find(
						Receipt.GetOperationId());
					if (!Found || Found->Prepared.Context != &Context
						|| !TargetsEqual(Found->Prepared.Targets, Targets))
					{
						SetBindingError(
							OutError,
							TEXT("rig_simulation_spatial_verification_request_mismatch"),
							Receipt.GetOperationId());
						return false;
					}
					Executed = MoveTemp(*Found);
					State->ExecutedByOperation.Remove(Receipt.GetOperationId());
				}

				FUeAgentRigSimulationSpatialExecutionError Error;
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
						EUeAgentRigSimulationSpatialReadPhase::Verification,
						Context,
						After,
						Error)
					|| !After.IsValid())
				{
					if (!Error.IsValid())
					{
						SetExecutionError(
							Error,
							TEXT("rig_simulation_spatial_verification_readback_missing"),
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
		if (Context.GetRequestKind() != EUeAgentRequestKind::Operation
			|| Context.GetDescriptorIdentity().GetDescriptorId()
				!= Contract.GetDescriptorId())
		{
			SetBindingError(
				OutError,
				TEXT("rig_simulation_spatial_context_descriptor_mismatch"),
				Contract.GetDescriptorId());
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
			CopyError(Error, OutError);
			return false;
		}

		TSharedPtr<const FUeAgentCanonicalSnapshot> Before;
		if (!Readback->TryRead(
				Contract,
				TypedRequest.ToSharedRef(),
				FirstReadTarget(Targets),
				EUeAgentRigSimulationSpatialReadPhase::Preflight,
				Context,
				Before,
				Error)
			|| !Before.IsValid())
		{
			if (!Error.IsValid())
			{
				SetExecutionError(
					Error,
					TEXT("rig_simulation_spatial_preflight_readback_missing"),
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
					TEXT("rig_simulation_spatial_change_draft_invalid"),
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
				TEXT("rig_simulation_spatial_planning_request_invalid"),
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
					TEXT("rig_simulation_spatial_request_already_prepared"),
					Context.GetRequestId());
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
		if (Registrations.Num() != 5)
		{
			SetExecutionError(
				OutError,
				TEXT("rig_simulation_spatial_query_contract_count_invalid"));
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
		if (OutChains.Num() != 13)
		{
			OutChains.Reset();
			SetExecutionError(
				OutError,
				TEXT("rig_simulation_spatial_operation_contract_count_invalid"));
			return false;
		}
		return true;
	}
}
