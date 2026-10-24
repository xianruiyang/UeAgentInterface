#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringExamples.h"

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/Function.h"

class FJsonObject;
class FJsonValue;

namespace UE::UAI::HjsonWorkspace
{
	struct FOwnedItemRemovalCandidate
	{
		FString ItemId;
		FString OwnerId;
	};
	// Only declared containment cascades. Callers compare the original item with
	// all explicit edits; changed survivors are left for native relationship validation.
	UEAGENTINTERFACE_API bool TryCollectOwnedItemRemovals(
		const TArray<FOwnedItemRemovalCandidate>& Candidates, const TArray<FString>& RemovedOwners,
		TFunctionRef<bool(const FString&, bool&)> IsUnchanged, TArray<FString>& OutRemovedItems);

	/** Merge partial projections of one captured item. Omitted fields preserve state;
	 * objects merge by field, arrays/scalars (including null) remain atomic.
 * A conflicting edit returns the leaf path and both authored values. */
	UEAGENTINTERFACE_API bool TryMergeKeyedItemStateEdits(
		const TSharedPtr<FJsonObject>& Captured,
		const TSharedPtr<FJsonObject>& Left,
		const TSharedPtr<FJsonObject>& Right,
		TSharedPtr<FJsonObject>& OutMerged, Contract::FDiagnostic& OutError);

	/** Binding metadata for an ordered owned item; never a business value or a global identity table. */
	UEAGENTINTERFACE_API bool TryGetOwnedItemBinding(
		const StateSemantic::FUeAgentCanonicalValue& Definition, FString& OutKey, FString& OutRelativePath,
		FWorkspaceError& OutError, int32 Occurrence = 0);

	struct FOrderedOwnedItemBinding
	{
		int32 CapturedIndex = INDEX_NONE;
		FString RelativePath;
		StateSemantic::FUeAgentCanonicalValuePtr Value;
	};

	UEAGENTINTERFACE_API bool TryResolveOrderedOwnedItems(
		const StateSemantic::FUeAgentCanonicalValue& ProposedItems,
		const StateSemantic::FUeAgentCanonicalValue& CapturedDefinitions,
		ContextResource::FUeAgentResourceTracker& Resources,
		TArray<FOrderedOwnedItemBinding>& OutItems, FWorkspaceError& OutError);

	struct FOrderedStructuredItemBinding
	{
		int32 CapturedIndex = INDEX_NONE;
		StateSemantic::FUeAgentCanonicalValuePtr Value;
	};

