// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Blueprint/Layout/Core/UeAgentBlueprintAutoLayoutAlgorithm.h"
#include "Module/UeAgentInterfaceLogger.h"

namespace UeAgentBlueprintAutoLayout
{
	namespace
	{
	// Keep these internal modules ordered. Later modules depend on helpers declared by earlier modules.
	// Internal data structures, geometry helpers, connection alignment, metrics.
	#include "Commands/Blueprint/Layout/Core/Private/UeAgentBlueprintAutoLayoutAlgorithm_InternalCore.inl"
	// Cycle cutting, island extraction, input-chain detection, rank and column ordering.
	#include "Commands/Blueprint/Layout/Core/Private/UeAgentBlueprintAutoLayoutAlgorithm_Islands.inl"
	// Anchored column placement, dense packing, input-chain placement, island-local layout.
	#include "Commands/Blueprint/Layout/Core/Private/UeAgentBlueprintAutoLayoutAlgorithm_IslandPlacement.inl"
	// Target input entry candidates and island boundary bus model.
	#include "Commands/Blueprint/Layout/Core/Private/UeAgentBlueprintAutoLayoutAlgorithm_InputModel.inl"
	// Frozen island placement, endpoint candidates, bus topology, and route diagnostics.
	#include "Commands/Blueprint/Layout/Core/Private/UeAgentBlueprintAutoLayoutAlgorithm_FrozenIslandSolver.inl"
	}

	bool FRect::Intersects(const FRect& Other, double Padding) const
	{
		constexpr double Epsilon = 0.5;
		return !(Right + Padding <= Other.Left + Epsilon
			|| Other.Right + Padding <= Left + Epsilon
			|| Bottom + Padding <= Other.Top + Epsilon
			|| Other.Bottom + Padding <= Top + Epsilon);
	}

	FString RouteKindToString(ERouteKind Kind)
	{
		switch (Kind)
		{
		case ERouteKind::Direct:
			return TEXT("direct");
		case ERouteKind::LocalBus:
			return TEXT("local_bus");
		case ERouteKind::ExternalBus:
			return TEXT("external_bus");
		default:
			return TEXT("direct");
		}
	}

	FString ReroutePointRoleToString(EReroutePointRole Role)
	{
		switch (Role)
		{
		case EReroutePointRole::TargetInput:
			return TEXT("target_input");
		case EReroutePointRole::SourceOutput:
			return TEXT("source_output");
		case EReroutePointRole::Auto:
		default:
			return TEXT("auto");
		}
	}

	FRect MakeNodeRect(const FNode& Node, bool bUseNewPosition)
	{
		const double X = NodeLeft(Node, bUseNewPosition);
		const double Y = NodeTop(Node, bUseNewPosition);
		return { X, Y, X + Node.Width, Y + Node.Height };
	}

	FVector2D GetSourcePort(const FNode& Node, const FEdge& Edge, bool bUseNewPosition)
	{
		const FVector2D Anchor = GetOutputAnchor(Node, Edge.SourcePinIndex);
		return FVector2D(NodeLeft(Node, bUseNewPosition) + Anchor.X, NodeTop(Node, bUseNewPosition) + Anchor.Y);
	}

	FVector2D GetTargetPort(const FNode& Node, const FEdge& Edge, bool bUseNewPosition)
	{
		const FVector2D Anchor = GetInputAnchor(Node, Edge.TargetPinIndex);
		return FVector2D(NodeLeft(Node, bUseNewPosition) + Anchor.X, NodeTop(Node, bUseNewPosition) + Anchor.Y);
	}

	static bool HasValidInputAnchor(const FNode& Node, int32 PinIndex)
	{
		return Node.InputPinAnchors.IsValidIndex(PinIndex)
			&& Node.InputPinAnchorValid.IsValidIndex(PinIndex)
			&& Node.InputPinAnchorValid[PinIndex] != 0;
	}

	static bool HasValidOutputAnchor(const FNode& Node, int32 PinIndex)
	{
		return Node.OutputPinAnchors.IsValidIndex(PinIndex)
			&& Node.OutputPinAnchorValid.IsValidIndex(PinIndex)
			&& Node.OutputPinAnchorValid[PinIndex] != 0;
	}

	static int32 CountMissingRequiredPinAnchors(const TArray<FNode>& Nodes, const TArray<FEdge>& Edges)
	{
		int32 Missing = 0;
		for (const FEdge& Edge : Edges)
		{
			if (!IsValidNodeIndex(Nodes, Edge.Source) || !IsValidNodeIndex(Nodes, Edge.Target))
			{
				continue;
			}
			if (!HasValidOutputAnchor(Nodes[Edge.Source], Edge.SourcePinIndex))
			{
				++Missing;
			}
			if (!HasValidInputAnchor(Nodes[Edge.Target], Edge.TargetPinIndex))
			{
				++Missing;
			}
		}
		return Missing;
	}

	static int32 CountMissingMeasuredNodeGeometry(const TArray<FNode>& Nodes)
	{
		int32 Missing = 0;
		for (const FNode& Node : Nodes)
		{
			if (!Node.bMeasuredGeometry
				|| !FMath::IsFinite(Node.Width)
				|| !FMath::IsFinite(Node.Height)
				|| Node.Width <= 0.0
				|| Node.Height <= 0.0)
			{
				++Missing;
			}
		}
		return Missing;
	}

	static TArray<int32> BuildNodeComponentMapFromIslandCuts(
		const TArray<FNode>& Nodes,
		const TArray<FEdge>& Edges,
		const TSet<int32>& IslandCutEdges,
		const TArray<int32>* CommentGroupByNode)
	{
		const TArray<TArray<int32>> Groups = BuildWeakIslands(Nodes, Edges, IslandCutEdges, CommentGroupByNode);
		TArray<int32> ComponentByNode;
		ComponentByNode.Init(INDEX_NONE, Nodes.Num());
		for (int32 ComponentIndex = 0; ComponentIndex < Groups.Num(); ++ComponentIndex)
		{
			for (const int32 NodeIndex : Groups[ComponentIndex])
			{
				if (ComponentByNode.IsValidIndex(NodeIndex))
				{
					ComponentByNode[NodeIndex] = ComponentIndex;
				}
			}
		}
		return ComponentByNode;
	}

