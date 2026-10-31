// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringPackageReadback.h"
#include "Utilities/AuthoringObjectApply/UeAgentTemplatePropertyPropagation.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"
#include "Core/AssetDurable/UeAgentPackageSave.h"
#include "Utilities/UeAgentFileOperationDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"

#include "Module/UeAgentInterfaceLogger.h"

#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Containers/UnrealString.h"
#include "ScopedTransaction.h"
#include "PackageTools.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/LinkerLoad.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace {
using namespace UeAgentAuthoringOperationApply;


FString NormalizeId(FString Value) {
  Value.TrimStartAndEndInline();
  Value.ToLowerInline();
  return Value;
}

void SetFailure(FOperationFailure& OutFailure, const FString& Code,
                const FString& PublicReason) {
  OutFailure.Diagnostic = UeAgentAuthoringDiagnostics::FDiagnostic::Failure(Code, PublicReason, TEXT("operation_apply"));
}

void SetPackageArtifactFailure(FOperationFailure& OutFailure,
                              const FString& PackageName,
                              const TCHAR* Cause, const TCHAR* Message) {
  UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
  Diagnostic.Code = TEXT("durable_package_artifact_unrecoverable");
  Diagnostic.CauseCode = Cause;
  Diagnostic.PublicMessage = Message;
  Diagnostic.Stage = TEXT("durable_package_resolution");
  Diagnostic.SubjectId = PackageName;
  // Package names can include private working-copy identities. Keep the exact
  // path in the developer log and expose the actionable cause through the same
  // diagnostic carrier used by every authoring consumer.
  FUeAgentInterfaceLogger::Log(TEXT("Durable package resolution failed: package=%s cause=%s"),
                              *PackageName, Cause);
  OutFailure.Diagnostic = MoveTemp(Diagnostic);
}

