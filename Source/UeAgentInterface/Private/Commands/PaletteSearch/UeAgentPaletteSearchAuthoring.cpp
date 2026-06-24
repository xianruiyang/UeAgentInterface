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
		Obj->SetStringField(TEXT("kind"), Candidate.Kind);
		Obj->SetStringField(TEXT("name"), Candidate.Name);
		Obj->SetStringField(TEXT("full_name"), Candidate.FullName);
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
		SeedObj->SetStringField(TEXT("operation"), TEXT("upsert"));
		SetIdentityFields(Candidate, SeedObj);
		SeedObj->SetArrayField(TEXT("inputs"), EmptyArray());
		return MakeRoot(TEXT("niagara_authoring_profile"), TEXT("niagara.modules[]"), TEXT("stages[].modules[]"), SeedObj);
	}

	static TSharedPtr<FJsonObject> BuildNiagaraDynamicInputSeed(const FCandidate& Candidate)
	{
		TSharedRef<FJsonObject> DynamicInputObj = MakeShared<FJsonObject>();
		SetIdentityFields(Candidate, DynamicInputObj);
		DynamicInputObj->SetArrayField(TEXT("inputs"), EmptyArray());

		TSharedRef<FJsonObject> SeedObj = MakeShared<FJsonObject>();
		SeedObj->SetStringField(TEXT("source"), TEXT("dynamic_input"));
		SeedObj->SetObjectField(TEXT("dynamic_input"), DynamicInputObj);
		return MakeRoot(TEXT("niagara_authoring_profile"), TEXT("niagara.modules[].inputs[].value"), TEXT("stages[].modules[].inputs[].value"), SeedObj);
	}

	static TSharedPtr<FJsonObject> BuildGraphNodeSeed(const FCandidate& Candidate, const FString& Route, const FString& Target, const FString& ProfilePath, const FString& LayoutSpace)
	{
		TSharedRef<FJsonObject> SeedObj = MakeShared<FJsonObject>();
		SeedObj->SetStringField(TEXT("operation"), TEXT("upsert"));
		SeedObj->SetStringField(TEXT("node_id"), MakeSafeObjectName(Candidate.Name, TEXT("Node")));
		SetIdentityFields(Candidate, SeedObj);
		SeedObj->SetObjectField(TEXT("layout"), MakeLayout(LayoutSpace, MakeSafeObjectName(Candidate.Name, TEXT("Node"))));
		return MakeRoot(Route, Target, ProfilePath, SeedObj);
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
	if (Candidate.Kind.Equals(TEXT("niagara_graph_function")))
	{
		return BuildGraphNodeSeed(Candidate, TEXT("niagara_script_authoring_profile"), TEXT("niagara_script.graph.nodes[]"), TEXT("graph.nodes[]"), TEXT("niagara_graph_2d"));
	}
	if (Candidate.Kind.Equals(TEXT("material_expression")) || Candidate.Kind.Equals(TEXT("material_function_call")))
	{
		TSharedPtr<FJsonObject> RootObj = BuildGraphNodeSeed(Candidate, TEXT("material_authoring_profile"), TEXT("material.graph.nodes[]"), TEXT("material_graph.nodes[]"), TEXT("material_graph_2d"));
		if (RootObj.IsValid())
		{
			const TSharedPtr<FJsonObject>* SeedObjPtr = nullptr;
			if (RootObj->TryGetObjectField(TEXT("seed"), SeedObjPtr) && SeedObjPtr && SeedObjPtr->IsValid())
			{
				(*SeedObjPtr)->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
			}
		}
		return RootObj;
	}
	if (Candidate.Kind.StartsWith(TEXT("blueprint_")))
	{
		TSharedPtr<FJsonObject> RootObj = BuildGraphNodeSeed(Candidate, TEXT("blueprint_umg_authoring_profile"), TEXT("blueprint.graph.nodes[]"), TEXT("blueprint_graphs[].nodes[]"), TEXT("event_graph_2d"));
		if (RootObj.IsValid())
		{
			const TSharedPtr<FJsonObject>* SeedObjPtr = nullptr;
			if (RootObj->TryGetObjectField(TEXT("seed"), SeedObjPtr) && SeedObjPtr && SeedObjPtr->IsValid())
			{
				(*SeedObjPtr)->SetArrayField(TEXT("pin_defaults"), EmptyArray());
			}
		}
		return RootObj;
	}
	if (Candidate.Kind.Equals(TEXT("actor_component")))
	{
		TSharedRef<FJsonObject> SeedObj = MakeShared<FJsonObject>();
		SeedObj->SetStringField(TEXT("operation"), TEXT("upsert"));
		SeedObj->SetStringField(TEXT("component_id"), MakeSafeObjectName(Candidate.Name, TEXT("Component")));
		SetIdentityFields(Candidate, SeedObj);
		SeedObj->SetStringField(TEXT("parent_component_id"), TEXT(""));
		SeedObj->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		return MakeRoot(TEXT("blueprint_umg_authoring_profile"), TEXT("blueprint.components[]"), TEXT("components[]"), SeedObj);
	}
	if (Candidate.Kind.Equals(TEXT("umg_widget")))
	{
		TSharedRef<FJsonObject> SeedObj = MakeShared<FJsonObject>();
		SeedObj->SetStringField(TEXT("operation"), TEXT("upsert"));
		SeedObj->SetStringField(TEXT("widget_id"), MakeSafeObjectName(Candidate.Name, TEXT("Widget")));
		SetIdentityFields(Candidate, SeedObj);
		SeedObj->SetStringField(TEXT("parent_widget_id"), TEXT(""));
		SeedObj->SetBoolField(TEXT("make_variable"), false);
		SeedObj->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		SeedObj->SetArrayField(TEXT("children_widget_ids"), EmptyArray());
		return MakeRoot(TEXT("blueprint_umg_authoring_profile"), TEXT("umg.widget_tree.widgets[]"), TEXT("widget_tree.widgets[]"), SeedObj);
	}
	return nullptr;
}
}
