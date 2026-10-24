#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

class UOptimusDeformer;

namespace UeAgentDeformerAuthoring
{
	inline constexpr const TCHAR* DomainId = TEXT("deformer");
	inline constexpr const TCHAR* TargetKind = TEXT("deformer_graph");
	inline constexpr const TCHAR* CompositionKind = TEXT("ue.deformer_graph.asset");
	inline constexpr const TCHAR* StateAccessorId = TEXT("deformer.graph.state");
	inline constexpr const TCHAR* GraphSemanticId = TEXT("uai.semantic.deformer_graph.graph");
	inline constexpr const TCHAR* ExportTargetSemanticId = TEXT("uai.semantic.deformer_graph.export.target");
	inline constexpr const TCHAR* NodeSemanticId = TEXT("uai.semantic.deformer_graph.node");
	inline constexpr const TCHAR* EdgeSemanticId = TEXT("uai.semantic.deformer_graph.edge");
	inline constexpr const TCHAR* NodePropertySemanticId = TEXT("uai.semantic.deformer_graph.node_property");
	inline constexpr const TCHAR* NodePropertyIdentityModelId = TEXT("deformer_node_property_identity");
	inline constexpr const TCHAR* GraphIdentityModelId = TEXT("deformer_graph_identity");
	inline constexpr const TCHAR* NodeIdentityModelId = TEXT("deformer_graph_node_identity");
	inline constexpr const TCHAR* EdgeIdentityModelId = TEXT("deformer_graph_edge_identity");
	inline constexpr const TCHAR* ExportContractId = TEXT("uai.export.deformer_graph");
	inline constexpr const TCHAR* ProjectionId = TEXT("uai.projection.deformer_graph");

	UEAGENTINTERFACE_API bool RegisterAuthoring(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildExportSnapshotReadRequests(
		UOptimusDeformer& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FString& IdentityAssetPath = FString(),
		const FString& IdentityObjectPath = FString());
	UEAGENTINTERFACE_API bool ExportWorkspace(
		UOptimusDeformer& Asset,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult);
}
