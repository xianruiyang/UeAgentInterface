#pragma once

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"

namespace UE::UAI::Composition::HjsonImport
{
	/** Explicit current coverage. Units outside this set remain source material,
	 * not observed current state. Structural edits require the complete path. */
	struct FScopedCurrentCoverage
	{
		TSet<FString> ValueUnits;
		TSet<FString> BindingUnits;
		struct FKeyedValueScope
		{
			FString RootField;
			TSet<FString> ObservedKeys;
		};
		// A domain can observe only the keys addressed by a declared keyed
		// operation. Other keys retain the accepted baseline, not currentness.
		TMap<FString, FKeyedValueScope> KeyedValueScopes;
	};
	/** Replay authored edits against a current capture, retaining its unedited state.
	 * Conflicts are diagnosed before any native mutation; files are never rewritten here. */
	UEAGENTINTERFACE_API bool TryRebaseSemanticWorkspace(
		const FResolvedSemanticWorkspaceRef& Source, const FAuthoringModelRef& Current,
		const StateSemantic::FUeAgentStateRevision& Revision, FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspaceRef& OutWorkspace, FSemanticWorkspaceImportError& OutError,
		const FScopedCurrentCoverage* Coverage = nullptr);
	/** Publication three-way merge. Unchanged accepted leaves take actual
	 * native results; newer author leaves remain pending author input. */
	bool TryMergeAcceptedAuthoringValues(const FResolvedSemanticWorkspaceRef& Latest,
		const FAuthoringModelRef& ActualModel, const HjsonWorkspace::FSemanticUnit& Accepted,
		const HjsonWorkspace::FSemanticUnit& Actual, const HjsonWorkspace::FSemanticUnit& Authored,
		FUeAgentResourceTracker& Resources, TOptional<HjsonWorkspace::FSemanticUnit>& Out,
		HjsonWorkspace::FWorkspaceError& Error);
}
