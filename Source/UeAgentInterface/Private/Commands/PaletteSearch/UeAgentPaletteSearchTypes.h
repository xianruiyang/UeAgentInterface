// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"

class FJsonObject;
class AActor;
class UBlueprint;
class ULevel;
class USCS_Node;
struct FActorSpawnParameters;

namespace UeAgentPaletteSearch
{
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
		bool bLibraryOnly;
		bool bIncludeItems;
		bool bIncludeGroups;
		bool bIncludeCategoryTree;
		bool bContextSensitive;
		bool bIncludeDebugFields;
		int32 Limit;
		int32 Offset;
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
		bool bInLibrary;
		bool bSuggested;
		bool bSupportedInContext;
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
		TArray<FString> Warnings;
	};

	bool ParseSearchParams(const TSharedPtr<FJsonObject>& Params, FSearchParams& OutParams, FString& OutError);
	bool SetCandidateIdentity(
		const FString& CatalogType,
		const FString& StableId,
		const FString& SearchKey,
		FCandidate& InOutCandidate,
		FString& OutError);
	UEAGENTINTERFACE_API bool TryCanonicalizeNiagaraTargetUsage(
		const FString& Value,
		FString& OutCanonicalUsage);
	UEAGENTINTERFACE_API bool ExecuteSearch(const FSearchParams& Params, FSearchResult& OutResult, FString& OutError);
	TSharedRef<FJsonObject> BuildSearchResponse(const FSearchParams& Params, const FSearchResult& Result);
	TSharedRef<FJsonObject> BuildCategoriesResponse(const FSearchParams& Params, const FSearchResult& Result);

	bool CollectNiagaraCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError);
	bool CollectMaterialCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError);
	bool CollectBlueprintCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError);
	/** Resolve a Blueprint graph selector to the stable identity used by authoring. */
	UEAGENTINTERFACE_API bool TryResolveBlueprintGraphIdentity(
		const FString& AssetPath,
		const FString& GraphSelector,
		FString& OutGraphId,
		FString& OutGraphName,
		FString& OutError);
	/** Resolve one exact Blueprint action from the same native menu used by search. */
	UEAGENTINTERFACE_API bool TryResolveBlueprintActionCandidate(
		const FSearchParams& Params,
		const FString& Name,
		const FString& Id,
		FCandidate& OutCandidate,
		FString& OutError);
	/** Resolve and invoke that exact native menu action. */
	UEAGENTINTERFACE_API bool TryPerformBlueprintActionCandidate(
		const FSearchParams& Params,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		class UEdGraphNode*& OutNode,
		FCandidate& OutCandidate,
		FString& OutError);
	/** Resolve one exact Material expression action from the same native menu used by search. */
	UEAGENTINTERFACE_API bool TryResolveMaterialActionCandidate(
		const FSearchParams& Params,
		const FString& Name,
		const FString& Id,
		FCandidate& OutCandidate,
		FString& OutError);
	/** Re-enumerate and invoke that exact native Material action. */
	UEAGENTINTERFACE_API bool TryPerformMaterialActionCandidate(
		const FSearchParams& Params,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		class UMaterialExpression*& OutExpression,
		FCandidate& OutCandidate,
		FString& OutError);
	bool CollectComponentWidgetCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError);
	/** Resolve one exact Add Component entry from the same native registry used by the editor menu. */
	UEAGENTINTERFACE_API bool TryResolveActorComponentCandidate(
		const FString& Name,
		const FString& Id,
		FCandidate& OutCandidate,
		FString& OutError);
	/** Resolve and invoke that exact Add Component entry in one Blueprint SCS context. */
	UEAGENTINTERFACE_API bool TryPerformBlueprintComponentCandidate(
		UBlueprint& Blueprint,
		USCS_Node* Parent,
		const FString& Name,
		const FString& Id,
		const FString& RequestedName,
		USCS_Node*& OutNode,
		FCandidate& OutCandidate,
		FString& OutError);
	bool CollectPlaceableActorCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError);
	/** Resolve one exact item from the same Placement Mode registry used by Place Actors. */
	UEAGENTINTERFACE_API bool TryResolvePlaceableActorCandidate(
		const FString& Name,
		const FString& Id,
		const FString& Category,
		FCandidate& OutCandidate,
		FString& OutError);
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
		FString& OutError);
}
