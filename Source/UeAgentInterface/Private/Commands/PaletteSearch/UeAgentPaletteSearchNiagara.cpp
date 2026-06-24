// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchAuthoring.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchNaming.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "NiagaraActions.h"
#include "NiagaraCommon.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraScript.h"

namespace UeAgentPaletteSearch
{
namespace
{
	static FString NormalizeFilterToken(const FString& InValue)
	{
		return InValue.TrimStartAndEnd().ToLower();
	}

	static bool IsNiagaraModuleCatalog(const FString& CatalogType)
	{
		return CatalogType.Equals(TEXT("niagara_module"));
	}

	static bool IsNiagaraDynamicInputCatalog(const FString& CatalogType)
	{
		return CatalogType.Equals(TEXT("niagara_dynamic_input")) || CatalogType.Equals(TEXT("niagara_input_value_source"));
	}

	static bool IsNiagaraGraphNodeCatalog(const FString& CatalogType)
	{
		return CatalogType.Equals(TEXT("niagara_graph_node")) || CatalogType.Equals(TEXT("niagara_function"));
	}

	static FString GetCanonicalNiagaraCandidateType(const FString& CatalogType)
	{
		if (IsNiagaraDynamicInputCatalog(CatalogType))
		{
			return TEXT("niagara_dynamic_input");
		}
		if (IsNiagaraGraphNodeCatalog(CatalogType))
		{
			return TEXT("niagara_graph_function");
		}
		return TEXT("niagara_module");
	}

	static bool ParseNiagaraTargetUsage(const FString& Text, ENiagaraScriptUsage& OutUsage)
	{
		const FString Token = NormalizeFilterToken(Text);
		if (Token.IsEmpty())
		{
			return false;
		}

		static const TPair<const TCHAR*, ENiagaraScriptUsage> UsageMap[] = {
			{ TEXT("function"), ENiagaraScriptUsage::Function },
			{ TEXT("module"), ENiagaraScriptUsage::Module },
			{ TEXT("dynamic_input"), ENiagaraScriptUsage::DynamicInput },
			{ TEXT("particle_spawn"), ENiagaraScriptUsage::ParticleSpawnScript },
			{ TEXT("particle_spawn_script"), ENiagaraScriptUsage::ParticleSpawnScript },
			{ TEXT("spawn"), ENiagaraScriptUsage::ParticleSpawnScript },
			{ TEXT("particle_update"), ENiagaraScriptUsage::ParticleUpdateScript },
			{ TEXT("particle_update_script"), ENiagaraScriptUsage::ParticleUpdateScript },
			{ TEXT("update"), ENiagaraScriptUsage::ParticleUpdateScript },
			{ TEXT("particle_event"), ENiagaraScriptUsage::ParticleEventScript },
			{ TEXT("event"), ENiagaraScriptUsage::ParticleEventScript },
			{ TEXT("particle_simulation_stage"), ENiagaraScriptUsage::ParticleSimulationStageScript },
			{ TEXT("simulation_stage"), ENiagaraScriptUsage::ParticleSimulationStageScript },
			{ TEXT("gpu_compute"), ENiagaraScriptUsage::ParticleGPUComputeScript },
			{ TEXT("emitter_spawn"), ENiagaraScriptUsage::EmitterSpawnScript },
			{ TEXT("emitter_update"), ENiagaraScriptUsage::EmitterUpdateScript },
			{ TEXT("system_spawn"), ENiagaraScriptUsage::SystemSpawnScript },
			{ TEXT("system_update"), ENiagaraScriptUsage::SystemUpdateScript },
		};

		for (const TPair<const TCHAR*, ENiagaraScriptUsage>& Pair : UsageMap)
		{
			if (Token.Equals(Pair.Key))
			{
				OutUsage = Pair.Value;
				return true;
			}
		}
		return false;
	}

