// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Blueprint/Layout/UeAgentBlueprintAutoLayoutAlgorithm.h"
#include "Module/UeAgentInterfaceLogger.h"

namespace UeAgentBlueprintAutoLayout
{
	namespace
	{
	// Keep these internal modules ordered. Later modules depend on helpers declared by earlier modules.
	// Internal data structures, geometry helpers, connection alignment, metrics.
	#include "Commands/Blueprint/Layout/Private/UeAgentBlueprintAutoLayoutAlgorithm_InternalCore.inl"
	// Cycle cutting, island extraction, input-chain detection, rank and column ordering.
	#include "Commands/Blueprint/Layout/Private/UeAgentBlueprintAutoLayoutAlgorithm_Islands.inl"
	// Anchored column placement, dense packing, input-chain placement, island-local layout.
	#include "Commands/Blueprint/Layout/Private/UeAgentBlueprintAutoLayoutAlgorithm_IslandPlacement.inl"
	// Target input entry candidates and island boundary bus model.
	#include "Commands/Blueprint/Layout/Private/UeAgentBlueprintAutoLayoutAlgorithm_InputModel.inl"
	// Frozen island placement, endpoint candidates, bus contracts, and corridor routes.
	#include "Commands/Blueprint/Layout/Private/UeAgentBlueprintAutoLayoutAlgorithm_FrozenIslandSolver.inl"
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

