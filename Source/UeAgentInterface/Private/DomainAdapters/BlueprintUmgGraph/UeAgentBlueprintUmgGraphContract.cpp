#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphContract.h"

namespace UE::UAI::DomainAdapters::BlueprintUmgGraph
{
	FBlueprintUmgGraphPortFamilyContract::
	FBlueprintUmgGraphPortFamilyContract(
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

	FBlueprintUmgGraphCapabilityContract::
	FBlueprintUmgGraphCapabilityContract(
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

	const TArray<FBlueprintUmgGraphPortFamilyContract>&
	GetBlueprintUmgGraphPortFamilyContracts()
	{
		static const TArray<FBlueprintUmgGraphPortFamilyContract> Contracts = {
			{TEXT("asset_lifecycle"),
				{TEXT("target_identity"), TEXT("target_kind"),
					TEXT("parent_class_identity")},
				{TEXT("apply"), TEXT("compile_or_refresh"), TEXT("settle"),
					TEXT("independent_readback")},
				TEXT("independent_loaded_asset_readback")},
			{TEXT("graph_mutation"),
				{TEXT("target_identity"), TEXT("graph_identity"),
					TEXT("item_identity"), TEXT("pin_identity"),
					TEXT("relation_identity")},
				{TEXT("apply"), TEXT("compile"), TEXT("settle"),
					TEXT("independent_readback")},
				TEXT("independent_graph_node_pin_and_link_readback")},
			{TEXT("hierarchy_mutation"),
				{TEXT("target_identity"), TEXT("tree_identity"),
					TEXT("item_identity"), TEXT("parent_identity"),
					TEXT("ordering_identity")},
				{TEXT("apply"), TEXT("refresh"), TEXT("compile"),
					TEXT("settle"), TEXT("independent_readback")},
				TEXT("independent_scs_or_widget_tree_readback")},
			{TEXT("binding_mutation"),
				{TEXT("target_identity"), TEXT("binding_identity"),
					TEXT("source_identity"), TEXT("target_identity_ref")},
				{TEXT("apply"), TEXT("compile"), TEXT("settle"),
					TEXT("independent_readback")},
				TEXT("independent_delegate_or_property_binding_readback")},
			{TEXT("event_discovery"),
				{TEXT("target_identity"), TEXT("owner_item_identity"),
					TEXT("owner_class_identity")},
				{TEXT("independent_readback")},
				TEXT("native_blueprint_assignable_delegate_enumeration")},
			{TEXT("compile_settle"),
				{TEXT("target_identity"), TEXT("target_generation")},
				{TEXT("compile_or_refresh"), TEXT("settle"),
					TEXT("independent_readback")},
				TEXT("compile_status_plus_distinct_post_barrier_readback")},
		};
		return Contracts;
	}

	const TArray<FBlueprintUmgGraphCapabilityContract>&
	GetBlueprintUmgGraphCapabilityContracts()
	{
		static const TArray<FBlueprintUmgGraphCapabilityContract> Contracts = {
			{TEXT("blueprint.asset.create"),
				TEXT("uai.operation.blueprint.asset.create"),
				TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("blueprint.authoring.apply"),
				TEXT("uai.operation.blueprint.authoring.apply"),
				TEXT("graph_mutation"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("blueprint.component.events.discover"),
				TEXT("uai.query.blueprint.component.events.discover"),
				TEXT("event_discovery"), TEXT("query"), TEXT("read_only"), 1},
			{TEXT("umg.asset.create"),
				TEXT("uai.operation.umg.asset.create"),
				TEXT("asset_lifecycle"), TEXT("operation"), TEXT("write"), 1},
			{TEXT("umg.widget.events.discover"),
				TEXT("uai.query.umg.widget.events.discover"),
				TEXT("event_discovery"), TEXT("query"), TEXT("read_only"), 1},
		};
		return Contracts;
	}
}
