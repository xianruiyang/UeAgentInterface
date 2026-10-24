// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"

class FJsonObject;
class AActor;
class UBlueprint;
class ULevel;
class USCS_Node;
class USceneComponent;
struct FActorSpawnParameters;
namespace UE::UAI::ContentDiscovery { struct FNameSelectionInputContext; }

namespace UE::UAI::ContentDiscovery
{
	struct FScopedNameDiscoveryEntry;
}

namespace UeAgentPaletteSearch
{
	using FDiagnostic = UE::UAI::Contract::FDiagnostic;
	// Reuse native catalogs only within one synchronous normalization or graph body phase.
	class FBlueprintActionBatchScope final
	{
	public:
		FBlueprintActionBatchScope();
		~FBlueprintActionBatchScope();
		FBlueprintActionBatchScope(const FBlueprintActionBatchScope&) = delete;
		FBlueprintActionBatchScope& operator=(const FBlueprintActionBatchScope&) = delete;
	private:
		void* State = nullptr;
	};
	void InvalidateBlueprintActionBatch();

	struct UEAGENTINTERFACE_API FSearchParams
	{
		FSearchParams();

		FString CatalogType;
		FString SearchText;
		FString SearchMode;
		FString Sort;
		FString AssetPath;
		FString GraphName;
		FString NodeGuid;
		FString PinGuid;
		FString PinName;
		FString CandidateId;
		FString TargetUsage;
		FString SuggestedFilter;
		TArray<FString> SourceFilters;
		TArray<FString> CategoryFilters;
		TArray<FString> TypeFilters;
		/** Public short semantic choices; never used as native rebinding assertions. */
		TMap<FString, FString> CandidateSelect;
		/** Safe semantic assertions used only to rebind a candidate on a duplicated validation Blueprint. */
		TMap<FString, FString> RequiredAuthoringSelectors;
		bool bLibraryOnly;
		bool bContextSensitive;
		/** Retain naming context from the same collection used to match this query. */
		bool bIncludeNameContext = false;
	};

	struct UEAGENTINTERFACE_API FCandidate
	{
		FCandidate();

		FString Kind;
		FString Name;
		FString FullName;
		FString SearchKey;
		FString DisplayName;
		FString Tooltip;
		FString Keywords;
		FString CategoryPath;
		FString Section;
		FString Type;
		FString SourceOrigin;
		FString SourceLabel;
		FString PackagePath;
		FString ObjectPath;
		FString WriteSupport;
		FString SupportReason;
		FDiagnostic SupportDiagnostic;
		bool bInLibrary;
		bool bSuggested;
		bool bSupportedInContext;
		/** Result of a provider-owned native query filter, before common scoring. */
		bool bMatchesNativeSearch = true;
		int32 UiOrder;
		double Score;
		TMap<FString, FString> AuthoringSelectors;
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;

		FString GetCandidateId() const;
		FString GetPublicId() const;
		FString GetStableId() const;
	};

	struct UEAGENTINTERFACE_API FSearchResult
	{
		FSearchResult();

		int32 TotalRaw;
		int32 TotalMatching;
		bool bHasTargetResolution;
		UeAgentAuthoringTargetLocator::FResolveResult TargetResolution;
		TArray<FCandidate> MatchingCandidates;
		TArray<FCandidate> NameContextCandidates;
		TArray<FString> Warnings;
	};

