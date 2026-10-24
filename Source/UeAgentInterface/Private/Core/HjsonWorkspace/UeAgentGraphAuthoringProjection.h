#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

class FJsonObject;

namespace UE::UAI::HjsonWorkspace
{
	class FAuthoringModel;
	class FResolvedSemanticWorkspace;

	/** Native consumers distinguish captured relations from explicit edits without
	 * exposing operation bookkeeping in the editable graph document. */
	struct UEAGENTINTERFACE_API FGraphAuthoringConnectionContext
	{
		TSet<FString> UnchangedConnections;
		bool bHasResolvedIntent = false;
		bool IsUnchanged(const FString& From, const FString& To) const;
		void RecordUnchanged(const FString& FromNode, const FString& FromPin,
			const FString& ToNode, const FString& ToPin);
		bool IsUnchanged(const FString& FromNode, const FString& FromPin,
			const FString& ToNode, const FString& ToPin) const;
	};

	/** Shared export/import classification; editable field omission cannot change the unit's projection. */
	UEAGENTINTERFACE_API bool IsGraphAuthoringStructureType(
		const FString& SemanticTypeRef, bool bHasCapturedNodesAndEdges = false);
	enum class EGraphAuthoringEndpointDirection : uint8
	{
		Input,
		Output,
	};

	struct UEAGENTINTERFACE_API FGraphAuthoringNodeAliasInput
	{
		/** Local lookup key; a semantic unit wrapper is not a native node identity. */
		FString SourceId;
		FString SemanticName;
		/** Stable captured node identity for duplicate-name ordering. Empty only
		 * when SourceId already is the native identity. */
		FString NativeIdentity;
	};

	struct UEAGENTINTERFACE_API FGraphAuthoringAliasProjection
	{
		TMap<FString, FString> AliasBySourceId;
		TMap<FString, FString> SourceIdByAlias;
	};

	struct UEAGENTINTERFACE_API FGraphAuthoringPinAliasInput
	{
		FString SourceId;
		FString SemanticName;
		int32 Occurrence = 0;
		EGraphAuthoringEndpointDirection Direction =
			EGraphAuthoringEndpointDirection::Input;
	};

	struct UEAGENTINTERFACE_API FGraphAuthoringPinAliasProjection
	{
		TMap<FString, FString> AliasBySourceId;
		TMap<FString, FString> SourceIdByAlias;
	};

	UEAGENTINTERFACE_API bool TryBuildGraphAuthoringAliasProjection(
		const TArray<FGraphAuthoringNodeAliasInput>& Nodes,
		FGraphAuthoringAliasProjection& OutProjection,
		FWorkspaceError& OutError);

	/** Read only native naming properties from the existing scalar/literal property representation. */
	const UE::UAI::StateSemantic::FUeAgentCanonicalValueRef* FindGraphNodeNativeNameProperty(
		const FString& PropertyName,
		const TMap<FString, UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>& Values);

	/** Existing node semantics and owned native properties supply names; neither changes native identity. */
	FString ReadGraphNodeSemanticName(
		const TMap<FString, UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>& Values,
		const TMap<FString, UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>* NativePropertyValues = nullptr);

	/** The native identity already carried by a captured graph node. */
	FString ReadGraphNodeNativeIdentity(
		const TMap<FString, UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>& Values);

	/** Shared node membership and names for graph references and physical navigation. */
	bool TryBuildGraphModelNodeAliases(const FAuthoringModel& Model, const FString& GraphId,
		FGraphAuthoringAliasProjection& OutProjection, FWorkspaceError& OutError);

	/** Read an existing alias from the captured graph's authoritative node rows. */
	UEAGENTINTERFACE_API bool TryResolveCapturedGraphNodeReference(
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Nodes,
		const FString& Reference,
		FString& OutNodeId,
		FWorkspaceError& OutError);

	/**
	 * Builds the readable, node-local Pin aliases used by both exported
	 * interface companions and post-reconstruct mutation adapters. SourceId is
	 * machine-owned and never enters the model-facing projection.
	 */
	UEAGENTINTERFACE_API bool TryBuildGraphAuthoringPinAliasProjection(
		const TArray<FGraphAuthoringPinAliasInput>& Pins,
		FGraphAuthoringPinAliasProjection& OutProjection,
		FWorkspaceError& OutError);

