// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialAuthoringExport.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringDescriptors.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/ObjectMacros.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

namespace UeAgentMaterialAuthoring
{
	bool TryCaptureMaterialAuthoringState(
		UObject& Asset,
		const FString& ProfileId,
		FMaterialAuthoringCapture& OutCapture,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutCapture = FMaterialAuthoringCapture();
		OutError.Reset();
		// Capture is also used by independent read queries. It must not require
		// an earlier import/export command to initialize its read dependencies.
		if (!RegisterDescriptors(OutError) || !RegisterStateAccessor(OutError))
		{
			return false;
		}
		const FString AssetKind = GetAssetKind(&Asset);
		OutCapture.TargetKind = AssetKind == TEXT("material_function")
			? TEXT("ue.material_function.asset")
			: AssetKind == TEXT("material_instance")
				? TEXT("ue.material_instance.asset")
				: AssetKind == TEXT("material")
					? TEXT("ue.material.asset")
					: FString();
		if (OutCapture.TargetKind.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_authoring_asset_kind_unsupported"),
				TEXT("The native asset is not a supported material, material function or material instance."), TEXT("export_capture"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = Asset.GetPathName();
			OutError.PropertyPath = TEXT("/asset_class");
			OutError.ActualValue = MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName());
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("material, material_function or material_instance"));
			OutError.ActualValuePresent = true;
			OutError.ExpectedValuePresent = true;
			return false;
		}

		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!BuildAssetExportSnapshotReadRequests(Asset, Requests, OutError))
		{
			return false;
		}
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
		{
			OutError = Capture.Diagnostic;
			return false;
		}
		OutCapture.Snapshot = MoveTemp(Capture.Snapshot);
		UE::UAI::ContextResource::FUeAgentResourceTracker Resources =
			UE::UAI::HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		UE::UAI::HjsonWorkspace::FWorkspaceError WorkspaceError;
		if (!UE::UAI::HjsonWorkspace::TryBuildCanonicalAuthoringCapture(
				OutCapture.TargetKind,
				ProfileId,
				OutCapture.Snapshot,
				Resources,
				OutCapture.CanonicalSnapshot,
				OutCapture.Model,
				WorkspaceError))
		{
			OutError = WorkspaceError.GetDiagnostic();
			return false;
		}
		if (!OutCapture.IsValid())
		{
			const bool bKindMissing = OutCapture.TargetKind.IsEmpty();
			const bool bSnapshotMissing = OutCapture.Snapshot.IsEmpty();
			const bool bCanonicalMissing = !OutCapture.CanonicalSnapshot.IsValid();
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_authoring_capture_failed"),
				bKindMissing ? TEXT("The native material capture has no target kind.")
					: bSnapshotMissing ? TEXT("The native material capture contains no snapshot entries.")
					: bCanonicalMissing ? TEXT("The native material capture has no canonical snapshot.")
					: TEXT("The native material capture has no authoring model."), TEXT("export_capture"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = Asset.GetPathName();
			OutError.PropertyPath = bKindMissing ? TEXT("/target_kind_present") : bSnapshotMissing ? TEXT("/snapshot_entries_present")
				: bCanonicalMissing ? TEXT("/canonical_snapshot_present") : TEXT("/authoring_model_present");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			OutError.ActualValuePresent = true;
			OutError.ExpectedValuePresent = true;
			return false;
		}
		return true;
	}

	bool ExportAssetWorkspace(
		UObject& Asset,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult)
	{
  auto& OutError = OutResult.Diagnostic;
		OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
		OutError.Reset();

		FMaterialAuthoringCapture Capture;
		if (!TryCaptureMaterialAuthoringState(
				Asset, ProfileId, Capture, OutError))
		{
			return false;
		}

		return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(Capture.TargetKind, ProfileId, Capture.Snapshot, WorkspaceRoot, OutResult);
	}
}