void SetSemanticComparisonFailure(
    FOperationFailure& OutFailure,
    const TArray<UeAgentAuthoringState::FReadRequest>& ActualRequests,
    const UeAgentAuthoringSemanticCompare::FComparisonReport& Report,
    bool bRestoring) {
  using UeAgentAuthoringDiagnostics::FDiagnostic;
  FDiagnostic Diagnostic = FDiagnostic::WrapFailure(
      Report.Diagnostic.IsValid() ? Report.Diagnostic.Code : TEXT("operation_semantic_verification_failed"),
      Report.Diagnostic, TEXT("semantic_verification"));
  // A failed comparator may stop before the report aggregates earlier differences.
  if (Report.Status != TEXT("different")) {
    for (const auto& Entry : Report.Entries) {
      if (!Entry.bEquivalent && Entry.Diagnostic.IsValid()
          && (Entry.Diagnostic.Code != Report.Diagnostic.Code
              || Entry.Diagnostic.SemanticId != Report.Diagnostic.SemanticId
              || Entry.Diagnostic.SubjectId != Report.Diagnostic.SubjectId)) {
        Diagnostic.Append(Entry.Diagnostic);
      }
    }
  }
  TFunction<void(FDiagnostic&)> BindSource = [&](FDiagnostic& Item) {
    const auto* Request = ActualRequests.FindByPredicate([&Item](const auto& Candidate) {
      return !Item.SemanticId.IsEmpty() && Candidate.SemanticId == Item.SemanticId
          && (Item.SubjectId.IsEmpty() || Candidate.TargetIdentity.CanonicalKey == Item.SubjectId);
    });
    if (Request && Item.TargetPath.IsEmpty()) Item.TargetPath = Request->RootOwnerPath;
    if (Item.PropertyPath.IsEmpty() && !Item.Source.JsonPath.IsEmpty()) {
      FString Parent, Field;
      if (Item.Source.JsonPath.Split(TEXT("/"), &Parent, &Field, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) {
        FGuid Identity;
        bool bFieldName = !Field.IsEmpty();
        for (TCHAR Character : Field) bFieldName &= FChar::IsAlnum(Character) || Character == TEXT('_');
        if (bFieldName && !Field.IsNumeric() && !FGuid::Parse(Field, Identity))
          Item.PropertyPath = Field.Replace(TEXT("~1"), TEXT("/")).Replace(TEXT("~0"), TEXT("~"));
      }
    }
    for (auto& Child : Item.Diagnostics) BindSource(Child);
  };
  BindSource(Diagnostic);
  if (bRestoring) {
    Diagnostic = FDiagnostic::WrapFailure(TEXT("operation_restore_semantic_unconfirmed"), Diagnostic,
        TEXT("semantic_restore"), TEXT("Restored semantic state differs from the before state; see diagnostics."));
    Diagnostic.Stage = TEXT("semantic_restore");
  }
  OutFailure.Diagnostic = MoveTemp(Diagnostic);
  UE_LOG(LogUeAgentInterface, Display,
         TEXT("StructuredApply phase=semantic_comparison_failed restoring=%d code=%s"),
         bRestoring, *OutFailure.Diagnostic.Code);
}

FString StableDocumentKey(const TSharedPtr<const FJsonObject> &Document) {
  if (!Document.IsValid())
    return FString();
  const TSharedPtr<FJsonObject> MutableView =
      ConstCastSharedPtr<FJsonObject>(Document);
  return UeAgentAuthoringState::MakeStableCanonicalValueKey(
      MakeShared<FJsonValueObject>(MutableView));
}

bool IsValidResolvedOperation(UeAgentApplyInputContract::EOperation Requested,
                              UeAgentApplyInputContract::EOperation Resolved) {
  using UeAgentApplyInputContract::EOperation;
  if (Requested == EOperation::Upsert) {
    return Resolved == EOperation::Create || Resolved == EOperation::Update;
  }
  return Requested == Resolved;
}

FString NormalizeAbsoluteFilename(FString Path) {
  Path = FPaths::ConvertRelativePathToFull(Path);
  FPaths::NormalizeFilename(Path);
  return Path;
}

FString DurableJournalRoot() {
  return NormalizeAbsoluteFilename(
      FPaths::ProjectSavedDir() /
      TEXT("UeAgentInterface/DurableTransactions"));
}

bool ResolvePackageArtifactPaths(const FPackageDurablePlan &Plan,
                                 FString &OutHeaderPath,
                                 FString &OutPayloadPath,
                                 FOperationFailure &OutFailure) {
  UPackage *Package = Plan.Package.Get();
  const FString PackageName = Plan.PackageName.TrimStartAndEnd();
  if (!Package && Plan.BeforePresence != EPackageBeforePresence::Absent && Plan.OwningPackageName.IsEmpty()) {
    SetPackageArtifactFailure(OutFailure, PackageName, TEXT("package_not_resident"),
                              TEXT("An existing package in the save set is no longer loaded."));
    return false;
  }
  if (Package == GetTransientPackage()) {
    SetPackageArtifactFailure(OutFailure, PackageName, TEXT("transient_package_in_save_set"),
                              TEXT("A transient working package was included in the persistent save set."));
    return false;
  }
  if (!FPackageName::IsValidLongPackageName(PackageName) || FPackageName::GetPackageMountPoint(PackageName).IsNone()) {
    SetPackageArtifactFailure(OutFailure, PackageName, TEXT("package_path_not_mounted"),
                              TEXT("A package in the save set has no valid mounted content path."));
    return false;
  }
  if (Package && Package->GetName() != PackageName) {
    SetPackageArtifactFailure(OutFailure, PackageName, TEXT("package_identity_changed"),
                              TEXT("A package changed identity after the persistent save plan was prepared."));
    return false;
  }

  const UObject *TopLevelAsset = Plan.TopLevelAsset.Get();
  const bool bMapPackage = (Package && Package->ContainsMap()) ||
                           (TopLevelAsset && TopLevelAsset->IsA<UWorld>());
  const FString Extension =
      bMapPackage ? FPackageName::GetMapPackageExtension()
                  : FPackageName::GetAssetPackageExtension();
  if (!FPackageName::TryConvertLongPackageNameToFilename(
          PackageName, OutHeaderPath, Extension)) {
    SetPackageArtifactFailure(OutFailure, PackageName, TEXT("package_filename_unresolved"),
                              TEXT("The package content path could not be resolved to a file for persistent save."));
    return false;
  }
  OutHeaderPath = NormalizeAbsoluteFilename(OutHeaderPath);
  OutPayloadPath = FPaths::ChangeExtension(OutHeaderPath, TEXT("upayload"));
  return true;
}

FString HeaderArtifactId(const FPackageDurablePlan &Plan) {
  return Plan.PlanId + TEXT(":header");
}

FString PayloadArtifactId(const FPackageDurablePlan &Plan) {
  return Plan.PlanId + TEXT(":payload");
}

void ReleasePackageLoaders(const TArray<FPackageDurablePlan> &Plans) {
  for (const FPackageDurablePlan &Plan : Plans) {
    if (UPackage *Package = Plan.Package.Get())
      ResetLoaders(Package);
  }
}

class FDefaultDurablePackageStager final : public IDurablePackageStager {
public:
  virtual bool BuildArtifactInventory(
      const TArray<FPackageDurablePlan> &Plans,
      TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>
          &OutArtifacts,
      FOperationFailure &OutFailure) override {
    OutArtifacts.Reset();
    OutFailure = FOperationFailure();
    TSet<FString> PlanIds;
    TSet<FString> PackagePaths;
    for (const FPackageDurablePlan &Plan : Plans) {
      const FString PlanId = Plan.PlanId.TrimStartAndEnd();
      FString HeaderPath;
      FString PayloadPath;
      if (PlanId.IsEmpty() ||
          !ResolvePackageArtifactPaths(Plan, HeaderPath, PayloadPath,
                                       OutFailure)) {
        if (OutFailure.Diagnostic.Code.IsEmpty())
          SetFailure(OutFailure, TEXT("durable_package_plan_invalid"), TEXT("The durable package plan identifier is empty."));
        OutArtifacts.Reset();
        return false;
      }
      const FString FoldedPlanId = PlanId.ToLower();
      const FString FoldedPackagePath = HeaderPath.ToLower();
      if (PlanIds.Contains(FoldedPlanId) ||
          PackagePaths.Contains(FoldedPackagePath)) {
        SetFailure(OutFailure, TEXT("durable_package_plan_duplicate"),
            PlanIds.Contains(FoldedPlanId) ? TEXT("A durable plan identifier is declared more than once.")
            : TEXT("Multiple durable plans refer to the same package file."));
        OutFailure.Diagnostic.PropertyPath = PlanIds.Contains(FoldedPlanId) ? TEXT("plan_id") : TEXT("package_file");
        OutFailure.Diagnostic.SubjectId = PlanId;
        OutFailure.Diagnostic.InternalContext = HeaderPath;
        OutArtifacts.Reset();
        return false;
      }
      PlanIds.Add(FoldedPlanId);
      PackagePaths.Add(FoldedPackagePath);

      UeAgentAuthoringWorkspaceTransaction::FDurableArtifact &Header =
          OutArtifacts.AddDefaulted_GetRef();
      Header.ArtifactId = HeaderArtifactId(Plan);
      Header.FinalPath = MoveTemp(HeaderPath);
      UeAgentAuthoringWorkspaceTransaction::FDurableArtifact &Payload =
          OutArtifacts.AddDefaulted_GetRef();
      Payload.ArtifactId = PayloadArtifactId(Plan);
      Payload.FinalPath = MoveTemp(PayloadPath);
    }
    return true;
  }

  virtual bool StagePackages(
      const TArray<FPackageDurablePlan> &Plans,
      TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>
          &InOutArtifacts,
      FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>
        ExpectedArtifacts;
    if (!BuildArtifactInventory(Plans, ExpectedArtifacts, OutFailure))
      return false;
    if (ExpectedArtifacts.Num() != InOutArtifacts.Num()) {
      SetFailure(OutFailure, TEXT("durable_package_stage_inventory_mismatch"), TEXT("The staged artifact inventory has a different number of entries than the package plans."));
      OutFailure.Diagnostic.PropertyPath = TEXT("artifacts/length");
      OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(ExpectedArtifacts.Num());
      OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(InOutArtifacts.Num());
      return false;
    }

    auto FindAssigned = [&InOutArtifacts](const FString &ArtifactId)
        -> UeAgentAuthoringWorkspaceTransaction::FDurableArtifact * {
      return InOutArtifacts.FindByPredicate(
          [&ArtifactId](const auto &Artifact) {
            return Artifact.ArtifactId == ArtifactId;
          });
    };
    const auto RemoveStagingFile = [&OutFailure](const FString& Path) {
      if (Path.IsEmpty() || IFileManager::Get().Delete(*Path, false, true, true)) return true;
      OutFailure.Diagnostic.Append(UE::UAI::FileDiagnostics::MakeFileOperationFailure(
          TEXT("durable_package_stage_cleanup_failed"), TEXT("A temporary package staging file could not be removed."), TEXT("staging_cleanup"), Path));
      return false;
    };
    auto CleanupAssigned = [&InOutArtifacts, &RemoveStagingFile]() {
      bool bClean = true;
      for (auto &Artifact : InOutArtifacts) {
        const bool bRemoved = RemoveStagingFile(Artifact.StagingPath);
        bClean &= bRemoved;
        if (bRemoved) Artifact.bStagedExists = false;
      }
      return bClean;
    };

    for (const auto &Expected : ExpectedArtifacts) {
      const auto *Assigned = FindAssigned(Expected.ArtifactId);
      if (!Assigned ||
          !FPaths::IsSamePath(Assigned->FinalPath, Expected.FinalPath) ||
          Assigned->StagingPath.IsEmpty() ||
          FPaths::IsRelative(Assigned->StagingPath)) {
        SetFailure(OutFailure, TEXT("durable_package_stage_inventory_mismatch"),
            !Assigned ? TEXT("A declared artifact is missing from the staging inventory.")
            : !FPaths::IsSamePath(Assigned->FinalPath, Expected.FinalPath) ? TEXT("The assigned artifact destination differs from the package plan.")
            : Assigned->StagingPath.IsEmpty() ? TEXT("The assigned artifact has no staging path.")
            : TEXT("The assigned artifact staging path must be absolute."));
        OutFailure.Diagnostic.SubjectId = Expected.ArtifactId;
        OutFailure.Diagnostic.PropertyPath = !Assigned ? TEXT("artifacts") : !FPaths::IsSamePath(Assigned->FinalPath, Expected.FinalPath) ? TEXT("final_path") : TEXT("staging_path");
        CleanupAssigned();
        return false;
      }
    }
    if (!CleanupAssigned()) return false;

    const auto StagePackage = [&](const FPackageDurablePlan& Plan,
        UeAgentAuthoringWorkspaceTransaction::FDurableArtifact* Header,
        UeAgentAuthoringWorkspaceTransaction::FDurableArtifact* Payload) -> bool {
      if (!Header || !Payload) {
        SetFailure(OutFailure, TEXT("durable_package_stage_inventory_mismatch"),
            !Header ? TEXT("The package header artifact is missing from the staging inventory.") : TEXT("The package payload artifact is missing from the staging inventory."));
        OutFailure.Diagnostic.SubjectId = Plan.PlanId;
        OutFailure.Diagnostic.PropertyPath = !Header ? TEXT("header") : TEXT("payload");
        CleanupAssigned();
        return false;
      }
      if (Plan.Intent == EPackageDurableIntent::Delete) {
        return true;
      }
      if (Plan.Intent == EPackageDurableIntent::Preserve) {
        for (auto* Artifact : {Header, Payload}) {
          if (!IFileManager::Get().FileExists(*Artifact->FinalPath)) continue;
          if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Artifact->StagingPath), true)
              || IFileManager::Get().Copy(*Artifact->StagingPath, *Artifact->FinalPath, true, true) != COPY_OK) {
            OutFailure.Diagnostic = UE::UAI::FileDiagnostics::MakeFileOperationFailure(
                TEXT("durable_package_preserve_failed"), TEXT("The unchanged package artifact could not be staged."),
                TEXT("persist"), Artifact->FinalPath);
            OutFailure.Diagnostic.SubjectId = Plan.PackageName;
            CleanupAssigned();
            return false;
          }
          Artifact->bStagedExists = true;
        }
        return true;
      }

      UPackage *Package = Plan.Package.Get();
      UObject *TopLevelAsset = Plan.TopLevelAsset.Get();
      // Native external-object deletion leaves an empty package (or releases
      // it during GC). Publish its absence; checkpoint staging still sees the
      // original live objects before the edit.
      if (!Plan.OwningPackageName.IsEmpty() &&
          (!Package || UPackage::IsEmptyPackage(Package))) {
        return true;
      }
      if (!Package || Package == GetTransientPackage()) {
        SetPackageArtifactFailure(OutFailure, Plan.PackageName, TEXT("package_unavailable_during_stage"),
                                  !Package ? TEXT("The package is missing before its save can be staged.") : TEXT("The transient package cannot be staged for saving."));
        CleanupAssigned();
        return false;
      }
      Package->FullyLoad();
      if (!TopLevelAsset || TopLevelAsset->GetPackage() != Package) {
        TopLevelAsset = Package->FindAssetInPackage();
      }
      if ((!TopLevelAsset || TopLevelAsset->GetPackage() != Package) && Plan.OwningPackageName.IsEmpty()) {
        SetFailure(OutFailure, TEXT("durable_package_top_level_asset_missing"), TEXT("The package has no top-level asset owned by that package."));
        OutFailure.Diagnostic.SubjectId = Plan.PackageName;
        CleanupAssigned();
        return false;
      }

      const FString HeaderExtension =
          FPaths::GetExtension(Header->FinalPath, true);
      const FString ScratchHeader = Header->StagingPath + HeaderExtension;
      const FString ScratchPayload =
          FPaths::ChangeExtension(ScratchHeader, TEXT("upayload"));
      const bool bHeaderRemoved = RemoveStagingFile(ScratchHeader);
      const bool bPayloadRemoved = RemoveStagingFile(ScratchPayload);
      if (!bHeaderRemoved || !bPayloadRemoved) { CleanupAssigned(); return false; }
      if (!IFileManager::Get().MakeDirectory(
              *FPaths::GetPath(Header->StagingPath), true)) {
        SetFailure(OutFailure, TEXT("durable_package_stage_root_unavailable"), TEXT("The package staging directory could not be created."));
        OutFailure.Diagnostic.SubjectId = Plan.PackageName;
        OutFailure.Diagnostic.InternalContext = Header->StagingPath;
        CleanupAssigned();
        return false;
      }

      FSavePackageArgs SaveArgs;
      SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
      SaveArgs.SaveFlags = SAVE_KeepDirty | SAVE_FromAutosave;
      UE::UAI::Contract::FDiagnostic Detail;
      if (!UE::UAI::AssetDurable::TrySaveLoadedPackage(
              *Package, TopLevelAsset, ScratchHeader, SaveArgs, Detail)) {
        OutFailure.Diagnostic = Detail;
        RemoveStagingFile(ScratchHeader);
        RemoveStagingFile(ScratchPayload);
        CleanupAssigned();
        return false;
      }
      if (!IFileManager::Get().Move(*Header->StagingPath, *ScratchHeader, true,
                                    true, false, true)) {
        OutFailure.Diagnostic = UE::UAI::FileDiagnostics::MakeFileOperationFailure(
            TEXT("durable_package_stage_move_failed"), TEXT("Moving the saved package header into staging failed."), TEXT("persist"), Header->StagingPath);
        OutFailure.Diagnostic.SubjectId = Plan.PackageName;
        RemoveStagingFile(ScratchHeader);
        RemoveStagingFile(ScratchPayload);
        CleanupAssigned();
        return false;
      }
      Header->bStagedExists = true;
      if (IFileManager::Get().FileExists(*ScratchPayload)) {
        if (!IFileManager::Get().Move(*Payload->StagingPath, *ScratchPayload,
                                      true, true, false, true)) {
          OutFailure.Diagnostic = UE::UAI::FileDiagnostics::MakeFileOperationFailure(
              TEXT("durable_package_stage_move_failed"), TEXT("Moving the saved package payload into staging failed."), TEXT("persist"), Payload->StagingPath);
          OutFailure.Diagnostic.SubjectId = Plan.PackageName;
          RemoveStagingFile(ScratchPayload);
          CleanupAssigned();
          return false;
        }
        Payload->bStagedExists = true;
      }
      return true;
    };
    for (const auto& Plan : Plans) {
      if (!StagePackage(Plan, FindAssigned(HeaderArtifactId(Plan)), FindAssigned(PayloadArtifactId(Plan)))) return false;
    }
    return true;
  }
};
} // namespace

