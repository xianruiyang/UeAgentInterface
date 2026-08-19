#include "DomainAdapters/PcgAiNodeGraph/UeAgentPcgAiNodeGraphContract.h"

namespace UE::UAI::DomainAdapters::PcgAiNodeGraph
{
	FPcgAiNodeGraphPortFamilyContract::FPcgAiNodeGraphPortFamilyContract(
		FString InFamilyId,
		TArray<FString> InRequiredIdentityFacets,
		TArray<FString> InRequiredStages,
		FString InBarrierKind,
		FString InOracleKind)
		: FamilyId(MoveTemp(InFamilyId))
		, RequiredIdentityFacets(MoveTemp(InRequiredIdentityFacets))
		, RequiredStages(MoveTemp(InRequiredStages))
		, BarrierKind(MoveTemp(InBarrierKind))
		, OracleKind(MoveTemp(InOracleKind))
	{
	}

	FPcgAiNodeGraphCapabilityContract::FPcgAiNodeGraphCapabilityContract(
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

	const TArray<FPcgAiNodeGraphPortFamilyContract>&
	GetPcgAiNodeGraphPortFamilyContracts()
	{
		static const TArray<FPcgAiNodeGraphPortFamilyContract> Contracts = {
			{TEXT("asset_lifecycle"),
				{TEXT("asset_kind"), TEXT("asset_path"), TEXT("native_class")},
				{TEXT("apply"), TEXT("refresh"), TEXT("independent_capture")},
				TEXT("refresh"), TEXT("reloaded_ue_asset_identity")},
			{TEXT("behavior_tree_graph_mutation"),
				{TEXT("graph_owner"), TEXT("graph_node_guid"),
					TEXT("runtime_node_class"), TEXT("relation_identity")},
				{TEXT("preflight"), TEXT("apply"), TEXT("compile_settle"),
					TEXT("independent_capture")},
				TEXT("compile"),
				TEXT("native_behavior_tree_graph_and_runtime_nodes")},
			{TEXT("blackboard_schema_mutation"),
				{TEXT("blackboard_owner"), TEXT("entry_name"),
					TEXT("key_type_class"), TEXT("entry_ownership")},
				{TEXT("preflight"), TEXT("apply"), TEXT("refresh"),
					TEXT("independent_capture")},
				TEXT("refresh"), TEXT("native_blackboard_entries_and_key_ids")},
			{TEXT("eqs_graph_mutation"),
				{TEXT("query_owner"), TEXT("graph_node_guid"),
					TEXT("generator_or_test_class"), TEXT("ordering_identity")},
				{TEXT("preflight"), TEXT("apply"), TEXT("compile_settle"),
					TEXT("independent_capture")},
				TEXT("compile"), TEXT("native_eqs_graph_and_query_options")},
			{TEXT("smart_object_definition_mutation"),
				{TEXT("definition_owner"), TEXT("slot_guid"),
					TEXT("behavior_or_data_type"), TEXT("attachment_identity")},
				{TEXT("preflight"), TEXT("apply"), TEXT("validate_settle"),
					TEXT("independent_capture")},
				TEXT("validate"),
				TEXT("native_smart_object_slots_and_definition_data")},
			{TEXT("state_tree_hierarchy_mutation"),
				{TEXT("tree_owner"), TEXT("state_or_node_guid"),
					TEXT("native_payload_type"), TEXT("relation_identity")},
				{TEXT("preflight"), TEXT("apply"), TEXT("compile_settle"),
					TEXT("independent_capture")},
				TEXT("compile"),
				TEXT("native_state_tree_editor_data_and_compiled_data")},
			{TEXT("pcg_graph_topology_mutation"),
				{TEXT("graph_owner"), TEXT("native_node_object"),
					TEXT("settings_class"), TEXT("pin_identity"),
					TEXT("edge_identity")},
				{TEXT("preflight"), TEXT("apply"), TEXT("compile_settle"),
					TEXT("independent_capture")},
				TEXT("compile"),
				TEXT("native_pcg_nodes_pins_edges_and_compiled_graph")},
			{TEXT("pcg_parameter_binding_mutation"),
				{TEXT("graph_or_instance_owner"), TEXT("property_guid"),
					TEXT("property_name"), TEXT("property_type")},
				{TEXT("preflight"), TEXT("apply"), TEXT("refresh"),
					TEXT("independent_capture")},
				TEXT("refresh"),
				TEXT("native_instanced_property_bag_and_instance_source")},
			{TEXT("pcg_component_binding_mutation"),
				{TEXT("explicit_actor_identity"), TEXT("component_identity"),
					TEXT("graph_identity"), TEXT("generation_policy")},
				{TEXT("preflight"), TEXT("apply"), TEXT("refresh"),
					TEXT("independent_capture")},
				TEXT("refresh"),
				TEXT("native_pcg_component_configuration")},
			{TEXT("pcg_data_asset_mutation"),
				{TEXT("data_asset_identity"), TEXT("data_type"),
					TEXT("source_identity"), TEXT("content_revision")},
				{TEXT("preflight"), TEXT("apply"), TEXT("validate_settle"),
					TEXT("independent_capture")},
				TEXT("validate"), TEXT("reloaded_pcg_data_asset_content")},
		};
		return Contracts;
	}

	const TArray<FPcgAiNodeGraphCapabilityContract>&
	GetPcgAiNodeGraphCapabilityContracts()
	{
		static const TArray<FPcgAiNodeGraphCapabilityContract> Contracts = {
			{TEXT("ai.behavior_tree.asset.create"), TEXT("uai.operation.ai.behavior_tree.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("ai.behavior_tree.graph.apply"), TEXT("uai.operation.ai.behavior_tree.graph.apply"), TEXT("behavior_tree_graph_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("ai.blackboard.asset.create"), TEXT("uai.operation.ai.blackboard.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("ai.blackboard.schema.apply"), TEXT("uai.operation.ai.blackboard.schema.apply"), TEXT("blackboard_schema_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("ai.eqs.asset.create"), TEXT("uai.operation.ai.eqs.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("ai.eqs.graph.apply"), TEXT("uai.operation.ai.eqs.graph.apply"), TEXT("eqs_graph_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("ai.smart_object_definition.asset.create"), TEXT("uai.operation.ai.smart_object_definition.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("ai.smart_object_definition.apply"), TEXT("uai.operation.ai.smart_object_definition.apply"), TEXT("smart_object_definition_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("ai.state_tree.asset.create"), TEXT("uai.operation.ai.state_tree.asset.create"), TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("ai.state_tree.hierarchy.apply"), TEXT("uai.operation.ai.state_tree.hierarchy.apply"), TEXT("state_tree_hierarchy_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("pcg.graph.apply"), TEXT("uai.operation.pcg.graph.apply"), TEXT("pcg_graph_topology_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("pcg.graph_instance.apply"), TEXT("uai.operation.pcg.graph_instance.apply"), TEXT("pcg_parameter_binding_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("pcg.graph_parameters.apply"), TEXT("uai.operation.pcg.graph_parameters.apply"), TEXT("pcg_parameter_binding_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("pcg.component.apply"), TEXT("uai.operation.pcg.component.apply"), TEXT("pcg_component_binding_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("pcg.data_asset.apply"), TEXT("uai.operation.pcg.data_asset.apply"), TEXT("pcg_data_asset_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("pcg.data_asset.create_from_level"), TEXT("uai.operation.pcg.data_asset.create_from_level"), TEXT("pcg_data_asset_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("pcg.component.world_binding.apply"), TEXT("uai.operation.pcg.component.world_binding.apply"), TEXT("pcg_component_binding_mutation"), TEXT("operation"), TEXT("write"), 1},
		};
		return Contracts;
	}
}
