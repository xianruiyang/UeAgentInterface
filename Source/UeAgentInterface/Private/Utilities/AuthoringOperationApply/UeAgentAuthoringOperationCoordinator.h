// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "UObject/WeakObjectPtr.h"

class FScopedTransaction;
class UClass;
class UObject;
class UPackage;
namespace UeAgentTemplatePropertyPropagation { class FScope; }

#define UE_AGENT_AUTHORING_OPERATION_DURABLE_PACKAGE_CONTRACT_VERSION 1

namespace UeAgentAuthoringOperationApply {
using FOperationFailure =
    UeAgentAuthoringWorkspaceTransaction::FOperationFailure;

enum class EPackageDurableIntent : uint8 {
  Save,
  Delete,
  // Included in a memory checkpoint, while a successful publication preserves
  // the existing disk version instead of saving unrelated dirty memory.
  Preserve,
};

inline const TCHAR* LexToString(EPackageDurableIntent Intent) {
  switch (Intent) {
    case EPackageDurableIntent::Save: return TEXT("save");
    case EPackageDurableIntent::Delete: return TEXT("delete");
    case EPackageDurableIntent::Preserve: return TEXT("preserve");
  }
  return TEXT("unknown");
}

enum class EPackageReadbackPolicy : uint8 {
  UnloadAndLoad,
  ReplaceResidentPackage,
};

enum class EPackageBeforePresence : uint8 { Present, Absent };

struct UEAGENTINTERFACE_API FPackageDurablePlan {
  FString PlanId;
  FString PackageName;
  TWeakObjectPtr<UPackage> Package;
  TWeakObjectPtr<UObject> TopLevelAsset;
  EPackageDurableIntent Intent = EPackageDurableIntent::Save;
  EPackageReadbackPolicy ReadbackPolicy = EPackageReadbackPolicy::UnloadAndLoad;
  EPackageBeforePresence BeforePresence = EPackageBeforePresence::Present;
  // Native-owned packages may become empty tombstones during an edit. Their
  // absence is staged in the same journal as the containing asset/map.
  FString OwningPackageName;
  // Domain scope for the existing ownership collector. Empty retains the
  // ordinary package closure; a scoped World supplies its selected actors.
  TFunction<void(UPackage*, TArray<UPackage*>&)> CollectOwnedPackages;
};

class UEAGENTINTERFACE_API IDurablePackageStager {
public:
  virtual ~IDurablePackageStager() = default;

  virtual bool BuildArtifactInventory(
      const TArray<FPackageDurablePlan> &Plans,
      TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>
          &OutArtifacts,
      FOperationFailure &OutFailure) = 0;
  virtual bool StagePackages(
      const TArray<FPackageDurablePlan> &Plans,
      TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>
          &InOutArtifacts,
      FOperationFailure &OutFailure) = 0;
};

UEAGENTINTERFACE_API TSharedRef<IDurablePackageStager>
CreateDefaultDurablePackageStager();

struct UEAGENTINTERFACE_API FApplyRequestContext {
  FString RequestId;
  FString JournalRoot;
};

UEAGENTINTERFACE_API FString GetAuthoringDurableJournalRoot();

UEAGENTINTERFACE_API bool MakeCommandApplyRequestContext(
    const FString &RequestId, FApplyRequestContext &OutContext,
    FOperationFailure &OutFailure);

UEAGENTINTERFACE_API bool BuildExecutionOptions(
    const FApplyRequestContext &Context,
    UeAgentAuthoringWorkspaceTransaction::FExecutionOptions &OutOptions,
    FOperationFailure &OutFailure);

UEAGENTINTERFACE_API bool BuildApplyEntryExecutionOptions(
    const FString &RequestId,
    UeAgentAuthoringWorkspaceTransaction::FExecutionOptions &OutOptions,
    FOperationFailure &OutFailure);

UEAGENTINTERFACE_API bool BuildPackageDurablePlan(
    const FString &PlanId, const FString &PackageName, UObject *Asset,
    EPackageDurableIntent Intent, TArray<FPackageDurablePlan> &OutPlans,
    FOperationFailure &OutFailure,
    bool bLifecycleStageable = true,
    EPackageBeforePresence BeforePresence = EPackageBeforePresence::Present,
    const UClass* AssetClass = nullptr);

enum class ERegistrationResult : uint8 {
  Registered,
  AlreadyRegistered,
};

struct UEAGENTINTERFACE_API FPrepareRequest {
  const UeAgentApplyInputContract::FContract *Contract = nullptr;
  const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *Unit = nullptr;
  const UeAgentApplyInputContract::FOperationInput *Input = nullptr;

  bool IsValid() const { return Contract && Unit && Input; }
};

class UEAGENTINTERFACE_API IPreparedOperationState {
public:
  virtual ~IPreparedOperationState() = default;

