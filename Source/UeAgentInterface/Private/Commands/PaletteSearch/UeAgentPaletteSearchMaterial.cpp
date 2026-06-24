// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchAuthoring.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchNaming.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MaterialEditorUtilities.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Misc/PackageName.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"

namespace UeAgentPaletteSearch
{
namespace
{
	static FString NormalizeObjectPath(const FString& InPath)
	{
		FString ObjectPath = InPath.TrimStartAndEnd();
		if (ObjectPath.IsEmpty() || ObjectPath.Contains(TEXT(".")))
		{
			return ObjectPath;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(ObjectPath);
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *ObjectPath, *AssetName);
	}

	static UObject* LoadObjectFromPath(const FString& InPath)
	{
		const FString ObjectPath = NormalizeObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	static UMaterialGraph* CreateTransientMaterialFunctionGraph(UMaterialFunction* Function, FString& OutError)
	{
		if (!Function)
		{
			OutError = TEXT("material_function_required");
			return nullptr;
		}

		UMaterial* PreviewMaterial = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transactional | RF_Transient);
		if (!PreviewMaterial)
		{
			OutError = TEXT("material_function_preview_material_create_failed");
			return nullptr;
		}
		PreviewMaterial->SetShadingModel(MSM_Unlit);
		PreviewMaterial->MaterialDomain = Function->PreviewMaterialDomain;

		UMaterialFunction* PreviewFunction = Cast<UMaterialFunction>(
			StaticDuplicateObject(
				Function,
				GetTransientPackage(),
				NAME_None,
				~RF_Standalone,
				Function->GetClass()));
		if (!PreviewFunction)
		{
			OutError = TEXT("material_function_preview_duplicate_failed");
			return nullptr;
		}
		PreviewFunction->ParentFunction = Function;
		PreviewFunction->SetFlags(RF_Transactional);

		UMaterialGraph* Graph = CastChecked<UMaterialGraph>(
			FBlueprintEditorUtils::CreateNewGraph(
				PreviewMaterial,
				NAME_None,
				UMaterialGraph::StaticClass(),
				UMaterialGraphSchema::StaticClass()));
		PreviewMaterial->MaterialGraph = Graph;
		Graph->Material = PreviewMaterial;
		Graph->MaterialFunction = PreviewFunction;
		Graph->OriginalMaterialFullName = Function->GetName();
		PreviewFunction->MaterialGraph = Graph;
		PreviewFunction->EditorMaterial = PreviewMaterial;

		PreviewMaterial->AssignExpressionCollection(PreviewFunction->GetExpressionCollection());
		PreviewMaterial->bEnableNewHLSLGenerator = PreviewFunction->IsUsingNewHLSLGenerator();
		for (UMaterialExpression* Expression : PreviewMaterial->GetExpressions())
		{
			if (Expression)
			{
				Expression->Function = nullptr;
				Expression->Material = PreviewMaterial;
			}
		}

		Graph->RebuildGraph();
		return Graph;
	}

	static UMaterialGraph* ResolveMaterialGraph(UObject* Asset, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("material_asset_not_found");
			return nullptr;
		}

		UMaterialGraph* Graph = nullptr;
		bool bNeedsRebuild = false;
		if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
			if (!Material->MaterialGraph)
			{
				Material->MaterialGraph = CastChecked<UMaterialGraph>(
					FBlueprintEditorUtils::CreateNewGraph(
						Material,
						NAME_None,
						UMaterialGraph::StaticClass(),
						UMaterialGraphSchema::StaticClass()));
				bNeedsRebuild = true;
			}
			Graph = Material->MaterialGraph;
			if (Graph)
			{
				Graph->Material = Material;
				Graph->MaterialFunction = nullptr;
				Graph->OriginalMaterialFullName = Material->GetName();
			}
		}
		else if (UMaterialFunction* Function = Cast<UMaterialFunction>(Asset))
		{
			Graph = CreateTransientMaterialFunctionGraph(Function, OutError);
		}
		else
		{
			OutError = TEXT("unsupported_material_asset_type");
			return nullptr;
		}

		if (!Graph)
		{
			OutError = TEXT("material_graph_not_found");
			return nullptr;
		}

