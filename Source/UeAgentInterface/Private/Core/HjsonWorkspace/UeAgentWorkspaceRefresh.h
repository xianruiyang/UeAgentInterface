#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

namespace UE::UAI::HjsonWorkspace
{
	/** Internal, request-local output. Never serialized into command parameters
	 * or receipts. A provider may leave it empty to retain published-export refresh. */
	struct FPreparedWorkspaceExport
	{
		FExportWorkspacePlanPtr Plan;
		FString PublisherOwnerId;
	};
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
