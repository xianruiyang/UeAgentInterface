#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialExecutionChains.h"

#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/Verification/UeAgentVerification.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
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

	bool FUeAgentWorldSpatialPreflightFence::TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const
	{
		OutError = {};
		if (!Entries.IsEmpty()) return true;
		OutError = FDiagnostic::Failure(TEXT("world_spatial_preflight_fence_missing"),
			TEXT("The preflight fence contains no target revision entries."), TEXT("native_execution"));
		OutError.PropertyPath = TEXT("/entries");
		OutError.SourceKind = TEXT("native_result");
		OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
		OutError.ActualValuePresent = true;
		OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one target revision entry"));
		return false;
	}

	bool FUeAgentWorldSpatialPreflightFence::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	namespace
	{
		FString InputSubject(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const TOptional<FUeAgentCandidateBinding>& Target,
			const FUeAgentOperationContext& Context)
		{
			const auto& Execution = Context.GetExecutionContext();
			if (Contract.GetFamilyId() == TEXT("graph_view")
				|| Contract.GetFamilyId() == TEXT("graph_layout"))
			{
				return Execution.GetAssetEditorIdentity();
			}
			if (Contract.GetFamilyId() == TEXT("viewport_projection")
				|| Contract.GetFamilyId() == TEXT("viewport_session_state"))
			{
				return Execution.GetViewportIdentity();
			}
			return Target.IsSet() ? Target->GetPublicRef() : Execution.GetWorldIdentity();
		}

		

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
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const FUeAgentWorldSpatialTypedRequestPtr& Request,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			if (!Request.IsValid())
			{
				OutError = ChainFact(TEXT("world_spatial_typed_request_mismatch"),
					TEXT("Decoder did not return a typed spatial request."), TEXT("/typed_request"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetCapabilityId());
				return false;
			}
			if (Request->GetTypeId() != Contract.GetTypedRequestId())
			{
				OutError = ChainFact(TEXT("world_spatial_typed_request_mismatch"),
					TEXT("Decoded spatial request has a different type from the execution-chain contract."), TEXT("/typed_request/type"),
					MakeShared<FJsonValueString>(Request->GetTypeId()), MakeShared<FJsonValueString>(Contract.GetTypedRequestId()), Contract.GetCapabilityId());
				return false;
			}
			if (Request->GetFamilyId() != Contract.GetFamilyId())
			{
				OutError = ChainFact(TEXT("world_spatial_typed_request_mismatch"),
					TEXT("Decoded spatial request belongs to a different port family."), TEXT("/typed_request/family"),
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

	FUeAgentWorldSpatialMutationResult::FUeAgentWorldSpatialMutationResult()
		: FUeAgentWorldSpatialMutationResult(FString(), FString())
	{
	}

	FUeAgentWorldSpatialMutationResult FUeAgentWorldSpatialMutationResult::NoMutation()
	{
		FUeAgentWorldSpatialMutationResult Result;
		Result.FailureFacts = UE::UAI::ExecutionMutation::FMutationFacts::NoMutation();
		return Result;
	}

	bool FUeAgentWorldSpatialMutationResult::IsNoMutationConfirmed() const
	{
		return FailureFacts.IsNoMutationConfirmed();
	}

	FUeAgentWorldSpatialMutationResult::FUeAgentWorldSpatialMutationResult(
		FString InOperationId,
		FString InMutationEvidenceId)
		: OperationId(MoveTemp(InOperationId))
		, MutationEvidenceId(MoveTemp(InMutationEvidenceId))
	{
		OperationId.TrimStartAndEndInline();
		MutationEvidenceId.TrimStartAndEndInline();
		if (OperationId.IsEmpty() || MutationEvidenceId.IsEmpty())
		{
			const bool bOperationMissing = OperationId.IsEmpty();
			ValidationDiagnostic = FDiagnostic::Failure(TEXT("world_spatial_mutation_result_invalid"),
				bOperationMissing ? TEXT("The mutation receipt is missing its operation identifier.")
					: TEXT("The mutation receipt is missing its evidence identifier."), TEXT("native_execution"));
			ValidationDiagnostic.PropertyPath = bOperationMissing ? TEXT("/operation_id") : TEXT("/mutation_evidence_id");
			ValidationDiagnostic.Source.JsonPath = ValidationDiagnostic.PropertyPath;
			ValidationDiagnostic.SourceKind = TEXT("native_result");
			ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(bOperationMissing ? OperationId : MutationEvidenceId);
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty identifier"));
		}
	}

	const FString& FUeAgentWorldSpatialMutationResult::GetOperationId() const { return OperationId; }
	const FString& FUeAgentWorldSpatialMutationResult::GetMutationEvidenceId() const { return MutationEvidenceId; }
	bool FUeAgentWorldSpatialMutationResult::TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const
	{
		OutError = ValidationDiagnostic;
		return !ValidationDiagnostic.IsValid();
	}
	bool FUeAgentWorldSpatialMutationResult::IsValid() const { return !ValidationDiagnostic.IsValid(); }

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
			ON_SCOPE_EXIT { SupplementChainTarget(OutError, Target.IsSet() ? Target->GetPublicRef() : FString()); };
			if (Descriptor.GetCapabilityId() != Contract.GetCapabilityId())
			{
				OutError = ChainFact(TEXT("world_spatial_query_binding_mismatch"),
					TEXT("Query capability differs from the execution-chain capability."), TEXT("/descriptor/capability_id"),
					MakeShared<FJsonValueString>(Descriptor.GetCapabilityId()), MakeShared<FJsonValueString>(Contract.GetCapabilityId()));
				return false;
			}
			if (Descriptor.GetIdentity().GetDescriptorId() != Contract.GetDescriptorId())
			{
				OutError = ChainFact(TEXT("world_spatial_query_binding_mismatch"),
					TEXT("Query descriptor id differs from the execution-chain descriptor."), TEXT("/descriptor/descriptor_id"),
					MakeShared<FJsonValueString>(Descriptor.GetIdentity().GetDescriptorId()), MakeShared<FJsonValueString>(Contract.GetDescriptorId()));
				return false;
			}
			if (Descriptor.GetProviderBinding() != Contract.GetBindingId())
			{
				OutError = ChainFact(TEXT("world_spatial_query_binding_mismatch"),
					TEXT("Query provider binding differs from the execution-chain binding."), TEXT("/descriptor/provider_binding_matches"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetBindingId());
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Descriptor.GetProviderBinding());
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Contract.GetBindingId());
				return false;
			}
			if (Context.GetRequestKind() != EUeAgentRequestKind::Query)
			{
				OutError = ChainFact(TEXT("world_spatial_context_descriptor_mismatch"),
					TEXT("A spatial query requires a query request context."), TEXT("/context/request_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Context.GetRequestKind())),
					MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentRequestKind::Query)));
				return false;
			}
			if (Context.GetDescriptorIdentity() != Descriptor.GetIdentity())
			{
				OutError = ChainDescriptorMismatch(TEXT("world_spatial_context_descriptor_mismatch"),
					Context.GetDescriptorIdentity(), Descriptor.GetIdentity(), TEXT("/context/descriptor"));
				return false;
			}

			FUeAgentWorldSpatialTypedRequestPtr Request;
			FUeAgentWorldSpatialExecutionError Error;
			if (!Decoder->TryDecode(
					Contract, CanonicalInput, Context.GetResourceTracker(), Request, Error,
					InputSubject(Contract, Target, Context))
				|| !IsTypedRequestExact(Contract, Request, Error))
			{
				OutError = Error.IsEmpty() ? ChainFact(TEXT("world_spatial_request_decode_failed"),
					TEXT("The spatial request decoder failed without returning a diagnostic."), TEXT("/decoder/succeeded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetTypedRequestId()) : MoveTemp(Error);
				return false;
			}

			TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
			const bool bRead = Readback->TryRead(
				Contract, Request.ToSharedRef(), Target, EUeAgentWorldSpatialReadPhase::Query, Context, Snapshot, Error);
			if (!bRead || !Snapshot.IsValid())
			{
				if (Error.IsEmpty())
					Error = ChainFact(TEXT("world_spatial_query_readback_missing"),
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
			FUeAgentWorldSpatialPreflightFence Fence;
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
			TSharedPtr<IUeAgentWorldSpatialMutationPort> Mutation;
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
				ON_SCOPE_EXIT { SupplementChainTarget(OutError, FString()); };
				if (!Request.IsValid())
				{
					OutError = Request.GetValidationDiagnostic();
					return false;
				}
				if (Request.GetDraft().GetDescriptorIdentity() != Context.GetDescriptorIdentity())
				{
					OutError = ChainDescriptorMismatch(TEXT("world_spatial_planning_context_mismatch"),
						Request.GetDraft().GetDescriptorIdentity(), Context.GetDescriptorIdentity(), TEXT("/draft/descriptor"));
					return false;
				}
				if (Context.GetDescriptorIdentity().GetDescriptorId() != Contract.GetDescriptorId())
				{
					OutError = ChainFact(TEXT("world_spatial_planning_context_mismatch"),
						TEXT("Planning context descriptor differs from the execution-chain descriptor."), TEXT("/context/descriptor/descriptor_id"),
						MakeShared<FJsonValueString>(Context.GetDescriptorIdentity().GetDescriptorId()), MakeShared<FJsonValueString>(Contract.GetDescriptorId()));
					return false;
				}
				{
					FScopeLock Lock(&State->Mutex);
					const auto* Prepared = State->PreparedByRequest.Find(Request.GetDraft().GetRequestId());
					if (!Prepared || Prepared->Context != &Context)
					{
						OutError = ChainFact(TEXT("world_spatial_planning_request_not_prepared"),
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
				bool bNoMutationConfirmed = true;
				ON_SCOPE_EXIT
				{
					SupplementChainTargets(OutError, Targets);
					if (!OutError.IsEmpty() && bNoMutationConfirmed)
					{
						OutReceipt = FExecutionReceipt(TEXT("world-spatial:") + Context.GetRequestId(),
							EExecutionPhase::Terminal, EExecutionStatus::Failed, OutError,
							EExecutionCancellationMode::NotRequested, false, {}, true, {},
							{TEXT("executing"), TEXT("terminal")});
					}
				};
				if (!Planning.IsReady())
				{
					OutError = ChainFact(TEXT("world_spatial_execution_plan_not_ready"),
						TEXT("The planning result is not ready for execution."), TEXT("/planning/ready"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetCapabilityId());
					return false;
				}
				if (!Planning.IsMutationPermitted())
				{
					OutError = ChainFact(TEXT("world_spatial_execution_plan_not_ready"),
						TEXT("The planning result does not permit mutation."), TEXT("/planning/mutation_permitted"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetCapabilityId());
					return false;
				}
				if (!Planning.GetPlan().IsValid())
				{
					OutError = ChainFact(TEXT("world_spatial_execution_plan_not_ready"),
						TEXT("The planning result has no executable plan."), TEXT("/planning/plan"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetCapabilityId());
					return false;
				}
				const FString RequestId = Planning.GetPlan()->GetRequestId();
				FUeAgentWorldSpatialOperationChain::FState::FPrepared Prepared;
				{
					FScopeLock Lock(&State->Mutex);
					auto* Found = State->PreparedByRequest.Find(RequestId);
					if (!Found || Found->Context != &Context)
					{
						OutError = ChainFact(TEXT("world_spatial_execution_request_mismatch"),
							Found ? TEXT("The prepared request belongs to a different execution context.") : TEXT("No preflight preparation exists for this execution request."),
							Found ? TEXT("/prepared_request/context_matches") : TEXT("/prepared_request/present"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), RequestId);
						return false;
					}
					if (!ValidateChainTargets(Found->Targets, Targets, OutError, TEXT("world_spatial_execution_request_mismatch"))) return false;
					Prepared = MoveTemp(*Found);
					State->PreparedByRequest.Remove(RequestId);
				}

				FUeAgentWorldSpatialMutationResult MutationResult;
				FUeAgentWorldSpatialExecutionError Error;
				bNoMutationConfirmed = false;
				const bool bMutated = Mutation->TryMutate(
					Contract, Prepared.Request.ToSharedRef(), Targets, Prepared.Fence, Context, MutationResult, Error);
				bNoMutationConfirmed = !bMutated && MutationResult.IsNoMutationConfirmed();
				FUeAgentWorldSpatialExecutionError ResultError;
				const bool bResultValid = MutationResult.TryValidate(ResultError);
				if (!bMutated || !bResultValid)
				{
					if (Error.IsEmpty())
					{
						Error = !bResultValid ? MoveTemp(ResultError) : ChainFact(TEXT("world_spatial_mutation_result_invalid"),
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
						OutError = ChainFact(TEXT("world_spatial_operation_id_duplicate"),
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
				ON_SCOPE_EXIT { SupplementChainTargets(OutError, Targets); };
				if (!Receipt.IsValid())
				{
					OutError = Receipt.GetValidationDiagnostic();
					return false;
				}
				if (Receipt.GetStatus() != EExecutionStatus::AppliedInMemory)
				{
					OutError = ChainFact(TEXT("world_spatial_verification_receipt_invalid"),
						TEXT("Spatial verification requires a receipt with AppliedInMemory status."), TEXT("/receipt/status"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Receipt.GetStatus())),
						MakeShared<FJsonValueNumber>(static_cast<int32>(EExecutionStatus::AppliedInMemory)), Receipt.GetOperationId());
					return false;
				}

				FUeAgentWorldSpatialOperationChain::FState::FExecuted Executed;
				{
					FScopeLock Lock(&State->Mutex);
					auto* Found = State->ExecutedByOperation.Find(Receipt.GetOperationId());
					if (!Found || Found->Prepared.Context != &Context)
					{
						OutError = ChainFact(TEXT("world_spatial_verification_request_mismatch"),
							Found ? TEXT("The execution record belongs to a different verification context.") : TEXT("No execution record exists for this verification receipt."),
							Found ? TEXT("/executed_operation/context_matches") : TEXT("/executed_operation/present"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Receipt.GetOperationId());
						return false;
					}
					if (!ValidateChainTargets(Found->Prepared.Targets, Targets, OutError, TEXT("world_spatial_verification_request_mismatch"))) return false;
					Executed = MoveTemp(*Found);
					State->ExecutedByOperation.Remove(Receipt.GetOperationId());
				}

				FUeAgentWorldSpatialExecutionError Error;
				if (!Settle->TrySettle(Contract, Executed.Mutation, Context, Error))
				{
					OutError = Error.IsEmpty() ? ChainFact(TEXT("world_spatial_settle_failed"),
						TEXT("The settle provider failed without returning a diagnostic."), TEXT("/settle/succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetSettleBarrier()) : MoveTemp(Error);
					return false;
				}
				TSharedPtr<const FUeAgentCanonicalSnapshot> After;
				const bool bRead = Readback->TryRead(
					Contract, Executed.Prepared.Request.ToSharedRef(), FirstReadTarget(Targets),
					EUeAgentWorldSpatialReadPhase::Verification, Context, After, Error);
				if (!bRead || !After.IsValid())
				{
					if (Error.IsEmpty())
						Error = ChainFact(TEXT("world_spatial_verification_readback_missing"),
							bRead ? TEXT("Verification readback succeeded without returning a snapshot.") : TEXT("Verification readback failed without returning a diagnostic."),
							bRead ? TEXT("/readback/snapshot") : TEXT("/readback/succeeded"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetReadbackId());
					ForwardDiagnostic(Error, OutError);
					return false;
				}
				if (!Comparator->TryCompare(
					Contract, Executed.Prepared.Request.ToSharedRef(), *Executed.Prepared.Before, *After, Context, OutOutcome, Error))
				{
					OutError = Error.IsEmpty() ? ChainFact(TEXT("world_spatial_comparison_failed"),
						TEXT("The spatial comparator failed without returning a diagnostic."), TEXT("/comparison/succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetOracleKind()) : MoveTemp(Error);
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
		ON_SCOPE_EXIT { SupplementChainTargets(OutError, Targets); };
		if (Context.GetRequestKind() != EUeAgentRequestKind::Operation)
		{
			OutError = ChainFact(TEXT("world_spatial_context_descriptor_mismatch"),
				TEXT("Spatial operation planning requires an operation request context."), TEXT("/context/request_kind"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(Context.GetRequestKind())),
				MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentRequestKind::Operation)));
			return false;
		}
		if (Context.GetDescriptorIdentity().GetDescriptorId() != Contract.GetDescriptorId())
		{
			OutError = ChainFact(TEXT("world_spatial_context_descriptor_mismatch"),
				TEXT("Operation context descriptor differs from the execution-chain descriptor."), TEXT("/context/descriptor/descriptor_id"),
				MakeShared<FJsonValueString>(Context.GetDescriptorIdentity().GetDescriptorId()), MakeShared<FJsonValueString>(Contract.GetDescriptorId()));
			return false;
		}

		FUeAgentWorldSpatialTypedRequestPtr TypedRequest;
		FUeAgentWorldSpatialExecutionError Error;
		if (!Decoder->TryDecode(
				Contract, CanonicalInput, Context.GetResourceTracker(), TypedRequest, Error,
				InputSubject(Contract, FirstReadTarget(Targets), Context))
			|| !IsTypedRequestExact(Contract, TypedRequest, Error))
		{
			OutError = Error.IsEmpty() ? ChainFact(TEXT("world_spatial_request_decode_failed"),
				TEXT("The spatial request decoder failed without returning a diagnostic."), TEXT("/decoder/succeeded"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetTypedRequestId()) : MoveTemp(Error);
			return false;
		}

		TSharedPtr<const FUeAgentCanonicalSnapshot> Before;
		const bool bRead = Readback->TryRead(
			Contract, TypedRequest.ToSharedRef(), FirstReadTarget(Targets), EUeAgentWorldSpatialReadPhase::Preflight, Context, Before, Error);
		if (!bRead || !Before.IsValid())
		{
			if (Error.IsEmpty())
				Error = ChainFact(TEXT("world_spatial_preflight_readback_missing"),
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
				else Error = ChainFact(TEXT("world_spatial_change_draft_invalid"),
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
				OutError = ChainFact(TEXT("world_spatial_request_already_prepared"),
					TEXT("A preflight preparation already exists for this request."), TEXT("/prepared_request/already_registered"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Context.GetRequestId());
				return false;
			}
			FUeAgentWorldSpatialPreflightFence Fence;
			const bool bNeedsFence = Contract.GetCapabilityId() == TEXT("graph.view.update")
				|| Contract.GetCapabilityId() == TEXT("mesh.transform.bake")
				|| Contract.GetCapabilityId() == TEXT("mesh.pivot.update")
				|| Contract.GetCapabilityId() == TEXT("world.object_align")
				|| Contract.GetCapabilityId() == TEXT("world.object_snap_to_surface")
				|| Contract.GetCapabilityId() == TEXT("viewport.state.restore");
			if (bNeedsFence && !State->Mutation.IsValid())
			{
				OutError = ChainFact(TEXT("world_spatial_mutation_port_missing"),
					TEXT("The mutation port required to capture the preflight fence is unavailable."), TEXT("/mutation_port"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetCapabilityId());
				return false;
			}
			if (bNeedsFence && (!State->Mutation->TryCaptureFence(
				Contract, TypedRequest.ToSharedRef(), Targets, Context, Fence, Error) || !Fence.TryValidate(Error)))
			{
				OutError = Error.IsEmpty() ? ChainFact(TEXT("world_spatial_preflight_fence_failed"),
					TEXT("The preflight fence provider failed without returning a diagnostic."), TEXT("/preflight_fence/capture_succeeded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Contract.GetCapabilityId()) : MoveTemp(Error);
				return false;
			}
			State->PreparedByRequest.Add(Context.GetRequestId(), {TypedRequest, Before, MoveTemp(Fence), Targets, &Context});
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
		FUeAgentQueryBindingError Error;
		if (!FUeAgentDomainQueryProviderRegistry::TryCreate(
				MoveTemp(Registrations), OutRegistry, Error))
		{
			OutError = Error;
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
			State->Mutation = Mutation;
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
		return true;
	}
}
