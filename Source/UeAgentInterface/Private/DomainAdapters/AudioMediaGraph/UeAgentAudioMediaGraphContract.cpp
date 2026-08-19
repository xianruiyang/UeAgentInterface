#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphContract.h"

namespace UE::UAI::DomainAdapters::AudioMediaGraph
{
	FAudioMediaGraphPortFamilyContract::FAudioMediaGraphPortFamilyContract(
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

	FAudioMediaGraphCapabilityContract::FAudioMediaGraphCapabilityContract(
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

	const TArray<FAudioMediaGraphPortFamilyContract>&
	GetAudioMediaGraphPortFamilyContracts()
	{
		static const TArray<FAudioMediaGraphPortFamilyContract> Contracts = {
			{TEXT("asset_lifecycle"), {TEXT("asset_kind"), TEXT("asset_path"), TEXT("native_class")}, {TEXT("apply"), TEXT("refresh"), TEXT("independent_capture")}, TEXT("refresh"), TEXT("reloaded_native_asset_identity")},
			{TEXT("metasound_document_graph_mutation"), {TEXT("asset_owner"), TEXT("frontend_node_guid"), TEXT("frontend_vertex_guid"), TEXT("class_name_and_version"), TEXT("pin_direction_and_type")}, {TEXT("preflight"), TEXT("apply"), TEXT("frontend_register_compile_settle"), TEXT("independent_capture")}, TEXT("compile"), TEXT("fresh_metasound_frontend_document_and_registration_state")},
			{TEXT("sound_cue_node_graph_mutation"), {TEXT("cue_owner"), TEXT("sound_node_object_path"), TEXT("sound_node_class"), TEXT("root_role"), TEXT("ordered_child_relation")}, {TEXT("preflight"), TEXT("apply"), TEXT("graph_sync_compile_settle"), TEXT("independent_capture")}, TEXT("compile"), TEXT("fresh_sound_cue_nodes_root_children_and_graph_links")},
			{TEXT("texture_graph_topology_mutation"), {TEXT("graph_owner"), TEXT("expression_object_path"), TEXT("ftg_node_id"), TEXT("ftg_pin_id"), TEXT("argument_name_direction_and_type"), TEXT("parameter_alias")}, {TEXT("preflight"), TEXT("apply"), TEXT("validate_evaluate_settle"), TEXT("independent_capture")}, TEXT("evaluate"), TEXT("fresh_texture_graph_nodes_pins_edges_params_and_output_settings")},
			{TEXT("texture_graph_artifact_state"), {TEXT("graph_owner"), TEXT("output_pin_identity"), TEXT("output_name"), TEXT("output_settings"), TEXT("evaluated_value_type")}, {TEXT("capture_after_settle"), TEXT("artifact_state_validate"), TEXT("independent_capture")}, TEXT("artifact_capture"), TEXT("post_evaluation_native_output_value_and_settings")},
		};
		return Contracts;
	}

	const TArray<FAudioMediaGraphCapabilityContract>&
	GetAudioMediaGraphCapabilityContracts()
	{
		static const TArray<FAudioMediaGraphCapabilityContract> Contracts = {
			{TEXT("audio_media.asset.create"), TEXT("uai.audio_media.asset.create.v1"), TEXT("asset_lifecycle"), TEXT("create"), TEXT("compound_effect"), 1},
			{TEXT("metasound.graph.interface.mutate"), TEXT("uai.metasound.graph.interface.mutate.v1"), TEXT("metasound_document_graph_mutation"), TEXT("mutate"), TEXT("compound_effect"), 1},
			{TEXT("metasound.graph.node.mutate"), TEXT("uai.metasound.graph.node.mutate.v1"), TEXT("metasound_document_graph_mutation"), TEXT("mutate"), TEXT("compound_effect"), 1},
			{TEXT("metasound.graph.link.mutate"), TEXT("uai.metasound.graph.link.mutate.v1"), TEXT("metasound_document_graph_mutation"), TEXT("mutate"), TEXT("compound_effect"), 1},
			{TEXT("metasound.graph.default.mutate"), TEXT("uai.metasound.graph.default.mutate.v1"), TEXT("metasound_document_graph_mutation"), TEXT("mutate"), TEXT("compound_effect"), 1},
			{TEXT("metasound.graph.compile_settle"), TEXT("uai.metasound.graph.compile_settle.v1"), TEXT("metasound_document_graph_mutation"), TEXT("settle"), TEXT("compound_effect"), 1},
			{TEXT("sound_cue.graph.node.mutate"), TEXT("uai.sound_cue.graph.node.mutate.v1"), TEXT("sound_cue_node_graph_mutation"), TEXT("mutate"), TEXT("compound_effect"), 1},
			{TEXT("sound_cue.graph.relation.mutate"), TEXT("uai.sound_cue.graph.relation.mutate.v1"), TEXT("sound_cue_node_graph_mutation"), TEXT("mutate"), TEXT("compound_effect"), 1},
			{TEXT("sound_cue.graph.root.mutate"), TEXT("uai.sound_cue.graph.root.mutate.v1"), TEXT("sound_cue_node_graph_mutation"), TEXT("mutate"), TEXT("compound_effect"), 1},
			{TEXT("sound_cue.graph.compile_settle"), TEXT("uai.sound_cue.graph.compile_settle.v1"), TEXT("sound_cue_node_graph_mutation"), TEXT("settle"), TEXT("compound_effect"), 1},
			{TEXT("texture_graph.graph.node.mutate"), TEXT("uai.texture_graph.graph.node.mutate.v1"), TEXT("texture_graph_topology_mutation"), TEXT("mutate"), TEXT("compound_effect"), 1},
			{TEXT("texture_graph.graph.link.mutate"), TEXT("uai.texture_graph.graph.link.mutate.v1"), TEXT("texture_graph_topology_mutation"), TEXT("mutate"), TEXT("compound_effect"), 1},
			{TEXT("texture_graph.graph.parameter.mutate"), TEXT("uai.texture_graph.graph.parameter.mutate.v1"), TEXT("texture_graph_topology_mutation"), TEXT("mutate"), TEXT("compound_effect"), 1},
			{TEXT("texture_graph.graph.validate_evaluate_settle"), TEXT("uai.texture_graph.graph.validate_evaluate_settle.v1"), TEXT("texture_graph_topology_mutation"), TEXT("settle"), TEXT("compound_effect"), 1},
			{TEXT("texture_graph.artifact.capture"), TEXT("uai.texture_graph.artifact.capture.v1"), TEXT("texture_graph_artifact_state"), TEXT("query"), TEXT("compound_query"), 1},
		};
		return Contracts;
	}
}
