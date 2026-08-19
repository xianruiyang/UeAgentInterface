// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialAuthoringExport.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringDescriptors.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

namespace UeAgentMaterialAuthoring
{
	bool ExportAssetWorkspace(
		UObject& Asset,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		FString& OutError)
	{
		OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
		OutError.Reset();

		const FString AssetKind = GetAssetKind(&Asset);
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!BuildAssetExportSnapshotReadRequests(Asset, Requests, OutError)) return false;
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
		{
			OutError = Capture.ErrorCode + TEXT(":") + Capture.ErrorDetail;
			return false;
		}

		return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(
			AssetKind == TEXT("material_function")
				? TEXT("ue.material_function.asset")
				: AssetKind == TEXT("material_instance")
					? TEXT("ue.material_instance.asset")
				: TEXT("ue.material.asset"),
			ProfileId,
			Capture.Snapshot,
			WorkspaceRoot,
			OutResult,
			OutError);
	}
}
