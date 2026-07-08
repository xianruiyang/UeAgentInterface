// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentPaletteSearch
{
	struct FSearchParams
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
		bool bIncludeLegacyAuthoring;
		bool bIncludeDebugFields;
		int32 Limit;
		int32 Offset;
	};

	struct FCandidate
	{
		FCandidate();

		FString CandidateId;
		FString Kind;
		FString Name;
		FString FullName;
		FString StableId;
		FString CanonicalKey;
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
		TSharedPtr<FJsonObject> JsonAuthoring;
		TSharedPtr<FJsonObject> LegacyAuthoring;
	};

	struct FSearchResult
	{
		FSearchResult();

		int32 TotalRaw;
		int32 TotalMatching;
		TArray<FCandidate> MatchingCandidates;
		TArray<FString> Warnings;
	};

	bool ParseSearchParams(const TSharedPtr<FJsonObject>& Params, FSearchParams& OutParams, FString& OutError);
	bool ExecuteSearch(const FSearchParams& Params, FSearchResult& OutResult, FString& OutError);
	TSharedRef<FJsonObject> BuildSearchResponse(const FSearchParams& Params, const FSearchResult& Result);
	TSharedRef<FJsonObject> BuildCategoriesResponse(const FSearchParams& Params, const FSearchResult& Result);

	bool CollectNiagaraCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError);
	bool CollectMaterialCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError);
	bool CollectBlueprintCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError);
	bool CollectComponentWidgetCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError);
	bool CollectPlaceableActorCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError);
}
