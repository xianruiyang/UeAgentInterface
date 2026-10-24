// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Audio/Authoring/UeAgentAudioAuthoringApplyProvider.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceInput.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDocuments.h"
#include "Domains/Audio/Authoring/UeAgentAudioGraphIntent.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringState.h"

namespace UeAgentAudioAuthoring {
namespace {
using namespace UeAgentApplyInputContract;
using namespace UeAgentAuthoringOperationApply;








using UE::UAI::Contract::FDiagnostic;

FDiagnostic AudioFailure(const TCHAR* Code, const FString& Reason, const FString& Stage,
    const FString& Target, const FString& Field, TSharedPtr<FJsonValue> Actual,
    TSharedPtr<FJsonValue> Expected, const FJsonObject* Input = nullptr,
    const FString& InputField = FString()) {
  FDiagnostic Error = FDiagnostic::Failure(Code, Reason, Stage);
  Error.TargetPath = Target;
  Error.PropertyPath = Field;
  Error.ActualValuePresent = Actual.IsValid();
  Error.ExpectedValuePresent = Expected.IsValid();
  Error.ActualValue = MoveTemp(Actual);
  Error.ExpectedValue = MoveTemp(Expected);
  if (Input) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Input, InputField);
  else Error.SourceKind = TEXT("native_result");
  return Error;
}

void SetFailure(FOperationFailure& OutFailure, FDiagnostic Diagnostic,
    const FString& Target = FString(), const FString& Stage = FString()) {
  if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target;
  if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = Stage;
  OutFailure.Diagnostic = MoveTemp(Diagnostic);
}

bool ReadBool(const TSharedPtr<const FJsonObject> &Object, const TCHAR *Field,
              const bool DefaultValue) {
  bool Value = DefaultValue;
  if (Object.IsValid())
    Object->TryGetBoolField(Field, Value);
  return Value;
}

bool EnsureMetaSoundAssetIdentity(UObject *Asset, const FString& Target, FDiagnostic& OutError) {
  if (!Asset) {
    OutError = AudioFailure(TEXT("metasound_asset_identity_missing"),
        TEXT("No MetaSound asset object is available to establish persistent asset identity."),
        TEXT("asset_identity"), Target, TEXT("asset_available"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  Asset->ClearFlags(RF_Transient);
  Asset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
  if (!Asset->IsAsset()) {
    OutError = AudioFailure(TEXT("metasound_asset_identity_invalid"),
        TEXT("The MetaSound object is not an asset after assigning the required asset flags."),
        TEXT("asset_identity"), Target, TEXT("is_asset"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.InternalContext = Asset->GetPathName();
    return false;
  }
  return true;
}

TSet<FString> GetDocumentFields(const EAssetKind Kind) {
  switch (Kind) {
  case EAssetKind::MetaSound:
    return {TEXT("settings"), TEXT("interfaces"), TEXT("members"),
            TEXT("graph"), TEXT("nodes"), TEXT("edges"), TEXT("connections")};
  case EAssetKind::SoundCue:
    return {TEXT("graph"), TEXT("nodes"), TEXT("edges"), TEXT("connections")};
  case EAssetKind::SoundSubmix:
    return {TEXT("settings"), TEXT("children")};
  default:
    return {};
  }
}

bool ExtractDocument(const EAssetKind Kind,
                     const TSharedPtr<const FJsonObject> &Extension,
                     TSharedPtr<FJsonObject> &OutDocument, UE::UAI::Contract::FDiagnostic& OutError) {
  OutDocument = MakeShared<FJsonObject>();
  OutError.Reset();
  if (!Extension.IsValid())
    return true;
  const TSet<FString> DocumentFields = GetDocumentFields(Kind);
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Field :
       Extension->Values) {
    if (Field.Key == TEXT("compile") || Field.Key == TEXT("save") || Field.Key == TEXT("complete_document"))
      continue;
    if (!DocumentFields.Contains(Field.Key)) {
      TArray<FString> Names = DocumentFields.Array();
      Names.Append({TEXT("compile"), TEXT("save"), TEXT("complete_document")});
      Names.Sort();
      TArray<TSharedPtr<FJsonValue>> Allowed;
      for (const FString& Name : Names) Allowed.Add(MakeShared<FJsonValueString>(Name));
      OutError = AudioFailure(TEXT("audio_provider_extension_unknown_field"),
          TEXT("The audio provider extension contains an unsupported field."), TEXT("audio_prepare"),
          FString(), Field.Key, MakeShared<FJsonValueString>(Field.Key),
          MakeShared<FJsonValueArray>(MoveTemp(Allowed)), Extension.Get(), Field.Key);
      return false;
    }
    OutDocument->SetField(Field.Key, Field.Value);
  }
  return true;
}

bool DocumentsEqual(UObject &A, UObject &B, const EAssetKind Kind,
                    const FString &CanonicalAssetPath, UE::UAI::Contract::FDiagnostic& OutReadError) {
  TSharedPtr<FJsonObject> ADocument;
  TSharedPtr<FJsonObject> BDocument;
  OutReadError.Reset();
  if (!BuildDocument(Kind, A, ADocument, OutReadError, CanonicalAssetPath)) {
    if (OutReadError.SourceKind.IsEmpty()) OutReadError.SourceKind = TEXT("native_result");
    OutReadError = UE::UAI::Contract::FDiagnostic::WrapFailure(
        TEXT("audio_noop_existing_document_read_failed"), OutReadError, TEXT("noop_comparison"),
        TEXT("The existing audio document could not be read for no-op comparison."));
    return false;
  }
  if (!BuildDocument(Kind, B, BDocument, OutReadError, CanonicalAssetPath)) {
    if (OutReadError.SourceKind.IsEmpty()) OutReadError.SourceKind = TEXT("native_result");
    OutReadError = UE::UAI::Contract::FDiagnostic::WrapFailure(
        TEXT("audio_noop_proposed_document_read_failed"), OutReadError, TEXT("noop_comparison"),
        TEXT("The proposed audio document could not be read for no-op comparison."));
    return false;
  }
  return UeAgentAuthoringState::MakeStableCanonicalValueKey(
             MakeShared<FJsonValueObject>(ADocument)) ==
             UeAgentAuthoringState::MakeStableCanonicalValueKey(
                 MakeShared<FJsonValueObject>(BDocument));
}

UObject *DuplicateAsset(UObject *Source, UObject *Outer, const FName Name,
                        const EObjectFlags Flags) {
  return Source && Outer ? StaticDuplicateObject(Source, Outer, Name, Flags)
                         : nullptr;
}

UObject *PublishAsset(UObject *Source, const FString &PackagePath) {
  UPackage *Package = CreatePackage(*PackagePath);
  const FString Name = FPackageName::GetLongPackageAssetName(PackagePath);
  UObject *Published =
      DuplicateAsset(Source, Package, FName(*Name), RF_AllFlags);
  if (Published) {
    Published->ClearFlags(RF_Transient);
    Published->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
  }
  return Published;
}

void RetireTransientAsset(TStrongObjectPtr<UObject> &Asset,
                          UObject *PreservedAsset = nullptr) {
  if (!Asset.IsValid())
    return;
  if (Asset.Get() != PreservedAsset) {
    Asset->ClearFlags(RF_Public | RF_Standalone);
    Asset->MarkAsGarbage();
  }
  Asset.Reset();
}

bool DeleteAsset(UObject* Asset, const FString& Target, UE::UAI::Contract::FDiagnostic& OutError)
{
  OutError.Reset();
  if (!Asset) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("audio_delete_target_missing"),
      TEXT("No audio asset object was supplied for deletion."), TEXT("authoring_apply"));
    OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Target; OutError.PropertyPath = TEXT("asset");
    OutError.ActualValuePresent = true; OutError.ActualValue = MakeShared<FJsonValueNull>();
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("audio asset object"));
    return false;
  }
  TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
  if (!ObjectTools::DeleteSingleObject(Asset, true)) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("audio_delete_rejected"),
      TEXT("The editor rejected deletion of the audio asset object."), TEXT("authoring_apply"),
      TEXT("DeleteSingleObject returned false with reference checking enabled; its bool API does not distinguish deletion-policy and reference-check rejection."));
    OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Target; OutError.PropertyPath = TEXT("deletion_succeeded");
    OutError.ActualValuePresent = true; OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
    OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
    return false;
  }
  return true;
}

