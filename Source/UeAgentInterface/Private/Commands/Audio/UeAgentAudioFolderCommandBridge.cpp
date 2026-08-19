// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Audio/UeAgentAudioFolderCommandBridge.h"

#include "Dom/JsonObject.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringApplyProvider.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDocuments.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringExport.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

namespace UeAgentAudioFolderCommandBridge {
namespace {
const UeAgentAudioAuthoring::FProfile *
Profile(const UeAgentAudioAuthoring::EAssetKind Kind, FString &OutError) {
  const UeAgentAudioAuthoring::FProfile *Result =
      UeAgentAudioAuthoring::FindProfile(Kind);
  if (!Result)
    OutError = TEXT("audio_authoring_profile_missing");
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

bool CreateAsset(const UeAgentAudioAuthoring::EAssetKind Kind,
                 const TSharedPtr<FJsonObject> &Params,
                 const TSharedRef<FJsonObject> &SeedDocument,
                 TSharedPtr<FJsonObject> &OutData, FString &OutError,
                 const FString &RequestId) {
  FString AssetPath;
  if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, OutError))
    return false;

  bool bReplaceExisting = false;
  bool bSave = true;
  if (Params.IsValid()) {
    Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
    Params->TryGetBoolField(TEXT("save_after_create"), bSave);
  }

  UeAgentAudioAuthoring::FAssetApplyRequest Request;
  Request.Kind = Kind;
  Request.Operation = bReplaceExisting
                          ? UeAgentApplyInputContract::EOperation::Upsert
                          : UeAgentApplyInputContract::EOperation::Create;
  Request.AssetPath = AssetPath;
  Request.Document = SeedDocument;
  Request.bCompile = Kind == UeAgentAudioAuthoring::EAssetKind::MetaSound;
  Request.bSave = bSave;
  UeAgentAudioAuthoring::FAssetApplyResult Result;
  const bool bApplied =
      UeAgentAudioAuthoring::ApplyAsset(Request, Result, RequestId);
  OutData = Result.ToJsonObject();
  OutData->SetBoolField(TEXT("created_asset"), bApplied);
  if (!bApplied)
    OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
        Result.ErrorCode, Result.ErrorDetail, TEXT("audio_asset_create_failed"));
  return bApplied;
}
} // namespace

bool CreateMetaSound(const TSharedPtr<FJsonObject> &Params,
                     TSharedPtr<FJsonObject> &OutData, FString &OutError,
                     const FString &RequestId) {
  FString AssetType = TEXT("source");
  if (Params.IsValid())
    Params->TryGetStringField(TEXT("asset_type"), AssetType);
  TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
  Settings->SetStringField(TEXT("asset_type"), AssetType);
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  Document->SetObjectField(TEXT("settings"), Settings);
  return CreateAsset(UeAgentAudioAuthoring::EAssetKind::MetaSound, Params,
                     Document, OutData, OutError, RequestId);
}

bool CreateSoundCue(const TSharedPtr<FJsonObject> &Params,
                    TSharedPtr<FJsonObject> &OutData, FString &OutError,
                    const FString &RequestId) {
  return CreateAsset(UeAgentAudioAuthoring::EAssetKind::SoundCue, Params,
                     MakeShared<FJsonObject>(), OutData, OutError, RequestId);
}

bool CreateSoundSubmix(const TSharedPtr<FJsonObject> &Params,
                       TSharedPtr<FJsonObject> &OutData, FString &OutError,
                       const FString &RequestId) {
  FString ClassPath;
  FString ParentPath;
  if (Params.IsValid()) {
    Params->TryGetStringField(TEXT("submix_class"), ClassPath);
    Params->TryGetStringField(TEXT("parent_submix"), ParentPath);
  }
  TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
  if (!ClassPath.IsEmpty())
    Settings->SetStringField(TEXT("submix_class"), ClassPath);
  if (!ParentPath.IsEmpty())
    Settings->SetStringField(TEXT("parent"), ParentPath);
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  Document->SetObjectField(TEXT("settings"), Settings);
  return CreateAsset(UeAgentAudioAuthoring::EAssetKind::SoundSubmix, Params,
                     Document, OutData, OutError, RequestId);
}

