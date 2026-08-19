#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphContract.h"

namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph
{
	FMaterialNiagaraGraphPortFamilyContract::
	FMaterialNiagaraGraphPortFamilyContract(
		FString InFamilyId,
		TArray<FString> InRequiredIdentityFacets,
		TArray<FString> InRequiredStages,
		FString InOracleKind)
		: FamilyId(MoveTemp(InFamilyId))
		, RequiredIdentityFacets(MoveTemp(InRequiredIdentityFacets))
		, RequiredStages(MoveTemp(InRequiredStages))
		, OracleKind(MoveTemp(InOracleKind))
	{
	}

	FMaterialNiagaraGraphCapabilityContract::
	FMaterialNiagaraGraphCapabilityContract(
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

	const TArray<FMaterialNiagaraGraphPortFamilyContract>&
	GetMaterialNiagaraGraphPortFamilyContracts()
	{
		static const TArray<FMaterialNiagaraGraphPortFamilyContract>
			Contracts = {
				{TEXT("asset_lifecycle"),
					{TEXT("asset_kind"), TEXT("asset_path"),
						TEXT("class_or_source_identity")},
					{TEXT("apply"), TEXT("refresh"),
						TEXT("independent_capture")},
					TEXT("reloaded_ue_asset_identity")},
				{TEXT("material_graph_mutation"),
					{TEXT("graph_owner"),
						TEXT("expression_guid_or_builtin_role"),
						TEXT("pin_identity"), TEXT("link_identity")},
					{TEXT("preflight"), TEXT("apply"),
						TEXT("compile_settle"),
						TEXT("independent_capture")},
					TEXT("native_material_expression_collection")},
				{TEXT("material_instance_parameter_mutation"),
					{TEXT("parameter_name"),
						TEXT("parameter_association"),
						TEXT("parameter_index")},
					{TEXT("preflight"), TEXT("apply"), TEXT("refresh"),
						TEXT("independent_capture")},
					TEXT("native_material_parameter_info")},
				{TEXT("niagara_stack_mutation"),
					{TEXT("emitter_handle_or_version"), TEXT("stage_key"),
						TEXT("module_node_guid"),
						TEXT("ordering_identity")},
					{TEXT("preflight"), TEXT("apply"),
						TEXT("compile_settle"),
						TEXT("independent_capture")},
					TEXT("native_niagara_parameter_map_chain")},
				{TEXT("niagara_compound_input"),
					{TEXT("stage_key"), TEXT("emitter_handle_id"),
						TEXT("module_node_guid"), TEXT("input_name")},
					{TEXT("delegate_validate"), TEXT("apply"),
						TEXT("compile_settle"),
						TEXT("independent_capture")},
					TEXT("D/NiagaraCompound")},
				{TEXT("niagara_renderer_event_parameter_mutation"),
					{TEXT("emitter_handle_or_version"),
						TEXT("native_item_identity"),
						TEXT("type_identity")},
					{TEXT("preflight"), TEXT("apply"), TEXT("refresh"),
						TEXT("independent_capture")},
					TEXT("native_niagara_emitter_and_parameter_store")},
				{TEXT("compile_refresh_settle"),
					{TEXT("target_identity"), TEXT("subject_generation")},
					{TEXT("request"), TEXT("wait"), TEXT("inspect"),
						TEXT("post_barrier_capture")},
					TEXT("native_compile_and_stack_diagnostics")},
			};
		return Contracts;
	}

	const TArray<FMaterialNiagaraGraphCapabilityContract>&
	GetMaterialNiagaraGraphCapabilityContracts()
	{
		static const TArray<FMaterialNiagaraGraphCapabilityContract>
			Contracts = {
				{TEXT("material.asset.create"),
					TEXT("uai.operation.material.asset.create"),
					TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
				{TEXT("material.graph.apply"),
					TEXT("uai.operation.material.graph.apply"),
					TEXT("material_graph_mutation"), TEXT("operation"), TEXT("write"), 1},
				{TEXT("material.function.graph.apply"),
					TEXT("uai.operation.material.function.graph.apply"),
					TEXT("material_graph_mutation"), TEXT("operation"), TEXT("write"), 1},
				{TEXT("material_instance.asset.create"),
					TEXT("uai.operation.material_instance.asset.create"),
					TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
				{TEXT("material_instance.parameters.apply"),
					TEXT("uai.operation.material_instance.parameters.apply"),
					TEXT("material_instance_parameter_mutation"), TEXT("operation"), TEXT("write"), 1},
				{TEXT("niagara.emitter.asset.create"),
					TEXT("uai.operation.niagara.emitter.asset.create"),
					TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
				{TEXT("niagara.system.asset.create"),
					TEXT("uai.operation.niagara.system.asset.create"),
					TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
				{TEXT("niagara.asset.duplicate"),
					TEXT("uai.operation.niagara.asset.duplicate"),
					TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
				{TEXT("niagara.stack.reorder"),
					TEXT("uai.operation.niagara.stack.reorder"),
					TEXT("niagara_stack_mutation"), TEXT("operation"), TEXT("write"), 1},
				{TEXT("niagara.stack.issue_fix"),
					TEXT("uai.operation.niagara.stack.issue_fix"),
					TEXT("niagara_stack_mutation"), TEXT("operation"), TEXT("write"), 1},
				{TEXT("niagara.compound.apply"),
					TEXT("uai.operation.niagara.compound.apply"),
					TEXT("niagara_compound_input"), TEXT("operation"), TEXT("write"), 1},
				{TEXT("niagara.renderer_event_parameter.apply"),
					TEXT("uai.operation.niagara.renderer_event_parameter.apply"),
					TEXT("niagara_renderer_event_parameter_mutation"),
					TEXT("operation"), TEXT("write"), 1},
				{TEXT("niagara.compile_refresh_settle"),
					TEXT("uai.operation.niagara.compile_refresh_settle"),
					TEXT("compile_refresh_settle"), TEXT("operation"), TEXT("write"), 1},
			};
		return Contracts;
	}

	FString GetMaterialNiagaraGraphCompoundDelegateAdapterId()
	{
		return TEXT("D/NiagaraCompound");
	}

	FString GetMaterialNiagaraGraphCompoundFailurePolicy()
	{
		return TEXT("propagate_exact_failure");
	}
}
