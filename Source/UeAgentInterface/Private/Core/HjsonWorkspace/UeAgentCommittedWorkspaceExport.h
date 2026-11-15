#pragma once

#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/HjsonWorkspace/UeAgentWorkspacePublication.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableTransaction.h"

namespace UE::UAI::HjsonWorkspace
{
    struct FCommittedWorkspaceDelta;
	/** Completed receipt names at the actual native capture boundary. The
	 * selected stream still validates each current record/phase/body itself. */
	struct FCommittedExportFence { TSet<FString> CompletedRequestKeys; };
    /** Reconstruct the exact accepted inventory baseline, excluding authored
     * additions. Missing captured units are a diagnostic, never guessed. */
    bool TryBuildCommittedExportBaseline(const FResolvedSemanticWorkspace& Workspace,
        ContextResource::FUeAgentResourceTracker& Resources, FAuthoringModelPtr& OutModel,
        FWorkspaceError& Error);

    struct FCommittedExportSelection
    {
        FString TargetKind;
        FString Target;
        FString PublisherOwnerId;
        FAuthoringModelPtr Model;
		FAuthoringModelPtr PublishedRows;
        TOptional<FCompositionSnapshotBinding> Binding;
        FWorkspacePublication SourcePublication;
        int64 SourceThroughSequence = 0;
        TSharedPtr<const FCommittedWorkspaceDelta> ActualChanges;
        int64 ThroughSequence = 0;
        int32 AppliedRecords = 0;
        // Present only when the current accepted Source can replace an old
        // unknown publication fact. Empty is proved empty, not a default.
        TOptional<TSet<FString>> ConsumerStateUpgradeDocuments;
        TArray<FString> ObservationScopes;
    };

    /** Reads only committed transaction evidence. Does not query native state
     * or use authored intent as a substitute for actual transaction output. */
    bool TrySelectCommittedWorkspaceExport(const FString& Root, const FString& Target,
        FAuthoringModelRef Baseline, const FString& PublishedSemanticFingerprint, ContextResource::FUeAgentResourceTracker& Resources,
        FCommittedExportSelection& Out, FWorkspaceError& Error);
    bool TryPrepareCommittedWorkspacePlan(const FCommittedExportSelection& Selection, const FString& Destination,
        FPreparedWorkspaceExport& Prepared, ContextResource::FUeAgentResourceTracker& Resources, FWorkspaceError& Error);

    /** Full/current incremental exports establish an explicit synchronization
     * point. An old default export may bootstrap only after exact model proof. */
    bool TryMakeCommittedExportState(const FString& TargetKind, const FString& Target,
        const FAuthoringModel& Model, const FCompositionSnapshotBinding& Binding,
        const FCommittedExportFence& CaptureFence, FString& OutState, FWorkspaceError& Error,
        const FWorkspacePublication* Publication = nullptr);
    /** Optional publication fact, not a cache or a consumption operation.
     * False means the old/mismatched state cannot prove which public documents
     * still contain newer author input. Callers then parse those documents. */
    bool TryReadPublishedPendingAuthorDocuments(const FString& State,
        const FWorkspacePublication& Publication, TSet<FString>& OutDocuments);
    /** Taken before native capture; later commits cannot be consumed by it. */
    bool TryCaptureCommittedExportFence(TSharedPtr<const FCommittedExportFence>& OutFence, FWorkspaceError& Error);
    bool TryPrepareCommittedAuthoringWriteback(const FString& Root, const FResolvedSemanticWorkspaceRef& Source,
        FWorkspaceError& Error);
    bool TryFindAppliedCommittedSource(const FString& Root, const FResolvedSemanticWorkspace& Source,
        bool& bApplied, FWorkspaceError& Error);
    bool TryResolveUnconsumedAuthoring(const FString& Root, const FResolvedSemanticWorkspaceRef& Source,
        ContextResource::FUeAgentResourceTracker& Resources, FResolvedSemanticWorkspaceRef& Out,
        FWorkspaceError& Error, TMap<FString, FSemanticUnit>* OutAcceptedDesired = nullptr);
    void AttachAcceptedActualUnits(const FResolvedSemanticWorkspace* AcceptedSource,
        const FAuthoringModel& Actual, TSharedPtr<const FCommittedWorkspaceDelta>& Delta);

    bool HasCommittedExportConsumer(const FString& TargetKind);
    bool TryPrepareCommittedImportState(const FString& Root, const FString& TargetKind, const FString& Target,
        const FAuthoringModel& Before, const FCompositionSnapshotBinding& Binding,
        ContextResource::FUeAgentResourceTracker& Resources, FString& OutBootstrapState, FWorkspaceError& Error);

    /** Request-local adapter for the existing durable transaction. Native
     * ownership and actual scope observation are supplied by the domain. */
    class FCommittedWorkspaceTransaction final
    {
    public:
        using FCaptureAfter = TFunction<bool(FAuthoringModelPtr&, TOptional<FCompositionSnapshotBinding>&,
            FString&, FWorkspaceError&)>;
        FCommittedWorkspaceTransaction(const FString& TargetKind, const FString& Target, const FString& Root,
            FString AcceptedSourceFingerprint, FString BootstrapState, FAuthoringModelRef Before, FCaptureAfter CaptureAfter,
            FResolvedSemanticWorkspacePtr AcceptedSource = {});
        /** Common native coordinator supplies its existing actual before/readback pair. */
        FCommittedWorkspaceTransaction(const FString& TargetKind, const FString& Target, const FString& Root,
            FString AcceptedSourceFingerprint, FAuthoringModelRef SourceBindings, FResolvedSemanticWorkspacePtr AcceptedSource = {});
        using FCaptureDelta = TFunction<bool(TSharedPtr<FJsonObject>&, TSharedPtr<const FCommittedWorkspaceDelta>&, TOptional<FCompositionSnapshotBinding>&,
            FString&, FWorkspaceError&)>;
        /** The domain retains complete native evidence; the shared delta owner
         * converts only its proved changed closure, or falls back completely. */
        FCommittedWorkspaceTransaction(const FString& TargetKind, const FString& Target, const FString& Root,
            FString AcceptedSourceFingerprint, FString BootstrapState, FAuthoringModelRef PublishedBaseline,
            const FCompositionSnapshotBinding& BeforeObservation, FCaptureDelta CaptureDelta, FResolvedSemanticWorkspacePtr AcceptedSource = {});
        bool IsReady(FWorkspaceError& Error) const;
        ~FCommittedWorkspaceTransaction();
        FCommittedWorkspaceTransaction(const FCommittedWorkspaceTransaction&) = delete;
        FCommittedWorkspaceTransaction& operator=(const FCommittedWorkspaceTransaction&) = delete;
    private:
        void Initialize(const FString& TargetKind, const FString& Root, FString AcceptedSourceFingerprint,
            FString BootstrapState, FAuthoringModelRef StreamModel,
            const FCompositionSnapshotBinding* BeforeObservation, FCaptureDelta CaptureDelta,
            UeAgentAuthoringWorkspaceTransaction::FScopedDurableConsumer::FObserve Observe = {},
            FResolvedSemanticWorkspacePtr AcceptedSource = {});
        FWorkspaceError PreparationError;
        TUniquePtr<UeAgentAuthoringWorkspaceTransaction::FScopedDurableConsumer> DurableScope;
    };
}