namespace UeAgentAuthoringOperationApply {
const TCHAR *FPreparedOperationMutation::BackendId =
    TEXT("uai.operation_apply.v1");

TSharedRef<IDurablePackageStager> CreateDefaultDurablePackageStager() {
  return MakeShared<FDefaultDurablePackageStager>();
}

bool MakeCommandApplyRequestContext(const FString &RequestId,
                                    FApplyRequestContext &OutContext,
                                    FOperationFailure &OutFailure) {
  OutContext = FApplyRequestContext();
  OutFailure = FOperationFailure();
  if (RequestId.TrimStartAndEnd().IsEmpty()) {
    SetFailure(OutFailure, TEXT("durable_request_id_missing"), TEXT("The apply request has no request identifier."));
    return false;
  }
  OutContext.RequestId = RequestId;
  OutContext.JournalRoot = DurableJournalRoot();
  return true;
}

bool BuildExecutionOptions(
    const FApplyRequestContext &Context,
    UeAgentAuthoringWorkspaceTransaction::FExecutionOptions &OutOptions,
    FOperationFailure &OutFailure) {
  OutOptions = UeAgentAuthoringWorkspaceTransaction::FExecutionOptions();
  OutFailure = FOperationFailure();
  if (Context.RequestId.TrimStartAndEnd().IsEmpty()) {
    SetFailure(OutFailure, TEXT("durable_request_id_missing"), TEXT("The apply request has no request identifier."));
    return false;
  }
  const FString ExpectedRoot = DurableJournalRoot();
  if (Context.JournalRoot.IsEmpty() || FPaths::IsRelative(Context.JournalRoot) ||
      !FPaths::IsSamePath(Context.JournalRoot, ExpectedRoot)) {
    SetFailure(OutFailure, TEXT("durable_journal_root_invalid"),
        Context.JournalRoot.IsEmpty() ? TEXT("The durable journal root is missing.")
        : FPaths::IsRelative(Context.JournalRoot) ? TEXT("The durable journal root must be absolute.")
        : TEXT("The durable journal root differs from the configured root."));
    OutFailure.Diagnostic.PropertyPath = TEXT("journal_root");
    OutFailure.Diagnostic.InternalContext = Context.JournalRoot;
    return false;
  }
  OutOptions.RequestId = Context.RequestId;
  OutOptions.JournalRoot = ExpectedRoot;
  return true;
}

bool BuildApplyEntryExecutionOptions(
    const FString &RequestId,
    UeAgentAuthoringWorkspaceTransaction::FExecutionOptions &OutOptions,
    FOperationFailure &OutFailure) {
  const FString EffectiveRequestId =
      RequestId.TrimStartAndEnd().IsEmpty()
          ? TEXT("internal-apply-") +
                FGuid::NewGuid().ToString(EGuidFormats::Digits)
          : RequestId;
  FApplyRequestContext Context;
  return MakeCommandApplyRequestContext(EffectiveRequestId, Context,
                                        OutFailure) &&
         BuildExecutionOptions(Context, OutOptions, OutFailure);
}

bool BuildPackageDurablePlan(
    const FString &PlanId, const FString &PackageName, UObject *Asset,
    EPackageDurableIntent Intent, TArray<FPackageDurablePlan> &OutPlans,
    FOperationFailure &OutFailure, bool bLifecycleStageable, EPackageBeforePresence BeforePresence,
    const UClass* AssetClass) {
  OutFailure = FOperationFailure();
  if (!bLifecycleStageable) {
    SetFailure(OutFailure, TEXT("durable_package_lifecycle_not_stageable"), TEXT("The operation lifecycle does not support staging this package."));
    OutFailure.Diagnostic.SubjectId = PlanId;
    return false;
  }
  const FString NormalizedPlanId = PlanId.TrimStartAndEnd();
  const FString NormalizedPackageName = PackageName.TrimStartAndEnd();
  if (NormalizedPlanId.IsEmpty() ||
      !FPackageName::IsValidLongPackageName(NormalizedPackageName) ||
      FPackageName::GetPackageMountPoint(NormalizedPackageName).IsNone()) {
    SetPackageArtifactFailure(OutFailure, NormalizedPackageName, TEXT("package_plan_identity_invalid"),
                              NormalizedPlanId.IsEmpty() ? TEXT("The persistent save plan identifier is empty.")
                                  : !FPackageName::IsValidLongPackageName(NormalizedPackageName) ? TEXT("The persistent save plan package name is invalid.")
                                  : TEXT("The persistent save plan package has no registered mount point."));
    return false;
  }

  UPackage *Package = Asset ? Asset->GetOutermost() : nullptr;
  if (!Package || Package == GetTransientPackage() ||
      Package->GetName() != NormalizedPackageName) {
    Package = FindPackage(nullptr, *NormalizedPackageName);
  }
  if (!Package && Intent == EPackageDurableIntent::Save &&
      BeforePresence != EPackageBeforePresence::Absent) {
    Package = CreatePackage(*NormalizedPackageName);
  }
  if ((!Package && BeforePresence != EPackageBeforePresence::Absent) || Package == GetTransientPackage()) {
    SetPackageArtifactFailure(OutFailure, NormalizedPackageName, TEXT("package_unavailable_for_save_plan"),
                              !Package ? TEXT("The persistent save plan package could not be found or created.") : TEXT("The transient package cannot be used in a persistent save plan."));
    return false;
  }

  FPackageDurablePlan &Plan = OutPlans.AddDefaulted_GetRef();
  Plan.PlanId = NormalizedPlanId;
  Plan.PackageName = NormalizedPackageName;
  Plan.Package = Package;
  Plan.TopLevelAsset = Asset;
  Plan.Intent = Intent;
  Plan.BeforePresence = BeforePresence;
  // Every Blueprint kind owns generated classes that live consumers can retain.
  // Native package replacement preserves class/instance remapping and rebuilds
  // dependency caches; unloading and loading an asset does not provide that lifetime.
  // Create plans precede publication, so the prepared asset can supply its type
  // without being mistaken for the live top-level asset owned by this package.
  const UClass* NativeAssetClass = Asset ? Asset->GetClass() : AssetClass;
  if (Intent == EPackageDurableIntent::Save && NativeAssetClass &&
      NativeAssetClass->IsChildOf(UBlueprint::StaticClass())) {
    Plan.ReadbackPolicy = EPackageReadbackPolicy::ReplaceResidentPackage;
  }
  return true;
}

bool IPreparedOperationState::BuildDurablePackagePlans(
    TArray<FPackageDurablePlan> &OutPlans,
    FOperationFailure &OutFailure) const {
  OutPlans.Reset();
  SetFailure(OutFailure, TEXT("durable_package_plan_not_supported"), TEXT("This prepared operation does not implement durable package planning."));
  OutFailure.Diagnostic.SubjectId = GetTargetKey();
  return false;
}

FProviderRegistry &FProviderRegistry::Get() {
  static FProviderRegistry Registry;
  return Registry;
}

bool FProviderRegistry::RegisterProvider(
    const TSharedRef<IApplyProvider> &Provider, ERegistrationResult &OutResult,
    UE::UAI::Contract::FDiagnostic& OutError) {
  const FString ProviderId = NormalizeId(Provider->GetProviderId());
  if (ProviderId.IsEmpty()) {
    OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("operation_provider_id_missing"), TEXT("provider_id"),
        MakeShared<FJsonValueString>(TEXT("nonempty provider identifier")), MakeShared<FJsonValueString>(Provider->GetProviderId()),
        TEXT("contract"), TEXT("The provider identifier is empty after normalization."));
    return false;
  }

  FWriteScopeLock Lock(RegistryLock);
  if (const TSharedPtr<IApplyProvider> *Existing = Providers.Find(ProviderId)) {
    if (Existing->Get() != &Provider.Get()) {
      OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("conflicting_operation_provider"),
          TEXT("A different provider instance is already registered with this identifier."), TEXT("contract"));
      OutError.CandidateName = ProviderId;
      return false;
    }
    OutResult = ERegistrationResult::AlreadyRegistered;
    OutError.Reset();
    return true;
  }

  Providers.Add(ProviderId, Provider);
  OutResult = ERegistrationResult::Registered;
  OutError.Reset();
  return true;
}

bool FProviderRegistry::CopyProvider(
    const FString &ProviderId, TSharedPtr<IApplyProvider> &OutProvider) const {
  FReadScopeLock Lock(RegistryLock);
  if (const TSharedPtr<IApplyProvider> *Found =
          Providers.Find(NormalizeId(ProviderId))) {
    OutProvider = *Found;
    return OutProvider.IsValid();
  }
  OutProvider.Reset();
  return false;
}

TArray<FString> FProviderRegistry::ListProviderIds() const {
  FReadScopeLock Lock(RegistryLock);
  TArray<FString> Result;
  Providers.GenerateKeyArray(Result);
  Result.Sort();
  return Result;
}

int32 FProviderRegistry::GetProviderCount() const {
  FReadScopeLock Lock(RegistryLock);
  return Providers.Num();
}

FPreparedOperationMutation::FPreparedOperationMutation(
    FString InUnitKey, UeAgentApplyInputContract::FContract InContract,
    UeAgentApplyInputContract::FOperationInput InInput,
    TUniquePtr<IPreparedOperationState> InState)
    : UnitKey(MoveTemp(InUnitKey)), Contract(MoveTemp(InContract)),
      Input(MoveTemp(InInput)), State(MoveTemp(InState)) {}

FString FPreparedOperationMutation::GetBackendId() const { return BackendId; }

FString FPreparedOperationMutation::GetUnitKey() const { return UnitKey; }

