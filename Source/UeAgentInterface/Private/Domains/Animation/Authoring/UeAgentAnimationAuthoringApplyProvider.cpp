// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringApplyProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Engine/Blueprint.h"
#include "Editor.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/MetaData.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceInput.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringState.h"

namespace UeAgentAnimationAuthoring {
namespace {
using namespace UeAgentApplyInputContract;
using namespace UeAgentAuthoringOperationApply;

UE::UAI::Contract::FDiagnostic NativeFailure(const FString& Code,
    const FString& Reason, const FString& Target = FString()) {
  auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
  Diagnostic.SourceKind = TEXT("native_result");
  Diagnostic.TargetPath = Target;
  return Diagnostic;
}

void SetFailure(FOperationFailure& OutFailure, const UE::UAI::Contract::FDiagnostic& Diagnostic) {
  OutFailure.Diagnostic = Diagnostic;
}

void MarkNativeSource(UE::UAI::Contract::FDiagnostic& Diagnostic) {
  if (Diagnostic.IsEmpty()) return;
  Diagnostic.SourceKind = TEXT("native_result");
  for (auto& Child : Diagnostic.Diagnostics) MarkNativeSource(Child);
  if (Diagnostic.PublicCause) {
    auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Diagnostic.PublicCause);
    MarkNativeSource(*Cause);
    Diagnostic.PublicCause = MoveTemp(Cause);
  }
}


UE::UAI::Contract::FDiagnostic InputFailure(const FString& Code,
    const FString& Reason, const FJsonObject& Object, const FString& Field,
    const FString& Pointer, const FString& Target = FString()) {
  auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
  Diagnostic.SourceKind = TEXT("related_input");
  Diagnostic.TargetPath = Target;
  Diagnostic.PropertyPath = Field;
  Diagnostic.Source.JsonPath = Pointer;
  const auto Value = Object.TryGetField(Field);
  Diagnostic.ActualValuePresent = Value.IsValid();
  if (Value.IsValid()) Diagnostic.ActualValue = Value;
  UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, Object, Field);
  return Diagnostic;
}

bool ReadBool(const TSharedPtr<const FJsonObject> &Object, const TCHAR *Field,
              const bool DefaultValue) {
  bool Value = DefaultValue;
  if (Object.IsValid())
    Object->TryGetBoolField(Field, Value);
  return Value;
}