TSharedRef<FJsonObject> StatsToJson(const FMutationStats &Stats) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetNumberField(TEXT("settings"), Stats.Settings);
  Result->SetNumberField(TEXT("members"), Stats.Members);
  Result->SetNumberField(TEXT("nodes"), Stats.Nodes);
  Result->SetNumberField(TEXT("edges"), Stats.Edges);
  Result->SetNumberField(TEXT("references"), Stats.References);
  Result->SetNumberField(TEXT("total"), Stats.Total());
  return Result;
}

FMutationStats StatsFromJson(const TSharedPtr<FJsonObject> &Object) {
  FMutationStats Result;
  if (!Object.IsValid())
    return Result;
  Object->TryGetNumberField(TEXT("settings"), Result.Settings);
  Object->TryGetNumberField(TEXT("members"), Result.Members);
  Object->TryGetNumberField(TEXT("nodes"), Result.Nodes);
  Object->TryGetNumberField(TEXT("edges"), Result.Edges);
  Object->TryGetNumberField(TEXT("references"), Result.References);
  return Result;
}

class FAssetLifecycleState final : public IReflectionOperationLifecycleState {
public:
  FAssetLifecycleState(const FProfile &InProfile, FString InPackagePath,
                       const EOperation InOperation, UObject *InExisting,
                       UObject *InProposed,
                       TSharedPtr<const FJsonObject> InDocument,
                       const FMutationStats &InStats, const bool bInCompile,
                       const bool bInSave, const bool bInDocumentNoOp,
                       TArray<FNodePropertyIntent> InPropertyIntents = {},
                       bool bInCompleteDocument = false,
                       FAudioGraphIntent InGraphIntent = {})
      : Profile(InProfile), PackagePath(MoveTemp(InPackagePath)),
        Operation(InOperation), Existing(InExisting), Proposed(InProposed),
        Document(MoveTemp(InDocument)), Stats(InStats), bCompile(bInCompile),
        bSave(bInSave), bDocumentNoOp(bInDocumentNoOp),
        PropertyIntents(MoveTemp(InPropertyIntents)), bCompleteDocument(bInCompleteDocument),
        ProposedGraphIntent(MoveTemp(InGraphIntent)), ActualGraphIntent(ProposedGraphIntent) {
    if (Document) DocumentSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*Document);
  }

  virtual ~FAssetLifecycleState() override { RetireProposed(); }

  virtual FString GetTargetKey() const override { return PackagePath; }
  virtual EOperation GetResolvedOperation() const override { return Operation; }
  virtual bool IsStateNoOp() const override {
    return Operation == EOperation::Update && bDocumentNoOp;
  }
  virtual bool IsNoOp() const override {
    return IsStateNoOp() && !bCompile && !bSave;
  }
  virtual FReflectionRoot GetProposedRoot() const override {
    return FReflectionRoot::MakeObject(Proposed.Get());
  }
  virtual FReflectionRoot GetActualRoot() const override {
    return FReflectionRoot::MakeObject(Actual ? Actual : Existing);
  }
  virtual bool ShouldApplyPropertiesToActualRoot() const override {
    return Operation == EOperation::Update;
  }

  virtual bool CaptureRollback(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Update && Existing) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!BuildDocument(Profile.Kind, *Existing, RollbackDocument, Error, PackagePath)) {
        SetFailure(OutFailure, MoveTemp(Error), PackagePath, TEXT("rollback_capture"));
        return false;
      }
    } else if (Operation == EOperation::Remove && Existing) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!RemovalStage.Capture(Existing, Error)) {
        SetFailure(OutFailure, MoveTemp(Error), PackagePath);
        return false;
      }
    }
    return true;
  }

  virtual bool ApplyProposed(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    return true;
  }

  virtual bool SupportsPersistentReadback() const override {
    return Operation == EOperation::Update;
  }
  virtual bool ReleaseForPersistentReadback(FOperationFailure &OutFailure) override {
    RetireProposed();
    Existing = nullptr;
    Actual = nullptr;
    return true;
  }
  virtual bool RebindAfterPersistentReadback(FOperationFailure &OutFailure) override {
    Existing = LoadAsset(Profile.Kind, PackagePath);
    Actual = Existing;
    if (!Existing) {
      SetFailure(OutFailure, AudioFailure(TEXT("audio_persistent_reload_failed"),
          TEXT("The saved audio asset could not be reloaded for persistent readback."), TEXT("persistent_readback"),
          PackagePath, TEXT("asset_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
      return false;
    }
    return true;
  }

  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(Document.Get(), [this](const FString& Pointer) {
      return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(DocumentSources, Pointer);
    });
    if (Operation == EOperation::Remove) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!RemovalStage.Stage(Error)) {
        SetFailure(OutFailure, MoveTemp(Error), PackagePath);
        return false;
      }
      Actual = nullptr;
      return true;
    }
    if (Operation == EOperation::Update) {
      FMutationStats AppliedStats;
      UE::UAI::Contract::FDiagnostic Error;
      if (!ApplyDocument(Profile.Kind, *Existing, Document, AppliedStats, Error,
                         PackagePath, nullptr, bCompleteDocument, &ActualGraphIntent)) {
        SetFailure(OutFailure, MoveTemp(Error), PackagePath);
        return false;
      }
      Stats = AppliedStats;
      Actual = Existing;
      bDocumentApplied = true;
      return true;
    }
    if (!Proposed.IsValid()) {
      SetFailure(OutFailure, AudioFailure(TEXT("audio_proposed_asset_missing"),
          TEXT("The prepared audio asset is no longer available for publication."), TEXT("audio_publish"),
          PackagePath, TEXT("proposed_asset_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
      return false;
    }
    Actual = PublishAsset(Proposed.Get(), PackagePath);
    if (!Actual) {
      SetFailure(OutFailure, AudioFailure(TEXT("audio_asset_publish_failed"),
          TEXT("Unreal did not publish the proposed audio asset into the target package."), TEXT("audio_publish"),
          PackagePath, TEXT("published_asset_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
      return false;
    }
    if (Profile.Kind == EAssetKind::MetaSound) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!EnsureMetaSoundAssetIdentity(Actual, PackagePath, Error)) {
        SetFailure(OutFailure, MoveTemp(Error), PackagePath);
        return false;
      }
    }
    FAssetRegistryModule::AssetCreated(Actual);
    Actual->MarkPackageDirty();
    bDocumentApplied = true;
    return true;
  }

  virtual bool Compile(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (!bCompile || Operation == EOperation::Remove)
      return true;
    UE::UAI::Contract::FDiagnostic Error;
    if (!CompileAsset(Profile.Kind, *(Actual ? Actual : Existing), Error)) {
      SetFailure(OutFailure, MoveTemp(Error), PackagePath);
      return false;
    }
    return VerifyOriginalIntent(Actual ? Actual : Existing, ActualGraphIntent, OutFailure);
  }

  virtual bool Save(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (!bSave || Operation == EOperation::Remove)
      return true;
    UE::UAI::Contract::FDiagnostic Error;
    if (!SaveAsset(Profile.Kind, *(Actual ? Actual : Existing), Error)) {
      SetFailure(OutFailure, MoveTemp(Error), PackagePath);
      return false;
    }
    return true;
  }

  virtual bool BuildDurablePackagePlans(
      TArray<FPackageDurablePlan> &OutPlans,
      FOperationFailure &OutFailure) const override {
    return BuildPackageDurablePlan(
        PackagePath, PackagePath,
        Operation == EOperation::Remove
            ? Existing
            : (Existing ? Existing : Proposed.Get()),
        Operation == EOperation::Remove ? EPackageDurableIntent::Delete
                                        : EPackageDurableIntent::Save,
        OutPlans, OutFailure);
  }

  virtual bool BuildDomainExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    if (Operation != EOperation::Remove && !VerifyOriginalIntent(Proposed.Get(), ProposedGraphIntent, OutFailure)) return false;
    return BuildReadRequests(
        Operation == EOperation::Remove ? nullptr : Proposed.Get(),
        Operation == EOperation::Remove, OutRequests, OutFailure);
  }

  virtual bool BuildDomainActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    if (bDocumentApplied && !VerifyOriginalIntent(Actual ? Actual : Existing, ActualGraphIntent, OutFailure)) return false;
    return BuildReadRequests(
        Operation == EOperation::Remove ? nullptr
                                        : (Actual ? Actual : Existing),
        Operation == EOperation::Remove, OutRequests, OutFailure);
  }

  virtual bool BuildDomainBeforeReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
      FOperationFailure& OutFailure) const override {
    // Checkpoint recovery bypasses ordinary Rollback(), so bDocumentApplied
    // may still be true. Restored state is compared with the captured baseline.
    UObject* Owner = Actual ? Actual : Existing;
    return BuildReadRequests(Owner, !Owner, OutRequests, OutFailure);
  }

  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const override {
    return StatsToJson(Stats);
  }

  virtual bool Commit(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove && RemovalStage.IsStaged()) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!DeleteAsset(RemovalStage.GetAsset(), PackagePath, Error)) {
        SetFailure(OutFailure, MoveTemp(Error), PackagePath);
        return false;
      }
      RemovalStage.Reset();
      Existing = nullptr;
    }
    RetireProposed();
    return true;
  }

  virtual bool Rollback(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    bDocumentApplied = false;
    UE::UAI::Contract::FDiagnostic Error;
    if (Operation == EOperation::Update && Existing &&
        RollbackDocument.IsValid()) {
      FMutationStats RollbackStats;
      if (!ApplyDocument(Profile.Kind, *Existing, RollbackDocument,
                         RollbackStats, Error, PackagePath, nullptr, true)) {
        SetFailure(OutFailure, MoveTemp(Error), PackagePath);
        return false;
      }
      Actual = Existing;
      RetireProposed();
      return true;
    }
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Actual) {
      if (!DeleteAsset(Actual, PackagePath, Error)) {
        TArray<UObject *> Objects = {Actual};
        const int32 DeletedCount = ObjectTools::DeleteObjectsUnchecked(Objects);
        if (DeletedCount <= 0) {
          Error.Diagnostics.Add(AudioFailure(TEXT("audio_rollback_delete_failed"),
              TEXT("The fallback deletion also failed to remove the audio asset during rollback."), TEXT("audio_rollback"),
              PackagePath, TEXT("deleted_objects"), MakeShared<FJsonValueNumber>(DeletedCount),
              MakeShared<FJsonValueString>(TEXT("at least one deleted object"))));
          UeAgentAuthoringDiagnostics::SetRollbackFacts(Error, false);
          SetFailure(OutFailure, MoveTemp(Error), PackagePath);
          return false;
        }
      }
      Actual = nullptr;
      RetireProposed();
      return true;
    }
    if (Operation == EOperation::Remove && RemovalStage.IsCaptured()) {
      if (!RemovalStage.Rollback(Error)) {
        SetFailure(OutFailure, MoveTemp(Error), PackagePath);
        return false;
      }
      Actual = Existing;
    }
    RetireProposed();
    return true;
  }