FString FPreparedOperationMutation::GetTargetKey() const {
  return State.IsValid() ? State->GetTargetKey() : FString();
}

bool FPreparedOperationMutation::IsNoOp() const {
  return State.IsValid() && State->IsNoOp();
}

bool FPreparedOperationMutation::IsStateNoOp() const {
  return State.IsValid() && State->IsStateNoOp();
}

TSharedPtr<FJsonObject>
FPreparedOperationMutation::BuildResultMetadata() const {
  return State.IsValid() ? State->BuildResultMetadata() : nullptr;
}

FWorkspaceOperationPreflightProvider::FWorkspaceOperationPreflightProvider(
    const UeAgentApplyInputContract::FRegistry &InContractRegistry,
    const FProviderRegistry &InProviderRegistry)
    : ContractRegistry(InContractRegistry),
      ProviderRegistry(InProviderRegistry) {}

bool TryResolveWorkspaceOperationInput(
    const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit,
    const UeAgentApplyInputContract::FRegistry &ContractRegistry,
    UeAgentApplyInputContract::FContract &OutContract,
    UeAgentApplyInputContract::FOperationInput &OutInput,
    FOperationFailure &OutFailure) {
  OutContract = UeAgentApplyInputContract::FContract();
  OutInput = UeAgentApplyInputContract::FOperationInput();
  OutFailure = FOperationFailure();
  if (!ContractRegistry.FindContract(Unit.ApplyContractRef, OutContract)) {
    SetFailure(OutFailure, TEXT("operation_contract_not_registered"), TEXT("The workspace operation references an unregistered apply contract."));
    OutFailure.Diagnostic.CandidateName = Unit.ApplyContractRef;
    OutFailure.Diagnostic.Source = Unit.Provenance.ToDiagnosticSource();
    return false;
  }

  UeAgentApplyInputContract::FParseFailure ParseFailure;
  if (!UeAgentApplyInputContract::ParseOperationInput(
          OutContract, Unit, OutInput, ParseFailure)) {
    OutFailure.Diagnostic = ParseFailure.Diagnostic;
    return false;
  }
  return true;
}

bool FWorkspaceOperationPreflightProvider::PreflightUnit(
    const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit,
    TUniquePtr<UeAgentAuthoringWorkspaceTransaction::IPreparedMutation>
        &OutMutation,
    FOperationFailure &OutFailure) const {
  OutMutation.Reset();
  OutFailure = FOperationFailure();

  const FString DocumentBefore = StableDocumentKey(Unit.Document);
  UeAgentApplyInputContract::FContract Contract;
  UeAgentApplyInputContract::FOperationInput Input;
  if (!TryResolveWorkspaceOperationInput(
          Unit, ContractRegistry, Contract, Input, OutFailure)) {
    return false;
  }

  TSharedPtr<IApplyProvider> Provider;
  if (!ProviderRegistry.CopyProvider(Contract.ProviderId, Provider)) {
    SetFailure(OutFailure, TEXT("operation_provider_not_registered"), TEXT("The apply contract references an unregistered operation provider."));
    OutFailure.Diagnostic.CandidateName = Contract.ProviderId;
    OutFailure.Diagnostic.Source = Unit.Provenance.ToDiagnosticSource();
    return false;
  }

  FPrepareRequest Request;
  Request.Contract = &Contract;
  Request.Unit = &Unit;
  Request.Input = &Input;
  TUniquePtr<IPreparedOperationState> State;
  const bool bPrepared = Provider->PrepareOperation(Request, State, OutFailure);
  if (!bPrepared || !State.IsValid()) {
    if (OutFailure.Diagnostic.Code.IsEmpty()) {
      SetFailure(OutFailure, TEXT("operation_provider_preflight_failed"),
          !bPrepared ? TEXT("The operation provider rejected preflight without a diagnostic.") : TEXT("The operation provider reported successful preflight without a prepared state."));
      OutFailure.Diagnostic.CandidateName = Contract.ProviderId;
      OutFailure.Diagnostic.Source = Unit.Provenance.ToDiagnosticSource();
    }
    return false;
  }

  if (DocumentBefore != StableDocumentKey(Unit.Document)) {
    SetFailure(OutFailure, TEXT("operation_provider_mutated_input"), TEXT("The operation provider modified the input document during preflight."));
    OutFailure.Diagnostic.CandidateName = Contract.ProviderId;
    OutFailure.Diagnostic.SubjectId = Unit.CanonicalUnitKey;
    OutFailure.Diagnostic.Source = Unit.Provenance.ToDiagnosticSource();
    return false;
  }

  const UeAgentApplyInputContract::EOperation ResolvedOperation =
      State->GetResolvedOperation();
  if (!IsValidResolvedOperation(Input.RequestedOperation, ResolvedOperation) ||
      !Contract.FindOperation(ResolvedOperation)) {
    SetFailure(OutFailure, TEXT("operation_provider_resolution_invalid"),
        !IsValidResolvedOperation(Input.RequestedOperation, ResolvedOperation) ? TEXT("The provider resolved the requested operation to an incompatible operation.")
        : TEXT("The provider resolved an operation that its apply contract does not declare."));
    OutFailure.Diagnostic.CandidateName = Contract.ProviderId;
    OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(UeAgentApplyInputContract::OperationToString(Input.RequestedOperation));
    OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueString>(UeAgentApplyInputContract::OperationToString(ResolvedOperation));
    OutFailure.Diagnostic.PropertyPath = TEXT("operation");
    OutFailure.Diagnostic.Source = Unit.Provenance.ToDiagnosticSource();
    return false;
  }
  if (State->GetTargetKey().TrimStartAndEnd().IsEmpty()) {
    SetFailure(OutFailure, TEXT("operation_provider_target_missing"), TEXT("The prepared operation has no target identity."));
    OutFailure.Diagnostic.CandidateName = Contract.ProviderId;
    OutFailure.Diagnostic.SubjectId = Unit.CanonicalUnitKey;
    OutFailure.Diagnostic.Source = Unit.Provenance.ToDiagnosticSource();
    return false;
  }

  Input.ResolvedOperation = ResolvedOperation;
  OutMutation = MakeUnique<FPreparedOperationMutation>(
      Unit.CanonicalUnitKey, MoveTemp(Contract), MoveTemp(Input),
      MoveTemp(State));
  return true;
}

FOperationTransactionBackend::FOperationTransactionBackend(
    FBackendOptions InOptions)
    : Options(MoveTemp(InOptions)) {
  if (!Options.PackageStager.IsValid())
    Options.PackageStager = CreateDefaultDurablePackageStager();
}

FOperationTransactionBackend::~FOperationTransactionBackend() = default;

FPreparedOperationMutation *FOperationTransactionBackend::ResolveMutation(
    UeAgentAuthoringWorkspaceTransaction::IPreparedMutation &Mutation,
    FOperationFailure &OutFailure) const {
  if (Mutation.GetBackendId() != FPreparedOperationMutation::BackendId) {
    SetFailure(OutFailure, TEXT("operation_backend_mismatch"), TEXT("The prepared mutation belongs to a different transaction backend."));
    OutFailure.Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(FPreparedOperationMutation::BackendId);
    OutFailure.Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Mutation.GetBackendId());
    return nullptr;
  }
  return static_cast<FPreparedOperationMutation *>(&Mutation);
}

void FOperationTransactionBackend::CapturePackageDirtyStates(
    const IPreparedOperationState &State) {
  TArray<UObject *> Objects;
  State.CollectRollbackObjects(Objects);
  TArray<UPackage*> Packages;
  for (UObject *Object : Objects) {
    if (Object && Object->GetPackage()) Packages.AddUnique(Object->GetPackage());
  }
  for (int32 Index = 0; Index < Packages.Num(); ++Index) {
    UPackage* Package = Packages[Index];
    UE::UAI::EditorResources::CollectOwnedPackages(Package, Packages);
    const bool bAlreadyCaptured = PackageDirtySnapshots.ContainsByPredicate(
        [Package](const FPackageDirtySnapshot &Snapshot) {
          return Snapshot.Package.Get() == Package;
        });
    if (bAlreadyCaptured)
      continue;
    FPackageDirtySnapshot &Snapshot = PackageDirtySnapshots.AddDefaulted_GetRef();
    Snapshot.Package = Package;
    Snapshot.PackageName = Package->GetName();
    Snapshot.bWasDirty = Package->IsDirty();
  }
}

void FOperationTransactionBackend::RestorePackageDirtyStates() {
  for (const FPackageDirtySnapshot &Snapshot : PackageDirtySnapshots) {
    // Replacement may keep the retired package alive through a native consumer.
    // The current package name owns the restored state, not that old pointer.
    UPackage *Package = FindPackage(nullptr, *Snapshot.PackageName);
    if (!Package && Snapshot.Package.IsValid() && Snapshot.Package->GetName() == Snapshot.PackageName)
      Package = Cast<UPackage>(Snapshot.Package.Get());
    if (Package)
      Package->SetDirtyFlag(Snapshot.bWasDirty);
  }
}

bool FOperationTransactionBackend::CaptureRequests(
    const TArray<UeAgentAuthoringState::FReadRequest> &Requests,
    UeAgentAuthoringState::FStateSnapshot &OutSnapshot,
    FOperationFailure &OutFailure) const {
  if (Requests.IsEmpty()) {
    SetFailure(OutFailure, TEXT("operation_verification_requests_missing"), TEXT("The operation supplied no state read requests for verification."));
    return false;
  }
  UeAgentAuthoringState::FCaptureResult CaptureResult;
  if (!UeAgentAuthoringState::CaptureSnapshot(Requests, CaptureResult,
                                              Options.SemanticRegistry,
                                              Options.StateAccessorRegistry)) {
    OutFailure.Diagnostic = CaptureResult.Diagnostic.IsValid() ? CaptureResult.Diagnostic : UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_snapshot_capture_failed"), TEXT("The operation snapshot could not be captured."), TEXT("capture"));
    return false;
  }
  OutSnapshot = MoveTemp(CaptureResult.Snapshot);
  return true;
}

