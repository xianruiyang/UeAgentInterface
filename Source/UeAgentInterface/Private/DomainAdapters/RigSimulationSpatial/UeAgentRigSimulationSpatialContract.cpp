#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialContract.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	FUeAgentRigSimulationSpatialPortFamilyContract::
	FUeAgentRigSimulationSpatialPortFamilyContract(
		FString InFamilyId,
		TArray<int32> InDimensions,
		TArray<FString> InAcceptedTimeStates,
		FString InSubjectPolicy,
		FString InSemanticFramePolicy,
		FString InWriteFramePolicy,
		FString InReadFramePolicy,
		FString InVerifyFramePolicy,
		FString InConversionChain,
		TArray<FString> InRequiredContextFacets,
		TArray<FString> InForbiddenImplicitFacets,
		FString InOracleKind)
		: FamilyId(MoveTemp(InFamilyId))
		, Dimensions(MoveTemp(InDimensions))
		, AcceptedTimeStates(MoveTemp(InAcceptedTimeStates))
		, SubjectPolicy(MoveTemp(InSubjectPolicy))
		, SemanticFramePolicy(MoveTemp(InSemanticFramePolicy))
		, WriteFramePolicy(MoveTemp(InWriteFramePolicy))
		, ReadFramePolicy(MoveTemp(InReadFramePolicy))
		, VerifyFramePolicy(MoveTemp(InVerifyFramePolicy))
		, ConversionChain(MoveTemp(InConversionChain))
		, RequiredContextFacets(MoveTemp(InRequiredContextFacets))
		, ForbiddenImplicitFacets(MoveTemp(InForbiddenImplicitFacets))
		, OracleKind(MoveTemp(InOracleKind))
	{
	}

	const FString& FUeAgentRigSimulationSpatialPortFamilyContract::
	GetFamilyId() const { return FamilyId; }
	const FString& FUeAgentRigSimulationSpatialPortFamilyContract::
	GetTemporalContractId() const { return TemporalContractId; }
	const FString& FUeAgentRigSimulationSpatialPortFamilyContract::
	GetSpaceContractId() const { return SpaceContractId; }
	const TArray<int32>& FUeAgentRigSimulationSpatialPortFamilyContract::
	GetDimensions() const { return Dimensions; }
	const TArray<FString>& FUeAgentRigSimulationSpatialPortFamilyContract::
	GetAcceptedTimeStates() const { return AcceptedTimeStates; }
	const FString& FUeAgentRigSimulationSpatialPortFamilyContract::
	GetSubjectPolicy() const { return SubjectPolicy; }
	const FString& FUeAgentRigSimulationSpatialPortFamilyContract::
	GetSemanticFramePolicy() const { return SemanticFramePolicy; }
	const FString& FUeAgentRigSimulationSpatialPortFamilyContract::
	GetWriteFramePolicy() const { return WriteFramePolicy; }
	const FString& FUeAgentRigSimulationSpatialPortFamilyContract::
	GetReadFramePolicy() const { return ReadFramePolicy; }
	const FString& FUeAgentRigSimulationSpatialPortFamilyContract::
	GetVerifyFramePolicy() const { return VerifyFramePolicy; }
	const FString& FUeAgentRigSimulationSpatialPortFamilyContract::
	GetConversionChain() const { return ConversionChain; }
	const TArray<FString>& FUeAgentRigSimulationSpatialPortFamilyContract::
	GetRequiredContextFacets() const { return RequiredContextFacets; }
	const TArray<FString>& FUeAgentRigSimulationSpatialPortFamilyContract::
	GetForbiddenImplicitFacets() const { return ForbiddenImplicitFacets; }
	const FString& FUeAgentRigSimulationSpatialPortFamilyContract::
	GetOracleKind() const { return OracleKind; }

	FUeAgentRigSimulationSpatialCapabilityContract::
	FUeAgentRigSimulationSpatialCapabilityContract(
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

	const FString& FUeAgentRigSimulationSpatialCapabilityContract::
	GetCapabilityId() const { return CapabilityId; }
	const FString& FUeAgentRigSimulationSpatialCapabilityContract::
	GetDescriptorId() const { return DescriptorId; }
	int32 FUeAgentRigSimulationSpatialCapabilityContract::
	GetVersion() const { return Version; }
	const FString& FUeAgentRigSimulationSpatialCapabilityContract::
	GetOperationKind() const { return OperationKind; }
	const FString& FUeAgentRigSimulationSpatialCapabilityContract::
	GetEffectClass() const { return EffectClass; }
	const FString& FUeAgentRigSimulationSpatialCapabilityContract::
	GetPortFamilyId() const { return PortFamilyId; }
	const FString& FUeAgentRigSimulationSpatialCapabilityContract::
	GetTemporalContractId() const { return TemporalContractId; }
	const FString& FUeAgentRigSimulationSpatialCapabilityContract::
	GetSpaceContractId() const { return SpaceContractId; }

	const TArray<FUeAgentRigSimulationSpatialPortFamilyContract>&
	GetRigSimulationSpatialPortFamilyContracts()
	{
		static const TArray<FUeAgentRigSimulationSpatialPortFamilyContract>
			Contracts = {
				{
					TEXT("skeletal_attachment"),
					{3},
					{TEXT("authored")},
					TEXT("explicit_skeleton_socket_parent_bone_and_optional_preview_mesh_refs"),
					TEXT("declared_socket_or_parent_bone_frame"),
					TEXT("socket_relative_to_exact_parent_bone"),
					TEXT("independent_socket_and_parent_bone_asset_readback"),
					TEXT("declared_socket_or_skeletal_component_frame"),
					TEXT("Fsemantic -> explicit parent-bone hierarchy conversion -> socket local write -> independent socket and parent readback -> explicit skeletal conversion -> Fverify"),
					{TEXT("skeleton_ref"), TEXT("source_revision"),
					 TEXT("stable_bone_refs"), TEXT("space_contract")},
					{TEXT("preview_mesh_as_frame_truth"),
					 TEXT("current_skeletal_mesh_editor"), TEXT("gizmo_axis"),
					 TEXT("field_name_frame_inference")},
					TEXT("independent_skeleton_socket_and_parent_bone_readback"),
				},
				{
					TEXT("rig_bake"),
					{3},
					{TEXT("authored")},
					TEXT("explicit_sequence_binding_rig_control_bone_and_target_asset_refs"),
					TEXT("declared_control_global_parent_or_skeletal_component_frame"),
					TEXT("target_animation_bone_local_or_control_local_key_frame"),
					TEXT("independent_target_tracks_controls_and_sequence_range_readback"),
					TEXT("declared_source_semantic_frame_at_exact_authored_time"),
					TEXT("authored source clock -> C4 exact temporal conversion -> bake sample clock; Fsemantic -> explicit control-parent-component chain -> native bake -> independent target key readback -> explicit inverse chain -> Fverify"),
					{TEXT("sequence_ref"), TEXT("binding_ref"),
					 TEXT("rig_ref"), TEXT("target_ref"),
					 TEXT("source_revision"), TEXT("temporal_domain"),
					 TEXT("space_contract")},
					{TEXT("editor_world"), TEXT("open_sequence_editor"),
					 TEXT("current_control_rig"), TEXT("implicit_display_rate"),
					 TEXT("captured_time_as_authored")},
					TEXT("independent_target_animation_or_control_key_readback"),
				},
				{
					TEXT("ik_retarget"),
					{3},
					{TEXT("authored"), TEXT("evaluation")},
					TEXT("explicit_source_target_skeleton_chain_goal_pose_and_retargeter_refs"),
					TEXT("declared_source_or_target_bone_control_component_or_retarget_global_frame"),
					TEXT("exact_ik_goal_pose_chain_or_retarget_definition_native_frame"),
					TEXT("independent_ik_hierarchy_goal_pose_chain_and_generated_asset_readback"),
					TEXT("declared_source_or_target_semantic_frame_without_cross_subject_shortcut"),
					TEXT("source semantic -> source bone/component chain -> explicit retarget mapping -> target component/bone chain -> native write or generation -> independent target readback -> Fverify"),
					{TEXT("source_subject_ref"), TEXT("target_subject_ref"),
					 TEXT("stable_bone_or_control_refs"),
					 TEXT("source_revision"), TEXT("space_contract")},
					{TEXT("preview_mesh_as_frame_truth"),
					 TEXT("auto_map_as_semantic_truth"),
					 TEXT("auto_align_as_semantic_truth"),
					 TEXT("current_ik_editor"), TEXT("gizmo_axis")},
					TEXT("independent_source_target_definition_pose_and_asset_readback"),
				},
				{
					TEXT("simulation_definition"),
					{3},
					{TEXT("authored")},
					TEXT("explicit_physics_asset_body_constraint_endpoint_and_material_refs"),
					TEXT("declared_constraint_or_body_semantic_frame"),
					TEXT("endpoint_body_local_constraint_frames_and_typed_nonspatial_settings"),
					TEXT("independent_physics_asset_body_pair_constraint_frame_and_material_readback"),
					TEXT("declared_constraint_frame_with_each_endpoint_kept_distinct"),
					TEXT("endpoint A semantic -> body-A local; endpoint B semantic -> body-B local; native definition write -> independent endpoint/frame readback -> explicit per-endpoint conversion -> Fverify"),
					{TEXT("physics_owner_ref"),
					 TEXT("stable_body_or_endpoint_refs"),
					 TEXT("source_revision"), TEXT("space_contract")},
					{TEXT("current_world"), TEXT("first_component"),
					 TEXT("default_constraint_actor"), TEXT("bare_world_axis"),
					 TEXT("field_name_frame_inference")},
					TEXT("independent_physics_asset_or_constraint_definition_readback"),
				},
				{
					TEXT("simulation_observation"),
					{3},
					{TEXT("authored"), TEXT("evaluation"), TEXT("simulation")},
					TEXT("explicit_physics_component_deformer_subject_or_pcg_data_subject_and_session"),
					TEXT("declared_component_import_data_or_simulation_frame"),
					TEXT("none_for_query_or_explicit_owned_simulation_session_for_probe"),
					TEXT("native_component_import_data_or_simulation_frame_with_generation_and_tick_evidence"),
					TEXT("declared_subject_frame_at_the_same_evaluation_or_simulation_point"),
					TEXT("declared temporal point -> C4 exact evaluation/simulation conversion; native spatial observation -> explicit import/data/component/simulation conversion -> Fverify; captured time only proves readback freshness"),
					{TEXT("subject_refs"), TEXT("source_revision"),
					 TEXT("temporal_domain"), TEXT("space_contract")},
					{TEXT("current_world"), TEXT("current_preview"),
					 TEXT("first_sample"), TEXT("implicit_30fps"),
					 TEXT("bare_pcg_point_transform"),
					 TEXT("captured_time_as_simulation_time"),
					 TEXT("screenshot_only")},
					TEXT("independent_component_asset_or_generated_data_readback"),
				},
			};
		return Contracts;
	}

	const TArray<FUeAgentRigSimulationSpatialCapabilityContract>&
	GetRigSimulationSpatialCapabilityContracts()
	{
		static const TArray<FUeAgentRigSimulationSpatialCapabilityContract>
			Contracts = {
				{TEXT("rig.skeleton.preview_mesh.bind"), TEXT("uai.operation.rig.skeleton.preview_mesh.bind"), 1, TEXT("operation"), TEXT("write"), TEXT("skeletal_attachment")},
				{TEXT("rig.skeleton.socket.update"), TEXT("uai.operation.rig.skeleton.socket.update"), 1, TEXT("operation"), TEXT("write"), TEXT("skeletal_attachment")},
				{TEXT("rig.control.bake.animation"), TEXT("uai.operation.rig.control.bake.animation"), 1, TEXT("operation"), TEXT("write"), TEXT("rig_bake")},
				{TEXT("rig.control.bake.controls"), TEXT("uai.operation.rig.control.bake.controls"), 1, TEXT("operation"), TEXT("write"), TEXT("rig_bake")},
				{TEXT("rig.ik.definition.update"), TEXT("uai.operation.rig.ik.definition.update"), 1, TEXT("operation"), TEXT("write"), TEXT("ik_retarget")},
				{TEXT("rig.retarget.definition.update"), TEXT("uai.operation.rig.retarget.definition.update"), 1, TEXT("operation"), TEXT("write"), TEXT("ik_retarget")},
				{TEXT("rig.retarget.pose.update"), TEXT("uai.operation.rig.retarget.pose.update"), 1, TEXT("operation"), TEXT("write"), TEXT("ik_retarget")},
				{TEXT("rig.retarget.pose.auto_align"), TEXT("uai.operation.rig.retarget.pose.auto_align"), 1, TEXT("operation"), TEXT("write"), TEXT("ik_retarget")},
				{TEXT("rig.retarget.assets.generate"), TEXT("uai.operation.rig.retarget.assets.generate"), 1, TEXT("operation"), TEXT("write"), TEXT("ik_retarget")},
				{TEXT("physics.material.update"), TEXT("uai.operation.physics.material.update"), 1, TEXT("operation"), TEXT("write"), TEXT("simulation_definition")},
				{TEXT("physics.asset.structure.replace"), TEXT("uai.operation.physics.asset.structure.replace"), 1, TEXT("operation"), TEXT("write"), TEXT("simulation_definition")},
				{TEXT("physics.constraint.update"), TEXT("uai.operation.physics.constraint.update"), 1, TEXT("operation"), TEXT("write"), TEXT("simulation_definition")},
				{TEXT("physics.simulation.probe"), TEXT("uai.operation.physics.simulation.probe"), 1, TEXT("operation"), TEXT("session_control"), TEXT("simulation_observation")},
				{TEXT("deformer.geometry.sample"), TEXT("uai.query.deformer.geometry.sample"), 1, TEXT("query"), TEXT("read_only"), TEXT("simulation_observation")},
				{TEXT("deformer.training_inputs.validate"), TEXT("uai.query.deformer.training_inputs.validate"), 1, TEXT("query"), TEXT("read_only"), TEXT("simulation_observation")},
				{TEXT("deformer.evaluation.sample"), TEXT("uai.query.deformer.evaluation.sample"), 1, TEXT("query"), TEXT("read_only"), TEXT("simulation_observation")},
				{TEXT("pcg.generated_spatial_data.read"), TEXT("uai.query.pcg.generated_spatial_data.read"), 1, TEXT("query"), TEXT("read_only"), TEXT("simulation_observation")},
				{TEXT("pcg.partition.spatial_state.read"), TEXT("uai.query.pcg.partition.spatial_state.read"), 1, TEXT("query"), TEXT("read_only"), TEXT("simulation_observation")},
			};
		return Contracts;
	}
}