		if (bNeedsRebuild)
		{
			Graph->RebuildGraph();
		}
		return Graph;
	}

	static UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& NodeGuidText)
	{
		if (!Graph || NodeGuidText.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}

		FGuid NodeGuid;
		if (!FGuid::Parse(NodeGuidText, NodeGuid))
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid)
			{
				return Node;
			}
		}
		return nullptr;
	}

	static bool PinNameMatches(const UEdGraphPin* Pin, const FString& PinName)
	{
		if (!Pin || PinName.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		return Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase)
			|| Pin->GetDisplayName().ToString().Equals(PinName, ESearchCase::IgnoreCase);
	}

	static UEdGraphPin* FindPin(UEdGraph* Graph, UEdGraphNode* Node, const FString& PinGuidText, const FString& PinName)
	{
		if (!Graph)
		{
			return nullptr;
		}

		FGuid PinGuid;
		const bool bHasPinGuid = FGuid::Parse(PinGuidText, PinGuid);
		auto MatchesPin = [bHasPinGuid, PinGuid, &PinName](UEdGraphPin* Pin)
		{
			return Pin && ((bHasPinGuid && Pin->PinId == PinGuid) || PinNameMatches(Pin, PinName));
		};

		if (Node)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (MatchesPin(Pin))
				{
					return Pin;
				}
			}
			return nullptr;
		}

		for (UEdGraphNode* GraphNode : Graph->Nodes)
		{
			if (!GraphNode)
			{
				continue;
			}
			for (UEdGraphPin* Pin : GraphNode->Pins)
			{
				if (MatchesPin(Pin))
				{
					return Pin;
				}
			}
		}
		return nullptr;
	}

	static bool WantsMaterialExpressions(const FString& CatalogType)
	{
		return CatalogType.Equals(TEXT("material_node")) || CatalogType.Equals(TEXT("material_expression"));
	}

	static bool WantsMaterialFunctions(const FString& CatalogType)
	{
		return CatalogType.Equals(TEXT("material_node")) || CatalogType.Equals(TEXT("material_function")) || CatalogType.Equals(TEXT("material_function_call"));
	}

	static TSharedRef<FJsonObject> BuildExpressionLegacyAuthoring(UClass* ExpressionClass)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("operation"), TEXT("add_material_expression"));
		Obj->SetStringField(TEXT("expression_class"), ExpressionClass ? ExpressionClass->GetPathName() : FString());
		Obj->SetNumberField(TEXT("node_pos_x"), 0);
		Obj->SetNumberField(TEXT("node_pos_y"), 0);
		return Obj;
	}

	static TSharedRef<FJsonObject> BuildFunctionLegacyAuthoring(const FString& FunctionPath)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("operation"), TEXT("add_material_function_call"));
		Obj->SetStringField(TEXT("expression_class"), UMaterialExpressionMaterialFunctionCall::StaticClass()->GetPathName());
		Obj->SetStringField(TEXT("material_function"), FunctionPath);
		Obj->SetStringField(TEXT("post_create_property"), TEXT("MaterialFunction"));
		Obj->SetStringField(TEXT("post_create_value_text"), FunctionPath);
		Obj->SetNumberField(TEXT("node_pos_x"), 0);
		Obj->SetNumberField(TEXT("node_pos_y"), 0);
		return Obj;
	}

	static FString BuildActionCategoryPath(const FString& RootCategory, const FEdGraphSchemaAction& Action)
	{
		const FString Category = Action.GetCategory().ToString().TrimStartAndEnd();
		if (Category.IsEmpty())
		{
			return RootCategory;
		}
		return RootCategory + TEXT("/") + Category.Replace(TEXT("|"), TEXT("/"));
	}

	static void AddActionCandidate(const FSearchParams& Params, const TSharedPtr<FEdGraphSchemaAction>& Action, int32 UiOrder, TArray<FCandidate>& OutCandidates)
	{
		if (!Action.IsValid())
		{
			return;
		}

		if (Action->GetTypeId() == FMaterialGraphSchemaAction_NewNode::StaticGetTypeId())
		{
			if (!WantsMaterialExpressions(Params.CatalogType))
			{
				return;
			}

			const TSharedPtr<FMaterialGraphSchemaAction_NewNode> NewNodeAction = StaticCastSharedPtr<FMaterialGraphSchemaAction_NewNode>(Action);
			UClass* ExpressionClass = NewNodeAction.IsValid() ? NewNodeAction->MaterialExpressionClass.Get() : nullptr;
			if (!ExpressionClass)
			{
				return;
			}

			FCandidate Candidate;
			Candidate.Kind = TEXT("material_expression");
			Candidate.Name = MenuNameFromText(Action->GetMenuDescription(), ExpressionClass->GetName());
			Candidate.FullName = ExpressionClass->GetPathName();
			Candidate.CandidateId = Candidate.Kind + TEXT(":") + Candidate.FullName;
			Candidate.StableId = ExpressionClass->GetPathName();
			Candidate.CanonicalKey = ExpressionClass->GetName();
			Candidate.DisplayName = Candidate.Name;
			Candidate.Tooltip = Action->GetTooltipDescription().ToString();
			Candidate.Keywords = Action->GetKeywords().ToString();
			Candidate.CategoryPath = BuildActionCategoryPath(TEXT("Expressions"), *Action);
			Candidate.Section = Params.PinGuid.IsEmpty() && Params.PinName.IsEmpty() ? TEXT("General") : TEXT("Pin Context");
			Candidate.Type = Candidate.Kind;
			Candidate.SourceOrigin = ExpressionClass->GetOutermost()->GetName().StartsWith(TEXT("/Script/")) ? TEXT("engine") : TEXT("project");
			Candidate.SourceLabel = ExpressionClass->GetOutermost()->GetName();
			Candidate.ObjectPath = ExpressionClass->GetPathName();
			Candidate.WriteSupport = TEXT("material_expression_graph_json");
			Candidate.SupportReason = TEXT("Can be written with expression_class via material_add_expression or material folder JSON.");
			Candidate.bInLibrary = true;
			Candidate.bSupportedInContext = true;
			Candidate.UiOrder = UiOrder;
			Candidate.JsonAuthoring = BuildJsonAuthoringPatch(Candidate);
			Candidate.LegacyAuthoring = BuildExpressionLegacyAuthoring(ExpressionClass);
			OutCandidates.Add(MoveTemp(Candidate));
			return;
		}

		if (Action->GetTypeId() == FMaterialGraphSchemaAction_NewFunctionCall::StaticGetTypeId())
		{
			if (!WantsMaterialFunctions(Params.CatalogType))
			{
				return;
			}

			const TSharedPtr<FMaterialGraphSchemaAction_NewFunctionCall> FunctionAction = StaticCastSharedPtr<FMaterialGraphSchemaAction_NewFunctionCall>(Action);
			const FString FunctionPath = FunctionAction.IsValid() ? FunctionAction->FunctionPath : FString();
			if (FunctionPath.IsEmpty())
			{
				return;
			}

			FCandidate Candidate;
			Candidate.Kind = TEXT("material_function_call");
			Candidate.Name = MenuNameFromText(Action->GetMenuDescription(), FunctionPath);
			Candidate.FullName = FunctionPath;
			Candidate.CandidateId = Candidate.Kind + TEXT(":") + Candidate.FullName;
			Candidate.StableId = FunctionPath;
			Candidate.CanonicalKey = FunctionPath;
			Candidate.DisplayName = Candidate.Name;
			Candidate.Tooltip = Action->GetTooltipDescription().ToString();
			Candidate.CategoryPath = BuildActionCategoryPath(TEXT("Functions"), *Action);
			Candidate.Section = Params.PinGuid.IsEmpty() && Params.PinName.IsEmpty() ? TEXT("General") : TEXT("Pin Context");
			Candidate.Type = Candidate.Kind;
			Candidate.SourceOrigin = FunctionPath.StartsWith(TEXT("/Engine/")) ? TEXT("engine") : TEXT("project");
			Candidate.SourceLabel = FunctionPath;
			Candidate.PackagePath = FunctionPath;
			Candidate.ObjectPath = FunctionPath;
			Candidate.WriteSupport = TEXT("material_expression_graph_json");
			Candidate.SupportReason = TEXT("Create MaterialExpressionMaterialFunctionCall, then set MaterialFunction to this function path.");
			Candidate.bInLibrary = true;
			Candidate.bSupportedInContext = true;
			Candidate.UiOrder = UiOrder;
			Candidate.JsonAuthoring = BuildJsonAuthoringPatch(Candidate);
			Candidate.LegacyAuthoring = BuildFunctionLegacyAuthoring(FunctionPath);
			OutCandidates.Add(MoveTemp(Candidate));
		}
	}
}