	/** Resolves readable local instance keys for ordinary struct-array items. */
	UEAGENTINTERFACE_API bool TryResolveOrderedStructuredItems(
		const StateSemantic::FUeAgentCanonicalValue& ProposedItems, int32 CapturedCount,
		ContextResource::FUeAgentResourceTracker& Resources,
		TArray<FOrderedStructuredItemBinding>& OutItems, FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryResolveOrderedStructuredItems(
		const StateSemantic::FUeAgentCanonicalValue& ProposedItems,
		const StateSemantic::FUeAgentCanonicalValue& CapturedDefinitions,
		ContextResource::FUeAgentResourceTracker& Resources,
		TArray<FOrderedStructuredItemBinding>& OutItems, FWorkspaceError& OutError);

	/** Removes only typed owned binding metadata from a public definition tree. */
	UEAGENTINTERFACE_API TSharedPtr<FJsonValue> ProjectPublicOwnedItemDefinition(const TSharedPtr<FJsonValue>& Value);

	struct UEAGENTINTERFACE_API FKeyedItemVisibleFieldPolicy
	{
		TSet<FString> VisibleTopLevelFields;
		/** Optional discriminated object: each branch lists its required public members. */
		FString DiscriminatorField;
		TMap<FString, TSet<FString>> BranchFields;
		/** A branch containing these alternatives requires exactly one, rather than every member. */
		TArray<TSet<FString>> ExclusiveBranchFields;
		bool IsValid() const { return !VisibleTopLevelFields.IsEmpty(); }
		bool Allows(const FString& Name) const
		{
			for (const FString& Field : VisibleTopLevelFields)
				if (Field.Equals(Name, ESearchCase::CaseSensitive)) return true;
			return false;
		}
	};

	struct UEAGENTINTERFACE_API FKeyedItemNamingPolicy
	{
		TArray<FString> Fields;
		FString FallbackName;
	};

	enum class EKeyedItemReferenceShape : uint8 { Single, Set };
	struct FKeyedItemReferenceField
	{
		// Captured references remain context even when their target disappears.
		// Native owners settle that context; only newly requested targets require resolution.
		FString TargetCollection; // Empty denotes this collection.
		EKeyedItemReferenceShape Shape = EKeyedItemReferenceShape::Single;
	};
	struct UEAGENTINTERFACE_API FKeyedItemProjectionContract
	{
		FString RootField;
		FKeyedItemVisibleFieldPolicy VisibleFieldPolicy;
		bool bIdentityFromMapKey = false;
		FKeyedItemNamingPolicy NamingPolicy;
		/** Child items represented exclusively by this collection's ordinary document. */
		FString AggregatedItemSemanticSuffix;
		FString AggregatedParentSemanticSuffix;
		TSet<FString> ReadOnlyRootFields;
		bool bMergeChildPropertyValues = true;
		/** Optional typed-record contract; independent of map-key identity. */
		FString RecordSchemaField;
		bool bMapKeyUsesNameIdentity = false;
		TSet<FString> EditableRootFields;
		/** Non-empty for captured arrays whose records carry their exact string identity. */
		FString CapturedRecordIdentityField;
		/** Literal records contain user data, not semantic tags or public references. */
		bool bInterpretSemanticValues = true;
		/** Optional readable map-key prefix for captured record arrays. */
		FString VisibleKeyPrefix;
		/** Explicit object fields use sparse member overlays and hide uncaptured public members. */
		TMap<FString, FKeyedItemVisibleFieldPolicy> ObjectFieldPolicies;
		/** Opt-in references within this collection; values are readable keys in ordinary input. */
		FString ReferenceIdentityField;
		TMap<FString, FKeyedItemReferenceField> ReferenceFields;
		bool bUseNewItemReferenceForIdentity = false;
		/** Nested collections share the same capture/desired-state owner. */
		TMap<FString, const FKeyedItemProjectionContract*> CollectionFieldContracts;
		/** Existing-only collections reject unknown visible keys before domain writes. */
		bool bAllowNewItems = true;
		/** Child collection defaults sourced from the captured parent record, never user-selected native types. */
		TMap<FString, TMap<FString, FString>> ChildRecordDefaultSources;
		// Parent-owned native record supplies type-dependent child defaults.
		TMap<FString, FString> ChildRecordDefaultObjectSources;
		/** Fixed native member collections retain all captured identities. */
		bool bAllowRemoveItems = true;
		/** False for records whose native kind is already fixed by their owning collection. */
		bool bRequiresCreationCandidate = true;
		/** Map-key records may opt into mutation instructions after complete desired-state resolution. */
		bool bEmitOperationRecords = false;
		/** Set-reconstruction consumers need unchanged captured rows as context,
		 * unlike consumers applying independent mutations to a live collection. */
		bool bRetainUnchangedOperationRecords = false;
		/** Unchanged context must not be replayed over native side effects. Never includes identity fields. */
		TSet<FString> DeltaOnlyFields;
		/** Optional editable fields omitted on export; absence restores their frozen captured values. */
		TSet<FString> ImplicitCapturedFields;
		/** Existing native sequence field for a flat collection; public navigation derives from it. */
		FString NativeOrderField;
		/** An authored rank is compacted into a native array position. Grouping
		 * belongs to the declared collection, not to arbitrary fields named index. */
		FString OrderRankField;
		TArray<FString> OrderRankGroupFields;
		TSet<FString> UnorderedKinds;
	/** Native identities derived from these semantic fields may change on update.
	 * Their aliases are resolved from the original request's normalized fields. */
	TMap<FString, TArray<FString>> DerivedIdentityFieldsByKind;
		/** Equal ranks follow the native record key when identity is separate from its readable alias. */
		FString OrderRankTieIdentityField;
		/** Optional reading recipes; not part of desired-state validation or instance identity. */
		TArray<FAuthoringExample> CreationExamples;
		/** Declared native constructor arguments can select a direct creation path
		 * when no candidate selector was authored. The native owner validates them. */
		TSet<FString> DirectCreationFields;
		bool IsReadOnlyRootField(const FString& Name) const
		{
			for (const FString& Field : ReadOnlyRootFields)
				if (Field.Equals(Name, ESearchCase::CaseSensitive)) return true;
			return false;
		}
		bool IsEditableRootField(const FString& Name) const
		{
			for (const FString& Field : EditableRootFields)
				if (Field.Equals(Name, ESearchCase::CaseSensitive)) return true;
			return false;
		}
	};

	/** Interpret operation rows and native aliases in the same collection layer.
	 * Creation identity is verified by its mutation owner; retained edits and
	 * removals remain checked independently against the captured native result.
	 * Optional native-to-authored aliases carry type/source metadata through
	 * exactly the same projection, rather than rebinding it by display name. */
	UEAGENTINTERFACE_API bool TryProjectKeyedItemOriginalIntent(
		const FKeyedItemProjectionContract& Contract,
		TSharedPtr<FJsonValue>& Before, TSharedPtr<FJsonValue>& Desired,
		TSharedPtr<FJsonValue>& Actual, TMap<FString, FString>& SourcePaths,
		UE::UAI::Contract::FDiagnostic& Error, const TSharedPtr<FJsonObject>& CreationArguments = nullptr,
		TMap<FString, FString>* OutNativeAliases = nullptr);

	/** Freeze constructor arguments in the existing import normalization scope. */
	UEAGENTINTERFACE_API bool TryRecordKeyedCreationArguments(const FString& RootField,
		const TSharedPtr<FJsonValue>& SourceRows, const TSharedPtr<FJsonObject>& NativeRows,
		const TMap<FString, FString>& NativeByNewKey, UE::UAI::Contract::FDiagnostic& Error);

	struct FKeyedItemReferenceCollection
	{
		const FKeyedItemProjectionContract* Contract = nullptr;
		const StateSemantic::FUeAgentCanonicalValue* CapturedItems = nullptr;
		const StateSemantic::FUeAgentCanonicalValue* DesiredItems = nullptr;
		bool bDesiredOperationRecords = false;
	};
	using FKeyedItemReferenceContext = TMap<FString, FKeyedItemReferenceCollection>;
	UEAGENTINTERFACE_API const FKeyedItemProjectionContract& GetLevelSequenceFolderProjectionContract();
	/** A reference to a newly created sibling is resolved after the domain allocates that sibling. */
	UEAGENTINTERFACE_API bool TryGetNewKeyedItemReference(
		const StateSemantic::FUeAgentCanonicalValue& Value, FString& OutKey);
	UEAGENTINTERFACE_API bool TryGetNewKeyedItemReference(const FJsonValue& Value, FString& OutKey);

	UEAGENTINTERFACE_API const TArray<FKeyedItemProjectionContract>&
		GetKeyedItemProjectionContracts();

	/** Returns the shared keyed-item projection contract for an ordinary root field. */
	UEAGENTINTERFACE_API bool TryGetKeyedItemProjectionContract(
		const FString& RootField,
		FKeyedItemProjectionContract& OutContract);

	/** Document-scoped keyed contract for Level Sequence scalar channel keys. */
	UEAGENTINTERFACE_API const FKeyedItemProjectionContract&
		GetLevelSequenceScalarKeyProjectionContract();
	UEAGENTINTERFACE_API const FKeyedItemProjectionContract&
		GetLevelSequenceScalarSectionProjectionContract();
	UEAGENTINTERFACE_API const FKeyedItemProjectionContract& GetLevelSequenceScalarChannelProjectionContract();
	UEAGENTINTERFACE_API const FKeyedItemProjectionContract&
		GetLevelSequenceRootScalarTrackProjectionContract();
	UEAGENTINTERFACE_API const FKeyedItemProjectionContract&
		GetLevelSequenceBindingScalarTrackProjectionContract();
	UEAGENTINTERFACE_API const FKeyedItemProjectionContract&
		GetLevelSequenceBindingProjectionContract(bool bIncludeLocalBindingTags = true);

	/** Shared ordinary-document policy for level actor/component hierarchy items. */
	UEAGENTINTERFACE_API const FKeyedItemVisibleFieldPolicy&
		GetActorComponentHierarchyItemVisibleFieldPolicy();

	/** Formats a readable stable key; only repeated semantic names get a suffix. */
	UEAGENTINTERFACE_API bool TryFormatReadableKeyedItemAlias(
		const FString& SemanticName,
		int32 Occurrence,
		FString& OutAlias,
		FWorkspaceError& OutError);

	/** Formats a one-based readable key for a captured ordered record. */
	UEAGENTINTERFACE_API bool TryFormatReadableKeyedItemOrdinal(
		const FString& Prefix, int32 CapturedIndex, FString& OutKey, FWorkspaceError& OutError);

	/** Allocates an alias unique across the caller's entire collection, including literal suffixes. */
	UEAGENTINTERFACE_API bool TryAllocateReadableKeyedItemAlias(
		const FString& SemanticName,
		TMap<FString, int32>& Occurrences,
		TSet<FString>& UsedAliases,
		FString& OutAlias,
		FWorkspaceError& OutError,
		const TSet<FString>* ReservedAliases = nullptr);

	/** Projects one captured item to its ordinary, human-readable value surface. */
	UEAGENTINTERFACE_API bool TryProjectItemVisibleObject(
		const FString& SourceUnit,
		const TSharedRef<FJsonObject>& CapturedItem,
		const FKeyedItemVisibleFieldPolicy& FieldPolicy,
		TSharedRef<FJsonObject>& OutVisibleItem,
		FWorkspaceError& OutError);

	/** Restores hidden captured fields, overlaying only approved ordinary fields. */
	UEAGENTINTERFACE_API bool TryResolveCapturedItemDesiredState(
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& VisibleItem,
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& CapturedItem,
		const FString& SourceUnit,
		const FKeyedItemVisibleFieldPolicy& FieldPolicy,
		UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr& OutResolvedItem,
		bool& bOutSemanticChanged,
		FWorkspaceError& OutError);

	/**
	 * Projects a machine-owned keyed item map to the ordinary model surface.
	 * Stable map keys remain readable handles; identity, revision, and operation
	 * material stays in the captured machine surface.
	 */
	enum class EKeyedItemProjectionInputKind : uint8
	{
		CapturedItems,
		/** Re-filter a map already named/projected by this owner; do not resolve identity again. */
		ProjectedMap,
	};
	UEAGENTINTERFACE_API bool TryProjectKeyedItemVisibleValue(
		const FString& SourceUnit,
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& CapturedItems,
		const FKeyedItemProjectionContract& Contract,
		UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr& OutVisibleItems,
		FWorkspaceError& OutError,
		EKeyedItemProjectionInputKind InputKind = EKeyedItemProjectionInputKind::CapturedItems,
		const FKeyedItemReferenceContext* ReferenceContext = nullptr);

	/**
	 * Resolves a complete desired keyed item map against its captured state.
	 * New items require only a string name or id, which is normalized to the
	 * existing candidate selector contract. Existing machine fields are restored
	 * from captured state and cannot be supplied by the visible document.
	 */
	enum class EKeyedItemInputKind : uint8
	{
		VisibleDesiredState,
		/** Only records produced by this owner after ordinary input validation. */
		ResolvedOperationRecords,
	};

	UEAGENTINTERFACE_API bool TryResolveKeyedItemDesiredState(
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& VisibleItems,
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& CapturedItems,
		const FString& SourceUnit,
		const FKeyedItemProjectionContract& Contract,
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue* RecordSchema,
		UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr& OutResolvedItems,
		bool& bOutSemanticChanged,
		FWorkspaceError& OutError,
		const StateSemantic::FUeAgentCanonicalValue* NestedRecordDefaults = nullptr,
		EKeyedItemInputKind InputKind = EKeyedItemInputKind::VisibleDesiredState,
		const FKeyedItemReferenceContext* ReferenceContext = nullptr);

	/** Same resolver with parser provenance for precise ordinary-workspace diagnostics. */
	UEAGENTINTERFACE_API bool TryResolveKeyedItemDesiredState(
		const StateSemantic::FUeAgentCanonicalValue& VisibleItems,
		const StateSemantic::FUeAgentCanonicalValue& CapturedItems,
		const FString& SourceUnit,
		const FKeyedItemProjectionContract& Contract,
		const StateSemantic::FUeAgentCanonicalValue* RecordSchema,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutResolvedItems,
		bool& bOutSemanticChanged,
		FWorkspaceError& OutError,
		const StateSemantic::FUeAgentCanonicalValue* NestedRecordDefaults,
		EKeyedItemInputKind InputKind,
		const FKeyedItemReferenceContext* ReferenceContext,
		const FHjsonNode* VisibleSource);

	UEAGENTINTERFACE_API bool TryResolveKeyedItemDesiredState(
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& VisibleItems,
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& CapturedItems,
		const FString& SourceUnit,
		UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr& OutResolvedItems,
		bool& bOutSemanticChanged,
		FWorkspaceError& OutError);
}
