// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UeAgentAuthoringWorkspaceTransaction {

UEAGENTINTERFACE_API FString BuildDurableRequestFingerprint(
    const UeAgentApplyWorkspaceContract::FContract &Contract,
    const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &Graph,
    const FPreflightResult &Prepared);

class UEAGENTINTERFACE_API FDurableTransactionCoordinator {
public:
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
  bool Stage(ITransactionBackend &Backend,
             FDurabilityResult &InOutResult,
             FOperationFailure &OutFailure);
  bool Publish(FDurabilityResult &InOutResult,
               FOperationFailure &OutFailure);
  bool Restore(bool bMemoryRollbackConfirmed,
               FDurabilityResult &InOutResult,
               FOperationFailure &OutFailure);
  bool Commit(FDurabilityResult &InOutResult,
              FOperationFailure &OutFailure);

  bool IsPrepared() const;

private:
  struct FImpl;
  TUniquePtr<FImpl> Impl;
};

} // namespace UeAgentAuthoringWorkspaceTransaction

