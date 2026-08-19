// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UeAgentPCGAuthoring
{
	inline constexpr const TCHAR* DomainId = TEXT("pcg");
	inline constexpr const TCHAR* ApplyProviderId = TEXT("pcg.graph.apply");
	inline constexpr const TCHAR* AssetInputContractId = TEXT("uai.apply_input.pcg.graph");
	inline constexpr const TCHAR* AssetWorkspaceContractId = TEXT("uai.apply_workspace.pcg.graph");
	inline constexpr const TCHAR* StateAccessorId = TEXT("pcg.graph.state");
	inline constexpr const TCHAR* FactoryDefaultProviderId = TEXT("pcg.graph.factory");

	inline constexpr const TCHAR* GraphSemanticId = TEXT("uai.semantic.pcg.graph");
	inline constexpr const TCHAR* ExportTargetSemanticId = TEXT("uai.semantic.pcg.export.target");
	inline constexpr const TCHAR* ExportNodeSemanticId = TEXT("uai.semantic.pcg.export.node");
	inline constexpr const TCHAR* ExportNodePropertySemanticId = TEXT("uai.semantic.pcg.export.node_property");
	inline constexpr const TCHAR* ExportPinSemanticId = TEXT("uai.semantic.pcg.export.pin");
	inline constexpr const TCHAR* ExportEdgeSemanticId = TEXT("uai.semantic.pcg.export.edge");
	inline constexpr const TCHAR* ExportParametersSemanticId = TEXT("uai.semantic.pcg.export.parameters");

	inline constexpr const TCHAR* GraphIdentityModelId = TEXT("pcg_graph_identity");
	inline constexpr const TCHAR* NodeIdentityModelId = TEXT("pcg_node_identity");
	inline constexpr const TCHAR* NodePropertyIdentityModelId = TEXT("pcg_node_property_identity");
	inline constexpr const TCHAR* PinIdentityModelId = TEXT("pcg_pin_identity");
	inline constexpr const TCHAR* EdgeIdentityModelId = TEXT("pcg_edge_identity");
	inline constexpr const TCHAR* ParametersIdentityModelId = TEXT("pcg_parameters_identity");

	UEAGENTINTERFACE_API bool RegisterDescriptors(FString& OutError);
}
