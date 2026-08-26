// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"

#include "Algo/Sort.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchAuthoring.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringCatalogCandidates.h"

namespace UeAgentPaletteSearch
{
FSearchParams::FSearchParams()
	: SearchMode(TEXT("fuzzy"))
	, Sort(TEXT("ui_order"))
	, SuggestedFilter(TEXT("all"))
	, bLibraryOnly(true)
	, bIncludeItems(true)
	, bIncludeGroups(true)
	, bIncludeCategoryTree(true)
	, bContextSensitive(true)
	, bIncludeDebugFields(false)
	, Limit(5)
	, Offset(0)
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
	FString& OutError)
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
	return UeAgentAuthoringCatalogCandidates::BuildIdentity(Input, InOutCandidate.Identity, OutError);
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

	static void ReadIntField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, int32& InOutValue)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		double Value = 0.0;
		if (Obj->TryGetNumberField(FieldName, Value))
		{
			InOutValue = FMath::RoundToInt(Value);
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

	static void SplitSearchTerms(const FString& SearchText, TArray<FString>& OutTerms)
	{
		TArray<FString> RawTerms;
		SearchText.ParseIntoArrayWS(RawTerms);
		for (const FString& Term : RawTerms)
		{
			const FString NormalizedTerm = NormalizeToken(Term);
			if (!NormalizedTerm.IsEmpty())
			{
				OutTerms.Add(NormalizedTerm);
			}
		}
	}

	static bool FieldMatchesExact(const TArray<FString>& Fields, const FString& Needle)
	{
		for (const FString& Field : Fields)
		{
			if (NormalizeToken(Field).Equals(Needle))
			{
				return true;
			}
		}
		return false;
	}

	static bool FieldMatchesPrefix(const TArray<FString>& Fields, const FString& Needle)
	{
		for (const FString& Field : Fields)
		{
			if (NormalizeToken(Field).StartsWith(Needle))
			{
				return true;
			}
		}
		return false;
	}

	static bool FieldMatchesContains(const TArray<FString>& Fields, const FString& Needle)
	{
		for (const FString& Field : Fields)
		{
			if (NormalizeToken(Field).Contains(Needle))
			{
				return true;
			}
		}
		return false;
	}

	static bool CandidateMatchesSearch(const FSearchParams& Params, const FCandidate& Candidate)
	{
		const FString SearchText = NormalizeToken(Params.SearchText);
		if (SearchText.IsEmpty())
		{
			return true;
		}

		const TArray<FString> Fields = {
			Candidate.Name,
			Candidate.FullName,
			Candidate.Kind,
			Candidate.DisplayName,
			Candidate.CategoryPath,
			Candidate.Keywords,
			Candidate.Tooltip,
			Candidate.SearchKey,
			Candidate.GetStableId(),
			Candidate.PackagePath,
			Candidate.ObjectPath
		};

		const FString SearchMode = NormalizeToken(Params.SearchMode);
		if (SearchMode.Equals(TEXT("exact")))
		{
			return FieldMatchesExact(Fields, SearchText);
		}
		if (SearchMode.Equals(TEXT("prefix")))
		{
			return FieldMatchesPrefix(Fields, SearchText);
		}
		if (SearchMode.Equals(TEXT("contains")))
		{
			return FieldMatchesContains(Fields, SearchText);
		}

		TArray<FString> Terms;
		SplitSearchTerms(SearchText, Terms);
		if (Terms.IsEmpty())
		{
			return true;
		}

		FString Haystack;
		for (const FString& Field : Fields)
		{
			Haystack += TEXT(" ");
			Haystack += NormalizeToken(Field);
		}

		for (const FString& Term : Terms)
		{
			if (!Haystack.Contains(Term))
			{
				return false;
			}
		}
		return true;
	}

	static double ComputeScore(const FSearchParams& Params, const FCandidate& Candidate)
	{
		const FString SearchText = NormalizeToken(Params.SearchText);
		if (SearchText.IsEmpty())
		{
			return 0.0;
		}

		const FString Name = NormalizeToken(Candidate.Name.IsEmpty() ? Candidate.DisplayName : Candidate.Name);
		const FString Category = NormalizeToken(Candidate.CategoryPath);
		const FString Keywords = NormalizeToken(Candidate.Keywords);
		double Score = 0.0;
		if (Name.Equals(SearchText))
		{
			Score += 100.0;
		}
		if (Name.StartsWith(SearchText))
		{
			Score += 60.0;
		}
		if (Name.Contains(SearchText))
		{
			Score += 35.0;
		}
		if (Keywords.Contains(SearchText))
		{
			Score += 15.0;
		}
		if (Category.Contains(SearchText))
		{
			Score += 8.0;
		}
		if (Candidate.bSuggested)
		{
			Score += 3.0;
		}
		return Score;
	}

	static TSharedPtr<FJsonValue> MakeStringValue(const FString& Value)
	{
		return MakeShared<FJsonValueString>(Value);
	}

	static TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeStringValue(Value));
		}
		return JsonValues;
	}

	static TArray<FString> SplitCategoryPath(const FString& CategoryPath)
	{
		TArray<FString> Parts;
		FString NormalizedPath = CategoryPath;
		NormalizedPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		NormalizedPath.ReplaceInline(TEXT("|"), TEXT("/"));
		NormalizedPath.ParseIntoArray(Parts, TEXT("/"), true);
		for (FString& Part : Parts)
		{
			Part = Part.TrimStartAndEnd();
		}
		Parts.RemoveAll([](const FString& Part) { return Part.IsEmpty(); });
		return Parts;
	}

	static int32 Utf8Bytes(const FString& Value)
	{
		return FTCHARToUTF8(*Value).Length();
	}

	static FString TruncateCell(const FString& Source, const int32 MaxCodepoints, const int32 MaxBytes, bool& bOutTruncated)
	{
		FString Result;
		Result.Reserve(FMath::Min(Source.Len(), MaxCodepoints));
		int32 Codepoints = 0;
		for (int32 Index = 0; Index < Source.Len() && Codepoints < MaxCodepoints; ++Codepoints)
		{
			const int32 Start = Index++;
			const uint32 First = static_cast<uint32>(Source[Start]);
			if (First >= 0xD800 && First <= 0xDBFF && Index < Source.Len())
			{
				const uint32 Second = static_cast<uint32>(Source[Index]);
				if (Second >= 0xDC00 && Second <= 0xDFFF) ++Index;
			}
			const FString Candidate = Result + Source.Mid(Start, Index - Start);
			if (Utf8Bytes(Candidate) > MaxBytes)
			{
				bOutTruncated = true;
				break;
			}
			Result = Candidate;
		}
		if (Result.Len() < Source.Len()) bOutTruncated = true;
		return Result;
	}

	static FString SanitizeCell(FString Value, const int32 MaxCodepoints, bool& bOutTruncated)
	{
		Value.ReplaceInline(TEXT("\t"), TEXT(" "));
		Value.ReplaceInline(TEXT("\n"), TEXT(" "));
		Value.ReplaceInline(TEXT("\r"), TEXT(" "));
		return TruncateCell(Value, MaxCodepoints, 2048, bOutTruncated);
	}

	static FString CandidateToTsv(const FCandidate& Candidate, bool& bOutTruncated)
	{
		const FString Name = SanitizeCell(
			Candidate.Name.IsEmpty() ? Candidate.DisplayName : Candidate.Name, 512, bOutTruncated);
		const FString Kind = SanitizeCell(Candidate.Kind, 512, bOutTruncated);
		const FString Category = SanitizeCell(Candidate.CategoryPath, 512, bOutTruncated);
		const FString Summary = SanitizeCell(Candidate.Tooltip, 512, bOutTruncated);
		return FString::Printf(TEXT("%s\t%s\t%s\t%s\t%s"),
			*Candidate.GetPublicId(), *Name, *Kind, *Category, *Summary);
	}

	static TArray<TSharedPtr<FJsonValue>> BuildGroupJson(const TArray<FCandidate>& Candidates)
	{
		TMap<FString, int32> CountsBySection;
		for (const FCandidate& Candidate : Candidates)
		{
			const FString Section = Candidate.Section.IsEmpty() ? TEXT("General") : Candidate.Section;
			CountsBySection.FindOrAdd(Section)++;
		}

		TArray<FString> SectionNames;
		CountsBySection.GetKeys(SectionNames);
		SectionNames.Sort();

		TArray<TSharedPtr<FJsonValue>> Groups;
		for (const FString& SectionName : SectionNames)
		{
			TSharedRef<FJsonObject> GroupObj = MakeShared<FJsonObject>();
			GroupObj->SetStringField(TEXT("section"), SectionName);
			GroupObj->SetNumberField(TEXT("count"), CountsBySection[SectionName]);
			Groups.Add(MakeShared<FJsonValueObject>(GroupObj));
		}
		return Groups;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildCategoryTreeJson(const TArray<FCandidate>& Candidates)
	{
		TMap<FString, int32> CountsByPath;
		for (const FCandidate& Candidate : Candidates)
		{
			const TArray<FString> Parts = SplitCategoryPath(Candidate.CategoryPath);
			FString RunningPath;
			for (const FString& Part : Parts)
			{
				RunningPath = RunningPath.IsEmpty() ? Part : RunningPath + TEXT("/") + Part;
				CountsByPath.FindOrAdd(RunningPath)++;
			}
		}

		TArray<FString> Paths;
		CountsByPath.GetKeys(Paths);
		Paths.Sort();

		TArray<TSharedPtr<FJsonValue>> Categories;
		for (const FString& Path : Paths)
		{
			TSharedRef<FJsonObject> CategoryObj = MakeShared<FJsonObject>();
			CategoryObj->SetStringField(TEXT("path"), Path);
			CategoryObj->SetArrayField(TEXT("parts"), MakeStringArray(SplitCategoryPath(Path)));
			CategoryObj->SetNumberField(TEXT("count"), CountsByPath[Path]);
			Categories.Add(MakeShared<FJsonValueObject>(CategoryObj));
		}
		return Categories;
	}

	static TSharedRef<FJsonObject> BuildQueryJson(const FSearchParams& Params)
	{
		TSharedRef<FJsonObject> QueryObj = MakeShared<FJsonObject>();
		QueryObj->SetStringField(TEXT("text"), Params.SearchText);
		QueryObj->SetStringField(TEXT("search_mode"), Params.SearchMode);
		QueryObj->SetStringField(TEXT("target_usage"), Params.TargetUsage);
		QueryObj->SetStringField(TEXT("suggested_filter"), Params.SuggestedFilter);
		QueryObj->SetBoolField(TEXT("library_only"), Params.bLibraryOnly);
		QueryObj->SetArrayField(TEXT("source"), MakeStringArray(Params.SourceFilters));
		QueryObj->SetArrayField(TEXT("categories"), MakeStringArray(Params.CategoryFilters));
		QueryObj->SetArrayField(TEXT("types"), MakeStringArray(Params.TypeFilters));
		TSharedRef<FJsonObject> ContextObj = MakeShared<FJsonObject>();
		ContextObj->SetStringField(TEXT("asset_path"), Params.AssetPath);
		ContextObj->SetStringField(TEXT("graph_name"), Params.GraphName);
		ContextObj->SetStringField(TEXT("node_guid"), Params.NodeGuid);
		ContextObj->SetStringField(TEXT("pin_guid"), Params.PinGuid);
		ContextObj->SetStringField(TEXT("pin_name"), Params.PinName);
		ContextObj->SetStringField(TEXT("candidate_id"), Params.CandidateId);
		QueryObj->SetObjectField(TEXT("context"), ContextObj);
		TSharedRef<FJsonObject> ViewObj = MakeShared<FJsonObject>();
		ViewObj->SetBoolField(TEXT("include_debug_fields"), Params.bIncludeDebugFields);
		QueryObj->SetObjectField(TEXT("view"), ViewObj);
		return QueryObj;
	}

	static TSharedRef<FJsonObject> BuildCountsJson(const FSearchParams& Params, const FSearchResult& Result)
	{
		TSharedRef<FJsonObject> CountsObj = MakeShared<FJsonObject>();
		const int32 ReturnedCount = FMath::Clamp(Result.TotalMatching - Params.Offset, 0, Params.Limit);
		CountsObj->SetNumberField(TEXT("total_raw"), Result.TotalRaw);
		CountsObj->SetNumberField(TEXT("total_matching"), Result.TotalMatching);
		CountsObj->SetNumberField(TEXT("returned"), ReturnedCount);
		CountsObj->SetNumberField(TEXT("catalog_total_count"), Result.TotalRaw);
		CountsObj->SetNumberField(TEXT("search_match_count"), Result.TotalMatching);
		CountsObj->SetNumberField(TEXT("returned_count"), ReturnedCount);
		return CountsObj;
	}

	static TSharedRef<FJsonObject> BuildPageJson(const FSearchParams& Params, const FSearchResult& Result)
	{
		TSharedRef<FJsonObject> PageObj = MakeShared<FJsonObject>();
		PageObj->SetNumberField(TEXT("offset"), Params.Offset);
		PageObj->SetNumberField(TEXT("limit"), Params.Limit);
		PageObj->SetStringField(TEXT("sort"), Params.Sort);
		PageObj->SetBoolField(TEXT("has_more"), Params.Offset + Params.Limit < Result.TotalMatching);
		return PageObj;
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

	static TSharedRef<FJsonObject> BuildCandidateJson(const FCandidate& Candidate)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("candidate_id"), Candidate.GetPublicId());
		Result->SetStringField(TEXT("kind"), Candidate.Kind);
		Result->SetStringField(TEXT("name"), Candidate.Name.IsEmpty() ? Candidate.DisplayName : Candidate.Name);
		Result->SetStringField(TEXT("full_name"), Candidate.FullName);
		return Result;
	}
}

