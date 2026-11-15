// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringWorkspace.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringApplyProvider.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentAnimationAuthoring {
using namespace UE::UAI;
using namespace HjsonWorkspace;
using namespace HjsonWorkspace::DomainAuthoring;

bool ImportAssetWorkspace(
    const FProfile& Profile, UObject& Asset,
    const FResolvedSemanticWorkspaceRef& Workspace,
    TFunctionRef<bool(FString&, FString&, FWorkspaceError&)> Readback,
    FString& OutPayload, FWorkspaceError& OutError) {
  const FString ObjectPath = Asset.GetPathName();
  const FString PackagePath = Asset.GetOutermost()->GetName();
  TArray<UeAgentAuthoringState::FReadRequest> Requests;
  UE::UAI::Contract::FDiagnostic Error;
  if (!BuildAssetExportSnapshotReadRequests(Profile, Asset, Requests, Error)) {
    OutError.SetFailure(Error, ObjectPath);
    return false;
  }
  SelectRootPropertyImportReadRequests(*Workspace, Requests);
  UeAgentAuthoringState::FCaptureResult Capture;
  if (!CapturePropertyImportState(*Workspace, Requests, Capture)) {
    if (Capture.Diagnostic.IsEmpty()) {
      auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("animation_semantic_capture_failed"),
          TEXT("The native animation snapshot capture failed without reporting a diagnostic."), TEXT("workspace_resolve"));
      Diagnostic.SourceKind = TEXT("native_result");
      Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
      Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
      OutError.SetFailure(MoveTemp(Diagnostic), ObjectPath);
    } else OutError.SetFailure(Capture.Diagnostic, ObjectPath);
    return false;
  }
  auto Resources = MakeSemanticWorkspaceResourceTracker();
  StateSemantic::FUeAgentCanonicalSnapshotPtr Snapshot;
  FAuthoringModelPtr Model;
  if (!TryBuildPropertyImportCurrentCapture(&Workspace.Get(),
          ResolveTargetCompositionKind(Profile.TargetKind, Profile.TypeId),
          TEXT("llm_authoring"), Capture.Snapshot, Resources, Snapshot, Model, OutError))
    return false;
  Capture.Snapshot.ReleaseNativeValues();
  return TryCoordinateSemanticWorkspaceImport(
      Workspace, Model.ToSharedRef(), Snapshot.ToSharedRef(),
      BuildSemanticWorkspaceImportDescriptor(TEXT("animation.semantic_workspace"), *Model),
      ObjectPath, Resources,
      {TEXT("animation-domain-provider"), PackagePath, ObjectPath, TEXT("animation-semantic"),
       TEXT("animation_semantic_prepare_failed"), TEXT("animation_semantic_apply_failed"),
       TEXT("animation_semantic_readback_payload_invalid"), TEXT("animation_compile_not_settled"),
       [&Profile, ObjectPath](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error) {
         UObject* Native = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
         if (!Native) {
           Error = Contract::FDiagnostic::Failure(TEXT("animation_workspace_readback_asset_missing"),
               TEXT("The animation asset could not be loaded for workspace readback."), TEXT("workspace_readback"));
           Error.SourceKind = TEXT("native_result");
           Error.TargetPath = ObjectPath;
           Error.ActualValue = MakeShared<FJsonValueNull>();
           Error.ExpectedValue = MakeShared<FJsonValueString>(Profile.TypeId);
           return false;
         }
         return BuildAssetExportSnapshotReadRequests(Profile, *Native, Requests, Error);
       }},
      [&Profile, &PackagePath](const FString& RequestId,
          const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
          Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection,
          FRootPropertyApplyOutcome& Outcome) {
        FAssetApplyRequest Request;
        Request.Kind = Profile.Kind;
        Request.Operation = UeAgentApplyInputContract::EOperation::Update;
        Request.AssetPath = PackagePath;
        Request.Properties = MoveTemp(Projection.RootObjectProperties);
        for (const auto& Change : Prepared.GetChangeSetDraft().GetChanges()) {
          if (Change.GetIdentity().GetSemanticId() == TEXT("core.property")) continue;
          TSharedPtr<FJsonValue> Json;
          const bool bExpectedSemantic = Change.GetIdentity().GetSemanticId() == Profile.DocumentSemanticId;
          const bool bDuplicateDocument = Request.Document.IsValid();
          const bool bHasProposedValue = Change.GetProposedAfter().IsValid();
          const bool bConverted = bExpectedSemantic && !bDuplicateDocument && bHasProposedValue
              && StateSemantic::CanonicalJson::TryToJsonValue(*Change.GetProposedAfter(), Json, Outcome.Diagnostic);
          if (!bExpectedSemantic || bDuplicateDocument || !bHasProposedValue || !bConverted
              || !Json.IsValid() || Json->Type != EJson::Object) {
            if (Outcome.Diagnostic.IsEmpty()) {
              const FString Reason = !bExpectedSemantic ? TEXT("The change semantic type is not supported by this animation provider.")
                  : bDuplicateDocument ? TEXT("The operation contains more than one animation document change.")
                  : !bHasProposedValue ? TEXT("The animation document change has no proposed value.")
                  : !bConverted ? TEXT("The proposed animation document could not be converted to JSON.")
                  : TEXT("The proposed animation document must project to a JSON object.");
              Outcome.Diagnostic = Contract::FDiagnostic::Failure(TEXT("animation_document_projection_failed"), Reason, TEXT("semantic_prepare"));
              Outcome.Diagnostic.SourceKind = TEXT("native_result");
              Outcome.Diagnostic.TargetPath = PackagePath;
              Outcome.Diagnostic.InternalContext = Change.GetIdentity().GetSemanticId() + TEXT(" -> ") + Profile.DocumentSemanticId;
              if (bDuplicateDocument && bExpectedSemantic) {
                Outcome.Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(2);
                Outcome.Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
              } else if (bExpectedSemantic && !bHasProposedValue) {
                Outcome.Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
                Outcome.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("proposed animation document value"));
              } else if (!bExpectedSemantic || !bConverted) {
                Outcome.Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
                Outcome.Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
              } else {
                FString Type = TEXT("missing");
                if (Json) {
                  switch (Json->Type) {
                  case EJson::Null: Type = TEXT("null"); break;
                  case EJson::String: Type = TEXT("string"); break;
                  case EJson::Number: Type = TEXT("number"); break;
                  case EJson::Boolean: Type = TEXT("boolean"); break;
                  case EJson::Array: Type = TEXT("array"); break;
                  case EJson::Object: Type = TEXT("object"); break;
                  default: Type = TEXT("none"); break;
                  }
                }
                Outcome.Diagnostic.ActualValue = MakeShared<FJsonValueString>(Type);
                Outcome.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
              }
            }
            Outcome.Phase = TEXT("semantic_prepare");
            return false;
          }
          Request.Document = Json->AsObject();
        }
        Request.bCompile = true;
        Request.bSave = true;
        Request.FinalReadback = Outcome.FinalReadback;
        FAssetApplyResult Result;
        const bool bApplied = ApplyAsset(Request, Result, RequestId);
        Outcome.bApplied = bApplied && Result.IsApplied();
        Outcome.Phase = MoveTemp(Result.Phase);
        Outcome.Diagnostic = MoveTemp(Result.Diagnostic);
        return bApplied;
      }, Readback, OutPayload, OutError);
}
}