bool Export(const UeAgentAudioAuthoring::EAssetKind Kind,
            const TSharedPtr<FJsonObject> &Params,
            TSharedPtr<FJsonObject> &OutData, FString &OutError) {
  FString AssetPath;
  if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, OutError))
    return false;
  const UeAgentAudioAuthoring::FProfile *AssetProfile = Profile(Kind, OutError);
  if (!AssetProfile)
    return false;
  UObject *Asset = UeAgentAudioAuthoring::LoadAsset(Kind, AssetPath);
  if (!Asset) {
    OutError = AssetProfile->TypeId + TEXT("_not_found");
    return false;
  }
  FString FolderPath;
  if (Params.IsValid())
    Params->TryGetStringField(TEXT("folder_path"), FolderPath);
  if (FolderPath.TrimStartAndEnd().IsEmpty() &&
      !UeAgentAudioAuthoring::ResolveDefaultWorkspaceFolder(
          *AssetProfile, AssetPath, FolderPath, OutError)) {
    return false;
  }
  FolderPath =
      UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
  FString ExportProfile = TEXT("llm_authoring");
  if (Params.IsValid())
    Params->TryGetStringField(TEXT("profile"), ExportProfile);
  UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
  if (!UeAgentAudioAuthoring::ExportAssetWorkspace(
          *AssetProfile, *Asset, FolderPath, ExportProfile, Result, OutError)) {
    OutData = Result.ToJsonObject();
    return false;
  }
  OutData = Result.ToJsonObject();
  OutData->SetStringField(
      TEXT("asset_path"),
      UeAgentAudioAuthoring::NormalizePackagePath(AssetPath));
  return true;
}

bool Apply(const UeAgentAudioAuthoring::EAssetKind Kind,
           const TSharedPtr<FJsonObject> &Params,
           TSharedPtr<FJsonObject> &OutData, FString &OutError,
           const FString &RequestId) {
  FString FolderPath;
  if (!ReadRequiredString(Params, TEXT("folder_path"), FolderPath, OutError))
    return false;
  const UeAgentAudioAuthoring::FProfile *AssetProfile = Profile(Kind, OutError);
  if (!AssetProfile)
    return false;
  FString AssetPath;
  if (Params.IsValid())
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
  FolderPath =
      UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
  UeAgentAudioAuthoring::FAssetApplyResult Result;
  const bool bApplied = UeAgentAudioAuthoring::ApplyAssetWorkspace(
      *AssetProfile, FolderPath, AssetPath, Result, RequestId);
  OutData = Result.ToJsonObject();
  if (!bApplied)
    OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
        Result.ErrorCode, Result.ErrorDetail, TEXT("audio_folder_apply_failed"));
  return bApplied;
}

bool Validate(const UeAgentAudioAuthoring::EAssetKind Kind,
              const TSharedPtr<FJsonObject> &Params,
              TSharedPtr<FJsonObject> &OutData, FString &OutError) {
  FString FolderPath;
  if (!ReadRequiredString(Params, TEXT("folder_path"), FolderPath, OutError))
    return false;
  const UeAgentAudioAuthoring::FProfile *AssetProfile = Profile(Kind, OutError);
  if (!AssetProfile)
    return false;
  FString AssetPath;
  if (Params.IsValid())
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
  FolderPath =
      UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
  UeAgentAudioAuthoring::FWorkspaceInspectResult Result;
  const bool bValid = UeAgentAudioAuthoring::InspectAssetWorkspace(
      *AssetProfile, FolderPath, AssetPath, Result);
  OutData = Result.ToJsonObject();
  if (!bValid)
    OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
        Result.ErrorCode, Result.ErrorDetail,
        TEXT("audio_folder_validation_failed"));
  return bValid;
}
} // namespace UeAgentAudioFolderCommandBridge
