// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringExport.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringProjection.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Engine/Blueprint.h"
#include "Misc/ScopeExit.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringExport/UeAgentSnapshotProjector.h"

namespace UeAgentBlueprintAuthoring {
namespace {
TSharedPtr<FJsonValue> RewritePackagePath(const TSharedPtr<FJsonValue> &Value,
                                          const FString &SourcePackage,
                                          const FString &TargetPackage) {
  if (!Value.IsValid() || SourcePackage.IsEmpty() || TargetPackage.IsEmpty() ||
      SourcePackage.Equals(TargetPackage, ESearchCase::CaseSensitive)) {
    return UeAgentAuthoringState::CloneCanonicalValue(Value);
  }
  switch (Value->Type) {
  case EJson::String: {
    FString Text = Value->AsString();
    Text.ReplaceInline(*SourcePackage, *TargetPackage,
                       ESearchCase::CaseSensitive);
    return MakeShared<FJsonValueString>(Text);
  }
  case EJson::Array: {
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const TSharedPtr<FJsonValue> &Item : Value->AsArray())
      Items.Add(RewritePackagePath(Item, SourcePackage, TargetPackage));
    return MakeShared<FJsonValueArray>(Items);
  }
  case EJson::Object: {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
         Value->AsObject()->Values)
      Object->SetField(Pair.Key, RewritePackagePath(Pair.Value, SourcePackage,
                                                    TargetPackage));
    return MakeShared<FJsonValueObject>(Object);
  }
  default:
    return UeAgentAuthoringState::CloneCanonicalValue(Value);
  }
}
} // namespace

static bool BuildReusableState(UBlueprint &Blueprint,
                                    TSharedPtr<FJsonObject> &OutExtension,
                                    UE::UAI::Contract::FDiagnostic& OutError,
                                    const FString &CanonicalAssetPath,
                                    const bool bForVerification) {
  OutExtension.Reset();
  OutError.Reset();
  const FString TargetPath = CanonicalAssetPath.IsEmpty()
      ? UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint)
      : CanonicalAssetPath;
  ON_SCOPE_EXIT {
    if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty())
      OutError.TargetPath = TargetPath;
  };

  UeAgentExportContract::FContract Contract;
  if (!GetExportContract(Contract, OutError))
    return false;
  const TSet<FString> ReusableFields = {
      TEXT("interfaces"),   TEXT("variables"), TEXT("delegates"), TEXT("class_defaults"),
      TEXT("graphs"),       TEXT("nodes"),     TEXT("node_properties"),
      TEXT("pin_defaults"), TEXT("edges")};
  Contract.Fields.RemoveAll(
      [&ReusableFields](const UeAgentExportContract::FFieldDescriptor &Field) {
        return !ReusableFields.Contains(Field.FieldId);
      });
  for (UeAgentExportContract::FFieldDescriptor &Field : Contract.Fields) {
    Field.OutputPath.ReplaceInline(
        TEXT("index.hjson#/provider_extension/"),
        TEXT("index.hjson#/provider_extension/blueprint/"));
  }

  TArray<UeAgentAuthoringState::FReadRequest> Requests;
  const auto AppendRequests = bForVerification
      ? AppendReusableBlueprintVerificationReadRequests
      : AppendReusableBlueprintExportSnapshotReadRequests;
  if (!AppendRequests(
          Blueprint, Requests, OutError,
          CanonicalAssetPath.IsEmpty() ? Blueprint.GetOutermost()->GetName()
                                       : CanonicalAssetPath)) {
    return false;
  }
  UeAgentAuthoringState::FCaptureResult Capture;
  if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)) {
    OutError = Capture.Diagnostic;
    return false;
  }
  UeAgentSnapshotProjector::FProjectionResult Projection;
  if (!UeAgentSnapshotProjector::ProjectSnapshot(
          Contract, TEXT("llm_authoring"), Capture.Snapshot, Projection)) {
    OutError = Projection.Diagnostic;
    return false;
  }
  if (Projection.Files.Num() != 1) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(
        TEXT("blueprint_reusable_projection_file_count_invalid"),
        TEXT("Reusable Blueprint projection must produce exactly one file."),
        TEXT("export_projection"));
    OutError.SourceKind = TEXT("native_result");
    OutError.PropertyPath = TEXT("projected_file_count");
    OutError.ActualValue = MakeShared<FJsonValueNumber>(Projection.Files.Num());
    OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
    return false;
  }
  TSharedPtr<FJsonObject> Root;
  if (!UeAgentSnapshotProjector::BuildProjectedFileRoot(Projection.Files[0],
                                                        Root, OutError)) {
    return false;
  }
  const TSharedPtr<FJsonObject> *Provider = nullptr;
  const TSharedPtr<FJsonObject> *BlueprintExtension = nullptr;
  if (!Root.IsValid() ||
      !Root->TryGetObjectField(TEXT("provider_extension"), Provider) ||
      !Provider || !Provider->IsValid() ||
      !(*Provider)->TryGetObjectField(TEXT("blueprint"), BlueprintExtension) ||
      !BlueprintExtension || !BlueprintExtension->IsValid()) {
    const bool bProviderObject = Provider && Provider->IsValid();
    OutError = UE::UAI::Contract::FDiagnostic::Failure(
        TEXT("blueprint_reusable_projection_missing_extension"),
        !Root.IsValid() ? TEXT("Reusable Blueprint projection produced no root object.")
            : !bProviderObject ? TEXT("The projected root has no provider_extension object.")
            : TEXT("The projected provider_extension has no blueprint object."),
        TEXT("export_projection"));
    OutError.SourceKind = TEXT("native_result");
    OutError.PropertyPath = !Root.IsValid() ? TEXT("/")
        : !bProviderObject ? TEXT("/provider_extension")
        : TEXT("/provider_extension/blueprint");
    OutError.ActualValue = !Root.IsValid() ? TSharedPtr<FJsonValue>()
        : !bProviderObject ? Root->TryGetField(TEXT("provider_extension"))
        : (*Provider)->TryGetField(TEXT("blueprint"));
    OutError.ActualValuePresent = OutError.ActualValue.IsValid();
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("an object"));
    return false;
  }
  const TSharedPtr<FJsonValue> Rewritten = RewritePackagePath(
      MakeShared<FJsonValueObject>(*BlueprintExtension),
      Blueprint.GetOutermost()->GetName(), CanonicalAssetPath);
  OutExtension = Rewritten.IsValid() ? Rewritten->AsObject() : nullptr;
  return true;
}