  virtual FString GetTargetKey() const = 0;
  virtual UeAgentApplyInputContract::EOperation
  GetResolvedOperation() const = 0;
  virtual bool IsNoOp() const { return false; }
  virtual bool IsStateNoOp() const { return IsNoOp(); }
  virtual void CollectRollbackObjects(TArray<UObject *> &OutObjects) const {}
  virtual bool CaptureRollback(FOperationFailure &OutFailure) = 0;
  virtual bool ApplyProposed(FOperationFailure &OutFailure) = 0;
  virtual bool CaptureExpectedAfterLifecycle() const { return false; }
  virtual bool BuildExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const = 0;
  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) = 0;
  virtual bool Compile(FOperationFailure &OutFailure) = 0;
  virtual bool Save(FOperationFailure &OutFailure) = 0;
  virtual bool BuildDurablePackagePlans(
      TArray<FPackageDurablePlan> &OutPlans,
      FOperationFailure &OutFailure) const;
  virtual bool BuildActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const = 0;
  virtual bool BuildBeforeReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const {
    return BuildActualReadRequests(OutRequests, OutFailure);
  }
  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const {
    return nullptr;
  }
  virtual bool Commit(FOperationFailure &OutFailure) {
    OutFailure = FOperationFailure();
    return true;
  }
  virtual bool Rollback(FOperationFailure &OutFailure) = 0;
  virtual bool SupportsPersistentReadback() const { return false; }
  virtual bool RequiresCheckpointRollback() const { return false; }
  virtual bool ReleaseForPersistentReadback(FOperationFailure &OutFailure) { return false; }
  virtual bool RebindAfterPersistentReadback(FOperationFailure &OutFailure) { return false; }
  virtual bool PrepareCheckpointRestore(FOperationFailure &OutFailure) {
    return ReleaseForPersistentReadback(OutFailure);
  }
  virtual bool RebindAfterCheckpointRestore(FOperationFailure &OutFailure) {
    return RebindAfterPersistentReadback(OutFailure);
  }
};

class UEAGENTINTERFACE_API IApplyProvider {
public:
  virtual ~IApplyProvider() = default;
  virtual FString GetProviderId() const = 0;
  virtual bool PrepareOperation(const FPrepareRequest &Request,
                                TUniquePtr<IPreparedOperationState> &OutState,
                                FOperationFailure &OutFailure) const = 0;
};

class UEAGENTINTERFACE_API FProviderRegistry {
public:
  FProviderRegistry() = default;
  FProviderRegistry(const FProviderRegistry &) = delete;
  FProviderRegistry &operator=(const FProviderRegistry &) = delete;

  static FProviderRegistry &Get();

  bool RegisterProvider(const TSharedRef<IApplyProvider> &Provider,
                        ERegistrationResult &OutResult, UE::UAI::Contract::FDiagnostic& OutError);
  bool CopyProvider(const FString &ProviderId,
                    TSharedPtr<IApplyProvider> &OutProvider) const;
  TArray<FString> ListProviderIds() const;
  int32 GetProviderCount() const;

private:
  mutable FRWLock RegistryLock;
  TMap<FString, TSharedPtr<IApplyProvider>> Providers;
};

class UEAGENTINTERFACE_API FPreparedOperationMutation final
    : public UeAgentAuthoringWorkspaceTransaction::IPreparedMutation {
public:
  static const TCHAR *BackendId;

  FPreparedOperationMutation(FString InUnitKey,
                             UeAgentApplyInputContract::FContract InContract,
                             UeAgentApplyInputContract::FOperationInput InInput,
                             TUniquePtr<IPreparedOperationState> InState);

  virtual FString GetBackendId() const override;
  virtual FString GetUnitKey() const override;
  virtual FString GetTargetKey() const override;
  virtual bool IsNoOp() const override;
  virtual bool IsStateNoOp() const override;
  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const override;

  const UeAgentApplyInputContract::FContract &GetContract() const {
    return Contract;
  }
  const UeAgentApplyInputContract::FOperationInput &GetInput() const {
    return Input;
  }

private:
  FString UnitKey;
  UeAgentApplyInputContract::FContract Contract;
  UeAgentApplyInputContract::FOperationInput Input;
  TUniquePtr<IPreparedOperationState> State;
  UeAgentAuthoringState::FStateSnapshot ExpectedSnapshot;
  UeAgentAuthoringState::FStateSnapshot BeforeSnapshot;
  bool bBeforeCaptured = false;
  bool bExpectedCaptured = false;
  bool bLifecycleApplied = false;

  friend class FOperationTransactionBackend;
};

