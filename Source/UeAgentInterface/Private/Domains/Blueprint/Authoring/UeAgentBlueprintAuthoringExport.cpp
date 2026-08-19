// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringExport.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringProjection.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Engine/Blueprint.h"
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

bool BuildReusableProviderExtension(UBlueprint &Blueprint,
                                    TSharedPtr<FJsonObject> &OutExtension,
                                    FString &OutError,
                                    const FString &CanonicalAssetPath) {
  OutExtension.Reset();
  OutError.Reset();

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
  if (!AppendReusableBlueprintExportSnapshotReadRequests(
          Blueprint, Requests, OutError, Blueprint.GetOutermost()->GetName())) {
    return false;
  }
  UeAgentAuthoringState::FCaptureResult Capture;
  if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)) {
    OutError = Capture.ErrorCode + TEXT(":") + Capture.ErrorDetail;
    return false;
  }
  UeAgentSnapshotProjector::FProjectionResult Projection;
  if (!UeAgentSnapshotProjector::ProjectSnapshot(
          Contract, TEXT("llm_authoring"), Capture.Snapshot, Projection)) {
    OutError = Projection.ErrorCode + TEXT(":") + Projection.ErrorDetail;
    return false;
  }
  if (Projection.Files.Num() != 1) {
    OutError = TEXT("blueprint_reusable_projection_file_count_invalid");
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
    OutError = TEXT("blueprint_reusable_projection_missing_extension");
    return false;
  }
  const TSharedPtr<FJsonValue> Rewritten = RewritePackagePath(
      MakeShared<FJsonValueObject>(*BlueprintExtension),
      Blueprint.GetOutermost()->GetName(), CanonicalAssetPath);
  OutExtension = Rewritten.IsValid() ? Rewritten->AsObject() : nullptr;
  return true;
}

bool ExportAssetWorkspace(
    UObject &Asset, const FString &WorkspaceRoot, const FString &ProfileId,
    UeAgentExportWorkspaceWriter::FWorkspaceResult &OutResult,
    FString &OutError) {
  OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
  OutError.Reset();

  TArray<UeAgentAuthoringState::FReadRequest> Requests;
  if (!BuildAssetExportSnapshotReadRequests(Asset, Requests, OutError))
    return false;

  UeAgentAuthoringState::FCaptureResult Capture;
  if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)) {
    OutError = Capture.ErrorCode + TEXT(":") + Capture.ErrorDetail;
    return false;
  }

  return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(
      TEXT("ue.blueprint.asset"), ProfileId, Capture.Snapshot,
      WorkspaceRoot, OutResult, OutError);
}
} // namespace UeAgentBlueprintAuthoring
