#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class FJsonObject;

namespace UE::UAI::ContentDiscovery
{
	enum class ENameComparison : uint8
	{
		CaseSensitive,
		IgnoreCase,
	};
	/** Native identity remains internal; Select contains only readable semantic distinctions. */
	struct FNameSelectionEntry final
	{
		FString Name;
		FString Identity;
		TMap<FString, FString> Select;
		/** Search-only metadata; never contributes to native equivalence or selection. */
		TArray<FString> SearchTerms;
	};

	/** All source rows for one native identity; alternate names/selectors remain replayable. */
	struct FNameSelectionGroup final
	{
		TArray<int32> EntryIndices;
	};

	/** Shared identity grouping for discovery and resolution. Input order selects display representatives. */
	UEAGENTINTERFACE_API bool TryGroupNameSelectionEntries(
		const TArray<FNameSelectionEntry>& Entries, const TArray<int32>& OrderedIndices,
		TArray<FNameSelectionGroup>& OutGroups, UE::UAI::Contract::FDiagnostic& OutError);

	/** Input container holding name/select; empty context preserves native caller provenance. */
	struct FNameSelectionInputContext final
	{
		FString JsonPath;
		FString SourceKind;
		FString NameField = TEXT("name");
		FString SelectField = TEXT("select");
	};

	/** Exact original names. Ambiguity carries readable name/choose facts; native identities stay internal. */
	UEAGENTINTERFACE_API bool TryResolveNameSelection(
		const TArray<FNameSelectionEntry>& Entries, const FString& Name,
		const TMap<FString, FString>& Select, int32& OutIndex,
		UE::UAI::Contract::FDiagnostic& OutError,
		ENameComparison Comparison = ENameComparison::CaseSensitive,
		const FNameSelectionInputContext* InputContext = nullptr);

	struct FScopedNameDiscoveryEntry final
	{
		FString Name;
		/** Ordered shortest-first readable semantic scopes. */
		TArray<FString> Scopes;
		/** Native identity shared by every representation of this selectable object. */
		FString Identity;
	};

	/** Immutable preparation shared by repeated lookups against the same entries. */
	class UEAGENTINTERFACE_API FScopedNameDiscoveryIndex final
	{
	public:
		FScopedNameDiscoveryIndex(const TArray<FScopedNameDiscoveryEntry>& Entries,
			ENameComparison InComparison);
		bool TryResolve(const FString& PublicName, int32& OutIndex, UE::UAI::Contract::FDiagnostic& OutError) const;

	private:
		TArray<FScopedNameDiscoveryEntry> CleanEntries;
		TMap<FString, TArray<int32>> Groups;
		ENameComparison Comparison;
	};

	/**
	 * Completeness facts shared by names-only providers. This is an internal
	 * contract helper; it does not add fields to the model-facing payload.
	 */
	struct FNameDiscoveryCompleteness final
	{
		int32 TotalMatches = 0;
		int32 ReturnedCount = 0;
		/** Enumeration failure only; a discovered action's write support belongs to describe/create. */
		UE::UAI::Contract::FDiagnostic SourceDiagnostic;
	};

	/** Construct a known native-catalog limitation where it is detected. */
	UEAGENTINTERFACE_API UE::UAI::Contract::FDiagnostic CandidateCatalogIncomplete(
		const FString& Reason, const FString& Target = FString());

	/** Whether one candidate label is safe and useful on the model-facing name surface. */
	UEAGENTINTERFACE_API bool IsReadableDiscoveryName(const FString& Value);

	/** Add shortest-first real namespace suffixes; never synthesize identities or ordinals. */
	UEAGENTINTERFACE_API void AppendReadableDiscoveryPathScopes(
		const FString& Path, TArray<FString>& OutScopes);

	/** Readable native namespace, retaining every named component needed for selection.
	 * Use only class/asset paths, never canonical keys, signatures or generated identities. */
	UEAGENTINTERFACE_API FString MakeReadableDiscoveryPath(const FString& Path);

	/**
	 * Validates a provider page before it is projected to names-only output.
	 * The complete result set must be returned.
	 */
	UEAGENTINTERFACE_API bool TryValidateNameDiscoveryCompleteness(
		const FNameDiscoveryCompleteness& Completeness,
		UE::UAI::Contract::FDiagnostic& OutError);