bool CollectMaterialCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError)
{
	if (!Params.CatalogType.Equals(TEXT("material_node")) &&
		!Params.CatalogType.Equals(TEXT("material_expression")) &&
		!Params.CatalogType.Equals(TEXT("material_function")) &&
		!Params.CatalogType.Equals(TEXT("material_function_call")))
	{
		OutError = FString::Printf(TEXT("unsupported Material catalog_type '%s'."), *Params.CatalogType);
		return false;
	}

	const UMaterialGraphSchema* Schema = UMaterialGraphSchema::StaticClass()->GetDefaultObject<UMaterialGraphSchema>();
	if (!Schema)
	{
		OutError = TEXT("material_graph_schema_unavailable");
		return false;
	}

	const UObject* MaterialContext = UMaterial::StaticClass()->GetDefaultObject();
	UEdGraphPin* ContextPin = nullptr;
	UMaterialGraph* ContextGraph = nullptr;
	const bool bHasPinContext = !Params.PinGuid.IsEmpty() || !Params.PinName.IsEmpty();
	if (bHasPinContext)
	{
		if (Params.AssetPath.IsEmpty())
		{
			OutError = TEXT("missing_asset_path_for_material_pin_context");
			return false;
		}

		UObject* Asset = LoadObjectFromPath(Params.AssetPath);
		ContextGraph = ResolveMaterialGraph(Asset, OutError);
		if (!ContextGraph)
		{
			return false;
		}
		const bool bMatchesSemanticGraphName =
			(Cast<UMaterial>(Asset) && Params.GraphName.Equals(TEXT("MaterialGraph"), ESearchCase::IgnoreCase))
			|| (Cast<UMaterialFunction>(Asset) && Params.GraphName.Equals(TEXT("MaterialFunctionGraph"), ESearchCase::IgnoreCase));
		if (!Params.GraphName.IsEmpty()
			&& !ContextGraph->GetName().Equals(Params.GraphName, ESearchCase::IgnoreCase)
			&& !ContextGraph->GetPathName().Equals(Params.GraphName, ESearchCase::IgnoreCase)
			&& !bMatchesSemanticGraphName)
		{
			OutError = TEXT("material_graph_not_found");
			return false;
		}

		UEdGraphNode* Node = FindNodeByGuid(ContextGraph, Params.NodeGuid);
		ContextPin = FindPin(ContextGraph, Node, Params.PinGuid, Params.PinName);
		if (!ContextPin)
		{
			OutError = TEXT("pin_not_found");
			return false;
		}
		MaterialContext = ContextGraph->GetMaterialOrFunction();
	}

	TArray<TSharedPtr<FEdGraphSchemaAction>> Actions;
	if (ContextGraph)
	{
		FGraphContextMenuBuilder ActionBuilder(ContextGraph);
		ActionBuilder.FromPin = ContextPin;
		Schema->GetGraphContextActions(ActionBuilder);
		for (int32 Index = 0; Index < ActionBuilder.GetNumActions(); ++Index)
		{
			Actions.Add(ActionBuilder.GetSchemaAction(Index));
		}
	}
	else
	{
		FGraphActionMenuBuilder ActionBuilder;
		Schema->GetPaletteActions(ActionBuilder, FString(), MaterialContext);
		for (int32 Index = 0; Index < ActionBuilder.GetNumActions(); ++Index)
		{
			Actions.Add(ActionBuilder.GetSchemaAction(Index));
		}
	}

	for (int32 Index = 0; Index < Actions.Num(); ++Index)
	{
		AddActionCandidate(Params, Actions[Index], Index, OutCandidates);
	}

	if (OutCandidates.IsEmpty())
	{
		OutWarnings.Add(bHasPinContext
			? TEXT("Material pin context returned no supported expression/function candidates.")
			: TEXT("Material palette returned no supported expression/function candidates."));
	}
	return true;
}
}
