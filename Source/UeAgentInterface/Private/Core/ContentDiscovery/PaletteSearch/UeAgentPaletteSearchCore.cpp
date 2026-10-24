// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"

#include "Algo/Sort.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchAuthoring.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringCatalogCandidates.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UeAgentPaletteSearch
{
FSearchParams::FSearchParams()
	: SearchMode(TEXT("fuzzy"))
	, Sort(TEXT("ui_order"))
	, SuggestedFilter(TEXT("all"))
	, bLibraryOnly(true)
	, bContextSensitive(true)
{
}

FCandidate::FCandidate()
	: bInLibrary(false)
	, bSuggested(false)
	, bSupportedInContext(true)
	, UiOrder(0)
	, Score(0.0)
{
}

FString FCandidate::GetCandidateId() const
{
	return Identity.CanonicalKey;
}

FString FCandidate::GetPublicId() const
{
	return UeAgentAuthoringCatalogCandidates::BuildPublicId(Identity);
}

FString FCandidate::GetStableId() const
{
	return Identity.GetSelector(TEXT("stable_id"));
}

FSearchResult::FSearchResult()
	: TotalRaw(0)
	, TotalMatching(0)
	, bHasTargetResolution(false)
{
}

bool SetCandidateIdentity(
	const FString& CatalogType,
	const FString& StableId,
	const FString& SearchKey,
	FCandidate& InOutCandidate,
	FDiagnostic& OutError)
{
	UeAgentAuthoringCatalogCandidates::FIdentityInput Input;
	Input.CatalogType = CatalogType;
	Input.TargetKind = InOutCandidate.Kind;
	Input.Name = InOutCandidate.Name;
	Input.DisplayName = InOutCandidate.DisplayName;
	Input.FullName = InOutCandidate.FullName;
	Input.StableId = StableId;
	Input.SearchKey = SearchKey;
	Input.ObjectPath = InOutCandidate.ObjectPath;
	Input.PackagePath = InOutCandidate.PackagePath;
	Input.AuthoringSelectors = InOutCandidate.AuthoringSelectors;
	InOutCandidate.SearchKey = SearchKey;
	if (UeAgentAuthoringCatalogCandidates::BuildIdentity(Input, InOutCandidate.Identity, OutError))
		return true;
	if (OutError.Stage.IsEmpty()) OutError.Stage = TEXT("candidate_identity");
	if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = InOutCandidate.Name;
	return false;
}

namespace
{
	static FString NormalizeToken(const FString& InValue)
	{
		return InValue.TrimStartAndEnd().ToLower();
	}

	static FString CanonicalizeCatalogType(const FString& CatalogType)
	{
		if (CatalogType.Equals(TEXT("level_actor")) || CatalogType.Equals(TEXT("actor_placeable")))
		{
			return TEXT("placeable_actor");
		}
		return CatalogType;
	}

	static void ReadStringArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, TArray<FString>& OutValues)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Obj->TryGetArrayField(FieldName, Values) || Values == nullptr)
		{
			FString SingleValue;
			if (Obj->TryGetStringField(FieldName, SingleValue) && !SingleValue.TrimStartAndEnd().IsEmpty())
			{
				OutValues.Add(SingleValue.TrimStartAndEnd());
			}
			return;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}

			FString TextValue;
			if (Value->TryGetString(TextValue) && !TextValue.TrimStartAndEnd().IsEmpty())
			{
				OutValues.Add(TextValue.TrimStartAndEnd());
			}
		}
	}

	static void ReadSourceFilters(const TSharedPtr<FJsonObject>& Obj, TArray<FString>& OutValues)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonObject>* SourceObjPtr = nullptr;
		if (Obj->TryGetObjectField(TEXT("source"), SourceObjPtr) && SourceObjPtr && SourceObjPtr->IsValid())
		{
			static const TCHAR* KnownSources[] = { TEXT("niagara"), TEXT("engine"), TEXT("game"), TEXT("project"), TEXT("plugins"), TEXT("developer") };
			for (const TCHAR* KnownSource : KnownSources)
			{
				bool bEnabled = false;
				if ((*SourceObjPtr)->TryGetBoolField(KnownSource, bEnabled) && bEnabled)
				{
					OutValues.Add(KnownSource);
				}
			}
			return;
		}

		ReadStringArray(Obj, TEXT("source"), OutValues);
	}

	static void ReadBoolField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, bool& InOutValue)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		bool bValue = false;
		if (Obj->TryGetBoolField(FieldName, bValue))
		{
			InOutValue = bValue;
		}
	}

	static void ReadStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FString& InOutValue)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		FString Value;
		if (Obj->TryGetStringField(FieldName, Value) && !Value.TrimStartAndEnd().IsEmpty())
		{
			InOutValue = Value.TrimStartAndEnd();
		}
	}

	static bool ContainsAnyNormalized(const TArray<FString>& Filters, const FString& Value)
	{
		if (Filters.IsEmpty())
		{
			return true;
		}

		const FString NormalizedValue = NormalizeToken(Value);
		for (const FString& Filter : Filters)
		{
			const FString NormalizedFilter = NormalizeToken(Filter);
			if (!NormalizedFilter.IsEmpty() && NormalizedValue.Contains(NormalizedFilter))
			{
				return true;
			}
		}
		return false;
	}

	static UE::UAI::ContentDiscovery::EContentDiscoveryMatchMode ToCommonMatchMode(
		const FString& SearchMode)
	{
		using UE::UAI::ContentDiscovery::EContentDiscoveryMatchMode;
		if (SearchMode.Equals(TEXT("exact"), ESearchCase::IgnoreCase))
		{
			return EContentDiscoveryMatchMode::Exact;
		}
		if (SearchMode.Equals(TEXT("prefix"), ESearchCase::IgnoreCase))
		{
			return EContentDiscoveryMatchMode::Prefix;
		}
		return EContentDiscoveryMatchMode::Text;
	}

	static bool TryScoreCandidate(
		const FSearchParams& Params,
		const FCandidate& Candidate,
		double& OutScore)
	{
		UE::UAI::ContentDiscovery::FContentDiscoveryRequest Request;
		Request.Query = Params.SearchText;
		Request.MatchMode = ToCommonMatchMode(Params.SearchMode);
		Request.ContentKinds = {TEXT("catalog_candidate")};
		const bool bBlueprintCandidate =
			Params.CatalogType.StartsWith(TEXT("blueprint_"));
		const FString PrimaryName = bBlueprintCandidate
			? GetBlueprintActionPublicName(Candidate)
			: (Candidate.Name.IsEmpty() ? Candidate.DisplayName : Candidate.Name);
		if (bBlueprintCandidate
			&& PrimaryName.StartsWith(TEXT("Cast To "), ESearchCase::IgnoreCase)
			&& !Request.Query.TrimStartAndEnd().StartsWith(
				TEXT("Cast"), ESearchCase::IgnoreCase))
		{
			return false;
		}
		TArray<FString> SearchTerms = bBlueprintCandidate
			? TArray<FString>{
				PrimaryName,
				Candidate.DisplayName,
				Candidate.Keywords,
				Candidate.Kind,
				Candidate.CategoryPath,
			}
			: TArray<FString>{
				Candidate.Name,
				Candidate.DisplayName,
				Candidate.FullName,
				Candidate.Kind,
				Candidate.CategoryPath,
				Candidate.Keywords,
				Candidate.Tooltip,
				Candidate.SearchKey,
				Candidate.GetStableId(),
				Candidate.PackagePath,
				Candidate.ObjectPath,
			};
		for (const TPair<FString, FString>& Selector : Candidate.AuthoringSelectors)
		{
			const FString Value = Selector.Value.TrimStartAndEnd();
			const bool bReadableSearchSelector =
				Selector.Key == TEXT("component_name")
				|| Selector.Key == TEXT("node_type")
				|| Selector.Key == TEXT("variable_name");
			if (bReadableSearchSelector
				&& !Value.IsEmpty()
				&& !Value.StartsWith(TEXT("/"))
				&& !Value.Contains(TEXT("\\")))
			{
				SearchTerms.Add(Value);
			}
		}
		if (bBlueprintCandidate && !Request.Query.TrimStartAndEnd().IsEmpty())
		{
			auto CompactReadable = [](const FString& Value)
			{
				FString Result;
				for (const TCHAR Character : Value.ToLower())
				{
					if (FChar::IsAlnum(Character))
					{
						Result.AppendChar(Character);
					}
				}
				return Result;
			};
			const FString CompactQuery = CompactReadable(Request.Query);
			bool bPhraseMatch = false;
			// Discovery accepts native menu text/keywords; creation still resolves the
			// public selector against its native action identity, not these search terms.
			TArray<FString> StrictReadableTerms{PrimaryName, Candidate.DisplayName, Candidate.Keywords};
			if (Candidate.Type == TEXT("blueprint_variable_node"))
			{
				StrictReadableTerms.Add(
					Candidate.AuthoringSelectors.FindRef(TEXT("node_type")));
				StrictReadableTerms.Add(
					Candidate.AuthoringSelectors.FindRef(TEXT("variable_name")));
			}
			const FString ComponentName =
				Candidate.AuthoringSelectors.FindRef(TEXT("component_name"));
			if (!ComponentName.IsEmpty())
			{
				StrictReadableTerms.Add(ComponentName);
			}
			TArray<FString> CompactTerms;
			for (const FString& Term : StrictReadableTerms)
			{
				const FString CompactTerm = CompactReadable(Term);
				if (!CompactTerm.IsEmpty())
				{
					CompactTerms.Add(CompactTerm);
					bPhraseMatch |= !CompactQuery.IsEmpty()
						&& CompactTerm.Contains(CompactQuery);
				}
			}
			if (!bPhraseMatch)
			{
				TArray<FString> QueryWords;
				Request.Query.ParseIntoArrayWS(QueryWords);
				for (const FString& QueryWord : QueryWords)
				{
					const FString CompactWord = CompactReadable(QueryWord);
					const bool bWordMatch = !CompactWord.IsEmpty()
						&& CompactTerms.ContainsByPredicate(
							[&CompactWord](const FString& Term)
							{
								return Term.Contains(CompactWord);
							});
					if (!bWordMatch)
					{
						return false;
					}
				}
			}
		}
		int32 CommonScore = 0;
		FString MatchReason;
		if (!UE::UAI::ContentDiscovery::TryScoreContentDiscoveryMatch(
				Request,
				bBlueprintCandidate ? PrimaryName : Candidate.GetCandidateId(),
				PrimaryName,
				SearchTerms,
				CommonScore,
				MatchReason))
		{
			return false;
		}
		OutScore = static_cast<double>(CommonScore)
			+ (Candidate.bSuggested ? 3.0 : 0.0);
		return true;
	}

	static UeAgentAuthoringCatalogCandidates::FSortKey MakeSortKey(const FCandidate& Candidate)
	{
		UeAgentAuthoringCatalogCandidates::FSortKey Key;
		Key.CanonicalKey = Candidate.GetCandidateId();
		Key.Name = Candidate.Name.IsEmpty() ? Candidate.DisplayName : Candidate.Name;
		Key.CategoryPath = Candidate.CategoryPath;
		Key.UiOrder = Candidate.UiOrder;
		Key.Score = Candidate.Score;
		return Key;
	}

	static void SortCandidates(const FSearchParams& Params, TArray<FCandidate>& Candidates)
	{
		using namespace UeAgentAuthoringCatalogCandidates;
		ESortMode Mode = ESortMode::UiOrder;
		if (Params.Sort.Equals(TEXT("name"))) Mode = ESortMode::Name;
		else if (Params.Sort.Equals(TEXT("category"))) Mode = ESortMode::Category;
		else if (!Params.SearchText.TrimStartAndEnd().IsEmpty()) Mode = ESortMode::Score;
		Algo::Sort(Candidates, [Mode](const FCandidate& Left, const FCandidate& Right)
		{
			return Less(MakeSortKey(Left), MakeSortKey(Right), Mode);
		});
	}

}

