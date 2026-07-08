// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_NodeGraphOps.h"
#include "UeAgentHttpServer_NodeGraphOpsInternal.h"
#include "Commands/Blueprint/Layout/Core/UeAgentBlueprintAutoLayoutAlgorithm.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentNodeGraphOps
{
	namespace Internal
	{
		UMaterialGraph* CreateTransientMaterialFunctionGraphForLayout(UMaterialFunction* Function, FString& OutError)
		{
			if (!Function)
			{
				OutError = TEXT("material_function_required_for_transient_layout_graph");
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
			UMaterialExpressionFunctionOutput* PreviewOutput = nullptr;
			UMaterialExpressionFunctionOutput* FirstOutput = nullptr;
			for (int32 ExpressionIndex = PreviewMaterial->GetExpressions().Num() - 1; ExpressionIndex >= 0; --ExpressionIndex)
			{
				if (UMaterialExpressionFunctionOutput* FunctionOutput =
					Cast<UMaterialExpressionFunctionOutput>(PreviewMaterial->GetExpressions()[ExpressionIndex]))
				{
					FirstOutput = FunctionOutput;
					if (FunctionOutput->bLastPreviewed)
					{
						PreviewOutput = FunctionOutput;
					}
				}
			}
			if (!PreviewOutput)
			{
				PreviewOutput = FirstOutput;
			}
			const FGuid PreviewOutputGuid = PreviewOutput
				? PreviewOutput->MaterialExpressionGuid
				: FGuid();
			if (PreviewOutput)
			{
				PreviewOutput->bLastPreviewed = true;
			}
			Graph->RebuildGraph();
			if (PreviewOutput)
			{
				int32 FunctionOutputNodeCount = 0;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node);
					if (MaterialNode && Cast<UMaterialExpressionFunctionOutput>(MaterialNode->MaterialExpression))
					{
						++FunctionOutputNodeCount;
					}
				}
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node))
					{
						const UMaterialExpression* NodeExpression = MaterialNode->MaterialExpression;
						const bool bMatchesPreviewOutput =
							NodeExpression == PreviewOutput
							|| (NodeExpression && PreviewOutputGuid.IsValid() && NodeExpression->MaterialExpressionGuid == PreviewOutputGuid)
							|| (FunctionOutputNodeCount == 1 && Cast<UMaterialExpressionFunctionOutput>(NodeExpression));
						MaterialNode->bIsPreviewExpression = bMatchesPreviewOutput;
					}
				}
			}
			return Graph;
		}
		UMaterialGraph* ResolveMaterialSharedGraph(UObject* Asset, FString& OutError)
		{
			if (!Asset)
			{
				OutError = TEXT("asset_not_found");
				return nullptr;
			}
			UMaterialGraph* Graph = nullptr;
			bool bGraphAlreadyRebuiltWithPreviewState = false;
			if (UMaterial* Material = Cast<UMaterial>(Asset))
			{
#if WITH_EDITORONLY_DATA
				if (!Material->MaterialGraph)
				{
					Material->MaterialGraph = CastChecked<UMaterialGraph>(
						FBlueprintEditorUtils::CreateNewGraph(
							Material,
							NAME_None,
							UMaterialGraph::StaticClass(),
							UMaterialGraphSchema::StaticClass()));
				}
				Graph = Material->MaterialGraph;
				if (Graph && Graph == Material->MaterialGraph)
				{
					Graph->Material = Material;
					Graph->MaterialFunction = nullptr;
					Graph->OriginalMaterialFullName = Material->GetName();
				}
#endif
			}
			else if (Cast<UMaterialFunction>(Asset))
			{
#if WITH_EDITORONLY_DATA
				UMaterialFunction* Function = CastChecked<UMaterialFunction>(Asset);
				Graph = CreateTransientMaterialFunctionGraphForLayout(Function, OutError);
				if (!Graph)
				{
					return nullptr;
				}
				bGraphAlreadyRebuiltWithPreviewState = true;
#endif
			}
			if (!Graph && !Cast<UMaterialFunction>(Asset))
			{
				bool bOpenedEditor = false;
				OpenEditorForGeometryIfNeeded(Asset, bOpenedEditor);
				PumpAndTickSlateForLayoutGeometry();
				PumpAndTickSlateForLayoutGeometry();
				if (UMaterial* Material = Cast<UMaterial>(Asset))
				{
#if WITH_EDITORONLY_DATA
					if (!Material->MaterialGraph)
					{
						Material->MaterialGraph = CastChecked<UMaterialGraph>(
							FBlueprintEditorUtils::CreateNewGraph(
								Material,
								NAME_None,
								UMaterialGraph::StaticClass(),
								UMaterialGraphSchema::StaticClass()));
					}
					Graph = Material->MaterialGraph;
					if (Graph)
					{
						Graph->Material = Material;
						Graph->MaterialFunction = nullptr;
						Graph->OriginalMaterialFullName = Material->GetName();
					}
#endif
				}
			}
			if (!Graph)
			{
				OutError = TEXT("material_edgraph_not_found");
				return nullptr;
			}
			if (!bGraphAlreadyRebuiltWithPreviewState)
			{
				Graph->RebuildGraph();
			}
			return Graph;
		}
		UMaterialExpression* FindOriginalFunctionExpressionForPreview(
			UMaterialFunction* OriginalFunction,
			const UMaterialExpression* PreviewExpression)
		{
			if (!OriginalFunction || !PreviewExpression)
			{
				return nullptr;
			}

			const FGuid PreviewGuid = PreviewExpression->MaterialExpressionGuid;
			UMaterialExpression* FirstClassMatch = nullptr;
			for (UMaterialExpression* OriginalExpression : OriginalFunction->GetExpressions())
			{
				if (!OriginalExpression)
				{
					continue;
				}
				if (OriginalExpression->GetClass() == PreviewExpression->GetClass() && !FirstClassMatch)
				{
					FirstClassMatch = OriginalExpression;
				}
				if (PreviewGuid.IsValid() && OriginalExpression->MaterialExpressionGuid == PreviewGuid)
				{
					if (OriginalExpression->GetClass() == PreviewExpression->GetClass())
					{
						return OriginalExpression;
					}
				}
			}

			return PreviewGuid.IsValid() ? nullptr : FirstClassMatch;
		}
		bool BuildMaterialFunctionExpressionWritebackMap(
			UMaterialFunction* OriginalFunction,
			const FUniversalLayoutModel& Model,
			TMap<UEdGraphNode*, UMaterialExpression*>& OutWritebackMap,
			FString& OutError)
		{
			OutWritebackMap.Reset();
			if (!OriginalFunction)
			{
				return true;
			}

			for (const FUniversalLayoutNodeRef& Ref : Model.NodeRefs)
			{
				if (Ref.bVirtual || !Ref.GraphNode)
				{
					continue;
				}
				UMaterialGraphNode* MaterialGraphNode = Cast<UMaterialGraphNode>(Ref.GraphNode);
				if (!MaterialGraphNode || !MaterialGraphNode->MaterialExpression)
				{
					continue;
				}

				UMaterialExpression* OriginalExpression =
					FindOriginalFunctionExpressionForPreview(OriginalFunction, MaterialGraphNode->MaterialExpression);
				if (!OriginalExpression)
				{
					OutError = FString::Printf(
						TEXT("material_function_preview_expression_writeback_failed graph_node=%s expression=%s class=%s guid=%s"),
						*Ref.GraphNode->GetName(),
						*MaterialGraphNode->MaterialExpression->GetName(),
						*MaterialGraphNode->MaterialExpression->GetClass()->GetName(),
						*MaterialGraphNode->MaterialExpression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphens));
					return false;
				}
				OutWritebackMap.Add(Ref.GraphNode, OriginalExpression);
			}

			return true;
		}
		bool LayoutMaterialExpressionGraph(
			UObject* Asset,
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutData,
			FString& OutError)
		{
			UMaterial* Material = Cast<UMaterial>(Asset);
			UMaterialFunctionInterface* Function = Cast<UMaterialFunctionInterface>(Asset);
			if (!Material && !Function)
			{
				OutError = TEXT("material_graph_asset_required");
				return false;
			}

			bool bSaveAfterLayout = false;
			bool bCompileAfterLayout = false;
			bool bInsertRerouteNodes = false;
			bool bReplaceExistingRerouteNodes = false;
			TryGetBoolFromEither(Params, nullptr, TEXT("save_after_layout"), bSaveAfterLayout);
			TryGetBoolFromEither(Params, nullptr, TEXT("compile_after_layout"), bCompileAfterLayout);
			TryGetLayoutBool(Params, TEXT("insert_reroute_nodes"), bInsertRerouteNodes);
			TryGetLayoutBool(Params, TEXT("replace_existing_reroute_nodes"), bReplaceExistingRerouteNodes);

			UMaterialGraph* Graph = ResolveMaterialSharedGraph(Asset, OutError);
			if (!Graph)
			{
				return false;
			}
			FUniversalLayoutModel Model = BuildGenericSharedLayoutModel(Graph);
			FSharedGraphGeometryStats GeometryStats;
			if (!CaptureSharedGraphRealGeometry(Asset, Graph, Model, GeometryStats, OutError, false))
			{
				return false;
			}
			UeAgentBlueprintAutoLayout::FOptions Options;
			UeAgentBlueprintAutoLayout::FRunResult RunResult;
			if (!RunSharedBlueprintLayoutCore(Model, Params, Options, RunResult, OutError))
			{
				return false;
			}
			const int32 PlannedRerouteKnotNodeCount = CountSharedPlannedRerouteKnotNodes(RunResult.ReroutePlans);
			if ((bInsertRerouteNodes || bReplaceExistingRerouteNodes) && PlannedRerouteKnotNodeCount > 0)
			{
				OutError = TEXT("material_graph_reroute_writeback_unsupported_for_adapter");
				return false;
			}
			TMap<UEdGraphNode*, UMaterialExpression*> MaterialFunctionWritebackMap;
			UMaterialFunction* OriginalMaterialFunction = Cast<UMaterialFunction>(Function);
			if (OriginalMaterialFunction
				&& !BuildMaterialFunctionExpressionWritebackMap(
					OriginalMaterialFunction,
					Model,
					MaterialFunctionWritebackMap,
					OutError))
			{
				return false;
			}

			int32 MovedCount = 0;
			TArray<TSharedPtr<FJsonValue>> NodeResults;
			TSet<UEdGraphNode*> NodesNeedingWrite;
			const int32 NodeCount = FMath::Min(Model.Nodes.Num(), Model.NodeRefs.Num());
			for (int32 Index = 0; Index < NodeCount; ++Index)
			{
				UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[Index];
				const FUniversalLayoutNodeRef& Ref = Model.NodeRefs[Index];
				const int32 NewX = FMath::RoundToInt(Node.NewX);
				const int32 NewY = FMath::RoundToInt(Node.NewY);
				if (!Ref.bVirtual && Ref.GraphNode)
				{
					bool bNeedsWrite = Ref.GraphNode->NodePosX != NewX || Ref.GraphNode->NodePosY != NewY;
					if (UMaterialGraphNode* MaterialGraphNode = Cast<UMaterialGraphNode>(Ref.GraphNode))
					{
						if (UMaterialExpression* PreviewExpression = MaterialGraphNode->MaterialExpression)
						{
							bNeedsWrite |= PreviewExpression->MaterialExpressionEditorX != NewX
								|| PreviewExpression->MaterialExpressionEditorY != NewY;
						}
					}
					if (UMaterialExpression** OriginalExpressionPtr = MaterialFunctionWritebackMap.Find(Ref.GraphNode))
					{
						if (UMaterialExpression* OriginalExpression = *OriginalExpressionPtr)
						{
							bNeedsWrite |= OriginalExpression->MaterialExpressionEditorX != NewX
								|| OriginalExpression->MaterialExpressionEditorY != NewY;
						}
					}
					if (bNeedsWrite)
					{
						++MovedCount;
						NodesNeedingWrite.Add(Ref.GraphNode);
					}
				}
				NodeResults.Add(MakeShared<FJsonValueObject>(SharedLayoutNodeToJson(Node, Ref)));
			}

			bool bSaved = false;
			bool bCompiled = false;
			FString CompileSkippedReason;
			TUniquePtr<FScopedTransaction> LayoutTransaction;
			if (MovedCount > 0)
			{
				LayoutTransaction = MakeUnique<FScopedTransaction>(
					NSLOCTEXT("UeAgentInterface", "NodeGraphLayoutMaterialTransaction", "UeAgentInterface Node Graph Layout"));
				Graph->Modify();
				Asset->Modify();
				for (int32 Index = 0; Index < NodeCount; ++Index)
				{
					const UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[Index];
					const FUniversalLayoutNodeRef& Ref = Model.NodeRefs[Index];
					if (!Ref.bVirtual && Ref.GraphNode)
					{
						if (!NodesNeedingWrite.Contains(Ref.GraphNode))
						{
							continue;
						}
						const int32 NewX = FMath::RoundToInt(Node.NewX);
						const int32 NewY = FMath::RoundToInt(Node.NewY);
						Ref.GraphNode->Modify();
						Ref.GraphNode->NodePosX = NewX;
						Ref.GraphNode->NodePosY = NewY;
						if (UMaterialGraphNode* MaterialGraphNode = Cast<UMaterialGraphNode>(Ref.GraphNode))
						{
							if (UMaterialExpression* PreviewExpression = MaterialGraphNode->MaterialExpression)
							{
								PreviewExpression->Modify();
								PreviewExpression->MaterialExpressionEditorX = NewX;
								PreviewExpression->MaterialExpressionEditorY = NewY;
							}
						}
						if (UMaterialExpression** OriginalExpressionPtr = MaterialFunctionWritebackMap.Find(Ref.GraphNode))
						{
							UMaterialExpression* OriginalExpression = *OriginalExpressionPtr;
							if (OriginalExpression)
							{
								OriginalExpression->Modify();
								OriginalExpression->MaterialExpressionEditorX = NewX;
								OriginalExpression->MaterialExpressionEditorY = NewY;
							}
						}
					}
				}
				if (!OriginalMaterialFunction)
				{
					Graph->LinkMaterialExpressionsFromGraph();
					if (Material && Graph->Material && Graph->Material != Material)
					{
						Material->AssignExpressionCollection(Graph->Material->GetExpressionCollection());
					}
				}
				Graph->NotifyGraphChanged();
				Asset->MarkPackageDirty();
				Asset->PostEditChange();
				if (bCompileAfterLayout)
				{
					if (Material)
					{
						UMaterialEditingLibrary::RecompileMaterial(Material);
						bCompiled = true;
					}
					else
					{
						CompileSkippedReason = TEXT("material_function_compile_not_supported_by_node_graph_layout");
					}
				}
				if (bSaveAfterLayout && !SaveAssetPackage(Asset, OutError))
				{
					if (LayoutTransaction)
					{
						LayoutTransaction->Cancel();
					}
					return false;
				}
				bSaved = bSaveAfterLayout;
			}

			OutData = MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
			OutData->SetStringField(TEXT("adapter"), AdapterForAsset(Asset));
			OutData->SetStringField(TEXT("graph_name"), Graph->GetName());
			OutData->SetStringField(TEXT("graph_path"), Graph->GetPathName());
			AddSharedLayoutRunFields(OutData, RunResult);
			AddSharedGeometryFields(OutData, GeometryStats);
			OutData->SetBoolField(TEXT("changed"), MovedCount > 0);
			OutData->SetBoolField(TEXT("saved"), bSaved);
			OutData->SetBoolField(TEXT("compiled"), bCompiled);
			if (!CompileSkippedReason.IsEmpty())
			{
				OutData->SetStringField(TEXT("compile_skipped_reason"), CompileSkippedReason);
			}
			OutData->SetNumberField(TEXT("output_access_offset_x"), Options.OutputAccessOffsetX);
			OutData->SetNumberField(TEXT("input_access_offset_x"), Options.InputAccessOffsetX);
			OutData->SetStringField(TEXT("access_offset_source"), Options.AccessOffsetSource);
			OutData->SetBoolField(TEXT("reroute_writeback_supported"), false);
			OutData->SetBoolField(TEXT("insert_reroute_nodes"), bInsertRerouteNodes);
			OutData->SetBoolField(TEXT("replace_existing_reroute_nodes"), bReplaceExistingRerouteNodes);
			OutData->SetNumberField(TEXT("planned_reroute_knot_node_count"), PlannedRerouteKnotNodeCount);
			OutData->SetNumberField(TEXT("node_count"), Model.WritableNodeCount);
			OutData->SetNumberField(TEXT("virtual_node_count"), Model.Nodes.Num() - Model.WritableNodeCount);
			OutData->SetNumberField(TEXT("edge_count"), Model.Edges.Num());
			OutData->SetNumberField(TEXT("collapsed_reroute_node_count"), Model.CollapsedRerouteNodeCount);
			OutData->SetNumberField(TEXT("collapsed_reroute_edge_count"), Model.CollapsedRerouteEdgeCount);
			OutData->SetNumberField(TEXT("moved_node_count"), MovedCount);
			OutData->SetArrayField(TEXT("nodes"), NodeResults);
			OutData->SetArrayField(TEXT("edges"), SharedLayoutEdgesToJson(Model));
			return true;
		}
	}
}
