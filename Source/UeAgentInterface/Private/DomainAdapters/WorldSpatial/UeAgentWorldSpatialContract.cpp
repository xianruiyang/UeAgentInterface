#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialContract.h"

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	FUeAgentWorldSpatialPortFamilyContract::
	FUeAgentWorldSpatialPortFamilyContract(
		FString InFamilyId,
		TArray<int32> InDimensions,
		FString InSubjectPolicy,
		FString InSemanticFramePolicy,
		FString InWriteFramePolicy,
		FString InReadFramePolicy,
		FString InVerifyFramePolicy,
		FString InDimensionBridge,
		FString InConversionChain,
		TArray<FString> InRequiredContextFacets)
		: FamilyId(MoveTemp(InFamilyId))
		, Dimensions(MoveTemp(InDimensions))
		, SubjectPolicy(MoveTemp(InSubjectPolicy))
		, SemanticFramePolicy(MoveTemp(InSemanticFramePolicy))
		, WriteFramePolicy(MoveTemp(InWriteFramePolicy))
		, ReadFramePolicy(MoveTemp(InReadFramePolicy))
		, VerifyFramePolicy(MoveTemp(InVerifyFramePolicy))
		, DimensionBridge(MoveTemp(InDimensionBridge))
		, ConversionChain(MoveTemp(InConversionChain))
		, RequiredContextFacets(MoveTemp(InRequiredContextFacets))
	{
	}

	const FString& FUeAgentWorldSpatialPortFamilyContract::GetFamilyId() const { return FamilyId; }
	const FString& FUeAgentWorldSpatialPortFamilyContract::GetSpaceContractId() const { return SpaceContractId; }
	const TArray<int32>& FUeAgentWorldSpatialPortFamilyContract::GetDimensions() const { return Dimensions; }
	const FString& FUeAgentWorldSpatialPortFamilyContract::GetSubjectPolicy() const { return SubjectPolicy; }
	const FString& FUeAgentWorldSpatialPortFamilyContract::GetSemanticFramePolicy() const { return SemanticFramePolicy; }
	const FString& FUeAgentWorldSpatialPortFamilyContract::GetWriteFramePolicy() const { return WriteFramePolicy; }
	const FString& FUeAgentWorldSpatialPortFamilyContract::GetReadFramePolicy() const { return ReadFramePolicy; }
	const FString& FUeAgentWorldSpatialPortFamilyContract::GetVerifyFramePolicy() const { return VerifyFramePolicy; }
	const FString& FUeAgentWorldSpatialPortFamilyContract::GetDimensionBridge() const { return DimensionBridge; }
	const FString& FUeAgentWorldSpatialPortFamilyContract::GetConversionChain() const { return ConversionChain; }
	const TArray<FString>& FUeAgentWorldSpatialPortFamilyContract::GetRequiredContextFacets() const { return RequiredContextFacets; }

	FUeAgentWorldSpatialCapabilityContract::
	FUeAgentWorldSpatialCapabilityContract(
		FString InCapabilityId,
		FString InDescriptorId,
		int32 InVersion,
		FString InOperationKind,
		FString InEffectClass,
		FString InPortFamilyId)
		: CapabilityId(MoveTemp(InCapabilityId))
		, DescriptorId(MoveTemp(InDescriptorId))
		, Version(InVersion)
		, OperationKind(MoveTemp(InOperationKind))
		, EffectClass(MoveTemp(InEffectClass))
		, PortFamilyId(MoveTemp(InPortFamilyId))
	{
	}

	const FString& FUeAgentWorldSpatialCapabilityContract::GetCapabilityId() const { return CapabilityId; }
	const FString& FUeAgentWorldSpatialCapabilityContract::GetDescriptorId() const { return DescriptorId; }
	int32 FUeAgentWorldSpatialCapabilityContract::GetVersion() const { return Version; }
	const FString& FUeAgentWorldSpatialCapabilityContract::GetOperationKind() const { return OperationKind; }
	const FString& FUeAgentWorldSpatialCapabilityContract::GetEffectClass() const { return EffectClass; }
	const FString& FUeAgentWorldSpatialCapabilityContract::GetPortFamilyId() const { return PortFamilyId; }
	const FString& FUeAgentWorldSpatialCapabilityContract::GetSpaceContractId() const { return SpaceContractId; }

	const TArray<FUeAgentWorldSpatialPortFamilyContract>&
	GetWorldSpatialPortFamilyContracts()
	{
		static const TArray<FUeAgentWorldSpatialPortFamilyContract> Contracts = {
			{
				TEXT("graph_view"), {2},
				TEXT("explicit_graph_editor_context_and_graph_ref"),
				TEXT("graph_canvas_canonical"),
				TEXT("graph_editor_view_local"),
				TEXT("same_graph_editor_view_local"),
				TEXT("graph_canvas_canonical"),
				TEXT("none"),
				TEXT("Fsemantic graph canvas -> C4 2D conversion -> frozen graph editor view -> read same editor view -> C4 2D conversion -> Fverify"),
				{TEXT("graph_editor_context"), TEXT("graph_ref")},
			},
			{
				TEXT("graph_layout"), {2},
				TEXT("explicit_graph_ref_and_stable_node_refs"),
				TEXT("graph_local_canonical"),
				TEXT("graph_node_integer_local"),
				TEXT("graph_node_integer_local"),
				TEXT("graph_local_canonical"),
				TEXT("none"),
				TEXT("Fsemantic graph local -> C4 2D conversion -> node positions -> independent node position readback -> C4 2D conversion -> Fverify"),
				{TEXT("asset"), TEXT("graph_ref")},
			},
			{
				TEXT("object_transform"), {3},
				TEXT("explicit_actor_component_mesh_element_or_surface_ref"),
				TEXT("declared_subject_frame"),
				TEXT("capability_declared_world_component_or_asset_local"),
				TEXT("native_subject_frame_with_evidence"),
				TEXT("declared_subject_frame"),
				TEXT("none"),
				TEXT("Fsemantic -> C4 3D canonical -> explicit parent/component/pivot chain -> native write -> independent geometry or transform readback -> C4 3D canonical -> Fverify"),
				{TEXT("world"), TEXT("target_refs")},
			},
			{
				TEXT("viewport_projection"), {2, 3},
				TEXT("explicit_viewport_camera_screen_or_world_ray_and_target_refs"),
				TEXT("screen_pixel_or_declared_world_ray"),
				TEXT("viewport_selection_session_when_applicable"),
				TEXT("viewport_camera_world_and_optional_component_local"),
				TEXT("same_subject_same_dimension_canonical_value"),
				TEXT("camera_deprojection"),
				TEXT("screen_pixel 2D -> viewport_normalized 2D -> camera_deprojection evidence -> new world_ray 3D -> optional world_hit 3D -> optional C4 component_local 3D"),
				{TEXT("viewport"), TEXT("camera")},
			},
			{
				TEXT("viewport_session_state"), {2, 3},
				TEXT("explicit_viewport_client_ref"),
				TEXT("camera_world_and_typed_session_flags_kept_separate"),
				TEXT("same_viewport_client"),
				TEXT("same_viewport_client"),
				TEXT("camera_world_for_spatial_values_and_exact_typed_flags_for_session_values"),
				TEXT("separate_values_only"),
				TEXT("freeze viewport client -> C4 camera spatial values plus typed nonspatial flags -> native session write -> read same viewport client -> compare each value only in its own subject and dimension"),
				{TEXT("viewport")},
			},
		};
		return Contracts;
	}

	const TArray<FUeAgentWorldSpatialCapabilityContract>&
	GetWorldSpatialCapabilityContracts()
	{
		static const TArray<FUeAgentWorldSpatialCapabilityContract> Contracts = {
			{TEXT("graph.view.read"), TEXT("uai.query.graph.view.read"), 1, TEXT("query"), TEXT("read_only"), TEXT("graph_view")},
			{TEXT("graph.view.update"), TEXT("uai.operation.graph.view.update"), 1, TEXT("operation"), TEXT("session_control"), TEXT("graph_view")},
			{TEXT("graph.layout.apply"), TEXT("uai.operation.graph.layout.apply"), 1, TEXT("operation"), TEXT("write"), TEXT("graph_layout")},
			{TEXT("world.object_transform.read"), TEXT("uai.query.world.object_transform.read"), 1, TEXT("query"), TEXT("read_only"), TEXT("object_transform")},
			{TEXT("world.object_align"), TEXT("uai.operation.world.object_align"), 1, TEXT("operation"), TEXT("write"), TEXT("object_transform")},
			{TEXT("world.object_snap_to_surface"), TEXT("uai.operation.world.object_snap_to_surface"), 1, TEXT("operation"), TEXT("write"), TEXT("object_transform")},
			{TEXT("mesh.transform.bake"), TEXT("uai.operation.mesh.transform.bake"), 1, TEXT("operation"), TEXT("write"), TEXT("object_transform")},
			{TEXT("mesh.pivot.update"), TEXT("uai.operation.mesh.pivot.update"), 1, TEXT("operation"), TEXT("write"), TEXT("object_transform")},
			{TEXT("world.transform_candidates.evaluate"), TEXT("uai.query.world.transform_candidates.evaluate"), 1, TEXT("query"), TEXT("read_only"), TEXT("object_transform")},
			{TEXT("viewport.ray.deproject"), TEXT("uai.query.viewport.ray.deproject"), 1, TEXT("query"), TEXT("read_only"), TEXT("viewport_projection")},
			{TEXT("viewport.ray.trace"), TEXT("uai.query.viewport.ray.trace"), 1, TEXT("query"), TEXT("read_only"), TEXT("viewport_projection")},
			{TEXT("viewport.actor.pick"), TEXT("uai.query.viewport.actor.pick"), 1, TEXT("query"), TEXT("read_only"), TEXT("viewport_projection")},
			{TEXT("viewport.actor.select"), TEXT("uai.operation.viewport.actor.select"), 1, TEXT("operation"), TEXT("session_control"), TEXT("viewport_projection")},
			{TEXT("mesh.element.select_by_ray"), TEXT("uai.operation.mesh.element.select_by_ray"), 1, TEXT("operation"), TEXT("session_control"), TEXT("viewport_projection")},
			{TEXT("viewport.pixel_scale.read"), TEXT("uai.query.viewport.pixel_scale.read"), 1, TEXT("query"), TEXT("read_only"), TEXT("viewport_projection")},
			{TEXT("viewport.camera.frame"), TEXT("uai.operation.viewport.camera.frame"), 1, TEXT("operation"), TEXT("session_control"), TEXT("viewport_session_state")},
			{TEXT("viewport.state.read"), TEXT("uai.query.viewport.state.read"), 1, TEXT("query"), TEXT("read_only"), TEXT("viewport_session_state")},
			{TEXT("viewport.state.restore"), TEXT("uai.operation.viewport.state.restore"), 1, TEXT("operation"), TEXT("session_control"), TEXT("viewport_session_state")},
			{TEXT("viewport.warning.list"), TEXT("uai.query.viewport.warning.list"), 1, TEXT("query"), TEXT("read_only"), TEXT("viewport_session_state")},
			{TEXT("viewport.view_mode.list"), TEXT("uai.query.viewport.view_mode.list"), 1, TEXT("query"), TEXT("read_only"), TEXT("viewport_session_state")},
			{TEXT("viewport.instance.list"), TEXT("uai.query.viewport.instance.list"), 1, TEXT("query"), TEXT("read_only"), TEXT("viewport_session_state")},
			{TEXT("viewport.state.update"), TEXT("uai.operation.viewport.state.update"), 1, TEXT("operation"), TEXT("session_control"), TEXT("viewport_session_state")},
		};
		return Contracts;
	}
}
