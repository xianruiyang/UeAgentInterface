// Copyright Epic Games, Inc. All Rights Reserved.
// Frozen-island deterministic inter-island placement and bus routing.

		static FRect IslandAabb(const FIsland& Island)
		{
			return {
				Island.X + Island.LocalBounds.Left,
				Island.Y + Island.LocalBounds.Top,
				Island.X + Island.LocalBounds.Right,
				Island.Y + Island.LocalBounds.Bottom
			};
		}

		static void ApplyIslandOffsets(TArray<FNode>& Nodes, const TArray<FIsland>& Islands)
		{
			for (const FIsland& Island : Islands)
			{
				for (const int32 NodeIndex : Island.Nodes)
				{
					if (Nodes.IsValidIndex(NodeIndex))
					{
						Nodes[NodeIndex].NewX += Island.X;
						Nodes[NodeIndex].NewY += Island.Y;
					}
				}
			}
		}

		static void SnapIslandOffsetsToPixelGrid(TArray<FIsland>& Islands)
		{
			for (FIsland& Island : Islands)
			{
				Island.X = SnapGraphCoordinate(Island.X);
				Island.Y = SnapGraphCoordinate(Island.Y);
				Island.PreferredY = SnapGraphCoordinate(Island.PreferredY);
			}
		}

		static TArray<int32> BuildIslandComponentMap(
			const TArray<FIsland>& Islands,
			const TArray<FIslandEdge>& IslandEdges,
			const TSet<int32>& RouteOnlyIslandEdgeIndices)
		{
			TArray<TArray<int32>> Adjacent;
			Adjacent.SetNum(Islands.Num());
			for (int32 IslandEdgeIndex = 0; IslandEdgeIndex < IslandEdges.Num(); ++IslandEdgeIndex)
			{
				const FIslandEdge& Edge = IslandEdges[IslandEdgeIndex];
				if (RouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex)
					|| !Islands.IsValidIndex(Edge.SourceIsland)
					|| !Islands.IsValidIndex(Edge.TargetIsland))
				{
					continue;
				}
				Adjacent[Edge.SourceIsland].Add(Edge.TargetIsland);
				Adjacent[Edge.TargetIsland].Add(Edge.SourceIsland);
			}

			TArray<int32> ComponentByIsland;
			ComponentByIsland.Init(INDEX_NONE, Islands.Num());
			TArray<uint8> Visited;
			Visited.Init(0, Islands.Num());
			for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
			{
				if (Visited[IslandIndex])
				{
					continue;
				}
				const int32 ComponentId = IslandIndex;
				TArray<int32> Queue;
				Queue.Add(IslandIndex);
				Visited[IslandIndex] = 1;
				for (int32 Cursor = 0; Cursor < Queue.Num(); ++Cursor)
				{
					const int32 Current = Queue[Cursor];
					ComponentByIsland[Current] = ComponentId;
					for (const int32 Next : Adjacent[Current])
					{
						if (Islands.IsValidIndex(Next) && !Visited[Next])
						{
							Visited[Next] = 1;
							Queue.Add(Next);
						}
					}
				}
			}
			return ComponentByIsland;
		}

		static void AssignIslandPlacementRanks(
			TArray<FIsland>& Islands,
			const TArray<FIslandEdge>& IslandEdges,
			const TSet<int32>& RouteOnlyIslandEdgeIndices)
		{
			for (FIsland& Island : Islands)
			{
				Island.Rank = 0;
			}
			for (int32 Iteration = 0; Iteration < Islands.Num(); ++Iteration)
			{
				bool bChanged = false;
				for (int32 IslandEdgeIndex = 0; IslandEdgeIndex < IslandEdges.Num(); ++IslandEdgeIndex)
				{
					const FIslandEdge& Edge = IslandEdges[IslandEdgeIndex];
					if (RouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex)
						|| !Islands.IsValidIndex(Edge.SourceIsland)
						|| !Islands.IsValidIndex(Edge.TargetIsland))
					{
						continue;
					}
					const int32 CandidateRank = Islands[Edge.SourceIsland].Rank + 1;
					if (CandidateRank > Islands[Edge.TargetIsland].Rank)
					{
						Islands[Edge.TargetIsland].Rank = CandidateRank;
						bChanged = true;
					}
				}
				if (!bChanged)
				{
					break;
				}
			}
		}

		static TArray<FRect> BuildIslandHardOccupancyRects(
			const TArray<FNode>& Nodes,
			const FIsland& Island,
			const FOptions& Options,
			const TSet<int32>* IgnoreNodes = nullptr)
		{
			TArray<FRect> Rects;
			const double Padding = FMath::Max(4.0, Options.NodePadding * 0.25);
			bool bSkippedIgnoredNode = false;
			for (const int32 NodeIndex : Island.Nodes)
			{
				if (IgnoreNodes && IgnoreNodes->Contains(NodeIndex))
				{
					bSkippedIgnoredNode = true;
					continue;
				}
				if (!Nodes.IsValidIndex(NodeIndex))
				{
					continue;
				}
				const FNode& Node = Nodes[NodeIndex];
				Rects.Add({
					Island.X + Node.NewX - Padding,
					Island.Y + Node.NewY - Padding,
					Island.X + Node.NewX + Node.Width + Padding,
					Island.Y + Node.NewY + Node.Height + Padding
				});
			}
			for (const FInputChain& Chain : Island.InputChains)
			{
				TArray<FRect> SegmentBounds = Chain.SegmentBounds;
				if (SegmentBounds.Num() == 0)
				{
					SegmentBounds = BuildInputChainOwnSegmentBounds(Nodes, Chain);
				}
				for (const FRect& Segment : SegmentBounds)
				{
					if (Segment.Width() <= 0.0 || Segment.Height() <= 0.0)
					{
						continue;
					}
					Rects.Add({
						Island.X + Segment.Left - Padding,
						Island.Y + Segment.Top - Padding,
						Island.X + Segment.Right + Padding,
						Island.Y + Segment.Bottom + Padding
					});
				}
			}
			if (Rects.Num() == 0 && !bSkippedIgnoredNode)
			{
				Rects.Add(IslandAabb(Island));
			}
			return Rects;
		}

		static TArray<FRect> BuildAllHardOccupancyRects(
			const TArray<FNode>& Nodes,
			const TArray<FIsland>& Islands,
			const FOptions& Options,
			const TSet<int32>* IgnoreIslands = nullptr,
			const TSet<int32>* IgnoreNodes = nullptr)
		{
			TArray<FRect> Rects;
			for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
			{
				if (IgnoreIslands && IgnoreIslands->Contains(IslandIndex))
				{
					continue;
				}
				if (Islands.IsValidIndex(IslandIndex))
				{
					Rects.Append(BuildIslandHardOccupancyRects(Nodes, Islands[IslandIndex], Options, IgnoreNodes));
				}
			}
			return Rects;
		}

		static TArray<FRect> BuildRouteHardOccupancyRects(
			const TArray<FNode>& Nodes,
			const TArray<FIsland>& Islands,
			const FOptions& Options,
			const FEdge& Edge)
		{
			TSet<int32> IgnoreNodes;
			if (Nodes.IsValidIndex(Edge.Source))
			{
				IgnoreNodes.Add(Edge.Source);
			}
			if (Nodes.IsValidIndex(Edge.Target))
			{
				IgnoreNodes.Add(Edge.Target);
			}
			for (const FIsland& Island : Islands)
			{
				if (!Island.bCommentIsland || Island.CommentNodeIndex == INDEX_NONE)
				{
					continue;
				}
				if (Island.NodeSet.Contains(Edge.Source) || Island.NodeSet.Contains(Edge.Target))
				{
					IgnoreNodes.Add(Island.CommentNodeIndex);
				}
			}
			return BuildAllHardOccupancyRects(Nodes, Islands, Options, nullptr, &IgnoreNodes);
		}

		static bool TryFindNodeWorldRectInIslands(
			const TArray<FNode>& Nodes,
			const TArray<FIsland>& Islands,
			const int32 NodeIndex,
			FRect& OutRect)
		{
			if (!Nodes.IsValidIndex(NodeIndex))
			{
				return false;
			}
			for (const FIsland& Island : Islands)
			{
				if (Island.NodeSet.Contains(NodeIndex))
				{
					OutRect = MakeNodeRectWithOffset(Nodes[NodeIndex], FVector2D(Island.X, Island.Y), true);
					return true;
				}
			}
			return false;
		}

		static bool SegmentHitsAnyRect(const FVector2D& A, const FVector2D& B, const TArray<FRect>& Rects)
		{
			for (const FRect& Rect : Rects)
			{
				if (SegmentIntersectsRect(A, B, Rect))
				{
					return true;
				}
			}
			return false;
		}

		static bool PolylineHitsHardOccupancy(const TArray<FVector2D>& Points, const TArray<FRect>& HardRects)
		{
			for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
			{
				if (SegmentHitsAnyRect(Points[Index], Points[Index + 1], HardRects))
				{
					return true;
				}
			}
			return false;
		}

		static int32 CountPolylineHardOccupancyHits(const TArray<FVector2D>& Points, const TArray<FRect>& HardRects)
		{
			int32 Count = 0;
			for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
			{
				for (const FRect& Rect : HardRects)
				{
					if (SegmentIntersectsRect(Points[Index], Points[Index + 1], Rect))
					{
						++Count;
					}
				}
			}
			return Count;
		}

		static bool EndpointOffsetTrianglesHitsHardOccupancy(const TArray<TArray<FVector2D>>& Triangles, const TArray<FRect>& HardRects)
		{
			for (const FRect& Rect : HardRects)
			{
				if (EndpointOffsetTrianglesIntersectsRect(Triangles, Rect))
				{
					return true;
				}
			}
			return false;
		}

		static int32 CountControlWireHardOccupancyHits(const TArray<FVector2D>& ControlPoints, const TArray<FRect>& HardRects)
		{
			int32 Count = 0;
			for (int32 Index = 0; Index + 1 < ControlPoints.Num(); ++Index)
			{
				const TArray<FVector2D> RenderedSegment = BuildDirectSplinePolyline(ControlPoints[Index], ControlPoints[Index + 1]);
				Count += CountPolylineHardOccupancyHits(RenderedSegment, HardRects);
			}
			return Count;
		}

		static bool ControlWireHitsHardOccupancy(const TArray<FVector2D>& ControlPoints, const TArray<FRect>& HardRects)
		{
			return CountControlWireHardOccupancyHits(ControlPoints, HardRects) > 0;
		}

		static bool TryBuildDirectEndpointKnotControlPoints(
			const FVector2D& SourcePoint,
			const FVector2D& TargetPoint,
			const FOptions& Options,
			TArray<FVector2D>& OutControlPoints,
			TArray<FVector2D>* OutInteriorPoints = nullptr)
		{
			OutControlPoints.Reset();
			if (OutInteriorPoints)
			{
				OutInteriorPoints->Reset();
			}
			const FVector2D SourceKnot(SourcePoint.X + Options.OutputAccessOffsetX, SourcePoint.Y);
			const FVector2D TargetKnot(TargetPoint.X - Options.InputAccessOffsetX, TargetPoint.Y);
			if (!FMath::IsFinite(SourceKnot.X)
				|| !FMath::IsFinite(SourceKnot.Y)
				|| !FMath::IsFinite(TargetKnot.X)
				|| !FMath::IsFinite(TargetKnot.Y))
			{
				return false;
			}
			if (SourceKnot.X > TargetKnot.X + 0.5)
			{
				return false;
			}
			OutControlPoints.Add(SourcePoint);
			OutControlPoints.Add(SourceKnot);
			OutControlPoints.Add(TargetKnot);
			OutControlPoints.Add(TargetPoint);
			if (OutInteriorPoints)
			{
				OutInteriorPoints->Add(SourceKnot);
				OutInteriorPoints->Add(TargetKnot);
			}
			return true;
		}

		static double PolylineManhattanLength(const TArray<FVector2D>& Points)
		{
			double Length = 0.0;
			for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
			{
				Length += FMath::Abs(Points[Index + 1].X - Points[Index].X)
					+ FMath::Abs(Points[Index + 1].Y - Points[Index].Y);
			}
			return Length;
		}

		static bool IsSameRowLeftEntryToTarget(const FIslandPortCandidate& Candidate)
		{
			return Candidate.Side == TEXT("left")
				&& FMath::IsNearlyEqual(Candidate.TargetLocal.Y, Candidate.ActualTargetLocal.Y, 0.5);
		}

		static bool UsesDirectLikeEntryRightLimit(const FIslandPortCandidate& Candidate)
		{
			return IsSameRowLeftEntryToTarget(Candidate) || Candidate.bSingleNodeBoundaryBus;
		}

		static bool IsSameRowForwardSegment(const FVector2D& Start, const FVector2D& End)
		{
			return FMath::IsNearlyEqual(Start.Y, End.Y, 0.5)
				&& Start.X <= End.X + 0.5;
		}

		static constexpr double SameRowEntryTargetMergeTolerance()
		{
			return 5.0;
		}

		static double ComputeMaxEntryLocalXToTarget(const FIslandPortCandidate& Candidate, const double TargetInputX, const FIsland& TargetIsland, const FOptions& Options)
		{
			if (UsesDirectLikeEntryRightLimit(Candidate))
			{
				return Candidate.ActualTargetLocal.X;
			}
			return ComputeMaxOutputXForNonNegativeShorten(TargetInputX, Options) - TargetIsland.X;
		}

		static double ComputeMaxEntryLocalXToTarget(const FIslandPortCandidate& Candidate, const FOptions& Options)
		{
			if (UsesDirectLikeEntryRightLimit(Candidate))
			{
				return Candidate.ActualTargetLocal.X;
			}
			return ComputeMaxOutputXForNonNegativeShorten(Candidate.ActualTargetLocal.X, Options);
		}

		static double MinTargetDeltaXForCandidate(const FIslandPortCandidate& Candidate, const FOptions& Options)
		{
			const double Padding = Candidate.Side == TEXT("left") ? FMath::Max(0.0, Options.NodePadding) : 0.0;
			const double MaxEntryXForTarget =
				FMath::Min(Candidate.AllowedMaxX, ComputeMaxEntryLocalXToTarget(Candidate, Options));
			const double FeasibleEntryX = MaxEntryXForTarget >= Candidate.AllowedMinX - 0.5
				? MaxEntryXForTarget
				: Candidate.AllowedMinX;
			return ComputeTargetOriginDeltaXForAlignedConnection(
				Candidate.SourceLocal,
				FVector2D(FeasibleEntryX, Candidate.TargetLocal.Y),
				Options) + Padding;
		}

		static FVector2D ResolveEntryLocalAfterPlacement(
			const FIslandPortCandidate& Candidate,
			const FIsland& SourceIsland,
			const FIsland& TargetIsland,
			const FOptions& Options,
			const double MinSourceToEntrySpanX = 0.0)
		{
			FVector2D EntryLocal = Candidate.TargetLocal;
			if (Candidate.AllowedMaxX > Candidate.AllowedMinX + 0.5
				|| Candidate.Side == TEXT("top")
				|| Candidate.Side == TEXT("bottom"))
			{
				const double SourceOutputX = SourceIsland.X + Candidate.SourceLocal.X;
				const double TargetInputX = TargetIsland.X + Candidate.ActualTargetLocal.X;
				const double MinEntryXFromSource =
					SourceOutputX
					+ FMath::Max(EndpointAccessClearanceX(Options), MinSourceToEntrySpanX)
					- TargetIsland.X;
				const double MaxEntryXToTarget = ComputeMaxEntryLocalXToTarget(Candidate, TargetInputX, TargetIsland, Options);
				const double Lower = FMath::Max(Candidate.AllowedMinX, MinEntryXFromSource);
				const double Upper = FMath::Min(Candidate.AllowedMaxX, MaxEntryXToTarget);
				if (Lower <= Upper)
				{
					const bool bBusPathEntry = MinSourceToEntrySpanX > EndpointAccessClearanceX(Options) + 0.5;
					const bool bSameRowLeftEntry = IsSameRowLeftEntryToTarget(Candidate);
					const double DesiredEntryX = (bBusPathEntry && !bSameRowLeftEntry) ? Upper : EntryLocal.X;
					EntryLocal.X = FMath::Clamp(DesiredEntryX, Lower, Upper);
				}
				else
				{
					EntryLocal.X = Upper;
				}
			}
			return EntryLocal;
		}

		static bool IsCandidateForwardValid(
			const FIsland& SourceIsland,
			const FIsland& TargetIsland,
			const FSelectedEndpointCandidate& Selected,
			const FOptions& Options)
		{
			const FVector2D SourcePoint(SourceIsland.X + Selected.SourceLocal.X, SourceIsland.Y + Selected.SourceLocal.Y);
			const FVector2D EntryPoint(TargetIsland.X + Selected.EntryLocal.X, TargetIsland.Y + Selected.EntryLocal.Y);
			const FVector2D TargetInputPoint(TargetIsland.X + Selected.ActualTargetLocal.X, TargetIsland.Y + Selected.ActualTargetLocal.Y);
			if (FVector2D::Distance(EntryPoint, TargetInputPoint) <= 0.5)
			{
				return HasNonNegativeConnectionAlignmentShorten(SourcePoint, TargetInputPoint, Options);
			}
			if (Selected.Side == TEXT("left") && IsSameRowForwardSegment(EntryPoint, TargetInputPoint))
			{
				return HasNonNegativeConnectionAlignmentShorten(SourcePoint, EntryPoint, Options);
			}
			return HasNonNegativeConnectionAlignmentShorten(SourcePoint, EntryPoint, Options)
				&& HasNonNegativeConnectionAlignmentShorten(EntryPoint, TargetInputPoint, Options);
		}

		static FPlacementProposal MakePlacementProposal(
			const FIslandPortCandidate& Candidate,
			const TSet<int32>& RouteOnlyEdgeIndices,
			const FOptions& Options)
		{
			FPlacementProposal Proposal;
			Proposal.Id = Candidate.Id;
			Proposal.EdgeIndex = Candidate.EdgeIndex;
			Proposal.CandidateId = Candidate.Id;
			Proposal.SourceIsland = Candidate.SourceIsland;
			Proposal.TargetIsland = Candidate.TargetIsland;
			Proposal.Side = Candidate.Side;
			Proposal.SourceLocal = Candidate.SourceLocal;
			Proposal.EntryLocal = Candidate.TargetLocal;
			Proposal.ActualTargetLocal = Candidate.ActualTargetLocal;
			Proposal.RequiredTargetDeltaX = MinTargetDeltaXForCandidate(Candidate, Options);
			Proposal.bRouteOnly = RouteOnlyEdgeIndices.Contains(Candidate.EdgeIndex);
			const double RequiredDeltaX = FMath::Max(0.0, Proposal.RequiredTargetDeltaX);
			const double VerticalResidual = FMath::Abs(Candidate.SourceLocal.Y - Candidate.ActualTargetLocal.Y);
			const double LeftOverstretchCost = Candidate.Side == TEXT("left")
				? FMath::Max(0.0, RequiredDeltaX - Options.HorizontalSpacing) * 0.08
				: 0.0;
			const double EdgeEntryCost = Candidate.Side == TEXT("left") ? 0.0 : 6.0;
			Proposal.Score =
				RequiredDeltaX * 0.12
				+ VerticalResidual * 0.035
				+ LeftOverstretchCost
				+ EdgeEntryCost
				+ static_cast<double>(Candidate.Id) * 0.001;
			return Proposal;
		}

		static void AddPlacementProposalDiagnostics(
			const TArray<FPlacementProposal>& Proposals,
			FRunResult& Result)
		{
			Result.DesignDiagnostics.PlacementProposalCount = Proposals.Num();
			for (const FPlacementProposal& Proposal : Proposals)
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetNumberField(TEXT("proposal_id"), Proposal.Id);
				Obj->SetNumberField(TEXT("edge_index"), Proposal.EdgeIndex);
				Obj->SetNumberField(TEXT("candidate_id"), Proposal.CandidateId);
				Obj->SetNumberField(TEXT("source_island_id"), Proposal.SourceIsland);
				Obj->SetNumberField(TEXT("target_island_id"), Proposal.TargetIsland);
				Obj->SetStringField(TEXT("side"), Proposal.Side);
				Obj->SetNumberField(TEXT("required_target_delta_x"), Proposal.RequiredTargetDeltaX);
				Obj->SetNumberField(TEXT("score"), Proposal.Score);
				Obj->SetBoolField(TEXT("route_only"), Proposal.bRouteOnly);
				Result.PlacementProposalDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}

		static FSelectedEndpointCandidate SelectEndpointFromProposals(
			const TArray<FIslandPortCandidate>& Candidates,
			const TArray<FPlacementProposal>& Proposals,
			const int32 EdgeIndex,
			FRunResult& Result)
		{
			FSelectedEndpointCandidate Selected;
			double BestScore = TNumericLimits<double>::Max();
			for (const FPlacementProposal& Proposal : Proposals)
			{
				if (Proposal.EdgeIndex != EdgeIndex || !Candidates.IsValidIndex(Proposal.CandidateId))
				{
					continue;
				}
				if (Proposal.Score < BestScore)
				{
					const FIslandPortCandidate& Candidate = Candidates[Proposal.CandidateId];
					BestScore = Proposal.Score;
					Selected.EdgeIndex = Candidate.EdgeIndex;
					Selected.CandidateId = Candidate.Id;
					Selected.SourceIsland = Candidate.SourceIsland;
					Selected.TargetIsland = Candidate.TargetIsland;
					Selected.BoundaryEntryId = Candidate.BoundaryEntryId;
					Selected.InternalBusGroupId = Candidate.InternalBusGroupId;
					Selected.Side = Candidate.Side;
					Selected.SourceLocal = Candidate.SourceLocal;
					Selected.EntryLocal = Candidate.TargetLocal;
					Selected.ActualTargetLocal = Candidate.ActualTargetLocal;
					Selected.MinTargetDeltaX = Proposal.RequiredTargetDeltaX;
					Selected.PrimaryResidual = Proposal.Score;
					Selected.SourcePortOrderKey = Candidate.SourceLocal.Y;
				}
			}
			if (Selected.EdgeIndex == INDEX_NONE)
			{
				++Result.DesignDiagnostics.RejectedProposalCount;
			}
			return Selected;
		}

		static void ResolveSelectedEndpointEntry(
			const TArray<FIslandPortCandidate>& Candidates,
			const TArray<FIsland>& Islands,
			const FOptions& Options,
			FSelectedEndpointCandidate& Selected,
			const double MinSourceToEntrySpanX = 0.0)
		{
			if (!Candidates.IsValidIndex(Selected.CandidateId)
				|| !Islands.IsValidIndex(Selected.SourceIsland)
				|| !Islands.IsValidIndex(Selected.TargetIsland))
			{
				return;
			}
			Selected.EntryLocal = ResolveEntryLocalAfterPlacement(
				Candidates[Selected.CandidateId],
				Islands[Selected.SourceIsland],
				Islands[Selected.TargetIsland],
				Options,
				MinSourceToEntrySpanX);
		}

		static void AddSelectedEndpointDiagnostics(
			FRunResult& Result,
			const FSelectedEndpointCandidate& Selected,
			const TArray<FIsland>& Islands,
			const FOptions& Options)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("edge_index"), Selected.EdgeIndex);
			Obj->SetNumberField(TEXT("candidate_id"), Selected.CandidateId);
			Obj->SetNumberField(TEXT("source_island_id"), Selected.SourceIsland);
			Obj->SetNumberField(TEXT("target_island_id"), Selected.TargetIsland);
			Obj->SetNumberField(TEXT("boundary_entry_id"), Selected.BoundaryEntryId);
			Obj->SetNumberField(TEXT("internal_bus_group_id"), Selected.InternalBusGroupId);
			Obj->SetStringField(TEXT("side"), Selected.Side);
			Obj->SetNumberField(TEXT("connection_group_id"), Selected.ConnectionGroupId);
			Obj->SetNumberField(TEXT("inter_island_bus_id"), Selected.InterIslandBusId);
			Obj->SetNumberField(TEXT("min_target_delta_x"), Selected.MinTargetDeltaX);
			Obj->SetNumberField(TEXT("primary_residual"), Selected.PrimaryResidual);
			Obj->SetNumberField(TEXT("source_port_order_key"), Selected.SourcePortOrderKey);
			Obj->SetBoolField(TEXT("placement_owner"), Selected.bPlacementOwner);
			Obj->SetObjectField(TEXT("source_local"), PointToJson(Selected.SourceLocal));
			Obj->SetObjectField(TEXT("entry_local"), PointToJson(Selected.EntryLocal));
			Obj->SetObjectField(TEXT("actual_target_local"), PointToJson(Selected.ActualTargetLocal));
			if (Islands.IsValidIndex(Selected.SourceIsland) && Islands.IsValidIndex(Selected.TargetIsland))
			{
				const FIsland& SourceIsland = Islands[Selected.SourceIsland];
				const FIsland& TargetIsland = Islands[Selected.TargetIsland];
				const FVector2D SourceAbs(SourceIsland.X + Selected.SourceLocal.X, SourceIsland.Y + Selected.SourceLocal.Y);
				const FVector2D EntryAbs(TargetIsland.X + Selected.EntryLocal.X, TargetIsland.Y + Selected.EntryLocal.Y);
				const FVector2D ActualTargetAbs(TargetIsland.X + Selected.ActualTargetLocal.X, TargetIsland.Y + Selected.ActualTargetLocal.Y);
				const double EntryToTargetShorten = (Selected.Side == TEXT("left") && IsSameRowForwardSegment(EntryAbs, ActualTargetAbs))
					? 0.0
					: ComputeConnectionAlignmentShortenX(EntryAbs, ActualTargetAbs, Options);
				Obj->SetObjectField(TEXT("source_abs"), PointToJson(SourceAbs));
				Obj->SetObjectField(TEXT("entry_abs"), PointToJson(EntryAbs));
				Obj->SetObjectField(TEXT("actual_target_abs"), PointToJson(ActualTargetAbs));
				Obj->SetNumberField(TEXT("alignment_shorten_x"), ComputeConnectionAlignmentShortenX(SourceAbs, ActualTargetAbs, Options));
				Obj->SetNumberField(TEXT("source_to_entry_shorten_x"), ComputeConnectionAlignmentShortenX(SourceAbs, EntryAbs, Options));
				Obj->SetNumberField(TEXT("entry_to_target_shorten_x"), EntryToTargetShorten);
				Obj->SetBoolField(TEXT("forward_valid"), IsCandidateForwardValid(SourceIsland, TargetIsland, Selected, Options));
			}
			Result.SelectedEndpointCandidateDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
		}

		static int32 FindIslandColumnForNode(const FIsland& Island, const int32 NodeIndex);
		struct FSourcePortOrderConstraint
		{
			int32 SourceIsland = INDEX_NONE;
			int32 UpperTargetIsland = INDEX_NONE;
			int32 LowerTargetIsland = INDEX_NONE;
			double UpperSourcePortY = 0.0;
			double LowerSourcePortY = 0.0;
		};

		static int64 MakeIslandPairKey(const int32 A, const int32 B)
		{
			return (static_cast<int64>(A) << 32) ^ static_cast<uint32>(B);
		}

		static TArray<FSourcePortOrderConstraint> BuildSourcePortOrderConstraints(
			const TArray<FIsland>& Islands,
			const TArray<FSelectedEndpointCandidate>& SelectedEndpoints,
			const TSet<int32>* OwnerEdgeIndices = nullptr)
		{
			struct FTargetOrderBucket
			{
				TArray<double> SourcePortYs;
			};

			TMap<int32, TMap<int32, FTargetOrderBucket>> TargetsBySource;
			for (const FSelectedEndpointCandidate& Selected : SelectedEndpoints)
			{
				if (OwnerEdgeIndices && !OwnerEdgeIndices->Contains(Selected.EdgeIndex))
				{
					continue;
				}
				if (!Islands.IsValidIndex(Selected.SourceIsland)
					|| !Islands.IsValidIndex(Selected.TargetIsland)
					|| Selected.SourceIsland == Selected.TargetIsland)
				{
					continue;
				}
				TargetsBySource
					.FindOrAdd(Selected.SourceIsland)
					.FindOrAdd(Selected.TargetIsland)
					.SourcePortYs.Add(Selected.SourcePortOrderKey);
			}

			TArray<FSourcePortOrderConstraint> Constraints;
			TSet<int64> SeenPairs;
			for (const TPair<int32, TMap<int32, FTargetOrderBucket>>& SourcePair : TargetsBySource)
			{
				TArray<TPair<int32, double>> TargetKeys;
				for (const TPair<int32, FTargetOrderBucket>& TargetPair : SourcePair.Value)
				{
					if (TargetPair.Value.SourcePortYs.Num() == 0)
					{
						continue;
					}
					TargetKeys.Add(TPair<int32, double>(TargetPair.Key, MedianOf(TargetPair.Value.SourcePortYs, 0.0)));
				}
				if (TargetKeys.Num() <= 1)
				{
					continue;
				}
				TargetKeys.Sort([](const TPair<int32, double>& A, const TPair<int32, double>& B)
				{
					if (!FMath::IsNearlyEqual(A.Value, B.Value, 0.5))
					{
						return A.Value < B.Value;
					}
					return A.Key < B.Key;
				});

				for (int32 UpperIndex = 0; UpperIndex < TargetKeys.Num(); ++UpperIndex)
				{
					for (int32 LowerIndex = UpperIndex + 1; LowerIndex < TargetKeys.Num(); ++LowerIndex)
					{
						if (FMath::IsNearlyEqual(TargetKeys[UpperIndex].Value, TargetKeys[LowerIndex].Value, 0.5))
						{
							continue;
						}
						const int64 PairKey = MakeIslandPairKey(TargetKeys[UpperIndex].Key, TargetKeys[LowerIndex].Key);
						if (SeenPairs.Contains(PairKey))
						{
							continue;
						}
						SeenPairs.Add(PairKey);

						FSourcePortOrderConstraint Constraint;
						Constraint.SourceIsland = SourcePair.Key;
						Constraint.UpperTargetIsland = TargetKeys[UpperIndex].Key;
						Constraint.LowerTargetIsland = TargetKeys[LowerIndex].Key;
						Constraint.UpperSourcePortY = TargetKeys[UpperIndex].Value;
						Constraint.LowerSourcePortY = TargetKeys[LowerIndex].Value;
						Constraints.Add(Constraint);
					}
				}
			}
			return Constraints;
		}

		static int32 CompareIslandsBySourcePortOrder(
			const int32 A,
			const int32 B,
			const TArray<FSourcePortOrderConstraint>& Constraints)
		{
			int32 Vote = 0;
			for (const FSourcePortOrderConstraint& Constraint : Constraints)
			{
				if (Constraint.UpperTargetIsland == A && Constraint.LowerTargetIsland == B)
				{
					--Vote;
				}
				else if (Constraint.UpperTargetIsland == B && Constraint.LowerTargetIsland == A)
				{
					++Vote;
				}
			}
			if (Vote < 0)
			{
				return -1;
			}
			if (Vote > 0)
			{
				return 1;
			}
			return 0;
		}

		static double MinYFromPlacedSourcePortOrderPredecessors(
			const int32 IslandIndex,
			const TArray<FIsland>& Islands,
			const TArray<uint8>& Placed,
			const TArray<FSourcePortOrderConstraint>& Constraints,
			const FOptions& Options)
		{
			double MinY = -TNumericLimits<double>::Max();
			for (const FSourcePortOrderConstraint& Constraint : Constraints)
			{
				if (Constraint.LowerTargetIsland != IslandIndex
					|| !Islands.IsValidIndex(Constraint.UpperTargetIsland)
					|| !Placed.IsValidIndex(Constraint.UpperTargetIsland)
					|| !Placed[Constraint.UpperTargetIsland])
				{
					continue;
				}
				MinY = FMath::Max(MinY, IslandAabb(Islands[Constraint.UpperTargetIsland]).Bottom + Options.NodePadding);
			}
			return MinY;
		}

		static int32 CountSourcePortOrderViolations(
			const TArray<FIsland>& Islands,
			const TArray<FSourcePortOrderConstraint>& Constraints)
		{
			int32 ViolationCount = 0;
			for (const FSourcePortOrderConstraint& Constraint : Constraints)
			{
				if (!Islands.IsValidIndex(Constraint.UpperTargetIsland) || !Islands.IsValidIndex(Constraint.LowerTargetIsland))
				{
					continue;
				}
				const FRect UpperRect = IslandAabb(Islands[Constraint.UpperTargetIsland]);
				const FRect LowerRect = IslandAabb(Islands[Constraint.LowerTargetIsland]);
				const double UpperCenterY = (UpperRect.Top + UpperRect.Bottom) * 0.5;
				const double LowerCenterY = (LowerRect.Top + LowerRect.Bottom) * 0.5;
				if (UpperCenterY >= LowerCenterY - 0.5)
				{
					++ViolationCount;
				}
			}
			return ViolationCount;
		}

		static bool ShouldUseTargetDoglegForBusBranch(
			const double TrunkY,
			const double TargetY,
			const FOptions& Options)
		{
			const double SmallVerticalDelta = FMath::Max(
				GetFallbackInterfaceRowSpacing(Options),
				Options.NodePadding);
			return FMath::Abs(TrunkY - TargetY) > SmallVerticalDelta;
		}

		static void CompactRoutePoints(FReroutePlan& Plan, const FVector2D& Start, const FVector2D& End, const FOptions& Options, const FVector2D* ProtectedPoint = nullptr)
		{
			const double MinDistance = FMath::Max(10.0, Options.NodePadding * 0.35);
			TArray<FVector2D> InteriorPoints;
			InteriorPoints.Reserve(Plan.Points.Num());
			for (const FVector2D& Point : Plan.Points)
			{
				const FVector2D SnappedPoint = SnapGraphPoint(Point);
				const bool bProtectedPoint = ProtectedPoint
					&& FVector2D::Distance(SnappedPoint, SnapGraphPoint(*ProtectedPoint)) <= 0.5;
				if (!bProtectedPoint
					&& (FVector2D::Distance(SnappedPoint, Start) < MinDistance || FVector2D::Distance(SnappedPoint, End) < MinDistance))
				{
					continue;
				}
				if (!bProtectedPoint
					&& InteriorPoints.Num() > 0
					&& FVector2D::Distance(InteriorPoints.Last(), SnappedPoint) < MinDistance)
				{
					continue;
				}
				InteriorPoints.Add(SnappedPoint);
			}

			TArray<FVector2D> FullRoute;
			FullRoute.Reserve(InteriorPoints.Num() + 2);
			FullRoute.Add(SnapGraphPoint(Start));
			for (const FVector2D& Point : InteriorPoints)
			{
				FullRoute.Add(Point);
			}
			FullRoute.Add(SnapGraphPoint(End));

			bool bChanged = true;
			while (bChanged)
			{
				bChanged = false;
				for (int32 RouteIndex = 1; RouteIndex + 1 < FullRoute.Num(); ++RouteIndex)
				{
					const FVector2D A = FullRoute[RouteIndex - 1];
					const FVector2D B = FullRoute[RouteIndex];
					const FVector2D C = FullRoute[RouteIndex + 1];
					const bool bProtectedPoint = ProtectedPoint
						&& FVector2D::Distance(B, SnapGraphPoint(*ProtectedPoint)) <= 0.5;
					if (bProtectedPoint)
					{
						continue;
					}
					const bool bPrevProtectedPoint = ProtectedPoint
						&& FVector2D::Distance(A, SnapGraphPoint(*ProtectedPoint)) <= 0.5;
					const bool bNextProtectedPoint = ProtectedPoint
						&& FVector2D::Distance(C, SnapGraphPoint(*ProtectedPoint)) <= 0.5;
					const bool bProtectedTrunkNeighbor =
						(bPrevProtectedPoint
							&& FMath::IsNearlyEqual(A.Y, B.Y, 0.5)
							&& FMath::Abs(B.X - A.X) >= MinDistance)
						|| (bNextProtectedPoint
							&& FMath::IsNearlyEqual(B.Y, C.Y, 0.5)
							&& FMath::Abs(C.X - B.X) >= MinDistance);
					if (bProtectedTrunkNeighbor)
					{
						continue;
					}
					const bool bCloseToNeighbor =
						FVector2D::Distance(A, B) < MinDistance
						|| FVector2D::Distance(B, C) < MinDistance;
					const bool bSameX = FMath::IsNearlyEqual(A.X, B.X, 0.5) && FMath::IsNearlyEqual(B.X, C.X, 0.5);
					const bool bSameY = FMath::IsNearlyEqual(A.Y, B.Y, 0.5) && FMath::IsNearlyEqual(B.Y, C.Y, 0.5);
					if (bCloseToNeighbor || bSameX || bSameY)
					{
						FullRoute.RemoveAt(RouteIndex);
						bChanged = true;
						break;
					}
				}
			}

			Plan.Points.Reset();
			Plan.Points.Reserve(FMath::Max(0, FullRoute.Num() - 2));
			for (int32 RouteIndex = 1; RouteIndex + 1 < FullRoute.Num(); ++RouteIndex)
			{
				Plan.Points.Add(FullRoute[RouteIndex]);
			}
			FinalizeReroutePlanPoints(Plan, Options);
		}

		static void AddRouteDiagnostics(
			FRunResult& Result,
			int32 EdgeIndex,
			const FString& Side,
			ERouteKind RouteKind,
			const TArray<FVector2D>& Points,
			bool bExternalUpgrade,
			bool bCrossesHardOccupancy);

		static void AddVisibleRouteLaneReservationRects(
			const TArray<FVector2D>& Route,
			double LaneSpacing,
			const FOptions& Options,
			TArray<FRect>& OutRects);

		static int32 FindIslandColumnForNode(const FIsland& Island, const int32 NodeIndex)
		{
			for (int32 ColumnIndex = 0; ColumnIndex < Island.Columns.Num(); ++ColumnIndex)
			{
				if (Island.Columns[ColumnIndex].Contains(NodeIndex))
				{
					return ColumnIndex;
				}
			}
			return INDEX_NONE;
		}

		static double ComputeIslandColumnRightBoundaryX(
			const TArray<FNode>& Nodes,
			const FIsland& Island,
			const int32 ColumnIndex,
			const FOptions& Options)
		{
			(void)Options;
			double LocalRight = -TNumericLimits<double>::Max();
			if (Island.Columns.IsValidIndex(ColumnIndex))
			{
				for (const int32 NodeIndex : Island.Columns[ColumnIndex])
				{
					if (Nodes.IsValidIndex(NodeIndex))
					{
						LocalRight = FMath::Max(LocalRight, Nodes[NodeIndex].NewX + Nodes[NodeIndex].Width);
					}
				}
			}
			if (LocalRight <= -TNumericLimits<double>::Max() * 0.5)
			{
				LocalRight = Island.LocalBounds.Right;
			}
			return SnapGraphCoordinate(Island.X + LocalRight);
		}

		static TArray<FRect> BuildSameColumnSiblingHardRects(
			const TArray<FNode>& Nodes,
			const FIsland& Island,
			const int32 ColumnIndex,
			const int32 SourceNode,
			const int32 TargetNode,
			const FOptions& Options)
		{
			(void)Options;
			TArray<FRect> Rects;
			if (!Island.Columns.IsValidIndex(ColumnIndex))
			{
				return Rects;
			}
			for (const int32 NodeIndex : Island.Columns[ColumnIndex])
			{
				if (NodeIndex == SourceNode || NodeIndex == TargetNode || !Nodes.IsValidIndex(NodeIndex))
				{
					continue;
				}
				const FNode& Node = Nodes[NodeIndex];
				Rects.Add({
					Island.X + Node.NewX,
					Island.Y + Node.NewY,
					Island.X + Node.NewX + Node.Width,
					Island.Y + Node.NewY + Node.Height
				});
			}
			return Rects;
		}

		static TArray<int32> FindLocalColumnCollisionRouteColumns(
			const TArray<FNode>& Nodes,
			const FIsland& Island,
			const int32 SourceColumn,
			const int32 TargetColumn,
			const int32 SourceNode,
			const int32 TargetNode,
			const TArray<FVector2D>& DirectControlPoints,
			const FOptions& Options)
		{
			TArray<int32> CollisionColumns;
			if (SourceColumn == INDEX_NONE || TargetColumn == INDEX_NONE)
			{
				return CollisionColumns;
			}
			if (DirectControlPoints.Num() < 2)
			{
				return CollisionColumns;
			}

			const FVector2D IslandOffset(Island.X, Island.Y);
			const FRect SourceRect = MakeNodeRectWithOffset(Nodes[SourceNode], IslandOffset, true);
			const FRect TargetRect = MakeNodeRectWithOffset(Nodes[TargetNode], IslandOffset, true);
			TArray<TArray<FVector2D>> EndpointTriangles;
			if (!TryBuildEndpointOffsetCollisionTriangles(
				DirectControlPoints[0],
				&SourceRect,
				DirectControlPoints.Last(),
				&TargetRect,
				Options,
				EndpointTriangles))
			{
				return CollisionColumns;
			}
			const TArray<FRect> SiblingRects =
				BuildSameColumnSiblingHardRects(Nodes, Island, SourceColumn, SourceNode, TargetNode, Options);
			for (const FRect& Rect : SiblingRects)
			{
				if (EndpointOffsetTrianglesIntersectsRect(EndpointTriangles, Rect))
				{
					CollisionColumns.Add(SourceColumn);
					break;
				}
			}
			return CollisionColumns;
		}

		static TArray<FVector2D> BuildSourceColumnRightBoundaryRoute(
			const FVector2D& Start,
			const FVector2D& End,
			const double ColumnRightX,
			const FOptions& Options)
		{
			(void)Options;
			const double GuardX = SnapGraphCoordinate(ColumnRightX);
			TArray<FVector2D> Route;
			Route.Add(Start);
			Route.Add(FVector2D(GuardX, Start.Y));
			Route.Add(End);
			return Route;
		}

		static void InsertSourceColumnRightBoundaryPoint(
			TArray<FVector2D>& Route,
			const FVector2D& Start,
			const double ColumnRightX,
			const FOptions& Options)
		{
			(void)Options;
			if (Route.Num() < 2)
			{
				return;
			}
			const double GuardX = SnapGraphCoordinate(ColumnRightX);
			const FVector2D GuardPoint(GuardX, Start.Y);
			if (FVector2D::Distance(GuardPoint, Start) < 0.5)
			{
				return;
			}
			if (Route.Num() > 1 && FVector2D::Distance(Route[1], GuardPoint) < 0.5)
			{
				return;
			}
			if (Route.Num() > 1 && Route[1].X < GuardPoint.X - 0.5)
			{
				Route[1] = GuardPoint;
				return;
			}
			Route.Insert(GuardPoint, 1);
		}

		static FString MakeIslandInternalBusKey(const int32 IslandIndex, const FString& SourceParamId)
		{
			return FString::Printf(TEXT("%d|%s"), IslandIndex, *SourceParamId);
		}

		static TArray<FVector2D> BuildSharedSourceBusRoutePolyline(
			const FVector2D& Start,
			const FVector2D& End,
			double TrunkY,
			double BranchX,
			const FOptions& Options);

		static void AppendBusRouteTargetEndpoint(
			TArray<FVector2D>& Points,
			const FVector2D& End,
			const FOptions& Options);

		static bool AppendBusRouteSourceEntry(
			TArray<FVector2D>& Points,
			const FVector2D& Start,
			double TrunkY,
			double SourceEscapeX,
			double BranchX,
			const FOptions& Options,
			double& OutCurrentX);

		static TArray<FVector2D> BuildIslandInternalSharedBusRoute(
			const FVector2D& Start,
			const FVector2D& End,
			const double TrunkY,
			const FOptions& Options)
		{
			const double BranchX = ComputeOutputXAlignedToInput(End.X, Options);
			return BuildSharedSourceBusRoutePolyline(Start, End, TrunkY, BranchX, Options);
		}

		static void AppendIslandInternalReroutePlans(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const TMap<int32, int32>& IslandByNode,
			const FOptions& Options,
			TArray<FReroutePlan>& InOutPlans,
			FRunResult& Result,
			const TSet<int32>* SkipEdgeIndices = nullptr)
		{
			TSet<int32> PlannedEdgeIndices;
			for (const FReroutePlan& Plan : InOutPlans)
			{
				if (Edges.IsValidIndex(Plan.EdgeIndex))
				{
					PlannedEdgeIndices.Add(Plan.EdgeIndex);
				}
			}

			TSet<int32> InputChainEdges;
			for (const FIsland& Island : Islands)
			{
				const TSet<int32> IslandInputChainEdges = BuildInputChainEdgeSet(Island);
				for (const int32 EdgeIndex : IslandInputChainEdges)
				{
					InputChainEdges.Add(EdgeIndex);
				}
			}

			struct FSharedIslandInternalBusCandidate
			{
				int32 EdgeIndex = INDEX_NONE;
				int32 IslandIndex = INDEX_NONE;
				FString SourceParamId;
				FVector2D Start;
				FVector2D End;
			};

			TMap<FString, TArray<FSharedIslandInternalBusCandidate>> SharedBusCandidatesByKey;
			for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
			{
				if (PlannedEdgeIndices.Contains(EdgeIndex)
					|| InputChainEdges.Contains(EdgeIndex)
					|| (SkipEdgeIndices && SkipEdgeIndices->Contains(EdgeIndex)))
				{
					continue;
				}
				const FEdge& Edge = Edges[EdgeIndex];
				if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
				{
					continue;
				}
				const int32* SourceIslandPtr = IslandByNode.Find(Edge.Source);
				const int32* TargetIslandPtr = IslandByNode.Find(Edge.Target);
				if (!SourceIslandPtr || !TargetIslandPtr || *SourceIslandPtr != *TargetIslandPtr || !Islands.IsValidIndex(*SourceIslandPtr))
				{
					continue;
				}

				const FIsland& Island = Islands[*SourceIslandPtr];
				if (Island.bCommentIsland)
				{
					continue;
				}
				const int32 SourceColumn = FindIslandColumnForNode(Island, Edge.Source);
				const int32 TargetColumn = FindIslandColumnForNode(Island, Edge.Target);
				if (SourceColumn == INDEX_NONE || TargetColumn == INDEX_NONE || FMath::Abs(TargetColumn - SourceColumn) <= 1)
				{
					continue;
				}

				const FString SourceParamId = MakeSourceParamId(Edge);
				const FString SharedBusKey = MakeIslandInternalBusKey(*SourceIslandPtr, SourceParamId);
				const FVector2D IslandOffset(Island.X, Island.Y);
				FSharedIslandInternalBusCandidate Candidate;
				Candidate.EdgeIndex = EdgeIndex;
				Candidate.IslandIndex = *SourceIslandPtr;
				Candidate.SourceParamId = SourceParamId;
				Candidate.Start = IslandOffset + GetSourcePort(Nodes[Edge.Source], Edge, true);
				Candidate.End = IslandOffset + GetTargetPort(Nodes[Edge.Target], Edge, true);
				SharedBusCandidatesByKey.FindOrAdd(SharedBusKey).Add(Candidate);
			}

			TMap<FString, double> SharedTrunkYByBusKey;
			TMap<FString, int32> SharedMergeGroupByBusKey;
			int32 NextIslandInternalMergeGroup = -2;
			for (const TPair<FString, TArray<FSharedIslandInternalBusCandidate>>& Pair : SharedBusCandidatesByKey)
			{
				const TArray<FSharedIslandInternalBusCandidate>& Candidates = Pair.Value;
				if (Candidates.Num() <= 1)
				{
					continue;
				}

				TArray<double> CandidateYs;
				auto AddCandidateY = [&CandidateYs](const double Value)
				{
					if (!FMath::IsFinite(Value))
					{
						return;
					}
					const double SnappedValue = SnapGraphCoordinate(Value);
					for (const double Existing : CandidateYs)
					{
						if (FMath::IsNearlyEqual(Existing, SnappedValue, 0.5))
						{
							return;
						}
					}
					CandidateYs.Add(SnappedValue);
				};
				auto AddCandidateYWithLanes = [&AddCandidateY](const double Value)
				{
					AddCandidateY(Value);
					const double LaneSpacing = GetBusAvoidanceDistance();
					constexpr int32 MaxLocalLaneSearch = 8;
					for (int32 Offset = 1; Offset <= MaxLocalLaneSearch; ++Offset)
					{
						AddCandidateY(Value - static_cast<double>(Offset) * LaneSpacing);
						AddCandidateY(Value + static_cast<double>(Offset) * LaneSpacing);
					}
				};

				double InterfaceYSum = 0.0;
				int32 InterfaceYCount = 0;
				double RouteSpanLeft = TNumericLimits<double>::Max();
				double RouteSpanRight = -TNumericLimits<double>::Max();
				for (const FSharedIslandInternalBusCandidate& Candidate : Candidates)
				{
					if (!Edges.IsValidIndex(Candidate.EdgeIndex))
					{
						continue;
					}
					InterfaceYSum += Candidate.Start.Y + Candidate.End.Y;
					InterfaceYCount += 2;
					AddCandidateYWithLanes(Candidate.Start.Y);
					AddCandidateYWithLanes(Candidate.End.Y);
					AddCandidateYWithLanes((Candidate.Start.Y + Candidate.End.Y) * 0.5);
					RouteSpanLeft = FMath::Min(RouteSpanLeft, FMath::Min(Candidate.Start.X, Candidate.End.X));
					RouteSpanRight = FMath::Max(RouteSpanRight, FMath::Max(Candidate.Start.X, Candidate.End.X));
					const FEdge& Edge = Edges[Candidate.EdgeIndex];
					const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edge);
					const double EdgeSpanLeft = FMath::Min(Candidate.Start.X, Candidate.End.X);
					const double EdgeSpanRight = FMath::Max(Candidate.Start.X, Candidate.End.X);
					for (const FRect& Rect : RouteHardRects)
					{
						if (EdgeSpanRight < Rect.Left - 0.5 || Rect.Right < EdgeSpanLeft - 0.5)
						{
							continue;
						}
						AddCandidateYWithLanes(Rect.Top - GetBusAvoidanceDistance());
						AddCandidateYWithLanes(Rect.Bottom + GetBusAvoidanceDistance());
					}
				}
				if (InterfaceYCount > 0)
				{
					AddCandidateYWithLanes(InterfaceYSum / static_cast<double>(InterfaceYCount));
				}
				if (Candidates.Num() > 0 && Islands.IsValidIndex(Candidates[0].IslandIndex))
				{
					const FIsland& Island = Islands[Candidates[0].IslandIndex];
					AddCandidateYWithLanes(Island.Y + Island.LocalBounds.Top - Options.NodePadding);
					AddCandidateYWithLanes(Island.Y + Island.LocalBounds.Bottom + Options.NodePadding);
				}

				TArray<FRect> ExistingBusReservationRects;
				const FString& CurrentSourceParamId = Candidates[0].SourceParamId;
				for (const FReroutePlan& ExistingPlan : InOutPlans)
				{
					if (!Edges.IsValidIndex(ExistingPlan.EdgeIndex))
					{
						continue;
					}
					const FEdge& ExistingEdge = Edges[ExistingPlan.EdgeIndex];
					if (MakeSourceParamId(ExistingEdge) == CurrentSourceParamId)
					{
						continue;
					}
					const int32* ExistingSourceIslandPtr = IslandByNode.Find(ExistingEdge.Source);
					const int32* ExistingTargetIslandPtr = IslandByNode.Find(ExistingEdge.Target);
					if (!ExistingSourceIslandPtr || !ExistingTargetIslandPtr
						|| !Islands.IsValidIndex(*ExistingSourceIslandPtr)
						|| !Islands.IsValidIndex(*ExistingTargetIslandPtr)
						|| !IsValidNodeIndex(Nodes, ExistingEdge.Source)
						|| !IsValidNodeIndex(Nodes, ExistingEdge.Target))
					{
						continue;
					}

					const FVector2D ExistingStart =
						FVector2D(Islands[*ExistingSourceIslandPtr].X, Islands[*ExistingSourceIslandPtr].Y)
						+ GetSourcePort(Nodes[ExistingEdge.Source], ExistingEdge, true);
					const FVector2D ExistingEnd =
						FVector2D(Islands[*ExistingTargetIslandPtr].X, Islands[*ExistingTargetIslandPtr].Y)
						+ GetTargetPort(Nodes[ExistingEdge.Target], ExistingEdge, true);
					TArray<FVector2D> ExistingRoute;
					ExistingRoute.Reserve(ExistingPlan.Points.Num() + 2);
					ExistingRoute.Add(ExistingStart);
					for (const FVector2D& Point : ExistingPlan.Points)
					{
						ExistingRoute.Add(Point);
					}
					ExistingRoute.Add(ExistingEnd);
					AddVisibleRouteLaneReservationRects(ExistingRoute, GetBusAvoidanceDistance(), Options, ExistingBusReservationRects);
				}
				for (const FRect& Rect : ExistingBusReservationRects)
				{
					if (RouteSpanRight < Rect.Left - 0.5 || Rect.Right < RouteSpanLeft - 0.5)
					{
						continue;
					}
					AddCandidateYWithLanes(Rect.Top - GetBusAvoidanceDistance());
					AddCandidateYWithLanes(Rect.Bottom + GetBusAvoidanceDistance());
				}

				bool bFoundBest = false;
				double BestY = 0.0;
				int32 BestHitCount = TNumericLimits<int32>::Max();
				int32 BestReservationHitCount = TNumericLimits<int32>::Max();
				double BestCost = TNumericLimits<double>::Max();
				const double IdealY = InterfaceYCount > 0 ? InterfaceYSum / static_cast<double>(InterfaceYCount) : 0.0;
				for (const double CandidateY : CandidateYs)
				{
					int32 TotalHitCount = 0;
					int32 TotalReservationHitCount = 0;
					double TotalLength = 0.0;
					bool bCandidateValid = true;
					for (const FSharedIslandInternalBusCandidate& Candidate : Candidates)
					{
						if (!Edges.IsValidIndex(Candidate.EdgeIndex))
						{
							bCandidateValid = false;
							break;
						}
						const FEdge& Edge = Edges[Candidate.EdgeIndex];
						const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edge);
						const TArray<FVector2D> CandidateRoute = BuildIslandInternalSharedBusRoute(Candidate.Start, Candidate.End, CandidateY, Options);
						if (CandidateRoute.Num() < 4 || !HasNonNegativeConnectionAlignmentShortenForEverySegment(CandidateRoute, Options))
						{
							bCandidateValid = false;
							break;
						}
						TotalHitCount += CountControlWireHardOccupancyHits(CandidateRoute, RouteHardRects);
						TotalReservationHitCount += CountPolylineHardOccupancyHits(CandidateRoute, ExistingBusReservationRects);
						TotalLength += PolylineManhattanLength(CandidateRoute);
					}
					if (!bCandidateValid)
					{
						continue;
					}
					const double Cost = static_cast<double>(TotalHitCount) * 1000000.0
						+ static_cast<double>(TotalReservationHitCount) * 100000.0
						+ TotalLength
						+ FMath::Abs(CandidateY - IdealY) * 0.05;
					if (!bFoundBest
						|| TotalHitCount < BestHitCount
						|| (TotalHitCount == BestHitCount && TotalReservationHitCount < BestReservationHitCount)
						|| (TotalHitCount == BestHitCount && TotalReservationHitCount == BestReservationHitCount && Cost + 0.001 < BestCost))
					{
						bFoundBest = true;
						BestY = CandidateY;
						BestHitCount = TotalHitCount;
						BestReservationHitCount = TotalReservationHitCount;
						BestCost = Cost;
					}
				}
				if (bFoundBest)
				{
					SharedTrunkYByBusKey.Add(Pair.Key, BestY);
					SharedMergeGroupByBusKey.Add(Pair.Key, NextIslandInternalMergeGroup--);
				}
			}

			for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
			{
				if (PlannedEdgeIndices.Contains(EdgeIndex)
					|| InputChainEdges.Contains(EdgeIndex)
					|| (SkipEdgeIndices && SkipEdgeIndices->Contains(EdgeIndex)))
				{
					continue;
				}
				const FEdge& Edge = Edges[EdgeIndex];
				if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
				{
					continue;
				}
				const int32* SourceIslandPtr = IslandByNode.Find(Edge.Source);
				const int32* TargetIslandPtr = IslandByNode.Find(Edge.Target);
				if (!SourceIslandPtr || !TargetIslandPtr || *SourceIslandPtr != *TargetIslandPtr || !Islands.IsValidIndex(*SourceIslandPtr))
				{
					continue;
				}

				const FIsland& Island = Islands[*SourceIslandPtr];
				if (Island.bCommentIsland)
				{
					continue;
				}

				const int32 SourceColumn = FindIslandColumnForNode(Island, Edge.Source);
				const int32 TargetColumn = FindIslandColumnForNode(Island, Edge.Target);
				if (SourceColumn == INDEX_NONE || TargetColumn == INDEX_NONE)
				{
					continue;
				}

				const FVector2D IslandOffset(Island.X, Island.Y);
				const FVector2D Start = IslandOffset + GetSourcePort(Nodes[Edge.Source], Edge, true);
				const FVector2D End = IslandOffset + GetTargetPort(Nodes[Edge.Target], Edge, true);
				const FString CurrentSourceParamId = MakeSourceParamId(Edge);
				const FString SharedBusKey = MakeIslandInternalBusKey(*SourceIslandPtr, CurrentSourceParamId);
				TArray<FRect> ExistingBusReservationRects;
				for (const FReroutePlan& ExistingPlan : InOutPlans)
				{
					if (!Edges.IsValidIndex(ExistingPlan.EdgeIndex) || ExistingPlan.EdgeIndex == EdgeIndex)
					{
						continue;
					}
					const FEdge& ExistingEdge = Edges[ExistingPlan.EdgeIndex];
					if (MakeSourceParamId(ExistingEdge) == CurrentSourceParamId)
					{
						continue;
					}
					const int32* ExistingSourceIslandPtr = IslandByNode.Find(ExistingEdge.Source);
					const int32* ExistingTargetIslandPtr = IslandByNode.Find(ExistingEdge.Target);
					if (!ExistingSourceIslandPtr || !ExistingTargetIslandPtr
						|| !Islands.IsValidIndex(*ExistingSourceIslandPtr)
						|| !Islands.IsValidIndex(*ExistingTargetIslandPtr)
						|| !IsValidNodeIndex(Nodes, ExistingEdge.Source)
						|| !IsValidNodeIndex(Nodes, ExistingEdge.Target))
					{
						continue;
					}

					const FVector2D ExistingStart =
						FVector2D(Islands[*ExistingSourceIslandPtr].X, Islands[*ExistingSourceIslandPtr].Y)
						+ GetSourcePort(Nodes[ExistingEdge.Source], ExistingEdge, true);
					const FVector2D ExistingEnd =
						FVector2D(Islands[*ExistingTargetIslandPtr].X, Islands[*ExistingTargetIslandPtr].Y)
						+ GetTargetPort(Nodes[ExistingEdge.Target], ExistingEdge, true);
					TArray<FVector2D> ExistingRoute;
					ExistingRoute.Reserve(ExistingPlan.Points.Num() + 2);
					ExistingRoute.Add(ExistingStart);
					for (const FVector2D& Point : ExistingPlan.Points)
					{
						ExistingRoute.Add(Point);
					}
					ExistingRoute.Add(ExistingEnd);

					const double LaneSpacing = GetBusAvoidanceDistance();
					const double HalfLane = FMath::Max(8.0, LaneSpacing * 0.5);
					const double AlongPad = FMath::Max(8.0, EndpointAccessClearanceX(Options) * 0.25);
					for (int32 SegmentIndex = 0; SegmentIndex + 1 < ExistingRoute.Num(); ++SegmentIndex)
					{
						const FVector2D A = ExistingRoute[SegmentIndex];
						const FVector2D B = ExistingRoute[SegmentIndex + 1];
						const double DX = FMath::Abs(B.X - A.X);
						const double DY = FMath::Abs(B.Y - A.Y);
						if (DX <= 0.5 && DY <= 0.5)
						{
							continue;
						}
						if (DY <= 0.5 && DX >= HalfLane)
						{
							ExistingBusReservationRects.Add({
								FMath::Min(A.X, B.X) - AlongPad,
								A.Y - HalfLane,
								FMath::Max(A.X, B.X) + AlongPad,
								A.Y + HalfLane
							});
						}
						else if (DY >= HalfLane && DX <= EndpointAccessClearanceX(Options) * 2.0)
						{
							ExistingBusReservationRects.Add({
								FMath::Min(A.X, B.X) - HalfLane,
								FMath::Min(A.Y, B.Y) - AlongPad,
								FMath::Max(A.X, B.X) + HalfLane,
								FMath::Max(A.Y, B.Y) + AlongPad
							});
						}
					}
				}
				const int32 ColumnDistance = FMath::Abs(TargetColumn - SourceColumn);
				const bool bCrossesMultipleColumns = ColumnDistance > 1;
				TArray<FVector2D> DirectControlPoints;
				DirectControlPoints.Add(Start);
				DirectControlPoints.Add(End);
				const TArray<int32> LocalCollisionColumns = FindLocalColumnCollisionRouteColumns(
					Nodes,
					Island,
					SourceColumn,
					TargetColumn,
					Edge.Source,
					Edge.Target,
					DirectControlPoints,
					Options);
				const bool bLocalColumnLineHitsSibling = LocalCollisionColumns.Num() > 0;
				double SourceColumnRightBoundaryX = 0.0;
				if (bLocalColumnLineHitsSibling)
				{
					SourceColumnRightBoundaryX =
						ComputeIslandColumnRightBoundaryX(Nodes, Island, LocalCollisionColumns[0], Options);
				}

				if (!bCrossesMultipleColumns && !bLocalColumnLineHitsSibling)
				{
					continue;
				}

				TArray<FVector2D> Route;
				FString Reason;
				const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edge);
				if (bLocalColumnLineHitsSibling && !bCrossesMultipleColumns)
				{
					Route = BuildSourceColumnRightBoundaryRoute(Start, End, SourceColumnRightBoundaryX, Options);
					Reason = TEXT("island_internal_same_column_right_boundary_bus");
				}
				else
				{
					TArray<double> CandidateYs;
					auto AddCandidateY = [&CandidateYs](const double Value)
					{
						if (!FMath::IsFinite(Value))
						{
							return;
						}
						const double SnappedValue = SnapGraphCoordinate(Value);
						for (const double Existing : CandidateYs)
						{
							if (FMath::IsNearlyEqual(Existing, SnappedValue, 0.5))
							{
								return;
							}
						}
						CandidateYs.Add(SnappedValue);
					};
					auto AddCandidateYWithLanes = [&AddCandidateY](const double Value)
					{
						AddCandidateY(Value);
						const double LaneSpacing = GetBusAvoidanceDistance();
						constexpr int32 MaxLocalLaneSearch = 8;
						for (int32 Offset = 1; Offset <= MaxLocalLaneSearch; ++Offset)
						{
							AddCandidateY(Value - static_cast<double>(Offset) * LaneSpacing);
							AddCandidateY(Value + static_cast<double>(Offset) * LaneSpacing);
						}
					};
					AddCandidateYWithLanes(Start.Y);
					AddCandidateYWithLanes(End.Y);
					AddCandidateYWithLanes((Start.Y + End.Y) * 0.5);
					AddCandidateYWithLanes(Island.Y + Island.LocalBounds.Top - Options.NodePadding);
					AddCandidateYWithLanes(Island.Y + Island.LocalBounds.Bottom + Options.NodePadding);
					const double RouteSpanLeft = FMath::Min(Start.X, End.X);
					const double RouteSpanRight = FMath::Max(Start.X, End.X);
					for (const FRect& Rect : RouteHardRects)
					{
						if (RouteSpanRight < Rect.Left - 0.5 || Rect.Right < RouteSpanLeft - 0.5)
						{
							continue;
						}
						AddCandidateYWithLanes(Rect.Top - GetBusAvoidanceDistance());
						AddCandidateYWithLanes(Rect.Bottom + GetBusAvoidanceDistance());
					}
					for (const FRect& Rect : ExistingBusReservationRects)
					{
						if (RouteSpanRight < Rect.Left - 0.5 || Rect.Right < RouteSpanLeft - 0.5)
						{
							continue;
						}
						AddCandidateYWithLanes(Rect.Top - GetBusAvoidanceDistance());
						AddCandidateYWithLanes(Rect.Bottom + GetBusAvoidanceDistance());
					}

					int32 BestHitCount = TNumericLimits<int32>::Max();
					int32 BestReservationHitCount = TNumericLimits<int32>::Max();
					double BestCost = TNumericLimits<double>::Max();
					if (const double* SharedTrunkY = SharedTrunkYByBusKey.Find(SharedBusKey))
					{
						CandidateYs.Reset();
						AddCandidateY(*SharedTrunkY);
					}
					for (const double CandidateY : CandidateYs)
					{
						TArray<FVector2D> CandidateRoute = BuildIslandInternalSharedBusRoute(Start, End, CandidateY, Options);
						if (CandidateRoute.Num() < 4 || !HasNonNegativeConnectionAlignmentShortenForEverySegment(CandidateRoute, Options))
						{
							continue;
						}
						const int32 HitCount = CountControlWireHardOccupancyHits(CandidateRoute, RouteHardRects);
						const int32 ReservationHitCount = CountPolylineHardOccupancyHits(CandidateRoute, ExistingBusReservationRects);
						const double Cost = static_cast<double>(HitCount) * 1000000.0
							+ static_cast<double>(ReservationHitCount) * 100000.0
							+ PolylineManhattanLength(CandidateRoute)
							+ FMath::Abs(CandidateY - ((Start.Y + End.Y) * 0.5)) * 0.05;
						if (HitCount < BestHitCount
							|| (HitCount == BestHitCount && ReservationHitCount < BestReservationHitCount)
							|| (HitCount == BestHitCount && ReservationHitCount == BestReservationHitCount && Cost < BestCost))
						{
							BestHitCount = HitCount;
							BestReservationHitCount = ReservationHitCount;
							BestCost = Cost;
							Route = MoveTemp(CandidateRoute);
						}
					}
					if (bLocalColumnLineHitsSibling)
					{
						InsertSourceColumnRightBoundaryPoint(Route, Start, SourceColumnRightBoundaryX, Options);
						Reason = TEXT("island_internal_multi_column_source_column_right_boundary_bus");
					}
					else
					{
						Reason = TEXT("island_internal_multi_column_bus");
					}
				}

				if (Route.Num() < 3)
				{
					continue;
				}

				FReroutePlan Plan;
				Plan.EdgeIndex = EdgeIndex;
				Plan.RouteKind = ERouteKind::LocalBus;
				Plan.BusId = FString::Printf(TEXT("island_internal_%d_%s"), Island.Id, *MakeSourceParamId(Edge));
				if (const int32* SharedMergeGroup = SharedMergeGroupByBusKey.Find(SharedBusKey))
				{
					Plan.AttachmentMergeGroup = *SharedMergeGroup;
				}
				Plan.Reason = Reason;
				for (int32 PointIndex = 1; PointIndex + 1 < Route.Num(); ++PointIndex)
				{
					Plan.Points.Add(Route[PointIndex]);
				}
				CompactRoutePoints(Plan, Start, End, Options);
				if (Plan.Points.Num() == 0)
				{
					continue;
				}

				InOutPlans.Add(Plan);
				PlannedEdgeIndices.Add(EdgeIndex);
				AddRouteDiagnostics(Result, EdgeIndex, TEXT("island_internal"), ERouteKind::LocalBus, Route, false, false);
				++Result.DesignDiagnostics.RouteKindUpgradeCount;
				if (bLocalColumnLineHitsSibling)
				{
					++Result.DesignDiagnostics.IslandInternalSameColumnCollisionBusCount;
				}
				if (bCrossesMultipleColumns)
				{
					++Result.DesignDiagnostics.IslandInternalMultiColumnBusCount;
				}
			}
		}
		static void AddVisibleRouteLaneReservationRects(
			const TArray<FVector2D>& Route,
			const double LaneSpacing,
			const FOptions& Options,
			TArray<FRect>& OutRects)
		{
			const double HalfLane = FMath::Max(8.0, LaneSpacing * 0.5);
			const double AlongPad = FMath::Max(8.0, EndpointAccessClearanceX(Options) * 0.25);
			for (int32 SegmentIndex = 0; SegmentIndex + 1 < Route.Num(); ++SegmentIndex)
			{
				const FVector2D A = Route[SegmentIndex];
				const FVector2D B = Route[SegmentIndex + 1];
				const double DX = FMath::Abs(B.X - A.X);
				const double DY = FMath::Abs(B.Y - A.Y);
				if (DX <= 0.5 && DY <= 0.5)
				{
					continue;
				}
				if (DY <= 0.5 && DX >= HalfLane)
				{
					OutRects.Add({
						FMath::Min(A.X, B.X) - AlongPad,
						A.Y - HalfLane,
						FMath::Max(A.X, B.X) + AlongPad,
						A.Y + HalfLane
					});
				}
				else if (DY >= HalfLane && DX <= EndpointAccessClearanceX(Options) * 2.0)
				{
					OutRects.Add({
						FMath::Min(A.X, B.X) - HalfLane,
						FMath::Min(A.Y, B.Y) - AlongPad,
						FMath::Max(A.X, B.X) + HalfLane,
						FMath::Max(A.Y, B.Y) + AlongPad
					});
				}
			}
		}










		static void AddRouteDiagnostics(
			FRunResult& Result,
			const int32 EdgeIndex,
			const FString& Side,
			const ERouteKind RouteKind,
			const TArray<FVector2D>& Points,
			const bool bExternalUpgrade,
			const bool bCrossesHardOccupancy)
		{
			TSharedPtr<FJsonObject> RouteObj = MakeShared<FJsonObject>();
			RouteObj->SetNumberField(TEXT("edge_index"), EdgeIndex);
			RouteObj->SetStringField(TEXT("side"), Side);
			RouteObj->SetStringField(TEXT("route_kind"), RouteKindToString(RouteKind));
			RouteObj->SetBoolField(TEXT("external_upgrade"), bExternalUpgrade);
			RouteObj->SetBoolField(TEXT("crosses_island"), bCrossesHardOccupancy);
			RouteObj->SetBoolField(TEXT("crosses_hard_occupancy"), bCrossesHardOccupancy);
			RouteObj->SetNumberField(TEXT("length"), PolylineManhattanLength(Points));
			RouteObj->SetArrayField(TEXT("points"), PointArrayToJson(Points));
			Result.RouteDiagnostics.Add(MakeShared<FJsonValueObject>(RouteObj));
			Result.DesignDiagnostics.TotalRouteLength += PolylineManhattanLength(Points);
			if (bCrossesHardOccupancy)
			{
				++Result.DesignDiagnostics.RouteThroughHardOccupancyCount;
			}
		}
		static bool IntervalOverlaps(const double AMin, const double AMax, const double BMin, const double BMax, const double Tolerance = 0.5)
		{
			return AMin <= BMax + Tolerance && BMin <= AMax + Tolerance;
		}
		static TArray<FVector2D> CompactVisiblePolyline(const TArray<FVector2D>& InPoints)
		{
			TArray<FVector2D> OutPoints;
			for (const FVector2D& Point : InPoints)
			{
				if (OutPoints.Num() > 0 && FVector2D::Distance(OutPoints.Last(), Point) < 0.5)
				{
					continue;
				}
				if (OutPoints.Num() >= 2)
				{
					const FVector2D A = OutPoints[OutPoints.Num() - 2];
					const FVector2D B = OutPoints.Last();
					const bool bSameX = FMath::IsNearlyEqual(A.X, B.X, 0.5) && FMath::IsNearlyEqual(B.X, Point.X, 0.5);
					const bool bSameY = FMath::IsNearlyEqual(A.Y, B.Y, 0.5) && FMath::IsNearlyEqual(B.Y, Point.Y, 0.5);
					if (bSameX || bSameY)
					{
						OutPoints.Last() = Point;
						continue;
					}
				}
				OutPoints.Add(Point);
			}
			return OutPoints;
		}

		static TArray<FVector2D> BuildSharedSourceBusRoutePolyline(
			const FVector2D& Start,
			const FVector2D& End,
			const double TrunkY,
			const double BranchX,
			const FOptions& Options)
		{
			const double EntryX = ComputeInputXAlignedToOutput(Start.X, Options);
			const double ExitX = ComputeOutputXAlignedToInput(End.X, Options);
			if (EntryX > ExitX + 0.5)
			{
				return BuildDirectSplinePolyline(Start, End);
			}

			const double MinSegmentX = EndpointAccessClearanceX(Options);
			const bool bNeedsSourceDogleg = !FMath::IsNearlyEqual(Start.Y, TrunkY, 0.5);
			const double MinBranchX = EntryX;
			const double MaxBranchX = ExitX;
			if (MaxBranchX < MinBranchX - 0.5)
			{
				return BuildDirectSplinePolyline(Start, End);
			}
			const double ClampedBranchX = SnapGraphCoordinate(FMath::Clamp(BranchX, MinBranchX, MaxBranchX));
			TArray<FVector2D> Points;
			double CurrentX = EntryX;
			if (!AppendBusRouteSourceEntry(Points, Start, TrunkY, EntryX, ClampedBranchX, Options, CurrentX))
			{
				return BuildDirectSplinePolyline(Start, End);
			}
			AppendBusRouteTargetEndpoint(Points, End, Options);
			return CompactVisiblePolyline(Points);
		}

		static bool AppendBusRouteSourceEntry(
			TArray<FVector2D>& Points,
			const FVector2D& Start,
			const double TrunkY,
			const double SourceEscapeX,
			const double BranchX,
			const FOptions& Options,
			double& OutCurrentX)
		{
			const double EntryX = ComputeInputXAlignedToOutput(Start.X, Options);
			const bool bNeedsSourceDogleg = !FMath::IsNearlyEqual(Start.Y, TrunkY, 0.5);
			if (BranchX < EntryX - 0.5)
			{
				return false;
			}

			const double MaxEscapeX = FMath::Max(EntryX, BranchX);
			const double EscapeX = SnapGraphCoordinate(FMath::Clamp(SourceEscapeX, EntryX, MaxEscapeX));
			auto AddPoint = [&Points](const FVector2D& Point)
			{
				if (Points.Num() == 0 || FVector2D::Distance(Points.Last(), Point) >= 0.5)
				{
					Points.Add(Point);
				}
			};

			AddPoint(Start);
			if (bNeedsSourceDogleg)
			{
				if (EscapeX > EntryX + 0.5)
				{
					AddPoint(FVector2D(EscapeX, Start.Y));
					const double TrunkEntryX = SnapGraphCoordinate(FMath::Max(EscapeX + EndpointAccessClearanceX(Options), EntryX));
					if (TrunkEntryX > BranchX + 0.5)
					{
						return false;
					}
					AddPoint(FVector2D(TrunkEntryX, TrunkY));
					OutCurrentX = TrunkEntryX;
				}
				else
				{
					AddPoint(FVector2D(EscapeX, TrunkY));
					OutCurrentX = EscapeX;
				}
			}
			else if (EscapeX > EntryX + 0.5)
			{
				AddPoint(FVector2D(EscapeX, Start.Y));
				OutCurrentX = EscapeX;
			}
			if (BranchX - OutCurrentX >= 0.5)
			{
				AddPoint(FVector2D(BranchX, TrunkY));
			}
			return true;
		}

		static void AppendBusRouteTargetEndpoint(
			TArray<FVector2D>& Points,
			const FVector2D& End,
			const FOptions& Options)
		{
			if (Points.Num() == 0)
			{
				Points.Add(End);
				return;
			}

			const double ExitX = ComputeOutputXAlignedToInput(End.X, Options);
			const double MinSegmentX = EndpointAccessClearanceX(Options);
			const FVector2D BranchStart = Points.Last();
			if (BranchStart.X > ExitX + 0.5)
			{
				Points.Add(End);
				return;
			}

			const bool bNeedsTargetDogleg = ShouldUseTargetDoglegForBusBranch(BranchStart.Y, End.Y, Options);
			if (!bNeedsTargetDogleg)
			{
				Points.Add(End);
				return;
			}
			if (bNeedsTargetDogleg)
			{
				const double BendX = BranchStart.X + MinSegmentX;
				const double TargetRowX = BendX + MinSegmentX;
				if (TargetRowX <= ExitX + 0.5)
				{
					Points.Add(FVector2D(BendX, BranchStart.Y));
					Points.Add(FVector2D(TargetRowX, End.Y));
				}
				else if (ExitX - BranchStart.X >= MinSegmentX - 0.5)
				{
					Points.Add(FVector2D(ExitX, End.Y));
				}
			}

			if (Points.Num() > 0)
			{
				const FVector2D Last = Points.Last();
				if (ExitX - Last.X >= MinSegmentX - 0.5
					&& (!FMath::IsNearlyEqual(Last.X, ExitX, 0.5)
						|| !FMath::IsNearlyEqual(Last.Y, End.Y, 0.5)))
				{
					Points.Add(FVector2D(ExitX, End.Y));
				}
			}
			Points.Add(End);
		}

		static void BuildSourceOrderAndFanoutDiagnostics(
			const TArray<FEdge>& Edges,
			TArray<FSelectedEndpointCandidate>& SelectedEndpoints,
			FRunResult& Result)
		{
			TMultiMap<FString, int32> SelectedBySourceParam;
			for (int32 SelectedIndex = 0; SelectedIndex < SelectedEndpoints.Num(); ++SelectedIndex)
			{
				if (Edges.IsValidIndex(SelectedEndpoints[SelectedIndex].EdgeIndex))
				{
					SelectedBySourceParam.Add(MakeSourceParamId(Edges[SelectedEndpoints[SelectedIndex].EdgeIndex]), SelectedIndex);
				}
			}

			int32 GroupId = 0;
			for (const TPair<FString, int32>& Pair : SelectedBySourceParam)
			{
				TArray<int32> GroupSelected;
				SelectedBySourceParam.MultiFind(Pair.Key, GroupSelected);
				GroupSelected.Sort();
				if (GroupSelected.Num() == 0 || GroupSelected[0] != Pair.Value)
				{
					continue;
				}
				for (const int32 SelectedIndex : GroupSelected)
				{
					if (SelectedEndpoints.IsValidIndex(SelectedIndex))
					{
						SelectedEndpoints[SelectedIndex].ConnectionGroupId = GroupId;
					}
				}
				TSharedPtr<FJsonObject> SourceObj = MakeShared<FJsonObject>();
				SourceObj->SetNumberField(TEXT("source_order_group_id"), GroupId);
				SourceObj->SetStringField(TEXT("source_param_id"), Pair.Key);
				SourceObj->SetNumberField(TEXT("selected_count"), GroupSelected.Num());
				Result.SourceOrderGroupDiagnostics.Add(MakeShared<FJsonValueObject>(SourceObj));
				++Result.DesignDiagnostics.SourceOrderGroupCount;
				if (GroupSelected.Num() > 1)
				{
					TSharedPtr<FJsonObject> FanoutObj = MakeShared<FJsonObject>();
					FanoutObj->SetNumberField(TEXT("fanout_intent_group_id"), GroupId);
					FanoutObj->SetStringField(TEXT("source_param_id"), Pair.Key);
					FanoutObj->SetNumberField(TEXT("selected_count"), GroupSelected.Num());
					Result.FanoutIntentGroupDiagnostics.Add(MakeShared<FJsonValueObject>(FanoutObj));
					++Result.DesignDiagnostics.FanoutIntentGroupCount;
				}
				++GroupId;
			}
		}

		struct FDeterministicIslandConnection
		{
			FSelectedEndpointCandidate Endpoint;
			bool bRouteOnly = false;
			bool bDirect = false;
			bool bBus = false;
			FString BusReason;
			FVector2D SourcePoint = FVector2D::ZeroVector;
			FVector2D EntryPoint = FVector2D::ZeroVector;
			FVector2D ActualTargetPoint = FVector2D::ZeroVector;
			double DirectSpanX = 0.0;
			TArray<FVector2D> DirectReroutePoints;
			int32 BusId = INDEX_NONE;
			int32 BranchSlotIndex = 0;
		};

		struct FDeterministicInterIslandBus
		{
			int32 Id = INDEX_NONE;
			FString Key;
			TArray<int32> ConnectionIndices;
			FVector2D SourcePoint = FVector2D::ZeroVector;
			double PreferredY = 0.0;
			double XMin = 0.0;
			double XMax = 0.0;
			int32 BundleId = INDEX_NONE;
			int32 SlotIndex = INDEX_NONE;
			int32 SignedSlotIndex = 0;
			int32 UpperAttachmentCount = 0;
			int32 LowerAttachmentCount = 0;
			int32 SlotBias = 0;
			double FinalY = 0.0;
		};

		struct FDeterministicInterIslandBusBundle
		{
			int32 Id = INDEX_NONE;
			TArray<int32> BusIds;
			double PreferredY = 0.0;
			double XMin = 0.0;
			double XMax = 0.0;
			int32 MaxSlotIndex = 0;
			double FinalCenterY = 0.0;
			TArray<FXStripCollisionBox> CollisionBoxes;
		};

		struct FBundleXPartition
		{
			double X0 = 0.0;
			double X1 = 0.0;
			double MidX = 0.0;
			TArray<int32> ActiveBundleIds;
		};

		struct FPartitionBandConstraint
		{
			int32 PartitionIndex = INDEX_NONE;
			int32 BandIndex = INDEX_NONE;
			int32 GroupOrderInBand = INDEX_NONE;
			int32 UpperBundleId = INDEX_NONE;
			int32 LowerBundleId = INDEX_NONE;
			double GroupTop = 0.0;
			double GroupBottom = 0.0;
			double PartitionX0 = 0.0;
			double PartitionX1 = 0.0;
			TArray<int32> IslandIds;
		};

		static bool ShouldBusBranchUseActualTargetAsVisibleTarget(const FDeterministicIslandConnection& Connection)
		{
			return Connection.Endpoint.Side == TEXT("left")
				&& IsSameRowForwardSegment(Connection.EntryPoint, Connection.ActualTargetPoint);
		}

		static FVector2D ResolveBusBranchVisibleTargetPoint(const FDeterministicIslandConnection& Connection)
		{
			return ShouldBusBranchUseActualTargetAsVisibleTarget(Connection)
				? Connection.ActualTargetPoint
				: Connection.EntryPoint;
		}

		static bool ShouldMergeBusVisibleTargetWithActualTarget(
			const FDeterministicIslandConnection& Connection,
			const FVector2D& VisibleTargetPoint,
			const FVector2D& ActualTargetPoint)
		{
			if (FVector2D::Distance(VisibleTargetPoint, ActualTargetPoint) <= SameRowEntryTargetMergeTolerance())
			{
				return true;
			}
			return Connection.Endpoint.bSingleNodeBoundaryBus
				&& FMath::Abs(VisibleTargetPoint.X - ActualTargetPoint.X) <= SameRowEntryTargetMergeTolerance();
		}

		static FString ResolveBusBranchTargetMode(const FDeterministicIslandConnection& Connection)
		{
			return ShouldBusBranchUseActualTargetAsVisibleTarget(Connection)
				? TEXT("actual_target")
				: TEXT("island_input_entry");
		}

		static double RequiredBusEntrySourceSpanX(
			const FDeterministicIslandConnection& Connection,
			const TArray<FDeterministicInterIslandBus>& Buses,
			const TArray<FIsland>& Islands,
			const FOptions& Options)
		{
			const double SegmentClearance = EndpointAccessClearanceX(Options);
			double RequiredSpan = BusPathAccessClearanceX(Options)
				+ FMath::Max(0, Connection.BranchSlotIndex) * GetCrossBusBranchLaneSpacing(Options);
			if (!Buses.IsValidIndex(Connection.BusId)
				|| !Islands.IsValidIndex(Connection.Endpoint.SourceIsland)
				|| !Islands.IsValidIndex(Connection.Endpoint.TargetIsland))
			{
				return RequiredSpan;
			}

			const FDeterministicInterIslandBus& Bus = Buses[Connection.BusId];
			const FIsland& SourceIsland = Islands[Connection.Endpoint.SourceIsland];
			const FIsland& TargetIsland = Islands[Connection.Endpoint.TargetIsland];
			const double SourceY = SourceIsland.Y + Connection.Endpoint.SourceLocal.Y;
			const double ActualTargetY = TargetIsland.Y + Connection.Endpoint.ActualTargetLocal.Y;
			if (ShouldUseTargetDoglegForBusBranch(SourceY, Bus.FinalY, Options))
			{
				RequiredSpan += SegmentClearance;
			}
			if (ShouldUseTargetDoglegForBusBranch(Bus.FinalY, ActualTargetY, Options))
			{
				RequiredSpan += SegmentClearance;
			}
			return RequiredSpan;
		}

		static double RequiredBusTargetLeftForForwardBranch(
			const FDeterministicIslandConnection& Connection,
			const double SourceOutputX,
			const FOptions& Options)
		{
			const double BranchSpacing = GetCrossBusBranchLaneSpacing(Options);
			return SourceOutputX
				+ BusPathAccessClearanceX(Options)
				+ FMath::Max(0, Connection.BranchSlotIndex) * BranchSpacing
				- Connection.Endpoint.ActualTargetLocal.X;
		}
		static double GetDirectConnectionMaxSpanX(const FOptions& Options)
		{
			return FMath::Max(Options.HorizontalSpacing * 2.5, EndpointAccessClearanceX(Options) * 6.0);
		}

		static FString MakeInterIslandBusKey(const FEdge& Edge)
		{
			return MakeSourceParamId(Edge);
		}

		static FSelectedEndpointCandidate MakeSelectedEndpointFromCandidate(const FIslandPortCandidate& Candidate, const FOptions& Options)
		{
			FSelectedEndpointCandidate Selected;
			Selected.EdgeIndex = Candidate.EdgeIndex;
			Selected.CandidateId = Candidate.Id;
			Selected.SourceIsland = Candidate.SourceIsland;
			Selected.TargetIsland = Candidate.TargetIsland;
			Selected.BoundaryEntryId = Candidate.BoundaryEntryId;
			Selected.InternalBusGroupId = Candidate.InternalBusGroupId;
			Selected.Side = Candidate.Side;
			Selected.SourceLocal = Candidate.SourceLocal;
			Selected.EntryLocal = Candidate.TargetLocal;
			Selected.ActualTargetLocal = Candidate.ActualTargetLocal;
			Selected.MinTargetDeltaX = MinTargetDeltaXForCandidate(Candidate, Options);
			Selected.SourcePortOrderKey = Candidate.SourceLocal.Y;
			Selected.bSingleNodeBoundaryBus = Candidate.bSingleNodeBoundaryBus;
			return Selected;
		}

		static bool IsBoundaryBusSideCandidate(const FString& Side)
		{
			return Side == TEXT("top") || Side == TEXT("bottom");
		}

		static void ApplyEndpointCandidateToSelected(
			const FIslandPortCandidate& Candidate,
			const FOptions& Options,
			FSelectedEndpointCandidate& InOutSelected)
		{
			InOutSelected.CandidateId = Candidate.Id;
			InOutSelected.SourceIsland = Candidate.SourceIsland;
			InOutSelected.TargetIsland = Candidate.TargetIsland;
			InOutSelected.BoundaryEntryId = Candidate.BoundaryEntryId;
			InOutSelected.InternalBusGroupId = Candidate.InternalBusGroupId;
			InOutSelected.Side = Candidate.Side;
			InOutSelected.SourceLocal = Candidate.SourceLocal;
			InOutSelected.EntryLocal = Candidate.TargetLocal;
			InOutSelected.ActualTargetLocal = Candidate.ActualTargetLocal;
			InOutSelected.MinTargetDeltaX = MinTargetDeltaXForCandidate(Candidate, Options);
			InOutSelected.SourcePortOrderKey = Candidate.SourceLocal.Y;
			InOutSelected.bSingleNodeBoundaryBus = Candidate.bSingleNodeBoundaryBus;
		}

		static bool ChooseInitialIslandCandidate(
			const TArray<FIslandPortCandidate>& Candidates,
			const int32 EdgeIndex,
			const FOptions& Options,
			FSelectedEndpointCandidate& OutSelected)
		{
			const FIslandPortCandidate* Best = nullptr;
			double BestScore = TNumericLimits<double>::Max();
			for (const FIslandPortCandidate& Candidate : Candidates)
			{
				if (Candidate.EdgeIndex != EdgeIndex)
				{
					continue;
				}
				const double SidePenalty = Candidate.Side == TEXT("left") ? 0.0 : 10000.0;
				const double Score = SidePenalty
					+ FMath::Abs(Candidate.TargetLocal.X) * 0.01
					+ static_cast<double>(Candidate.Id) * 0.0001;
				if (!Best || Score < BestScore)
				{
					Best = &Candidate;
					BestScore = Score;
				}
			}
			if (!Best)
			{
				return false;
			}
			OutSelected = MakeSelectedEndpointFromCandidate(*Best, Options);
			return true;
		}

		static int32 CountNonRouteIncomingIslandEdges(
			const TArray<FIsland>& Islands,
			const TArray<FIslandEdge>& IslandEdges,
			const TSet<int32>& RouteOnlyIslandEdgeIndices,
			const int32 IslandIndex)
		{
			if (!Islands.IsValidIndex(IslandIndex))
			{
				return 0;
			}
			int32 Count = 0;
			for (const int32 IslandEdgeIndex : Islands[IslandIndex].IncomingEdges)
			{
				if (IslandEdges.IsValidIndex(IslandEdgeIndex)
					&& !RouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex))
				{
					++Count;
				}
			}
			return Count;
		}

		static void SortReadyIslandsByDesignKey(
			TArray<int32>& Ready,
			const TArray<FIsland>& Islands,
			const TArray<FIslandEdge>& IslandEdges,
			const TSet<int32>& RouteOnlyIslandEdgeIndices,
			const TMap<int32, FRect>* IslandOccupiedBounds = nullptr)
		{
			Ready.Sort([&Islands, &IslandEdges, &RouteOnlyIslandEdgeIndices, IslandOccupiedBounds](const int32 A, const int32 B)
			{
				if (!Islands.IsValidIndex(A) || !Islands.IsValidIndex(B))
				{
					return A < B;
				}
				if (!FMath::IsNearlyEqual(Islands[A].X, Islands[B].X, 0.5))
				{
					return Islands[A].X < Islands[B].X;
				}
				const int32 IncomingA = CountNonRouteIncomingIslandEdges(Islands, IslandEdges, RouteOnlyIslandEdgeIndices, A);
				const int32 IncomingB = CountNonRouteIncomingIslandEdges(Islands, IslandEdges, RouteOnlyIslandEdgeIndices, B);
				if (IncomingA != IncomingB)
				{
					return IncomingA > IncomingB;
				}
				const FRect BoundsA = IslandOccupiedBounds != nullptr && IslandOccupiedBounds->Find(A) != nullptr
					? *IslandOccupiedBounds->Find(A)
					: Islands[A].LocalBounds;
				const FRect BoundsB = IslandOccupiedBounds != nullptr && IslandOccupiedBounds->Find(B) != nullptr
					? *IslandOccupiedBounds->Find(B)
					: Islands[B].LocalBounds;
				if (!FMath::IsNearlyEqual(BoundsA.Height(), BoundsB.Height(), 0.5))
				{
					return BoundsA.Height() > BoundsB.Height();
				}
				if (!FMath::IsNearlyEqual(BoundsA.Width(), BoundsB.Width(), 0.5))
				{
					return BoundsA.Width() > BoundsB.Width();
				}
				return A < B;
			});
		}

		static TArray<int32> BuildIslandTopoOrder(
			const TArray<FIsland>& Islands,
			const TArray<FIslandEdge>& IslandEdges,
			const TSet<int32>& RouteOnlyIslandEdgeIndices,
			FRunResult& Result,
			const bool bRecordDiagnostics = true)
		{
			TArray<TArray<int32>> Outgoing;
			Outgoing.SetNum(Islands.Num());
			TArray<int32> Indegree;
			Indegree.Init(0, Islands.Num());
			for (int32 IslandEdgeIndex = 0; IslandEdgeIndex < IslandEdges.Num(); ++IslandEdgeIndex)
			{
				if (RouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex))
				{
					continue;
				}
				const FIslandEdge& IslandEdge = IslandEdges[IslandEdgeIndex];
				if (!Islands.IsValidIndex(IslandEdge.SourceIsland) || !Islands.IsValidIndex(IslandEdge.TargetIsland))
				{
					continue;
				}
				Outgoing[IslandEdge.SourceIsland].Add(IslandEdgeIndex);
				++Indegree[IslandEdge.TargetIsland];
			}

			TArray<int32> Ready;
			for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
			{
				if (Indegree[IslandIndex] == 0)
				{
					Ready.Add(IslandIndex);
				}
			}
			TArray<int32> Order;
			while (Ready.Num() > 0)
			{
				SortReadyIslandsByDesignKey(Ready, Islands, IslandEdges, RouteOnlyIslandEdgeIndices);
				const int32 IslandIndex = Ready[0];
				Ready.RemoveAt(0, 1, EAllowShrinking::No);
				Order.Add(IslandIndex);
				for (const int32 EdgeIndex : Outgoing[IslandIndex])
				{
					const int32 TargetIsland = IslandEdges[EdgeIndex].TargetIsland;
					if (!Indegree.IsValidIndex(TargetIsland))
					{
						continue;
					}
					--Indegree[TargetIsland];
					if (Indegree[TargetIsland] == 0)
					{
						Ready.Add(TargetIsland);
					}
				}
			}
			if (Order.Num() != Islands.Num())
			{
				for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
				{
					if (!Order.Contains(IslandIndex))
					{
						if (bRecordDiagnostics)
						{
							++Result.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount;
						}
						Order.Add(IslandIndex);
					}
				}
			}
			if (bRecordDiagnostics)
			{
				Result.DesignDiagnostics.IslandTopologicalOrderCount = Order.Num();
			}
			return Order;
		}

		static double ComputeIslandLeftXFromPlacedParents(
			const TArray<FIsland>& Islands,
			const TArray<FIslandEdge>& IslandEdges,
			const TArray<FDeterministicIslandConnection>& Connections,
			const TSet<int32>& RouteOnlyIslandEdgeIndices,
			const TMap<int32, int32>& ConnectionByEdge,
			const int32 IslandIndex,
			const FOptions& Options,
			FRunResult* Result,
			const bool bRecordDiagnostics)
		{
			if (!Islands.IsValidIndex(IslandIndex))
			{
				return Options.OriginX;
			}
			bool bHasIncoming = false;
			double RequiredLeftX = 0.0;
			for (const int32 IslandEdgeIndex : Islands[IslandIndex].IncomingEdges)
			{
				if (!IslandEdges.IsValidIndex(IslandEdgeIndex)
					|| RouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex))
				{
					continue;
				}
				const FIslandEdge& IslandEdge = IslandEdges[IslandEdgeIndex];
				const int32* ConnectionIndexPtr = ConnectionByEdge.Find(IslandEdge.EdgeIndex);
				if (!ConnectionIndexPtr || !Connections.IsValidIndex(*ConnectionIndexPtr) || !Islands.IsValidIndex(IslandEdge.SourceIsland))
				{
					continue;
				}
				const FSelectedEndpointCandidate& Endpoint = Connections[*ConnectionIndexPtr].Endpoint;
				const double ParentInterfaceX = Islands[IslandEdge.SourceIsland].X + Endpoint.SourceLocal.X;
				double ConstraintEntryLocalX = Endpoint.EntryLocal.X;
				if (Endpoint.Side == TEXT("top") || Endpoint.Side == TEXT("bottom"))
				{
					ConstraintEntryLocalX = FMath::Min(
						ConstraintEntryLocalX,
						ComputeOutputXAlignedToInput(Endpoint.ActualTargetLocal.X, Options));
				}
				const double CandidateRequiredLeftX =
					ComputeInputXAlignedToOutput(ParentInterfaceX, Options) - ConstraintEntryLocalX;
				RequiredLeftX = bHasIncoming ? FMath::Max(RequiredLeftX, CandidateRequiredLeftX) : CandidateRequiredLeftX;
				bHasIncoming = true;

				if (bRecordDiagnostics && Result)
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("kind"), TEXT("island_x_parent_constraint"));
					Obj->SetNumberField(TEXT("edge_index"), IslandEdge.EdgeIndex);
					Obj->SetNumberField(TEXT("parent_island_id"), IslandEdge.SourceIsland);
					Obj->SetNumberField(TEXT("target_island_id"), IslandIndex);
					Obj->SetNumberField(TEXT("parent_interface_x"), ParentInterfaceX);
					Obj->SetNumberField(TEXT("target_candidate_local_x"), Endpoint.EntryLocal.X);
					Obj->SetNumberField(TEXT("target_x_constraint_entry_local_x"), ConstraintEntryLocalX);
					Obj->SetNumberField(TEXT("required_target_left_x"), CandidateRequiredLeftX);
					Result->PlacementProposalDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}
			return SnapGraphCoordinate(bHasIncoming ? RequiredLeftX : Options.OriginX);
		}

		static int32 CompareInterfaceOrderTuples(const TArray<double>* A, const TArray<double>* B)
		{
			const int32 NumA = A ? A->Num() : 0;
			const int32 NumB = B ? B->Num() : 0;
			const int32 Num = FMath::Min(NumA, NumB);
			for (int32 Index = 0; Index < Num; ++Index)
			{
				if (!FMath::IsNearlyEqual((*A)[Index], (*B)[Index], 0.5))
				{
					return (*A)[Index] < (*B)[Index] ? -1 : 1;
				}
			}
			if (NumA != NumB)
			{
				return NumA < NumB ? -1 : 1;
			}
			return 0;
		}

		static FString MakeRootTargetKey(const int32 Root, const int32 Target)
		{
			return FString::Printf(TEXT("%d|%d"), Root, Target);
		}

		static double RootInfluenceDepthWeight(const int32 Depth)
		{
			if (Depth <= 1)
			{
				return 4.0;
			}
			if (Depth == 2)
			{
				return 3.0;
			}
			if (Depth == 3)
			{
				return 2.0;
			}
			return 1.0;
		}

		static double GetRootTargetWeight(
			const TMap<FString, double>& WeightByRootTarget,
			const int32 Root,
			const int32 Target)
		{
			if (const double* Weight = WeightByRootTarget.Find(MakeRootTargetKey(Root, Target)))
			{
				return FMath::Max(KINDA_SMALL_NUMBER, *Weight);
			}
			return 1.0;
		}

		static bool RootSetEquals(const TSet<int32>& A, const TSet<int32>& B)
		{
			if (A.Num() != B.Num())
			{
				return false;
			}
			for (const int32 Value : A)
			{
				if (!B.Contains(Value))
				{
					return false;
				}
			}
			return true;
		}

		static void SortUniqueInterfaceYs(TArray<double>& Values)
		{
			Values.Sort();
			for (int32 Index = Values.Num() - 1; Index > 0; --Index)
			{
				if (FMath::IsNearlyEqual(Values[Index], Values[Index - 1], 0.5))
				{
					Values.RemoveAt(Index, 1, EAllowShrinking::No);
				}
			}
		}