	static FString SourceToString(EScriptSource Source)
	{
		switch (Source)
		{
		case EScriptSource::Niagara:
			return TEXT("niagara");
		case EScriptSource::Game:
			return TEXT("game");
		case EScriptSource::Plugins:
			return TEXT("plugins");
		case EScriptSource::Developer:
			return TEXT("developer");
		default:
			return TEXT("unknown");
		}
	}

	static bool IsSourceAllowed(EScriptSource Source, const TArray<FString>& SourceFilters)
	{
		TArray<FString> Filters = SourceFilters;
		if (Filters.IsEmpty())
		{
			Filters = { TEXT("niagara"), TEXT("engine"), TEXT("game"), TEXT("project"), TEXT("plugins") };
		}

		const FString SourceName = SourceToString(Source);
		for (const FString& Filter : Filters)
		{
			const FString Token = NormalizeFilterToken(Filter);
			if (Token.Equals(SourceName))
			{
				return true;
			}
			if (Source == EScriptSource::Niagara && Token.Equals(TEXT("engine")))
			{
				return true;
			}
			if (Source == EScriptSource::Game && Token.Equals(TEXT("project")))
			{
				return true;
			}
		}
		return false;
	}

	static FNiagaraEditorUtilities::FGetFilteredScriptAssetsOptions::ESuggestedFiltering ParseSuggestedFilter(const FString& Text)
	{
		const FString Token = NormalizeFilterToken(Text);
		if (Token.Equals(TEXT("only_suggested")) || Token.Equals(TEXT("suggested_only")) || Token.Equals(TEXT("suggested")))
		{
			return FNiagaraEditorUtilities::FGetFilteredScriptAssetsOptions::OnlySuggested;
		}
		if (Token.Equals(TEXT("no_suggested")) || Token.Equals(TEXT("exclude_suggested")) || Token.Equals(TEXT("general_only")))
		{
			return FNiagaraEditorUtilities::FGetFilteredScriptAssetsOptions::NoSuggested;
		}
		return FNiagaraEditorUtilities::FGetFilteredScriptAssetsOptions::NoFiltering;
	}

	static FString NormalizeCategoryPath(const FString& RootCategory, const FString& AssetCategory)
	{
		FString Category = AssetCategory.TrimStartAndEnd();
		Category.ReplaceInline(TEXT("\\"), TEXT("/"));
		Category.ReplaceInline(TEXT("|"), TEXT("/"));
		while (Category.Contains(TEXT("//")))
		{
			Category.ReplaceInline(TEXT("//"), TEXT("/"));
		}

		if (Category.IsEmpty())
		{
			return RootCategory;
		}

		const FString LowerRoot = RootCategory.ToLower();
		if (Category.ToLower().Equals(LowerRoot) || Category.ToLower().StartsWith(LowerRoot + TEXT("/")))
		{
			return Category;
		}
		return RootCategory + TEXT("/") + Category;
	}