private:
  bool VerifyOriginalIntent(UObject* Root, const FAudioGraphIntent& GraphIntent, FOperationFailure& OutFailure) const {
    UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(Document.Get(), [this](const FString& Pointer) {
      return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(DocumentSources, Pointer);
    });
    UE::UAI::Contract::FDiagnostic Error;
    if (Root && VerifyNodePropertyIntents(*Root, PropertyIntents, Error)
        && VerifyDocumentIntent(Profile.Kind, *Root, Document, bCompleteDocument, Error)
        && GraphIntent.Verify(*Root, Error)) return true;
    if (!Root) Error = AudioFailure(TEXT("audio_document_intent_owner_missing"),
        TEXT("No audio asset object is available to observe the requested document state."), TEXT("audio_intent_observation"),
        PackagePath, TEXT("asset_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    if (Error.TargetPath.IsEmpty()) Error.TargetPath = PackagePath;
    if (Document) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Document);
    UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("audio_document_intent_not_retained"),
        Error, TEXT("audio_intent_observation"));
    return true;
  }
  void RetireProposed() { RetireTransientAsset(Proposed, Actual); }

  bool
  BuildReadRequests(UObject *Asset, const bool bExpectedAbsent,
                    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                    FOperationFailure &OutFailure) const {
    UE::UAI::Contract::FDiagnostic Error;
    if (!BuildAssetSnapshotReadRequests(Profile, Asset, PackagePath,
                                        bExpectedAbsent, OutRequests, Error)) {
      SetFailure(OutFailure, MoveTemp(Error), PackagePath);
      return false;
    }
    return true;
  }

  FProfile Profile;
  FString PackagePath;
  EOperation Operation;
  UObject *Existing = nullptr;
  TStrongObjectPtr<UObject> Proposed;
  TSharedPtr<FJsonObject> RollbackDocument;
  FAssetRemovalStage RemovalStage;
  UObject *Actual = nullptr;
  TSharedPtr<const FJsonObject> Document;
  TMap<FString, UeAgentAuthoringDiagnostics::FSourceLocation> DocumentSources;
  mutable FMutationStats Stats;
  bool bCompile = true;
  bool bSave = false;
  bool bDocumentNoOp = false;
  TArray<FNodePropertyIntent> PropertyIntents;
  bool bCompleteDocument = false;
  FAudioGraphIntent ProposedGraphIntent;
  FAudioGraphIntent ActualGraphIntent;
  bool bDocumentApplied = false;
};

