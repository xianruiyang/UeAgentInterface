// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UeAgentAuthoringState { class FStateSnapshot; }

namespace UeAgentAuthoringWorkspaceTransaction {

// Existing operation recovery and AssetDurable maintenance share custody of
// authoring journals. Identity is issued by this owner, never a caller path.
FString GetDurableTransactionRoot(bool bWorldImport = false);
FString MakeDurableJournalRecoveryId(const FString& JournalPath);
bool IsAuthoringDurableJournalId(const FString& JournalId);
bool TryRecoverDurableRequest(const FString& RequestId, TSharedPtr<FJsonObject>& OutResult,
    UE::UAI::Contract::FDiagnostic& OutError);
void DiscoverAuthoringJournals(TArray<FString>& OutJournalIds,
    TArray<UE::UAI::Contract::FDiagnostic>& OutWarnings);
bool TryInspectAuthoringJournal(const FString& JournalId, TSharedPtr<FJsonObject>& OutResult,
    UE::UAI::Contract::FDiagnostic& OutError);
bool TryReconcileAuthoringJournal(const FString& JournalId, const FString& ExpectedOwner,
    int64 ExpectedFence, TSharedPtr<FJsonObject>& OutResult, UE::UAI::Contract::FDiagnostic& OutError);

// A consumer may seal pure recovery evidence inside the existing native
// transaction. The durable owner decides when it becomes committed; evidence
// never substitutes for artifact verification or request replay validation.
// Optional pure values owned by the consumer which sealed the durable body.
// This request-local handoff is never persisted or used instead of journal
// phase/byte validation. It cannot contain native transaction bindings.
struct IDurableConsumerMaterial {
  virtual ~IDurableConsumerMaterial() = default;
  virtual const TCHAR* GetMaterialType() const = 0;
};

struct FDurableConsumerRecord {
  FString RequestKey;
  FString Phase;
  FString OwnerId;
  FString StreamId;
  int64 Sequence = 0;
  FString IntentJson;
  FString VerifiedJson;
  TSharedPtr<const IDurableConsumerMaterial> Material;
  bool bCurrentRequestCommit = false; // Runtime origin; never persisted or inferred from phase alone.
};

struct FDurableConsumerHistoryState;
/** One bounded parsed-material candidate, moved into each request (never a
 * second full copy). Every consumer enumerates current journal names.
 * Active journals are always read. Completed receipts may reuse exact bytes
 * only under bounded same-request path guards and a current kernel read lease;
 * unsupported proof and budget misses perform the original real read. Parsed
 * data can survive the request only after all native proofs/guards are dropped,
 * and is reused only after exact comparison with freshly read source text. */
class FScopedDurableConsumerHistory final {
public:
  FScopedDurableConsumerHistory();
  ~FScopedDurableConsumerHistory();
  FScopedDurableConsumerHistory(const FScopedDurableConsumerHistory&) = delete;
  FScopedDurableConsumerHistory& operator=(const FScopedDurableConsumerHistory&) = delete;
private:
  TUniquePtr<FDurableConsumerHistoryState> State;
};

/** The consumer has sealed its selected records into an export plan/state.
 * Release optional request-local proofs/guards before publication takes its own
 * complete read set. A later consumer performs fresh reads; pure parsed text is
 * still bounded by the original shared 64 MiB material budget. */
void ReleaseDurableConsumerHistoryReads();

enum class EDurableConsumerBody : uint8 { Metadata, Intent, Complete };

/** Existing completed receipt identities visible before a native full capture.
 * Names only, request-local; no journal body, global sequence or new store. */
void CaptureDurableCompletedRequestKeys(const FString& JournalRoot, TSet<FString>& OutRequestKeys);

UEAGENTINTERFACE_API bool ReadDurableConsumerRecords(
    const FString& JournalRoot, const FString& OwnerId, const FString& StreamId,
    TArray<FDurableConsumerRecord>& OutRecords, FOperationFailure& OutFailure);
UEAGENTINTERFACE_API bool ReadDurableConsumerRecords(
    const FString& JournalRoot, const FString& OwnerId, const FString& StreamId,
    TArray<FDurableConsumerRecord>& OutRecords, FOperationFailure& OutFailure,
    EDurableConsumerBody Body, int64 BodyAfterSequence = 0);

class UEAGENTINTERFACE_API FScopedDurableConsumer final {
public:
  using FSeal = TFunction<bool(int64, FString&, TSharedPtr<const IDurableConsumerMaterial>&, FOperationFailure&)>;
  using FObserve = TFunction<void(const UeAgentAuthoringState::FStateSnapshot&, const UeAgentAuthoringState::FStateSnapshot&)>;
  FScopedDurableConsumer(FString OwnerId, FString StreamId, FString IntentJson, FSeal Seal, FObserve Observe = {});
  // Handoff of facts already captured by the native operation; no reads or native retention.
  static void ObserveActualState(const UeAgentAuthoringState::FStateSnapshot& Before,
      const UeAgentAuthoringState::FStateSnapshot& Actual);
  static bool HasActualStateConsumer();
  static bool IsObservationPending();
  static void MarkObservationPending();
  static TSharedPtr<const IDurableConsumerMaterial> FindCommittedMaterial(const FDurableConsumerRecord& Record,
      bool* OutCurrentRequestCommit = nullptr);
  ~FScopedDurableConsumer();
  FScopedDurableConsumer(const FScopedDurableConsumer&) = delete;
  FScopedDurableConsumer& operator=(const FScopedDurableConsumer&) = delete;
  int64 GetCommittedSequence() const { return CommittedSequence; }
private:
  friend class FDurableTransactionCoordinator;
  FScopedDurableConsumer* Previous = nullptr;
  FString OwnerId, StreamId, IntentJson;
  FSeal Seal;
  FObserve Observe;
  bool bObservationPending = false;
  int64 CommittedSequence = 0;
  TArray<FDurableConsumerRecord> CommittedRecords;
};

// Synchronous request-local observation, shared by all domain adapters. Contains
// only recovery semantics; journal identities remain inside the durable owner.
class UEAGENTINTERFACE_API FScopedDurableRecoveryNotice {
public:
  explicit FScopedDurableRecoveryNotice(FString InOriginRequestId = {});
  ~FScopedDurableRecoveryNotice();
  FScopedDurableRecoveryNotice(const FScopedDurableRecoveryNotice&) = delete;
  FScopedDurableRecoveryNotice& operator=(const FScopedDurableRecoveryNotice&) = delete;
  bool HasPreservedMemoryEvidence() const { return bMemoryEvidencePreserved; }
  const TArray<FString>& GetJournalIds() const { return JournalIds; }
  const FString& GetOriginRequestId() const { return OriginRequestId; }
  void RetainJournal(const FString& Id) { if (!Id.IsEmpty()) JournalIds.AddUnique(Id); }
private:
  friend class FDurableTransactionCoordinator;
  FScopedDurableRecoveryNotice* Previous = nullptr;
  bool bMemoryEvidencePreserved = false;
  FString OriginRequestId;
  TArray<FString> JournalIds;
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
  // Called after independent verification and before the native transaction
  // ends. Failure follows the existing rollback path.
  bool SealConsumerEvidence(FOperationFailure& OutFailure);
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
