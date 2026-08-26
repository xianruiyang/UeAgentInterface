// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "UObject/WeakObjectPtr.h"

class FScopedTransaction;
class UObject;
class UPackage;

#define UE_AGENT_AUTHORING_OPERATION_DURABLE_PACKAGE_CONTRACT_VERSION 1

namespace UeAgentAuthoringOperationApply {
using FOperationFailure =
    UeAgentAuthoringWorkspaceTransaction::FOperationFailure;

enum class EPackageDurableIntent : uint8 {
  Save,
  Delete,
};

struct UEAGENTINTERFACE_API FPackageDurablePlan {
  FString PlanId;
  FString PackageName;
  TWeakObjectPtr<UPackage> Package;
  TWeakObjectPtr<UObject> TopLevelAsset;
  EPackageDurableIntent Intent = EPackageDurableIntent::Save;
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
    bool bLifecycleStageable = true);

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
  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const {
    return nullptr;
  }
  virtual bool Commit(FOperationFailure &OutFailure) {
    OutFailure = FOperationFailure();
    return true;
  }
  virtual bool Rollback(FOperationFailure &OutFailure) = 0;
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
                        ERegistrationResult &OutResult, FString &OutError);
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
  bool bExpectedCaptured = false;
  bool bLifecycleApplied = false;

  friend class FOperationTransactionBackend;
};

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

struct UEAGENTINTERFACE_API FBackendOptions {
  bool bUseEditorTransaction = true;
  FText TransactionDescription;
  const UeAgentAuthoringSemanticRegistry::FRegistry *SemanticRegistry = nullptr;
  const UeAgentAuthoringState::FStateAccessorRegistry *StateAccessorRegistry =
      nullptr;
  const UeAgentAuthoringSemanticCompare::FComparatorRegistry
      *ComparatorRegistry = nullptr;
  TSharedPtr<IDurablePackageStager> PackageStager;
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
  virtual bool Verify(FOperationFailure &OutFailure) override;
  virtual bool RollbackUnit(
      UeAgentAuthoringWorkspaceTransaction::IPreparedMutation &Mutation,
      FOperationFailure &OutFailure) override;
  virtual bool EndWorkspace(bool bCommit,
                            FOperationFailure &OutFailure) override;
  virtual void FinalizeDurableCommit(bool bCommitted) override;

private:
  struct FPackageDirtySnapshot {
    TWeakObjectPtr<UObject> Package;
    bool bWasDirty = false;
  };

  FPreparedOperationMutation *ResolveMutation(
      UeAgentAuthoringWorkspaceTransaction::IPreparedMutation &Mutation,
      FOperationFailure &OutFailure) const;
  void CapturePackageDirtyStates(const IPreparedOperationState &State);
  void RestorePackageDirtyStates();
  bool
  CaptureRequests(const TArray<UeAgentAuthoringState::FReadRequest> &Requests,
                  UeAgentAuthoringState::FStateSnapshot &OutSnapshot,
                  FOperationFailure &OutFailure) const;

  FBackendOptions Options;
  TUniquePtr<FScopedTransaction> EditorTransaction;
  TArray<FPreparedOperationMutation *> AppliedMutations;
  TArray<FPackageDirtySnapshot> PackageDirtySnapshots;
  TArray<FPackageDurablePlan> DurablePackagePlans;
  bool bWorkspaceOpen = false;
  bool bAwaitingDurableFinalization = false;
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