UEAGENTINTERFACE_API bool TryResolveWorkspaceOperationInput(
    const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit,
    const UeAgentApplyInputContract::FRegistry &ContractRegistry,
    UeAgentApplyInputContract::FContract &OutContract,
    UeAgentApplyInputContract::FOperationInput &OutInput,
    FOperationFailure &OutFailure);

class UEAGENTINTERFACE_API FWorkspaceOperationPreflightProvider final
    : public UeAgentAuthoringWorkspaceTransaction::IPreflightProvider {
public:
  FWorkspaceOperationPreflightProvider(
      const UeAgentApplyInputContract::FRegistry &InContractRegistry,
      const FProviderRegistry &InProviderRegistry);

  virtual bool PreflightUnit(
      const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit,
      TUniquePtr<UeAgentAuthoringWorkspaceTransaction::IPreparedMutation>
          &OutMutation,
      FOperationFailure &OutFailure) const override;

private:
  const UeAgentApplyInputContract::FRegistry &ContractRegistry;
  const FProviderRegistry &ProviderRegistry;
};

struct FPublishedPackageReadback {
  FString PackageName;
  FString FinalPath;
  EPackageDurableIntent Intent = EPackageDurableIntent::Save;
};

struct FOperationReadback {
  UeAgentApplyInputContract::EOperation Operation;
  TArray<UeAgentAuthoringState::FReadRequest> Requests;
  const UeAgentAuthoringState::FStateSnapshot* Expected = nullptr;
  const UeAgentAuthoringState::FStateSnapshot* Before = nullptr;
};

struct UEAGENTINTERFACE_API FBackendOptions {
  bool bUseEditorTransaction = true;
  FText TransactionDescription;
  const UeAgentAuthoringSemanticRegistry::FRegistry *SemanticRegistry = nullptr;
  const UeAgentAuthoringState::FStateAccessorRegistry *StateAccessorRegistry =
      nullptr;
  const UeAgentAuthoringSemanticCompare::FComparatorRegistry
      *ComparatorRegistry = nullptr;
  TSharedPtr<IDurablePackageStager> PackageStager;
  // Synchronous public-import readback, executed within the recoverable boundary.
  TFunction<bool(FOperationFailure &)> FinalReadback;
  // An optional domain observer of the actual published files. Unset means
  // this shape needs the existing unload/rebind path. Verify must run while
  // its independently loaded objects are alive; it is the same comparison
  // used by the ordinary transaction, not a second apply or transaction.
  TFunction<TOptional<bool>(const TArray<FPublishedPackageReadback>&,
      TArray<FOperationReadback>&,
      TFunctionRef<bool(FOperationFailure&)>, FOperationFailure&)>
      ObservePublishedReadback;
  // Settle shared native effects after every operation has compiled, before
  // refreshing the durable inventory, verifying or saving the workspace.
  TFunction<bool(FOperationFailure &)> FinalizeNativeUpdate;
};

