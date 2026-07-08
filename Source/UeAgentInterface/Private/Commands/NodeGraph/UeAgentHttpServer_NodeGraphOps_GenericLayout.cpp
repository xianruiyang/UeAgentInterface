// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_NodeGraphOps.h"
#include "UeAgentHttpServer_NodeGraphOpsInternal.h"

#include "Commands/Blueprint/Layout/Core/UeAgentBlueprintAutoLayoutAlgorithm.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/RigVMEdGraph.h"
#include "ScopedTransaction.h"

namespace UeAgentNodeGraphOps
{
	namespace Internal
	{
		bool LayoutGenericEdGraph(
			UObject* Asset,
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutData,
			FString& OutError)
		{
			FString GraphRef;
			TSharedPtr<FJsonObject> Selector;
			if (TryGetObjectField(Params, TEXT("graph_selector"), Selector))
			{
				TryGetStringFromEither(Selector, nullptr, TEXT("graph_path"), GraphRef);
				if (GraphRef.IsEmpty())
				{
					TryGetStringFromEither(Selector, nullptr, TEXT("graph_name"), GraphRef);
				}
			}
			if (GraphRef.IsEmpty())
			{
				TryGetStringFromEither(Params, nullptr, TEXT("graph_path"), GraphRef);
			}
			if (GraphRef.IsEmpty())
			{
				TryGetStringFromEither(Params, nullptr, TEXT("graph_name"), GraphRef);
			}

			UEdGraph* Graph = ResolveGenericGraph(Asset, GraphRef);
			if (!Graph)
			{
				OutError = TEXT("graph_not_found");
				return false;
			}
			if (IsGraphLikelyStateMachineTopLevel(Graph))
			{
				OutError = TEXT("unsupported_non_pin_node_graph_or_vertical_state_graph");
				return false;
			}

			bool bSaveAfterLayout = false;
			TryGetBoolFromEither(Params, nullptr, TEXT("save_after_layout"), bSaveAfterLayout);
			bool bInsertRerouteNodes = false;
			bool bReplaceExistingRerouteNodes = false;
			bool bAllowRemoveAllRerouteNodes = false;
			TryGetLayoutBool(Params, TEXT("insert_reroute_nodes"), bInsertRerouteNodes);
			TryGetLayoutBool(Params, TEXT("replace_existing_reroute_nodes"), bReplaceExistingRerouteNodes);
			TryGetLayoutBool(Params, TEXT("allow_remove_all_reroute_nodes"), bAllowRemoveAllRerouteNodes);
			const bool bEffectiveReplaceExistingRerouteNodes = bReplaceExistingRerouteNodes || bInsertRerouteNodes;
			URigVMEdGraph* RigGraph = Cast<URigVMEdGraph>(Graph);
			const bool bRerouteWritebackSupported = RigGraph != nullptr;

			FUniversalLayoutModel Model = BuildGenericSharedLayoutModel(Graph);
			FSharedGraphGeometryStats GeometryStats;
			if (!CaptureSharedGraphRealGeometry(Asset, Graph, Model, GeometryStats, OutError, true))
			{
				return false;
			}
			UeAgentBlueprintAutoLayout::FOptions Options;
			UeAgentBlueprintAutoLayout::FRunResult RunResult;
			if (!RunSharedBlueprintLayoutCore(Model, Params, Options, RunResult, OutError))
			{
				return false;
			}
			const int32 ExistingRerouteNodeCount = RigGraph ? CountRigVMRerouteNodes(RigGraph) : 0;
			const int32 PlannedRerouteKnotNodeCount = CountSharedPlannedRerouteKnotNodes(RunResult.ReroutePlans);
			if ((bInsertRerouteNodes || bReplaceExistingRerouteNodes)
				&& !bRerouteWritebackSupported
				&& PlannedRerouteKnotNodeCount > 0)
			{
				OutError = TEXT("generic_edgraph_reroute_writeback_unsupported_for_adapter");
				return false;
			}
			if (bEffectiveReplaceExistingRerouteNodes
				&& ExistingRerouteNodeCount > 0
				&& PlannedRerouteKnotNodeCount == 0
				&& !bAllowRemoveAllRerouteNodes)
			{
				OutError = FString::Printf(
					TEXT("reroute_delete_all_guard_blocked existing_reroute_nodes=%d planned_reroute_knots=0; pass allow_remove_all_reroute_nodes=true only when intentionally flattening every existing reroute"),
					ExistingRerouteNodeCount);
				return false;
			}

			int32 MovedCount = 0;
			TArray<TSharedPtr<FJsonValue>> NodeResults;
			const int32 NodeCount = FMath::Min(Model.Nodes.Num(), Model.NodeRefs.Num());
			for (int32 Index = 0; Index < NodeCount; ++Index)
			{
				UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[Index];
				const FUniversalLayoutNodeRef& Ref = Model.NodeRefs[Index];
				const int32 NewX = FMath::RoundToInt(Node.NewX);
				const int32 NewY = FMath::RoundToInt(Node.NewY);
				if (!Ref.bVirtual && Ref.GraphNode)
				{
					const int32 OldX = Ref.GraphNode->NodePosX;
					const int32 OldY = Ref.GraphNode->NodePosY;
					if (OldX != NewX || OldY != NewY)
					{
						++MovedCount;
					}
				}
				NodeResults.Add(MakeShared<FJsonValueObject>(SharedLayoutNodeToJson(Node, Ref)));
			}

			bool bSaved = false;
			int32 CreatedRerouteNodeCount = 0;
			const bool bNeedsRerouteWrite = bRerouteWritebackSupported
				&& bEffectiveReplaceExistingRerouteNodes
				&& (ExistingRerouteNodeCount > 0 || PlannedRerouteKnotNodeCount > 0);
			const bool bNeedsWrite = MovedCount > 0 || bNeedsRerouteWrite;
			TUniquePtr<FScopedTransaction> LayoutTransaction;
			if (bNeedsWrite)
			{
				LayoutTransaction = MakeUnique<FScopedTransaction>(
					NSLOCTEXT("UeAgentInterface", "NodeGraphLayoutGenericTransaction", "UeAgentInterface Node Graph Layout"));
				Graph->Modify();
				Asset->Modify();
				for (int32 Index = 0; Index < NodeCount; ++Index)
				{
					const UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[Index];
					const FUniversalLayoutNodeRef& Ref = Model.NodeRefs[Index];
					if (!Ref.bVirtual && Ref.GraphNode)
					{
						const int32 NewX = FMath::RoundToInt(Node.NewX);
						const int32 NewY = FMath::RoundToInt(Node.NewY);
						if (Ref.GraphNode->NodePosX != NewX || Ref.GraphNode->NodePosY != NewY)
						{
							ApplySharedGraphNodePosition(Ref.GraphNode, NewX, NewY);
						}
					}
				}
				if (bNeedsRerouteWrite)
				{
					CreatedRerouteNodeCount = ApplyRigVMReroutePlans(
						RigGraph,
						Model,
						bInsertRerouteNodes ? RunResult.ReroutePlans : TArray<UeAgentBlueprintAutoLayout::FReroutePlan>(),
						bEffectiveReplaceExistingRerouteNodes,
						OutError);
					if (!OutError.IsEmpty())
					{
						if (LayoutTransaction)
						{
							LayoutTransaction->Cancel();
						}
						return false;
					}
				}
				Graph->NotifyGraphChanged();
				Asset->MarkPackageDirty();
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
			OutData->SetBoolField(TEXT("changed"), bNeedsWrite);
			OutData->SetBoolField(TEXT("saved"), bSaved);
			OutData->SetNumberField(TEXT("output_access_offset_x"), Options.OutputAccessOffsetX);
			OutData->SetNumberField(TEXT("input_access_offset_x"), Options.InputAccessOffsetX);
			OutData->SetStringField(TEXT("access_offset_source"), Options.AccessOffsetSource);
			OutData->SetBoolField(TEXT("reroute_writeback_supported"), bRerouteWritebackSupported);
			OutData->SetBoolField(TEXT("insert_reroute_nodes"), bInsertRerouteNodes);
			OutData->SetBoolField(TEXT("replace_existing_reroute_nodes"), bReplaceExistingRerouteNodes);
			OutData->SetBoolField(TEXT("effective_replace_existing_reroute_nodes"), bEffectiveReplaceExistingRerouteNodes);
			OutData->SetBoolField(TEXT("allow_remove_all_reroute_nodes"), bAllowRemoveAllRerouteNodes);
			OutData->SetNumberField(TEXT("existing_reroute_node_count"), ExistingRerouteNodeCount);
			OutData->SetNumberField(TEXT("planned_reroute_knot_node_count"), PlannedRerouteKnotNodeCount);
			OutData->SetNumberField(TEXT("node_count"), Model.WritableNodeCount);
			OutData->SetNumberField(TEXT("virtual_node_count"), Model.Nodes.Num() - Model.WritableNodeCount);
			OutData->SetNumberField(TEXT("edge_count"), Model.Edges.Num());
			OutData->SetNumberField(TEXT("collapsed_reroute_node_count"), Model.CollapsedRerouteNodeCount);
			OutData->SetNumberField(TEXT("collapsed_reroute_edge_count"), Model.CollapsedRerouteEdgeCount);
			OutData->SetNumberField(TEXT("moved_node_count"), MovedCount);
			OutData->SetNumberField(TEXT("created_reroute_node_count"), CreatedRerouteNodeCount);
			OutData->SetArrayField(TEXT("nodes"), NodeResults);
			OutData->SetArrayField(TEXT("edges"), SharedLayoutEdgesToJson(Model));
			return true;
		}
	}