bool FOperationTransactionBackend::BeginWorkspace(
    const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &Graph,
    FOperationFailure &OutFailure) {
  OutFailure = FOperationFailure();
  if (bWorkspaceOpen || bAwaitingDurableFinalization ||
      !Graph.IsReadyFor(Workspace)) {
    SetFailure(OutFailure, TEXT("operation_workspace_begin_invalid"),
        bWorkspaceOpen ? TEXT("An operation workspace is already open.")
        : bAwaitingDurableFinalization ? TEXT("The previous workspace is awaiting durable finalization.")
        : TEXT("The dependency graph is not ready for this workspace."));
    return false;
  }
  AppliedMutations.Reset();
  CapturedMutations.Reset();
  bNativeStateReleased = false;
  PackageDirtySnapshots.Reset();
  DurablePackagePlans.Reset();
  if (Options.bUseEditorTransaction) {
    const FText Description =
        Options.TransactionDescription.IsEmpty()
            ? NSLOCTEXT("UeAgentInterface", "StructuredApplyTransaction",
                        "Apply structured authoring workspace")
            : Options.TransactionDescription;
    EditorTransaction = MakeUnique<FScopedTransaction>(Description);
  }
  TemplatePropagation = MakeUnique<UeAgentTemplatePropertyPropagation::FScope>();
  bWorkspaceOpen = true;
  return true;
}

bool FOperationTransactionBackend::CaptureRollback(
    UeAgentAuthoringWorkspaceTransaction::IPreparedMutation &Mutation,
    FOperationFailure &OutFailure) {
  FPreparedOperationMutation *Prepared = ResolveMutation(Mutation, OutFailure);
  if (!bWorkspaceOpen || !Prepared || !Prepared->State.IsValid()) {
    if (OutFailure.Diagnostic.Code.IsEmpty())
      SetFailure(OutFailure, TEXT("operation_rollback_capture_invalid"),
          !bWorkspaceOpen ? TEXT("The operation workspace is not open.")
          : !Prepared ? TEXT("The prepared mutation could not be resolved.")
          : TEXT("The prepared mutation has no operation state."));
    return false;
  }
  CapturePackageDirtyStates(*Prepared->State);
  if (RequiresPersistentReadback() || Prepared->State->RequiresCheckpointRollback()) {
    if (!Prepared->State->SupportsPersistentReadback()) {
      SetFailure(OutFailure, TEXT("operation_persistent_readback_not_supported"), TEXT("The prepared operation does not support the persistent readback required for rollback."));
      OutFailure.Diagnostic.SubjectId = Prepared->GetTargetKey();
      return false;
    }
    TArray<UeAgentAuthoringState::FReadRequest> Requests;
    if (!Prepared->State->BuildBeforeReadRequests(Requests, OutFailure) ||
        !CaptureRequests(Requests, Prepared->BeforeSnapshot, OutFailure))
      return false;
    Prepared->BeforeSnapshot.ReleaseNativeValues();
    Prepared->bBeforeCaptured = true;
    CapturedMutations.Add(Prepared);
  }
  return Prepared->State->CaptureRollback(OutFailure);
}

bool FOperationTransactionBackend::RequiresPersistentReadback() const {
  return !!Options.FinalReadback;
}

bool FOperationTransactionBackend::HasReleasedNativeState() const {
  return bNativeStateReleased;
}

bool FOperationTransactionBackend::RequiresCheckpointRollback() const {
  for (const auto *Prepared : CapturedMutations)
    if (Prepared->State->RequiresCheckpointRollback()) return true;
  return false;
}

bool FOperationTransactionBackend::RequiresMemoryCheckpoint() const {
  return RequiresPersistentReadback() || RequiresCheckpointRollback();
}

bool FOperationTransactionBackend::UnloadForReadback(FOperationFailure &OutFailure, bool bRestoringBefore) {
  // Cancel recording before unload; the workspace remains logically open.
  if (EditorTransaction.IsValid()) {
    EditorTransaction->Cancel();
    EditorTransaction.Reset();
  }
  bNativeStateReleased = true;
  for (auto *Prepared : CapturedMutations) {
    Prepared->ExpectedSnapshot.ReleaseNativeValues();
    if (!(bRestoringBefore ? Prepared->State->PrepareCheckpointRestore(OutFailure)
                          : Prepared->State->ReleaseForPersistentReadback(OutFailure)))
      return false;
  }
  TArray<UPackage *> Packages;
  for (const auto &Plan : DurablePackagePlans) {
    if (!bRestoringBefore && Plan.Intent == EPackageDurableIntent::Preserve) continue;
    if (bRestoringBefore && Plan.BeforePresence == EPackageBeforePresence::Absent) continue;
    if (UPackage *Package = FindPackage(nullptr, *Plan.PackageName)) {
      ResetLoaders(Package);
      // The checkpoint/disk artifacts own both versions until final commit.
      Package->SetDirtyFlag(false);
      if (Plan.ReadbackPolicy == EPackageReadbackPolicy::UnloadAndLoad)
        Packages.AddUnique(Package);
    }
  }
  if (!Packages.IsEmpty()) {
    FText Error;
    if (!UPackageTools::UnloadPackages(Packages, Error, true)) {
      SetFailure(OutFailure, TEXT("operation_persistent_unload_failed"), TEXT("Native unload could not release the packages required for persistent readback."));
      OutFailure.Diagnostic.InternalContext = Error.ToString();
      OutFailure.Diagnostic.ResourceDimension = TEXT("packages");
      OutFailure.Diagnostic.ResourceObserved = Packages.Num();
      TArray<TSharedPtr<FJsonValue>> PackageNames;
      for (const auto* Package : Packages) PackageNames.Add(MakeShared<FJsonValueString>(Package->GetName()));
      OutFailure.Diagnostic.ActualEvidence = MakeShared<FJsonValueArray>(MoveTemp(PackageNames));
      return false;
    }
  }
  return true;
}

bool FOperationTransactionBackend::ReloadForReadback(FOperationFailure &OutFailure, bool bRestoringBefore) {
  // File publication (or checkpoint restoration) precedes this phase. Native
  // keep-alive references require package replacement, not a second disk owner.
  TArray<UPackage *> ResidentPackages;
  TArray<FString> ResidentMaps;
  for (const auto &Plan : DurablePackagePlans) {
    if (!bRestoringBefore && Plan.Intent == EPackageDurableIntent::Preserve) continue;
    if (bRestoringBefore && Plan.BeforePresence == EPackageBeforePresence::Absent) continue;
    if (UPackage *Package = FindPackage(nullptr, *Plan.PackageName)) {
      if (!bRestoringBefore && Plan.BeforePresence == EPackageBeforePresence::Absent
          && Plan.Intent == EPackageDurableIntent::Save) {
        FString HeaderPath, PayloadPath;
        if (!ResolvePackageArtifactPaths(Plan, HeaderPath, PayloadPath, OutFailure))
          return false;
        if (!IFileManager::Get().FileExists(*HeaderPath)) {
          SetFailure(OutFailure, TEXT("operation_created_package_not_published"), TEXT("The newly created package was not published before persistent readback."));
          OutFailure.Diagnostic.SubjectId = Plan.PackageName;
          return false;
        }
        // Autosave staging preserves this flag. Publication now supplies the
        // durable package that the normal save lifecycle would acknowledge.
        Package->ClearPackageFlags(PKG_NewlyCreated);
      }
      if (Package->HasAnyPackageFlags(PKG_ContainsMap)) {
        ResidentMaps.AddUnique(Plan.PackageName);
        continue;
      }
      // UnloadPackages can succeed while another native consumer retains the
      // package. LoadAsset would then return the previous in-memory object,
      // including during checkpoint rollback. Reuse native replacement for
      // every surviving ordinary package, not only generated-class owners.
      ResidentPackages.AddUnique(Package);
    }
  }
  if (!ResidentPackages.IsEmpty()) {
    UE::UAI::Contract::FDiagnostic Detail;
    if (!TryReplaceResidentPackages(ResidentPackages, Detail)) {
      OutFailure.Diagnostic = MoveTemp(Detail);
      return false;
    }
  }
  for (const FString& Name : ResidentMaps) {
    UE::UAI::Contract::FDiagnostic Detail;
    if (!TryReloadPersistedPackage(Name, Detail)) {
      OutFailure.Diagnostic = MoveTemp(Detail);
      return false;
    }
  }
  for (auto *Prepared : CapturedMutations) {
    if (!(bRestoringBefore ? Prepared->State->RebindAfterCheckpointRestore(OutFailure)
                          : Prepared->State->RebindAfterPersistentReadback(OutFailure)))
      return false;
  }
  for (auto &Plan : DurablePackagePlans) {
    UPackage *Package = FindPackage(nullptr, *Plan.PackageName);
    Plan.Package = Package;
    Plan.TopLevelAsset = Package ? FindObject<UObject>(Package,
        *FPackageName::GetLongPackageAssetName(Plan.PackageName)) : nullptr;
  }
  return true;
}

bool FOperationTransactionBackend::FinalizeVerification(FOperationFailure &OutFailure) {
  if (!RequiresPersistentReadback())
    return true;
  if (CapturedMutations.IsEmpty())
    return Options.FinalReadback(OutFailure);
  if (Options.ObservePublishedReadback && !PublishedReadbackPackages.IsEmpty()) {
    TArray<FOperationReadback> Readbacks;
    if (!BuildReadbackRequests(Readbacks, OutFailure)) return false;
    if (Readbacks.Num() == AppliedMutations.Num()) {
      bool bVerified = false;
      const TOptional<bool> Observed = Options.ObservePublishedReadback(
          PublishedReadbackPackages, Readbacks,
          [this, &Readbacks, &bVerified](FOperationFailure& Failure) {
            bVerified = true;
            return VerifyReadbackRequests(Readbacks, Failure);
          }, OutFailure);
      if (Observed.IsSet()) {
        if (Observed.GetValue() && !bVerified) {
          SetFailure(OutFailure, TEXT("operation_persisted_observation_incomplete"),
              TEXT("The persisted observer returned without executing the operation readback."));
          return false;
        }
        if (!Observed.GetValue()) return false;
        for (auto* Prepared : CapturedMutations)
          Prepared->ExpectedSnapshot.ReleaseNativeValues();
        // The published artifacts/checkpoint now own both durable versions,
        // exactly as in UnloadForReadback. FinalReadback may restore another
        // Editor world before durable commit; the just-saved package must not
        // be mistaken for unsaved user work during that handoff.
        if (EditorTransaction.IsValid()) {
          EditorTransaction->Cancel();
          EditorTransaction.Reset();
        }
        for (const auto& Published : PublishedReadbackPackages) {
          if (Published.Intent != EPackageDurableIntent::Save) continue;
          for (const auto& Plan : DurablePackagePlans) {
            if (Plan.Intent == EPackageDurableIntent::Save && Plan.PackageName == Published.PackageName) {
              if (UPackage* Package = Plan.Package.Get()) Package->SetDirtyFlag(false);
              break;
            }
          }
        }
        return Options.FinalReadback(OutFailure);
      }
    }
  }
  if (!UnloadForReadback(OutFailure) || !ReloadForReadback(OutFailure) ||
      !Verify(OutFailure))
    return false;
  return Options.FinalReadback(OutFailure);
}