bool ParseSearchParams(const TSharedPtr<FJsonObject>& Params, FSearchParams& OutParams, FString& OutError)
{
	if (!Params.IsValid())
	{
		OutError = TEXT("params object is required.");
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
	ReadBoolField(Params, TEXT("include_items"), OutParams.bIncludeItems);
	ReadBoolField(Params, TEXT("include_groups"), OutParams.bIncludeGroups);
	ReadBoolField(Params, TEXT("include_category_tree"), OutParams.bIncludeCategoryTree);
	ReadBoolField(Params, TEXT("context_sensitive"), OutParams.bContextSensitive);
	ReadBoolField(Params, TEXT("include_debug_fields"), OutParams.bIncludeDebugFields);
	ReadIntField(Params, TEXT("limit"), OutParams.Limit);
	ReadIntField(Params, TEXT("offset"), OutParams.Offset);

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
		ReadBoolField(*ViewObjPtr, TEXT("include_items"), OutParams.bIncludeItems);
		ReadBoolField(*ViewObjPtr, TEXT("include_groups"), OutParams.bIncludeGroups);
		ReadBoolField(*ViewObjPtr, TEXT("include_category_tree"), OutParams.bIncludeCategoryTree);
		ReadBoolField(*ViewObjPtr, TEXT("include_debug_fields"), OutParams.bIncludeDebugFields);
	}

	const TSharedPtr<FJsonObject>* PageObjPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("page"), PageObjPtr) && PageObjPtr && PageObjPtr->IsValid())
	{
		ReadIntField(*PageObjPtr, TEXT("limit"), OutParams.Limit);
		ReadIntField(*PageObjPtr, TEXT("offset"), OutParams.Offset);
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
	if (OutParams.Limit < 1 || OutParams.Limit > 20)
	{
		OutError = TEXT("catalog_page_limit_exceeded");
		return false;
	}
	OutParams.Offset = FMath::Max(0, OutParams.Offset);

	if (OutParams.CatalogType.IsEmpty())
	{
		OutError = TEXT("catalog_type is required.");
		return false;
	}

	return true;
}

