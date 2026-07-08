// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/PaletteSearch/UeAgentPaletteSearchAuthoring.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UeAgentPaletteSearch
{
namespace
{
	static void SetIdentityFields(const FCandidate& Candidate, const TSharedRef<FJsonObject>& Obj)
	{
		Obj->SetStringField(TEXT("name"), Candidate.Name);
	}

	static FString PathSeedFromCandidate(const FCandidate& Candidate)
	{
		if (!Candidate.ObjectPath.IsEmpty())
		{
			return Candidate.ObjectPath;
		}
		if (!Candidate.PackagePath.IsEmpty())
		{
			return Candidate.PackagePath;
		}
		if (Candidate.FullName.StartsWith(TEXT("/")))
		{
			return Candidate.FullName;
		}
		return FString();
	}

	static bool TrySplitOwnerMemberPath(const FString& FullName, FString& OutPath, FString& OutMemberName)
	{
		FString Left;
		FString Right;
		if (!FullName.Split(TEXT(":"), &Left, &Right))
		{
			return false;
		}
		Left.TrimStartAndEndInline();
		Right.TrimStartAndEndInline();
		if (Left.IsEmpty() || Right.IsEmpty())
		{
			return false;
		}
		OutPath = Left;
		OutMemberName = Right;
		return true;
	}

	static bool ShouldPreferPathSeed(const FCandidate& Candidate)
	{
		return
			Candidate.Kind.Equals(TEXT("material_function_call")) ||
			Candidate.Kind.Equals(TEXT("material_expression")) ||
			Candidate.Kind.Equals(TEXT("actor_component")) ||
			Candidate.Kind.Equals(TEXT("umg_widget")) ||
			Candidate.Kind.Equals(TEXT("placeable_actor")) ||
			Candidate.Kind.Equals(TEXT("niagara_module")) ||
			Candidate.Kind.Equals(TEXT("niagara_dynamic_input")) ||
			Candidate.Kind.Equals(TEXT("niagara_graph_function")) ||
			Candidate.Kind.StartsWith(TEXT("blueprint_"));
	}

	static void SetMinimalIdentityFields(const FCandidate& Candidate, const TSharedRef<FJsonObject>& Obj)
	{
		FString OwnerPath;
		FString MemberName;
		if (Candidate.Kind.Equals(TEXT("blueprint_call_function")) && TrySplitOwnerMemberPath(Candidate.FullName, OwnerPath, MemberName))
		{
			Obj->SetStringField(TEXT("path"), OwnerPath);
			Obj->SetStringField(TEXT("function_name"), MemberName);
			return;
		}

		if (ShouldPreferPathSeed(Candidate))
		{
			const FString Path = PathSeedFromCandidate(Candidate);
			if (!Path.IsEmpty())
			{
				Obj->SetStringField(TEXT("path"), Path);
				return;
			}
		}

		SetIdentityFields(Candidate, Obj);
	}

	static bool CatalogNameEquals(const FString& A, const FString& B)
	{
		return A.TrimStartAndEnd().Equals(B.TrimStartAndEnd(), ESearchCase::IgnoreCase);
	}

	static TSharedRef<FJsonObject> MakePosition(int32 X = 0, int32 Y = 0)
	{
		TSharedRef<FJsonObject> PositionObj = MakeShared<FJsonObject>();
		PositionObj->SetNumberField(TEXT("x"), X);
		PositionObj->SetNumberField(TEXT("y"), Y);
		return PositionObj;
	}

	static TSharedRef<FJsonObject> MakeSpaceContract(const FString& Subject, int32 Dimension, const FString& Frame, const FString& Semantic)
	{
		TSharedRef<FJsonObject> ContractObj = MakeShared<FJsonObject>();
		ContractObj->SetStringField(TEXT("subject"), Subject);
		ContractObj->SetNumberField(TEXT("dimension"), Dimension);
		ContractObj->SetStringField(TEXT("Fsemantic"), Frame);
		ContractObj->SetStringField(TEXT("Fwrite"), Frame);
		ContractObj->SetStringField(TEXT("Fread"), Frame);
		ContractObj->SetStringField(TEXT("Fverify"), Frame);
		ContractObj->SetStringField(TEXT("conversion_chain"), FString::Printf(TEXT("%s already expressed in %s; verify by UE readback after apply."), *Semantic, *Frame));
		return ContractObj;
	}

	static TSharedRef<FJsonObject> MakeLayout(const FString& Space, const FString& Subject)
	{
		TSharedRef<FJsonObject> LayoutObj = MakeShared<FJsonObject>();
		LayoutObj->SetStringField(TEXT("space"), Space);
		LayoutObj->SetObjectField(TEXT("space_contract"), MakeSpaceContract(Subject, 2, Space, TEXT("graph editor layout coordinates")));
		LayoutObj->SetObjectField(TEXT("position"), MakePosition());
		return LayoutObj;
	}

	static TArray<TSharedPtr<FJsonValue>> EmptyArray()
	{
		return TArray<TSharedPtr<FJsonValue>>();
	}

	static TArray<TSharedPtr<FJsonValue>> MakeNumberArray(double A, double B, double C)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Add(MakeShared<FJsonValueNumber>(A));
		Values.Add(MakeShared<FJsonValueNumber>(B));
		Values.Add(MakeShared<FJsonValueNumber>(C));
		return Values;
	}

	static TSharedPtr<FJsonObject> MakeRoot(const FString& Route, const FString& Target, const FString& ProfilePath, const TSharedRef<FJsonObject>& Seed)
	{
		TSharedRef<FJsonObject> RootObj = MakeShared<FJsonObject>();
		RootObj->SetStringField(TEXT("schema"), TEXT("uai.catalog_authoring_seed.v1"));
		RootObj->SetStringField(TEXT("route"), Route);
		RootObj->SetStringField(TEXT("target"), Target);
		RootObj->SetStringField(TEXT("profile_path"), ProfilePath);
		RootObj->SetBoolField(TEXT("readback_after_apply"), true);
		RootObj->SetObjectField(TEXT("seed"), Seed);
		return RootObj;
	}

	static FString MakeSafeObjectName(const FString& Name, const FString& Suffix)
	{
		FString Result;
		for (const TCHAR Ch : Name)
		{
			if (FChar::IsAlnum(Ch) || Ch == TEXT('_'))
			{
				Result.AppendChar(Ch);
			}
		}
		if (Result.IsEmpty())
		{
			Result = TEXT("New");
		}
		if (!Suffix.IsEmpty() && !Result.EndsWith(Suffix, ESearchCase::IgnoreCase))
		{
			Result += Suffix;
		}
		return Result;
	}

	static TSharedPtr<FJsonObject> BuildNiagaraModuleSeed(const FCandidate& Candidate)
	{
		TSharedRef<FJsonObject> SeedObj = MakeShared<FJsonObject>();
		SetMinimalIdentityFields(Candidate, SeedObj);
		return MakeRoot(TEXT("niagara_authoring_profile"), TEXT("niagara.modules[]"), TEXT("stages[].modules[]"), SeedObj);
	}

	static TSharedPtr<FJsonObject> BuildNiagaraDynamicInputSeed(const FCandidate& Candidate)
	{
		TSharedRef<FJsonObject> SeedObj = MakeShared<FJsonObject>();
		SetMinimalIdentityFields(Candidate, SeedObj);
		return MakeRoot(TEXT("niagara_authoring_profile"), TEXT("niagara.modules[].inputs[].value"), TEXT("stages[].modules[].inputs[].value"), SeedObj);
	}

	static TSharedPtr<FJsonObject> BuildNiagaraInputValueSourceSeed(const FCandidate& Candidate)
	{
		TSharedRef<FJsonObject> SeedObj = MakeShared<FJsonObject>();
		SetMinimalIdentityFields(Candidate, SeedObj);
		return MakeRoot(TEXT("niagara_authoring_profile"), TEXT("niagara.modules[].inputs[].value"), TEXT("stages[].modules[].inputs[].value"), SeedObj);
	}

	static TSharedPtr<FJsonObject> BuildGraphNodeSeed(const FCandidate& Candidate, const FString& Route, const FString& Target, const FString& ProfilePath, const FString& LayoutSpace)
	{
		TSharedRef<FJsonObject> SeedObj = MakeShared<FJsonObject>();
		SetMinimalIdentityFields(Candidate, SeedObj);
		return MakeRoot(Route, Target, ProfilePath, SeedObj);
	}

	static TSharedRef<FJsonObject> MakeDefaultActorTransform(const FString& ActorId)
	{
		TSharedRef<FJsonObject> TransformObj = MakeShared<FJsonObject>();
		TransformObj->SetStringField(TEXT("space"), TEXT("ue_world"));
		TransformObj->SetObjectField(TEXT("space_contract"), MakeSpaceContract(
			ActorId,
			3,
			TEXT("ue_world"),
			TEXT("default actor spawn transform in UE editor world coordinates")));
		TransformObj->SetArrayField(TEXT("location"), MakeNumberArray(0.0, 0.0, 0.0));
		TransformObj->SetArrayField(TEXT("rotation"), MakeNumberArray(0.0, 0.0, 0.0));
		TransformObj->SetArrayField(TEXT("scale"), MakeNumberArray(1.0, 1.0, 1.0));
		return TransformObj;
	}

	static TSharedPtr<FJsonObject> BuildPlaceableActorSeed(const FCandidate& Candidate)
	{
		TSharedRef<FJsonObject> SeedObj = MakeShared<FJsonObject>();
		SetMinimalIdentityFields(Candidate, SeedObj);
		return MakeRoot(TEXT("level_folder_package"), TEXT("level.actors.items[]"), TEXT("actors/items/<actor_id>/actor.json"), SeedObj);
	}
}

