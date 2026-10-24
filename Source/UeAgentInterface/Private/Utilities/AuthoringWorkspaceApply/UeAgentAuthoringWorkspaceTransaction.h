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
  UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
};

// Evidence of this operation's completed stages, not a source asset's stored
// compiler freshness. Only ExecuteAtomic publishes it during synchronous final
// readback; nested operations restore the enclosing scope on return.
struct UEAGENTINTERFACE_API FFinalReadbackEvidence {
  bool bMutationPhaseCompleted = false;
  bool bNativeUpdateRequested = false;
  bool bNativeUpdateCompleted = false;
  bool bSaveCompleted = false;

  TSharedRef<FJsonObject> ToJsonObject() const;
  static bool TryFromJsonObject(const FJsonObject& Object, FFinalReadbackEvidence& OutEvidence,
      UE::UAI::Contract::FDiagnostic& OutError);
};

UEAGENTINTERFACE_API bool TryGetFinalReadbackEvidence(FFinalReadbackEvidence& OutEvidence);

enum class EDurableArtifactKind : uint8 { File, DirectoryTree };

struct UEAGENTINTERFACE_API FDurableArtifact {
  FString ArtifactId;
  FString FinalPath;
  FString StagingPath;
  bool bStagedExists = false;
  EDurableArtifactKind Kind = EDurableArtifactKind::File;
};

struct UEAGENTINTERFACE_API FDurabilityResult {
  FString JournalSchema;
  FString JournalPhase;
  FString JournalPath;
  FString RecoveryStatus;
  TArray<FString> PhaseHistory;
  TArray<UeAgentAuthoringDiagnostics::FDiagnostic> Warnings;
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
  // Native creation can allocate owned packages after preparation. They have
  // no captured dirty memory before this edit; their disk before-images still
  // enter this same journal before any publication.
  virtual bool CollectAdditionalDurableArtifacts(TArray<FDurableArtifact>& OutArtifacts,
      FOperationFailure& OutFailure) { OutArtifacts.Reset(); return true; }
  virtual bool StageMemoryCheckpoint(TArray<FDurableArtifact> &InOutArtifacts,
      FOperationFailure &OutFailure) {
    return StageDurableArtifacts(InOutArtifacts, OutFailure);
  }
		virtual bool FinalizeDurableCommit(bool bCommitted, FOperationFailure& OutFailure);
  // Persistent readback is part of this transaction, before either commit.
  virtual bool RequiresPersistentReadback() const { return false; }
  virtual bool RequiresMemoryCheckpoint() const { return RequiresPersistentReadback(); }
  virtual bool RequiresCheckpointRollback() const { return false; }
  virtual bool HasReleasedNativeState() const { return false; }
  virtual bool FinalizeVerification(FOperationFailure &OutFailure) { return true; }
  virtual bool PrepareCheckpointRestore(FOperationFailure &OutFailure) { return true; }
  virtual bool ReloadCheckpoint(FOperationFailure &OutFailure) { return true; }
  virtual bool VerifyRestoredState(FOperationFailure &OutFailure) { return true; }
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
  bool IsReady(UeAgentAuthoringDiagnostics::FDiagnostic* OutError) const;

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
