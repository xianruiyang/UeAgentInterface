#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialExecutionChains.h"

#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/Verification/UeAgentVerification.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::DomainAdapters::WorldSpatial
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
			FUeAgentWorldSpatialExecutionError& OutError,
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
			const FUeAgentWorldSpatialExecutionError& Error,
			FUeAgentQueryBindingError& OutError)
		{
			OutError = {};
			OutError.Code = Error.Code;
			OutError.SubjectId = Error.SubjectId;
			OutError.Detail = Error.Detail;
		}

		void CopyError(
			const FUeAgentWorldSpatialExecutionError& Error,
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
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const FUeAgentWorldSpatialTypedRequestPtr& Request,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			if (!Request.IsValid()
				|| Request->GetTypeId() != Contract.GetTypedRequestId()
				|| Request->GetFamilyId() != Contract.GetFamilyId())
			{
				SetExecutionError(
					OutError,
					TEXT("world_spatial_typed_request_mismatch"),
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

	FUeAgentWorldSpatialExecutionChainContract::
	FUeAgentWorldSpatialExecutionChainContract(
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

	const FString& FUeAgentWorldSpatialExecutionChainContract::GetCapabilityId() const { return CapabilityId; }
	const FString& FUeAgentWorldSpatialExecutionChainContract::GetDescriptorId() const { return DescriptorId; }
	const FString& FUeAgentWorldSpatialExecutionChainContract::GetOperationKind() const { return OperationKind; }
	const FString& FUeAgentWorldSpatialExecutionChainContract::GetC3Lane() const { return C3Lane; }
	const FString& FUeAgentWorldSpatialExecutionChainContract::GetFamilyId() const { return FamilyId; }
	const FString& FUeAgentWorldSpatialExecutionChainContract::GetBindingId() const { return BindingId; }
	const FString& FUeAgentWorldSpatialExecutionChainContract::GetTypedRequestId() const { return TypedRequestId; }
	const FString& FUeAgentWorldSpatialExecutionChainContract::GetTypedResultId() const { return TypedResultId; }
	const FString& FUeAgentWorldSpatialExecutionChainContract::GetC6ChangeKind() const { return C6ChangeKind; }
	const FString& FUeAgentWorldSpatialExecutionChainContract::GetNativeActionId() const { return NativeActionId; }
	const FString& FUeAgentWorldSpatialExecutionChainContract::GetReadbackId() const { return ReadbackId; }
	const FString& FUeAgentWorldSpatialExecutionChainContract::GetReadbackOwner() const { return ReadbackOwner; }
	const FString& FUeAgentWorldSpatialExecutionChainContract::GetSettleBarrier() const { return SettleBarrier; }
	const FString& FUeAgentWorldSpatialExecutionChainContract::GetOracleKind() const { return OracleKind; }
	bool FUeAgentWorldSpatialExecutionChainContract::RequiresNativeMutation() const { return bRequiresNativeMutation; }
	bool FUeAgentWorldSpatialExecutionChainContract::ReadsBackAfterMutation() const { return bReadsBackAfterMutation; }

	const TArray<FUeAgentWorldSpatialExecutionChainContract>&
	GetWorldSpatialExecutionChainContracts()
	{
		static const TArray<FUeAgentWorldSpatialExecutionChainContract> Contracts = {
			{TEXT("graph.view.read"), TEXT("uai.query.graph.view.read"), TEXT("query"), TEXT("c3q"), TEXT("graph_view"), TEXT("world_spatial.graph_view.observe"), TEXT("world_spatial.graph_view.read.request.v1"), TEXT("world_spatial.graph_view.read.result.v1"), TEXT(""), TEXT(""), TEXT("world_spatial.graph_view.readback"), TEXT("live_and_blueprint_document_state"), TEXT("none"), TEXT("live_and_persisted_graph_view_readback"), false, false},
			{TEXT("graph.view.update"), TEXT("uai.operation.graph.view.update"), TEXT("operation"), TEXT("c3o"), TEXT("graph_view"), TEXT("world_spatial.graph_view.set_state"), TEXT("world_spatial.graph_view.update.request.v1"), TEXT("world_spatial.graph_view.update.result.v1"), TEXT("world_spatial.graph_view.state"), TEXT("world_spatial.graph_view.set_state.mutate"), TEXT("world_spatial.graph_view.readback"), TEXT("live_and_blueprint_document_state"), TEXT("graph_editor_refresh_and_optional_package_save"), TEXT("live_and_persisted_graph_view_readback"), true, true},
			{TEXT("graph.layout.apply"), TEXT("uai.operation.graph.layout.apply"), TEXT("operation"), TEXT("c3o"), TEXT("graph_layout"), TEXT("world_spatial.graph_layout.apply"), TEXT("world_spatial.graph_layout.apply.request.v1"), TEXT("world_spatial.graph_layout.apply.result.v1"), TEXT("world_spatial.graph_layout.nodes"), TEXT("world_spatial.graph_layout.apply.mutate"), TEXT("world_spatial.graph_layout.read_nodes"), TEXT("independent_graph_node_reader"), TEXT("graph_layout_refresh"), TEXT("independent_node_position_and_connection_readback"), true, true},
			{TEXT("world.object_transform.read"), TEXT("uai.query.world.object_transform.read"), TEXT("query"), TEXT("c3q"), TEXT("object_transform"), TEXT("world_spatial.object_transform.read"), TEXT("world_spatial.object_transform.read.request.v1"), TEXT("world_spatial.object_transform.read.result.v1"), TEXT(""), TEXT(""), TEXT("world_spatial.object_transform.readback"), TEXT("independent_target_transform_reader"), TEXT("none"), TEXT("independent_target_spatial_readback"), false, false},
			{TEXT("world.object_transform.update"), TEXT("uai.operation.world.object_transform.update"), TEXT("operation"), TEXT("c3o"), TEXT("object_transform"), TEXT("world_spatial.object_transform.update"), TEXT("world_spatial.object_transform.update.request.v1"), TEXT("world_spatial.object_transform.update.result.v1"), TEXT("world_spatial.object_transform.update"), TEXT("world_spatial.object_transform.update.mutate"), TEXT("world_spatial.object_transform.persisted_readback"), TEXT("independent_actor_transform_reader"), TEXT("world_transform_propagation_and_package_save"), TEXT("independent_persisted_actor_transform_readback"), true, true},
			{TEXT("world.object_align"), TEXT("uai.operation.world.object_align"), TEXT("operation"), TEXT("c3o"), TEXT("object_transform"), TEXT("world_spatial.object_transform.align"), TEXT("world_spatial.object_transform.align.request.v1"), TEXT("world_spatial.object_transform.align.result.v1"), TEXT("world_spatial.object_transform.align"), TEXT("world_spatial.object_transform.align.mutate"), TEXT("world_spatial.object_transform.readback"), TEXT("independent_anchor_and_transform_reader"), TEXT("world_transform_propagation"), TEXT("independent_anchor_and_transform_readback"), true, true},
			{TEXT("world.object_snap_to_surface"), TEXT("uai.operation.world.object_snap_to_surface"), TEXT("operation"), TEXT("c3o"), TEXT("object_transform"), TEXT("world_spatial.object_transform.snap_to_surface"), TEXT("world_spatial.object_transform.snap_to_surface.request.v1"), TEXT("world_spatial.object_transform.snap_to_surface.result.v1"), TEXT("world_spatial.object_transform.snap_to_surface"), TEXT("world_spatial.object_transform.snap_to_surface.mutate"), TEXT("world_spatial.object_transform.surface_and_transform_readback"), TEXT("independent_surface_and_transform_reader"), TEXT("world_transform_propagation"), TEXT("independent_surface_hit_and_transform_readback"), true, true},
			{TEXT("mesh.transform.bake"), TEXT("uai.operation.mesh.transform.bake"), TEXT("operation"), TEXT("c3o"), TEXT("object_transform"), TEXT("world_spatial.object_transform.bake_mesh"), TEXT("world_spatial.object_transform.bake_mesh.request.v1"), TEXT("world_spatial.object_transform.bake_mesh.result.v1"), TEXT("world_spatial.object_transform.bake_mesh"), TEXT("world_spatial.object_transform.bake_mesh.mutate"), TEXT("world_spatial.object_transform.mesh_geometry_and_placement_readback"), TEXT("independent_mesh_geometry_reader"), TEXT("mesh_rebuild"), TEXT("independent_mesh_geometry_and_placement_readback"), true, true},
			{TEXT("mesh.pivot.update"), TEXT("uai.operation.mesh.pivot.update"), TEXT("operation"), TEXT("c3o"), TEXT("object_transform"), TEXT("world_spatial.object_transform.update_pivot"), TEXT("world_spatial.object_transform.update_pivot.request.v1"), TEXT("world_spatial.object_transform.update_pivot.result.v1"), TEXT("world_spatial.object_transform.update_pivot"), TEXT("world_spatial.object_transform.update_pivot.mutate"), TEXT("world_spatial.object_transform.pivot_geometry_and_placement_readback"), TEXT("independent_pivot_geometry_reader"), TEXT("mesh_rebuild"), TEXT("independent_pivot_geometry_and_placement_readback"), true, true},
			{TEXT("viewport.ray.deproject"), TEXT("uai.query.viewport.ray.deproject"), TEXT("query"), TEXT("c3q"), TEXT("viewport_projection"), TEXT("world_spatial.viewport_projection.deproject"), TEXT("world_spatial.viewport_projection.deproject.request.v1"), TEXT("world_spatial.viewport_projection.deproject.result.v1"), TEXT(""), TEXT(""), TEXT("world_spatial.viewport_projection.camera_deprojection_readback"), TEXT("exact_viewport_camera_reader"), TEXT("none"), TEXT("camera_projection_reconstruction"), false, false},
			{TEXT("viewport.ray.trace"), TEXT("uai.query.viewport.ray.trace"), TEXT("query"), TEXT("c3q"), TEXT("viewport_projection"), TEXT("world_spatial.viewport_projection.trace"), TEXT("world_spatial.viewport_projection.trace.request.v1"), TEXT("world_spatial.viewport_projection.trace.result.v1"), TEXT(""), TEXT(""), TEXT("world_spatial.viewport_projection.world_trace_readback"), TEXT("independent_world_trace_reader"), TEXT("none"), TEXT("independent_world_trace"), false, false},
			{TEXT("viewport.actor.pick"), TEXT("uai.query.viewport.actor.pick"), TEXT("query"), TEXT("c3q"), TEXT("viewport_projection"), TEXT("world_spatial.viewport_projection.pick_actor"), TEXT("world_spatial.viewport_projection.pick_actor.request.v1"), TEXT("world_spatial.viewport_projection.pick_actor.result.v1"), TEXT(""), TEXT(""), TEXT("world_spatial.viewport_projection.pick_actor_readback"), TEXT("independent_world_trace_target_reader"), TEXT("none"), TEXT("independent_world_trace_target_identity"), false, false},
			{TEXT("viewport.actor.select"), TEXT("uai.operation.viewport.actor.select"), TEXT("operation"), TEXT("c3o"), TEXT("viewport_projection"), TEXT("world_spatial.viewport_projection.select_actor"), TEXT("world_spatial.viewport_projection.select_actor.request.v1"), TEXT("world_spatial.viewport_projection.select_actor.result.v1"), TEXT("world_spatial.viewport_projection.select_actor"), TEXT("world_spatial.viewport_projection.select_actor.mutate"), TEXT("world_spatial.viewport_projection.editor_selection_readback"), TEXT("independent_editor_selection_reader"), TEXT("editor_selection_refresh"), TEXT("independent_editor_selection_readback"), true, true},
			{TEXT("mesh.element.select_by_ray"), TEXT("uai.operation.mesh.element.select_by_ray"), TEXT("operation"), TEXT("c3o"), TEXT("viewport_projection"), TEXT("world_spatial.viewport_projection.select_mesh_element"), TEXT("world_spatial.viewport_projection.select_mesh_element.request.v1"), TEXT("world_spatial.viewport_projection.select_mesh_element.result.v1"), TEXT("world_spatial.viewport_projection.select_mesh_element"), TEXT("world_spatial.viewport_projection.select_mesh_element.mutate"), TEXT("world_spatial.viewport_projection.modeling_selection_readback"), TEXT("independent_modeling_selection_reader"), TEXT("modeling_selection_refresh"), TEXT("independent_modeling_selection_readback"), true, true},
			{TEXT("viewport.pixel_scale.read"), TEXT("uai.query.viewport.pixel_scale.read"), TEXT("query"), TEXT("c3q"), TEXT("viewport_projection"), TEXT("world_spatial.viewport_projection.read_pixel_scale"), TEXT("world_spatial.viewport_projection.read_pixel_scale.request.v1"), TEXT("world_spatial.viewport_projection.read_pixel_scale.result.v1"), TEXT(""), TEXT(""), TEXT("world_spatial.viewport_projection.pixel_scale_readback"), TEXT("camera_neighbor_sample_reader"), TEXT("none"), TEXT("camera_projection_neighbor_sample"), false, false},
			{TEXT("viewport.camera.frame"), TEXT("uai.operation.viewport.camera.frame"), TEXT("operation"), TEXT("c3o"), TEXT("viewport_session_state"), TEXT("world_spatial.viewport_session.frame_camera"), TEXT("world_spatial.viewport_session.frame_camera.request.v1"), TEXT("world_spatial.viewport_session.frame_camera.result.v1"), TEXT("world_spatial.viewport_session.frame_camera"), TEXT("world_spatial.viewport_session.frame_camera.mutate"), TEXT("world_spatial.viewport_session.state_readback"), TEXT("same_viewport_client_reader"), TEXT("viewport_redraw"), TEXT("same_viewport_camera_readback"), true, true},
			{TEXT("viewport.state.read"), TEXT("uai.query.viewport.state.read"), TEXT("query"), TEXT("c3q"), TEXT("viewport_session_state"), TEXT("world_spatial.viewport_session.read_state"), TEXT("world_spatial.viewport_session.read_state.request.v1"), TEXT("world_spatial.viewport_session.read_state.result.v1"), TEXT(""), TEXT(""), TEXT("world_spatial.viewport_session.state_readback"), TEXT("same_viewport_client_reader"), TEXT("none"), TEXT("same_viewport_state_readback"), false, false},
			{TEXT("viewport.state.restore"), TEXT("uai.operation.viewport.state.restore"), TEXT("operation"), TEXT("c3o"), TEXT("viewport_session_state"), TEXT("world_spatial.viewport_session.restore_state"), TEXT("world_spatial.viewport_session.restore_state.request.v1"), TEXT("world_spatial.viewport_session.restore_state.result.v1"), TEXT("world_spatial.viewport_session.restore_state"), TEXT("world_spatial.viewport_session.restore_state.mutate"), TEXT("world_spatial.viewport_session.state_readback"), TEXT("same_viewport_client_reader"), TEXT("viewport_redraw"), TEXT("same_viewport_snapshot_readback"), true, true},
			{TEXT("viewport.warning.list"), TEXT("uai.query.viewport.warning.list"), TEXT("query"), TEXT("c3q"), TEXT("viewport_session_state"), TEXT("world_spatial.viewport_session.list_warnings"), TEXT("world_spatial.viewport_session.list_warnings.request.v1"), TEXT("world_spatial.viewport_session.list_warnings.result.v1"), TEXT(""), TEXT(""), TEXT("world_spatial.viewport_session.warning_readback"), TEXT("independent_viewport_warning_reader"), TEXT("none"), TEXT("independent_viewport_warning_projection"), false, false},
			{TEXT("viewport.view_mode.list"), TEXT("uai.query.viewport.view_mode.list"), TEXT("query"), TEXT("c3q"), TEXT("viewport_session_state"), TEXT("world_spatial.viewport_session.list_view_modes"), TEXT("world_spatial.viewport_session.list_view_modes.request.v1"), TEXT("world_spatial.viewport_session.list_view_modes.result.v1"), TEXT(""), TEXT(""), TEXT("world_spatial.viewport_session.view_mode_catalog_readback"), TEXT("engine_view_mode_catalog_reader"), TEXT("none"), TEXT("engine_view_mode_catalog"), false, false},
			{TEXT("viewport.instance.list"), TEXT("uai.query.viewport.instance.list"), TEXT("query"), TEXT("c3q"), TEXT("viewport_session_state"), TEXT("world_spatial.viewport_session.list_instances"), TEXT("world_spatial.viewport_session.list_instances.request.v1"), TEXT("world_spatial.viewport_session.list_instances.result.v1"), TEXT(""), TEXT(""), TEXT("world_spatial.viewport_session.instance_catalog_readback"), TEXT("editor_viewport_client_catalog_reader"), TEXT("none"), TEXT("editor_viewport_client_catalog"), false, false},
			{TEXT("viewport.state.update"), TEXT("uai.operation.viewport.state.update"), TEXT("operation"), TEXT("c3o"), TEXT("viewport_session_state"), TEXT("world_spatial.viewport_session.update_state"), TEXT("world_spatial.viewport_session.update_state.request.v1"), TEXT("world_spatial.viewport_session.update_state.result.v1"), TEXT("world_spatial.viewport_session.update_state"), TEXT("world_spatial.viewport_session.update_state.mutate"), TEXT("world_spatial.viewport_session.state_readback"), TEXT("same_viewport_client_reader"), TEXT("viewport_redraw"), TEXT("same_viewport_typed_patch_readback"), true, true},
		};
		return Contracts;
	}

	FUeAgentWorldSpatialMutationResult::FUeAgentWorldSpatialMutationResult(
		FString InOperationId,
		FString InMutationEvidenceId)
		: OperationId(MoveTemp(InOperationId))
		, MutationEvidenceId(MoveTemp(InMutationEvidenceId))
	{
		OperationId.TrimStartAndEndInline();
		MutationEvidenceId.TrimStartAndEndInline();
		bValid = !OperationId.IsEmpty() && !MutationEvidenceId.IsEmpty();
	}

	const FString& FUeAgentWorldSpatialMutationResult::GetOperationId() const { return OperationId; }
	const FString& FUeAgentWorldSpatialMutationResult::GetMutationEvidenceId() const { return MutationEvidenceId; }
	bool FUeAgentWorldSpatialMutationResult::IsValid() const { return bValid; }

	class FWorldSpatialQueryProvider final : public IUeAgentDomainQueryProvider
	{
	public:
		FWorldSpatialQueryProvider(
			FUeAgentWorldSpatialExecutionChainContract InContract,
			TSharedRef<IUeAgentWorldSpatialRequestDecoder> InDecoder,
			TSharedRef<IUeAgentWorldSpatialReadbackPort> InReadback)
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
				OutError.Code = TEXT("world_spatial_query_binding_mismatch");
				OutError.SubjectId = Contract.GetBindingId();
				return false;
			}
			if (Context.GetRequestKind() != EUeAgentRequestKind::Query
				|| Context.GetDescriptorIdentity() != Descriptor.GetIdentity())
			{
				OutError.Code = TEXT("world_spatial_context_descriptor_mismatch");
				OutError.SubjectId = Contract.GetDescriptorId();
				return false;
			}

			FUeAgentWorldSpatialTypedRequestPtr Request;
			FUeAgentWorldSpatialExecutionError Error;
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
					EUeAgentWorldSpatialReadPhase::Query,
					Context,
					Snapshot,
					Error)
				|| !Snapshot.IsValid())
			{
				if (!Error.IsValid())
				{
					SetExecutionError(
						Error,
						TEXT("world_spatial_query_readback_missing"),
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
		FUeAgentWorldSpatialExecutionChainContract Contract;
		TSharedRef<IUeAgentWorldSpatialRequestDecoder> Decoder;
		TSharedRef<IUeAgentWorldSpatialReadbackPort> Readback;
	};

	class FUeAgentWorldSpatialOperationChain::FState final
	{
	public:
		struct FPrepared
		{
			FUeAgentWorldSpatialTypedRequestPtr Request;
			TSharedPtr<const FUeAgentCanonicalSnapshot> Before;
			TArray<FUeAgentOperationTargetBinding> Targets;
			const FUeAgentOperationContext* Context = nullptr;
		};

		struct FExecuted
		{
			FPrepared Prepared;
			FUeAgentWorldSpatialMutationResult Mutation;
		};

		FCriticalSection Mutex;
		TMap<FString, FPrepared> PreparedByRequest;
		TMap<FString, FExecuted> ExecutedByOperation;
	};

	namespace
	{
		class FWorldSpatialPlanner final : public IUeAgentOperationPlanner
		{
		public:
			FWorldSpatialPlanner(
				FUeAgentWorldSpatialExecutionChainContract InContract,
				TSharedRef<FUeAgentWorldSpatialOperationChain::FState, ESPMode::ThreadSafe> InState)
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
						TEXT("world_spatial_planning_context_mismatch"),
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
							TEXT("world_spatial_planning_request_not_prepared"),
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
			FUeAgentWorldSpatialExecutionChainContract Contract;
			TSharedRef<FUeAgentWorldSpatialOperationChain::FState, ESPMode::ThreadSafe> State;
		};

		class FWorldSpatialExecutor final : public IUeAgentOperationExecutor
		{
		public:
			FWorldSpatialExecutor(
				FUeAgentWorldSpatialExecutionChainContract InContract,
				TSharedRef<FUeAgentWorldSpatialOperationChain::FState, ESPMode::ThreadSafe> InState,
				TSharedRef<IUeAgentWorldSpatialMutationPort> InMutation)
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
						TEXT("world_spatial_execution_plan_not_ready"),
						Contract.GetCapabilityId());
					return false;
				}
				const FString RequestId = Planning.GetPlan()->GetRequestId();
				FUeAgentWorldSpatialOperationChain::FState::FPrepared Prepared;
				{
					FScopeLock Lock(&State->Mutex);
					auto* Found = State->PreparedByRequest.Find(RequestId);
					if (!Found || Found->Context != &Context
						|| !TargetsEqual(Found->Targets, Targets))
					{
						SetBindingError(
							OutError,
							TEXT("world_spatial_execution_request_mismatch"),
							RequestId);
						return false;
					}
					Prepared = MoveTemp(*Found);
					State->PreparedByRequest.Remove(RequestId);
				}

				FUeAgentWorldSpatialMutationResult MutationResult;
				FUeAgentWorldSpatialExecutionError Error;
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
							TEXT("world_spatial_mutation_result_invalid"),
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
							TEXT("world_spatial_operation_id_duplicate"),
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
			FUeAgentWorldSpatialExecutionChainContract Contract;
			TSharedRef<FUeAgentWorldSpatialOperationChain::FState, ESPMode::ThreadSafe> State;
			TSharedRef<IUeAgentWorldSpatialMutationPort> Mutation;
		};

		class FWorldSpatialVerifier final : public IUeAgentOperationVerifier
		{
		public:
			FWorldSpatialVerifier(
				FUeAgentWorldSpatialExecutionChainContract InContract,
				TSharedRef<FUeAgentWorldSpatialOperationChain::FState, ESPMode::ThreadSafe> InState,
				TSharedRef<IUeAgentWorldSpatialReadbackPort> InReadback,
				TSharedRef<IUeAgentWorldSpatialSettlePort> InSettle,
				TSharedRef<IUeAgentWorldSpatialResultComparator> InComparator)
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
						TEXT("world_spatial_verification_receipt_invalid"),
						Receipt.GetOperationId());
					return false;
				}

				FUeAgentWorldSpatialOperationChain::FState::FExecuted Executed;
				{
					FScopeLock Lock(&State->Mutex);
					auto* Found = State->ExecutedByOperation.Find(
						Receipt.GetOperationId());
					if (!Found || Found->Prepared.Context != &Context
						|| !TargetsEqual(Found->Prepared.Targets, Targets))
					{
						SetBindingError(
							OutError,
							TEXT("world_spatial_verification_request_mismatch"),
							Receipt.GetOperationId());
						return false;
					}
					Executed = MoveTemp(*Found);
					State->ExecutedByOperation.Remove(Receipt.GetOperationId());
				}

				FUeAgentWorldSpatialExecutionError Error;
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
						EUeAgentWorldSpatialReadPhase::Verification,
						Context,
						After,
						Error)
					|| !After.IsValid())
				{
					if (!Error.IsValid())
					{
						SetExecutionError(
							Error,
							TEXT("world_spatial_verification_readback_missing"),
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
			FUeAgentWorldSpatialExecutionChainContract Contract;
			TSharedRef<FUeAgentWorldSpatialOperationChain::FState, ESPMode::ThreadSafe> State;
			TSharedRef<IUeAgentWorldSpatialReadbackPort> Readback;
			TSharedRef<IUeAgentWorldSpatialSettlePort> Settle;
			TSharedRef<IUeAgentWorldSpatialResultComparator> Comparator;
		};
	}

	FUeAgentWorldSpatialOperationChain::FUeAgentWorldSpatialOperationChain(
		FUeAgentWorldSpatialExecutionChainContract InContract,
		TSharedRef<IUeAgentWorldSpatialRequestDecoder> InDecoder,
		TSharedRef<IUeAgentWorldSpatialReadbackPort> InReadback,
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

	const FUeAgentWorldSpatialExecutionChainContract&
	FUeAgentWorldSpatialOperationChain::GetContract() const
	{
		return Contract;
	}

	bool FUeAgentWorldSpatialOperationChain::TryBuildPlanningRequest(
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
				TEXT("world_spatial_context_descriptor_mismatch"),
				Contract.GetDescriptorId());
			return false;
		}

		FUeAgentWorldSpatialTypedRequestPtr TypedRequest;
		FUeAgentWorldSpatialExecutionError Error;
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
				EUeAgentWorldSpatialReadPhase::Preflight,
				Context,
				Before,
				Error)
			|| !Before.IsValid())
		{
			if (!Error.IsValid())
			{
				SetExecutionError(
					Error,
					TEXT("world_spatial_preflight_readback_missing"),
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
					TEXT("world_spatial_change_draft_invalid"),
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
				TEXT("world_spatial_planning_request_invalid"),
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
					TEXT("world_spatial_request_already_prepared"),
					Context.GetRequestId());
				return false;
			}
			State->PreparedByRequest.Add(
				Context.GetRequestId(),
				{TypedRequest, Before, Targets, &Context});
		}
		return true;
	}

	IUeAgentOperationPlanner& FUeAgentWorldSpatialOperationChain::GetPlanner() const { return Planner.Get(); }
	IUeAgentOperationExecutor& FUeAgentWorldSpatialOperationChain::GetExecutor() const { return Executor.Get(); }
	IUeAgentOperationVerifier& FUeAgentWorldSpatialOperationChain::GetVerifier() const { return Verifier.Get(); }

	bool TryCreateWorldSpatialQueryProviderRegistry(
		TSharedRef<IUeAgentWorldSpatialRequestDecoder> Decoder,
		TSharedRef<IUeAgentWorldSpatialReadbackPort> Readback,
		TSharedPtr<const FUeAgentDomainQueryProviderRegistry>& OutRegistry,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutRegistry.Reset();
		OutError = {};
		TArray<FUeAgentDomainQueryProviderRegistration> Registrations;
		for (const FUeAgentWorldSpatialExecutionChainContract& Contract
			: GetWorldSpatialExecutionChainContracts())
		{
			if (Contract.GetC3Lane() != TEXT("c3q"))
			{
				continue;
			}
			Registrations.Emplace(
				Contract.GetBindingId(),
				MakeShared<FWorldSpatialQueryProvider>(
					Contract, Decoder, Readback));
		}
		if (Registrations.Num() != 10)
		{
			SetExecutionError(
				OutError,
				TEXT("world_spatial_query_contract_count_invalid"));
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

	bool TryCreateWorldSpatialOperationChains(
		TSharedRef<IUeAgentWorldSpatialRequestDecoder> Decoder,
		TSharedRef<IUeAgentWorldSpatialReadbackPort> Readback,
		TSharedRef<IUeAgentWorldSpatialMutationPort> Mutation,
		TSharedRef<IUeAgentWorldSpatialSettlePort> Settle,
		TSharedRef<IUeAgentWorldSpatialResultComparator> Comparator,
		TArray<TSharedRef<FUeAgentWorldSpatialOperationChain>>& OutChains,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutChains.Reset();
		OutError = {};
		for (const FUeAgentWorldSpatialExecutionChainContract& Contract
			: GetWorldSpatialExecutionChainContracts())
		{
			if (Contract.GetC3Lane() != TEXT("c3o"))
			{
				continue;
			}
			const TSharedRef<FUeAgentWorldSpatialOperationChain::FState, ESPMode::ThreadSafe>
				State = MakeShared<FUeAgentWorldSpatialOperationChain::FState, ESPMode::ThreadSafe>();
			const TSharedRef<IUeAgentOperationPlanner> Planner =
				MakeShared<FWorldSpatialPlanner>(Contract, State);
			const TSharedRef<IUeAgentOperationExecutor> Executor =
				MakeShared<FWorldSpatialExecutor>(Contract, State, Mutation);
			const TSharedRef<IUeAgentOperationVerifier> Verifier =
				MakeShared<FWorldSpatialVerifier>(
					Contract, State, Readback, Settle, Comparator);
			OutChains.Add(MakeShareable(
				new FUeAgentWorldSpatialOperationChain(
					Contract,
					Decoder,
					Readback,
					State,
					Planner,
					Executor,
					Verifier)));
		}
		if (OutChains.Num() != 12)
		{
			OutChains.Reset();
			SetExecutionError(
				OutError,
				TEXT("world_spatial_operation_contract_count_invalid"));
			return false;
		}
		return true;
	}
}
