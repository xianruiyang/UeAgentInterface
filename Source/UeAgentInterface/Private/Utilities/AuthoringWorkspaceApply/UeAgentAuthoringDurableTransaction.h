// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UeAgentAuthoringWorkspaceTransaction {

// Synchronous request-local observation, shared by all domain adapters. Contains
// only recovery semantics; journal identities remain inside the durable owner.
class UEAGENTINTERFACE_API FScopedDurableRecoveryNotice {
public:
  FScopedDurableRecoveryNotice();
  ~FScopedDurableRecoveryNotice();
  FScopedDurableRecoveryNotice(const FScopedDurableRecoveryNotice&) = delete;
  FScopedDurableRecoveryNotice& operator=(const FScopedDurableRecoveryNotice&) = delete;
  bool HasPreservedMemoryEvidence() const { return bMemoryEvidencePreserved; }
private:
  friend class FDurableTransactionCoordinator;
  FScopedDurableRecoveryNotice* Previous = nullptr;
  bool bMemoryEvidencePreserved = false;
};

UEAGENTINTERFACE_API FString BuildDurableRequestFingerprint(
    const UeAgentApplyWorkspaceContract::FContract &Contract,
    const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &Graph,
    const FPreflightResult &Prepared);

class UEAGENTINTERFACE_API FDurableTransactionCoordinator {
public:
  using FStageSource = TFunctionRef<bool(TArray<FDurableArtifact>&, FOperationFailure&)>;
  FDurableTransactionCoordinator();
  ~FDurableTransactionCoordinator();

  FDurableTransactionCoordinator(const FDurableTransactionCoordinator &) =
      delete;
  FDurableTransactionCoordinator &
  operator=(const FDurableTransactionCoordinator &) = delete;

  bool RecoverPending(const FExecutionOptions &Options,
                      FDurabilityResult &InOutResult,
                      FOperationFailure &OutFailure);
  bool TryReplay(const FExecutionOptions &Options,
                 const FString &RequestFingerprint, bool &OutReplayed,
                 FDurabilityResult &InOutResult,
                 FOperationFailure &OutFailure);
  bool Prepare(const FExecutionOptions &Options,
               const FString &RequestFingerprint,
               ITransactionBackend &Backend,
               const TArray<IPreparedMutation *> &Mutations,
               FDurabilityResult &InOutResult,
               FOperationFailure &OutFailure);
  // Native owners which must publish through an editor lifecycle (e.g. SaveMap)
  // use the same before-images, journal and recovery as staged workspace saves.
  bool Prepare(const FExecutionOptions &Options,
               const FString &RequestFingerprint,
               TArray<FDurableArtifact> Artifacts,
               FDurabilityResult &InOutResult,
               FOperationFailure &OutFailure);
  // Records durable output after a native save; independent semantic readback
  // must still succeed before Commit. Failure keeps the before-images available.
  bool RecordNativePublication(FDurabilityResult &InOutResult,
                               FOperationFailure &OutFailure);
  bool Stage(ITransactionBackend &Backend,
             FDurabilityResult &InOutResult,
             FOperationFailure &OutFailure);
  bool Stage(FStageSource Source, FDurabilityResult &InOutResult,
             FOperationFailure &OutFailure);
  bool RefreshInventory(ITransactionBackend& Backend, FDurabilityResult& InOutResult,
                        FOperationFailure& OutFailure);
  bool Publish(FDurabilityResult &InOutResult,
               FOperationFailure &OutFailure);
  bool CaptureMemoryCheckpoint(ITransactionBackend &Backend,
                               FDurabilityResult &InOutResult,
                               FOperationFailure &OutFailure);
  bool CaptureMemoryCheckpoint(FStageSource Source, FDurabilityResult &InOutResult,
                               FOperationFailure &OutFailure);
  bool PublishMemoryCheckpoint(FDurabilityResult &InOutResult,
                               FOperationFailure &OutFailure);
  bool RestoreDiskArtifacts(FDurabilityResult &InOutResult,
                            FOperationFailure &OutFailure);
  bool FinalizeRestore(bool bMemoryRollbackConfirmed,
                       FDurabilityResult &InOutResult,
                       FOperationFailure &OutFailure);
  bool Restore(bool bMemoryRollbackConfirmed,
               FDurabilityResult &InOutResult,
               FOperationFailure &OutFailure);
  bool Commit(FDurabilityResult &InOutResult,
              FOperationFailure &OutFailure);
  /** Finish an unsaved edit: retire its checkpoint without publishing asset files
   * or creating a durable replay acknowledgement for volatile memory state. */
  bool CompleteMemoryOnly(FDurabilityResult &InOutResult,
                          FOperationFailure &OutFailure);

  bool IsPrepared() const;

private:
  bool EnlistArtifacts(TArray<FDurableArtifact> Artifacts, bool bMemoryBeforeFromDisk,
                       FOperationFailure& OutFailure);
  struct FImpl;
  TUniquePtr<FImpl> Impl;
};

} // namespace UeAgentAuthoringWorkspaceTransaction
