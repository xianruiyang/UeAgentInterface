// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"

#include "Algo/Sort.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UeAgentPaletteSearch
{
namespace
{
	static FString NormalizeToken(const FString& InValue)
	{
		return InValue.TrimStartAndEnd().ToLower();
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
			Candidate.CanonicalKey,
			Candidate.StableId,
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

	static TSharedRef<FJsonObject> CandidateToJson(const FSearchParams& Params, const FCandidate& Candidate)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		const FString Kind = Candidate.Kind.IsEmpty() ? Candidate.Type : Candidate.Kind;
		const FString Name = Candidate.Name.IsEmpty() ? Candidate.DisplayName : Candidate.Name;
		const FString FullName = Candidate.FullName.IsEmpty() ? Candidate.StableId : Candidate.FullName;
		Obj->SetStringField(TEXT("candidate_id"), Candidate.CandidateId);
		Obj->SetStringField(TEXT("kind"), Kind);
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("full_name"), FullName);
		Obj->SetStringField(TEXT("category_path"), Candidate.CategoryPath);
		Obj->SetArrayField(TEXT("category_parts"), MakeStringArray(SplitCategoryPath(Candidate.CategoryPath)));
		Obj->SetStringField(TEXT("section"), Candidate.Section);
		Obj->SetStringField(TEXT("type"), Kind);
		Obj->SetStringField(TEXT("source_origin"), Candidate.SourceOrigin);
		Obj->SetStringField(TEXT("source_label"), Candidate.SourceLabel);
		Obj->SetStringField(TEXT("write_support"), Candidate.WriteSupport);
		Obj->SetStringField(TEXT("support_reason"), Candidate.SupportReason);
		Obj->SetBoolField(TEXT("in_library"), Candidate.bInLibrary);
		Obj->SetBoolField(TEXT("suggested"), Candidate.bSuggested);
		Obj->SetBoolField(TEXT("supported_in_context"), Candidate.bSupportedInContext);
		Obj->SetNumberField(TEXT("score"), Candidate.Score);
		if (!Candidate.Tooltip.IsEmpty())
		{
			Obj->SetStringField(TEXT("tooltip"), Candidate.Tooltip);
		}
		if (!Candidate.Keywords.IsEmpty())
		{
			Obj->SetStringField(TEXT("keywords"), Candidate.Keywords);
		}
		if (Candidate.JsonAuthoring.IsValid())
		{
			Obj->SetObjectField(TEXT("json_authoring"), Candidate.JsonAuthoring);
		}
		if (Params.bIncludeLegacyAuthoring && Candidate.LegacyAuthoring.IsValid())
		{
			Obj->SetObjectField(TEXT("legacy_authoring"), Candidate.LegacyAuthoring);
			Obj->SetObjectField(TEXT("recommended_authoring"), Candidate.LegacyAuthoring);
		}
		if (Params.bIncludeDebugFields)
		{
			Obj->SetStringField(TEXT("stable_id"), Candidate.StableId);
			Obj->SetStringField(TEXT("canonical_key"), Candidate.CanonicalKey);
			Obj->SetStringField(TEXT("display_name"), Candidate.DisplayName);
			Obj->SetStringField(TEXT("package_path"), Candidate.PackagePath);
			Obj->SetStringField(TEXT("object_path"), Candidate.ObjectPath);
		}
		return Obj;
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
		ViewObj->SetBoolField(TEXT("include_legacy_authoring"), Params.bIncludeLegacyAuthoring);
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
	ReadBoolField(Params, TEXT("include_legacy_authoring"), OutParams.bIncludeLegacyAuthoring);
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
		ReadBoolField(*ViewObjPtr, TEXT("include_legacy_authoring"), OutParams.bIncludeLegacyAuthoring);
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

	OutParams.CatalogType = NormalizeToken(OutParams.CatalogType);
	OutParams.SearchMode = NormalizeToken(OutParams.SearchMode);
	OutParams.Sort = NormalizeToken(OutParams.Sort);
	OutParams.SuggestedFilter = NormalizeToken(OutParams.SuggestedFilter);
	OutParams.TargetUsage = NormalizeToken(OutParams.TargetUsage);
	OutParams.Limit = FMath::Clamp(OutParams.Limit, 1, 200);
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
	else
	{
		OutError = FString::Printf(TEXT("unsupported catalog_type '%s'. Supported now: niagara_module, niagara_dynamic_input, niagara_graph_node, material_node, material_expression, material_function_call, blueprint_node, actor_component, umg_widget."), *Params.CatalogType);
		return false;
	}

	OutResult.TotalRaw = Candidates.Num();
	for (FCandidate& Candidate : Candidates)
	{
		const FString CandidateKind = Candidate.Kind.IsEmpty() ? Candidate.Type : Candidate.Kind;
		if (!Params.CandidateId.IsEmpty() && !Candidate.CandidateId.Equals(Params.CandidateId, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!ContainsAnyNormalized(Params.TypeFilters, CandidateKind))
		{
			continue;
		}
		if (!ContainsAnyNormalized(Params.CategoryFilters, Candidate.CategoryPath))
		{
			continue;
		}
		if (!CandidateMatchesSearch(Params, Candidate))
		{
			continue;
		}

		Candidate.Score = ComputeScore(Params, Candidate);
		OutResult.MatchingCandidates.Add(Candidate);
	}

	OutResult.TotalMatching = OutResult.MatchingCandidates.Num();

	if (Params.Sort.Equals(TEXT("name")))
	{
		Algo::Sort(OutResult.MatchingCandidates, [](const FCandidate& A, const FCandidate& B)
		{
			const FString AName = A.Name.IsEmpty() ? A.DisplayName : A.Name;
			const FString BName = B.Name.IsEmpty() ? B.DisplayName : B.Name;
			return AName < BName;
		});
	}
	else if (Params.Sort.Equals(TEXT("category")))
	{
		Algo::Sort(OutResult.MatchingCandidates, [](const FCandidate& A, const FCandidate& B)
		{
			const FString AName = A.Name.IsEmpty() ? A.DisplayName : A.Name;
			const FString BName = B.Name.IsEmpty() ? B.DisplayName : B.Name;
			if (A.CategoryPath.Equals(B.CategoryPath))
			{
				return AName < BName;
			}
			return A.CategoryPath < B.CategoryPath;
		});
	}
	else if (!Params.SearchText.TrimStartAndEnd().IsEmpty())
	{
		Algo::Sort(OutResult.MatchingCandidates, [](const FCandidate& A, const FCandidate& B)
		{
			if (!FMath::IsNearlyEqual(A.Score, B.Score))
			{
				return A.Score > B.Score;
			}
			return A.UiOrder < B.UiOrder;
		});
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

	if (Params.bIncludeGroups)
	{
		Root->SetArrayField(TEXT("groups"), BuildGroupJson(Result.MatchingCandidates));
	}
	if (Params.bIncludeCategoryTree)
	{
		Root->SetArrayField(TEXT("category_tree"), BuildCategoryTreeJson(Result.MatchingCandidates));
	}
	if (Params.bIncludeItems)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		const int32 LastIndex = FMath::Min(Result.MatchingCandidates.Num(), Params.Offset + Params.Limit);
		for (int32 Index = Params.Offset; Index < LastIndex; ++Index)
		{
			Items.Add(MakeShared<FJsonValueObject>(CandidateToJson(Params, Result.MatchingCandidates[Index])));
		}
		Root->SetArrayField(TEXT("items"), Items);
	}
	Root->SetArrayField(TEXT("warnings"), MakeStringArray(Result.Warnings));
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
