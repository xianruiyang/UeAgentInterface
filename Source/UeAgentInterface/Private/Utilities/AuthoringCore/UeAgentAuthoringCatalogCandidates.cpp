// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringCatalogCandidates.h"

#include "Misc/SecureHash.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"

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

		bool MatchesModelFacingName(
			const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity,
			const FString& Name)
		{
			static const TCHAR* NameSelectors[] = {
				TEXT("name"),
				TEXT("display_name"),
				TEXT("full_name"),
				TEXT("search_key"),
			};
			for (const TCHAR* Selector : NameSelectors)
			{
				if (Identity.GetSelector(Selector).Equals(
						Name,
						ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return false;
		}
	}

	bool BuildIdentity(
		const FIdentityInput& Input,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		UE::UAI::Contract::FDiagnostic& OutError)
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
		if (!Selectors.FindRef(TEXT("native_action_signature")).IsEmpty())
		{
			// AuthoringSelectors contains captured action semantics, never menu
			// labels/categories. Keep the raw native signature for re-resolution,
			// and distinguish all bindings that its spawner omitted from identity.
			TArray<FString> BindingFields;
			Input.AuthoringSelectors.GetKeys(BindingFields);
			BindingFields.Remove(TEXT("native_action_signature"));
			BindingFields.Remove(TEXT("native_action_binding"));
			BindingFields.Sort();
			FString BindingMaterial;
			for (const FString& Field : BindingFields)
			{
				const FString Value = Input.AuthoringSelectors.FindChecked(Field).TrimStartAndEnd();
				if (!Value.IsEmpty())
				{
					BindingMaterial += FString::Printf(TEXT("%d:%s%d:%s"),
						Field.Len(), *Field, Value.Len(), *Value);
				}
			}
			if (!BindingMaterial.IsEmpty())
			{
				Selectors.Add(TEXT("native_action_binding"), FMD5::HashAnsiString(*BindingMaterial));
			}
		}
		return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
			TEXT("catalog_candidate_identity"), Input.TargetKind, Selectors, OutIdentity, OutError);
	}

	FString BuildPublicId(
		const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity)
	{
		if (!Identity.IsValid())
		{
			return FString();
		}
		return TEXT("candidate:") + FMD5::HashAnsiString(*Identity.CanonicalKey);
	}

	bool ResolveExactCandidate(
		const FString& Name,
		const FString& Id,
		const TArray<UeAgentAuthoringIdentity::FCanonicalIdentity>& Identities,
		UeAgentAuthoringTargetLocator::FResolveResult& OutResult,
		const int32 MaxDiagnosticCandidates)
	{
		const FString CleanName = Name.TrimStartAndEnd();
		const FString CleanId = Id.TrimStartAndEnd();
		OutResult = {};
		OutResult.Status = TEXT("failed");
		if (CleanName.IsEmpty() && CleanId.IsEmpty())
		{
			OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("target_query_invalid"), TEXT("A target name or candidate ID is required."));
			return false;
		}

		if (!CleanId.IsEmpty())
		{
			TArray<int32> PublicIdMatches;
			for (int32 Index = 0; Index < Identities.Num(); ++Index)
			{
				if (BuildPublicId(Identities[Index]).Equals(
						CleanId,
						ESearchCase::IgnoreCase))
				{
					PublicIdMatches.Add(Index);
				}
			}
			if (PublicIdMatches.Num() > 1)
			{
				OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("target_ambiguous"), TEXT("The candidate ID identifies more than one target."));
				return false;
			}
			if (PublicIdMatches.Num() == 1)
			{
				const int32 ResolvedIndex = PublicIdMatches[0];
				if (!CleanName.IsEmpty())
				{
					const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity =
						Identities[ResolvedIndex];
					if (!MatchesModelFacingName(Identity, CleanName))
					{
						OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("target_identity_conflict"), TEXT("The supplied name and candidate ID identify different targets."));
						OutResult.ConflictingSelectors.Add(TEXT("name"));
						return false;
					}
				}
				OutResult.Status = TEXT("resolved");
				OutResult.Diagnostic.Reset();
				OutResult.ResolvedIndex = ResolvedIndex;
				return true;
			}

			UeAgentAuthoringTargetLocator::FTargetQuery Query;
			Query.ModelId = TEXT("catalog_candidate_identity");
			Query.CanonicalKey = CleanId;
			if (!UeAgentAuthoringTargetLocator::ResolveExact(
					Query,
					Identities,
					OutResult,
					MaxDiagnosticCandidates))
			{
				return false;
			}
			if (!CleanName.IsEmpty()
				&& Identities.IsValidIndex(OutResult.ResolvedIndex))
			{
				const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity =
					Identities[OutResult.ResolvedIndex];
				if (!MatchesModelFacingName(Identity, CleanName))
				{
					OutResult.Status = TEXT("failed");
					OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("target_identity_conflict"), TEXT("The supplied name and candidate ID identify different targets."));
					OutResult.ResolvedIndex = INDEX_NONE;
					OutResult.ConflictingSelectors.Add(TEXT("name"));
					return false;
				}
			}
			return true;
		}

		static const TCHAR* SelectorOrder[] = {
			TEXT("name"),
			TEXT("display_name"),
			TEXT("full_name"),
			TEXT("search_key"),
		};
		for (const TCHAR* SelectorName : SelectorOrder)
		{
			UeAgentAuthoringTargetLocator::FTargetQuery Query;
			Query.ModelId = TEXT("catalog_candidate_identity");
			Query.Selectors.Add(SelectorName, CleanName);
			if (UeAgentAuthoringTargetLocator::ResolveExact(
					Query,
					Identities,
					OutResult,
					MaxDiagnosticCandidates))
			{
				return true;
			}
			if (OutResult.Diagnostic.Code != TEXT("target_not_found"))
			{
				return false;
			}
		}
		return false;
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
