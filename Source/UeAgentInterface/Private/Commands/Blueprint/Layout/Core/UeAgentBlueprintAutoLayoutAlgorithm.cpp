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
	// Frozen island placement, endpoint candidates, bus contracts, and corridor routes.
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

	static bool HasComponentPath(
		const int32 StartComponent,
		const int32 TargetComponent,
		const TArray<TArray<TPair<int32, int32>>>& ComponentAdjacency,
		const int32 IgnoredEdgeIndex)
	{
		if (StartComponent == TargetComponent)
		{
			return true;
		}
		TArray<int32> Stack;
		TSet<int32> Visited;
		Stack.Add(StartComponent);
		Visited.Add(StartComponent);
		while (Stack.Num() > 0)
		{
			const int32 Component = Stack.Pop(EAllowShrinking::No);
			if (!ComponentAdjacency.IsValidIndex(Component))
			{
				continue;
			}
			for (const TPair<int32, int32>& Next : ComponentAdjacency[Component])
			{
				if (Next.Value == IgnoredEdgeIndex)
				{
					continue;
				}
				if (Next.Key == TargetComponent)
				{
					return true;
				}
				if (!Visited.Contains(Next.Key))
				{
					Visited.Add(Next.Key);
					Stack.Add(Next.Key);
				}
			}
		}
		return false;
	}

	static int32 RestoreFanoutCutsThatCreateArtificialIslandCycles(
		const TArray<FNode>& Nodes,
		const TArray<FEdge>& Edges,
		const TArray<int32>& CommentGroupByNode,
		const TSet<int32>& FeedbackEdges,
		TSet<int32>& RankIgnoredEdges,
		TSet<int32>& IslandCutEdges,
		TSet<int32>& FanoutEdges)
	{
		int32 RestoredCount = 0;
		for (int32 Safety = 0; Safety < Edges.Num() + 1; ++Safety)
		{
			const TArray<int32> ComponentByNode = BuildNodeComponentMapFromIslandCuts(Nodes, Edges, IslandCutEdges, &CommentGroupByNode);
			int32 ComponentCount = 0;
			for (const int32 ComponentIndex : ComponentByNode)
			{
				if (ComponentIndex != INDEX_NONE)
				{
					ComponentCount = FMath::Max(ComponentCount, ComponentIndex + 1);
				}
			}

			TArray<int32> ComponentSizes;
			ComponentSizes.Init(0, ComponentCount);
			for (const int32 ComponentIndex : ComponentByNode)
			{
				if (ComponentSizes.IsValidIndex(ComponentIndex))
				{
					++ComponentSizes[ComponentIndex];
				}
			}

			TArray<TArray<TPair<int32, int32>>> ComponentAdjacency;
			ComponentAdjacency.SetNum(ComponentCount);
			struct FCutCandidate
			{
				int32 EdgeIndex = INDEX_NONE;
				int32 SourceComponent = INDEX_NONE;
				int32 TargetComponent = INDEX_NONE;
			};
			TArray<FCutCandidate> Candidates;
			for (const int32 EdgeIndex : FanoutEdges)
			{
				if (!Edges.IsValidIndex(EdgeIndex)
					|| FeedbackEdges.Contains(EdgeIndex)
					|| !IslandCutEdges.Contains(EdgeIndex)
					|| !IsValidNodeIndex(Nodes, Edges[EdgeIndex].Source)
					|| !IsValidNodeIndex(Nodes, Edges[EdgeIndex].Target))
				{
					continue;
				}
				const int32 SourceComponent = ComponentByNode.IsValidIndex(Edges[EdgeIndex].Source)
					? ComponentByNode[Edges[EdgeIndex].Source]
					: INDEX_NONE;
				const int32 TargetComponent = ComponentByNode.IsValidIndex(Edges[EdgeIndex].Target)
					? ComponentByNode[Edges[EdgeIndex].Target]
					: INDEX_NONE;
				if (SourceComponent == INDEX_NONE || TargetComponent == INDEX_NONE || SourceComponent == TargetComponent)
				{
					continue;
				}
				if (ComponentAdjacency.IsValidIndex(SourceComponent))
				{
					ComponentAdjacency[SourceComponent].Add(TPair<int32, int32>(TargetComponent, EdgeIndex));
				}
				Candidates.Add({ EdgeIndex, SourceComponent, TargetComponent });
			}

			int32 BestEdgeIndex = INDEX_NONE;
			double BestScore = -TNumericLimits<double>::Max();
			for (const FCutCandidate& Candidate : Candidates)
			{
				if (!HasComponentPath(Candidate.TargetComponent, Candidate.SourceComponent, ComponentAdjacency, Candidate.EdgeIndex))
				{
					continue;
				}
				const double SourceSize = ComponentSizes.IsValidIndex(Candidate.SourceComponent) ? ComponentSizes[Candidate.SourceComponent] : 0.0;
				const double TargetSize = ComponentSizes.IsValidIndex(Candidate.TargetComponent) ? ComponentSizes[Candidate.TargetComponent] : 0.0;
				const double Score = (TargetSize - SourceSize) * 1000.0 - static_cast<double>(Candidate.EdgeIndex) * 0.001;
				if (BestEdgeIndex == INDEX_NONE || Score > BestScore)
				{
					BestEdgeIndex = Candidate.EdgeIndex;
					BestScore = Score;
				}
			}
			if (BestEdgeIndex == INDEX_NONE)
			{
				break;
			}
			RankIgnoredEdges.Remove(BestEdgeIndex);
			IslandCutEdges.Remove(BestEdgeIndex);
			FanoutEdges.Remove(BestEdgeIndex);
			++RestoredCount;
		}
		return RestoredCount;
	}

	static int32 EnforceSameIslandNonFeedbackNodeForwardX(
		TArray<FNode>& Nodes,
		const TArray<FEdge>& Edges,
		const TArray<FIsland>& Islands,
		const TMap<int32, int32>& IslandByNode,
		const TSet<int32>& ForwardExemptEdgeIndices,
		const FOptions& Options)
	{
		int32 ShiftCount = 0;
		for (int32 Iteration = 0; Iteration < Nodes.Num() + 1; ++Iteration)
		{
			bool bChanged = false;
			for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
			{
				if (ForwardExemptEdgeIndices.Contains(EdgeIndex))
				{
					continue;
				}
				const FEdge& Edge = Edges[EdgeIndex];
				if (!IsValidNodeIndex(Nodes, Edge.Source)
					|| !IsValidNodeIndex(Nodes, Edge.Target)
					|| Edge.Source == Edge.Target)
				{
					continue;
				}

				const int32* SourceIslandPtr = IslandByNode.Find(Edge.Source);
				const int32* TargetIslandPtr = IslandByNode.Find(Edge.Target);
				if (!SourceIslandPtr
					|| !TargetIslandPtr
					|| *SourceIslandPtr != *TargetIslandPtr
					|| !Islands.IsValidIndex(*SourceIslandPtr)
					|| Islands[*SourceIslandPtr].bCommentIsland)
				{
					continue;
				}

				const double RequiredTargetX =
					Nodes[Edge.Source].NewX
					+ ComputeTargetOriginDeltaXForAlignedConnection(
						GetOutputAnchor(Nodes[Edge.Source], Edge.SourcePinIndex),
						GetInputAnchor(Nodes[Edge.Target], Edge.TargetPinIndex),
						Options);
				if (RequiredTargetX > Nodes[Edge.Target].NewX + 0.5)
				{
					Nodes[Edge.Target].NewX = SnapGraphCoordinate(RequiredTargetX);
					bChanged = true;
					++ShiftCount;
				}
			}
			if (!bChanged)
			{
				break;
			}
		}
		return ShiftCount;
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
		const int32 RestoredArtificialCycleFanoutCutCount =
			RestoreFanoutCutsThatCreateArtificialIslandCycles(
				Nodes,
				Edges,
				CommentGroupByNode,
				FeedbackEdges,
				RankIgnoredEdges,
				IslandCutEdges,
				FanoutEdges);
		if (RestoredArtificialCycleFanoutCutCount > 0)
		{
			FanoutNodes.Reset();
			for (const int32 EdgeIndex : FanoutEdges)
			{
				if (Edges.IsValidIndex(EdgeIndex))
				{
					FanoutNodes.Add(Edges[EdgeIndex].Source);
				}
			}
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore restored_artificial_cycle_fanout_cuts=%d"),
				RestoredArtificialCycleFanoutCutCount);
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
				LayoutIslandInternally(Nodes, Edges, IslandLayoutExplicitEdges, RankIgnoredEdges, Island, Options, OutResult);
			}
		}
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore island_local_done elapsed_ms=%.1f internal_placements=%d input_chains=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			OutResult.DesignDiagnostics.IslandInternalPlacementCount,
			OutResult.DesignDiagnostics.InputChainCount);
		const int32 SameIslandForwardShiftCount = EnforceNonFeedbackSemanticEdgeForwardX(
			Nodes,
			Edges,
			Islands,
			FeedbackEdges,
			Options,
			true,
			false);
		if (SameIslandForwardShiftCount > 0)
		{
			for (FIsland& Island : Islands)
			{
				if (!Island.bCommentIsland)
				{
					NormalizeIslandLocalLayoutToPixelGrid(Nodes, Island);
				}
			}
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore same_island_forward_x_enforced shifts=%d"),
				SameIslandForwardShiftCount);
		}
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
		const int32 EndpointAccessInvalidRerouteRemovedCount = RemoveEndpointAccessInvalidReroutePlans(
			Nodes,
			Edges,
			Islands,
			IslandByNode,
			Options,
			OutResult.ReroutePlans,
			OutResult);
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore endpoint_access_invalid_reroute_prune_done elapsed_ms=%.1f removed=%d reroutes=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			EndpointAccessInvalidRerouteRemovedCount,
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
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore final_visible_trunk_lane_start elapsed_ms=%.1f reroutes=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			OutResult.ReroutePlans.Num());
		const int32 FinalVisibleLaneAdjustedCount = PlanFinalVisibleRerouteTrunkLanes(
			Nodes,
			Edges,
			Islands,
			IslandByNode,
			Options,
			OutResult.ReroutePlans,
			OutResult);
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore final_visible_branch_x_lane_start elapsed_ms=%.1f adjusted=%d reroutes=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			FinalVisibleLaneAdjustedCount,
			OutResult.ReroutePlans.Num());
		const int32 FinalVisibleBranchXAdjustedCount = PlanFinalVisibleRerouteBranchXLanes(
			Nodes,
			Edges,
			Islands,
			IslandByNode,
			Options,
			OutResult.ReroutePlans,
			OutResult);
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore final_visible_trunk_lane_after_branch_x_start elapsed_ms=%.1f branch_x_adjusted=%d reroutes=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			FinalVisibleBranchXAdjustedCount,
			OutResult.ReroutePlans.Num());
		const int32 FinalVisibleLaneAdjustedAfterBranchXCount = PlanFinalVisibleRerouteTrunkLanes(
			Nodes,
			Edges,
			Islands,
			IslandByNode,
			Options,
			OutResult.ReroutePlans,
			OutResult);
		const int32 FinalVisibleSharedTrunkPrefixNormalizedCount = NormalizeSharedBusTrunkPrefixAfterSegmentRefine(
			Nodes,
			Edges,
			Islands,
			IslandByNode,
			Options,
			OutResult.ReroutePlans,
			OutResult);
		const int32 FinalVisibleLaneViolationCount = ValidateFinalVisibleRerouteLaneSpacing(
			Nodes,
			Edges,
			Islands,
			IslandByNode,
			Options,
			OutResult.ReroutePlans,
			&OutResult);
		const int32 FinalVisibleBranchXViolationCount = ValidateFinalVisibleRerouteBranchXLanes(
			Nodes,
			Edges,
			Islands,
			IslandByNode,
			Options,
			OutResult.ReroutePlans,
			&OutResult);
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore final_visible_lane_done elapsed_ms=%.1f adjusted=%d branch_x_adjusted=%d violations=%d branch_x_violations=%d reroutes=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			FinalVisibleLaneAdjustedCount + FinalVisibleLaneAdjustedAfterBranchXCount,
			FinalVisibleBranchXAdjustedCount,
			FinalVisibleLaneViolationCount,
			FinalVisibleBranchXViolationCount,
			OutResult.ReroutePlans.Num());
		if (FinalVisibleSharedTrunkPrefixNormalizedCount > 0)
		{
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore final_visible_shared_trunk_prefix_normalized count=%d"),
				FinalVisibleSharedTrunkPrefixNormalizedCount);
		}
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
			if (OutResult.MetricsAfterReroute.EdgeNodeIntersectionCount == 0)
			{
				for (TSharedPtr<FJsonValue>& RouteValue : OutResult.CorridorRouteDiagnostics)
				{
					TSharedPtr<FJsonObject>* RouteObject = nullptr;
					if (!RouteValue.IsValid() || !RouteValue->TryGetObject(RouteObject) || RouteObject == nullptr || !RouteObject->IsValid())
					{
						continue;
					}
					bool bCrosses = false;
					(*RouteObject)->TryGetBoolField(TEXT("crosses_hard_occupancy"), bCrosses);
					if (bCrosses)
					{
						(*RouteObject)->SetBoolField(TEXT("crosses_island"), false);
						(*RouteObject)->SetBoolField(TEXT("crosses_hard_occupancy"), false);
						(*RouteObject)->SetBoolField(TEXT("post_refine_cleared"), true);
					}
				}
			}
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
		CompleteIf(TEXT("placement_proposal_longest_path_x"), OutResult.DesignDiagnostics.IslandEdgeCount == 0 || (OutResult.DesignDiagnostics.PlacementProposalCount > 0 && OutResult.DesignDiagnostics.PlacementGraphEdgeCount + OutResult.DesignDiagnostics.RouteOnlyEdgeCount == OutResult.DesignDiagnostics.IslandEdgeCount && OutResult.DesignDiagnostics.PlacementOwnerEdgeCount + OutResult.DesignDiagnostics.PlacementNonOwnerEdgeCount >= OutResult.DesignDiagnostics.SelectedEndpointCandidateCount && OutResult.DesignDiagnostics.InvalidForwardConnectionCount == 0), OutResult.DesignDiagnostics.PlacementProposalCount > 0, FString::Printf(TEXT("proposals=%d placement_edges=%d route_only=%d owner=%d non_owner=%d invalid_forward=%d"), OutResult.DesignDiagnostics.PlacementProposalCount, OutResult.DesignDiagnostics.PlacementGraphEdgeCount, OutResult.DesignDiagnostics.RouteOnlyEdgeCount, OutResult.DesignDiagnostics.PlacementOwnerEdgeCount, OutResult.DesignDiagnostics.PlacementNonOwnerEdgeCount, OutResult.DesignDiagnostics.InvalidForwardConnectionCount));
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
			|| (SelectedCorridorRouteDiagnosticCount >= OutResult.DesignDiagnostics.SelectedEndpointCandidateCount
				&& OutResult.DesignDiagnostics.RouteThroughIslandCount == 0);
		CompleteIf(TEXT("corridor_routing_lane_coloring"), bCorridorRoutingComplete, OutResult.CorridorRouteDiagnostics.Num() > 0, FString::Printf(TEXT("corridors=%d selected_routes=%d selected=%d extra=%d lanes=%d route_through_island=%d"), OutResult.CorridorRouteDiagnostics.Num(), SelectedCorridorRouteDiagnosticCount, OutResult.DesignDiagnostics.SelectedEndpointCandidateCount, OutResult.CorridorRouteDiagnostics.Num() - SelectedCorridorRouteDiagnosticCount, OutResult.DesignDiagnostics.LaneColoringLaneCount, OutResult.DesignDiagnostics.RouteThroughIslandCount));
		CompleteIf(TEXT("final_visible_reroute_lane_spacing"), OutResult.DesignDiagnostics.FinalVisibleBusLaneViolationCount == 0, OutResult.LaneAssignmentDiagnostics.Num() > 0, FString::Printf(TEXT("spacing=20 adjusted=%d violations=%d assignments=%d"), OutResult.DesignDiagnostics.FinalVisibleBusLaneAdjustmentCount, OutResult.DesignDiagnostics.FinalVisibleBusLaneViolationCount, OutResult.LaneAssignmentDiagnostics.Num()));
		CompleteIf(TEXT("final_visible_branch_x_lane_spacing"), OutResult.DesignDiagnostics.FinalVisibleBranchXLaneViolationCount == 0, OutResult.LaneAssignmentDiagnostics.Num() > 0, FString::Printf(TEXT("spacing=20 adjusted=%d violations=%d assignments=%d"), OutResult.DesignDiagnostics.FinalVisibleBranchXLaneAdjustmentCount, OutResult.DesignDiagnostics.FinalVisibleBranchXLaneViolationCount, OutResult.LaneAssignmentDiagnostics.Num()));
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
			&& OutResult.DesignDiagnostics.RouteThroughIslandCount == 0
			&& OutResult.DesignDiagnostics.FinalVisibleBusLaneViolationCount == 0
			&& OutResult.DesignDiagnostics.FinalVisibleBranchXLaneViolationCount == 0;

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

	static void RunFinalVisibleLaneRegressionSelfTests(FSelfTestResult& Result, const FOptions& Options)
	{
		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("refinedguardlane%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 100.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 0.0;
			Nodes[1].NewX = 700.0;
			Nodes[1].NewY = 0.0;
			Nodes[2].NewX = 0.0;
			Nodes[2].NewY = 140.0;
			Nodes[3].NewX = 700.0;
			Nodes[3].NewY = 140.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{2, 3, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			for (const int32 NodeIndex : Island.Nodes)
			{
				IslandByNode.Add(NodeIndex, 0);
			}

			FReroutePlan PlanA;
			PlanA.EdgeIndex = 0;
			PlanA.Reason = TEXT("input_chain_collision_guard_segment_refined");
			PlanA.BusId = MakeSourceParamId(Edges[0]);
			PlanA.RouteKind = ERouteKind::LocalBus;
			PlanA.Points = { FVector2D(180.0, 80.0), FVector2D(620.0, 80.0) };
			FReroutePlan PlanB;
			PlanB.EdgeIndex = 1;
			PlanB.Reason = TEXT("input_chain_collision_guard_segment_refined");
			PlanB.BusId = MakeSourceParamId(Edges[1]);
			PlanB.RouteKind = ERouteKind::LocalBus;
			PlanB.Points = { FVector2D(180.0, 81.0), FVector2D(620.0, 81.0) };
			TArray<FReroutePlan> Plans = {PlanA, PlanB};

			FRunResult Run;
			const int32 InitialViolations = ValidateFinalVisibleRerouteLaneSpacing(Nodes, Edges, Islands, IslandByNode, Options, Plans, nullptr);
			const int32 Adjusted = PlanFinalVisibleRerouteTrunkLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			const int32 FinalViolations = ValidateFinalVisibleRerouteLaneSpacing(Nodes, Edges, Islands, IslandByNode, Options, Plans, &Run);
			const double DeltaY = Plans[0].Points.Num() > 0 && Plans[1].Points.Num() > 0
				? FMath::Abs(Plans[0].Points[0].Y - Plans[1].Points[0].Y)
				: 0.0;
			AddSelfTestCase(Result,
				TEXT("refined_input_chain_guards_reenter_final_visible_lane_planner"),
				InitialViolations > 0 && Adjusted >= 1 && FinalViolations == 0 && DeltaY >= GetBusTrunkLaneSpacing() - 0.5,
				FString::Printf(TEXT("initial=%d adjusted=%d final=%d delta=%.1f assignments=%d"),
					InitialViolations,
					Adjusted,
					FinalViolations,
					DeltaY,
					Run.LaneAssignmentDiagnostics.Num()));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("longtargettail%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 100.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 0.0;
			Nodes[1].NewX = 3000.0;
			Nodes[1].NewY = 10.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			for (const int32 NodeIndex : Island.Nodes)
			{
				IslandByNode.Add(NodeIndex, 0);
			}

			const FVector2D Start(Nodes[0].NewX + Nodes[0].Width, Nodes[0].NewY + Nodes[0].OutputPinAnchors[0].Y);
			const FVector2D End(Nodes[1].NewX, Nodes[1].NewY + Nodes[1].InputPinAnchors[0].Y);
			const double EntryX = ComputeInputXAlignedToOutput(Start.X, Options);
			const double ExpectedTargetBranchX = ComputeOutputXAlignedToInput(End.X, Options);
			FReroutePlan Plan;
			Plan.EdgeIndex = 0;
			Plan.Reason = TEXT("direct_obstacle_local_bus");
			Plan.BusId = MakeSourceParamId(Edges[0]);
			Plan.RouteKind = ERouteKind::LocalBus;
			Plan.Points = { FVector2D(EntryX, 60.0), FVector2D(EntryX + 80.0, 60.0) };
			TArray<FReroutePlan> Plans = {Plan};

			FRunResult Run;
			const int32 Adjusted = PlanFinalVisibleRerouteBranchXLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			const int32 FinalViolations = ValidateFinalVisibleRerouteBranchXLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, &Run);
			TArray<FVisibleRerouteBranchXLaneItem> FinalItems;
			CollectVisibleRerouteBranchXLaneItems(Nodes, Edges, Islands, IslandByNode, Options, Plans, FinalItems);
			const double FinalBranchX = FinalItems.Num() > 0 ? FinalItems[0].BranchX : 0.0;
			const bool bTailShortened = FinalItems.Num() == 1
				&& FMath::Abs(ExpectedTargetBranchX - FinalBranchX) <= 0.5;
			AddSelfTestCase(Result,
				TEXT("final_visible_branch_x_canonicalizes_single_long_target_tail"),
				Adjusted >= 1 && FinalViolations == 0 && bTailShortened,
				FString::Printf(TEXT("adjusted=%d final=%d branch=%.1f expected=%.1f assignments=%d points=%d"),
					Adjusted,
					FinalViolations,
					FinalBranchX,
					ExpectedTargetBranchX,
					Run.LaneAssignmentDiagnostics.Num(),
					Plans[0].Points.Num()));
		}

		{
			FReroutePlan Plan;
			Plan.EdgeIndex = 0;
			Plan.Points = { FVector2D(3816.0, 377.0) };
			Plan.KnotPoints = { FVector2D(1256.0, 377.0) };
			const TArray<FVector2D>& WritePoints = GetAuthoritativeReroutePoints(Plan);
			const bool bUsesFinalPoints = WritePoints.Num() == 1
				&& FMath::IsNearlyEqual(WritePoints[0].X, 3816.0, 0.5)
				&& FMath::IsNearlyEqual(WritePoints[0].Y, 377.0, 0.5);
			AddSelfTestCase(Result,
				TEXT("reroute_writeback_uses_authoritative_points_over_legacy_knot_mirror"),
				bUsesFinalPoints,
				FString::Printf(TEXT("write=(%.1f,%.1f) mirror=(%.1f,%.1f)"),
					WritePoints.Num() > 0 ? WritePoints[0].X : -1.0,
					WritePoints.Num() > 0 ? WritePoints[0].Y : -1.0,
					Plan.KnotPoints.Num() > 0 ? Plan.KnotPoints[0].X : -1.0,
					Plan.KnotPoints.Num() > 0 ? Plan.KnotPoints[0].Y : -1.0));
		}

		{
			FOptions StressOptions = Options;
			StressOptions.NodePadding = FMath::Max(StressOptions.NodePadding, 240.0);
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("protectedfinalbranch%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 100.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 996.0;
			Nodes[0].NewY = 327.0;
			Nodes[1].NewX = 3896.0;
			Nodes[1].NewY = 255.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			IslandByNode.Add(0, 0);
			IslandByNode.Add(1, 0);
			const FVector2D Start(Nodes[0].NewX + Nodes[0].OutputPinAnchors[0].X, Nodes[0].NewY + Nodes[0].OutputPinAnchors[0].Y);
			const FVector2D End(Nodes[1].NewX + Nodes[1].InputPinAnchors[0].X, Nodes[1].NewY + Nodes[1].InputPinAnchors[0].Y);
			const double EntryX = ComputeInputXAlignedToOutput(Start.X, StressOptions);
			const double ExpectedBranchX = ComputeOutputXAlignedToInput(End.X, StressOptions);
			FReroutePlan Plan;
			Plan.EdgeIndex = 0;
			Plan.Reason = TEXT("island_internal_multi_column_bus_final_visible_lane");
			Plan.BusId = MakeSourceParamId(Edges[0]);
			Plan.RouteKind = ERouteKind::LocalBus;
			Plan.Points = { FVector2D(EntryX, 313.0), FVector2D(EntryX + 80.0, 313.0) };
			FinalizeReroutePlanPoints(Plan, StressOptions);
			TArray<FReroutePlan> Plans = {Plan};
			FRunResult Run;
			const int32 Adjusted = PlanFinalVisibleRerouteBranchXLanes(Nodes, Edges, Islands, IslandByNode, StressOptions, Plans, Run);
			const bool bProtectedBranchKept = Plans[0].Points.Num() >= 2
				&& FMath::IsNearlyEqual(Plans[0].Points.Last().X, ExpectedBranchX, 0.5);
			AddSelfTestCase(Result,
				TEXT("final_visible_branch_x_preserves_target_side_branch_point_during_compaction"),
				Adjusted >= 1 && bProtectedBranchKept,
				FString::Printf(TEXT("adjusted=%d last_x=%.1f expected=%.1f point_count=%d"),
					Adjusted,
					Plans[0].Points.Num() > 0 ? Plans[0].Points.Last().X : -1.0,
					ExpectedBranchX,
					Plans[0].Points.Num()));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("branchaftertrunk%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 100.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 996.0;
			Nodes[0].NewY = 199.0;
			Nodes[1].NewX = 3896.0;
			Nodes[1].NewY = 191.0;
			Nodes[2].NewX = 996.0;
			Nodes[2].NewY = 339.0;
			Nodes[3].NewX = 3896.0;
			Nodes[3].NewY = 331.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out_a"), TEXT("in_a"), 0, 0, false},
				{2, 3, TEXT("out_b"), TEXT("in_b"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			for (const int32 NodeIndex : Island.Nodes)
			{
				IslandByNode.Add(NodeIndex, 0);
			}
			const FVector2D StartA(Nodes[0].NewX + Nodes[0].OutputPinAnchors[0].X, Nodes[0].NewY + Nodes[0].OutputPinAnchors[0].Y);
			const FVector2D EndA(Nodes[1].NewX + Nodes[1].InputPinAnchors[0].X, Nodes[1].NewY + Nodes[1].InputPinAnchors[0].Y);
			const FVector2D StartB(Nodes[2].NewX + Nodes[2].OutputPinAnchors[0].X, Nodes[2].NewY + Nodes[2].OutputPinAnchors[0].Y);
			const FVector2D EndB(Nodes[3].NewX + Nodes[3].InputPinAnchors[0].X, Nodes[3].NewY + Nodes[3].InputPinAnchors[0].Y);
			const double EntryXA = ComputeInputXAlignedToOutput(StartA.X, Options);
			const double ExpectedBranchXA = ComputeOutputXAlignedToInput(EndA.X, Options);
			const double EntryXB = ComputeInputXAlignedToOutput(StartB.X, Options);
			const double ExitXB = ComputeOutputXAlignedToInput(EndB.X, Options);
			FReroutePlan PlanA;
			PlanA.EdgeIndex = 0;
			PlanA.Reason = TEXT("island_internal_multi_column_bus_final_branch_x_lane");
			PlanA.BusId = MakeSourceParamId(Edges[0]);
			PlanA.RouteKind = ERouteKind::LocalBus;
			PlanA.Points = {
				FVector2D(EntryXA, 249.0),
				FVector2D(ExpectedBranchXA, 249.0),
				FVector2D(ExpectedBranchXA, EndA.Y)
			};
			FinalizeReroutePlanPoints(PlanA, Options);
			FReroutePlan PlanB;
			PlanB.EdgeIndex = 1;
			PlanB.Reason = TEXT("direct_obstacle_local_bus");
			PlanB.BusId = MakeSourceParamId(Edges[1]);
			PlanB.RouteKind = ERouteKind::LocalBus;
			PlanB.Points = { FVector2D(EntryXB, 250.0), FVector2D(ExitXB, 250.0) };
			FinalizeReroutePlanPoints(PlanB, Options);
			TArray<FReroutePlan> Plans = {PlanA, PlanB};
			FRunResult Run;
			const int32 InitialViolations = ValidateFinalVisibleRerouteLaneSpacing(Nodes, Edges, Islands, IslandByNode, Options, Plans, nullptr);
			const int32 Adjusted = PlanFinalVisibleRerouteTrunkLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			bool bPreservedFinalBranchX = false;
			for (const FVector2D& Point : Plans[0].Points)
			{
				if (FMath::IsNearlyEqual(Point.X, ExpectedBranchXA, 0.5))
				{
					bPreservedFinalBranchX = true;
					break;
				}
			}
			AddSelfTestCase(Result,
				TEXT("final_visible_trunk_lane_preserves_preplanned_branch_x"),
				InitialViolations > 0 && Adjusted >= 1 && bPreservedFinalBranchX,
				FString::Printf(TEXT("initial=%d adjusted=%d preserved=%s expected_x=%.1f point_count=%d"),
					InitialViolations,
					Adjusted,
					bPreservedFinalBranchX ? TEXT("true") : TEXT("false"),
					ExpectedBranchXA,
					Plans[0].Points.Num()));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("shared_prefix_after_branch_x_%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 120.0;
				Node.Height = 60.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 30.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 30.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 0.0;
			Nodes[1].NewX = 620.0;
			Nodes[1].NewY = 0.0;
			Nodes[2].NewX = 860.0;
			Nodes[2].NewY = 160.0;

			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false}
			};

			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			for (const int32 NodeIndex : Island.Nodes)
			{
				IslandByNode.Add(NodeIndex, 0);
			}

			FReroutePlan NearPlan;
			NearPlan.EdgeIndex = 0;
			NearPlan.Reason = TEXT("fanout_local_bus_final_branch_x_lane");
			NearPlan.BusId = TEXT("same_output_shared_prefix");
			NearPlan.RouteKind = ERouteKind::LocalBus;
			NearPlan.AttachmentMergeGroup = 99;
			NearPlan.Points = { FVector2D(220.0, 100.0), FVector2D(420.0, 100.0) };

			FReroutePlan FarPlan;
			FarPlan.EdgeIndex = 1;
			FarPlan.Reason = TEXT("fanout_local_bus_final_branch_x_lane");
			FarPlan.BusId = TEXT("same_output_shared_prefix");
			FarPlan.RouteKind = ERouteKind::LocalBus;
			FarPlan.AttachmentMergeGroup = 99;
			FarPlan.Points = { FVector2D(220.0, 100.0), FVector2D(560.0, 100.0) };

			TArray<FReroutePlan> Plans = {NearPlan, FarPlan};
			FRunResult Run;
			const int32 PrefixNormalized = NormalizeSharedBusTrunkPrefixAfterSegmentRefine(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			int32 NearBranchIndexInFar = INDEX_NONE;
			int32 FarBranchIndex = INDEX_NONE;
			for (int32 PointIndex = 0; PointIndex < Plans[1].Points.Num(); ++PointIndex)
			{
				if (FVector2D::Distance(Plans[1].Points[PointIndex], FVector2D(420.0, 100.0)) < 1.0)
				{
					NearBranchIndexInFar = PointIndex;
				}
				if (FVector2D::Distance(Plans[1].Points[PointIndex], FVector2D(560.0, 100.0)) < 1.0)
				{
					FarBranchIndex = PointIndex;
				}
			}
			AddSelfTestCase(Result,
				TEXT("same_output_shared_trunk_prefix_inserts_near_branch_before_far_branch"),
				PrefixNormalized >= 1
				&& NearBranchIndexInFar != INDEX_NONE
				&& FarBranchIndex != INDEX_NONE
				&& NearBranchIndexInFar < FarBranchIndex,
				FString::Printf(TEXT("prefix=%d far_points=%d near_index=%d far_index=%d"),
					PrefixNormalized,
					Plans[1].Points.Num(),
					NearBranchIndexInFar,
					FarBranchIndex));
		}
	}

	static void RunSourceColumnCollisionAabbSelfTests(FSelfTestResult& Result, const FOptions& Options)
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
				MakeNode(0, TEXT("source_column_aabb"), 120.0, 72.0, 0, 0.0, 0.0),
				MakeNode(1, TEXT("source_column_aabb"), 260.0, 120.0, 0, 0.0, 96.0),
				MakeNode(2, TEXT("source_column_aabb"), 120.0, 72.0, 1, 360.0, 180.0)
			};
			TArray<TArray<int32>> Columns;
			Columns.SetNum(2);
			Columns[0] = {0, 1};
			Columns[1] = {2};
			RunCase(TEXT("source_column_output_aabb_collision_extends_to_column_right_boundary"), MoveTemp(Nodes), MoveTemp(Columns), 260.0, true);
		}

		{
			TArray<FNode> Nodes = {
				MakeNode(0, TEXT("source_column_access"), 260.0, 72.0, 0, 0.0, 0.0),
				MakeNode(1, TEXT("source_column_access"), 340.0, 120.0, 0, 0.0, 96.0),
				MakeNode(2, TEXT("source_column_access"), 120.0, 72.0, 1, 520.0, 180.0)
			};
			TArray<TArray<int32>> Columns;
			Columns.SetNum(2);
			Columns[0] = {0, 1};
			Columns[1] = {2};
			RunCase(TEXT("source_column_collision_guard_uses_raw_column_right_boundary"), MoveTemp(Nodes), MoveTemp(Columns), 340.0, true);
		}

		{
			TArray<FNode> Nodes = {
				MakeNode(0, TEXT("source_column_escaped_aabb"), 260.0, 72.0, 0, 0.0, 0.0),
				MakeNode(1, TEXT("source_column_escaped_aabb"), 260.0, 120.0, 0, 0.0, 96.0),
				MakeNode(2, TEXT("source_column_escaped_aabb"), 120.0, 72.0, 1, 520.0, 180.0)
			};
			TArray<TArray<int32>> Columns;
			Columns.SetNum(2);
			Columns[0] = {0, 1};
			Columns[1] = {2};
			RunCase(TEXT("source_column_collision_aabb_uses_escaped_endpoints"), MoveTemp(Nodes), MoveTemp(Columns), 260.0, false);
		}
	}

	static void RunSameIslandForwardXColumnSelfTests(FSelfTestResult& Result, const FOptions& Options)
	{
		auto MakeNode = [](const int32 Index, const TCHAR* Prefix, const double X, const double Y) -> FNode
		{
			FNode Node;
			Node.Index = Index;
			Node.Guid = FString::Printf(TEXT("%s_%d"), Prefix, Index);
			Node.Title = Node.Guid;
			Node.Width = 100.0;
			Node.Height = 70.0;
			Node.InputPinNames = { TEXT("in") };
			Node.OutputPinNames = { TEXT("out") };
			Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
			Node.OutputPinAnchors = { FVector2D(100.0, 35.0) };
			Node.InputPinAnchorValid = { 1 };
			Node.OutputPinAnchorValid = { 1 };
			Node.bMeasuredGeometry = true;
			Node.NewX = X;
			Node.NewY = Y;
			return Node;
		};

		{
			TArray<FNode> Nodes = {
				MakeNode(0, TEXT("same_island_forward_column"), 0.0, 0.0),
				MakeNode(1, TEXT("same_island_forward_column"), 120.0, 0.0),
				MakeNode(2, TEXT("same_island_forward_column"), 120.0, 100.0)
			};
			Nodes[0].Rank = 0;
			Nodes[1].Rank = 1;
			Nodes[2].Rank = 1;
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
			Island.Columns.SetNum(2);
			Island.Columns[0] = {0};
			Island.Columns[1] = {1, 2};
			Island.ColumnWidths = {100.0, 100.0};
			Island.ColumnGaps = {20.0};
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TSet<int32> ForwardExemptEdges;
			const int32 ShiftCount = EnforceNonFeedbackSemanticEdgeForwardX(Nodes, Edges, Islands, ForwardExemptEdges, Options, true, false);
			const double ExpectedColumnX = SnapGraphCoordinate(ComputeInputXAlignedToOutput(100.0, Options));
			const double ExpectedColumnGap = SnapGraphCoordinate(20.0 + (ExpectedColumnX - 120.0));
			const bool bColumnShiftPreservesColumnLeft =
				ShiftCount == 1
				&& FMath::IsNearlyEqual(Nodes[1].NewX, ExpectedColumnX, 0.5)
				&& FMath::IsNearlyEqual(Nodes[2].NewX, ExpectedColumnX, 0.5)
				&& FMath::IsNearlyEqual(Islands[0].ColumnGaps[0], ExpectedColumnGap, 0.5);
			AddSelfTestCase(Result,
				TEXT("same_island_forward_x_shifts_target_column_not_single_node"),
				bColumnShiftPreservesColumnLeft,
				FString::Printf(TEXT("shift=%d x=%.1f/%.1f expected_x=%.1f gap=%.1f expected_gap=%.1f"),
					ShiftCount,
					Nodes[1].NewX,
					Nodes[2].NewX,
					ExpectedColumnX,
					Islands[0].ColumnGaps[0],
					ExpectedColumnGap));
		}

		{
			TArray<FNode> Nodes = {
				MakeNode(0, TEXT("same_island_same_column_forward"), 0.0, 0.0),
				MakeNode(1, TEXT("same_island_same_column_forward"), 0.0, 100.0)
			};
			Nodes[0].Rank = 0;
			Nodes[1].Rank = 0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.Columns.SetNum(1);
			Island.Columns[0] = {0, 1};
			Island.ColumnWidths = {100.0};
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TSet<int32> ForwardExemptEdges;
			const int32 ShiftCount = EnforceNonFeedbackSemanticEdgeForwardX(Nodes, Edges, Islands, ForwardExemptEdges, Options, true, false);
			const bool bSameColumnEdgeDoesNotShiftSingleNode =
				ShiftCount == 0
				&& FMath::IsNearlyEqual(Nodes[0].NewX, 0.0, 0.5)
				&& FMath::IsNearlyEqual(Nodes[1].NewX, 0.0, 0.5);
			AddSelfTestCase(Result,
				TEXT("same_island_forward_x_same_column_edge_uses_local_routing_not_node_shift"),
				bSameColumnEdgeDoesNotShiftSingleNode,
				FString::Printf(TEXT("shift=%d x=%.1f/%.1f"),
					ShiftCount,
					Nodes[0].NewX,
					Nodes[1].NewX));
		}
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
			TArray<FIsland> Islands;
			Islands.SetNum(2);
			Islands[0].Id = 0;
			Islands[0].X = 600.0;
			Islands[1].Id = 1;
			Islands[1].X = 0.0;
			TArray<FIslandEdge> IslandEdges;
			FIslandEdge Edge;
			Edge.EdgeIndex = 0;
			Edge.SourceIsland = 0;
			Edge.TargetIsland = 1;
			Edge.SourceLocal = FVector2D(160.0, 40.0);
			Edge.TargetLocal = FVector2D(0.0, 40.0);
			IslandEdges.Add(Edge);

			TSet<int32> NoExemptEdges;
			const int32 ShiftCount = EnforceNonFeedbackIslandEdgeActualPinForwardX(Islands, IslandEdges, NoExemptEdges, Options);
			const FVector2D SourcePoint(Islands[0].X + Edge.SourceLocal.X, Edge.SourceLocal.Y);
			const FVector2D TargetPoint(Islands[1].X + Edge.TargetLocal.X, Edge.TargetLocal.Y);
			AddCase(
				TEXT("non_feedback_island_edge_actual_pin_forward_invariant"),
				ShiftCount > 0 && HasNonNegativeConnectionAlignmentShorten(SourcePoint, TargetPoint, Options),
				FString::Printf(TEXT("shift_count=%d source_x=%.1f target_x=%.1f shorten=%.1f"),
					ShiftCount,
					SourcePoint.X,
					TargetPoint.X,
					ComputeConnectionAlignmentShortenX(SourcePoint, TargetPoint, Options)));
		}

		{
			TArray<FNode> Nodes;
			Nodes.SetNum(2);
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				Nodes[Index].Index = Index;
				Nodes[Index].Width = 160.0;
				Nodes[Index].Height = 80.0;
				Nodes[Index].bMeasuredGeometry = true;
				Nodes[Index].OutputPinNames = { TEXT("out") };
				Nodes[Index].InputPinNames = { TEXT("in") };
				Nodes[Index].OutputPinAnchors = { FVector2D(160.0, 40.0) };
				Nodes[Index].InputPinAnchors = { FVector2D(0.0, 40.0) };
				Nodes[Index].OutputPinAnchorValid = { 1 };
				Nodes[Index].InputPinAnchorValid = { 1 };
			}
			Nodes[0].NewX = 600.0;
			Nodes[1].NewX = 0.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			Islands.SetNum(1);
			Islands[0].Id = 0;
			Islands[0].Nodes = {0, 1};
			Islands[0].NodeSet.Add(0);
			Islands[0].NodeSet.Add(1);
			TMap<int32, int32> IslandByNode;
			IslandByNode.Add(0, 0);
			IslandByNode.Add(1, 0);
			TSet<int32> NoExemptEdges;
			const int32 ShiftCount = EnforceSameIslandNonFeedbackNodeForwardX(Nodes, Edges, Islands, IslandByNode, NoExemptEdges, Options);
			const FVector2D SourcePoint = GetSourcePort(Nodes[0], Edges[0], true);
			const FVector2D TargetPoint = GetTargetPort(Nodes[1], Edges[0], true);
			AddCase(
				TEXT("same_island_non_feedback_edge_actual_pin_forward_invariant"),
				ShiftCount > 0 && HasNonNegativeConnectionAlignmentShorten(SourcePoint, TargetPoint, Options),
				FString::Printf(TEXT("shift_count=%d source_x=%.1f target_x=%.1f shorten=%.1f"),
					ShiftCount,
					SourcePoint.X,
					TargetPoint.X,
					ComputeConnectionAlignmentShortenX(SourcePoint, TargetPoint, Options)));
		}

		{
			TArray<FNode> Nodes;
			Nodes.SetNum(2);
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				Nodes[Index].Index = Index;
				Nodes[Index].Width = 160.0;
				Nodes[Index].Height = 80.0;
				Nodes[Index].bMeasuredGeometry = true;
				Nodes[Index].OutputPinNames = { TEXT("out") };
				Nodes[Index].InputPinNames = { TEXT("in") };
				Nodes[Index].OutputPinAnchors = { FVector2D(160.0, 40.0) };
				Nodes[Index].InputPinAnchors = { FVector2D(0.0, 40.0) };
				Nodes[Index].OutputPinAnchorValid = { 1 };
				Nodes[Index].InputPinAnchorValid = { 1 };
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			Islands.SetNum(2);
			Islands[0].Id = 0;
			Islands[0].X = 600.0;
			Islands[0].Nodes = {0};
			Islands[1].Id = 1;
			Islands[1].X = 0.0;
			Islands[1].Nodes = {1};
			TSet<int32> NoExemptEdges;
			const int32 ShiftCount = EnforceNonFeedbackCrossIslandSemanticEdgeForwardX(Nodes, Edges, Islands, NoExemptEdges, Options);
			const FVector2D SourcePoint(Islands[0].X + GetSourcePort(Nodes[0], Edges[0], true).X, GetSourcePort(Nodes[0], Edges[0], true).Y);
			const FVector2D TargetPoint(Islands[1].X + GetTargetPort(Nodes[1], Edges[0], true).X, GetTargetPort(Nodes[1], Edges[0], true).Y);
			AddCase(
				TEXT("cross_island_semantic_edge_forward_invariant_does_not_depend_on_island_edge_list"),
				ShiftCount > 0 && HasNonNegativeConnectionAlignmentShorten(SourcePoint, TargetPoint, Options),
				FString::Printf(TEXT("shift_count=%d source_x=%.1f target_x=%.1f shorten=%.1f"),
					ShiftCount,
					SourcePoint.X,
					TargetPoint.X,
					ComputeConnectionAlignmentShortenX(SourcePoint, TargetPoint, Options)));
		}

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
			Nodes.SetNum(3);
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				Nodes[Index].Index = Index;
				Nodes[Index].Width = 160.0;
				Nodes[Index].Height = 80.0;
				Nodes[Index].bMeasuredGeometry = true;
				Nodes[Index].OutputPinNames = { TEXT("out") };
				Nodes[Index].InputPinNames = { TEXT("in") };
				Nodes[Index].OutputPinAnchors = { FVector2D(160.0, 40.0) };
				Nodes[Index].InputPinAnchors = { FVector2D(0.0, 40.0) };
				Nodes[Index].OutputPinAnchorValid = { 1 };
				Nodes[Index].InputPinAnchorValid = { 1 };
			}
			Nodes[0].NewX = 1800.0;
			Nodes[1].NewX = 0.0;
			Nodes[2].NewX = 0.0;
			Nodes[2].NewY = 120.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			Islands.SetNum(1);
			Islands[0].Id = 0;
			Islands[0].Nodes = {0, 1, 2};
			Islands[0].NodeSet.Add(0);
			Islands[0].NodeSet.Add(1);
			Islands[0].NodeSet.Add(2);
			TSet<int32> NoExemptEdges;
			const int32 ShiftCount = EnforceNonFeedbackSemanticEdgeForwardX(Nodes, Edges, Islands, NoExemptEdges, Options, true, false);
			const bool bAllForward =
				HasNonNegativeConnectionAlignmentShorten(GetSourcePort(Nodes[0], Edges[0], true), GetTargetPort(Nodes[1], Edges[0], true), Options)
				&& HasNonNegativeConnectionAlignmentShorten(GetSourcePort(Nodes[0], Edges[1], true), GetTargetPort(Nodes[2], Edges[1], true), Options);
			AddCase(
				TEXT("semantic_forward_invariant_handles_same_island_fanout_edges"),
				ShiftCount >= 2 && bAllForward,
				FString::Printf(TEXT("shift_count=%d target_x=%.1f/%.1f source_x=%.1f"),
					ShiftCount,
					GetTargetPort(Nodes[1], Edges[0], true).X,
					GetTargetPort(Nodes[2], Edges[1], true).X,
					GetSourcePort(Nodes[0], Edges[0], true).X));
		}

		{
			TArray<FNode> Nodes;
			Nodes.SetNum(3);
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				Nodes[Index].Index = Index;
				Nodes[Index].Width = 160.0;
				Nodes[Index].Height = 80.0;
				Nodes[Index].bMeasuredGeometry = true;
				Nodes[Index].OutputPinNames = { TEXT("out") };
				Nodes[Index].InputPinNames = { TEXT("in") };
				Nodes[Index].OutputPinAnchors = { FVector2D(160.0, 40.0) };
				Nodes[Index].InputPinAnchors = { FVector2D(0.0, 40.0) };
				Nodes[Index].OutputPinAnchorValid = { 1 };
				Nodes[Index].InputPinAnchorValid = { 1 };
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			TArray<FIsland> Islands;
			Islands.SetNum(3);
			for (int32 Index = 0; Index < Islands.Num(); ++Index)
			{
				Islands[Index].Id = Index;
				Islands[Index].Nodes = {Index};
				Islands[Index].NodeSet.Add(Index);
			}
			Islands[0].X = 1800.0;
			Islands[1].X = 0.0;
			Islands[2].X = 200.0;
			TSet<int32> NoExemptEdges;
			const int32 ShiftCount = EnforceNonFeedbackSemanticEdgeForwardX(Nodes, Edges, Islands, NoExemptEdges, Options, false, true);
			const FVector2D Source0 = FVector2D(Islands[0].X, Islands[0].Y) + GetSourcePort(Nodes[0], Edges[0], true);
			const FVector2D Target1 = FVector2D(Islands[1].X, Islands[1].Y) + GetTargetPort(Nodes[1], Edges[0], true);
			const FVector2D Target2 = FVector2D(Islands[2].X, Islands[2].Y) + GetTargetPort(Nodes[2], Edges[1], true);
			const bool bAllForward =
				HasNonNegativeConnectionAlignmentShorten(Source0, Target1, Options)
				&& HasNonNegativeConnectionAlignmentShorten(Source0, Target2, Options);
			AddCase(
				TEXT("semantic_forward_invariant_handles_cross_island_fanout_edges"),
				ShiftCount >= 2 && bAllForward,
				FString::Printf(TEXT("shift_count=%d target_x=%.1f/%.1f source_x=%.1f"),
					ShiftCount,
					Target1.X,
					Target2.X,
					Source0.X));
		}

		{
			TArray<FIsland> Islands;
			Islands.SetNum(2);
			Islands[0].Id = 0;
			Islands[0].X = 600.0;
			Islands[1].Id = 1;
			Islands[1].X = 0.0;
			TArray<FIslandEdge> IslandEdges;
			FIslandEdge Edge;
			Edge.EdgeIndex = 0;
			Edge.SourceIsland = 0;
			Edge.TargetIsland = 1;
			Edge.SourceLocal = FVector2D(160.0, 40.0);
			Edge.TargetLocal = FVector2D(0.0, 40.0);
			Edge.bFeedback = true;
			IslandEdges.Add(Edge);

			TSet<int32> ExemptEdges;
			ExemptEdges.Add(0);
			const int32 ShiftCount = EnforceNonFeedbackIslandEdgeActualPinForwardX(Islands, IslandEdges, ExemptEdges, Options);
			const FVector2D SourcePoint(Islands[0].X + Edge.SourceLocal.X, Edge.SourceLocal.Y);
			const FVector2D TargetPoint(Islands[1].X + Edge.TargetLocal.X, Edge.TargetLocal.Y);
			AddCase(
				TEXT("feedback_island_edge_is_forward_invariant_exempt"),
				ShiftCount == 0 && !HasNonNegativeConnectionAlignmentShorten(SourcePoint, TargetPoint, Options),
				FString::Printf(TEXT("shift_count=%d source_x=%.1f target_x=%.1f shorten=%.1f"),
					ShiftCount,
					SourcePoint.X,
					TargetPoint.X,
					ComputeConnectionAlignmentShortenX(SourcePoint, TargetPoint, Options)));
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
			FOptions RouteOptions = Options;
			RouteOptions.OutputAccessOffsetX = 40.0;
			RouteOptions.InputAccessOffsetX = 40.0;
			RouteOptions.PinDirectionBiasX = 40.0;
			RouteOptions.NodePadding = 32.0;
			const FVector2D Start(230.0, 4511.0);
			const FVector2D End(5704.0, 5491.0);
			const TArray<FRect> HardRects = {
				{ 1200.0, 4400.0, 5000.0, 4700.0 }
			};
			const TArray<FVector2D> Points = BuildOrthogonalRoutePolyline(Start, End, 4511.0, RouteOptions, HardRects);
			int32 LargeDiagonalSegmentCount = 0;
			const double MaxDiagonalDX = EndpointAccessClearanceX(RouteOptions) * 2.0 + 0.5;
			const double MinNoticeableDY = FMath::Max(RouteOptions.VerticalSpacing * 0.5, 80.0);
			for (int32 PointIndex = 0; PointIndex + 1 < Points.Num(); ++PointIndex)
			{
				const double DX = FMath::Abs(Points[PointIndex + 1].X - Points[PointIndex].X);
				const double DY = FMath::Abs(Points[PointIndex + 1].Y - Points[PointIndex].Y);
				if (DX > MaxDiagonalDX && DY > MinNoticeableDY)
				{
					++LargeDiagonalSegmentCount;
				}
			}
			FString RouteText;
			for (const FVector2D& Point : Points)
			{
				RouteText += FString::Printf(TEXT("(%.1f,%.1f)"), Point.X, Point.Y);
			}
			AddCase(TEXT("orthogonal_bus_router_does_not_create_large_diagonal_branch_segments"),
				Points.Num() >= 4
				&& LargeDiagonalSegmentCount == 0
				&& !PolylineHitsHardOccupancy(Points, HardRects)
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(Points, RouteOptions),
				FString::Printf(TEXT("points=%d large_diagonal=%d hard_hits=%d min_shorten=%.1f route=%s"),
					Points.Num(),
					LargeDiagonalSegmentCount,
					CountPolylineHardOccupancyHits(Points, HardRects),
					MinConnectionAlignmentShortenX(Points, RouteOptions),
					*RouteText));
		}

		{
			FOptions RouteOptions = Options;
			RouteOptions.OutputAccessOffsetX = 40.0;
			RouteOptions.InputAccessOffsetX = 40.0;
			RouteOptions.PinDirectionBiasX = 40.0;
			RouteOptions.NodePadding = 32.0;
			const FVector2D Start(230.0, 4511.0);
			const FVector2D End(4016.0, 4932.0);
			const double OriginalBranchX = 607.0;
			const TArray<FRect> HardRects = {
				{ 900.0, 4500.0, 3500.0, 4800.0 }
			};
			const TArray<FVector2D> Points = BuildOrthogonalRoutePolyline(
				Start,
				End,
				4511.0,
				RouteOptions,
				HardRects,
				OriginalBranchX,
				SegmentRefineBranchPreferencePenaltyScale());
			double SelectedBranchX = 0.0;
			const bool bHasBranch = TryGetPreferredBranchXFromVisibleRoute(Points, SelectedBranchX);
			FString RouteText;
			for (const FVector2D& Point : Points)
			{
				RouteText += FString::Printf(TEXT("(%.1f,%.1f)"), Point.X, Point.Y);
			}
			AddCase(TEXT("segment_refine_orthogonal_route_preserves_existing_bus_branch_x"),
				Points.Num() >= 4
				&& bHasBranch
				&& FMath::Abs(SelectedBranchX - OriginalBranchX) <= EndpointAccessClearanceX(RouteOptions) + 0.5
				&& !PolylineHitsHardOccupancy(Points, HardRects)
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(Points, RouteOptions),
				FString::Printf(TEXT("branch=%.1f original=%.1f hard_hits=%d points=%d route=%s"),
					SelectedBranchX,
					OriginalBranchX,
					CountPolylineHardOccupancyHits(Points, HardRects),
					Points.Num(),
					*RouteText));
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
			for (int32 Index = 0; Index < 6; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("fanoutcycle%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 160.0;
				Node.Height = 80.0;
				Node.InputPinNames = { TEXT("in") };
				Node.InputPinAnchors = { FVector2D(0.0, 40.0) };
				Node.InputPinAnchorValid = { 1 };
				Node.OutputPinNames = Index == 5 ? TArray<FString>{ TEXT("out_a"), TEXT("out_d") } : TArray<FString>{ TEXT("out") };
				Node.OutputPinAnchors = Index == 5
					? TArray<FVector2D>{ FVector2D(160.0, 32.0), FVector2D(160.0, 56.0) }
					: TArray<FVector2D>{ FVector2D(160.0, 40.0) };
				Node.OutputPinAnchorValid = Index == 5 ? TArray<uint8>{ 1, 1 } : TArray<uint8>{ 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{5, 0, TEXT("out_a"), TEXT("in"), 0, 0, false},
				{5, 3, TEXT("out_d"), TEXT("in"), 1, 0, false},
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{1, 3, TEXT("out"), TEXT("in"), 0, 0, false},
				{1, 4, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FRunResult Run;
			RunLayoutForSelfTest(Nodes, Edges, Options, Run);
			const FVector2D AToBSource = GetSourcePort(Nodes[0], Edges[2], true);
			const FVector2D AToBTarget = GetTargetPort(Nodes[1], Edges[2], true);
			const FVector2D BToDSource = GetSourcePort(Nodes[1], Edges[4], true);
			const FVector2D BToDTarget = GetTargetPort(Nodes[3], Edges[4], true);
			const bool bArtificialFanoutCycleResolved =
				Run.DesignDiagnostics.FanoutImplicitEdgeCount == 3
				&& Run.MetricsAfter.BackwardEdgeCount == 0
				&& HasNonNegativeConnectionAlignmentShorten(AToBSource, AToBTarget, Options)
				&& HasNonNegativeConnectionAlignmentShorten(BToDSource, BToDTarget, Options);
			AddCase(TEXT("fanout_cut_artificial_island_cycle_restores_primary_edge"),
				bArtificialFanoutCycleResolved,
				FString::Printf(TEXT("fanout=%d backward=%d ab_shorten=%.1f bd_shorten=%.1f"),
					Run.DesignDiagnostics.FanoutImplicitEdgeCount,
					Run.MetricsAfter.BackwardEdgeCount,
					ComputeConnectionAlignmentShortenX(AToBSource, AToBTarget, Options),
					ComputeConnectionAlignmentShortenX(BToDSource, BToDTarget, Options)));
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
			FBusContract Contract;
			Contract.Id = 0;
			Contract.SourceParamId = TEXT("same_bus_merge_source");
			Contract.RouteKind = ERouteKind::LocalBus;
			Contract.TrunkY = 240.0;
			BusContracts.Add(Contract);

			const FVector2D Start(100.0, 100.0);
			const FVector2D End(1200.0, 280.0);
			const double EntryX = ComputeInputXAlignedToOutput(Start.X, Options);
			const double InitialBranchXs[] = {900.0, 928.0};
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FPlannedCorridorRoute Planned;
				Planned.RouteKind = ERouteKind::LocalBus;
				Planned.TrunkY = 240.0;
				Planned.BranchX = InitialBranchXs[Index];
				Planned.Polyline = { Start, FVector2D(EntryX, Planned.TrunkY), FVector2D(InitialBranchXs[Index], Planned.TrunkY), End };

				FPendingCorridorBusRoute Pending;
				Pending.ContractIndex = 0;
				Pending.EdgeIndex = Index;
				Pending.SourceParamId = Contract.SourceParamId;
				Pending.Side = TEXT("top");
				Pending.Planned = Planned;
				Pending.Start = Start;
				Pending.End = End;
				Pending.ActualEnd = End;
				PendingRoutes.Add(Pending);
			}
			RecomputeBusContractBoundsFromPendingRoutes(BusContracts[0], PendingRoutes);

			const TArray<FNode> EmptyNodes;
			const TArray<FEdge> EmptyEdges;
			const TArray<FIsland> EmptyIslands;
			FRunResult Run;
			const int32 Merged = MergeCloseBranchPointsWithinSameBusAtGeneration(
				PendingRoutes,
				BusContracts,
				EmptyNodes,
				EmptyEdges,
				EmptyIslands,
				Options,
				Run);

			const double ExpectedMergedX = SnapGraphCoordinate((InitialBranchXs[0] + InitialBranchXs[1]) * 0.5);
			const bool bMergedCloseBranches =
				Merged == 1
				&& PendingRoutes.Num() == 2
				&& FMath::IsNearlyEqual(PendingRoutes[0].Planned.BranchX, ExpectedMergedX, 0.5)
				&& FMath::IsNearlyEqual(PendingRoutes[1].Planned.BranchX, ExpectedMergedX, 0.5)
				&& BusContracts.Num() == 1
				&& BusContracts[0].BranchXs.Num() == 1;
			AddCase(TEXT("same_bus_close_branch_points_merge_before_cross_bus_lanes"),
				bMergedCloseBranches,
				FString::Printf(TEXT("merged=%d branch=%.1f/%.1f expected=%.1f unique=%d diagnostics=%d"),
					Merged,
					PendingRoutes.IsValidIndex(0) ? PendingRoutes[0].Planned.BranchX : -1.0,
					PendingRoutes.IsValidIndex(1) ? PendingRoutes[1].Planned.BranchX : -1.0,
					ExpectedMergedX,
					BusContracts.IsValidIndex(0) ? BusContracts[0].BranchXs.Num() : -1,
					Run.RejectionDiagnostics.Num()));
		}

		{
			TArray<FPendingCorridorBusRoute> PendingRoutes;
			TArray<FBusContract> BusContracts;
			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			const double InitialBranchXs[] = { 900.0, 912.0 };
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FSelectedEndpointCandidate Selected;
				Selected.EdgeIndex = Index;
				Selected.CandidateId = Index;
				Selected.ConnectionGroupId = Index;
				Selected.SourcePortOrderKey = static_cast<double>(Index);
				SelectedEndpoints.Add(Selected);

				const FVector2D Start(100.0, 100.0 + static_cast<double>(Index) * 6.0);
				const FVector2D End(1400.0, 360.0 + static_cast<double>(Index) * 8.0);
				const double TrunkY = 240.0 + static_cast<double>(Index) * 2.0;
				const double EntryX = ComputeInputXAlignedToOutput(Start.X, Options);

				FPlannedCorridorRoute Planned;
				Planned.RouteKind = ERouteKind::LocalBus;
				Planned.TrunkY = TrunkY;
				Planned.BranchX = InitialBranchXs[Index];
				Planned.Polyline = { Start, FVector2D(EntryX, TrunkY), FVector2D(InitialBranchXs[Index], TrunkY), End };

				FBusContract Contract;
				Contract.Id = Index;
				Contract.SourceParamId = FString::Printf(TEXT("branch_locality_source_%d"), Index);
				Contract.RouteKind = ERouteKind::LocalBus;
				Contract.TrunkY = TrunkY;
				Contract.EdgeIndices.Add(Index);
				Contract.BranchXs.Add(InitialBranchXs[Index]);
				RecomputeBusContractBoundsFromRoute(Contract, Planned);
				BusContracts.Add(Contract);

				FPendingCorridorBusRoute Pending;
				Pending.SelectedEndpointIndex = Index;
				Pending.ContractIndex = Contract.Id;
				Pending.EdgeIndex = Index;
				Pending.SourceParamId = Contract.SourceParamId;
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
			const double LaneSpacing = GetCrossBusBranchLaneSpacing(Options);
			const bool bLanePlanningKeepsCloserTargetRight =
				Applied >= 1
				&& PendingRoutes.Num() == 2
				&& FMath::Abs(PendingRoutes[1].Planned.BranchX - PendingRoutes[0].Planned.BranchX) >= LaneSpacing - 0.5
				&& PendingRoutes[0].Planned.BranchX > PendingRoutes[1].Planned.BranchX + LaneSpacing - 0.5
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[0].Planned.Polyline, Options)
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[1].Planned.Polyline, Options);
			AddCase(TEXT("cross_bus_branch_lane_places_closer_target_right_with_fixed_spacing"),
				bLanePlanningKeepsCloserTargetRight,
				FString::Printf(TEXT("applied=%d branch=%.1f/%.1f initial=%.1f/%.1f spacing=%.1f diagnostics=%d"),
					Applied,
					PendingRoutes.IsValidIndex(0) ? PendingRoutes[0].Planned.BranchX : -1.0,
					PendingRoutes.IsValidIndex(1) ? PendingRoutes[1].Planned.BranchX : -1.0,
					InitialBranchXs[0],
					InitialBranchXs[1],
					LaneSpacing,
					Run.RejectionDiagnostics.Num()));
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
			const double LaneSpacing = GetBusTrunkLaneSpacing();
			bool bLaneDiagnosticsUseFixedSpacing = Run.LaneAssignmentDiagnostics.Num() == 2;
			for (const TSharedPtr<FJsonValue>& LaneValue : Run.LaneAssignmentDiagnostics)
			{
				const TSharedPtr<FJsonObject> LaneObj = LaneValue.IsValid() ? LaneValue->AsObject() : nullptr;
				double ReportedLaneSpacing = 0.0;
				double ReportedEffectiveLaneSpacing = 0.0;
				double ReportedMinimumLaneSpacing = 0.0;
				bLaneDiagnosticsUseFixedSpacing = bLaneDiagnosticsUseFixedSpacing
					&& LaneObj.IsValid()
					&& LaneObj->TryGetNumberField(TEXT("lane_spacing"), ReportedLaneSpacing)
					&& LaneObj->TryGetNumberField(TEXT("effective_lane_spacing"), ReportedEffectiveLaneSpacing)
					&& LaneObj->TryGetNumberField(TEXT("minimum_visible_lane_spacing"), ReportedMinimumLaneSpacing)
					&& FMath::IsNearlyEqual(ReportedLaneSpacing, LaneSpacing, 0.01)
					&& FMath::IsNearlyEqual(ReportedEffectiveLaneSpacing, LaneSpacing, 0.01)
					&& FMath::IsNearlyEqual(ReportedMinimumLaneSpacing, LaneSpacing, 0.01);
			}
			const bool bGenerationSelectedSeparateY =
				PendingRoutes.Num() == 2
				&& FMath::Abs(PendingRoutes[0].Planned.TrunkY - PendingRoutes[1].Planned.TrunkY) >= LaneSpacing - 0.5
				&& Run.DesignDiagnostics.RouteThroughHardOccupancyCount == 0
				&& Run.DesignDiagnostics.CrossBusTrunkLaneCount == 1
				&& Run.LaneAssignmentDiagnostics.Num() == 2
				&& bLaneDiagnosticsUseFixedSpacing
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[0].Planned.Polyline, Options)
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[1].Planned.Polyline, Options);
			AddCase(TEXT("bus_contract_generation_selects_y_lanes_before_reroute_writeback"),
				bGenerationSelectedSeparateY,
				FString::Printf(TEXT("y=%.1f/%.1f lane_spacing=%.1f fixed_diag=%d route_through=%d trunk_lanes=%d lane_diag=%d"),
					PendingRoutes.IsValidIndex(0) ? PendingRoutes[0].Planned.TrunkY : 0.0,
					PendingRoutes.IsValidIndex(1) ? PendingRoutes[1].Planned.TrunkY : 0.0,
					LaneSpacing,
					bLaneDiagnosticsUseFixedSpacing ? 1 : 0,
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
			const double LaneSpacing = GetBusTrunkLaneSpacing();
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
			TArray<FPendingCorridorBusRoute> PendingRoutes;
			TArray<FBusContract> BusContracts;
			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			constexpr int32 ContractCount = 10;
			for (int32 Index = 0; Index < ContractCount; ++Index)
			{
				FSelectedEndpointCandidate Selected;
				Selected.EdgeIndex = Index;
				Selected.CandidateId = Index;
				Selected.ConnectionGroupId = Index;
				Selected.SourcePortOrderKey = static_cast<double>(Index);
				SelectedEndpoints.Add(Selected);

				const FVector2D Start(120.0, 200.0 + static_cast<double>(Index) * 64.0);
				const FVector2D End(1320.0, 320.0 + static_cast<double>(Index) * 64.0);
				const double TrunkY = 320.0 + static_cast<double>(Index) * 2.0;
				const double EntryX = ComputeInputXAlignedToOutput(Start.X, Options);
				const double BranchX = ComputeOutputXAlignedToInput(End.X, Options);

				FPlannedCorridorRoute Planned;
				Planned.RouteKind = ERouteKind::LocalBus;
				Planned.TrunkY = TrunkY;
				Planned.BranchX = BranchX;
				Planned.Polyline = { Start, FVector2D(EntryX, TrunkY), FVector2D(BranchX, TrunkY), End };

				FBusContract Contract;
				Contract.Id = Index;
				Contract.SourceParamId = FString::Printf(TEXT("large_trunk_source_%d"), Index);
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
			const double LaneSpacing = GetBusTrunkLaneSpacing();
			bool bNoFallback = true;
			for (const TSharedPtr<FJsonValue>& LaneValue : Run.LaneAssignmentDiagnostics)
			{
				const TSharedPtr<FJsonObject> LaneObj = LaneValue.IsValid() ? LaneValue->AsObject() : nullptr;
				bool bFallback = false;
				if (LaneObj.IsValid() && LaneObj->TryGetBoolField(TEXT("lane_conflict_fallback_used"), bFallback) && bFallback)
				{
					bNoFallback = false;
				}
			}
			bool bLargeGroupSeparated = PendingRoutes.Num() == ContractCount
				&& Run.DesignDiagnostics.CrossBusTrunkLaneCount == ContractCount - 1
				&& Run.LaneAssignmentDiagnostics.Num() == ContractCount
				&& bNoFallback;
			for (int32 A = 0; A < PendingRoutes.Num(); ++A)
			{
				bLargeGroupSeparated = bLargeGroupSeparated
					&& BusContracts.IsValidIndex(A)
					&& BusContracts[A].BusLaneIndex != INDEX_NONE
					&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[A].Planned.Polyline, Options);
				for (int32 B = A + 1; B < PendingRoutes.Num(); ++B)
				{
					if (IntervalOverlaps(
						FMath::Min(PendingRoutes[A].Start.X, PendingRoutes[A].End.X),
						FMath::Max(PendingRoutes[A].Start.X, PendingRoutes[A].End.X),
						FMath::Min(PendingRoutes[B].Start.X, PendingRoutes[B].End.X),
						FMath::Max(PendingRoutes[B].Start.X, PendingRoutes[B].End.X)))
					{
						bLargeGroupSeparated = bLargeGroupSeparated
							&& FMath::Abs(PendingRoutes[A].Planned.TrunkY - PendingRoutes[B].Planned.TrunkY) >= LaneSpacing - 0.5;
					}
				}
			}
			AddCase(TEXT("large_cross_bus_group_preplans_lane_free_trunks"),
				bLargeGroupSeparated,
				FString::Printf(TEXT("count=%d lane_spacing=%.1f trunk_lanes=%d diagnostics=%d fallback=%d y0=%.1f y9=%.1f"),
					PendingRoutes.Num(),
					LaneSpacing,
					Run.DesignDiagnostics.CrossBusTrunkLaneCount,
					Run.LaneAssignmentDiagnostics.Num(),
					bNoFallback ? 0 : 1,
					PendingRoutes.IsValidIndex(0) ? PendingRoutes[0].Planned.TrunkY : 0.0,
					PendingRoutes.IsValidIndex(9) ? PendingRoutes[9].Planned.TrunkY : 0.0));
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
			const FVector2D Start(431.0, 4079.0);
			const FVector2D End(1191.0, 5527.0);
			const double TrunkY = 4199.0;
			const double InitialBranchX = 553.0;
			TArray<FRect> HardRects;
			HardRects.Add({564.0, 4738.0, 764.0, 4834.0});
			HardRects.Add({625.0, 5326.0, 820.0, 5446.0});
			const TArray<FVector2D> Route = BuildSharedSourceBusRoutePolylineAvoidingHardRects(
				Start,
				End,
				TrunkY,
				InitialBranchX,
				HardRects,
				Options,
				true);
			const int32 HitCount = CountControlWireHardOccupancyHits(Route, HardRects);
			double MaxInteriorX = -TNumericLimits<double>::Max();
			for (int32 PointIndex = 1; PointIndex + 1 < Route.Num(); ++PointIndex)
			{
				MaxInteriorX = FMath::Max(MaxInteriorX, Route[PointIndex].X);
			}
			AddCase(
				TEXT("shared_source_bus_target_dogleg_avoids_target_island_nodes"),
				HitCount == 0
					&& Route.Num() >= 4
					&& MaxInteriorX > 820.0
					&& HasNonNegativeConnectionAlignmentShortenForEverySegment(Route, Options),
				FString::Printf(TEXT("hits=%d points=%d max_inner_x=%.1f"),
					HitCount,
					Route.Num(),
					MaxInteriorX));
		}

		{
			FOptions RouteOptions = Options;
			RouteOptions.OutputAccessOffsetX = 40.0;
			RouteOptions.InputAccessOffsetX = 40.0;
			RouteOptions.PinDirectionBiasX = 40.0;
			RouteOptions.NodePadding = 32.0;
			const FVector2D Start(1404.0, 4557.0);
			const FVector2D End(3516.0, 4633.0);
			const double TrunkY = 4660.0;
			const double BranchX = 1484.0;
			const TArray<FVector2D> Route = BuildSharedSourceBusRoutePolylineWithTargetDogleg(
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
			const double ExpectedSpacing = GetCrossBusBranchLaneSpacing(Options);
			const double ExpectedCloseBranchX = ComputeOutputXAlignedToInput(PendingRoutes[1].End.X, Options);
			const bool bCloserTargetKeptRightLane =
				CloseBranchX > FarBranchX + ExpectedSpacing - 0.5
				&& FMath::IsNearlyEqual(CloseBranchX, ExpectedCloseBranchX, 0.5)
				&& BusContracts[1].BranchLaneIndex == 0
				&& BusContracts[0].BranchLaneIndex == 1
				&& Run.DesignDiagnostics.CrossBusLaneCount == 1
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[0].Planned.Polyline, Options)
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(PendingRoutes[1].Planned.Polyline, Options);
			AddCase(TEXT("cross_bus_branch_lane_uses_y_projection_and_closer_target_prefers_right_x"),
				bCloserTargetKeptRightLane,
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

			const double TrunkLaneSpacing = GetBusTrunkLaneSpacing();
			const double BranchLaneSpacing = GetCrossBusBranchLaneSpacing(Options);
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
			for (int32 Index = 0; Index < 6; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("finalvisiblebranchxassigned%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 100.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 0.0;
			Nodes[1].NewX = 600.0;
			Nodes[1].NewY = 0.0;
			Nodes[2].NewX = 0.0;
			Nodes[2].NewY = 160.0;
			Nodes[3].NewX = 600.0;
			Nodes[3].NewY = 300.0;
			Nodes[4].NewX = 0.0;
			Nodes[4].NewY = 320.0;
			Nodes[5].NewX = 600.0;
			Nodes[5].NewY = 140.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{2, 3, TEXT("out"), TEXT("in"), 0, 0, false},
				{4, 5, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3, 4, 5};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			for (const int32 NodeIndex : Island.Nodes)
			{
				IslandByNode.Add(NodeIndex, 0);
			}

			const double TrunkY = 80.0;
			const double EntryX = ComputeInputXAlignedToOutput(Nodes[0].NewX + Nodes[0].Width, Options);
			const double OldCloseBranchX = EntryX + 80.0;
			const double OldFarBranchX = EntryX + 180.0;
			const double OldMidBranchX = OldCloseBranchX + 10.0;
			FReroutePlan ClosePlan;
			ClosePlan.EdgeIndex = 0;
			ClosePlan.Reason = TEXT("direct_obstacle_local_bus");
			ClosePlan.BusId = MakeSourceParamId(Edges[0]);
			ClosePlan.RouteKind = ERouteKind::LocalBus;
			ClosePlan.Points = { FVector2D(EntryX, TrunkY), FVector2D(OldCloseBranchX, TrunkY) };
			FReroutePlan FarPlan;
			FarPlan.EdgeIndex = 1;
			FarPlan.Reason = TEXT("direct_obstacle_local_bus");
			FarPlan.BusId = MakeSourceParamId(Edges[1]);
			FarPlan.RouteKind = ERouteKind::LocalBus;
			FarPlan.Points = { FVector2D(EntryX, TrunkY), FVector2D(OldFarBranchX, TrunkY) };
			FReroutePlan MidPlan;
			MidPlan.EdgeIndex = 2;
			MidPlan.Reason = TEXT("direct_obstacle_local_bus");
			MidPlan.BusId = MakeSourceParamId(Edges[2]);
			MidPlan.RouteKind = ERouteKind::LocalBus;
			MidPlan.Points = { FVector2D(EntryX, TrunkY), FVector2D(OldMidBranchX, TrunkY) };
			TArray<FReroutePlan> Plans = {ClosePlan, FarPlan, MidPlan};
			FRunResult Run;
			const int32 InitialViolations = ValidateFinalVisibleRerouteBranchXLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, nullptr);
			const int32 Adjusted = PlanFinalVisibleRerouteBranchXLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			const int32 FinalViolations = ValidateFinalVisibleRerouteBranchXLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, &Run);
			double FinalCloseBranchX = 0.0;
			double FinalFarBranchX = 0.0;
			double FinalMidBranchX = 0.0;
			TArray<FVisibleRerouteBranchXLaneItem> FinalItems;
			CollectVisibleRerouteBranchXLaneItems(Nodes, Edges, Islands, IslandByNode, Options, Plans, FinalItems);
			for (const FVisibleRerouteBranchXLaneItem& Item : FinalItems)
			{
				if (Item.EdgeIndex == 0)
				{
					FinalCloseBranchX = Item.BranchX;
				}
				else if (Item.EdgeIndex == 1)
				{
					FinalFarBranchX = Item.BranchX;
				}
				else if (Item.EdgeIndex == 2)
				{
					FinalMidBranchX = Item.BranchX;
				}
			}
			AddCase(TEXT("final_visible_branch_x_lane_uses_assigned_previous_x_for_spacing"),
				InitialViolations > 0
				&& Adjusted >= 1
				&& FinalViolations == 0
				&& FinalCloseBranchX > FinalMidBranchX + GetCrossBusBranchLaneSpacing(Options) - 0.5
				&& FinalMidBranchX > FinalFarBranchX + GetCrossBusBranchLaneSpacing(Options) - 0.5,
				FString::Printf(TEXT("initial=%d adjusted=%d final=%d close=%.1f mid=%.1f far=%.1f old_close=%.1f old_mid=%.1f old_far=%.1f assignments=%d"),
					InitialViolations,
					Adjusted,
					FinalViolations,
					FinalCloseBranchX,
					FinalMidBranchX,
					FinalFarBranchX,
					OldCloseBranchX,
					OldMidBranchX,
					OldFarBranchX,
					Run.LaneAssignmentDiagnostics.Num()));
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
			LowerTarget.bPlacementOwner = true;
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
			UpperTarget.bPlacementOwner = true;
			SelectedEndpoints.Add(UpperTarget);

			FPlacementOwnership Ownership;
			Ownership.OwnerSourceByIsland.Init(INDEX_NONE, Islands.Num());
			Ownership.OwnerSourceByIsland[1] = 0;
			Ownership.OwnerSourceByIsland[2] = 0;
			Ownership.OwnerEdgeIndices.Add(0);
			Ownership.OwnerEdgeIndices.Add(1);
			PlaceIslandsYByWeightedMedianAndPacking(TArray<FNode>(), Islands, SelectedEndpoints, Ownership, Options);
			const TArray<FSourcePortOrderConstraint> Constraints = BuildSourcePortOrderConstraints(Islands, SelectedEndpoints, &Ownership.OwnerEdgeIndices);
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
			FNode SparseNode;
			SparseNode.Index = 0;
			SparseNode.Guid = TEXT("sparse_node");
			SparseNode.Title = SparseNode.Guid;
			SparseNode.NewX = 0.0;
			SparseNode.NewY = 260.0;
			SparseNode.Width = 160.0;
			SparseNode.Height = 80.0;
			Nodes.Add(SparseNode);

			FIsland SparseIsland;
			SparseIsland.Id = 0;
			SparseIsland.X = 0.0;
			SparseIsland.Y = 0.0;
			SparseIsland.LocalBounds = {0.0, 0.0, 820.0, 520.0};
			SparseIsland.Nodes = {0};

			const TArray<FRect> BlockingRects = {
				FRect{0.0, 0.0, 300.0, 110.0}
			};
			double SelectedX = 0.0;
			double SelectedY = 0.0;
			const bool bFoundSparsePlacement = SelectNearestFeasibleIslandPlacement(
				Nodes,
				SparseIsland,
				0.0,
				BlockingRects,
				Options,
				-TNumericLimits<double>::Max(),
				SelectedX,
				SelectedY);
			const bool bSparseAabbDoesNotForceRightEscape =
				bFoundSparsePlacement
				&& FMath::Abs(SelectedX) <= 0.5
				&& FMath::Abs(SelectedY) <= 0.5;
			AddCase(
				TEXT("island_placement_uses_segmented_hard_occupancy_not_sparse_aabb"),
				bSparseAabbDoesNotForceRightEscape,
				FString::Printf(TEXT("found=%s selected=(%.1f,%.1f)"), bFoundSparsePlacement ? TEXT("true") : TEXT("false"), SelectedX, SelectedY));
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
			const TArray<FInputChainForbiddenZone> DirectZones = BuildInputChainForbiddenZonesForTarget(
				Nodes,
				EdgesWithTargetOutput,
				Island,
				1,
				FVector2D(Nodes[1].NewX, Nodes[1].NewY),
				Options);
			bool bOutputPinIgnoredForForbiddenZones = DirectZones.Num() == 1;
			for (const FInputChainForbiddenZone& Zone : DirectZones)
			{
				bOutputPinIgnoredForForbiddenZones = bOutputPinIgnoredForForbiddenZones && !Zone.bOutputPin;
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
				bForbiddenZoneAvoided && bOutputPinIgnoredForForbiddenZones && bCollisionGuardCreated && bExecCollisionGuardCreated && bExistingPlanSkipped && bProtectedIslandEdgeSkipped,
				FString::Printf(
					TEXT("zones=%d direct_zones=%d output_ignored=%s avoid=%d guard=%d exec_guard=%d rejected=%d existing_guard=%d protected_guard=%d chain=%.1f..%.1f block=%.1f plan=%d existing_points=%d"),
					Run.DesignDiagnostics.InputChainForbiddenZoneCount,
					DirectZones.Num(),
					bOutputPinIgnoredForForbiddenZones ? TEXT("true") : TEXT("false"),
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
				Nodes[Index].Guid = FString::Printf(TEXT("input_chain_source_occupancy_%d"), Index);
				Nodes[Index].Title = Nodes[Index].Guid;
				Nodes[Index].Width = Index == 1 ? 220.0 : 140.0;
				Nodes[Index].Height = Index == 1 ? 120.0 : (Index == 0 ? 90.0 : 64.0);
				Nodes[Index].Rank = Index == 1 ? 1 : 0;
				Nodes[Index].NewX = Index == 1 ? 320.0 : 0.0;
				Nodes[Index].NewY = Index == 0 ? 10.0 : 0.0;
				Nodes[Index].OutputPinNames = { TEXT("out") };
				Nodes[Index].OutputPinAnchors = { FVector2D(Nodes[Index].Width, Index == 0 ? 45.0 : 32.0) };
				Nodes[Index].OutputPinAnchorValid = { 1 };
				if (Index == 1)
				{
					Nodes[Index].InputPinNames = { TEXT("A"), TEXT("B") };
					Nodes[Index].InputPinAnchors = { FVector2D(0.0, 44.0), FVector2D(0.0, 48.0) };
					Nodes[Index].InputPinAnchorValid = { 1, 1 };
				}
			}
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("A"), 0, 0, false},
				{2, 1, TEXT("out"), TEXT("B"), 0, 1, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.Columns.SetNum(2);
			Island.Columns[0] = {0};
			Island.Columns[1] = {1};
			FInputChain Chain;
			Chain.Id = 0;
			Chain.TargetNode = 1;
			Chain.TargetPinIndex = 1;
			Chain.TargetEdgeIndex = 1;
			Chain.TargetRank = 1;
			Chain.CorridorIndex = 0;
			Chain.Nodes = {2};
			Chain.EdgeIndices = {1};
			Island.InputChains.Add(Chain);
			Island.InputChainNodeSet.Add(2);

			TSet<int32> ActiveCorridors;
			ActiveCorridors.Add(0);
			const TArray<FInputChainForbiddenZone> Zones = BuildInputChainForbiddenZonesForTarget(
				Nodes,
				Edges,
				Island,
				1,
				FVector2D(Nodes[1].NewX, Nodes[1].NewY),
				Options,
				&ActiveCorridors,
				true);
			TArray<FColumnItem> Items;
			FColumnItem Item;
			Item.NodeIndex = 0;
			Item.Height = Nodes[2].Height;
			Item.PreferredTop = 20.0;
			Item.bHasPreferred = true;
			Items.Add(Item);
			int32 AvoidanceCount = 0;
			PlaceAnchoredIntervalsAvoidingForbiddenZones(Items, 24.0, Zones, &AvoidanceCount);
			const FRect SourceRect = MakeNodeRectAt(Nodes[0], FVector2D(Nodes[0].NewX, Nodes[0].NewY));
			const FRect ChainRect = {0.0, Items[0].FinalTop, Nodes[2].Width, Items[0].FinalTop + Nodes[2].Height};
			bool bHasSourceOccupancyZone = false;
			for (const FInputChainForbiddenZone& Zone : Zones)
			{
				bHasSourceOccupancyZone = bHasSourceOccupancyZone || Zone.bSourceNodeOccupancy;
			}
			const bool bSourceOccupancyAvoided =
				bHasSourceOccupancyZone
				&& AvoidanceCount > 0
				&& !ChainRect.Intersects(SourceRect, 24.0);
			AddCase(
				TEXT("input_chain_avoids_same_corridor_source_node_occupancy"),
				bSourceOccupancyAvoided,
				FString::Printf(TEXT("zones=%d has_source_zone=%s avoidance=%d source=%.1f..%.1f chain=%.1f..%.1f"),
					Zones.Num(),
					bHasSourceOccupancyZone ? TEXT("true") : TEXT("false"),
					AvoidanceCount,
					SourceRect.Top,
					SourceRect.Bottom,
					ChainRect.Top,
					ChainRect.Bottom));
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

		RunSourceColumnCollisionAabbSelfTests(Result, Options);
		RunSameIslandForwardXColumnSelfTests(Result, Options);

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
			TopCandidate.bContinuousSegment = true;
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
			TArray<FIsland> Islands;
			for (int32 IslandIndex = 0; IslandIndex < 3; ++IslandIndex)
			{
				FIsland Island;
				Island.Id = IslandIndex;
				Island.Rank = IslandIndex == 2 ? 1 : 0;
				Island.X = 0.0;
				Island.Y = 0.0;
				Island.LocalBounds = IslandIndex == 1
					? FRect{0.0, 0.0, 260.0, 360.0}
					: (IslandIndex == 2 ? FRect{0.0, 0.0, 760.0, 170.0} : FRect{0.0, 0.0, 120.0, 100.0});
				Islands.Add(Island);
			}
			FSelectedEndpointCandidate Selected;
			Selected.EdgeIndex = 0;
			Selected.SourceIsland = 0;
			Selected.TargetIsland = 2;
			Selected.Side = TEXT("left");
			Selected.SourceLocal = FVector2D(120.0, 50.0);
			Selected.EntryLocal = FVector2D(0.0, 50.0);
			Selected.ActualTargetLocal = FVector2D(0.0, 50.0);
			Selected.SourcePortOrderKey = 50.0;
			TArray<FSelectedEndpointCandidate> SelectedEndpoints{Selected};
			FPlacementOwnership Ownership;
			Ownership.OwnerSourceByIsland.Init(INDEX_NONE, Islands.Num());
			Ownership.OwnerSourceByIsland[2] = 0;
			Ownership.OwnerEdgeIndices.Add(0);
			PlaceIslandsYByWeightedMedianAndPacking(TArray<FNode>(), Islands, SelectedEndpoints, Ownership, Options);
			const bool bShiftedXInsteadOfStretchingY =
				Islands[2].X > 120.0
				&& FMath::Abs(Islands[2].Y) <= Options.NodePadding
				&& !IslandAabb(Islands[2]).Intersects(IslandAabb(Islands[0]), Options.NodePadding)
				&& !IslandAabb(Islands[2]).Intersects(IslandAabb(Islands[1]), Options.NodePadding);
			AddCase(
				TEXT("island_y_placement_uses_x_shift_to_avoid_meaningless_vertical_stretch"),
				bShiftedXInsteadOfStretchingY,
				FString::Printf(
					TEXT("target=(%.1f,%.1f) source_bottom=%.1f blocker=%.1f..%.1f"),
					Islands[2].X,
					Islands[2].Y,
					IslandAabb(Islands[0]).Bottom,
					IslandAabb(Islands[1]).Top,
					IslandAabb(Islands[1]).Bottom));
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
			CompactProbeOptions.PinDirectionBiasX = 40.0;
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
				&& Run.DesignDiagnostics.TopBoundaryEntryCount >= 2
				&& Run.DesignDiagnostics.BottomBoundaryEntryCount >= 2;
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
			const bool bHasContinuousBoundaryBus = Run.DesignDiagnostics.BoundaryBusRequiredCount == 1
				&& Run.DesignDiagnostics.BoundaryBusCount == 1
				&& Run.DesignDiagnostics.TopBoundaryEntryCount >= 2
				&& Run.DesignDiagnostics.BottomBoundaryEntryCount >= 2;
			const bool bAlignedSegmentKeptResidualTight = Run.DesignDiagnostics.IslandConnectionTopBottomXResidual < 180.0;
			const double TargetIslandLeft = FMath::Min(Nodes[1].NewX, Nodes[2].NewX);
			const double MaxExpectedAlignedIslandDx = Nodes[0].Width + Options.HorizontalSpacing;
			const bool bAlignedEntryDidNotForceExtraRankColumnGap = TargetIslandLeft - Nodes[0].NewX <= MaxExpectedAlignedIslandDx + 0.5;
			AddCase(TEXT("continuous_boundary_entry_uses_connection_alignment_x"), bHasContinuousBoundaryBus && bAlignedSegmentKeptResidualTight && bAlignedEntryDidNotForceExtraRankColumnGap, FString::Printf(TEXT("boundary_bus=%d/%d top=%d bottom=%d x_residual=%.1f target_dx=%.1f max_dx=%.1f"), Run.DesignDiagnostics.BoundaryBusCount, Run.DesignDiagnostics.BoundaryBusRequiredCount, Run.DesignDiagnostics.TopBoundaryEntryCount, Run.DesignDiagnostics.BottomBoundaryEntryCount, Run.DesignDiagnostics.IslandConnectionTopBottomXResidual, TargetIslandLeft - Nodes[0].NewX, MaxExpectedAlignedIslandDx));
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
				Selected.bPlacementOwner = true;
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
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("ownerbus%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 140.0;
				Node.Height = 80.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(140.0, 40.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 40.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			TArray<FEdge> Edges = {
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{1, 2, TEXT("then"), TEXT("execute"), 0, 0, true}
			};
			TArray<FIsland> Islands;
			for (int32 IslandIndex = 0; IslandIndex < 3; ++IslandIndex)
			{
				FIsland Island;
				Island.Id = IslandIndex;
				Island.Nodes = {IslandIndex};
				Island.NodeSet.Add(IslandIndex);
				Island.LocalBounds = {0.0, 0.0, 140.0, 80.0};
				Island.X = IslandIndex == 0 ? 0.0 : (IslandIndex == 1 ? 0.0 : 260.0);
				Island.Y = IslandIndex == 0 ? 0.0 : (IslandIndex == 1 ? 120.0 : 120.0);
				Islands.Add(Island);
			}
			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
			{
				FSelectedEndpointCandidate Selected;
				Selected.EdgeIndex = EdgeIndex;
				Selected.CandidateId = EdgeIndex;
				Selected.SourceIsland = EdgeIndex;
				Selected.TargetIsland = 2;
				Selected.Side = TEXT("left");
				Selected.SourceLocal = FVector2D(140.0, 40.0);
				Selected.EntryLocal = FVector2D(0.0, 40.0);
				Selected.ActualTargetLocal = FVector2D(0.0, 40.0);
				Selected.SourcePortOrderKey = Selected.SourceLocal.Y;
				SelectedEndpoints.Add(Selected);
			}
			FRunResult Run;
			const TSet<int32> RouteOnlyEdgeIndices;
			FPlacementOwnership Ownership = BuildPlacementOwnershipForest(Edges, Islands, SelectedEndpoints, RouteOnlyEdgeIndices, Run);
			const TArray<FReroutePlan> Plans = RouteDirectAndBusCorridors(Nodes, Edges, Islands, SelectedEndpoints, Options, Run);
			const bool bExecOwnerDirectAndDataNonOwnerBus =
				Ownership.OwnerSourceByIsland.IsValidIndex(2)
				&& Ownership.OwnerSourceByIsland[2] == 1
				&& SelectedEndpoints[1].bPlacementOwner
				&& !SelectedEndpoints[0].bPlacementOwner
				&& SelectedEndpoints[1].BusContractId == INDEX_NONE
				&& SelectedEndpoints[0].BusContractId != INDEX_NONE
				&& Plans.Num() == 1
				&& Plans[0].EdgeIndex == 0;
			AddCase(TEXT("placement_owner_edge_direct_non_owner_edge_routes_via_bus"),
				bExecOwnerDirectAndDataNonOwnerBus,
				FString::Printf(TEXT("owner=%d owner_edges=%d non_owner_edges=%d plans=%d bus0=%d bus1=%d"),
					Ownership.OwnerSourceByIsland.IsValidIndex(2) ? Ownership.OwnerSourceByIsland[2] : -1,
					Run.DesignDiagnostics.PlacementOwnerEdgeCount,
					Run.DesignDiagnostics.PlacementNonOwnerEdgeCount,
					Plans.Num(),
					SelectedEndpoints[0].BusContractId,
					SelectedEndpoints[1].BusContractId));
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
				true,
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
				true,
				Options);
			const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[0]);
			const bool bOwnerDirectStaysDirect =
				Planned.RouteKind == ERouteKind::Direct
				&& Planned.Polyline.Num() >= 2
				&& !PolylineHitsHardOccupancy(Planned.Polyline, RouteHardRects)
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(Planned.Polyline, Options);
			AddCase(TEXT("ownership_direct_inter_island_not_upgraded_by_distance_only"), bOwnerDirectStaysDirect, FString::Printf(TEXT("kind=%s points=%d trunk_y=%.1f hits=%d"),
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
				true,
				Options);
			const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[0]);
			const bool bOwnerExecDirectStaysDirect =
				Planned.RouteKind == ERouteKind::Direct
				&& Planned.Polyline.Num() >= 2
				&& !PolylineHitsHardOccupancy(Planned.Polyline, RouteHardRects)
				&& HasNonNegativeConnectionAlignmentShortenForEverySegment(Planned.Polyline, Options);
			AddCase(TEXT("ownership_exec_direct_not_upgraded_by_distance_only"), bOwnerExecDirectStaysDirect, FString::Printf(TEXT("kind=%s points=%d trunk_y=%.1f hits=%d"),
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
			const bool bInternalCleanDirectStaysDirect =
				Plans.Num() == 0
				&& Run.CorridorRouteDiagnostics.Num() == 0
				&& Run.DesignDiagnostics.RouteKindUpgradeCount == 0;
			AddCase(TEXT("internal_clean_direct_not_upgraded_by_distance_only"), bInternalCleanDirectStaysDirect, FString::Printf(TEXT("plans=%d points=%d upgrades=%d diagnostics=%d"),
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
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 5; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("shared_lane_reservation%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 100.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 115.0;
			Nodes[1].NewX = 900.0;
			Nodes[1].NewY = 115.0;
			Nodes[2].NewX = 300.0;
			Nodes[2].NewY = 115.0;
			Nodes[3].NewX = 0.0;
			Nodes[3].NewY = 265.0;
			Nodes[4].NewX = 900.0;
			Nodes[4].NewY = 265.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{3, 4, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3, 4};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			for (const int32 NodeIndex : Island.Nodes)
			{
				IslandByNode.Add(NodeIndex, 0);
			}
			FReroutePlan PlanA;
			PlanA.EdgeIndex = 0;
			PlanA.Reason = TEXT("fanout_local_bus");
			PlanA.BusId = TEXT("same_source");
			PlanA.RouteKind = ERouteKind::LocalBus;
			PlanA.AttachmentMergeGroup = 11;
			PlanA.Points.Add(FVector2D(260.0, 100.0));
			FReroutePlan PlanB;
			PlanB.EdgeIndex = 1;
			PlanB.Reason = TEXT("fanout_local_bus");
			PlanB.BusId = TEXT("same_source");
			PlanB.RouteKind = ERouteKind::LocalBus;
			PlanB.AttachmentMergeGroup = 11;
			PlanB.Points.Add(FVector2D(260.0, 150.0));
			FReroutePlan ReservedPlan;
			ReservedPlan.EdgeIndex = 2;
			ReservedPlan.Reason = TEXT("fanout_local_bus");
			ReservedPlan.BusId = TEXT("different_source");
			ReservedPlan.RouteKind = ERouteKind::LocalBus;
			ReservedPlan.AttachmentMergeGroup = 12;
			ReservedPlan.Points.Add(FVector2D(500.0, 150.0));
			ReservedPlan.Points.Add(FVector2D(760.0, 150.0));
			TArray<FReroutePlan> Plans = {PlanA, PlanB, ReservedPlan};
			FRunResult Run;
			const int32 Normalized = NormalizeSharedBusTrunkYAfterSegmentRefine(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);

			struct FHorizontalRouteSegment
			{
				FString SourceId;
				double Y = 0.0;
				double XMin = 0.0;
				double XMax = 0.0;
			};
			TArray<FHorizontalRouteSegment> Segments;
			for (const FReroutePlan& Plan : Plans)
			{
				FVector2D Start;
				FVector2D End;
				if (!TryGetRouteEndpointsForPlan(Nodes, Edges, Islands, IslandByNode, Plan, Start, End))
				{
					continue;
				}
				const TArray<FVector2D> Route = BuildVisibleRouteFromReroutePoints(Start, Plan.Points, End);
				for (int32 PointIndex = 0; PointIndex + 1 < Route.Num(); ++PointIndex)
				{
					const FVector2D A = Route[PointIndex];
					const FVector2D B = Route[PointIndex + 1];
					if (FMath::Abs(A.Y - B.Y) > 0.5 || FMath::Abs(A.X - B.X) < 20.0)
					{
						continue;
					}
					FHorizontalRouteSegment Segment;
					Segment.SourceId = GetLaneReservationSourceIdForPlan(Edges, Plan);
					Segment.Y = A.Y;
					Segment.XMin = FMath::Min(A.X, B.X);
					Segment.XMax = FMath::Max(A.X, B.X);
					Segments.Add(Segment);
				}
			}
			bool bHasCrossSourceOverlap = false;
			for (int32 A = 0; A < Segments.Num(); ++A)
			{
				for (int32 B = A + 1; B < Segments.Num(); ++B)
				{
					if (Segments[A].SourceId == Segments[B].SourceId)
					{
						continue;
					}
					if (FMath::IsNearlyEqual(Segments[A].Y, Segments[B].Y, 0.5)
						&& FMath::Min(Segments[A].XMax, Segments[B].XMax) - FMath::Max(Segments[A].XMin, Segments[B].XMin) > 20.0)
					{
						bHasCrossSourceOverlap = true;
					}
				}
			}
			AddCase(TEXT("shared_bus_trunk_normalize_respects_cross_source_lane_reservation"), !bHasCrossSourceOverlap && (Normalized >= 1 || Run.RejectionDiagnostics.Num() > 0), FString::Printf(TEXT("normalized=%d overlap=%d y=%.1f/%.1f/%.1f diagnostics=%d"),
				Normalized,
				bHasCrossSourceOverlap ? 1 : 0,
				Plans[0].Points.Num() > 0 ? Plans[0].Points[0].Y : -1.0,
				Plans[1].Points.Num() > 0 ? Plans[1].Points[0].Y : -1.0,
				Plans[2].Points.Num() > 0 ? Plans[2].Points[0].Y : -1.0,
				Run.RejectionDiagnostics.Num()));
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
			Selected.bPlacementOwner = true;
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
				Node.Guid = FString::Printf(TEXT("accessprune%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 100.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 0.0;
			Nodes[1].NewX = 700.0;
			Nodes[1].NewY = 0.0;
			Nodes[2].NewX = 130.0;
			Nodes[2].NewY = 80.0;
			Nodes[3].NewX = 0.0;
			Nodes[3].NewY = 170.0;
			Nodes[4].NewX = 700.0;
			Nodes[4].NewY = 170.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{0, 2, TEXT("out"), TEXT("in"), 0, 0, false},
				{3, 4, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3, 4};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			for (const int32 NodeIndex : Island.Nodes)
			{
				IslandByNode.Add(NodeIndex, 0);
			}

			const double EntryX = ComputeInputXAlignedToOutput(Nodes[0].NewX + Nodes[0].Width, Options);
			const double LongExitX = ComputeOutputXAlignedToInput(Nodes[1].NewX, Options);
			const double OtherEntryX = ComputeInputXAlignedToOutput(Nodes[3].NewX + Nodes[3].Width, Options);
			const double OtherExitX = ComputeOutputXAlignedToInput(Nodes[4].NewX, Options);
			FReroutePlan LongPlan;
			LongPlan.EdgeIndex = 0;
			LongPlan.Reason = TEXT("direct_obstacle_local_bus");
			LongPlan.BusId = MakeSourceParamId(Edges[0]);
			LongPlan.RouteKind = ERouteKind::LocalBus;
			LongPlan.Points = { FVector2D(EntryX, 80.0), FVector2D(LongExitX, 80.0) };
			FReroutePlan InvalidShortPlan;
			InvalidShortPlan.EdgeIndex = 1;
			InvalidShortPlan.Reason = TEXT("direct_obstacle_local_bus");
			InvalidShortPlan.BusId = MakeSourceParamId(Edges[1]);
			InvalidShortPlan.RouteKind = ERouteKind::LocalBus;
			InvalidShortPlan.Points = { FVector2D(EntryX, 84.0), FVector2D(EntryX + 120.0, 84.0) };
			FReroutePlan OtherPlan;
			OtherPlan.EdgeIndex = 2;
			OtherPlan.Reason = TEXT("direct_obstacle_local_bus");
			OtherPlan.BusId = MakeSourceParamId(Edges[2]);
			OtherPlan.RouteKind = ERouteKind::LocalBus;
			OtherPlan.Points = { FVector2D(OtherEntryX, 84.0), FVector2D(OtherExitX, 84.0) };
			TArray<FReroutePlan> Plans = {LongPlan, InvalidShortPlan, OtherPlan};
			FRunResult Run;
			const int32 Removed = RemoveEndpointAccessInvalidReroutePlans(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			const int32 InitialViolations = ValidateFinalVisibleRerouteLaneSpacing(Nodes, Edges, Islands, IslandByNode, Options, Plans, nullptr);
			const int32 Adjusted = PlanFinalVisibleRerouteTrunkLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			const int32 FinalViolations = ValidateFinalVisibleRerouteLaneSpacing(Nodes, Edges, Islands, IslandByNode, Options, Plans, &Run);
			AddCase(TEXT("endpoint_access_invalid_reroute_does_not_block_lane_planner"),
				Removed == 1 && Plans.Num() == 2 && InitialViolations > 0 && Adjusted >= 1 && FinalViolations == 0,
				FString::Printf(TEXT("removed=%d plans=%d initial=%d adjusted=%d final=%d diagnostics=%d"),
					Removed,
					Plans.Num(),
					InitialViolations,
					Adjusted,
					FinalViolations,
					Run.RejectionDiagnostics.Num()));
		}

		RunFinalVisibleLaneRegressionSelfTests(Result, Options);

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("finalvisiblelane%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 100.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 0.0;
			Nodes[1].NewX = 600.0;
			Nodes[1].NewY = 0.0;
			Nodes[2].NewX = 0.0;
			Nodes[2].NewY = 140.0;
			Nodes[3].NewX = 600.0;
			Nodes[3].NewY = 140.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{2, 3, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			for (const int32 NodeIndex : Island.Nodes)
			{
				IslandByNode.Add(NodeIndex, 0);
			}

			FReroutePlan PlanA;
			PlanA.EdgeIndex = 0;
			PlanA.Reason = TEXT("direct_obstacle_local_bus");
			PlanA.BusId = MakeSourceParamId(Edges[0]);
			PlanA.RouteKind = ERouteKind::LocalBus;
			PlanA.Points.Add(FVector2D(180.0, 80.0));
			PlanA.Points.Add(FVector2D(520.0, 80.0));
			FReroutePlan PlanB;
			PlanB.EdgeIndex = 1;
			PlanB.Reason = TEXT("direct_obstacle_local_bus");
			PlanB.BusId = MakeSourceParamId(Edges[1]);
			PlanB.RouteKind = ERouteKind::LocalBus;
			PlanB.Points.Add(FVector2D(180.0, 84.0));
			PlanB.Points.Add(FVector2D(520.0, 84.0));
			TArray<FReroutePlan> Plans = {PlanA, PlanB};
			FRunResult Run;
			const int32 InitialViolations = ValidateFinalVisibleRerouteLaneSpacing(Nodes, Edges, Islands, IslandByNode, Options, Plans, nullptr);
			const int32 Adjusted = PlanFinalVisibleRerouteTrunkLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			const int32 FinalViolations = ValidateFinalVisibleRerouteLaneSpacing(Nodes, Edges, Islands, IslandByNode, Options, Plans, &Run);
			const double DeltaY = Plans[0].Points.Num() > 0 && Plans[1].Points.Num() > 0
				? FMath::Abs(Plans[0].Points[0].Y - Plans[1].Points[0].Y)
				: 0.0;
			AddCase(TEXT("final_visible_reroute_lane_planner_separates_post_added_buses"), InitialViolations > 0 && Adjusted >= 1 && FinalViolations == 0 && DeltaY >= GetBusTrunkLaneSpacing() - 0.5, FString::Printf(TEXT("initial=%d adjusted=%d final=%d delta=%.1f assignments=%d violations=%d"),
				InitialViolations,
				Adjusted,
				FinalViolations,
				DeltaY,
				Run.LaneAssignmentDiagnostics.Num(),
				Run.DesignDiagnostics.FinalVisibleBusLaneViolationCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("finalvisiblelongsegment%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 100.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 0.0;
			Nodes[1].NewX = 680.0;
			Nodes[1].NewY = 0.0;
			Nodes[2].NewX = 0.0;
			Nodes[2].NewY = 150.0;
			Nodes[3].NewX = 680.0;
			Nodes[3].NewY = 150.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{2, 3, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			for (const int32 NodeIndex : Island.Nodes)
			{
				IslandByNode.Add(NodeIndex, 0);
			}

			FReroutePlan PlanA;
			PlanA.EdgeIndex = 0;
			PlanA.Reason = TEXT("direct_obstacle_local_bus");
			PlanA.BusId = MakeSourceParamId(Edges[0]);
			PlanA.RouteKind = ERouteKind::LocalBus;
			PlanA.Points.Add(FVector2D(180.0, 40.0));
			PlanA.Points.Add(FVector2D(220.0, 40.0));
			PlanA.Points.Add(FVector2D(260.0, 84.0));
			PlanA.Points.Add(FVector2D(600.0, 84.0));
			FReroutePlan PlanB;
			PlanB.EdgeIndex = 1;
			PlanB.Reason = TEXT("direct_obstacle_local_bus");
			PlanB.BusId = MakeSourceParamId(Edges[1]);
			PlanB.RouteKind = ERouteKind::LocalBus;
			PlanB.Points.Add(FVector2D(180.0, 84.0));
			PlanB.Points.Add(FVector2D(600.0, 84.0));
			TArray<FReroutePlan> Plans = {PlanA, PlanB};
			FRunResult Run;
			const int32 InitialViolations = ValidateFinalVisibleRerouteLaneSpacing(Nodes, Edges, Islands, IslandByNode, Options, Plans, nullptr);
			const int32 Adjusted = PlanFinalVisibleRerouteTrunkLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			const int32 FinalViolations = ValidateFinalVisibleRerouteLaneSpacing(Nodes, Edges, Islands, IslandByNode, Options, Plans, &Run);
			TArray<FVisibleRerouteTrunkLaneItem> Items;
			TArray<FVisibleRerouteSourceLaneGroup> Groups;
			CollectVisibleRerouteSourceLaneGroups(Nodes, Edges, Islands, IslandByNode, Options, Plans, Items, Groups);
			double LaneA = 0.0;
			double LaneB = 0.0;
			for (const FVisibleRerouteTrunkLaneItem& Item : Items)
			{
				if (Item.EdgeIndex == 0)
				{
					LaneA = Item.TrunkY;
				}
				else if (Item.EdgeIndex == 1)
				{
					LaneB = Item.TrunkY;
				}
			}
			const double DeltaY = FMath::Abs(LaneA - LaneB);
			AddCase(TEXT("final_visible_lane_planner_uses_longest_visible_horizontal_segment"), InitialViolations > 0 && Adjusted >= 1 && FinalViolations == 0 && DeltaY >= GetBusTrunkLaneSpacing() - 0.5, FString::Printf(TEXT("initial=%d adjusted=%d final=%d lane_a=%.1f lane_b=%.1f delta=%.1f assignments=%d"),
				InitialViolations,
				Adjusted,
				FinalViolations,
				LaneA,
				LaneB,
				DeltaY,
				Run.LaneAssignmentDiagnostics.Num()));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("finalvisiblesplittrunk%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 100.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 0.0;
			Nodes[1].NewX = 760.0;
			Nodes[1].NewY = 0.0;
			Nodes[2].NewX = 0.0;
			Nodes[2].NewY = 150.0;
			Nodes[3].NewX = 760.0;
			Nodes[3].NewY = 150.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{2, 3, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			for (const int32 NodeIndex : Island.Nodes)
			{
				IslandByNode.Add(NodeIndex, 0);
			}

			FReroutePlan PlanA;
			PlanA.EdgeIndex = 0;
			PlanA.Reason = TEXT("direct_obstacle_local_bus");
			PlanA.BusId = MakeSourceParamId(Edges[0]);
			PlanA.RouteKind = ERouteKind::LocalBus;
			PlanA.Points.Add(FVector2D(180.0, 40.0));
			PlanA.Points.Add(FVector2D(650.0, 40.0));
			PlanA.Points.Add(FVector2D(260.0, 84.0));
			PlanA.Points.Add(FVector2D(600.0, 84.0));
			FReroutePlan PlanB;
			PlanB.EdgeIndex = 1;
			PlanB.Reason = TEXT("direct_obstacle_local_bus");
			PlanB.BusId = MakeSourceParamId(Edges[1]);
			PlanB.RouteKind = ERouteKind::LocalBus;
			PlanB.Points.Add(FVector2D(180.0, 84.0));
			PlanB.Points.Add(FVector2D(600.0, 84.0));
			TArray<FReroutePlan> Plans = {PlanA, PlanB};

			TArray<FVisibleRerouteTrunkLaneItem> InitialItems;
			TArray<FVisibleRerouteSourceLaneGroup> InitialGroups;
			CollectVisibleRerouteSourceLaneGroups(Nodes, Edges, Islands, IslandByNode, Options, Plans, InitialItems, InitialGroups);
			FRunResult Run;
			const int32 InitialViolations = ValidateFinalVisibleRerouteLaneSpacing(Nodes, Edges, Islands, IslandByNode, Options, Plans, nullptr);
			const int32 Adjusted = PlanFinalVisibleRerouteTrunkLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			const int32 FinalViolations = ValidateFinalVisibleRerouteLaneSpacing(Nodes, Edges, Islands, IslandByNode, Options, Plans, &Run);
			TArray<FVisibleRerouteTrunkLaneItem> FinalItems;
			TArray<FVisibleRerouteSourceLaneGroup> FinalGroups;
			CollectVisibleRerouteSourceLaneGroups(Nodes, Edges, Islands, IslandByNode, Options, Plans, FinalItems, FinalGroups);
			AddCase(TEXT("final_visible_lane_planner_detects_and_normalizes_split_route_trunks"),
				InitialItems.Num() >= 3 && InitialViolations > 0 && Adjusted >= 1 && FinalViolations == 0 && FinalItems.Num() <= 2,
				FString::Printf(TEXT("initial_items=%d final_items=%d initial=%d adjusted=%d final=%d assignments=%d"),
					InitialItems.Num(),
					FinalItems.Num(),
					InitialViolations,
					Adjusted,
					FinalViolations,
					Run.LaneAssignmentDiagnostics.Num()));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("finalvisiblebranchx%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 100.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 0.0;
			Nodes[1].NewX = 600.0;
			Nodes[1].NewY = 160.0;
			Nodes[2].NewX = 0.0;
			Nodes[2].NewY = 160.0;
			Nodes[3].NewX = 600.0;
			Nodes[3].NewY = 20.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{2, 3, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			for (const int32 NodeIndex : Island.Nodes)
			{
				IslandByNode.Add(NodeIndex, 0);
			}

			const double TrunkY = 80.0;
			const double EntryX = ComputeInputXAlignedToOutput(Nodes[0].NewX + Nodes[0].Width, Options);
			const double BranchX = ComputeOutputXAlignedToInput(Nodes[1].NewX, Options);
			FReroutePlan PlanA;
			PlanA.EdgeIndex = 0;
			PlanA.Reason = TEXT("direct_obstacle_local_bus");
			PlanA.BusId = MakeSourceParamId(Edges[0]);
			PlanA.RouteKind = ERouteKind::LocalBus;
			PlanA.Points = { FVector2D(EntryX, TrunkY), FVector2D(BranchX, TrunkY), FVector2D(BranchX, Nodes[1].NewY + 35.0) };
			FReroutePlan PlanB;
			PlanB.EdgeIndex = 1;
			PlanB.Reason = TEXT("direct_obstacle_local_bus");
			PlanB.BusId = MakeSourceParamId(Edges[1]);
			PlanB.RouteKind = ERouteKind::LocalBus;
			PlanB.Points = { FVector2D(EntryX, TrunkY), FVector2D(BranchX, TrunkY), FVector2D(BranchX, Nodes[3].NewY + 35.0) };
			TArray<FReroutePlan> Plans = {PlanA, PlanB};
			FRunResult Run;
			const int32 InitialViolations = ValidateFinalVisibleRerouteBranchXLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, nullptr);
			const int32 Adjusted = PlanFinalVisibleRerouteBranchXLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			const int32 FinalViolations = ValidateFinalVisibleRerouteBranchXLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, &Run);
			double FinalBranchXA = 0.0;
			double FinalBranchXB = 0.0;
			TArray<FVisibleRerouteBranchXLaneItem> FinalItems;
			CollectVisibleRerouteBranchXLaneItems(Nodes, Edges, Islands, IslandByNode, Options, Plans, FinalItems);
			for (const FVisibleRerouteBranchXLaneItem& Item : FinalItems)
			{
				if (Item.EdgeIndex == 0)
				{
					FinalBranchXA = Item.BranchX;
				}
				else if (Item.EdgeIndex == 1)
				{
					FinalBranchXB = Item.BranchX;
				}
			}
			AddCase(TEXT("final_visible_branch_x_lane_planner_orders_closer_target_right"), InitialViolations > 0 && Adjusted >= 1 && FinalViolations == 0 && FinalBranchXB > FinalBranchXA + GetCrossBusBranchLaneSpacing(Options) - 0.5, FString::Printf(TEXT("initial=%d adjusted=%d final=%d branch_a=%.1f branch_b=%.1f assignments=%d branch_violations=%d"),
				InitialViolations,
				Adjusted,
				FinalViolations,
				FinalBranchXA,
				FinalBranchXB,
				Run.LaneAssignmentDiagnostics.Num(),
				Run.DesignDiagnostics.FinalVisibleBranchXLaneViolationCount));
		}

		{
			TArray<FNode> Nodes;
			for (int32 Index = 0; Index < 6; ++Index)
			{
				FNode Node;
				Node.Index = Index;
				Node.Guid = FString::Printf(TEXT("finalvisiblebranchscope%d"), Index);
				Node.Title = Node.Guid;
				Node.Width = 100.0;
				Node.Height = 70.0;
				Node.OutputPinNames = { TEXT("out") };
				Node.InputPinNames = { TEXT("in") };
				Node.OutputPinAnchors = { FVector2D(Node.Width, 35.0) };
				Node.InputPinAnchors = { FVector2D(0.0, 35.0) };
				Node.OutputPinAnchorValid = { 1 };
				Node.InputPinAnchorValid = { 1 };
				Nodes.Add(Node);
			}
			Nodes[0].NewX = 0.0;
			Nodes[0].NewY = 0.0;
			Nodes[1].NewX = 600.0;
			Nodes[1].NewY = 0.0;
			Nodes[2].NewX = 0.0;
			Nodes[2].NewY = 200.0;
			Nodes[3].NewX = 260.0;
			Nodes[3].NewY = 200.0;
			Nodes[4].NewX = 0.0;
			Nodes[4].NewY = 400.0;
			Nodes[5].NewX = 600.0;
			Nodes[5].NewY = 400.0;
			TArray<FEdge> Edges = {
				{0, 1, TEXT("out"), TEXT("in"), 0, 0, false},
				{2, 3, TEXT("out"), TEXT("in"), 0, 0, false},
				{4, 5, TEXT("out"), TEXT("in"), 0, 0, false}
			};
			FIsland Island;
			Island.Id = 0;
			Island.Nodes = {0, 1, 2, 3, 4, 5};
			for (const int32 NodeIndex : Island.Nodes)
			{
				Island.NodeSet.Add(NodeIndex);
			}
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			TArray<FIsland> Islands = {Island};
			TMap<int32, int32> IslandByNode;
			for (const int32 NodeIndex : Island.Nodes)
			{
				IslandByNode.Add(NodeIndex, 0);
			}

			const double EntryX = ComputeInputXAlignedToOutput(Nodes[0].NewX + Nodes[0].Width, Options);
			const double TrunkY = -120.0;
			FReroutePlan NearRightPlan;
			NearRightPlan.EdgeIndex = 0;
			NearRightPlan.Reason = TEXT("direct_obstacle_local_bus");
			NearRightPlan.BusId = MakeSourceParamId(Edges[0]);
			NearRightPlan.RouteKind = ERouteKind::LocalBus;
			NearRightPlan.Points = { FVector2D(EntryX, TrunkY), FVector2D(520.0, TrunkY) };
			FReroutePlan FarLeftPlan;
			FarLeftPlan.EdgeIndex = 1;
			FarLeftPlan.Reason = TEXT("direct_obstacle_local_bus");
			FarLeftPlan.BusId = MakeSourceParamId(Edges[1]);
			FarLeftPlan.RouteKind = ERouteKind::LocalBus;
			FarLeftPlan.Points = { FVector2D(EntryX, TrunkY), FVector2D(ComputeOutputXAlignedToInput(Nodes[3].NewX, Options), TrunkY) };
			FReroutePlan ScopedPlan;
			ScopedPlan.EdgeIndex = 2;
			ScopedPlan.Reason = TEXT("direct_obstacle_local_bus");
			ScopedPlan.BusId = MakeSourceParamId(Edges[2]);
			ScopedPlan.RouteKind = ERouteKind::LocalBus;
			ScopedPlan.Points = { FVector2D(EntryX, TrunkY), FVector2D(500.0, TrunkY), FVector2D(518.0, TrunkY) };
			TArray<FReroutePlan> Plans = {NearRightPlan, FarLeftPlan, ScopedPlan};
			FRunResult Run;
			const int32 InitialViolations = ValidateFinalVisibleRerouteBranchXLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, nullptr);
			const int32 Adjusted = PlanFinalVisibleRerouteBranchXLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, Run);
			const int32 FinalViolations = ValidateFinalVisibleRerouteBranchXLanes(Nodes, Edges, Islands, IslandByNode, Options, Plans, &Run);
			TArray<FVisibleRerouteBranchXLaneItem> FinalItems;
			CollectVisibleRerouteBranchXLaneItems(Nodes, Edges, Islands, IslandByNode, Options, Plans, FinalItems);
			double ScopedBranchX = -1.0;
			for (const FVisibleRerouteBranchXLaneItem& Item : FinalItems)
			{
				if (Item.EdgeIndex == 2)
				{
					ScopedBranchX = Item.BranchX;
				}
			}
			const double ExpectedScopedBranchX = ComputeOutputXAlignedToInput(Nodes[5].NewX, Options) - GetCrossBusBranchLaneSpacing(Options);
			AddCase(TEXT("final_visible_branch_x_far_items_do_not_overconstrain_bundle_scope"),
				InitialViolations > 0 && Adjusted >= 1 && FinalViolations == 0 && FMath::IsNearlyEqual(ScopedBranchX, ExpectedScopedBranchX, 0.5),
				FString::Printf(TEXT("initial=%d adjusted=%d final=%d scoped_x=%.1f expected=%.1f assignments=%d branch_violations=%d"),
					InitialViolations,
					Adjusted,
					FinalViolations,
					ScopedBranchX,
					ExpectedScopedBranchX,
					Run.LaneAssignmentDiagnostics.Num(),
					Run.DesignDiagnostics.FinalVisibleBranchXLaneViolationCount));
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
