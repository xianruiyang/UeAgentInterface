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
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		FString& OutError)
	{
		OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
		OutError.Reset();
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!BuildAssetExportSnapshotReadRequests(Graph, Requests, OutError)) return false;
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
		{
			OutError = Capture.ErrorCode + TEXT(":") + Capture.ErrorDetail;
			return false;
		}
		return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(
			TEXT("ue.pcg_graph.asset"),
			ProfileId,
			Capture.Snapshot,
			WorkspaceRoot,
			OutResult,
			OutError);
	}
}
