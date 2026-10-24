// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class UObject;

namespace UeAgentBlueprintAuthoring
{
	inline constexpr const TCHAR* DomainId = TEXT("blueprint");
	inline constexpr const TCHAR* ApplyProviderId = TEXT("blueprint.apply");
	inline constexpr const TCHAR* AssetInputContractId = TEXT("uai.apply_input.blueprint.asset");
	inline constexpr const TCHAR* AssetWorkspaceContractId = TEXT("uai.apply_workspace.blueprint.asset");
	inline constexpr const TCHAR* StateAccessorId = TEXT("blueprint.state");
	inline constexpr const TCHAR* FactoryDefaultProviderId = TEXT("blueprint.factory");

	inline constexpr const TCHAR* AssetSemanticId = TEXT("uai.semantic.blueprint.asset");
	inline constexpr const TCHAR* ExportTargetSemanticId = TEXT("uai.semantic.blueprint.export.target");
	inline constexpr const TCHAR* ParentClassSemanticId = TEXT("uai.semantic.blueprint.parent_class");
	inline constexpr const TCHAR* InterfaceSemanticId = TEXT("uai.semantic.blueprint.interface");
	inline constexpr const TCHAR* VariableSemanticId = TEXT("uai.semantic.blueprint.member.variable");
	inline constexpr const TCHAR* DelegateSemanticId = TEXT("uai.semantic.blueprint.member.delegate");
	inline constexpr const TCHAR* ComponentSemanticId = TEXT("uai.semantic.blueprint.component");
	inline constexpr const TCHAR* GraphSemanticId = TEXT("uai.semantic.blueprint.graph");
	inline constexpr const TCHAR* NodeSemanticId = TEXT("uai.semantic.blueprint.node");
	inline constexpr const TCHAR* PinDefaultSemanticId = TEXT("uai.semantic.blueprint.pin_default");
	inline constexpr const TCHAR* EdgeSemanticId = TEXT("uai.semantic.blueprint.edge");
	inline constexpr const TCHAR* ClassDefaultPropertySemanticId = TEXT("uai.semantic.blueprint.class_default.property");
	inline constexpr const TCHAR* ComponentPropertySemanticId = TEXT("uai.semantic.blueprint.component.property");
	inline constexpr const TCHAR* NodePropertySemanticId = TEXT("uai.semantic.blueprint.node.property");

	inline constexpr const TCHAR* AssetIdentityModelId = TEXT("blueprint_asset_identity");
	inline constexpr const TCHAR* InterfaceIdentityModelId = TEXT("blueprint_interface_identity");
	inline constexpr const TCHAR* MemberIdentityModelId = TEXT("blueprint_member_identity");
	inline constexpr const TCHAR* ComponentIdentityModelId = TEXT("blueprint_component_identity");
	inline constexpr const TCHAR* GraphIdentityModelId = TEXT("blueprint_graph_identity");
	inline constexpr const TCHAR* NodeIdentityModelId = TEXT("blueprint_node_identity");
	inline constexpr const TCHAR* PinDefaultIdentityModelId = TEXT("blueprint_pin_default_identity");
	inline constexpr const TCHAR* EdgeIdentityModelId = TEXT("blueprint_edge_identity");
	inline constexpr const TCHAR* ClassDefaultPropertyIdentityModelId = TEXT("blueprint_class_default_property_identity");
	inline constexpr const TCHAR* ComponentPropertyIdentityModelId = TEXT("blueprint_component_property_identity");
	inline constexpr const TCHAR* NodePropertyIdentityModelId = TEXT("blueprint_node_property_identity");

	UEAGENTINTERFACE_API bool RegisterDescriptors(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API FString GetAssetKind(const UObject* Object);
	UEAGENTINTERFACE_API FString GetAssetSemanticId(const UObject* Object);
	UEAGENTINTERFACE_API FString GetAssetIdentityModelId(const UObject* Object);
	UEAGENTINTERFACE_API FString GetAssetTargetKind(const UObject* Object);
}
