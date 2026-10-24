#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

namespace UE::UAI::HjsonWorkspace
{
	/** Public diagnostic for a failure after the asset transaction committed. */
	UEAGENTINTERFACE_API Contract::FDiagnostic MakeAppliedWorkspaceFailure(
		const FString& Code, const FWorkspaceError& Cause);
	/** Called only after successful asset apply/readback. Failure does not undo
	 * the asset operation; the caller must report that boundary explicitly. */
	UEAGENTINTERFACE_API bool TryRefreshImportedWorkspace(
		const FString& WorkspaceRoot,
		const TArray<FString>& ManagedSourcePaths,
		TFunctionRef<bool(const FString&, FWorkspaceError&)> ExportCurrent,
		ContextResource::FUeAgentResourceTracker& Resources,
		FWorkspaceError& OutError, TArray<Contract::FDiagnostic>& OutWarnings);
}
