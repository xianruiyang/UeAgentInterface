#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"

namespace UE::UAI::HjsonWorkspace
{
	UEAGENTINTERFACE_API bool TryResolveSemanticWorkspace(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		ContextResource::FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError);

	/**
	 * Resolves an authoring workspace after LLM-facing managed units were edited.
	 * The machine manifest remains the immutable inventory/base binding; changed
	 * unit hashes and the resulting semantic fingerprint are recomputed from the
	 * edited units instead of requiring the caller to edit machine-owned hashes.
	 */
	UEAGENTINTERFACE_API bool TryResolveEditedSemanticWorkspace(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		ContextResource::FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError);
}