class UEAGENTINTERFACE_API FOperationTransactionBackend final
    : public UeAgentAuthoringWorkspaceTransaction::ITransactionBackend {
public:
  explicit FOperationTransactionBackend(
      FBackendOptions InOptions = FBackendOptions());
  virtual ~FOperationTransactionBackend() override;

  virtual bool BeginWorkspace(
      const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
      const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &Graph,
      FOperationFailure &OutFailure) override;
  virtual bool CaptureRollback(
      UeAgentAuthoringWorkspaceTransaction::IPreparedMutation &Mutation,
      FOperationFailure &OutFailure) override;
  virtual bool
  ApplyUnit(UeAgentAuthoringWorkspaceTransaction::IPreparedMutation &Mutation,
            FOperationFailure &OutFailure) override;
  virtual bool Compile(FOperationFailure &OutFailure) override;
  virtual bool Save(FOperationFailure &OutFailure) override;
  virtual bool SupportsDurableTransactions() const override;
  virtual bool BuildDurableArtifactInventory(
      const TArray<UeAgentAuthoringWorkspaceTransaction::IPreparedMutation *>
          &Mutations,
      TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>
          &OutArtifacts,
      FOperationFailure &OutFailure) override;
  virtual bool StageDurableArtifacts(
      TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>
          &InOutArtifacts,
      FOperationFailure &OutFailure) override;
  virtual bool CollectAdditionalDurableArtifacts(
      TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& OutArtifacts,
      FOperationFailure& OutFailure) override;
  virtual bool StageMemoryCheckpoint(
      TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact> &InOutArtifacts,
      FOperationFailure &OutFailure) override;
  virtual bool Verify(FOperationFailure &OutFailure) override;
  virtual bool RollbackUnit(
      UeAgentAuthoringWorkspaceTransaction::IPreparedMutation &Mutation,
      FOperationFailure &OutFailure) override;
  virtual bool EndWorkspace(bool bCommit,
                            FOperationFailure &OutFailure) override;
		virtual bool FinalizeDurableCommit(bool bCommitted, FOperationFailure& OutFailure) override;
  virtual bool RequiresPersistentReadback() const override;
  virtual bool RequiresMemoryCheckpoint() const override;
  virtual bool RequiresCheckpointRollback() const override;
  virtual bool HasReleasedNativeState() const override;
  virtual bool FinalizeVerification(FOperationFailure &OutFailure) override;
  virtual bool PrepareCheckpointRestore(FOperationFailure &OutFailure) override;
  virtual bool ReloadCheckpoint(FOperationFailure &OutFailure) override;
  virtual bool VerifyRestoredState(FOperationFailure &OutFailure) override;

private:
  struct FPackageDirtySnapshot {
    TWeakObjectPtr<UObject> Package;
    FString PackageName;
    bool bWasDirty = false;
  };

  FPreparedOperationMutation *ResolveMutation(
      UeAgentAuthoringWorkspaceTransaction::IPreparedMutation &Mutation,
      FOperationFailure &OutFailure) const;
  void CapturePackageDirtyStates(const IPreparedOperationState &State);
  void CapturePackageDirtyState(UPackage* Package);
  void RestorePackageDirtyStates();
  bool
  CaptureRequests(const TArray<UeAgentAuthoringState::FReadRequest> &Requests,
                  UeAgentAuthoringState::FStateSnapshot &OutSnapshot,
                  FOperationFailure &OutFailure) const;
  bool BuildReadbackRequests(TArray<FOperationReadback>& OutReadbacks,
                            FOperationFailure& OutFailure);
  bool VerifyReadbackRequests(TArray<FOperationReadback>& Readbacks,
                             FOperationFailure& OutFailure);

  FBackendOptions Options;
  TUniquePtr<FScopedTransaction> EditorTransaction;
  TUniquePtr<UeAgentTemplatePropertyPropagation::FScope> TemplatePropagation;
  TArray<FPreparedOperationMutation *> AppliedMutations;
  TArray<FPreparedOperationMutation *> CapturedMutations;
  TArray<FPackageDirtySnapshot> PackageDirtySnapshots;
  TMap<FString, int32> PackageDirtySnapshotIndex;
  TArray<FPackageDurablePlan> DurablePackagePlans;
  TArray<FPublishedPackageReadback> PublishedReadbackPackages;
  bool bWorkspaceOpen = false;
  bool bAwaitingDurableFinalization = false;
  bool bNativeStateReleased = false;
  bool bObservingPublishedState = false;
  bool UnloadForReadback(FOperationFailure &OutFailure, bool bRestoringBefore = false);
  bool ReloadForReadback(FOperationFailure &OutFailure, bool bRestoringBefore = false);
};

struct UEAGENTINTERFACE_API FApplyWorkspaceResult {
  UeAgentAuthoringWorkspaceTransaction::FPreflightResult Preflight;
  UeAgentAuthoringWorkspaceTransaction::FExecutionResult Execution;
  TMap<FString, TSharedPtr<FJsonObject>> UnitResultMetadata;

  FApplyWorkspaceResult() = default;
  FApplyWorkspaceResult(FApplyWorkspaceResult &&) = default;
  FApplyWorkspaceResult &operator=(FApplyWorkspaceResult &&) = default;
  FApplyWorkspaceResult(const FApplyWorkspaceResult &) = delete;
  FApplyWorkspaceResult &operator=(const FApplyWorkspaceResult &) = delete;
};

struct UEAGENTINTERFACE_API FApplyFailureProjection {
  bool bPreflight = false;
  FString Phase;
  UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;

  bool IsSet() const { return !Diagnostic.IsEmpty(); }
};

UEAGENTINTERFACE_API FApplyFailureProjection ProjectApplyFailure(
    const UeAgentAuthoringWorkspaceTransaction::FPreflightResult &Result);
UEAGENTINTERFACE_API FApplyFailureProjection ProjectApplyFailure(
    const UeAgentAuthoringWorkspaceTransaction::FExecutionResult &Result);
UEAGENTINTERFACE_API bool TryProjectApplyFailure(
    const FApplyWorkspaceResult &Result, FApplyFailureProjection &OutFailure);

UEAGENTINTERFACE_API bool ApplyWorkspace(
    const UeAgentApplyWorkspaceContract::FContract &WorkspaceContract,
    const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &Graph,
    const UeAgentApplyInputContract::FRegistry &InputContractRegistry,
    const FProviderRegistry &ProviderRegistry,
    const UeAgentAuthoringWorkspaceTransaction::FExecutionOptions
        &ExecutionOptions,
    const FBackendOptions &BackendOptions, FApplyWorkspaceResult &OutResult);
} // namespace UeAgentAuthoringOperationApply