TSet<FString> GetDocumentFields(const EAssetKind Kind) {
  switch (Kind) {
  case EAssetKind::PoseAsset:
    return {TEXT("settings"), TEXT("pose_data"), TEXT("pose_data_definitions")};
  case EAssetKind::AnimBlueprint:
    return {TEXT("settings"), TEXT("blueprint"), TEXT("state_machines")};
  case EAssetKind::Skeleton:
    return {TEXT("settings"),    TEXT("sockets"),     TEXT("virtual_bones"),
            TEXT("slot_groups"), TEXT("retargeting"), TEXT("bones")};
  case EAssetKind::SkeletalMesh:
    return {TEXT("settings"),      TEXT("materials"),  TEXT("sockets"),
            TEXT("summary"),       TEXT("lods"),       TEXT("sections"),
            TEXT("morph_targets"), TEXT("import_data")};
  case EAssetKind::AnimSequence:
    return {TEXT("skeleton"), TEXT("frame_rate"), TEXT("number_of_frames"), TEXT("bone_tracks"),
            TEXT("float_curves"), TEXT("transform_curves"), TEXT("notify_tracks"), TEXT("notifies"), TEXT("sync_markers"), TEXT("metadata"),
            TEXT("metadata_changes"), TEXT("curve_changes"), TEXT("event_changes")};
  case EAssetKind::BlendSpace:
    return {TEXT("skeleton"), TEXT("dimension")};
  case EAssetKind::AnimMontage:
    return {TEXT("settings"), TEXT("slots"), TEXT("sections"), TEXT("notify_tracks"), TEXT("notifies"), TEXT("metadata")};
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
    if (Field.Key == TEXT("compile") || Field.Key == TEXT("save"))
      continue;
    if (Kind == EAssetKind::AnimBlueprint && Field.Key == TEXT("blueprint_surfaces"))
      continue;
    if (!DocumentFields.Contains(Field.Key)) {
      OutError = InputFailure(TEXT("animation_provider_extension_unknown_field"),
          TEXT("This field is not supported by the selected animation asset provider."),
          *Extension, Field.Key, TEXT("/provider_extension/")
              + Field.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
      OutError.ActualValue = MakeShared<FJsonValueString>(Field.Key);
      TArray<FString> AllowedFields = DocumentFields.Array();
      AllowedFields.Add(TEXT("compile"));
      AllowedFields.Add(TEXT("save"));
      if (Kind == EAssetKind::AnimBlueprint) AllowedFields.Add(TEXT("blueprint_surfaces"));
      AllowedFields.Sort();
      TArray<TSharedPtr<FJsonValue>> Expected;
      for (const auto& Name : AllowedFields) Expected.Add(MakeShared<FJsonValueString>(Name));
      OutError.ExpectedValue = MakeShared<FJsonValueArray>(Expected);
      return false;
    }
    OutDocument->SetField(Field.Key, Field.Value);
  }
  return true;
}

bool ApplyProviderDocument(const EAssetKind Kind, UObject& Asset,
    const TSharedPtr<const FJsonObject>& Document, FMutationStats& Stats,
    UE::UAI::Contract::FDiagnostic& OutError, const FString& AssetPath) {
  const bool bApplied = ApplyDocument(Kind, Asset, Document, Stats, OutError, AssetPath);
  if (!bApplied && !OutError.IsEmpty()) {
    if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPath;
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/provider_extension"));
  }
  return bApplied;
}

bool DocumentsEqual(UObject &A, UObject &B, const EAssetKind Kind,
                    const FString &CanonicalAssetPath, UE::UAI::Contract::FDiagnostic& OutReadError) {
  TSharedPtr<FJsonObject> ADocument;
  TSharedPtr<FJsonObject> BDocument;
  OutReadError.Reset();
  if (!BuildVerificationDocument(Kind, A, ADocument, OutReadError, CanonicalAssetPath)) {
    MarkNativeSource(OutReadError);
    if (OutReadError.SourceKind.IsEmpty()) OutReadError.SourceKind = TEXT("native_result");
    OutReadError = UE::UAI::Contract::FDiagnostic::WrapFailure(
        TEXT("animation_noop_existing_document_read_failed"), OutReadError, TEXT("noop_comparison"),
        OutReadError.IsEmpty() ? TEXT("The existing animation document could not be read for no-op comparison; native readback returned no diagnostic.") : FString());
    return false;
  }
  if (!BuildVerificationDocument(Kind, B, BDocument, OutReadError, CanonicalAssetPath)) {
    MarkNativeSource(OutReadError);
    if (OutReadError.SourceKind.IsEmpty()) OutReadError.SourceKind = TEXT("native_result");
    OutReadError = UE::UAI::Contract::FDiagnostic::WrapFailure(
        TEXT("animation_noop_proposed_document_read_failed"), OutReadError, TEXT("noop_comparison"),
        OutReadError.IsEmpty() ? TEXT("The proposed animation document could not be read for no-op comparison; native readback returned no diagnostic.") : FString());
    return false;
  }
  return UeAgentAuthoringState::MakeStableCanonicalValueKey(
             MakeShared<FJsonValueObject>(ADocument)) ==
             UeAgentAuthoringState::MakeStableCanonicalValueKey(
                 MakeShared<FJsonValueObject>(BDocument));
}

UObject *DuplicateAsset(UObject *Source, UObject *Outer, const FName Name,
                        const EObjectFlags Flags, UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Source || !Outer) return nullptr;
  UObject* Copy = Cast<UBlueprint>(Source)
      ? UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(CastChecked<UBlueprint>(Source), Outer, Name, false, &OutError)
      : StaticDuplicateObject(Source, Outer, Name, Flags);
  if (Copy) FMetaData::CopyMetadata(Source, Copy);
  return Copy;
}

UObject *PublishAsset(UObject *Source, const FString &PackagePath, UE::UAI::Contract::FDiagnostic& OutError) {
  UPackage *Package = CreatePackage(*PackagePath);
  const FString Name = FPackageName::GetLongPackageAssetName(PackagePath);
  if (auto* Blueprint = Cast<UBlueprint>(Source))
    return UeAgentBlueprintAssetLifecycle::PublishBlueprint(Blueprint, Package, Name, OutError);
  UObject *Published = DuplicateAsset(Source, Package, FName(*Name), RF_AllFlags, OutError);
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
    // Native Blueprint actions can open a toolkit even for a validation copy.
    // End that toolkit's ownership before retiring its only editing object.
    if (GEditor) {
      if (UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()) {
        Editors->CloseAllEditorsForAsset(Asset.Get());
      }
    }
    Asset->ClearFlags(RF_Public | RF_Standalone);
    Asset->MarkAsGarbage();
  }
  Asset.Reset();
}

bool DeleteAsset(UObject *Asset, const FString& Target, UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (!Asset) {
    OutError = NativeFailure(TEXT("animation_delete_target_missing"),
        TEXT("There is no animation asset object to delete."), Target);
    OutError.ActualValue = MakeShared<FJsonValueNull>();
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("animation asset object"));
    return false;
  }
  TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
  if (!ObjectTools::DeleteSingleObject(Asset, true)) {
    OutError = NativeFailure(TEXT("animation_delete_rejected"),
        TEXT("The editor did not delete the animation asset."), Target);
    OutError.InternalContext = Asset->GetPathName();
    OutError.PropertyPath = TEXT("deletion_succeeded");
    OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
    OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
    return false;
  }
  return true;
}

