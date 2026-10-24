// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UeAgentMaterialAuthoring
{
	inline constexpr const TCHAR* DomainId = TEXT("material");
	inline constexpr const TCHAR* ApplyProviderId = TEXT("material.apply");
	inline constexpr const TCHAR* GraphMutationApplyProviderId = TEXT("material.graph.apply");
	inline constexpr const TCHAR* AssetInputContractId = TEXT("uai.apply_input.material.asset");
	inline constexpr const TCHAR* AssetWorkspaceContractId = TEXT("uai.apply_workspace.material.asset");
	inline constexpr const TCHAR* GraphMutationInputContractId = TEXT("uai.apply_input.material.graph_mutation");
	inline constexpr const TCHAR* GraphMutationWorkspaceContractId = TEXT("uai.apply_workspace.material.graph_mutation");
	inline constexpr const TCHAR* StateAccessorId = TEXT("material.state");
	inline constexpr const TCHAR* FactoryDefaultProviderId = TEXT("material.factory");

	inline constexpr const TCHAR* MaterialSemanticId = TEXT("uai.semantic.material.asset");
	inline constexpr const TCHAR* MaterialFunctionSemanticId = TEXT("uai.semantic.material.function");
	inline constexpr const TCHAR* MaterialInstanceSemanticId = TEXT("uai.semantic.material.instance");
	inline constexpr const TCHAR* ExportTargetSemanticId = TEXT("uai.semantic.material.export.target");
	inline constexpr const TCHAR* ExportFunctionTargetSemanticId = TEXT("uai.semantic.material.export.function_target");
	inline constexpr const TCHAR* ExportInstanceTargetSemanticId = TEXT("uai.semantic.material.export.instance_target");
	inline constexpr const TCHAR* ExportInstanceParameterSemanticId = TEXT("uai.semantic.material.export.instance_parameter");
	inline constexpr const TCHAR* ExportInstanceLayersSemanticId = TEXT("uai.semantic.material.export.instance_layers");
	inline constexpr const TCHAR* MaterialInstanceLayersIdentityModelId = TEXT("material_instance_layers_identity");
	inline constexpr const TCHAR* ExportExpressionSemanticId = TEXT("uai.semantic.material.export.expression");
	inline constexpr const TCHAR* ExportExpressionPropertySemanticId = TEXT("uai.semantic.material.export.expression_property");
	inline constexpr const TCHAR* ExportEdgeSemanticId = TEXT("uai.semantic.material.export.edge");
	inline constexpr const TCHAR* ExportRootInputSemanticId = TEXT("uai.semantic.material.export.root_input");
	inline constexpr const TCHAR* ExportFunctionSettingsSemanticId = TEXT("uai.semantic.material.export.function_settings");
	inline constexpr const TCHAR* ExportGraphSemanticId = TEXT("uai.semantic.material.export.graph");
	inline constexpr const TCHAR* ExportRootNodeSemanticId = TEXT("uai.semantic.material.export.root_node.expression");
	inline constexpr const TCHAR* MaterialIdentityModelId = TEXT("material_asset_identity");
	inline constexpr const TCHAR* MaterialFunctionIdentityModelId = TEXT("material_function_identity");
	inline constexpr const TCHAR* MaterialInstanceIdentityModelId = TEXT("material_instance_identity");
	inline constexpr const TCHAR* MaterialInstanceParameterIdentityModelId = TEXT("material_instance_parameter_identity");
	inline constexpr const TCHAR* MaterialExpressionIdentityModelId = TEXT("material_expression_identity");
	inline constexpr const TCHAR* MaterialExpressionPropertyIdentityModelId = TEXT("material_expression_property_identity");
	inline constexpr const TCHAR* MaterialEdgeIdentityModelId = TEXT("material_edge_identity");
	inline constexpr const TCHAR* MaterialRootInputIdentityModelId = TEXT("material_root_input_identity");
	inline constexpr const TCHAR* MaterialGraphIdentityModelId = TEXT("material_graph_identity");
	inline constexpr const TCHAR* MaterialRootNodeIdentityModelId = TEXT("material_root_node_identity");

	UEAGENTINTERFACE_API bool RegisterDescriptors(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API FString GetAssetKind(const UObject* Object);
	UEAGENTINTERFACE_API FString GetAssetSemanticId(const UObject* Object);
	UEAGENTINTERFACE_API FString GetAssetIdentityModelId(const UObject* Object);
	UEAGENTINTERFACE_API FString GetAssetTargetKind(const UObject* Object);
}