bool FOperationTransactionBackend::PrepareCheckpointRestore(FOperationFailure &OutFailure) {
  return UnloadForReadback(OutFailure, true);
}

bool FOperationTransactionBackend::ReloadCheckpoint(FOperationFailure &OutFailure) {
  const bool bReloaded = ReloadForReadback(OutFailure, true);
  ReleasePackageLoaders(DurablePackagePlans);
  return bReloaded;
}

bool FOperationTransactionBackend::VerifyRestoredState(FOperationFailure &OutFailure) {
  for (auto *Prepared : CapturedMutations) {
    TArray<UeAgentAuthoringState::FReadRequest> Requests;
    UeAgentAuthoringState::FStateSnapshot Actual;
    if (!Prepared->bBeforeCaptured ||
        !Prepared->State->BuildBeforeReadRequests(Requests, OutFailure) ||
        !CaptureRequests(Requests, Actual, OutFailure))
      return false;
    UeAgentAuthoringSemanticCompare::FComparisonReport Report;
    if (!UeAgentAuthoringSemanticCompare::CompareSnapshots(
            Prepared->BeforeSnapshot, Actual,
            UeAgentAuthoringSemanticCompare::FCompareOptions(), Report,
            Options.SemanticRegistry, Options.ComparatorRegistry) || !Report.bEquivalent) {
      SetSemanticComparisonFailure(OutFailure, Requests, Report, true);
      return false;
    }
  }
  RestorePackageDirtyStates();
  return true;
}

bool FOperationTransactionBackend::ApplyUnit(
    UeAgentAuthoringWorkspaceTransaction::IPreparedMutation &Mutation,
    FOperationFailure &OutFailure) {
  FPreparedOperationMutation *Prepared = ResolveMutation(Mutation, OutFailure);
  if (!bWorkspaceOpen || !Prepared || !Prepared->State.IsValid()) {
    if (OutFailure.Diagnostic.Code.IsEmpty())
      SetFailure(OutFailure, TEXT("operation_apply_invalid"),
          !bWorkspaceOpen ? TEXT("The operation workspace is not open.")
          : !Prepared ? TEXT("The prepared mutation could not be resolved.")
          : TEXT("The prepared mutation has no operation state."));
    return false;
  }

  if (!Prepared->State->ApplyProposed(OutFailure))
    return false;

  FUeAgentInterfaceLogger::Log(
      TEXT("StructuredApply phase=expected_snapshot_begin target=%s"),
      *Prepared->State->GetTargetKey());
  TArray<UeAgentAuthoringState::FReadRequest> ExpectedRequests;
  // This is a native persistence baseline, not the original authored intent.
  // Providers verify their retained decoded intent before supplying these
  // requests; actual readback must not replace that independent expectation.
  if (!Prepared->State->BuildExpectedReadRequests(ExpectedRequests,
                                                  OutFailure) ||
      !CaptureRequests(ExpectedRequests, Prepared->ExpectedSnapshot,
                       OutFailure)) {
    UeAgentAuthoringDiagnostics::ReportConsistencyWarning(OutFailure.Diagnostic.Code,
        OutFailure.Diagnostic, TEXT("expected_state_observation"));
    OutFailure = FOperationFailure();
  } else {
    Prepared->bExpectedCaptured = true;
  }

  UE::UAI::Contract::FDiagnostic NormalizationError;
  if (!UeAgentNativeNormalizationIntent::FinalizeValues(Prepared->ExpectedSnapshot, NormalizationError)) {
    UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("native_normalization_readback_failed"),
        NormalizationError, TEXT("expected_state_observation"));
  }
  FUeAgentInterfaceLogger::Log(
      TEXT("StructuredApply phase=expected_snapshot_end target=%s requests=%d"),
      *Prepared->State->GetTargetKey(), ExpectedRequests.Num());
  if (!Prepared->State->ApplyLifecycle(OutFailure))
    return false;
  Prepared->bLifecycleApplied = true;
  AppliedMutations.Add(Prepared);
  return true;
}

bool FOperationTransactionBackend::Compile(FOperationFailure &OutFailure) {
  // Persistent operations already discard this recording before native unload.
  // End it after authored mutations, before native settle: PCG deliberately
  // pauses while an Editor transaction is active. Workspace checkpoints, not
  // the discarded Undo record, remain responsible for atomic failure recovery.
  if (RequiresPersistentReadback() && EditorTransaction.IsValid()) {
    EditorTransaction->Cancel();
    EditorTransaction.Reset();
  }
  for (FPreparedOperationMutation *Prepared : AppliedMutations) {
    if (!Prepared || !Prepared->State.IsValid()) {
      SetFailure(OutFailure, TEXT("operation_compile_state_missing"),
          !Prepared ? TEXT("The applied mutation is null before compilation.") : TEXT("The applied mutation has no operation state to compile."));
      return false;
    }
    if (!Prepared->State->Compile(OutFailure)) return false;
  }
  if (!AppliedMutations.IsEmpty() && Options.FinalizeNativeUpdate &&
      !Options.FinalizeNativeUpdate(OutFailure)) return false;
  return true;
}

bool FOperationTransactionBackend::Save(FOperationFailure &OutFailure) {
  for (FPreparedOperationMutation *Prepared : AppliedMutations) {
    if (!Prepared || !Prepared->State.IsValid()) {
      SetFailure(OutFailure, TEXT("operation_save_state_missing"),
          !Prepared ? TEXT("The applied mutation is null before saving.") : TEXT("The applied mutation has no operation state to save."));
      return false;
    }
    if (!Prepared->State->Save(OutFailure)) return false;
  }
  return true;
}

bool FOperationTransactionBackend::SupportsDurableTransactions() const {
  return Options.PackageStager.IsValid();
}