TSharedRef<FJsonObject> StatsToJson(const FMutationStats &Stats) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetNumberField(TEXT("settings"), Stats.Settings);
  Result->SetNumberField(TEXT("sockets"), Stats.Sockets);
  Result->SetNumberField(TEXT("virtual_bones"), Stats.VirtualBones);
  Result->SetNumberField(TEXT("slot_groups"), Stats.SlotGroups);
  Result->SetNumberField(TEXT("retarget_modes"), Stats.RetargetModes);
  Result->SetNumberField(TEXT("compatible_skeletons"),
                         Stats.CompatibleSkeletons);
  Result->SetNumberField(TEXT("materials"), Stats.Materials);
  Result->SetNumberField(TEXT("references"), Stats.References);
  Result->SetNumberField(TEXT("total"), Stats.Total());
  return Result;
}

FMutationStats StatsFromJson(const TSharedPtr<FJsonObject> &Object) {
  FMutationStats Result;
  if (!Object.IsValid())
    return Result;
  Object->TryGetNumberField(TEXT("settings"), Result.Settings);
  Object->TryGetNumberField(TEXT("sockets"), Result.Sockets);
  Object->TryGetNumberField(TEXT("virtual_bones"), Result.VirtualBones);
  Object->TryGetNumberField(TEXT("slot_groups"), Result.SlotGroups);
  Object->TryGetNumberField(TEXT("retarget_modes"), Result.RetargetModes);
  Object->TryGetNumberField(TEXT("compatible_skeletons"),
                            Result.CompatibleSkeletons);
  Object->TryGetNumberField(TEXT("materials"), Result.Materials);
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
                       TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> InBlueprintMutation = {})
      : Profile(InProfile), PackagePath(MoveTemp(InPackagePath)),
        Operation(InOperation), Existing(InExisting), Proposed(InProposed),
        Document(MoveTemp(InDocument)), Stats(InStats), bCompile(bInCompile),
        bSave(bInSave), bDocumentNoOp(bInDocumentNoOp),
        BlueprintMutation(MoveTemp(InBlueprintMutation)) {}

  virtual ~FAssetLifecycleState() override { RetireProposed(); }

  virtual FString GetTargetKey() const override { return PackagePath; }
  virtual void ConfigurePropertyOptions(UeAgentAuthoringObjectApply::FOptions& Options) const override {
    Options.DiagnosticTargetPath = PackagePath;
  }
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
    if (RequiresCheckpointRollback()) return true;
    if (Operation == EOperation::Update && Existing) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!BuildDocument(Profile.Kind, *Existing, RollbackDocument, Error)) {
        MarkNativeSource(Error);
        SetFailure(OutFailure, Error.IsEmpty() ? NativeFailure(TEXT("animation_rollback_capture_failed"),
            TEXT("The animation asset document could not be captured for rollback."), PackagePath) : Error);
        return false;
      }
    } else if (Operation == EOperation::Remove && Existing) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!RemovalStage.Capture(Existing, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
    }
    return true;
  }

  virtual bool ApplyProposed(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Profile.Kind == EAssetKind::BlendSpace && Proposed.IsValid()) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!CompileAsset(Profile.Kind, *Proposed, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
    }
    return true;
  }

  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!RemovalStage.Stage(Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      Actual = nullptr;
      return true;
    }
    if (Operation == EOperation::Update) {
      FMutationStats AppliedStats;
      UE::UAI::Contract::FDiagnostic Error;
      if (!ApplyProviderDocument(Profile.Kind, *Existing, Document, AppliedStats, Error,
                         PackagePath)) {
        SetFailure(OutFailure, Error.IsEmpty() ? NativeFailure(TEXT("animation_document_apply_failed"),
            TEXT("Applying the animation document failed without returning a diagnostic."), PackagePath) : Error);
        return false;
      }
      Stats = AppliedStats;
      if (BlueprintMutation.IsValid()) {
        UeAgentBlueprintAuthoring::FMutationCounters Counters;
        if (!BlueprintMutation->ApplyTo(*CastChecked<UBlueprint>(Existing), Counters, Error)) {
          if (!Error.IsEmpty()) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/provider_extension/blueprint_surfaces"));
          SetFailure(OutFailure, Error.IsEmpty() ? NativeFailure(TEXT("animation_blueprint_mutation_apply_failed"),
              TEXT("Applying the prepared Blueprint surface mutation failed without returning a diagnostic."), PackagePath) : Error);
          return false;
        }
      }
      Actual = Existing;
      bMutationApplied = true;
      return true;
    }
    if (!Proposed.IsValid()) {
      auto Diagnostic = NativeFailure(TEXT("animation_proposed_asset_missing"),
          TEXT("The prepared animation asset is no longer available for publication."), PackagePath);
      Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
      Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Profile.TypeId);
      SetFailure(OutFailure, Diagnostic);
      return false;
    }
    Actual = PublishAsset(Proposed.Get(), PackagePath, OutFailure.Diagnostic);
    if (!Actual) {
      if (!OutFailure.Diagnostic.IsEmpty()) return false;
      auto Diagnostic = NativeFailure(TEXT("animation_asset_publish_failed"),
          TEXT("The editor returned no animation asset when publishing the prepared object."), PackagePath);
      Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
      Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Profile.TypeId);
      Diagnostic.MutationMayHaveStarted = true;
      SetFailure(OutFailure, Diagnostic);
      return false;
    }
    FAssetRegistryModule::AssetCreated(Actual);
    Actual->MarkPackageDirty();
    bMutationApplied = true;
    return true;
  }

  virtual bool Compile(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove)
      return true;
    UE::UAI::Contract::FDiagnostic Error;
    if (bCompile && !CompileAsset(Profile.Kind, *(Actual ? Actual : Existing), Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    if (bCompile && BlueprintMutation.IsValid()) {
      UeAgentBlueprintAuthoring::FMutationCounters Counters;
      if (!BlueprintMutation->FinalizeAfterCompile(
              *CastChecked<UBlueprint>(Actual ? Actual : Existing), Counters, Error)) {
        if (!Error.IsEmpty()) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/provider_extension/blueprint_surfaces"));
        SetFailure(OutFailure, Error.IsEmpty() ? NativeFailure(TEXT("animation_blueprint_defaults_apply_failed"),
            TEXT("Finalizing animation Blueprint defaults failed without returning a diagnostic."), PackagePath) : Error);
        return false;
      }
    }
    const auto* Blueprint = Cast<UBlueprint>(Actual ? Actual : Existing);
    return !Blueprint || UeAgentBlueprintAssetLifecycle::ValidateBlueprintPersistence(*Blueprint, OutFailure.Diagnostic);
  }

  virtual bool Save(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (!bSave || Operation == EOperation::Remove)
      return true;
    UE::UAI::Contract::FDiagnostic Error;
    if (!SaveAsset(Profile.Kind, *(Actual ? Actual : Existing), Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    return true;
  }

  virtual bool BuildDurablePackagePlans(
      TArray<FPackageDurablePlan> &OutPlans,
      FOperationFailure &OutFailure) const override {
    const bool bNewAsset = Operation == EOperation::Create || Operation == EOperation::Clone;
    return BuildPackageDurablePlan(
        PackagePath, PackagePath,
        Operation == EOperation::Remove
            ? Existing
            : (bNewAsset ? Actual : Existing),
        Operation == EOperation::Remove ? EPackageDurableIntent::Delete
                                        : EPackageDurableIntent::Save,
        OutPlans, OutFailure, true,
        bNewAsset ? EPackageBeforePresence::Absent : EPackageBeforePresence::Present,
        Proposed.IsValid() ? Proposed->GetClass() : nullptr);
  }

  virtual bool BuildDomainExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    return BuildReadRequests(
        Operation == EOperation::Remove ? nullptr : Proposed.Get(),
        Operation == EOperation::Remove, OutRequests, OutFailure, true);
  }

  virtual bool BuildDomainActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    return BuildReadRequests(
        Operation == EOperation::Remove ? nullptr
                                        : (Actual ? Actual : Existing),
        Operation == EOperation::Remove, OutRequests, OutFailure, bMutationApplied);
  }

  virtual bool BuildDomainBeforeReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
      FOperationFailure& OutFailure) const override {
    return BuildReadRequests(Operation == EOperation::Remove ? nullptr : (Actual ? Actual : Existing),
        Operation == EOperation::Remove, OutRequests, OutFailure);
  }

  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const override {
    return StatsToJson(Stats);
  }

  virtual bool SupportsPersistentReadback() const override {
    return bSave && (Operation == EOperation::Update || Operation == EOperation::Create || Operation == EOperation::Clone);
  }

  virtual bool RequiresCheckpointRollback() const override {
    return (Profile.Kind == EAssetKind::AnimBlueprint || Profile.Kind == EAssetKind::AnimSequence ||
            Profile.Kind == EAssetKind::BlendSpace || Profile.Kind == EAssetKind::AnimMontage || Profile.Kind == EAssetKind::PoseAsset) &&
           Operation == EOperation::Update && bSave;
  }

  virtual bool PrepareCheckpointRestore(FOperationFailure &OutFailure) override {
    return ReleaseForPersistentReadback(OutFailure);
  }

  virtual bool RebindAfterCheckpointRestore(FOperationFailure &OutFailure) override {
    if (Operation == EOperation::Create || Operation == EOperation::Clone) {
      bool bPresent = false;
      const bool bInspected = UE::UAI::HjsonWorkspace::DomainAuthoring::TryInspectObjectTargetPresence(PackagePath, bPresent);
      if (!bInspected || bPresent) {
        auto Diagnostic = NativeFailure(TEXT("animation_absent_restore_unconfirmed"),
            !bInspected ? TEXT("The target presence could not be inspected after checkpoint restore.")
                : TEXT("The animation asset is still present after restoring its expected absence."), PackagePath);
        Diagnostic.PropertyPath = bInspected ? TEXT("asset_present") : TEXT("presence_inspection_succeeded");
        Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(bInspected ? bPresent : false);
        Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(!bInspected);
        SetFailure(OutFailure, Diagnostic);
        return false;
      }
      return true;
    }
    return RebindAfterPersistentReadback(OutFailure);
  }

  virtual bool
  ReleaseForPersistentReadback(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    RetireProposed();
    Existing = nullptr;
    Actual = nullptr;
    return true;
  }

  virtual bool
  RebindAfterPersistentReadback(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    Existing = LoadAsset(Profile.Kind, PackagePath);
    Actual = Existing;
    if (!Existing) {
      auto Diagnostic = NativeFailure(TEXT("animation_persistent_reload_failed"),
          TEXT("The persisted target could not be loaded as the expected animation asset type."), PackagePath);
      Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
      Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Profile.TypeId);
      SetFailure(OutFailure, Diagnostic);
      return false;
    }
    return true;
  }

  virtual bool Commit(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove && RemovalStage.IsStaged()) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!DeleteAsset(RemovalStage.GetAsset(), PackagePath, Error)) {
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
    UE::UAI::Contract::FDiagnostic Error;
    if (Operation == EOperation::Update && Existing &&
        RollbackDocument.IsValid()) {
      FMutationStats RollbackStats;
      if (!ApplyDocument(Profile.Kind, *Existing, RollbackDocument,
                         RollbackStats, Error, PackagePath)) {
        // Rollback restores a captured native document, not the new authored input.
        MarkNativeSource(Error);
        SetFailure(OutFailure, Error.IsEmpty() ? NativeFailure(TEXT("animation_rollback_restore_failed"),
            TEXT("Restoring the captured animation document failed without returning a diagnostic."), PackagePath) : Error);
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
          auto Fallback = NativeFailure(TEXT("animation_rollback_delete_failed"),
              TEXT("The fallback deletion also failed to remove the created animation asset."), PackagePath);
          Fallback.Stage = TEXT("rollback");
          Fallback.PropertyPath = TEXT("deleted_objects");
          Fallback.ActualValue = MakeShared<FJsonValueNumber>(DeletedCount);
          Fallback.ExpectedValue = MakeShared<FJsonValueNumber>(1);
          if (Error.IsEmpty()) Error = MoveTemp(Fallback);
          else Error.Append(MoveTemp(Fallback));
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
                    FOperationFailure &OutFailure, bool bVerifyIntent = false) const {
    UE::UAI::Contract::FDiagnostic Error;
    if (bVerifyIntent && !bExpectedAbsent && Asset && BlueprintMutation
        && !BlueprintMutation->VerifyIntent(*CastChecked<UBlueprint>(Asset), Error)) {
      if (!Error.IsEmpty()) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/provider_extension/blueprint_surfaces"));
      UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("animation_blueprint_authored_intent_failed"), Error, TEXT("animation_readback"));
      Error.Reset();
    }
    if (!BuildAssetSnapshotReadRequests(Profile, Asset, PackagePath,
                                        bExpectedAbsent, OutRequests, Error)) {
      SetFailure(OutFailure, Error.IsEmpty() ? NativeFailure(TEXT("animation_snapshot_request_failed"),
          TEXT("The animation snapshot read requests could not be constructed."), PackagePath) : Error);
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
  bool bMutationApplied = false;
  TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> BlueprintMutation;
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
      auto Diagnostic = NativeFailure(TEXT("animation_apply_request_invalid"),
          TEXT("Animation apply requires a contract, workspace unit, operation input, and target object."));
      const auto Actual = MakeShared<FJsonObject>();
      Actual->SetBoolField(TEXT("contract"), Request.Contract != nullptr);
      Actual->SetBoolField(TEXT("unit"), Request.Unit != nullptr);
      Actual->SetBoolField(TEXT("input"), Request.Input != nullptr);
      Actual->SetBoolField(TEXT("target"), Request.Input && Request.Input->Target.IsValid());
      const auto Expected = MakeShared<FJsonObject>();
      for (const auto& Field : Actual->Values) Expected->SetBoolField(Field.Key, true);
      Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
      Diagnostic.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
      SetFailure(OutFailure, Diagnostic);
      return false;
    }
    const FProfile *Profile =
        FindProfileByTargetKind(Request.Contract->TargetKind);
    if (!Profile) {
      auto Diagnostic = NativeFailure(TEXT("animation_apply_profile_missing"),
          TEXT("No animation authoring profile is registered for the operation's target kind."));
      Diagnostic.InternalContext = Request.Contract->TargetKind;
      Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
      Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered animation authoring profile"));
      SetFailure(OutFailure, Diagnostic);
      return false;
    }
    FString PackagePath;
    Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
    PackagePath = NormalizePackagePath(PackagePath);
    if (!FPackageName::IsValidLongPackageName(PackagePath) ||
        FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty() ||
        (!PackagePath.Equals(TEXT("/Game")) &&
         !PackagePath.StartsWith(TEXT("/Game/")))) {
      auto Diagnostic = InputFailure(TEXT("invalid_asset_path"),
          TEXT("The animation target must be a valid /Game package path containing an asset name."),
          *Request.Input->Target, TEXT("asset_path"), TEXT("/target/asset_path"), PackagePath);
      Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("valid /Game/.../AssetName package path"));
      SetFailure(OutFailure, Diagnostic);
      return false;
    }
    FString AssetKind;
    if (Request.Input->Target->TryGetStringField(TEXT("asset_kind"),
                                                 AssetKind) &&
        !AssetKind.Equals(Profile->TypeId, ESearchCase::IgnoreCase)) {
      auto Diagnostic = InputFailure(TEXT("animation_asset_kind_conflict"),
          TEXT("The asserted asset kind differs from the animation provider's asset kind."),
          *Request.Input->Target, TEXT("asset_kind"), TEXT("/target/asset_kind"), PackagePath);
      Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Profile->TypeId);
      SetFailure(OutFailure, Diagnostic);
      return false;
    }

    UObject *Existing = LoadAsset(Profile->Kind, PackagePath);
    EOperation Operation = Request.Input->RequestedOperation;
    if (Operation == EOperation::Upsert)
      Operation = Existing ? EOperation::Update : EOperation::Create;
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Existing) {
      auto Diagnostic = InputFailure(TEXT("asset_already_exists"),
          FString::Printf(TEXT("The %s operation requires an unoccupied target asset path."), *OperationToString(Operation)),
          *Request.Input->Target, TEXT("asset_path"), TEXT("/target/asset_path"), PackagePath);
      Diagnostic.SourceKind = TEXT("related_input");
      Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
      Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
      SetFailure(OutFailure, Diagnostic);
      return false;
    }
    if ((Operation == EOperation::Update || Operation == EOperation::Remove) &&
        !Existing) {
      auto Diagnostic = InputFailure(TEXT("asset_not_found"),
          FString::Printf(TEXT("The %s target could not be loaded as the expected animation asset type."), *OperationToString(Operation)),
          *Request.Input->Target, TEXT("asset_path"), TEXT("/target/asset_path"), PackagePath);
      Diagnostic.SourceKind = TEXT("related_input");
      Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
      Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Profile->TypeId);
      SetFailure(OutFailure, Diagnostic);
      return false;
    }

    TSharedPtr<FJsonObject> Document;
    UE::UAI::Contract::FDiagnostic Error;
    if (!ExtractDocument(Profile->Kind, Request.Input->ProviderExtension,
                         Document, Error)) {
      if (Error.TargetPath.IsEmpty()) Error.TargetPath = PackagePath;
      SetFailure(OutFailure, Error);
      return false;
    }
    TSharedPtr<const FJsonObject> BlueprintSurfaces;
    if (Request.Input->ProviderExtension.IsValid() &&
        Request.Input->ProviderExtension->HasField(TEXT("blueprint_surfaces"))) {
      const TSharedPtr<FJsonObject>* Surfaces = nullptr;
      if (Profile->Kind != EAssetKind::AnimBlueprint ||
          !Request.Input->ProviderExtension->TryGetObjectField(TEXT("blueprint_surfaces"), Surfaces) ||
          !Surfaces || !Surfaces->IsValid()) {
        auto Diagnostic = InputFailure(TEXT("animation_blueprint_surfaces_invalid"),
            Profile->Kind != EAssetKind::AnimBlueprint
                ? TEXT("blueprint_surfaces is supported only for animation Blueprint assets.")
                : TEXT("blueprint_surfaces must be an object."),
            *Request.Input->ProviderExtension, TEXT("blueprint_surfaces"), TEXT("/provider_extension/blueprint_surfaces"), PackagePath);
        if (Profile->Kind != EAssetKind::AnimBlueprint) {
          Diagnostic.ActualValue = MakeShared<FJsonValueString>(Profile->TypeId);
          Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(AssetKindToString(EAssetKind::AnimBlueprint));
          Diagnostic.SourceKind = TEXT("related_input");
        } else Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
        SetFailure(OutFailure, Diagnostic);
        return false;
      }
      BlueprintSurfaces = *Surfaces;
      if (!BlueprintSurfaces->Values.IsEmpty() &&
          (Operation != EOperation::Update ||
           !ReadBool(Request.Input->ProviderExtension, TEXT("compile"), true) ||
           !ReadBool(Request.Input->ProviderExtension, TEXT("save"), false))) {
        auto Diagnostic = InputFailure(TEXT("animation_blueprint_surfaces_persistent_update_required"),
            TEXT("Non-empty Blueprint surfaces require update with compile=true and save=true."),
            *Request.Input->ProviderExtension, TEXT("blueprint_surfaces"), TEXT("/provider_extension/blueprint_surfaces"), PackagePath);
        const auto Actual = MakeShared<FJsonObject>();
        Actual->SetStringField(TEXT("operation"), OperationToString(Operation));
        Actual->SetBoolField(TEXT("compile"), ReadBool(Request.Input->ProviderExtension, TEXT("compile"), true));
        Actual->SetBoolField(TEXT("save"), ReadBool(Request.Input->ProviderExtension, TEXT("save"), false));
        const auto Expected = MakeShared<FJsonObject>();
        Expected->SetStringField(TEXT("operation"), TEXT("update"));
        Expected->SetBoolField(TEXT("compile"), true);
        Expected->SetBoolField(TEXT("save"), true);
        Diagnostic.SourceKind = TEXT("related_input");
        Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
        Diagnostic.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
        SetFailure(OutFailure, Diagnostic);
        return false;
      }
    }
    UObject *Source = nullptr;
    if (Operation == EOperation::Clone) {
      FString SourcePath;
      Request.Input->Target->TryGetStringField(TEXT("source_asset_path"),
                                               SourcePath);
      Source = LoadAsset(Profile->Kind, SourcePath);
      if (!Source) {
        auto Diagnostic = InputFailure(TEXT("source_asset_not_found"),
            TEXT("The clone source could not be loaded as the expected animation asset type."),
            *Request.Input->Target, TEXT("source_asset_path"), TEXT("/target/source_asset_path"), PackagePath);
        Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("loadable ") + Profile->TypeId + TEXT(" asset path"));
        SetFailure(OutFailure, Diagnostic);
        return false;
      }
    }

    UObject *Proposed = nullptr;
    FMutationStats Stats;
    if (Operation != EOperation::Remove) {
      UPackage *Scratch = CreatePackage(
          *FString::Printf(TEXT("/Temp/UAI/Animation/Proposed_%s"),
                           *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
      FFactorySeedDiagnosticSource SeedSource;
      SeedSource.TargetPath = PackagePath;
      SeedSource.Skeleton.JsonPath = TEXT("/provider_extension/skeleton");
      SeedSource.Dimension.JsonPath = TEXT("/provider_extension/dimension");
      SeedSource.Settings.JsonPath = TEXT("/provider_extension/settings");
      Proposed = Operation == EOperation::Create
                     ? CreateFactorySeed(Profile->Kind, Scratch, NAME_None,
                                         RF_Transactional, Document, Error, SeedSource)
                     : DuplicateAsset(Operation == EOperation::Clone ? Source
                                                                     : Existing,
                                      Scratch, NAME_None, RF_Transactional, Error);
      if (!Proposed) {
        if (Error.IsEmpty()) {
          Error = NativeFailure(TEXT("animation_proposed_asset_create_failed"),
              Operation == EOperation::Create ? TEXT("The factory returned no proposed animation asset.")
                  : TEXT("The editor returned no proposed animation asset when duplicating the source."), PackagePath);
          Error.ActualValue = MakeShared<FJsonValueNull>();
          Error.ExpectedValue = MakeShared<FJsonValueString>(Profile->TypeId);
        }
        SetFailure(OutFailure, Error);
        return false;
      }
      TStrongObjectPtr<UObject> ProposedGuard(Proposed);
      if (UBlueprint* ProposedBlueprint = Cast<UBlueprint>(Proposed))
        UeAgentBlueprintAssetLifecycle::SetAuthoringContext(
            *ProposedBlueprint, PackagePath, Cast<UBlueprint>(Existing));
      auto ApplyProposedDocument = [&]() {
        if (!UeAgentNativeNormalizationIntent::IsActive())
          return ApplyProviderDocument(Profile->Kind, *Proposed, Document, Stats, Error, PackagePath);
        TArray<UeAgentAuthoringState::FReadRequest> Requests;
        if (!BuildAssetSnapshotReadRequests(*Profile, Proposed, PackagePath, false, Requests, Error)) return false;
        UeAgentNativeNormalizationIntent::FValueScope Scope(Requests[0].TargetIdentity, Requests[0].SemanticId);
        return ApplyProviderDocument(Profile->Kind, *Proposed, Document, Stats, Error, PackagePath);
      };
      if (!ApplyProposedDocument()) {
        RetireTransientAsset(ProposedGuard);
        SetFailure(OutFailure, Error.IsEmpty() ? NativeFailure(TEXT("animation_document_preflight_failed"),
            TEXT("Applying the animation document to the proposed asset failed without returning a diagnostic."), PackagePath) : Error);
        return false;
      }

      TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> BlueprintMutation;
      if (BlueprintSurfaces.IsValid() && !BlueprintSurfaces->Values.IsEmpty()) {
        UeAgentBlueprintAuthoring::FMutationCounters Counters;
        if (!UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
                *CastChecked<UBlueprint>(Proposed), BlueprintSurfaces,
                BlueprintMutation, Counters, Error)) {
          if (!Error.IsEmpty()) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/provider_extension/blueprint_surfaces"));
          RetireTransientAsset(ProposedGuard);
          SetFailure(OutFailure, Error.IsEmpty() ? NativeFailure(TEXT("animation_blueprint_mutation_preflight_failed"),
              TEXT("Preparing the proposed animation Blueprint surfaces failed without returning a diagnostic."), PackagePath) : Error);
          return false;
        }
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
          bDocumentNoOp, MoveTemp(BlueprintMutation));
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
  const TArray<EOperation> All =
          TArray<EOperation>{EOperation::Create, EOperation::Update,
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
  if (Profile.Kind == EAssetKind::AnimBlueprint)
    Result.InputFields.Add(Field(TEXT("blueprint_surfaces"),
                                 TEXT("provider_extension"), TEXT("object")));
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
  if (Request.BlueprintSurfaces.IsValid())
    Extension->SetObjectField(TEXT("blueprint_surfaces"), Request.BlueprintSurfaces);
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
    OutDiagnostic = NativeFailure(TEXT("animation_workspace_contract_missing"),
        TEXT("The animation workspace contract is not registered."));
    OutDiagnostic.InternalContext = Profile.WorkspaceContractId;
    OutDiagnostic.ActualValue = MakeShared<FJsonValueNull>();
    OutDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered animation workspace contract"));
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
  if (!LoadWorkspace(Profile, Source, Contract, Load, OutResult.Diagnostic)) {
    if (OutResult.Diagnostic.TargetPath.IsEmpty()) OutResult.Diagnostic.TargetPath = ExpectedAssetPath;
    return false;
  }
  UeAgentAuthoringAssetWorkspaceInput::FTargetRequest TargetRequest;
  TargetRequest.ExpectedAssetPath = ExpectedAssetPath;
  TargetRequest.Errors.EntryInvalid = TEXT("animation_workspace_entry_invalid");
  TargetRequest.Errors.AssetPathConflict =
      TEXT("animation_workspace_asset_path_conflict");
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
    TSharedPtr<IDurablePackageStager> PackageStager = {}) {
  OutResult = FAssetApplyResult();
  OutResult.Status = TEXT("failed");
  UeAgentApplyWorkspaceContract::FContract Contract;
  UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
  if (!LoadWorkspace(Profile, Source, Contract, Load, OutResult.Diagnostic)) {
    OutResult.Phase = TEXT("load");
    if (OutResult.Diagnostic.TargetPath.IsEmpty()) OutResult.Diagnostic.TargetPath = ExpectedAssetPath;
    return false;
  }
  UeAgentAuthoringAssetWorkspaceInput::FTargetRequest TargetRequest;
  TargetRequest.ExpectedAssetPath = ExpectedAssetPath;
  TargetRequest.Errors.EntryInvalid = TEXT("animation_workspace_entry_invalid");
  TargetRequest.Errors.AssetPathConflict =
      TEXT("animation_workspace_asset_path_conflict");
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
  BackendOptions.PackageStager = MoveTemp(PackageStager);
  BackendOptions.bUseEditorTransaction =
      RequestedOperation != EOperation::Remove;
  BackendOptions.TransactionDescription =
      NSLOCTEXT("UeAgentInterface", "AnimationAssetWorkspaceApply",
                "Apply animation asset workspace");
  FApplyWorkspaceResult ApplyResult;
  UeAgentAuthoringAssetWorkspaceApply::FFailure ApplyFailure;
  const bool bApplied = UeAgentAuthoringAssetWorkspaceApply::Execute(
      Contract, Load.Workspace, FProviderRegistry::Get(), RequestId, bSave,
      BackendOptions, ApplyResult, ApplyFailure);
  OutResult.UnitsChecked = ApplyResult.Preflight.UnitsChecked;
  OutResult.MutationsApplied = ApplyResult.Execution.MutationsApplied;
  OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
  OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
  if (!bApplied) {
    OutResult.Phase = ApplyFailure.Phase;
    OutResult.Diagnostic = ApplyFailure.Diagnostic;
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

TSharedRef<IApplyProvider> MakeApplyProvider() {
  static const TSharedRef<IReflectionOperationLifecycle> Lifecycle =
      MakeShared<FAssetLifecycle>();
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bCompactValues = true;
  return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle,
                                              Options);
}

bool ApplyAsset(const FAssetApplyRequest &Request,
                FAssetApplyResult &OutResult, const FString &RequestId) {
  const FProfile *Profile = FindProfile(Request.Kind);
  if (!Profile) {
    OutResult = FAssetApplyResult();
    OutResult.Phase = TEXT("preflight");
    OutResult.Diagnostic = NativeFailure(TEXT("animation_apply_profile_missing"),
        TEXT("No animation authoring profile is registered for the requested asset kind."), Request.AssetPath);
    OutResult.Diagnostic.Stage = OutResult.Phase;
    const auto Actual = MakeShared<FJsonObject>();
    Actual->SetStringField(TEXT("enum_type"), TEXT("animation asset kind"));
    Actual->SetStringField(TEXT("name"), AssetKindToString(Request.Kind));
    Actual->SetNumberField(TEXT("value"), static_cast<int32>(Request.Kind));
    OutResult.Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
    OutResult.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered animation asset kind"));
    return false;
  }
  return ExecuteWorkspaceSource(
      *Profile,
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
          MakeApplyDocument(Request), TEXT("inline://animation-asset-apply")),
      Request.AssetPath, OutResult, RequestId, Request.FinalReadback, Request.PackageStager);
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
} // namespace UeAgentAnimationAuthoring
