#pragma once

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"

namespace UE::UAI::Composition::HjsonImport
{
	/** Replay authored edits against a current capture, retaining its unedited state.
	 * Conflicts are diagnosed before any native mutation; files are never rewritten here. */
	UEAGENTINTERFACE_API bool TryRebaseSemanticWorkspace(
		const FResolvedSemanticWorkspaceRef& Source, const FAuthoringModelRef& Current,
		const StateSemantic::FUeAgentStateRevision& Revision, FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspaceRef& OutWorkspace, FSemanticWorkspaceImportError& OutError);
}