bool FOperationTransactionBackend::BuildDurableArtifactInventory(
    const TArray<UeAgentAuthoringWorkspaceTransaction::IPreparedMutation *>
        &Mutations,
    TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>
        &OutArtifacts,
    FOperationFailure &OutFailure) {
  OutArtifacts.Reset();
  OutFailure = FOperationFailure();
  DurablePackagePlans.Reset();
  if (!bWorkspaceOpen || !Options.PackageStager.IsValid()) {
    SetFailure(OutFailure, TEXT("durable_package_stager_unavailable"),
        !bWorkspaceOpen ? TEXT("The operation workspace is not open.") : TEXT("No durable package stager is available."));
    return false;
  }

  for (int32 MutationIndex = 0; MutationIndex < Mutations.Num(); ++MutationIndex) {
    auto* Mutation = Mutations[MutationIndex];
    if (!Mutation) {
      SetFailure(OutFailure, TEXT("durable_package_mutation_invalid"), TEXT("The durable inventory contains a null prepared mutation."));
      OutFailure.Diagnostic.PropertyPath = TEXT("mutations/") + FString::FromInt(MutationIndex);
      OutFailure.Diagnostic.ActualValuePresent = false;
      DurablePackagePlans.Reset();
      return false;
    }
    FPreparedOperationMutation *Prepared =
        ResolveMutation(*Mutation, OutFailure);
    if (!Prepared || !Prepared->State.IsValid()) {
      if (OutFailure.Diagnostic.IsEmpty()) SetFailure(OutFailure, TEXT("durable_package_mutation_invalid"), TEXT("The prepared mutation has no operation state for durable planning."));
      DurablePackagePlans.Reset();
      return false;
    }
    TArray<FPackageDurablePlan> MutationPlans;
    if (!Prepared->State->BuildDurablePackagePlans(MutationPlans,
                                                   OutFailure)) {
      DurablePackagePlans.Reset();
      return false;
    }
    if (MutationPlans.IsEmpty()) {
      SetFailure(OutFailure, TEXT("durable_package_plan_missing"), TEXT("The prepared operation supplied no durable package plans."));
      OutFailure.Diagnostic.SubjectId = Prepared->State->GetTargetKey();
      DurablePackagePlans.Reset();
      return false;
    }
    for (const auto &Plan : MutationPlans) {
      if (Plan.ReadbackPolicy == EPackageReadbackPolicy::ReplaceResidentPackage
          && (Plan.Intent != EPackageDurableIntent::Save
              || (Plan.Package.IsValid() && Plan.Package->HasAnyPackageFlags(PKG_ContainsMap)))) {
        SetFailure(OutFailure, TEXT("durable_package_readback_policy_invalid"),
            Plan.Intent != EPackageDurableIntent::Save ? TEXT("Resident replacement requires a save plan.") : TEXT("Resident replacement cannot be used for a map package."));
        OutFailure.Diagnostic.SubjectId = Plan.PackageName;
        OutFailure.Diagnostic.PropertyPath = Plan.Intent != EPackageDurableIntent::Save ? TEXT("intent") : TEXT("readback_policy");
        OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Plan.Intent != EPackageDurableIntent::Save ? TEXT("save") : TEXT("unload_and_load"));
        OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueString>(Plan.Intent != EPackageDurableIntent::Save ? LexToString(Plan.Intent) : TEXT("replace_resident_package"));
        DurablePackagePlans.Reset();
        return false;
      }
      bool bAlreadyDeclared = false;
      for (const auto &Existing : DurablePackagePlans) {
        if (!Existing.PackageName.Equals(Plan.PackageName, ESearchCase::IgnoreCase))
          continue;
        if (Existing.ReadbackPolicy != Plan.ReadbackPolicy || Existing.BeforePresence != Plan.BeforePresence) {
          SetFailure(OutFailure, TEXT("durable_package_readback_policy_conflict"),
              Existing.ReadbackPolicy != Plan.ReadbackPolicy ? TEXT("Plans for the same package declare different readback policies.") : TEXT("Plans for the same package disagree about its presence before the operation."));
          OutFailure.Diagnostic.SubjectId = Plan.PackageName;
          OutFailure.Diagnostic.PropertyPath = Existing.ReadbackPolicy != Plan.ReadbackPolicy ? TEXT("readback_policy") : TEXT("before_presence");
          OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Existing.ReadbackPolicy != Plan.ReadbackPolicy
              ? (Existing.ReadbackPolicy == EPackageReadbackPolicy::ReplaceResidentPackage ? TEXT("replace_resident_package") : TEXT("unload_and_load"))
              : (Existing.BeforePresence == EPackageBeforePresence::Absent ? TEXT("absent") : TEXT("present")));
          OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueString>(Existing.ReadbackPolicy != Plan.ReadbackPolicy
              ? (Plan.ReadbackPolicy == EPackageReadbackPolicy::ReplaceResidentPackage ? TEXT("replace_resident_package") : TEXT("unload_and_load"))
              : (Plan.BeforePresence == EPackageBeforePresence::Absent ? TEXT("absent") : TEXT("present")));
          DurablePackagePlans.Reset();
          return false;
        }
        if (!Existing.PlanId.Equals(Plan.PlanId, ESearchCase::CaseSensitive)
            || Existing.Package != Plan.Package
            || Existing.TopLevelAsset != Plan.TopLevelAsset
            || Existing.Intent != Plan.Intent) {
          SetFailure(OutFailure, TEXT("durable_package_plan_conflict"),
              !Existing.PlanId.Equals(Plan.PlanId, ESearchCase::CaseSensitive) ? TEXT("Plans for the same package have different plan identities.")
              : Existing.Package != Plan.Package ? TEXT("Plans for the same package refer to different native package instances.")
              : Existing.TopLevelAsset != Plan.TopLevelAsset ? TEXT("Plans for the same package refer to different top-level asset instances.")
              : TEXT("Plans for the same package request different publication intents."));
          OutFailure.Diagnostic.SubjectId = Plan.PackageName;
          OutFailure.Diagnostic.PropertyPath = !Existing.PlanId.Equals(Plan.PlanId, ESearchCase::CaseSensitive) ? TEXT("plan_id")
              : Existing.Package != Plan.Package ? TEXT("package") : Existing.TopLevelAsset != Plan.TopLevelAsset ? TEXT("top_level_asset") : TEXT("intent");
          if (OutFailure.Diagnostic.PropertyPath == TEXT("intent")) {
            OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(LexToString(Existing.Intent));
            OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueString>(LexToString(Plan.Intent));
          }
          DurablePackagePlans.Reset();
          return false;
        }
        bAlreadyDeclared = true;
        break;
      }
      if (!bAlreadyDeclared)
        DurablePackagePlans.Add(Plan);
    }
  }
  if (DurablePackagePlans.IsEmpty()) {
    SetFailure(OutFailure, TEXT("durable_package_plan_missing"), TEXT("No durable package plans were supplied for the workspace."));
    return false;
  }
  // Checkpoint the native ownership closure before any mutation. The same
  // collector serves ordinary saves; referencers and unrelated dirty packages
  // are deliberately outside this scope.
  for (int32 Index = 0; Index < DurablePackagePlans.Num(); ++Index) {
    const auto Parent = DurablePackagePlans[Index];
    TArray<UPackage*> Owned;
    UE::UAI::EditorResources::CollectOwnedPackages(Parent.Package.Get(), Owned);
    for (UPackage* Package : Owned) {
      if (DurablePackagePlans.ContainsByPredicate([&](const auto& Plan) { return Plan.PackageName == Package->GetName(); })) continue;
      FPackageDurablePlan Plan;
      Plan.PlanId = Package->GetName();
      Plan.PackageName = Package->GetName();
      Plan.Package = Package;
      Plan.TopLevelAsset = Package->FindAssetInPackage();
      Plan.OwningPackageName = Parent.PackageName;
      Plan.Intent = Parent.Intent;
      DurablePackagePlans.Add(MoveTemp(Plan));
    }
  }
  if (!Options.PackageStager->BuildArtifactInventory(
          DurablePackagePlans, OutArtifacts, OutFailure)) {
    DurablePackagePlans.Reset();
    OutArtifacts.Reset();
    return false;
  }
  return true;
}

bool FOperationTransactionBackend::StageDurableArtifacts(
    TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>
        &InOutArtifacts,
    FOperationFailure &OutFailure) {
  OutFailure = FOperationFailure();
  if (!bWorkspaceOpen || !Options.PackageStager.IsValid() ||
      DurablePackagePlans.IsEmpty()) {
    SetFailure(OutFailure, TEXT("durable_package_stage_invalid"),
        !bWorkspaceOpen ? TEXT("The operation workspace is not open.")
        : !Options.PackageStager.IsValid() ? TEXT("No durable package stager is available.")
        : TEXT("No durable package plans are available for staging."));
    return false;
  }
  for (auto &Plan : DurablePackagePlans) {
    if (Plan.BeforePresence != EPackageBeforePresence::Absent) continue;
    UPackage* Package = FindPackage(nullptr, *Plan.PackageName);
    Plan.Package = Package;
    Plan.TopLevelAsset = Package ? FindObject<UObject>(Package,
        *FPackageName::GetLongPackageAssetName(Plan.PackageName)) : nullptr;
  }
  if (!Options.PackageStager->StagePackages(
      DurablePackagePlans, InOutArtifacts, OutFailure)) return false;
  PublishedReadbackPackages.Reset();
  for (const FPackageDurablePlan& Plan : DurablePackagePlans) {
    const auto* Header = InOutArtifacts.FindByPredicate([&Plan](const auto& Artifact) {
      return Artifact.ArtifactId == HeaderArtifactId(Plan);
    });
    if (!Header) { PublishedReadbackPackages.Reset(); break; }
    PublishedReadbackPackages.Add({Plan.PackageName, Header->FinalPath, Plan.Intent});
  }
  return true;
}

bool FOperationTransactionBackend::CollectAdditionalDurableArtifacts(
    TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& OutArtifacts,
    FOperationFailure& OutFailure) {
  OutArtifacts.Reset();
  TArray<FPackageDurablePlan> Added;
  for (int32 Index = 0; Index < DurablePackagePlans.Num(); ++Index) {
    const auto Parent = DurablePackagePlans[Index];
    TArray<UPackage*> Owned;
    UE::UAI::EditorResources::CollectOwnedPackages(Parent.Package.Get(), Owned);
    for (UPackage* Package : Owned) {
      if (DurablePackagePlans.ContainsByPredicate([&](const auto& Plan) { return Plan.PackageName == Package->GetName(); })) continue;
      FPackageDurablePlan Plan;
      Plan.PlanId = Package->GetName(); Plan.PackageName = Package->GetName(); Plan.Package = Package;
      Plan.TopLevelAsset = Package->FindAssetInPackage(); Plan.OwningPackageName = Parent.PackageName;
      Plan.Intent = Parent.Intent;
      // Existing loaded owned state was captured during preparation. A package
      // first allocated/loaded by this operation has only its disk before-image.
      Plan.BeforePresence = FPackageName::DoesPackageExist(Plan.PackageName)
          ? EPackageBeforePresence::Present : EPackageBeforePresence::Absent;
      Added.Add(Plan); DurablePackagePlans.Add(MoveTemp(Plan));
    }
  }
  return Added.IsEmpty() || Options.PackageStager->BuildArtifactInventory(Added, OutArtifacts, OutFailure);
}

bool FOperationTransactionBackend::StageMemoryCheckpoint(
    TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact> &InOutArtifacts,
    FOperationFailure &OutFailure) {
  if (!bWorkspaceOpen || !Options.PackageStager.IsValid() || DurablePackagePlans.IsEmpty()) {
    SetFailure(OutFailure, TEXT("durable_package_stage_invalid"),
        !bWorkspaceOpen ? TEXT("The operation workspace is not open.")
        : !Options.PackageStager.IsValid() ? TEXT("No durable package stager is available.")
        : TEXT("No durable package plans are available for staging."));
    return false;
  }
  TArray<FPackageDurablePlan> BeforePlans = DurablePackagePlans;
  for (auto &Plan : BeforePlans) {
    // A requested deletion still has a live before-image to restore.
    Plan.Intent = Plan.BeforePresence == EPackageBeforePresence::Absent
        ? EPackageDurableIntent::Delete : EPackageDurableIntent::Save;
  }
  return Options.PackageStager->StagePackages(BeforePlans, InOutArtifacts, OutFailure);
}

bool FOperationTransactionBackend::Verify(FOperationFailure &OutFailure) {
  TArray<FOperationReadback> Readbacks;
  return BuildReadbackRequests(Readbacks, OutFailure)
      && VerifyReadbackRequests(Readbacks, OutFailure);
}

bool FOperationTransactionBackend::BuildReadbackRequests(
    TArray<FOperationReadback>& OutReadbacks, FOperationFailure& OutFailure) {
  OutReadbacks.Reset();
  const auto Warn = [&]() {
    UeAgentAuthoringDiagnostics::ReportConsistencyWarning(OutFailure.Diagnostic.Code,
        OutFailure.Diagnostic, TEXT("operation_state_observation"));
    OutFailure = FOperationFailure();
  };
  for (FPreparedOperationMutation *Prepared : AppliedMutations) {
    if (!Prepared || !Prepared->State.IsValid() ||
        !Prepared->bExpectedCaptured) {
      SetFailure(OutFailure, TEXT("operation_expected_snapshot_missing"),
          !Prepared ? TEXT("The applied mutation is null.")
          : !Prepared->State.IsValid() ? TEXT("The applied mutation has no operation state.")
          : TEXT("The expected operation snapshot was not captured."));
      Warn();
      continue;
    }

    TArray<UeAgentAuthoringState::FReadRequest> ActualRequests;
    if (!Prepared->State->BuildActualReadRequests(ActualRequests, OutFailure))
      { Warn(); continue; }
    OutReadbacks.Add({Prepared->State->GetResolvedOperation(),
        MoveTemp(ActualRequests), &Prepared->ExpectedSnapshot});
  }
  return true;
}