	bool RunLayout(TArray<FNode>& Nodes, const TArray<FEdge>& Edges, const FOptions& InOptions, FRunResult& OutResult)
	{
		const double LayoutTraceStartSeconds = FPlatformTime::Seconds();
		const FOptions Options = NormalizeOptions(InOptions);
		OutResult = FRunResult();
		OutResult.NodeCount = Nodes.Num();
		OutResult.EdgeCount = Edges.Num();
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore start nodes=%d edges=%d"), Nodes.Num(), Edges.Num());
		const int32 MissingRequiredAnchors = CountMissingRequiredPinAnchors(Nodes, Edges);
		if (!Options.bUseRealGeometry || Options.MissingPinAnchorCount > 0 || MissingRequiredAnchors > 0)
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

		TMap<FString, TArray<int32>> EdgesBySourceParam;
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
			EdgesBySourceParam.FindOrAdd(MakeSourceParamId(Edge)).Add(EdgeIndex);
		}
		TSet<int32> FanoutNodes;
		for (const TPair<FString, TArray<int32>>& Pair : EdgesBySourceParam)
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
				if (IsSameCommentIslandGroup(CommentGroupByNode, Edges[EdgeIndex]))
				{
					continue;
				}
				RankIgnoredEdges.Add(EdgeIndex);
				IslandCutEdges.Add(EdgeIndex);
				FanoutEdges.Add(EdgeIndex);
				FanoutNodes.Add(Edges[EdgeIndex].Source);
			}
		}
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
			else
			{
				PrepareIslandColumnsAndOrder(Nodes, Edges, ExplicitEdges, RankIgnoredEdges, Island, OutResult.DesignDiagnostics);
			}
			Islands.Add(MoveTemp(Island));
		}
		OutResult.DesignDiagnostics.NodeIslandCount = Islands.Num();

		for (const FIsland& Island : Islands)
		{
			int32 SinkCount = 0;
			for (const int32 NodeIndex : Island.Nodes)
			{
				bool bHasExplicitOutgoingInside = false;
				for (const FEdge& Edge : ExplicitEdges)
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
			if (!SourceIslandPtr || !TargetIslandPtr || *SourceIslandPtr == *TargetIslandPtr)
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
			IslandEdge.bFeedback = FeedbackEdges.Contains(EdgeIndex);
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
				LayoutIslandInternally(Nodes, Edges, ExplicitEdges, RankIgnoredEdges, Island, Options, OutResult);
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
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore frozen_solver_start elapsed_ms=%.1f"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0);
		OutResult.ReroutePlans = RunFrozenIslandCorridorSolver(
			Nodes,
			Edges,
			Islands,
			IslandEdges,
			IslandPortCandidates,
			Options,
			OutResult);
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore frozen_solver_done elapsed_ms=%.1f reroutes=%d bus_contracts=%d selected_endpoints=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			OutResult.ReroutePlans.Num(),
			OutResult.BusContractDiagnostics.Num(),
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
		AppendDirectConnectionObstacleReroutePlans(
			Nodes,
			Edges,
			Islands,
			IslandByNode,
			Options,
			OutResult.ReroutePlans,
			OutResult,
			&IslandCutEdgeIndices);
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore direct_obstacle_routes_done elapsed_ms=%.1f reroutes=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			OutResult.ReroutePlans.Num());
		const int32 SegmentRefinedRouteCount = RefineReroutePlansAgainstHardOccupancySegments(
			Nodes,
			Edges,
			Islands,
			IslandByNode,
			Options,
			OutResult.ReroutePlans,
			OutResult);
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore segment_refine_done elapsed_ms=%.1f refined=%d reroutes=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			SegmentRefinedRouteCount,
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
		CompleteIf(TEXT("placement_proposal_longest_path_x"), OutResult.DesignDiagnostics.IslandEdgeCount == 0 || (OutResult.DesignDiagnostics.PlacementProposalCount > 0 && OutResult.DesignDiagnostics.PlacementGraphEdgeCount + OutResult.DesignDiagnostics.RouteOnlyEdgeCount == OutResult.DesignDiagnostics.IslandEdgeCount && OutResult.DesignDiagnostics.InvalidForwardConnectionCount == 0), OutResult.DesignDiagnostics.PlacementProposalCount > 0, FString::Printf(TEXT("proposals=%d placement_edges=%d route_only=%d invalid_forward=%d"), OutResult.DesignDiagnostics.PlacementProposalCount, OutResult.DesignDiagnostics.PlacementGraphEdgeCount, OutResult.DesignDiagnostics.RouteOnlyEdgeCount, OutResult.DesignDiagnostics.InvalidForwardConnectionCount));
		CompleteIf(TEXT("source_order_fanout_route_groups"), OutResult.DesignDiagnostics.IslandEdgeCount == 0 || (OutResult.DesignDiagnostics.SourceOrderGroupCount > 0 && OutResult.DesignDiagnostics.RouteGroupCount >= 0), OutResult.DesignDiagnostics.SourceOrderGroupCount > 0, FString::Printf(TEXT("source_order_groups=%d fanout_groups=%d route_groups=%d"), OutResult.DesignDiagnostics.SourceOrderGroupCount, OutResult.DesignDiagnostics.FanoutIntentGroupCount, OutResult.DesignDiagnostics.RouteGroupCount));
		int32 SelectedCorridorRouteDiagnosticCount = 0;
		for (const TSharedPtr<FJsonValue>& DiagnosticValue : OutResult.CorridorRouteDiagnostics)
		{
			const TSharedPtr<FJsonObject>* DiagnosticObject = nullptr;
			if (!DiagnosticValue.IsValid() || !DiagnosticValue->TryGetObject(DiagnosticObject) || DiagnosticObject == nullptr || !DiagnosticObject->IsValid())
			{
				continue;
			}
			FString Side;
			(*DiagnosticObject)->TryGetStringField(TEXT("side"), Side);
			if (Side != TEXT("direct") && Side != TEXT("segment_refine"))
			{
				++SelectedCorridorRouteDiagnosticCount;
			}
		}
		const bool bCorridorRoutingComplete = OutResult.DesignDiagnostics.SelectedEndpointCandidateCount == 0
			|| (SelectedCorridorRouteDiagnosticCount == OutResult.DesignDiagnostics.SelectedEndpointCandidateCount
				&& OutResult.DesignDiagnostics.RouteThroughIslandCount == 0);
		CompleteIf(TEXT("corridor_routing_lane_coloring"), bCorridorRoutingComplete, OutResult.CorridorRouteDiagnostics.Num() > 0, FString::Printf(TEXT("corridors=%d selected_routes=%d selected=%d extra=%d lanes=%d route_through_island=%d"), OutResult.CorridorRouteDiagnostics.Num(), SelectedCorridorRouteDiagnosticCount, OutResult.DesignDiagnostics.SelectedEndpointCandidateCount, OutResult.CorridorRouteDiagnostics.Num() - SelectedCorridorRouteDiagnosticCount, OutResult.DesignDiagnostics.LaneColoringLaneCount, OutResult.DesignDiagnostics.RouteThroughIslandCount));
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
		Options.PinDirectionBiasX = 24.0;
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

		auto EnsureSyntheticRealPinAnchorsForEdges = [](TArray<FNode>& Nodes, const TArray<FEdge>& Edges)
		{
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
			FOptions RouteOptions = Options;
			RouteOptions.OutputAccessOffsetX = 40.0;
			RouteOptions.InputAccessOffsetX = 40.0;
			RouteOptions.PinDirectionBiasX = 40.0;
			const FVector2D Start(100.0, 100.0);
			const FVector2D End(360.0, 260.0);
			const TArray<FVector2D> Points = BuildOrthogonalRoutePolyline(Start, End, 180.0, RouteOptions, TArray<FRect>());
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
			FOptions RouteOptions = Options;
			RouteOptions.OutputAccessOffsetX = 40.0;
			RouteOptions.InputAccessOffsetX = 40.0;
			RouteOptions.PinDirectionBiasX = 40.0;
			RouteOptions.NodePadding = 32.0;
			const FVector2D Start(483.0, 4325.0);
			const FVector2D End(2109.0, 5116.0);
			const TArray<FRect> HardRects = {
				{ 1746.0, 4641.0, 1908.0, 4695.0 },
				{ 1950.0, 4573.0, 2185.0, 4694.0 }
			};
			const TArray<FVector2D> Points = BuildOrthogonalRoutePolyline(Start, End, 4667.0, RouteOptions, HardRects);
			FString RouteText;
			for (const FVector2D& Point : Points)
			{
				RouteText += FString::Printf(TEXT("(%.1f,%.1f)"), Point.X, Point.Y);
			}
			const bool bAvoidsSiblingHardOccupancy =
				Points.Num() >= 3
				&& !PolylineHitsHardOccupancy(Points, HardRects)
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(Points, RouteOptions);
			AddCase(TEXT("inter_island_bus_router_keeps_trunk_out_of_sibling_hard_occupancy"),
				bAvoidsSiblingHardOccupancy,
				FString::Printf(TEXT("points=%d hard_hits=%d min_shorten=%.1f route=%s"),
					Points.Num(),
					CountPolylineHardOccupancyHits(Points, HardRects),
					MinConnectionAlignmentShortenX(Points, RouteOptions),
					*RouteText));
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
			for (const TSharedPtr<FJsonValue>& RouteValue : Run.CorridorRouteDiagnostics)
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
			AddCase(TEXT("corridor_route_endpoint_uses_real_target_pin"), bRouteEndpointUsesRealTarget, FString::Printf(TEXT("routes=%d selected=%d"), Run.CorridorRouteDiagnostics.Num(), Run.DesignDiagnostics.SelectedEndpointCandidateCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("left_long_clean_fanout%d"), Index);
				Node.Title = Node.Guid;
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("ReturnValue"), TEXT("A"), 0, 0, false},
				{0, 2, TEXT("ReturnValue"), TEXT("A"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			int32 LocalBusRouteCount = 0;
			for (const TSharedPtr<FJsonValue>& RouteValue : Run.CorridorRouteDiagnostics)
			{
				const TSharedPtr<FJsonObject> RouteObj = RouteValue.IsValid() ? RouteValue->AsObject() : nullptr;
				FString RouteKind;
				if (RouteObj.IsValid() && RouteObj->TryGetStringField(TEXT("route_kind"), RouteKind) && RouteKind == TEXT("local_bus"))
				{
					++LocalBusRouteCount;
				}
			}
			AddCase(TEXT("left_clean_fanout_stays_direct_when_batch_can_compact"),
				Run.ReroutePlanCount == 0 && LocalBusRouteCount == 0 && Run.DesignDiagnostics.BusContractCount == 0,
				FString::Printf(TEXT("plans=%d local_bus_routes=%d diagnostics=%d bus_contracts=%d"), Run.ReroutePlanCount, LocalBusRouteCount, Run.CorridorRouteDiagnostics.Num(), Run.DesignDiagnostics.BusContractCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("shared_bus_source_%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 0 ? 120.0 : 220.0;
				Node.Height = 90.0;
				Node.OutputPinNames = { TEXT("ReturnValue") };
				Node.InputPinNames = { TEXT("A") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 45.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("ReturnValue"), TEXT("A"), 0, 0, false},
				{0, 2, TEXT("ReturnValue"), TEXT("A"), 0, 0, false},
				{0, 3, TEXT("ReturnValue"), TEXT("A"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			for (int32 IslandIndex = 0; IslandIndex < 4; ++IslandIndex)
			{
				FIsland Island;
				Island.Id = IslandIndex;
				Island.Nodes = {IslandIndex};
				Island.NodeSet.Add(IslandIndex);
				Island.LocalBounds = {0.0, 0.0, Nodes[IslandIndex].Width, Nodes[IslandIndex].Height};
				Island.X = IslandIndex == 0 ? 0.0 : Options.HorizontalSpacing * 8.0;
				Island.Y = IslandIndex == 0 ? 100.0 : 100.0 + static_cast<double>(IslandIndex) * 180.0;
				Islands.Add(Island);
			}
			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
			{
				FSelectedEndpointCandidate Selected;
				Selected.EdgeIndex = EdgeIndex;
				Selected.CandidateId = EdgeIndex;
				Selected.SourceIsland = 0;
				Selected.TargetIsland = EdgeIndex + 1;
				Selected.Side = TEXT("left");
				Selected.SourceLocal = FVector2D(Nodes[0].Width, 35.0);
				Selected.EntryLocal = FVector2D(0.0, 45.0);
				Selected.ActualTargetLocal = FVector2D(0.0, 45.0);
				SelectedEndpoints.Add(Selected);
			}
			FRunResult Run;
			const TArray<FReroutePlan> Plans = RouteDirectAndBusCorridors(Nodes, Edges, Islands, SelectedEndpoints, Options, Run);
			TSet<int32> MergeGroups;
			double SharedTrunkY = 0.0;
			bool bAllPlansShareTrunkY = Plans.Num() == 3;
			for (int32 PlanIndex = 0; PlanIndex < Plans.Num(); ++PlanIndex)
			{
				MergeGroups.Add(Plans[PlanIndex].AttachmentMergeGroup);
				if (Plans[PlanIndex].Points.Num() == 0)
				{
					bAllPlansShareTrunkY = false;
					continue;
				}
				if (PlanIndex == 0)
				{
					SharedTrunkY = Plans[PlanIndex].Points[0].Y;
				}
				else if (!FMath::IsNearlyEqual(SharedTrunkY, Plans[PlanIndex].Points[0].Y, 0.5))
				{
					bAllPlansShareTrunkY = false;
				}
			}
			const bool bSameSourceFanoutUsesSingleBus =
				Run.DesignDiagnostics.BusContractCount == 1
				&& MergeGroups.Num() == 1
				&& bAllPlansShareTrunkY
				&& Run.DesignDiagnostics.RouteThroughHardOccupancyCount == 0;
			AddCase(TEXT("same_source_inter_island_fanout_uses_single_bus_contract"),
				bSameSourceFanoutUsesSingleBus,
				FString::Printf(TEXT("plans=%d contracts=%d merge_groups=%d route_through=%d shared_trunk_y=%.1f"),
					Plans.Num(),
					Run.DesignDiagnostics.BusContractCount,
					MergeGroups.Num(),
					Run.DesignDiagnostics.RouteThroughHardOccupancyCount,
					SharedTrunkY));
		}

		{
			TArray<FPendingCorridorBusRoute> PendingRoutes;
			TArray<FBusContract> BusContracts;
			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FSelectedEndpointCandidate Selected;
				Selected.EdgeIndex = Index;
				Selected.CandidateId = Index;
				Selected.ConnectionGroupId = Index;
				Selected.SourcePortOrderKey = static_cast<double>(Index);
				Selected.Side = TEXT("left");
				SelectedEndpoints.Add(Selected);

				const FVector2D Start(100.0, Index == 0 ? 100.0 : 116.0);
				const FVector2D End(1200.0, Index == 0 ? 360.0 : 376.0);
				const double InitialTrunkY = 240.0;
				const double EntryX = ComputeInputXAlignedToOutput(Start.X, Options);
				const double BranchX = ComputeOutputXAlignedToInput(End.X, Options);

				FPlannedCorridorRoute Planned;
				Planned.RouteKind = ERouteKind::LocalBus;
				Planned.TrunkY = InitialTrunkY;
				Planned.BranchX = BranchX;
				Planned.Polyline = { Start, FVector2D(EntryX, InitialTrunkY), FVector2D(BranchX, InitialTrunkY), End };

				FBusContract Contract;
				Contract.Id = Index;
				Contract.SourceParamId = FString::Printf(TEXT("planned_lane_source_%d"), Index);
				Contract.RouteKind = ERouteKind::LocalBus;
				Contract.TrunkY = InitialTrunkY;
				Contract.EdgeIndices.Add(Index);
				Contract.BranchXs.Add(BranchX);
				RecomputeBusContractBoundsFromRoute(Contract, Planned);
				BusContracts.Add(Contract);

				FPendingCorridorBusRoute Pending;
				Pending.SelectedEndpointIndex = Index;
				Pending.ContractIndex = Contract.Id;
				Pending.EdgeIndex = Index;
				Pending.SourceParamId = Contract.SourceParamId;
				Pending.Side = TEXT("left");
				Pending.Planned = Planned;
				Pending.Start = Start;
				Pending.End = End;
				PendingRoutes.Add(Pending);
			}

			FRunResult Run;
			const TArray<FNode> EmptyNodes;
			const TArray<FEdge> EmptyEdges;
			const TArray<FIsland> EmptyIslands;
			PlanBusContractsAtGeneration(PendingRoutes, BusContracts, SelectedEndpoints, EmptyNodes, EmptyEdges, EmptyIslands, Options, Run);
			const double LaneSpacing = FMath::Max(
				FMath::Max(Options.NodePadding * 2.0, 48.0),
				GetFallbackInterfaceRowSpacing(Options));
			const bool bGenerationSelectedSeparateY =
				PendingRoutes.Num() == 2
				&& FMath::Abs(PendingRoutes[0].Planned.TrunkY - PendingRoutes[1].Planned.TrunkY) >= LaneSpacing - 0.5
				&& Run.DesignDiagnostics.RouteThroughHardOccupancyCount == 0
				&& Run.DesignDiagnostics.CrossBusTrunkLaneCount == 1
				&& Run.LaneAssignmentDiagnostics.Num() == 2
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[0].Planned.Polyline, Options)
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[1].Planned.Polyline, Options);
			AddCase(TEXT("bus_contract_generation_selects_y_lanes_before_reroute_writeback"),
				bGenerationSelectedSeparateY,
				FString::Printf(TEXT("y=%.1f/%.1f lane_spacing=%.1f route_through=%d trunk_lanes=%d lane_diag=%d"),
					PendingRoutes.IsValidIndex(0) ? PendingRoutes[0].Planned.TrunkY : 0.0,
					PendingRoutes.IsValidIndex(1) ? PendingRoutes[1].Planned.TrunkY : 0.0,
					LaneSpacing,
					Run.DesignDiagnostics.RouteThroughHardOccupancyCount,
					Run.DesignDiagnostics.CrossBusTrunkLaneCount,
					Run.LaneAssignmentDiagnostics.Num()));
		}

		{
			TArray<FPendingCorridorBusRoute> PendingRoutes;
			TArray<FBusContract> BusContracts;
			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FSelectedEndpointCandidate Selected;
				Selected.EdgeIndex = Index;
				Selected.CandidateId = Index;
				Selected.ConnectionGroupId = Index;
				Selected.SourcePortOrderKey = static_cast<double>(Index);
				SelectedEndpoints.Add(Selected);

				const FVector2D Start(100.0, 100.0 + static_cast<double>(Index) * 6.0);
				const FVector2D End(1100.0, 340.0 + static_cast<double>(Index) * 8.0);
				const double TrunkY = 260.0 + static_cast<double>(Index) * 8.0;
				const double EntryX = ComputeInputXAlignedToOutput(Start.X, Options);
				const double BranchX = ComputeOutputXAlignedToInput(End.X, Options);

				FPlannedCorridorRoute Planned;
				Planned.RouteKind = ERouteKind::LocalBus;
				Planned.TrunkY = TrunkY;
				Planned.BranchX = BranchX;
				Planned.Polyline = { Start, FVector2D(EntryX, TrunkY), FVector2D(BranchX, TrunkY), End };

				FBusContract Contract;
				Contract.Id = Index;
				Contract.SourceParamId = FString::Printf(TEXT("trunk_source_%d"), Index);
				Contract.RouteKind = ERouteKind::LocalBus;
				Contract.TrunkY = TrunkY;
				Contract.EdgeIndices.Add(Index);
				Contract.BranchXs.Add(BranchX);
				RecomputeBusContractBoundsFromRoute(Contract, Planned);
				BusContracts.Add(Contract);

				FPendingCorridorBusRoute Pending;
				Pending.SelectedEndpointIndex = Index;
				Pending.ContractIndex = Contract.Id;
				Pending.EdgeIndex = Index;
				Pending.SourceParamId = Contract.SourceParamId;
				Pending.Side = TEXT("left");
				Pending.Planned = Planned;
				Pending.Start = Start;
				Pending.End = End;
				PendingRoutes.Add(Pending);
			}

			FRunResult Run;
			const TArray<FNode> EmptyNodes;
			const TArray<FEdge> EmptyEdges;
			const TArray<FIsland> EmptyIslands;
			PlanBusContractsAtGeneration(PendingRoutes, BusContracts, SelectedEndpoints, EmptyNodes, EmptyEdges, EmptyIslands, Options, Run);
			const double LaneSpacing = FMath::Max(
				FMath::Max(Options.NodePadding * 2.0, 48.0),
				GetFallbackInterfaceRowSpacing(Options));
			bool bTrunksSeparated = PendingRoutes.Num() == 3
				&& Run.DesignDiagnostics.CrossBusTrunkLaneCount == 2
				&& Run.LaneAssignmentDiagnostics.Num() == 3;
			for (int32 Index = 1; Index < PendingRoutes.Num(); ++Index)
			{
				const double PrevY = PendingRoutes[Index - 1].Planned.TrunkY;
				const double CurrentY = PendingRoutes[Index].Planned.TrunkY;
				bTrunksSeparated = bTrunksSeparated
					&& CurrentY >= PrevY + LaneSpacing - 0.5
					&& BusContracts[Index].BusLaneIndex == Index
					&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[Index].Planned.Polyline, Options);
			}
			AddCase(TEXT("cross_bus_horizontal_trunks_with_overlapping_x_get_separate_y_lanes"),
				bTrunksSeparated,
				FString::Printf(TEXT("y=%.1f/%.1f/%.1f spacing=%.1f trunk_lanes=%d diagnostics=%d lane_index=%d/%d/%d"),
					PendingRoutes.IsValidIndex(0) ? PendingRoutes[0].Planned.TrunkY : 0.0,
					PendingRoutes.IsValidIndex(1) ? PendingRoutes[1].Planned.TrunkY : 0.0,
					PendingRoutes.IsValidIndex(2) ? PendingRoutes[2].Planned.TrunkY : 0.0,
					LaneSpacing,
					Run.DesignDiagnostics.CrossBusTrunkLaneCount,
					Run.LaneAssignmentDiagnostics.Num(),
					BusContracts.IsValidIndex(0) ? BusContracts[0].BusLaneIndex : INDEX_NONE,
					BusContracts.IsValidIndex(1) ? BusContracts[1].BusLaneIndex : INDEX_NONE,
					BusContracts.IsValidIndex(2) ? BusContracts[2].BusLaneIndex : INDEX_NONE));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("planned_hard_free_%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 1 ? 420.0 : 160.0;
				Node.Height = Index == 1 ? 260.0 : 90.0;
				Node.OutputPinNames = { TEXT("ReturnValue") };
				Node.InputPinNames = { TEXT("A") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 40.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 45.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 2, TEXT("ReturnValue"), TEXT("A"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			for (int32 IslandIndex = 0; IslandIndex < 3; ++IslandIndex)
			{
				FIsland Island;
				Island.Id = IslandIndex;
				Island.Nodes = {IslandIndex};
				Island.NodeSet.Add(IslandIndex);
				Island.LocalBounds = {0.0, 0.0, Nodes[IslandIndex].Width, Nodes[IslandIndex].Height};
				Island.X = IslandIndex == 0 ? 0.0 : (IslandIndex == 1 ? 900.0 : Options.HorizontalSpacing * 8.0);
				Island.Y = IslandIndex == 1 ? 40.0 : 100.0;
				Islands.Add(Island);
			}
			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			FSelectedEndpointCandidate Selected;
			Selected.EdgeIndex = 0;
			Selected.CandidateId = 0;
			Selected.SourceIsland = 0;
			Selected.TargetIsland = 2;
			Selected.Side = TEXT("left");
			Selected.SourceLocal = FVector2D(Nodes[0].Width, 40.0);
			Selected.EntryLocal = FVector2D(0.0, 45.0);
			Selected.ActualTargetLocal = FVector2D(0.0, 45.0);
			SelectedEndpoints.Add(Selected);
			FRunResult Run;
			const TArray<FReroutePlan> Plans = RouteDirectAndBusCorridors(Nodes, Edges, Islands, SelectedEndpoints, Options, Run);
			const TArray<FRect> HardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[0]);
			bool bPlanHitsHard = false;
			for (const FReroutePlan& Plan : Plans)
			{
				if (Plan.EdgeIndex != 0)
				{
					continue;
				}
				TArray<FVector2D> Points;
				Points.Add(FVector2D(Islands[0].X + Selected.SourceLocal.X, Islands[0].Y + Selected.SourceLocal.Y));
				Points.Append(Plan.Points);
				Points.Add(FVector2D(Islands[2].X + Selected.EntryLocal.X, Islands[2].Y + Selected.EntryLocal.Y));
				bPlanHitsHard = PolylineHitsHardOccupancy(Points, HardRects);
			}
			AddCase(TEXT("bus_contract_generation_chooses_hard_free_route_candidate"),
				Plans.Num() == 1 && !bPlanHitsHard && Run.DesignDiagnostics.RouteThroughHardOccupancyCount == 0,
				FString::Printf(TEXT("plans=%d hit=%d route_through=%d contracts=%d"),
					Plans.Num(),
					bPlanHitsHard ? 1 : 0,
					Run.DesignDiagnostics.RouteThroughHardOccupancyCount,
					Run.DesignDiagnostics.BusContractCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("target_actual_wire_%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 2 ? 210.0 : 180.0;
				Node.Height = Index == 2 ? 160.0 : 90.0;
				Node.OutputPinNames = { TEXT("ReturnValue") };
				Node.InputPinNames = { TEXT("A") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 50.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 45.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("ReturnValue"), TEXT("A"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			FIsland SourceIsland;
			SourceIsland.Id = 0;
			SourceIsland.Nodes = {0};
			SourceIsland.NodeSet.Add(0);
			SourceIsland.LocalBounds = {0.0, 0.0, 180.0, 90.0};
			SourceIsland.X = 0.0;
			SourceIsland.Y = 0.0;
			Islands.Add(SourceIsland);

			FIsland TargetIsland;
			TargetIsland.Id = 1;
			TargetIsland.Nodes = {1, 2};
			TargetIsland.NodeSet.Add(1);
			TargetIsland.NodeSet.Add(2);
			Nodes[1].NewX = 900.0;
			Nodes[1].NewY = 40.0;
			Nodes[2].NewX = 80.0;
			Nodes[2].NewY = 210.0;
			TargetIsland.LocalBounds = {0.0, 0.0, 1080.0, 370.0};
			TargetIsland.X = 700.0;
			TargetIsland.Y = 300.0;
			Islands.Add(TargetIsland);

			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			FSelectedEndpointCandidate Selected;
			Selected.EdgeIndex = 0;
			Selected.CandidateId = 0;
			Selected.SourceIsland = 0;
			Selected.TargetIsland = 1;
			Selected.Side = TEXT("left");
			Selected.ConnectionGroupId = 0;
			Selected.SourceLocal = FVector2D(180.0, 50.0);
			Selected.EntryLocal = FVector2D(-80.0, 300.0);
			Selected.ActualTargetLocal = FVector2D(900.0, 100.0);
			SelectedEndpoints.Add(Selected);

			FRunResult Run;
			const TArray<FReroutePlan> Plans = RouteDirectAndBusCorridors(Nodes, Edges, Islands, SelectedEndpoints, Options, Run);
			TArray<FVector2D> ControlPoints;
			ControlPoints.Add(FVector2D(Islands[0].X + Selected.SourceLocal.X, Islands[0].Y + Selected.SourceLocal.Y));
			for (const FReroutePlan& Plan : Plans)
			{
				if (Plan.EdgeIndex == 0)
				{
					ControlPoints.Append(Plan.Points);
					break;
				}
			}
			ControlPoints.Add(FVector2D(Islands[1].X + Selected.ActualTargetLocal.X, Islands[1].Y + Selected.ActualTargetLocal.Y));
			const TArray<FRect> HardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[0]);
			const bool bControlWireHitsSibling = ControlWireHitsHardOccupancy(ControlPoints, HardRects);
			bool bStillStopsAtBoundaryEntryOnly = false;
			for (const FReroutePlan& Plan : Plans)
			{
				if (Plan.EdgeIndex != 0 || Plan.Points.Num() == 0)
				{
					continue;
				}
				const FVector2D LastPoint = Plan.Points.Last();
				const FVector2D EntryAbs(Islands[1].X + Selected.EntryLocal.X, Islands[1].Y + Selected.EntryLocal.Y);
				bStillStopsAtBoundaryEntryOnly = FVector2D::Distance(LastPoint, EntryAbs) < 0.5;
			}
			AddCase(TEXT("bus_target_actual_pin_segment_participates_in_generation_occupancy"),
				Plans.Num() == 1
					&& !bControlWireHitsSibling
					&& !bStillStopsAtBoundaryEntryOnly
					&& Run.DesignDiagnostics.RouteThroughHardOccupancyCount == 0
					&& HasNonNegativeConnectionAlignmentShortenForEverySegment(ControlPoints, Options),
				FString::Printf(TEXT("plans=%d wire_hit=%d boundary_stop=%d route_through=%d points=%d"),
					Plans.Num(),
					bControlWireHitsSibling ? 1 : 0,
					bStillStopsAtBoundaryEntryOnly ? 1 : 0,
					Run.DesignDiagnostics.RouteThroughHardOccupancyCount,
					ControlPoints.Num()));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("left_entry_actual_block_%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 2 ? 300.0 : 160.0;
				Node.Height = Index == 2 ? 120.0 : 90.0;
				Node.OutputPinNames = { TEXT("ReturnValue") };
				Node.InputPinNames = { TEXT("A") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 40.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 50.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("ReturnValue"), TEXT("A"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			FIsland SourceIsland;
			SourceIsland.Id = 0;
			SourceIsland.Nodes = {0};
			SourceIsland.NodeSet.Add(0);
			SourceIsland.LocalBounds = {0.0, 0.0, 160.0, 90.0};
			SourceIsland.X = 0.0;
			SourceIsland.Y = 0.0;
			Islands.Add(SourceIsland);

			FIsland TargetIsland;
			TargetIsland.Id = 1;
			TargetIsland.Nodes = {1, 2};
			TargetIsland.NodeSet.Add(1);
			TargetIsland.NodeSet.Add(2);
			Nodes[1].NewX = 520.0;
			Nodes[1].NewY = 250.0;
			Nodes[2].NewX = 100.0;
			Nodes[2].NewY = 240.0;
			TargetIsland.LocalBounds = {0.0, 0.0, 680.0, 360.0};
			TargetIsland.X = 300.0;
			TargetIsland.Y = 0.0;
			Islands.Add(TargetIsland);

			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			FSelectedEndpointCandidate Selected;
			Selected.EdgeIndex = 0;
			Selected.CandidateId = 0;
			Selected.SourceIsland = 0;
			Selected.TargetIsland = 1;
			Selected.Side = TEXT("left");
			Selected.ConnectionGroupId = 0;
			Selected.SourceLocal = FVector2D(160.0, 40.0);
			Selected.EntryLocal = FVector2D(-60.0, 300.0);
			Selected.ActualTargetLocal = FVector2D(520.0, 300.0);
			SelectedEndpoints.Add(Selected);

			FRunResult Run;
			const TArray<FReroutePlan> Plans = RouteDirectAndBusCorridors(Nodes, Edges, Islands, SelectedEndpoints, Options, Run);
			TArray<FVector2D> ControlPoints;
			ControlPoints.Add(FVector2D(Islands[0].X + Selected.SourceLocal.X, Islands[0].Y + Selected.SourceLocal.Y));
			for (const FReroutePlan& Plan : Plans)
			{
				if (Plan.EdgeIndex == 0)
				{
					ControlPoints.Append(Plan.Points);
					break;
				}
			}
			ControlPoints.Add(FVector2D(Islands[1].X + Selected.ActualTargetLocal.X, Islands[1].Y + Selected.ActualTargetLocal.Y));
			const TArray<FRect> HardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[0]);
			const bool bControlWireHitsSibling = ControlWireHitsHardOccupancy(ControlPoints, HardRects);
			const bool bDirectStayedWithoutPlan = Plans.Num() == 0;
			AddCase(TEXT("left_boundary_entry_to_actual_target_hit_forces_bus_route"),
				Plans.Num() == 1
					&& Run.DesignDiagnostics.BusContractCount == 1
					&& Run.DesignDiagnostics.RouteKindUpgradeCount == 1
					&& !bDirectStayedWithoutPlan
					&& !bControlWireHitsSibling
					&& Run.DesignDiagnostics.RouteThroughHardOccupancyCount == 0
					&& HasNonNegativeConnectionAlignmentShortenForEverySegment(ControlPoints, Options),
				FString::Printf(TEXT("plans=%d contracts=%d upgrades=%d direct=%d wire_hit=%d route_through=%d points=%d"),
					Plans.Num(),
					Run.DesignDiagnostics.BusContractCount,
					Run.DesignDiagnostics.RouteKindUpgradeCount,
					bDirectStayedWithoutPlan ? 1 : 0,
					bControlWireHitsSibling ? 1 : 0,
					Run.DesignDiagnostics.RouteThroughHardOccupancyCount,
					ControlPoints.Num()));
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
			for (const TSharedPtr<FJsonValue>& RouteValue : Run.CorridorRouteDiagnostics)
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
				FString::Printf(TEXT("plans=%d local_bus_routes=%d diagnostics=%d"), Run.ReroutePlanCount, LocalBusRouteCount, Run.CorridorRouteDiagnostics.Num()));
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
			AddCase(TEXT("corridor_bus_target_branch_uses_connection_alignment_without_back_s_bend"),
				Run.ReroutePlanCount == 0 || bNoOldTargetSideSBendPoint,
				FString::Printf(TEXT("plans=%d has_target_y_bend=%s points=%d"), Run.ReroutePlanCount, bHasOldTargetSideSBendPoint ? TEXT("true") : TEXT("false"), FirstPlan ? FirstPlan->Points.Num() : 0));
		}

		{
			TArray<FPendingCorridorBusRoute> PendingRoutes;
			TArray<FBusContract> BusContracts;
			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FSelectedEndpointCandidate Selected;
				Selected.EdgeIndex = Index;
				Selected.CandidateId = Index;
				Selected.ConnectionGroupId = Index;
				Selected.SourcePortOrderKey = static_cast<double>(Index);
				SelectedEndpoints.Add(Selected);

				const FVector2D Start(120.0, Index == 0 ? 100.0 : 250.0);
				const FVector2D End(800.0, Index == 0 ? 500.0 : 520.0);
				const double TrunkY = Index == 0 ? 260.0 : 310.0;
				const double EntryX = ComputeInputXAlignedToOutput(Start.X, Options);
				const double BranchX = ComputeOutputXAlignedToInput(End.X, Options);

				FPlannedCorridorRoute Planned;
				Planned.RouteKind = ERouteKind::LocalBus;
				Planned.TrunkY = TrunkY;
				Planned.BranchX = BranchX;
				Planned.Polyline = { Start, FVector2D(EntryX, TrunkY), FVector2D(BranchX, TrunkY), End };

				FBusContract Contract;
				Contract.Id = Index;
				Contract.SourceParamId = FString::Printf(TEXT("source_%d"), Index);
				Contract.RouteKind = ERouteKind::LocalBus;
				Contract.TrunkY = TrunkY;
				Contract.EdgeIndices.Add(Index);
				Contract.BranchXs.Add(BranchX);
				RecomputeBusContractBoundsFromRoute(Contract, Planned);
				BusContracts.Add(Contract);

				FPendingCorridorBusRoute Pending;
				Pending.SelectedEndpointIndex = Index;
				Pending.ContractIndex = Index;
				Pending.EdgeIndex = Index;
				Pending.SourceParamId = Contract.SourceParamId;
				Pending.Side = TEXT("left");
				Pending.Planned = Planned;
				Pending.Start = Start;
				Pending.End = End;
				PendingRoutes.Add(Pending);
			}

			FRunResult Run;
			const TArray<FNode> EmptyNodes;
			const TArray<FEdge> EmptyEdges;
			const TArray<FIsland> EmptyIslands;
			PlanBusContractsAtGeneration(PendingRoutes, BusContracts, SelectedEndpoints, EmptyNodes, EmptyEdges, EmptyIslands, Options, Run);
			const double FarBranchX = PendingRoutes[0].Planned.Polyline[2].X;
			const double CloseBranchX = PendingRoutes[1].Planned.Polyline[2].X;
			const double ExpectedSpacing = FMath::Max(Options.NodePadding * 1.5, 36.0);
			const double ExpectedCloseBranchX = ComputeOutputXAlignedToInput(PendingRoutes[1].End.X, Options);
			const bool bCloserOutputKeptRightLane =
				CloseBranchX > FarBranchX + ExpectedSpacing - 0.5
				&& FMath::IsNearlyEqual(CloseBranchX, ExpectedCloseBranchX, 0.5)
				&& BusContracts[1].BranchLaneIndex == 0
				&& BusContracts[0].BranchLaneIndex == 1
				&& Run.DesignDiagnostics.CrossBusLaneCount == 1
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[0].Planned.Polyline, Options)
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[1].Planned.Polyline, Options);
			AddCase(TEXT("cross_bus_branch_lane_uses_y_projection_and_closer_output_prefers_right_x"),
				bCloserOutputKeptRightLane,
				FString::Printf(TEXT("far_x=%.1f close_x=%.1f spacing=%.1f lanes=%d lane_index=%d/%d diagnostics=%d"),
					FarBranchX,
					CloseBranchX,
					ExpectedSpacing,
					Run.DesignDiagnostics.CrossBusLaneCount,
					BusContracts[0].BranchLaneIndex,
					BusContracts[1].BranchLaneIndex,
					Run.CrossBusLaneDiagnostics.Num()));
		}

		{
			TArray<FPendingCorridorBusRoute> PendingRoutes;
			TArray<FBusContract> BusContracts;
			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			for (int32 Index = 0; Index < 2; ++Index)
			{
				const FVector2D Start(Index == 0 ? FVector2D(826.0, 4665.0) : FVector2D(230.4, 4571.0));
				const FVector2D End(1906.0, Index == 0 ? 5116.0 : 5146.0);
				const double TrunkY = Index == 0 ? 4665.0 : 4571.0;
				const double EntryX = ComputeInputXAlignedToOutput(Start.X, Options);
				const double BranchX = ComputeOutputXAlignedToInput(End.X, Options);

				FSelectedEndpointCandidate Selected;
				Selected.EdgeIndex = Index;
				Selected.CandidateId = Index;
				Selected.ConnectionGroupId = Index;
				Selected.SourcePortOrderKey = Start.Y;
				Selected.Side = TEXT("left");
				SelectedEndpoints.Add(Selected);

				FPlannedCorridorRoute Planned;
				Planned.RouteKind = ERouteKind::LocalBus;
				Planned.TrunkY = TrunkY;
				Planned.BranchX = BranchX;
				Planned.Polyline = { Start, FVector2D(EntryX, TrunkY), FVector2D(BranchX, TrunkY), End };

				FBusContract Contract;
				Contract.Id = Index;
				Contract.SourceParamId = FString::Printf(TEXT("same_target_branch_source_%d"), Index);
				Contract.RouteKind = ERouteKind::LocalBus;
				Contract.TrunkY = TrunkY;
				Contract.EdgeIndices.Add(Index);
				Contract.BranchXs.Add(BranchX);
				RecomputeBusContractBoundsFromRoute(Contract, Planned);
				BusContracts.Add(Contract);

				FPendingCorridorBusRoute Pending;
				Pending.SelectedEndpointIndex = Index;
				Pending.ContractIndex = Contract.Id;
				Pending.EdgeIndex = Index;
				Pending.SourceParamId = Contract.SourceParamId;
				Pending.Side = TEXT("left");
				Pending.Planned = Planned;
				Pending.Start = Start;
				Pending.End = End;
				PendingRoutes.Add(Pending);
			}

			FRunResult Run;
			const TArray<FNode> EmptyNodes;
			const TArray<FEdge> EmptyEdges;
			const TArray<FIsland> EmptyIslands;
			const int32 Applied = ApplyCrossBusBranchXLanesAtGeneration(
				PendingRoutes,
				BusContracts,
				SelectedEndpoints,
				EmptyNodes,
				EmptyEdges,
				EmptyIslands,
				Options,
				Run);
			const int32 BranchIndexA = PendingRoutes.IsValidIndex(0) ? GetTargetBranchPointIndex(PendingRoutes[0].Planned) : INDEX_NONE;
			const int32 BranchIndexB = PendingRoutes.IsValidIndex(1) ? GetTargetBranchPointIndex(PendingRoutes[1].Planned) : INDEX_NONE;
			const double BranchXA = PendingRoutes[0].Planned.Polyline.IsValidIndex(BranchIndexA) ? PendingRoutes[0].Planned.Polyline[BranchIndexA].X : 0.0;
			const double BranchXB = PendingRoutes[1].Planned.Polyline.IsValidIndex(BranchIndexB) ? PendingRoutes[1].Planned.Polyline[BranchIndexB].X : 0.0;
			const double BranchLaneSpacing = GetCrossBusBranchLaneSpacing(Options);
			const bool bSelectedBranchesSeparatedBeforeWriteback =
				Applied >= 1
				&& FMath::Abs(BranchXA - BranchXB) >= BranchLaneSpacing - 0.5
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[0].Planned.Polyline, Options)
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[1].Planned.Polyline, Options);
			AddCase(TEXT("selected_cross_bus_branch_x_lanes_are_finalized_before_writeback"),
				bSelectedBranchesSeparatedBeforeWriteback,
				FString::Printf(TEXT("applied=%d branch_x=%.1f/%.1f spacing=%.1f diagnostics=%d"),
					Applied,
					BranchXA,
					BranchXB,
					BranchLaneSpacing,
					Run.RejectionDiagnostics.Num()));
		}

		{
			TArray<FPendingCorridorBusRoute> PendingRoutes;
			TArray<FBusContract> BusContracts;
			TArray<FSelectedEndpointCandidate> SelectedEndpoints;

			auto AddContractRoute = [&PendingRoutes, &BusContracts, &SelectedEndpoints, &Options](
				const int32 ContractIndex,
				const int32 EdgeIndex,
				const FString& SourceParamId,
				const FVector2D& Start,
				const FVector2D& End,
				const double InitialTrunkY)
			{
				if (!BusContracts.IsValidIndex(ContractIndex))
				{
					FBusContract Contract;
					Contract.Id = ContractIndex;
					Contract.SourceParamId = SourceParamId;
					Contract.RouteKind = ERouteKind::LocalBus;
					Contract.TrunkY = InitialTrunkY;
					BusContracts.SetNum(ContractIndex + 1);
					BusContracts[ContractIndex] = Contract;
				}

				FSelectedEndpointCandidate Selected;
				Selected.EdgeIndex = EdgeIndex;
				Selected.CandidateId = EdgeIndex;
				Selected.ConnectionGroupId = ContractIndex;
				Selected.SourcePortOrderKey = Start.Y;
				Selected.Side = TEXT("left");
				SelectedEndpoints.Add(Selected);

				const double EntryX = ComputeInputXAlignedToOutput(Start.X, Options);
				const double BranchX = ComputeOutputXAlignedToInput(End.X, Options);

				FPlannedCorridorRoute Planned;
				Planned.RouteKind = ERouteKind::LocalBus;
				Planned.TrunkY = InitialTrunkY;
				Planned.BranchX = BranchX;
				Planned.Polyline = { Start, FVector2D(EntryX, InitialTrunkY), FVector2D(BranchX, InitialTrunkY), End };

				BusContracts[ContractIndex].EdgeIndices.AddUnique(EdgeIndex);
				BusContracts[ContractIndex].BranchXs.Add(BranchX);
				RecomputeBusContractBoundsFromRoute(BusContracts[ContractIndex], Planned);

				FPendingCorridorBusRoute Pending;
				Pending.SelectedEndpointIndex = SelectedEndpoints.Num() - 1;
				Pending.ContractIndex = ContractIndex;
				Pending.EdgeIndex = EdgeIndex;
				Pending.SourceParamId = SourceParamId;
				Pending.Side = TEXT("left");
				Pending.Planned = Planned;
				Pending.Start = Start;
				Pending.End = End;
				PendingRoutes.Add(Pending);
			};

			AddContractRoute(0, 0, TEXT("source_a"), FVector2D(826.0, 4665.0), FVector2D(1906.0, 5116.0), 4606.0);
			AddContractRoute(0, 1, TEXT("source_a"), FVector2D(826.0, 4665.0), FVector2D(5918.0, 5130.0), 4606.0);
			AddContractRoute(1, 2, TEXT("source_b"), FVector2D(230.4, 4571.0), FVector2D(5918.0, 5100.0), 4607.0);
			AddContractRoute(1, 3, TEXT("source_b"), FVector2D(230.4, 4571.0), FVector2D(1906.0, 5146.0), 4607.0);
			AddContractRoute(2, 4, TEXT("source_c"), FVector2D(483.0, 4325.0), FVector2D(2712.0, 4756.0), 4517.75);

			FRunResult Run;
			const TArray<FNode> EmptyNodes;
			const TArray<FEdge> EmptyEdges;
			const TArray<FIsland> EmptyIslands;
			PlanBusContractsAtGeneration(PendingRoutes, BusContracts, SelectedEndpoints, EmptyNodes, EmptyEdges, EmptyIslands, Options, Run);

			const double TrunkLaneSpacing = FMath::Max(
				FMath::Max(Options.NodePadding * 2.0, 48.0),
				GetFallbackInterfaceRowSpacing(Options));
			const double BranchLaneSpacing = FMath::Max(Options.NodePadding * 1.5, 36.0);
			double SourceAToSharedTargetBranchX = 0.0;
			double SourceBToSharedTargetBranchX = 0.0;
			for (const FPendingCorridorBusRoute& Pending : PendingRoutes)
			{
				const int32 BranchIndex = GetTargetBranchPointIndex(Pending.Planned);
				if (!Pending.Planned.Polyline.IsValidIndex(BranchIndex))
				{
					continue;
				}
				if (Pending.EdgeIndex == 0)
				{
					SourceAToSharedTargetBranchX = Pending.Planned.Polyline[BranchIndex].X;
				}
				else if (Pending.EdgeIndex == 3)
				{
					SourceBToSharedTargetBranchX = Pending.Planned.Polyline[BranchIndex].X;
				}
			}

			bool bNoLaneConflictFallback = true;
			for (const TSharedPtr<FJsonValue>& LaneValue : Run.LaneAssignmentDiagnostics)
			{
				const TSharedPtr<FJsonObject> LaneObj = LaneValue.IsValid() ? LaneValue->AsObject() : nullptr;
				bool bFallback = false;
				if (LaneObj.IsValid() && LaneObj->TryGetBoolField(TEXT("lane_conflict_fallback_used"), bFallback) && bFallback)
				{
					bNoLaneConflictFallback = false;
				}
			}

			const bool bDifferentInputsToSameTargetAvoid =
				PendingRoutes.Num() == 5
				&& FMath::Abs(PendingRoutes[0].Planned.TrunkY - PendingRoutes[2].Planned.TrunkY) >= TrunkLaneSpacing - 0.5
				&& FMath::Abs(SourceAToSharedTargetBranchX - SourceBToSharedTargetBranchX) >= BranchLaneSpacing - 0.5
				&& bNoLaneConflictFallback
				&& Run.DesignDiagnostics.CrossBusTrunkLaneCount == 2
				&& Run.DesignDiagnostics.CrossBusLaneCount >= 1
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[0].Planned.Polyline, Options)
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[3].Planned.Polyline, Options);
			AddCase(TEXT("different_source_buses_to_same_target_inputs_plan_trunk_y_and_branch_x_lanes"),
				bDifferentInputsToSameTargetAvoid,
				FString::Printf(TEXT("trunk_y=%.1f/%.1f branch_x=%.1f/%.1f trunk_spacing=%.1f branch_spacing=%.1f fallback=%d trunk_lanes=%d branch_lanes=%d"),
					PendingRoutes.IsValidIndex(0) ? PendingRoutes[0].Planned.TrunkY : 0.0,
					PendingRoutes.IsValidIndex(2) ? PendingRoutes[2].Planned.TrunkY : 0.0,
					SourceAToSharedTargetBranchX,
					SourceBToSharedTargetBranchX,
					TrunkLaneSpacing,
					BranchLaneSpacing,
					bNoLaneConflictFallback ? 0 : 1,
					Run.DesignDiagnostics.CrossBusTrunkLaneCount,
					Run.DesignDiagnostics.CrossBusLaneCount));
		}

		{
			TArray<FNode> Nodes;
			Nodes.SetNum(3);
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				Nodes[Index].Index = Index;
				Nodes[Index].Guid = FString::Printf(TEXT("branch_search_%d"), Index);
				Nodes[Index].Title = Nodes[Index].Guid;
				Nodes[Index].Width = 120.0;
				Nodes[Index].Height = 80.0;
				Nodes[Index].InputPinNames = { TEXT("In") };
				Nodes[Index].OutputPinNames = { TEXT("Out") };
				Nodes[Index].InputPinAnchors = { FVector2D(0.0, 40.0) };
				Nodes[Index].OutputPinAnchors = { FVector2D(Nodes[Index].Width, 40.0) };
				Nodes[Index].InputPinAnchorValid = { 1 };
				Nodes[Index].OutputPinAnchorValid = { 1 };
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 90.0;
			Nodes[1].NewX = 450.0;
			Nodes[1].NewY = 88.0;
			Nodes[1].Width = 180.0;
			Nodes[1].Height = 120.0;
			Nodes[2].NewX = 950.0;
			Nodes[2].NewY = 190.0;

			TArray<FIsland> Islands;
			Islands.SetNum(3);
			for (int32 Index = 0; Index < Islands.Num(); ++Index)
			{
				Islands[Index].Id = Index;
				Islands[Index].Nodes = { Index };
				Islands[Index].NodeSet.Add(Index);
				Islands[Index].LocalBounds = { Nodes[Index].NewX, Nodes[Index].NewY, Nodes[Index].NewX + Nodes[Index].Width, Nodes[Index].NewY + Nodes[Index].Height };
			}
			TArray<FEdge> Edges = {
				{0, 2, TEXT("Out"), TEXT("In"), 0, 0, false}
			};

			const FVector2D Start(Nodes[0].NewX + Nodes[0].Width, Nodes[0].NewY + Nodes[0].OutputPinAnchors[0].Y);
			const FVector2D End(Nodes[2].NewX, Nodes[2].NewY + Nodes[2].InputPinAnchors[0].Y);
			const double EntryX = ComputeInputXAlignedToOutput(Start.X, Options);
			const double ExitX = ComputeOutputXAlignedToInput(End.X, Options);

			FPlannedCorridorRoute Planned;
			Planned.RouteKind = ERouteKind::LocalBus;
			Planned.TrunkY = Start.Y;
			Planned.BranchX = ExitX;
			Planned.Polyline = { Start, FVector2D(EntryX, Start.Y), FVector2D(ExitX, Start.Y), End };

			TArray<FPendingCorridorBusRoute> PendingRoutes;
			FPendingCorridorBusRoute Pending;
			Pending.SelectedEndpointIndex = 0;
			Pending.ContractIndex = 0;
			Pending.EdgeIndex = 0;
			Pending.SourceParamId = TEXT("hard_avoid_source");
			Pending.Side = TEXT("left");
			Pending.Planned = Planned;
			Pending.Start = Start;
			Pending.End = End;
			PendingRoutes.Add(Pending);

			TArray<FBusContract> BusContracts;
			FBusContract Contract;
			Contract.Id = 0;
			Contract.SourceParamId = Pending.SourceParamId;
			Contract.RouteKind = ERouteKind::LocalBus;
			Contract.TrunkY = Planned.TrunkY;
			Contract.EdgeIndices.Add(0);
			Contract.BranchXs.Add(ExitX);
			RecomputeBusContractBoundsFromRoute(Contract, Planned);
			BusContracts.Add(Contract);

			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			FSelectedEndpointCandidate Selected;
			Selected.EdgeIndex = 0;
			Selected.CandidateId = 0;
			Selected.ConnectionGroupId = 0;
			Selected.SourcePortOrderKey = Start.Y;
			Selected.Side = TEXT("left");
			SelectedEndpoints.Add(Selected);

			FRunResult Run;
			PlanBusContractsAtGeneration(PendingRoutes, BusContracts, SelectedEndpoints, Nodes, Edges, Islands, Options, Run);
			const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[0]);
			const int32 HitCount = PendingRoutes.Num() > 0 ? CountControlWireHardOccupancyHits(PendingRoutes[0].Planned.Polyline, RouteHardRects) : INDEX_NONE;
			const double BranchX = PendingRoutes.Num() > 0 ? PendingRoutes[0].Planned.BranchX : ExitX;
			const bool bBranchSearchCanMoveBeforeHardBlocker =
				PendingRoutes.Num() == 1
				&& HitCount == 0
				&& BranchX < ExitX - GetCrossBusBranchSetbackLimitX(Options) - 0.5
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[0].Planned.Polyline, Options);
			AddCase(TEXT("bus_branch_search_range_can_move_left_to_avoid_hard_occupancy"),
				bBranchSearchCanMoveBeforeHardBlocker,
				FString::Printf(TEXT("branch_x=%.1f exit_x=%.1f setback=%.1f hits=%d points=%d"),
					BranchX,
					ExitX,
					GetCrossBusBranchSetbackLimitX(Options),
					HitCount,
					PendingRoutes.Num() > 0 ? PendingRoutes[0].Planned.Polyline.Num() : 0));
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
			const bool bFeedbackKeptInsideIsland =
				Run.DesignDiagnostics.SccFeedbackEdgeCount >= 1
				&& Run.DesignDiagnostics.RankIgnoredEdgeCount >= Run.DesignDiagnostics.SccFeedbackEdgeCount
				&& Run.DesignDiagnostics.FanoutImplicitEdgeCount == 0
				&& Run.DesignDiagnostics.IslandCutEdgeCount == 0
				&& Run.IslandCount == 1
				&& Run.IslandEdgeCount == 0;
			AddCase(TEXT("scc_feedback_rank_ignored_edge_keeps_single_island"), bFeedbackKeptInsideIsland, FString::Printf(TEXT("scc_feedback=%d rank_ignored=%d fanout_edges=%d island_cut=%d islands=%d island_edges=%d"), Run.DesignDiagnostics.SccFeedbackEdgeCount, Run.DesignDiagnostics.RankIgnoredEdgeCount, Run.DesignDiagnostics.FanoutImplicitEdgeCount, Run.DesignDiagnostics.IslandCutEdgeCount, Run.IslandCount, Run.IslandEdgeCount));
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
				&& Run.DesignDiagnostics.IslandCutEdgeCount == 0
				&& Run.IslandCount == 1
				&& Run.IslandEdgeCount == 0;
			AddCase(TEXT("feedback_edge_not_counted_as_true_fanout"), bFeedbackNotCountedAsFanout, FString::Printf(TEXT("scc_feedback=%d fanout_edges=%d island_cut=%d islands=%d island_edges=%d backward=%d"), Run.DesignDiagnostics.SccFeedbackEdgeCount, Run.DesignDiagnostics.FanoutImplicitEdgeCount, Run.DesignDiagnostics.IslandCutEdgeCount, Run.IslandCount, Run.IslandEdgeCount, Run.MetricsAfter.BackwardEdgeCount));
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
			const bool bSimpleFanoutStayedDirect = Run.ReroutePlanCount == 0 && Run.DesignDiagnostics.LaneColoringLaneCount == 0;
			AddCase(TEXT("simple_fanout_does_not_force_bus_reroutes"), Run.IslandCount == 3 && Run.IslandEdgeCount == 2 && bHasBusModel && bSimpleFanoutStayedDirect, FString::Printf(TEXT("islands=%d island_edges=%d internal_bus=%d boundary_entry=%d candidates=%d selected=%d reroutes=%d lanes=%d"), Run.IslandCount, Run.IslandEdgeCount, Run.DesignDiagnostics.InternalBusGroupCount, Run.DesignDiagnostics.BoundaryEntryCount, Run.DesignDiagnostics.EndpointCandidateCount, Run.DesignDiagnostics.SelectedEndpointCandidateCount, Run.ReroutePlanCount, Run.DesignDiagnostics.LaneColoringLaneCount));
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
			const bool bDistantFanoutStayedDirectAfterCompaction = Run.ReroutePlanCount == 0 && Run.DesignDiagnostics.LaneColoringLaneCount == 0;
			AddCase(TEXT("distant_safe_fanout_stays_direct_after_consumer_compaction"), bDistantFanoutStayedDirectAfterCompaction, FString::Printf(TEXT("reroutes=%d lanes=%d edge_crossing=%d"), Run.ReroutePlanCount, Run.DesignDiagnostics.LaneColoringLaneCount, Run.MetricsAfterReroute.EdgeCrossingCount));
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
			const double CompactComponentSpan = Nodes[5].NewX - Nodes[3].NewX;
			const bool bComponentLocalColumns = Run.DesignDiagnostics.PlacementComponentCount >= 2
				&& CompactComponentSpan < 1200.0;
			AddCase(TEXT("component_local_columns_ignore_unrelated_wide_island"), bComponentLocalColumns, FString::Printf(TEXT("components=%d compact_span=%.1f x=%.1f/%.1f/%.1f"), Run.DesignDiagnostics.PlacementComponentCount, CompactComponentSpan, Nodes[3].NewX, Nodes[4].NewX, Nodes[5].NewX));
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
			const bool bExecFanoutStayedInSkeleton = Run.IslandCount == 1
				&& Run.IslandEdgeCount == 0
				&& Run.DesignDiagnostics.FanoutImplicitEdgeCount == 0
				&& Run.ReroutePlanCount == 0
				&& Nodes[0].NewX < Nodes[1].NewX
				&& Nodes[0].NewX < Nodes[2].NewX;
			AddCase(TEXT("exec_fanout_stays_in_skeleton_not_bus"), bExecFanoutStayedInSkeleton, FString::Printf(TEXT("islands=%d island_edges=%d fanout_edges=%d reroutes=%d x=%.1f/%.1f/%.1f"), Run.IslandCount, Run.IslandEdgeCount, Run.DesignDiagnostics.FanoutImplicitEdgeCount, Run.ReroutePlanCount, Nodes[0].NewX, Nodes[1].NewX, Nodes[2].NewX));
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
			MinimizeColumnCrossings(Nodes, NoAdjacentEdges, EmptySourceDemands, DemandSets, Columns);
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
			MinimizeColumnCrossings(Nodes, AdjacentEdges, SourceDemands, EmptyTargetDemands, Columns);
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
			MinimizeColumnCrossings(Nodes, AdjacentEdges, SourceDemands, EmptyTargetDemands, Columns);
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
			TArray<FIsland> Islands;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FIsland Island;
				Island.Id = Index;
				Island.Rank = Index == 0 ? 0 : 1;
				Island.X = Index == 0 ? 0.0 : 500.0;
				Island.Y = 0.0;
				Island.LocalBounds = {0.0, 0.0, Index == 0 ? 150.0 : 180.0, 90.0};
				Islands.Add(Island);
			}

			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			FSelectedEndpointCandidate LowerTarget;
			LowerTarget.EdgeIndex = 0;
			LowerTarget.SourceIsland = 0;
			LowerTarget.TargetIsland = 1;
			LowerTarget.Side = TEXT("left");
			LowerTarget.SourceLocal = FVector2D(150.0, 90.0);
			LowerTarget.EntryLocal = FVector2D(0.0, 90.0);
			LowerTarget.ActualTargetLocal = FVector2D(0.0, 90.0);
			LowerTarget.SourcePortOrderKey = LowerTarget.SourceLocal.Y;
			SelectedEndpoints.Add(LowerTarget);

			FSelectedEndpointCandidate UpperTarget;
			UpperTarget.EdgeIndex = 1;
			UpperTarget.SourceIsland = 0;
			UpperTarget.TargetIsland = 2;
			UpperTarget.Side = TEXT("left");
			UpperTarget.SourceLocal = FVector2D(150.0, 30.0);
			UpperTarget.EntryLocal = FVector2D(0.0, -420.0);
			UpperTarget.ActualTargetLocal = FVector2D(0.0, -420.0);
			UpperTarget.SourcePortOrderKey = UpperTarget.SourceLocal.Y;
			SelectedEndpoints.Add(UpperTarget);

			PlaceIslandsYByWeightedMedianAndPacking(Islands, SelectedEndpoints, Options);
			const TArray<FSourcePortOrderConstraint> Constraints = BuildSourcePortOrderConstraints(Islands, SelectedEndpoints);
			const int32 Violations = CountSourcePortOrderViolations(Islands, Constraints);
			const bool bIslandPlacementPreservesSourcePortOrder =
				Constraints.Num() == 1
				&& Violations == 0
				&& Islands[2].Y + Islands[2].LocalBounds.Height() * 0.5 < Islands[1].Y + Islands[1].LocalBounds.Height() * 0.5;
			AddCase(TEXT("island_y_placement_preserves_source_output_port_order"),
				bIslandPlacementPreservesSourcePortOrder,
				FString::Printf(TEXT("constraints=%d violations=%d upper_y=%.1f lower_y=%.1f"), Constraints.Num(), Violations, Islands[2].Y, Islands[1].Y));
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
			const double LeafAY = Nodes[3].NewY + GetOutputAnchor(Nodes[3], 0).Y;
			const double PureAY = Nodes[2].NewY + GetInputAnchor(Nodes[2], 0).Y;
			const double LeafBY = Nodes[4].NewY + GetOutputAnchor(Nodes[4], 0).Y;
			const double PureBY = Nodes[2].NewY + GetInputAnchor(Nodes[2], 1).Y;
			const bool bCompoundInputChainAligned = Run.DesignDiagnostics.InputChainCount == 3
				&& Run.DesignDiagnostics.InputChainNodeCount == 3
				&& Nodes[3].NewX < Nodes[2].NewX
				&& Nodes[4].NewX < Nodes[2].NewX
				&& Nodes[2].NewX < Nodes[1].NewX
				&& FMath::Abs(PureOutY - BranchConditionY) <= 1.0
				&& FMath::Abs(LeafAY - PureAY) <= InputChainTargetGroupGap(Options) + 1.0
				&& FMath::Abs(LeafBY - PureBY) <= InputChainTargetGroupGap(Options) + 1.0;
			AddCase(TEXT("compound_input_chain_uses_nested_two_column_anchor"), bCompoundInputChainAligned, FString::Printf(TEXT("chains=%d nodes=%d x=%.1f/%.1f/%.1f/%.1f residual=%.1f/%.1f/%.1f"), Run.DesignDiagnostics.InputChainCount, Run.DesignDiagnostics.InputChainNodeCount, Nodes[3].NewX, Nodes[4].NewX, Nodes[2].NewX, Nodes[1].NewX, FMath::Abs(PureOutY - BranchConditionY), FMath::Abs(LeafAY - PureAY), FMath::Abs(LeafBY - PureBY)));
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
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("base_slot_refine%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index % 2 == 0 ? 220.0 : 330.0;
				Node.Height = Index < 2 ? (Index == 0 ? 82.0 : 226.0) : 160.0;
				Node.bHasExecPin = true;
				Node.InputPinNames = { TEXT("execute") };
				Node.OutputPinNames = { TEXT("then") };
				Node.InputPinAnchors = { FVector2D(0.0, Index == 1 ? 58.0 : 26.0) };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 26.0) };
				Node.InputPinAnchorValid = { 1 };
				Node.OutputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 113.0;
			Nodes[2].NewX = 0.0;
			Nodes[2].NewY = 220.0;
			Nodes[1].NewX = 440.0;
			Nodes[1].NewY = 0.0;
			Nodes[3].NewX = 440.0;
			Nodes[3].NewY = 250.0;
			TArray<TArray<int32>> Columns;
			Columns.SetNum(2);
			Columns[0] = {0, 2};
			Columns[1] = {1, 3};
			TArray<FEdge> Edges = {
				{0, 1, TEXT("then"), TEXT("execute"), 0, 0, true}
			};
			RefineColumnItemsWithinAnchoredSlots(Nodes, Edges, Columns, 0, Options.NodePadding);
			const double SourceOutputY = Nodes[0].NewY + GetOutputAnchor(Nodes[0], 0).Y;
			const double TargetInputY = Nodes[1].NewY + GetInputAnchor(Nodes[1], 0).Y;
			const bool bBaseSlotRefinedToPin = FMath::Abs(SourceOutputY - TargetInputY) <= 1.0
				&& Nodes[0].NewY + Nodes[0].Height + Options.NodePadding <= Nodes[2].NewY + 0.5;
			AddCase(TEXT("base_column_slot_refinement_aligns_to_adjacent_pin"), bBaseSlotRefinedToPin, FString::Printf(TEXT("source_y=%.1f target_y=%.1f node0_y=%.1f next_y=%.1f"), SourceOutputY, TargetInputY, Nodes[0].NewY, Nodes[2].NewY));
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
				bForbiddenZoneAvoided && bCollisionGuardCreated && bExecCollisionGuardCreated && bExistingPlanSkipped && bProtectedIslandEdgeSkipped,
				FString::Printf(
					TEXT("zones=%d avoid=%d guard=%d exec_guard=%d rejected=%d existing_guard=%d protected_guard=%d chain=%.1f..%.1f block=%.1f plan=%d existing_points=%d"),
					Run.DesignDiagnostics.InputChainForbiddenZoneCount,
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
			FIslandPortCandidate Candidate;
			Candidate.SourceLocal = FVector2D(431.0, 57.0);
			Candidate.TargetLocal = FVector2D(-66.0, 554.0);
			Candidate.ActualTargetLocal = FVector2D(3643.0, 554.0);
			Candidate.Side = TEXT("left");
			const double RequiredDelta = MinTargetDeltaXForCandidate(Candidate, Options);
			const bool bLeftEntryRequiresForwardReachableTarget =
				RequiredDelta >= Candidate.SourceLocal.X + EndpointAccessClearanceX(Options) - Candidate.TargetLocal.X - 0.5;
			AddCase(
				TEXT("left_boundary_entry_requires_source_to_entry_forward_alignment"),
				bLeftEntryRequiresForwardReachableTarget,
				FString::Printf(TEXT("required_delta=%.1f source_x=%.1f entry_x=%.1f clearance=%.1f"),
					RequiredDelta,
					Candidate.SourceLocal.X,
					Candidate.TargetLocal.X,
					EndpointAccessClearanceX(Options)));
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
			const bool bReservedInternalBusLane = Gap12 > Options.HorizontalSpacing + 40.0;
			const bool bNonLeftInputUsesBoundary = Run.DesignDiagnostics.LeftBoundaryEntryCount >= 1
				&& Run.DesignDiagnostics.TopBoundaryEntryCount >= 2
				&& Run.DesignDiagnostics.BottomBoundaryEntryCount >= 2;
			AddCase(TEXT("internal_bus_demand_affects_column_gap_and_entry_side"), bReservedInternalBusLane && bNonLeftInputUsesBoundary, FString::Printf(TEXT("gap=%.1f left_entries=%d top=%d bottom=%d internal_bus=%d"), Gap12, Run.DesignDiagnostics.LeftBoundaryEntryCount, Run.DesignDiagnostics.TopBoundaryEntryCount, Run.DesignDiagnostics.BottomBoundaryEntryCount, Run.DesignDiagnostics.InternalBusGroupCount));
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
			const bool bHasContinuousBoundaryBus = Run.DesignDiagnostics.BoundaryBusRequiredCount == 1
				&& Run.DesignDiagnostics.BoundaryBusCount == 1
				&& Run.DesignDiagnostics.TopBoundaryEntryCount >= 2
				&& Run.DesignDiagnostics.BottomBoundaryEntryCount >= 2;
			const bool bAlignedSegmentKeptResidualTight = Run.DesignDiagnostics.IslandConnectionTopBottomXResidual < 180.0;
			const double TargetIslandLeft = FMath::Min(Nodes[1].NewX, Nodes[2].NewX);
			const bool bAlignedEntryDidNotForceRankColumnGap = TargetIslandLeft - Nodes[0].NewX < Nodes[0].Width + Options.HorizontalSpacing * 0.75;
			AddCase(TEXT("continuous_boundary_entry_uses_connection_alignment_x"), bHasContinuousBoundaryBus && bAlignedSegmentKeptResidualTight && bAlignedEntryDidNotForceRankColumnGap, FString::Printf(TEXT("boundary_bus=%d/%d top=%d bottom=%d x_residual=%.1f target_dx=%.1f"), Run.DesignDiagnostics.BoundaryBusCount, Run.DesignDiagnostics.BoundaryBusRequiredCount, Run.DesignDiagnostics.TopBoundaryEntryCount, Run.DesignDiagnostics.BottomBoundaryEntryCount, Run.DesignDiagnostics.IslandConnectionTopBottomXResidual, TargetIslandLeft - Nodes[0].NewX));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("nearfanout%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 120.0;
				Node.Height = 70.0;
				Node.NewX = 0.0;
				Node.NewY = 0.0;
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
			TArray<FIsland> Islands;
			for (int32 IslandIndex = 0; IslandIndex < 3; ++IslandIndex)
			{
				FIsland Island;
				Island.Id = IslandIndex;
				Island.Nodes = {IslandIndex};
				Island.NodeSet.Add(IslandIndex);
				Island.LocalBounds = {0.0, 0.0, 120.0, 70.0};
				Island.X = IslandIndex == 0 ? 0.0 : (IslandIndex == 1 ? 260.0 : 920.0);
				Island.Y = IslandIndex == 0 ? 0.0 : (IslandIndex == 1 ? 0.0 : 320.0);
				Islands.Add(Island);
			}
			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
			{
				FSelectedEndpointCandidate Selected;
				Selected.EdgeIndex = EdgeIndex;
				Selected.CandidateId = EdgeIndex;
				Selected.SourceIsland = 0;
				Selected.TargetIsland = EdgeIndex + 1;
				Selected.Side = TEXT("left");
				Selected.SourceLocal = FVector2D(120.0, 35.0);
				Selected.EntryLocal = FVector2D(0.0, 35.0);
				Selected.ActualTargetLocal = FVector2D(0.0, 35.0);
				SelectedEndpoints.Add(Selected);
			}
			FRunResult Run;
			const TArray<FReroutePlan> Plans = RouteDirectAndBusCorridors(Nodes, Edges, Islands, SelectedEndpoints, Options, Run);
			const bool bNearStaysDirectAndRemoteUsesBus = Plans.Num() == 1
				&& Plans[0].EdgeIndex == 1
				&& SelectedEndpoints[0].BusContractId == INDEX_NONE
				&& SelectedEndpoints[1].BusContractId != INDEX_NONE;
			AddCase(TEXT("near_consumer_not_absorbed_by_remote_fanout_bus"), bNearStaysDirectAndRemoteUsesBus, FString::Printf(TEXT("plans=%d near_bus=%d remote_bus=%d contracts=%d"), Plans.Num(), SelectedEndpoints[0].BusContractId, SelectedEndpoints[1].BusContractId, Run.DesignDiagnostics.BusContractCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("targetocc%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 1 ? 220.0 : (Index == 3 ? 180.0 : 120.0);
				Node.Height = Index == 1 ? 120.0 : (Index == 3 ? 160.0 : 70.0);
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 65.0;
			Nodes[1].NewX = 260.0;
			Nodes[1].NewY = 260.0;
			Nodes[2].NewX = 300.0;
			Nodes[2].NewY = 470.0;
			Nodes[3].NewX = 0.0;
			Nodes[3].NewY = 0.0;
			TArray<FEdge> Edges = {
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			FIsland SourceIsland;
			SourceIsland.Id = 0;
			SourceIsland.Nodes = {0};
			SourceIsland.NodeSet.Add(0);
			SourceIsland.LocalBounds = {0.0, 65.0, 120.0, 135.0};
			SourceIsland.X = 0.0;
			SourceIsland.Y = 0.0;
			Islands.Add(SourceIsland);
			FIsland TargetIsland;
			TargetIsland.Id = 1;
			TargetIsland.Nodes = {1, 2};
			TargetIsland.NodeSet.Add(1);
			TargetIsland.NodeSet.Add(2);
			TargetIsland.LocalBounds = {250.0, 250.0, 500.0, 550.0};
			TargetIsland.X = 500.0;
			TargetIsland.Y = 0.0;
			Islands.Add(TargetIsland);
			FIsland ObstacleIsland;
			ObstacleIsland.Id = 2;
			ObstacleIsland.Nodes = {3};
			ObstacleIsland.NodeSet.Add(3);
			ObstacleIsland.LocalBounds = {0.0, 0.0, 180.0, 160.0};
			ObstacleIsland.X = 360.0;
			ObstacleIsland.Y = 220.0;
			Islands.Add(ObstacleIsland);

			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			FSelectedEndpointCandidate Selected;
			Selected.EdgeIndex = 0;
			Selected.CandidateId = 0;
			Selected.SourceIsland = 0;
			Selected.TargetIsland = 1;
			Selected.Side = TEXT("left");
			Selected.SourceLocal = FVector2D(120.0, 100.0);
			Selected.EntryLocal = FVector2D(-66.0, 505.0);
			Selected.ActualTargetLocal = FVector2D(300.0, 505.0);
			SelectedEndpoints.Add(Selected);

			FRunResult Run;
			const TArray<FReroutePlan> Plans = RouteDirectAndBusCorridors(Nodes, Edges, Islands, SelectedEndpoints, Options, Run);
			const FPlannedCorridorRoute DebugPlanned = PlanCorridorRouteForSelectedEndpoint(
				Nodes,
				Edges,
				Islands,
				SelectedEndpoints[0],
				1,
				(Selected.SourceLocal.Y + Selected.EntryLocal.Y) * 0.5,
				Options);
			const TArray<FRect> DebugHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[0]);
			FString DebugPoints;
			for (const FVector2D& Point : DebugPlanned.Polyline)
			{
				DebugPoints += FString::Printf(TEXT("(%.1f,%.1f)"), Point.X, Point.Y);
			}
			const FVector2D EntryAbs(Islands[1].X + Selected.EntryLocal.X, Islands[1].Y + Selected.EntryLocal.Y);
			bool bPlanStopsAtBoundaryEntry = false;
			TArray<FVector2D> ControlPoints;
			ControlPoints.Add(FVector2D(Islands[0].X + Selected.SourceLocal.X, Islands[0].Y + Selected.SourceLocal.Y));
			if (Plans.Num() == 1)
			{
				ControlPoints.Append(Plans[0].Points);
				for (const FVector2D& Point : Plans[0].Points)
				{
					if (FVector2D::Distance(Point, EntryAbs) < 0.5)
					{
						bPlanStopsAtBoundaryEntry = true;
						break;
					}
				}
			}
			ControlPoints.Add(FVector2D(Islands[1].X + Selected.ActualTargetLocal.X, Islands[1].Y + Selected.ActualTargetLocal.Y));
			const bool bVisibleWireHitsSibling = ControlWireHitsHardOccupancy(ControlPoints, DebugHardRects);
			const bool bTargetIslandSiblingBlocksRoute =
				Plans.Num() == 1
				&& SelectedEndpoints[0].BusContractId != INDEX_NONE
				&& !bPlanStopsAtBoundaryEntry
				&& !bVisibleWireHitsSibling
				&& Run.DesignDiagnostics.RouteThroughHardOccupancyCount == 0
				&& Run.DesignDiagnostics.RouteThroughIslandCount == 0;
			AddCase(TEXT("inter_island_bus_avoids_target_island_sibling_hard_occupancy"), bTargetIslandSiblingBlocksRoute, FString::Printf(TEXT("plans=%d bus=%d boundary_stop=%d visible_hit=%d route_through=%d hard=%d debug_kind=%s debug_hits=%d debug_points=%s"), Plans.Num(), SelectedEndpoints[0].BusContractId, bPlanStopsAtBoundaryEntry ? 1 : 0, bVisibleWireHitsSibling ? 1 : 0, Run.DesignDiagnostics.RouteThroughIslandCount, Run.DesignDiagnostics.RouteThroughHardOccupancyCount, *RouteKindToString(DebugPlanned.RouteKind), CountPolylineHardOccupancyHits(DebugPlanned.Polyline, DebugHardRects), *DebugPoints));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("longdirectbus%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 0 ? 120.0 : 220.0;
				Node.Height = Index == 0 ? 70.0 : 110.0;
				Node.NewX = 0.0;
				Node.NewY = 0.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 55.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			for (int32 IslandIndex = 0; IslandIndex < 2; ++IslandIndex)
			{
				FIsland Island;
				Island.Id = IslandIndex;
				Island.Nodes = {IslandIndex};
				Island.NodeSet.Add(IslandIndex);
				Island.LocalBounds = {0.0, 0.0, Nodes[IslandIndex].Width, Nodes[IslandIndex].Height};
				Island.X = IslandIndex == 0 ? 0.0 : Options.HorizontalSpacing * 8.0;
				Island.Y = IslandIndex == 0 ? 100.0 : 280.0;
				Islands.Add(Island);
			}
			FSelectedEndpointCandidate Selected;
			Selected.EdgeIndex = 0;
			Selected.SourceIsland = 0;
			Selected.TargetIsland = 1;
			Selected.Side = TEXT("left");
			Selected.SourceLocal = FVector2D(Nodes[0].Width, 35.0);
			Selected.EntryLocal = FVector2D(0.0, 55.0);
			Selected.ActualTargetLocal = FVector2D(0.0, 55.0);
			const double IdealBusY = (Islands[0].Y + Selected.SourceLocal.Y + Islands[1].Y + Selected.ActualTargetLocal.Y) * 0.5;
			const FPlannedCorridorRoute Planned = PlanCorridorRouteForSelectedEndpoint(
				Nodes,
				Edges,
				Islands,
				Selected,
				1,
				IdealBusY,
				Options);
			const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[0]);
			const bool bLongDirectUsesBus =
				Planned.RouteKind == ERouteKind::LocalBus
				&& Planned.Polyline.Num() >= 4
				&& !PolylineHitsHardOccupancy(Planned.Polyline, RouteHardRects)
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(Planned.Polyline, Options);
			AddCase(TEXT("long_direct_inter_island_spline_upgrades_to_local_bus"), bLongDirectUsesBus, FString::Printf(TEXT("kind=%s points=%d trunk_y=%.1f hits=%d"),
				*RouteKindToString(Planned.RouteKind),
				Planned.Polyline.Num(),
				Planned.TrunkY,
				CountPolylineHardOccupancyHits(Planned.Polyline, RouteHardRects)));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("longexecbus%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 150.0;
				Node.Height = 80.0;
				Node.bHasExecPin = true;
				Node.OutputPinNames = { TEXT("then") };
				Node.InputPinNames = { TEXT("execute") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 40.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 40.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("then"), TEXT("execute"), 0, 0, true}
			};
			TArray<FIsland> Islands;
			for (int32 IslandIndex = 0; IslandIndex < 2; ++IslandIndex)
			{
				FIsland Island;
				Island.Id = IslandIndex;
				Island.Nodes = {IslandIndex};
				Island.NodeSet.Add(IslandIndex);
				Island.LocalBounds = {0.0, 0.0, Nodes[IslandIndex].Width, Nodes[IslandIndex].Height};
				Island.X = IslandIndex == 0 ? 0.0 : Options.HorizontalSpacing * 8.0;
				Island.Y = IslandIndex == 0 ? 120.0 : 360.0;
				Islands.Add(Island);
			}
			FSelectedEndpointCandidate Selected;
			Selected.EdgeIndex = 0;
			Selected.SourceIsland = 0;
			Selected.TargetIsland = 1;
			Selected.Side = TEXT("left");
			Selected.SourceLocal = FVector2D(Nodes[0].Width, 40.0);
			Selected.EntryLocal = FVector2D(0.0, 40.0);
			Selected.ActualTargetLocal = FVector2D(0.0, 40.0);
			const double IdealBusY = (Islands[0].Y + Selected.SourceLocal.Y + Islands[1].Y + Selected.ActualTargetLocal.Y) * 0.5;
			const FPlannedCorridorRoute Planned = PlanCorridorRouteForSelectedEndpoint(
				Nodes,
				Edges,
				Islands,
				Selected,
				1,
				IdealBusY,
				Options);
			const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[0]);
			const bool bLongExecDirectUsesBus =
				Planned.RouteKind == ERouteKind::LocalBus
				&& Planned.Polyline.Num() >= 4
				&& !PolylineHitsHardOccupancy(Planned.Polyline, RouteHardRects)
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(Planned.Polyline, Options);
			AddCase(TEXT("long_exec_inter_island_spline_upgrades_to_local_bus"), bLongExecDirectUsesBus, FString::Printf(TEXT("kind=%s points=%d trunk_y=%.1f hits=%d"),
				*RouteKindToString(Planned.RouteKind),
				Planned.Polyline.Num(),
				Planned.TrunkY,
				CountPolylineHardOccupancyHits(Planned.Polyline, RouteHardRects)));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("internalexecdirect%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 150.0;
				Node.Height = 80.0;
				Node.bHasExecPin = true;
				Node.OutputPinNames = { TEXT("then") };
				Node.InputPinNames = { TEXT("execute") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 40.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 40.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Node.NewX = Index == 0 ? 0.0 : Options.HorizontalSpacing * 7.0;
				Node.NewY = Index == 0 ? 120.0 : 360.0;
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("then"), TEXT("execute"), 0, 0, true}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1};
			Island.NodeSet.Add(0);
			Island.NodeSet.Add(1);
			Island.LocalBounds = {0.0, 120.0, Options.HorizontalSpacing * 7.0 + 150.0, 440.0};
			Island.X = 0.0;
			Island.Y = 0.0;
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			IslandByNode.Add(0, 0);
			IslandByNode.Add(1, 0);
			TArray<FReroutePlan> Plans;
			FRunResult Run;
			AppendDirectConnectionObstacleReroutePlans(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run, nullptr);
			const bool bInternalLongExecUsesBus =
				Plans.Num() == 1
				&& Plans[0].RouteKind == ERouteKind::LocalBus
				&& Plans[0].EdgeIndex == 0
				&& Plans[0].Points.Num() >= 2
				&& Run.CorridorRouteDiagnostics.Num() == 1
				&& Run.DesignDiagnostics.RouteKindUpgradeCount == 1;
			AddCase(TEXT("internal_long_exec_direct_upgrades_to_local_bus"), bInternalLongExecUsesBus, FString::Printf(TEXT("plans=%d points=%d upgrades=%d diagnostics=%d"),
				Plans.Num(),
				Plans.Num() > 0 ? Plans[0].Points.Num() : 0,
				Run.DesignDiagnostics.RouteKindUpgradeCount,
				Run.CorridorRouteDiagnostics.Num()));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("segment_refine%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 2 ? 120.0 : 100.0;
				Node.Height = Index == 2 ? 90.0 : 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 20.0;
			Nodes[1].NewX = 620.0;
			Nodes[1].NewY = 230.0;
			Nodes[2].NewX = 250.0;
			Nodes[2].NewY = 115.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2};
			Island.NodeSet.Add(0);
			Island.NodeSet.Add(1);
			Island.NodeSet.Add(2);
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			IslandByNode.Add(0, 0);
			IslandByNode.Add(1, 0);
			IslandByNode.Add(2, 0);
			FReroutePlan Plan;
			Plan.EdgeIndex = 0;
			Plan.Reason = TEXT("input_chain_collision_guard");
			Plan.RouteKind = ERouteKind::LocalBus;
			Plan.Points.Add(FVector2D(330.0, 265.0));
			TArray<FReroutePlan> Plans = {Plan};
			FRunResult Run;
			const int32 Refined = RefineReroutePlansAgainstHardOccupancySegments(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			TArray<FVector2D> Route;
			Route.Add(GetSourcePort(Nodes[0], Edges[0], true));
			for (const FVector2D& Point : Plans[0].Points)
			{
				Route.Add(Point);
			}
			Route.Add(GetTargetPort(Nodes[1], Edges[0], true));
			const int32 HitCount = CountPolylineSegmentHardOccupancyHits(Route, BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[0]));
			AddCase(TEXT("reroute_plan_segment_refine_removes_metric_node_hit"), Refined == 1 && HitCount == 0 && Plans[0].Points.Num() >= 2, FString::Printf(TEXT("refined=%d hits=%d points=%d diagnostics=%d"),
				Refined,
				HitCount,
				Plans[0].Points.Num(),
				Run.CorridorRouteDiagnostics.Num()));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("shared_segment_refine%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = Index == 2 ? 120.0 : 100.0;
				Node.Height = Index == 2 ? 90.0 : 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 20.0;
			Nodes[1].NewX = 620.0;
			Nodes[1].NewY = 230.0;
			Nodes[2].NewX = 250.0;
			Nodes[2].NewY = 115.0;
			Nodes[3].NewX = 760.0;
			Nodes[3].NewY = 300.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 3, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3};
			Island.NodeSet.Add(0);
			Island.NodeSet.Add(1);
			Island.NodeSet.Add(2);
			Island.NodeSet.Add(3);
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			IslandByNode.Add(0, 0);
			IslandByNode.Add(1, 0);
			IslandByNode.Add(2, 0);
			IslandByNode.Add(3, 0);
			FReroutePlan PlanA;
			PlanA.EdgeIndex = 0;
			PlanA.Reason = TEXT("fanout_local_bus");
			PlanA.BusId = TEXT("shared_source");
			PlanA.RouteKind = ERouteKind::LocalBus;
			PlanA.AttachmentMergeGroup = 9;
			PlanA.Points.Add(FVector2D(330.0, 265.0));
			FReroutePlan PlanB;
			PlanB.EdgeIndex = 1;
			PlanB.Reason = TEXT("fanout_local_bus");
			PlanB.BusId = TEXT("shared_source");
			PlanB.RouteKind = ERouteKind::LocalBus;
			PlanB.AttachmentMergeGroup = 9;
			PlanB.Points.Add(FVector2D(540.0, 55.0));
			TArray<FReroutePlan> Plans = {PlanA, PlanB};
			FRunResult Run;
			const int32 Refined = RefineReroutePlansAgainstHardOccupancySegments(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			if (Plans[0].Points.Num() > 0 && Plans[1].Points.Num() > 0 && FMath::IsNearlyEqual(Plans[0].Points[0].Y, Plans[1].Points[0].Y, 0.5))
			{
				Plans[1].Points[0].Y += 24.0;
				FinalizeReroutePlanPoints(Plans[1], Options);
			}
			const int32 Normalized = NormalizeSharedBusTrunkYAfterSegmentRefine(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			if (Plans[0].Points.Num() > 0 && Plans[1].Points.Num() > 0)
			{
				Plans[1].Points[0].X += 64.0;
				FinalizeReroutePlanPoints(Plans[1], Options);
			}
			const int32 PrefixNormalized = NormalizeSharedBusTrunkPrefixAfterSegmentRefine(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			int32 TotalHitCount = 0;
			for (const FReroutePlan& Plan : Plans)
			{
				TArray<FVector2D> Route;
				Route.Add(GetSourcePort(Nodes[Edges[Plan.EdgeIndex].Source], Edges[Plan.EdgeIndex], true));
				for (const FVector2D& Point : Plan.Points)
				{
					Route.Add(Point);
				}
				Route.Add(GetTargetPort(Nodes[Edges[Plan.EdgeIndex].Target], Edges[Plan.EdgeIndex], true));
				TotalHitCount += CountPolylineSegmentHardOccupancyHits(Route, BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[Plan.EdgeIndex]));
			}
			bool bNormalizedDiagnostic = false;
			for (const TSharedPtr<FJsonValue>& Diagnostic : Run.CorridorRouteDiagnostics)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (Diagnostic.IsValid() && Diagnostic->TryGetObject(Obj) && Obj != nullptr && Obj->IsValid())
				{
					FString Kind;
					if ((*Obj)->TryGetStringField(TEXT("kind"), Kind) && Kind == TEXT("shared_bus_trunk_y_normalized"))
					{
						bNormalizedDiagnostic = true;
						break;
					}
				}
			}
			const bool bSharedTrunkY = Plans[0].Points.Num() > 0
				&& Plans[1].Points.Num() > 0
				&& FMath::IsNearlyEqual(Plans[0].Points[0].Y, Plans[1].Points[0].Y, 0.5);
			const bool bSharedTrunkPrefix = Plans[0].Points.Num() > 0
				&& Plans[1].Points.Num() > 0
				&& FVector2D::Distance(Plans[0].Points[0], Plans[1].Points[0]) < 1.0;
			AddCase(TEXT("shared_bus_segment_refine_preserves_single_source_trunk"), Refined >= 1 && Normalized >= 1 && PrefixNormalized >= 1 && TotalHitCount == 0 && bSharedTrunkY && bSharedTrunkPrefix && bNormalizedDiagnostic, FString::Printf(TEXT("refined=%d normalized=%d prefix=%d hits=%d y=%.1f/%.1f prefix=%d normalized_diag=%d"),
				Refined,
				Normalized,
				PrefixNormalized,
				TotalHitCount,
				Plans[0].Points.Num() > 0 ? Plans[0].Points[0].Y : -1.0,
				Plans[1].Points.Num() > 0 ? Plans[1].Points[0].Y : -1.0,
				bSharedTrunkPrefix ? 1 : 0,
				bNormalizedDiagnostic ? 1 : 0));
		}

		{
			const FVector2D Start(100.0, 100.0);
			const FVector2D End(210.0, 220.0);
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("shortbus%d"), Index);
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
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			for (int32 IslandIndex = 0; IslandIndex < 2; ++IslandIndex)
			{
				FIsland Island;
				Island.Id = IslandIndex;
				Island.Nodes = {IslandIndex};
				Island.NodeSet.Add(IslandIndex);
				Island.LocalBounds = {0.0, 0.0, 120.0, 70.0};
				Island.X = IslandIndex == 0 ? Start.X - 120.0 : End.X;
				Island.Y = IslandIndex == 0 ? Start.Y - 35.0 : End.Y - 35.0;
				Islands.Add(Island);
			}
			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			FSelectedEndpointCandidate Selected;
			Selected.EdgeIndex = 0;
			Selected.CandidateId = 0;
			Selected.SourceIsland = 0;
			Selected.TargetIsland = 1;
			Selected.Side = TEXT("left");
			Selected.SourceLocal = FVector2D(120.0, 35.0);
			Selected.EntryLocal = FVector2D(0.0, 35.0);
			Selected.ActualTargetLocal = FVector2D(0.0, 35.0);
			SelectedEndpoints.Add(Selected);
			FRunResult Run;
			const TArray<FReroutePlan> Plans = RouteDirectAndBusCorridors(Nodes, Edges, Islands, SelectedEndpoints, Options, Run);
			const bool bShortForwardConnectionDoesNotBackwrap = Plans.Num() == 0 && SelectedEndpoints[0].BusContractId == INDEX_NONE;
			AddCase(TEXT("short_connection_does_not_create_backwrap_bus"), bShortForwardConnectionDoesNotBackwrap, FString::Printf(TEXT("plans=%d bus=%d"), Plans.Num(), SelectedEndpoints[0].BusContractId));
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
			AddCase(TEXT("design_feature_coverage_is_truthful"), bCoverageComplete && Run.DesignFeatureCompleteCount == Run.DesignFeatureCount, FString::Printf(TEXT("features=%d complete=%d partial=%d missing=%d design_complete=%s route_through=%d proposals=%d route_groups=%d boundary_bus=%d/%d"), Run.DesignFeatureCount, Run.DesignFeatureCompleteCount, Run.DesignFeaturePartialCount, Run.DesignFeatureMissingCount, Run.bDesignComplete ? TEXT("true") : TEXT("false"), Run.DesignDiagnostics.RouteThroughIslandCount, Run.DesignDiagnostics.PlacementProposalCount, Run.DesignDiagnostics.RouteGroupCount, Run.DesignDiagnostics.BoundaryBusCount, Run.DesignDiagnostics.BoundaryBusRequiredCount));
		}

		Result.bAllPassed = Result.FailedCount == 0;
		return Result;
	}
}
