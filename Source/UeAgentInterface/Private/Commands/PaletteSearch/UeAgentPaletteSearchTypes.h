// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentPaletteSearch
{
	struct FSearchParams
	{
		FString CatalogType;
		FString SearchText;
		FString SearchMode = TEXT("fuzzy");
		FString Sort = TEXT("ui_order");
		FString AssetPath;
		FString GraphName;
		FString NodeGuid;
		FString PinGuid;
		FString PinName;
		FString CandidateId;
		FString TargetUsage;
		FString SuggestedFilter = TEXT("all");
		TArray<FString> SourceFilters;
		TArray<FString> CategoryFilters;
		TArray<FString> TypeFilters;
		bool bLibraryOnly = true;
		bool bIncludeItems = true;
		bool bIncludeGroups = true;
		bool bIncludeCategoryTree = true;
		bool bContextSensitive = true;
		bool bIncludeLegacyAuthoring = false;
		bool bIncludeDebugFields = false;
		int32 Limit = 50;
		int32 Offset = 0;
	};

	struct FCandidate
	{
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
		bool bInLibrary = false;
		bool bSuggested = false;
		bool bSupportedInContext = true;
		int32 UiOrder = 0;
		double Score = 0.0;
		TSharedPtr<FJsonObject> JsonAuthoring;
		TSharedPtr<FJsonObject> LegacyAuthoring;
	};

	struct FSearchResult
	{
		int32 TotalRaw = 0;
		int32 TotalMatching = 0;
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
}