	/**
	 * Projects domain-owned, already ordered candidate names to a complete
	 * model-facing discovery payload: {"names":[...]}. The domain remains
	 * responsible for enumeration, ordering, authorization and later semantic
	 * re-resolution.
	 */
	UEAGENTINTERFACE_API bool TryBuildNamesOnlyPayload(
		const TArray<FString>& OrderedNames,
		ENameComparison Comparison,
		FString& OutPayloadJson,
		UE::UAI::Contract::FDiagnostic& OutError);

	/** Executable candidate discovery retains native identities until the final name projection. */
	UEAGENTINTERFACE_API bool TryBuildCandidateDiscoveryNamesForIndices(
		const TArray<FNameSelectionEntry>& Entries, const TArray<int32>& MatchedIndices,
		ENameComparison Comparison, TArray<FString>& OutNames, UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryBuildCandidateNamesOnlyPayload(
		const TArray<FNameSelectionEntry>& OrderedEntries, ENameComparison Comparison,
		FString& OutPayloadJson, UE::UAI::Contract::FDiagnostic& OutError,
		const UE::UAI::Contract::FDiagnostic& SourceDiagnostic = {});

	UEAGENTINTERFACE_API bool TryBuildCandidateNamesOnlyPayloadForIndices(
		const TArray<FNameSelectionEntry>& Entries, const TArray<int32>& MatchedIndices,
		ENameComparison Comparison, FString& OutPayloadJson, UE::UAI::Contract::FDiagnostic& OutError,
		const UE::UAI::Contract::FDiagnostic& SourceDiagnostic = {});

	/** Attach the domain's public query context once, without changing candidate selectors. */
	UEAGENTINTERFACE_API bool TryAddDiscoveryScope(
		const TSharedRef<FJsonObject>& Scope, FString& InOutPayloadJson, UE::UAI::Contract::FDiagnostic& OutError);

	/**
	 * Builds one readable public name per source row, preserving index correspondence.
	 * Representations of the same native identity may share a name. Distinct identities receive
	 * the shortest semantic-scope prefix that makes the whole
	 * duplicate group unique. Machine identities and ordinal fallbacks are not
	 * accepted.
	 */
	UEAGENTINTERFACE_API bool TryBuildScopedDiscoveryNames(
		const TArray<FScopedNameDiscoveryEntry>& Entries,
		ENameComparison Comparison,
		TArray<FString>& OutNames,
		UE::UAI::Contract::FDiagnostic& OutError);

	/**
	 * Builds one name per matched native identity, while testing each alias
	 * against its complete same-name group. Unrelated ambiguous groups do not
	 * block a focused query.
	 */
	UEAGENTINTERFACE_API bool TryBuildScopedDiscoveryNamesForIndices(
		const TArray<FScopedNameDiscoveryEntry>& Entries,
		const TArray<int32>& MatchedIndices,
		ENameComparison Comparison,
		TArray<FString>& OutNames,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryBuildScopedNamesOnlyPayload(
		const TArray<FScopedNameDiscoveryEntry>& Entries,
		ENameComparison Comparison,
		FString& OutPayloadJson,
		UE::UAI::Contract::FDiagnostic& OutError);

	/**
	 * Projects a matched subset. Each alias is compared with its complete
	 * same-name group and is never recomputed against only the subset.
	 */
	UEAGENTINTERFACE_API bool TryBuildScopedNamesOnlyPayloadForIndices(
		const TArray<FScopedNameDiscoveryEntry>& Entries,
		const TArray<int32>& MatchedIndices,
		ENameComparison Comparison,
		FString& OutPayloadJson,
		UE::UAI::Contract::FDiagnostic& OutError);

	/**
	 * Resolves a bare name or an exact semantic-scope prefix to one current
	 * entry. Previously emitted scopes remain valid when the candidate set
	 * changes; missing, changed or ambiguous scopes are never ignored.
	 */
	UEAGENTINTERFACE_API bool TryResolveScopedDiscoveryName(
		const TArray<FScopedNameDiscoveryEntry>& Entries,
		const FString& PublicName,
		ENameComparison Comparison,
		int32& OutIndex,
		UE::UAI::Contract::FDiagnostic& OutError);

}