	static FGraphRef FromInternalGraphRef(const Internal::FGraphRef& InGraphRef)
	{
		FGraphRef OutGraphRef;
		OutGraphRef.Graph = InGraphRef.Graph;
		OutGraphRef.GraphKind = InGraphRef.GraphKind;
		OutGraphRef.bSupported = InGraphRef.bSupported;
		OutGraphRef.UnsupportedReason = InGraphRef.UnsupportedReason;
		return OutGraphRef;
	}

	static Internal::FGraphRef ToInternalGraphRef(const FGraphRef& InGraphRef)
	{
		Internal::FGraphRef OutGraphRef;
		OutGraphRef.Graph = InGraphRef.Graph;
		OutGraphRef.GraphKind = InGraphRef.GraphKind;
		OutGraphRef.bSupported = InGraphRef.bSupported;
		OutGraphRef.UnsupportedReason = InGraphRef.UnsupportedReason;
		return OutGraphRef;
	}

	UObject* LoadAssetObject(const FString& InPath)
	{
		return Internal::LoadAssetObject(InPath);
	}

	bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, TSharedPtr<FJsonObject>& OutObj)
	{
		return Internal::TryGetObjectField(Obj, Field, OutObj);
	}

	bool HasRemovedDryRunParameter(const TSharedPtr<FJsonObject>& Params)
	{
		return Internal::HasRemovedDryRunParameter(Params);
	}

