// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/PCG/Authoring/UeAgentPCGAuthoringExport.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

namespace UeAgentPCGAuthoring
{
	bool ExportAssetWorkspace(
		UPCGGraph& Graph,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult)
	{
  auto& OutError = OutResult.Diagnostic;
		OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
		OutError.Reset();
		UeAgentAuthoringState::FStateSnapshot Snapshot;
		if (!CaptureAssetExportSnapshot(Graph, Snapshot, OutError)) return false;
		return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(TEXT("ue.pcg_graph.asset"), ProfileId, Snapshot, WorkspaceRoot, OutResult);
	}
}