bool FOperationTransactionBackend::VerifyReadbackRequests(
    const TArray<FOperationReadback>& Readbacks, FOperationFailure& OutFailure) {
  const auto Warn = [&]() {
    UeAgentAuthoringDiagnostics::ReportConsistencyWarning(OutFailure.Diagnostic.Code,
        OutFailure.Diagnostic, TEXT("operation_state_observation"));
    OutFailure = FOperationFailure();
  };
  for (const FOperationReadback& Readback : Readbacks) {
    UeAgentAuthoringState::FStateSnapshot ActualSnapshot;
    if (!CaptureRequests(Readback.Requests, ActualSnapshot, OutFailure))
      { Warn(); continue; }

    UeAgentAuthoringSemanticCompare::FComparisonReport Report;
    if (!UeAgentAuthoringSemanticCompare::CompareSnapshots(
            *Readback.Expected, ActualSnapshot,
            UeAgentAuthoringSemanticCompare::FCompareOptions(), Report,
            Options.SemanticRegistry, Options.ComparatorRegistry) ||
        !Report.bEquivalent) {
      SetSemanticComparisonFailure(OutFailure, Readback.Requests, Report, false);
      Warn();
    }
  }
  return true;
}

bool FOperationTransactionBackend::RollbackUnit(
    UeAgentAuthoringWorkspaceTransaction::IPreparedMutation &Mutation,
    FOperationFailure &OutFailure) {
  FPreparedOperationMutation *Prepared = ResolveMutation(Mutation, OutFailure);
  if (!Prepared || !Prepared->State.IsValid()) {
    if (OutFailure.Diagnostic.IsEmpty()) SetFailure(OutFailure, TEXT("operation_rollback_state_missing"), TEXT("The prepared mutation has no operation state to roll back."));
    return false;
  }
  const bool bResult = Prepared->State->Rollback(OutFailure);
  AppliedMutations.RemoveSingleSwap(Prepared, EAllowShrinking::No);
  Prepared->bExpectedCaptured = false;
  Prepared->bLifecycleApplied = false;
  return bResult;
}

bool FOperationTransactionBackend::EndWorkspace(bool bCommit,
                                                FOperationFailure &OutFailure) {
  OutFailure = FOperationFailure();
  if (!bWorkspaceOpen) {
    SetFailure(OutFailure, TEXT("operation_workspace_not_open"), TEXT("The operation workspace is not open."));
    return false;
  }
  if (bCommit) {
    for (FPreparedOperationMutation *Prepared : AppliedMutations) {
      if (!Prepared || !Prepared->State.IsValid()) {
        SetFailure(OutFailure, TEXT("operation_commit_state_missing"),
            !Prepared ? TEXT("The applied mutation is null at commit.") : TEXT("The applied mutation has no operation state to commit."));
        return false;
      }
      if (!Prepared->State->Commit(OutFailure)) return false;
    }
  }
  if (!bCommit && EditorTransaction.IsValid())
    EditorTransaction->Cancel();
  if (!bCommit && TemplatePropagation) {
    UE::UAI::Contract::FDiagnostic Error;
    if (!TemplatePropagation->Restore(Error)) {
      OutFailure.Diagnostic = UeAgentAuthoringDiagnostics::FDiagnostic::WrapFailure(
          TEXT("template_instance_rollback_failed"), Error, TEXT("rollback"), TEXT("Restoring propagated template properties failed."));
      return false;
    }
  }
  if (!bCommit)
    RestorePackageDirtyStates();
  const bool bNeedsDurableFinalization =
      bCommit && !DurablePackagePlans.IsEmpty();
  EditorTransaction.Reset();
  AppliedMutations.Reset();
  if (!bNeedsDurableFinalization) {
    TemplatePropagation.Reset();
    PackageDirtySnapshots.Reset();
    DurablePackagePlans.Reset();
  }
  bAwaitingDurableFinalization = bNeedsDurableFinalization;
  bWorkspaceOpen = false;
  return true;
}

bool FOperationTransactionBackend::FinalizeDurableCommit(bool bCommitted, FOperationFailure& OutFailure) {
  OutFailure = FOperationFailure();
  if (!bAwaitingDurableFinalization)
    return true;
  bool bRestored = true;
  if (bCommitted) {
    for (const FPackageDurablePlan &Plan : DurablePackagePlans) {
      if (Plan.Intent == EPackageDurableIntent::Save) {
        if (UPackage *Package = Plan.Package.Get())
          Package->SetDirtyFlag(false);
      }
    }
  } else {
    if (TemplatePropagation) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!TemplatePropagation->Restore(Error)) {
        OutFailure.Diagnostic = UeAgentAuthoringDiagnostics::FDiagnostic::WrapFailure(
            TEXT("template_instance_rollback_failed"), Error, TEXT("rollback"), TEXT("Restoring propagated template properties after durable commit failure failed."));
        OutFailure.Diagnostic.RollbackAttempted = true;
        OutFailure.Diagnostic.RollbackSucceeded = false;
        OutFailure.Diagnostic.bRetrySafe = false;
        bRestored = false;
      }
    }
    RestorePackageDirtyStates();
  }
  TemplatePropagation.Reset();
  PackageDirtySnapshots.Reset();
  DurablePackagePlans.Reset();
  bAwaitingDurableFinalization = false;
  return bRestored;
}

namespace {

} // namespace

FApplyFailureProjection ProjectApplyFailure(
    const UeAgentAuthoringWorkspaceTransaction::FPreflightResult &Result) {
  FApplyFailureProjection Projection;
  Projection.bPreflight = true;
  Projection.Phase = TEXT("preflight");
  Projection.Diagnostic = Result.Diagnostic;
  return Projection;
}

FApplyFailureProjection ProjectApplyFailure(
    const UeAgentAuthoringWorkspaceTransaction::FExecutionResult &Result) {
  FApplyFailureProjection Projection;
  Projection.Phase = Result.Phase;
  Projection.Diagnostic = Result.Diagnostic;
  const bool bRestored = Result.bRollbackAttempted && Result.bRollbackSucceeded;
  const bool bStateKnown = Result.Status != TEXT("state_unknown") &&
      (Result.MutationAttempts == 0 || bRestored);
  Projection.Diagnostic.bRetrySafe = bStateKnown && Result.Diagnostic.bRetrySafe;
  if (Projection.Diagnostic.Stage.IsEmpty()) Projection.Diagnostic.Stage = Projection.Phase;
  Projection.Diagnostic.TransactionStatus = Result.Status;
  Projection.Diagnostic.RollbackAttempted = Result.bRollbackAttempted;
  Projection.Diagnostic.RollbackSucceeded = Result.bRollbackSucceeded;
  return Projection;
}

bool TryProjectApplyFailure(const FApplyWorkspaceResult &Result,
                            FApplyFailureProjection &OutFailure) {
  OutFailure = FApplyFailureProjection();
  const bool bHasPreflightFailure =
      !Result.Preflight.Diagnostic.IsEmpty();
  if (!Result.Preflight.IsReady() && bHasPreflightFailure) {
    OutFailure = ProjectApplyFailure(Result.Preflight);
    return OutFailure.IsSet();
  }
  const bool bHasExecutionFailure =
      !Result.Execution.Diagnostic.IsEmpty();
  if (!Result.Execution.IsApplied() && bHasExecutionFailure) {
    OutFailure = ProjectApplyFailure(Result.Execution);
    return OutFailure.IsSet();
  }
  return false;
}

bool ApplyWorkspace(
    const UeAgentApplyWorkspaceContract::FContract &WorkspaceContract,
    const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &Graph,
    const UeAgentApplyInputContract::FRegistry &InputContractRegistry,
    const FProviderRegistry &ProviderRegistry,
    const UeAgentAuthoringWorkspaceTransaction::FExecutionOptions
        &ExecutionOptions,
    const FBackendOptions &BackendOptions, FApplyWorkspaceResult &OutResult) {
  OutResult = FApplyWorkspaceResult();
  FWorkspaceOperationPreflightProvider PreflightProvider(InputContractRegistry,
                                                         ProviderRegistry);
  if (!UeAgentAuthoringWorkspaceTransaction::PreflightWorkspace(
          WorkspaceContract, Workspace, Graph, PreflightProvider,
          OutResult.Preflight)) {
    UE_LOG(LogUeAgentInterface, Display,
           TEXT("StructuredApply phase=preflight_failed error=%s cause=%s detail=%s"),
           *OutResult.Preflight.Diagnostic.Code, *OutResult.Preflight.Diagnostic.CauseCode,
           *OutResult.Preflight.Diagnostic.InternalContext);
    return false;
  }

  FOperationTransactionBackend Backend(BackendOptions);
  const bool bApplied = UeAgentAuthoringWorkspaceTransaction::ExecuteAtomic(
      WorkspaceContract, Workspace, Graph, OutResult.Preflight, Backend,
      ExecutionOptions, OutResult.Execution);
  if (bApplied) {
    for (const UeAgentAuthoringWorkspaceTransaction::FPreparedUnit &Unit :
         OutResult.Preflight.Units) {
      if (!Unit.Mutation.IsValid())
        continue;
      TSharedPtr<FJsonObject> Metadata = Unit.Mutation->BuildResultMetadata();
      if (Metadata.IsValid()) {
        OutResult.UnitResultMetadata.Add(Unit.Mutation->GetUnitKey(),
                                         MoveTemp(Metadata));
      }
    }
  }
  return bApplied;
}
} // namespace UeAgentAuthoringOperationApply