class FAssetLifecycle final : public IReflectionOperationLifecycle {
public:
  virtual bool
  PrepareLifecycle(const FPrepareRequest &Request,
                   TUniquePtr<IReflectionOperationLifecycleState> &OutState,
                   FOperationFailure &OutFailure) const override {
    OutState.Reset();
    OutFailure = FOperationFailure();
    if (!Request.IsValid() || !Request.Input->Target.IsValid() ||
        !Request.Contract) {
      const FString Missing = !Request.Contract ? TEXT("contract") : !Request.Unit ? TEXT("unit")
          : !Request.Input ? TEXT("input") : TEXT("target");
      SetFailure(OutFailure, AudioFailure(TEXT("audio_apply_request_invalid"),
          TEXT("Audio preparation requires a valid ") + Missing + TEXT("."), TEXT("audio_prepare"),
          FString(), TEXT("request.") + Missing + TEXT(".available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
      return false;
    }
    const FProfile *Profile =
        FindProfileByTargetKind(Request.Contract->TargetKind);
    if (!Profile) {
      TArray<TSharedPtr<FJsonValue>> Kinds;
      for (const FProfile& Supported : GetProfiles()) Kinds.Add(MakeShared<FJsonValueString>(Supported.TargetKind));
      SetFailure(OutFailure, AudioFailure(TEXT("audio_apply_profile_missing"),
          TEXT("No audio profile is registered for the requested target kind."), TEXT("audio_prepare"),
          FString(), TEXT("target_kind"), MakeShared<FJsonValueString>(Request.Contract->TargetKind), MakeShared<FJsonValueArray>(MoveTemp(Kinds))));
      return false;
    }
    FString PackagePath;
    Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
    PackagePath = NormalizePackagePath(PackagePath);
    if (!FPackageName::IsValidLongPackageName(PackagePath) ||
        FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty() ||
        (!PackagePath.Equals(TEXT("/Game")) &&
         !PackagePath.StartsWith(TEXT("/Game/")))) {
      SetFailure(OutFailure, AudioFailure(TEXT("invalid_asset_path"),
          TEXT("An audio asset requires a valid long package path under /Game with a nonempty asset name."), TEXT("audio_prepare"),
          PackagePath, TEXT("asset_path"), Request.Input->Target->TryGetField(TEXT("asset_path")),
          MakeShared<FJsonValueString>(TEXT("a valid /Game/... asset package path")), Request.Input->Target.Get(), TEXT("asset_path")));
      return false;
    }
    FString AssetKind;
    if (Request.Input->Target->TryGetStringField(TEXT("asset_kind"),
                                                 AssetKind) &&
        !AssetKind.Equals(Profile->TypeId, ESearchCase::IgnoreCase)) {
      SetFailure(OutFailure, AudioFailure(TEXT("audio_asset_kind_conflict"),
          TEXT("The asserted audio asset kind does not match the selected profile."), TEXT("audio_prepare"),
          PackagePath, TEXT("asset_kind"), MakeShared<FJsonValueString>(AssetKind),
          MakeShared<FJsonValueString>(Profile->TypeId), Request.Input->Target.Get(), TEXT("asset_kind")));
      return false;
    }

    UObject *Existing = LoadAsset(Profile->Kind, PackagePath);
    EOperation Operation = Request.Input->RequestedOperation;
    if (Operation == EOperation::Upsert)
      Operation = Existing ? EOperation::Update : EOperation::Create;
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Existing) {
      SetFailure(OutFailure, AudioFailure(TEXT("asset_already_exists"),
          Operation == EOperation::Clone ? TEXT("Clone requires an unused destination audio asset path.") : TEXT("Create requires an unused audio asset path."),
          TEXT("audio_prepare"), PackagePath, TEXT("asset_exists"), MakeShared<FJsonValueBoolean>(true),
          MakeShared<FJsonValueBoolean>(false), Request.Input->Target.Get(), TEXT("asset_path")));
      return false;
    }
    if ((Operation == EOperation::Update || Operation == EOperation::Remove) &&
        !Existing) {
      SetFailure(OutFailure, AudioFailure(TEXT("asset_not_found"),
          Operation == EOperation::Remove ? TEXT("Remove requires an existing audio asset of the selected kind.") : TEXT("Update requires an existing audio asset of the selected kind."),
          TEXT("audio_prepare"), PackagePath, TEXT("asset_exists"), MakeShared<FJsonValueBoolean>(false),
          MakeShared<FJsonValueBoolean>(true), Request.Input->Target.Get(), TEXT("asset_path")));
      return false;
    }

    TSharedPtr<FJsonObject> Document;
    UE::UAI::Contract::FDiagnostic Error;
    if (!ExtractDocument(Profile->Kind, Request.Input->ProviderExtension,
                         Document, Error)) {
      SetFailure(OutFailure, MoveTemp(Error), PackagePath, TEXT("audio_prepare"));
      return false;
    }
    // ExtractDocument keeps the same field paths in a new root. Preserve the
    // original extension map before preparation and the later lifecycle diverge.
    const auto DocumentSources = Request.Input->ProviderExtension
        ? UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*Request.Input->ProviderExtension)
        : TMap<FString, UeAgentAuthoringDiagnostics::FSourceLocation>();
    UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(Document.Get(), [&DocumentSources](const FString& Pointer) {
      return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(DocumentSources, Pointer);
    });
    UObject *Source = nullptr;
    FString SourcePath;
    if (Operation == EOperation::Clone) {
      Request.Input->Target->TryGetStringField(TEXT("source_asset_path"),
                                               SourcePath);
      Source = LoadAsset(Profile->Kind, SourcePath);
      if (!Source) {
        SetFailure(OutFailure, AudioFailure(TEXT("source_asset_not_found"),
            TEXT("The clone source could not be loaded as the selected audio asset kind."), TEXT("audio_prepare"),
            PackagePath, TEXT("source_asset_path"), Request.Input->Target->TryGetField(TEXT("source_asset_path")),
            MakeShared<FJsonValueString>(TEXT("an existing ") + Profile->TypeId + TEXT(" asset")), Request.Input->Target.Get(), TEXT("source_asset_path")));
        return false;
      }
    }

    UObject *Proposed = nullptr;
    FMutationStats Stats;
    TArray<FNodePropertyIntent> PropertyIntents;
    FAudioGraphIntent GraphIntent;
    if (Operation != EOperation::Remove) {
      UPackage *Scratch = CreatePackage(
          *FString::Printf(TEXT("/Temp/UAI/Audio/Proposed_%s"),
                           *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
      UObject *ProposedSource =
          Operation == EOperation::Clone ? Source : Existing;
      if (Profile->Kind == EAssetKind::MetaSound && ProposedSource &&
          !EnsureMetaSoundAssetIdentity(ProposedSource, Operation == EOperation::Clone ? NormalizePackagePath(SourcePath) : PackagePath, Error)) {
        SetFailure(OutFailure, MoveTemp(Error), PackagePath);
        return false;
      }
      Proposed = Operation == EOperation::Create
                     ? CreateFactorySeed(Profile->Kind, Scratch, NAME_None,
                                         RF_Transactional, Document, Error)
                     : DuplicateAsset(ProposedSource,
                                      Scratch, NAME_None,
                                      Profile->Kind == EAssetKind::MetaSound
                                          ? RF_Public | RF_Standalone |
                                                RF_Transactional
                                          : RF_Transactional);
      if (!Proposed) {
        if (Operation == EOperation::Create) SetFailure(OutFailure, MoveTemp(Error), PackagePath);
        else {
          auto DuplicationError = AudioFailure(TEXT("audio_proposed_asset_create_failed"),
              TEXT("Unreal did not duplicate the source audio asset for preparation."), TEXT("audio_prepare"),
              PackagePath, TEXT("proposed_asset_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
          DuplicationError.CandidateName = Operation == EOperation::Clone ? SourcePath : PackagePath;
          SetFailure(OutFailure, MoveTemp(DuplicationError), PackagePath);
        }
        return false;
      }
      if (Profile->Kind == EAssetKind::MetaSound) {
        if (!EnsureMetaSoundAssetIdentity(Proposed, PackagePath, Error)) {
          TStrongObjectPtr<UObject> InvalidGuard(Proposed);
          RetireTransientAsset(InvalidGuard);
          SetFailure(OutFailure, MoveTemp(Error), PackagePath);
          return false;
        }
      }
      TStrongObjectPtr<UObject> ProposedGuard(Proposed);
      if (!ApplyDocument(Profile->Kind, *Proposed, Document, Stats, Error,
                         PackagePath, &PropertyIntents,
                         ReadBool(Request.Input->ProviderExtension, TEXT("complete_document"), false), &GraphIntent)) {
        RetireTransientAsset(ProposedGuard);
        SetFailure(OutFailure, MoveTemp(Error), PackagePath);
        return false;
      }

      UE::UAI::Contract::FDiagnostic ComparisonError;
      const bool bDocumentNoOp =
          Operation == EOperation::Update && Existing &&
          DocumentsEqual(*Existing, *Proposed, Profile->Kind, PackagePath, ComparisonError);
      if (!ComparisonError.IsEmpty()) {
        if (ComparisonError.TargetPath.IsEmpty()) ComparisonError.TargetPath = PackagePath;
        UeAgentAuthoringDiagnostics::ReportConsistencyWarning(ComparisonError);
      }
      OutState = MakeUnique<FAssetLifecycleState>(
          *Profile, PackagePath, Operation, Existing, Proposed, Document, Stats,
          ReadBool(Request.Input->ProviderExtension, TEXT("compile"), true),
          ReadBool(Request.Input->ProviderExtension, TEXT("save"), false),
          bDocumentNoOp, MoveTemp(PropertyIntents),
          ReadBool(Request.Input->ProviderExtension, TEXT("complete_document"), false), MoveTemp(GraphIntent));
      ProposedGuard.Reset();
      return true;
    }

    OutState = MakeUnique<FAssetLifecycleState>(
        *Profile, PackagePath, Operation, Existing, Proposed, Document, Stats,
        ReadBool(Request.Input->ProviderExtension, TEXT("compile"), true),
        ReadBool(Request.Input->ProviderExtension, TEXT("save"), false), false);
    return true;
  }
};

FInputFieldDescriptor Field(const TCHAR *Name, const TCHAR *Role,
                            const TCHAR *TypeId,
                            const TArray<EOperation> &RequiredFor = {}) {
  FInputFieldDescriptor Result;
  Result.Name = Name;
  Result.Role = Role;
  Result.TypeId = TypeId;
  Result.RequiredFor = RequiredFor;
  return Result;
}

FOperationDescriptor OperationDescriptor(const EOperation Operation) {
  FOperationDescriptor Result;
  Result.Operation = Operation;
  Result.IdentityOneOf = {{TEXT("asset_path")}};
  Result.AcceptedSections = {TEXT("target"), TEXT("provider_extension")};
  if (Operation != EOperation::Remove)
    Result.AcceptedSections.Add(TEXT("properties"));
  if (Operation == EOperation::Create) {
    Result.OmittedFieldPolicy = TEXT("ue_native_default");
    Result.NativeDefaultPolicy = TEXT("required");
  } else if (Operation == EOperation::Update) {
    Result.OmittedFieldPolicy = TEXT("preserve_current");
    Result.NativeDefaultPolicy = TEXT("forbidden");
  } else if (Operation == EOperation::Clone ||
             Operation == EOperation::Upsert) {
    Result.OmittedFieldPolicy = TEXT("ue_native_default");
    Result.NativeDefaultPolicy = TEXT("allowed");
  } else {
    Result.OmittedFieldPolicy = TEXT("not_applicable");
    Result.NativeDefaultPolicy = TEXT("not_applicable");
  }
  Result.bDestructive = Operation == EOperation::Remove;
  return Result;
}

FContract MakeInputContract(const FProfile &Profile) {
  FContract Result;
  Result.Schema = TEXT("uai.apply_input_contract.v1");
  Result.ContractId = Profile.InputContractId;
  Result.ContractVersion = TEXT("1.0.0");
  Result.ProfileId = Profile.ProfileId;
  Result.TargetKind = Profile.TargetKind;
  Result.SemanticDescriptorRefs = {Profile.DocumentSemanticId,
                                   Profile.ExportTargetSemanticId};
  Result.ReflectionBaseline.bEnabled = true;
  Result.ReflectionBaseline.RootKinds = {TEXT("uobject"), TEXT("ustruct")};
  Result.ReflectionBaseline.RecursiveNodes = {
      TEXT("scalar"),    TEXT("struct"),      TEXT("owned_object"),
      TEXT("reference"), TEXT("array"),       TEXT("set"),
      TEXT("map"),       TEXT("fixed_array"), TEXT("optional")};
  Result.ReflectionBaseline.UnknownFieldPolicy = TEXT("reject");
  Result.ReflectionBaseline.ExternalReferencePolicy =
      TEXT("replace_reference_only");
  Result.ReflectionBaseline.OwnedObjectPolicy =
      TEXT("create_or_locate_with_real_outer_then_recurse");
  Result.PropertyPayload.Mode = TEXT("reflection");
  Result.PropertyPayload.PropertyPathField = TEXT("property_path");
  Result.PropertyPayload.ValueField = TEXT("value");
  Result.PropertyPayload.OptionalAssertionFields = {
      TEXT("kind"),         TEXT("struct_path"),  TEXT("enum_path"),
      TEXT("element_type"), TEXT("key_type"),     TEXT("mapped_type"),
      TEXT("inner_type"),   TEXT("fixed_length"), TEXT("allowed_class")};
  Result.PropertyPayload.OmittedPatchFieldPolicy = TEXT("preserve_current");
  const TArray<EOperation> All = {EOperation::Create, EOperation::Update,
                                  EOperation::Upsert, EOperation::Remove,
                                  EOperation::Clone};
  Result.InputFields = {
      Field(TEXT("asset_path"), TEXT("identity"), TEXT("ue_asset_path"), All),
      Field(TEXT("asset_kind"), TEXT("type_assertion"), *Profile.TypeId),
      Field(TEXT("source_asset_path"), TEXT("selector"), TEXT("ue_asset_path"),
            {EOperation::Clone}),
      Field(TEXT("operation"), TEXT("operation_control"),
            TEXT("apply_operation"), All),
      Field(TEXT("properties"), TEXT("intent"),
            TEXT("property_descriptor_list")),
      Field(TEXT("compile"), TEXT("provider_extension"), TEXT("bool")),
      Field(TEXT("save"), TEXT("provider_extension"), TEXT("bool")),
      Field(TEXT("complete_document"), TEXT("provider_extension"), TEXT("bool")),
  };
  for (const FString &Name : GetDocumentFields(Profile.Kind))
    Result.InputFields.Add(Field(*Name, TEXT("provider_extension"),
                                 TEXT("object_or_typed_value")));
  for (const EOperation Item : All)
    Result.Operations.Add(OperationDescriptor(Item));
  Result.ProviderId = ApplyProviderId;
  Result.UnknownInputFieldPolicy = TEXT("reject");
  Result.Verification.StateSource = TEXT("shared_state_snapshot");
  Result.Verification.CompareSource = TEXT("shared_semantic_compare");
  Result.Verification.bRequired = true;
  return Result;
}

UeAgentApplyWorkspaceContract::FContract
MakeWorkspaceContract(const FProfile &Profile) {
  using namespace UeAgentApplyWorkspaceContract;
  UeAgentApplyWorkspaceContract::FContract Result;
  Result.Schema = TEXT("uai.apply_workspace_contract.v1");
  Result.ContractId = Profile.WorkspaceContractId;
  Result.ContractVersion = TEXT("1.0.0");
  Result.ProfileId = Profile.ProfileId;
  Result.Sources = {
      {ESourceKind::Directory, TEXT("physical"), TEXT("index")},
      {ESourceKind::SingleFile, TEXT("virtual"), TEXT("object")},
      {ESourceKind::Inline, TEXT("virtual"), TEXT("object")},
  };
  Result.Entry.DirectoryEntry = TEXT("index.hjson");
  Result.Entry.VirtualUnitId = TEXT("asset");
  FUnitDescriptor Unit;
  Unit.UnitId = TEXT("asset");
  Unit.PathPattern = TEXT("index.hjson");
  Unit.Role = TEXT("object");
  Unit.Cardinality = TEXT("one");
  Unit.bRequired = true;
  Unit.RouteId = Profile.ProfileId;
  Unit.ApplyContractRef = Profile.InputContractId;
  Unit.MergePolicy = TEXT("replace");
  Unit.AbsentSemantics = TEXT("error");
  Unit.EmptySemantics = TEXT("error");
  Result.Units = {Unit};
  Result.ReferencePolicy.Base = TEXT("workspace_root");
  Result.ReferencePolicy.RootContainment = TEXT("required");
  Result.ReferencePolicy.ExternalReferences = TEXT("reject");
  Result.ReferencePolicy.CaseCollision = TEXT("reject");
  Result.DependencyPolicy.EdgeSource = TEXT("contract_and_references");
  Result.DependencyPolicy.CyclePolicy = TEXT("reject");
  Result.DependencyPolicy.UnresolvedPolicy = TEXT("reject");
  Result.DependencyPolicy.Order = TEXT("stable_topological");
  Result.Preflight.Scope = TEXT("whole_workspace");
  Result.Preflight.MutationBeforeSuccess = TEXT("forbid");
  Result.Preflight.bDiagnosticsIncludeSource = true;
  Result.Transaction.Mode = TEXT("atomic");
  Result.Transaction.RollbackOn = {TEXT("mutation_failed"),
                                   TEXT("compile_failed"), TEXT("save_failed"),
                                   TEXT("verification_failed")};
  Result.Execution.Compile = TEXT("provider_hook");
  Result.Execution.Save = TEXT("caller_controlled");
  Result.Execution.Verification = TEXT("shared_snapshot_required");
  Result.UnknownFilePolicy = TEXT("reject");
  Result.DuplicateTargetPolicy = TEXT("reject");
  return Result;
}

TSharedRef<FJsonObject> MakeApplyDocument(const FAssetApplyRequest &Request) {
  TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
  Target->SetStringField(TEXT("asset_path"), Request.AssetPath);
  Target->SetStringField(TEXT("asset_kind"), AssetKindToString(Request.Kind));
  if (!Request.SourceAssetPath.IsEmpty())
    Target->SetStringField(TEXT("source_asset_path"), Request.SourceAssetPath);
  TSharedRef<FJsonObject> Extension =
      Request.Document.IsValid()
          ? MakeShared<FJsonObject>(Request.Document->Values)
          : MakeShared<FJsonObject>();
  Extension->SetBoolField(TEXT("compile"), Request.bCompile);
  Extension->SetBoolField(TEXT("save"), Request.bSave);
  Extension->SetBoolField(TEXT("complete_document"), Request.bCompleteDocument);
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  Document->SetStringField(TEXT("operation"),
                           OperationToString(Request.Operation));
  Document->SetObjectField(TEXT("target"), Target);
  Document->SetObjectField(TEXT("provider_extension"), Extension);
  if (!Request.Properties.IsEmpty())
    Document->SetArrayField(TEXT("properties"), Request.Properties);
  return Document;
}

bool LoadWorkspace(
    const FProfile &Profile,
    const UeAgentAuthoringWorkspaceLoader::FSourceRequest &Source,
    UeAgentApplyWorkspaceContract::FContract &OutContract,
    UeAgentAuthoringWorkspaceLoader::FLoadResult &OutLoad,
    UE::UAI::Contract::FDiagnostic& OutDiagnostic) {
  if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
          Profile.WorkspaceContractId, OutContract)) {
    OutDiagnostic = AudioFailure(TEXT("audio_workspace_contract_missing"),
        TEXT("The audio profile references an unregistered workspace contract."), TEXT("workspace_load"),
        FString(), TEXT("workspace_contract_id"), MakeShared<FJsonValueString>(Profile.WorkspaceContractId),
        MakeShared<FJsonValueString>(TEXT("registered workspace contract")));
    return false;
  }
  if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(OutContract, Source,
                                                      OutLoad)) {
    OutDiagnostic = OutLoad.Diagnostic;
    return false;
  }
  return true;
}

bool BuildDependencyGraph(
    const UeAgentApplyWorkspaceContract::FContract &Contract,
    const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &OutGraph,
    UE::UAI::Contract::FDiagnostic& OutDiagnostic) {
  UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure Failure;
  if (UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
          Contract, Workspace,
          UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(),
          OutGraph, Failure))
    return true;
  OutDiagnostic = Failure.Diagnostic;
  return false;
}

bool InspectWorkspaceSource(
    const FProfile &Profile,
    const UeAgentAuthoringWorkspaceLoader::FSourceRequest &Source,
    const FString &ExpectedAssetPath, FWorkspaceInspectResult &OutResult) {
  OutResult = FWorkspaceInspectResult();
  OutResult.Status = TEXT("failed");
  UeAgentApplyWorkspaceContract::FContract Contract;
  UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
  if (!LoadWorkspace(Profile, Source, Contract, Load, OutResult.Diagnostic))
    return false;
  UeAgentAuthoringAssetWorkspaceInput::FTargetRequest TargetRequest;
  TargetRequest.ExpectedAssetPath = ExpectedAssetPath;
  TargetRequest.Errors.EntryInvalid = TEXT("audio_workspace_entry_invalid");
  TargetRequest.Errors.AssetPathConflict =
      TEXT("audio_workspace_asset_path_conflict");
  UeAgentAuthoringAssetWorkspaceInput::FTargetResult Target;
  if (!UeAgentAuthoringAssetWorkspaceInput::ResolveTarget(
          Load.Workspace, TargetRequest, Target)) {
    OutResult.Diagnostic = Target.Diagnostic;
    return false;
  }
  const EOperation Operation = Target.Input.RequestedOperation;
  OutResult.AssetPath = Target.AssetPath;
  OutResult.RequestedOperation = OperationToString(Operation);
  OutResult.bAssetExists =
      LoadAsset(Profile.Kind, OutResult.AssetPath) != nullptr;
  OutResult.bWouldCreate =
      Operation == EOperation::Create ||
      (Operation == EOperation::Upsert && !OutResult.bAssetExists);
  UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
  if (!BuildDependencyGraph(Contract, Load.Workspace, Graph,
                            OutResult.Diagnostic))
    return false;
  FWorkspaceOperationPreflightProvider Provider(
      UeAgentApplyInputContract::FRegistry::Get(), FProviderRegistry::Get());
  UeAgentAuthoringWorkspaceTransaction::FPreflightResult Preflight;
  if (!UeAgentAuthoringWorkspaceTransaction::PreflightWorkspace(
          Contract, Load.Workspace, Graph, Provider, Preflight)) {
    const FApplyFailureProjection Failure = ProjectApplyFailure(Preflight);
    OutResult.UnitsChecked = Preflight.UnitsChecked;
    OutResult.Diagnostic = Failure.Diagnostic;
    return false;
  }
  OutResult.UnitsChecked = Preflight.UnitsChecked;
  if (Preflight.Units.Num() == 1 && Preflight.Units[0].Mutation.IsValid()) {
    OutResult.bNoOp = Preflight.Units[0].Mutation->IsStateNoOp();
    OutResult.PlannedStats =
        StatsFromJson(Preflight.Units[0].Mutation->BuildResultMetadata());
  }
  OutResult.Status = TEXT("ready");
  return true;
}

bool ExecuteWorkspaceSource(
    const FProfile &Profile,
    const UeAgentAuthoringWorkspaceLoader::FSourceRequest &Source,
    const FString &ExpectedAssetPath, FAssetApplyResult &OutResult,
    const FString &RequestId,
    TFunction<bool(FOperationFailure &)> FinalReadback = {},
    bool bCompactValues = false) {
  OutResult = FAssetApplyResult();
  OutResult.Status = TEXT("failed");
  UeAgentApplyWorkspaceContract::FContract Contract;
  UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
  if (!LoadWorkspace(Profile, Source, Contract, Load, OutResult.Diagnostic)) {
    OutResult.Phase = TEXT("load");
    return false;
  }
  UeAgentAuthoringAssetWorkspaceInput::FTargetRequest TargetRequest;
  TargetRequest.ExpectedAssetPath = ExpectedAssetPath;
  TargetRequest.Errors.EntryInvalid = TEXT("audio_workspace_entry_invalid");
  TargetRequest.Errors.AssetPathConflict =
      TEXT("audio_workspace_asset_path_conflict");
  UeAgentAuthoringAssetWorkspaceInput::FTargetResult Target;
  if (!UeAgentAuthoringAssetWorkspaceInput::ResolveTarget(
          Load.Workspace, TargetRequest, Target)) {
    OutResult.Phase = Target.Phase;
    OutResult.Diagnostic = Target.Diagnostic;
    return false;
  }
  const UeAgentApplyInputContract::FOperationInput &OperationInput = Target.Input;
  const EOperation RequestedOperation = OperationInput.RequestedOperation;
  OutResult.AssetPath = Target.AssetPath;
  const bool bExisted = LoadAsset(Profile.Kind, OutResult.AssetPath) != nullptr;
  bool bSave = false;
  if (OperationInput.ProviderExtension.IsValid())
    OperationInput.ProviderExtension->TryGetBoolField(TEXT("save"), bSave);
  FBackendOptions BackendOptions;
  BackendOptions.FinalReadback = MoveTemp(FinalReadback);
  BackendOptions.bUseEditorTransaction =
      RequestedOperation != EOperation::Remove;
  BackendOptions.TransactionDescription =
      NSLOCTEXT("UeAgentInterface", "AudioAssetWorkspaceApply",
                "Apply audio asset workspace");
  FApplyWorkspaceResult ApplyResult;
  UeAgentAuthoringAssetWorkspaceApply::FFailure ApplyFailure;
  FProviderRegistry LocalProviders;
  const FProviderRegistry* Providers = &FProviderRegistry::Get();
  if (bCompactValues) {
    UeAgentAuthoringOperationApply::ERegistrationResult Registered;
    UE::UAI::Contract::FDiagnostic RegistrationError;
    if (!LocalProviders.RegisterProvider(MakeApplyProvider(true), Registered, RegistrationError)) {
      OutResult.Phase = TEXT("preflight");
      OutResult.Diagnostic = RegistrationError;
      return false;
    }
    Providers = &LocalProviders;
  }
  const bool bApplied = UeAgentAuthoringAssetWorkspaceApply::Execute(
      Contract, Load.Workspace, *Providers, RequestId, bSave,
      BackendOptions, ApplyResult, ApplyFailure);
  OutResult.UnitsChecked = ApplyResult.Preflight.UnitsChecked;
  OutResult.MutationsApplied = ApplyResult.Execution.MutationsApplied;
  OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
  OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
  if (!bApplied) {
    OutResult.Status = ApplyResult.Execution.Status;
    OutResult.Phase = ApplyFailure.Phase;
    OutResult.Diagnostic = ApplyFailure.Diagnostic;
    if (OutResult.bRollbackAttempted) {
      if (!OutResult.bRollbackSucceeded)
        {
        const auto Cause = OutResult.Diagnostic;
        OutResult.Diagnostic.Code = TEXT("audio_import_state_unknown");
        OutResult.Diagnostic.PublicMessage = TEXT("Rollback did not restore a confirmed asset state.");
        OutResult.Diagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Cause);
      }
      UeAgentAuthoringDiagnostics::SetRollbackFacts(OutResult.Diagnostic, OutResult.bRollbackSucceeded);
    }
    return false;
  }
  const EOperation Resolved =
      RequestedOperation == EOperation::Upsert
          ? (bExisted ? EOperation::Update : EOperation::Create)
          : RequestedOperation;
  OutResult.Status = TEXT("applied");
  OutResult.Phase = TEXT("complete");
  OutResult.ResolvedOperation = OperationToString(Resolved);
  OutResult.Asset = Resolved == EOperation::Remove
                        ? nullptr
                        : LoadAsset(Profile.Kind, OutResult.AssetPath);
  OutResult.ObjectPath = OutResult.Asset ? OutResult.Asset->GetPathName()
                                         : ToObjectPath(OutResult.AssetPath);
  if (const TSharedPtr<FJsonObject> *Metadata =
          ApplyResult.UnitResultMetadata.Find(TEXT("asset"));
      Metadata && Metadata->IsValid())
    OutResult.Stats = StatsFromJson(*Metadata);
  return true;
}
} // namespace