TSharedPtr<FJsonObject> BuildJsonAuthoringPatch(const FCandidate& Candidate)
{
	if (Candidate.Kind.Equals(TEXT("niagara_module")))
	{
		return BuildNiagaraModuleSeed(Candidate);
	}
	if (Candidate.Kind.Equals(TEXT("niagara_dynamic_input")))
	{
		return BuildNiagaraDynamicInputSeed(Candidate);
	}
	if (Candidate.Kind.Equals(TEXT("niagara_input_value_source")))
	{
		return BuildNiagaraInputValueSourceSeed(Candidate);
	}
	if (Candidate.Kind.Equals(TEXT("niagara_graph_function")))
	{
		return BuildGraphNodeSeed(Candidate, TEXT("niagara_script_authoring_profile"), TEXT("niagara_script.graph.nodes[]"), TEXT("graph.nodes[]"), TEXT("niagara_graph_2d"));
	}
	if (Candidate.Kind.Equals(TEXT("material_expression")) || Candidate.Kind.Equals(TEXT("material_function_call")))
	{
		return BuildGraphNodeSeed(Candidate, TEXT("material_authoring_profile"), TEXT("material.graph.nodes[]"), TEXT("material_graph.nodes[]"), TEXT("material_graph_2d"));
	}
	if (Candidate.Kind.StartsWith(TEXT("blueprint_")))
	{
		return BuildGraphNodeSeed(Candidate, TEXT("blueprint_umg_authoring_profile"), TEXT("blueprint.graph.nodes[]"), TEXT("blueprint_graphs[].nodes[]"), TEXT("event_graph_2d"));
	}
	if (Candidate.Kind.Equals(TEXT("actor_component")))
	{
		TSharedRef<FJsonObject> SeedObj = MakeShared<FJsonObject>();
		SetMinimalIdentityFields(Candidate, SeedObj);
		return MakeRoot(TEXT("blueprint_umg_authoring_profile"), TEXT("blueprint.components[]"), TEXT("components[]"), SeedObj);
	}
	if (Candidate.Kind.Equals(TEXT("umg_widget")))
	{
		TSharedRef<FJsonObject> SeedObj = MakeShared<FJsonObject>();
		SetMinimalIdentityFields(Candidate, SeedObj);
		return MakeRoot(TEXT("blueprint_umg_authoring_profile"), TEXT("umg.widget_tree.widgets[]"), TEXT("widget_tree.widgets[]"), SeedObj);
	}
	if (Candidate.Kind.Equals(TEXT("placeable_actor")))
	{
		return BuildPlaceableActorSeed(Candidate);
	}
	return nullptr;
}

