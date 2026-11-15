// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringExport.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

namespace UeAgentRiggingAuthoring
{
	bool ExportAssetWorkspace(
		const FProfile& Profile,
		UObject& Asset,
		const FString& WorkspaceRoot,
		const FString& ExportProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		UE::UAI::HjsonWorkspace::FPreparedWorkspaceExport* PreparedExport)
	{
  auto& OutError = OutResult.Diagnostic;
		OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
		OutError.Reset();
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!BuildAssetExportSnapshotReadRequests(Profile, Asset, Requests, OutError)) return false;
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
		{
			OutError = Capture.Diagnostic;
			return false;
		}
		return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(UE::UAI::HjsonWorkspace::ResolveTargetCompositionKind(
				Profile.TargetKind,
				Profile.TypeId), ExportProfileId, Capture.Snapshot, WorkspaceRoot, OutResult, PreparedExport);
	}
}