	bool ParseSearchParams(const TSharedPtr<FJsonObject>& Params, FSearchParams& OutParams, FDiagnostic& OutError);
	bool SetCandidateIdentity(
		const FString& CatalogType,
		const FString& StableId,
		const FString& SearchKey,
		FCandidate& InOutCandidate,
		FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool TryCanonicalizeNiagaraTargetUsage(
		const FString& Value,
		FString& OutCanonicalUsage);
	/** Whether a Blueprint candidate authoring selector is safe to project and round-trip publicly. */
	UEAGENTINTERFACE_API bool IsPublicBlueprintCandidateSemanticField(
		const FString& Field);
	/** Readable name exposed by Blueprint discovery and accepted by re-resolution. */
	UEAGENTINTERFACE_API FString GetBlueprintActionPublicName(
		const FCandidate& Candidate);
	/** Build the single shared readable-name and minimal semantic-scope view. */
	UEAGENTINTERFACE_API bool TryBuildBlueprintActionScopedDiscoveryEntry(
		const FCandidate& Candidate,
		UE::UAI::ContentDiscovery::FScopedNameDiscoveryEntry& OutEntry);
	UEAGENTINTERFACE_API bool ExecuteSearch(const FSearchParams& Params, FSearchResult& OutResult, FDiagnostic& OutError);

	bool CollectNiagaraCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool CollectMaterialCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FDiagnostic& OutError);
	bool CollectBlueprintCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FDiagnostic& OutError);
	/** Resolve a Blueprint graph selector to the stable identity used by authoring. */
	UEAGENTINTERFACE_API bool TryResolveBlueprintGraphIdentity(
		const FString& AssetPath,
		const FString& GraphSelector,
		FString& OutGraphId,
		FString& OutGraphName,
		FDiagnostic& OutError);
	/** Resolve one exact Blueprint action from the same native menu used by search. */
	UEAGENTINTERFACE_API bool TryResolveBlueprintActionCandidate(
		const FSearchParams& Params,
		const FString& Name,
		const FString& Id,
		FCandidate& OutCandidate,
		FDiagnostic& OutError);
	/** Resolve and invoke that exact native menu action. */
	UEAGENTINTERFACE_API bool TryPerformBlueprintActionCandidate(
		const FSearchParams& Params,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		class UEdGraphNode*& OutNode,
		FCandidate& OutCandidate,
		FDiagnostic& OutError);
	/** Resolve one exact Material expression action from the same native menu used by search. */
	UEAGENTINTERFACE_API bool TryResolveMaterialActionCandidate(
		const FSearchParams& Params,
		const FString& Name,
		const TMap<FString, FString>& Select,
		const FString& Id,
		FCandidate& OutCandidate,
		FDiagnostic& OutError,
		const UE::UAI::ContentDiscovery::FNameSelectionInputContext* InputContext = nullptr);
	/** Re-enumerate and invoke that exact native Material action. */
	UEAGENTINTERFACE_API bool TryPerformMaterialActionCandidate(
		const FSearchParams& Params,
		const FString& Name,
		const TMap<FString, FString>& Select,
		const FString& Id,
		const FVector2f& Location,
		class UMaterialExpression*& OutExpression,
		FCandidate& OutCandidate,
		FDiagnostic& OutError);
	bool CollectComponentWidgetCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FDiagnostic& OutError);
	/** Resolve one exact Add Component entry from the same native registry used by the editor menu. */
	UEAGENTINTERFACE_API bool TryResolveActorComponentCandidate(
		const FString& Name,
		const FString& Id,
		FCandidate& OutCandidate,
		FDiagnostic& OutError,
		const TMap<FString, FString>& Select = {});
	/** Resolve and invoke that exact Add Component entry in one Blueprint SCS context. */
	UEAGENTINTERFACE_API bool TryPerformBlueprintComponentCandidate(
		UBlueprint& Blueprint,
		USCS_Node* Parent,
		const FString& Name,
		const FString& Id,
		const FString& RequestedName,
		USCS_Node*& OutNode,
		FCandidate& OutCandidate,
		FDiagnostic& OutError,
		USceneComponent* InheritedParent = nullptr,
		const TMap<FString, FString>& Select = {});
	bool CollectPlaceableActorCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FDiagnostic& OutError);
	/** Resolve one exact item from the same Placement Mode registry used by Place Actors. */
	UEAGENTINTERFACE_API bool TryResolvePlaceableActorCandidate(
		const FString& Name,
		const FString& Id,
		const FString& Category,
		FCandidate& OutCandidate,
		FDiagnostic& OutError,
		const TMap<FString, FString>& Select = {});
	/** Re-resolve and place the exact native item through UE's Placement Subsystem. */
	UEAGENTINTERFACE_API bool TryPerformPlaceableActorCandidate(
		const FString& Name,
		const FString& Id,
		const FString& Category,
		ULevel& Level,
		const FTransform& Transform,
		const FActorSpawnParameters& SpawnParameters,
		AActor*& OutActor,
		FCandidate& OutCandidate,
		FDiagnostic& OutError,
		const TMap<FString, FString>& Select = {});
}
