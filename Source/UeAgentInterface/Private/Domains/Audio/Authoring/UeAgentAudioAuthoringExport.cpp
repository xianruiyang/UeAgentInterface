// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Audio/Authoring/UeAgentAudioAuthoringExport.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDocuments.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringState.h"
#include "Misc/Paths.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"

namespace UeAgentAudioAuthoring {
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
    UeAgentExportWorkspaceWriter::FWorkspaceResult &OutResult) {
  auto& OutError = OutResult.Diagnostic;
  OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
  OutError.Reset();
  TArray<UeAgentAuthoringState::FReadRequest> Requests;
  if (!BuildAssetExportSnapshotReadRequests(Profile, Asset, Requests, OutError))
    return false;
  UeAgentAuthoringState::FCaptureResult Capture;
  if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)) {
    OutError = Capture.Diagnostic;
    return false;
  }
  return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(UE::UAI::HjsonWorkspace::ResolveTargetCompositionKind(
          Profile.TargetKind, Profile.TypeId), ExportProfileId, Capture.Snapshot, WorkspaceRoot, OutResult);
}
} // namespace UeAgentAudioAuthoring
