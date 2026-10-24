#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace UE::UAI::HjsonWorkspace
{
	struct FSemanticWorkspaceReadState;

	/** Shares immutable parses within one synchronous import. Files are still
	 * read on every pass; reuse requires equal source identity and text. */
	class UEAGENTINTERFACE_API FSemanticWorkspaceReadScope final
	{
	public:
		FSemanticWorkspaceReadScope();
		~FSemanticWorkspaceReadScope();
		FSemanticWorkspaceReadScope(const FSemanticWorkspaceReadScope&) = delete;
		FSemanticWorkspaceReadScope& operator=(const FSemanticWorkspaceReadScope&) = delete;
	private:
		TUniquePtr<FSemanticWorkspaceReadState> State;
	};

	/** Includes old inventory files and newly consumed content references, not
	 * unrelated files present in the directory. Uses the import parser itself. */
	UEAGENTINTERFACE_API bool TryCollectSemanticWorkspaceManagedPaths(
		const FString& WorkspaceRoot,
		ContextResource::FUeAgentResourceTracker& Resources,
		TArray<FString>& OutPaths,
		FWorkspaceError& OutError);
	/** Shared type/value interpretation for a workspace property or a native default template. */
	UEAGENTINTERFACE_API bool TryResolveKeyedPropertyValues(
		const FHjsonNode& Row, const FString& ExpectedType,
		const TArray<FString>& TemplateFields,
		ContextResource::FUeAgentResourceTracker& Resources,
		TMap<FString, StateSemantic::FUeAgentCanonicalValueRef>& OutValues,
		FWorkspaceError& OutError, const TMap<FString, FString>* PublicRefs = nullptr,
		FHjsonNodePtr* OutValueSources = nullptr);
	UEAGENTINTERFACE_API bool TryProjectHjsonCanonicalValue(
		const FHjsonNode& Root, ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutValue, FWorkspaceError& OutError,
		bool bInterpretSemanticValues = true);

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
	 * Editable resolution requires immutable per-unit manifest envelopes; legacy
	 * full-row workspaces without envelopes must be re-exported before editing.
	 */
	UEAGENTINTERFACE_API bool TryResolveEditedSemanticWorkspace(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		ContextResource::FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError);
}