	static TSharedRef<FJsonObject> BuildNiagaraLegacyAuthoring(const FString& CandidateType, const FString& PackagePath, const FString& ObjectPath, const FString& DisplayName)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (CandidateType.Equals(TEXT("niagara_module")))
		{
			Obj->SetStringField(TEXT("operation"), TEXT("add_module"));
			Obj->SetStringField(TEXT("module_name"), DisplayName);
			Obj->SetStringField(TEXT("module_script_path"), PackagePath);
			Obj->SetStringField(TEXT("module_script_asset_path"), ObjectPath.IsEmpty() ? PackagePath : ObjectPath);
			Obj->SetStringField(TEXT("object_path"), ObjectPath);
		}
		else if (CandidateType.Equals(TEXT("niagara_graph_function")))
		{
			Obj->SetStringField(TEXT("operation"), TEXT("add_function_call_node"));
			Obj->SetStringField(TEXT("node_type"), TEXT("function_call"));
			Obj->SetStringField(TEXT("node_class"), TEXT("/Script/NiagaraEditor.NiagaraNodeFunctionCall"));
			Obj->SetStringField(TEXT("function_name"), DisplayName);
			Obj->SetStringField(TEXT("function_script_path"), PackagePath);
			Obj->SetStringField(TEXT("function_script_asset_path"), ObjectPath.IsEmpty() ? PackagePath : ObjectPath);
			Obj->SetStringField(TEXT("object_path"), ObjectPath);
			Obj->SetStringField(TEXT("target_folder_json"), TEXT("graph/nodes.json"));
		}
		else
		{
			Obj->SetStringField(TEXT("operation"), TEXT("set_value_source"));
			Obj->SetStringField(TEXT("value_source"), TEXT("dynamic_input"));
			Obj->SetStringField(TEXT("script_asset_path"), PackagePath);
			Obj->SetStringField(TEXT("dynamic_input_script_asset_path"), ObjectPath.IsEmpty() ? PackagePath : ObjectPath);
			Obj->SetStringField(TEXT("object_path"), ObjectPath);
			Obj->SetArrayField(TEXT("inputs"), TArray<TSharedPtr<FJsonValue>>());
		}
		return Obj;
	}

	static FString GetTextTag(const FAssetData& AssetData, FName TagName)
	{
		FText TextValue;
		if (AssetData.GetTagValue(TagName, TextValue))
		{
			return TextValue.ToString();
		}

		FString StringValue;
		if (AssetData.GetTagValue(TagName, StringValue))
		{
			return StringValue;
		}
		return FString();
	}

	static bool GetBoolTag(const FAssetData& AssetData, FName TagName)
	{
		bool bValue = false;
		return AssetData.GetTagValue(TagName, bValue) && bValue;
	}
}

