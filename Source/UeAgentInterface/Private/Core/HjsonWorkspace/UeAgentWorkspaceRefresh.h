#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentWorkspacePublication.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"

namespace UE::UAI::ArtifactPublisher { class FArtifactPublishRequest; class IArtifactPublishVerification; }

namespace UE::UAI::HjsonWorkspace
{
	struct FCommittedWorkspaceDelta;
	struct FCommittedExportFence;
	/** Internal request-local publication settings and prepared-plan output.
	 * Never serialized into public parameters or receipts. bPrepareOnly selects
	 * preparation versus immediate publication; both retain the chosen schema. */
	struct FPreparedWorkspaceExport
	{
		FExportWorkspacePlanPtr Plan;
		FString PublisherOwnerId;
		FWorkspacePublication Publication;
		bool bPrepareOnly = true;
		// Explicit request classification from the publication entry, never inferred
		// from missing optional Source/layout material in a downstream consumer.
		bool bInitializePublication = false;
		// Rich C++ result callers retain details; the public Level command consumes
		// only status/diagnostics/publisher warnings and selects false explicitly.
		bool bIncludeResultFileDetails = true;
		FHjsonOperationCheckpoint SourceCheckpoint;
		TArray<FString> ManagedSourcePaths;
		// Existing accepted destination, distinct from a fresh staging directory.
		FString AcceptedWorkspaceRoot;
		// Private consumer watermark, atomically published in the existing owner marker.
		FString ConsumerState;
		// Actual cumulative difference is bound to the destination publication,
		// not an owning previous Model or a last-cache pointer.
		FWorkspacePublication SourcePublication;
		// Actual accepted publisher, bound to SourcePublication. Model producers
		// do not choose ownership for an existing destination.
		FString SourcePublisherOwnerId;
		FAuthoringModelPtr PublishedRows;
		int64 SourceThroughSequence = 0;
		TSharedPtr<const FCommittedWorkspaceDelta> ActualChanges;
		// Newer author input is distinct from actual UE/publication state.
		// Only these K public value patches survive the committed writeback.
		TMap<FString, FSemanticUnit> PendingAuthorUnits;
		TMap<FString, TMap<FString, TMap<FString, StateSemantic::FUeAgentCanonicalValueRef>>> PendingKeyedAuthorRows;
		TMap<FString, TPair<FString, FString>> PendingAuthorLocations;
		// Capture-time synchronization fence. Selected committed output already
		// supplies ConsumerState and does not recapture or advance this fence.
		TSharedPtr<const FCommittedExportFence> CommittedCaptureFence;
		FString CommittedTarget;
		bool bCommittedRequested = false;
		FString ActualExportMode;
		TSharedPtr<ArtifactPublisher::IArtifactPublishVerification> Verification;
	};
	/** One forwarding owner for both direct and verified producers. */
	void ConfigureWorkspacePublication(const FPreparedWorkspaceExport* Prepared,
		ArtifactPublisher::FArtifactPublishRequest& Request);
	/** Resolve the complete existing workspace without importing or requiring its
	 * native revision to remain current. Pending authored edits are a conflict. */
	UEAGENTINTERFACE_API bool TryPrepareIncrementalWorkspaceExport(
		const FString& WorkspaceRoot, const FString& Target, const FString& ProfileId,
		ContextResource::FUeAgentResourceTracker& Resources, TArray<FString>& OutManagedPaths,
		FHjsonOperationCheckpoint& OutSourceCheckpoint, FWorkspaceError& OutError, bool bAllowProfileChange = false,
		bool bAllowAppliedCommittedInput = false);
	/** Shared update/publish owner for explicit incremental export and import refresh. */
	UEAGENTINTERFACE_API bool TryRefreshWorkspace(
		const FString& WorkspaceRoot, const TArray<FString>& ManagedSourcePaths,
		TFunctionRef<bool(const FString&, FPreparedWorkspaceExport&, FWorkspaceError&)> ExportCurrent,
		ContextResource::FUeAgentResourceTracker& Resources, FWorkspaceError& OutError,
		TArray<Contract::FDiagnostic>& OutWarnings, FHjsonOperationCheckpoint SourceCheckpoint = {});
	/** Public diagnostic for a failure after the asset transaction committed. */
	UEAGENTINTERFACE_API Contract::FDiagnostic MakeAppliedWorkspaceFailure(
		const FString& Code, const FWorkspaceError& Cause);
	/** Called only after successful asset apply/readback. Failure does not undo
	 * the asset operation; the caller must report that boundary explicitly. */
	UEAGENTINTERFACE_API bool TryRefreshImportedWorkspace(
		const FString& WorkspaceRoot,
		const TArray<FString>& ManagedSourcePaths,
		TFunctionRef<bool(const FString&, FPreparedWorkspaceExport&, FWorkspaceError&)> ExportCurrent,
		ContextResource::FUeAgentResourceTracker& Resources,
		FWorkspaceError& OutError, TArray<Contract::FDiagnostic>& OutWarnings);
	/** Compatibility path for exporters that still publish their fresh workspace. */
	UEAGENTINTERFACE_API bool TryRefreshImportedWorkspace(
		const FString& WorkspaceRoot,
		const TArray<FString>& ManagedSourcePaths,
		TFunctionRef<bool(const FString&, FWorkspaceError&)> ExportCurrent,
		ContextResource::FUeAgentResourceTracker& Resources,
		FWorkspaceError& OutError, TArray<Contract::FDiagnostic>& OutWarnings);
}
