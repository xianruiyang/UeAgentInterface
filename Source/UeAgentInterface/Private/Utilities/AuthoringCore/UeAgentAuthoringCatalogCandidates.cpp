// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringCatalogCandidates.h"

namespace UeAgentAuthoringCatalogCandidates
{
	namespace
	{
		FString NormalizeForSort(FString Value)
		{
			Value.TrimStartAndEndInline();
			Value.ToLowerInline();
			return Value;
		}

		int32 CompareText(const FString& Left, const FString& Right)
		{
			return NormalizeForSort(Left).Compare(NormalizeForSort(Right), ESearchCase::CaseSensitive);
		}

		bool LessByNameThenIdentity(const FSortKey& Left, const FSortKey& Right)
		{
			const int32 NameComparison = CompareText(Left.Name, Right.Name);
			if (NameComparison != 0) return NameComparison < 0;
			return Left.CanonicalKey < Right.CanonicalKey;
		}
	}

	bool BuildIdentity(
		const FIdentityInput& Input,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		FString& OutError)
	{
		TMap<FString, FString> Selectors;
		Selectors.Add(TEXT("catalog_type"), Input.CatalogType);
		Selectors.Add(TEXT("full_name"), Input.FullName.IsEmpty() ? Input.StableId : Input.FullName);
		Selectors.Add(TEXT("stable_id"), Input.StableId);
		Selectors.Add(TEXT("name"), Input.Name);
		Selectors.Add(TEXT("display_name"), Input.DisplayName);
		Selectors.Add(TEXT("search_key"), Input.SearchKey);
		Selectors.Add(TEXT("object_path"), Input.ObjectPath);
		Selectors.Add(TEXT("package_path"), Input.PackagePath);
		for (const TPair<FString, FString>& Selector : Input.AuthoringSelectors)
		{
			Selectors.Add(Selector.Key, Selector.Value);
		}
		return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
			TEXT("catalog_candidate_identity"), Input.TargetKind, Selectors, OutIdentity, OutError);
	}

	bool Less(const FSortKey& Left, const FSortKey& Right, ESortMode Mode)
	{
		if (Mode == ESortMode::Score && !FMath::IsNearlyEqual(Left.Score, Right.Score)) return Left.Score > Right.Score;
		if (Mode == ESortMode::Category)
		{
			const int32 CategoryComparison = CompareText(Left.CategoryPath, Right.CategoryPath);
			if (CategoryComparison != 0) return CategoryComparison < 0;
			return LessByNameThenIdentity(Left, Right);
		}
		if (Mode == ESortMode::Name) return LessByNameThenIdentity(Left, Right);
		if (Left.UiOrder != Right.UiOrder) return Left.UiOrder < Right.UiOrder;
		return LessByNameThenIdentity(Left, Right);
	}

	void Sort(TArray<FSortKey>& Keys, ESortMode Mode)
	{
		Keys.Sort([Mode](const FSortKey& Left, const FSortKey& Right)
		{
			return Less(Left, Right, Mode);
		});
	}
}