bool ParseSearchParams(const TSharedPtr<FJsonObject>& Params, FSearchParams& OutParams, FDiagnostic& OutError)
{
	if (!Params.IsValid())
	{
		OutError = FDiagnostic::Failure(TEXT("params_required"), TEXT("params object is required."), TEXT("candidate_search"));
		return false;
	}

	ReadStringField(Params, TEXT("catalog_type"), OutParams.CatalogType);
	ReadStringField(Params, TEXT("type"), OutParams.CatalogType);
	ReadStringField(Params, TEXT("query"), OutParams.SearchText);
	ReadStringField(Params, TEXT("text"), OutParams.SearchText);
	ReadStringField(Params, TEXT("search_text"), OutParams.SearchText);
	ReadStringField(Params, TEXT("search"), OutParams.SearchText);
	ReadStringField(Params, TEXT("asset_path"), OutParams.AssetPath);
	ReadStringField(Params, TEXT("graph_name"), OutParams.GraphName);
	ReadStringField(Params, TEXT("node_guid"), OutParams.NodeGuid);
	ReadStringField(Params, TEXT("pin_guid"), OutParams.PinGuid);
	ReadStringField(Params, TEXT("pin_id"), OutParams.PinGuid);
	ReadStringField(Params, TEXT("pin_name"), OutParams.PinName);
	ReadStringField(Params, TEXT("candidate_id"), OutParams.CandidateId);
	ReadStringField(Params, TEXT("search_mode"), OutParams.SearchMode);
	ReadStringField(Params, TEXT("sort"), OutParams.Sort);
	ReadStringField(Params, TEXT("target_usage"), OutParams.TargetUsage);
	ReadStringField(Params, TEXT("suggested_filter"), OutParams.SuggestedFilter);
	ReadBoolField(Params, TEXT("library_only"), OutParams.bLibraryOnly);
	ReadBoolField(Params, TEXT("context_sensitive"), OutParams.bContextSensitive);

	const TSharedPtr<FJsonObject>* QueryObjPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("query"), QueryObjPtr) && QueryObjPtr && QueryObjPtr->IsValid())
	{
		ReadStringField(*QueryObjPtr, TEXT("text"), OutParams.SearchText);
		ReadStringField(*QueryObjPtr, TEXT("search_mode"), OutParams.SearchMode);
	}

	const TSharedPtr<FJsonObject>* ContextObjPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("context"), ContextObjPtr) && ContextObjPtr && ContextObjPtr->IsValid())
	{
		ReadStringField(*ContextObjPtr, TEXT("asset_path"), OutParams.AssetPath);
		ReadStringField(*ContextObjPtr, TEXT("graph_name"), OutParams.GraphName);
		ReadStringField(*ContextObjPtr, TEXT("node_guid"), OutParams.NodeGuid);
		ReadStringField(*ContextObjPtr, TEXT("pin_guid"), OutParams.PinGuid);
		ReadStringField(*ContextObjPtr, TEXT("pin_id"), OutParams.PinGuid);
		ReadStringField(*ContextObjPtr, TEXT("pin_name"), OutParams.PinName);
		ReadStringField(*ContextObjPtr, TEXT("candidate_id"), OutParams.CandidateId);
	}

	const TSharedPtr<FJsonObject>* FiltersObjPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("filters"), FiltersObjPtr) && FiltersObjPtr && FiltersObjPtr->IsValid())
	{
		ReadStringField(*FiltersObjPtr, TEXT("target_usage"), OutParams.TargetUsage);
		ReadStringField(*FiltersObjPtr, TEXT("suggested"), OutParams.SuggestedFilter);
		ReadStringField(*FiltersObjPtr, TEXT("suggested_filter"), OutParams.SuggestedFilter);
		ReadBoolField(*FiltersObjPtr, TEXT("library_only"), OutParams.bLibraryOnly);
		ReadBoolField(*FiltersObjPtr, TEXT("context_sensitive"), OutParams.bContextSensitive);
		ReadStringArray(*FiltersObjPtr, TEXT("categories"), OutParams.CategoryFilters);
		ReadStringArray(*FiltersObjPtr, TEXT("category_paths"), OutParams.CategoryFilters);
		ReadStringArray(*FiltersObjPtr, TEXT("types"), OutParams.TypeFilters);
		ReadSourceFilters(*FiltersObjPtr, OutParams.SourceFilters);
	}

	const TSharedPtr<FJsonObject>* ViewObjPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("view"), ViewObjPtr) && ViewObjPtr && ViewObjPtr->IsValid())
	{
	}

	const TSharedPtr<FJsonObject>* PageObjPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("page"), PageObjPtr) && PageObjPtr && PageObjPtr->IsValid())
	{
		ReadStringField(*PageObjPtr, TEXT("sort"), OutParams.Sort);
	}

	ReadStringArray(Params, TEXT("source"), OutParams.SourceFilters);
	ReadStringArray(Params, TEXT("sources"), OutParams.SourceFilters);
	ReadStringArray(Params, TEXT("categories"), OutParams.CategoryFilters);
	ReadStringArray(Params, TEXT("category_paths"), OutParams.CategoryFilters);
	ReadStringArray(Params, TEXT("types"), OutParams.TypeFilters);

	OutParams.CatalogType = CanonicalizeCatalogType(NormalizeToken(OutParams.CatalogType));
	OutParams.SearchMode = NormalizeToken(OutParams.SearchMode);
	OutParams.Sort = NormalizeToken(OutParams.Sort);
	OutParams.SuggestedFilter = NormalizeToken(OutParams.SuggestedFilter);
	OutParams.TargetUsage = NormalizeToken(OutParams.TargetUsage);

	if (OutParams.CatalogType.IsEmpty())
	{
		OutError = FDiagnostic::Failure(TEXT("catalog_type_required"), TEXT("catalog_type is required."), TEXT("candidate_search"));
		return false;
	}

	return true;
}