	FString AdapterForAsset(const UObject* Asset)
	{
		return Internal::AdapterForAsset(Asset);
	}

	TSharedPtr<FJsonObject> GraphRefToJson(const FGraphRef& GraphRef)
	{
		return Internal::GraphRefToJson(ToInternalGraphRef(GraphRef));
	}

	TArray<FGraphRef> CollectBlueprintGraphs(UBlueprint* Blueprint, bool bIncludeNestedGraphs)
	{
		TArray<FGraphRef> OutGraphs;
		const TArray<Internal::FGraphRef> InternalGraphs = Internal::CollectBlueprintGraphs(Blueprint, bIncludeNestedGraphs);
		OutGraphs.Reserve(InternalGraphs.Num());
		for (const Internal::FGraphRef& GraphRef : InternalGraphs)
		{
			OutGraphs.Add(FromInternalGraphRef(GraphRef));
		}
		return OutGraphs;
	}

	TArray<FGraphRef> CollectGenericEdGraphs(UObject* Asset)
	{
		TArray<FGraphRef> OutGraphs;
		const TArray<Internal::FGraphRef> InternalGraphs = Internal::CollectGenericEdGraphs(Asset);
		OutGraphs.Reserve(InternalGraphs.Num());
		for (const Internal::FGraphRef& GraphRef : InternalGraphs)
		{
			OutGraphs.Add(FromInternalGraphRef(GraphRef));
		}
		return OutGraphs;
	}

	void AddMaterialGraphSummary(UObject* Asset, TArray<TSharedPtr<FJsonValue>>& Graphs)
	{
		Internal::AddMaterialGraphSummary(Asset, Graphs);
	}

	TSharedPtr<FJsonObject> BuildBlueprintLayoutParams(const TSharedPtr<FJsonObject>& Params)
	{
		return Internal::BuildBlueprintLayoutParams(Params);
	}

	bool LayoutMaterialExpressionGraph(UObject* Asset, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, FString& OutError)
	{
		return Internal::LayoutMaterialExpressionGraph(Asset, Params, OutData, OutError);
	}

	bool LayoutGenericEdGraph(UObject* Asset, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, FString& OutError)
	{
		return Internal::LayoutGenericEdGraph(Asset, Params, OutData, OutError);
	}
}

