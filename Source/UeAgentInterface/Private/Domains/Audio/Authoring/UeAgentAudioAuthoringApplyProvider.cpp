// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Audio/Authoring/UeAgentAudioAuthoringApplyProvider.h"

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
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDocuments.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringState.h"

namespace UeAgentAudioAuthoring {
namespace {
using namespace UeAgentApplyInputContract;
using namespace UeAgentAuthoringOperationApply;

void SetFailure(FOperationFailure &OutFailure, const FString &Code,
                const FString &Detail = FString()) {
  OutFailure = FOperationFailure();
  OutFailure.ErrorCode = Code;
  OutFailure.ErrorDetail = Detail.IsEmpty() ? Code : Detail;
}

bool ReadBool(const TSharedPtr<const FJsonObject> &Object, const TCHAR *Field,
              const bool DefaultValue) {
  bool Value = DefaultValue;
  if (Object.IsValid())
    Object->TryGetBoolField(Field, Value);
  return Value;
}

bool EnsureMetaSoundAssetIdentity(UObject *Asset, FString &OutError) {
  if (!Asset) {
    OutError = TEXT("metasound_asset_identity_missing");
    return false;
  }
  Asset->ClearFlags(RF_Transient);
  Asset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
  if (!Asset->IsAsset()) {
    OutError = TEXT("metasound_asset_identity_invalid:") + Asset->GetPathName();
    return false;
  }
  return true;
}

TSet<FString> GetDocumentFields(const EAssetKind Kind) {
  switch (Kind) {
  case EAssetKind::MetaSound:
    return {TEXT("settings"), TEXT("interfaces"), TEXT("members"),
            TEXT("graph")};
  case EAssetKind::SoundCue:
    return {TEXT("graph")};
  case EAssetKind::SoundSubmix:
    return {TEXT("settings"), TEXT("children")};
  default:
    return {};
  }
}

bool ExtractDocument(const EAssetKind Kind,
                     const TSharedPtr<const FJsonObject> &Extension,
                     TSharedPtr<FJsonObject> &OutDocument, FString &OutError) {
  OutDocument = MakeShared<FJsonObject>();
  OutError.Reset();
  if (!Extension.IsValid())
    return true;
  const TSet<FString> DocumentFields = GetDocumentFields(Kind);
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Field :
       Extension->Values) {
    if (Field.Key == TEXT("compile") || Field.Key == TEXT("save"))
      continue;
    if (!DocumentFields.Contains(Field.Key)) {
      OutError = TEXT("audio_provider_extension_unknown_field:") + Field.Key;
      return false;
    }
    OutDocument->SetField(Field.Key, Field.Value);
  }
  return true;
}

bool DocumentsEqual(UObject &A, UObject &B, const EAssetKind Kind,
                    const FString &CanonicalAssetPath) {
  TSharedPtr<FJsonObject> ADocument;
  TSharedPtr<FJsonObject> BDocument;
  FString Error;
  return BuildDocument(Kind, A, ADocument, Error, CanonicalAssetPath) &&
         BuildDocument(Kind, B, BDocument, Error, CanonicalAssetPath) &&
         UeAgentAuthoringState::MakeStableCanonicalValueKey(
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

bool DeleteAsset(UObject *Asset, FString &OutError) {
  OutError.Reset();
  if (!Asset) {
    OutError = TEXT("audio_delete_target_missing");
    return false;
  }
  TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
  if (!ObjectTools::DeleteSingleObject(Asset, true)) {
    OutError = TEXT("audio_delete_blocked_by_references");
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
                       const bool bInSave, const bool bInDocumentNoOp)
      : Profile(InProfile), PackagePath(MoveTemp(InPackagePath)),
        Operation(InOperation), Existing(InExisting), Proposed(InProposed),
        Document(MoveTemp(InDocument)), Stats(InStats), bCompile(bInCompile),
        bSave(bInSave), bDocumentNoOp(bInDocumentNoOp) {}

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
      FString Error;
      if (!BuildDocument(Profile.Kind, *Existing, RollbackDocument, Error)) {
        SetFailure(OutFailure, TEXT("audio_rollback_capture_failed"), Error);
        return false;
      }
    } else if (Operation == EOperation::Remove && Existing) {
      FString Error;
      if (!RemovalStage.Capture(Existing, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
    }
    return true;
  }

  virtual bool ApplyProposed(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    return true;
  }

  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove) {
      FString Error;
      if (!RemovalStage.Stage(Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      Actual = nullptr;
      return true;
    }
    if (Operation == EOperation::Update) {
      FMutationStats AppliedStats;
      FString Error;
      if (!ApplyDocument(Profile.Kind, *Existing, Document, AppliedStats, Error,
                         PackagePath)) {
        SetFailure(OutFailure, TEXT("audio_document_apply_failed"), Error);
        return false;
      }
      Stats = AppliedStats;
      Actual = Existing;
      return true;
    }
    if (!Proposed.IsValid()) {
      SetFailure(OutFailure, TEXT("audio_proposed_asset_missing"));
      return false;
    }
    Actual = PublishAsset(Proposed.Get(), PackagePath);
    if (!Actual) {
      SetFailure(OutFailure, TEXT("audio_asset_publish_failed"));
      return false;
    }
    if (Profile.Kind == EAssetKind::MetaSound) {
      FString Error;
      if (!EnsureMetaSoundAssetIdentity(Actual, Error)) {
        SetFailure(OutFailure, TEXT("audio_published_asset_invalid"), Error);
        return false;
      }
    }
    FAssetRegistryModule::AssetCreated(Actual);
    Actual->MarkPackageDirty();
    return true;
  }

  virtual bool Compile(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (!bCompile || Operation == EOperation::Remove)
      return true;
    FString Error;
    if (!CompileAsset(Profile.Kind, *(Actual ? Actual : Existing), Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    return true;
  }

  virtual bool Save(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (!bSave || Operation == EOperation::Remove)
      return true;
    FString Error;
    if (!SaveAsset(Profile.Kind, *(Actual ? Actual : Existing), Error)) {
      SetFailure(OutFailure, Error);
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
    return BuildReadRequests(
        Operation == EOperation::Remove ? nullptr : Proposed.Get(),
        Operation == EOperation::Remove, OutRequests, OutFailure);
  }

  virtual bool BuildDomainActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    return BuildReadRequests(
        Operation == EOperation::Remove ? nullptr
                                        : (Actual ? Actual : Existing),
        Operation == EOperation::Remove, OutRequests, OutFailure);
  }

  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const override {
    return StatsToJson(Stats);
  }

  virtual bool Commit(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove && RemovalStage.IsStaged()) {
      FString Error;
      if (!DeleteAsset(RemovalStage.GetAsset(), Error)) {
        SetFailure(OutFailure, Error);
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
    FString Error;
    if (Operation == EOperation::Update && Existing &&
        RollbackDocument.IsValid()) {
      FMutationStats RollbackStats;
      if (!ApplyDocument(Profile.Kind, *Existing, RollbackDocument,
                         RollbackStats, Error, PackagePath)) {
        SetFailure(OutFailure, TEXT("audio_rollback_restore_failed"), Error);
        return false;
      }
      Actual = Existing;
      RetireProposed();
      return true;
    }
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Actual) {
      if (!DeleteAsset(Actual, Error)) {
        TArray<UObject *> Objects = {Actual};
        if (ObjectTools::DeleteObjectsUnchecked(Objects) <= 0) {
          SetFailure(OutFailure, Error);
          return false;
        }
      }
      Actual = nullptr;
      RetireProposed();
      return true;
    }
    if (Operation == EOperation::Remove && RemovalStage.IsCaptured()) {
      if (!RemovalStage.Rollback(Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      Actual = Existing;
    }
    RetireProposed();
    return true;
  }

private:
  void RetireProposed() { RetireTransientAsset(Proposed, Actual); }

  bool
  BuildReadRequests(UObject *Asset, const bool bExpectedAbsent,
                    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                    FOperationFailure &OutFailure) const {
    FString Error;
    if (!BuildAssetSnapshotReadRequests(Profile, Asset, PackagePath,
                                        bExpectedAbsent, OutRequests, Error)) {
      SetFailure(OutFailure, TEXT("audio_snapshot_request_failed"), Error);
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
  mutable FMutationStats Stats;
  bool bCompile = true;
  bool bSave = false;
  bool bDocumentNoOp = false;
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
      SetFailure(OutFailure, TEXT("audio_apply_request_invalid"));
      return false;
    }
    const FProfile *Profile =
        FindProfileByTargetKind(Request.Contract->TargetKind);
    if (!Profile) {
      SetFailure(OutFailure, TEXT("audio_apply_profile_missing"),
                 Request.Contract->TargetKind);
      return false;
    }
    FString PackagePath;
    Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
    PackagePath = NormalizePackagePath(PackagePath);
    if (!FPackageName::IsValidLongPackageName(PackagePath) ||
        FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty() ||
        (!PackagePath.Equals(TEXT("/Game")) &&
         !PackagePath.StartsWith(TEXT("/Game/")))) {
      SetFailure(OutFailure, TEXT("invalid_asset_path"), PackagePath);
      return false;
    }
    FString AssetKind;
    if (Request.Input->Target->TryGetStringField(TEXT("asset_kind"),
                                                 AssetKind) &&
        !AssetKind.Equals(Profile->TypeId, ESearchCase::IgnoreCase)) {
      SetFailure(OutFailure, TEXT("audio_asset_kind_conflict"), AssetKind);
      return false;
    }

    UObject *Existing = LoadAsset(Profile->Kind, PackagePath);
    EOperation Operation = Request.Input->RequestedOperation;
    if (Operation == EOperation::Upsert)
      Operation = Existing ? EOperation::Update : EOperation::Create;
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Existing) {
      SetFailure(OutFailure, TEXT("asset_already_exists"), PackagePath);
      return false;
    }
    if ((Operation == EOperation::Update || Operation == EOperation::Remove) &&
        !Existing) {
      SetFailure(OutFailure, TEXT("asset_not_found"), PackagePath);
      return false;
    }

    TSharedPtr<FJsonObject> Document;
    FString Error;
    if (!ExtractDocument(Profile->Kind, Request.Input->ProviderExtension,
                         Document, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    UObject *Source = nullptr;
    if (Operation == EOperation::Clone) {
      FString SourcePath;
      Request.Input->Target->TryGetStringField(TEXT("source_asset_path"),
                                               SourcePath);
      Source = LoadAsset(Profile->Kind, SourcePath);
      if (!Source) {
        SetFailure(OutFailure, TEXT("source_asset_not_found"), SourcePath);
        return false;
      }
    }

    UObject *Proposed = nullptr;
    FMutationStats Stats;
    if (Operation != EOperation::Remove) {
      UPackage *Scratch = CreatePackage(
          *FString::Printf(TEXT("/Temp/UAI/Audio/Proposed_%s"),
                           *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
      UObject *ProposedSource =
          Operation == EOperation::Clone ? Source : Existing;
      if (Profile->Kind == EAssetKind::MetaSound && ProposedSource &&
          !EnsureMetaSoundAssetIdentity(ProposedSource, Error)) {
        SetFailure(OutFailure, TEXT("audio_proposed_source_invalid"), Error);
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
        SetFailure(OutFailure, Error.IsEmpty()
                                   ? TEXT("audio_proposed_asset_create_failed")
                                   : Error);
        return false;
      }
      if (Profile->Kind == EAssetKind::MetaSound) {
        if (!EnsureMetaSoundAssetIdentity(Proposed, Error)) {
          TStrongObjectPtr<UObject> InvalidGuard(Proposed);
          RetireTransientAsset(InvalidGuard);
          SetFailure(OutFailure, TEXT("audio_proposed_asset_invalid"), Error);
          return false;
        }
      }
      TStrongObjectPtr<UObject> ProposedGuard(Proposed);
      if (!ApplyDocument(Profile->Kind, *Proposed, Document, Stats, Error,
                         PackagePath)) {
        RetireTransientAsset(ProposedGuard);
        SetFailure(OutFailure, TEXT("audio_document_preflight_failed"), Error);
        return false;
      }

      const bool bDocumentNoOp =
          Operation == EOperation::Update && Existing &&
          DocumentsEqual(*Existing, *Proposed, Profile->Kind, PackagePath);
      OutState = MakeUnique<FAssetLifecycleState>(
          *Profile, PackagePath, Operation, Existing, Proposed, Document, Stats,
          ReadBool(Request.Input->ProviderExtension, TEXT("compile"), true),
          ReadBool(Request.Input->ProviderExtension, TEXT("save"), false),
          bDocumentNoOp);
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
  Result.ReflectionBaseline.RuntimeLimitsSource = TEXT("shared_runtime_limits");
  Result.PropertyPayload.Mode = TEXT("reflection");
  Result.PropertyPayload.PropertyPathField = TEXT("property_path");
  Result.PropertyPayload.ValueField = TEXT("value");
  Result.PropertyPayload.OptionalAssertionFields = {
      TEXT("value_kind"),   TEXT("struct_path"),  TEXT("enum_path"),
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
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  Document->SetStringField(TEXT("operation"),
                           OperationToString(Request.Operation));
  Document->SetObjectField(TEXT("target"), Target);
  Document->SetObjectField(TEXT("provider_extension"), Extension);
  if (!Request.Properties.IsEmpty())
    Document->SetArrayField(TEXT("properties"), Request.Properties);
  return Document;
}

bool ReadWorkspaceIdentity(
    const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    FString &OutAssetPath, EOperation &OutOperation,
    TSharedPtr<const FJsonObject> &OutDocument) {
  const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *Unit =
      Workspace.FindSingleUnit(TEXT("asset"));
  OutDocument = Unit ? Unit->Document : nullptr;
  const TSharedPtr<FJsonObject> *Target = nullptr;
  FString OperationText;
  return OutDocument.IsValid() &&
         OutDocument->TryGetStringField(TEXT("operation"), OperationText) &&
         ParseOperation(OperationText, OutOperation) &&
         OutDocument->TryGetObjectField(TEXT("target"), Target) && Target &&
         Target->IsValid() &&
         (*Target)->TryGetStringField(TEXT("asset_path"), OutAssetPath);
}

bool LoadWorkspace(
    const FProfile &Profile,
    const UeAgentAuthoringWorkspaceLoader::FSourceRequest &Source,
    UeAgentApplyWorkspaceContract::FContract &OutContract,
    UeAgentAuthoringWorkspaceLoader::FLoadResult &OutLoad,
    FString &OutErrorCode, FString &OutErrorDetail) {
  if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
          Profile.WorkspaceContractId, OutContract)) {
    OutErrorCode = TEXT("audio_workspace_contract_missing");
    return false;
  }
  if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(OutContract, Source,
                                                      OutLoad)) {
    OutErrorCode = OutLoad.ErrorCode;
    OutErrorDetail = OutLoad.ErrorDetail;
    return false;
  }
  return true;
}

bool ValidateExpectedPath(const FString &Actual, const FString &Expected,
                          FString &OutErrorCode, FString &OutErrorDetail) {
  if (Expected.IsEmpty() || Actual.Equals(Expected, ESearchCase::IgnoreCase))
    return true;
  OutErrorCode = TEXT("audio_workspace_asset_path_conflict");
  OutErrorDetail = Actual + TEXT(" != ") + Expected;
  return false;
}

bool BuildDependencyGraph(
    const UeAgentApplyWorkspaceContract::FContract &Contract,
    const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &OutGraph,
    FString &OutErrorCode, FString &OutErrorDetail) {
  UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure Failure;
  if (UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
          Contract, Workspace,
          UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(),
          OutGraph, Failure))
    return true;
  OutErrorCode = Failure.ErrorCode;
  OutErrorDetail = Failure.ErrorDetail;
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
  if (!LoadWorkspace(Profile, Source, Contract, Load, OutResult.ErrorCode,
                     OutResult.ErrorDetail))
    return false;
  TSharedPtr<const FJsonObject> Document;
  EOperation Operation;
  if (!ReadWorkspaceIdentity(Load.Workspace, OutResult.AssetPath, Operation,
                             Document)) {
    OutResult.ErrorCode = TEXT("audio_workspace_entry_invalid");
    return false;
  }
  OutResult.AssetPath = NormalizePackagePath(OutResult.AssetPath);
  OutResult.RequestedOperation = OperationToString(Operation);
  if (!ValidateExpectedPath(OutResult.AssetPath,
                            NormalizePackagePath(ExpectedAssetPath),
                            OutResult.ErrorCode, OutResult.ErrorDetail))
    return false;
  OutResult.bAssetExists =
      LoadAsset(Profile.Kind, OutResult.AssetPath) != nullptr;
  OutResult.bWouldCreate =
      Operation == EOperation::Create ||
      (Operation == EOperation::Upsert && !OutResult.bAssetExists);
  UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
  if (!BuildDependencyGraph(Contract, Load.Workspace, Graph,
                            OutResult.ErrorCode, OutResult.ErrorDetail))
    return false;
  FWorkspaceOperationPreflightProvider Provider(
      UeAgentApplyInputContract::FRegistry::Get(), FProviderRegistry::Get());
  UeAgentAuthoringWorkspaceTransaction::FPreflightResult Preflight;
  if (!UeAgentAuthoringWorkspaceTransaction::PreflightWorkspace(
          Contract, Load.Workspace, Graph, Provider, Preflight)) {
    OutResult.UnitsChecked = Preflight.UnitsChecked;
    OutResult.ErrorCode = Preflight.CauseCode.IsEmpty() ? Preflight.ErrorCode
                                                        : Preflight.CauseCode;
    OutResult.ErrorDetail = Preflight.ErrorDetail;
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
    const FString &RequestId) {
  OutResult = FAssetApplyResult();
  OutResult.Status = TEXT("failed");
  UeAgentApplyWorkspaceContract::FContract Contract;
  UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
  if (!LoadWorkspace(Profile, Source, Contract, Load, OutResult.ErrorCode,
                     OutResult.ErrorDetail)) {
    OutResult.Phase = TEXT("load");
    return false;
  }
  TSharedPtr<const FJsonObject> Document;
  EOperation RequestedOperation;
  if (!ReadWorkspaceIdentity(Load.Workspace, OutResult.AssetPath,
                             RequestedOperation, Document)) {
    OutResult.Phase = TEXT("load");
    OutResult.ErrorCode = TEXT("audio_workspace_entry_invalid");
    return false;
  }
  OutResult.AssetPath = NormalizePackagePath(OutResult.AssetPath);
  if (!ValidateExpectedPath(OutResult.AssetPath,
                            NormalizePackagePath(ExpectedAssetPath),
                            OutResult.ErrorCode, OutResult.ErrorDetail)) {
    OutResult.Phase = TEXT("preflight");
    return false;
  }
  UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
  if (!BuildDependencyGraph(Contract, Load.Workspace, Graph,
                            OutResult.ErrorCode, OutResult.ErrorDetail)) {
    OutResult.Phase = TEXT("preflight");
    return false;
  }
  const bool bExisted = LoadAsset(Profile.Kind, OutResult.AssetPath) != nullptr;
  bool bSave = false;
  const TSharedPtr<FJsonObject> *Extension = nullptr;
  if (Document->TryGetObjectField(TEXT("provider_extension"), Extension) &&
      Extension && Extension->IsValid())
    (*Extension)->TryGetBoolField(TEXT("save"), bSave);
  FBackendOptions BackendOptions;
  BackendOptions.bUseEditorTransaction =
      RequestedOperation != EOperation::Remove;
  BackendOptions.TransactionDescription =
      NSLOCTEXT("UeAgentInterface", "AudioAssetWorkspaceApply",
                "Apply audio asset workspace");
  UeAgentAuthoringWorkspaceTransaction::FExecutionOptions ExecutionOptions;
  FOperationFailure RequestFailure;
  if (!BuildApplyEntryExecutionOptions(RequestId, ExecutionOptions,
                                       RequestFailure)) {
    OutResult.Phase = TEXT("preflight");
    OutResult.ErrorCode = RequestFailure.ErrorCode;
    OutResult.ErrorDetail = RequestFailure.ErrorDetail;
    return false;
  }
  ExecutionOptions.bSaveWhenCallerControlled = bSave;
  FApplyWorkspaceResult ApplyResult;
  const bool bApplied = ApplyWorkspace(
      Contract, Load.Workspace, Graph,
      UeAgentApplyInputContract::FRegistry::Get(), FProviderRegistry::Get(),
      ExecutionOptions, BackendOptions, ApplyResult);
  OutResult.UnitsChecked = ApplyResult.Preflight.UnitsChecked;
  OutResult.MutationsApplied = ApplyResult.Execution.MutationsApplied;
  OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
  OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
  if (!bApplied) {
    const bool bPreflight = !ApplyResult.Preflight.IsReady();
    OutResult.Phase =
        bPreflight ? TEXT("preflight") : ApplyResult.Execution.Phase;
    OutResult.ErrorCode = bPreflight ? ApplyResult.Preflight.CauseCode
                                     : ApplyResult.Execution.CauseCode;
    if (OutResult.ErrorCode.IsEmpty())
      OutResult.ErrorCode = bPreflight ? ApplyResult.Preflight.ErrorCode
                                       : ApplyResult.Execution.ErrorCode;
    OutResult.ErrorDetail = bPreflight ? ApplyResult.Preflight.ErrorDetail
                                       : ApplyResult.Execution.ErrorDetail;
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
  if (!ErrorCode.IsEmpty())
    Json->SetStringField(TEXT("error_code"), ErrorCode);
  if (!ErrorDetail.IsEmpty())
    Json->SetStringField(TEXT("error_detail"), ErrorDetail);
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
  if (!ErrorCode.IsEmpty())
    Json->SetStringField(TEXT("error_code"), ErrorCode);
  if (!ErrorDetail.IsEmpty())
    Json->SetStringField(TEXT("error_detail"), ErrorDetail);
  return Json;
}

bool RegisterApplyContracts(FString &OutError) {
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

TSharedRef<IApplyProvider> MakeApplyProvider() {
  static const TSharedRef<IReflectionOperationLifecycle> Lifecycle =
      MakeShared<FAssetLifecycle>();
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle,
                                              Options);
}

bool ApplyAsset(const FAssetApplyRequest &Request,
                FAssetApplyResult &OutResult, const FString &RequestId) {
  const FProfile *Profile = FindProfile(Request.Kind);
  if (!Profile) {
    OutResult = FAssetApplyResult();
    OutResult.ErrorCode = TEXT("audio_apply_profile_missing");
    return false;
  }
  return ExecuteWorkspaceSource(
      *Profile,
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
          MakeApplyDocument(Request), TEXT("inline://audio-asset-apply")),
      Request.AssetPath, OutResult, RequestId);
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
