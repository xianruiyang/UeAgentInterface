// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceRouter.h"

class FJsonObject;

#define UE_AGENT_AUTHORING_DURABLE_TRANSACTION_CONTRACT_VERSION 1

namespace UeAgentAuthoringWorkspaceTransaction {
class IPreflightProvider;
struct FPreflightResult;

UEAGENTINTERFACE_API bool PreflightWorkspace(
    const UeAgentApplyWorkspaceContract::FContract &Contract,
    const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &Graph,
    const IPreflightProvider &Provider, FPreflightResult &OutResult);

struct UEAGENTINTERFACE_API FOperationFailure {
  FString ErrorCode;
  FString ErrorDetail;
  UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
};

struct UEAGENTINTERFACE_API FDurableArtifact {
  FString ArtifactId;
  FString FinalPath;
  FString StagingPath;
  bool bStagedExists = false;
};

struct UEAGENTINTERFACE_API FDurabilityResult {
  FString JournalSchema;
  FString JournalPhase;
  FString JournalPath;
  FString RecoveryStatus;
  TArray<FString> PhaseHistory;
  int32 ArtifactCount = 0;
  bool bReplayed = false;
  bool bRestoreAttempted = false;
  bool bRestoreSucceeded = false;

  bool IsPresent() const;
  TSharedRef<FJsonObject> ToJsonObject() const;
};

class UEAGENTINTERFACE_API IPreparedMutation {
public:
  virtual ~IPreparedMutation() = default;
  virtual FString GetBackendId() const { return FString(); }
  virtual FString GetUnitKey() const = 0;
  virtual FString GetTargetKey() const = 0;
  virtual bool IsNoOp() const { return false; }
  virtual bool IsStateNoOp() const { return IsNoOp(); }
  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const {
    return nullptr;
  }
};

class UEAGENTINTERFACE_API IPreflightProvider {
public:
  virtual ~IPreflightProvider() = default;
  virtual bool
  PreflightUnit(const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit,
                TUniquePtr<IPreparedMutation> &OutMutation,
                FOperationFailure &OutFailure) const = 0;
};

class UEAGENTINTERFACE_API ITransactionBackend {
public:
  virtual ~ITransactionBackend() = default;

  virtual bool BeginWorkspace(
      const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
      const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &Graph,
      FOperationFailure &OutFailure) = 0;
  virtual bool CaptureRollback(IPreparedMutation &Mutation,
                               FOperationFailure &OutFailure) = 0;
  virtual bool ApplyUnit(IPreparedMutation &Mutation,
                         FOperationFailure &OutFailure) = 0;
  virtual bool Compile(FOperationFailure &OutFailure) = 0;
  virtual bool Save(FOperationFailure &OutFailure) = 0;
  virtual bool Verify(FOperationFailure &OutFailure) = 0;
  virtual bool RollbackUnit(IPreparedMutation &Mutation,
                            FOperationFailure &OutFailure) = 0;
  virtual bool EndWorkspace(bool bCommit, FOperationFailure &OutFailure) = 0;

  virtual bool SupportsDurableTransactions() const;
  virtual bool BuildDurableArtifactInventory(
      const TArray<IPreparedMutation *> &Mutations,
      TArray<FDurableArtifact> &OutArtifacts,
      FOperationFailure &OutFailure);
  virtual bool StageDurableArtifacts(
      TArray<FDurableArtifact> &InOutArtifacts,
      FOperationFailure &OutFailure);
  virtual void FinalizeDurableCommit(bool bCommitted);
};

struct UEAGENTINTERFACE_API FPreparedUnit {
  int32 WorkspaceUnitIndex = INDEX_NONE;
  TUniquePtr<IPreparedMutation> Mutation;

  FPreparedUnit() = default;
  FPreparedUnit(FPreparedUnit &&) = default;
  FPreparedUnit &operator=(FPreparedUnit &&) = default;
  FPreparedUnit(const FPreparedUnit &) = delete;
  FPreparedUnit &operator=(const FPreparedUnit &) = delete;
};

struct UEAGENTINTERFACE_API FPreflightResult {
  FString Status;
  FString ErrorCode;
  FString ErrorDetail;
  FString CauseCode;
  FString ContractId;
  FString ContractVersion;
  UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
  TArray<FString> OrderedUnitKeys;
  TArray<FPreparedUnit> Units;
  int32 UnitsChecked = 0;

  FPreflightResult() = default;
  FPreflightResult(FPreflightResult &&) = default;
  FPreflightResult &operator=(FPreflightResult &&) = default;
  FPreflightResult(const FPreflightResult &) = delete;
  FPreflightResult &operator=(const FPreflightResult &) = delete;

  bool IsReady() const;

private:
  bool bValidated = false;

  friend UEAGENTINTERFACE_API bool PreflightWorkspace(
      const UeAgentApplyWorkspaceContract::FContract &Contract,
      const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
      const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &Graph,
      const IPreflightProvider &Provider, FPreflightResult &OutResult);
};

struct UEAGENTINTERFACE_API FExecutionOptions {
  bool bSaveWhenCallerControlled = false;
  FString RequestId;
  FString JournalRoot;
};

struct UEAGENTINTERFACE_API FExecutionResult {
  FString Status;
  FString Phase;
  FString ErrorCode;
  FString ErrorDetail;
  FString CauseCode;
  UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
  TArray<FString> AttemptedUnitKeys;
  TArray<FString> RolledBackUnitKeys;
  int32 MutationAttempts = 0;
  int32 MutationsApplied = 0;
  int32 RollbackAttempts = 0;
  bool bRollbackAttempted = false;
  bool bRollbackSucceeded = false;
  FDurabilityResult Durability;

  bool IsApplied() const;
  TSharedRef<FJsonObject> ToJsonObject() const;
};

UEAGENTINTERFACE_API bool ExecuteAtomic(
    const UeAgentApplyWorkspaceContract::FContract &Contract,
    const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &Graph,
    FPreflightResult &Prepared, ITransactionBackend &Backend,
    const FExecutionOptions &Options, FExecutionResult &OutResult);
} // namespace UeAgentAuthoringWorkspaceTransaction
