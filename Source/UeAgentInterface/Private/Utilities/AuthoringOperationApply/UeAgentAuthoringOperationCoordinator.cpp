// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

#include "Module/UeAgentInterfaceLogger.h"

#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
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

void SetFailure(FOperationFailure &OutFailure, const FString &ErrorCode,
                const FString &ErrorDetail = FString()) {
  OutFailure = FOperationFailure();
  OutFailure.ErrorCode = ErrorCode;
  OutFailure.ErrorDetail = ErrorDetail.IsEmpty() ? ErrorCode : ErrorDetail;
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
  if (!Package || Package == GetTransientPackage() ||
      !FPackageName::IsValidLongPackageName(PackageName) ||
      FPackageName::GetPackageMountPoint(PackageName).IsNone() ||
      Package->GetName() != PackageName) {
    SetFailure(OutFailure, TEXT("durable_package_artifact_unrecoverable"),
               PackageName);
    return false;
  }

  const UObject *TopLevelAsset = Plan.TopLevelAsset.Get();
  const bool bMapPackage = Package->ContainsMap() ||
                           (TopLevelAsset && TopLevelAsset->IsA<UWorld>());
  const FString Extension =
      bMapPackage ? FPackageName::GetMapPackageExtension()
                  : FPackageName::GetAssetPackageExtension();
  if (!FPackageName::TryConvertLongPackageNameToFilename(
          PackageName, OutHeaderPath, Extension)) {
    SetFailure(OutFailure, TEXT("durable_package_artifact_unrecoverable"),
               PackageName);
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
        if (OutFailure.ErrorCode.IsEmpty())
          SetFailure(OutFailure, TEXT("durable_package_plan_invalid"));
        OutArtifacts.Reset();
        return false;
      }
      const FString FoldedPlanId = PlanId.ToLower();
      const FString FoldedPackagePath = HeaderPath.ToLower();
      if (PlanIds.Contains(FoldedPlanId) ||
          PackagePaths.Contains(FoldedPackagePath)) {
        SetFailure(OutFailure, TEXT("durable_package_plan_duplicate"),
                   PlanId);
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
      SetFailure(OutFailure, TEXT("durable_package_stage_inventory_mismatch"));
      return false;
    }

    auto FindAssigned = [&InOutArtifacts](const FString &ArtifactId)
        -> UeAgentAuthoringWorkspaceTransaction::FDurableArtifact * {
      return InOutArtifacts.FindByPredicate(
          [&ArtifactId](const auto &Artifact) {
            return Artifact.ArtifactId == ArtifactId;
          });
    };
    auto CleanupAssigned = [&InOutArtifacts]() {
      for (auto &Artifact : InOutArtifacts) {
        if (!Artifact.StagingPath.IsEmpty())
          IFileManager::Get().Delete(*Artifact.StagingPath, false, true, true);
        Artifact.bStagedExists = false;
      }
    };

    for (const auto &Expected : ExpectedArtifacts) {
      const auto *Assigned = FindAssigned(Expected.ArtifactId);
      if (!Assigned ||
          !FPaths::IsSamePath(Assigned->FinalPath, Expected.FinalPath) ||
          Assigned->StagingPath.IsEmpty() ||
          FPaths::IsRelative(Assigned->StagingPath)) {
        SetFailure(OutFailure,
                   TEXT("durable_package_stage_inventory_mismatch"),
                   Expected.ArtifactId);
        CleanupAssigned();
        return false;
      }
    }
    CleanupAssigned();
    ReleasePackageLoaders(Plans);

    for (const FPackageDurablePlan &Plan : Plans) {
      auto *Header = FindAssigned(HeaderArtifactId(Plan));
      auto *Payload = FindAssigned(PayloadArtifactId(Plan));
      if (!Header || !Payload) {
        SetFailure(OutFailure,
                   TEXT("durable_package_stage_inventory_mismatch"),
                   Plan.PlanId);
        CleanupAssigned();
        return false;
      }
      if (Plan.Intent == EPackageDurableIntent::Delete) {
        continue;
      }

      UPackage *Package = Plan.Package.Get();
      UObject *TopLevelAsset = Plan.TopLevelAsset.Get();
      if (!Package || Package == GetTransientPackage()) {
        SetFailure(OutFailure, TEXT("durable_package_artifact_unrecoverable"),
                   Plan.PackageName);
        CleanupAssigned();
        return false;
      }
      if (!TopLevelAsset || TopLevelAsset->GetOutermost() != Package) {
        const FString AssetName =
            FPackageName::GetLongPackageAssetName(Plan.PackageName);
        TopLevelAsset = FindObject<UObject>(Package, *AssetName);
      }
      if (!TopLevelAsset || TopLevelAsset->GetOutermost() != Package) {
        SetFailure(OutFailure, TEXT("durable_package_top_level_asset_missing"),
                   Plan.PackageName);
        CleanupAssigned();
        return false;
      }

      const FString HeaderExtension =
          FPaths::GetExtension(Header->FinalPath, true);
      const FString ScratchHeader = Header->StagingPath + HeaderExtension;
      const FString ScratchPayload =
          FPaths::ChangeExtension(ScratchHeader, TEXT("upayload"));
      IFileManager::Get().Delete(*ScratchHeader, false, true, true);
      IFileManager::Get().Delete(*ScratchPayload, false, true, true);
      if (!IFileManager::Get().MakeDirectory(
              *FPaths::GetPath(Header->StagingPath), true)) {
        SetFailure(OutFailure, TEXT("durable_package_stage_root_unavailable"),
                   Header->StagingPath);
        CleanupAssigned();
        return false;
      }

      FSavePackageArgs SaveArgs;
      SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
      SaveArgs.SaveFlags =
          SAVE_NoError | SAVE_KeepDirty | SAVE_FromAutosave;
      if (!UPackage::SavePackage(Package, TopLevelAsset, *ScratchHeader,
                                 SaveArgs)) {
        SetFailure(OutFailure, TEXT("durable_package_stage_save_failed"),
                   Plan.PackageName);
        IFileManager::Get().Delete(*ScratchHeader, false, true, true);
        IFileManager::Get().Delete(*ScratchPayload, false, true, true);
        CleanupAssigned();
        return false;
      }
      if (!IFileManager::Get().Move(*Header->StagingPath, *ScratchHeader, true,
                                    true, false, true)) {
        SetFailure(OutFailure, TEXT("durable_package_stage_move_failed"),
                   Header->ArtifactId);
        IFileManager::Get().Delete(*ScratchHeader, false, true, true);
        IFileManager::Get().Delete(*ScratchPayload, false, true, true);
        CleanupAssigned();
        return false;
      }
      Header->bStagedExists = true;
      if (IFileManager::Get().FileExists(*ScratchPayload)) {
        if (!IFileManager::Get().Move(*Payload->StagingPath, *ScratchPayload,
                                      true, true, false, true)) {
          SetFailure(OutFailure, TEXT("durable_package_stage_move_failed"),
                     Payload->ArtifactId);
          IFileManager::Get().Delete(*ScratchPayload, false, true, true);
          CleanupAssigned();
          return false;
        }
        Payload->bStagedExists = true;
      }
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
    SetFailure(OutFailure, TEXT("durable_request_id_missing"));
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
    SetFailure(OutFailure, TEXT("durable_request_id_missing"));
    return false;
  }
  const FString ExpectedRoot = DurableJournalRoot();
  if (Context.JournalRoot.IsEmpty() || FPaths::IsRelative(Context.JournalRoot) ||
      !FPaths::IsSamePath(Context.JournalRoot, ExpectedRoot)) {
    SetFailure(OutFailure, TEXT("durable_journal_root_invalid"),
               Context.JournalRoot);
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
    FOperationFailure &OutFailure, bool bLifecycleStageable) {
  OutFailure = FOperationFailure();
  if (!bLifecycleStageable) {
    SetFailure(OutFailure,
               TEXT("durable_package_lifecycle_not_stageable"), PlanId);
    return false;
  }
  const FString NormalizedPlanId = PlanId.TrimStartAndEnd();
  const FString NormalizedPackageName = PackageName.TrimStartAndEnd();
  if (NormalizedPlanId.IsEmpty() ||
      !FPackageName::IsValidLongPackageName(NormalizedPackageName) ||
      FPackageName::GetPackageMountPoint(NormalizedPackageName).IsNone()) {
    SetFailure(OutFailure, TEXT("durable_package_artifact_unrecoverable"),
               NormalizedPackageName);
    return false;
  }

  UPackage *Package = Asset ? Asset->GetOutermost() : nullptr;
  if (!Package || Package == GetTransientPackage() ||
      Package->GetName() != NormalizedPackageName) {
    Package = FindPackage(nullptr, *NormalizedPackageName);
  }
  if (!Package && Intent == EPackageDurableIntent::Save)
    Package = CreatePackage(*NormalizedPackageName);
  if (!Package || Package == GetTransientPackage()) {
    SetFailure(OutFailure, TEXT("durable_package_artifact_unrecoverable"),
               NormalizedPackageName);
    return false;
  }

  FPackageDurablePlan &Plan = OutPlans.AddDefaulted_GetRef();
  Plan.PlanId = NormalizedPlanId;
  Plan.PackageName = NormalizedPackageName;
  Plan.Package = Package;
  Plan.TopLevelAsset = Asset;
  Plan.Intent = Intent;
  return true;
}

bool IPreparedOperationState::BuildDurablePackagePlans(
    TArray<FPackageDurablePlan> &OutPlans,
    FOperationFailure &OutFailure) const {
  OutPlans.Reset();
  SetFailure(OutFailure, TEXT("durable_package_plan_not_supported"),
             GetTargetKey());
  return false;
}

FProviderRegistry &FProviderRegistry::Get() {
  static FProviderRegistry Registry;
  return Registry;
}

bool FProviderRegistry::RegisterProvider(
    const TSharedRef<IApplyProvider> &Provider, ERegistrationResult &OutResult,
    FString &OutError) {
  const FString ProviderId = NormalizeId(Provider->GetProviderId());
  if (ProviderId.IsEmpty()) {
    OutError = TEXT("operation_provider_id_missing");
    return false;
  }

  FWriteScopeLock Lock(RegistryLock);
  if (const TSharedPtr<IApplyProvider> *Existing = Providers.Find(ProviderId)) {
    if (Existing->Get() != &Provider.Get()) {
      OutError = TEXT("conflicting_operation_provider_") + ProviderId;
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

bool FWorkspaceOperationPreflightProvider::PreflightUnit(
    const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit,
    TUniquePtr<UeAgentAuthoringWorkspaceTransaction::IPreparedMutation>
        &OutMutation,
    FOperationFailure &OutFailure) const {
  OutMutation.Reset();
  OutFailure = FOperationFailure();

  UeAgentApplyInputContract::FContract Contract;
  if (!ContractRegistry.FindContract(Unit.ApplyContractRef, Contract)) {
    SetFailure(OutFailure, TEXT("operation_contract_not_registered"),
               Unit.ApplyContractRef);
    return false;
  }

  const FString DocumentBefore = StableDocumentKey(Unit.Document);
  UeAgentApplyInputContract::FOperationInput Input;
  UeAgentApplyInputContract::FParseFailure ParseFailure;
  if (!UeAgentApplyInputContract::ParseOperationInput(Contract, Unit, Input,
                                                      ParseFailure)) {
    SetFailure(OutFailure, ParseFailure.ErrorCode, ParseFailure.ErrorDetail);
    return false;
  }

  TSharedPtr<IApplyProvider> Provider;
  if (!ProviderRegistry.CopyProvider(Contract.ProviderId, Provider)) {
    SetFailure(OutFailure, TEXT("operation_provider_not_registered"),
               Contract.ProviderId);
    return false;
  }

  FPrepareRequest Request;
  Request.Contract = &Contract;
  Request.Unit = &Unit;
  Request.Input = &Input;
  TUniquePtr<IPreparedOperationState> State;
  if (!Provider->PrepareOperation(Request, State, OutFailure) ||
      !State.IsValid()) {
    if (OutFailure.ErrorCode.IsEmpty()) {
      SetFailure(OutFailure, TEXT("operation_provider_preflight_failed"),
                 Contract.ProviderId);
    }
    return false;
  }

  if (DocumentBefore != StableDocumentKey(Unit.Document)) {
    SetFailure(OutFailure, TEXT("operation_provider_mutated_input"),
               Unit.CanonicalUnitKey);
    return false;
  }

  const UeAgentApplyInputContract::EOperation ResolvedOperation =
      State->GetResolvedOperation();
  if (!IsValidResolvedOperation(Input.RequestedOperation, ResolvedOperation) ||
      !Contract.FindOperation(ResolvedOperation)) {
    SetFailure(
        OutFailure, TEXT("operation_provider_resolution_invalid"),
        UeAgentApplyInputContract::OperationToString(Input.RequestedOperation) +
            TEXT("->") +
            UeAgentApplyInputContract::OperationToString(ResolvedOperation));
    return false;
  }
  if (State->GetTargetKey().TrimStartAndEnd().IsEmpty()) {
    SetFailure(OutFailure, TEXT("operation_provider_target_missing"),
               Unit.CanonicalUnitKey);
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
    SetFailure(OutFailure, TEXT("operation_backend_mismatch"),
               Mutation.GetBackendId());
    return nullptr;
  }
  return static_cast<FPreparedOperationMutation *>(&Mutation);
}

void FOperationTransactionBackend::CapturePackageDirtyStates(
    const IPreparedOperationState &State) {
  TArray<UObject *> Objects;
  State.CollectRollbackObjects(Objects);
  for (UObject *Object : Objects) {
    UPackage *Package = Object ? Object->GetPackage() : nullptr;
    if (!Package)
      continue;
    const bool bAlreadyCaptured = PackageDirtySnapshots.ContainsByPredicate(
        [Package](const FPackageDirtySnapshot &Snapshot) {
          return Snapshot.Package.Get() == Package;
        });
    if (bAlreadyCaptured)
      continue;
    FPackageDirtySnapshot &Snapshot = PackageDirtySnapshots.AddDefaulted_GetRef();
    Snapshot.Package = Package;
    Snapshot.bWasDirty = Package->IsDirty();
  }
}

void FOperationTransactionBackend::RestorePackageDirtyStates() {
  for (const FPackageDirtySnapshot &Snapshot : PackageDirtySnapshots) {
    if (UPackage *Package = Cast<UPackage>(Snapshot.Package.Get()))
      Package->SetDirtyFlag(Snapshot.bWasDirty);
  }
}

bool FOperationTransactionBackend::CaptureRequests(
    const TArray<UeAgentAuthoringState::FReadRequest> &Requests,
    UeAgentAuthoringState::FStateSnapshot &OutSnapshot,
    FOperationFailure &OutFailure) const {
  if (Requests.IsEmpty()) {
    SetFailure(OutFailure, TEXT("operation_verification_requests_missing"));
    return false;
  }
  UeAgentAuthoringState::FCaptureResult CaptureResult;
  if (!UeAgentAuthoringState::CaptureSnapshot(Requests, CaptureResult,
                                              Options.SemanticRegistry,
                                              Options.StateAccessorRegistry)) {
    SetFailure(OutFailure,
               CaptureResult.ErrorCode.IsEmpty()
                   ? TEXT("operation_snapshot_capture_failed")
                   : CaptureResult.ErrorCode,
               CaptureResult.ErrorDetail);
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
    SetFailure(OutFailure, TEXT("operation_workspace_begin_invalid"));
    return false;
  }
  AppliedMutations.Reset();
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
  bWorkspaceOpen = true;
  return true;
}

bool FOperationTransactionBackend::CaptureRollback(
    UeAgentAuthoringWorkspaceTransaction::IPreparedMutation &Mutation,
    FOperationFailure &OutFailure) {
  FPreparedOperationMutation *Prepared = ResolveMutation(Mutation, OutFailure);
  if (!bWorkspaceOpen || !Prepared || !Prepared->State.IsValid()) {
    if (OutFailure.ErrorCode.IsEmpty())
      SetFailure(OutFailure, TEXT("operation_rollback_capture_invalid"));
    return false;
  }
  CapturePackageDirtyStates(*Prepared->State);
  return Prepared->State->CaptureRollback(OutFailure);
}

bool FOperationTransactionBackend::ApplyUnit(
    UeAgentAuthoringWorkspaceTransaction::IPreparedMutation &Mutation,
    FOperationFailure &OutFailure) {
  FPreparedOperationMutation *Prepared = ResolveMutation(Mutation, OutFailure);
  if (!bWorkspaceOpen || !Prepared || !Prepared->State.IsValid()) {
    if (OutFailure.ErrorCode.IsEmpty())
      SetFailure(OutFailure, TEXT("operation_apply_invalid"));
    return false;
  }

  if (!Prepared->State->ApplyProposed(OutFailure))
    return false;

  FUeAgentInterfaceLogger::Log(
      TEXT("StructuredApply phase=expected_snapshot_begin target=%s"),
      *Prepared->State->GetTargetKey());
  TArray<UeAgentAuthoringState::FReadRequest> ExpectedRequests;
  if (!Prepared->State->BuildExpectedReadRequests(ExpectedRequests,
                                                  OutFailure) ||
      !CaptureRequests(ExpectedRequests, Prepared->ExpectedSnapshot,
                       OutFailure)) {
    return false;
  }

  FUeAgentInterfaceLogger::Log(
      TEXT("StructuredApply phase=expected_snapshot_end target=%s requests=%d"),
      *Prepared->State->GetTargetKey(), ExpectedRequests.Num());
  Prepared->bExpectedCaptured = true;
  if (!Prepared->State->ApplyLifecycle(OutFailure))
    return false;
  Prepared->bLifecycleApplied = true;
  AppliedMutations.Add(Prepared);
  return true;
}

bool FOperationTransactionBackend::Compile(FOperationFailure &OutFailure) {
  for (FPreparedOperationMutation *Prepared : AppliedMutations) {
    if (!Prepared || !Prepared->State.IsValid() ||
        !Prepared->State->Compile(OutFailure))
      return false;
  }
  return true;
}

bool FOperationTransactionBackend::Save(FOperationFailure &OutFailure) {
  for (FPreparedOperationMutation *Prepared : AppliedMutations) {
    if (!Prepared || !Prepared->State.IsValid() ||
        !Prepared->State->Save(OutFailure))
      return false;
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
    SetFailure(OutFailure, TEXT("durable_package_stager_unavailable"));
    return false;
  }

  for (UeAgentAuthoringWorkspaceTransaction::IPreparedMutation *Mutation :
       Mutations) {
    if (!Mutation) {
      SetFailure(OutFailure, TEXT("durable_package_mutation_invalid"));
      DurablePackagePlans.Reset();
      return false;
    }
    FPreparedOperationMutation *Prepared =
        ResolveMutation(*Mutation, OutFailure);
    if (!Prepared || !Prepared->State.IsValid()) {
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
      SetFailure(OutFailure, TEXT("durable_package_plan_missing"),
                 Prepared->State->GetTargetKey());
      DurablePackagePlans.Reset();
      return false;
    }
    DurablePackagePlans.Append(MoveTemp(MutationPlans));
  }
  if (DurablePackagePlans.IsEmpty()) {
    SetFailure(OutFailure, TEXT("durable_package_plan_missing"));
    return false;
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
    SetFailure(OutFailure, TEXT("durable_package_stage_invalid"));
    return false;
  }
  ReleasePackageLoaders(DurablePackagePlans);
  return Options.PackageStager->StagePackages(
      DurablePackagePlans, InOutArtifacts, OutFailure);
}

bool FOperationTransactionBackend::Verify(FOperationFailure &OutFailure) {
  for (FPreparedOperationMutation *Prepared : AppliedMutations) {
    if (!Prepared || !Prepared->State.IsValid() ||
        !Prepared->bExpectedCaptured) {
      SetFailure(OutFailure, TEXT("operation_expected_snapshot_missing"));
      return false;
    }

    TArray<UeAgentAuthoringState::FReadRequest> ActualRequests;
    if (!Prepared->State->BuildActualReadRequests(ActualRequests, OutFailure))
      return false;
    UeAgentAuthoringState::FStateSnapshot ActualSnapshot;
    if (!CaptureRequests(ActualRequests, ActualSnapshot, OutFailure))
      return false;

    UeAgentAuthoringSemanticCompare::FComparisonReport Report;
    if (!UeAgentAuthoringSemanticCompare::CompareSnapshots(
            Prepared->ExpectedSnapshot, ActualSnapshot,
            UeAgentAuthoringSemanticCompare::FCompareOptions(), Report,
            Options.SemanticRegistry, Options.ComparatorRegistry) ||
        !Report.bEquivalent) {
      TArray<FString> Differences;
      const TArray<UeAgentAuthoringState::FStateEntry> ExpectedEntries =
          Prepared->ExpectedSnapshot.CopyEntries();
      const TArray<UeAgentAuthoringState::FStateEntry> ActualEntries =
          ActualSnapshot.CopyEntries();
      for (const UeAgentAuthoringSemanticCompare::FEntryComparison &Entry :
           Report.Entries) {
        if (!Entry.bEquivalent) {
          const UeAgentAuthoringState::FStateEntry *ExpectedEntry =
              ExpectedEntries.FindByPredicate(
                  [&Entry](
                      const UeAgentAuthoringState::FStateEntry &Candidate) {
                    return Candidate.SemanticId == Entry.SemanticId &&
                           Candidate.TargetIdentity.CanonicalKey ==
                               Entry.TargetIdentity.CanonicalKey;
                  });
          const UeAgentAuthoringState::FStateEntry *ActualEntry =
              ActualEntries.FindByPredicate(
                  [&Entry](
                      const UeAgentAuthoringState::FStateEntry &Candidate) {
                    return Candidate.SemanticId == Entry.SemanticId &&
                           Candidate.TargetIdentity.CanonicalKey ==
                               Entry.TargetIdentity.CanonicalKey;
                  });
          Differences.Add(FString::Printf(
              TEXT("%s:%s:%s:%s:expected=%d/%d:actual=%d/%d"),
              *Entry.SemanticId, *Entry.ErrorCode, *Entry.ErrorDetail,
              *Entry.TargetIdentity.CanonicalKey,
              ExpectedEntry ? ExpectedEntry->bAvailable : false,
              ExpectedEntry ? ExpectedEntry->bEditable : false,
              ActualEntry ? ActualEntry->bAvailable : false,
              ActualEntry ? ActualEntry->bEditable : false));
        }
      }
      SetFailure(OutFailure,
                 Report.ErrorCode.IsEmpty()
                     ? TEXT("operation_semantic_verification_failed")
                     : Report.ErrorCode,
                 Differences.IsEmpty() ? Report.ErrorDetail
                                       : FString::Join(Differences, TEXT(";")));
      return false;
    }
  }
  return true;
}

bool FOperationTransactionBackend::RollbackUnit(
    UeAgentAuthoringWorkspaceTransaction::IPreparedMutation &Mutation,
    FOperationFailure &OutFailure) {
  FPreparedOperationMutation *Prepared = ResolveMutation(Mutation, OutFailure);
  if (!Prepared || !Prepared->State.IsValid())
    return false;
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
    SetFailure(OutFailure, TEXT("operation_workspace_not_open"));
    return false;
  }
  if (bCommit) {
    for (FPreparedOperationMutation *Prepared : AppliedMutations) {
      if (!Prepared || !Prepared->State.IsValid() ||
          !Prepared->State->Commit(OutFailure))
        return false;
    }
  }
  if (!bCommit && EditorTransaction.IsValid())
    EditorTransaction->Cancel();
  if (!bCommit)
    RestorePackageDirtyStates();
  const bool bNeedsDurableFinalization =
      bCommit && !DurablePackagePlans.IsEmpty();
  EditorTransaction.Reset();
  AppliedMutations.Reset();
  if (!bNeedsDurableFinalization) {
    PackageDirtySnapshots.Reset();
    DurablePackagePlans.Reset();
  }
  bAwaitingDurableFinalization = bNeedsDurableFinalization;
  bWorkspaceOpen = false;
  return true;
}

void FOperationTransactionBackend::FinalizeDurableCommit(bool bCommitted) {
  if (!bAwaitingDurableFinalization)
    return;
  if (bCommitted) {
    for (const FPackageDurablePlan &Plan : DurablePackagePlans) {
      if (Plan.Intent == EPackageDurableIntent::Save) {
        if (UPackage *Package = Plan.Package.Get())
          Package->SetDirtyFlag(false);
      }
    }
  } else {
    RestorePackageDirtyStates();
  }
  PackageDirtySnapshots.Reset();
  DurablePackagePlans.Reset();
  bAwaitingDurableFinalization = false;
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
           *OutResult.Preflight.ErrorCode, *OutResult.Preflight.CauseCode,
           *OutResult.Preflight.ErrorDetail);
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