	static TArray<int32> SortedIntSetValues(const TSet<int32>& Values)
	{
		TArray<int32> Out = Values.Array();
		Out.Sort();
		return Out;
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	static TArray<TSharedPtr<FJsonValue>> EdgeSetToJson(
		const TArray<FNode>& Nodes,
		const TArray<FEdge>& Edges,
		const TSet<int32>& EdgeIndices,
		const TSet<int32>& RankIgnoredEdges,
		const TSet<int32>& FeedbackEdges,
		const TSet<int32>& FanoutEdges,
		const TSet<int32>& IslandCutEdges)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const int32 EdgeIndex : SortedIntSetValues(EdgeIndices))
		{
			if (!Edges.IsValidIndex(EdgeIndex))
			{
				continue;
			}
			const FEdge& Edge = Edges[EdgeIndex];
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("edge_index"), EdgeIndex);
			Obj->SetNumberField(TEXT("source_node_index"), Edge.Source);
			Obj->SetNumberField(TEXT("target_node_index"), Edge.Target);
			Obj->SetStringField(TEXT("source_title"), Nodes.IsValidIndex(Edge.Source) ? Nodes[Edge.Source].Title : FString());
			Obj->SetStringField(TEXT("target_title"), Nodes.IsValidIndex(Edge.Target) ? Nodes[Edge.Target].Title : FString());
			Obj->SetStringField(TEXT("source_pin"), Edge.SourcePin);
			Obj->SetStringField(TEXT("target_pin"), Edge.TargetPin);
			Obj->SetStringField(TEXT("source_param_id"), MakeSourceParamId(Edge));
			Obj->SetBoolField(TEXT("exec"), Edge.bExec);
			Obj->SetBoolField(TEXT("rank_ignored"), RankIgnoredEdges.Contains(EdgeIndex));
			Obj->SetBoolField(TEXT("feedback_cut"), FeedbackEdges.Contains(EdgeIndex));
			Obj->SetBoolField(TEXT("fanout_cut"), FanoutEdges.Contains(EdgeIndex));
			Obj->SetBoolField(TEXT("island_cut"), IslandCutEdges.Contains(EdgeIndex));
			Out.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Out;
	}

	static void AppendIslandPartitionStageDiagnostics(
		FRunResult& Result,
		const FString& StageName,
		const TArray<FNode>& Nodes,
		const TArray<FEdge>& Edges,
		const TSet<int32>& IslandCutEdges,
		const TSet<int32>& RankIgnoredEdges,
		const TSet<int32>& FeedbackEdges,
		const TSet<int32>& FanoutEdges,
		const TArray<int32>* CommentGroupByNode)
	{
		const TArray<TArray<int32>> Groups = BuildWeakIslands(Nodes, Edges, IslandCutEdges, CommentGroupByNode);
		TArray<int32> IslandByNode;
		IslandByNode.Init(INDEX_NONE, Nodes.Num());
		for (int32 IslandIndex = 0; IslandIndex < Groups.Num(); ++IslandIndex)
		{
			for (const int32 NodeIndex : Groups[IslandIndex])
			{
				if (IslandByNode.IsValidIndex(NodeIndex))
				{
					IslandByNode[NodeIndex] = IslandIndex;
				}
			}
		}

		TSet<int32> CrossingCutEdges;
		for (const int32 EdgeIndex : IslandCutEdges)
		{
			if (!Edges.IsValidIndex(EdgeIndex))
			{
				continue;
			}
			const FEdge& Edge = Edges[EdgeIndex];
			const int32 SourceIsland = IslandByNode.IsValidIndex(Edge.Source) ? IslandByNode[Edge.Source] : INDEX_NONE;
			const int32 TargetIsland = IslandByNode.IsValidIndex(Edge.Target) ? IslandByNode[Edge.Target] : INDEX_NONE;
			if (SourceIsland != INDEX_NONE && TargetIsland != INDEX_NONE && SourceIsland != TargetIsland)
			{
				CrossingCutEdges.Add(EdgeIndex);
			}
		}

		TSharedPtr<FJsonObject> StageObj = MakeShared<FJsonObject>();
		StageObj->SetStringField(TEXT("stage"), StageName);
		StageObj->SetNumberField(TEXT("island_count"), Groups.Num());
		StageObj->SetNumberField(TEXT("island_cut_edge_count"), IslandCutEdges.Num());
		StageObj->SetNumberField(TEXT("rank_ignored_edge_count"), RankIgnoredEdges.Num());
		StageObj->SetNumberField(TEXT("feedback_edge_count"), FeedbackEdges.Num());
		StageObj->SetNumberField(TEXT("fanout_edge_count"), FanoutEdges.Num());
		StageObj->SetNumberField(TEXT("crossing_cut_edge_count"), CrossingCutEdges.Num());
		StageObj->SetArrayField(TEXT("island_cut_edge_indices"), IntArrayToJson(SortedIntSetValues(IslandCutEdges)));
		StageObj->SetArrayField(TEXT("rank_ignored_edge_indices"), IntArrayToJson(SortedIntSetValues(RankIgnoredEdges)));
		StageObj->SetArrayField(TEXT("feedback_edge_indices"), IntArrayToJson(SortedIntSetValues(FeedbackEdges)));
		StageObj->SetArrayField(TEXT("fanout_edge_indices"), IntArrayToJson(SortedIntSetValues(FanoutEdges)));
		StageObj->SetArrayField(TEXT("crossing_cut_edges"), EdgeSetToJson(Nodes, Edges, CrossingCutEdges, RankIgnoredEdges, FeedbackEdges, FanoutEdges, IslandCutEdges));

		TArray<TSharedPtr<FJsonValue>> IslandValues;
		IslandValues.Reserve(Groups.Num());
		for (int32 IslandIndex = 0; IslandIndex < Groups.Num(); ++IslandIndex)
		{
			const TArray<int32>& Group = Groups[IslandIndex];
			TArray<FString> Titles;
			Titles.Reserve(Group.Num());
			bool bHasExec = false;
			bool bCommentIsland = false;
			int32 CommentGroup = INDEX_NONE;
			for (const int32 NodeIndex : Group)
			{
				if (!Nodes.IsValidIndex(NodeIndex))
				{
					continue;
				}
				Titles.Add(Nodes[NodeIndex].Title);
				bHasExec = bHasExec || Nodes[NodeIndex].bHasExecPin;
				if (CommentGroupByNode && CommentGroupByNode->IsValidIndex(NodeIndex) && (*CommentGroupByNode)[NodeIndex] != INDEX_NONE)
				{
					bCommentIsland = true;
					CommentGroup = (*CommentGroupByNode)[NodeIndex];
				}
			}

			TSharedPtr<FJsonObject> IslandObj = MakeShared<FJsonObject>();
			IslandObj->SetNumberField(TEXT("island_id"), IslandIndex);
			IslandObj->SetNumberField(TEXT("node_count"), Group.Num());
			IslandObj->SetBoolField(TEXT("has_exec_node"), bHasExec);
			IslandObj->SetBoolField(TEXT("comment_island"), bCommentIsland);
			IslandObj->SetNumberField(TEXT("comment_group"), CommentGroup);
			IslandObj->SetArrayField(TEXT("node_indices"), IntArrayToJson(Group));
			IslandObj->SetArrayField(TEXT("node_titles"), StringArrayToJson(Titles));
			IslandValues.Add(MakeShared<FJsonValueObject>(IslandObj));
		}
		StageObj->SetArrayField(TEXT("islands"), IslandValues);
		Result.IslandPartitionStageDiagnostics.Add(MakeShared<FJsonValueObject>(StageObj));
	}

	bool RunLayout(TArray<FNode>& Nodes, const TArray<FEdge>& Edges, const FOptions& InOptions, FRunResult& OutResult)
	{
		const double LayoutTraceStartSeconds = FPlatformTime::Seconds();
		const FOptions Options = NormalizeOptions(InOptions);
		OutResult = FRunResult();
		OutResult.NodeCount = Nodes.Num();
		OutResult.EdgeCount = Edges.Num();
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore start nodes=%d edges=%d"), Nodes.Num(), Edges.Num());
		const int32 MissingRequiredAnchors = CountMissingRequiredPinAnchors(Nodes, Edges);
		const int32 MissingMeasuredNodeGeometry = FMath::Max(
			Options.MissingMeasuredNodeGeometryCount,
			CountMissingMeasuredNodeGeometry(Nodes));
		if (!Options.bUseRealGeometry
			|| Options.MissingPinAnchorCount > 0
			|| MissingRequiredAnchors > 0
			|| MissingMeasuredNodeGeometry > 0
			|| Options.OutputAccessOffsetX <= 0.0
			|| Options.InputAccessOffsetX <= 0.0)
		{
			OutResult.DesignDiagnostics.RouteThroughIslandCount = 0;
			return false;
		}
		OutResult.MetricsBefore = EvaluateMetrics(Nodes, Edges, nullptr, false, Options.NodePadding);

		for (FNode& Node : Nodes)
		{
			Node.NewX = 0.0;
			Node.NewY = 0.0;
			Node.InputChainTargetGroupKey = INDEX_NONE;
			Node.InputChainTargetGroupPadding = -1.0;
		}

		TSet<int32> RankIgnoredEdges;
		TSet<int32> IslandCutEdges;
		TSet<int32> FeedbackEdges;
		TSet<int32> FanoutEdges;
		MarkFeedbackEdgesAsRankIgnored(Nodes, Edges, RankIgnoredEdges, FeedbackEdges, OutResult.DesignDiagnostics);
		const TArray<int32> CommentGroupByNode = BuildCommentIslandGroupByNode(Nodes);
		TSet<int32> FeedbackOnlyIslandCutEdges;
		for (const int32 EdgeIndex : FeedbackEdges)
		{
			if (Edges.IsValidIndex(EdgeIndex))
			{
				FeedbackOnlyIslandCutEdges.Add(EdgeIndex);
			}
		}
		AppendIslandPartitionStageDiagnostics(
			OutResult,
			TEXT("after_scc_feedback_cut"),
			Nodes,
			Edges,
			FeedbackOnlyIslandCutEdges,
			RankIgnoredEdges,
			FeedbackEdges,
			FanoutEdges,
			&CommentGroupByNode);

		TMap<int32, TArray<int32>> EdgesBySourceNode;
		for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
		{
			const FEdge& Edge = Edges[EdgeIndex];
			if (!IsValidNodeIndex(Nodes, Edge.Source) || !IsValidNodeIndex(Nodes, Edge.Target))
			{
				continue;
			}
			if (FeedbackEdges.Contains(EdgeIndex))
			{
				continue;
			}
			if (IsSameCommentIslandGroup(CommentGroupByNode, Edge))
			{
				continue;
			}
			EdgesBySourceNode.FindOrAdd(Edge.Source).Add(EdgeIndex);
		}
		TSet<int32> FanoutNodes;
		for (const TPair<int32, TArray<int32>>& Pair : EdgesBySourceNode)
		{
			if (Pair.Value.Num() < 2)
			{
				continue;
			}
			for (const int32 EdgeIndex : Pair.Value)
			{
				if (!Edges.IsValidIndex(EdgeIndex))
				{
					continue;
				}
				RankIgnoredEdges.Add(EdgeIndex);
				IslandCutEdges.Add(EdgeIndex);
				FanoutEdges.Add(EdgeIndex);
				FanoutNodes.Add(Edges[EdgeIndex].Source);
			}
		}
		TSet<int32> FeedbackAndFanoutIslandCutEdges = IslandCutEdges;
		for (const int32 EdgeIndex : FeedbackEdges)
		{
			if (Edges.IsValidIndex(EdgeIndex))
			{
				FeedbackAndFanoutIslandCutEdges.Add(EdgeIndex);
			}
		}
		AppendIslandPartitionStageDiagnostics(
			OutResult,
			TEXT("after_fanout_cut_before_comment_boundary"),
			Nodes,
			Edges,
			FeedbackAndFanoutIslandCutEdges,
			RankIgnoredEdges,
			FeedbackEdges,
			FanoutEdges,
			&CommentGroupByNode);
		for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
		{
			const FEdge& Edge = Edges[EdgeIndex];
			if (!IsValidNodeIndex(Nodes, Edge.Source) || !IsValidNodeIndex(Nodes, Edge.Target))
			{
				continue;
			}
			if (CrossesCommentIslandBoundary(CommentGroupByNode, Edge))
			{
				++OutResult.DesignDiagnostics.CommentIslandBoundaryEdgeCount;
				if (!IslandCutEdges.Contains(EdgeIndex))
				{
					IslandCutEdges.Add(EdgeIndex);
				}
			}
		}
		for (const int32 EdgeIndex : FeedbackEdges)
		{
			if (Edges.IsValidIndex(EdgeIndex))
			{
				IslandCutEdges.Add(EdgeIndex);
			}
		}
		AppendIslandPartitionStageDiagnostics(
			OutResult,
			TEXT("after_all_island_cuts_before_final_islands"),
			Nodes,
			Edges,
			IslandCutEdges,
			RankIgnoredEdges,
			FeedbackEdges,
			FanoutEdges,
			&CommentGroupByNode);
		OutResult.DesignDiagnostics.FanoutNodeCount = FanoutNodes.Num();
		OutResult.DesignDiagnostics.FanoutImplicitEdgeCount = FanoutEdges.Num();
		OutResult.DesignDiagnostics.RankIgnoredEdgeCount = RankIgnoredEdges.Num();
		OutResult.DesignDiagnostics.IslandCutEdgeCount = IslandCutEdges.Num();
		OutResult.DesignDiagnostics.ImplicitEdgeCount = IslandCutEdges.Num();
		TArray<FEdge> ExplicitEdges;
		ExplicitEdges.Reserve(Edges.Num());
		for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
		{
			if (!RankIgnoredEdges.Contains(EdgeIndex))
			{
				ExplicitEdges.Add(Edges[EdgeIndex]);
			}
		}

		const TArray<TArray<int32>> IslandGroups = BuildWeakIslands(Nodes, Edges, IslandCutEdges, &CommentGroupByNode);
		AppendIslandPartitionStageDiagnostics(
			OutResult,
			TEXT("final_weak_connected_islands"),
			Nodes,
			Edges,
			IslandCutEdges,
			RankIgnoredEdges,
			FeedbackEdges,
			FanoutEdges,
			&CommentGroupByNode);
		TArray<FIsland> Islands;
		TMap<int32, int32> IslandByNode;
		for (int32 IslandIndex = 0; IslandIndex < IslandGroups.Num(); ++IslandIndex)
		{
			FIsland Island;
			Island.Id = IslandIndex;
			Island.Nodes = IslandGroups[IslandIndex];
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
				IslandByNode.Add(NodeIndex, IslandIndex);
				Island.bHasExec = Island.bHasExec || Nodes[NodeIndex].bHasExecPin;
				if (CommentGroupByNode.IsValidIndex(NodeIndex) && CommentGroupByNode[NodeIndex] != INDEX_NONE)
				{
					Island.bCommentIsland = true;
					Island.CommentGroup = CommentGroupByNode[NodeIndex];
					if (Nodes[NodeIndex].bIsCommentNode)
					{
						Island.CommentNodeIndex = NodeIndex;
					}
				}
			}
			Island.PreferredY = 0.0;
			if (Island.bCommentIsland)
			{
				++OutResult.DesignDiagnostics.CommentIslandCount;
				OutResult.DesignDiagnostics.CommentIslandNodeCount += Island.Nodes.Num();
			}
			Islands.Add(MoveTemp(Island));
		}
		OutResult.DesignDiagnostics.NodeIslandCount = Islands.Num();

		TArray<FEdge> IslandLayoutExplicitEdges = ExplicitEdges;
		for (const int32 EdgeIndex : RankIgnoredEdges)
		{
			if (!Edges.IsValidIndex(EdgeIndex) || FeedbackEdges.Contains(EdgeIndex))
			{
				continue;
			}
			const FEdge& Edge = Edges[EdgeIndex];
			const int32* SourceIslandPtr = IslandByNode.Find(Edge.Source);
			const int32* TargetIslandPtr = IslandByNode.Find(Edge.Target);
			if (SourceIslandPtr && TargetIslandPtr && *SourceIslandPtr == *TargetIslandPtr)
			{
				IslandLayoutExplicitEdges.Add(Edge);
			}
		}

		for (FIsland& Island : Islands)
		{
			if (!Island.bCommentIsland)
			{
				PrepareIslandColumnsAndOrder(Nodes, Edges, IslandLayoutExplicitEdges, RankIgnoredEdges, Island, OutResult.DesignDiagnostics);
			}
		}

		for (const FIsland& Island : Islands)
		{
			int32 SinkCount = 0;
			for (const int32 NodeIndex : Island.Nodes)
			{
				bool bHasExplicitOutgoingInside = false;
				for (const FEdge& Edge : IslandLayoutExplicitEdges)
				{
					if (Edge.Source == NodeIndex && Island.NodeSet.Contains(Edge.Target))
					{
						bHasExplicitOutgoingInside = true;
						break;
					}
				}
				if (!bHasExplicitOutgoingInside)
				{
					++SinkCount;
				}
			}
			if (SinkCount > 1)
			{
				++OutResult.DesignDiagnostics.MultiSinkIslandCount;
			}
		}

		TArray<FIslandEdge> IslandEdges;
		for (const int32 EdgeIndex : IslandCutEdges)
		{
			if (!Edges.IsValidIndex(EdgeIndex))
			{
				continue;
			}
			const FEdge& Edge = Edges[EdgeIndex];
			const int32* SourceIslandPtr = IslandByNode.Find(Edge.Source);
			const int32* TargetIslandPtr = IslandByNode.Find(Edge.Target);
			const bool bFeedbackEdge = FeedbackEdges.Contains(EdgeIndex);
			if (!SourceIslandPtr || !TargetIslandPtr || (*SourceIslandPtr == *TargetIslandPtr && !bFeedbackEdge))
			{
				if (SourceIslandPtr && TargetIslandPtr && *SourceIslandPtr == *TargetIslandPtr)
				{
					++OutResult.DesignDiagnostics.ImplicitEdgeInsideIslandCount;
				}
				continue;
			}
			FIslandEdge IslandEdge;
			IslandEdge.EdgeIndex = EdgeIndex;
			IslandEdge.SourceIsland = *SourceIslandPtr;
			IslandEdge.TargetIsland = *TargetIslandPtr;
			IslandEdge.TargetSide = TEXT("left");
			IslandEdge.bFeedback = bFeedbackEdge;
			IslandEdge.bFanout = FanoutEdges.Contains(EdgeIndex);
			IslandEdge.bCommentBoundary = CrossesCommentIslandBoundary(CommentGroupByNode, Edge);
			const int32 IslandEdgeIndex = IslandEdges.Num();
			IslandEdges.Add(IslandEdge);
			Islands[IslandEdge.SourceIsland].OutgoingEdges.Add(IslandEdgeIndex);
			Islands[IslandEdge.TargetIsland].IncomingEdges.Add(IslandEdgeIndex);
		}
		OutResult.DesignDiagnostics.IslandEdgeCount = IslandEdges.Num();
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore islands_ready elapsed_ms=%.1f islands=%d island_edges=%d cut_edges=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			Islands.Num(),
			IslandEdges.Num(),
			IslandCutEdges.Num());
		AssignInternalBusLaneDemand(Nodes, Edges, Islands, IslandEdges);
		for (FIsland& Island : Islands)
		{
			if (Island.bCommentIsland)
			{
				FreezeCommentIslandLocalLayout(Nodes, Island, OutResult);
			}
			else
			{
				LayoutIslandInternally(Nodes, Edges, IslandLayoutExplicitEdges, RankIgnoredEdges, Island, Options, OutResult);
			}
		}
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore island_local_done elapsed_ms=%.1f internal_placements=%d input_chains=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			OutResult.DesignDiagnostics.IslandInternalPlacementCount,
			OutResult.DesignDiagnostics.InputChainCount);
		const FIslandLocalLayoutSnapshot IslandLocalLayoutSnapshot = FreezeIslandLocalLayout(Nodes, Islands, OutResult);
		for (FIslandEdge& IslandEdge : IslandEdges)
		{
			if (!Edges.IsValidIndex(IslandEdge.EdgeIndex))
			{
				continue;
			}
			const FEdge& Edge = Edges[IslandEdge.EdgeIndex];
			if (Nodes.IsValidIndex(Edge.Source))
			{
				IslandEdge.SourceLocal = GetSourcePort(Nodes[Edge.Source], Edge, true);
			}
			if (Nodes.IsValidIndex(Edge.Target))
			{
				IslandEdge.TargetLocal = GetTargetPort(Nodes[Edge.Target], Edge, true);
			}
		}
		TArray<FInternalBusGroup> InternalBusGroups;
		TArray<FBoundaryEntry> BoundaryEntries;
		TArray<FBoundaryBus> BoundaryBuses;
		TArray<FIslandPortCandidate> IslandPortCandidates;
		BuildInputEntryAndPortModel(
			Nodes,
			Edges,
			Islands,
			IslandEdges,
			Options,
			OutResult,
			InternalBusGroups,
			BoundaryEntries,
			BoundaryBuses,
			IslandPortCandidates);
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore input_model_done elapsed_ms=%.1f internal_bus=%d entries=%d port_candidates=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			OutResult.DesignDiagnostics.InternalBusGroupCount,
			OutResult.DesignDiagnostics.BoundaryEntryCount,
			OutResult.DesignDiagnostics.EndpointCandidateCount);
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore inter_island_solver_start elapsed_ms=%.1f"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0);
		OutResult.ReroutePlans = RunDeterministicInterIslandSolver(
			Nodes,
			Edges,
			Islands,
			IslandEdges,
			InternalBusGroups,
			IslandPortCandidates,
			Options,
			OutResult);
		if (OutResult.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount > 0)
		{
			UE_LOG(LogUeAgentInterface, Warning, TEXT("BlueprintLayoutCore failed residual island cycle count=%d"),
				OutResult.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount);
			return false;
		}
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore inter_island_solver_done elapsed_ms=%.1f reroutes=%d inter_island_buses=%d selected_endpoints=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			OutResult.ReroutePlans.Num(),
			OutResult.DesignDiagnostics.InterIslandBusCount,
			OutResult.DesignDiagnostics.SelectedEndpointCandidateCount);
		TSet<int32> IslandCutEdgeIndices;
		for (const FIslandEdge& IslandEdge : IslandEdges)
		{
			if (Edges.IsValidIndex(IslandEdge.EdgeIndex))
			{
				IslandCutEdgeIndices.Add(IslandEdge.EdgeIndex);
			}
		}
		AppendInputChainCollisionReroutePlans(
			Nodes,
			Edges,
			Islands,
			Options,
			OutResult.ReroutePlans,
			OutResult,
			&IslandCutEdgeIndices);
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore input_chain_guard_done elapsed_ms=%.1f reroutes=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			OutResult.ReroutePlans.Num());
		AppendIslandInternalReroutePlans(
			Nodes,
			Edges,
			Islands,
			IslandByNode,
			Options,
			OutResult.ReroutePlans,
			OutResult,
			&IslandCutEdgeIndices);
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore island_internal_routes_done elapsed_ms=%.1f multi_column=%d same_column_collision=%d reroutes=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			OutResult.DesignDiagnostics.IslandInternalMultiColumnBusCount,
			OutResult.DesignDiagnostics.IslandInternalSameColumnCollisionBusCount,
			OutResult.ReroutePlans.Num());
		SnapReroutePlansToPixelGrid(OutResult.ReroutePlans);
		const bool bIslandLocalLayoutStableBeforeApply = ValidateIslandLocalLayout(
			Nodes,
			Islands,
			IslandLocalLayoutSnapshot,
			false,
			TEXT("before_apply_island_offsets"),
			OutResult);
		ApplyIslandOffsets(Nodes, Islands);
		if (bIslandLocalLayoutStableBeforeApply)
		{
			ValidateIslandLocalLayout(
				Nodes,
				Islands,
				IslandLocalLayoutSnapshot,
				true,
				TEXT("after_apply_island_offsets"),
				OutResult);
		}

		OutResult.RankCount = 0;
		for (const FIsland& Island : Islands)
		{
			OutResult.RankCount = FMath::Max(OutResult.RankCount, Island.Rank + 1);
		}
		for (const FNode& Node : Nodes)
		{
			if (FMath::Abs(Node.NewX - Node.OldX) > 0.5 || FMath::Abs(Node.NewY - Node.OldY) > 0.5)
			{
				++OutResult.MovedNodeCount;
			}
		}

		OutResult.ReroutePlanCount = OutResult.ReroutePlans.Num();
		for (const FReroutePlan& Plan : OutResult.ReroutePlans)
		{
			if (Plan.RouteKind == ERouteKind::ExternalBus)
			{
				++OutResult.ExternalBusEdgeCount;
			}
			else if (Plan.RouteKind == ERouteKind::LocalBus)
			{
				++OutResult.LocalBusEdgeCount;
			}
		}

		OutResult.MetricsAfter = EvaluateMetrics(Nodes, Edges, nullptr, true, Options.NodePadding, &FeedbackEdges);
		OutResult.MetricsAfterReroute = EvaluateMetrics(Nodes, Edges, &OutResult.ReroutePlans, true, Options.NodePadding, &FeedbackEdges);
		if (OutResult.MetricsAfterReroute.EdgeNodeIntersectionCount > 0)
		{
			AppendEdgeNodeIntersectionDiagnostics(
				Nodes,
				Edges,
				&OutResult.ReroutePlans,
				true,
				TEXT("after_reroute_plan"),
				OutResult);
		}
		if (OutResult.MetricsAfterReroute.EdgeNodeIntersectionCount < OutResult.DesignDiagnostics.RouteThroughHardOccupancyCount)
		{
			OutResult.DesignDiagnostics.RouteThroughHardOccupancyCount = OutResult.MetricsAfterReroute.EdgeNodeIntersectionCount;
			OutResult.DesignDiagnostics.RouteThroughIslandCount = OutResult.MetricsAfterReroute.EdgeNodeIntersectionCount;
		}
		OutResult.IslandCount = Islands.Num();
		OutResult.IslandEdgeCount = IslandEdges.Num();
		auto CompleteIf = [&](const FString& Key, bool bComplete, bool bPartial, const FString& Detail)
		{
			AddDesignFeature(OutResult, Key, bComplete ? TEXT("complete") : (bPartial ? TEXT("partial") : TEXT("missing")), Detail);
		};
		CompleteIf(TEXT("whole_graph_preprocessing_geometry"), Options.MissingPinAnchorCount == 0, OutResult.NodeCount > 0, FString::Printf(TEXT("use_real_geometry=%s missing_pin_anchor=%d"), Options.bUseRealGeometry ? TEXT("true") : TEXT("false"), Options.MissingPinAnchorCount));
		CompleteIf(TEXT("semantic_graph_and_reroute_fold"), OutResult.NodeCount > 0, false, FString::Printf(TEXT("nodes=%d edges=%d"), OutResult.NodeCount, OutResult.EdgeCount));
		CompleteIf(TEXT("connection_alignment_shorten"), Options.OutputAccessOffsetX > 0.0 && Options.InputAccessOffsetX > 0.0 && Options.MissingPinAnchorCount == 0, Options.OutputAccessOffsetX > 0.0 && Options.InputAccessOffsetX > 0.0, FString::Printf(TEXT("output_offset_x=%.1f input_offset_x=%.1f source=%s missing_pin_anchor=%d"), Options.OutputAccessOffsetX, Options.InputAccessOffsetX, *Options.AccessOffsetSource, Options.MissingPinAnchorCount));
		CompleteIf(TEXT("auxiliary_global_rank"), OutResult.NodeCount > 0, false, FString::Printf(TEXT("rank_count=%d"), OutResult.RankCount));
		CompleteIf(TEXT("scc_feedback_rank_ignored_edges"), OutResult.DesignDiagnostics.SccCountBeforeCut == 0 || OutResult.DesignDiagnostics.SccFeedbackEdgeCount > 0, false, FString::Printf(TEXT("scc_before=%d feedback_edges=%d rank_ignored=%d"), OutResult.DesignDiagnostics.SccCountBeforeCut, OutResult.DesignDiagnostics.SccFeedbackEdgeCount, OutResult.DesignDiagnostics.RankIgnoredEdgeCount));
		CompleteIf(TEXT("fanout_implicit_edges"), OutResult.DesignDiagnostics.FanoutNodeCount == 0 || OutResult.DesignDiagnostics.FanoutImplicitEdgeCount > 0, false, FString::Printf(TEXT("fanout_nodes=%d fanout_edges=%d"), OutResult.DesignDiagnostics.FanoutNodeCount, OutResult.DesignDiagnostics.FanoutImplicitEdgeCount));
		CompleteIf(TEXT("comment_box_special_island"), OutResult.DesignDiagnostics.CommentIslandNodeCount == 0 || OutResult.DesignDiagnostics.CommentIslandCount > 0, false, FString::Printf(TEXT("comment_islands=%d comment_nodes=%d boundary_edges=%d"), OutResult.DesignDiagnostics.CommentIslandCount, OutResult.DesignDiagnostics.CommentIslandNodeCount, OutResult.DesignDiagnostics.CommentIslandBoundaryEdgeCount));
		CompleteIf(TEXT("weak_connected_node_islands"), OutResult.DesignDiagnostics.NodeIslandCount > 0, false, FString::Printf(TEXT("islands=%d multi_sink=%d"), OutResult.DesignDiagnostics.NodeIslandCount, OutResult.DesignDiagnostics.MultiSinkIslandCount));
		CompleteIf(TEXT("island_graph"), OutResult.DesignDiagnostics.IslandCutEdgeCount == OutResult.DesignDiagnostics.IslandEdgeCount + OutResult.DesignDiagnostics.ImplicitEdgeInsideIslandCount, OutResult.DesignDiagnostics.IslandEdgeCount > 0, FString::Printf(TEXT("island_cut=%d island_edges=%d inside=%d"), OutResult.DesignDiagnostics.IslandCutEdgeCount, OutResult.DesignDiagnostics.IslandEdgeCount, OutResult.DesignDiagnostics.ImplicitEdgeInsideIslandCount));
		CompleteIf(TEXT("right_anchored_island_rank"), OutResult.DesignDiagnostics.NodeIslandCount > 0, false, FString::Printf(TEXT("islands=%d rank_count=%d"), OutResult.DesignDiagnostics.NodeIslandCount, OutResult.RankCount));
		CompleteIf(TEXT("input_chain_attachment"), OutResult.DesignDiagnostics.InputChainNodeCount >= 0, false, FString::Printf(TEXT("chains=%d chain_nodes=%d"), OutResult.DesignDiagnostics.InputChainCount, OutResult.DesignDiagnostics.InputChainNodeCount));
		CompleteIf(TEXT("input_chain_forbidden_zone_and_collision_guard"), OutResult.DesignDiagnostics.InputChainCount == 0 || OutResult.DesignDiagnostics.InputChainForbiddenZoneCount >= 0, false, FString::Printf(TEXT("forbidden_zones=%d avoidances=%d collision_guards=%d rejected=%d"), OutResult.DesignDiagnostics.InputChainForbiddenZoneCount, OutResult.DesignDiagnostics.InputChainForbiddenAvoidanceCount, OutResult.DesignDiagnostics.InputChainCollisionRerouteCount, OutResult.DesignDiagnostics.InputChainCollisionRerouteRejectedCount));
		CompleteIf(TEXT("two_column_anchored_placement"), OutResult.DesignDiagnostics.IslandInternalPlacementCount == OutResult.DesignDiagnostics.NodeIslandCount, OutResult.DesignDiagnostics.AnchoredColumnPlacementCount > 0, FString::Printf(TEXT("island_internal=%d anchored_columns=%d unconnected=%d"), OutResult.DesignDiagnostics.IslandInternalPlacementCount, OutResult.DesignDiagnostics.AnchoredColumnPlacementCount, OutResult.DesignDiagnostics.AnchoredColumnUnconnectedItemCount));
		CompleteIf(TEXT("input_entry_internal_bus"), OutResult.DesignDiagnostics.IslandEdgeCount == 0 || (OutResult.DesignDiagnostics.InternalBusGroupCount > 0 && OutResult.DesignDiagnostics.BoundaryEntryCount > 0), OutResult.DesignDiagnostics.InternalBusGroupCount > 0, FString::Printf(TEXT("internal_bus=%d boundary_entry=%d"), OutResult.DesignDiagnostics.InternalBusGroupCount, OutResult.DesignDiagnostics.BoundaryEntryCount));
		CompleteIf(TEXT("boundary_bus_model"), OutResult.DesignDiagnostics.BoundaryBusCount >= OutResult.DesignDiagnostics.BoundaryBusRequiredCount, OutResult.DesignDiagnostics.InternalBusGroupCount > 0, FString::Printf(TEXT("boundary_bus=%d required=%d source_param=%d internal_bus=%d"), OutResult.DesignDiagnostics.BoundaryBusCount, OutResult.DesignDiagnostics.BoundaryBusRequiredCount, OutResult.DesignDiagnostics.SourceParamCount, OutResult.DesignDiagnostics.InternalBusGroupCount));
		CompleteIf(TEXT("island_internal_node_placement"), OutResult.DesignDiagnostics.IslandInternalPlacementCount == OutResult.DesignDiagnostics.NodeIslandCount && OutResult.MetricsAfter.OverlapCount == 0, OutResult.DesignDiagnostics.IslandInternalPlacementCount > 0, FString::Printf(TEXT("placements=%d overlap=%d"), OutResult.DesignDiagnostics.IslandInternalPlacementCount, OutResult.MetricsAfter.OverlapCount));
		CompleteIf(TEXT("island_local_layout_freeze"), OutResult.DesignDiagnostics.IslandLocalFrozenNodeCount == OutResult.NodeCount && OutResult.DesignDiagnostics.IslandLocalMembershipMutationCount == 0 && OutResult.DesignDiagnostics.IslandLocalMutationCount == 0, OutResult.DesignDiagnostics.IslandLocalFrozenNodeCount > 0 && OutResult.DesignDiagnostics.IslandLocalMutationCount == 0, FString::Printf(TEXT("frozen_nodes=%d nodes=%d membership_mutations=%d local_mutations=%d max_delta=%.3f"), OutResult.DesignDiagnostics.IslandLocalFrozenNodeCount, OutResult.NodeCount, OutResult.DesignDiagnostics.IslandLocalMembershipMutationCount, OutResult.DesignDiagnostics.IslandLocalMutationCount, OutResult.DesignDiagnostics.IslandLocalMaxMutation));
		CompleteIf(TEXT("frozen_endpoint_candidates"), OutResult.DesignDiagnostics.IslandEdgeCount == 0 || (OutResult.DesignDiagnostics.EndpointCandidateCount >= OutResult.DesignDiagnostics.IslandEdgeCount && OutResult.DesignDiagnostics.SelectedEndpointCandidateCount == OutResult.DesignDiagnostics.IslandEdgeCount), OutResult.DesignDiagnostics.EndpointCandidateCount > 0, FString::Printf(TEXT("candidates=%d selected=%d island_edges=%d"), OutResult.DesignDiagnostics.EndpointCandidateCount, OutResult.DesignDiagnostics.SelectedEndpointCandidateCount, OutResult.DesignDiagnostics.IslandEdgeCount));
		CompleteIf(TEXT("inter_island_topological_x"), OutResult.DesignDiagnostics.IslandEdgeCount == 0 || (OutResult.DesignDiagnostics.PlacementGraphEdgeCount + OutResult.DesignDiagnostics.RouteOnlyEdgeCount == OutResult.DesignDiagnostics.IslandEdgeCount && OutResult.DesignDiagnostics.IslandTopologicalOrderCount == OutResult.DesignDiagnostics.NodeIslandCount && OutResult.DesignDiagnostics.InvalidForwardConnectionCount == 0), OutResult.DesignDiagnostics.IslandTopologicalOrderCount > 0, FString::Printf(TEXT("topo_order=%d islands=%d placement_edges=%d route_only=%d residual_cycles=%d invalid_forward=%d"), OutResult.DesignDiagnostics.IslandTopologicalOrderCount, OutResult.DesignDiagnostics.NodeIslandCount, OutResult.DesignDiagnostics.PlacementGraphEdgeCount, OutResult.DesignDiagnostics.RouteOnlyEdgeCount, OutResult.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount, OutResult.DesignDiagnostics.InvalidForwardConnectionCount));
		CompleteIf(TEXT("root_continuous_y_order"), OutResult.DesignDiagnostics.RootIslandCount == 0 || (OutResult.DesignDiagnostics.RootIslandTopYCount == OutResult.DesignDiagnostics.RootIslandCount && OutResult.DesignDiagnostics.RootContinuousOrderExactLimitExceeded == 0), OutResult.DesignDiagnostics.RootIslandTopYCount > 0, FString::Printf(TEXT("roots=%d placed=%d components=%d score=%.1f inversion=%.1f exact_limit=%d"), OutResult.DesignDiagnostics.RootIslandCount, OutResult.DesignDiagnostics.RootIslandTopYCount, OutResult.DesignDiagnostics.RootShareComponentCount, OutResult.DesignDiagnostics.RootContinuousOrderScore, OutResult.DesignDiagnostics.RootContinuousOrderInversionScore, OutResult.DesignDiagnostics.RootContinuousOrderExactLimitExceeded));
		CompleteIf(TEXT("remaining_island_topological_y"), OutResult.DesignDiagnostics.NodeIslandCount == 0 || OutResult.DesignDiagnostics.RootIslandTopYCount + OutResult.DesignDiagnostics.IslandPlacementOrderCount >= OutResult.DesignDiagnostics.NodeIslandCount, OutResult.DesignDiagnostics.IslandPlacementOrderCount > 0, FString::Printf(TEXT("root_placed=%d remaining_placed=%d islands=%d"), OutResult.DesignDiagnostics.RootIslandTopYCount, OutResult.DesignDiagnostics.IslandPlacementOrderCount, OutResult.DesignDiagnostics.NodeIslandCount));
		CompleteIf(TEXT("inter_island_direct_bus_classification"), OutResult.DesignDiagnostics.IslandEdgeCount == 0 || OutResult.DesignDiagnostics.DirectIslandConnectionCount + OutResult.DesignDiagnostics.BusIslandConnectionCount + OutResult.DesignDiagnostics.InterIslandFeedbackBusConnectionCount >= OutResult.DesignDiagnostics.SelectedEndpointCandidateCount, OutResult.DesignDiagnostics.SelectedEndpointCandidateCount > 0, FString::Printf(TEXT("selected=%d direct=%d bus=%d feedback_bus=%d"), OutResult.DesignDiagnostics.SelectedEndpointCandidateCount, OutResult.DesignDiagnostics.DirectIslandConnectionCount, OutResult.DesignDiagnostics.BusIslandConnectionCount, OutResult.DesignDiagnostics.InterIslandFeedbackBusConnectionCount));
		CompleteIf(TEXT("source_order_fanout_route_groups"), OutResult.DesignDiagnostics.IslandEdgeCount == 0 || (OutResult.DesignDiagnostics.SourceOrderGroupCount > 0 && OutResult.DesignDiagnostics.RouteGroupCount >= 0), OutResult.DesignDiagnostics.SourceOrderGroupCount > 0, FString::Printf(TEXT("source_order_groups=%d fanout_groups=%d route_groups=%d"), OutResult.DesignDiagnostics.SourceOrderGroupCount, OutResult.DesignDiagnostics.FanoutIntentGroupCount, OutResult.DesignDiagnostics.RouteGroupCount));
		CompleteIf(TEXT("inter_island_bus_bundle_planning"), OutResult.DesignDiagnostics.InterIslandBusCount == 0 || (OutResult.DesignDiagnostics.InterIslandBusBundleCount > 0 && OutResult.DesignDiagnostics.InterIslandBusSlotAssignmentCount >= OutResult.DesignDiagnostics.InterIslandBusCount && OutResult.DesignDiagnostics.RouteThroughIslandCount == 0), OutResult.DesignDiagnostics.InterIslandBusCount > 0, FString::Printf(TEXT("buses=%d bundles=%d slots=%d max_slot=%d collision_boxes=%d island_boxes=%d route_through_island=%d"), OutResult.DesignDiagnostics.InterIslandBusCount, OutResult.DesignDiagnostics.InterIslandBusBundleCount, OutResult.DesignDiagnostics.InterIslandBusSlotAssignmentCount, OutResult.DesignDiagnostics.InterIslandBusMaxSlotCount, OutResult.DesignDiagnostics.InterIslandBusBundleCollisionBoxCount, OutResult.DesignDiagnostics.IslandBusCollisionBoxCount, OutResult.DesignDiagnostics.RouteThroughIslandCount));
		CompleteIf(TEXT("inter_island_branch_slot_planning"), OutResult.DesignDiagnostics.IncomingBusBranchCount == 0 || (OutResult.DesignDiagnostics.InterIslandBusBranchXSlotCount > 0 && OutResult.DesignDiagnostics.InterIslandBusBranchKnotCount + OutResult.DesignDiagnostics.InterIslandBusBranchKnotOmittedCount >= OutResult.DesignDiagnostics.IncomingBusBranchCount), OutResult.DesignDiagnostics.IncomingBusBranchCount > 0, FString::Printf(TEXT("branches=%d x_slots=%d right_shift=%d knots=%d omitted=%d"), OutResult.DesignDiagnostics.IncomingBusBranchCount, OutResult.DesignDiagnostics.InterIslandBusBranchXSlotCount, OutResult.DesignDiagnostics.IslandBusBranchRightShiftCount, OutResult.DesignDiagnostics.InterIslandBusBranchKnotCount, OutResult.DesignDiagnostics.InterIslandBusBranchKnotOmittedCount));
		bool bPixelSnappedNodes = true;
		for (const FNode& Node : Nodes)
		{
			bPixelSnappedNodes = bPixelSnappedNodes
				&& FMath::IsNearlyEqual(Node.NewX, SnapGraphCoordinate(Node.NewX), 0.001)
				&& FMath::IsNearlyEqual(Node.NewY, SnapGraphCoordinate(Node.NewY), 0.001);
		}
		bool bPixelSnappedReroutes = true;
		for (const FReroutePlan& Plan : OutResult.ReroutePlans)
		{
			for (const FVector2D& Point : Plan.Points)
			{
				bPixelSnappedReroutes = bPixelSnappedReroutes
					&& FMath::IsNearlyEqual(Point.X, SnapGraphCoordinate(Point.X), 0.001)
					&& FMath::IsNearlyEqual(Point.Y, SnapGraphCoordinate(Point.Y), 0.001);
			}
		}
		CompleteIf(TEXT("pixel_grid_writeback_snap"), bPixelSnappedNodes && bPixelSnappedReroutes, false, FString::Printf(TEXT("nodes=%s reroutes=%s"), bPixelSnappedNodes ? TEXT("snapped") : TEXT("fractional"), bPixelSnappedReroutes ? TEXT("snapped") : TEXT("fractional")));
		CompleteIf(TEXT("metrics_diagnostics_writeback_contract"), OutResult.InvalidIslandEdgeCount == 0 && OutResult.DesignDiagnostics.RouteThroughIslandCount == 0, OutResult.InvalidIslandEdgeCount == 0, FString::Printf(TEXT("invalid_island_edge=%d route_through_island=%d"), OutResult.InvalidIslandEdgeCount, OutResult.DesignDiagnostics.RouteThroughIslandCount));

		OutResult.bDesignComplete = OutResult.DesignFeatureMissingCount == 0
			&& OutResult.DesignFeaturePartialCount == 0
			&& OutResult.MetricsAfter.OverlapCount == 0
			&& OutResult.MetricsAfter.BackwardEdgeCount == 0
			&& OutResult.InvalidIslandEdgeCount == 0
			&& OutResult.DesignDiagnostics.RouteThroughIslandCount == 0;

		for (const FIsland& Island : Islands)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("island_id"), Island.Id);
			Obj->SetNumberField(TEXT("rank"), Island.Rank);
			Obj->SetNumberField(TEXT("node_count"), Island.Nodes.Num());
			Obj->SetBoolField(TEXT("has_exec_node"), Island.bHasExec);
			Obj->SetBoolField(TEXT("comment_island"), Island.bCommentIsland);
			Obj->SetNumberField(TEXT("comment_node_index"), Island.CommentNodeIndex);
			Obj->SetNumberField(TEXT("comment_group"), Island.CommentGroup);
			Obj->SetObjectField(TEXT("local_bounds"), RectToJson(Island.LocalBounds));
			Obj->SetArrayField(TEXT("node_indices"), IntArrayToJson(Island.Nodes));
			Obj->SetNumberField(TEXT("incoming_island_edge_count"), Island.IncomingEdges.Num());
			Obj->SetNumberField(TEXT("outgoing_island_edge_count"), Island.OutgoingEdges.Num());
			OutResult.IslandDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
		}
		for (const FIslandEdge& IslandEdge : IslandEdges)
		{
			const FEdge& Edge = Edges[IslandEdge.EdgeIndex];
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("edge_index"), IslandEdge.EdgeIndex);
			Obj->SetNumberField(TEXT("source_island_id"), IslandEdge.SourceIsland);
			Obj->SetNumberField(TEXT("target_island_id"), IslandEdge.TargetIsland);
			Obj->SetStringField(TEXT("source_pin"), Edge.SourcePin);
			Obj->SetStringField(TEXT("target_pin"), Edge.TargetPin);
			Obj->SetBoolField(TEXT("implicit_feedback_edge"), IslandEdge.bFeedback);
			Obj->SetBoolField(TEXT("implicit_fanout_edge"), IslandEdge.bFanout);
			Obj->SetBoolField(TEXT("comment_boundary_edge"), IslandEdge.bCommentBoundary);
			Obj->SetBoolField(TEXT("rank_ignored_feedback_edge"), IslandEdge.bFeedback);
			Obj->SetBoolField(TEXT("island_cut_fanout_edge"), IslandEdge.bFanout);
			Obj->SetStringField(TEXT("target_entry_side"), IslandEdge.TargetSide);
			OutResult.IslandEdgeDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
		}
		if (!OutResult.bDesignComplete)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("reason"), TEXT("final_metrics_failed"));
			Obj->SetNumberField(TEXT("overlap_count"), OutResult.MetricsAfter.OverlapCount);
			Obj->SetNumberField(TEXT("backward_edge_count"), OutResult.MetricsAfter.BackwardEdgeCount);
			OutResult.RejectionDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return true;
	}

	TSharedPtr<FJsonObject> MetricsToJson(const FMetrics& Metrics)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("overlap_count"), Metrics.OverlapCount);
		Obj->SetNumberField(TEXT("edge_crossing_count"), Metrics.EdgeCrossingCount);
		Obj->SetNumberField(TEXT("edge_node_intersection_count"), Metrics.EdgeNodeIntersectionCount);
		Obj->SetNumberField(TEXT("backward_edge_count"), Metrics.BackwardEdgeCount);
		Obj->SetNumberField(TEXT("pin_order_violation_count"), Metrics.PinOrderViolationCount);
		Obj->SetNumberField(TEXT("total_wire_manhattan_length"), Metrics.TotalWireLength);
		Obj->SetNumberField(TEXT("total_non_exec_wire_manhattan_length"), Metrics.TotalNonExecWireLength);
		Obj->SetNumberField(TEXT("layout_width"), Metrics.LayoutWidth);
		Obj->SetNumberField(TEXT("layout_height"), Metrics.LayoutHeight);
		Obj->SetNumberField(TEXT("layout_area"), Metrics.LayoutArea);
		return Obj;
	}

	static void AddSelfTestCase(FSelfTestResult& Result, const FString& Name, const bool bPassed, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("status"), bPassed ? TEXT("passed") : TEXT("failed"));
		Obj->SetStringField(TEXT("detail"), Detail);
		Result.Cases.Add(MakeShared<FJsonValueObject>(Obj));
		++Result.CaseCount;
		if (bPassed)
		{
			++Result.PassedCount;
		}
		else
		{
			++Result.FailedCount;
		}
	}


	static void RunSourceColumnCollisionSelfTests(FSelfTestResult& Result, const FOptions& Options)
	{
		auto MakeNode = [](const int32 Index, const TCHAR* Prefix, const double Width, const double Height, const int32 Rank, const double X, const double Y) -> FNode
		{
			FNode Node;
			Node.Index = Index;
			Node.Guid = FString::Printf(TEXT("%s_%d"), Prefix, Index);
			Node.Title = Node.Guid;
			Node.Width = Width;
			Node.Height = Height;
			Node.InputPinNames = { TEXT("in") };
			Node.InputPinAnchors = { FVector2D(0.0, 36.0) };
			Node.InputPinAnchorValid = { 1 };
			Node.OutputPinNames = { TEXT("out") };
			Node.OutputPinAnchors = { FVector2D(Width, 36.0) };
			Node.OutputPinAnchorValid = { 1 };
			Node.bMeasuredGeometry = true;
			Node.Rank = Rank;
			Node.NewX = X;
			Node.NewY = Y;
			return Node;
		};

		auto RunCase = [&Result, &Options](
			const FString& Name,
			TArray<FNode> Nodes,
			TArray<TArray<int32>> Columns,
			const double ExpectedRightBoundaryX,
			const bool bExpectGuard)
		{
			TArray<FEdge> Edges = {
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.Columns = MoveTemp(Columns);
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			IslandByNode.Add(0, 0);
			IslandByNode.Add(1, 0);
			IslandByNode.Add(2, 0);
			TArray<FReroutePlan> Plans;
			FRunResult Run;
			AppendIslandInternalReroutePlans(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			const bool bHasExpectedGuard =
				Plans.Num() == 1
				&& Plans[0].EdgeIndex == 0
				&& Plans[0].Reason == TEXT("island_internal_same_column_right_boundary_bus")
				&& Plans[0].Points.Num() == 1
				&& FMath::IsNearlyEqual(Plans[0].Points[0].X, ExpectedRightBoundaryX, 1.0)
				&& FMath::IsNearlyEqual(Plans[0].Points[0].Y, 36.0, 1.0)
				&& Run.DesignDiagnostics.IslandInternalSameColumnCollisionBusCount == 1
				&& Run.DesignDiagnostics.IslandInternalMultiColumnBusCount == 0;
			const bool bHasNoGuard =
				Plans.Num() == 0
				&& Run.DesignDiagnostics.IslandInternalSameColumnCollisionBusCount == 0;
			AddSelfTestCase(
				Result,
				Name,
				bExpectGuard ? bHasExpectedGuard : bHasNoGuard,
				FString::Printf(TEXT("plans=%d same_column=%d multi=%d first=(%.1f,%.1f) expected_x=%.1f reason=%s"),
					Plans.Num(),
					Run.DesignDiagnostics.IslandInternalSameColumnCollisionBusCount,
					Run.DesignDiagnostics.IslandInternalMultiColumnBusCount,
					Plans.Num() > 0 && Plans[0].Points.Num() > 0 ? Plans[0].Points[0].X : -1.0,
					Plans.Num() > 0 && Plans[0].Points.Num() > 0 ? Plans[0].Points[0].Y : -1.0,
					ExpectedRightBoundaryX,
					Plans.Num() > 0 ? *Plans[0].Reason : TEXT("none")));
		};

		{
			TArray<FNode> Nodes = {
				MakeNode(0, TEXT("island_internal_same_col"), 120.0, 72.0, 0, 0.0, 0.0),
				MakeNode(1, TEXT("island_internal_same_col"), 172.0, 100.0, 0, 0.0, 110.0),
				MakeNode(2, TEXT("island_internal_same_col"), 120.0, 72.0, 0, 0.0, 240.0)
			};
			TArray<TArray<int32>> Columns;
			Columns.SetNum(1);
			Columns[0] = {0, 1, 2};
			RunCase(TEXT("island_internal_same_column_collision_extends_to_column_right_boundary"), MoveTemp(Nodes), MoveTemp(Columns), 172.0, true);
		}

		{
			TArray<FNode> Nodes = {
				MakeNode(0, TEXT("source_column_triangles"), 120.0, 72.0, 0, 0.0, 0.0),
				MakeNode(1, TEXT("source_column_triangles"), 260.0, 120.0, 0, 0.0, 96.0),
				MakeNode(2, TEXT("source_column_triangles"), 120.0, 72.0, 1, 360.0, 180.0)
			};
			TArray<TArray<int32>> Columns;
			Columns.SetNum(2);
			Columns[0] = {0, 1};
			Columns[1] = {2};
			RunCase(TEXT("source_column_output_triangle_collision_extends_to_column_right_boundary"), MoveTemp(Nodes), MoveTemp(Columns), 260.0, true);
		}

		{
			TArray<FNode> Nodes = {
				MakeNode(0, TEXT("source_column_access"), 260.0, 72.0, 0, 0.0, 0.0),
				MakeNode(1, TEXT("source_column_access"), 340.0, 120.0, 0, 0.0, 78.0),
				MakeNode(2, TEXT("source_column_access"), 120.0, 72.0, 1, 520.0, 180.0)
			};
			TArray<TArray<int32>> Columns;
			Columns.SetNum(2);
			Columns[0] = {0, 1};
			Columns[1] = {2};
			RunCase(TEXT("source_column_collision_guard_uses_column_right_boundary"), MoveTemp(Nodes), MoveTemp(Columns), 340.0, true);
		}

		{
			TArray<FNode> Nodes = {
				MakeNode(0, TEXT("source_column_endpoint_triangles"), 260.0, 72.0, 0, 0.0, 0.0),
				MakeNode(1, TEXT("source_column_endpoint_triangles"), 260.0, 120.0, 0, 0.0, 96.0),
				MakeNode(2, TEXT("source_column_endpoint_triangles"), 120.0, 72.0, 1, 520.0, 180.0)
			};
			TArray<TArray<int32>> Columns;
			Columns.SetNum(2);
			Columns[0] = {0, 1};
			Columns[1] = {2};
			RunCase(TEXT("source_column_collision_uses_endpoint_offset_triangles"), MoveTemp(Nodes), MoveTemp(Columns), 260.0, false);
		}
	}
	static void RunInterIslandBranchSlotSelfTests(FSelfTestResult& Result, const FOptions& Options)
	{
		TArray<FIsland> Islands;
		Islands.SetNum(4);
		for (int32 Index = 0; Index < Islands.Num(); ++Index)
		{
			Islands[Index].Id = Index;
			Islands[Index].LocalBounds = {0.0, 0.0, 140.0, 120.0};
		}
		Islands[0].X = 0.0;
		Islands[0].Y = 300.0;
		Islands[1].X = 600.0;
		Islands[1].Y = 0.0;
		Islands[2].X = 0.0;
		Islands[2].Y = 0.0;
		Islands[3].X = 600.0;
		Islands[3].Y = 240.0;

		TArray<FIslandEdge> IslandEdges;
		IslandEdges.SetNum(2);
		IslandEdges[0].EdgeIndex = 0;
		IslandEdges[0].SourceIsland = 0;
		IslandEdges[0].TargetIsland = 1;
		IslandEdges[0].SourceLocal = FVector2D(140.0, 100.0);
		IslandEdges[0].TargetLocal = FVector2D(0.0, 130.0);
		IslandEdges[1].EdgeIndex = 1;
		IslandEdges[1].SourceIsland = 2;
		IslandEdges[1].TargetIsland = 3;
		IslandEdges[1].SourceLocal = FVector2D(140.0, 100.0);
		IslandEdges[1].TargetLocal = FVector2D(0.0, 180.0);

		TArray<FDeterministicInterIslandBus> Buses;
		Buses.SetNum(2);
		for (int32 BusIndex = 0; BusIndex < Buses.Num(); ++BusIndex)
		{
			Buses[BusIndex].Id = BusIndex;
			Buses[BusIndex].Key = FString::Printf(TEXT("slot_order_bus_%d"), BusIndex);
			Buses[BusIndex].FinalY = 100.0;
			Buses[BusIndex].SlotIndex = BusIndex;
			Buses[BusIndex].ConnectionIndices = {BusIndex};
		}

		TArray<FDeterministicIslandConnection> Connections;
		Connections.SetNum(2);
		for (int32 ConnectionIndex = 0; ConnectionIndex < Connections.Num(); ++ConnectionIndex)
		{
			const FIslandEdge& Edge = IslandEdges[ConnectionIndex];
			Connections[ConnectionIndex].bBus = true;
			Connections[ConnectionIndex].BusId = ConnectionIndex;
			Connections[ConnectionIndex].Endpoint.EdgeIndex = Edge.EdgeIndex;
			Connections[ConnectionIndex].Endpoint.SourceIsland = Edge.SourceIsland;
			Connections[ConnectionIndex].Endpoint.TargetIsland = Edge.TargetIsland;
			Connections[ConnectionIndex].Endpoint.SourceLocal = Edge.SourceLocal;
			Connections[ConnectionIndex].Endpoint.EntryLocal = Edge.TargetLocal;
			Connections[ConnectionIndex].Endpoint.ActualTargetLocal = Edge.TargetLocal;
		}

		TSet<int32> RouteOnlyIslandEdgeIndices;
		const TArray<int32> TopoOrder = {0, 2, 1, 3};
		FRunResult Run;
		TArray<FNode> EmptyNodes;
		TArray<FEdge> EmptyEdges;
		TArray<FInternalBusGroup> EmptyInternalBusGroups;
		TArray<FIslandPortCandidate> EmptyCandidates;
		ResolveBusBranchSlotsAndRightShift(
			EmptyNodes,
			EmptyEdges,
			EmptyInternalBusGroups,
			Islands,
			Connections,
			EmptyCandidates,
			Buses,
			IslandEdges,
			RouteOnlyIslandEdgeIndices,
			TopoOrder,
			Options,
			Run,
			true);

		const bool bTargetNearBusGetsRightLane =
			Connections[0].BranchSlotIndex == 0
			&& Connections[1].BranchSlotIndex == 1
			&& Run.DesignDiagnostics.InterIslandBusBranchXSlotCount >= 2;
		AddSelfTestCase(Result,
			TEXT("inter_island_branch_x_slot_prefers_target_endpoint_near_bus"),
			bTargetNearBusGetsRightLane,
			FString::Printf(TEXT("slot_near=%d slot_far=%d x_slots=%d incoming=%d"),
				Connections[0].BranchSlotIndex,
				Connections[1].BranchSlotIndex,
				Run.DesignDiagnostics.InterIslandBusBranchXSlotCount,
				Run.DesignDiagnostics.IncomingBusBranchCount));
	}

	static void RunInterIslandBusSlotBiasSelfTests(FSelfTestResult& Result)
	{
		{
			TArray<FDeterministicIslandConnection> Connections;
			Connections.SetNum(2);
			for (int32 ConnectionIndex = 0; ConnectionIndex < Connections.Num(); ++ConnectionIndex)
			{
				Connections[ConnectionIndex].BusId = 0;
				Connections[ConnectionIndex].SourcePoint = FVector2D(0.0, 20.0);
				Connections[ConnectionIndex].ActualTargetPoint = FVector2D(400.0, 150.0 + ConnectionIndex * 20.0);
				Connections[ConnectionIndex].EntryPoint = Connections[ConnectionIndex].ActualTargetPoint;
			}

			TArray<FDeterministicInterIslandBus> Buses;
			Buses.SetNum(1);
			Buses[0].Id = 0;
			Buses[0].SourcePoint = FVector2D(0.0, 20.0);
			Buses[0].PreferredY = 100.0;
			Buses[0].ConnectionIndices = {0, 1};

			ComputeInterIslandBusSlotBiases(Connections, Buses);
			const bool bSourceInterfaceCountedOnce =
				Buses[0].UpperAttachmentCount == 1
				&& Buses[0].LowerAttachmentCount == 2
				&& Buses[0].SlotBias == -1;
			AddSelfTestCase(Result,
				TEXT("inter_island_bus_slot_bias_deduplicates_source_interface"),
				bSourceInterfaceCountedOnce,
				FString::Printf(TEXT("upper=%d lower=%d bias=%d"),
					Buses[0].UpperAttachmentCount,
					Buses[0].LowerAttachmentCount,
					Buses[0].SlotBias));
		}

		{
			TArray<FDeterministicInterIslandBus> Buses;
			Buses.SetNum(2);
			for (int32 BusIndex = 0; BusIndex < Buses.Num(); ++BusIndex)
			{
				Buses[BusIndex].Id = BusIndex;
				Buses[BusIndex].BundleId = 0;
				Buses[BusIndex].XMin = 0.0;
				Buses[BusIndex].XMax = 400.0;
				Buses[BusIndex].PreferredY = 100.0;
				Buses[BusIndex].SlotBias = BusIndex == 0 ? 2 : -2;
			}

			FDeterministicInterIslandBusBundle Bundle;
			Bundle.Id = 0;
			Bundle.BusIds = {0, 1};
			FRunResult Run;
			AssignInterIslandBusSlotsByBias(Buses, Bundle, Run, true);

			const bool bUpperBiasGetsUpperSlot =
				Bundle.MaxSlotIndex == 1
				&& Buses[0].SignedSlotIndex < Buses[1].SignedSlotIndex
				&& Buses[0].SlotIndex < Buses[1].SlotIndex
				&& Run.DesignDiagnostics.InterIslandBusSlotAssignmentCount == 2;
			AddSelfTestCase(Result,
				TEXT("inter_island_bus_slot_bias_prefers_upper_and_lower_slots"),
				bUpperBiasGetsUpperSlot,
				FString::Printf(TEXT("signed=(%d,%d) slot=(%d,%d) max=%d assignments=%d"),
					Buses[0].SignedSlotIndex,
					Buses[1].SignedSlotIndex,
					Buses[0].SlotIndex,
					Buses[1].SlotIndex,
					Bundle.MaxSlotIndex,
					Run.DesignDiagnostics.InterIslandBusSlotAssignmentCount));
		}
	}

	static void RunA49IslandOffsetMergeSelfTests(FSelfTestResult& Result)
	{
		const double DownOnce = MergeA49IslandDirectionalOffset(0.0, 120.0, 1);
		const double DownMerged = MergeA49IslandDirectionalOffset(DownOnce, 300.0, 1);
		const double DownSmallerIgnored = MergeA49IslandDirectionalOffset(DownMerged, 180.0, 1);
		const double UpOnce = MergeA49IslandDirectionalOffset(0.0, -80.0, -1);
		const double UpMerged = MergeA49IslandDirectionalOffset(UpOnce, -200.0, -1);
		const double UpSmallerIgnored = MergeA49IslandDirectionalOffset(UpMerged, -120.0, -1);
		const bool bDirectionalOffsetTakesExtremum =
			FMath::IsNearlyEqual(DownOnce, 120.0, 0.001)
			&& FMath::IsNearlyEqual(DownMerged, 300.0, 0.001)
			&& FMath::IsNearlyEqual(DownSmallerIgnored, 300.0, 0.001)
			&& FMath::IsNearlyEqual(UpOnce, -80.0, 0.001)
			&& FMath::IsNearlyEqual(UpMerged, -200.0, 0.001)
			&& FMath::IsNearlyEqual(UpSmallerIgnored, -200.0, 0.001);
		AddSelfTestCase(Result,
			TEXT("a49_island_directional_offset_merges_instead_of_accumulates"),
			bDirectionalOffsetTakesExtremum,
			FString::Printf(TEXT("down=%.1f/%.1f/%.1f up=%.1f/%.1f/%.1f"),
				DownOnce,
				DownMerged,
				DownSmallerIgnored,
				UpOnce,
				UpMerged,
				UpSmallerIgnored));
	}

	FSelfTestResult RunSelfTest()
	{
		FSelfTestResult Result;
		auto AddCase = [&Result](const FString& Name, bool bPassed, const FString& Detail)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Name);
			Obj->SetStringField(TEXT("status"), bPassed ? TEXT("passed") : TEXT("failed"));
			Obj->SetStringField(TEXT("detail"), Detail);
			Result.Cases.Add(MakeShared<FJsonValueObject>(Obj));
			++Result.CaseCount;
			if (bPassed)
			{
				++Result.PassedCount;
			}
			else
			{
				++Result.FailedCount;
			}
		};

		FOptions Options;
		Options.HorizontalSpacing = 260.0;
		Options.VerticalSpacing = 120.0;
		Options.NodePadding = 32.0;
		Options.OutputAccessOffsetX = 24.0;
		Options.InputAccessOffsetX = 24.0;
		Options.AccessOffsetSource = TEXT("self_test");
		Options.bUseRealGeometry = true;
		Options.MissingPinAnchorCount = 0;

		{
			TArray<FNode> MissingAnchorNodes;
			MissingAnchorNodes.SetNum(2);
			MissingAnchorNodes[0].Index = 0;
			MissingAnchorNodes[0].Guid = TEXT("missing_source");
			MissingAnchorNodes[0].OutputPinNames = { TEXT("out") };
			MissingAnchorNodes[0].OutputPinAnchors = { FVector2D::ZeroVector };
			MissingAnchorNodes[0].OutputPinAnchorValid = { 0 };
			MissingAnchorNodes[1].Index = 1;
			MissingAnchorNodes[1].Guid = TEXT("missing_target");
			MissingAnchorNodes[1].InputPinNames = { TEXT("in") };
			MissingAnchorNodes[1].InputPinAnchors = { FVector2D::ZeroVector };
			MissingAnchorNodes[1].InputPinAnchorValid = { 0 };
			TArray<FEdge> MissingAnchorEdges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult MissingAnchorRun;
			const bool bRejectedMissingAnchors = !RunLayout(MissingAnchorNodes, MissingAnchorEdges, Options, MissingAnchorRun);
			AddCase(
				TEXT("layout_rejects_missing_real_pin_anchor"),
				bRejectedMissingAnchors,
				FString::Printf(TEXT("accepted=%d nodes=%d edges=%d"), bRejectedMissingAnchors ? 0 : 1, MissingAnchorRun.NodeCount, MissingAnchorRun.EdgeCount));
		}

		{
			TArray<FNode> MissingGeometryNodes;
			MissingGeometryNodes.SetNum(2);
			for (int32 Index = 0; Index < MissingGeometryNodes.Num(); ++Index)
			{
				MissingGeometryNodes[Index].Index = Index;
				MissingGeometryNodes[Index].Guid = FString::Printf(TEXT("missing_geometry_%d"), Index);
				MissingGeometryNodes[Index].Width = 120.0;
				MissingGeometryNodes[Index].Height = 72.0;
				MissingGeometryNodes[Index].bMeasuredGeometry = false;
			}
			MissingGeometryNodes[0].OutputPinNames = { TEXT("out") };
			MissingGeometryNodes[0].OutputPinAnchors = { FVector2D(120.0, 36.0) };
			MissingGeometryNodes[0].OutputPinAnchorValid = { 1 };
			MissingGeometryNodes[1].InputPinNames = { TEXT("in") };
			MissingGeometryNodes[1].InputPinAnchors = { FVector2D(0.0, 36.0) };
			MissingGeometryNodes[1].InputPinAnchorValid = { 1 };
			TArray<FEdge> MissingGeometryEdges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult MissingGeometryRun;
			const bool bRejectedMissingGeometry = !RunLayout(MissingGeometryNodes, MissingGeometryEdges, Options, MissingGeometryRun);
			AddCase(
				TEXT("layout_rejects_missing_real_node_geometry"),
				bRejectedMissingGeometry,
				FString::Printf(TEXT("accepted=%d nodes=%d edges=%d"), bRejectedMissingGeometry ? 0 : 1, MissingGeometryRun.NodeCount, MissingGeometryRun.EdgeCount));
		}

		auto EnsureSyntheticRealPinAnchorsForEdges = [](TArray<FNode>& Nodes, const TArray<FEdge>& Edges)
		{
			for (FNode& Node : Nodes)
			{
				Node.bMeasuredGeometry = true;
			}
			auto EnsureAnchor = [](FNode& Node, const bool bInput, const int32 PinIndex)
			{
				if (PinIndex == INDEX_NONE)
				{
					return;
				}
				TArray<FString>& PinNames = bInput ? Node.InputPinNames : Node.OutputPinNames;
				TArray<FVector2D>& Anchors = bInput ? Node.InputPinAnchors : Node.OutputPinAnchors;
				TArray<uint8>& AnchorValid = bInput ? Node.InputPinAnchorValid : Node.OutputPinAnchorValid;
				while (PinNames.Num() <= PinIndex)
				{
					PinNames.Add(FString::Printf(TEXT("pin_%d"), PinNames.Num()));
				}
				Anchors.SetNum(PinNames.Num());
				AnchorValid.SetNum(PinNames.Num());
				if (AnchorValid[PinIndex] == 0)
				{
					Anchors[PinIndex] = FVector2D(bInput ? 0.0 : Node.Width, 35.0 + static_cast<double>(PinIndex) * 34.0);
					AnchorValid[PinIndex] = 1;
				}
			};
			for (const FEdge& Edge : Edges)
			{
				if (Nodes.IsValidIndex(Edge.Source))
				{
					EnsureAnchor(Nodes[Edge.Source], false, Edge.SourcePinIndex);
				}
				if (Nodes.IsValidIndex(Edge.Target))
				{
					EnsureAnchor(Nodes[Edge.Target], true, Edge.TargetPinIndex);
				}
			}
		};
		auto RunLayoutForSelfTest = [&](TArray<FNode>& Nodes, const TArray<FEdge>& Edges, const FOptions& LocalOptions, FRunResult& Run)
		{
			EnsureSyntheticRealPinAnchorsForEdges(Nodes, Edges);
			return RunLayout(Nodes, Edges, LocalOptions, Run);
		};

								{
			TArray<FNode> Nodes;
			Nodes.SetNum(4);
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				Nodes[Index].Index = Index;
				Nodes[Index].Guid = FString::Printf(TEXT("island_internal_shared_multi_col_%d"), Index);
				Nodes[Index].Title = Nodes[Index].Guid;
				Nodes[Index].Width = 120.0;
				Nodes[Index].Height = 72.0;
				Nodes[Index].InputPinNames = { TEXT("in") };
				Nodes[Index].InputPinAnchors = { FVector2D(0.0, 36.0) };
				Nodes[Index].InputPinAnchorValid = { 1 };
				Nodes[Index].OutputPinNames = { TEXT("out") };
				Nodes[Index].OutputPinAnchors = { FVector2D(120.0, 36.0) };
				Nodes[Index].OutputPinAnchorValid = { 1 };
				Nodes[Index].bMeasuredGeometry = true;
				Nodes[Index].Rank = Index == 0 ? 0 : (Index == 1 ? 1 : 2);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 120.0;
			Nodes[1].NewX = 260.0;
			Nodes[1].NewY = 0.0;
			Nodes[2].NewX = 520.0;
			Nodes[2].NewY = 0.0;
			Nodes[3].NewX = 520.0;
			Nodes[3].NewY = 240.0;
			TArray<FEdge> Edges = {
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 3, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.Columns.SetNum(3);
			Island.Columns[0] = {0};
			Island.Columns[1] = {1};
			Island.Columns[2] = {2, 3};
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			for (const int32 NodeIndex : Island.Nodes)
			{
				IslandByNode.Add(NodeIndex, 0);
			}
			TArray<FReroutePlan> Plans;
			FRunResult Run;
			AppendIslandInternalReroutePlans(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			const bool bSharedBus =
				Plans.Num() == 2
				&& Plans[0].BusId == Plans[1].BusId
				&& Plans[0].AttachmentMergeGroup != INDEX_NONE
				&& Plans[0].AttachmentMergeGroup == Plans[1].AttachmentMergeGroup
				&& Plans[0].Points.Num() >= 1
				&& Plans[1].Points.Num() >= 1
				&& FMath::IsNearlyEqual(Plans[0].Points[0].Y, Plans[1].Points[0].Y, 0.5)
				&& Run.DesignDiagnostics.IslandInternalMultiColumnBusCount == 2;
			AddCase(
				TEXT("island_internal_same_source_multi_column_edges_share_one_trunk"),
				bSharedBus,
				FString::Printf(TEXT("plans=%d bus=%s/%s merge=%d/%d y=%.1f/%.1f multi=%d"),
					Plans.Num(),
					Plans.Num() > 0 ? *Plans[0].BusId : TEXT("none"),
					Plans.Num() > 1 ? *Plans[1].BusId : TEXT("none"),
					Plans.Num() > 0 ? Plans[0].AttachmentMergeGroup : INDEX_NONE,
					Plans.Num() > 1 ? Plans[1].AttachmentMergeGroup : INDEX_NONE,
					Plans.Num() > 0 && Plans[0].Points.Num() > 0 ? Plans[0].Points[0].Y : -1.0,
					Plans.Num() > 1 && Plans[1].Points.Num() > 0 ? Plans[1].Points[0].Y : -1.0,
					Run.DesignDiagnostics.IslandInternalMultiColumnBusCount));
		}

								{
			TArray<FNode> Nodes;
			Nodes.SetNum(4);
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				Nodes[Index].Index = Index;
				Nodes[Index].Width = 120.0;
				Nodes[Index].Height = 72.0;
			}
			Nodes[0].Guid = TEXT("comment");
			Nodes[0].Title = TEXT("Manual Comment Island");
			Nodes[0].ClassName = TEXT("EdGraphNode_Comment");
			Nodes[0].bIsCommentNode = true;
			Nodes[0].CommentDepth = 4;
			Nodes[0].OldX = 100.0;
			Nodes[0].OldY = 100.0;
			Nodes[0].Width = 520.0;
			Nodes[0].Height = 300.0;
			Nodes[1].Guid = TEXT("inside_a");
			Nodes[1].OldX = 180.0;
			Nodes[1].OldY = 180.0;
			Nodes[2].Guid = TEXT("inside_b");
			Nodes[2].OldX = 360.0;
			Nodes[2].OldY = 220.0;
			Nodes[3].Guid = TEXT("outside");
			Nodes[3].OldX = 760.0;
			Nodes[3].OldY = 190.0;
			TArray<FEdge> Edges = {
				{1, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{1, 3, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const FVector2D CommentToInsideA(Nodes[1].NewX - Nodes[0].NewX, Nodes[1].NewY - Nodes[0].NewY);
			const FVector2D CommentToInsideB(Nodes[2].NewX - Nodes[0].NewX, Nodes[2].NewY - Nodes[0].NewY);
			const bool bCommentIslandFrozen =
				Run.DesignDiagnostics.CommentIslandCount == 1
				&& Run.DesignDiagnostics.CommentIslandBoundaryEdgeCount == 1
				&& Run.IslandCount == 2
				&& Run.IslandEdgeCount == 1
				&& Run.DesignDiagnostics.IslandLocalMutationCount == 0
				&& FMath::IsNearlyEqual(CommentToInsideA.X, 80.0, 0.5)
				&& FMath::IsNearlyEqual(CommentToInsideA.Y, 80.0, 0.5)
				&& FMath::IsNearlyEqual(CommentToInsideB.X, 260.0, 0.5)
				&& FMath::IsNearlyEqual(CommentToInsideB.Y, 120.0, 0.5);
			AddCase(
				TEXT("comment_box_nodes_form_frozen_special_island"),
				bCommentIslandFrozen,
				FString::Printf(TEXT("comment_islands=%d boundary_edges=%d islands=%d island_edges=%d rel_a=(%.1f,%.1f) rel_b=(%.1f,%.1f)"),
					Run.DesignDiagnostics.CommentIslandCount,
					Run.DesignDiagnostics.CommentIslandBoundaryEdgeCount,
					Run.IslandCount,
					Run.IslandEdgeCount,
					CommentToInsideA.X,
					CommentToInsideA.Y,
					CommentToInsideB.X,
					CommentToInsideB.Y));
		};

		{
			TArray<FNode> GuardNodes;
			GuardNodes.SetNum(2);
			for (int32 Index = 0; Index < GuardNodes.Num(); ++Index)
			{
				GuardNodes[Index].Index = Index;
				GuardNodes[Index].Width = 120.0;
				GuardNodes[Index].Height = 80.0;
				GuardNodes[Index].NewX = static_cast<double>(Index) * 180.0;
				GuardNodes[Index].NewY = static_cast<double>(Index) * 40.0;
			}
			FIsland GuardIsland;
			GuardIsland.Id = 7;
			GuardIsland.Nodes = {0, 1};
			GuardIsland.NodeSet.Add(0);
			GuardIsland.NodeSet.Add(1);
			GuardIsland.LocalBounds.Left = 0.0;
			GuardIsland.LocalBounds.Top = 0.0;
			GuardIsland.LocalBounds.Right = 300.0;
			GuardIsland.LocalBounds.Bottom = 120.0;
			TArray<FIsland> GuardIslands;
			GuardIslands.Add(GuardIsland);
			FRunResult GuardRun;
			const FIslandLocalLayoutSnapshot GuardSnapshot = FreezeIslandLocalLayout(GuardNodes, GuardIslands, GuardRun);
			const bool bFrozenBeforeMutation = ValidateIslandLocalLayout(GuardNodes, GuardIslands, GuardSnapshot, false, TEXT("self_test_frozen"), GuardRun);
			GuardNodes[1].NewY += 12.0;
			const bool bDetectedMutation = !ValidateIslandLocalLayout(GuardNodes, GuardIslands, GuardSnapshot, false, TEXT("self_test_mutated"), GuardRun)
				&& GuardRun.DesignDiagnostics.IslandLocalMutationCount == 1
				&& GuardRun.DesignDiagnostics.IslandLocalMaxMutation > 11.0
				&& GuardRun.IslandLocalMutationDiagnostics.Num() == 1;
			AddCase(
				TEXT("island_local_layout_freeze_detects_late_mutation"),
				bFrozenBeforeMutation && bDetectedMutation,
				FString::Printf(TEXT("frozen=%d mutations=%d max_delta=%.1f"), GuardRun.DesignDiagnostics.IslandLocalFrozenNodeCount, GuardRun.DesignDiagnostics.IslandLocalMutationCount, GuardRun.DesignDiagnostics.IslandLocalMaxMutation));
		}

		{
			FNode Source;
			Source.Index = 0;
			Source.Width = 220.0;
			Source.Height = 90.0;
			Source.OldX = 100.0;
			Source.OldY = 50.0;
			Source.OutputPinNames = { TEXT("out") };
			Source.OutputPinAnchors = { FVector2D(178.0, 31.0) };
			Source.OutputPinAnchorValid = { 1 };
			FNode Target;
			Target.Index = 1;
			Target.Width = 180.0;
			Target.Height = 90.0;
			Target.OldX = 500.0;
			Target.OldY = 70.0;
			Target.InputPinNames = { TEXT("in") };
			Target.InputPinAnchors = { FVector2D(13.0, 47.0) };
			Target.InputPinAnchorValid = { 1 };
			FEdge Edge{0, 1, TEXT("out"), TEXT("in"), 0, 0, false};
			const FVector2D SourcePort = GetSourcePort(Source, Edge, false);
			const FVector2D TargetPort = GetTargetPort(Target, Edge, false);
			const double ShortenX = ComputeConnectionAlignmentShortenX(SourcePort, TargetPort, Options);
			const double ExpectedShortenX = (TargetPort.X - SourcePort.X) - EndpointAccessClearanceX(Options);
			const bool bUsesMeasuredXAndShorten =
				FMath::IsNearlyEqual(SourcePort.X, 278.0, 0.001)
				&& FMath::IsNearlyEqual(TargetPort.X, 513.0, 0.001)
				&& FMath::IsNearlyEqual(SourcePort.Y, 81.0, 0.001)
				&& FMath::IsNearlyEqual(TargetPort.Y, 117.0, 0.001)
				&& FMath::IsNearlyEqual(ShortenX, ExpectedShortenX, 0.001)
				&& ShortenX > 0.0;
			AddCase(TEXT("connection_alignment_shorten_uses_real_pin_x"), bUsesMeasuredXAndShorten, FString::Printf(TEXT("source=(%.1f,%.1f) target=(%.1f,%.1f) shorten=%.1f expected=%.1f"), SourcePort.X, SourcePort.Y, TargetPort.X, TargetPort.Y, ShortenX, ExpectedShortenX));
		}

		{
			FOptions AsymmetricOptions = Options;
			AsymmetricOptions.OutputAccessOffsetX = 32.0;
			AsymmetricOptions.InputAccessOffsetX = 18.0;
			FNode Source;
			Source.Index = 0;
			Source.Width = 220.0;
			Source.Height = 90.0;
			Source.OldX = 100.0;
			Source.OldY = 50.0;
			Source.OutputPinNames = { TEXT("out") };
			Source.OutputPinAnchors = { FVector2D(178.0, 31.0) };
			Source.OutputPinAnchorValid = { 1 };
			FNode Target;
			Target.Index = 1;
			Target.Width = 180.0;
			Target.Height = 90.0;
			Target.OldX = 500.0;
			Target.OldY = 70.0;
			Target.InputPinNames = { TEXT("in") };
			Target.InputPinAnchors = { FVector2D(13.0, 47.0) };
			Target.InputPinAnchorValid = { 1 };
			FEdge Edge{0, 1, TEXT("out"), TEXT("in"), 0, 0, false};
			const FVector2D SourcePort = GetSourcePort(Source, Edge, false);
			const FVector2D TargetPort = GetTargetPort(Target, Edge, false);

			FReroutePlan Plan;
			Plan.Points = { FVector2D(260.0, 80.0), FVector2D(320.0, 80.0) };
			FinalizeReroutePlanPoints(Plan, AsymmetricOptions);
			const double PositiveShorten = ComputeConnectionAlignmentShortenX(FVector2D(100.0, 0.0), FVector2D(230.0, 0.0), AsymmetricOptions);
			const double NegativeShorten = ComputeConnectionAlignmentShortenX(FVector2D(100.0, 0.0), FVector2D(140.0, 0.0), AsymmetricOptions);
			const bool bAsymmetricAccessOffsets =
				FMath::IsNearlyEqual(SourcePort.X, 278.0, 0.001)
				&& FMath::IsNearlyEqual(TargetPort.X, 513.0, 0.001)
				&& FMath::IsNearlyEqual(PositiveShorten, 80.0, 0.001)
				&& FMath::IsNearlyEqual(NegativeShorten, -10.0, 0.001)
				&& Plan.KnotPoints.Num() == 2
				&& Plan.PointRoles.Num() == 2
				&& Plan.PointRoles[0] == EReroutePointRole::Auto
				&& Plan.PointRoles[1] == EReroutePointRole::Auto
				&& FMath::IsNearlyEqual(Plan.KnotPoints[0].X, Plan.Points[0].X, 0.001)
				&& FMath::IsNearlyEqual(Plan.KnotPoints[1].X, Plan.Points[1].X, 0.001);
			AddCase(TEXT("connection_alignment_shorten_supports_asymmetric_offsets_and_visible_knot_points"),
				bAsymmetricAccessOffsets,
				FString::Printf(TEXT("source_x=%.1f target_x=%.1f positive=%.1f negative=%.1f knot_x=%.1f/%.1f roles=%s/%s"),
					SourcePort.X,
					TargetPort.X,
					PositiveShorten,
					NegativeShorten,
					Plan.KnotPoints.Num() > 0 ? Plan.KnotPoints[0].X : -1.0,
					Plan.KnotPoints.Num() > 1 ? Plan.KnotPoints[1].X : -1.0,
					Plan.PointRoles.Num() > 0 ? *ReroutePointRoleToString(Plan.PointRoles[0]) : TEXT("missing"),
					Plan.PointRoles.Num() > 1 ? *ReroutePointRoleToString(Plan.PointRoles[1]) : TEXT("missing")));
		}

		{
			FOptions DirectEndpointKnotOptions = Options;
			DirectEndpointKnotOptions.OutputAccessOffsetX = 40.0;
			DirectEndpointKnotOptions.InputAccessOffsetX = 40.0;
			const FVector2D SourcePoint(0.0, 0.0);
			const FVector2D TargetPoint(200.0, 100.0);
			const FRect BroadFalsePositiveRect{ 45.0, 80.0, 65.0, 95.0 };
			const FRect RepairableRect{ 55.0, 25.0, 65.0, 35.0 };
			TArray<FRect> RepairableRects;
			RepairableRects.Add(RepairableRect);
			FRect BroadAabb;
			const bool bBroadAabbBuilt = TryBuildEscapedEndpointAabb(SourcePoint, nullptr, TargetPoint, nullptr, DirectEndpointKnotOptions, BroadAabb);
			TArray<TArray<FVector2D>> EndpointTriangles;
			const bool bEndpointTrianglesBuilt = TryBuildEndpointOffsetCollisionTriangles(
				SourcePoint,
				nullptr,
				TargetPoint,
				nullptr,
				DirectEndpointKnotOptions,
				EndpointTriangles);
			TArray<FVector2D> ControlPoints;
			TArray<FVector2D> InteriorPoints;
			const bool bEndpointKnotsBuilt = TryBuildDirectEndpointKnotControlPoints(
				SourcePoint,
				TargetPoint,
				DirectEndpointKnotOptions,
				ControlPoints,
				&InteriorPoints);
			const bool bBroadAabbFalsePositive =
				bBroadAabbBuilt
				&& BroadAabb.Intersects(BroadFalsePositiveRect, 0.0)
				&& bEndpointTrianglesBuilt
				&& !EndpointOffsetTrianglesIntersectsRect(EndpointTriangles, BroadFalsePositiveRect);
			const bool bEndpointTrianglesHitRepairableRect =
				bEndpointTrianglesBuilt
				&& EndpointOffsetTrianglesIntersectsRect(EndpointTriangles, RepairableRect);
			const bool bEndpointKnotsClear = bEndpointKnotsBuilt && !ControlWireHitsHardOccupancy(ControlPoints, RepairableRects);
			AddCase(
				TEXT("final_direct_endpoint_knots_use_endpoint_offset_triangles"),
				bBroadAabbFalsePositive
					&& bEndpointTrianglesHitRepairableRect
					&& bEndpointKnotsClear
					&& InteriorPoints.Num() == 2
					&& FMath::IsNearlyEqual(InteriorPoints[0].X, 40.0, 0.001)
					&& FMath::IsNearlyEqual(InteriorPoints[1].X, 160.0, 0.001),
				FString::Printf(
					TEXT("broad_false_positive=%s triangle_repair_hit=%s endpoint_clear=%s interior=%d source_knot=(%.1f,%.1f) target_knot=(%.1f,%.1f)"),
					bBroadAabbFalsePositive ? TEXT("true") : TEXT("false"),
					bEndpointTrianglesHitRepairableRect ? TEXT("true") : TEXT("false"),
					bEndpointKnotsClear ? TEXT("true") : TEXT("false"),
					InteriorPoints.Num(),
					InteriorPoints.Num() > 0 ? InteriorPoints[0].X : -1.0,
					InteriorPoints.Num() > 0 ? InteriorPoints[0].Y : -1.0,
					InteriorPoints.Num() > 1 ? InteriorPoints[1].X : -1.0,
					InteriorPoints.Num() > 1 ? InteriorPoints[1].Y : -1.0));
		}

		{
			FOptions BusPathOptions = Options;
			BusPathOptions.OutputAccessOffsetX = 32.0;
			BusPathOptions.InputAccessOffsetX = 18.0;
			FDeterministicIslandConnection BusConnection;
			BusConnection.BranchSlotIndex = 2;
			const double DirectClearance = EndpointAccessClearanceX(BusPathOptions);
			const double BusPathClearance = BusPathAccessClearanceX(BusPathOptions);
			const double RequiredSpan = RequiredBusEntrySourceSpanX(
				BusConnection,
				TArray<FDeterministicInterIslandBus>(),
				TArray<FIsland>(),
				BusPathOptions);
			const double ExpectedRequiredSpan = BusPathClearance + 2.0 * GetCrossBusBranchLaneSpacing(BusPathOptions);
			const bool bBusPathUsesTwoEndpointPairs =
				FMath::IsNearlyEqual(DirectClearance, 50.0, 0.001)
				&& FMath::IsNearlyEqual(BusPathClearance, 100.0, 0.001)
				&& FMath::IsNearlyEqual(RequiredSpan, ExpectedRequiredSpan, 0.001);
			AddCase(
				TEXT("inter_island_bus_path_uses_two_endpoint_access_clearances"),
				bBusPathUsesTwoEndpointPairs,
				FString::Printf(TEXT("direct=%.1f bus_path=%.1f required=%.1f expected=%.1f"), DirectClearance, BusPathClearance, RequiredSpan, ExpectedRequiredSpan));
		}

		{
			FOptions EntryOptions = Options;
			EntryOptions.OutputAccessOffsetX = 40.0;
			EntryOptions.InputAccessOffsetX = 40.0;
			FIslandPortCandidate Candidate;
			Candidate.Side = TEXT("left");
			Candidate.SourceLocal = FVector2D::ZeroVector;
			Candidate.TargetLocal = FVector2D(-57.2, 120.0);
			Candidate.ActualTargetLocal = FVector2D(286.0, 0.0);
			Candidate.AllowedMinX = -57.2;
			Candidate.AllowedMaxX = 206.0;
			FIsland SourceIsland;
			SourceIsland.X = -300.0;
			FIsland TargetIsland;
			TargetIsland.X = 0.0;
			const FVector2D DirectEntry = ResolveEntryLocalAfterPlacement(
				Candidate,
				SourceIsland,
				TargetIsland,
				EntryOptions,
				0.0);
			const FVector2D BusEntry = ResolveEntryLocalAfterPlacement(
				Candidate,
				SourceIsland,
				TargetIsland,
				EntryOptions,
				BusPathAccessClearanceX(EntryOptions));
			const bool bBusEntryUsesRightmostFeasibleX =
				FMath::IsNearlyEqual(DirectEntry.X, Candidate.TargetLocal.X, 0.001)
				&& FMath::IsNearlyEqual(BusEntry.X, Candidate.AllowedMaxX, 0.001);
			AddCase(
				TEXT("bus_entry_resolution_uses_rightmost_feasible_candidate"),
				bBusEntryUsesRightmostFeasibleX,
				FString::Printf(TEXT("direct_entry=%.1f bus_entry=%.1f allowed_max=%.1f"), DirectEntry.X, BusEntry.X, Candidate.AllowedMaxX));
		}

		{
			FOptions RouteOptions = Options;
			RouteOptions.OutputAccessOffsetX = 40.0;
			RouteOptions.InputAccessOffsetX = 40.0;
			const FVector2D Start(100.0, 100.0);
			const FVector2D End(360.0, 260.0);
			const TArray<FVector2D> Points = BuildIslandInternalSharedBusRoute(Start, End, 180.0, RouteOptions);
			double SourceShorten = -9999.0;
			double TargetShorten = -9999.0;
			const double MinSegmentShorten = MinConnectionAlignmentShortenX(Points, RouteOptions);
			if (Points.Num() >= 3)
			{
				SourceShorten = ComputeConnectionAlignmentShortenX(Start, Points[1], RouteOptions);
				TargetShorten = ComputeConnectionAlignmentShortenX(Points[Points.Num() - 2], End, RouteOptions);
			}
			const bool bVerticalEndpointAccessUsesShorten =
				Points.Num() >= 3
				&& SourceShorten >= -0.5
				&& TargetShorten >= -0.5
				&& MinSegmentShorten >= -0.5
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(Points, RouteOptions)
				&& Points[1].X >= ComputeInputXAlignedToOutput(Start.X, RouteOptions) - 0.5
				&& Points[Points.Num() - 2].X <= ComputeOutputXAlignedToInput(End.X, RouteOptions) + 0.5;
			AddCase(TEXT("vertical_bus_endpoint_access_requires_non_negative_connection_shorten"),
				bVerticalEndpointAccessUsesShorten,
				FString::Printf(TEXT("points=%d source_shorten=%.1f target_shorten=%.1f min_segment_shorten=%.1f first_x=%.1f last_x=%.1f"),
					Points.Num(),
					SourceShorten,
					TargetShorten,
					MinSegmentShorten,
					Points.Num() > 1 ? Points[1].X : -1.0,
					Points.Num() > 1 ? Points[Points.Num() - 2].X : -1.0));
		}

		{
			TArray<FXStripCollisionBox> Boxes;
			AddXStripCollisionBox(
				Boxes,
				EXStripCollisionOwnerKind::InterIslandBusBundle,
				2,
				INDEX_NONE,
				{ 0.0, 100.0, 120.0, 140.0 },
				118.0);
			AddXStripCollisionBox(
				Boxes,
				EXStripCollisionOwnerKind::Island,
				7,
				INDEX_NONE,
				{ 80.0, 130.0, 200.0, 190.0 });
			AddXStripCollisionBox(
				Boxes,
				EXStripCollisionOwnerKind::InputChain,
				3,
				11,
				{ 240.0, 100.0, 280.0, 130.0 });
			double LeftmostHitX = TNumericLimits<double>::Max();
			const bool bFoundLeftmost = FindLeftmostIntersectingXStripLeft(
				{ 90.0, 120.0, 250.0, 150.0 },
				Boxes,
				LeftmostHitX);
			const bool bXStripCollision =
				Boxes.Num() == 3
				&& Boxes[0].OwnerKind == EXStripCollisionOwnerKind::InterIslandBusBundle
				&& Boxes[1].OwnerKind == EXStripCollisionOwnerKind::Island
				&& Boxes[2].SecondaryOwnerId == 11
				&& FMath::IsNearlyEqual(Boxes[0].CenterY(), 118.0, 0.5)
				&& FMath::IsNearlyEqual(Boxes[1].CenterY(), 160.0, 0.5)
				&& XStripIntersects(Boxes[0], Boxes[1], 0.0)
				&& !XStripIntersects(Boxes[0], Boxes[2], 0.0)
				&& XStripOverlapsXRange(Boxes[2], 250.0, 260.0)
				&& bFoundLeftmost
				&& FMath::IsNearlyEqual(LeftmostHitX, 0.0, 0.5);
			AddCase(TEXT("x_strip_collision_box_unifies_bundle_island_input_chain_segments"),
				bXStripCollision,
				FString::Printf(TEXT("boxes=%d leftmost=%.1f kinds=%d/%d/%d"),
					Boxes.Num(),
					LeftmostHitX,
					Boxes.Num() > 0 ? static_cast<int32>(Boxes[0].OwnerKind) : -1,
					Boxes.Num() > 1 ? static_cast<int32>(Boxes[1].OwnerKind) : -1,
					Boxes.Num() > 2 ? static_cast<int32>(Boxes[2].OwnerKind) : -1));
		}

		{
			TArray<FColumnItem> Items;
			FColumnItem UpperItem;
			UpperItem.NodeIndex = 0;
			UpperItem.Height = 176.0;
			UpperItem.PreferredTop = 101.0;
			UpperItem.bHasPreferred = true;
			Items.Add(UpperItem);
			FColumnItem LowerItem;
			LowerItem.NodeIndex = 1;
			LowerItem.Height = 94.0;
			LowerItem.PreferredTop = 141.5;
			LowerItem.bHasPreferred = true;
			Items.Add(LowerItem);
			PlaceAnchoredIntervals(Items, 24.0);
			const double UpperResidual = FMath::Abs(Items[0].FinalTop - Items[0].PreferredTop);
			const double LowerResidual = FMath::Abs(Items[1].FinalTop - Items[1].PreferredTop);
			const bool bBaseAnchoredAndLowerAvoidedDown =
				FMath::Abs((Items[1].FinalTop - Items[0].FinalTop) - (Items[0].Height + 24.0)) <= 0.5
				&& UpperResidual <= 0.5
				&& FMath::IsNearlyEqual(Items[0].FinalTop, 101.0, 0.5)
				&& FMath::IsNearlyEqual(Items[1].FinalTop, 301.0, 0.5)
				&& LowerResidual > 120.0;
			AddCase(TEXT("two_column_anchor_places_base_then_directional_collision_avoidance"), bBaseAnchoredAndLowerAvoidedDown, FString::Printf(TEXT("upper=%.1f pref=%.1f lower=%.1f pref=%.1f residual=%.1f/%.1f"), Items[0].FinalTop, Items[0].PreferredTop, Items[1].FinalTop, Items[1].PreferredTop, UpperResidual, LowerResidual));
		}

		{
			TArray<FColumnItem> Items;
			const double Heights[] = {112.0, 107.0, 107.0};
			const double PreferredTops[] = {52.0, 237.0, 269.0};
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FColumnItem Item;
				Item.NodeIndex = Index;
				Item.Height = Heights[Index];
				Item.PreferredTop = PreferredTops[Index];
				Item.bHasPreferred = true;
				Items.Add(Item);
			}
			PlaceAnchoredIntervals(Items, 24.0);
			const bool bNonBaseKeepsPreferredUntilCollision =
				FMath::IsNearlyEqual(Items[1].FinalTop, 237.0, 0.5)
				&& FMath::IsNearlyEqual(Items[0].FinalTop, 52.0, 0.5)
				&& FMath::IsNearlyEqual(Items[2].FinalTop, 368.0, 0.5);
			AddCase(TEXT("three_item_two_column_anchor_keeps_nonbase_preferred_until_collision"), bNonBaseKeepsPreferredUntilCollision, FString::Printf(TEXT("tops=%.1f/%.1f/%.1f pref=%.1f/%.1f/%.1f"), Items[0].FinalTop, Items[1].FinalTop, Items[2].FinalTop, PreferredTops[0], PreferredTops[1], PreferredTops[2]));
		}

		{
			FInterfaceBundleAnchorAccumulator Anchor;
			Anchor.Add(100.0, 10.0);
			Anchor.Add(140.0, 30.0);
			Anchor.Add(300.0, 70.0);
			double PreferredOriginY = 0.0;
			const bool bResolved = Anchor.ResolvePreferredOriginY(PreferredOriginY);
			const double ExpectedMidpointOriginY = ((100.0 + 300.0) * 0.5) - ((10.0 + 70.0) * 0.5);
			const double OldAverageOriginY = ((100.0 - 10.0) + (140.0 - 30.0) + (300.0 - 70.0)) / 3.0;
			const bool bUsesMidpointNotAverage = bResolved
				&& FMath::Abs(PreferredOriginY - ExpectedMidpointOriginY) <= 0.5
				&& FMath::Abs(PreferredOriginY - OldAverageOriginY) > 0.5;
			AddCase(TEXT("two_column_anchor_uses_interface_bundle_midpoint_not_average"), bUsesMidpointNotAverage, FString::Printf(TEXT("origin=%.1f midpoint=%.1f old_average=%.1f"), PreferredOriginY, ExpectedMidpointOriginY, OldAverageOriginY));
		}

		{
			TArray<FColumnItem> Items;
			const double Heights[] = {40.0, 40.0, 40.0};
			const double PreferredTops[] = {0.0, 50.0, 100.0};
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FColumnItem Item;
				Item.NodeIndex = Index;
				Item.Height = Heights[Index];
				Item.PreferredTop = PreferredTops[Index];
				Item.bHasPreferred = true;
				Items.Add(Item);
			}
			TArray<FInputChainForbiddenZone> Zones;
			FInputChainForbiddenZone Zone;
			Zone.Top = 45.0;
			Zone.Bottom = 95.0;
			Zones.Add(Zone);
			int32 AvoidanceCount = 0;
			PlaceAnchoredIntervalsAvoidingForbiddenZones(Items, 10.0, Zones, &AvoidanceCount);
			const bool bInputChainBaseAvoidsNearestZoneAndPropagates =
				FMath::IsNearlyEqual(Items[1].FinalTop, -5.0, 0.5)
				&& FMath::IsNearlyEqual(Items[0].FinalTop, -55.0, 0.5)
				&& FMath::IsNearlyEqual(Items[2].FinalTop, 105.0, 0.5)
				&& AvoidanceCount >= 2;
			AddCase(TEXT("input_chain_two_column_anchor_moves_base_out_of_forbidden_zone"), bInputChainBaseAvoidsNearestZoneAndPropagates, FString::Printf(TEXT("tops=%.1f/%.1f/%.1f avoid=%d zone=%.1f..%.1f"), Items[0].FinalTop, Items[1].FinalTop, Items[2].FinalTop, AvoidanceCount, Zone.Top, Zone.Bottom));
		}

		{
			TArray<FPortOrderDemand> WeightedCandidates;
			FPortOrderDemand NearDemand;
			NearDemand.bValid = true;
			NearDemand.GroupNode = 42;
			NearDemand.Value = 0.0;
			NearDemand.Distance = 1;
			NearDemand.Weight = ColumnDistanceInfluenceWeight(NearDemand.Distance);
			WeightedCandidates.Add(NearDemand);
			FPortOrderDemand FarDemand;
			FarDemand.bValid = true;
			FarDemand.GroupNode = 42;
			FarDemand.Value = 1.0;
			FarDemand.Distance = 3;
			FarDemand.Weight = ColumnDistanceInfluenceWeight(FarDemand.Distance);
			WeightedCandidates.Add(FarDemand);
			const FPortOrderDemandSet WeightedDemandSet = MergePortOrderDemandCandidates(WeightedCandidates);
			const FPortOrderDemand* MergedDemand = FindPortOrderDemandByGroup(WeightedDemandSet, 42);
			const bool bDemandMergeUsesDistanceWeight =
				MergedDemand
				&& FMath::IsNearlyEqual(MergedDemand->Value, 0.2, 0.001)
				&& FMath::IsNearlyEqual(MergedDemand->Weight, 1.25, 0.001);
			AddCase(TEXT("column_port_order_demand_uses_distance_decay_weight"),
				bDemandMergeUsesDistanceWeight,
				FString::Printf(TEXT("value=%.3f weight=%.3f"),
					MergedDemand ? MergedDemand->Value : -1.0,
					MergedDemand ? MergedDemand->Weight : -1.0));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("external_source_tie%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 150.0;
				Node.Height = 96.0;
				Node.Rank = Index == 0 ? 0 : (Index == 3 ? 1 : 0);
				Node.InputPinNames = { TEXT("in") };
				Node.InputPinAnchors = { FVector2D(0.0, 48.0) };
				Node.InputPinAnchorValid = { 1 };
				Node.OutputPinNames = { TEXT("out") };
				Node.OutputPinAnchors = { FVector2D(150.0, 48.0) };
				Node.OutputPinAnchorValid = { 1 };
				if (Index == 0)
				{
					Node.OutputPinNames = { TEXT("upper"), TEXT("lower") };
					Node.OutputPinAnchors = { FVector2D(150.0, 28.0), FVector2D(150.0, 68.0) };
					Node.OutputPinAnchorValid = { 1, 1 };
				}
				Nodes.Add(Node);
			}
			const TArray<FEdge> AllEdges = {
				{0, 1, TEXT("lower"), TEXT("in"), 1, 0, true},
				{0, 2, TEXT("upper"), TEXT("in"), 0, 0, true},
				{1, 3, TEXT("out"), TEXT("in"), 0, 0, true},
				{2, 3, TEXT("out"), TEXT("in"), 0, 0, true}
			};
			const TArray<FEdge> ExplicitEdges = {
				{1, 3, TEXT("out"), TEXT("in"), 0, 0, true},
				{2, 3, TEXT("out"), TEXT("in"), 0, 0, true}
			};
			const TArray<int32> IslandNodes = {1, 2, 3};
			const TArray<FPortOrderDemandSet> SourceDemands = ComputeSourcePortOrderDemandSets(Nodes, ExplicitEdges, IslandNodes);
			const TArray<FPortOrderDemandSet> TargetDemands = ComputeDownstreamPortOrderDemandSets(Nodes, ExplicitEdges, IslandNodes);
			const TArray<FPortOrderDemandSet> ExternalSourceDemands = ComputeExternalSourcePortOrderDemandSets(Nodes, AllEdges, ExplicitEdges, IslandNodes);
			TArray<TArray<int32>> Columns;
			Columns.SetNum(2);
			Columns[0] = {1, 2};
			Columns[1] = {3};
			MinimizeColumnCrossings(Nodes, ExplicitEdges, SourceDemands, TargetDemands, &ExternalSourceDemands, Columns);
			const FPortOrderDemand* Demand1 = ExternalSourceDemands.IsValidIndex(1) ? FindPortOrderDemandByGroup(ExternalSourceDemands[1], 0) : nullptr;
			const FPortOrderDemand* Demand2 = ExternalSourceDemands.IsValidIndex(2) ? FindPortOrderDemandByGroup(ExternalSourceDemands[2], 0) : nullptr;
			const bool bExternalSourceTieBreak =
				Demand1 && Demand2
				&& Demand2->Value < Demand1->Value
				&& Columns[0].Num() == 2
				&& Columns[0][0] == 2
				&& Columns[0][1] == 1;
			AddCase(TEXT("external_source_order_tiebreaks_same_right_target_input"),
				bExternalSourceTieBreak,
				FString::Printf(TEXT("demand=%.3f/%.3f col0=%d,%d"),
					Demand1 ? Demand1->Value : -1.0,
					Demand2 ? Demand2->Value : -1.0,
					Columns[0].Num() > 0 ? Columns[0][0] : -1,
					Columns[0].Num() > 1 ? Columns[0][1] : -1));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("sameislandfanout%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 160.0;
				Node.Height = 80.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(160.0, 40.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 40.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{1, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{1, 3, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const FVector2D SourcePoint = GetSourcePort(Nodes[1], Edges[2], true);
			const FVector2D TargetPoint = GetTargetPort(Nodes[2], Edges[2], true);
			const bool bSameIslandFanoutForward =
				Run.DesignDiagnostics.FanoutImplicitEdgeCount >= 2
				&& Run.MetricsAfter.BackwardEdgeCount == 0
				&& HasNonNegativeConnectionAlignmentShorten(SourcePoint, TargetPoint, Options);
			AddCase(TEXT("rank_ignored_fanout_edges_do_not_create_backward_layout"),
				bSameIslandFanoutForward,
				FString::Printf(TEXT("fanout=%d inside=%d backward=%d source_x=%.1f target_x=%.1f shorten=%.1f"),
					Run.DesignDiagnostics.FanoutImplicitEdgeCount,
					Run.DesignDiagnostics.ImplicitEdgeInsideIslandCount,
					Run.MetricsAfter.BackwardEdgeCount,
					SourcePoint.X,
					TargetPoint.X,
					ComputeConnectionAlignmentShortenX(SourcePoint, TargetPoint, Options)));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("endpoint_%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 0 ? 120.0 : 180.0;
				Node.Height = 90.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 40.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 72.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			bool bRouteEndpointUsesRealTarget = false;
			for (const TSharedPtr<FJsonValue>& RouteValue : Run.RouteDiagnostics)
			{
				const TSharedPtr<FJsonObject> RouteObj = RouteValue.IsValid() ? RouteValue->AsObject() : nullptr;
				double EdgeIndex = -1.0;
				if (!RouteObj.IsValid() || !RouteObj->TryGetNumberField(TEXT("edge_index"), EdgeIndex) || static_cast<int32>(EdgeIndex) != 0)
				{
					continue;
				}
				const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
				if (RouteObj.IsValid() && RouteObj->TryGetArrayField(TEXT("points"), Points) && Points && Points->Num() > 0)
				{
					const TSharedPtr<FJsonObject> LastPoint = (*Points)[Points->Num() - 1]->AsObject();
					double LastX = 0.0;
					double LastY = 0.0;
					if (LastPoint.IsValid() && LastPoint->TryGetNumberField(TEXT("x"), LastX) && LastPoint->TryGetNumberField(TEXT("y"), LastY))
					{
						const FVector2D Target = GetTargetPort(Nodes[1], Edges[0], true);
						bRouteEndpointUsesRealTarget = FMath::IsNearlyEqual(LastX, Target.X, 0.001)
							&& FMath::IsNearlyEqual(LastY, Target.Y, 0.001);
					}
				}
			}
			AddCase(TEXT("route_diagnostic_endpoint_uses_real_target_pin"), bRouteEndpointUsesRealTarget, FString::Printf(TEXT("routes=%d selected=%d"), Run.RouteDiagnostics.Num(), Run.DesignDiagnostics.SelectedEndpointCandidateCount));
		}

		{
			FOptions RouteOptions = Options;
			RouteOptions.OutputAccessOffsetX = 40.0;
			RouteOptions.InputAccessOffsetX = 40.0;
			RouteOptions.NodePadding = 32.0;
			const FVector2D Start(1404.0, 4557.0);
			const FVector2D End(3516.0, 4633.0);
			const double TrunkY = 4660.0;
			const double BranchX = 1484.0;
			const TArray<FVector2D> Route = BuildSharedSourceBusRoutePolyline(
				Start,
				End,
				TrunkY,
				BranchX,
				RouteOptions);
			double MaxInteriorX = -TNumericLimits<double>::Max();
			for (int32 PointIndex = 1; PointIndex + 1 < Route.Num(); ++PointIndex)
			{
				MaxInteriorX = FMath::Max(MaxInteriorX, Route[PointIndex].X);
			}
			AddCase(
				TEXT("shared_source_bus_near_target_y_skips_remote_target_dogleg"),
				Route.Num() == 3
					&& FMath::Abs(TrunkY - End.Y) <= GetFallbackInterfaceRowSpacing(RouteOptions)
					&& MaxInteriorX <= BranchX + 0.5
					&& HasNonNegativeConnectionAlignmentShortenForEverySegment(Route, RouteOptions),
				FString::Printf(TEXT("points=%d max_inner_x=%.1f branch_x=%.1f dy=%.1f"),
					Route.Num(),
					MaxInteriorX,
					BranchX,
					FMath::Abs(TrunkY - End.Y)));
		}

				{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("top_fanout_%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 0 ? 140.0 : 220.0;
				Node.Height = 100.0;
				Node.OutputPinNames = { TEXT("ReturnValue") };
				Node.InputPinNames = { TEXT("A") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 45.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 55.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("ReturnValue"), TEXT("A"), 0, 0, false},
				{0, 2, TEXT("ReturnValue"), TEXT("A"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			int32 LocalBusRouteCount = 0;
			for (const TSharedPtr<FJsonValue>& RouteValue : Run.RouteDiagnostics)
			{
				const TSharedPtr<FJsonObject> RouteObj = RouteValue.IsValid() ? RouteValue->AsObject() : nullptr;
				FString RouteKind;
				if (RouteObj.IsValid() && RouteObj->TryGetStringField(TEXT("route_kind"), RouteKind) && RouteKind == TEXT("local_bus"))
				{
					++LocalBusRouteCount;
				}
			}
			AddCase(TEXT("top_bottom_short_fanout_stays_direct_without_obstacle"),
				LocalBusRouteCount == 0,
				FString::Printf(TEXT("plans=%d local_bus_routes=%d diagnostics=%d"), Run.ReroutePlanCount, LocalBusRouteCount, Run.RouteDiagnostics.Num()));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("local_bus_align_%d"), Index);
				Node.Title = Node.Guid;
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("value"), TEXT("x"), 0, 0, false},
				{0, 2, TEXT("value"), TEXT("x"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const FReroutePlan* FirstPlan = nullptr;
			for (const FReroutePlan& Plan : Run.ReroutePlans)
			{
				if (Plan.EdgeIndex == 0)
				{
					FirstPlan = &Plan;
					break;
				}
			}
			bool bNoOldTargetSideSBendPoint = true;
			bool bHasOldTargetSideSBendPoint = false;
			if (FirstPlan && Edges.IsValidIndex(FirstPlan->EdgeIndex))
			{
				const FVector2D TargetInput = GetTargetPort(Nodes[Edges[FirstPlan->EdgeIndex].Target], Edges[FirstPlan->EdgeIndex], true);
				const double AlignedOutputX = ComputeOutputXAlignedToInput(TargetInput.X, Options);
				for (const FVector2D& Point : FirstPlan->Points)
				{
					if (FMath::IsNearlyEqual(Point.Y, TargetInput.Y, 0.001) && !FMath::IsNearlyEqual(Point.X, AlignedOutputX, 1.0))
					{
						bHasOldTargetSideSBendPoint = true;
						bNoOldTargetSideSBendPoint = false;
					}
				}
			}
			AddCase(TEXT("bus_target_branch_uses_connection_alignment_without_back_s_bend"),
				Run.ReroutePlanCount == 0 || bNoOldTargetSideSBendPoint,
				FString::Printf(TEXT("plans=%d has_target_y_bend=%s points=%d"), Run.ReroutePlanCount, bHasOldTargetSideSBendPoint ? TEXT("true") : TEXT("false"), FirstPlan ? FirstPlan->Points.Num() : 0));
		}

										{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("fanoutsplit%d"), Index);
				Node.Title = Node.Guid;
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("ReturnValue"), TEXT("A"), 0, 0, false},
				{0, 2, TEXT("ReturnValue"), TEXT("A"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const FReroutePlan* NearPlan = nullptr;
			const FReroutePlan* FarPlan = nullptr;
			for (const FReroutePlan& Plan : Run.ReroutePlans)
			{
				if (Plan.EdgeIndex == 0)
				{
					NearPlan = &Plan;
				}
				else if (Plan.EdgeIndex == 1)
				{
					FarPlan = &Plan;
				}
			}
			const bool bFanoutHasRouteGroup = Run.DesignDiagnostics.ConnectionGroupCount >= 1
				&& Run.DesignDiagnostics.SelectedEndpointCandidateCount == 2;
			AddCase(TEXT("fanout_route_groups_siblings"),
				bFanoutHasRouteGroup,
				FString::Printf(TEXT("plans=%d selected=%d groups=%d near_kind=%s near_points=%d far_kind=%s far_points=%d route_through=%d"),
					Run.ReroutePlanCount,
					Run.DesignDiagnostics.SelectedEndpointCandidateCount,
					Run.DesignDiagnostics.ConnectionGroupCount,
					NearPlan ? *RouteKindToString(NearPlan->RouteKind) : TEXT("none"),
					NearPlan ? NearPlan->Points.Num() : 0,
					FarPlan ? *RouteKindToString(FarPlan->RouteKind) : TEXT("none"),
					FarPlan ? FarPlan->Points.Num() : 0,
					Run.DesignDiagnostics.RouteThroughIslandCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("feedbackkeep%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 160.0;
				Node.Height = 80.0;
				Node.OldX = Index * 300.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(160.0, 40.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 40.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{1, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{2, 0, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const bool bFeedbackBecameRouteOnlyIslandEdge =
				Run.DesignDiagnostics.SccFeedbackEdgeCount >= 1
				&& Run.DesignDiagnostics.RankIgnoredEdgeCount >= Run.DesignDiagnostics.SccFeedbackEdgeCount
				&& Run.DesignDiagnostics.FanoutImplicitEdgeCount == 0
				&& Run.DesignDiagnostics.IslandCutEdgeCount >= Run.DesignDiagnostics.SccFeedbackEdgeCount
				&& Run.IslandCount == 1
				&& Run.IslandEdgeCount >= Run.DesignDiagnostics.SccFeedbackEdgeCount
				&& Run.DesignDiagnostics.RouteOnlyEdgeCount >= Run.DesignDiagnostics.SccFeedbackEdgeCount;
			AddCase(TEXT("scc_feedback_becomes_route_only_island_edge"), bFeedbackBecameRouteOnlyIslandEdge, FString::Printf(TEXT("scc_feedback=%d rank_ignored=%d fanout_edges=%d island_cut=%d islands=%d island_edges=%d route_only=%d"), Run.DesignDiagnostics.SccFeedbackEdgeCount, Run.DesignDiagnostics.RankIgnoredEdgeCount, Run.DesignDiagnostics.FanoutImplicitEdgeCount, Run.DesignDiagnostics.IslandCutEdgeCount, Run.IslandCount, Run.IslandEdgeCount, Run.DesignDiagnostics.RouteOnlyEdgeCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("feedbackfanout%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 160.0;
				Node.Height = 80.0;
				Node.OldX = Index * 300.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(160.0, 40.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 40.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{1, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{2, 0, TEXT("out"), TEXT("in"), 0, 0, false},
				{2, 3, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const bool bFeedbackNotCountedAsFanout =
				Run.DesignDiagnostics.SccFeedbackEdgeCount >= 1
				&& Run.DesignDiagnostics.FanoutImplicitEdgeCount == 0
				&& Run.DesignDiagnostics.IslandCutEdgeCount >= Run.DesignDiagnostics.SccFeedbackEdgeCount
				&& Run.IslandCount == 1
				&& Run.IslandEdgeCount >= Run.DesignDiagnostics.SccFeedbackEdgeCount
				&& Run.DesignDiagnostics.RouteOnlyEdgeCount >= Run.DesignDiagnostics.SccFeedbackEdgeCount;
			AddCase(TEXT("feedback_edge_not_counted_as_true_fanout"), bFeedbackNotCountedAsFanout, FString::Printf(TEXT("scc_feedback=%d fanout_edges=%d island_cut=%d islands=%d island_edges=%d route_only=%d backward=%d"), Run.DesignDiagnostics.SccFeedbackEdgeCount, Run.DesignDiagnostics.FanoutImplicitEdgeCount, Run.DesignDiagnostics.IslandCutEdgeCount, Run.IslandCount, Run.IslandEdgeCount, Run.DesignDiagnostics.RouteOnlyEdgeCount, Run.MetricsAfter.BackwardEdgeCount));
		}

		{
			TArray<FIsland> TestIslands;
			TestIslands.SetNum(2);
			for (int32 Index = 0; Index < TestIslands.Num(); ++Index)
			{
				TestIslands[Index].Id = Index;
				TestIslands[Index].Rank = Index;
			}
			TArray<FIslandEdge> TestIslandEdges;
			FIslandEdge Edge01;
			Edge01.EdgeIndex = 0;
			Edge01.SourceIsland = 0;
			Edge01.TargetIsland = 1;
			TestIslandEdges.Add(Edge01);
			FIslandEdge Edge10;
			Edge10.EdgeIndex = 1;
			Edge10.SourceIsland = 1;
			Edge10.TargetIsland = 0;
			TestIslandEdges.Add(Edge10);
			TSet<int32> EmptyRouteOnly;
			FRunResult CycleRun;
			const TArray<int32> Order = BuildIslandTopoOrder(TestIslands, TestIslandEdges, EmptyRouteOnly, CycleRun);
			const bool bResidualCycleIsDiagnosticOnly =
				Order.Num() == TestIslands.Num()
				&& CycleRun.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount > 0;
			AddCase(TEXT("residual_island_cycle_is_diagnostic_not_route_only"), bResidualCycleIsDiagnosticOnly, FString::Printf(TEXT("order=%d residual=%d"), Order.Num(), CycleRun.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount));
		}

		{
			TArray<FIsland> TestIslands;
			TestIslands.SetNum(5);
			for (int32 Index = 0; Index < TestIslands.Num(); ++Index)
			{
				TestIslands[Index].Id = Index;
				TestIslands[Index].LocalBounds = {0.0, 0.0, 120.0, 80.0};
				TestIslands[Index].Y = static_cast<double>(Index) * 100.0;
			}
			TestIslands[0].Y = 200.0;
			TestIslands[1].Y = 0.0;

			TArray<FIslandEdge> TestIslandEdges;
			auto AddIslandEdge = [&TestIslandEdges](const int32 EdgeIndex, const int32 SourceIsland, const int32 TargetIsland, const double TargetY)
			{
				FIslandEdge Edge;
				Edge.EdgeIndex = EdgeIndex;
				Edge.SourceIsland = SourceIsland;
				Edge.TargetIsland = TargetIsland;
				Edge.SourceLocal = FVector2D(120.0, 40.0);
				Edge.TargetLocal = FVector2D(0.0, TargetY);
				TestIslandEdges.Add(Edge);
			};
			AddIslandEdge(0, 0, 2, 40.0);
			AddIslandEdge(1, 1, 3, 40.0);
			AddIslandEdge(2, 2, 4, 20.0);
			AddIslandEdge(3, 3, 4, 80.0);

			TSet<int32> EmptyRouteOnly;
			TArray<FDeterministicIslandConnection> EmptyConnections;
			const TArray<int32> TopoOrder = {0, 1, 2, 3, 4};
			FRunResult RootRun;
			const TArray<int32> Order = ComputeRootIslandContinuousOrder(
				TestIslands,
				TestIslandEdges,
				EmptyConnections,
				EmptyRouteOnly,
				TopoOrder,
				RootRun);
			const bool bMultiLayerConvergenceOrdersByDownstreamInterface =
				Order.Num() >= 2
				&& Order[0] == 0
				&& Order[1] == 1
				&& RootRun.DesignDiagnostics.RootScoringTargetCount == 1
				&& RootRun.DesignDiagnostics.RootShareComponentCount == 1
				&& RootRun.DesignDiagnostics.RootContinuousOrderInversionScore == 0;
			FString OrderText;
			for (const int32 IslandId : Order)
			{
				if (!OrderText.IsEmpty())
				{
					OrderText += TEXT(",");
				}
				OrderText += FString::FromInt(IslandId);
			}
			AddCase(
				TEXT("root_continuous_order_uses_multilayer_convergence_target"),
				bMultiLayerConvergenceOrdersByDownstreamInterface,
				FString::Printf(TEXT("order=%s scoring_targets=%d components=%d score=%.1f inversion=%.1f"),
					*OrderText,
					RootRun.DesignDiagnostics.RootScoringTargetCount,
					RootRun.DesignDiagnostics.RootShareComponentCount,
					RootRun.DesignDiagnostics.RootContinuousOrderScore,
					RootRun.DesignDiagnostics.RootContinuousOrderInversionScore));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("f%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 120.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(120.0, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const bool bHasBusModel = Run.DesignDiagnostics.InternalBusGroupCount >= 2
				&& Run.DesignDiagnostics.BoundaryEntryCount >= 2
				&& Run.DesignDiagnostics.EndpointCandidateCount >= 2
				&& Run.DesignDiagnostics.SelectedEndpointCandidateCount == Run.IslandEdgeCount;
			const bool bSameOutputFanoutStayedBus =
				Run.DesignDiagnostics.DirectIslandConnectionCount == 0
				&& Run.DesignDiagnostics.BusIslandConnectionCount == 2
				&& Run.ReroutePlanCount >= 2
				&& Run.DesignDiagnostics.FanoutIntentGroupCount >= 1;
			AddCase(TEXT("same_output_fanout_stays_bus_not_direct"), Run.IslandCount == 3 && Run.IslandEdgeCount == 2 && bHasBusModel && bSameOutputFanoutStayedBus, FString::Printf(TEXT("islands=%d island_edges=%d internal_bus=%d boundary_entry=%d candidates=%d selected=%d direct=%d bus=%d reroutes=%d fanout_groups=%d"), Run.IslandCount, Run.IslandEdgeCount, Run.DesignDiagnostics.InternalBusGroupCount, Run.DesignDiagnostics.BoundaryEntryCount, Run.DesignDiagnostics.EndpointCandidateCount, Run.DesignDiagnostics.SelectedEndpointCandidateCount, Run.DesignDiagnostics.DirectIslandConnectionCount, Run.DesignDiagnostics.BusIslandConnectionCount, Run.ReroutePlanCount, Run.DesignDiagnostics.FanoutIntentGroupCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("leftcompact%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 0 ? 620.0 : 120.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const double TargetLeft = FMath::Min(Nodes[1].NewX, Nodes[2].NewX);
			const double TargetDx = TargetLeft - Nodes[0].NewX;
			const bool bLeftEntryUsesPortDelta = TargetDx > Nodes[0].Width
				&& TargetDx < Nodes[0].Width + Options.HorizontalSpacing * 0.55
				&& Run.MetricsAfter.OverlapCount == 0;
			AddCase(TEXT("left_boundary_island_x_uses_port_delta_not_whole_column_gap"), bLeftEntryUsesPortDelta, FString::Printf(TEXT("target_dx=%.1f source_width=%.1f spacing=%.1f overlap=%d"), TargetDx, Nodes[0].Width, Options.HorizontalSpacing, Run.MetricsAfter.OverlapCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("farfanout%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 120.0;
				Node.Height = Index == 0 ? 70.0 : 820.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(120.0, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, Node.Height * 0.5) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const bool bDistantSameOutputFanoutStayedBus =
				Run.DesignDiagnostics.DirectIslandConnectionCount == 0
				&& Run.DesignDiagnostics.BusIslandConnectionCount == 2
				&& Run.ReroutePlanCount >= 2
				&& Run.DesignDiagnostics.FanoutIntentGroupCount >= 1;
			AddCase(TEXT("distant_same_output_fanout_stays_bus_after_consumer_compaction"), bDistantSameOutputFanoutStayedBus, FString::Printf(TEXT("direct=%d bus=%d reroutes=%d lanes=%d edge_crossing=%d fanout_groups=%d"), Run.DesignDiagnostics.DirectIslandConnectionCount, Run.DesignDiagnostics.BusIslandConnectionCount, Run.ReroutePlanCount, Run.DesignDiagnostics.LaneColoringLaneCount, Run.MetricsAfterReroute.EdgeCrossingCount, Run.DesignDiagnostics.FanoutIntentGroupCount));
		}

		{
			TMap<int32, TSet<int32>> OutputsByRoot;
			OutputsByRoot.FindOrAdd(1).Add(100);
			OutputsByRoot.FindOrAdd(1).Add(200);
			OutputsByRoot.FindOrAdd(2).Add(100);
			OutputsByRoot.FindOrAdd(2).Add(200);
			TMap<FString, TArray<double>> InterfaceOrderByRootTarget;
			InterfaceOrderByRootTarget.Add(MakeRootTargetKey(1, 100), {1.0});
			InterfaceOrderByRootTarget.Add(MakeRootTargetKey(2, 100), {0.0});
			InterfaceOrderByRootTarget.Add(MakeRootTargetKey(1, 200), {0.0});
			InterfaceOrderByRootTarget.Add(MakeRootTargetKey(2, 200), {1.0});
			TMap<FString, double> WeightByRootTarget;
			WeightByRootTarget.Add(MakeRootTargetKey(1, 100), ColumnDistanceInfluenceWeight(1));
			WeightByRootTarget.Add(MakeRootTargetKey(2, 100), ColumnDistanceInfluenceWeight(1));
			WeightByRootTarget.Add(MakeRootTargetKey(1, 200), ColumnDistanceInfluenceWeight(2));
			WeightByRootTarget.Add(MakeRootTargetKey(2, 200), ColumnDistanceInfluenceWeight(2));
			double AdjacentFirstScore = 0.0;
			double AdjacentFirstInversion = 0.0;
			ScoreRootIslandOrder({2, 1}, OutputsByRoot, InterfaceOrderByRootTarget, WeightByRootTarget, AdjacentFirstScore, AdjacentFirstInversion);
			double DistantFirstScore = 0.0;
			double DistantFirstInversion = 0.0;
			ScoreRootIslandOrder({1, 2}, OutputsByRoot, InterfaceOrderByRootTarget, WeightByRootTarget, DistantFirstScore, DistantFirstInversion);
			const bool bAdjacentColumnDominatesDistantConflict =
				FMath::IsNearlyEqual(AdjacentFirstScore, DistantFirstScore, 0.001)
				&& AdjacentFirstInversion < DistantFirstInversion;
			AddCase(TEXT("topology_column_order_weights_adjacent_over_distant_target"),
				bAdjacentColumnDominatesDistantConflict,
				FString::Printf(TEXT("adjacent_inv=%.3f distant_inv=%.3f score=%.3f/%.3f"),
					AdjacentFirstInversion,
					DistantFirstInversion,
					AdjacentFirstScore,
					DistantFirstScore));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 8; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("sourcepull%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 120.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(120.0, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{5, 6, TEXT("out"), TEXT("in"), 0, 0, false},
				{5, 7, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const double SourceCenterY = Nodes[5].NewY + Nodes[5].Height * 0.5;
			const double ConsumerCenterY = (Nodes[6].NewY + Nodes[6].Height * 0.5 + Nodes[7].NewY + Nodes[7].Height * 0.5) * 0.5;
			const bool bSourcePulledToConsumers = FMath::Abs(SourceCenterY - ConsumerCenterY) <= Options.VerticalSpacing * 1.5;
			AddCase(TEXT("source_island_pulled_to_consumer_entries"), bSourcePulledToConsumers, FString::Printf(TEXT("source_y=%.1f consumer_mid_y=%.1f delta=%.1f"), SourceCenterY, ConsumerCenterY, FMath::Abs(SourceCenterY - ConsumerCenterY)));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 8; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("componentx%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 1 ? 2200.0 : 120.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{3, 4, TEXT("out"), TEXT("in"), 0, 0, false},
				{3, 6, TEXT("out"), TEXT("in"), 0, 0, false},
				{4, 5, TEXT("out"), TEXT("in"), 0, 0, false},
				{4, 7, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const double WideSinkLeft = Nodes[1].NewX;
			const double OtherSinkLeft = Nodes[2].NewX;
			const double LeftResidual = FMath::Abs(WideSinkLeft - OtherSinkLeft);
			const bool bReadySetNoOutputIslandsShareLeftEdge = Run.DesignDiagnostics.PlacementComponentCount >= 2
				&& LeftResidual <= 0.5;
			AddCase(TEXT("ready_set_no_output_islands_share_left_edge"), bReadySetNoOutputIslandsShareLeftEdge, FString::Printf(TEXT("components=%d left=%.1f/%.1f residual=%.1f x=%.1f/%.1f/%.1f"), Run.DesignDiagnostics.PlacementComponentCount, WideSinkLeft, OtherSinkLeft, LeftResidual, Nodes[1].NewX, Nodes[4].NewX, Nodes[5].NewX));
			const bool bBidirectionalReanchorRanFromMiddleColumn = Run.DesignDiagnostics.IslandLeftwardReanchorBaseColumn == 1
				&& Run.DesignDiagnostics.IslandLeftwardReanchorColumnCount >= 2;
			AddCase(TEXT("ready_set_bidirectional_reanchor_runs_from_tallest_column"), bBidirectionalReanchorRanFromMiddleColumn, FString::Printf(TEXT("base_column=%d reanchor_columns=%d"), Run.DesignDiagnostics.IslandLeftwardReanchorBaseColumn, Run.DesignDiagnostics.IslandLeftwardReanchorColumnCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("execfanout%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 130.0;
				Node.Height = 80.0;
				Node.bHasExecPin = true;
				Node.OutputPinNames = { TEXT("then"), TEXT("else") };
				Node.InputPinNames = { TEXT("execute") };
				Node.OutputPinAnchors = { FVector2D(130.0, 30.0), FVector2D(130.0, 56.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 40.0) };
				Node.OutputPinAnchorValid = { 1, 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("then"), TEXT("execute"), 0, 0, true},
				{0, 2, TEXT("else"), TEXT("execute"), 1, 0, true}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const bool bExecFanoutSplitSourceIsland = Run.IslandCount == 3
				&& Run.IslandEdgeCount == 2
				&& Run.DesignDiagnostics.FanoutImplicitEdgeCount == 2
				&& Nodes[0].NewX < Nodes[1].NewX
				&& Nodes[0].NewX < Nodes[2].NewX;
			AddCase(TEXT("exec_total_output_fanout_splits_source_island"), bExecFanoutSplitSourceIsland, FString::Printf(TEXT("islands=%d island_edges=%d fanout_edges=%d reroutes=%d x=%.1f/%.1f/%.1f"), Run.IslandCount, Run.IslandEdgeCount, Run.DesignDiagnostics.FanoutImplicitEdgeCount, Run.ReroutePlanCount, Nodes[0].NewX, Nodes[1].NewX, Nodes[2].NewX));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("c%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 150.0;
				Node.Height = 80.0;
				Node.bHasExecPin = true;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(150.0, 40.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 40.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, true},
				{1, 2, TEXT("out"), TEXT("in"), 0, 0, true},
				{2, 3, TEXT("out"), TEXT("in"), 0, 0, true}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const bool bColumnsIncrease = Nodes[0].NewX < Nodes[1].NewX && Nodes[1].NewX < Nodes[2].NewX && Nodes[2].NewX < Nodes[3].NewX;
			AddCase(TEXT("island_column_assignment_and_two_column_anchor"), bColumnsIncrease && Run.MetricsAfter.OverlapCount == 0, FString::Printf(TEXT("x=%.1f/%.1f/%.1f/%.1f overlap=%d"), Nodes[0].NewX, Nodes[1].NewX, Nodes[2].NewX, Nodes[3].NewX, Run.MetricsAfter.OverlapCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("portorder%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 150.0;
				Node.Height = 96.0;
				Node.bHasExecPin = true;
				Node.OutputPinNames = { TEXT("out") };
				Node.OutputPinAnchors = { FVector2D(150.0, 48.0) };
				Node.OutputPinAnchorValid = { 1 };
				if (Index == 3)
				{
					Node.InputPinNames = { TEXT("upper"), TEXT("lower") };
					Node.InputPinAnchors = { FVector2D(0.0, 28.0), FVector2D(0.0, 68.0) };
					Node.InputPinAnchorValid = { 1, 1 };
				}
				else
				{
					Node.InputPinNames = { TEXT("in") };
					Node.InputPinAnchors = { FVector2D(0.0, 48.0) };
					Node.InputPinAnchorValid = { 1 };
				}
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, true},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, true},
				{1, 3, TEXT("out"), TEXT("lower"), 0, 1, true},
				{2, 3, TEXT("out"), TEXT("upper"), 0, 0, true}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const bool bPortOrderControlsSameNeighbor =
				Nodes[1].Rank == Nodes[2].Rank
				&& Nodes[2].NewY < Nodes[1].NewY
				&& Run.MetricsAfter.OverlapCount == 0;
			AddCase(TEXT("same_neighbor_input_port_order_controls_column_order"), bPortOrderControlsSameNeighbor, FString::Printf(TEXT("rank=%d/%d y_upper_source=%.1f y_lower_source=%.1f overlap=%d"), Nodes[2].Rank, Nodes[1].Rank, Nodes[2].NewY, Nodes[1].NewY, Run.MetricsAfter.OverlapCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("outputportorder%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 150.0;
				Node.Height = 96.0;
				Node.bHasExecPin = true;
				Node.InputPinNames = { TEXT("in") };
				Node.InputPinAnchors = { FVector2D(0.0, 48.0) };
				Node.InputPinAnchorValid = { 1 };
				if (Index == 0)
				{
					Node.OutputPinNames = { TEXT("upper"), TEXT("lower") };
					Node.OutputPinAnchors = { FVector2D(150.0, 28.0), FVector2D(150.0, 68.0) };
					Node.OutputPinAnchorValid = { 1, 1 };
				}
				else
				{
					Node.OutputPinNames = { TEXT("out") };
					Node.OutputPinAnchors = { FVector2D(150.0, 48.0) };
					Node.OutputPinAnchorValid = { 1 };
				}
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 2, TEXT("lower"), TEXT("in"), 1, 0, true},
				{0, 1, TEXT("upper"), TEXT("in"), 0, 0, true}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const bool bOutputPortOrderControlsSameNeighbor =
				Nodes[1].Rank == Nodes[2].Rank
				&& Nodes[1].NewY < Nodes[2].NewY
				&& Run.MetricsAfter.OverlapCount == 0;
			AddCase(TEXT("same_neighbor_output_port_order_controls_column_order"), bOutputPortOrderControlsSameNeighbor, FString::Printf(TEXT("rank=%d/%d y_upper_target=%.1f y_lower_target=%.1f overlap=%d"), Nodes[1].Rank, Nodes[2].Rank, Nodes[1].NewY, Nodes[2].NewY, Run.MetricsAfter.OverlapCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 5; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("demandprop%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 150.0;
				Node.Height = 96.0;
				Node.Rank = Index < 2 ? 0 : (Index < 4 ? 1 : 2);
				Node.InputPinNames = { TEXT("in") };
				Node.InputPinAnchors = { FVector2D(0.0, 48.0) };
				Node.InputPinAnchorValid = { 1 };
				Node.OutputPinNames = { TEXT("out") };
				Node.OutputPinAnchors = { FVector2D(150.0, 48.0) };
				Node.OutputPinAnchorValid = { 1 };
				if (Index == 4)
				{
					Node.InputPinNames = { TEXT("upper"), TEXT("lower") };
					Node.InputPinAnchors = { FVector2D(0.0, 28.0), FVector2D(0.0, 68.0) };
					Node.InputPinAnchorValid = { 1, 1 };
				}
				Nodes.Add(Node);
			}
			TArray<FEdge> AllEdges = {
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, true},
				{2, 4, TEXT("out"), TEXT("lower"), 0, 1, true},
				{1, 3, TEXT("out"), TEXT("in"), 0, 0, true},
				{3, 4, TEXT("out"), TEXT("upper"), 0, 0, true}
			};
			const TArray<int32> IslandNodes = {0, 1, 2, 3};
			TArray<FPortOrderDemandSet> DemandSets = ComputeDownstreamPortOrderDemandSets(Nodes, AllEdges, IslandNodes);
			const FPortOrderDemand* Demand0 = DemandSets.IsValidIndex(0) ? FindPortOrderDemandByGroup(DemandSets[0], 4) : nullptr;
			const FPortOrderDemand* Demand1 = DemandSets.IsValidIndex(1) ? FindPortOrderDemandByGroup(DemandSets[1], 4) : nullptr;
			TArray<TArray<int32>> Columns;
			Columns.SetNum(2);
			Columns[0] = {0, 1};
			Columns[1] = {2, 3};
			TArray<FEdge> NoAdjacentEdges;
			TArray<FPortOrderDemandSet> EmptySourceDemands;
			EmptySourceDemands.SetNum(Nodes.Num());
			MinimizeColumnCrossings(Nodes, NoAdjacentEdges, EmptySourceDemands, DemandSets, nullptr, Columns);
			const bool bDemandPropagated =
				Demand0 && Demand1
				&& Demand1->Value < Demand0->Value
				&& Columns[0].Num() == 2
				&& Columns[0][0] == 1
				&& Columns[0][1] == 0;
			AddCase(TEXT("downstream_port_order_demand_propagates_across_columns"), bDemandPropagated, FString::Printf(TEXT("group=%d/%d demand=%.3f/%.3f col0=%d,%d"), Demand1 ? Demand1->GroupNode : -1, Demand0 ? Demand0->GroupNode : -1, Demand1 ? Demand1->Value : -1.0, Demand0 ? Demand0->Value : -1.0, Columns[0].Num() > 0 ? Columns[0][0] : -1, Columns[0].Num() > 1 ? Columns[0][1] : -1));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 5; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("sourceprop%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 150.0;
				Node.Height = 96.0;
				Node.Rank = Index == 0 ? 0 : (Index < 3 ? 1 : 2);
				Node.InputPinNames = { TEXT("in") };
				Node.InputPinAnchors = { FVector2D(0.0, 48.0) };
				Node.InputPinAnchorValid = { 1 };
				Node.OutputPinNames = { TEXT("out") };
				Node.OutputPinAnchors = { FVector2D(150.0, 48.0) };
				Node.OutputPinAnchorValid = { 1 };
				if (Index == 0)
				{
					Node.OutputPinNames = { TEXT("upper"), TEXT("lower") };
					Node.OutputPinAnchors = { FVector2D(150.0, 28.0), FVector2D(150.0, 68.0) };
					Node.OutputPinAnchorValid = { 1, 1 };
				}
				Nodes.Add(Node);
			}
			TArray<FEdge> AllEdges = {
				{0, 2, TEXT("lower"), TEXT("in"), 1, 0, true},
				{2, 4, TEXT("out"), TEXT("in"), 0, 0, true},
				{0, 1, TEXT("upper"), TEXT("in"), 0, 0, true},
				{1, 3, TEXT("out"), TEXT("in"), 0, 0, true}
			};
			const TArray<int32> IslandNodes = {1, 2, 3, 4};
			TArray<FPortOrderDemandSet> SourceDemands = ComputeSourcePortOrderDemandSets(Nodes, AllEdges, IslandNodes);
			const FPortOrderDemand* Demand3 = SourceDemands.IsValidIndex(3) ? FindPortOrderDemandByGroup(SourceDemands[3], 0) : nullptr;
			const FPortOrderDemand* Demand4 = SourceDemands.IsValidIndex(4) ? FindPortOrderDemandByGroup(SourceDemands[4], 0) : nullptr;
			TArray<FPortOrderDemandSet> EmptyTargetDemands;
			EmptyTargetDemands.SetNum(Nodes.Num());
			TArray<TArray<int32>> Columns;
			Columns.SetNum(2);
			Columns[0] = {2, 1};
			Columns[1] = {4, 3};
			TArray<FEdge> AdjacentEdges = {
				{2, 4, TEXT("out"), TEXT("in"), 0, 0, true},
				{1, 3, TEXT("out"), TEXT("in"), 0, 0, true}
			};
			MinimizeColumnCrossings(Nodes, AdjacentEdges, SourceDemands, EmptyTargetDemands, nullptr, Columns);
			const bool bSourceDemandPropagated =
				Demand3 && Demand4
				&& Demand3->Value < Demand4->Value
				&& Columns[0].Num() == 2
				&& Columns[1].Num() == 2
				&& Columns[0][0] == 1
				&& Columns[0][1] == 2
				&& Columns[1][0] == 3
				&& Columns[1][1] == 4;
			AddCase(TEXT("source_port_order_demand_propagates_across_columns"), bSourceDemandPropagated, FString::Printf(TEXT("group=%d/%d demand=%.3f/%.3f col0=%d,%d col1=%d,%d"), Demand3 ? Demand3->GroupNode : -1, Demand4 ? Demand4->GroupNode : -1, Demand3 ? Demand3->Value : -1.0, Demand4 ? Demand4->Value : -1.0, Columns[0].Num() > 0 ? Columns[0][0] : -1, Columns[0].Num() > 1 ? Columns[0][1] : -1, Columns[1].Num() > 0 ? Columns[1][0] : -1, Columns[1].Num() > 1 ? Columns[1][1] : -1));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("nested_source_order%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 150.0;
				Node.Height = 96.0;
				Node.Rank = Index == 0 ? 0 : (Index == 1 ? 1 : 2);
				Node.InputPinNames = { TEXT("in") };
				Node.InputPinAnchors = { FVector2D(0.0, 48.0) };
				Node.InputPinAnchorValid = { 1 };
				Node.OutputPinNames = { TEXT("out") };
				Node.OutputPinAnchors = { FVector2D(150.0, 48.0) };
				Node.OutputPinAnchorValid = { 1 };
				if (Index == 0)
				{
					Node.OutputPinNames = { TEXT("upper"), TEXT("lower") };
					Node.OutputPinAnchors = { FVector2D(150.0, 28.0), FVector2D(150.0, 68.0) };
					Node.OutputPinAnchorValid = { 1, 1 };
				}
				Nodes.Add(Node);
			}
			const TArray<FEdge> AllEdges = {
				{0, 1, TEXT("upper"), TEXT("in"), 0, 0, true},
				{1, 3, TEXT("out"), TEXT("in"), 0, 0, true},
				{0, 2, TEXT("lower"), TEXT("in"), 1, 0, true}
			};
			const TArray<int32> IslandNodes = {1, 2, 3};
			const TArray<FPortOrderDemandSet> SourceDemands = ComputeSourcePortOrderDemandSets(Nodes, AllEdges, IslandNodes);
			const FPortOrderDemand* UpperPathDemand = SourceDemands.IsValidIndex(3) ? FindPortOrderDemandByGroup(SourceDemands[3], 0) : nullptr;
			const FPortOrderDemand* LowerPathDemand = SourceDemands.IsValidIndex(2) ? FindPortOrderDemandByGroup(SourceDemands[2], 0) : nullptr;
			TArray<FPortOrderDemandSet> EmptyTargetDemands;
			EmptyTargetDemands.SetNum(Nodes.Num());
			TArray<TArray<int32>> Columns;
			Columns.SetNum(3);
			Columns[1] = {1};
			Columns[2] = {2, 3};
			const TArray<FEdge> AdjacentEdges = {
				{1, 3, TEXT("out"), TEXT("in"), 0, 0, true}
			};
			MinimizeColumnCrossings(Nodes, AdjacentEdges, SourceDemands, EmptyTargetDemands, nullptr, Columns);
			const bool bNestedSourceOrderSurvivesIntermediateBranch =
				UpperPathDemand && LowerPathDemand
				&& UpperPathDemand->Value < LowerPathDemand->Value
				&& Columns[2].Num() == 2
				&& Columns[2][0] == 3
				&& Columns[2][1] == 2;
			AddCase(TEXT("nested_source_port_order_overrides_neighbor_presence"),
				bNestedSourceOrderSurvivesIntermediateBranch,
				FString::Printf(TEXT("upper=%.3f lower=%.3f col2=%d,%d"),
					UpperPathDemand ? UpperPathDemand->Value : -1.0,
					LowerPathDemand ? LowerPathDemand->Value : -1.0,
					Columns[2].Num() > 0 ? Columns[2][0] : -1,
					Columns[2].Num() > 1 ? Columns[2][1] : -1));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("slack%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 150.0;
				Node.Height = 80.0;
				Node.bHasExecPin = Index < 3;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(150.0, 40.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 40.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, true},
				{1, 2, TEXT("out"), TEXT("in"), 0, 0, true},
				{3, 2, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const bool bSingleConsumerPulledRight = Nodes[3].Rank == Nodes[2].Rank - 1
				&& Nodes[3].NewX > Nodes[0].NewX
				&& Nodes[3].NewX < Nodes[2].NewX;
			AddCase(TEXT("rank_assignment_minimizes_connected_rank_slack"), bSingleConsumerPulledRight, FString::Printf(TEXT("ranks=%d/%d/%d/%d x=%.1f/%.1f/%.1f/%.1f"), Nodes[0].Rank, Nodes[1].Rank, Nodes[2].Rank, Nodes[3].Rank, Nodes[0].NewX, Nodes[1].NewX, Nodes[2].NewX, Nodes[3].NewX));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("input_chain%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 150.0;
				Node.Height = 80.0;
				Node.bHasExecPin = Index == 0 || Index == 3;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(150.0, 40.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 40.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 3, TEXT("out"), TEXT("in"), 0, 0, true},
				{1, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{2, 3, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const bool bChainExtractedAndInserted = Run.DesignDiagnostics.InputChainCount == 2
				&& Run.DesignDiagnostics.InputChainNodeCount == 2
				&& Nodes[0].NewX < Nodes[1].NewX
				&& Nodes[1].NewX < Nodes[2].NewX
				&& Nodes[2].NewX < Nodes[3].NewX
				&& Nodes[3].Rank == 1;
			AddCase(TEXT("nested_single_input_chain_attaches_between_columns"), bChainExtractedAndInserted, FString::Printf(TEXT("chains=%d nodes=%d rank=%d/%d/%d/%d x=%.1f/%.1f/%.1f/%.1f"), Run.DesignDiagnostics.InputChainCount, Run.DesignDiagnostics.InputChainNodeCount, Nodes[0].Rank, Nodes[1].Rank, Nodes[2].Rank, Nodes[3].Rank, Nodes[0].NewX, Nodes[1].NewX, Nodes[2].NewX, Nodes[3].NewX));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 5; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("compound_input_chain%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 2 ? 220.0 : (Index <= 1 ? 180.0 : 146.0);
				Node.Height = Index == 2 ? 112.0 : (Index <= 1 ? 94.0 : 38.0);
				Node.bHasExecPin = Index <= 1;
				if (Index == 1)
				{
					Node.InputPinNames = { TEXT("exec"), TEXT("condition") };
					Node.InputPinAnchors = { FVector2D(0.0, 24.0), FVector2D(0.0, 66.0) };
					Node.InputPinAnchorValid = { 1, 1 };
					Node.OutputPinNames = { TEXT("then") };
					Node.OutputPinAnchors = { FVector2D(Node.Width, 24.0) };
					Node.OutputPinAnchorValid = { 1 };
				}
				else if (Index == 2)
				{
					Node.InputPinNames = { TEXT("a"), TEXT("b") };
					Node.InputPinAnchors = { FVector2D(0.0, 36.0), FVector2D(0.0, 76.0) };
					Node.InputPinAnchorValid = { 1, 1 };
					Node.OutputPinNames = { TEXT("out") };
					Node.OutputPinAnchors = { FVector2D(Node.Width, 56.0) };
					Node.OutputPinAnchorValid = { 1 };
				}
				else if (Index >= 3)
				{
					Node.InputPinNames.Reset();
					Node.InputPinAnchors.Reset();
					Node.InputPinAnchorValid.Reset();
					Node.OutputPinNames = { TEXT("value") };
					Node.OutputPinAnchors = { FVector2D(Node.Width, 19.0) };
					Node.OutputPinAnchorValid = { 1 };
				}
				else
				{
					Node.InputPinNames = { TEXT("exec") };
					Node.InputPinAnchors = { FVector2D(0.0, 24.0) };
					Node.InputPinAnchorValid = { 1 };
					Node.OutputPinNames = { TEXT("then") };
					Node.OutputPinAnchors = { FVector2D(Node.Width, 24.0) };
					Node.OutputPinAnchorValid = { 1 };
				}
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("then"), TEXT("exec"), 0, 0, true},
				{2, 1, TEXT("out"), TEXT("condition"), 0, 1, false},
				{3, 2, TEXT("value"), TEXT("a"), 0, 0, false},
				{4, 2, TEXT("value"), TEXT("b"), 0, 1, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const double PureOutY = Nodes[2].NewY + GetOutputAnchor(Nodes[2], 0).Y;
			const double BranchConditionY = Nodes[1].NewY + GetInputAnchor(Nodes[1], 1).Y;
			const double BranchExecY = Nodes[1].NewY + GetInputAnchor(Nodes[1], 0).Y;
			const double LeafAY = Nodes[3].NewY + GetOutputAnchor(Nodes[3], 0).Y;
			const double PureAY = Nodes[2].NewY + GetInputAnchor(Nodes[2], 0).Y;
			const double LeafBY = Nodes[4].NewY + GetOutputAnchor(Nodes[4], 0).Y;
			const double PureBY = Nodes[2].NewY + GetInputAnchor(Nodes[2], 1).Y;
			const double ZoneHalfHeight = InputChainForbiddenZoneHalfHeight(Options);
			const bool bPureAvoidsBranchExecZone =
				!(Nodes[2].NewY < BranchExecY + ZoneHalfHeight && Nodes[2].NewY + Nodes[2].Height > BranchExecY - ZoneHalfHeight);
			const bool bCompoundInputChainAligned = Run.DesignDiagnostics.InputChainCount == 3
				&& Run.DesignDiagnostics.InputChainNodeCount == 3
				&& Nodes[3].NewX < Nodes[2].NewX
				&& Nodes[4].NewX < Nodes[2].NewX
				&& Nodes[2].NewX < Nodes[1].NewX
				&& bPureAvoidsBranchExecZone
				&& Run.DesignDiagnostics.InputChainForbiddenZoneCount >= 1
				&& Run.DesignDiagnostics.InputChainForbiddenAvoidanceCount >= 1
				&& FMath::Abs(LeafAY - PureAY) <= InputChainTargetGroupGap(Options) + 1.0
				&& FMath::Abs(LeafBY - PureBY) <= InputChainTargetGroupGap(Options) + 1.0;
			AddCase(TEXT("compound_input_chain_uses_nested_two_column_anchor"), bCompoundInputChainAligned, FString::Printf(TEXT("chains=%d nodes=%d x=%.1f/%.1f/%.1f/%.1f residual=%.1f/%.1f/%.1f exec_avoided=%s"), Run.DesignDiagnostics.InputChainCount, Run.DesignDiagnostics.InputChainNodeCount, Nodes[3].NewX, Nodes[4].NewX, Nodes[2].NewX, Nodes[1].NewX, FMath::Abs(PureOutY - BranchConditionY), FMath::Abs(LeafAY - PureAY), FMath::Abs(LeafBY - PureBY), bPureAvoidsBranchExecZone ? TEXT("true") : TEXT("false")));
		}

		{
			FOptions PixelOptions = Options;
			PixelOptions.OriginX = 10.35;
			PixelOptions.OriginY = 20.65;
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("pixel_snap%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 123.0;
				Node.Height = 67.0;
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinAnchors = { FVector2D(0.0, 31.2) };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 31.2) };
				Node.InputPinAnchorValid = { 1 };
				Node.OutputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, PixelOptions, Run);
			FReroutePlan Plan;
			Plan.Points = { FVector2D(1.25, 2.75), FVector2D(11.5, 9.49) };
			FinalizeReroutePlanPoints(Plan, PixelOptions);
			bool bNodePositionsSnapped = true;
			for (const FNode& Node : Nodes)
			{
				bNodePositionsSnapped = bNodePositionsSnapped
					&& FMath::IsNearlyEqual(Node.NewX, SnapGraphCoordinate(Node.NewX), 0.001)
					&& FMath::IsNearlyEqual(Node.NewY, SnapGraphCoordinate(Node.NewY), 0.001);
			}
			bool bReroutePointsSnapped = Plan.Points.Num() == Plan.KnotPoints.Num();
			for (const FVector2D& Point : Plan.Points)
			{
				bReroutePointsSnapped = bReroutePointsSnapped
					&& FMath::IsNearlyEqual(Point.X, SnapGraphCoordinate(Point.X), 0.001)
					&& FMath::IsNearlyEqual(Point.Y, SnapGraphCoordinate(Point.Y), 0.001);
			}
			AddCase(TEXT("layout_output_positions_are_pixel_snapped"), bNodePositionsSnapped && bReroutePointsSnapped, FString::Printf(TEXT("node0=(%.3f,%.3f) node1=(%.3f,%.3f) reroutes=%d"), Nodes[0].NewX, Nodes[0].NewY, Nodes[1].NewX, Nodes[1].NewY, Plan.Points.Num()));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 13; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("input_chain_corridor%d"), Index);
				Node.Title = Node.Guid;
				Node.bHasExecPin = Index <= 6;
				Node.Width = Index == 0 ? 363.0 : (Index <= 6 ? 250.0 : (Index % 2 == 0 ? 237.0 : 195.0));
				Node.Height = Index == 0 ? 294.0 : (Index >= 4 && Index <= 6 ? 126.0 : 112.0);
				Node.Rank = Index == 0 ? 0 : (Index <= 3 ? 1 : (Index <= 6 ? 2 : 1));
				Node.InputPinNames = { TEXT("exec"), TEXT("a"), TEXT("b") };
				Node.OutputPinNames = { TEXT("exec"), TEXT("out") };
				Node.InputPinAnchors = { FVector2D(0.0, 24.0), FVector2D(0.0, 60.0), FVector2D(0.0, 96.0) };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 24.0), FVector2D(Node.Width, 60.0) };
				Node.InputPinAnchorValid = { 1, 1, 1 };
				Node.OutputPinAnchorValid = { 1, 1 };
				if (Index >= 7)
				{
					Node.Height = 70.0;
					Node.InputPinNames.Reset();
					Node.InputPinAnchors.Reset();
					Node.InputPinAnchorValid.Reset();
					Node.OutputPinNames = { TEXT("value") };
					Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
					Node.OutputPinAnchorValid = { 1 };
				}
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("exec"), TEXT("exec"), 0, 0, true},
				{0, 2, TEXT("exec"), TEXT("exec"), 0, 0, true},
				{0, 3, TEXT("exec"), TEXT("exec"), 0, 0, true},
				{1, 4, TEXT("exec"), TEXT("exec"), 0, 0, true},
				{2, 5, TEXT("exec"), TEXT("exec"), 0, 0, true},
				{3, 6, TEXT("exec"), TEXT("exec"), 0, 0, true},
				{7, 4, TEXT("value"), TEXT("a"), 0, 1, false},
				{8, 4, TEXT("value"), TEXT("b"), 0, 2, false},
				{9, 5, TEXT("value"), TEXT("a"), 0, 1, false},
				{10, 5, TEXT("value"), TEXT("b"), 0, 2, false},
				{11, 6, TEXT("value"), TEXT("a"), 0, 1, false},
				{12, 6, TEXT("value"), TEXT("b"), 0, 2, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.Columns.SetNum(3);
			Island.Columns[0] = {0};
			Island.Columns[1] = {1, 2, 3};
			Island.Columns[2] = {4, 5, 6};
			for (int32 ChainIndex = 0; ChainIndex < 6; ++ChainIndex)
			{
				FInputChain Chain;
				Chain.Id = ChainIndex;
				Chain.TargetNode = ChainIndex < 2 ? 4 : (ChainIndex < 4 ? 5 : 6);
				Chain.TargetPinIndex = (ChainIndex % 2) + 1;
				Chain.TargetEdgeIndex = 6 + ChainIndex;
				Chain.Nodes = {7 + ChainIndex};
				Chain.EdgeIndices = {6 + ChainIndex};
				Island.InputChains.Add(Chain);
				Island.InputChainNodeSet.Add(7 + ChainIndex);
			}
			FRunResult Run;
			TSet<int32> EmptyRankIgnoredEdges;
			LayoutIslandInternally(Nodes, Edges, Edges, EmptyRankIgnoredEdges, Island, Options, Run);
			double PreviousColumnRight = -TNumericLimits<double>::Max();
			double TargetColumnLeft = TNumericLimits<double>::Max();
			for (int32 NodeIndex = 1; NodeIndex <= 3; ++NodeIndex)
			{
				PreviousColumnRight = FMath::Max(PreviousColumnRight, Nodes[NodeIndex].NewX + Nodes[NodeIndex].Width);
			}
			for (int32 NodeIndex = 4; NodeIndex <= 6; ++NodeIndex)
			{
				TargetColumnLeft = FMath::Min(TargetColumnLeft, Nodes[NodeIndex].NewX);
			}
			bool bChainsStayInReservedCorridor = true;
			for (int32 NodeIndex = 7; NodeIndex <= 12; ++NodeIndex)
			{
				bChainsStayInReservedCorridor = bChainsStayInReservedCorridor
					&& Nodes[NodeIndex].NewX + 0.5 >= PreviousColumnRight + Options.NodePadding
					&& Nodes[NodeIndex].NewX + Nodes[NodeIndex].Width <= TargetColumnLeft - EndpointAccessClearanceX(Options) + 0.5;
			}
			bool bChainsStayAttachedToTargetRows = true;
			for (int32 TargetGroup = 0; TargetGroup < 3; ++TargetGroup)
			{
				const int32 TargetNode = 4 + TargetGroup;
				const int32 FirstChainNode = 7 + TargetGroup * 2;
				const int32 SecondChainNode = FirstChainNode + 1;
				const double GroupTop = FMath::Min(Nodes[FirstChainNode].NewY, Nodes[SecondChainNode].NewY);
				const double GroupBottom = FMath::Max(Nodes[FirstChainNode].NewY + Nodes[FirstChainNode].Height, Nodes[SecondChainNode].NewY + Nodes[SecondChainNode].Height);
				const double TargetTop = Nodes[TargetNode].NewY;
				const double TargetBottom = Nodes[TargetNode].NewY + Nodes[TargetNode].Height;
				const double ExecY = TargetTop + GetInputAnchor(Nodes[TargetNode], 0).Y;
				const double FirstOutputY = Nodes[FirstChainNode].NewY + GetOutputAnchor(Nodes[FirstChainNode], 0).Y;
				const double FirstTargetY = TargetTop + GetInputAnchor(Nodes[TargetNode], 1).Y;
				const double SecondOutputY = Nodes[SecondChainNode].NewY + GetOutputAnchor(Nodes[SecondChainNode], 0).Y;
				const double SecondTargetY = TargetTop + GetInputAnchor(Nodes[TargetNode], 2).Y;
				const double FirstPinResidual = FMath::Abs(FirstOutputY - FirstTargetY);
				const double SecondPinResidual = SecondOutputY - SecondTargetY;
				bChainsStayAttachedToTargetRows = bChainsStayAttachedToTargetRows
					&& GroupTop + 0.5 >= TargetTop - Options.NodePadding
					&& GroupBottom <= TargetBottom + Options.VerticalSpacing + 0.5
					&& FirstOutputY > ExecY + 4.0
					&& FirstPinResidual <= 1.0
					&& SecondPinResidual >= -0.5
					&& SecondPinResidual <= Nodes[SecondChainNode].Height + InputChainTargetGroupGap(Options) + 1.0;
			}
			AddCase(TEXT("input_chain_corridor_keeps_parameters_between_columns"), bChainsStayInReservedCorridor && bChainsStayAttachedToTargetRows, FString::Printf(TEXT("prev_right=%.1f target_left=%.1f attached=%s chain_y=%.1f/%.1f/%.1f/%.1f/%.1f/%.1f"), PreviousColumnRight, TargetColumnLeft, bChainsStayAttachedToTargetRows ? TEXT("true") : TEXT("false"), Nodes[7].NewY, Nodes[8].NewY, Nodes[9].NewY, Nodes[10].NewY, Nodes[11].NewY, Nodes[12].NewY));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("input_chain_slot%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 0 ? 180.0 : (Index == 1 ? 240.0 : 150.0);
				Node.Height = Index == 1 ? 120.0 : 64.0;
				Node.bHasExecPin = Index == 1;
				Node.OutputPinNames = { TEXT("value") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 32.0) };
				Node.OutputPinAnchorValid = { 1 };
				if (Index == 1)
				{
					Node.InputPinNames = { TEXT("exec"), TEXT("upper"), TEXT("lower") };
					Node.InputPinAnchors = { FVector2D(0.0, 24.0), FVector2D(0.0, 58.0), FVector2D(0.0, 94.0) };
					Node.InputPinAnchorValid = { 1, 1, 1 };
				}
				else
				{
					Node.InputPinNames.Reset();
					Node.InputPinAnchors.Reset();
					Node.InputPinAnchorValid.Reset();
				}
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("exec"), TEXT("exec"), 0, 0, true},
				{2, 1, TEXT("value"), TEXT("upper"), 0, 1, false},
				{3, 1, TEXT("value"), TEXT("lower"), 0, 2, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.Columns.SetNum(2);
			Island.Columns[0] = {0};
			Island.Columns[1] = {1};
			for (int32 ChainIndex = 0; ChainIndex < 2; ++ChainIndex)
			{
				FInputChain Chain;
				Chain.Id = ChainIndex;
				Chain.TargetNode = 1;
				Chain.TargetPinIndex = ChainIndex + 1;
				Chain.TargetEdgeIndex = ChainIndex + 1;
				Chain.Nodes = {ChainIndex + 2};
				Chain.EdgeIndices = {ChainIndex + 1};
				Island.InputChains.Add(Chain);
				Island.InputChainNodeSet.Add(ChainIndex + 2);
			}
			FRunResult Run;
			TSet<int32> EmptyRankIgnoredEdges;
			LayoutIslandInternally(Nodes, Edges, Edges, EmptyRankIgnoredEdges, Island, Options, Run);
			const double TargetTop = Nodes[1].NewY;
			const double UpperOutputY = Nodes[2].NewY + GetOutputAnchor(Nodes[2], 0).Y;
			const double LowerOutputY = Nodes[3].NewY + GetOutputAnchor(Nodes[3], 0).Y;
			const double UpperTargetY = TargetTop + GetInputAnchor(Nodes[1], 1).Y;
			const double LowerTargetY = TargetTop + GetInputAnchor(Nodes[1], 2).Y;
			const double UpperPinResidual = FMath::Abs(UpperOutputY - UpperTargetY);
			const double LowerPinResidual = LowerOutputY - LowerTargetY;
			const double GroupTop = FMath::Min(Nodes[2].NewY, Nodes[3].NewY);
			const double GroupBottom = FMath::Max(Nodes[2].NewY + Nodes[2].Height, Nodes[3].NewY + Nodes[3].Height);
			const double TargetSlotBottom = TargetTop + FMath::Max(Nodes[1].Height, GroupBottom - TargetTop);
			const bool bInputChainSlotPlacement = UpperOutputY < LowerOutputY
				&& GroupTop + 0.5 >= TargetTop
				&& GroupBottom <= TargetSlotBottom + 0.5
				&& UpperPinResidual <= 1.0
				&& LowerPinResidual >= -0.5
				&& LowerPinResidual <= Nodes[3].Height + InputChainTargetGroupGap(Options) + 1.0
				&& Run.DesignDiagnostics.IslandLocalMutationCount == 0;
			AddCase(TEXT("input_chain_uses_target_slot_without_external_offset"), bInputChainSlotPlacement, FString::Printf(TEXT("upper_y=%.1f/%.1f lower_y=%.1f/%.1f residual=%.1f/%.1f group=%.1f..%.1f slot=%.1f..%.1f"), UpperOutputY, UpperTargetY, LowerOutputY, LowerTargetY, UpperPinResidual, LowerPinResidual, GroupTop, GroupBottom, TargetTop, TargetSlotBottom));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("strict_single_pass%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 160.0;
				Node.Height = 80.0;
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinAnchors = { FVector2D(0.0, 20.0) };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 50.0) };
				Node.InputPinAnchorValid = { 1 };
				Node.OutputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 80.0;
			Nodes[2].NewX = 520.0;
			Nodes[2].NewY = 300.0;
			TArray<TArray<int32>> Columns;
			Columns.SetNum(3);
			Columns[0] = {0};
			Columns[1] = {1};
			Columns[2] = {2};
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{1, 2, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			TArray<FWeightedKnownSet> KnownSets;
			AddWeightedKnownSetForColumn(Columns, 0, 1.0, KnownSets);
			TArray<TSharedPtr<FJsonValue>> ItemDiagnostics;
			PlaceAnchoredColumnByKnownSets(
				Nodes,
				Edges,
				KnownSets,
				Columns[1],
				260.0,
				Options.NodePadding,
				nullptr,
				nullptr,
				nullptr,
				TEXT("strict_single_right_propagation"),
				0,
				1,
				&ItemDiagnostics);
			const double LeftY = Nodes[0].NewY + GetOutputAnchor(Nodes[0], 0).Y;
			const double TargetInputY = Nodes[1].NewY + GetInputAnchor(Nodes[1], 0).Y;
			const double TargetOutputY = Nodes[1].NewY + GetOutputAnchor(Nodes[1], 0).Y;
			const double FutureRightY = Nodes[2].NewY + GetInputAnchor(Nodes[2], 0).Y;
			const bool bStrictSinglePassUsesOnlyKnownAdjacent =
				FMath::Abs(LeftY - TargetInputY) <= 1.0
				&& FMath::Abs(TargetOutputY - FutureRightY) > 100.0
				&& ItemDiagnostics.Num() == 1;
			AddCase(TEXT("strict_single_pass_column_uses_only_immediate_known_neighbor"), bStrictSinglePassUsesOnlyKnownAdjacent, FString::Printf(TEXT("left_residual=%.1f future_right_residual=%.1f item_diags=%d target_y=%.1f"), FMath::Abs(LeftY - TargetInputY), FMath::Abs(TargetOutputY - FutureRightY), ItemDiagnostics.Num(), Nodes[1].NewY));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("input_chain_forbidden%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 1 ? 230.0 : 140.0;
				Node.Height = Index == 1 ? 120.0 : 50.0;
				Node.bHasExecPin = Index == 0 || Index == 1 || Index == 3;
				Node.OutputPinNames = { TEXT("out") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 25.0) };
				Node.OutputPinAnchorValid = { 1 };
				if (Index == 1)
				{
					Node.InputPinNames = { TEXT("exec"), TEXT("blocked"), TEXT("chain") };
					Node.InputPinAnchors = { FVector2D(0.0, 24.0), FVector2D(0.0, 58.0), FVector2D(0.0, 62.0) };
					Node.InputPinAnchorValid = { 1, 1, 1 };
				}
				else
				{
					Node.InputPinNames.Reset();
					Node.InputPinAnchors.Reset();
					Node.InputPinAnchorValid.Reset();
				}
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("exec"), 0, 0, true},
				{2, 1, TEXT("out"), TEXT("chain"), 0, 2, false},
				{3, 1, TEXT("out"), TEXT("blocked"), 0, 1, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.Columns.SetNum(2);
			Island.Columns[0] = {0, 3};
			Island.Columns[1] = {1};
			FInputChain Chain;
			Chain.Id = 0;
			Chain.TargetNode = 1;
			Chain.TargetPinIndex = 2;
			Chain.TargetEdgeIndex = 1;
			Chain.Nodes = {2};
			Chain.EdgeIndices = {1};
			Island.InputChains.Add(Chain);
			Island.InputChainNodeSet.Add(2);
			FRunResult Run;
			TSet<int32> EmptyRankIgnoredEdges;
			LayoutIslandInternally(Nodes, Edges, Edges, EmptyRankIgnoredEdges, Island, Options, Run);
			TArray<FEdge> EdgesWithTargetOutput = Edges;
			EdgesWithTargetOutput.Add({1, 0, TEXT("out"), TEXT("unused"), 0, 0, false});
			FIsland BoundaryOnlyIsland = Island;
			BoundaryOnlyIsland.Nodes = {1, 2};
			BoundaryOnlyIsland.NodeSet.Reset();
			BoundaryOnlyIsland.NodeSet.Add(1);
			BoundaryOnlyIsland.NodeSet.Add(2);
			const TArray<FInputChainForbiddenZone> DirectZones = BuildInputChainForbiddenZonesForTarget(
				Nodes,
				EdgesWithTargetOutput,
				BoundaryOnlyIsland,
				1,
				FVector2D(Nodes[1].NewX, Nodes[1].NewY),
				Options);
			bool bOutputPinIgnoredForForbiddenZones = DirectZones.Num() == 2;
			bool bExecAndExternalInputsIncludedForForbiddenZones = DirectZones.Num() == 2;
			for (const FInputChainForbiddenZone& Zone : DirectZones)
			{
				bOutputPinIgnoredForForbiddenZones = bOutputPinIgnoredForForbiddenZones && !Zone.bOutputPin;
				bExecAndExternalInputsIncludedForForbiddenZones =
					bExecAndExternalInputsIncludedForForbiddenZones && (Zone.PinIndex == 0 || Zone.PinIndex == 1);
			}

			const double BlockedPinY = Nodes[1].NewY + GetInputAnchor(Nodes[1], 1).Y;
			const double ZoneHalfHeight = InputChainForbiddenZoneHalfHeight(Options);
			const double ChainTop = Nodes[2].NewY;
			const double ChainBottom = Nodes[2].NewY + Nodes[2].Height;
			const bool bForbiddenZoneAvoided =
				Run.DesignDiagnostics.InputChainForbiddenZoneCount >= 1
				&& Run.DesignDiagnostics.InputChainForbiddenAvoidanceCount >= 1
				&& !(ChainTop < BlockedPinY + ZoneHalfHeight && ChainBottom > BlockedPinY - ZoneHalfHeight);

			auto PlaceSourceLeftOfChain = [&Nodes, &Options](const int32 NodeIndex, const FRect& ChainSegment, const double SourcePinY)
			{
				Nodes[NodeIndex].NewX = SnapGraphCoordinate(ChainSegment.Left - Nodes[NodeIndex].Width - EndpointAccessClearanceX(Options) - 24.0);
				Nodes[NodeIndex].NewY = SnapGraphCoordinate(SourcePinY - GetOutputAnchor(Nodes[NodeIndex], 0).Y);
			};

			TArray<FReroutePlan> Plans;
			if (Island.InputChains.Num() > 0 && Island.InputChains[0].SegmentBounds.Num() > 0)
			{
				const FRect ChainSegment = Island.InputChains[0].SegmentBounds[0];
				PlaceSourceLeftOfChain(0, ChainSegment, ChainSegment.Top - 120.0);
				PlaceSourceLeftOfChain(3, ChainSegment, ChainSegment.CenterY());
			}
			FRunResult RerouteRun;
			TArray<FIsland> CollisionIslands;
			CollisionIslands.Add(Island);
			AppendInputChainCollisionReroutePlans(Nodes, Edges, CollisionIslands, Options, Plans, RerouteRun);
			const bool bCollisionGuardCreated =
				RerouteRun.DesignDiagnostics.InputChainCollisionRerouteCount == 1
				&& Plans.Num() == 1
				&& Plans[0].EdgeIndex == 2
				&& Plans[0].Reason == TEXT("input_chain_collision_guard")
				&& Island.InputChains.Num() > 0
				&& Island.InputChains[0].SegmentBounds.Num() > 0
				&& FMath::Abs(Plans[0].Points[0].X - Island.InputChains[0].SegmentBounds[0].Left) <= 1.0;

			TArray<FReroutePlan> ExecPlans;
			if (Island.InputChains.Num() > 0 && Island.InputChains[0].SegmentBounds.Num() > 0)
			{
				const FRect ChainSegment = Island.InputChains[0].SegmentBounds[0];
				PlaceSourceLeftOfChain(0, ChainSegment, ChainSegment.CenterY());
				PlaceSourceLeftOfChain(3, ChainSegment, ChainSegment.Top - 120.0);
			}
			FRunResult ExecRerouteRun;
			AppendInputChainCollisionReroutePlans(Nodes, Edges, CollisionIslands, Options, ExecPlans, ExecRerouteRun);
			const bool bExecCollisionGuardCreated =
				ExecRerouteRun.DesignDiagnostics.InputChainCollisionRerouteCount == 1
				&& ExecPlans.Num() == 1
				&& ExecPlans[0].EdgeIndex == 0
				&& ExecPlans[0].Reason == TEXT("input_chain_collision_guard")
				&& Island.InputChains.Num() > 0
				&& Island.InputChains[0].SegmentBounds.Num() > 0
				&& FMath::Abs(ExecPlans[0].Points[0].X - Island.InputChains[0].SegmentBounds[0].Left) <= 1.0;

			TArray<FReroutePlan> ExistingPlans;
			if (Island.InputChains.Num() > 0 && Island.InputChains[0].SegmentBounds.Num() > 0)
			{
				const FRect ChainSegment = Island.InputChains[0].SegmentBounds[0];
				PlaceSourceLeftOfChain(0, ChainSegment, ChainSegment.Top - 120.0);
				PlaceSourceLeftOfChain(3, ChainSegment, ChainSegment.CenterY());
				FReroutePlan ExistingPlan;
				ExistingPlan.EdgeIndex = 2;
				ExistingPlan.Reason = TEXT("fanout_local_bus");
				ExistingPlan.BusId = TEXT("synthetic_existing_bus");
				ExistingPlan.RouteKind = ERouteKind::LocalBus;
				ExistingPlan.Points.Add(FVector2D(ChainSegment.Left - EndpointAccessClearanceX(Options) - 24.0, ChainSegment.CenterY()));
				FinalizeReroutePlanPoints(ExistingPlan, Options);
				ExistingPlans.Add(ExistingPlan);
			}
			FRunResult ExistingPlanRerouteRun;
			AppendInputChainCollisionReroutePlans(Nodes, Edges, CollisionIslands, Options, ExistingPlans, ExistingPlanRerouteRun);
			const bool bExistingPlanSkipped =
				ExistingPlanRerouteRun.DesignDiagnostics.InputChainCollisionRerouteCount == 0
				&& ExistingPlans.Num() == 1
				&& ExistingPlans[0].Points.Num() == 1
				&& !ExistingPlans[0].Reason.Contains(TEXT("input_chain_collision_guard"));

			TArray<FReroutePlan> ProtectedPlans;
			TSet<int32> ProtectedEdges;
			ProtectedEdges.Add(2);
			FRunResult ProtectedRerouteRun;
			AppendInputChainCollisionReroutePlans(Nodes, Edges, CollisionIslands, Options, ProtectedPlans, ProtectedRerouteRun, &ProtectedEdges);
			const bool bProtectedIslandEdgeSkipped =
				ProtectedRerouteRun.DesignDiagnostics.InputChainCollisionRerouteCount == 0
				&& ProtectedPlans.Num() == 0;

			AddCase(
				TEXT("input_chain_forbidden_zone_and_collision_guard"),
				bForbiddenZoneAvoided && bOutputPinIgnoredForForbiddenZones && bExecAndExternalInputsIncludedForForbiddenZones && bCollisionGuardCreated && bExecCollisionGuardCreated && bExistingPlanSkipped && bProtectedIslandEdgeSkipped,
				FString::Printf(
					TEXT("zones=%d direct_zones=%d output_ignored=%s exec_external_inputs=%s avoid=%d guard=%d exec_guard=%d rejected=%d existing_guard=%d protected_guard=%d chain=%.1f..%.1f block=%.1f plan=%d existing_points=%d"),
					Run.DesignDiagnostics.InputChainForbiddenZoneCount,
					DirectZones.Num(),
					bOutputPinIgnoredForForbiddenZones ? TEXT("true") : TEXT("false"),
					bExecAndExternalInputsIncludedForForbiddenZones ? TEXT("true") : TEXT("false"),
					Run.DesignDiagnostics.InputChainForbiddenAvoidanceCount,
					RerouteRun.DesignDiagnostics.InputChainCollisionRerouteCount,
					ExecRerouteRun.DesignDiagnostics.InputChainCollisionRerouteCount,
					RerouteRun.DesignDiagnostics.InputChainCollisionRerouteRejectedCount,
					ExistingPlanRerouteRun.DesignDiagnostics.InputChainCollisionRerouteCount,
					ProtectedRerouteRun.DesignDiagnostics.InputChainCollisionRerouteCount,
					ChainTop,
					ChainBottom,
					BlockedPinY,
					Plans.Num(),
					ExistingPlans.Num() > 0 ? ExistingPlans[0].Points.Num() : 0));
		}

		{
			TArray<FNode> Nodes;
			Nodes.SetNum(3);
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				Nodes[Index].Index = Index;
				Nodes[Index].Guid = FString::Printf(TEXT("island_internal_multi_col_%d"), Index);
				Nodes[Index].Title = Nodes[Index].Guid;
				Nodes[Index].Width = 120.0;
				Nodes[Index].Height = 72.0;
				Nodes[Index].InputPinNames = { TEXT("in") };
				Nodes[Index].InputPinAnchors = { FVector2D(0.0, 36.0) };
				Nodes[Index].InputPinAnchorValid = { 1 };
				Nodes[Index].OutputPinNames = { TEXT("out") };
				Nodes[Index].OutputPinAnchors = { FVector2D(120.0, 36.0) };
				Nodes[Index].OutputPinAnchorValid = { 1 };
				Nodes[Index].bMeasuredGeometry = true;
				Nodes[Index].Rank = Index;
				Nodes[Index].NewX = static_cast<double>(Index) * 260.0;
				Nodes[Index].NewY = Index == 2 ? 80.0 : 0.0;
			}
			TArray<FEdge> Edges = {
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.Columns.SetNum(3);
			Island.Columns[0] = {0};
			Island.Columns[1] = {1};
			Island.Columns[2] = {2};
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			IslandByNode.Add(0, 0);
			IslandByNode.Add(1, 0);
			IslandByNode.Add(2, 0);
			TArray<FReroutePlan> Plans;
			FRunResult Run;
			AppendIslandInternalReroutePlans(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			const bool bMultiColumnInternalBus =
				Plans.Num() == 1
				&& Plans[0].EdgeIndex == 0
				&& Plans[0].RouteKind == ERouteKind::LocalBus
				&& Plans[0].Reason == TEXT("island_internal_multi_column_bus")
				&& Plans[0].Points.Num() >= 2
				&& Run.DesignDiagnostics.IslandInternalMultiColumnBusCount == 1;
			AddCase(
				TEXT("island_internal_multi_column_edge_uses_local_bus"),
				bMultiColumnInternalBus,
				FString::Printf(TEXT("plans=%d points=%d multi=%d reason=%s"),
					Plans.Num(),
					Plans.Num() > 0 ? Plans[0].Points.Num() : 0,
					Run.DesignDiagnostics.IslandInternalMultiColumnBusCount,
					Plans.Num() > 0 ? *Plans[0].Reason : TEXT("none")));
		}

		RunSourceColumnCollisionSelfTests(Result, Options);
		RunInterIslandBranchSlotSelfTests(Result, Options);
		RunInterIslandBusSlotBiasSelfTests(Result);
		RunA49IslandOffsetMergeSelfTests(Result);

		{
			FReroutePlan Plan;
			const FVector2D Start(100.0, 20.0);
			const FVector2D End(300.0, 100.0);
			Plan.Points = {
				FVector2D(160.0, 20.0),
				FVector2D(220.0, 20.0),
				FVector2D(220.0, 100.0)
			};
			CompactRoutePoints(Plan, Start, End, Options);
			const bool bFullRouteCompactionRemovesLeadingCollinearGuard =
				Plan.Points.Num() == 2
				&& FMath::IsNearlyEqual(Plan.Points[0].X, 220.0, 0.5)
				&& FMath::IsNearlyEqual(Plan.Points[0].Y, 20.0, 0.5)
				&& FMath::IsNearlyEqual(Plan.Points[1].X, 220.0, 0.5)
				&& FMath::IsNearlyEqual(Plan.Points[1].Y, 100.0, 0.5);
			AddCase(
				TEXT("reroute_full_route_compaction_removes_leading_collinear_guard"),
				bFullRouteCompactionRemovesLeadingCollinearGuard,
				FString::Printf(TEXT("points=%d first=(%.1f,%.1f) second=(%.1f,%.1f)"),
					Plan.Points.Num(),
					Plan.Points.Num() > 0 ? Plan.Points[0].X : -1.0,
					Plan.Points.Num() > 0 ? Plan.Points[0].Y : -1.0,
					Plan.Points.Num() > 1 ? Plan.Points[1].X : -1.0,
					Plan.Points.Num() > 1 ? Plan.Points[1].Y : -1.0));
		}

		{
			const FString SharedKey = MakeRerouteAttachmentPointMergeKey(77, FVector2D(320.2, 140.4));
			const FString SamePointSharedKey = MakeRerouteAttachmentPointMergeKey(77, FVector2D(320.4, 140.2));
			const FString DifferentPointSharedKey = MakeRerouteAttachmentPointMergeKey(77, FVector2D(340.0, 140.0));
			const bool bAttachmentPointMergeKeyIgnoresRoutePointIndex =
				!SharedKey.IsEmpty()
				&& SharedKey == SamePointSharedKey
				&& SharedKey != DifferentPointSharedKey;
			AddCase(
				TEXT("reroute_attachment_point_merge_key_uses_group_and_point_only"),
				bAttachmentPointMergeKeyIgnoresRoutePointIndex,
				FString::Printf(TEXT("key=%s same=%s different=%s"),
					*SharedKey,
					*SamePointSharedKey,
					*DifferentPointSharedKey));
		}

		{
			FIslandPortCandidate Candidate;
			Candidate.SourceLocal = FVector2D(431.0, 57.0);
			Candidate.TargetLocal = FVector2D(-66.0, 554.0);
			Candidate.ActualTargetLocal = FVector2D(540.0, 554.0);
			Candidate.Side = TEXT("left");
			Candidate.AllowedMinX = Candidate.TargetLocal.X;
			Candidate.AllowedMaxX = ComputeOutputXAlignedToInput(Candidate.ActualTargetLocal.X, Options);
			const double RequiredDelta = MinTargetDeltaXForCandidate(Candidate, Options);
			const double FixedLeftDelta = ComputeTargetOriginDeltaXForAlignedConnection(Candidate.SourceLocal, Candidate.TargetLocal, Options)
				+ FMath::Max(0.0, Options.NodePadding);
			const double ExpectedDelta = ComputeTargetOriginDeltaXForAlignedConnection(
				Candidate.SourceLocal,
				FVector2D(Candidate.AllowedMaxX, Candidate.TargetLocal.Y),
				Options)
				+ FMath::Max(0.0, Options.NodePadding);
			const bool bLeftEntryUsesAllowedRightLimit =
				FMath::IsNearlyEqual(RequiredDelta, ExpectedDelta, 0.5)
				&& RequiredDelta + 0.5 < FixedLeftDelta;
			AddCase(
				TEXT("left_boundary_entry_uses_allowed_max_for_forward_alignment"),
				bLeftEntryUsesAllowedRightLimit,
				FString::Printf(TEXT("required_delta=%.1f expected=%.1f fixed_delta=%.1f allowed=%.1f..%.1f clearance=%.1f"),
					RequiredDelta,
					ExpectedDelta,
					FixedLeftDelta,
					Candidate.AllowedMinX,
					Candidate.AllowedMaxX,
					EndpointAccessClearanceX(Options)));
		}

		{
			TArray<FIslandPortCandidate> Candidates;
			FIslandPortCandidate LeftCandidate;
			LeftCandidate.Id = 0;
			LeftCandidate.EdgeIndex = 0;
			LeftCandidate.SourceIsland = 0;
			LeftCandidate.TargetIsland = 1;
			LeftCandidate.Side = TEXT("left");
			LeftCandidate.SourceLocal = FVector2D(600.0, 50.0);
			LeftCandidate.TargetLocal = FVector2D(-80.0, 50.0);
			LeftCandidate.ActualTargetLocal = FVector2D(650.0, 50.0);
			LeftCandidate.AllowedMinX = -80.0;
			LeftCandidate.AllowedMaxX = -80.0;
			Candidates.Add(LeftCandidate);

			FIslandPortCandidate TopCandidate;
			TopCandidate.Id = 1;
			TopCandidate.EdgeIndex = 0;
			TopCandidate.SourceIsland = 0;
			TopCandidate.TargetIsland = 1;
			TopCandidate.Side = TEXT("top");
			TopCandidate.SourceLocal = FVector2D(600.0, 50.0);
			TopCandidate.TargetLocal = FVector2D(560.0, -100.0);
			TopCandidate.ActualTargetLocal = FVector2D(650.0, 300.0);
			TopCandidate.AllowedMinX = 0.0;
			TopCandidate.AllowedMaxX = 560.0;
			Candidates.Add(TopCandidate);

			const TSet<int32> RouteOnlyEdges;
			TArray<FPlacementProposal> Proposals;
			for (const FIslandPortCandidate& Candidate : Candidates)
			{
				Proposals.Add(MakePlacementProposal(Candidate, RouteOnlyEdges, Options));
			}
			FRunResult Run;
			const FSelectedEndpointCandidate Selected = SelectEndpointFromProposals(Candidates, Proposals, 0, Run);
			const bool bCompactTopSelected =
				Selected.EdgeIndex == 0
				&& Selected.Side == TEXT("top")
				&& Proposals[1].RequiredTargetDeltaX + 0.5 < Proposals[0].RequiredTargetDeltaX;
			AddCase(
				TEXT("placement_endpoint_selection_prefers_shorter_x_with_vertical_spread"),
				bCompactTopSelected,
				FString::Printf(
					TEXT("selected=%s left_dx=%.1f left_score=%.1f top_dx=%.1f top_score=%.1f"),
					*Selected.Side,
					Proposals[0].RequiredTargetDeltaX,
					Proposals[0].Score,
					Proposals[1].RequiredTargetDeltaX,
					Proposals[1].Score));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 5; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("stable%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 140.0 + Index * 15.0;
				Node.Height = 80.0 + (Index % 2) * 20.0;
				Node.OldX = 1200.0 - Index * 97.0;
				Node.OldY = 900.0 + Index * 173.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, Node.Height * 0.5) };
				Node.InputPinAnchors = { FVector2D(0.0, Node.Height * 0.5) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{2, 3, TEXT("out"), TEXT("in"), 0, 0, false},
				{4, 3, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult FirstRun;
			RunLayoutForSelfTest(Nodes, Edges, Options, FirstRun);
			TArray<FVector2D> FirstPositions;
			for (FNode& Node : Nodes)
			{
				FirstPositions.Add(FVector2D(Node.NewX, Node.NewY));
				Node.OldX = Node.NewX;
				Node.OldY = Node.NewY;
			}
			FRunResult SecondRun;
			RunLayoutForSelfTest(Nodes, Edges, Options, SecondRun);
			double MaxDelta = 0.0;
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				MaxDelta = FMath::Max(MaxDelta, FVector2D::Distance(FirstPositions[Index], FVector2D(Nodes[Index].NewX, Nodes[Index].NewY)));
			}
			AddCase(TEXT("layout_is_non_iterative_and_idempotent"), MaxDelta <= 1.0 && SecondRun.MetricsAfter.OverlapCount == 0, FString::Printf(TEXT("max_delta=%.3f overlap=%d"), MaxDelta, SecondRun.MetricsAfter.OverlapCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 8; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("componentstable%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 130.0 + (Index % 3) * 20.0;
				Node.Height = 76.0 + (Index % 2) * 18.0;
				Node.OldX = 800.0 - Index * 91.0;
				Node.OldY = (Index % 2 == 0 ? 1200.0 : -600.0) + Index * 137.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, Node.Height * 0.5) };
				Node.InputPinAnchors = { FVector2D(0.0, Node.Height * 0.5) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{3, 4, TEXT("out"), TEXT("in"), 0, 0, false},
				{4, 5, TEXT("out"), TEXT("in"), 0, 0, false},
				{6, 5, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult FirstRun;
			RunLayoutForSelfTest(Nodes, Edges, Options, FirstRun);
			TArray<FVector2D> FirstPositions;
			for (FNode& Node : Nodes)
			{
				FirstPositions.Add(FVector2D(Node.NewX, Node.NewY));
				Node.OldX = Node.NewX;
				Node.OldY = Node.NewY;
			}
			FRunResult SecondRun;
			RunLayoutForSelfTest(Nodes, Edges, Options, SecondRun);
			double MaxDelta = 0.0;
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				MaxDelta = FMath::Max(MaxDelta, FVector2D::Distance(FirstPositions[Index], FVector2D(Nodes[Index].NewX, Nodes[Index].NewY)));
			}
			AddCase(TEXT("disconnected_components_stack_is_idempotent"), MaxDelta <= 1.0 && SecondRun.MetricsAfter.OverlapCount == 0, FString::Printf(TEXT("components=%d max_delta=%.3f overlap=%d"), SecondRun.DesignDiagnostics.PlacementComponentCount, MaxDelta, SecondRun.MetricsAfter.OverlapCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("bus%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 120.0;
				Node.Height = 70.0;
				Node.OldX = Index * 30.0;
				Node.OldY = Index * 20.0;
				Node.bHasExecPin = Index == 1;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(120.0, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{1, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 3, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const double Gap12 = Nodes[2].NewX - Nodes[1].NewX - Nodes[1].Width;
			TArray<TArray<int32>> ProbeColumns;
			ProbeColumns.Add(TArray<int32>{1});
			ProbeColumns.Add(TArray<int32>{2});
			const double ExpectedRunOneBusGap = ComputeCompactIslandColumnGap(Nodes, ProbeColumns, 0, 1, 0.0, Options);
			FOptions CompactProbeOptions = Options;
			CompactProbeOptions.NodePadding = 16.0;
			CompactProbeOptions.OutputAccessOffsetX = 50.0;
			CompactProbeOptions.InputAccessOffsetX = 30.0;
			TArray<FNode> FormulaProbeNodes = Nodes;
			for (const int32 NodeIndex : {1, 2})
			{
				if (!FormulaProbeNodes.IsValidIndex(NodeIndex))
				{
					continue;
				}
				FormulaProbeNodes[NodeIndex].InputPinAnchors = { FVector2D(0.0, 20.0), FVector2D(0.0, 62.0) };
				FormulaProbeNodes[NodeIndex].OutputPinAnchors = { FVector2D(120.0, 20.0), FVector2D(120.0, 62.0) };
				FormulaProbeNodes[NodeIndex].InputPinAnchorValid = { 1, 1 };
				FormulaProbeNodes[NodeIndex].OutputPinAnchorValid = { 1, 1 };
			}
			const double CompactDirectGap = ComputeCompactIslandColumnGap(FormulaProbeNodes, ProbeColumns, 0, 0, 0.0, CompactProbeOptions);
			const double CompactOneBusGap = ComputeCompactIslandColumnGap(FormulaProbeNodes, ProbeColumns, 0, 1, 0.0, CompactProbeOptions);
			const double CompactTwoBusGap = ComputeCompactIslandColumnGap(FormulaProbeNodes, ProbeColumns, 0, 2, 0.0, CompactProbeOptions);
			const bool bReservedInternalBusLane = Gap12 >= ExpectedRunOneBusGap - 0.5
				&& Gap12 <= ExpectedRunOneBusGap + 0.5
				&& Gap12 < Options.HorizontalSpacing;
			const bool bCompactFormulaUsesBranchAvoidance = FMath::IsNearlyEqual(CompactDirectGap, 42.0, 0.5)
				&& FMath::IsNearlyEqual(CompactOneBusGap, 80.0, 0.5)
				&& FMath::IsNearlyEqual(CompactTwoBusGap, 80.0, 0.5);
			const bool bNonLeftInputUsesBoundary = Run.DesignDiagnostics.LeftBoundaryEntryCount >= 1
				&& Run.DesignDiagnostics.BoundaryBusRequiredCount >= 1
				&& Run.DesignDiagnostics.BoundaryBusCount >= 1
				&& Run.DesignDiagnostics.TopBoundaryEntryCount >= 1
				&& Run.DesignDiagnostics.BottomBoundaryEntryCount >= 1;
			AddCase(TEXT("internal_bus_demand_compacts_column_gap_and_entry_side"), bReservedInternalBusLane && bCompactFormulaUsesBranchAvoidance && bNonLeftInputUsesBoundary, FString::Printf(TEXT("gap=%.1f expected_run_one=%.1f formula_direct=%.1f formula_one=%.1f formula_two=%.1f left_entries=%d top=%d bottom=%d internal_bus=%d"), Gap12, ExpectedRunOneBusGap, CompactDirectGap, CompactOneBusGap, CompactTwoBusGap, Run.DesignDiagnostics.LeftBoundaryEntryCount, Run.DesignDiagnostics.TopBoundaryEntryCount, Run.DesignDiagnostics.BottomBoundaryEntryCount, Run.DesignDiagnostics.InternalBusGroupCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("segment%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 120.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(120.0, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{1, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const bool bHasBoundaryBusLeftCandidates = Run.DesignDiagnostics.BoundaryBusRequiredCount == 1
				&& Run.DesignDiagnostics.BoundaryBusCount == 1
				&& Run.DesignDiagnostics.TopBoundaryEntryCount >= 1
				&& Run.DesignDiagnostics.BottomBoundaryEntryCount >= 1;
			const double ExpectedLeftX = -Options.HorizontalSpacing * 0.22;
			int32 InvalidRangeEntryCount = 0;
			int32 NonLeftEdgeEntryCount = 0;
			int32 MovableEntryCount = 0;
			for (const TSharedPtr<FJsonValue>& Value : Run.BoundaryEntryDiagnostics)
			{
				const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Obj.IsValid())
				{
					++InvalidRangeEntryCount;
					++NonLeftEdgeEntryCount;
					continue;
				}
				const double MinX = Obj->GetNumberField(TEXT("allowed_min_x"));
				const double MaxX = Obj->GetNumberField(TEXT("allowed_max_x"));
				if (MaxX + 0.5 < MinX)
				{
					++InvalidRangeEntryCount;
				}
				if (MaxX > MinX + 0.5)
				{
					++MovableEntryCount;
				}
				const TSharedPtr<FJsonObject> LocalPoint = Obj->GetObjectField(TEXT("local_point"));
				const double LocalX = LocalPoint.IsValid() ? LocalPoint->GetNumberField(TEXT("x")) : TNumericLimits<double>::Max();
				if (!FMath::IsNearlyEqual(LocalX, ExpectedLeftX, 0.5) || !FMath::IsNearlyEqual(MinX, LocalX, 0.5))
				{
					++NonLeftEdgeEntryCount;
				}
			}
			const bool bBoundaryEntriesExposeRightLimit =
				bHasBoundaryBusLeftCandidates
				&& InvalidRangeEntryCount == 0
				&& NonLeftEdgeEntryCount == 0
				&& MovableEntryCount > 0;
			AddCase(TEXT("boundary_entries_expose_left_edge_with_right_limit"), bBoundaryEntriesExposeRightLimit, FString::Printf(TEXT("boundary_bus=%d/%d top=%d bottom=%d invalid_range=%d non_left=%d movable=%d expected_left_x=%.1f"), Run.DesignDiagnostics.BoundaryBusCount, Run.DesignDiagnostics.BoundaryBusRequiredCount, Run.DesignDiagnostics.TopBoundaryEntryCount, Run.DesignDiagnostics.BottomBoundaryEntryCount, InvalidRangeEntryCount, NonLeftEdgeEntryCount, MovableEntryCount, ExpectedLeftX));
		}

		{
			TArray<FNode> Nodes;
			Nodes.SetNum(3);
			Nodes[0].Index = 0;
			Nodes[0].Guid = TEXT("single_node_boundary_source");
			Nodes[0].Title = Nodes[0].Guid;
			Nodes[0].Width = 120.0;
			Nodes[0].Height = 70.0;
			Nodes[0].OutputPinNames = { TEXT("out") };
			Nodes[0].OutputPinAnchors = { FVector2D(120.0, 40.0) };
			Nodes[0].OutputPinAnchorValid = { 1 };
			Nodes[1].Index = 1;
			Nodes[1].Guid = TEXT("single_node_boundary_target");
			Nodes[1].Title = Nodes[1].Guid;
			Nodes[1].Width = 160.0;
			Nodes[1].Height = 80.0;
			Nodes[1].NewX = 180.0;
			Nodes[1].InputPinNames = { TEXT("in") };
			Nodes[1].InputPinAnchors = { FVector2D(20.0, 40.0) };
			Nodes[1].InputPinAnchorValid = { 1 };
			Nodes[2].Index = 2;
			Nodes[2].Guid = TEXT("single_node_boundary_blocker");
			Nodes[2].Title = Nodes[2].Guid;
			Nodes[2].Width = 80.0;
			Nodes[2].Height = 60.0;
			Nodes[2].NewX = 40.0;
			Nodes[2].NewY = 10.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			Islands.SetNum(2);
			Islands[0].Id = 0;
			Islands[0].Nodes = {0};
			Islands[0].NodeSet.Add(0);
			Islands[0].LocalBounds = BoundsForNodes(Nodes, Islands[0].Nodes);
			Islands[1].Id = 1;
			Islands[1].Nodes = {1, 2};
			Islands[1].NodeSet.Add(1);
			Islands[1].NodeSet.Add(2);
			Islands[1].LocalBounds = BoundsForNodes(Nodes, Islands[1].Nodes);
			FIslandEdge IslandEdge;
			IslandEdge.EdgeIndex = 0;
			IslandEdge.SourceIsland = 0;
			IslandEdge.TargetIsland = 1;
			IslandEdge.SourceLocal = GetSourcePort(Nodes[0], Edges[0], true);
			IslandEdge.TargetLocal = GetTargetPort(Nodes[1], Edges[0], true);
			FRunResult Run;
			TArray<FInternalBusGroup> InternalBusGroups;
			TArray<FBoundaryEntry> BoundaryEntries;
			TArray<FBoundaryBus> BoundaryBuses;
			TArray<FIslandPortCandidate> Candidates;
			TArray<FIslandEdge> IslandEdges = { IslandEdge };
			BuildInputEntryAndPortModel(Nodes, Edges, Islands, IslandEdges, Options, Run, InternalBusGroups, BoundaryEntries, BoundaryBuses, Candidates);
			int32 SingleNodeEntries = 0;
			int32 DirectLikeRightLimitEntries = 0;
			for (const FBoundaryEntry& Entry : BoundaryEntries)
			{
				if (Entry.bSingleNodeBoundaryBus)
				{
					++SingleNodeEntries;
					if (FMath::IsNearlyEqual(Entry.AllowedMaxX, IslandEdge.TargetLocal.X, 0.5))
					{
						++DirectLikeRightLimitEntries;
					}
				}
			}
			const bool bSingleNodeBoundaryBusUsesDirectLikeRightLimit =
				BoundaryBuses.Num() == 1
				&& Run.DesignDiagnostics.TopBoundaryEntryCount == 1
				&& Run.DesignDiagnostics.BottomBoundaryEntryCount == 1
				&& SingleNodeEntries == 2
				&& DirectLikeRightLimitEntries == 2;
			AddCase(TEXT("single_node_boundary_bus_uses_direct_left_right_limit"), bSingleNodeBoundaryBusUsesDirectLikeRightLimit, FString::Printf(TEXT("buses=%d top=%d bottom=%d single_node_entries=%d direct_like=%d target_x=%.1f"),
				BoundaryBuses.Num(),
				Run.DesignDiagnostics.TopBoundaryEntryCount,
				Run.DesignDiagnostics.BottomBoundaryEntryCount,
				SingleNodeEntries,
				DirectLikeRightLimitEntries,
				IslandEdge.TargetLocal.X));
		}

		{
			TArray<FIsland> Islands;
			Islands.SetNum(2);
			Islands[0].Id = 0;
			Islands[0].X = 0.0;
			Islands[0].Y = 520.0;
			Islands[0].LocalBounds = { 0.0, 0.0, 120.0, 70.0 };
			Islands[1].Id = 1;
			Islands[1].X = 320.0;
			Islands[1].Y = 100.0;
			Islands[1].LocalBounds = { 0.0, 0.0, 220.0, 120.0 };

			TArray<FIslandPortCandidate> Candidates;
			FIslandPortCandidate TopCandidate;
			TopCandidate.Id = 0;
			TopCandidate.EdgeIndex = 0;
			TopCandidate.SourceIsland = 0;
			TopCandidate.TargetIsland = 1;
			TopCandidate.BoundaryEntryId = 10;
			TopCandidate.InternalBusGroupId = 3;
			TopCandidate.Side = TEXT("top");
			TopCandidate.SourceLocal = FVector2D(120.0, 35.0);
			TopCandidate.TargetLocal = FVector2D(-66.0, -80.0);
			TopCandidate.ActualTargetLocal = FVector2D(40.0, 70.0);
			TopCandidate.AllowedMinX = TopCandidate.TargetLocal.X;
			TopCandidate.AllowedMaxX = ComputeOutputXAlignedToInput(TopCandidate.ActualTargetLocal.X, Options);
			Candidates.Add(TopCandidate);

			FIslandPortCandidate BottomCandidate = TopCandidate;
			BottomCandidate.Id = 1;
			BottomCandidate.BoundaryEntryId = 11;
			BottomCandidate.Side = TEXT("bottom");
			BottomCandidate.TargetLocal = FVector2D(-66.0, 200.0);
			BottomCandidate.AllowedMinX = BottomCandidate.TargetLocal.X;
			Candidates.Add(BottomCandidate);

			TArray<FDeterministicInterIslandBus> Buses;
			FDeterministicInterIslandBus Bus;
			Bus.Id = 0;
			Bus.FinalY = Islands[1].Y + BottomCandidate.TargetLocal.Y;
			Buses.Add(Bus);

			TArray<FDeterministicIslandConnection> Connections;
			FDeterministicIslandConnection Connection;
			Connection.bBus = true;
			Connection.BusId = 0;
			Connection.Endpoint = MakeSelectedEndpointFromCandidate(TopCandidate, Options);
			Connections.Add(Connection);

			TArray<FNode> EmptyNodes;
			TArray<FEdge> EmptyEdges;
			ResolveBusConnectionEntriesForCurrentPlacement(EmptyNodes, EmptyEdges, Candidates, Islands, Buses, Options, Connections);
			const bool bBoundaryBusSideTracksInterIslandBus =
				Connections.Num() == 1
				&& Connections[0].Endpoint.CandidateId == BottomCandidate.Id
				&& Connections[0].Endpoint.BoundaryEntryId == BottomCandidate.BoundaryEntryId
				&& Connections[0].Endpoint.Side == TEXT("bottom")
				&& FMath::IsNearlyEqual(Connections[0].Endpoint.EntryLocal.Y, BottomCandidate.TargetLocal.Y, 0.5);
			AddCase(TEXT("boundary_bus_side_tracks_inter_island_bus_side"), bBoundaryBusSideTracksInterIslandBus, FString::Printf(TEXT("side=%s candidate=%d entry_y=%.1f bus_y=%.1f"),
				*Connections[0].Endpoint.Side,
				Connections[0].Endpoint.CandidateId,
				Connections[0].Endpoint.EntryLocal.Y,
				Buses[0].FinalY));
		}

		{
			TArray<FNode> Nodes;
			Nodes.SetNum(2);
			Nodes[0].Index = 0;
			Nodes[0].Guid = TEXT("direct_left_source");
			Nodes[0].Title = Nodes[0].Guid;
			Nodes[0].Width = 120.0;
			Nodes[0].Height = 70.0;
			Nodes[0].OutputPinNames = { TEXT("out") };
			Nodes[0].OutputPinAnchors = { FVector2D(120.0, 35.0) };
			Nodes[0].OutputPinAnchorValid = { 1 };
			Nodes[1].Index = 1;
			Nodes[1].Guid = TEXT("direct_left_target");
			Nodes[1].Title = Nodes[1].Guid;
			Nodes[1].Width = 168.0;
			Nodes[1].Height = 80.0;
			Nodes[1].InputPinNames = { TEXT("in") };
			Nodes[1].InputPinAnchors = { FVector2D(6.0, 40.0) };
			Nodes[1].InputPinAnchorValid = { 1 };

			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			Islands.SetNum(2);
			Islands[0].Id = 0;
			Islands[0].Nodes = {0};
			Islands[0].NodeSet.Add(0);
			Islands[0].LocalBounds = BoundsForNodes(Nodes, Islands[0].Nodes);
			Islands[1].Id = 1;
			Islands[1].Nodes = {1};
			Islands[1].NodeSet.Add(1);
			Islands[1].LocalBounds = BoundsForNodes(Nodes, Islands[1].Nodes);
			FIslandEdge IslandEdge;
			IslandEdge.EdgeIndex = 0;
			IslandEdge.SourceIsland = 0;
			IslandEdge.TargetIsland = 1;
			IslandEdge.SourceLocal = GetSourcePort(Nodes[0], Edges[0], true);
			IslandEdge.TargetLocal = GetTargetPort(Nodes[1], Edges[0], true);
			TArray<FIslandEdge> IslandEdges = {IslandEdge};
			FRunResult Run;
			TArray<FInternalBusGroup> InternalBusGroups;
			TArray<FBoundaryEntry> BoundaryEntries;
			TArray<FBoundaryBus> BoundaryBuses;
			TArray<FIslandPortCandidate> Candidates;
			BuildInputEntryAndPortModel(Nodes, Edges, Islands, IslandEdges, Options, Run, InternalBusGroups, BoundaryEntries, BoundaryBuses, Candidates);
			Islands[0].X = -600.0;
			Islands[1].X = 0.0;
			const FVector2D ResolvedBusPathEntry = Candidates.Num() > 0
				? ResolveEntryLocalAfterPlacement(Candidates[0], Islands[0], Islands[1], Options, BusPathAccessClearanceX(Options))
				: FVector2D::ZeroVector;
			const double ExpectedLeftX = -Options.HorizontalSpacing * 0.22;
			const bool bSingleInterfaceUsesDirectLeft =
				InternalBusGroups.Num() == 1
				&& BoundaryEntries.Num() == 1
				&& BoundaryBuses.Num() == 0
				&& Candidates.Num() == 1
				&& BoundaryEntries[0].Side == TEXT("left")
				&& FMath::IsNearlyEqual(BoundaryEntries[0].LocalPoint.X, ExpectedLeftX, 0.5)
				&& FMath::IsNearlyEqual(BoundaryEntries[0].LocalPoint.Y, IslandEdge.TargetLocal.Y, 0.5)
				&& FMath::IsNearlyEqual(BoundaryEntries[0].AllowedMaxX, IslandEdge.TargetLocal.X, 0.5)
				&& FMath::IsNearlyEqual(InternalBusGroups[0].EntryLocal.X, IslandEdge.TargetLocal.X, 0.5)
				&& FMath::IsNearlyEqual(InternalBusGroups[0].EntryLocal.Y, IslandEdge.TargetLocal.Y, 0.5)
				&& FMath::IsNearlyEqual(ResolvedBusPathEntry.X, ExpectedLeftX, 0.5)
				&& FMath::IsNearlyEqual(ResolvedBusPathEntry.Y, IslandEdge.TargetLocal.Y, 0.5)
				&& Run.DesignDiagnostics.LeftBoundaryEntryCount == 1
				&& Run.DesignDiagnostics.TopBoundaryEntryCount == 0
				&& Run.DesignDiagnostics.BottomBoundaryEntryCount == 0;
			AddCase(TEXT("single_external_input_interface_uses_direct_left_when_clear"), bSingleInterfaceUsesDirectLeft, FString::Printf(TEXT("groups=%d entries=%d buses=%d candidates=%d left=%d top=%d bottom=%d entry=(%.1f,%.1f) target=(%.1f,%.1f) allowed_max=%.1f resolved=(%.1f,%.1f)"),
				InternalBusGroups.Num(),
				BoundaryEntries.Num(),
				BoundaryBuses.Num(),
				Candidates.Num(),
				Run.DesignDiagnostics.LeftBoundaryEntryCount,
				Run.DesignDiagnostics.TopBoundaryEntryCount,
				Run.DesignDiagnostics.BottomBoundaryEntryCount,
				BoundaryEntries.Num() > 0 ? BoundaryEntries[0].LocalPoint.X : 0.0,
				BoundaryEntries.Num() > 0 ? BoundaryEntries[0].LocalPoint.Y : 0.0,
				IslandEdge.TargetLocal.X,
				IslandEdge.TargetLocal.Y,
				BoundaryEntries.Num() > 0 ? BoundaryEntries[0].AllowedMaxX : 0.0,
				ResolvedBusPathEntry.X,
				ResolvedBusPathEntry.Y));
		}

		{
			TArray<FNode> Nodes;
			Nodes.SetNum(3);
			Nodes[0].Index = 0;
			Nodes[0].Guid = TEXT("same_row_merge_source");
			Nodes[0].Title = Nodes[0].Guid;
			Nodes[0].Width = 120.0;
			Nodes[0].Height = 70.0;
			Nodes[0].OutputPinNames = { TEXT("out") };
			Nodes[0].OutputPinAnchors = { FVector2D(120.0, 35.0) };
			Nodes[0].OutputPinAnchorValid = { 1 };
			Nodes[1].Index = 1;
			Nodes[1].Guid = TEXT("same_row_merge_target");
			Nodes[1].Title = Nodes[1].Guid;
			Nodes[1].Width = 160.0;
			Nodes[1].Height = 80.0;
			Nodes[1].InputPinNames = { TEXT("in") };
			Nodes[1].InputPinAnchors = { FVector2D(100.0, 35.0) };
			Nodes[1].InputPinAnchorValid = { 1 };
			Nodes[2].Index = 2;
			Nodes[2].Guid = TEXT("same_row_merge_extra");
			Nodes[2].Title = Nodes[2].Guid;
			Nodes[2].Width = 80.0;
			Nodes[2].Height = 60.0;
			Nodes[2].OldY = 220.0;
			Nodes[2].NewY = 220.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			Islands.SetNum(2);
			Islands[0].Id = 0;
			Islands[0].X = -600.0;
			Islands[0].Y = 0.0;
			Islands[0].Nodes = {0};
			Islands[0].NodeSet.Add(0);
			Islands[0].LocalBounds = BoundsForNodes(Nodes, Islands[0].Nodes);
			Islands[1].Id = 1;
			Islands[1].X = 0.0;
			Islands[1].Y = 0.0;
			Islands[1].Nodes = {1, 2};
			Islands[1].NodeSet.Add(1);
			Islands[1].NodeSet.Add(2);
			Islands[1].LocalBounds = BoundsForNodes(Nodes, Islands[1].Nodes);

			FSelectedEndpointCandidate Endpoint;
			Endpoint.EdgeIndex = 0;
			Endpoint.SourceIsland = 0;
			Endpoint.TargetIsland = 1;
			Endpoint.Side = TEXT("left");
			Endpoint.SourceLocal = FVector2D(120.0, 35.0);
			Endpoint.EntryLocal = FVector2D(96.0, 35.0);
			Endpoint.ActualTargetLocal = FVector2D(100.0, 35.0);

			FDeterministicIslandConnection Connection;
			Connection.Endpoint = Endpoint;
			Connection.bBus = true;
			Connection.BusId = 0;
			Connection.BusReason = TEXT("test");
			FDeterministicInterIslandBus Bus;
			Bus.Id = 0;
			Bus.Key = TEXT("same_row_merge_bus");
			Bus.SlotIndex = 0;
			Bus.FinalY = 65.0;
			TArray<FDeterministicInterIslandBus> TestBuses = {Bus};
			TArray<FDeterministicIslandConnection> TestConnections = {Connection};
			FRunResult RunNear;
			const TArray<FReroutePlan> NearPlans = MaterializeInterIslandBusPlans(Nodes, Edges, Islands, TestConnections, TestBuses, Options, RunNear);
			const bool bNearBranchTargetsActual =
				NearPlans.Num() == 1
				&& NearPlans[0].Points.Num() == 2
				&& FMath::IsNearlyEqual(NearPlans[0].Points.Last().X, ComputeOutputXAlignedToInput(100.0, Options), 0.5)
				&& FMath::IsNearlyEqual(NearPlans[0].Points.Last().Y, Bus.FinalY, 0.5);

			Connection.Endpoint.EntryLocal = FVector2D(94.0, 35.0);
			TestConnections[0] = Connection;
			FRunResult RunFar;
			const TArray<FReroutePlan> FarPlans = MaterializeInterIslandBusPlans(Nodes, Edges, Islands, TestConnections, TestBuses, Options, RunFar);
			const bool bFarBranchTargetsActual =
				FarPlans.Num() == 1
				&& FarPlans[0].Points.Num() == 2
				&& FMath::IsNearlyEqual(FarPlans[0].Points.Last().X, ComputeOutputXAlignedToInput(100.0, Options), 0.5)
				&& FMath::IsNearlyEqual(FarPlans[0].Points.Last().Y, Bus.FinalY, 0.5)
				&& RunFar.InterIslandBusBranchVisibleTargetDiagnostics.Num() == 1;
			AddCase(TEXT("inter_island_bus_same_row_branch_targets_actual_pin"), bNearBranchTargetsActual && bFarBranchTargetsActual, FString::Printf(TEXT("near_plans=%d near_points=%d far_plans=%d far_points=%d visible_targets=%d"),
				NearPlans.Num(),
				NearPlans.Num() > 0 ? NearPlans[0].Points.Num() : 0,
				FarPlans.Num(),
				FarPlans.Num() > 0 ? FarPlans[0].Points.Num() : 0,
				RunFar.InterIslandBusBranchVisibleTargetDiagnostics.Num()));
		}

		{
			TArray<FNode> Nodes;
			Nodes.SetNum(2);
			Nodes[0].Index = 0;
			Nodes[0].Guid = TEXT("single_node_boundary_merge_source");
			Nodes[0].Title = Nodes[0].Guid;
			Nodes[0].Width = 120.0;
			Nodes[0].Height = 70.0;
			Nodes[0].OutputPinNames = { TEXT("out") };
			Nodes[0].OutputPinAnchors = { FVector2D(120.0, 35.0) };
			Nodes[0].OutputPinAnchorValid = { 1 };
			Nodes[1].Index = 1;
			Nodes[1].Guid = TEXT("single_node_boundary_merge_target");
			Nodes[1].Title = Nodes[1].Guid;
			Nodes[1].Width = 160.0;
			Nodes[1].Height = 80.0;
			Nodes[1].InputPinNames = { TEXT("in") };
			Nodes[1].InputPinAnchors = { FVector2D(100.0, 35.0) };
			Nodes[1].InputPinAnchorValid = { 1 };
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			Islands.SetNum(2);
			Islands[0].Id = 0;
			Islands[0].X = -600.0;
			Islands[0].Y = 0.0;
			Islands[0].Nodes = {0};
			Islands[0].NodeSet.Add(0);
			Islands[0].LocalBounds = BoundsForNodes(Nodes, Islands[0].Nodes);
			Islands[1].Id = 1;
			Islands[1].X = 0.0;
			Islands[1].Y = 0.0;
			Islands[1].Nodes = {1};
			Islands[1].NodeSet.Add(1);
			Islands[1].LocalBounds = BoundsForNodes(Nodes, Islands[1].Nodes);

			FSelectedEndpointCandidate Endpoint;
			Endpoint.EdgeIndex = 0;
			Endpoint.SourceIsland = 0;
			Endpoint.TargetIsland = 1;
			Endpoint.Side = TEXT("top");
			Endpoint.SourceLocal = FVector2D(120.0, 35.0);
			Endpoint.EntryLocal = FVector2D(96.0, -80.0);
			Endpoint.ActualTargetLocal = FVector2D(100.0, 35.0);
			Endpoint.bSingleNodeBoundaryBus = true;

			FDeterministicIslandConnection Connection;
			Connection.Endpoint = Endpoint;
			Connection.bBus = true;
			Connection.BusId = 0;
			Connection.BranchSlotIndex = 0;
			Connection.BusReason = TEXT("test");
			FDeterministicInterIslandBus Bus;
			Bus.Id = 0;
			Bus.Key = TEXT("single_node_boundary_merge_bus");
			Bus.SlotIndex = 0;
			Bus.FinalY = -80.0;
			const TArray<FDeterministicInterIslandBus> TestBuses = { Bus };
			const TArray<FDeterministicIslandConnection> TestConnections = { Connection };
			FRunResult Run;
			const TArray<FReroutePlan> Plans = MaterializeInterIslandBusPlans(Nodes, Edges, Islands, TestConnections, TestBuses, Options, Run);
			bool bDiagMerged = false;
			if (Run.InterIslandBusBranchVisibleTargetDiagnostics.Num() == 1)
			{
				const TSharedPtr<FJsonObject> Obj = Run.InterIslandBusBranchVisibleTargetDiagnostics[0]->AsObject();
				bDiagMerged = Obj.IsValid() && Obj->GetBoolField(TEXT("merged_with_actual_target"));
			}
			const bool bSingleNodeBoundaryBusMergesNearXVisibleTarget =
				Plans.Num() == 1
				&& Plans[0].Points.Num() == 2
				&& bDiagMerged;
			AddCase(TEXT("single_node_boundary_bus_merges_visible_target_by_x_tolerance"), bSingleNodeBoundaryBusMergesNearXVisibleTarget, FString::Printf(TEXT("plans=%d points=%d merged=%s"),
				Plans.Num(),
				Plans.Num() > 0 ? Plans[0].Points.Num() : 0,
				bDiagMerged ? TEXT("true") : TEXT("false")));
		}

		{
			TArray<FNode> Nodes;
			Nodes.SetNum(2);
			Nodes[0].Index = 0;
			Nodes[0].Guid = TEXT("single_node_bus_source");
			Nodes[0].Title = Nodes[0].Guid;
			Nodes[0].Width = 120.0;
			Nodes[0].Height = 70.0;
			Nodes[0].OutputPinNames = { TEXT("out") };
			Nodes[0].OutputPinAnchors = { FVector2D(120.0, 35.0) };
			Nodes[0].OutputPinAnchorValid = { 1 };
			Nodes[1].Index = 1;
			Nodes[1].Guid = TEXT("single_node_bus_target");
			Nodes[1].Title = Nodes[1].Guid;
			Nodes[1].Width = 160.0;
			Nodes[1].Height = 80.0;
			Nodes[1].InputPinNames = { TEXT("in") };
			Nodes[1].InputPinAnchors = { FVector2D(120.0, 35.0) };
			Nodes[1].InputPinAnchorValid = { 1 };
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			Islands.SetNum(2);
			Islands[0].Id = 0;
			Islands[0].X = -600.0;
			Islands[0].Y = 0.0;
			Islands[0].Nodes = {0};
			Islands[0].NodeSet.Add(0);
			Islands[0].LocalBounds = BoundsForNodes(Nodes, Islands[0].Nodes);
			Islands[1].Id = 1;
			Islands[1].X = 0.0;
			Islands[1].Y = 0.0;
			Islands[1].Nodes = {1};
			Islands[1].NodeSet.Add(1);
			Islands[1].LocalBounds = BoundsForNodes(Nodes, Islands[1].Nodes);

			FSelectedEndpointCandidate Endpoint;
			Endpoint.EdgeIndex = 0;
			Endpoint.SourceIsland = 0;
			Endpoint.TargetIsland = 1;
			Endpoint.Side = TEXT("left");
			Endpoint.SourceLocal = FVector2D(120.0, 35.0);
			Endpoint.EntryLocal = FVector2D(120.0, 35.0);
			Endpoint.ActualTargetLocal = FVector2D(120.0, 35.0);

			FDeterministicIslandConnection Connection;
			Connection.Endpoint = Endpoint;
			Connection.bBus = true;
			Connection.BusId = 0;
			Connection.BranchSlotIndex = 0;
			Connection.BusReason = TEXT("test");
			FDeterministicInterIslandBus Bus;
			Bus.Id = 0;
			Bus.Key = TEXT("single_node_bus");
			Bus.SlotIndex = 0;
			Bus.FinalY = 35.0;
			const TArray<FDeterministicInterIslandBus> TestBuses = { Bus };
			const TArray<FDeterministicIslandConnection> TestConnections = { Connection };
			FRunResult Run;
			const TArray<FReroutePlan> Plans = MaterializeInterIslandBusPlans(Nodes, Edges, Islands, TestConnections, TestBuses, Options, Run);
			bool bDiagOmitted = false;
			if (Run.InterIslandBusBranchVisibleTargetDiagnostics.Num() == 1)
			{
				const TSharedPtr<FJsonObject> Obj = Run.InterIslandBusBranchVisibleTargetDiagnostics[0]->AsObject();
				bDiagOmitted = Obj.IsValid() && Obj->GetBoolField(TEXT("branch_knot_omitted"));
			}
			const bool bSingleNodeIslandOmitsBranchKnot =
				Plans.Num() == 1
				&& Plans[0].Points.Num() == 1
				&& FMath::IsNearlyEqual(Plans[0].Points[0].X, ComputeInputXAlignedToOutput(-480.0, Options), 0.5)
				&& FMath::IsNearlyEqual(Plans[0].Points[0].Y, Bus.FinalY, 0.5)
				&& Run.DesignDiagnostics.InterIslandBusBranchKnotCount == 0
				&& Run.DesignDiagnostics.InterIslandBusBranchKnotOmittedCount == 1
				&& bDiagOmitted;
			AddCase(TEXT("single_node_island_bus_branch_can_omit_external_knot"), bSingleNodeIslandOmitsBranchKnot, FString::Printf(TEXT("plans=%d points=%d knots=%d omitted=%d diag=%s"),
				Plans.Num(),
				Plans.Num() > 0 ? Plans[0].Points.Num() : 0,
				Run.DesignDiagnostics.InterIslandBusBranchKnotCount,
				Run.DesignDiagnostics.InterIslandBusBranchKnotOmittedCount,
				bDiagOmitted ? TEXT("true") : TEXT("false")));
		}

																{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 5; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("m%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 140.0 + Index * 10.0;
				Node.Height = 80.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 40.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 40.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{3, 4, TEXT("out"), TEXT("in"), 0, 0, false},
				{4, 3, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const bool bCoverageComplete = Run.DesignFeatureMissingCount == 0 && Run.DesignFeaturePartialCount == 0;
			FString IncompleteFeatures;
			for (const TSharedPtr<FJsonValue>& Value : Run.DesignFeatureCoverage)
			{
				const TSharedPtr<FJsonObject> Feature = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Feature.IsValid())
				{
					continue;
				}
				const FString Status = Feature->GetStringField(TEXT("status"));
				if (Status == TEXT("complete"))
				{
					continue;
				}
				if (!IncompleteFeatures.IsEmpty())
				{
					IncompleteFeatures += TEXT(";");
				}
				IncompleteFeatures += Feature->GetStringField(TEXT("key"));
				IncompleteFeatures += TEXT("=");
				IncompleteFeatures += Status;
				IncompleteFeatures += TEXT("(");
				IncompleteFeatures += Feature->GetStringField(TEXT("detail"));
				IncompleteFeatures += TEXT(")");
			}
			AddCase(TEXT("design_feature_coverage_is_truthful"), bCoverageComplete && Run.DesignFeatureCompleteCount == Run.DesignFeatureCount, FString::Printf(TEXT("features=%d complete=%d partial=%d missing=%d design_complete=%s route_through=%d proposals=%d route_groups=%d boundary_bus=%d/%d incomplete=%s"), Run.DesignFeatureCount, Run.DesignFeatureCompleteCount, Run.DesignFeaturePartialCount, Run.DesignFeatureMissingCount, Run.bDesignComplete ? TEXT("true") : TEXT("false"), Run.DesignDiagnostics.RouteThroughIslandCount, Run.DesignDiagnostics.PlacementProposalCount, Run.DesignDiagnostics.RouteGroupCount, Run.DesignDiagnostics.BoundaryBusCount, Run.DesignDiagnostics.BoundaryBusRequiredCount, *IncompleteFeatures));
		}

		Result.bAllPassed = Result.FailedCount == 0;
		return Result;
	}
}