TSharedRef<FJsonObject> FAssetApplyResult::ToJsonObject() const {
  TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
  Json->SetStringField(TEXT("status"), Status);
  Json->SetStringField(TEXT("phase"), Phase);
  Json->SetStringField(TEXT("resolved_operation"), ResolvedOperation);
  Json->SetStringField(TEXT("asset_path"), AssetPath);
  Json->SetStringField(TEXT("object_path"), ObjectPath);
  Json->SetNumberField(TEXT("units_checked"), UnitsChecked);
  Json->SetNumberField(TEXT("mutations_applied"), MutationsApplied);
  Json->SetBoolField(TEXT("rollback_attempted"), bRollbackAttempted);
  Json->SetBoolField(TEXT("rollback_succeeded"), bRollbackSucceeded);
  Json->SetObjectField(TEXT("applied"), StatsToJson(Stats));
  if (!Diagnostic.IsEmpty())
  {
    Json->SetStringField(TEXT("error_code"), Diagnostic.Code);
    Json->SetObjectField(TEXT("diagnostic"), Diagnostic.ToJson());
  }
  return Json;
}

TSharedRef<FJsonObject> FWorkspaceInspectResult::ToJsonObject() const {
  TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
  Json->SetStringField(TEXT("status"), Status);
  Json->SetStringField(TEXT("asset_path"), AssetPath);
  Json->SetStringField(TEXT("requested_operation"), RequestedOperation);
  Json->SetNumberField(TEXT("units_checked"), UnitsChecked);
  Json->SetBoolField(TEXT("asset_exists"), bAssetExists);
  Json->SetBoolField(TEXT("would_create"), bWouldCreate);
  Json->SetBoolField(TEXT("no_op"), bNoOp);
  Json->SetObjectField(TEXT("planned"), StatsToJson(PlannedStats));
  if (!Diagnostic.IsEmpty())
  {
    Json->SetStringField(TEXT("error_code"), Diagnostic.Code);
    Json->SetObjectField(TEXT("diagnostic"), Diagnostic.ToJson());
  }
  return Json;
}

