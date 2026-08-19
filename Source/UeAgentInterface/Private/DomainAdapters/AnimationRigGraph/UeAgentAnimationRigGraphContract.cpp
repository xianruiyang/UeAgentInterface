#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphContract.h"

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	FAnimationRigGraphPortFamilyContract::FAnimationRigGraphPortFamilyContract(
		FString InFamilyId,
		TArray<FString> InRequiredIdentityFacets,
		TArray<FString> InRequiredStages,
		FString InBarrierKind,
		FString InOracleKind,
		FString InAvailabilityPolicy)
		: FamilyId(MoveTemp(InFamilyId))
		, RequiredIdentityFacets(MoveTemp(InRequiredIdentityFacets))
		, RequiredStages(MoveTemp(InRequiredStages))
		, BarrierKind(MoveTemp(InBarrierKind))
		, OracleKind(MoveTemp(InOracleKind))
		, AvailabilityPolicy(MoveTemp(InAvailabilityPolicy))
	{
	}

	FAnimationRigGraphCapabilityContract::FAnimationRigGraphCapabilityContract(
		FString InCapabilityId,
		FString InDescriptorId,
		FString InPortFamilyId,
		FString InOperationKind,
		FString InEffectClass,
		const int32 InVersion)
		: CapabilityId(MoveTemp(InCapabilityId))
		, DescriptorId(MoveTemp(InDescriptorId))
		, PortFamilyId(MoveTemp(InPortFamilyId))
		, OperationKind(MoveTemp(InOperationKind))
		, EffectClass(MoveTemp(InEffectClass))
		, Version(InVersion)
	{
	}

	const TArray<FAnimationRigGraphPortFamilyContract>&
	GetAnimationRigGraphPortFamilyContracts()
	{
		static const TArray<FAnimationRigGraphPortFamilyContract> Contracts = {
			{TEXT("asset_lifecycle"), {TEXT("asset_kind"), TEXT("asset_path"), TEXT("native_class")}, {TEXT("apply"), TEXT("refresh"), TEXT("independent_capture")}, TEXT("refresh"), TEXT("reloaded_ue_asset_identity"), TEXT("required")},
			{TEXT("anim_blueprint_graph_mutation"), {TEXT("blueprint_owner"), TEXT("graph_guid"), TEXT("node_guid"), TEXT("carrier_and_runtime_type"), TEXT("pin_and_link_identity"), TEXT("layer_identity")}, {TEXT("preflight"), TEXT("apply"), TEXT("compile_settle"), TEXT("independent_capture")}, TEXT("compile"), TEXT("native_anim_blueprint_graph_generated_class_and_compile_status"), TEXT("required")},
			{TEXT("blend_space_definition_mutation"), {TEXT("blend_space_owner"), TEXT("axis_native_key"), TEXT("sample_native_composite"), TEXT("animation_identity"), TEXT("skeleton_identity")}, {TEXT("preflight"), TEXT("apply"), TEXT("validate_settle"), TEXT("independent_capture")}, TEXT("validate"), TEXT("native_blend_parameters_samples_and_skeleton_binding"), TEXT("required")},
			{TEXT("skeleton_definition_mutation"), {TEXT("skeleton_owner"), TEXT("bone_native_key"), TEXT("relation_kind"), TEXT("source_identity"), TEXT("target_identity")}, {TEXT("preflight"), TEXT("apply"), TEXT("refresh"), TEXT("independent_capture")}, TEXT("refresh"), TEXT("native_reference_skeleton_compatibility_and_virtual_bones"), TEXT("required")},
			{TEXT("control_rig_hierarchy_graph_mutation"), {TEXT("control_rig_owner"), TEXT("rig_element_key"), TEXT("parent_relation"), TEXT("control_type"), TEXT("rigvm_node_pin_link_identity")}, {TEXT("preflight"), TEXT("apply"), TEXT("compile_settle"), TEXT("independent_capture")}, TEXT("compile"), TEXT("native_control_rig_hierarchy_rigvm_and_compile_log"), TEXT("required")},
			{TEXT("control_rig_shape_library_mutation"), {TEXT("library_owner"), TEXT("shape_name"), TEXT("mesh_identity"), TEXT("material_identity"), TEXT("ordering_identity")}, {TEXT("preflight"), TEXT("apply"), TEXT("refresh"), TEXT("independent_capture")}, TEXT("refresh"), TEXT("native_control_rig_shape_definitions"), TEXT("required")},
			{TEXT("deformer_graph_topology_mutation"), {TEXT("deformer_owner"), TEXT("optimus_graph_object"), TEXT("optimus_node_object"), TEXT("pin_identity"), TEXT("link_identity")}, {TEXT("preflight"), TEXT("apply"), TEXT("compile_settle"), TEXT("independent_capture")}, TEXT("compile"), TEXT("native_optimus_graph_nodes_pins_links_and_status"), TEXT("required")},
			{TEXT("deformer_source_binding_mutation"), {TEXT("asset_owner"), TEXT("concrete_native_class"), TEXT("binding_native_key"), TEXT("property_type"), TEXT("referenced_asset_identity")}, {TEXT("preflight"), TEXT("apply"), TEXT("validate_settle"), TEXT("independent_capture")}, TEXT("validate"), TEXT("concrete_native_typed_binding_readback"), TEXT("conditional_concrete_native_class")},
			{TEXT("mesh_deformer_collection_mutation"), {TEXT("collection_owner"), TEXT("entry_asset_identity"), TEXT("entry_kind"), TEXT("ordering_identity")}, {TEXT("preflight"), TEXT("apply"), TEXT("validate_settle"), TEXT("independent_capture")}, TEXT("validate"), TEXT("native_mesh_deformer_collection_entries"), TEXT("required")},
			{TEXT("ml_deformer_configuration_mutation"), {TEXT("model_owner"), TEXT("concrete_model_class"), TEXT("skeletal_mesh_identity"), TEXT("animation_identity"), TEXT("target_cache_identity"), TEXT("deformer_graph_identity")}, {TEXT("preflight"), TEXT("apply"), TEXT("validate_settle"), TEXT("independent_capture")}, TEXT("validate"), TEXT("concrete_native_ml_model_training_input_bindings_without_training"), TEXT("conditional_concrete_native_class")},
			{TEXT("ik_rig_definition_mutation"), {TEXT("ik_rig_owner"), TEXT("goal_native_key"), TEXT("solver_type_and_order"), TEXT("retarget_root"), TEXT("chain_native_key_and_endpoints")}, {TEXT("preflight"), TEXT("apply"), TEXT("validate_settle"), TEXT("independent_capture")}, TEXT("validate"), TEXT("native_ik_rig_controller_goals_solvers_and_chains"), TEXT("required")},
			{TEXT("ik_retargeter_definition_mutation"), {TEXT("retargeter_owner"), TEXT("source_and_target_rig_identity"), TEXT("chain_mapping_identity"), TEXT("mapping_operation_identity"), TEXT("settings_scope")}, {TEXT("preflight"), TEXT("apply"), TEXT("validate_settle"), TEXT("independent_capture")}, TEXT("validate"), TEXT("native_ik_retargeter_controller_mapping_and_settings"), TEXT("required")},
		};
		return Contracts;
	}

	const TArray<FAnimationRigGraphCapabilityContract>&
	GetAnimationRigGraphCapabilityContracts()
	{
		static const TArray<FAnimationRigGraphCapabilityContract> Contracts = {
			{TEXT("animation.anim_blueprint.graph.apply"), TEXT("uai.operation.animation.anim_blueprint.graph.apply"), TEXT("anim_blueprint_graph_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.anim_blueprint.asset.create"), TEXT("uai.operation.animation.anim_blueprint.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.anim_blueprint.layer_interface.asset.create"), TEXT("uai.operation.animation.anim_blueprint.layer_interface.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.blend_space.definition.apply"), TEXT("uai.operation.animation.blend_space.definition.apply"), TEXT("blend_space_definition_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.blend_space.asset.create"), TEXT("uai.operation.animation.blend_space.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.skeleton.definition.apply"), TEXT("uai.operation.animation.skeleton.definition.apply"), TEXT("skeleton_definition_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.skeleton.compatibility.apply"), TEXT("uai.operation.animation.skeleton.compatibility.apply"), TEXT("skeleton_definition_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.skeleton.virtual_bone.apply"), TEXT("uai.operation.animation.skeleton.virtual_bone.apply"), TEXT("skeleton_definition_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.control_rig.hierarchy_graph.apply"), TEXT("uai.operation.animation.control_rig.hierarchy_graph.apply"), TEXT("control_rig_hierarchy_graph_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.control_rig.asset.create"), TEXT("uai.operation.animation.control_rig.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.control_rig.shape_library.apply"), TEXT("uai.operation.animation.control_rig.shape_library.apply"), TEXT("control_rig_shape_library_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.control_rig.shape_library.asset.create"), TEXT("uai.operation.animation.control_rig.shape_library.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.deformer_graph.topology.apply"), TEXT("uai.operation.animation.deformer_graph.topology.apply"), TEXT("deformer_graph_topology_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.deformer_graph.asset.create"), TEXT("uai.operation.animation.deformer_graph.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.deformer_source_library.bindings.apply"), TEXT("uai.operation.animation.deformer_source_library.bindings.apply"), TEXT("deformer_source_binding_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.deformer_source_library.asset.create"), TEXT("uai.operation.animation.deformer_source_library.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.mesh_deformer_collection.entries.apply"), TEXT("uai.operation.animation.mesh_deformer_collection.entries.apply"), TEXT("mesh_deformer_collection_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.mesh_deformer_collection.asset.create"), TEXT("uai.operation.animation.mesh_deformer_collection.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.ml_deformer.configuration.apply"), TEXT("uai.operation.animation.ml_deformer.configuration.apply"), TEXT("ml_deformer_configuration_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.ml_deformer.asset.create"), TEXT("uai.operation.animation.ml_deformer.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.ik_retargeter.definition.apply"), TEXT("uai.operation.animation.ik_retargeter.definition.apply"), TEXT("ik_retargeter_definition_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.ik_retargeter.asset.create"), TEXT("uai.operation.animation.ik_retargeter.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.ik_retargeter.settings.apply"), TEXT("uai.operation.animation.ik_retargeter.settings.apply"), TEXT("ik_retargeter_definition_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.ik_rig.definition.auto_generate"), TEXT("uai.operation.animation.ik_rig.definition.auto_generate"), TEXT("ik_rig_definition_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.ik_rig.definition.apply"), TEXT("uai.operation.animation.ik_rig.definition.apply"), TEXT("ik_rig_definition_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("animation.ik_rig.asset.create"), TEXT("uai.operation.animation.ik_rig.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
		};
		return Contracts;
	}
}
