// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Animation/UeAgentAnimationFolderCommandBridge.h"

#include "Dom/JsonObject.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringApplyProvider.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringExport.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

namespace UeAgentAnimationFolderCommandBridge {
namespace {
const UeAgentAnimationAuthoring::FProfile *
Profile(const UeAgentAnimationAuthoring::EAssetKind Kind, FString &OutError) {
  const UeAgentAnimationAuthoring::FProfile *Result =
      UeAgentAnimationAuthoring::FindProfile(Kind);
  if (!Result)
    OutError = TEXT("animation_authoring_profile_missing");
  return Result;
}

bool ReadRequiredString(const TSharedPtr<FJsonObject> &Params,
                        const TCHAR *Field, FString &OutValue,
                        FString &OutError) {
  if (!Params.IsValid() || !Params->TryGetStringField(Field, OutValue) ||
      OutValue.TrimStartAndEnd().IsEmpty()) {
    OutError = FString(Field) + TEXT("_required");
    return false;
  }
  return true;
}

} // namespace

bool Export(const UeAgentAnimationAuthoring::EAssetKind Kind,
            const TSharedPtr<FJsonObject> &Params,
            TSharedPtr<FJsonObject> &OutData, FString &OutError) {
  FString AssetPath;
  if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, OutError))
    return false;
  const UeAgentAnimationAuthoring::FProfile *AssetProfile =
      Profile(Kind, OutError);
  if (!AssetProfile)
    return false;
  UObject *Asset = UeAgentAnimationAuthoring::LoadAsset(Kind, AssetPath);
  if (!Asset) {
    OutError = AssetProfile->TypeId + TEXT("_not_found");
    return false;
  }
  FString FolderPath;
  if (Params.IsValid())
    Params->TryGetStringField(TEXT("folder_path"), FolderPath);
  if (FolderPath.TrimStartAndEnd().IsEmpty() &&
      !UeAgentAnimationAuthoring::ResolveDefaultWorkspaceFolder(
          *AssetProfile, AssetPath, FolderPath, OutError)) {
    return false;
  }
  FolderPath =
      UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
  FString ExportProfile = TEXT("llm_authoring");
  if (Params.IsValid())
    Params->TryGetStringField(TEXT("profile"), ExportProfile);
  UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
  if (!UeAgentAnimationAuthoring::ExportAssetWorkspace(
          *AssetProfile, *Asset, FolderPath, ExportProfile, Result, OutError)) {
    OutData = Result.ToJsonObject();
    return false;
  }
  OutData = Result.ToJsonObject();
  OutData->SetStringField(
      TEXT("asset_path"),
      UeAgentAnimationAuthoring::NormalizePackagePath(AssetPath));
  return true;
}

bool Apply(const UeAgentAnimationAuthoring::EAssetKind Kind,
           const TSharedPtr<FJsonObject> &Params,
           TSharedPtr<FJsonObject> &OutData, FString &OutError,
           const FString &RequestId) {
  FString FolderPath;
  if (!ReadRequiredString(Params, TEXT("folder_path"), FolderPath, OutError))
    return false;
  const UeAgentAnimationAuthoring::FProfile *AssetProfile =
      Profile(Kind, OutError);
  if (!AssetProfile)
    return false;
  FString AssetPath;
  if (Params.IsValid())
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
  FolderPath =
      UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
  UeAgentAnimationAuthoring::FAssetApplyResult Result;
  const bool bApplied = UeAgentAnimationAuthoring::ApplyAssetWorkspace(
      *AssetProfile, FolderPath, AssetPath, Result, RequestId);
  OutData = Result.ToJsonObject();
  if (!bApplied)
    OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
        Result.ErrorCode, Result.ErrorDetail,
        TEXT("animation_folder_apply_failed"));
  return bApplied;
}

bool Validate(const UeAgentAnimationAuthoring::EAssetKind Kind,
              const TSharedPtr<FJsonObject> &Params,
              TSharedPtr<FJsonObject> &OutData, FString &OutError) {
  FString FolderPath;
  if (!ReadRequiredString(Params, TEXT("folder_path"), FolderPath, OutError))
    return false;
  const UeAgentAnimationAuthoring::FProfile *AssetProfile =
      Profile(Kind, OutError);
  if (!AssetProfile)
    return false;
  FString AssetPath;
  if (Params.IsValid())
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
  FolderPath =
      UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
  UeAgentAnimationAuthoring::FWorkspaceInspectResult Result;
  const bool bValid = UeAgentAnimationAuthoring::InspectAssetWorkspace(
      *AssetProfile, FolderPath, AssetPath, Result);
  OutData = Result.ToJsonObject();
  if (!bValid)
    OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
        Result.ErrorCode, Result.ErrorDetail,
        TEXT("animation_folder_validation_failed"));
  return bValid;
}
} // namespace UeAgentAnimationFolderCommandBridge
