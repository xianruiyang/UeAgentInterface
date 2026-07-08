// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_NodeGraphOps.h"
#include "UeAgentHttpServer_NodeGraphOpsInternal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunctionInterface.h"
#include "MaterialShared.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentNodeGraphOps
{
	namespace Internal
	{
		FRootMaterialPropertySpec MakeRootMaterialPropertySpec(const TCHAR* Name, const EMaterialProperty Property)
		{
			FRootMaterialPropertySpec Spec;
			Spec.Name = Name;
			Spec.Property = Property;
			return Spec;
		}
		bool IsGraphLikelyStateMachineTopLevel(const UEdGraph* Graph)
		{
			if (!Graph)
			{
				return false;
			}
			const FString ClassName = Graph->GetClass()->GetName();
			return ClassName.Contains(TEXT("AnimationStateMachineGraph"))
				|| ClassName.Contains(TEXT("StateMachineGraph"))
				|| ClassName.Contains(TEXT("BehaviorTree"))
				|| ClassName.Contains(TEXT("StateTree"));
		}
		FString BlueprintGraphKind(const UBlueprint* Blueprint, const UEdGraph* Graph)
		{
			if (!Blueprint || !Graph)
			{
				return TEXT("unknown");
			}
			auto ContainsGraph = [Graph](const TArray<UEdGraph*>& Graphs) -> bool
			{
				for (const UEdGraph* Candidate : Graphs)
				{
					if (Candidate == Graph)
					{
						return true;
					}
				}
				return false;
			};
			if (ContainsGraph(Blueprint->UbergraphPages))
			{
				return TEXT("event_graph");
			}
			if (ContainsGraph(Blueprint->FunctionGraphs))
			{
				return TEXT("function_graph");
			}
			if (ContainsGraph(Blueprint->MacroGraphs))
			{
				return TEXT("macro_graph");
			}
			if (ContainsGraph(Blueprint->DelegateSignatureGraphs))
			{
				return TEXT("delegate_graph");
			}
			return TEXT("nested_graph");
		}
		int32 CountGraphEdges(const UEdGraph* Graph)
		{
			if (!Graph)
			{
				return 0;
			}
			int32 Count = 0;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output)
					{
						continue;
					}
					for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode() && LinkedPin->GetOwningNode()->GetGraph() == Graph)
						{
							++Count;
						}
					}
				}
			}
			return Count;
		}
		int32 CountSemanticNodes(const UEdGraph* Graph)
		{
			if (!Graph)
			{
				return 0;
			}
			int32 Count = 0;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && !Node->IsA<UEdGraphNode_Comment>())
				{
					++Count;
				}
			}
			return Count;
		}
		TSharedPtr<FJsonObject> GraphRefToJson(const FGraphRef& GraphRef)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("graph_name"), GraphRef.Graph ? GraphRef.Graph->GetName() : TEXT(""));
			Item->SetStringField(TEXT("graph_path"), GraphRef.Graph ? GraphRef.Graph->GetPathName() : TEXT(""));
			Item->SetStringField(TEXT("graph_kind"), GraphRef.GraphKind);
			Item->SetStringField(TEXT("graph_class"), GraphRef.Graph ? GraphRef.Graph->GetClass()->GetPathName() : TEXT(""));
			Item->SetBoolField(TEXT("supported"), GraphRef.bSupported);
			if (!GraphRef.bSupported)
			{
				Item->SetStringField(TEXT("unsupported_reason"), GraphRef.UnsupportedReason);
			}
			Item->SetNumberField(TEXT("node_count"), CountSemanticNodes(GraphRef.Graph));
			Item->SetNumberField(TEXT("edge_count"), CountGraphEdges(GraphRef.Graph));
			return Item;
		}
		void AddUniqueGraph(TArray<FGraphRef>& Graphs, TSet<UEdGraph*>& SeenGraphs, UEdGraph* Graph, const FString& Kind)
		{
			if (!Graph || SeenGraphs.Contains(Graph))
			{
				return;
			}
			SeenGraphs.Add(Graph);
			FGraphRef Ref;
			Ref.Graph = Graph;
			Ref.GraphKind = Kind;
			if (IsGraphLikelyStateMachineTopLevel(Graph))
			{
				Ref.bSupported = false;
				Ref.UnsupportedReason = TEXT("non_pin_node_graph_or_vertical_state_graph");
			}
			Graphs.Add(Ref);
		}
		TArray<FGraphRef> CollectBlueprintGraphs(UBlueprint* Blueprint, const bool bIncludeNestedGraphs)
		{
			TArray<FGraphRef> Graphs;
			TSet<UEdGraph*> SeenGraphs;
			if (!Blueprint)
			{
				return Graphs;
			}
			for (UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				AddUniqueGraph(Graphs, SeenGraphs, Graph, TEXT("event_graph"));
			}
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				AddUniqueGraph(Graphs, SeenGraphs, Graph, TEXT("function_graph"));
			}
			for (UEdGraph* Graph : Blueprint->MacroGraphs)
			{
				AddUniqueGraph(Graphs, SeenGraphs, Graph, TEXT("macro_graph"));
			}
			for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
			{
				AddUniqueGraph(Graphs, SeenGraphs, Graph, TEXT("delegate_graph"));
			}
			if (bIncludeNestedGraphs)
			{
				TArray<UEdGraph*> AllGraphs;
				Blueprint->GetAllGraphs(AllGraphs);
				for (UEdGraph* Graph : AllGraphs)
				{
					AddUniqueGraph(Graphs, SeenGraphs, Graph, BlueprintGraphKind(Blueprint, Graph));
				}
			}
			return Graphs;
		}
		TArray<FGraphRef> CollectGenericEdGraphs(UObject* Asset)
		{
			TArray<FGraphRef> Graphs;
			TSet<UEdGraph*> SeenGraphs;
			if (!Asset)
			{
				return Graphs;
			}
			TArray<UObject*> ChildObjects;
			GetObjectsWithOuter(Asset, ChildObjects, true);
			for (UObject* Child : ChildObjects)
			{
				if (UEdGraph* Graph = Cast<UEdGraph>(Child))
				{
					AddUniqueGraph(Graphs, SeenGraphs, Graph, TEXT("edgraph"));
				}
			}
			return Graphs;
		}
		const TArray<FRootMaterialPropertySpec>& GetRootMaterialPropertySpecs()
		{
			static const TArray<FRootMaterialPropertySpec> Specs = {
				MakeRootMaterialPropertySpec(TEXT("BaseColor"), MP_BaseColor),
				MakeRootMaterialPropertySpec(TEXT("EmissiveColor"), MP_EmissiveColor),
				MakeRootMaterialPropertySpec(TEXT("Opacity"), MP_Opacity),
				MakeRootMaterialPropertySpec(TEXT("OpacityMask"), MP_OpacityMask),
				MakeRootMaterialPropertySpec(TEXT("Metallic"), MP_Metallic),
				MakeRootMaterialPropertySpec(TEXT("Specular"), MP_Specular),
				MakeRootMaterialPropertySpec(TEXT("Roughness"), MP_Roughness),
				MakeRootMaterialPropertySpec(TEXT("Normal"), MP_Normal),
				MakeRootMaterialPropertySpec(TEXT("Tangent"), MP_Tangent),
				MakeRootMaterialPropertySpec(TEXT("WorldPositionOffset"), MP_WorldPositionOffset),
				MakeRootMaterialPropertySpec(TEXT("SubsurfaceColor"), MP_SubsurfaceColor),
				MakeRootMaterialPropertySpec(TEXT("AmbientOcclusion"), MP_AmbientOcclusion),
				MakeRootMaterialPropertySpec(TEXT("Refraction"), MP_Refraction),
				MakeRootMaterialPropertySpec(TEXT("PixelDepthOffset"), MP_PixelDepthOffset),
				MakeRootMaterialPropertySpec(TEXT("Anisotropy"), MP_Anisotropy),
				MakeRootMaterialPropertySpec(TEXT("FrontMaterial"), MP_FrontMaterial)
			};
			return Specs;
		}
		TArray<UMaterialExpression*> GetMaterialGraphExpressions(UObject* Asset)
		{
			TArray<UMaterialExpression*> Expressions;
			if (UMaterial* Material = Cast<UMaterial>(Asset))
			{
				for (UMaterialExpression* Expression : Material->GetExpressions())
				{
					if (Expression)
					{
						Expressions.Add(Expression);
					}
				}
			}
			else if (UMaterialFunctionInterface* Function = Cast<UMaterialFunctionInterface>(Asset))
			{
				for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
				{
					if (Expression)
					{
						Expressions.Add(Expression.Get());
					}
				}
			}
			return Expressions;
		}
		int32 CountMaterialExpressionEdges(const TArray<UMaterialExpression*>& Expressions)
		{
			TSet<UMaterialExpression*> ExpressionSet(Expressions);
			int32 Count = 0;
			for (UMaterialExpression* Expression : Expressions)
			{
				if (!Expression)
				{
					continue;
				}
				for (FExpressionInputIterator It{ Expression }; It; ++It)
				{
					if (It->Expression && ExpressionSet.Contains(It->Expression))
					{
						++Count;
					}
				}
			}
			return Count;
		}
		int32 CountMaterialRootEdges(UMaterial* Material, const TArray<UMaterialExpression*>& Expressions)
		{
			if (!Material)
			{
				return 0;
			}
			TSet<UMaterialExpression*> ExpressionSet(Expressions);
			int32 Count = 0;
			for (const FRootMaterialPropertySpec& PropertySpec : GetRootMaterialPropertySpecs())
			{
				FExpressionInput* Input = Material->GetExpressionInputForProperty(PropertySpec.Property);
				if (Input && Input->Expression && ExpressionSet.Contains(Input->Expression))
				{
					++Count;
				}
			}
			return Count;
		}
		int32 CountMaterialLayoutEdges(UObject* Asset, const TArray<UMaterialExpression*>& Expressions)
		{
			return CountMaterialExpressionEdges(Expressions) + CountMaterialRootEdges(Cast<UMaterial>(Asset), Expressions);
		}
		void AddMaterialGraphSummary(UObject* Asset, TArray<TSharedPtr<FJsonValue>>& Graphs)
		{
			TArray<UMaterialExpression*> Expressions;
			FString GraphName = TEXT("MaterialGraph");
			FString GraphKind = TEXT("material_graph");
			if (Asset->IsA<UMaterial>())
			{
				Expressions = GetMaterialGraphExpressions(Asset);
			}
			else if (Asset->IsA<UMaterialFunctionInterface>())
			{
				Expressions = GetMaterialGraphExpressions(Asset);
				GraphName = TEXT("MaterialFunctionGraph");
				GraphKind = TEXT("material_function_graph");
			}
			else
			{
				return;
			}

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("graph_name"), GraphName);
			Item->SetStringField(TEXT("graph_path"), Asset->GetPathName() + TEXT(":") + GraphName);
			Item->SetStringField(TEXT("graph_kind"), GraphKind);
			Item->SetStringField(TEXT("graph_class"), TEXT("material_expression_graph"));
			Item->SetBoolField(TEXT("supported"), true);
			Item->SetStringField(TEXT("layout_adapter"), TEXT("shared_blueprint_layout_core"));
			Item->SetNumberField(TEXT("node_count"), Expressions.Num());
			Item->SetNumberField(TEXT("edge_count"), CountMaterialLayoutEdges(Asset, Expressions));
			Graphs.Add(MakeShared<FJsonValueObject>(Item));
		}
	}
}