bool ExecuteSearch(const FSearchParams& Params, FSearchResult& OutResult, FDiagnostic& OutError)
{
	TArray<FCandidate> Candidates;
	if (Params.CatalogType.StartsWith(TEXT("niagara_")))
	{
		if (!CollectNiagaraCandidates(Params, Candidates, OutResult.Warnings, OutError))
		{
			return false;
		}
	}
	else if (Params.CatalogType.StartsWith(TEXT("material_")))
	{
		if (!CollectMaterialCandidates(Params, Candidates, OutResult.Warnings, OutError))
		{
			return false;
		}
	}
	else if (Params.CatalogType.StartsWith(TEXT("blueprint_")))
	{
		if (!CollectBlueprintCandidates(Params, Candidates, OutResult.Warnings, OutError))
		{
			return false;
		}
	}
	else if (Params.CatalogType.Equals(TEXT("actor_component")) || Params.CatalogType.Equals(TEXT("umg_widget")))
	{
		if (!CollectComponentWidgetCandidates(Params, Candidates, OutResult.Warnings, OutError))
		{
			return false;
		}
	}
	else if (Params.CatalogType.Equals(TEXT("placeable_actor")))
	{
		if (!CollectPlaceableActorCandidates(Params, Candidates, OutResult.Warnings, OutError))
		{
			return false;
		}
	}
	else
	{
		OutError = FDiagnostic::Failure(TEXT("catalog_type_unsupported"), FString::Printf(TEXT("unsupported catalog_type '%s'. Supported now: niagara_module, niagara_dynamic_input, niagara_input_value_source, niagara_graph_node, material_node, material_expression, material_function_call, blueprint_node, actor_component, umg_widget, placeable_actor (aliases: level_actor, actor_placeable)."), *Params.CatalogType), TEXT("candidate_resolve"));
		return false;
	}

	OutResult.TotalRaw = Candidates.Num();
	TArray<FCandidate> FilteredCandidates;
	for (FCandidate& Candidate : Candidates)
	{
		if (!Candidate.Identity.IsValid())
		{
			OutError = FDiagnostic::Failure(TEXT("catalog_candidate_identity_missing"), TEXT("The native catalog candidate has no usable identity."), TEXT("candidate_identity"));
			OutError.CandidateName = Candidate.Name;
			OutError.InternalContext = Candidate.Kind + TEXT(":") + Candidate.FullName;
			return false;
		}
		const FString CandidateKind = Candidate.Kind.IsEmpty() ? Candidate.Type : Candidate.Kind;
		if (!ContainsAnyNormalized(Params.TypeFilters, CandidateKind))
		{
			continue;
		}
		const bool bNativePlaceableSurface = Params.CatalogType.Equals(TEXT("placeable_actor"));
		if (!bNativePlaceableSurface && !ContainsAnyNormalized(Params.CategoryFilters, Candidate.CategoryPath))
		{
			continue;
		}
		if (Params.bIncludeNameContext)
		{
			OutResult.NameContextCandidates.Add(Candidate);
		}
		double CandidateScore = 0.0;
		if (!Candidate.bMatchesNativeSearch
			|| (!bNativePlaceableSurface && !TryScoreCandidate(Params, Candidate, CandidateScore))) continue;

		Candidate.Score = CandidateScore;
		FilteredCandidates.Add(Candidate);
	}
	if (!Params.CatalogType.Equals(TEXT("placeable_actor")))
	{
		SortCandidates(Params, FilteredCandidates);
	}
	// Discovery keeps every matched representation until the shared public
	// projection. Exact legacy lookups need one representative per native action.
	if (!Params.CandidateId.IsEmpty() || Params.SearchMode.Equals(TEXT("exact")))
	{
		TArray<UE::UAI::ContentDiscovery::FNameSelectionEntry> IdentityEntries;
		TArray<int32> IdentityIndices;
		for (int32 Index = 0; Index < FilteredCandidates.Num(); ++Index)
		{
			const auto& Candidate = FilteredCandidates[Index];
			IdentityEntries.Add({Candidate.Name, Candidate.Identity.CanonicalKey, {}});
			IdentityIndices.Add(Index);
		}
		TArray<UE::UAI::ContentDiscovery::FNameSelectionGroup> IdentityGroups;
		if (!UE::UAI::ContentDiscovery::TryGroupNameSelectionEntries(
			IdentityEntries, IdentityIndices, IdentityGroups, OutError)) return false;
		TArray<FCandidate> DistinctCandidates;
		DistinctCandidates.Reserve(IdentityGroups.Num());
		for (const auto& Group : IdentityGroups)
			DistinctCandidates.Add(MoveTemp(FilteredCandidates[Group.EntryIndices[0]]));
		FilteredCandidates = MoveTemp(DistinctCandidates);
	}

	if (!Params.CandidateId.IsEmpty())
	{
		TArray<UeAgentAuthoringIdentity::FCanonicalIdentity> Identities;
		for (const FCandidate& Candidate : FilteredCandidates)
		{
			Identities.Add(Candidate.Identity);
		}
		OutResult.bHasTargetResolution = true;
		if (UeAgentAuthoringCatalogCandidates::ResolveExactCandidate(
				FString(),
				Params.CandidateId,
				Identities,
				OutResult.TargetResolution)
			&& FilteredCandidates.IsValidIndex(
				OutResult.TargetResolution.ResolvedIndex))
		{
			OutResult.MatchingCandidates.Add(FilteredCandidates[
				OutResult.TargetResolution.ResolvedIndex]);
		}
	}
	else
	{
		OutResult.MatchingCandidates = MoveTemp(FilteredCandidates);
		if (Params.SearchMode.Equals(TEXT("exact")))
		{
			TArray<UeAgentAuthoringIdentity::FCanonicalIdentity> Identities;
			for (const FCandidate& Candidate : OutResult.MatchingCandidates)
			{
				Identities.Add(Candidate.Identity);
			}
			OutResult.bHasTargetResolution = true;
			if (UeAgentAuthoringCatalogCandidates::ResolveExactCandidate(
					Params.SearchText,
					FString(),
					Identities,
					OutResult.TargetResolution))
			{
				const int32 ResolvedIndex =
					OutResult.TargetResolution.ResolvedIndex;
				if (OutResult.MatchingCandidates.IsValidIndex(ResolvedIndex))
				{
					const FCandidate ResolvedCandidate =
						OutResult.MatchingCandidates[ResolvedIndex];
					OutResult.MatchingCandidates = {ResolvedCandidate};
				}
			}
		}
	}
	OutResult.TotalMatching = OutResult.MatchingCandidates.Num();
	const bool bRequestsCandidateDetail = !Params.CandidateId.IsEmpty()
		|| Params.SearchMode.Equals(TEXT("exact"), ESearchCase::IgnoreCase);
	if (bRequestsCandidateDetail && OutResult.MatchingCandidates.Num() == 1)
	{
		TSharedPtr<FJsonObject> Seed;
		TSharedPtr<FJsonObject> Placement;
		if (!BuildAuthoringProjection(OutResult.MatchingCandidates[0], Seed, Placement, OutError)) return false;
	}

	return true;
}

}