bool BuildReusableProviderExtension(UBlueprint &Blueprint,
    TSharedPtr<FJsonObject> &OutExtension, UE::UAI::Contract::FDiagnostic& OutError,
    const FString &CanonicalAssetPath) {
  return BuildReusableState(Blueprint, OutExtension, OutError, CanonicalAssetPath, false);
}

bool BuildReusableVerificationState(UBlueprint &Blueprint,
    TSharedPtr<FJsonObject> &OutState, UE::UAI::Contract::FDiagnostic& OutError,
    const FString &CanonicalAssetPath) {
  return BuildReusableState(Blueprint, OutState, OutError, CanonicalAssetPath, true);
}

bool ExportAssetWorkspace(
    UObject &Asset, const FString &WorkspaceRoot, const FString &ProfileId,
    UeAgentExportWorkspaceWriter::FWorkspaceResult &OutResult) {
  auto& OutError = OutResult.Diagnostic;
  OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
  OutError.Reset();

  TArray<UeAgentAuthoringState::FReadRequest> Requests;
  if (!BuildAssetExportSnapshotReadRequests(Asset, Requests, OutError)) {
    UE_LOG(LogTemp, Error, TEXT("Blueprint workspace export requests failed for %s: %s"),
           *Asset.GetPathName(), *OutError.ToPublicDetail());
    return false;
  }

  UeAgentAuthoringState::FCaptureResult Capture;
  if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)) {
    OutError = Capture.Diagnostic;
    UE_LOG(LogTemp, Error, TEXT("Blueprint workspace export capture failed for %s: %s"),
           *Asset.GetPathName(), *OutError.ToPublicDetail());
    return false;
  }

  const bool bExported = UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(TEXT("ue.blueprint.asset"), ProfileId, Capture.Snapshot, WorkspaceRoot, OutResult);
  if (!bExported) {
    UE_LOG(LogTemp, Error, TEXT("Blueprint workspace export projection failed for %s: %s %s"),
           *Asset.GetPathName(), *OutResult.Diagnostic.Code, *OutError.ToPublicDetail());
  }
  return bExported;
}
} // namespace UeAgentBlueprintAuthoring
