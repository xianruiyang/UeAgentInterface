// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringExport.h"

#include "Animation/AnimBlueprint.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringState.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Misc/Paths.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"

namespace UeAgentAnimationAuthoring {
bool ResolveDefaultWorkspaceFolder(const FProfile &Profile,
                                   const FString &AssetPath,
                                   FString &OutWorkspaceRoot,
                                   FString &OutError) {
  const FString DomainRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
      FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), Profile.TypeId));
  return UeAgentAuthoringWorkspacePaths::ResolveAssetWorkspaceFolder(
      DomainRoot, NormalizePackagePath(AssetPath), OutWorkspaceRoot, OutError);
}

bool ExportAssetWorkspace(
    const FProfile &Profile, UObject &Asset, const FString &WorkspaceRoot,
    const FString &ExportProfileId,
    UeAgentExportWorkspaceWriter::FWorkspaceResult &OutResult,
    FString &OutError) {
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
      OutError = TEXT("animation_anim_blueprint_export_type_invalid");
      return false;
    }
    Requests.RemoveAll([&Profile](
                           const UeAgentAuthoringState::FReadRequest &Request) {
      return Request.SemanticId == Profile.DocumentSemanticId;
    });
    if (!UeAgentBlueprintAuthoring::
            AppendReusableBlueprintExportSnapshotReadRequests(
                *AnimBlueprint, Requests, OutError,
                Asset.GetOutermost()->GetName())) {
      return false;
    }
  }
  UeAgentAuthoringState::FCaptureResult Capture;
  if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)) {
    OutError = Capture.ErrorCode + TEXT(":") + Capture.ErrorDetail;
    return false;
  }
  if (bAnimBlueprint) {
    return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(
        TEXT("ue.anim_blueprint.asset"), ExportProfileId, Capture.Snapshot,
        WorkspaceRoot, OutResult, OutError);
  }
  return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(
      UE::UAI::HjsonWorkspace::ResolveTargetCompositionKind(
          Profile.TargetKind, Profile.TypeId),
      ExportProfileId, Capture.Snapshot, WorkspaceRoot, OutResult, OutError);
}
} // namespace UeAgentAnimationAuthoring