	/** Model-facing interface shared by workspace export and public graph queries. */
	UEAGENTINTERFACE_API bool TryProjectGraphAuthoringPinInterface(
		const TSharedRef<FJsonObject>& Captured,
		TSharedRef<FJsonObject>& OutInterface,
		FWorkspaceError& OutError,
		TMap<FString, FString>* OutAliasBySelector = nullptr);

	/** Project captured defaults through the same node-local input aliases as the interface. */
	UEAGENTINTERFACE_API TSharedRef<FJsonObject> ProjectGraphAuthoringPinDefaults(
		const TSharedRef<FJsonObject>& Captured,
		const TMap<FString, FString>& AliasBySelector);

	UEAGENTINTERFACE_API bool TryFormatGraphAuthoringEndpoint(
		const FString& NodeAlias,
		const FString& PinName,
		int32 PinOccurrence,
		EGraphAuthoringEndpointDirection Direction,
		FString& OutEndpoint,
		FWorkspaceError& OutError);

	/** Parses the shared model-facing NodeAlias.PinAlias endpoint syntax. */
	UEAGENTINTERFACE_API bool TryParseGraphAuthoringEndpoint(
		const FString& Endpoint,
		FString& OutNodeAlias,
		FString& OutPinAlias,
		FWorkspaceError& OutError);

	/** Graph node_references declares named nullable slots: native IDs in capture, aliases in visible rows.
 * Missing slots preserve their captured references; empty strings clear them. Node deletion clears
 * unchanged nullable references. Newly authored dangling references and undeclared slots are errors.
	 */
	UEAGENTINTERFACE_API bool TryProjectGraphAuthoringVisibleRow(
		const FString& UnitId,
		const TSharedRef<FJsonObject>& CompactRow,
		TSharedRef<FJsonObject>& OutRow,
		TSharedPtr<FJsonObject>& OutInterfaceRow,
		FWorkspaceError& OutError,
		FGraphAuthoringConnectionContext* OutConnectionContext = nullptr);

	/**
	 * Projects a resolved graph semantic unit back to the shared readable graph
	 * document consumed by typed domain adapters. Removed nodes and edges are
	 * omitted, while machine identity remains outside the returned document.
	 */
	UEAGENTINTERFACE_API bool TryProjectGraphAuthoringConsumerDocument(
		const FString& UnitId,
		const TMap<FString,
			UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>& ResolvedValues,
		TSharedRef<FJsonObject>& OutDocument,
		FWorkspaceError& OutError,
		const FResolvedSemanticWorkspace* Workspace = nullptr,
		FGraphAuthoringConnectionContext* OutConnectionContext = nullptr);

	/** Operation consumers must not replay untouched node state after native reconstruction.
	 * Complete-document consumers keep using TryProjectGraphAuthoringConsumerDocument. */
	UEAGENTINTERFACE_API void ProjectGraphAuthoringPinDefaultChanges(
		const FJsonObject& CapturedNode, FJsonObject& DesiredNode);

	UEAGENTINTERFACE_API bool TryProjectGraphAuthoringNodeChanges(
		const FString& UnitId,
		const TMap<FString, UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>& CapturedValues,
		const TMap<FString, UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>& ResolvedValues,
		TSharedPtr<FJsonObject>& OutNodes, FWorkspaceError& OutError,
		bool bRetainContextNodes = false);

	/** Resolve native graph/node identities through the captured common graph row.
	 * A derived field has no authored leaf; return its node and incident connections
	 * as related sources, rather than inventing an input field location. */
	void FindGraphAuthoringDiagnosticSources(const FResolvedSemanticWorkspace& Workspace,
		const FString& DocumentPointer, const FSourceProvenance*& OutExactSource,
		TArray<const FSourceProvenance*>& OutRelatedSources, const FString& GraphUnitId = FString());

	/**
	 * Normalizes the supported authored graph-position aliases to the single
	 * common node field `pos: { x, y }`. Position objects and two-item arrays
	 * may contain integer or decimal coordinates. Domain adapters consume only
	 * the normalized field and do not reinterpret public aliases themselves.
	 */
	UEAGENTINTERFACE_API bool TryNormalizeGraphAuthoringNodePosition(
		TMap<FString,
			UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>& InOutFields,
		const FString& SourceUnit,
		UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryResolveGraphAuthoringProjection(
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& VisibleRoot,
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& CapturedRoot,
		const FString& SourceUnit,
		UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
		TMap<FString, UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>&
			OutValues,
		FWorkspaceError& OutError,
		const FHjsonNode* SourceRoot = nullptr,
		FHjsonNodePtr* OutSourceRoot = nullptr);
}