bool CollectNiagaraCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError)
{
	ENiagaraScriptUsage ScriptUsage = ENiagaraScriptUsage::Module;
	FString RootCategory = TEXT("Modules");
	const FString CandidateType = GetCanonicalNiagaraCandidateType(Params.CatalogType);
	if (IsNiagaraModuleCatalog(Params.CatalogType))
	{
		ScriptUsage = ENiagaraScriptUsage::Module;
		RootCategory = TEXT("Modules");
	}
	else if (IsNiagaraDynamicInputCatalog(Params.CatalogType))
	{
		ScriptUsage = ENiagaraScriptUsage::DynamicInput;
		RootCategory = TEXT("Dynamic Inputs");
	}
	else if (IsNiagaraGraphNodeCatalog(Params.CatalogType))
	{
		ScriptUsage = ENiagaraScriptUsage::Function;
		RootCategory = TEXT("Functions");
	}
	else
	{
		OutError = FString::Printf(TEXT("unsupported Niagara catalog_type '%s'."), *Params.CatalogType);
		return false;
	}

	FNiagaraEditorUtilities::FGetFilteredScriptAssetsOptions Options;
	Options.ScriptUsageToInclude = ScriptUsage;
	Options.bIncludeNonLibraryScripts = !Params.bLibraryOnly;
	Options.SuggestedFiltering = ParseSuggestedFilter(Params.SuggestedFilter);

	ENiagaraScriptUsage TargetUsage = ENiagaraScriptUsage::Module;
	if (ParseNiagaraTargetUsage(Params.TargetUsage, TargetUsage))
	{
		Options.TargetUsageToMatch = TargetUsage;
	}
	else if (!Params.TargetUsage.IsEmpty())
	{
		OutWarnings.Add(FString::Printf(TEXT("Unknown Niagara target_usage '%s'; target usage filter was ignored."), *Params.TargetUsage));
	}

	TArray<FAssetData> ScriptAssets;
	FNiagaraEditorUtilities::GetFilteredScriptAssets(Options, ScriptAssets);

	int32 UiOrder = 0;
	for (const FAssetData& ScriptAsset : ScriptAssets)
	{
		const TTuple<EScriptSource, FText> Source = FNiagaraEditorUtilities::GetScriptSource(ScriptAsset);
		if (!IsSourceAllowed(Source.Key, Params.SourceFilters))
		{
			continue;
		}

		const bool bInLibrary = FNiagaraEditorUtilities::IsScriptAssetInLibrary(ScriptAsset);
		const bool bSuggested = GetBoolTag(ScriptAsset, GET_MEMBER_NAME_CHECKED(FVersionedNiagaraScriptData, bSuggested));
		const FString PackagePath = ScriptAsset.PackageName.ToString();
		const FString ObjectPath = ScriptAsset.GetSoftObjectPath().ToString();
		const FString DisplayName = StripUiOnlyMarkers(FNiagaraEditorUtilities::FormatScriptName(ScriptAsset.AssetName, bInLibrary).ToString());
		const FString Description = GetTextTag(ScriptAsset, GET_MEMBER_NAME_CHECKED(FVersionedNiagaraScriptData, Description));
		const FString Keywords = GetTextTag(ScriptAsset, GET_MEMBER_NAME_CHECKED(FVersionedNiagaraScriptData, Keywords));
		const FString AssetCategory = GetTextTag(ScriptAsset, GET_MEMBER_NAME_CHECKED(FVersionedNiagaraScriptData, Category));

		FCandidate Candidate;
		Candidate.Kind = CandidateType;
		Candidate.Name = DisplayName;
		Candidate.FullName = ObjectPathOrPackagePath(ObjectPath, PackagePath);
		Candidate.CandidateId = Candidate.Kind + TEXT(":") + Candidate.FullName;
		Candidate.StableId = PackagePath;
		Candidate.CanonicalKey = ScriptAsset.AssetName.ToString();
		Candidate.DisplayName = DisplayName;
		Candidate.Tooltip = FNiagaraEditorUtilities::FormatScriptDescription(FText::FromString(Description), ScriptAsset.GetSoftObjectPath(), bInLibrary).ToString();
		Candidate.Keywords = Keywords;
		Candidate.CategoryPath = NormalizeCategoryPath(RootCategory, AssetCategory);
		Candidate.Section = bSuggested ? TEXT("Suggested") : TEXT("General");
		Candidate.Type = CandidateType;
		Candidate.SourceOrigin = SourceToString(Source.Key);
		Candidate.SourceLabel = Source.Value.ToString();
		Candidate.PackagePath = PackagePath;
		Candidate.ObjectPath = ObjectPath;
		if (CandidateType.Equals(TEXT("niagara_module")))
		{
			Candidate.WriteSupport = TEXT("stack_module_folder_json");
			Candidate.SupportReason = TEXT("Can be written as module_script_path/module_script_asset_path in Niagara stage module JSON.");
		}
		else if (CandidateType.Equals(TEXT("niagara_graph_function")))
		{
			Candidate.WriteSupport = TEXT("niagara_script_graph_folder_json");
			Candidate.SupportReason = TEXT("Can be referenced by Niagara script graph folder JSON as a UNiagaraNodeFunctionCall; edit graph/nodes.json and graph/links.json through niagara_script_export_folder/apply_folder.");
		}
		else
		{
			Candidate.WriteSupport = TEXT("module_input_value_source_json");
			Candidate.SupportReason = TEXT("Can be written as dynamic input script_asset_path in Niagara module input value JSON.");
		}
		Candidate.bInLibrary = bInLibrary;
		Candidate.bSuggested = bSuggested;
		Candidate.bSupportedInContext = true;
		Candidate.UiOrder = UiOrder++;
		Candidate.JsonAuthoring = BuildJsonAuthoringPatch(Candidate);
		Candidate.LegacyAuthoring = BuildNiagaraLegacyAuthoring(CandidateType, PackagePath, ObjectPath, DisplayName);
		OutCandidates.Add(MoveTemp(Candidate));
	}

	return true;
}
}
