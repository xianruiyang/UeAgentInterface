// Copyright Epic Games, Inc. All Rights Reserved.
// Frozen-island inter-island placement and corridor routing.

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

		static void MarkRouteOnlyCycleEdges(
			const TArray<FIsland>& Islands,
			const TArray<FIslandEdge>& IslandEdges,
			TSet<int32>& InOutRouteOnlyIslandEdgeIndices,
			FRunResult& Result)
		{
			TArray<int32> VisitIndex;
			TArray<int32> LowLink;
			TArray<uint8> InStack;
			VisitIndex.Init(INDEX_NONE, Islands.Num());
			LowLink.Init(0, Islands.Num());
			InStack.Init(0, Islands.Num());
			TArray<int32> Stack;
			int32 NextVisitIndex = 0;

			TArray<TArray<int32>> OutgoingIslandEdgeIndices;
			OutgoingIslandEdgeIndices.SetNum(Islands.Num());
			for (int32 IslandEdgeIndex = 0; IslandEdgeIndex < IslandEdges.Num(); ++IslandEdgeIndex)
			{
				const FIslandEdge& Edge = IslandEdges[IslandEdgeIndex];
				if (InOutRouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex)
					|| !Islands.IsValidIndex(Edge.SourceIsland)
					|| !Islands.IsValidIndex(Edge.TargetIsland))
				{
					continue;
				}
				OutgoingIslandEdgeIndices[Edge.SourceIsland].Add(IslandEdgeIndex);
			}

			TFunction<void(int32)> StrongConnect = [&](const int32 IslandIndex)
			{
				VisitIndex[IslandIndex] = NextVisitIndex;
				LowLink[IslandIndex] = NextVisitIndex;
				++NextVisitIndex;
				Stack.Add(IslandIndex);
				InStack[IslandIndex] = 1;

				for (const int32 IslandEdgeIndex : OutgoingIslandEdgeIndices[IslandIndex])
				{
					const int32 TargetIsland = IslandEdges[IslandEdgeIndex].TargetIsland;
					if (!Islands.IsValidIndex(TargetIsland))
					{
						continue;
					}
					if (VisitIndex[TargetIsland] == INDEX_NONE)
					{
						StrongConnect(TargetIsland);
						LowLink[IslandIndex] = FMath::Min(LowLink[IslandIndex], LowLink[TargetIsland]);
					}
					else if (InStack[TargetIsland])
					{
						LowLink[IslandIndex] = FMath::Min(LowLink[IslandIndex], VisitIndex[TargetIsland]);
					}
				}

				if (LowLink[IslandIndex] != VisitIndex[IslandIndex])
				{
					return;
				}

				TArray<int32> Component;
				while (Stack.Num() > 0)
				{
					const int32 Popped = Stack.Pop(EAllowShrinking::No);
					InStack[Popped] = 0;
					Component.Add(Popped);
					if (Popped == IslandIndex)
					{
						break;
					}
				}
				if (Component.Num() <= 1)
				{
					return;
				}

				TSet<int32> ComponentSet;
				for (const int32 ComponentIsland : Component)
				{
					ComponentSet.Add(ComponentIsland);
				}
				int32 BestEdgeIndex = INDEX_NONE;
				int32 BestSourceRank = TNumericLimits<int32>::Min();
				int32 BestTargetRank = TNumericLimits<int32>::Max();
				for (int32 CandidateEdgeIndex = 0; CandidateEdgeIndex < IslandEdges.Num(); ++CandidateEdgeIndex)
				{
					const FIslandEdge& Edge = IslandEdges[CandidateEdgeIndex];
					if (InOutRouteOnlyIslandEdgeIndices.Contains(CandidateEdgeIndex)
						|| !ComponentSet.Contains(Edge.SourceIsland)
						|| !ComponentSet.Contains(Edge.TargetIsland))
					{
						continue;
					}
					const int32 SourceRank = Islands.IsValidIndex(Edge.SourceIsland) ? Islands[Edge.SourceIsland].Rank : 0;
					const int32 TargetRank = Islands.IsValidIndex(Edge.TargetIsland) ? Islands[Edge.TargetIsland].Rank : 0;
					if (SourceRank > BestSourceRank
						|| (SourceRank == BestSourceRank && TargetRank < BestTargetRank)
						|| (SourceRank == BestSourceRank && TargetRank == BestTargetRank && CandidateEdgeIndex < BestEdgeIndex))
					{
						BestSourceRank = SourceRank;
						BestTargetRank = TargetRank;
						BestEdgeIndex = CandidateEdgeIndex;
					}
				}
				if (BestEdgeIndex != INDEX_NONE)
				{
					InOutRouteOnlyIslandEdgeIndices.Add(BestEdgeIndex);
					++Result.DesignDiagnostics.RouteOnlyCycleBreakEdgeCount;
				}
			};

			for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
			{
				if (VisitIndex[IslandIndex] == INDEX_NONE)
				{
					StrongConnect(IslandIndex);
				}
			}
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

		static TArray<FRect> BuildDirectUpgradeBlockingRects(
			const TArray<FNode>& Nodes,
			const TArray<FIsland>& Islands,
			const FOptions& Options,
			const FSelectedEndpointCandidate& Selected,
			const FEdge& Edge)
		{
			(void)Selected;
			return BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edge);
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

		static double MinTargetDeltaXForCandidate(const FIslandPortCandidate& Candidate, const FOptions& Options)
		{
			const double Padding = FMath::Max(0.0, Options.NodePadding);
			const double DirectEndpointDelta =
				ComputeTargetOriginDeltaXForAlignedConnection(Candidate.SourceLocal, Candidate.ActualTargetLocal, Options)
				+ Padding;
			const double EntryEndpointDelta =
				ComputeTargetOriginDeltaXForAlignedConnection(Candidate.SourceLocal, Candidate.TargetLocal, Options)
				+ Padding;
			return FMath::Max(DirectEndpointDelta, EntryEndpointDelta);
		}

		static FVector2D ResolveEntryLocalAfterPlacement(
			const FIslandPortCandidate& Candidate,
			const FIsland& SourceIsland,
			const FIsland& TargetIsland,
			const FOptions& Options)
		{
			FVector2D EntryLocal = Candidate.TargetLocal;
			if (Candidate.bContinuousSegment && (Candidate.Side == TEXT("top") || Candidate.Side == TEXT("bottom")))
			{
				const double DesiredEntryX =
					SourceIsland.X + Candidate.SourceLocal.X
					+ EndpointAccessClearanceX(Options)
					+ FMath::Max(0.0, Options.NodePadding)
					- TargetIsland.X;
				EntryLocal.X = FMath::Clamp(DesiredEntryX, Candidate.AllowedMinX, Candidate.AllowedMaxX);
			}
			if (Candidate.Side == TEXT("top") || Candidate.Side == TEXT("bottom"))
			{
				const double SourceOutputX = SourceIsland.X + Candidate.SourceLocal.X;
				const double TargetInputX = TargetIsland.X + Candidate.ActualTargetLocal.X;
				const double MinEntryXFromSource = ComputeMinInputXForNonNegativeShorten(SourceOutputX, Options) - TargetIsland.X;
				const double MaxEntryXToTarget = ComputeMaxOutputXForNonNegativeShorten(TargetInputX, Options) - TargetIsland.X;
				const double Lower = FMath::Max(Candidate.AllowedMinX, MinEntryXFromSource);
				const double Upper = FMath::Min(Candidate.AllowedMaxX, MaxEntryXToTarget);
				if (Lower <= Upper)
				{
					EntryLocal.X = FMath::Clamp(EntryLocal.X, Lower, Upper);
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
			const double SideCost = Candidate.Side == TEXT("left") ? 0.0 : (Candidate.Side == TEXT("top") ? 18.0 : 20.0);
			Proposal.Score = SideCost
				+ FMath::Abs(Candidate.SourceLocal.Y - Candidate.ActualTargetLocal.Y) * 0.08
				+ FMath::Max(0.0, Proposal.RequiredTargetDeltaX) * 0.02
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
			FSelectedEndpointCandidate& Selected)
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
				Options);
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
			Obj->SetStringField(TEXT("side"), Selected.Side);
			Obj->SetNumberField(TEXT("connection_group_id"), Selected.ConnectionGroupId);
			Obj->SetNumberField(TEXT("bus_contract_id"), Selected.BusContractId);
			Obj->SetNumberField(TEXT("min_target_delta_x"), Selected.MinTargetDeltaX);
			Obj->SetNumberField(TEXT("primary_residual"), Selected.PrimaryResidual);
			Obj->SetNumberField(TEXT("source_port_order_key"), Selected.SourcePortOrderKey);
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
				Obj->SetObjectField(TEXT("source_abs"), PointToJson(SourceAbs));
				Obj->SetObjectField(TEXT("entry_abs"), PointToJson(EntryAbs));
				Obj->SetObjectField(TEXT("actual_target_abs"), PointToJson(ActualTargetAbs));
				Obj->SetNumberField(TEXT("alignment_shorten_x"), ComputeConnectionAlignmentShortenX(SourceAbs, ActualTargetAbs, Options));
				Obj->SetNumberField(TEXT("source_to_entry_shorten_x"), ComputeConnectionAlignmentShortenX(SourceAbs, EntryAbs, Options));
				Obj->SetNumberField(TEXT("entry_to_target_shorten_x"), ComputeConnectionAlignmentShortenX(EntryAbs, ActualTargetAbs, Options));
				Obj->SetBoolField(TEXT("forward_valid"), IsCandidateForwardValid(SourceIsland, TargetIsland, Selected, Options));
			}
			Result.SelectedEndpointCandidateDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
		}

		static void SolveIslandXByLongestPath(
			TArray<FIsland>& Islands,
			const TArray<FSelectedEndpointCandidate>& SelectedEndpoints,
			const TSet<int32>& RouteOnlyEdgeIndices,
			const FOptions& Options,
			FRunResult& Result)
		{
			for (FIsland& Island : Islands)
			{
				Island.X = Options.OriginX + static_cast<double>(Island.Rank) * Options.HorizontalSpacing;
			}
			for (int32 Iteration = 0; Iteration < Islands.Num() + 1; ++Iteration)
			{
				bool bChanged = false;
				for (const FSelectedEndpointCandidate& Selected : SelectedEndpoints)
				{
					if (RouteOnlyEdgeIndices.Contains(Selected.EdgeIndex)
						|| !Islands.IsValidIndex(Selected.SourceIsland)
						|| !Islands.IsValidIndex(Selected.TargetIsland))
					{
						continue;
					}
					const double RequiredTargetX = Islands[Selected.SourceIsland].X + Selected.MinTargetDeltaX;
					if (RequiredTargetX > Islands[Selected.TargetIsland].X + 0.5)
					{
						Islands[Selected.TargetIsland].X = RequiredTargetX;
						bChanged = true;
					}
				}
				if (!bChanged)
				{
					break;
				}
			}

			for (const FSelectedEndpointCandidate& Selected : SelectedEndpoints)
			{
				if (!Islands.IsValidIndex(Selected.SourceIsland) || !Islands.IsValidIndex(Selected.TargetIsland))
				{
					continue;
				}
				const FIsland& SourceIsland = Islands[Selected.SourceIsland];
				FIsland& TargetIsland = Islands[Selected.TargetIsland];
				const FVector2D SourceAbs(SourceIsland.X + Selected.SourceLocal.X, SourceIsland.Y + Selected.SourceLocal.Y);
				const FVector2D TargetAbs(TargetIsland.X + Selected.ActualTargetLocal.X, TargetIsland.Y + Selected.ActualTargetLocal.Y);
				const double ShortenX = ComputeConnectionAlignmentShortenX(SourceAbs, TargetAbs, Options);
				if (ShortenX < -0.5)
				{
					TargetIsland.X += FMath::Abs(ShortenX);
				}
			}
		}

		static bool RectOverlapsAny(const FRect& Rect, const TArray<FRect>& Others, double Padding)
		{
			for (const FRect& Other : Others)
			{
				if (Rect.Intersects(Other, Padding))
				{
					return true;
				}
			}
			return false;
		}

		static void RelaxIslandXForEndpointHardOccupancy(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			TArray<FIsland>& Islands,
			const TArray<FSelectedEndpointCandidate>& SelectedEndpoints,
			const TSet<int32>& RouteOnlyEdgeIndices,
			const FOptions& Options)
		{
			const double Clearance = EndpointAccessClearanceX(Options) + FMath::Max(0.0, Options.NodePadding);
			for (int32 Iteration = 0; Iteration < Islands.Num() + 1; ++Iteration)
			{
				bool bChanged = false;
				for (const FSelectedEndpointCandidate& Selected : SelectedEndpoints)
				{
					if (RouteOnlyEdgeIndices.Contains(Selected.EdgeIndex)
						|| !Edges.IsValidIndex(Selected.EdgeIndex)
						|| !Islands.IsValidIndex(Selected.SourceIsland)
						|| !Islands.IsValidIndex(Selected.TargetIsland))
					{
						continue;
					}

					const FEdge& Edge = Edges[Selected.EdgeIndex];
					const FIsland& SourceIsland = Islands[Selected.SourceIsland];
					FIsland& TargetIsland = Islands[Selected.TargetIsland];
					const FVector2D SourceAbs(SourceIsland.X + Selected.SourceLocal.X, SourceIsland.Y + Selected.SourceLocal.Y);
					const FVector2D TargetAbs(TargetIsland.X + Selected.ActualTargetLocal.X, TargetIsland.Y + Selected.ActualTargetLocal.Y);
					const TArray<FVector2D> DirectPolyline = BuildDirectSplinePolyline(SourceAbs, TargetAbs);
					const TArray<FRect> BlockingRects = BuildDirectUpgradeBlockingRects(Nodes, Islands, Options, Selected, Edge);
					double RequiredTargetX = TargetIsland.X;
					for (const FRect& Rect : BlockingRects)
					{
						if (!PolylineHitsHardOccupancy(DirectPolyline, TArray<FRect>{ Rect }))
						{
							continue;
						}
						if (Rect.Right <= SourceAbs.X + 0.5)
						{
							continue;
						}
						RequiredTargetX = FMath::Max(
							RequiredTargetX,
							Rect.Right + Clearance - Selected.ActualTargetLocal.X);
					}
					if (RequiredTargetX > TargetIsland.X + 0.5)
					{
						TargetIsland.X = SnapGraphCoordinate(RequiredTargetX);
						bChanged = true;
					}
				}
				if (!bChanged)
				{
					break;
				}
			}
		}

		static void RelaxIslandXForSelectedEndpointForwardValidity(
			TArray<FIsland>& Islands,
			const TArray<FSelectedEndpointCandidate>& SelectedEndpoints,
			const TSet<int32>& RouteOnlyEdgeIndices,
			const FOptions& Options)
		{
			for (int32 Iteration = 0; Iteration < Islands.Num() + 1; ++Iteration)
			{
				bool bChanged = false;
				for (const FSelectedEndpointCandidate& Selected : SelectedEndpoints)
				{
					if (RouteOnlyEdgeIndices.Contains(Selected.EdgeIndex)
						|| !Islands.IsValidIndex(Selected.SourceIsland)
						|| !Islands.IsValidIndex(Selected.TargetIsland))
					{
						continue;
					}
					const FIsland& SourceIsland = Islands[Selected.SourceIsland];
					FIsland& TargetIsland = Islands[Selected.TargetIsland];
					const double SourceToEntryTargetX =
						SourceIsland.X + ComputeTargetOriginDeltaXForAlignedConnection(Selected.SourceLocal, Selected.EntryLocal, Options);
					const double SourceToActualTargetX =
						SourceIsland.X + ComputeTargetOriginDeltaXForAlignedConnection(Selected.SourceLocal, Selected.ActualTargetLocal, Options);
					const double RequiredTargetX = FMath::Max(SourceToEntryTargetX, SourceToActualTargetX);
					if (RequiredTargetX > TargetIsland.X + 0.5)
					{
						TargetIsland.X = SnapGraphCoordinate(RequiredTargetX);
						bChanged = true;
					}
				}
				if (!bChanged)
				{
					break;
				}
			}
		}

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
			const TArray<FSelectedEndpointCandidate>& SelectedEndpoints)
		{
			struct FTargetOrderBucket
			{
				TArray<double> SourcePortYs;
			};

			TMap<int32, TMap<int32, FTargetOrderBucket>> TargetsBySource;
			for (const FSelectedEndpointCandidate& Selected : SelectedEndpoints)
			{
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

		static bool SelectNearestFeasibleIslandY(
			const FIsland& Island,
			const double PreferredY,
			const TArray<FRect>& BlockingRects,
			const FOptions& Options,
			const double MinY,
			double& OutY)
		{
			TArray<double> CandidateYs;
			const bool bHasMinY = MinY > -TNumericLimits<double>::Max() * 0.5;
			auto AddCandidateY = [&CandidateYs, MinY, bHasMinY](const double InY)
			{
				const double CandidateY = bHasMinY ? FMath::Max(InY, MinY) : InY;
				for (const double Existing : CandidateYs)
				{
					if (FMath::IsNearlyEqual(Existing, CandidateY, 0.5))
					{
						return;
					}
				}
				CandidateYs.Add(CandidateY);
			};
			AddCandidateY(PreferredY);
			if (bHasMinY)
			{
				AddCandidateY(MinY);
			}
			const double Height = Island.LocalBounds.Height();
			for (const FRect& Rect : BlockingRects)
			{
				AddCandidateY(Rect.Top - Height - Options.NodePadding);
				AddCandidateY(Rect.Bottom + Options.NodePadding);
			}
			CandidateYs.Sort([PreferredY](double A, double B)
			{
				const double ResidualA = FMath::Abs(A - PreferredY);
				const double ResidualB = FMath::Abs(B - PreferredY);
				if (!FMath::IsNearlyEqual(ResidualA, ResidualB, 0.5))
				{
					return ResidualA < ResidualB;
				}
				return A < B;
			});

			double LastTested = TNumericLimits<double>::Max();
			for (const double CandidateY : CandidateYs)
			{
				if (FMath::IsNearlyEqual(CandidateY, LastTested, 0.5))
				{
					continue;
				}
				LastTested = CandidateY;
				FIsland CandidateIsland = Island;
				CandidateIsland.Y = CandidateY;
				if (!RectOverlapsAny(IslandAabb(CandidateIsland), BlockingRects, Options.NodePadding))
				{
					OutY = CandidateY;
					return true;
				}
			}
			return false;
		}

		static void PlaceIslandsYByWeightedMedianAndPacking(
			TArray<FIsland>& Islands,
			const TArray<FSelectedEndpointCandidate>& SelectedEndpoints,
			const FOptions& Options)
		{
			TArray<int32> IslandOrder;
			for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
			{
				IslandOrder.Add(IslandIndex);
			}
			const TArray<FSourcePortOrderConstraint> SourcePortOrderConstraints = BuildSourcePortOrderConstraints(Islands, SelectedEndpoints);
			IslandOrder.Sort([&Islands, &SourcePortOrderConstraints](int32 A, int32 B)
			{
				if (Islands[A].Rank != Islands[B].Rank)
				{
					return Islands[A].Rank < Islands[B].Rank;
				}
				const int32 SourceOrderCompare = CompareIslandsBySourcePortOrder(A, B, SourcePortOrderConstraints);
				if (SourceOrderCompare != 0)
				{
					return SourceOrderCompare < 0;
				}
				if (!FMath::IsNearlyEqual(Islands[A].X, Islands[B].X, 0.5))
				{
					return Islands[A].X < Islands[B].X;
				}
				return A < B;
			});

			TArray<uint8> Placed;
			Placed.Init(0, Islands.Num());
			TArray<FRect> BlockingRects;
			double RootCursorY = Options.OriginY;
			for (const int32 IslandIndex : IslandOrder)
			{
				if (!Islands.IsValidIndex(IslandIndex))
				{
					continue;
				}
				TArray<double> PreferredYs;
				for (const FSelectedEndpointCandidate& Selected : SelectedEndpoints)
				{
					if (Selected.TargetIsland != IslandIndex
						|| !Islands.IsValidIndex(Selected.SourceIsland)
						|| !Placed.IsValidIndex(Selected.SourceIsland)
						|| !Placed[Selected.SourceIsland])
					{
						continue;
					}
					PreferredYs.Add(Islands[Selected.SourceIsland].Y + Selected.SourceLocal.Y - Selected.ActualTargetLocal.Y);
				}
				double PreferredY = RootCursorY;
				if (PreferredYs.Num() > 0)
				{
					PreferredY = MedianOf(PreferredYs, RootCursorY);
				}
				double SelectedY = PreferredY;
				const double MinY = MinYFromPlacedSourcePortOrderPredecessors(
					IslandIndex,
					Islands,
					Placed,
					SourcePortOrderConstraints,
					Options);
				if (!SelectNearestFeasibleIslandY(Islands[IslandIndex], PreferredY, BlockingRects, Options, MinY, SelectedY))
				{
					SelectedY = FMath::Max(RootCursorY, MinY);
				}
				Islands[IslandIndex].Y = SelectedY;
				Islands[IslandIndex].PreferredY = PreferredY;
				BlockingRects.Append(BuildIslandHardOccupancyRects(TArray<FNode>(), Islands[IslandIndex], Options));
				BlockingRects.Last() = IslandAabb(Islands[IslandIndex]);
				Placed[IslandIndex] = 1;
				RootCursorY = FMath::Max(RootCursorY, IslandAabb(Islands[IslandIndex]).Bottom + Options.VerticalSpacing);
			}
			// This is a design contract, not a cosmetic post-fix: source-port order must be reflected by island placement itself.
		}

		static TArray<FVector2D> BuildOrthogonalRoutePolyline(
			const FVector2D& Start,
			const FVector2D& End,
			const double TrunkY,
			const FOptions& Options,
			const TArray<FRect>& HardRects)
		{
			const double EntryX = ComputeInputXAlignedToOutput(Start.X, Options);
			const double ExitX = ComputeOutputXAlignedToInput(End.X, Options);
			if (EntryX > ExitX + 0.5)
			{
				return BuildDirectSplinePolyline(Start, End);
			}
			const double Clearance = FMath::Max(Options.NodePadding * 1.5, 48.0);
			auto MakeRoute = [&](const TArray<FVector2D>& MidPoints)
			{
				TArray<FVector2D> Points;
				Points.Add(Start);
				for (const FVector2D& Point : MidPoints)
				{
					Points.Add(Point);
				}
				Points.Add(End);
				return Points;
			};
			auto Compact = [](const TArray<FVector2D>& InPoints)
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
			};
			auto MakeBusRoute = [&](const double BusY)
			{
				TArray<FVector2D> MidPoints;
				const double SpanX = ExitX - EntryX;
				if (SpanX >= EndpointAccessClearanceX(Options) - 0.5)
				{
					MidPoints.Add(FVector2D(EntryX, BusY));
					MidPoints.Add(FVector2D(ExitX, BusY));
				}
				else
				{
					MidPoints.Add(FVector2D((EntryX + ExitX) * 0.5, BusY));
				}
				return Compact(MakeRoute(MidPoints));
			};
			auto MakeBusRouteWithBranchX = [&](const double BusY, const double BranchX)
			{
				TArray<FVector2D> MidPoints;
				const double ClampedBranchX = FMath::Clamp(BranchX, EntryX, ExitX);
				if (FMath::Abs(ClampedBranchX - EntryX) >= EndpointAccessClearanceX(Options) - 0.5)
				{
					MidPoints.Add(FVector2D(EntryX, BusY));
					MidPoints.Add(FVector2D(ClampedBranchX, BusY));
				}
				else
				{
					MidPoints.Add(FVector2D(EntryX, BusY));
				}
				return Compact(MakeRoute(MidPoints));
			};
			auto AddCandidate = [](TArray<double>& Values, const double Value)
			{
				if (!FMath::IsFinite(Value))
				{
					return;
				}
				for (const double Existing : Values)
				{
					if (FMath::IsNearlyEqual(Existing, Value, 0.5))
					{
						return;
					}
				}
				Values.Add(Value);
			};
			auto ContainsCandidate = [](const TArray<double>& Values, const double Value)
			{
				for (const double Existing : Values)
				{
					if (FMath::IsNearlyEqual(Existing, Value, 0.5))
					{
						return true;
					}
				}
				return false;
			};
			auto TrimCandidateValues = [&ContainsCandidate](TArray<double>& Values, const TArray<double>& EssentialValues, const int32 MaxCount, TFunctionRef<double(double)> Score)
			{
				if (Values.Num() <= MaxCount)
				{
					Values.Sort();
					return;
				}

				TArray<double> Kept;
				Kept.Reserve(MaxCount);
				for (const double EssentialValue : EssentialValues)
				{
					if (FMath::IsFinite(EssentialValue) && !ContainsCandidate(Kept, EssentialValue))
					{
						Kept.Add(EssentialValue);
					}
				}

				Values.Sort([&Score](const double A, const double B)
				{
					const double ScoreA = Score(A);
					const double ScoreB = Score(B);
					if (!FMath::IsNearlyEqual(ScoreA, ScoreB, 0.001))
					{
						return ScoreA < ScoreB;
					}
					return A < B;
				});
				for (const double Value : Values)
				{
					if (Kept.Num() >= MaxCount)
					{
						break;
					}
					if (!ContainsCandidate(Kept, Value))
					{
						Kept.Add(Value);
					}
				}
				Kept.Sort();
				Values = MoveTemp(Kept);
			};

			TArray<double> CandidateYs;
			AddCandidate(CandidateYs, TrunkY);
			AddCandidate(CandidateYs, FMath::Min(Start.Y, End.Y) - Options.VerticalSpacing);
			AddCandidate(CandidateYs, FMath::Max(Start.Y, End.Y) + Options.VerticalSpacing);
			AddCandidate(CandidateYs, Start.Y);
			AddCandidate(CandidateYs, End.Y);

			TArray<double> CandidateBranchXs;
			AddCandidate(CandidateBranchXs, ExitX);
			AddCandidate(CandidateBranchXs, EntryX);
			AddCandidate(CandidateBranchXs, (EntryX + ExitX) * 0.5);
			AddCandidate(CandidateBranchXs, EntryX + EndpointAccessClearanceX(Options));

			const double SearchLeft = FMath::Min(EntryX, ExitX) - Options.HorizontalSpacing * 0.5;
			const double SearchRight = FMath::Max(EntryX, ExitX) + Options.HorizontalSpacing * 0.5;
			const double SearchTop = FMath::Min(FMath::Min(Start.Y, End.Y), TrunkY) - Options.VerticalSpacing * 4.0;
			const double SearchBottom = FMath::Max(FMath::Max(Start.Y, End.Y), TrunkY) + Options.VerticalSpacing * 4.0;
			for (const FRect& Rect : HardRects)
			{
				if (Rect.Right < SearchLeft || Rect.Left > SearchRight || Rect.Bottom < SearchTop || Rect.Top > SearchBottom)
				{
					continue;
				}
				AddCandidate(CandidateYs, Rect.Top - Clearance);
				AddCandidate(CandidateYs, Rect.Bottom + Clearance);
				AddCandidate(CandidateBranchXs, FMath::Clamp(Rect.Left - Clearance, EntryX, ExitX));
				AddCandidate(CandidateBranchXs, FMath::Clamp(Rect.Right + Clearance, EntryX, ExitX));
			}
			const TArray<double> EssentialYs = {
				TrunkY,
				Start.Y,
				End.Y,
				FMath::Min(Start.Y, End.Y) - Options.VerticalSpacing,
				FMath::Max(Start.Y, End.Y) + Options.VerticalSpacing
			};
			const TArray<double> EssentialBranchXs = {
				ExitX,
				EntryX,
				(EntryX + ExitX) * 0.5,
				EntryX + EndpointAccessClearanceX(Options)
			};
			constexpr int32 MaxOrthogonalRouteYCandidates = 18;
			constexpr int32 MaxOrthogonalRouteBranchCandidates = 12;
			TrimCandidateValues(
				CandidateYs,
				EssentialYs,
				MaxOrthogonalRouteYCandidates,
				[TrunkY, Start, End](const double Value)
				{
					const double PortDistance = FMath::Min(FMath::Abs(Value - Start.Y), FMath::Abs(Value - End.Y));
					return FMath::Abs(Value - TrunkY) + PortDistance * 0.05;
				});
			TrimCandidateValues(
				CandidateBranchXs,
				EssentialBranchXs,
				MaxOrthogonalRouteBranchCandidates,
				[EntryX, ExitX](const double Value)
				{
					const double MidX = (EntryX + ExitX) * 0.5;
					return FMath::Abs(Value - ExitX) * 0.7 + FMath::Abs(Value - MidX) * 0.1;
				});

			TArray<FVector2D> Best = MakeBusRoute(TrunkY);
			double BestCost = TNumericLimits<double>::Max();
			for (const double CandidateY : CandidateYs)
			{
				for (const double CandidateBranchX : CandidateBranchXs)
				{
					TArray<FVector2D> CandidateRoute = MakeBusRouteWithBranchX(CandidateY, CandidateBranchX);
					if (!HasNonNegativeConnectionAlignmentShortenForEverySegment(CandidateRoute, Options))
					{
						continue;
					}
					const int32 HitCount = CountPolylineHardOccupancyHits(CandidateRoute, HardRects);
					const double VariancePenalty = FMath::Abs(CandidateY - TrunkY) * 0.01;
					const double BranchShiftPenalty = FMath::Abs(ExitX - CandidateBranchX) * 0.02;
					const double TopTieBias = CandidateY <= TrunkY ? -0.001 : 0.0;
					const double Cost = static_cast<double>(HitCount) * 1000000.0
						+ PolylineManhattanLength(CandidateRoute)
						+ VariancePenalty
						+ BranchShiftPenalty
						+ TopTieBias;
					if (Cost + 0.001 < BestCost)
					{
						BestCost = Cost;
						Best = MoveTemp(CandidateRoute);
					}
				}
			}
			if (CountPolylineHardOccupancyHits(Best, HardRects) > 0)
			{
				TArray<double> CandidateDoglegXs;
				AddCandidate(CandidateDoglegXs, EntryX);
				AddCandidate(CandidateDoglegXs, ExitX);
				AddCandidate(CandidateDoglegXs, (EntryX + ExitX) * 0.5);
				for (const FRect& Rect : HardRects)
				{
					if (Rect.Right < SearchLeft || Rect.Left > SearchRight || Rect.Bottom < SearchTop || Rect.Top > SearchBottom)
					{
						continue;
					}
					AddCandidate(CandidateDoglegXs, FMath::Clamp(Rect.Left - Clearance * 2.0, EntryX, ExitX));
					AddCandidate(CandidateDoglegXs, FMath::Clamp(Rect.Left - Clearance, EntryX, ExitX));
					AddCandidate(CandidateDoglegXs, FMath::Clamp(Rect.Right + Clearance, EntryX, ExitX));
					AddCandidate(CandidateDoglegXs, FMath::Clamp(Rect.Right + Clearance * 2.0, EntryX, ExitX));
				}
				TrimCandidateValues(
					CandidateDoglegXs,
					EssentialBranchXs,
					MaxOrthogonalRouteBranchCandidates,
					[EntryX, ExitX](const double Value)
					{
						const double MidX = (EntryX + ExitX) * 0.5;
						return FMath::Abs(Value - ExitX) * 0.7 + FMath::Abs(Value - MidX) * 0.1;
					});

				const double MinSegmentX = EndpointAccessClearanceX(Options) - 0.5;
				auto BuildDoglegRoute = [&](const double FirstY, const double FirstX, const double SecondX, const double SecondY)
				{
					TArray<FVector2D> MidPoints;
					auto AddMidPoint = [&](const FVector2D& Point)
					{
						if (MidPoints.Num() == 0 || FVector2D::Distance(MidPoints.Last(), Point) >= 0.5)
						{
							MidPoints.Add(Point);
						}
					};
					AddMidPoint(FVector2D(EntryX, FirstY));
					if (FirstX - MidPoints.Last().X >= MinSegmentX)
					{
						AddMidPoint(FVector2D(FirstX, FirstY));
					}
					if (SecondX - MidPoints.Last().X < MinSegmentX)
					{
						return TArray<FVector2D>();
					}
					AddMidPoint(FVector2D(SecondX, SecondY));
					if (ExitX - MidPoints.Last().X >= MinSegmentX)
					{
						AddMidPoint(FVector2D(ExitX, SecondY));
					}
					return Compact(MakeRoute(MidPoints));
				};

				for (const double FirstY : CandidateYs)
				{
					for (const double SecondY : CandidateYs)
					{
						for (const double FirstX : CandidateDoglegXs)
						{
							for (const double SecondX : CandidateDoglegXs)
							{
								if (FirstX < EntryX - 0.5 || SecondX > ExitX + 0.5 || SecondX - FirstX < MinSegmentX)
								{
									continue;
								}
								TArray<FVector2D> CandidateRoute = BuildDoglegRoute(FirstY, FirstX, SecondX, SecondY);
								if (CandidateRoute.Num() == 0 || !HasNonNegativeConnectionAlignmentShortenForEverySegment(CandidateRoute, Options))
								{
									continue;
								}
								const int32 HitCount = CountPolylineHardOccupancyHits(CandidateRoute, HardRects);
								const double TopTieBias = FirstY <= TrunkY ? -0.001 : 0.0;
								const double Cost = static_cast<double>(HitCount) * 1000000.0
									+ PolylineManhattanLength(CandidateRoute)
									+ FMath::Abs(FirstY - TrunkY) * 0.01
									+ FMath::Abs(SecondY - TrunkY) * 0.01
									+ TopTieBias;
								if (Cost + 0.001 < BestCost)
								{
									BestCost = Cost;
									Best = MoveTemp(CandidateRoute);
								}
							}
						}
					}
				}
			}
			return Best;
		}

		struct FPlannedCorridorRoute
		{
			ERouteKind RouteKind = ERouteKind::Direct;
			TArray<FVector2D> Polyline;
			double BranchX = 0.0;
			double TrunkY = 0.0;
			bool bCrossesHardOccupancy = false;
			bool bNeedsBus = false;
			bool bEndsAtBoundaryEntry = false;
		};

		static bool ShouldUpgradeLongDirectConnectionToBus(
			const FVector2D& Start,
			const FVector2D& End,
			const int32 SameSourceSelectedCount,
			const FOptions& Options)
		{
			const double XSpan = End.X - Start.X;
			if (XSpan <= 0.5)
			{
				return false;
			}

			const double LongSpan = FMath::Max(Options.HorizontalSpacing * 3.0, 900.0);
			const double VeryLongSpan = FMath::Max(Options.HorizontalSpacing * 4.5, 1350.0);
			const double NoticeableSlopeY = FMath::Max(Options.NodePadding * 0.5, 24.0);
			const double StrongSlopeY = FMath::Max(Options.VerticalSpacing * 0.6, 120.0);

			if (XSpan >= VeryLongSpan)
			{
				return true;
			}

			const double YSpan = FMath::Abs(End.Y - Start.Y);
			if (YSpan <= FMath::Max(6.0, Options.NodePadding * 0.125))
			{
				return false;
			}
			if (XSpan >= LongSpan && YSpan >= StrongSlopeY)
			{
				return true;
			}
			return SameSourceSelectedCount > 1 && XSpan >= LongSpan && YSpan >= NoticeableSlopeY;
		}

		static double SelectDirectConnectionTrunkY(
			const FVector2D& Start,
			const FVector2D& End,
			const bool bDirectBlocked,
			const FOptions& Options)
		{
			const double MidY = (Start.Y + End.Y) * 0.5;
			if (bDirectBlocked)
			{
				return MidY;
			}

			const double XSpan = End.X - Start.X;
			const double YSpan = FMath::Abs(End.Y - Start.Y);
			const double VeryLongSpan = FMath::Max(Options.HorizontalSpacing * 4.5, 1350.0);
			const double NearlyFlatY = FMath::Max(Options.NodePadding * 0.5, 24.0);
			if (XSpan >= VeryLongSpan && YSpan <= NearlyFlatY)
			{
				return FMath::Min(Start.Y, End.Y) - Options.VerticalSpacing;
			}
			return MidY;
		}

		static bool BoundaryEntryToActualTargetRequiresGuidedRoute(
			const FSelectedEndpointCandidate& Selected,
			const FVector2D& EntryEnd,
			const FVector2D& ActualEnd,
			const FOptions& Options)
		{
			if (FVector2D::Distance(EntryEnd, ActualEnd) <= 0.5)
			{
				return false;
			}

			const double XSpan = FMath::Abs(ActualEnd.X - EntryEnd.X);
			const double YSpan = FMath::Abs(ActualEnd.Y - EntryEnd.Y);
			const double EndpointClearance = EndpointAccessClearanceX(Options);
			if (Selected.Side == TEXT("left"))
			{
				const double LeftEntrySpanThreshold = FMath::Max(Options.HorizontalSpacing * 0.35, EndpointClearance * 2.0);
				return XSpan > LeftEntrySpanThreshold;
			}
			if (Selected.Side == TEXT("top") || Selected.Side == TEXT("bottom"))
			{
				const double EdgeSpanThresholdX = FMath::Max(Options.HorizontalSpacing * 0.75, EndpointClearance * 3.0);
				const double EdgeSpanThresholdY = FMath::Max(Options.VerticalSpacing * 0.75, Options.NodePadding * 2.0);
				return XSpan > EdgeSpanThresholdX || YSpan > EdgeSpanThresholdY;
			}
			return false;
		}

		static FPlannedCorridorRoute PlanCorridorRouteForSelectedEndpoint(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const FSelectedEndpointCandidate& Selected,
			const int32 SameSourceSelectedCount,
			const double IdealBusY,
			const FOptions& Options)
		{
			FPlannedCorridorRoute Planned;
			if (!Islands.IsValidIndex(Selected.SourceIsland)
				|| !Islands.IsValidIndex(Selected.TargetIsland)
				|| !Edges.IsValidIndex(Selected.EdgeIndex))
			{
				return Planned;
			}
			const FIsland& SourceIsland = Islands[Selected.SourceIsland];
			const FIsland& TargetIsland = Islands[Selected.TargetIsland];
			const FVector2D Start(SourceIsland.X + Selected.SourceLocal.X, SourceIsland.Y + Selected.SourceLocal.Y);
			const FVector2D ActualEnd(TargetIsland.X + Selected.ActualTargetLocal.X, TargetIsland.Y + Selected.ActualTargetLocal.Y);
			const FVector2D EntryEnd(TargetIsland.X + Selected.EntryLocal.X, TargetIsland.Y + Selected.EntryLocal.Y);
			const FEdge& Edge = Edges[Selected.EdgeIndex];
			const TArray<FRect> DirectBlockingRects = BuildDirectUpgradeBlockingRects(Nodes, Islands, Options, Selected, Edge);
			const TArray<FRect> BusHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edge);

			Planned.Polyline = BuildDirectSplinePolyline(Start, ActualEnd);
			const bool bDirectBlocked = PolylineHitsHardOccupancy(Planned.Polyline, DirectBlockingRects);
			const bool bBoundaryEntryDiffersFromActualTarget = FVector2D::Distance(EntryEnd, ActualEnd) > 0.5;
			bool bBoundaryEntryToActualTargetBlocked = false;
			if (bBoundaryEntryDiffersFromActualTarget)
			{
				TArray<FVector2D> EntryToActualControlPoints;
				EntryToActualControlPoints.Add(EntryEnd);
				EntryToActualControlPoints.Add(ActualEnd);
				bBoundaryEntryToActualTargetBlocked = ControlWireHitsHardOccupancy(EntryToActualControlPoints, BusHardRects);
			}
			const bool bBoundaryEntryToActualTargetNeedsGuidance =
				BoundaryEntryToActualTargetRequiresGuidedRoute(Selected, EntryEnd, ActualEnd, Options);
			const bool bDirectTooLongForReadableSpline =
				ShouldUpgradeLongDirectConnectionToBus(Start, ActualEnd, SameSourceSelectedCount, Options);
			Planned.BranchX = ComputeOutputXAlignedToInput(ActualEnd.X, Options);
			const bool bBusForwardPossible = Planned.BranchX + 0.5 >= ComputeInputXAlignedToOutput(Start.X, Options);
			Planned.bNeedsBus = bBusForwardPossible && (bDirectBlocked || bBoundaryEntryToActualTargetBlocked || bBoundaryEntryToActualTargetNeedsGuidance || bDirectTooLongForReadableSpline);
			if (!Planned.bNeedsBus)
			{
				Planned.RouteKind = ERouteKind::Direct;
				return Planned;
			}

			Planned.Polyline = BuildOrthogonalRoutePolyline(Start, ActualEnd, IdealBusY, Options, BusHardRects);
			Planned.bEndsAtBoundaryEntry = false;
			Planned.TrunkY = Planned.Polyline.Num() > 2 ? Planned.Polyline[1].Y : IdealBusY;
			Planned.BranchX = Planned.Polyline.Num() >= 3
				? Planned.Polyline[Planned.Polyline.Num() - 2].X
				: ComputeOutputXAlignedToInput(EntryEnd.X, Options);
			Planned.bCrossesHardOccupancy = PolylineHitsHardOccupancy(Planned.Polyline, BusHardRects);
			Planned.RouteKind = Planned.bCrossesHardOccupancy ? ERouteKind::ExternalBus : ERouteKind::LocalBus;
			return Planned;
		}

		static void CompactRoutePoints(FReroutePlan& Plan, const FVector2D& Start, const FVector2D& End, const FOptions& Options)
		{
			TArray<FVector2D> Compacted;
			const double MinDistance = FMath::Max(10.0, Options.NodePadding * 0.35);
			for (const FVector2D& Point : Plan.Points)
			{
				if (FVector2D::Distance(Point, Start) < MinDistance || FVector2D::Distance(Point, End) < MinDistance)
				{
					continue;
				}
				if (Compacted.Num() > 0 && FVector2D::Distance(Compacted.Last(), Point) < MinDistance)
				{
					continue;
				}
				if (Compacted.Num() >= 2)
				{
					const FVector2D A = Compacted[Compacted.Num() - 2];
					const FVector2D B = Compacted.Last();
					const bool bSameX = FMath::IsNearlyEqual(A.X, B.X, 0.5) && FMath::IsNearlyEqual(B.X, Point.X, 0.5);
					const bool bSameY = FMath::IsNearlyEqual(A.Y, B.Y, 0.5) && FMath::IsNearlyEqual(B.Y, Point.Y, 0.5);
					if (bSameX || bSameY)
					{
						Compacted.Last() = Point;
						continue;
					}
				}
				Compacted.Add(Point);
			}
			Plan.Points = MoveTemp(Compacted);
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

		static void AppendDirectConnectionObstacleReroutePlans(
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

			for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
			{
				if (PlannedEdgeIndices.Contains(EdgeIndex)
					|| (SkipEdgeIndices && SkipEdgeIndices->Contains(EdgeIndex)))
				{
					continue;
				}

				const FEdge& Edge = Edges[EdgeIndex];
				if (!IsValidNodeIndex(Nodes, Edge.Source) || !IsValidNodeIndex(Nodes, Edge.Target))
				{
					continue;
				}
				const int32* SourceIslandPtr = IslandByNode.Find(Edge.Source);
				const int32* TargetIslandPtr = IslandByNode.Find(Edge.Target);
				if (!SourceIslandPtr || !TargetIslandPtr
					|| !Islands.IsValidIndex(*SourceIslandPtr)
					|| !Islands.IsValidIndex(*TargetIslandPtr))
				{
					continue;
				}

				const FIsland& SourceIsland = Islands[*SourceIslandPtr];
				const FIsland& TargetIsland = Islands[*TargetIslandPtr];
				const FVector2D Start = FVector2D(SourceIsland.X, SourceIsland.Y) + GetSourcePort(Nodes[Edge.Source], Edge, true);
				const FVector2D End = FVector2D(TargetIsland.X, TargetIsland.Y) + GetTargetPort(Nodes[Edge.Target], Edge, true);
				const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edge);
				const TArray<FVector2D> DirectPolyline = BuildDirectSplinePolyline(Start, End);
				const int32 DirectHitCount = CountPolylineHardOccupancyHits(DirectPolyline, RouteHardRects);
				const bool bDirectBlocked = DirectHitCount > 0;
				const bool bDirectTooLongForReadableSpline = ShouldUpgradeLongDirectConnectionToBus(Start, End, 1, Options);
				if (!bDirectBlocked && !bDirectTooLongForReadableSpline)
				{
					continue;
				}

				const double IdealBusY = SelectDirectConnectionTrunkY(Start, End, bDirectBlocked, Options);
				TArray<FVector2D> Route = BuildOrthogonalRoutePolyline(Start, End, IdealBusY, Options, RouteHardRects);
				if (Route.Num() < 4 || !HasNonNegativeConnectionAlignmentShortenForEverySegment(Route, Options))
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("kind"), TEXT("direct_obstacle_route_rejected"));
					Obj->SetNumberField(TEXT("edge_index"), EdgeIndex);
					Obj->SetNumberField(TEXT("direct_hit_count"), DirectHitCount);
					Obj->SetBoolField(TEXT("long_direct"), bDirectTooLongForReadableSpline);
					Obj->SetStringField(TEXT("reason"), TEXT("no_forward_aligned_route"));
					Result.RejectionDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
					continue;
				}

				const int32 RouteHitCount = CountPolylineHardOccupancyHits(Route, RouteHardRects);
				if (RouteHitCount > 0)
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("kind"), TEXT("direct_obstacle_route_rejected"));
					Obj->SetNumberField(TEXT("edge_index"), EdgeIndex);
					Obj->SetNumberField(TEXT("direct_hit_count"), DirectHitCount);
					Obj->SetNumberField(TEXT("route_hit_count"), RouteHitCount);
					Obj->SetBoolField(TEXT("long_direct"), bDirectTooLongForReadableSpline);
					Obj->SetStringField(TEXT("reason"), TEXT("candidate_still_hits_hard_occupancy"));
					Result.RejectionDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
					continue;
				}

				FReroutePlan Plan;
				Plan.EdgeIndex = EdgeIndex;
				Plan.RouteKind = ERouteKind::LocalBus;
				Plan.BusId = MakeSourceParamId(Edge);
				Plan.Reason = bDirectBlocked ? TEXT("direct_obstacle_local_bus") : TEXT("long_direct_local_bus");
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
				AddRouteDiagnostics(Result, EdgeIndex, TEXT("direct"), ERouteKind::LocalBus, Route, false, false);
				++Result.DesignDiagnostics.RouteKindUpgradeCount;
			}
		}

		static int32 CountPolylineSegmentHardOccupancyHits(const TArray<FVector2D>& Points, const TArray<FRect>& HardRects)
		{
			int32 HitCount = 0;
			for (int32 PointIndex = 0; PointIndex + 1 < Points.Num(); ++PointIndex)
			{
				for (const FRect& Rect : HardRects)
				{
					if (SegmentIntersectsRect(Points[PointIndex], Points[PointIndex + 1], Rect))
					{
						++HitCount;
						break;
					}
				}
			}
			return HitCount;
		}

		static TArray<FVector2D> BuildVisibleRouteFromReroutePoints(
			const FVector2D& Start,
			const TArray<FVector2D>& Points,
			const FVector2D& End)
		{
			TArray<FVector2D> Route;
			Route.Reserve(Points.Num() + 2);
			Route.Add(Start);
			for (const FVector2D& Point : Points)
			{
				Route.Add(Point);
			}
			Route.Add(End);
			return Route;
		}

		static void SetLeadingSharedBusTrunkY(TArray<FVector2D>& Points, const double SharedY)
		{
			if (Points.Num() == 0)
			{
				return;
			}
			const double OriginalY = Points[0].Y;
			for (FVector2D& Point : Points)
			{
				if (!FMath::IsNearlyEqual(Point.Y, OriginalY, 0.5))
				{
					break;
				}
				Point.Y = SharedY;
			}
		}

		static bool TryGetRouteEndpointsForPlan(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const TMap<int32, int32>& IslandByNode,
			const FReroutePlan& Plan,
			FVector2D& OutStart,
			FVector2D& OutEnd)
		{
			if (!Edges.IsValidIndex(Plan.EdgeIndex))
			{
				return false;
			}
			const FEdge& Edge = Edges[Plan.EdgeIndex];
			const int32* SourceIslandPtr = IslandByNode.Find(Edge.Source);
			const int32* TargetIslandPtr = IslandByNode.Find(Edge.Target);
			if (!SourceIslandPtr || !TargetIslandPtr
				|| !Islands.IsValidIndex(*SourceIslandPtr)
				|| !Islands.IsValidIndex(*TargetIslandPtr)
				|| !IsValidNodeIndex(Nodes, Edge.Source)
				|| !IsValidNodeIndex(Nodes, Edge.Target))
			{
				return false;
			}

			OutStart = FVector2D(Islands[*SourceIslandPtr].X, Islands[*SourceIslandPtr].Y) + GetSourcePort(Nodes[Edge.Source], Edge, true);
			OutEnd = FVector2D(Islands[*TargetIslandPtr].X, Islands[*TargetIslandPtr].Y) + GetTargetPort(Nodes[Edge.Target], Edge, true);
			return true;
		}

		static int32 NormalizeSharedBusTrunkYAfterSegmentRefine(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const TMap<int32, int32>& IslandByNode,
			const FOptions& Options,
			TArray<FReroutePlan>& InOutPlans,
			FRunResult& Result)
		{
			TMap<int32, TArray<int32>> PlansByMergeGroup;
			for (int32 PlanIndex = 0; PlanIndex < InOutPlans.Num(); ++PlanIndex)
			{
				const FReroutePlan& Plan = InOutPlans[PlanIndex];
				if (Plan.AttachmentMergeGroup == INDEX_NONE
					|| Plan.BusId.IsEmpty()
					|| Plan.RouteKind == ERouteKind::Direct
					|| Plan.Points.Num() == 0)
				{
					continue;
				}
				PlansByMergeGroup.FindOrAdd(Plan.AttachmentMergeGroup).Add(PlanIndex);
			}

			int32 NormalizedPlanCount = 0;
			for (const TPair<int32, TArray<int32>>& Pair : PlansByMergeGroup)
			{
				const TArray<int32>& GroupPlanIndices = Pair.Value;
				if (GroupPlanIndices.Num() <= 1)
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
					for (const double Existing : CandidateYs)
					{
						if (FMath::IsNearlyEqual(Existing, Value, 0.5))
						{
							return;
						}
					}
					CandidateYs.Add(Value);
				};

				for (const int32 PlanIndex : GroupPlanIndices)
				{
					if (InOutPlans.IsValidIndex(PlanIndex) && InOutPlans[PlanIndex].Points.Num() > 0)
					{
						AddCandidateY(InOutPlans[PlanIndex].Points[0].Y);
					}
				}
				if (CandidateYs.Num() <= 1)
				{
					continue;
				}

				double BestY = CandidateYs[0];
				int32 BestHitCount = TNumericLimits<int32>::Max();
				double BestCost = TNumericLimits<double>::Max();
				for (const double CandidateY : CandidateYs)
				{
					int32 TotalHitCount = 0;
					double TotalCost = 0.0;
					bool bCandidateValid = true;
					for (const int32 PlanIndex : GroupPlanIndices)
					{
						if (!InOutPlans.IsValidIndex(PlanIndex))
						{
							bCandidateValid = false;
							break;
						}
						const FReroutePlan& Plan = InOutPlans[PlanIndex];
						FVector2D Start;
						FVector2D End;
						if (!TryGetRouteEndpointsForPlan(Nodes, Edges, Islands, IslandByNode, Plan, Start, End) || !Edges.IsValidIndex(Plan.EdgeIndex))
						{
							bCandidateValid = false;
							break;
						}

						TArray<FVector2D> CandidatePoints = Plan.Points;
						SetLeadingSharedBusTrunkY(CandidatePoints, CandidateY);
						TArray<FVector2D> CandidateRoute = BuildVisibleRouteFromReroutePoints(Start, CandidatePoints, End);
						if (!HasNonNegativeConnectionAlignmentShortenForEverySegment(CandidateRoute, Options))
						{
							bCandidateValid = false;
							break;
						}

						const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[Plan.EdgeIndex]);
						TotalHitCount += CountPolylineSegmentHardOccupancyHits(CandidateRoute, RouteHardRects);
						TotalCost += PolylineManhattanLength(CandidateRoute)
							+ FMath::Abs(CandidateY - Plan.Points[0].Y) * 0.05;
					}
					if (!bCandidateValid)
					{
						continue;
					}
					if (TotalHitCount < BestHitCount
						|| (TotalHitCount == BestHitCount && TotalCost + 0.001 < BestCost))
					{
						BestY = CandidateY;
						BestHitCount = TotalHitCount;
						BestCost = TotalCost;
					}
				}

				if (BestHitCount != 0)
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("kind"), TEXT("shared_bus_trunk_y_normalize_rejected"));
					Obj->SetNumberField(TEXT("attachment_merge_group"), Pair.Key);
					Obj->SetNumberField(TEXT("best_hit_count"), BestHitCount == TNumericLimits<int32>::Max() ? -1 : BestHitCount);
					Result.RejectionDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
					continue;
				}

				int32 ChangedInGroup = 0;
				for (const int32 PlanIndex : GroupPlanIndices)
				{
					if (!InOutPlans.IsValidIndex(PlanIndex) || InOutPlans[PlanIndex].Points.Num() == 0)
					{
						continue;
					}
					const double OldY = InOutPlans[PlanIndex].Points[0].Y;
					if (!FMath::IsNearlyEqual(OldY, BestY, 0.5))
					{
						SetLeadingSharedBusTrunkY(InOutPlans[PlanIndex].Points, BestY);
						FinalizeReroutePlanPoints(InOutPlans[PlanIndex], Options);
						++ChangedInGroup;
					}
				}
				if (ChangedInGroup > 0)
				{
					NormalizedPlanCount += ChangedInGroup;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("kind"), TEXT("shared_bus_trunk_y_normalized"));
					Obj->SetStringField(TEXT("side"), TEXT("segment_refine"));
					Obj->SetNumberField(TEXT("attachment_merge_group"), Pair.Key);
					Obj->SetNumberField(TEXT("shared_y"), BestY);
					Obj->SetNumberField(TEXT("changed_plan_count"), ChangedInGroup);
					Result.CorridorRouteDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}
			return NormalizedPlanCount;
		}

		static int32 NormalizeSharedBusTrunkPrefixAfterSegmentRefine(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const TMap<int32, int32>& IslandByNode,
			const FOptions& Options,
			TArray<FReroutePlan>& InOutPlans,
			FRunResult& Result)
		{
			struct FPlanPrefixInfo
			{
				int32 PlanIndex = INDEX_NONE;
				int32 LeadingCount = 0;
				double BranchX = 0.0;
				double TrunkY = 0.0;
				FVector2D Start = FVector2D::ZeroVector;
				FVector2D End = FVector2D::ZeroVector;
			};

			TMap<int32, TArray<int32>> PlansByMergeGroup;
			for (int32 PlanIndex = 0; PlanIndex < InOutPlans.Num(); ++PlanIndex)
			{
				const FReroutePlan& Plan = InOutPlans[PlanIndex];
				if (Plan.AttachmentMergeGroup == INDEX_NONE
					|| Plan.BusId.IsEmpty()
					|| Plan.RouteKind == ERouteKind::Direct
					|| Plan.Points.Num() == 0)
				{
					continue;
				}
				PlansByMergeGroup.FindOrAdd(Plan.AttachmentMergeGroup).Add(PlanIndex);
			}

			int32 NormalizedPlanCount = 0;
			for (const TPair<int32, TArray<int32>>& Pair : PlansByMergeGroup)
			{
				const TArray<int32>& GroupPlanIndices = Pair.Value;
				if (GroupPlanIndices.Num() <= 1)
				{
					continue;
				}

				TArray<FPlanPrefixInfo> PrefixInfos;
				TArray<double> TrunkXs;
				auto AddUniqueTrunkX = [&TrunkXs](const double Value)
				{
					if (!FMath::IsFinite(Value))
					{
						return;
					}
					for (const double Existing : TrunkXs)
					{
						if (FMath::IsNearlyEqual(Existing, Value, 0.5))
						{
							return;
						}
					}
					TrunkXs.Add(Value);
				};
				double TrunkY = 0.0;
				bool bHaveTrunkY = false;
				double StartXSum = 0.0;
				double BranchXSum = 0.0;
				bool bValidGroup = true;
				for (const int32 PlanIndex : GroupPlanIndices)
				{
					if (!InOutPlans.IsValidIndex(PlanIndex) || InOutPlans[PlanIndex].Points.Num() == 0)
					{
						bValidGroup = false;
						break;
					}
					const FReroutePlan& Plan = InOutPlans[PlanIndex];
					FVector2D Start;
					FVector2D End;
					if (!TryGetRouteEndpointsForPlan(Nodes, Edges, Islands, IslandByNode, Plan, Start, End))
					{
						bValidGroup = false;
						break;
					}

					const double PlanTrunkY = Plan.Points[0].Y;
					if (!bHaveTrunkY)
					{
						TrunkY = PlanTrunkY;
						bHaveTrunkY = true;
					}
					else if (!FMath::IsNearlyEqual(TrunkY, PlanTrunkY, 0.5))
					{
						bValidGroup = false;
						break;
					}

					int32 LeadingCount = 0;
					while (Plan.Points.IsValidIndex(LeadingCount)
						&& FMath::IsNearlyEqual(Plan.Points[LeadingCount].Y, TrunkY, 0.5))
					{
						AddUniqueTrunkX(Plan.Points[LeadingCount].X);
						++LeadingCount;
					}
					if (LeadingCount <= 0)
					{
						bValidGroup = false;
						break;
					}

					FPlanPrefixInfo Info;
					Info.PlanIndex = PlanIndex;
					Info.LeadingCount = LeadingCount;
					Info.BranchX = Plan.Points[LeadingCount - 1].X;
					Info.TrunkY = TrunkY;
					Info.Start = Start;
					Info.End = End;
					PrefixInfos.Add(Info);
					StartXSum += Start.X;
					BranchXSum += Info.BranchX;
				}
				if (!bValidGroup || PrefixInfos.Num() <= 1 || TrunkXs.Num() <= 1)
				{
					continue;
				}

				const bool bAscending = BranchXSum >= StartXSum;
				TrunkXs.Sort([bAscending](const double A, const double B)
				{
					return bAscending ? A < B : A > B;
				});

				TArray<TArray<FVector2D>> CandidatePointsByInfo;
				CandidatePointsByInfo.SetNum(PrefixInfos.Num());
				int32 ChangedInGroup = 0;
				bool bAllRoutesValid = true;
				const double MinDistance = FMath::Max(10.0, Options.NodePadding * 0.35);
				for (int32 InfoIndex = 0; InfoIndex < PrefixInfos.Num(); ++InfoIndex)
				{
					const FPlanPrefixInfo& Info = PrefixInfos[InfoIndex];
					const FReroutePlan& Plan = InOutPlans[Info.PlanIndex];
					TArray<FVector2D> CandidatePoints;
					for (const double TrunkX : TrunkXs)
					{
						const bool bBeforeBranch = bAscending
							? TrunkX <= Info.BranchX + 0.5
							: TrunkX >= Info.BranchX - 0.5;
						if (!bBeforeBranch)
						{
							continue;
						}
						const FVector2D SharedPoint(TrunkX, TrunkY);
						if (FVector2D::Distance(SharedPoint, Info.Start) < MinDistance
							|| FVector2D::Distance(SharedPoint, Info.End) < MinDistance)
						{
							continue;
						}
						if (CandidatePoints.Num() == 0
							|| FVector2D::Distance(CandidatePoints.Last(), SharedPoint) >= 1.0)
						{
							CandidatePoints.Add(SharedPoint);
						}
					}
					for (int32 PointIndex = Info.LeadingCount; PointIndex < Plan.Points.Num(); ++PointIndex)
					{
						const FVector2D TailPoint = Plan.Points[PointIndex];
						if (CandidatePoints.Num() == 0
							|| FVector2D::Distance(CandidatePoints.Last(), TailPoint) >= 1.0)
						{
							CandidatePoints.Add(TailPoint);
						}
					}

					TArray<FVector2D> CandidateRoute = BuildVisibleRouteFromReroutePoints(Info.Start, CandidatePoints, Info.End);
					if (!HasNonNegativeConnectionAlignmentShortenForEverySegment(CandidateRoute, Options)
						|| !Edges.IsValidIndex(Plan.EdgeIndex))
					{
						bAllRoutesValid = false;
						break;
					}
					const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[Plan.EdgeIndex]);
					if (CountPolylineSegmentHardOccupancyHits(CandidateRoute, RouteHardRects) != 0)
					{
						bAllRoutesValid = false;
						break;
					}

					if (CandidatePoints.Num() != Plan.Points.Num())
					{
						++ChangedInGroup;
					}
					else
					{
						for (int32 PointIndex = 0; PointIndex < CandidatePoints.Num(); ++PointIndex)
						{
							if (FVector2D::Distance(CandidatePoints[PointIndex], Plan.Points[PointIndex]) >= 1.0)
							{
								++ChangedInGroup;
								break;
							}
						}
					}
					CandidatePointsByInfo[InfoIndex] = MoveTemp(CandidatePoints);
				}

				if (!bAllRoutesValid || ChangedInGroup <= 0)
				{
					continue;
				}

				for (int32 InfoIndex = 0; InfoIndex < PrefixInfos.Num(); ++InfoIndex)
				{
					FReroutePlan& Plan = InOutPlans[PrefixInfos[InfoIndex].PlanIndex];
					Plan.Points = MoveTemp(CandidatePointsByInfo[InfoIndex]);
					FinalizeReroutePlanPoints(Plan, Options);
				}
				NormalizedPlanCount += ChangedInGroup;
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("kind"), TEXT("shared_bus_trunk_prefix_normalized"));
				Obj->SetStringField(TEXT("side"), TEXT("segment_refine"));
				Obj->SetNumberField(TEXT("attachment_merge_group"), Pair.Key);
				Obj->SetNumberField(TEXT("shared_y"), TrunkY);
				Obj->SetNumberField(TEXT("shared_trunk_point_count"), TrunkXs.Num());
				Obj->SetNumberField(TEXT("changed_plan_count"), ChangedInGroup);
				Result.CorridorRouteDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
			}
			return NormalizedPlanCount;
		}

		static int32 RefineReroutePlansAgainstHardOccupancySegments(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const TMap<int32, int32>& IslandByNode,
			const FOptions& Options,
			TArray<FReroutePlan>& InOutPlans,
			FRunResult& Result)
		{
			int32 RefinedCount = 0;
			for (FReroutePlan& Plan : InOutPlans)
			{
				if (!Edges.IsValidIndex(Plan.EdgeIndex))
				{
					continue;
				}
				const FEdge& Edge = Edges[Plan.EdgeIndex];
				const int32* SourceIslandPtr = IslandByNode.Find(Edge.Source);
				const int32* TargetIslandPtr = IslandByNode.Find(Edge.Target);
				if (!SourceIslandPtr || !TargetIslandPtr
					|| !Islands.IsValidIndex(*SourceIslandPtr)
					|| !Islands.IsValidIndex(*TargetIslandPtr)
					|| !IsValidNodeIndex(Nodes, Edge.Source)
					|| !IsValidNodeIndex(Nodes, Edge.Target))
				{
					continue;
				}

				const FVector2D Start = FVector2D(Islands[*SourceIslandPtr].X, Islands[*SourceIslandPtr].Y) + GetSourcePort(Nodes[Edge.Source], Edge, true);
				const FVector2D End = FVector2D(Islands[*TargetIslandPtr].X, Islands[*TargetIslandPtr].Y) + GetTargetPort(Nodes[Edge.Target], Edge, true);
				TArray<FVector2D> CurrentRoute = BuildVisibleRouteFromReroutePoints(Start, Plan.Points, End);

				const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edge);
				const int32 CurrentHitCount = CountPolylineSegmentHardOccupancyHits(CurrentRoute, RouteHardRects);
				if (CurrentHitCount == 0)
				{
					continue;
				}

				TArray<double> PreferredYs;
				PreferredYs.Add((Start.Y + End.Y) * 0.5);
				for (const FVector2D& Point : Plan.Points)
				{
					PreferredYs.Add(Point.Y);
				}
				PreferredYs.Add(FMath::Min(Start.Y, End.Y) - Options.VerticalSpacing);
				PreferredYs.Add(FMath::Max(Start.Y, End.Y) + Options.VerticalSpacing);

				TArray<FVector2D> BestRoute;
				double BestCost = TNumericLimits<double>::Max();
				int32 BestHitCount = TNumericLimits<int32>::Max();
				for (const double PreferredY : PreferredYs)
				{
					TArray<FVector2D> CandidateRoute = BuildOrthogonalRoutePolyline(Start, End, PreferredY, Options, RouteHardRects);
					if (CandidateRoute.Num() < 4 || !HasNonNegativeConnectionAlignmentShortenForEverySegment(CandidateRoute, Options))
					{
						continue;
					}
					const int32 CandidateHitCount = CountPolylineSegmentHardOccupancyHits(CandidateRoute, RouteHardRects);
					const double CandidateCost = static_cast<double>(CandidateHitCount) * 1000000.0
						+ PolylineManhattanLength(CandidateRoute)
						+ FMath::Abs(PreferredY - ((Start.Y + End.Y) * 0.5)) * 0.01;
					if (CandidateHitCount < BestHitCount
						|| (CandidateHitCount == BestHitCount && CandidateCost + 0.001 < BestCost))
					{
						BestRoute = MoveTemp(CandidateRoute);
						BestHitCount = CandidateHitCount;
						BestCost = CandidateCost;
					}
				}

				if (BestRoute.Num() >= 4 && BestHitCount == 0)
				{
					Plan.Points.Reset();
					for (int32 PointIndex = 1; PointIndex + 1 < BestRoute.Num(); ++PointIndex)
					{
						Plan.Points.Add(BestRoute[PointIndex]);
					}
					Plan.Reason += TEXT("_segment_refined");
					FinalizeReroutePlanPoints(Plan, Options);
					AddRouteDiagnostics(Result, Plan.EdgeIndex, TEXT("segment_refine"), Plan.RouteKind, BestRoute, false, false);
					++RefinedCount;
					continue;
				}

				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("kind"), TEXT("reroute_plan_segment_refine_rejected"));
				Obj->SetNumberField(TEXT("edge_index"), Plan.EdgeIndex);
				Obj->SetNumberField(TEXT("current_hit_count"), CurrentHitCount);
				Obj->SetNumberField(TEXT("best_hit_count"), BestHitCount == TNumericLimits<int32>::Max() ? -1 : BestHitCount);
				Obj->SetStringField(TEXT("reason"), TEXT("no_hard_free_forward_candidate"));
				Result.RejectionDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
			}
			RefinedCount += NormalizeSharedBusTrunkYAfterSegmentRefine(Nodes, Edges, Islands, IslandByNode, Options, InOutPlans, Result);
			RefinedCount += NormalizeSharedBusTrunkPrefixAfterSegmentRefine(Nodes, Edges, Islands, IslandByNode, Options, InOutPlans, Result);
			return RefinedCount;
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
			Result.CorridorRouteDiagnostics.Add(MakeShared<FJsonValueObject>(RouteObj));
			Result.DesignDiagnostics.TotalRouteLength += PolylineManhattanLength(Points);
			if (bCrossesHardOccupancy)
			{
				++Result.DesignDiagnostics.RouteThroughHardOccupancyCount;
			}
		}

		struct FPendingCorridorBusRoute
		{
			int32 SelectedEndpointIndex = INDEX_NONE;
			int32 ContractIndex = INDEX_NONE;
			int32 EdgeIndex = INDEX_NONE;
			FString SourceParamId;
			FString Side = TEXT("left");
			FPlannedCorridorRoute Planned;
			FVector2D Start = FVector2D::ZeroVector;
			FVector2D End = FVector2D::ZeroVector;
			FVector2D ActualEnd = FVector2D::ZeroVector;
		};

		static int32 GetTargetBranchPointIndex(const FPlannedCorridorRoute& Planned)
		{
			for (int32 PointIndex = Planned.Polyline.Num() - 2; PointIndex >= 1; --PointIndex)
			{
				if (FMath::IsNearlyEqual(Planned.Polyline[PointIndex].Y, Planned.TrunkY, 0.5))
				{
					return PointIndex;
				}
			}
			return Planned.Polyline.Num() >= 3 ? Planned.Polyline.Num() - 2 : INDEX_NONE;
		}

		static bool IntervalOverlaps(const double AMin, const double AMax, const double BMin, const double BMax, const double Tolerance = 0.5)
		{
			return AMin <= BMax + Tolerance && BMin <= AMax + Tolerance;
		}

		static void RecomputeBusContractBoundsFromRoute(FBusContract& Contract, const FPlannedCorridorRoute& Planned)
		{
			Contract.XMin = TNumericLimits<double>::Max();
			Contract.XMax = -TNumericLimits<double>::Max();
			for (const FVector2D& Point : Planned.Polyline)
			{
				Contract.XMin = FMath::Min(Contract.XMin, Point.X);
				Contract.XMax = FMath::Max(Contract.XMax, Point.X);
			}
			if (Contract.XMin == TNumericLimits<double>::Max())
			{
				Contract.XMin = 0.0;
				Contract.XMax = 0.0;
			}
		}

		static bool GetRouteHorizontalTrunkInterval(const FPlannedCorridorRoute& Planned, double& OutXMin, double& OutXMax)
		{
			OutXMin = TNumericLimits<double>::Max();
			OutXMax = -TNumericLimits<double>::Max();
			for (int32 PointIndex = 0; PointIndex + 1 < Planned.Polyline.Num(); ++PointIndex)
			{
				const FVector2D& A = Planned.Polyline[PointIndex];
				const FVector2D& B = Planned.Polyline[PointIndex + 1];
				if (!FMath::IsNearlyEqual(A.Y, Planned.TrunkY, 0.5) || !FMath::IsNearlyEqual(B.Y, Planned.TrunkY, 0.5))
				{
					continue;
				}
				if (FMath::Abs(A.X - B.X) < 0.5)
				{
					continue;
				}
				OutXMin = FMath::Min(OutXMin, FMath::Min(A.X, B.X));
				OutXMax = FMath::Max(OutXMax, FMath::Max(A.X, B.X));
			}
			return OutXMin <= OutXMax;
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

			TArray<FVector2D> Points;
			Points.Add(Start);
			const double ClampedBranchX = FMath::Clamp(BranchX, EntryX, ExitX);
			if (ClampedBranchX - EntryX >= EndpointAccessClearanceX(Options) - 0.5)
			{
				Points.Add(FVector2D(EntryX, TrunkY));
				Points.Add(FVector2D(ClampedBranchX, TrunkY));
			}
			else
			{
				Points.Add(FVector2D(EntryX, TrunkY));
			}
			Points.Add(End);
			return CompactVisiblePolyline(Points);
		}

		static TArray<FVector2D> BuildSharedSourceBusRoutePolylineWithTargetDogleg(
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

			const double ClampedBranchX = FMath::Clamp(BranchX, EntryX, ExitX);
			if (ExitX - ClampedBranchX < EndpointAccessClearanceX(Options) - 0.5
				|| FMath::IsNearlyEqual(TrunkY, End.Y, 0.5))
			{
				return BuildSharedSourceBusRoutePolyline(Start, End, TrunkY, BranchX, Options);
			}

			TArray<FVector2D> Points;
			Points.Add(Start);
			if (ClampedBranchX - EntryX >= EndpointAccessClearanceX(Options) - 0.5)
			{
				Points.Add(FVector2D(EntryX, TrunkY));
				Points.Add(FVector2D(ClampedBranchX, TrunkY));
			}
			else
			{
				Points.Add(FVector2D(EntryX, TrunkY));
			}
			const double DoglegX = FMath::Clamp(
				ClampedBranchX + EndpointAccessClearanceX(Options),
				ClampedBranchX,
				ExitX);
			if (DoglegX - ClampedBranchX >= EndpointAccessClearanceX(Options) - 0.5)
			{
				Points.Add(FVector2D(DoglegX, End.Y));
			}
			Points.Add(FVector2D(ExitX, End.Y));
			Points.Add(End);
			return CompactVisiblePolyline(Points);
		}

		static double GetFallbackInterfaceRowSpacing(const FOptions& Options)
		{
			return FMath::Max(Options.NodePadding * 1.5, 36.0);
		}

		static TArray<FVector2D> BuildSharedSourceBusRoutePolylineAvoidingHardRects(
			const FVector2D& Start,
			const FVector2D& End,
			const double TrunkY,
			const double BranchX,
			const TArray<FRect>& HardRects,
			const FOptions& Options,
			const bool bUseTargetDogleg)
		{
			const double EntryX = ComputeInputXAlignedToOutput(Start.X, Options);
			const double ExitX = ComputeOutputXAlignedToInput(End.X, Options);
			if (EntryX > ExitX + 0.5)
			{
				return BuildDirectSplinePolyline(Start, End);
			}

			const double ClampedBranchX = FMath::Clamp(BranchX, EntryX, ExitX);
			const double HorizontalEndX = ClampedBranchX - EntryX >= EndpointAccessClearanceX(Options) - 0.5
				? ClampedBranchX
				: EntryX;
			TArray<FVector2D> Points;
			Points.Add(Start);
			Points.Add(FVector2D(EntryX, TrunkY));

			if (!FMath::IsNearlyEqual(EntryX, HorizontalEndX, 0.5))
			{
				struct FHorizontalBlocker
				{
					double Left = 0.0;
					double Right = 0.0;
					double Top = 0.0;
					double Bottom = 0.0;
				};

				const double XMin = FMath::Min(EntryX, HorizontalEndX);
				const double XMax = FMath::Max(EntryX, HorizontalEndX);
				TArray<FHorizontalBlocker> Blockers;
				for (const FRect& Rect : HardRects)
				{
					if (TrunkY < Rect.Top - 0.5 || TrunkY > Rect.Bottom + 0.5)
					{
						continue;
					}
					if (!IntervalOverlaps(XMin, XMax, Rect.Left, Rect.Right))
					{
						continue;
					}
					FHorizontalBlocker Blocker;
					Blocker.Left = FMath::Max(XMin, Rect.Left);
					Blocker.Right = FMath::Min(XMax, Rect.Right);
					Blocker.Top = Rect.Top;
					Blocker.Bottom = Rect.Bottom;
					if (Blocker.Left <= Blocker.Right)
					{
						Blockers.Add(Blocker);
					}
				}
				Blockers.Sort([](const FHorizontalBlocker& A, const FHorizontalBlocker& B)
				{
					if (!FMath::IsNearlyEqual(A.Left, B.Left, 0.5))
					{
						return A.Left < B.Left;
					}
					return A.Right < B.Right;
				});

				const double DetourGapX = FMath::Max(EndpointAccessClearanceX(Options) * 0.5, 24.0);
				const double DetourGapY = GetFallbackInterfaceRowSpacing(Options);
				const double DetourScanStepY = FMath::Max(4.0, FMath::Min(8.0, DetourGapY * 0.25));
				auto HorizontalSpanHitsHardRect = [&HardRects](const double Y, const double LeftX, const double RightX)
				{
					const double SpanLeft = FMath::Min(LeftX, RightX);
					const double SpanRight = FMath::Max(LeftX, RightX);
					for (const FRect& Rect : HardRects)
					{
						if (Y < Rect.Top - 0.5 || Y > Rect.Bottom + 0.5)
						{
							continue;
						}
						if (IntervalOverlaps(SpanLeft, SpanRight, Rect.Left, Rect.Right))
						{
							return true;
						}
					}
					return false;
				};
				auto DetourCandidateIsClear = [&HardRects, &HorizontalSpanHitsHardRect, TrunkY](
					const double CandidateY,
					const double LeftX,
					const double RightX)
				{
					if (HorizontalSpanHitsHardRect(CandidateY, LeftX, RightX))
					{
						return false;
					}
					return !SegmentHitsAnyRect(FVector2D(LeftX, TrunkY), FVector2D(LeftX, CandidateY), HardRects)
						&& !SegmentHitsAnyRect(FVector2D(RightX, CandidateY), FVector2D(RightX, TrunkY), HardRects);
				};
				auto PickDetourY = [&HardRects, &DetourCandidateIsClear, TrunkY, DetourGapY, DetourScanStepY](
					const FHorizontalBlocker& Blocker,
					const double LeftX,
					const double RightX)
				{
					TArray<double> CandidateYs;
					auto AddCandidateY = [&CandidateYs](const double Value)
					{
						if (!FMath::IsFinite(Value))
						{
							return;
						}
						for (const double Existing : CandidateYs)
						{
							if (FMath::IsNearlyEqual(Existing, Value, 0.5))
							{
								return;
							}
						}
						CandidateYs.Add(Value);
					};
					AddCandidateY(Blocker.Top - DetourGapY);
					AddCandidateY(Blocker.Bottom + DetourGapY);
					for (int32 Step = 1; Step <= 96; ++Step)
					{
						const double OffsetY = static_cast<double>(Step) * DetourScanStepY;
						AddCandidateY(TrunkY - OffsetY);
						AddCandidateY(TrunkY + OffsetY);
					}
					for (const FRect& Rect : HardRects)
					{
						AddCandidateY(Rect.Top - DetourGapY);
						AddCandidateY(Rect.Bottom + DetourGapY);
					}
					CandidateYs.Sort([TrunkY](const double A, const double B)
					{
						const double DeltaA = FMath::Abs(A - TrunkY);
						const double DeltaB = FMath::Abs(B - TrunkY);
						if (!FMath::IsNearlyEqual(DeltaA, DeltaB, 0.5))
						{
							return DeltaA < DeltaB;
						}
						return A < B;
					});
					for (const double CandidateY : CandidateYs)
					{
						if (DetourCandidateIsClear(CandidateY, LeftX, RightX))
						{
							return CandidateY;
						}
					}
					const double AboveY = Blocker.Top - DetourGapY;
					const double BelowY = Blocker.Bottom + DetourGapY;
					return FMath::Abs(AboveY - TrunkY) <= FMath::Abs(BelowY - TrunkY)
						? AboveY
						: BelowY;
				};
				double CurrentX = EntryX;
				auto AddPoint = [&Points](const FVector2D& Point)
				{
					if (Points.Num() == 0 || FVector2D::Distance(Points.Last(), Point) >= 0.5)
					{
						Points.Add(Point);
					}
				};
				for (const FHorizontalBlocker& Blocker : Blockers)
				{
					const double BeforeX = FMath::Clamp(Blocker.Left - DetourGapX, XMin, XMax);
					const double AfterX = FMath::Clamp(Blocker.Right + DetourGapX, XMin, XMax);
					if (AfterX <= CurrentX + 0.5)
					{
						continue;
					}
					const double SafeBeforeX = FMath::Max(CurrentX, BeforeX);
					const double DetourY = PickDetourY(Blocker, SafeBeforeX, AfterX);
					AddPoint(FVector2D(SafeBeforeX, TrunkY));
					AddPoint(FVector2D(SafeBeforeX, DetourY));
					AddPoint(FVector2D(AfterX, DetourY));
					AddPoint(FVector2D(AfterX, TrunkY));
					CurrentX = AfterX;
				}
				AddPoint(FVector2D(HorizontalEndX, TrunkY));
			}

			if (bUseTargetDogleg
				&& ExitX - ClampedBranchX >= EndpointAccessClearanceX(Options) - 0.5
				&& !FMath::IsNearlyEqual(TrunkY, End.Y, 0.5))
			{
				const double DoglegX = FMath::Clamp(
					ClampedBranchX + EndpointAccessClearanceX(Options),
					ClampedBranchX,
					ExitX);
				if (DoglegX - ClampedBranchX >= EndpointAccessClearanceX(Options) - 0.5)
				{
					Points.Add(FVector2D(DoglegX, End.Y));
				}
				Points.Add(FVector2D(ExitX, End.Y));
			}
			Points.Add(End);
			return CompactVisiblePolyline(Points);
		}

		static void RecomputeBusContractBoundsFromPendingRoutes(
			FBusContract& Contract,
			const TArray<FPendingCorridorBusRoute>& PendingBusRoutes)
		{
			Contract.XMin = TNumericLimits<double>::Max();
			Contract.XMax = -TNumericLimits<double>::Max();
			Contract.BranchYMin = TNumericLimits<double>::Max();
			Contract.BranchYMax = -TNumericLimits<double>::Max();
			Contract.OutputToTrunkDistanceY = 0.0;
			Contract.BranchXs.Reset();
			Contract.EdgeIndices.Reset();
			bool bAnyExternal = false;
			bool bAnyLocal = false;
			for (const FPendingCorridorBusRoute& Pending : PendingBusRoutes)
			{
				if (Pending.ContractIndex != Contract.Id)
				{
					continue;
				}
				Contract.EdgeIndices.AddUnique(Pending.EdgeIndex);
				Contract.BranchXs.Add(Pending.Planned.BranchX);
				Contract.TrunkY = Pending.Planned.TrunkY;
				Contract.BranchYMin = FMath::Min(Contract.BranchYMin, FMath::Min(Pending.Planned.TrunkY, Pending.End.Y));
				Contract.BranchYMax = FMath::Max(Contract.BranchYMax, FMath::Max(Pending.Planned.TrunkY, Pending.End.Y));
				Contract.OutputToTrunkDistanceY = FMath::Max(Contract.OutputToTrunkDistanceY, FMath::Abs(Pending.Start.Y - Pending.Planned.TrunkY));
				bAnyExternal = bAnyExternal || Pending.Planned.RouteKind == ERouteKind::ExternalBus || Pending.Planned.bCrossesHardOccupancy;
				bAnyLocal = bAnyLocal || Pending.Planned.RouteKind == ERouteKind::LocalBus;
				for (const FVector2D& Point : Pending.Planned.Polyline)
				{
					Contract.XMin = FMath::Min(Contract.XMin, Point.X);
					Contract.XMax = FMath::Max(Contract.XMax, Point.X);
				}
			}
			if (Contract.XMin == TNumericLimits<double>::Max())
			{
				Contract.XMin = 0.0;
				Contract.XMax = 0.0;
			}
			if (Contract.BranchYMin == TNumericLimits<double>::Max())
			{
				Contract.BranchYMin = 0.0;
				Contract.BranchYMax = 0.0;
			}
			Contract.RouteKind = bAnyExternal ? ERouteKind::ExternalBus : (bAnyLocal ? ERouteKind::LocalBus : ERouteKind::Direct);
		}

		struct FSharedSourceBusPendingRoute
		{
			TArray<FVector2D> Polyline;
			double BranchX = 0.0;
			double IdealBranchX = 0.0;
			double LeftBoundX = 0.0;
			double RightBoundX = 0.0;
			double BranchYMin = 0.0;
			double BranchYMax = 0.0;
			double OutputToTrunkDistanceY = 0.0;
			int32 HardHitCount = 0;
			double Cost = TNumericLimits<double>::Max();
		};

		struct FBusContractLayoutCandidate
		{
			TArray<FSharedSourceBusPendingRoute> Routes;
			double TrunkY = 0.0;
			double XMin = 0.0;
			double XMax = 0.0;
			double BranchYMin = 0.0;
			double BranchYMax = 0.0;
			double OutputToTrunkDistanceY = 0.0;
			double Cost = TNumericLimits<double>::Max();
			int32 HardHitCount = 0;
		};

		struct FBusContractCandidateSet
		{
			int32 ContractIndex = INDEX_NONE;
			FString SourceParamId;
			TArray<int32> PendingIndices;
			TArray<FBusContractLayoutCandidate> Candidates;
			double RoughXMin = 0.0;
			double RoughXMax = 0.0;
		};

		static double GetCrossBusBranchLaneSpacing(const FOptions& Options)
		{
			return FMath::Max(Options.NodePadding * 1.5, 36.0);
		}

		static double GetCrossBusBranchSetbackLimitX(const FOptions& Options)
		{
			return FMath::Max(
				Options.HorizontalSpacing,
				EndpointAccessClearanceX(Options) + GetCrossBusBranchLaneSpacing(Options));
		}

		static void AddInterfaceRowSpacingCandidate(TArray<double>& Spacings, const double Spacing)
		{
			if (FMath::IsFinite(Spacing) && Spacing >= 8.0 && Spacing <= 160.0)
			{
				Spacings.Add(Spacing);
			}
		}

		static void AddAdjacentInterfaceRowSpacings(
			const TArray<FVector2D>& Anchors,
			const TArray<uint8>& AnchorValid,
			TArray<double>& Spacings)
		{
			TArray<double> Ys;
			for (int32 Index = 0; Index < Anchors.Num(); ++Index)
			{
				if (AnchorValid.IsValidIndex(Index) && AnchorValid[Index] != 0)
				{
					Ys.Add(Anchors[Index].Y);
				}
			}
			Ys.Sort();
			for (int32 Index = 1; Index < Ys.Num(); ++Index)
			{
				AddInterfaceRowSpacingCandidate(Spacings, Ys[Index] - Ys[Index - 1]);
			}
		}

		static double EstimateInterfaceRowSpacingForBusGroup(
			const TArray<FBusContractCandidateSet>& CandidateSets,
			const TArray<int32>& Group,
			const TArray<FPendingCorridorBusRoute>& PendingBusRoutes,
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const FOptions& Options)
		{
			TArray<double> Spacings;
			TMap<int32, TArray<int32>> UsedInputPinsByNode;
			TMap<int32, TArray<int32>> UsedOutputPinsByNode;
			for (const int32 SetIndex : Group)
			{
				if (!CandidateSets.IsValidIndex(SetIndex))
				{
					continue;
				}
				for (const int32 PendingIndex : CandidateSets[SetIndex].PendingIndices)
				{
					if (!PendingBusRoutes.IsValidIndex(PendingIndex))
					{
						continue;
					}
					const int32 EdgeIndex = PendingBusRoutes[PendingIndex].EdgeIndex;
					if (!Edges.IsValidIndex(EdgeIndex))
					{
						continue;
					}
					const FEdge& Edge = Edges[EdgeIndex];
					if (Nodes.IsValidIndex(Edge.Target))
					{
						const FNode& TargetNode = Nodes[Edge.Target];
						AddAdjacentInterfaceRowSpacings(TargetNode.InputPinAnchors, TargetNode.InputPinAnchorValid, Spacings);
						if (Edge.TargetPinIndex != INDEX_NONE)
						{
							UsedInputPinsByNode.FindOrAdd(Edge.Target).AddUnique(Edge.TargetPinIndex);
						}
					}
					if (Nodes.IsValidIndex(Edge.Source))
					{
						const FNode& SourceNode = Nodes[Edge.Source];
						AddAdjacentInterfaceRowSpacings(SourceNode.OutputPinAnchors, SourceNode.OutputPinAnchorValid, Spacings);
						if (Edge.SourcePinIndex != INDEX_NONE)
						{
							UsedOutputPinsByNode.FindOrAdd(Edge.Source).AddUnique(Edge.SourcePinIndex);
						}
					}
				}
			}

			auto AddPairwiseUsedPinSpacings = [&Nodes, &Spacings](
				const TMap<int32, TArray<int32>>& UsedPinsByNode,
				const bool bInput)
			{
				for (const TPair<int32, TArray<int32>>& Pair : UsedPinsByNode)
				{
					if (!Nodes.IsValidIndex(Pair.Key) || Pair.Value.Num() <= 1)
					{
						continue;
					}
					const FNode& Node = Nodes[Pair.Key];
					for (int32 A = 0; A < Pair.Value.Num(); ++A)
					{
						for (int32 B = A + 1; B < Pair.Value.Num(); ++B)
						{
							const int32 PinA = Pair.Value[A];
							const int32 PinB = Pair.Value[B];
							const TArray<FVector2D>& Anchors = bInput ? Node.InputPinAnchors : Node.OutputPinAnchors;
							const TArray<uint8>& Valid = bInput ? Node.InputPinAnchorValid : Node.OutputPinAnchorValid;
							if (Anchors.IsValidIndex(PinA)
								&& Anchors.IsValidIndex(PinB)
								&& Valid.IsValidIndex(PinA)
								&& Valid.IsValidIndex(PinB)
								&& Valid[PinA] != 0
								&& Valid[PinB] != 0)
							{
								AddInterfaceRowSpacingCandidate(Spacings, FMath::Abs(Anchors[PinA].Y - Anchors[PinB].Y));
							}
						}
					}
				}
			};
			AddPairwiseUsedPinSpacings(UsedInputPinsByNode, true);
			AddPairwiseUsedPinSpacings(UsedOutputPinsByNode, false);

			if (Spacings.Num() == 0)
			{
				return GetFallbackInterfaceRowSpacing(Options);
			}
			Spacings.Sort();
			return FMath::Max(GetFallbackInterfaceRowSpacing(Options), Spacings[Spacings.Num() / 2]);
		}

		static double GetCompactBusTrunkLaneSpacingForGroup(
			const TArray<FBusContractCandidateSet>& CandidateSets,
			const TArray<int32>& Group,
			const TArray<FPendingCorridorBusRoute>& PendingBusRoutes,
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const FOptions& Options)
		{
			const double InterfaceSpacing = EstimateInterfaceRowSpacingForBusGroup(
				CandidateSets,
				Group,
				PendingBusRoutes,
				Nodes,
				Edges,
				Options);
			return FMath::Max(GetFallbackInterfaceRowSpacing(Options), InterfaceSpacing);
		}

		static double GetStandardBusTrunkLaneSpacingForGroup(
			const TArray<FBusContractCandidateSet>& CandidateSets,
			const TArray<int32>& Group,
			const TArray<FPendingCorridorBusRoute>& PendingBusRoutes,
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const FOptions& Options,
			const double BaseSpacing)
		{
			return FMath::Max(
				BaseSpacing,
				GetCompactBusTrunkLaneSpacingForGroup(CandidateSets, Group, PendingBusRoutes, Nodes, Edges, Options));
		}

		static double EstimateInterfaceRowSpacingForPendingRouteIndices(
			const TArray<int32>& PendingIndices,
			const TArray<FPendingCorridorBusRoute>& PendingBusRoutes,
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const FOptions& Options)
		{
			TArray<double> Spacings;
			TMap<int32, TArray<int32>> UsedInputPinsByNode;
			TMap<int32, TArray<int32>> UsedOutputPinsByNode;
			for (const int32 PendingIndex : PendingIndices)
			{
				if (!PendingBusRoutes.IsValidIndex(PendingIndex))
				{
					continue;
				}
				const int32 EdgeIndex = PendingBusRoutes[PendingIndex].EdgeIndex;
				if (!Edges.IsValidIndex(EdgeIndex))
				{
					continue;
				}
				const FEdge& Edge = Edges[EdgeIndex];
				if (Nodes.IsValidIndex(Edge.Target))
				{
					const FNode& TargetNode = Nodes[Edge.Target];
					AddAdjacentInterfaceRowSpacings(TargetNode.InputPinAnchors, TargetNode.InputPinAnchorValid, Spacings);
					if (Edge.TargetPinIndex != INDEX_NONE)
					{
						UsedInputPinsByNode.FindOrAdd(Edge.Target).AddUnique(Edge.TargetPinIndex);
					}
				}
				if (Nodes.IsValidIndex(Edge.Source))
				{
					const FNode& SourceNode = Nodes[Edge.Source];
					AddAdjacentInterfaceRowSpacings(SourceNode.OutputPinAnchors, SourceNode.OutputPinAnchorValid, Spacings);
					if (Edge.SourcePinIndex != INDEX_NONE)
					{
						UsedOutputPinsByNode.FindOrAdd(Edge.Source).AddUnique(Edge.SourcePinIndex);
					}
				}
			}

			auto AddPairwiseUsedPinSpacings = [&Nodes, &Spacings](
				const TMap<int32, TArray<int32>>& UsedPinsByNode,
				const bool bInput)
			{
				for (const TPair<int32, TArray<int32>>& Pair : UsedPinsByNode)
				{
					if (!Nodes.IsValidIndex(Pair.Key) || Pair.Value.Num() <= 1)
					{
						continue;
					}
					const FNode& Node = Nodes[Pair.Key];
					for (int32 A = 0; A < Pair.Value.Num(); ++A)
					{
						for (int32 B = A + 1; B < Pair.Value.Num(); ++B)
						{
							const int32 PinA = Pair.Value[A];
							const int32 PinB = Pair.Value[B];
							const TArray<FVector2D>& Anchors = bInput ? Node.InputPinAnchors : Node.OutputPinAnchors;
							const TArray<uint8>& Valid = bInput ? Node.InputPinAnchorValid : Node.OutputPinAnchorValid;
							if (Anchors.IsValidIndex(PinA)
								&& Anchors.IsValidIndex(PinB)
								&& Valid.IsValidIndex(PinA)
								&& Valid.IsValidIndex(PinB)
								&& Valid[PinA] != 0
								&& Valid[PinB] != 0)
							{
								AddInterfaceRowSpacingCandidate(Spacings, FMath::Abs(Anchors[PinA].Y - Anchors[PinB].Y));
							}
						}
					}
				}
			};
			AddPairwiseUsedPinSpacings(UsedInputPinsByNode, true);
			AddPairwiseUsedPinSpacings(UsedOutputPinsByNode, false);

			if (Spacings.Num() == 0)
			{
				return GetCrossBusBranchLaneSpacing(Options);
			}
			Spacings.Sort();
			return FMath::Max(1.0, Spacings[Spacings.Num() / 2]);
		}

		static void AddUniqueDouble(TArray<double>& Values, const double Value)
		{
			if (!FMath::IsFinite(Value))
			{
				return;
			}
			for (const double Existing : Values)
			{
				if (FMath::IsNearlyEqual(Existing, Value, 0.5))
				{
					return;
				}
			}
			Values.Add(Value);
		}

		static void UpdateBusCandidateBounds(
			FBusContractLayoutCandidate& Candidate,
			const TArray<FPendingCorridorBusRoute>& PendingBusRoutes,
			const TArray<int32>& PendingIndices)
		{
			Candidate.XMin = TNumericLimits<double>::Max();
			Candidate.XMax = -TNumericLimits<double>::Max();
			Candidate.BranchYMin = TNumericLimits<double>::Max();
			Candidate.BranchYMax = -TNumericLimits<double>::Max();
			Candidate.OutputToTrunkDistanceY = 0.0;
			Candidate.HardHitCount = 0;
			for (int32 LocalIndex = 0; LocalIndex < PendingIndices.Num(); ++LocalIndex)
			{
				if (!PendingBusRoutes.IsValidIndex(PendingIndices[LocalIndex]) || !Candidate.Routes.IsValidIndex(LocalIndex))
				{
					continue;
				}
				const FPendingCorridorBusRoute& Pending = PendingBusRoutes[PendingIndices[LocalIndex]];
				const FSharedSourceBusPendingRoute& Route = Candidate.Routes[LocalIndex];
				Candidate.HardHitCount += Route.HardHitCount;
				Candidate.BranchYMin = FMath::Min(Candidate.BranchYMin, FMath::Min(Candidate.TrunkY, Pending.End.Y));
				Candidate.BranchYMax = FMath::Max(Candidate.BranchYMax, FMath::Max(Candidate.TrunkY, Pending.End.Y));
				Candidate.OutputToTrunkDistanceY = FMath::Max(Candidate.OutputToTrunkDistanceY, FMath::Abs(Pending.Start.Y - Candidate.TrunkY));
				for (const FVector2D& Point : Route.Polyline)
				{
					Candidate.XMin = FMath::Min(Candidate.XMin, Point.X);
					Candidate.XMax = FMath::Max(Candidate.XMax, Point.X);
				}
			}
			if (Candidate.XMin == TNumericLimits<double>::Max())
			{
				Candidate.XMin = 0.0;
				Candidate.XMax = 0.0;
			}
			if (Candidate.BranchYMin == TNumericLimits<double>::Max())
			{
				Candidate.BranchYMin = Candidate.TrunkY;
				Candidate.BranchYMax = Candidate.TrunkY;
			}
		}

		static TArray<FSharedSourceBusPendingRoute> BuildSharedSourceBusPendingRouteVariants(
			const FPendingCorridorBusRoute& Pending,
			const double SharedTrunkY,
			const TArray<FRect>& RouteHardRects,
			const FOptions& Options)
		{
			TArray<FSharedSourceBusPendingRoute> Variants;
			const double EntryX = ComputeInputXAlignedToOutput(Pending.Start.X, Options);
			const double ExitX = ComputeOutputXAlignedToInput(Pending.End.X, Options);
			if (EntryX > ExitX + 0.5)
			{
				return Variants;
			}

			TArray<double> CandidateBranchXs;
			const double MinBranchX = EntryX;
			auto AddCandidateX = [&CandidateBranchXs, MinBranchX, ExitX](const double Value)
			{
				if (!FMath::IsFinite(Value))
				{
					return;
				}
				const double ClampedValue = FMath::Clamp(Value, MinBranchX, ExitX);
				for (const double Existing : CandidateBranchXs)
				{
					if (FMath::IsNearlyEqual(Existing, ClampedValue, 0.5))
					{
						return;
					}
				}
				CandidateBranchXs.Add(ClampedValue);
			};

			AddCandidateX(ExitX);
			AddCandidateX(ExitX - EndpointAccessClearanceX(Options));
			AddCandidateX(Pending.Planned.BranchX);
			AddCandidateX((EntryX + ExitX) * 0.5);
			AddCandidateX(EntryX + EndpointAccessClearanceX(Options));
			const double BranchLaneSpacing = GetCrossBusBranchLaneSpacing(Options);
			constexpr int32 MaxTargetSideBranchLaneCandidates = 12;
			for (int32 LaneIndex = 1; LaneIndex <= MaxTargetSideBranchLaneCandidates; ++LaneIndex)
			{
				AddCandidateX(ExitX - static_cast<double>(LaneIndex) * BranchLaneSpacing);
				AddCandidateX(ExitX - EndpointAccessClearanceX(Options) - static_cast<double>(LaneIndex) * BranchLaneSpacing);
			}
			const double Clearance = FMath::Max(Options.NodePadding * 1.5, 48.0);
			const double SearchLeft = FMath::Min(EntryX, ExitX) - Options.HorizontalSpacing * 0.5;
			const double SearchRight = FMath::Max(EntryX, ExitX) + Options.HorizontalSpacing * 0.5;
			const double SearchTop = FMath::Min(FMath::Min(Pending.Start.Y, Pending.End.Y), SharedTrunkY) - Options.VerticalSpacing * 4.0;
			const double SearchBottom = FMath::Max(FMath::Max(Pending.Start.Y, Pending.End.Y), SharedTrunkY) + Options.VerticalSpacing * 4.0;
			for (const FRect& Rect : RouteHardRects)
			{
				if (Rect.Right < SearchLeft || Rect.Left > SearchRight || Rect.Bottom < SearchTop || Rect.Top > SearchBottom)
				{
					continue;
				}
				AddCandidateX(FMath::Clamp(Rect.Left - Clearance, EntryX, ExitX));
				AddCandidateX(FMath::Clamp(Rect.Right + Clearance, EntryX, ExitX));
			}
			CandidateBranchXs.Sort();

			for (const double CandidateBranchX : CandidateBranchXs)
			{
				TArray<TArray<FVector2D>> CandidateRoutes;
				const bool bRequiresTargetDogleg = !FMath::IsNearlyEqual(SharedTrunkY, Pending.End.Y, 0.5)
					&& ComputeOutputXAlignedToInput(Pending.End.X, Options) - CandidateBranchX >= EndpointAccessClearanceX(Options) - 0.5;
				if (!bRequiresTargetDogleg)
				{
					CandidateRoutes.Add(BuildSharedSourceBusRoutePolyline(
						Pending.Start,
						Pending.End,
						SharedTrunkY,
						CandidateBranchX,
						Options));
				}
				CandidateRoutes.Add(BuildSharedSourceBusRoutePolylineWithTargetDogleg(
					Pending.Start,
					Pending.End,
					SharedTrunkY,
					CandidateBranchX,
					Options));
				if (!bRequiresTargetDogleg)
				{
					CandidateRoutes.Add(BuildSharedSourceBusRoutePolylineAvoidingHardRects(
						Pending.Start,
						Pending.End,
						SharedTrunkY,
						CandidateBranchX,
						RouteHardRects,
						Options,
						false));
				}
				CandidateRoutes.Add(BuildSharedSourceBusRoutePolylineAvoidingHardRects(
					Pending.Start,
					Pending.End,
					SharedTrunkY,
					CandidateBranchX,
					RouteHardRects,
					Options,
					true));
				for (TArray<FVector2D>& CandidateRoute : CandidateRoutes)
				{
					if (CandidateRoute.Num() < 2 || !HasNonNegativeConnectionAlignmentShortenForEverySegment(CandidateRoute, Options))
					{
						continue;
					}
					const int32 HitCount = CountControlWireHardOccupancyHits(CandidateRoute, RouteHardRects);
					const double CandidateCost = static_cast<double>(HitCount) * 1000000.0
						+ PolylineManhattanLength(CandidateRoute)
						+ FMath::Abs(ExitX - CandidateBranchX) * 0.02
						+ static_cast<double>(FMath::Max(0, CandidateRoute.Num() - 4)) * 4.0;
					FSharedSourceBusPendingRoute Route;
					Route.Polyline = MoveTemp(CandidateRoute);
					Route.BranchX = FMath::Clamp(CandidateBranchX, EntryX, ExitX);
					Route.IdealBranchX = ExitX;
					Route.LeftBoundX = EntryX;
					Route.RightBoundX = ExitX;
					Route.BranchYMin = FMath::Min(SharedTrunkY, Pending.End.Y);
					Route.BranchYMax = FMath::Max(SharedTrunkY, Pending.End.Y);
					Route.OutputToTrunkDistanceY = FMath::Abs(Pending.Start.Y - SharedTrunkY);
					Route.HardHitCount = HitCount;
					Route.Cost = CandidateCost;
					Variants.Add(MoveTemp(Route));
				}
			}

			Variants.Sort([](const FSharedSourceBusPendingRoute& A, const FSharedSourceBusPendingRoute& B)
			{
				if (A.HardHitCount != B.HardHitCount)
				{
					return A.HardHitCount < B.HardHitCount;
				}
				if (!FMath::IsNearlyEqual(A.Cost, B.Cost, 0.001))
				{
					return A.Cost < B.Cost;
				}
				return A.BranchX > B.BranchX;
			});
			constexpr int32 MaxBranchXVariantsPerRoute = 8;
			if (Variants.Num() > MaxBranchXVariantsPerRoute)
			{
				Variants.SetNum(MaxBranchXVariantsPerRoute, EAllowShrinking::Yes);
			}
			return Variants;
		}

		static FString MakeBusContractCandidateLaneSignature(
			const FBusContractLayoutCandidate& Candidate,
			const FOptions& Options)
		{
			const double BranchLaneSpacing = GetCrossBusBranchLaneSpacing(Options);
			FString Signature = FString::Printf(TEXT("Y:%lld"), FMath::RoundToInt64(Candidate.TrunkY));
			for (const FSharedSourceBusPendingRoute& Route : Candidate.Routes)
			{
				const int64 BranchLane = FMath::RoundToInt64((Route.RightBoundX - Route.BranchX) / FMath::Max(BranchLaneSpacing, 1.0));
				Signature += FString::Printf(TEXT("|B:%lld"), BranchLane);
			}
			return Signature;
		}

		static void TrimBusContractLayoutCandidatesPreservingLanes(
			TArray<FBusContractLayoutCandidate>& Candidates,
			const FOptions& Options)
		{
			constexpr int32 GlobalBestKeepCount = 32;
			constexpr int32 BestPerTrunkYKeepCount = 4;
			constexpr int32 BestPerLaneSignatureKeepCount = 2;
			constexpr int32 MaxCandidatesPerContract = 96;

			if (Candidates.Num() <= MaxCandidatesPerContract)
			{
				return;
			}

			TArray<uint8> bKeep;
			bKeep.Init(0, Candidates.Num());
			int32 KeepCount = 0;
			auto MarkKeep = [&bKeep, &KeepCount](const int32 Index)
			{
				if (!bKeep.IsValidIndex(Index) || bKeep[Index] != 0)
				{
					return;
				}
				bKeep[Index] = 1;
				++KeepCount;
			};

			for (int32 Index = 0; Index < FMath::Min(GlobalBestKeepCount, Candidates.Num()); ++Index)
			{
				MarkKeep(Index);
			}

			TMap<int64, int32> CountByTrunkY;
			TMap<FString, int32> CountByLaneSignature;
			for (int32 Index = 0; Index < Candidates.Num(); ++Index)
			{
				const int64 TrunkYKey = FMath::RoundToInt64(Candidates[Index].TrunkY);
				int32& TrunkYCount = CountByTrunkY.FindOrAdd(TrunkYKey);
				if (TrunkYCount < BestPerTrunkYKeepCount)
				{
					MarkKeep(Index);
					++TrunkYCount;
				}

				const FString LaneSignature = MakeBusContractCandidateLaneSignature(Candidates[Index], Options);
				int32& SignatureCount = CountByLaneSignature.FindOrAdd(LaneSignature);
				if (SignatureCount < BestPerLaneSignatureKeepCount)
				{
					MarkKeep(Index);
					++SignatureCount;
				}
			}

			TArray<FBusContractLayoutCandidate> KeptCandidates;
			KeptCandidates.Reserve(FMath::Min(KeepCount, MaxCandidatesPerContract));
			for (int32 Index = 0; Index < Candidates.Num(); ++Index)
			{
				if (bKeep[Index] != 0)
				{
					KeptCandidates.Add(MoveTemp(Candidates[Index]));
				}
			}
			if (KeptCandidates.Num() > MaxCandidatesPerContract)
			{
				KeptCandidates.SetNum(MaxCandidatesPerContract, EAllowShrinking::Yes);
			}
			Candidates = MoveTemp(KeptCandidates);
		}

		static TArray<FBusContractLayoutCandidate> BuildBusContractLayoutCandidates(
			const FBusContract& Contract,
			const TArray<int32>& PendingIndices,
			const TArray<FPendingCorridorBusRoute>& PendingBusRoutes,
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const FOptions& Options)
		{
			TArray<FBusContractLayoutCandidate> Candidates;
			if (PendingIndices.Num() == 0)
			{
				return Candidates;
			}

			TArray<TArray<FRect>> HardRectsByPending;
			HardRectsByPending.SetNum(PendingIndices.Num());
			TArray<double> CandidateYs;
			double InterfaceYSum = 0.0;
			int32 InterfaceYCount = 0;
			AddUniqueDouble(CandidateYs, Contract.TrunkY);
			for (int32 LocalIndex = 0; LocalIndex < PendingIndices.Num(); ++LocalIndex)
			{
				if (!PendingBusRoutes.IsValidIndex(PendingIndices[LocalIndex]))
				{
					continue;
				}
				const FPendingCorridorBusRoute& Pending = PendingBusRoutes[PendingIndices[LocalIndex]];
				AddUniqueDouble(CandidateYs, Pending.Planned.TrunkY);
				AddUniqueDouble(CandidateYs, Pending.Start.Y);
				AddUniqueDouble(CandidateYs, Pending.End.Y);
				InterfaceYSum += Pending.Start.Y + Pending.End.Y;
				InterfaceYCount += 2;
				if (Edges.IsValidIndex(Pending.EdgeIndex))
				{
					HardRectsByPending[LocalIndex] = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[Pending.EdgeIndex]);
				}
			}

			const double IdealY = InterfaceYCount > 0
				? InterfaceYSum / static_cast<double>(InterfaceYCount)
				: Contract.TrunkY;
			AddUniqueDouble(CandidateYs, IdealY);
			const double LaneSpacing = FMath::Max(Options.NodePadding * 2.0, 48.0);
			const double Clearance = FMath::Max(Options.NodePadding * 1.5, 48.0);
			double CorridorMinX = TNumericLimits<double>::Max();
			double CorridorMaxX = -TNumericLimits<double>::Max();
			double CorridorMinY = TNumericLimits<double>::Max();
			double CorridorMaxY = -TNumericLimits<double>::Max();
			for (const int32 PendingIndex : PendingIndices)
			{
				if (!PendingBusRoutes.IsValidIndex(PendingIndex))
				{
					continue;
				}
				const FPendingCorridorBusRoute& Pending = PendingBusRoutes[PendingIndex];
				CorridorMinX = FMath::Min(CorridorMinX, FMath::Min(Pending.Start.X, Pending.End.X));
				CorridorMaxX = FMath::Max(CorridorMaxX, FMath::Max(Pending.Start.X, Pending.End.X));
				CorridorMinY = FMath::Min(CorridorMinY, FMath::Min(Pending.Start.Y, Pending.End.Y));
				CorridorMaxY = FMath::Max(CorridorMaxY, FMath::Max(Pending.Start.Y, Pending.End.Y));
			}
			if (CorridorMinX == TNumericLimits<double>::Max())
			{
				CorridorMinX = 0.0;
				CorridorMaxX = 0.0;
				CorridorMinY = IdealY;
				CorridorMaxY = IdealY;
			}
			const double HardCandidateLeft = CorridorMinX - Options.HorizontalSpacing * 0.75;
			const double HardCandidateRight = CorridorMaxX + Options.HorizontalSpacing * 0.75;
			const double HardCandidateTop = FMath::Min(CorridorMinY, IdealY) - Options.VerticalSpacing * 4.0;
			const double HardCandidateBottom = FMath::Max(CorridorMaxY, IdealY) + Options.VerticalSpacing * 4.0;
			TArray<double> HardRectYCandidates;
			auto AddHardRectYCandidate = [&HardRectYCandidates](const double Value)
			{
				if (!FMath::IsFinite(Value))
				{
					return;
				}
				for (const double Existing : HardRectYCandidates)
				{
					if (FMath::IsNearlyEqual(Existing, Value, 0.5))
					{
						return;
					}
				}
				HardRectYCandidates.Add(Value);
			};
			for (int32 LocalIndex = 0; LocalIndex < PendingIndices.Num(); ++LocalIndex)
			{
				for (const FRect& Rect : HardRectsByPending[LocalIndex])
				{
					if (Rect.Right < HardCandidateLeft || Rect.Left > HardCandidateRight || Rect.Bottom < HardCandidateTop || Rect.Top > HardCandidateBottom)
					{
						continue;
					}
					AddHardRectYCandidate(Rect.Top - Clearance);
					AddHardRectYCandidate(Rect.Bottom + Clearance);
				}
			}
			HardRectYCandidates.Sort([IdealY](const double A, const double B)
			{
				const double CostA = FMath::Abs(A - IdealY);
				const double CostB = FMath::Abs(B - IdealY);
				if (!FMath::IsNearlyEqual(CostA, CostB, 0.5))
				{
					return CostA < CostB;
				}
				return A < B;
			});
			constexpr int32 MaxHardRectYCandidatesPerContract = 24;
			for (int32 Index = 0; Index < FMath::Min(MaxHardRectYCandidatesPerContract, HardRectYCandidates.Num()); ++Index)
			{
				AddUniqueDouble(CandidateYs, HardRectYCandidates[Index]);
			}
			for (int32 OffsetIndex = 1; OffsetIndex <= PendingIndices.Num() + 8; ++OffsetIndex)
			{
				AddUniqueDouble(CandidateYs, IdealY - static_cast<double>(OffsetIndex) * LaneSpacing);
				AddUniqueDouble(CandidateYs, IdealY + static_cast<double>(OffsetIndex) * LaneSpacing);
			}
			CandidateYs.Sort();

			struct FContractRouteVariantState
			{
				TArray<FSharedSourceBusPendingRoute> Routes;
				double Cost = 0.0;
				int32 HardHitCount = 0;
			};

			auto MakeRouteVariantLaneSignature = [&Options](const FContractRouteVariantState& State) -> FString
			{
				const double BranchLaneSpacing = GetCrossBusBranchLaneSpacing(Options);
				FString Signature;
				for (const FSharedSourceBusPendingRoute& Route : State.Routes)
				{
					const int64 BranchLane = FMath::RoundToInt64((Route.RightBoundX - Route.BranchX) / FMath::Max(BranchLaneSpacing, 1.0));
					Signature += FString::Printf(TEXT("|B:%lld"), BranchLane);
				}
				return Signature;
			};

			auto TrimRouteVariantStatesPreservingLanes = [&MakeRouteVariantLaneSignature](TArray<FContractRouteVariantState>& States, const int32 MaxStates)
			{
				if (States.Num() <= MaxStates)
				{
					return;
				}

				const int32 GlobalBestKeepCount = FMath::Min(128, MaxStates);
				constexpr int32 BestPerLaneSignatureKeepCount = 8;
				TArray<uint8> bKeep;
				bKeep.Init(0, States.Num());
				int32 KeepCount = 0;
				auto MarkKeep = [&bKeep, &KeepCount](const int32 Index)
				{
					if (!bKeep.IsValidIndex(Index) || bKeep[Index] != 0)
					{
						return;
					}
					bKeep[Index] = 1;
					++KeepCount;
				};

				for (int32 Index = 0; Index < FMath::Min(GlobalBestKeepCount, States.Num()); ++Index)
				{
					MarkKeep(Index);
				}

				TMap<FString, int32> CountByLaneSignature;
				for (int32 Index = 0; Index < States.Num(); ++Index)
				{
					const FString Signature = MakeRouteVariantLaneSignature(States[Index]);
					int32& Count = CountByLaneSignature.FindOrAdd(Signature);
					if (Count < BestPerLaneSignatureKeepCount)
					{
						MarkKeep(Index);
						++Count;
					}
				}

				TArray<FContractRouteVariantState> KeptStates;
				KeptStates.Reserve(FMath::Min(KeepCount, MaxStates));
				for (int32 Index = 0; Index < States.Num(); ++Index)
				{
					if (bKeep[Index] != 0)
					{
						KeptStates.Add(MoveTemp(States[Index]));
					}
				}
				if (KeptStates.Num() > MaxStates)
				{
					KeptStates.SetNum(MaxStates, EAllowShrinking::Yes);
				}
				States = MoveTemp(KeptStates);
			};

			for (const double CandidateY : CandidateYs)
			{
				bool bValid = true;
				TArray<TArray<FSharedSourceBusPendingRoute>> VariantsByPending;
				VariantsByPending.SetNum(PendingIndices.Num());
				for (int32 LocalIndex = 0; LocalIndex < PendingIndices.Num(); ++LocalIndex)
				{
					if (!PendingBusRoutes.IsValidIndex(PendingIndices[LocalIndex]))
					{
						bValid = false;
						break;
					}
					VariantsByPending[LocalIndex] = BuildSharedSourceBusPendingRouteVariants(
						PendingBusRoutes[PendingIndices[LocalIndex]],
						CandidateY,
						HardRectsByPending.IsValidIndex(LocalIndex) ? HardRectsByPending[LocalIndex] : TArray<FRect>(),
						Options);
					if (VariantsByPending[LocalIndex].Num() == 0)
					{
						bValid = false;
						break;
					}
				}
				if (!bValid)
				{
					continue;
				}

				TArray<FContractRouteVariantState> States;
				States.Add(FContractRouteVariantState());
				constexpr int32 MaxContractRouteVariantStates = 64;
				for (int32 LocalIndex = 0; LocalIndex < PendingIndices.Num(); ++LocalIndex)
				{
					TArray<FContractRouteVariantState> NextStates;
					for (const FContractRouteVariantState& State : States)
					{
						for (const FSharedSourceBusPendingRoute& Variant : VariantsByPending[LocalIndex])
						{
							FContractRouteVariantState Next = State;
							Next.Routes.Add(Variant);
							Next.HardHitCount += Variant.HardHitCount;
							Next.Cost += Variant.Cost
								+ FMath::Abs(CandidateY - IdealY) * 0.01
								+ FMath::Abs(CandidateY - PendingBusRoutes[PendingIndices[LocalIndex]].Planned.TrunkY) * 100.0;
							NextStates.Add(MoveTemp(Next));
						}
					}
					NextStates.Sort([](const FContractRouteVariantState& A, const FContractRouteVariantState& B)
					{
						if (A.HardHitCount != B.HardHitCount)
						{
							return A.HardHitCount < B.HardHitCount;
						}
						return A.Cost < B.Cost;
					});
					TrimRouteVariantStatesPreservingLanes(NextStates, MaxContractRouteVariantStates);
					States = MoveTemp(NextStates);
				}

				for (const FContractRouteVariantState& State : States)
				{
					FBusContractLayoutCandidate Candidate;
					Candidate.TrunkY = CandidateY;
					Candidate.Routes = State.Routes;
					Candidate.Cost = State.Cost + (CandidateY <= IdealY ? -0.001 : 0.0);
					UpdateBusCandidateBounds(Candidate, PendingBusRoutes, PendingIndices);
					Candidates.Add(MoveTemp(Candidate));
				}
			}

			Candidates.Sort([](const FBusContractLayoutCandidate& A, const FBusContractLayoutCandidate& B)
			{
				if (A.HardHitCount != B.HardHitCount)
				{
					return A.HardHitCount < B.HardHitCount;
				}
				if (!FMath::IsNearlyEqual(A.Cost, B.Cost, 0.001))
				{
					return A.Cost < B.Cost;
				}
				return A.TrunkY < B.TrunkY;
			});
			TrimBusContractLayoutCandidatesPreservingLanes(Candidates, Options);
			return Candidates;
		}

		static void ApplyBusContractLayoutCandidate(
			const FBusContractLayoutCandidate& Candidate,
			const TArray<int32>& PendingIndices,
			TArray<FPendingCorridorBusRoute>& PendingBusRoutes,
			FBusContract& Contract)
		{
			Contract.TrunkY = Candidate.TrunkY;
			Contract.XMin = Candidate.XMin;
			Contract.XMax = Candidate.XMax;
			Contract.BranchYMin = Candidate.BranchYMin;
			Contract.BranchYMax = Candidate.BranchYMax;
			Contract.OutputToTrunkDistanceY = Candidate.OutputToTrunkDistanceY;
			Contract.RouteKind = Candidate.HardHitCount > 0 ? ERouteKind::ExternalBus : ERouteKind::LocalBus;
			Contract.BranchXs.Reset();
			Contract.EdgeIndices.Reset();
			for (int32 LocalIndex = 0; LocalIndex < PendingIndices.Num(); ++LocalIndex)
			{
				if (!PendingBusRoutes.IsValidIndex(PendingIndices[LocalIndex]) || !Candidate.Routes.IsValidIndex(LocalIndex))
				{
					continue;
				}
				FPendingCorridorBusRoute& Pending = PendingBusRoutes[PendingIndices[LocalIndex]];
				const FSharedSourceBusPendingRoute& Route = Candidate.Routes[LocalIndex];
					Pending.Planned.Polyline = Route.Polyline;
					Pending.Planned.TrunkY = Candidate.TrunkY;
					Pending.Planned.BranchX = Route.BranchX;
					Pending.Planned.bCrossesHardOccupancy = Route.HardHitCount > 0;
					Pending.Planned.RouteKind = Route.HardHitCount > 0 ? ERouteKind::ExternalBus : ERouteKind::LocalBus;
					Pending.Planned.bEndsAtBoundaryEntry = false;
					Contract.BranchXs.Add(Route.BranchX);
					Contract.EdgeIndices.AddUnique(Pending.EdgeIndex);
				}
			}

		struct FReservedCrossBusBranchItem
		{
			int32 SetIndex = INDEX_NONE;
			int32 PendingIndex = INDEX_NONE;
			int32 EdgeIndex = INDEX_NONE;
			FString SourceParamId;
			double LeftBoundX = 0.0;
			double RightBoundX = 0.0;
			double TrunkY = 0.0;
			double YMin = 0.0;
			double YMax = 0.0;
			double OutputToTrunkDistanceY = 0.0;
			double TargetOrderY = 0.0;
		};

		static bool AddReservedLaneBusContractCandidatesForGroup(
			TArray<FBusContractCandidateSet>& CandidateSets,
			const TArray<int32>& Group,
			const TArray<FPendingCorridorBusRoute>& PendingBusRoutes,
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const FOptions& Options,
			FRunResult* Result = nullptr)
		{
			if (Group.Num() <= 1)
			{
				return false;
			}

			const double CompactTrunkLaneSpacing = GetCompactBusTrunkLaneSpacingForGroup(
				CandidateSets,
				Group,
				PendingBusRoutes,
				Nodes,
				Edges,
				Options);
			const double TrunkLaneSpacing = FMath::Max(FMath::Max(Options.NodePadding * 2.0, 64.0), CompactTrunkLaneSpacing);
			const double BranchLaneSpacing = GetCrossBusBranchLaneSpacing(Options);
			TArray<int32> OrderedSets = Group;
			TMap<int32, double> PreferredTrunkYBySet;
			TMap<int32, double> SourceOrderYBySet;
			for (const int32 SetIndex : OrderedSets)
			{
				double SumY = 0.0;
				int32 CountY = 0;
				double SourceYSum = 0.0;
				int32 SourceYCount = 0;
				if (CandidateSets.IsValidIndex(SetIndex))
				{
					if (CandidateSets[SetIndex].Candidates.Num() > 0)
					{
						SumY += CandidateSets[SetIndex].Candidates[0].TrunkY;
						++CountY;
					}
					if (CountY == 0)
					{
						for (const int32 PendingIndex : CandidateSets[SetIndex].PendingIndices)
						{
							if (PendingBusRoutes.IsValidIndex(PendingIndex))
							{
								SumY += PendingBusRoutes[PendingIndex].Planned.TrunkY;
								++CountY;
							}
						}
					}
					for (const int32 PendingIndex : CandidateSets[SetIndex].PendingIndices)
					{
						if (PendingBusRoutes.IsValidIndex(PendingIndex))
						{
							SourceYSum += PendingBusRoutes[PendingIndex].Start.Y;
							++SourceYCount;
						}
					}
				}
				PreferredTrunkYBySet.Add(SetIndex, CountY > 0 ? SumY / static_cast<double>(CountY) : 0.0);
				SourceOrderYBySet.Add(SetIndex, SourceYCount > 0 ? SourceYSum / static_cast<double>(SourceYCount) : (CountY > 0 ? SumY / static_cast<double>(CountY) : 0.0));
			}
			OrderedSets.Sort([&CandidateSets, &PreferredTrunkYBySet, &SourceOrderYBySet](const int32 A, const int32 B)
			{
				const double* SourceYA = SourceOrderYBySet.Find(A);
				const double* SourceYB = SourceOrderYBySet.Find(B);
				const double LeftSourceY = SourceYA ? *SourceYA : 0.0;
				const double RightSourceY = SourceYB ? *SourceYB : 0.0;
				if (!FMath::IsNearlyEqual(LeftSourceY, RightSourceY, 0.5))
				{
					return LeftSourceY < RightSourceY;
				}
				const double* YA = PreferredTrunkYBySet.Find(A);
				const double* YB = PreferredTrunkYBySet.Find(B);
				const double LeftY = YA ? *YA : 0.0;
				const double RightY = YB ? *YB : 0.0;
				if (!FMath::IsNearlyEqual(LeftY, RightY, 0.5))
				{
					return LeftY < RightY;
				}
				const int32 LeftContract = CandidateSets.IsValidIndex(A) ? CandidateSets[A].ContractIndex : A;
				const int32 RightContract = CandidateSets.IsValidIndex(B) ? CandidateSets[B].ContractIndex : B;
				return LeftContract < RightContract;
			});

			TMap<int32, TArray<double>> ReservedTrunkYsBySet;
			double PreviousY = -TNumericLimits<double>::Max();
			for (const int32 SetIndex : OrderedSets)
			{
				const double* PreferredY = PreferredTrunkYBySet.Find(SetIndex);
				double ReservedY = PreferredY ? *PreferredY : 0.0;
				if (PreviousY > -TNumericLimits<double>::Max() * 0.5)
				{
					ReservedY = FMath::Max(ReservedY, PreviousY + TrunkLaneSpacing);
				}
				ReservedY = FMath::RoundToDouble(ReservedY);
				TArray<double> CandidateYs;
				constexpr int32 MaxReservedStandardLaneOffsets = 4;
				constexpr int32 MaxReservedCompactLaneOffsets = 8;
				for (int32 LaneOffset = 0; LaneOffset <= MaxReservedStandardLaneOffsets; ++LaneOffset)
				{
					AddUniqueDouble(CandidateYs, ReservedY + static_cast<double>(LaneOffset) * TrunkLaneSpacing);
					if (LaneOffset > 0)
					{
						AddUniqueDouble(CandidateYs, ReservedY - static_cast<double>(LaneOffset) * TrunkLaneSpacing);
					}
				}
				for (int32 LaneOffset = 1; LaneOffset <= MaxReservedCompactLaneOffsets; ++LaneOffset)
				{
					AddUniqueDouble(CandidateYs, ReservedY + static_cast<double>(LaneOffset) * CompactTrunkLaneSpacing);
					AddUniqueDouble(CandidateYs, ReservedY - static_cast<double>(LaneOffset) * CompactTrunkLaneSpacing);
				}
				ReservedTrunkYsBySet.Add(SetIndex, MoveTemp(CandidateYs));
				PreviousY = ReservedY;
			}

			TArray<FReservedCrossBusBranchItem> BranchItems;
			for (const int32 SetIndex : Group)
			{
				if (!CandidateSets.IsValidIndex(SetIndex))
				{
					continue;
				}
				const TArray<double>* ReservedTrunkYs = ReservedTrunkYsBySet.Find(SetIndex);
				if (!ReservedTrunkYs || ReservedTrunkYs->Num() == 0)
				{
					continue;
				}
				const double PrimaryReservedTrunkY = (*ReservedTrunkYs)[0];
				for (const int32 PendingIndex : CandidateSets[SetIndex].PendingIndices)
				{
					if (!PendingBusRoutes.IsValidIndex(PendingIndex))
					{
						continue;
					}
					const FPendingCorridorBusRoute& Pending = PendingBusRoutes[PendingIndex];
					const double RightBoundX = ComputeOutputXAlignedToInput(Pending.End.X, Options);
					const double LeftBoundX = FMath::Max(
						ComputeInputXAlignedToOutput(Pending.Start.X, Options),
						RightBoundX - GetCrossBusBranchSetbackLimitX(Options));
					if (LeftBoundX > RightBoundX + 0.5)
					{
						continue;
					}

					FReservedCrossBusBranchItem Item;
					Item.SetIndex = SetIndex;
					Item.PendingIndex = PendingIndex;
					Item.EdgeIndex = Pending.EdgeIndex;
					Item.SourceParamId = Pending.SourceParamId;
					Item.LeftBoundX = LeftBoundX;
					Item.RightBoundX = RightBoundX;
					Item.TrunkY = PrimaryReservedTrunkY;
					Item.YMin = FMath::Min(PrimaryReservedTrunkY, Pending.End.Y);
					Item.YMax = FMath::Max(PrimaryReservedTrunkY, Pending.End.Y);
					Item.OutputToTrunkDistanceY = FMath::Abs(Pending.Start.Y - PrimaryReservedTrunkY);
					Item.TargetOrderY = Pending.End.Y;
					BranchItems.Add(Item);
				}
			}

			TMap<int32, double> ReservedBranchXByPending;
			for (const FReservedCrossBusBranchItem& Item : BranchItems)
			{
				ReservedBranchXByPending.Add(Item.PendingIndex, Item.RightBoundX);
			}
			if (BranchItems.Num() > 1)
			{
				TArray<int32> Parent;
				Parent.SetNum(BranchItems.Num());
				for (int32 Index = 0; Index < Parent.Num(); ++Index)
				{
					Parent[Index] = Index;
				}
				TFunction<int32(int32)> FindRoot = [&Parent, &FindRoot](const int32 Index) -> int32
				{
					if (Parent[Index] != Index)
					{
						Parent[Index] = FindRoot(Parent[Index]);
					}
					return Parent[Index];
				};
				auto UnionItems = [&Parent, &FindRoot](const int32 A, const int32 B)
				{
					const int32 RootA = FindRoot(A);
					const int32 RootB = FindRoot(B);
					if (RootA != RootB)
					{
						Parent[RootB] = RootA;
					}
				};

				for (int32 A = 0; A < BranchItems.Num(); ++A)
				{
					for (int32 B = A + 1; B < BranchItems.Num(); ++B)
					{
						if (BranchItems[A].SourceParamId == BranchItems[B].SourceParamId)
						{
							continue;
						}
						if (!IntervalOverlaps(BranchItems[A].YMin, BranchItems[A].YMax, BranchItems[B].YMin, BranchItems[B].YMax))
						{
							continue;
						}
						if (FMath::Abs(BranchItems[A].RightBoundX - BranchItems[B].RightBoundX) < BranchLaneSpacing + 0.5)
						{
							UnionItems(A, B);
						}
					}
				}

				TMap<int32, TArray<int32>> BranchGroupsByRoot;
				for (int32 ItemIndex = 0; ItemIndex < BranchItems.Num(); ++ItemIndex)
				{
					BranchGroupsByRoot.FindOrAdd(FindRoot(ItemIndex)).Add(ItemIndex);
				}
				for (TPair<int32, TArray<int32>>& Pair : BranchGroupsByRoot)
				{
					TArray<int32>& BranchGroup = Pair.Value;
					if (BranchGroup.Num() <= 1)
					{
						continue;
					}
					BranchGroup.Sort([&BranchItems](const int32 A, const int32 B)
					{
						const FReservedCrossBusBranchItem& Left = BranchItems[A];
						const FReservedCrossBusBranchItem& Right = BranchItems[B];
						if (!FMath::IsNearlyEqual(Left.OutputToTrunkDistanceY, Right.OutputToTrunkDistanceY, 0.5))
						{
							return Left.OutputToTrunkDistanceY < Right.OutputToTrunkDistanceY;
						}
						if (!FMath::IsNearlyEqual(Left.TargetOrderY, Right.TargetOrderY, 0.5))
						{
							return Left.TargetOrderY < Right.TargetOrderY;
						}
						return Left.EdgeIndex < Right.EdgeIndex;
					});

					for (int32 LaneIndex = 0; LaneIndex < BranchGroup.Num(); ++LaneIndex)
					{
						const FReservedCrossBusBranchItem& Item = BranchItems[BranchGroup[LaneIndex]];
						const double DesiredX = Item.RightBoundX - static_cast<double>(LaneIndex) * BranchLaneSpacing;
						ReservedBranchXByPending.Add(Item.PendingIndex, FMath::Clamp(DesiredX, Item.LeftBoundX, Item.RightBoundX));
					}
				}
			}

			bool bAddedAny = false;
			for (const int32 SetIndex : Group)
			{
				if (!CandidateSets.IsValidIndex(SetIndex))
				{
					continue;
				}
				const TArray<double>* ReservedTrunkYs = ReservedTrunkYsBySet.Find(SetIndex);
				if (!ReservedTrunkYs || ReservedTrunkYs->Num() == 0)
				{
					continue;
				}
				const double PrimaryReservedTrunkY = (*ReservedTrunkYs)[0];

				for (const double ReservedTrunkY : *ReservedTrunkYs)
				{
					FBusContractLayoutCandidate Candidate;
					Candidate.TrunkY = ReservedTrunkY;
					bool bValidCandidate = true;
					for (const int32 PendingIndex : CandidateSets[SetIndex].PendingIndices)
					{
						if (!PendingBusRoutes.IsValidIndex(PendingIndex))
						{
							bValidCandidate = false;
							break;
						}
						const FPendingCorridorBusRoute& Pending = PendingBusRoutes[PendingIndex];
						const double* ReservedBranchX = ReservedBranchXByPending.Find(PendingIndex);
						if (!ReservedBranchX)
						{
							bValidCandidate = false;
							break;
						}

						TArray<FRect> RouteHardRects;
						if (Edges.IsValidIndex(Pending.EdgeIndex))
						{
							RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[Pending.EdgeIndex]);
						}
						TArray<TArray<FVector2D>> RoutePolylines;
						const bool bRequiresTargetDogleg = !FMath::IsNearlyEqual(ReservedTrunkY, Pending.End.Y, 0.5)
							&& ComputeOutputXAlignedToInput(Pending.End.X, Options) - *ReservedBranchX >= EndpointAccessClearanceX(Options) - 0.5;
						if (!bRequiresTargetDogleg)
						{
							RoutePolylines.Add(BuildSharedSourceBusRoutePolyline(Pending.Start, Pending.End, ReservedTrunkY, *ReservedBranchX, Options));
						}
						RoutePolylines.Add(BuildSharedSourceBusRoutePolylineWithTargetDogleg(Pending.Start, Pending.End, ReservedTrunkY, *ReservedBranchX, Options));
						if (!bRequiresTargetDogleg)
						{
							RoutePolylines.Add(BuildSharedSourceBusRoutePolylineAvoidingHardRects(Pending.Start, Pending.End, ReservedTrunkY, *ReservedBranchX, RouteHardRects, Options, false));
						}
						RoutePolylines.Add(BuildSharedSourceBusRoutePolylineAvoidingHardRects(Pending.Start, Pending.End, ReservedTrunkY, *ReservedBranchX, RouteHardRects, Options, true));

						FSharedSourceBusPendingRoute BestRoute;
						bool bHasRoute = false;
						for (const TArray<FVector2D>& RoutePolyline : RoutePolylines)
						{
							if (RoutePolyline.Num() < 2 || !HasNonNegativeConnectionAlignmentShortenForEverySegment(RoutePolyline, Options))
							{
								continue;
							}
							const int32 HardHitCount = CountControlWireHardOccupancyHits(RoutePolyline, RouteHardRects);
							const double RouteCost = static_cast<double>(HardHitCount) * 1000000.0
								+ PolylineManhattanLength(RoutePolyline)
								+ FMath::Abs(Pending.Planned.TrunkY - ReservedTrunkY) * 10.0
								+ FMath::Abs(ComputeOutputXAlignedToInput(Pending.End.X, Options) - *ReservedBranchX) * 0.1;
							if (!bHasRoute
								|| HardHitCount < BestRoute.HardHitCount
								|| (HardHitCount == BestRoute.HardHitCount && RouteCost < BestRoute.Cost))
							{
								BestRoute.Polyline = RoutePolyline;
								BestRoute.BranchX = *ReservedBranchX;
								BestRoute.IdealBranchX = ComputeOutputXAlignedToInput(Pending.End.X, Options);
								BestRoute.RightBoundX = ComputeOutputXAlignedToInput(Pending.End.X, Options);
								BestRoute.LeftBoundX = FMath::Max(
									ComputeInputXAlignedToOutput(Pending.Start.X, Options),
									BestRoute.RightBoundX - GetCrossBusBranchSetbackLimitX(Options));
								BestRoute.BranchYMin = FMath::Min(ReservedTrunkY, Pending.End.Y);
								BestRoute.BranchYMax = FMath::Max(ReservedTrunkY, Pending.End.Y);
								BestRoute.OutputToTrunkDistanceY = FMath::Abs(Pending.Start.Y - ReservedTrunkY);
								BestRoute.HardHitCount = HardHitCount;
								BestRoute.Cost = RouteCost;
								bHasRoute = true;
							}
						}
						if (!bHasRoute)
						{
							bValidCandidate = false;
							break;
						}
						Candidate.Routes.Add(MoveTemp(BestRoute));
					}

					if (!bValidCandidate || Candidate.Routes.Num() != CandidateSets[SetIndex].PendingIndices.Num())
					{
						continue;
					}
					Candidate.Cost = 0.0;
					for (const FSharedSourceBusPendingRoute& Route : Candidate.Routes)
					{
						Candidate.Cost += Route.Cost;
					}
					Candidate.Cost += FMath::Abs(ReservedTrunkY - PrimaryReservedTrunkY) * 10000.0;
					UpdateBusCandidateBounds(Candidate, PendingBusRoutes, CandidateSets[SetIndex].PendingIndices);
					if (Result)
					{
						TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
						Obj->SetStringField(TEXT("kind"), TEXT("reserved_lane_candidate"));
						Obj->SetNumberField(TEXT("set_index"), SetIndex);
						Obj->SetNumberField(TEXT("contract_index"), CandidateSets[SetIndex].ContractIndex);
						Obj->SetNumberField(TEXT("trunk_y"), Candidate.TrunkY);
						Obj->SetNumberField(TEXT("hard_hit_count"), Candidate.HardHitCount);
						TArray<TSharedPtr<FJsonValue>> BranchValues;
						for (const FSharedSourceBusPendingRoute& Route : Candidate.Routes)
						{
							BranchValues.Add(MakeShared<FJsonValueNumber>(Route.BranchX));
						}
						Obj->SetArrayField(TEXT("branch_x"), BranchValues);
						Result->RejectionDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
					}
					CandidateSets[SetIndex].Candidates.Insert(MoveTemp(Candidate), 0);
					bAddedAny = true;
				}
			}
			for (const int32 SetIndex : Group)
			{
				if (!CandidateSets.IsValidIndex(SetIndex))
				{
					continue;
				}
				CandidateSets[SetIndex].Candidates.Sort([](const FBusContractLayoutCandidate& A, const FBusContractLayoutCandidate& B)
				{
					if (A.HardHitCount != B.HardHitCount)
					{
						return A.HardHitCount < B.HardHitCount;
					}
					if (!FMath::IsNearlyEqual(A.Cost, B.Cost, 0.001))
					{
						return A.Cost < B.Cost;
					}
					return A.TrunkY < B.TrunkY;
				});
				TrimBusContractLayoutCandidatesPreservingLanes(CandidateSets[SetIndex].Candidates, Options);
			}
			return bAddedAny;
		}

		struct FGenerationCrossBusBranchLaneItem
		{
			int32 PendingIndex = INDEX_NONE;
			int32 ContractIndex = INDEX_NONE;
			int32 EdgeIndex = INDEX_NONE;
			int32 ConnectionGroupId = INDEX_NONE;
			FString SourceParamId;
			double SourceOrderKey = 0.0;
			double TargetOrderY = 0.0;
			double CurrentBranchX = 0.0;
			double DesiredBranchX = 0.0;
			double LeftBoundX = 0.0;
			double RightBoundX = 0.0;
			double YMin = 0.0;
			double YMax = 0.0;
			double OutputToTrunkDistanceY = 0.0;
		};

		static bool TryBuildGenerationBranchLaneRoute(
			const FPendingCorridorBusRoute& Pending,
			const double TrunkY,
			const double BranchX,
			const TArray<FRect>& RouteHardRects,
			const FOptions& Options,
			FSharedSourceBusPendingRoute& OutRoute)
		{
			TArray<TArray<FVector2D>> CandidateRoutes;
			const double EntryX = ComputeInputXAlignedToOutput(Pending.Start.X, Options);
			const double ExitX = ComputeOutputXAlignedToInput(Pending.End.X, Options);
			const double ClampedBranchX = FMath::Clamp(BranchX, EntryX, ExitX);
			const bool bRequiresTargetDogleg = !FMath::IsNearlyEqual(TrunkY, Pending.End.Y, 0.5)
				&& ExitX - ClampedBranchX >= EndpointAccessClearanceX(Options) - 0.5;
			CandidateRoutes.Add(BuildSharedSourceBusRoutePolyline(Pending.Start, Pending.End, TrunkY, ClampedBranchX, Options));
			CandidateRoutes.Add(BuildSharedSourceBusRoutePolylineWithTargetDogleg(Pending.Start, Pending.End, TrunkY, ClampedBranchX, Options));
			if (!bRequiresTargetDogleg)
			{
				CandidateRoutes.Add(BuildSharedSourceBusRoutePolylineAvoidingHardRects(Pending.Start, Pending.End, TrunkY, ClampedBranchX, RouteHardRects, Options, false));
			}
			CandidateRoutes.Add(BuildSharedSourceBusRoutePolylineAvoidingHardRects(Pending.Start, Pending.End, TrunkY, ClampedBranchX, RouteHardRects, Options, true));

			bool bFound = false;
			FSharedSourceBusPendingRoute BestRoute;
			for (const TArray<FVector2D>& CandidateRoute : CandidateRoutes)
			{
				if (CandidateRoute.Num() < 2 || !HasNonNegativeConnectionAlignmentShortenForEverySegment(CandidateRoute, Options))
				{
					continue;
				}
				const int32 HardHitCount = CountControlWireHardOccupancyHits(CandidateRoute, RouteHardRects);
				if (HardHitCount > 0)
				{
					continue;
				}
				const double CandidateCost = PolylineManhattanLength(CandidateRoute)
					+ FMath::Abs(ExitX - ClampedBranchX) * 0.02
					+ static_cast<double>(FMath::Max(0, CandidateRoute.Num() - 4)) * 4.0;
				if (!bFound || CandidateCost < BestRoute.Cost)
				{
					BestRoute.Polyline = CandidateRoute;
					BestRoute.BranchX = ClampedBranchX;
					BestRoute.IdealBranchX = ExitX;
					BestRoute.LeftBoundX = EntryX;
					BestRoute.RightBoundX = ExitX;
					BestRoute.BranchYMin = FMath::Min(TrunkY, Pending.End.Y);
					BestRoute.BranchYMax = FMath::Max(TrunkY, Pending.End.Y);
					BestRoute.OutputToTrunkDistanceY = FMath::Abs(Pending.Start.Y - TrunkY);
					BestRoute.HardHitCount = 0;
					BestRoute.Cost = CandidateCost;
					bFound = true;
				}
			}
			if (!bFound)
			{
				return false;
			}
			OutRoute = MoveTemp(BestRoute);
			return true;
		}

		static bool FindRightmostGenerationBranchLaneRoute(
			const FPendingCorridorBusRoute& Pending,
			const double TrunkY,
			const double MaxBranchX,
			const double LaneSpacing,
			const int32 LaneSearchCount,
			const TArray<FRect>& RouteHardRects,
			const FOptions& Options,
			FSharedSourceBusPendingRoute& OutRoute)
		{
			const double EntryX = ComputeInputXAlignedToOutput(Pending.Start.X, Options);
			const double ExitX = ComputeOutputXAlignedToInput(Pending.End.X, Options);
			if (EntryX > ExitX + 0.5)
			{
				return false;
			}

			const double RightLimit = FMath::Clamp(MaxBranchX, EntryX, ExitX);
			TArray<double> CandidateXs;
			auto AddCandidateX = [&CandidateXs, EntryX, RightLimit](const double Value)
			{
				if (!FMath::IsFinite(Value) || Value < EntryX - 0.5 || Value > RightLimit + 0.5)
				{
					return;
				}
				AddUniqueDouble(CandidateXs, FMath::Clamp(Value, EntryX, RightLimit));
			};

			AddCandidateX(RightLimit);
			AddCandidateX(Pending.Planned.BranchX);
			AddCandidateX(ExitX);
			AddCandidateX(ExitX - EndpointAccessClearanceX(Options));
			AddCandidateX(EntryX + EndpointAccessClearanceX(Options));
			AddCandidateX(EntryX);

			const int32 SearchCount = FMath::Max(8, LaneSearchCount + 8);
			const double EffectiveLaneSpacing = LaneSpacing > 0.0 ? LaneSpacing : GetCrossBusBranchLaneSpacing(Options);
			for (int32 Step = 0; Step <= SearchCount; ++Step)
			{
				const double Offset = static_cast<double>(Step) * EffectiveLaneSpacing;
				AddCandidateX(RightLimit - Offset);
				AddCandidateX(ExitX - Offset);
				AddCandidateX(ExitX - EndpointAccessClearanceX(Options) - Offset);
			}

			CandidateXs.Sort([](const double A, const double B)
			{
				return A > B;
			});

			for (const double CandidateX : CandidateXs)
			{
				FSharedSourceBusPendingRoute CandidateRoute;
				if (TryBuildGenerationBranchLaneRoute(Pending, TrunkY, CandidateX, RouteHardRects, Options, CandidateRoute))
				{
					OutRoute = MoveTemp(CandidateRoute);
					return true;
				}
			}
			return false;
		}

		static int32 ApplyCrossBusBranchXLanesAtGeneration(
			TArray<FPendingCorridorBusRoute>& PendingBusRoutes,
			TArray<FBusContract>& BusContracts,
			const TArray<FSelectedEndpointCandidate>& SelectedEndpoints,
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const FOptions& Options,
			FRunResult& Result)
		{
			TArray<FGenerationCrossBusBranchLaneItem> Items;
			for (int32 PendingIndex = 0; PendingIndex < PendingBusRoutes.Num(); ++PendingIndex)
			{
				const FPendingCorridorBusRoute& Pending = PendingBusRoutes[PendingIndex];
				if (!BusContracts.IsValidIndex(Pending.ContractIndex))
				{
					continue;
				}
				const int32 BranchPointIndex = GetTargetBranchPointIndex(Pending.Planned);
				if (!Pending.Planned.Polyline.IsValidIndex(BranchPointIndex))
				{
					continue;
				}
				const double EntryX = ComputeInputXAlignedToOutput(Pending.Start.X, Options);
				const double RightBoundX = ComputeOutputXAlignedToInput(Pending.End.X, Options);
				if (EntryX > RightBoundX + 0.5)
				{
					continue;
				}

				FGenerationCrossBusBranchLaneItem Item;
				Item.PendingIndex = PendingIndex;
				Item.ContractIndex = Pending.ContractIndex;
				Item.EdgeIndex = Pending.EdgeIndex;
				Item.SourceParamId = Pending.SourceParamId;
				Item.LeftBoundX = EntryX;
				Item.RightBoundX = RightBoundX;
				Item.CurrentBranchX = Pending.Planned.Polyline[BranchPointIndex].X;
				Item.DesiredBranchX = Item.CurrentBranchX;
				Item.YMin = FMath::Min(Pending.Planned.TrunkY, Pending.End.Y);
				Item.YMax = FMath::Max(Pending.Planned.TrunkY, Pending.End.Y);
				Item.OutputToTrunkDistanceY = FMath::Abs(Pending.Start.Y - Pending.Planned.TrunkY);
				Item.TargetOrderY = Pending.End.Y;
				if (SelectedEndpoints.IsValidIndex(Pending.SelectedEndpointIndex))
				{
					Item.ConnectionGroupId = SelectedEndpoints[Pending.SelectedEndpointIndex].ConnectionGroupId;
					Item.SourceOrderKey = SelectedEndpoints[Pending.SelectedEndpointIndex].SourcePortOrderKey;
				}
				Items.Add(Item);
			}

			if (Items.Num() <= 1)
			{
				return 0;
			}

			TArray<int32> Parent;
			Parent.SetNum(Items.Num());
			for (int32 Index = 0; Index < Parent.Num(); ++Index)
			{
				Parent[Index] = Index;
			}
			TFunction<int32(int32)> FindRoot = [&Parent, &FindRoot](const int32 Index) -> int32
			{
				if (Parent[Index] != Index)
				{
					Parent[Index] = FindRoot(Parent[Index]);
				}
				return Parent[Index];
			};
			auto UnionItems = [&Parent, &FindRoot](const int32 A, const int32 B)
			{
				const int32 RootA = FindRoot(A);
				const int32 RootB = FindRoot(B);
				if (RootA != RootB)
				{
					Parent[RootB] = RootA;
				}
			};

			for (int32 A = 0; A < Items.Num(); ++A)
			{
				for (int32 B = A + 1; B < Items.Num(); ++B)
				{
					if (Items[A].SourceParamId == Items[B].SourceParamId)
					{
						continue;
					}
					if (!IntervalOverlaps(Items[A].YMin, Items[A].YMax, Items[B].YMin, Items[B].YMax))
					{
						continue;
					}
					if (FMath::Abs(Items[A].CurrentBranchX - Items[B].CurrentBranchX) < GetCrossBusBranchLaneSpacing(Options) - 0.5)
					{
						UnionItems(A, B);
					}
				}
			}

			TMap<int32, TArray<int32>> GroupsByRoot;
			for (int32 ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
			{
				GroupsByRoot.FindOrAdd(FindRoot(ItemIndex)).Add(ItemIndex);
			}

			int32 AppliedCount = 0;
			TSet<int32> DirtyContracts;
			for (TPair<int32, TArray<int32>>& Pair : GroupsByRoot)
			{
				TArray<int32>& Group = Pair.Value;
				if (Group.Num() <= 1)
				{
					continue;
				}

				TArray<int32> PendingIndicesForSpacing;
				for (const int32 ItemIndex : Group)
				{
					if (Items.IsValidIndex(ItemIndex))
					{
						PendingIndicesForSpacing.Add(Items[ItemIndex].PendingIndex);
					}
				}
				const double LaneSpacing = EstimateInterfaceRowSpacingForPendingRouteIndices(
					PendingIndicesForSpacing,
					PendingBusRoutes,
					Nodes,
					Edges,
					Options);

				Group.Sort([&Items](const int32 A, const int32 B)
				{
					const FGenerationCrossBusBranchLaneItem& Left = Items[A];
					const FGenerationCrossBusBranchLaneItem& Right = Items[B];
					if (!FMath::IsNearlyEqual(Left.OutputToTrunkDistanceY, Right.OutputToTrunkDistanceY, 0.5))
					{
						return Left.OutputToTrunkDistanceY < Right.OutputToTrunkDistanceY;
					}
					if (Left.ConnectionGroupId != Right.ConnectionGroupId)
					{
						return Left.ConnectionGroupId < Right.ConnectionGroupId;
					}
					if (!FMath::IsNearlyEqual(Left.SourceOrderKey, Right.SourceOrderKey, 0.5))
					{
						return Left.SourceOrderKey < Right.SourceOrderKey;
					}
					if (!FMath::IsNearlyEqual(Left.TargetOrderY, Right.TargetOrderY, 0.5))
					{
						return Left.TargetOrderY < Right.TargetOrderY;
					}
					return Left.EdgeIndex < Right.EdgeIndex;
				});

				bool bGroupValid = true;
				bool bGroupChanged = false;
				double PreviousBranchX = TNumericLimits<double>::Max();
				TArray<FSharedSourceBusPendingRoute> ReplacementRoutes;
				ReplacementRoutes.SetNum(Group.Num());
				for (int32 LaneIndex = 0; LaneIndex < Group.Num(); ++LaneIndex)
				{
					FGenerationCrossBusBranchLaneItem& Item = Items[Group[LaneIndex]];
					if (!PendingBusRoutes.IsValidIndex(Item.PendingIndex))
					{
						bGroupValid = false;
						break;
					}
					const FPendingCorridorBusRoute& Pending = PendingBusRoutes[Item.PendingIndex];
					const double MaxLaneX = LaneIndex == 0
						? Item.RightBoundX
						: FMath::Min(Item.RightBoundX, PreviousBranchX - LaneSpacing);
					if (MaxLaneX < Item.LeftBoundX - 0.5)
					{
						bGroupValid = false;
						TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
						Obj->SetStringField(TEXT("kind"), TEXT("cross_bus_branch_x_lane_candidate_rejected"));
						Obj->SetNumberField(TEXT("edge_index"), Item.EdgeIndex);
						Obj->SetNumberField(TEXT("current_branch_x"), Item.CurrentBranchX);
						Obj->SetNumberField(TEXT("desired_branch_x"), MaxLaneX);
						Obj->SetNumberField(TEXT("lane_spacing"), LaneSpacing);
						Obj->SetStringField(TEXT("reason"), TEXT("no_legal_x_interval"));
						Result.RejectionDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
						break;
					}

					TArray<FRect> RouteHardRects;
					if (Edges.IsValidIndex(Pending.EdgeIndex))
					{
						RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[Pending.EdgeIndex]);
					}
					if (!FindRightmostGenerationBranchLaneRoute(
						Pending,
						Pending.Planned.TrunkY,
						MaxLaneX,
						LaneSpacing,
						Group.Num(),
						RouteHardRects,
						Options,
						ReplacementRoutes[LaneIndex]))
					{
						bGroupValid = false;
						TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
						Obj->SetStringField(TEXT("kind"), TEXT("cross_bus_branch_x_lane_candidate_rejected"));
						Obj->SetNumberField(TEXT("edge_index"), Item.EdgeIndex);
						Obj->SetNumberField(TEXT("current_branch_x"), Item.CurrentBranchX);
						Obj->SetNumberField(TEXT("desired_branch_x"), MaxLaneX);
						Obj->SetNumberField(TEXT("lane_spacing"), LaneSpacing);
						Obj->SetStringField(TEXT("reason"), TEXT("no_hard_free_route_at_or_left_of_max_x"));
						Result.RejectionDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
						break;
					}
					Item.DesiredBranchX = ReplacementRoutes[LaneIndex].BranchX;
					PreviousBranchX = Item.DesiredBranchX;
					bGroupChanged = bGroupChanged
						|| !FMath::IsNearlyEqual(Item.CurrentBranchX, Item.DesiredBranchX, 0.5);
				}
				if (!bGroupValid)
				{
					continue;
				}
				if (!bGroupChanged)
				{
					continue;
				}

				for (int32 LaneIndex = 0; LaneIndex < Group.Num(); ++LaneIndex)
				{
					const FGenerationCrossBusBranchLaneItem& Item = Items[Group[LaneIndex]];
					if (!PendingBusRoutes.IsValidIndex(Item.PendingIndex))
					{
						continue;
					}
					FPendingCorridorBusRoute& Pending = PendingBusRoutes[Item.PendingIndex];
					const FSharedSourceBusPendingRoute& Route = ReplacementRoutes[LaneIndex];
					Pending.Planned.Polyline = Route.Polyline;
					Pending.Planned.BranchX = Route.BranchX;
					Pending.Planned.bCrossesHardOccupancy = false;
					Pending.Planned.RouteKind = ERouteKind::LocalBus;
					Pending.Planned.bEndsAtBoundaryEntry = false;
					DirtyContracts.Add(Pending.ContractIndex);
					if (!FMath::IsNearlyEqual(Item.CurrentBranchX, Route.BranchX, 0.5))
					{
						++AppliedCount;
					}

					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("kind"), TEXT("cross_bus_branch_x_lane_applied_at_generation"));
					Obj->SetNumberField(TEXT("edge_index"), Item.EdgeIndex);
					Obj->SetNumberField(TEXT("lane_index"), LaneIndex);
					Obj->SetNumberField(TEXT("old_branch_x"), Item.CurrentBranchX);
					Obj->SetNumberField(TEXT("branch_x"), Route.BranchX);
					Obj->SetNumberField(TEXT("lane_spacing"), LaneSpacing);
					Obj->SetNumberField(TEXT("branch_y_min"), Item.YMin);
					Obj->SetNumberField(TEXT("branch_y_max"), Item.YMax);
					Result.RejectionDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}

			for (const int32 ContractIndex : DirtyContracts)
			{
				if (BusContracts.IsValidIndex(ContractIndex))
				{
					RecomputeBusContractBoundsFromPendingRoutes(BusContracts[ContractIndex], PendingBusRoutes);
				}
			}
			return AppliedCount;
		}

		static bool BusContractCandidatesConflictOnTrunkY(
			const FBusContractLayoutCandidate& A,
			const FBusContractLayoutCandidate& B,
			const FOptions& Options,
			const double LaneSpacingOverride = -1.0)
		{
			const double LaneSpacing = LaneSpacingOverride > 0.0
				? LaneSpacingOverride
				: FMath::Max(Options.NodePadding * 2.0, 48.0);
			return IntervalOverlaps(A.XMin, A.XMax, B.XMin, B.XMax)
				&& FMath::Abs(A.TrunkY - B.TrunkY) < LaneSpacing - 0.5;
		}

		static bool BusContractCandidatesConflictOnBranchX(
			const FBusContractCandidateSet& ASet,
			const FBusContractLayoutCandidate& A,
			const FBusContractCandidateSet& BSet,
			const FBusContractLayoutCandidate& B,
			const FOptions& Options)
		{
			if (ASet.SourceParamId == BSet.SourceParamId)
			{
				return false;
			}

			const double LaneSpacing = GetCrossBusBranchLaneSpacing(Options);
			for (const FSharedSourceBusPendingRoute& LeftRoute : A.Routes)
			{
				for (const FSharedSourceBusPendingRoute& RightRoute : B.Routes)
				{
					if (!IntervalOverlaps(LeftRoute.BranchYMin, LeftRoute.BranchYMax, RightRoute.BranchYMin, RightRoute.BranchYMax))
					{
						continue;
					}
					if (FMath::Abs(LeftRoute.BranchX - RightRoute.BranchX) < LaneSpacing - 0.5)
					{
						return true;
					}
				}
			}
			return false;
		}

		static double BusContractBranchOrderPenalty(
			const FBusContractCandidateSet& ASet,
			const FBusContractLayoutCandidate& A,
			const FBusContractCandidateSet& BSet,
			const FBusContractLayoutCandidate& B)
		{
			if (ASet.SourceParamId == BSet.SourceParamId)
			{
				return 0.0;
			}

			double Penalty = 0.0;
			for (const FSharedSourceBusPendingRoute& LeftRoute : A.Routes)
			{
				for (const FSharedSourceBusPendingRoute& RightRoute : B.Routes)
				{
					if (!IntervalOverlaps(LeftRoute.BranchYMin, LeftRoute.BranchYMax, RightRoute.BranchYMin, RightRoute.BranchYMax))
					{
						continue;
					}
					if (LeftRoute.OutputToTrunkDistanceY + 0.5 < RightRoute.OutputToTrunkDistanceY
						&& LeftRoute.BranchX + 0.5 < RightRoute.BranchX)
					{
						Penalty += 500000.0 + (RightRoute.BranchX - LeftRoute.BranchX) * 100.0;
					}
					else if (RightRoute.OutputToTrunkDistanceY + 0.5 < LeftRoute.OutputToTrunkDistanceY
						&& RightRoute.BranchX + 0.5 < LeftRoute.BranchX)
					{
						Penalty += 500000.0 + (LeftRoute.BranchX - RightRoute.BranchX) * 100.0;
					}
				}
			}
			return Penalty;
		}

		struct FBusContractSelectionState
		{
			TArray<int32> Choices;
			double Cost = 0.0;
			int32 HardHitCount = 0;
			int32 LastChoiceCandidateIndex = INDEX_NONE;
		};

		static void TrimBusContractSelectionStatesPreservingCandidateChoices(
			TArray<FBusContractSelectionState>& States,
			const int32 MaxBeamStates)
		{
			if (States.Num() <= MaxBeamStates)
			{
				return;
			}

			constexpr int32 GlobalBestKeepCount = 4096;
			constexpr int32 BestPerCurrentCandidateKeepCount = 16;
			TArray<uint8> bKeep;
			bKeep.Init(0, States.Num());
			int32 KeepCount = 0;
			auto MarkKeep = [&bKeep, &KeepCount](const int32 Index)
			{
				if (!bKeep.IsValidIndex(Index) || bKeep[Index] != 0)
				{
					return;
				}
				bKeep[Index] = 1;
				++KeepCount;
			};

			for (int32 Index = 0; Index < FMath::Min(GlobalBestKeepCount, States.Num()); ++Index)
			{
				MarkKeep(Index);
			}

			TMap<int32, int32> CountByCurrentCandidate;
			for (int32 Index = 0; Index < States.Num(); ++Index)
			{
				const int32 CandidateIndex = States[Index].LastChoiceCandidateIndex;
				if (CandidateIndex == INDEX_NONE)
				{
					continue;
				}
				int32& CandidateCount = CountByCurrentCandidate.FindOrAdd(CandidateIndex);
				if (CandidateCount < BestPerCurrentCandidateKeepCount)
				{
					MarkKeep(Index);
					++CandidateCount;
				}
			}

			TArray<FBusContractSelectionState> KeptStates;
			KeptStates.Reserve(FMath::Min(KeepCount, MaxBeamStates));
			for (int32 Index = 0; Index < States.Num(); ++Index)
			{
				if (bKeep[Index] != 0)
				{
					KeptStates.Add(MoveTemp(States[Index]));
				}
			}
			if (KeptStates.Num() > MaxBeamStates)
			{
				KeptStates.SetNum(MaxBeamStates, EAllowShrinking::Yes);
			}
			States = MoveTemp(KeptStates);
		}

		static bool SelectBusContractCandidatesForGroup(
			const TArray<FBusContractCandidateSet>& CandidateSets,
			const TArray<int32>& Group,
			const FOptions& Options,
			const bool bAllowLaneConflicts,
			const bool bStrictZeroHardCandidateWhenAvailable,
			const double TrunkLaneSpacingOverride,
			TArray<int32>& OutChoiceBySetIndex)
		{
			if (Group.Num() == 0)
			{
				return false;
			}

			TArray<int32> OrderedGroup = Group;
			OrderedGroup.Sort([&CandidateSets](const int32 A, const int32 B)
			{
				const FBusContractCandidateSet& Left = CandidateSets[A];
				const FBusContractCandidateSet& Right = CandidateSets[B];
				const double LeftY = Left.Candidates.Num() > 0 ? Left.Candidates[0].TrunkY : 0.0;
				const double RightY = Right.Candidates.Num() > 0 ? Right.Candidates[0].TrunkY : 0.0;
				if (!FMath::IsNearlyEqual(LeftY, RightY, 0.5))
				{
					return LeftY < RightY;
				}
				return Left.ContractIndex < Right.ContractIndex;
			});

			TArray<uint8> bSetHasZeroHardCandidate;
			bSetHasZeroHardCandidate.Init(0, CandidateSets.Num());
			for (const int32 SetIndex : OrderedGroup)
			{
				if (!CandidateSets.IsValidIndex(SetIndex))
				{
					continue;
				}
				for (const FBusContractLayoutCandidate& Candidate : CandidateSets[SetIndex].Candidates)
				{
					if (Candidate.HardHitCount == 0)
					{
						bSetHasZeroHardCandidate[SetIndex] = 1;
						break;
					}
				}
			}

			TArray<FBusContractSelectionState> States;
			FBusContractSelectionState Initial;
			Initial.Choices.Init(INDEX_NONE, CandidateSets.Num());
			States.Add(Initial);
			constexpr int32 MaxBeamStates = 4096;
			for (const int32 SetIndex : OrderedGroup)
			{
				if (!CandidateSets.IsValidIndex(SetIndex) || CandidateSets[SetIndex].Candidates.Num() == 0)
				{
					return false;
				}
				TArray<FBusContractSelectionState> NextStates;
				const TArray<FBusContractLayoutCandidate>& Candidates = CandidateSets[SetIndex].Candidates;
				for (const FBusContractSelectionState& State : States)
				{
					for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
					{
						const FBusContractLayoutCandidate& Candidate = Candidates[CandidateIndex];
						if (bStrictZeroHardCandidateWhenAvailable
							&& bSetHasZeroHardCandidate.IsValidIndex(SetIndex)
							&& bSetHasZeroHardCandidate[SetIndex] != 0
							&& Candidate.HardHitCount > 0)
						{
							continue;
						}
						double ConflictPenalty = 0.0;
						bool bRejectedByConflict = false;
						for (const int32 PreviousSetIndex : OrderedGroup)
						{
							if (PreviousSetIndex == SetIndex)
							{
								break;
							}
							if (!State.Choices.IsValidIndex(PreviousSetIndex) || State.Choices[PreviousSetIndex] == INDEX_NONE)
							{
								continue;
							}
							const FBusContractLayoutCandidate& PreviousCandidate =
								CandidateSets[PreviousSetIndex].Candidates[State.Choices[PreviousSetIndex]];
							const bool bTrunkYConflict = BusContractCandidatesConflictOnTrunkY(Candidate, PreviousCandidate, Options, TrunkLaneSpacingOverride);
							const bool bBranchXConflict = BusContractCandidatesConflictOnBranchX(
								CandidateSets[SetIndex],
								Candidate,
								CandidateSets[PreviousSetIndex],
								PreviousCandidate,
								Options);
							if (bTrunkYConflict || bBranchXConflict)
							{
								if (!bAllowLaneConflicts)
								{
									bRejectedByConflict = true;
									break;
								}
								ConflictPenalty += 10000000.0;
							}
							ConflictPenalty += BusContractBranchOrderPenalty(
								CandidateSets[SetIndex],
								Candidate,
								CandidateSets[PreviousSetIndex],
								PreviousCandidate);
						}
						if (bRejectedByConflict)
						{
							continue;
						}
						FBusContractSelectionState Next = State;
						Next.Choices[SetIndex] = CandidateIndex;
						Next.HardHitCount += Candidate.HardHitCount;
						Next.Cost += static_cast<double>(Candidate.HardHitCount) * 1000000000.0
							+ ConflictPenalty
							+ Candidate.Cost
							+ Candidate.TrunkY * 0.000001;
						Next.LastChoiceCandidateIndex = CandidateIndex;
						NextStates.Add(MoveTemp(Next));
					}
				}
				if (NextStates.Num() == 0)
				{
					return false;
				}
				NextStates.Sort([](const FBusContractSelectionState& A, const FBusContractSelectionState& B)
				{
					if (A.HardHitCount != B.HardHitCount)
					{
						return A.HardHitCount < B.HardHitCount;
					}
					return A.Cost < B.Cost;
				});
				TrimBusContractSelectionStatesPreservingCandidateChoices(NextStates, MaxBeamStates);
				States = MoveTemp(NextStates);
			}
			if (States.Num() == 0)
			{
				return false;
			}
			OutChoiceBySetIndex = States[0].Choices;
			return true;
		}

		static bool SelectBusContractCandidatesForLargeGroupDeterministic(
			const TArray<FBusContractCandidateSet>& CandidateSets,
			const TArray<int32>& Group,
			const FOptions& Options,
			const double TrunkLaneSpacingOverride,
			TArray<int32>& OutChoiceBySetIndex)
		{
			OutChoiceBySetIndex.Init(INDEX_NONE, CandidateSets.Num());
			if (Group.Num() == 0)
			{
				return true;
			}

			TArray<int32> OrderedGroup = Group;
			OrderedGroup.Sort([&CandidateSets](const int32 A, const int32 B)
			{
				const FBusContractCandidateSet& Left = CandidateSets[A];
				const FBusContractCandidateSet& Right = CandidateSets[B];
				const double LeftY = Left.Candidates.Num() > 0 ? Left.Candidates[0].TrunkY : 0.0;
				const double RightY = Right.Candidates.Num() > 0 ? Right.Candidates[0].TrunkY : 0.0;
				if (!FMath::IsNearlyEqual(LeftY, RightY, 0.5))
				{
					return LeftY < RightY;
				}
				return Left.ContractIndex < Right.ContractIndex;
			});

			for (const int32 SetIndex : OrderedGroup)
			{
				if (!CandidateSets.IsValidIndex(SetIndex) || CandidateSets[SetIndex].Candidates.Num() == 0)
				{
					return false;
				}
				bool bHasZeroHardCandidate = false;
				for (const FBusContractLayoutCandidate& Candidate : CandidateSets[SetIndex].Candidates)
				{
					if (Candidate.HardHitCount == 0)
					{
						bHasZeroHardCandidate = true;
						break;
					}
				}

				auto PickBest = [&](const bool bRequireZeroHard) -> int32
				{
					int32 BestCandidateIndex = INDEX_NONE;
					double BestCost = TNumericLimits<double>::Max();
					for (int32 CandidateIndex = 0; CandidateIndex < CandidateSets[SetIndex].Candidates.Num(); ++CandidateIndex)
					{
						const FBusContractLayoutCandidate& Candidate = CandidateSets[SetIndex].Candidates[CandidateIndex];
						if (bRequireZeroHard && Candidate.HardHitCount > 0)
						{
							continue;
						}
						double ConflictPenalty = 0.0;
						for (const int32 PreviousSetIndex : OrderedGroup)
						{
							if (PreviousSetIndex == SetIndex)
							{
								break;
							}
							if (!OutChoiceBySetIndex.IsValidIndex(PreviousSetIndex) || OutChoiceBySetIndex[PreviousSetIndex] == INDEX_NONE)
							{
								continue;
							}
							const FBusContractLayoutCandidate& PreviousCandidate =
								CandidateSets[PreviousSetIndex].Candidates[OutChoiceBySetIndex[PreviousSetIndex]];
							if (BusContractCandidatesConflictOnTrunkY(Candidate, PreviousCandidate, Options, TrunkLaneSpacingOverride))
							{
								ConflictPenalty += 10000000.0;
							}
							if (BusContractCandidatesConflictOnBranchX(
								CandidateSets[SetIndex],
								Candidate,
								CandidateSets[PreviousSetIndex],
								PreviousCandidate,
								Options))
							{
								ConflictPenalty += 10000000.0;
							}
							ConflictPenalty += BusContractBranchOrderPenalty(
								CandidateSets[SetIndex],
								Candidate,
								CandidateSets[PreviousSetIndex],
								PreviousCandidate);
						}

						const double Cost = static_cast<double>(Candidate.HardHitCount) * 1000000000.0
							+ ConflictPenalty
							+ Candidate.Cost
							+ Candidate.TrunkY * 0.000001;
						if (BestCandidateIndex == INDEX_NONE || Cost < BestCost)
						{
							BestCandidateIndex = CandidateIndex;
							BestCost = Cost;
						}
					}
					return BestCandidateIndex;
				};

				int32 SelectedCandidate = PickBest(bHasZeroHardCandidate);
				if (SelectedCandidate == INDEX_NONE)
				{
					SelectedCandidate = PickBest(false);
				}
				if (SelectedCandidate == INDEX_NONE)
				{
					return false;
				}
				OutChoiceBySetIndex[SetIndex] = SelectedCandidate;
			}
			return true;
		}

		static void EmitPlannedCrossBusBranchLaneDiagnostics(
			const TArray<FPendingCorridorBusRoute>& PendingBusRoutes,
			TArray<FBusContract>& BusContracts,
			const TArray<FSelectedEndpointCandidate>& SelectedEndpoints,
			const FOptions& Options,
			FRunResult& Result);

		static void PlanBusContractsAtGeneration(
			TArray<FPendingCorridorBusRoute>& PendingBusRoutes,
			TArray<FBusContract>& BusContracts,
			const TArray<FSelectedEndpointCandidate>& SelectedEndpoints,
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const FOptions& Options,
			FRunResult& Result)
		{
			if (PendingBusRoutes.Num() == 0 || BusContracts.Num() == 0)
			{
				return;
			}

			TArray<FBusContractCandidateSet> CandidateSets;
			for (int32 ContractIndex = 0; ContractIndex < BusContracts.Num(); ++ContractIndex)
			{
				FBusContractCandidateSet Set;
				Set.ContractIndex = ContractIndex;
				Set.SourceParamId = BusContracts[ContractIndex].SourceParamId;
				for (int32 PendingIndex = 0; PendingIndex < PendingBusRoutes.Num(); ++PendingIndex)
				{
					if (PendingBusRoutes[PendingIndex].ContractIndex == BusContracts[ContractIndex].Id)
					{
						Set.PendingIndices.Add(PendingIndex);
					}
				}
				Set.Candidates = BuildBusContractLayoutCandidates(
					BusContracts[ContractIndex],
					Set.PendingIndices,
					PendingBusRoutes,
					Nodes,
					Edges,
					Islands,
					Options);
				if (Set.Candidates.Num() == 0)
				{
					continue;
				}
				Set.RoughXMin = TNumericLimits<double>::Max();
				Set.RoughXMax = -TNumericLimits<double>::Max();
				for (const FBusContractLayoutCandidate& Candidate : Set.Candidates)
				{
					Set.RoughXMin = FMath::Min(Set.RoughXMin, Candidate.XMin);
					Set.RoughXMax = FMath::Max(Set.RoughXMax, Candidate.XMax);
				}
				if (Set.RoughXMin == TNumericLimits<double>::Max())
				{
					Set.RoughXMin = 0.0;
					Set.RoughXMax = 0.0;
				}
				CandidateSets.Add(MoveTemp(Set));
			}

			if (CandidateSets.Num() == 0)
			{
				return;
			}

			TArray<int32> Parent;
			Parent.SetNum(CandidateSets.Num());
			for (int32 Index = 0; Index < Parent.Num(); ++Index)
			{
				Parent[Index] = Index;
			}
			TFunction<int32(int32)> FindRoot = [&Parent, &FindRoot](const int32 Index) -> int32
			{
				if (Parent[Index] != Index)
				{
					Parent[Index] = FindRoot(Parent[Index]);
				}
				return Parent[Index];
			};
			auto UnionSets = [&Parent, &FindRoot](const int32 A, const int32 B)
			{
				const int32 RootA = FindRoot(A);
				const int32 RootB = FindRoot(B);
				if (RootA != RootB)
				{
					Parent[RootB] = RootA;
				}
			};
			for (int32 A = 0; A < CandidateSets.Num(); ++A)
			{
				for (int32 B = A + 1; B < CandidateSets.Num(); ++B)
				{
					if (IntervalOverlaps(CandidateSets[A].RoughXMin, CandidateSets[A].RoughXMax, CandidateSets[B].RoughXMin, CandidateSets[B].RoughXMax))
					{
						UnionSets(A, B);
					}
				}
			}

			TMap<int32, TArray<int32>> GroupsByRoot;
			for (int32 SetIndex = 0; SetIndex < CandidateSets.Num(); ++SetIndex)
			{
				GroupsByRoot.FindOrAdd(FindRoot(SetIndex)).Add(SetIndex);
			}

			TArray<int32> ChosenCandidateBySet;
			ChosenCandidateBySet.Init(INDEX_NONE, CandidateSets.Num());
			for (TPair<int32, TArray<int32>>& Pair : GroupsByRoot)
			{
				TArray<int32> Group = Pair.Value;
				AddReservedLaneBusContractCandidatesForGroup(
					CandidateSets,
					Group,
					PendingBusRoutes,
					Nodes,
					Edges,
					Islands,
					Options,
					&Result);
				TArray<int32> GroupChoices;
				bool bUsedLaneConflictFallback = false;
				bool bUsedCompactTrunkSpacing = false;
				bool bUsedHardHitRelaxedForLaneSpacing = false;
				bool bUsedLargeGroupDeterministicSelection = false;
				const double CompactTrunkLaneSpacing = GetCompactBusTrunkLaneSpacingForGroup(
					CandidateSets,
					Group,
					PendingBusRoutes,
					Nodes,
					Edges,
					Options);
				const double StandardTrunkLaneSpacing = GetStandardBusTrunkLaneSpacingForGroup(
					CandidateSets,
					Group,
					PendingBusRoutes,
					Nodes,
					Edges,
					Options,
					FMath::Max(Options.NodePadding * 2.0, 48.0));
				int32 TotalCandidateCountForGroup = 0;
				for (const int32 SetIndex : Group)
				{
					if (CandidateSets.IsValidIndex(SetIndex))
					{
						TotalCandidateCountForGroup += CandidateSets[SetIndex].Candidates.Num();
					}
				}
				const bool bUseLargeGroupDeterministicSelection = Group.Num() > 8 || TotalCandidateCountForGroup > 512;
				if (bUseLargeGroupDeterministicSelection)
				{
					bUsedLargeGroupDeterministicSelection = SelectBusContractCandidatesForLargeGroupDeterministic(
						CandidateSets,
						Group,
						Options,
						StandardTrunkLaneSpacing,
						GroupChoices);
				}
				if (!bUsedLargeGroupDeterministicSelection
					&& !SelectBusContractCandidatesForGroup(CandidateSets, Group, Options, false, true, StandardTrunkLaneSpacing, GroupChoices))
				{
					bUsedCompactTrunkSpacing = SelectBusContractCandidatesForGroup(
						CandidateSets,
						Group,
						Options,
						false,
						true,
						CompactTrunkLaneSpacing,
						GroupChoices);
					if (!bUsedCompactTrunkSpacing)
					{
						bUsedHardHitRelaxedForLaneSpacing = SelectBusContractCandidatesForGroup(
							CandidateSets,
							Group,
							Options,
							false,
							false,
							StandardTrunkLaneSpacing,
							GroupChoices);
						if (!bUsedHardHitRelaxedForLaneSpacing)
						{
							bUsedCompactTrunkSpacing = SelectBusContractCandidatesForGroup(
								CandidateSets,
								Group,
								Options,
								false,
								false,
								CompactTrunkLaneSpacing,
								GroupChoices);
							bUsedHardHitRelaxedForLaneSpacing = bUsedCompactTrunkSpacing;
						}
						if (!bUsedHardHitRelaxedForLaneSpacing)
						{
							bUsedLaneConflictFallback = true;
							if (!SelectBusContractCandidatesForGroup(CandidateSets, Group, Options, true, true, StandardTrunkLaneSpacing, GroupChoices))
							{
								SelectBusContractCandidatesForGroup(CandidateSets, Group, Options, true, false, StandardTrunkLaneSpacing, GroupChoices);
							}
						}
					}
				}
				for (const int32 SetIndex : Group)
				{
					if (GroupChoices.IsValidIndex(SetIndex))
					{
						ChosenCandidateBySet[SetIndex] = GroupChoices[SetIndex];
					}
				}

				TArray<int32> LaneOrder = Group;
				LaneOrder.Sort([&CandidateSets, &ChosenCandidateBySet](const int32 A, const int32 B)
				{
					const int32 ChoiceA = ChosenCandidateBySet.IsValidIndex(A) ? ChosenCandidateBySet[A] : INDEX_NONE;
					const int32 ChoiceB = ChosenCandidateBySet.IsValidIndex(B) ? ChosenCandidateBySet[B] : INDEX_NONE;
					const double YA = CandidateSets[A].Candidates.IsValidIndex(ChoiceA) ? CandidateSets[A].Candidates[ChoiceA].TrunkY : 0.0;
					const double YB = CandidateSets[B].Candidates.IsValidIndex(ChoiceB) ? CandidateSets[B].Candidates[ChoiceB].TrunkY : 0.0;
					if (!FMath::IsNearlyEqual(YA, YB, 0.5))
					{
						return YA < YB;
					}
					return CandidateSets[A].ContractIndex < CandidateSets[B].ContractIndex;
				});
				for (int32 LaneIndex = 0; LaneIndex < LaneOrder.Num(); ++LaneIndex)
				{
					const int32 SetIndex = LaneOrder[LaneIndex];
					if (!CandidateSets.IsValidIndex(SetIndex)
						|| !BusContracts.IsValidIndex(CandidateSets[SetIndex].ContractIndex)
						|| !ChosenCandidateBySet.IsValidIndex(SetIndex)
						|| !CandidateSets[SetIndex].Candidates.IsValidIndex(ChosenCandidateBySet[SetIndex]))
					{
						continue;
					}
					FBusContract& Contract = BusContracts[CandidateSets[SetIndex].ContractIndex];
					Contract.BusLaneIndex = LaneIndex;
					const FBusContractLayoutCandidate& Candidate = CandidateSets[SetIndex].Candidates[ChosenCandidateBySet[SetIndex]];
					TSharedPtr<FJsonObject> LaneObj = MakeShared<FJsonObject>();
					LaneObj->SetNumberField(TEXT("bus_contract_id"), Contract.Id);
					LaneObj->SetNumberField(TEXT("edge_index"), Contract.EdgeIndices.Num() > 0 ? Contract.EdgeIndices[0] : INDEX_NONE);
					LaneObj->SetStringField(TEXT("orientation"), TEXT("horizontal"));
					LaneObj->SetNumberField(TEXT("lane_index"), LaneIndex);
					LaneObj->SetNumberField(TEXT("original_trunk_y"), Contract.TrunkY);
					LaneObj->SetNumberField(TEXT("trunk_y"), Candidate.TrunkY);
					LaneObj->SetNumberField(TEXT("x_min"), Candidate.XMin);
					LaneObj->SetNumberField(TEXT("x_max"), Candidate.XMax);
					LaneObj->SetNumberField(TEXT("lane_spacing"), StandardTrunkLaneSpacing);
					LaneObj->SetNumberField(TEXT("effective_lane_spacing"), bUsedCompactTrunkSpacing ? CompactTrunkLaneSpacing : StandardTrunkLaneSpacing);
					LaneObj->SetNumberField(TEXT("minimum_visible_lane_spacing"), CompactTrunkLaneSpacing);
					LaneObj->SetNumberField(TEXT("lane_stack_cost"), Candidate.Cost);
					LaneObj->SetBoolField(TEXT("x_projection_overlap_requires_y_lane"), Group.Num() > 1);
					LaneObj->SetBoolField(TEXT("lane_stack_solved_as_group"), true);
					LaneObj->SetBoolField(TEXT("compact_trunk_lane_spacing_used"), bUsedCompactTrunkSpacing);
					LaneObj->SetBoolField(TEXT("hard_hit_relaxed_for_lane_spacing"), bUsedHardHitRelaxedForLaneSpacing);
					LaneObj->SetBoolField(TEXT("lane_conflict_fallback_used"), bUsedLaneConflictFallback);
					LaneObj->SetBoolField(TEXT("large_group_deterministic_selection_used"), bUsedLargeGroupDeterministicSelection);
					Result.LaneAssignmentDiagnostics.Add(MakeShared<FJsonValueObject>(LaneObj));
				}
				if (Group.Num() > 1)
				{
					Result.DesignDiagnostics.CrossBusTrunkLaneCount += Group.Num() - 1;
				}
			}

			for (int32 SetIndex = 0; SetIndex < CandidateSets.Num(); ++SetIndex)
			{
				const int32 ContractIndex = CandidateSets[SetIndex].ContractIndex;
				const int32 CandidateIndex = ChosenCandidateBySet.IsValidIndex(SetIndex) ? ChosenCandidateBySet[SetIndex] : INDEX_NONE;
				if (!BusContracts.IsValidIndex(ContractIndex) || !CandidateSets[SetIndex].Candidates.IsValidIndex(CandidateIndex))
				{
					continue;
				}
				ApplyBusContractLayoutCandidate(
					CandidateSets[SetIndex].Candidates[CandidateIndex],
					CandidateSets[SetIndex].PendingIndices,
					PendingBusRoutes,
					BusContracts[ContractIndex]);
			}

			ApplyCrossBusBranchXLanesAtGeneration(
				PendingBusRoutes,
				BusContracts,
				SelectedEndpoints,
				Nodes,
				Edges,
				Islands,
				Options,
				Result);

			EmitPlannedCrossBusBranchLaneDiagnostics(PendingBusRoutes, BusContracts, SelectedEndpoints, Options, Result);

			(void)SelectedEndpoints;
		}

		struct FPlannedCrossBusBranchLaneItem
		{
			int32 PendingIndex = INDEX_NONE;
			int32 ContractIndex = INDEX_NONE;
			int32 EdgeIndex = INDEX_NONE;
			int32 ConnectionGroupId = INDEX_NONE;
			FString SourceParamId;
			double SourceOrderKey = 0.0;
			double TargetOrderKey = 0.0;
			double IdealBranchX = 0.0;
			double SelectedBranchX = 0.0;
			double LeftBoundX = 0.0;
			double RightBoundX = 0.0;
			double YMin = 0.0;
			double YMax = 0.0;
			double OutputToTrunkDistanceY = 0.0;
		};

		static void EmitPlannedCrossBusBranchLaneDiagnostics(
			const TArray<FPendingCorridorBusRoute>& PendingBusRoutes,
			TArray<FBusContract>& BusContracts,
			const TArray<FSelectedEndpointCandidate>& SelectedEndpoints,
			const FOptions& Options,
			FRunResult& Result)
		{
			TArray<FPlannedCrossBusBranchLaneItem> Items;
			for (int32 PendingIndex = 0; PendingIndex < PendingBusRoutes.Num(); ++PendingIndex)
			{
				const FPendingCorridorBusRoute& Pending = PendingBusRoutes[PendingIndex];
				if (!BusContracts.IsValidIndex(Pending.ContractIndex))
				{
					continue;
				}
				const int32 BranchPointIndex = GetTargetBranchPointIndex(Pending.Planned);
				if (!Pending.Planned.Polyline.IsValidIndex(BranchPointIndex))
				{
					continue;
				}
				const double RightBoundX = ComputeOutputXAlignedToInput(Pending.End.X, Options);
				const double LeftBoundX = FMath::Max(
					ComputeInputXAlignedToOutput(Pending.Start.X, Options),
					RightBoundX - GetCrossBusBranchSetbackLimitX(Options));
				if (LeftBoundX > RightBoundX + 0.5)
				{
					continue;
				}
				FPlannedCrossBusBranchLaneItem Item;
				Item.PendingIndex = PendingIndex;
				Item.ContractIndex = Pending.ContractIndex;
				Item.EdgeIndex = Pending.EdgeIndex;
				Item.SourceParamId = Pending.SourceParamId;
				Item.IdealBranchX = RightBoundX;
				Item.SelectedBranchX = Pending.Planned.Polyline[BranchPointIndex].X;
				Item.LeftBoundX = LeftBoundX;
				Item.RightBoundX = RightBoundX;
				Item.YMin = FMath::Min(Pending.Planned.TrunkY, Pending.End.Y);
				Item.YMax = FMath::Max(Pending.Planned.TrunkY, Pending.End.Y);
				Item.OutputToTrunkDistanceY = FMath::Abs(Pending.Start.Y - Pending.Planned.TrunkY);
				Item.TargetOrderKey = Pending.End.Y;
				if (SelectedEndpoints.IsValidIndex(Pending.SelectedEndpointIndex))
				{
					Item.ConnectionGroupId = SelectedEndpoints[Pending.SelectedEndpointIndex].ConnectionGroupId;
					Item.SourceOrderKey = SelectedEndpoints[Pending.SelectedEndpointIndex].SourcePortOrderKey;
				}
				Items.Add(Item);

				FBusContract& Contract = BusContracts[Pending.ContractIndex];
				Contract.BranchYMin = Item.YMin;
				Contract.BranchYMax = Item.YMax;
				Contract.OutputToTrunkDistanceY = Item.OutputToTrunkDistanceY;
				Contract.BranchLaneIndex = 0;
			}

			if (Items.Num() <= 1)
			{
				return;
			}

			TArray<int32> Parent;
			Parent.SetNum(Items.Num());
			for (int32 Index = 0; Index < Parent.Num(); ++Index)
			{
				Parent[Index] = Index;
			}
			TFunction<int32(int32)> FindRoot = [&Parent, &FindRoot](const int32 Index) -> int32
			{
				if (Parent[Index] != Index)
				{
					Parent[Index] = FindRoot(Parent[Index]);
				}
				return Parent[Index];
			};
			auto UnionItems = [&Parent, &FindRoot](const int32 A, const int32 B)
			{
				const int32 RootA = FindRoot(A);
				const int32 RootB = FindRoot(B);
				if (RootA != RootB)
				{
					Parent[RootB] = RootA;
				}
			};

			const double LaneSpacing = FMath::Max(Options.NodePadding * 1.5, 36.0);
			for (int32 A = 0; A < Items.Num(); ++A)
			{
				for (int32 B = A + 1; B < Items.Num(); ++B)
				{
					if (Items[A].ContractIndex == Items[B].ContractIndex)
					{
						continue;
					}
					if (Items[A].SourceParamId == Items[B].SourceParamId)
					{
						continue;
					}
					const bool bBranchYOverlaps = IntervalOverlaps(Items[A].YMin, Items[A].YMax, Items[B].YMin, Items[B].YMax);
					const bool bBranchXWouldConflictWithoutPlanning = FMath::Abs(Items[A].IdealBranchX - Items[B].IdealBranchX) < LaneSpacing + 0.5;
					const bool bSelectedBranchXStillConflicts = FMath::Abs(Items[A].SelectedBranchX - Items[B].SelectedBranchX) < LaneSpacing - 0.5;
					if (bBranchYOverlaps && (bBranchXWouldConflictWithoutPlanning || bSelectedBranchXStillConflicts))
					{
						UnionItems(A, B);
					}
				}
			}

			TMap<int32, TArray<int32>> GroupsByRoot;
			for (int32 ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
			{
				GroupsByRoot.FindOrAdd(FindRoot(ItemIndex)).Add(ItemIndex);
			}

			for (TPair<int32, TArray<int32>>& Pair : GroupsByRoot)
			{
				TArray<int32>& Group = Pair.Value;
				if (Group.Num() <= 1)
				{
					continue;
				}
				Group.Sort([&Items](const int32 A, const int32 B)
				{
					const FPlannedCrossBusBranchLaneItem& Left = Items[A];
					const FPlannedCrossBusBranchLaneItem& Right = Items[B];
					if (!FMath::IsNearlyEqual(Left.OutputToTrunkDistanceY, Right.OutputToTrunkDistanceY, 0.5))
					{
						return Left.OutputToTrunkDistanceY < Right.OutputToTrunkDistanceY;
					}
					if (Left.ConnectionGroupId != Right.ConnectionGroupId)
					{
						return Left.ConnectionGroupId < Right.ConnectionGroupId;
					}
					if (!FMath::IsNearlyEqual(Left.SourceOrderKey, Right.SourceOrderKey, 0.5))
					{
						return Left.SourceOrderKey < Right.SourceOrderKey;
					}
					if (!FMath::IsNearlyEqual(Left.TargetOrderKey, Right.TargetOrderKey, 0.5))
					{
						return Left.TargetOrderKey < Right.TargetOrderKey;
					}
					return Left.EdgeIndex < Right.EdgeIndex;
				});

				for (int32 LaneIndex = 0; LaneIndex < Group.Num(); ++LaneIndex)
				{
					const FPlannedCrossBusBranchLaneItem& Item = Items[Group[LaneIndex]];
					if (!BusContracts.IsValidIndex(Item.ContractIndex))
					{
						continue;
					}
					FBusContract& Contract = BusContracts[Item.ContractIndex];
					Contract.BranchLaneIndex = LaneIndex;
					Contract.BranchYMin = Item.YMin;
					Contract.BranchYMax = Item.YMax;
					Contract.OutputToTrunkDistanceY = Item.OutputToTrunkDistanceY;

					TSharedPtr<FJsonObject> LaneObj = MakeShared<FJsonObject>();
					LaneObj->SetNumberField(TEXT("bus_contract_id"), Contract.Id);
					LaneObj->SetNumberField(TEXT("edge_index"), Item.EdgeIndex);
					LaneObj->SetNumberField(TEXT("lane_index"), LaneIndex);
					LaneObj->SetNumberField(TEXT("branch_x"), Item.SelectedBranchX);
					LaneObj->SetNumberField(TEXT("ideal_branch_x"), Item.IdealBranchX);
					LaneObj->SetNumberField(TEXT("right_bound_x"), Item.RightBoundX);
					LaneObj->SetNumberField(TEXT("left_bound_x"), Item.LeftBoundX);
					LaneObj->SetNumberField(TEXT("trunk_y"), Contract.TrunkY);
					LaneObj->SetNumberField(TEXT("branch_y_min"), Item.YMin);
					LaneObj->SetNumberField(TEXT("branch_y_max"), Item.YMax);
					LaneObj->SetNumberField(TEXT("output_to_bus_bundle_y_distance"), Item.OutputToTrunkDistanceY);
					LaneObj->SetBoolField(TEXT("closer_output_prefers_right_branch_x"), true);
					LaneObj->SetBoolField(TEXT("branch_x_planned_at_generation"), true);
					LaneObj->SetBoolField(TEXT("selected_branch_x_still_conflicts"), false);
					Result.CrossBusLaneDiagnostics.Add(MakeShared<FJsonValueObject>(LaneObj));
				}
				Result.DesignDiagnostics.CrossBusLaneCount += FMath::Max(0, Group.Num() - 1);
			}
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

		static TArray<FReroutePlan> RouteDirectAndBusCorridors(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			TArray<FSelectedEndpointCandidate>& SelectedEndpoints,
			const FOptions& Options,
			FRunResult& Result)
		{
			const double RouteTraceStartSeconds = FPlatformTime::Seconds();
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore route_corridors_start selected=%d"),
				SelectedEndpoints.Num());
			TArray<FReroutePlan> Plans;
			TMultiMap<FString, int32> SelectedBySourceParam;
			for (int32 SelectedIndex = 0; SelectedIndex < SelectedEndpoints.Num(); ++SelectedIndex)
			{
				if (Edges.IsValidIndex(SelectedEndpoints[SelectedIndex].EdgeIndex))
				{
					SelectedBySourceParam.Add(MakeSourceParamId(Edges[SelectedEndpoints[SelectedIndex].EdgeIndex]), SelectedIndex);
				}
			}

			TArray<FBusContract> BusContracts;
			TArray<FPendingCorridorBusRoute> PendingBusRoutes;
			for (const TPair<FString, int32>& Pair : SelectedBySourceParam)
			{
				TArray<int32> GroupSelected;
				SelectedBySourceParam.MultiFind(Pair.Key, GroupSelected);
				GroupSelected.Sort();
				if (GroupSelected.Num() == 0 || GroupSelected[0] != Pair.Value)
				{
					continue;
				}

				TArray<double> InterfaceYs;
				int32 GroupContractIndex = INDEX_NONE;
				for (const int32 SelectedIndex : GroupSelected)
				{
					if (!SelectedEndpoints.IsValidIndex(SelectedIndex))
					{
						continue;
					}
					const FSelectedEndpointCandidate& Selected = SelectedEndpoints[SelectedIndex];
					if (!Islands.IsValidIndex(Selected.SourceIsland) || !Islands.IsValidIndex(Selected.TargetIsland))
					{
						continue;
					}
					InterfaceYs.Add(Islands[Selected.SourceIsland].Y + Selected.SourceLocal.Y);
					InterfaceYs.Add(Islands[Selected.TargetIsland].Y + Selected.EntryLocal.Y);
				}
				double InterfaceYSum = 0.0;
				for (const double InterfaceY : InterfaceYs)
				{
					InterfaceYSum += InterfaceY;
				}
				const double IdealBusY = InterfaceYs.Num() > 0 ? InterfaceYSum / static_cast<double>(InterfaceYs.Num()) : Options.OriginY;
				for (const double InterfaceY : InterfaceYs)
				{
					const double Residual = InterfaceY - IdealBusY;
					Result.DesignDiagnostics.BusYVarianceTotal += Residual * Residual;
				}

				for (const int32 SelectedIndex : GroupSelected)
				{
					if (!SelectedEndpoints.IsValidIndex(SelectedIndex))
					{
						continue;
					}
					FSelectedEndpointCandidate& Selected = SelectedEndpoints[SelectedIndex];
					if (!Islands.IsValidIndex(Selected.SourceIsland) || !Islands.IsValidIndex(Selected.TargetIsland) || !Edges.IsValidIndex(Selected.EdgeIndex))
					{
						continue;
					}
					const FIsland& SourceIsland = Islands[Selected.SourceIsland];
					const FIsland& TargetIsland = Islands[Selected.TargetIsland];
					const FVector2D Start(SourceIsland.X + Selected.SourceLocal.X, SourceIsland.Y + Selected.SourceLocal.Y);

					const FPlannedCorridorRoute Planned = PlanCorridorRouteForSelectedEndpoint(
						Nodes,
						Edges,
						Islands,
						Selected,
						GroupSelected.Num(),
						IdealBusY,
						Options);
					const FVector2D RouteEnd = Planned.Polyline.Num() > 0
						? Planned.Polyline.Last()
						: FVector2D(TargetIsland.X + Selected.EntryLocal.X, TargetIsland.Y + Selected.EntryLocal.Y);
					const FVector2D ActualEnd(TargetIsland.X + Selected.ActualTargetLocal.X, TargetIsland.Y + Selected.ActualTargetLocal.Y);

					if (Planned.RouteKind == ERouteKind::Direct)
					{
						AddRouteDiagnostics(Result, Selected.EdgeIndex, Selected.Side, ERouteKind::Direct, Planned.Polyline, false, false);
						Result.DesignDiagnostics.IslandConnectionTotalLength += PolylineManhattanLength(Planned.Polyline);
						continue;
					}

					if (GroupContractIndex == INDEX_NONE)
					{
						FBusContract Contract;
						Contract.Id = BusContracts.Num();
						Contract.SourceParamId = Pair.Key;
						Contract.RouteKind = Planned.RouteKind;
						Contract.TrunkY = IdealBusY;
						Contract.XMin = TNumericLimits<double>::Max();
						Contract.XMax = -TNumericLimits<double>::Max();
						GroupContractIndex = Contract.Id;
						BusContracts.Add(Contract);
					}

					Selected.BusContractId = GroupContractIndex;
					FPendingCorridorBusRoute Pending;
					Pending.SelectedEndpointIndex = SelectedIndex;
					Pending.ContractIndex = GroupContractIndex;
					Pending.EdgeIndex = Selected.EdgeIndex;
					Pending.SourceParamId = Pair.Key;
					Pending.Side = Selected.Side;
					Pending.Planned = Planned;
					Pending.Start = Start;
					Pending.End = ActualEnd;
					Pending.ActualEnd = ActualEnd;
					PendingBusRoutes.Add(Pending);
				}
			}

			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore route_corridors_groups_done elapsed_ms=%.1f contracts=%d pending=%d"),
				(FPlatformTime::Seconds() - RouteTraceStartSeconds) * 1000.0,
				BusContracts.Num(),
				PendingBusRoutes.Num());
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore plan_bus_contracts_start elapsed_ms=%.1f"),
				(FPlatformTime::Seconds() - RouteTraceStartSeconds) * 1000.0);
			PlanBusContractsAtGeneration(PendingBusRoutes, BusContracts, SelectedEndpoints, Nodes, Edges, Islands, Options, Result);
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore plan_bus_contracts_done elapsed_ms=%.1f lane_diags=%d"),
				(FPlatformTime::Seconds() - RouteTraceStartSeconds) * 1000.0,
				Result.LaneAssignmentDiagnostics.Num());
			for (FBusContract& Contract : BusContracts)
			{
				RecomputeBusContractBoundsFromPendingRoutes(Contract, PendingBusRoutes);
			}

			for (FPendingCorridorBusRoute& Pending : PendingBusRoutes)
			{
				if (!BusContracts.IsValidIndex(Pending.ContractIndex))
				{
					continue;
				}
				FBusContract& Contract = BusContracts[Pending.ContractIndex];
				if (Edges.IsValidIndex(Pending.EdgeIndex))
				{
					const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[Pending.EdgeIndex]);
					Pending.Planned.bCrossesHardOccupancy = ControlWireHitsHardOccupancy(Pending.Planned.Polyline, RouteHardRects);
					Pending.Planned.RouteKind = Pending.Planned.bCrossesHardOccupancy ? ERouteKind::ExternalBus : ERouteKind::LocalBus;
				}
				FReroutePlan Plan;
				Plan.EdgeIndex = Pending.EdgeIndex;
				Plan.RouteKind = Pending.Planned.RouteKind;
				Plan.BusId = Pending.SourceParamId;
				Plan.AttachmentMergeGroup = Contract.Id;
				Plan.BusLaneIndex = Contract.BusLaneIndex;
				Plan.BranchLaneIndex = Contract.BranchLaneIndex;
				Plan.Reason = Pending.Planned.RouteKind == ERouteKind::ExternalBus ? TEXT("external_corridor") : TEXT("fanout_local_bus");
				for (int32 PointIndex = 1; PointIndex + 1 < Pending.Planned.Polyline.Num(); ++PointIndex)
				{
					Plan.Points.Add(Pending.Planned.Polyline[PointIndex]);
				}
				if (Pending.Planned.bEndsAtBoundaryEntry && Pending.Planned.Polyline.Num() > 0)
				{
					Plan.Points.Add(Pending.Planned.Polyline.Last());
				}
				CompactRoutePoints(Plan, Pending.Start, Pending.ActualEnd, Options);
				if (Plan.Points.Num() > 0)
				{
					Plans.Add(Plan);
				}

				if (Contract.BranchXs.Num() > 1)
				{
					++Result.DesignDiagnostics.BranchMergeGroupCount;
				}
				AddRouteDiagnostics(Result, Pending.EdgeIndex, Pending.Side, Pending.Planned.RouteKind, Pending.Planned.Polyline, Pending.Planned.RouteKind == ERouteKind::ExternalBus, Pending.Planned.bCrossesHardOccupancy);
				Result.DesignDiagnostics.IslandConnectionTotalLength += PolylineManhattanLength(Pending.Planned.Polyline);
				++Result.DesignDiagnostics.RouteKindUpgradeCount;
				if (Pending.Planned.RouteKind == ERouteKind::ExternalBus)
				{
					++Result.DesignDiagnostics.IslandConnectionExternalUpgradeCount;
				}
			}

			Result.DesignDiagnostics.BusContractCount = BusContracts.Num();
			Result.DesignDiagnostics.RouteGroupCount = Result.DesignDiagnostics.ConnectionGroupCount;
			Result.DesignDiagnostics.LaneColoringLaneCount = BusContracts.Num() > 0
				? FMath::Max(Result.DesignDiagnostics.LaneColoringLaneCount, Result.DesignDiagnostics.CrossBusLaneCount + 1)
				: 0;
			Result.DesignDiagnostics.RouteThroughIslandCount = Result.DesignDiagnostics.RouteThroughHardOccupancyCount;
			for (const FBusContract& Contract : BusContracts)
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetNumberField(TEXT("bus_contract_id"), Contract.Id);
				Obj->SetStringField(TEXT("source_param_id"), Contract.SourceParamId);
				Obj->SetStringField(TEXT("route_kind"), RouteKindToString(Contract.RouteKind));
				Obj->SetNumberField(TEXT("trunk_y"), Contract.TrunkY);
				Obj->SetNumberField(TEXT("x_min"), Contract.XMin);
				Obj->SetNumberField(TEXT("x_max"), Contract.XMax);
				Obj->SetNumberField(TEXT("branch_y_min"), Contract.BranchYMin);
				Obj->SetNumberField(TEXT("branch_y_max"), Contract.BranchYMax);
				Obj->SetNumberField(TEXT("output_to_bus_bundle_y_distance"), Contract.OutputToTrunkDistanceY);
				Obj->SetNumberField(TEXT("bus_lane_index"), Contract.BusLaneIndex);
				Obj->SetNumberField(TEXT("branch_lane_index"), Contract.BranchLaneIndex);
				Obj->SetArrayField(TEXT("edge_indices"), IntArrayToJson(Contract.EdgeIndices));
				TArray<TSharedPtr<FJsonValue>> BranchValues;
				for (const double BranchX : Contract.BranchXs)
				{
					BranchValues.Add(MakeShared<FJsonValueNumber>(BranchX));
				}
				Obj->SetArrayField(TEXT("merged_branch_x"), BranchValues);
				Result.BusContractDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
			}
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore route_corridors_done elapsed_ms=%.1f plans=%d contracts=%d"),
				(FPlatformTime::Seconds() - RouteTraceStartSeconds) * 1000.0,
				Plans.Num(),
				BusContracts.Num());
			return Plans;
		}

		static TArray<FReroutePlan> RunFrozenIslandCorridorSolver(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			TArray<FIsland>& Islands,
			const TArray<FIslandEdge>& IslandEdges,
			const TArray<FIslandPortCandidate>& Candidates,
			const FOptions& Options,
			FRunResult& Result)
		{
			TSet<int32> RouteOnlyIslandEdgeIndices;
			TSet<int32> RouteOnlyEdgeIndices;
			for (int32 IslandEdgeIndex = 0; IslandEdgeIndex < IslandEdges.Num(); ++IslandEdgeIndex)
			{
				if (IslandEdges[IslandEdgeIndex].bFeedback)
				{
					RouteOnlyIslandEdgeIndices.Add(IslandEdgeIndex);
					RouteOnlyEdgeIndices.Add(IslandEdges[IslandEdgeIndex].EdgeIndex);
				}
			}
			AssignIslandPlacementRanks(Islands, IslandEdges, RouteOnlyIslandEdgeIndices);
			for (int32 Safety = 0; Safety < Islands.Num() + 1; ++Safety)
			{
				const int32 RouteOnlyCountBefore = RouteOnlyIslandEdgeIndices.Num();
				MarkRouteOnlyCycleEdges(Islands, IslandEdges, RouteOnlyIslandEdgeIndices, Result);
				if (RouteOnlyIslandEdgeIndices.Num() == RouteOnlyCountBefore)
				{
					break;
				}
			}
			RouteOnlyEdgeIndices.Reset();
			for (int32 IslandEdgeIndex = 0; IslandEdgeIndex < IslandEdges.Num(); ++IslandEdgeIndex)
			{
				if (RouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex))
				{
					RouteOnlyEdgeIndices.Add(IslandEdges[IslandEdgeIndex].EdgeIndex);
				}
			}
			AssignIslandPlacementRanks(Islands, IslandEdges, RouteOnlyIslandEdgeIndices);

			const TArray<int32> ComponentByIsland = BuildIslandComponentMap(Islands, IslandEdges, RouteOnlyIslandEdgeIndices);
			TSet<int32> ComponentIds;
			for (const int32 ComponentId : ComponentByIsland)
			{
				if (ComponentId != INDEX_NONE)
				{
					ComponentIds.Add(ComponentId);
				}
			}
			Result.DesignDiagnostics.PlacementComponentCount = ComponentIds.Num();
			Result.DesignDiagnostics.RouteOnlyEdgeCount = RouteOnlyIslandEdgeIndices.Num();
			Result.DesignDiagnostics.PlacementGraphEdgeCount = FMath::Max(0, IslandEdges.Num() - RouteOnlyIslandEdgeIndices.Num());
			Result.DesignDiagnostics.EndpointCandidateCount = Candidates.Num();

			TArray<FPlacementProposal> Proposals;
			Proposals.Reserve(Candidates.Num());
			for (const FIslandPortCandidate& Candidate : Candidates)
			{
				Proposals.Add(MakePlacementProposal(Candidate, RouteOnlyEdgeIndices, Options));
			}
			AddPlacementProposalDiagnostics(Proposals, Result);

			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			TSet<int32> SelectedEdgeIndices;
			for (const FIslandEdge& IslandEdge : IslandEdges)
			{
				if (!Edges.IsValidIndex(IslandEdge.EdgeIndex))
				{
					continue;
				}
				FSelectedEndpointCandidate Selected = SelectEndpointFromProposals(Candidates, Proposals, IslandEdge.EdgeIndex, Result);
				if (Selected.EdgeIndex == INDEX_NONE || SelectedEdgeIndices.Contains(Selected.EdgeIndex))
				{
					continue;
				}
				SelectedEndpoints.Add(Selected);
				SelectedEdgeIndices.Add(Selected.EdgeIndex);
			}

			SolveIslandXByLongestPath(Islands, SelectedEndpoints, RouteOnlyEdgeIndices, Options, Result);
			for (FSelectedEndpointCandidate& Selected : SelectedEndpoints)
			{
				ResolveSelectedEndpointEntry(Candidates, Islands, Options, Selected);
			}
			SolveIslandXByLongestPath(Islands, SelectedEndpoints, RouteOnlyEdgeIndices, Options, Result);
			PlaceIslandsYByWeightedMedianAndPacking(Islands, SelectedEndpoints, Options);
			Result.DesignDiagnostics.SourcePortOrderViolationCount = CountSourcePortOrderViolations(
				Islands,
				BuildSourcePortOrderConstraints(Islands, SelectedEndpoints));
			SnapIslandOffsetsToPixelGrid(Islands);
			for (FSelectedEndpointCandidate& Selected : SelectedEndpoints)
			{
				ResolveSelectedEndpointEntry(Candidates, Islands, Options, Selected);
			}
			RelaxIslandXForEndpointHardOccupancy(Nodes, Edges, Islands, SelectedEndpoints, RouteOnlyEdgeIndices, Options);
			RelaxIslandXForSelectedEndpointForwardValidity(Islands, SelectedEndpoints, RouteOnlyEdgeIndices, Options);
			SnapIslandOffsetsToPixelGrid(Islands);
			for (FSelectedEndpointCandidate& Selected : SelectedEndpoints)
			{
				ResolveSelectedEndpointEntry(Candidates, Islands, Options, Selected);
				if (Islands.IsValidIndex(Selected.SourceIsland) && Islands.IsValidIndex(Selected.TargetIsland))
				{
					const FIsland& SourceIsland = Islands[Selected.SourceIsland];
					const FIsland& TargetIsland = Islands[Selected.TargetIsland];
					if (!IsCandidateForwardValid(SourceIsland, TargetIsland, Selected, Options))
					{
						++Result.DesignDiagnostics.InvalidForwardConnectionCount;
					}
					Selected.PrimaryResidual = Selected.Side == TEXT("left")
						? FMath::Abs((SourceIsland.Y + Selected.SourceLocal.Y) - (TargetIsland.Y + Selected.EntryLocal.Y))
						: FMath::Abs(ComputeConnectionAlignmentShortenX(
								FVector2D(SourceIsland.X + Selected.SourceLocal.X, SourceIsland.Y + Selected.SourceLocal.Y),
								FVector2D(TargetIsland.X + Selected.EntryLocal.X, TargetIsland.Y + Selected.EntryLocal.Y),
								Options))
							+ FMath::Abs(ComputeConnectionAlignmentShortenX(
								FVector2D(TargetIsland.X + Selected.EntryLocal.X, TargetIsland.Y + Selected.EntryLocal.Y),
								FVector2D(TargetIsland.X + Selected.ActualTargetLocal.X, TargetIsland.Y + Selected.ActualTargetLocal.Y),
								Options));
					if (Selected.Side == TEXT("left"))
					{
						Result.DesignDiagnostics.IslandConnectionLeftYResidual += Selected.PrimaryResidual;
					}
					else
					{
						Result.DesignDiagnostics.IslandConnectionTopBottomXResidual += Selected.PrimaryResidual;
					}
				}
			}

			BuildSourceOrderAndFanoutDiagnostics(Edges, SelectedEndpoints, Result);
			Result.DesignDiagnostics.ConnectionGroupCount = Result.DesignDiagnostics.SourceOrderGroupCount;
			Result.DesignDiagnostics.SelectedEndpointCandidateCount = SelectedEndpoints.Num();
			for (const FSelectedEndpointCandidate& Selected : SelectedEndpoints)
			{
				AddSelectedEndpointDiagnostics(Result, Selected, Islands, Options);
			}
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore frozen_solver_before_route selected=%d route_only=%d placement_components=%d"),
				SelectedEndpoints.Num(),
				RouteOnlyIslandEdgeIndices.Num(),
				Result.DesignDiagnostics.PlacementComponentCount);
			return RouteDirectAndBusCorridors(Nodes, Edges, Islands, SelectedEndpoints, Options, Result);
		}
