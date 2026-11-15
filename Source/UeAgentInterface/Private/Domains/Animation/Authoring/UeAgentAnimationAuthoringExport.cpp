// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringExport.h"

#include "Animation/AnimBlueprint.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringState.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "UObject/Package.h"
#include "UObject/Class.h"

namespace UeAgentAnimationAuthoring {
bool ResolveDefaultWorkspaceFolder(const FProfile &Profile,
                                   const FString &AssetPath,
                                   FString &OutWorkspaceRoot,
                                   UE::UAI::Contract::FDiagnostic& OutError) {
  const FString DomainRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
      FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), Profile.TypeId));
  return UeAgentAuthoringWorkspacePaths::ResolveAssetWorkspaceFolder(
      DomainRoot, NormalizePackagePath(AssetPath), OutWorkspaceRoot, OutError);
}

bool ExportAssetWorkspace(
    const FProfile &Profile, UObject &Asset, const FString &WorkspaceRoot,
    const FString &ExportProfileId,
    UeAgentExportWorkspaceWriter::FWorkspaceResult &OutResult,
    UE::UAI::HjsonWorkspace::FPreparedWorkspaceExport* PreparedExport) {
  auto& OutError = OutResult.Diagnostic;
  OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
  OutError.Reset();
  TArray<UeAgentAuthoringState::FReadRequest> Requests;
  if (!BuildAssetExportSnapshotReadRequests(Profile, Asset, Requests, OutError))
    return false;
  const bool bAnimBlueprint =
      Profile.Kind == EAssetKind::AnimBlueprint;
  if (bAnimBlueprint) {
    UAnimBlueprint *AnimBlueprint = Cast<UAnimBlueprint>(&Asset);
    if (!AnimBlueprint) {
      OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_anim_blueprint_export_type_invalid"),
          TEXT("Animation Blueprint export requires an AnimBlueprint asset."), TEXT("export_capture"));
      OutError.SourceKind = TEXT("native_result");
      OutError.InternalContext = Asset.GetPathName();
      OutError.ActualValue = MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName());
      OutError.ExpectedValue = MakeShared<FJsonValueString>(UAnimBlueprint::StaticClass()->GetPathName());
      return false;
    }
    if (!UeAgentBlueprintAuthoring::
            AppendReusableBlueprintExportSnapshotReadRequests(
                *AnimBlueprint, Requests, OutError,
                Asset.GetOutermost()->GetName())) {
      return false;
    }
  }
  UeAgentAuthoringState::FCaptureResult Capture;
  if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)) {
    OutError = Capture.Diagnostic;
    return false;
  }
  if (bAnimBlueprint) {
    return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(TEXT("ue.anim_blueprint.asset"), ExportProfileId, Capture.Snapshot, WorkspaceRoot, OutResult, PreparedExport);
  }
  return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(UE::UAI::HjsonWorkspace::ResolveTargetCompositionKind(
          Profile.TargetKind, Profile.TypeId), ExportProfileId, Capture.Snapshot, WorkspaceRoot, OutResult, PreparedExport);
}
} // namespace UeAgentAnimationAuthoring