bool RegisterApplyContracts(UE::UAI::Contract::FDiagnostic& OutError) {
  for (const FProfile &Profile : GetProfiles()) {
    UeAgentApplyInputContract::ERegistrationResult InputResult;
    if (!UeAgentApplyInputContract::FRegistry::Get().RegisterContract(
            MakeInputContract(Profile), InputResult, OutError))
      return false;
    UeAgentApplyWorkspaceContract::ERegistrationResult WorkspaceResult;
    if (!UeAgentApplyWorkspaceContract::FRegistry::Get().RegisterContract(
            MakeWorkspaceContract(Profile), WorkspaceResult, OutError))
      return false;
  }
  return true;
}

TSharedRef<IApplyProvider> MakeApplyProvider(bool bCompactValues) {
  static const TSharedRef<IReflectionOperationLifecycle> Lifecycle =
      MakeShared<FAssetLifecycle>();
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bCompactValues = bCompactValues;
  return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle,
                                              Options);
}

bool ApplyAsset(const FAssetApplyRequest &Request,
                FAssetApplyResult &OutResult, const FString &RequestId) {
  const FProfile *Profile = FindProfile(Request.Kind);
  if (!Profile) {
    OutResult = FAssetApplyResult();
    OutResult.Diagnostic = AudioFailure(TEXT("audio_apply_profile_missing"),
        TEXT("No audio apply profile is registered for the requested asset kind."), OutResult.Phase,
        Request.AssetPath, TEXT("asset_kind"), MakeShared<FJsonValueString>(AssetKindToString(Request.Kind)),
        MakeShared<FJsonValueString>(TEXT("asset kind with a registered audio apply profile")));
    return false;
  }
  return ExecuteWorkspaceSource(
      *Profile,
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
          MakeApplyDocument(Request), TEXT("inline://audio-asset-apply")),
      Request.AssetPath, OutResult, RequestId, Request.FinalReadback, Request.bCompactValues);
}

bool ApplyAssetWorkspace(const FProfile &Profile, const FString &WorkspaceRoot,
                         const FString &ExpectedAssetPath,
                         FAssetApplyResult &OutResult,
                         const FString &RequestId) {
  return ExecuteWorkspaceSource(
      Profile,
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeDirectory(
          WorkspaceRoot),
      ExpectedAssetPath, OutResult, RequestId);
}

bool InspectAssetWorkspace(const FProfile &Profile,
                           const FString &WorkspaceRoot,
                           const FString &ExpectedAssetPath,
                           FWorkspaceInspectResult &OutResult) {
  return InspectWorkspaceSource(
      Profile,
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeDirectory(
          WorkspaceRoot),
      ExpectedAssetPath, OutResult);
}
} // namespace UeAgentAudioAuthoring