bool ResolveCatalogNameToCandidate(
	const FString& CatalogType,
	const FString& Name,
	FCandidate& OutCandidate,
	FString& OutError,
	const FString& AssetPath,
	const FString& GraphName)
{
	const FString TrimmedName = Name.TrimStartAndEnd();
	if (CatalogType.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_catalog_type");
		return false;
	}
	if (TrimmedName.IsEmpty())
	{
		OutError = TEXT("missing_catalog_name");
		return false;
	}

	FSearchParams Params;
	Params.CatalogType = CatalogType;
	Params.SearchText = TrimmedName;
	Params.SearchMode = TEXT("exact");
	Params.Sort = TEXT("score");
	Params.AssetPath = AssetPath;
	Params.GraphName = GraphName;
	Params.Limit = 50;
	Params.bIncludeItems = true;
	Params.bIncludeGroups = false;
	Params.bIncludeCategoryTree = false;
	Params.bIncludeLegacyAuthoring = false;
	Params.bIncludeDebugFields = false;

	FSearchResult Result;
	if (!ExecuteSearch(Params, Result, OutError))
	{
		return false;
	}

	TArray<FCandidate> ExactMatches;
	for (const FCandidate& Candidate : Result.MatchingCandidates)
	{
		if (CatalogNameEquals(Candidate.Name, TrimmedName) ||
			CatalogNameEquals(Candidate.DisplayName, TrimmedName) ||
			CatalogNameEquals(Candidate.FullName, TrimmedName))
		{
			ExactMatches.Add(Candidate);
		}
	}

	if (ExactMatches.IsEmpty())
	{
		OutError = FString::Printf(TEXT("catalog_name_not_found:%s:%s"), *CatalogType, *TrimmedName);
		return false;
	}

	TArray<FCandidate> NameMatches = ExactMatches.FilterByPredicate([&TrimmedName](const FCandidate& Candidate)
	{
		return CatalogNameEquals(Candidate.Name, TrimmedName);
	});
	const TArray<FCandidate>& PreferredMatches = NameMatches.IsEmpty() ? ExactMatches : NameMatches;
	if (PreferredMatches.Num() != 1)
	{
		OutError = FString::Printf(TEXT("catalog_name_ambiguous:%s:%s:%d"), *CatalogType, *TrimmedName, PreferredMatches.Num());
		return false;
	}

	OutCandidate = PreferredMatches[0];
	return true;
}
}
