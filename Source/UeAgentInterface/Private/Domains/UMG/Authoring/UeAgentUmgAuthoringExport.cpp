// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/UMG/Authoring/UeAgentUmgAuthoringExport.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "WidgetBlueprint.h"

namespace UeAgentUmgAuthoring
{
	bool ExportAssetWorkspace(
		UWidgetBlueprint& Asset,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult)
	{
  auto& OutError = OutResult.Diagnostic;
		OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
		OutError.Reset();

		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!BuildAssetExportSnapshotReadRequests(Asset, Requests, OutError)) return false;

		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
		{
			OutError = Capture.Diagnostic;
			return false;
		}

		return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(TEXT("ue.widget_blueprint.asset"), ProfileId, Capture.Snapshot, WorkspaceRoot, OutResult);
	}
}