bool ExecuteSearch(const FSearchParams& Params, FSearchResult& OutResult, FString& OutError)
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
		OutError = FString::Printf(TEXT("unsupported catalog_type '%s'. Supported now: niagara_module, niagara_dynamic_input, niagara_input_value_source, niagara_graph_node, material_node, material_expression, material_function_call, blueprint_node, actor_component, umg_widget, placeable_actor (aliases: level_actor, actor_placeable)."), *Params.CatalogType);
		return false;
	}

	OutResult.TotalRaw = Candidates.Num();
	TArray<FCandidate> FilteredCandidates;
	TSet<FString> SeenCanonicalKeys;
	for (FCandidate& Candidate : Candidates)
	{
		if (!Candidate.Identity.IsValid())
		{
			OutError = FString::Printf(TEXT("catalog_candidate_identity_missing:%s:%s"), *Candidate.Kind, *Candidate.FullName);
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
		if (!bNativePlaceableSurface && !CandidateMatchesSearch(Params, Candidate))
		{
			continue;
		}
		FString CanonicalKey = Candidate.Identity.CanonicalKey;
		CanonicalKey.ToLowerInline();
		if (SeenCanonicalKeys.Contains(CanonicalKey))
		{
			continue;
		}
		SeenCanonicalKeys.Add(MoveTemp(CanonicalKey));

		Candidate.Score = ComputeScore(Params, Candidate);
		FilteredCandidates.Add(Candidate);
	}
	if (!Params.CatalogType.Equals(TEXT("placeable_actor")))
	{
		SortCandidates(Params, FilteredCandidates);
	}

	if (!Params.CandidateId.IsEmpty())
	{
		TArray<int32> PublicIdMatches;
		for (int32 Index = 0; Index < FilteredCandidates.Num(); ++Index)
		{
			if (FilteredCandidates[Index].GetPublicId().Equals(
					Params.CandidateId, ESearchCase::IgnoreCase))
			{
				PublicIdMatches.Add(Index);
			}
		}
		OutResult.bHasTargetResolution = true;
		if (PublicIdMatches.Num() == 1)
		{
			OutResult.TargetResolution.Status = TEXT("resolved");
			OutResult.TargetResolution.ErrorCode = TEXT("target_resolved");
			OutResult.TargetResolution.ResolvedIndex = PublicIdMatches[0];
			OutResult.MatchingCandidates.Add(
				FilteredCandidates[PublicIdMatches[0]]);
		}
		else if (PublicIdMatches.Num() > 1)
		{
			OutResult.TargetResolution.Status = TEXT("failed");
			OutResult.TargetResolution.ErrorCode = TEXT("target_ambiguous");
		}
		else
		{
			UeAgentAuthoringIdentity::FCanonicalIdentity ParsedIdentity;
			FString ParseError;
			UeAgentAuthoringTargetLocator::FTargetQuery Query;
			if (UeAgentAuthoringIdentity::ParseCanonicalKey(
					Params.CandidateId, ParsedIdentity, ParseError)
				&& ParsedIdentity.ModelId == TEXT("catalog_candidate_identity"))
			{
				Query.ModelId = ParsedIdentity.ModelId;
				Query.TargetKind = ParsedIdentity.TargetKind;
				Query.CanonicalKey = Params.CandidateId;
			}
			else
			{
				Query.ModelId = TEXT("catalog_candidate_identity");
				Query.CanonicalKey = Params.CandidateId;
			}
			TArray<UeAgentAuthoringIdentity::FCanonicalIdentity> Identities;
			for (const FCandidate& Candidate : FilteredCandidates)
			{
				Identities.Add(Candidate.Identity);
			}
			if (UeAgentAuthoringTargetLocator::ResolveExact(
					Query, Identities, OutResult.TargetResolution)
				&& FilteredCandidates.IsValidIndex(
					OutResult.TargetResolution.ResolvedIndex))
			{
				OutResult.MatchingCandidates.Add(FilteredCandidates[
					OutResult.TargetResolution.ResolvedIndex]);
			}
		}
	}
	else
	{
		OutResult.MatchingCandidates = MoveTemp(FilteredCandidates);
		if (Params.SearchMode.Equals(TEXT("exact")))
		{
			TArray<UeAgentAuthoringIdentity::FCanonicalIdentity> Identities;
			for (const FCandidate& Candidate : OutResult.MatchingCandidates) Identities.Add(Candidate.Identity);
			static const TCHAR* SelectorOrder[] = { TEXT("name"), TEXT("display_name"), TEXT("full_name"), TEXT("search_key") };
			for (const TCHAR* SelectorName : SelectorOrder)
			{
				UeAgentAuthoringTargetLocator::FTargetQuery Query;
				Query.ModelId = TEXT("catalog_candidate_identity");
				Query.Selectors.Add(SelectorName, Params.SearchText);
				OutResult.bHasTargetResolution = true;
				if (UeAgentAuthoringTargetLocator::ResolveExact(Query, Identities, OutResult.TargetResolution))
				{
					const int32 ResolvedIndex = OutResult.TargetResolution.ResolvedIndex;
					if (OutResult.MatchingCandidates.IsValidIndex(ResolvedIndex))
					{
						const FCandidate ResolvedCandidate = OutResult.MatchingCandidates[ResolvedIndex];
						OutResult.MatchingCandidates = { ResolvedCandidate };
					}
					break;
				}
				if (OutResult.TargetResolution.ErrorCode != TEXT("target_not_found")) break;
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

TSharedRef<FJsonObject> BuildSearchResponse(const FSearchParams& Params, const FSearchResult& Result)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("uai.node_catalog.search.v3"));
	Root->SetStringField(TEXT("catalog_type"), Params.CatalogType);
	Root->SetObjectField(TEXT("query"), BuildQueryJson(Params));
	Root->SetObjectField(TEXT("counts"), BuildCountsJson(Params, Result));
	Root->SetObjectField(TEXT("page"), BuildPageJson(Params, Result));
	if (Result.bHasTargetResolution)
	{
		Root->SetObjectField(TEXT("candidate_resolution"), Result.TargetResolution.ToJsonObject());
	}

	if (Params.bIncludeGroups)
	{
		Root->SetArrayField(TEXT("groups"), BuildGroupJson(Result.MatchingCandidates));
	}
	if (Params.bIncludeCategoryTree)
	{
		Root->SetArrayField(TEXT("category_tree"), BuildCategoryTreeJson(Result.MatchingCandidates));
	}
	const bool bRequestsCandidateDetail =
		!Params.CandidateId.IsEmpty() || Params.SearchMode.Equals(TEXT("exact"), ESearchCase::IgnoreCase);
	if (Params.bIncludeItems && !bRequestsCandidateDetail)
	{
		constexpr int32 MaxRows = 20;
		constexpr int32 MaxBytes = 16384;
		FString Tsv = TEXT("candidate_id\tname\tkind\tcategory\tsummary");
		bool bTruncated = false;
		int32 RowsEmitted = 0;
		const int32 LastIndex = FMath::Min(Result.MatchingCandidates.Num(), Params.Offset + Params.Limit);
		for (int32 Index = Params.Offset; Index < LastIndex; ++Index)
		{
			bool bRowTruncated = false;
			const FString Row = TEXT("\n") + CandidateToTsv(Result.MatchingCandidates[Index], bRowTruncated);
			if (Utf8Bytes(Tsv) + Utf8Bytes(Row) > MaxBytes)
			{
				bTruncated = true;
				break;
			}
			Tsv += Row;
			bTruncated |= bRowTruncated;
			++RowsEmitted;
		}
		bTruncated |= Params.Offset + RowsEmitted < Result.MatchingCandidates.Num();
		Root->SetStringField(TEXT("items_tsv"), Tsv);
		TSharedRef<FJsonObject> Budget = MakeShared<FJsonObject>();
		Budget->SetNumberField(TEXT("max_rows"), MaxRows);
		Budget->SetNumberField(TEXT("max_bytes"), MaxBytes);
		Budget->SetNumberField(TEXT("max_cell_bytes"), 2048);
		Budget->SetNumberField(TEXT("tooltip_max_codepoints"), 512);
		Budget->SetNumberField(TEXT("rows_emitted"), RowsEmitted);
		Budget->SetNumberField(TEXT("bytes_emitted"), Utf8Bytes(Tsv));
		Budget->SetBoolField(TEXT("truncated"), bTruncated);
		Root->SetObjectField(TEXT("table_budget"), Budget);
	}
	if (bRequestsCandidateDetail && Result.MatchingCandidates.Num() == 1)
	{
		Root->SetObjectField(TEXT("candidate"), BuildCandidateJson(Result.MatchingCandidates[0]));
	}
	TArray<FString> ResponseWarnings = Result.Warnings;
	if (bRequestsCandidateDetail && Result.MatchingCandidates.Num() == 1)
	{
		TSharedPtr<FJsonObject> Seed;
		TSharedPtr<FJsonObject> Placement;
		FString ProjectionError;
		if (!BuildAuthoringProjection(Result.MatchingCandidates[0], Seed, Placement, ProjectionError))
		{
			ResponseWarnings.Add(ProjectionError);
		}
		else if (Seed.IsValid() && Placement.IsValid())
		{
			Root->SetObjectField(TEXT("seed"), Seed);
			Root->SetObjectField(TEXT("placement"), Placement);
		}
	}
	Root->SetArrayField(TEXT("warnings"), MakeStringArray(ResponseWarnings));
	return Root;
}

TSharedRef<FJsonObject> BuildCategoriesResponse(const FSearchParams& Params, const FSearchResult& Result)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("uai.node_catalog.categories.v3"));
	Root->SetStringField(TEXT("catalog_type"), Params.CatalogType);
	Root->SetObjectField(TEXT("query"), BuildQueryJson(Params));
	Root->SetObjectField(TEXT("counts"), BuildCountsJson(Params, Result));
	Root->SetArrayField(TEXT("groups"), BuildGroupJson(Result.MatchingCandidates));
	Root->SetArrayField(TEXT("category_tree"), BuildCategoryTreeJson(Result.MatchingCandidates));
	Root->SetArrayField(TEXT("warnings"), MakeStringArray(Result.Warnings));
	return Root;
}

}
