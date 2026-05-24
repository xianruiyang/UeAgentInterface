// Copyright Epic Games, Inc. All Rights Reserved.
// Cycle cutting, island extraction, input-chain detection, rank and column ordering.

		static void AssignAuxiliaryRanks(const TArray<FNode>& Nodes, const TArray<FEdge>& Edges, TArray<int32>& OutRank)
		{
			OutRank.Init(0, Nodes.Num());
			for (int32 Iter = 0; Iter < Nodes.Num(); ++Iter)
			{
				bool bChanged = false;
				for (const FEdge& Edge : Edges)
				{
					if (!IsValidNodeIndex(Nodes, Edge.Source) || !IsValidNodeIndex(Nodes, Edge.Target))
					{
						continue;
					}
					const int32 Candidate = FMath::Min(OutRank[Edge.Source] + 1, Nodes.Num());
					if (Candidate > OutRank[Edge.Target])
					{
						OutRank[Edge.Target] = Candidate;
						bChanged = true;
					}
				}
				if (!bChanged)
				{
					break;
				}
			}
		}

		static void StrongConnect(
			int32 NodeIndex,
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TSet<int32>& RankIgnoredEdges,
			TArray<TArray<int32>>& OutSccs,
			TArray<int32>& IndexByNode,
			TArray<int32>& LowLinkByNode,
			TArray<int32>& Stack,
			TSet<int32>& StackSet,
			int32& NextIndex)
		{
			IndexByNode[NodeIndex] = NextIndex;
			LowLinkByNode[NodeIndex] = NextIndex;
			++NextIndex;
			Stack.Add(NodeIndex);
			StackSet.Add(NodeIndex);

			for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
			{
				if (RankIgnoredEdges.Contains(EdgeIndex) || Edges[EdgeIndex].Source != NodeIndex)
				{
					continue;
				}
				const int32 Target = Edges[EdgeIndex].Target;
				if (!IsValidNodeIndex(Nodes, Target))
				{
					continue;
				}
				if (IndexByNode[Target] == INDEX_NONE)
				{
					StrongConnect(Target, Nodes, Edges, RankIgnoredEdges, OutSccs, IndexByNode, LowLinkByNode, Stack, StackSet, NextIndex);
					LowLinkByNode[NodeIndex] = FMath::Min(LowLinkByNode[NodeIndex], LowLinkByNode[Target]);
				}
				else if (StackSet.Contains(Target))
				{
					LowLinkByNode[NodeIndex] = FMath::Min(LowLinkByNode[NodeIndex], IndexByNode[Target]);
				}
			}

			if (LowLinkByNode[NodeIndex] == IndexByNode[NodeIndex])
			{
				TArray<int32> Scc;
				while (Stack.Num() > 0)
				{
					const int32 Popped = Stack.Pop();
					StackSet.Remove(Popped);
					Scc.Add(Popped);
					if (Popped == NodeIndex)
					{
						break;
					}
				}
				OutSccs.Add(MoveTemp(Scc));
			}
		}

		static TArray<TArray<int32>> FindSccs(const TArray<FNode>& Nodes, const TArray<FEdge>& Edges, const TSet<int32>& RankIgnoredEdges)
		{
			TArray<TArray<int32>> Sccs;
			TArray<int32> IndexByNode;
			TArray<int32> LowLinkByNode;
			IndexByNode.Init(INDEX_NONE, Nodes.Num());
			LowLinkByNode.Init(0, Nodes.Num());
			TArray<int32> Stack;
			TSet<int32> StackSet;
			int32 NextIndex = 0;
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				if (IsValidNodeIndex(Nodes, NodeIndex) && IndexByNode[NodeIndex] == INDEX_NONE)
				{
					StrongConnect(NodeIndex, Nodes, Edges, RankIgnoredEdges, Sccs, IndexByNode, LowLinkByNode, Stack, StackSet, NextIndex);
				}
			}
			return Sccs;
		}

		static void MarkFeedbackEdgesAsRankIgnored(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			TSet<int32>& RankIgnoredEdges,
			TSet<int32>& FeedbackEdges,
			FDesignDiagnostics& Diagnostics)
		{
			TArray<int32> AuxRank;
			AssignAuxiliaryRanks(Nodes, Edges, AuxRank);
			for (const TArray<int32>& Scc : FindSccs(Nodes, Edges, RankIgnoredEdges))
			{
				if (Scc.Num() > 1)
				{
					++Diagnostics.SccCountBeforeCut;
				}
			}
			for (int32 Guard = 0; Guard < Nodes.Num(); ++Guard)
			{
				TArray<TArray<int32>> Sccs = FindSccs(Nodes, Edges, RankIgnoredEdges);
				bool bCutAny = false;
				for (const TArray<int32>& Scc : Sccs)
				{
					if (Scc.Num() <= 1)
					{
						continue;
					}
					TSet<int32> SccSet;
					for (const int32 NodeIndex : Scc)
					{
						SccSet.Add(NodeIndex);
					}
					int32 BestEdge = INDEX_NONE;
					double BestScore = -TNumericLimits<double>::Max();
					for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
					{
						const FEdge& Edge = Edges[EdgeIndex];
						if (RankIgnoredEdges.Contains(EdgeIndex) || !SccSet.Contains(Edge.Source) || !SccSet.Contains(Edge.Target))
						{
							continue;
						}
						const int32 SourceRank = AuxRank.IsValidIndex(Edge.Source) ? AuxRank[Edge.Source] : 0;
						const int32 TargetRank = AuxRank.IsValidIndex(Edge.Target) ? AuxRank[Edge.Target] : 0;
						const bool bLooksBackward = SourceRank > TargetRank || (SourceRank == TargetRank && Edge.Source > Edge.Target);
						const double Score = (bLooksBackward ? 1000000.0 : 0.0)
							+ static_cast<double>(SourceRank - TargetRank) * 1000.0
							+ Nodes[Edge.Source].OldX - Nodes[Edge.Target].OldX
							+ static_cast<double>(Edge.Source - Edge.Target) * 0.01;
						if (BestEdge == INDEX_NONE || Score > BestScore)
						{
							BestEdge = EdgeIndex;
							BestScore = Score;
						}
					}
					if (BestEdge != INDEX_NONE)
					{
						RankIgnoredEdges.Add(BestEdge);
						FeedbackEdges.Add(BestEdge);
						++Diagnostics.SccFeedbackEdgeCount;
						bCutAny = true;
					}
				}
				if (!bCutAny)
				{
					break;
				}
			}
		}

		static FRect MakeOldNodeRect(const FNode& Node)
		{
			return { Node.OldX, Node.OldY, Node.OldX + Node.Width, Node.OldY + Node.Height };
		}

		static bool RectFullyContains(const FRect& Outer, const FRect& Inner, const double Tolerance = 1.0)
		{
			return Inner.Left >= Outer.Left - Tolerance
				&& Inner.Top >= Outer.Top - Tolerance
				&& Inner.Right <= Outer.Right + Tolerance
				&& Inner.Bottom <= Outer.Bottom + Tolerance;
		}

		static TArray<int32> BuildCommentIslandGroupByNode(const TArray<FNode>& Nodes)
		{
			TArray<int32> GroupByNode;
			GroupByNode.Init(INDEX_NONE, Nodes.Num());

			TArray<int32> CommentNodes;
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				if (IsValidNodeIndex(Nodes, NodeIndex) && Nodes[NodeIndex].bIsCommentNode)
				{
					CommentNodes.Add(NodeIndex);
				}
			}
			if (CommentNodes.Num() == 0)
			{
				return GroupByNode;
			}

			TMap<int32, int32> GroupByCommentNode;
			for (const int32 CommentNodeIndex : CommentNodes)
			{
				const int32 GroupId = GroupByCommentNode.Num();
				GroupByCommentNode.Add(CommentNodeIndex, GroupId);
				GroupByNode[CommentNodeIndex] = GroupId;
			}

			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				if (!IsValidNodeIndex(Nodes, NodeIndex) || Nodes[NodeIndex].bIsCommentNode)
				{
					continue;
				}
				const FRect NodeRect = MakeOldNodeRect(Nodes[NodeIndex]);
				int32 BestCommentNode = INDEX_NONE;
				double BestArea = TNumericLimits<double>::Max();
				int32 BestDepth = TNumericLimits<int32>::Min();
				for (const int32 CommentNodeIndex : CommentNodes)
				{
					const FNode& CommentNode = Nodes[CommentNodeIndex];
					const FRect CommentRect = MakeOldNodeRect(CommentNode);
					if (!RectFullyContains(CommentRect, NodeRect))
					{
						continue;
					}
					const double Area = FMath::Max(0.0, CommentRect.Width()) * FMath::Max(0.0, CommentRect.Height());
					if (BestCommentNode == INDEX_NONE
						|| Area < BestArea - 0.5
						|| (FMath::IsNearlyEqual(Area, BestArea, 0.5) && CommentNode.CommentDepth > BestDepth))
					{
						BestCommentNode = CommentNodeIndex;
						BestArea = Area;
						BestDepth = CommentNode.CommentDepth;
					}
				}
				if (BestCommentNode != INDEX_NONE)
				{
					GroupByNode[NodeIndex] = GroupByCommentNode.FindRef(BestCommentNode);
				}
			}
			return GroupByNode;
		}

		static bool IsSameCommentIslandGroup(const TArray<int32>& CommentGroupByNode, const FEdge& Edge)
		{
			return CommentGroupByNode.IsValidIndex(Edge.Source)
				&& CommentGroupByNode.IsValidIndex(Edge.Target)
				&& CommentGroupByNode[Edge.Source] != INDEX_NONE
				&& CommentGroupByNode[Edge.Source] == CommentGroupByNode[Edge.Target];
		}

		static bool CrossesCommentIslandBoundary(const TArray<int32>& CommentGroupByNode, const FEdge& Edge)
		{
			if (!CommentGroupByNode.IsValidIndex(Edge.Source) || !CommentGroupByNode.IsValidIndex(Edge.Target))
			{
				return false;
			}
			const int32 SourceGroup = CommentGroupByNode[Edge.Source];
			const int32 TargetGroup = CommentGroupByNode[Edge.Target];
			return SourceGroup != TargetGroup && (SourceGroup != INDEX_NONE || TargetGroup != INDEX_NONE);
		}

		static TArray<TArray<int32>> BuildWeakIslands(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TSet<int32>& IslandCutEdges,
			const TArray<int32>* ForcedIslandGroupByNode = nullptr)
		{
			TArray<TArray<int32>> Adj;
			Adj.SetNum(Nodes.Num());
			if (ForcedIslandGroupByNode)
			{
				TMap<int32, int32> FirstNodeByForcedGroup;
				for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
				{
					if (!IsValidNodeIndex(Nodes, NodeIndex) || !ForcedIslandGroupByNode->IsValidIndex(NodeIndex))
					{
						continue;
					}
					const int32 GroupId = (*ForcedIslandGroupByNode)[NodeIndex];
					if (GroupId == INDEX_NONE)
					{
						continue;
					}
					if (int32* FirstNode = FirstNodeByForcedGroup.Find(GroupId))
					{
						Adj[*FirstNode].Add(NodeIndex);
						Adj[NodeIndex].Add(*FirstNode);
					}
					else
					{
						FirstNodeByForcedGroup.Add(GroupId, NodeIndex);
					}
				}
			}
			for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
			{
				if (IslandCutEdges.Contains(EdgeIndex))
				{
					continue;
				}
				const FEdge& Edge = Edges[EdgeIndex];
				if (!IsValidNodeIndex(Nodes, Edge.Source) || !IsValidNodeIndex(Nodes, Edge.Target))
				{
					continue;
				}
				if (ForcedIslandGroupByNode
					&& ForcedIslandGroupByNode->IsValidIndex(Edge.Source)
					&& ForcedIslandGroupByNode->IsValidIndex(Edge.Target))
				{
					const int32 SourceGroup = (*ForcedIslandGroupByNode)[Edge.Source];
					const int32 TargetGroup = (*ForcedIslandGroupByNode)[Edge.Target];
					if (SourceGroup != TargetGroup && (SourceGroup != INDEX_NONE || TargetGroup != INDEX_NONE))
					{
						continue;
					}
				}
				Adj[Edge.Source].Add(Edge.Target);
				Adj[Edge.Target].Add(Edge.Source);
			}
			TArray<uint8> Visited;
			Visited.Init(0, Nodes.Num());
			TArray<TArray<int32>> Islands;
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				if (!IsValidNodeIndex(Nodes, NodeIndex) || Visited[NodeIndex])
				{
					continue;
				}
				TArray<int32> Queue;
				TArray<int32> Group;
				Queue.Add(NodeIndex);
				Visited[NodeIndex] = 1;
				for (int32 Cursor = 0; Cursor < Queue.Num(); ++Cursor)
				{
					const int32 Current = Queue[Cursor];
					Group.Add(Current);
					for (const int32 Next : Adj[Current])
					{
						if (IsValidNodeIndex(Nodes, Next) && !Visited[Next])
						{
							Visited[Next] = 1;
							Queue.Add(Next);
						}
					}
				}
				Group.Sort([&Nodes](int32 A, int32 B)
				{
					return Nodes[A].Index < Nodes[B].Index;
				});
				Islands.Add(MoveTemp(Group));
			}
			return Islands;
		}

		static bool IsInputChainNodeCandidate(const FNode& Node)
		{
			return !Node.bHasExecPin && !Node.bRerouteNode;
		}

		static double EstimateInputChainWidth(const TArray<FNode>& Nodes, const FInputChain& Chain, double NodeGap)
		{
			double Width = 0.0;
			for (int32 Index = 0; Index < Chain.Nodes.Num(); ++Index)
			{
				if (!Nodes.IsValidIndex(Chain.Nodes[Index]))
				{
					continue;
				}
				Width += Nodes[Chain.Nodes[Index]].Width;
				if (Index + 1 < Chain.Nodes.Num())
				{
					Width += NodeGap;
				}
			}
			return Width;
		}

		static double EstimateInputChainHeight(const TArray<FNode>& Nodes, const FInputChain& Chain)
		{
			double Height = 0.0;
			for (const int32 NodeIndex : Chain.Nodes)
			{
				if (Nodes.IsValidIndex(NodeIndex))
				{
					Height = FMath::Max(Height, Nodes[NodeIndex].Height);
				}
			}
			return Height;
		}

		static TArray<double> BuildColumnLefts(const FIsland& Island)
		{
			TArray<double> ColumnLefts;
			ColumnLefts.SetNum(Island.ColumnWidths.Num());
			for (int32 ColumnIndex = 1; ColumnIndex < ColumnLefts.Num(); ++ColumnIndex)
			{
				const double PreviousWidth = Island.ColumnWidths.IsValidIndex(ColumnIndex - 1)
					? Island.ColumnWidths[ColumnIndex - 1]
					: 0.0;
				const double PreviousGap = Island.ColumnGaps.IsValidIndex(ColumnIndex - 1)
					? Island.ColumnGaps[ColumnIndex - 1]
					: 0.0;
				ColumnLefts[ColumnIndex] = ColumnLefts[ColumnIndex - 1] + PreviousWidth + PreviousGap;
			}
			return ColumnLefts;
		}

		static void ExtractInputChains(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& AllEdges,
			const TSet<int32>& RankIgnoredEdges,
			FIsland& Island,
			FDesignDiagnostics& Diagnostics)
		{
			Island.InputChains.Reset();
			Island.InputChainNodeSet.Reset();

			TArray<TArray<int32>> Incoming;
			TArray<TArray<int32>> Outgoing;
			TArray<TArray<int32>> AllDataIncoming;
			TArray<TArray<int32>> AllDataOutgoing;
			Incoming.SetNum(Nodes.Num());
			Outgoing.SetNum(Nodes.Num());
			AllDataIncoming.SetNum(Nodes.Num());
			AllDataOutgoing.SetNum(Nodes.Num());
			TSet<int32> ExplicitEdgeSet;
			for (int32 EdgeIndex = 0; EdgeIndex < AllEdges.Num(); ++EdgeIndex)
			{
				const FEdge& Edge = AllEdges[EdgeIndex];
				if (!IsValidNodeIndex(Nodes, Edge.Source) || !IsValidNodeIndex(Nodes, Edge.Target) || Edge.bExec)
				{
					continue;
				}
				AllDataIncoming[Edge.Target].Add(EdgeIndex);
				AllDataOutgoing[Edge.Source].Add(EdgeIndex);
				if (RankIgnoredEdges.Contains(EdgeIndex))
				{
					continue;
				}
				Incoming[Edge.Target].Add(EdgeIndex);
				Outgoing[Edge.Source].Add(EdgeIndex);
				ExplicitEdgeSet.Add(EdgeIndex);
			}

			struct FCandidate
			{
				int32 TargetNode = INDEX_NONE;
				int32 TargetPinIndex = INDEX_NONE;
				int32 TargetEdgeIndex = INDEX_NONE;
				TArray<int32> Nodes;
				TArray<int32> EdgeIndices;
			};
			TArray<FCandidate> Candidates;

			TFunction<bool(int32, TSet<int32>&)> CanBecomeCompoundInputNode =
				[&](int32 NodeIndex, TSet<int32>& Visiting) -> bool
			{
				if (!Island.NodeSet.Contains(NodeIndex)
					|| !IsInputChainNodeCandidate(Nodes[NodeIndex])
					|| !AllDataOutgoing.IsValidIndex(NodeIndex)
					|| AllDataOutgoing[NodeIndex].Num() != 1
					|| Visiting.Contains(NodeIndex))
				{
					return false;
				}
				Visiting.Add(NodeIndex);
				if (AllDataIncoming.IsValidIndex(NodeIndex))
				{
					for (const int32 IncomingEdgeIndex : AllDataIncoming[NodeIndex])
					{
						if (!ExplicitEdgeSet.Contains(IncomingEdgeIndex) || !AllEdges.IsValidIndex(IncomingEdgeIndex))
						{
							Visiting.Remove(NodeIndex);
							return false;
						}
						const FEdge& IncomingEdge = AllEdges[IncomingEdgeIndex];
						if (!Island.NodeSet.Contains(IncomingEdge.Source)
							|| IncomingEdge.Target != NodeIndex
							|| IncomingEdge.bExec
							|| !Outgoing.IsValidIndex(IncomingEdge.Source)
							|| Outgoing[IncomingEdge.Source].Num() != 1
							|| Outgoing[IncomingEdge.Source][0] != IncomingEdgeIndex)
						{
							Visiting.Remove(NodeIndex);
							return false;
						}
						if (!CanBecomeCompoundInputNode(IncomingEdge.Source, Visiting))
						{
							Visiting.Remove(NodeIndex);
							return false;
						}
					}
				}
				Visiting.Remove(NodeIndex);
				return true;
			};

			for (int32 EdgeIndex = 0; EdgeIndex < AllEdges.Num(); ++EdgeIndex)
			{
				if (!ExplicitEdgeSet.Contains(EdgeIndex))
				{
					continue;
				}
				const FEdge& TargetEdge = AllEdges[EdgeIndex];
				if (!Island.NodeSet.Contains(TargetEdge.Source) || !Island.NodeSet.Contains(TargetEdge.Target))
				{
					continue;
				}
				if (!IsInputChainNodeCandidate(Nodes[TargetEdge.Source]))
				{
					continue;
				}
				if (!Outgoing.IsValidIndex(TargetEdge.Source)
					|| Outgoing[TargetEdge.Source].Num() != 1
					|| Outgoing[TargetEdge.Source][0] != EdgeIndex)
				{
					continue;
				}
				if (!AllDataOutgoing.IsValidIndex(TargetEdge.Source)
					|| AllDataOutgoing[TargetEdge.Source].Num() != 1
					|| AllDataOutgoing[TargetEdge.Source][0] != EdgeIndex)
				{
					continue;
				}
				TSet<int32> Visiting;
				if (!CanBecomeCompoundInputNode(TargetEdge.Source, Visiting))
				{
					continue;
				}

				FCandidate Candidate;
				Candidate.TargetNode = TargetEdge.Target;
				Candidate.TargetPinIndex = TargetEdge.TargetPinIndex;
				Candidate.TargetEdgeIndex = EdgeIndex;
				Candidate.Nodes.Add(TargetEdge.Source);
				Candidate.EdgeIndices.Add(EdgeIndex);
				Candidates.Add(MoveTemp(Candidate));
			}

			Candidates.Sort([](const FCandidate& A, const FCandidate& B)
			{
				if (A.Nodes.Num() != B.Nodes.Num())
				{
					return A.Nodes.Num() > B.Nodes.Num();
				}
				if (A.TargetNode != B.TargetNode)
				{
					return A.TargetNode < B.TargetNode;
				}
				return A.TargetPinIndex < B.TargetPinIndex;
			});

			TSet<int32> AssignedNodes;
			for (const FCandidate& Candidate : Candidates)
			{
				bool bOverlapsExistingChain = false;
				for (const int32 NodeIndex : Candidate.Nodes)
				{
					if (AssignedNodes.Contains(NodeIndex))
					{
						bOverlapsExistingChain = true;
						break;
					}
				}
				if (bOverlapsExistingChain)
				{
					continue;
				}

				FInputChain Chain;
				Chain.Id = Island.InputChains.Num();
				Chain.TargetNode = Candidate.TargetNode;
				Chain.TargetPinIndex = Candidate.TargetPinIndex;
				Chain.TargetEdgeIndex = Candidate.TargetEdgeIndex;
				Chain.Nodes = Candidate.Nodes;
				Chain.EdgeIndices = Candidate.EdgeIndices;
				Island.InputChains.Add(MoveTemp(Chain));
				for (const int32 NodeIndex : Candidate.Nodes)
				{
					AssignedNodes.Add(NodeIndex);
					Island.InputChainNodeSet.Add(NodeIndex);
				}
			}

			Diagnostics.InputChainCount += Island.InputChains.Num();
			Diagnostics.InputChainNodeCount += Island.InputChainNodeSet.Num();
		}

		static void AssignInputChainRanksAndCorridors(
			TArray<FNode>& Nodes,
			FIsland& Island,
			const TArray<int32>& RootTargets)
		{
			TMap<int32, TArray<int32>> ChainsByTargetNode;
			for (int32 ChainIndex = 0; ChainIndex < Island.InputChains.Num(); ++ChainIndex)
			{
				if (Island.InputChains.IsValidIndex(ChainIndex))
				{
					ChainsByTargetNode.FindOrAdd(Island.InputChains[ChainIndex].TargetNode).Add(ChainIndex);
				}
			}

			TSet<int32> VisitedTargets;
			TSet<int32> ActiveTargets;
			TFunction<void(int32)> AssignForTarget = [&](const int32 TargetNode)
			{
				if (!Nodes.IsValidIndex(TargetNode) || VisitedTargets.Contains(TargetNode) || ActiveTargets.Contains(TargetNode))
				{
					return;
				}
				const TArray<int32>* ChainIndices = ChainsByTargetNode.Find(TargetNode);
				if (!ChainIndices)
				{
					return;
				}
				ActiveTargets.Add(TargetNode);
				for (const int32 ChainIndex : *ChainIndices)
				{
					if (!Island.InputChains.IsValidIndex(ChainIndex))
					{
						continue;
					}
					FInputChain& Chain = Island.InputChains[ChainIndex];
					Chain.TargetRank = Nodes[TargetNode].Rank;
					Chain.CorridorIndex = Chain.TargetRank > 0 ? Chain.TargetRank - 1 : INDEX_NONE;
					for (int32 ChainNodeIndex = 0; ChainNodeIndex < Chain.Nodes.Num(); ++ChainNodeIndex)
					{
						const int32 NodeIndex = Chain.Nodes[ChainNodeIndex];
						if (!Nodes.IsValidIndex(NodeIndex))
						{
							continue;
						}
						const int32 DistanceToTarget = Chain.Nodes.Num() - ChainNodeIndex;
						Nodes[NodeIndex].Rank = FMath::Max(0, Chain.TargetRank - DistanceToTarget);
						Nodes[NodeIndex].Order = Nodes[TargetNode].Order + ChainNodeIndex;
						AssignForTarget(NodeIndex);
					}
				}
				ActiveTargets.Remove(TargetNode);
				VisitedTargets.Add(TargetNode);
			};

			for (const int32 RootTarget : RootTargets)
			{
				AssignForTarget(RootTarget);
			}
			for (FInputChain& Chain : Island.InputChains)
			{
				if (Chain.TargetRank == 0 && Nodes.IsValidIndex(Chain.TargetNode))
				{
					AssignForTarget(Chain.TargetNode);
				}
			}
		}

		static void AssignRanksInsideIsland(TArray<FNode>& Nodes, const TArray<FEdge>& Edges, const TArray<int32>& IslandNodes)
		{
			TSet<int32> IslandSet;
			for (const int32 NodeIndex : IslandNodes)
			{
				IslandSet.Add(NodeIndex);
			}
			for (const int32 NodeIndex : IslandNodes)
			{
				Nodes[NodeIndex].Rank = 0;
			}

			TSet<int32> NodesWithOutgoing;
			for (const FEdge& Edge : Edges)
			{
				if (IslandSet.Contains(Edge.Source) && IslandSet.Contains(Edge.Target))
				{
					NodesWithOutgoing.Add(Edge.Source);
				}
			}
			TMap<int32, int32> DepthFromOutput;
			for (const int32 NodeIndex : IslandNodes)
			{
				if (!NodesWithOutgoing.Contains(NodeIndex))
				{
					DepthFromOutput.Add(NodeIndex, 0);
				}
			}
			for (int32 Iter = 0; Iter < IslandNodes.Num(); ++Iter)
			{
				bool bChanged = false;
				for (const FEdge& Edge : Edges)
				{
					if (!IslandSet.Contains(Edge.Source) || !IslandSet.Contains(Edge.Target))
					{
						continue;
					}
					const int32* TargetDepth = DepthFromOutput.Find(Edge.Target);
					if (!TargetDepth)
					{
						continue;
					}
					const int32 Candidate = *TargetDepth + 1;
					const int32* Current = DepthFromOutput.Find(Edge.Source);
					if (!Current || Candidate > *Current)
					{
						DepthFromOutput.Add(Edge.Source, Candidate);
						bChanged = true;
					}
				}
				if (!bChanged)
				{
					break;
				}
			}
			int32 MaxDepth = 0;
			for (const int32 NodeIndex : IslandNodes)
			{
				const int32 Depth = DepthFromOutput.FindRef(NodeIndex);
				MaxDepth = FMath::Max(MaxDepth, Depth);
			}
			for (const int32 NodeIndex : IslandNodes)
			{
				if (const int32* Depth = DepthFromOutput.Find(NodeIndex))
				{
					Nodes[NodeIndex].Rank = MaxDepth - *Depth;
				}
			}

			int32 MinRank = TNumericLimits<int32>::Max();
			for (const int32 NodeIndex : IslandNodes)
			{
				MinRank = FMath::Min(MinRank, Nodes[NodeIndex].Rank);
			}
			if (MinRank != 0 && MinRank != TNumericLimits<int32>::Max())
			{
				for (const int32 NodeIndex : IslandNodes)
				{
					Nodes[NodeIndex].Rank -= MinRank;
				}
			}
		}

		static TArray<TArray<int32>> BuildColumns(const TArray<FNode>& Nodes, const TArray<int32>& IslandNodes)
		{
			int32 MaxRank = 0;
			for (const int32 NodeIndex : IslandNodes)
			{
				MaxRank = FMath::Max(MaxRank, Nodes[NodeIndex].Rank);
			}
			TArray<TArray<int32>> Columns;
			Columns.SetNum(MaxRank + 1);
			for (const int32 NodeIndex : IslandNodes)
			{
				Columns[Nodes[NodeIndex].Rank].Add(NodeIndex);
			}
			for (TArray<int32>& Column : Columns)
			{
				Column.Sort([&Nodes](int32 A, int32 B)
				{
					return Nodes[A].Index < Nodes[B].Index;
				});
				for (int32 Order = 0; Order < Column.Num(); ++Order)
				{
					// const_cast is avoided by setting order in caller after build.
				}
			}
			return Columns;
		}

		static double InputPortFraction(const FNode& Node, int32 PinIndex)
		{
			const FVector2D LocalAnchor = GetInputAnchor(Node, PinIndex);
			return Node.Height > 1.0
				? FMath::Clamp(LocalAnchor.Y / Node.Height, 0.0, 0.999)
				: 0.0;
		}

		static double OutputPortFraction(const FNode& Node, int32 PinIndex)
		{
			const FVector2D LocalAnchor = GetOutputAnchor(Node, PinIndex);
			return Node.Height > 1.0
				? FMath::Clamp(LocalAnchor.Y / Node.Height, 0.0, 0.999)
				: 0.0;
		}

		static bool DemandNearlyEqual(const FPortOrderDemand& A, const FPortOrderDemand& B)
		{
			return A.bValid == B.bValid
				&& A.GroupNode == B.GroupNode
				&& A.Distance == B.Distance
				&& FMath::IsNearlyEqual(A.Weight, B.Weight, 0.0001)
				&& FMath::IsNearlyEqual(A.Value, B.Value, 0.0001);
		}

		static FPortOrderDemand SelectSingleGroupDemand(const TArray<FPortOrderDemand>& Candidates)
		{
			FPortOrderDemand Result;
			TMap<int32, double> SumByGroup;
			TMap<int32, double> WeightByGroup;
			TMap<int32, int32> CountByGroup;
			TMap<int32, int32> MinDistanceByGroup;
			for (const FPortOrderDemand& Candidate : Candidates)
			{
				if (!Candidate.bValid || Candidate.GroupNode == INDEX_NONE)
				{
					continue;
				}
				const double Weight = Candidate.Weight > KINDA_SMALL_NUMBER
					? Candidate.Weight
					: ColumnDistanceInfluenceWeight(Candidate.Distance);
				SumByGroup.FindOrAdd(Candidate.GroupNode) += Candidate.Value * Weight;
				WeightByGroup.FindOrAdd(Candidate.GroupNode) += Weight;
				CountByGroup.FindOrAdd(Candidate.GroupNode) += 1;
				int32& MinDistance = MinDistanceByGroup.FindOrAdd(Candidate.GroupNode);
				if (MinDistance == 0 || Candidate.Distance < MinDistance)
				{
					MinDistance = Candidate.Distance;
				}
			}
			if (CountByGroup.Num() != 1)
			{
				return Result;
			}
			for (const TPair<int32, int32>& Pair : CountByGroup)
			{
				Result.bValid = Pair.Value > 0;
				Result.GroupNode = Pair.Key;
				Result.Value = WeightByGroup.FindRef(Pair.Key) > KINDA_SMALL_NUMBER
					? SumByGroup.FindRef(Pair.Key) / WeightByGroup.FindRef(Pair.Key)
					: 0.0;
				Result.Distance = MinDistanceByGroup.FindRef(Pair.Key);
				Result.Weight = WeightByGroup.FindRef(Pair.Key) > KINDA_SMALL_NUMBER
					? WeightByGroup.FindRef(Pair.Key)
					: ColumnDistanceInfluenceWeight(Result.Distance);
				break;
			}
			return Result;
		}

		static FPortOrderDemandSet MergePortOrderDemandCandidates(const TArray<FPortOrderDemand>& Candidates)
		{
			struct FGroupAccumulator
			{
				double WeightedSum = 0.0;
				double WeightSum = 0.0;
				int32 Count = 0;
				int32 MinDistance = MAX_int32;
			};

			TMap<int32, FGroupAccumulator> Accumulators;
			for (const FPortOrderDemand& Candidate : Candidates)
			{
				if (!Candidate.bValid || Candidate.GroupNode == INDEX_NONE)
				{
					continue;
				}
				FGroupAccumulator& Accumulator = Accumulators.FindOrAdd(Candidate.GroupNode);
				const double Weight = Candidate.Weight > KINDA_SMALL_NUMBER
					? Candidate.Weight
					: ColumnDistanceInfluenceWeight(Candidate.Distance);
				Accumulator.WeightedSum += Candidate.Value * Weight;
				Accumulator.WeightSum += Weight;
				++Accumulator.Count;
				Accumulator.MinDistance = FMath::Min(Accumulator.MinDistance, Candidate.Distance);
			}

			FPortOrderDemandSet Result;
			for (const TPair<int32, FGroupAccumulator>& Pair : Accumulators)
			{
				if (Pair.Value.Count <= 0)
				{
					continue;
				}
				FPortOrderDemand Demand;
				Demand.bValid = true;
				Demand.GroupNode = Pair.Key;
				Demand.Value = Pair.Value.WeightSum > KINDA_SMALL_NUMBER
					? Pair.Value.WeightedSum / Pair.Value.WeightSum
					: 0.0;
				Demand.Distance = Pair.Value.MinDistance == MAX_int32 ? 0 : Pair.Value.MinDistance;
				Demand.Weight = Pair.Value.WeightSum > KINDA_SMALL_NUMBER
					? Pair.Value.WeightSum
					: ColumnDistanceInfluenceWeight(Demand.Distance);
				Result.Demands.Add(Demand);
			}
			Result.Demands.Sort([](const FPortOrderDemand& A, const FPortOrderDemand& B)
			{
				if (A.Distance != B.Distance)
				{
					return A.Distance < B.Distance;
				}
				return A.GroupNode < B.GroupNode;
			});
			return Result;
		}

		static bool DemandSetNearlyEqual(const FPortOrderDemandSet& A, const FPortOrderDemandSet& B)
		{
			if (A.Demands.Num() != B.Demands.Num())
			{
				return false;
			}
			for (int32 Index = 0; Index < A.Demands.Num(); ++Index)
			{
				if (!DemandNearlyEqual(A.Demands[Index], B.Demands[Index]))
				{
					return false;
				}
			}
			return true;
		}

		static const FPortOrderDemand* FindPortOrderDemandByGroup(const FPortOrderDemandSet& Set, int32 GroupNode)
		{
			for (const FPortOrderDemand& Demand : Set.Demands)
			{
				if (Demand.bValid && Demand.GroupNode == GroupNode)
				{
					return &Demand;
				}
			}
			return nullptr;
		}

		static TArray<FPortOrderDemandSet> ComputeDownstreamPortOrderDemandSets(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<int32>& IslandNodes)
		{
			TArray<FPortOrderDemandSet> Demands;
			Demands.SetNum(Nodes.Num());
			TSet<int32> IslandSet;
			int32 MaxRank = 0;
			for (const int32 NodeIndex : IslandNodes)
			{
				IslandSet.Add(NodeIndex);
				if (Nodes.IsValidIndex(NodeIndex))
				{
					MaxRank = FMath::Max(MaxRank, Nodes[NodeIndex].Rank);
				}
			}

			TMap<int32, TSet<int32>> DistinctInputPinsByNode;
			for (const FEdge& Edge : Edges)
			{
				if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
				{
					continue;
				}
				TSet<int32>& Pins = DistinctInputPinsByNode.FindOrAdd(Edge.Target);
				Pins.Add(Edge.TargetPinIndex);
			}

			for (int32 Iter = 0; Iter < FMath::Max(1, MaxRank + 2); ++Iter)
			{
				bool bChanged = false;
				for (int32 Rank = MaxRank; Rank >= 0; --Rank)
				{
					for (const int32 NodeIndex : IslandNodes)
					{
						if (!Nodes.IsValidIndex(NodeIndex) || Nodes[NodeIndex].Rank != Rank)
						{
							continue;
						}
						TArray<FPortOrderDemand> Candidates;
						for (const FEdge& Edge : Edges)
						{
							if (Edge.Source != NodeIndex || !Nodes.IsValidIndex(Edge.Target))
							{
								continue;
							}
							bool bAdded = false;
							const TSet<int32>* TargetInputPins = DistinctInputPinsByNode.Find(Edge.Target);
							const bool bTargetSplitsByInputPort = TargetInputPins && TargetInputPins->Num() > 1;
							if (bTargetSplitsByInputPort)
							{
								FPortOrderDemand Direct;
								Direct.bValid = true;
								Direct.GroupNode = Edge.Target;
								Direct.Value = InputPortFraction(Nodes[Edge.Target], Edge.TargetPinIndex);
								Direct.Distance = 1;
								Direct.Weight = ColumnDistanceInfluenceWeight(Direct.Distance);
								Candidates.Add(Direct);
								bAdded = true;
							}
							if (IslandSet.Contains(Edge.Target) && Demands.IsValidIndex(Edge.Target))
							{
								for (FPortOrderDemand Propagated : Demands[Edge.Target].Demands)
								{
									Propagated.Distance += 1;
									Propagated.Weight = ColumnDistanceInfluenceWeight(Propagated.Distance);
									Candidates.Add(Propagated);
									bAdded = true;
								}
							}
							if (!bAdded)
							{
								FPortOrderDemand Direct;
								Direct.bValid = true;
								Direct.GroupNode = Edge.Target;
								Direct.Value = InputPortFraction(Nodes[Edge.Target], Edge.TargetPinIndex);
								Direct.Distance = 1;
								Direct.Weight = ColumnDistanceInfluenceWeight(Direct.Distance);
								Candidates.Add(Direct);
							}
						}
						const FPortOrderDemandSet NewDemand = MergePortOrderDemandCandidates(Candidates);
						if (!DemandSetNearlyEqual(Demands[NodeIndex], NewDemand))
						{
							Demands[NodeIndex] = NewDemand;
							bChanged = true;
						}
					}
				}
				if (!bChanged)
				{
					break;
				}
			}
			return Demands;
		}

		static TArray<FPortOrderDemandSet> ComputeSourcePortOrderDemandSets(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<int32>& IslandNodes)
		{
			TArray<FPortOrderDemandSet> Demands;
			Demands.SetNum(Nodes.Num());
			TSet<int32> IslandSet;
			int32 MaxRank = 0;
			for (const int32 NodeIndex : IslandNodes)
			{
				IslandSet.Add(NodeIndex);
				if (Nodes.IsValidIndex(NodeIndex))
				{
					MaxRank = FMath::Max(MaxRank, Nodes[NodeIndex].Rank);
				}
			}

			TMap<int32, TSet<int32>> DistinctOutputPinsByNode;
			for (const FEdge& Edge : Edges)
			{
				if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
				{
					continue;
				}
				TSet<int32>& Pins = DistinctOutputPinsByNode.FindOrAdd(Edge.Source);
				Pins.Add(Edge.SourcePinIndex);
			}

			for (int32 Iter = 0; Iter < FMath::Max(1, MaxRank + 2); ++Iter)
			{
				bool bChanged = false;
				for (int32 Rank = 0; Rank <= MaxRank; ++Rank)
				{
					for (const int32 NodeIndex : IslandNodes)
					{
						if (!Nodes.IsValidIndex(NodeIndex) || Nodes[NodeIndex].Rank != Rank)
						{
							continue;
						}
						TArray<FPortOrderDemand> Candidates;
						for (const FEdge& Edge : Edges)
						{
							if (Edge.Target != NodeIndex || !Nodes.IsValidIndex(Edge.Source))
							{
								continue;
							}

							bool bAdded = false;
							const TSet<int32>* SourceOutputPins = DistinctOutputPinsByNode.Find(Edge.Source);
							const bool bSourceSplitsByOutputPort = SourceOutputPins && SourceOutputPins->Num() > 1;
							if (bSourceSplitsByOutputPort)
							{
								FPortOrderDemand Direct;
								Direct.bValid = true;
								Direct.GroupNode = Edge.Source;
								Direct.Value = OutputPortFraction(Nodes[Edge.Source], Edge.SourcePinIndex);
								Direct.Distance = 1;
								Direct.Weight = ColumnDistanceInfluenceWeight(Direct.Distance);
								Candidates.Add(Direct);
								bAdded = true;
							}
							if (IslandSet.Contains(Edge.Source) && Demands.IsValidIndex(Edge.Source))
							{
								for (FPortOrderDemand Propagated : Demands[Edge.Source].Demands)
								{
									Propagated.Distance += 1;
									Propagated.Weight = ColumnDistanceInfluenceWeight(Propagated.Distance);
									Candidates.Add(Propagated);
									bAdded = true;
								}
							}
							if (!bAdded)
							{
								FPortOrderDemand Direct;
								Direct.bValid = true;
								Direct.GroupNode = Edge.Source;
								Direct.Value = OutputPortFraction(Nodes[Edge.Source], Edge.SourcePinIndex);
								Direct.Distance = 1;
								Direct.Weight = ColumnDistanceInfluenceWeight(Direct.Distance);
								Candidates.Add(Direct);
							}
						}
						const FPortOrderDemandSet NewDemand = MergePortOrderDemandCandidates(Candidates);
						if (!DemandSetNearlyEqual(Demands[NodeIndex], NewDemand))
						{
							Demands[NodeIndex] = NewDemand;
							bChanged = true;
						}
					}
				}
				if (!bChanged)
				{
					break;
				}
			}
			return Demands;
		}

		static TArray<FPortOrderDemand> ComputeDownstreamPortOrderDemands(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<int32>& IslandNodes)
		{
			const TArray<FPortOrderDemandSet> DemandSets = ComputeDownstreamPortOrderDemandSets(Nodes, Edges, IslandNodes);
			TArray<FPortOrderDemand> Result;
			Result.SetNum(DemandSets.Num());
			for (int32 Index = 0; Index < DemandSets.Num(); ++Index)
			{
				if (DemandSets[Index].Demands.Num() > 0)
				{
					Result[Index] = DemandSets[Index].Demands[0];
				}
			}
			return Result;
		}

		static TArray<FPortOrderDemand> ComputeSourcePortOrderDemands(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<int32>& IslandNodes)
		{
			const TArray<FPortOrderDemandSet> DemandSets = ComputeSourcePortOrderDemandSets(Nodes, Edges, IslandNodes);
			TArray<FPortOrderDemand> Result;
			Result.SetNum(DemandSets.Num());
			for (int32 Index = 0; Index < DemandSets.Num(); ++Index)
			{
				if (DemandSets[Index].Demands.Num() > 0)
				{
					Result[Index] = DemandSets[Index].Demands[0];
				}
			}
			return Result;
		}

		static TArray<FPortOrderDemandSet> ComputeExternalSourcePortOrderDemandSets(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& AllEdges,
			const TArray<FEdge>& ExplicitEdges,
			const TArray<int32>& IslandNodes)
		{
			TArray<FPortOrderDemandSet> Demands;
			Demands.SetNum(Nodes.Num());
			TSet<int32> IslandSet;
			int32 MaxRank = 0;
			for (const int32 NodeIndex : IslandNodes)
			{
				IslandSet.Add(NodeIndex);
				if (Nodes.IsValidIndex(NodeIndex))
				{
					MaxRank = FMath::Max(MaxRank, Nodes[NodeIndex].Rank);
				}
			}

			TMap<int32, TArray<FPortOrderDemand>> DirectDemandsByTarget;
			for (const FEdge& Edge : AllEdges)
			{
				if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
				{
					continue;
				}
				if (IslandSet.Contains(Edge.Source) || !IslandSet.Contains(Edge.Target))
				{
					continue;
				}

				FPortOrderDemand Direct;
				Direct.bValid = true;
				Direct.GroupNode = Edge.Source;
				Direct.Value = OutputPortFraction(Nodes[Edge.Source], Edge.SourcePinIndex);
				Direct.Distance = 1;
				Direct.Weight = ColumnDistanceInfluenceWeight(Direct.Distance);
				DirectDemandsByTarget.FindOrAdd(Edge.Target).Add(Direct);
			}

			for (int32 Iter = 0; Iter < FMath::Max(1, MaxRank + 2); ++Iter)
			{
				bool bChanged = false;
				for (int32 Rank = 0; Rank <= MaxRank; ++Rank)
				{
					for (const int32 NodeIndex : IslandNodes)
					{
						if (!Nodes.IsValidIndex(NodeIndex) || Nodes[NodeIndex].Rank != Rank)
						{
							continue;
						}

						TArray<FPortOrderDemand> Candidates;
						if (const TArray<FPortOrderDemand>* DirectDemands = DirectDemandsByTarget.Find(NodeIndex))
						{
							Candidates.Append(*DirectDemands);
						}

						for (const FEdge& Edge : ExplicitEdges)
						{
							if (Edge.Target != NodeIndex || !IslandSet.Contains(Edge.Source) || !Nodes.IsValidIndex(Edge.Source))
							{
								continue;
							}
							if (!Demands.IsValidIndex(Edge.Source))
							{
								continue;
							}
							for (FPortOrderDemand Propagated : Demands[Edge.Source].Demands)
							{
								Propagated.Distance += 1;
								Propagated.Weight = ColumnDistanceInfluenceWeight(Propagated.Distance);
								Candidates.Add(Propagated);
							}
						}

						const FPortOrderDemandSet NewDemand = MergePortOrderDemandCandidates(Candidates);
						if (!DemandSetNearlyEqual(Demands[NodeIndex], NewDemand))
						{
							Demands[NodeIndex] = NewDemand;
							bChanged = true;
						}
					}
				}
				if (!bChanged)
				{
					break;
				}
			}
			return Demands;
		}

		static bool ComparePortOrderDemand(const FPortOrderDemand& A, const FPortOrderDemand& B, bool& bOutLess)
		{
			if (A.bValid && B.bValid && A.GroupNode == B.GroupNode && !FMath::IsNearlyEqual(A.Value, B.Value, 0.001))
			{
				bOutLess = A.Value < B.Value;
				return true;
			}
			return false;
		}

		static bool ComparePortOrderDemandSets(const FPortOrderDemandSet& A, const FPortOrderDemandSet& B, bool& bOutLess)
		{
			const FPortOrderDemand* BestA = nullptr;
			const FPortOrderDemand* BestB = nullptr;
			double BestPairWeight = -1.0;
			int32 BestMaxDistance = MAX_int32;
			int32 BestTotalDistance = MAX_int32;
			for (const FPortOrderDemand& DemandA : A.Demands)
			{
				if (!DemandA.bValid)
				{
					continue;
				}
				const FPortOrderDemand* DemandB = FindPortOrderDemandByGroup(B, DemandA.GroupNode);
				if (!DemandB || FMath::IsNearlyEqual(DemandA.Value, DemandB->Value, 0.001))
				{
					continue;
				}
				const int32 MaxDistance = FMath::Max(DemandA.Distance, DemandB->Distance);
				const int32 TotalDistance = DemandA.Distance + DemandB->Distance;
				const double WeightA = DemandA.Weight > KINDA_SMALL_NUMBER
					? DemandA.Weight
					: ColumnDistanceInfluenceWeight(DemandA.Distance);
				const double WeightB = DemandB->Weight > KINDA_SMALL_NUMBER
					? DemandB->Weight
					: ColumnDistanceInfluenceWeight(DemandB->Distance);
				const double PairWeight = WeightA * WeightB;
				const bool bBetter =
					!BestA
					|| PairWeight > BestPairWeight + 0.0001
					|| (FMath::IsNearlyEqual(PairWeight, BestPairWeight, 0.0001) && MaxDistance < BestMaxDistance)
					|| (FMath::IsNearlyEqual(PairWeight, BestPairWeight, 0.0001) && MaxDistance == BestMaxDistance && TotalDistance < BestTotalDistance)
					|| (FMath::IsNearlyEqual(PairWeight, BestPairWeight, 0.0001) && MaxDistance == BestMaxDistance && TotalDistance == BestTotalDistance && DemandA.GroupNode < BestA->GroupNode);
				if (bBetter)
				{
					BestA = &DemandA;
					BestB = DemandB;
					BestPairWeight = PairWeight;
					BestMaxDistance = MaxDistance;
					BestTotalDistance = TotalDistance;
				}
			}
			if (!BestA || !BestB)
			{
				return false;
			}
			bOutLess = BestA->Value < BestB->Value;
			return true;
		}

		static void SortColumnByKnownNeighbor(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FPortOrderDemandSet>& SourceDemands,
			const TArray<FPortOrderDemandSet>& TargetDemands,
			const TArray<FPortOrderDemandSet>* ExternalSourceDemands,
			const TArray<int32>& KnownColumn,
			TArray<int32>& TargetColumn,
			bool bKnownIsLeft)
		{
			TMap<int32, int32> KnownOrder;
			for (int32 Index = 0; Index < KnownColumn.Num(); ++Index)
			{
				KnownOrder.Add(KnownColumn[Index], Index);
			}
			auto KnownEndpointPortFraction = [&Nodes, bKnownIsLeft](const FEdge& Edge) -> double
			{
				const int32 KnownNodeIndex = bKnownIsLeft ? Edge.Source : Edge.Target;
				if (!Nodes.IsValidIndex(KnownNodeIndex))
				{
					return 0.0;
				}
				const FNode& KnownNode = Nodes[KnownNodeIndex];
				return bKnownIsLeft
					? OutputPortFraction(KnownNode, Edge.SourcePinIndex)
					: InputPortFraction(KnownNode, Edge.TargetPinIndex);
			};
			auto MakeRightTargetInputKey = [](const FEdge& Edge) -> FString
			{
				return FString::Printf(TEXT("%d|%d|%s"), Edge.Target, Edge.TargetPinIndex, *Edge.TargetPin);
			};
			TMap<int32, FColumnSortKey> KeyByTarget;
			for (const int32 TargetNode : TargetColumn)
			{
				double Sum = 0.0;
				double PortSum = 0.0;
				int32 Count = 0;
				for (const FEdge& Edge : Edges)
				{
					const bool bMatch = bKnownIsLeft
						? (Edge.Target == TargetNode && KnownOrder.Contains(Edge.Source))
						: (Edge.Source == TargetNode && KnownOrder.Contains(Edge.Target));
					if (bMatch)
					{
						Sum += KnownOrder.FindRef(bKnownIsLeft ? Edge.Source : Edge.Target);
						PortSum += KnownEndpointPortFraction(Edge);
						++Count;
					}
				}
				FColumnSortKey Key;
				Key.bHasNeighbor = Count > 0;
				Key.NeighborOrder = Count > 0
					? Sum / static_cast<double>(Count)
					: static_cast<double>(Nodes[TargetNode].Index);
				Key.NeighborPortFraction = Count > 0
					? PortSum / static_cast<double>(Count)
					: 0.0;
				Key.StableIndex = Nodes[TargetNode].Index;
				if (SourceDemands.IsValidIndex(TargetNode))
				{
					Key.SourceDemands = SourceDemands[TargetNode];
				}
				if (TargetDemands.IsValidIndex(TargetNode))
				{
					Key.TargetDemands = TargetDemands[TargetNode];
				}
				if (ExternalSourceDemands && ExternalSourceDemands->IsValidIndex(TargetNode))
				{
					Key.ExternalSourceDemands = (*ExternalSourceDemands)[TargetNode];
				}
				for (const FEdge& Edge : Edges)
				{
					if (Edge.Source != TargetNode || !Nodes.IsValidIndex(Edge.Target) || !Nodes.IsValidIndex(Edge.Source))
					{
						continue;
					}
					if (Nodes[Edge.Target].Rank <= Nodes[Edge.Source].Rank)
					{
						continue;
					}
					Key.RightTargetInputKeys.Add(MakeRightTargetInputKey(Edge));
				}
				KeyByTarget.Add(TargetNode, Key);
			}
			TargetColumn.Sort([&KeyByTarget, &Nodes](int32 A, int32 B)
			{
				const FColumnSortKey KeyA = KeyByTarget.FindRef(A);
				const FColumnSortKey KeyB = KeyByTarget.FindRef(B);
				bool bDemandLess = false;
				if (ComparePortOrderDemandSets(KeyA.SourceDemands, KeyB.SourceDemands, bDemandLess))
				{
					return bDemandLess;
				}
				if (ComparePortOrderDemandSets(KeyA.TargetDemands, KeyB.TargetDemands, bDemandLess))
				{
					return bDemandLess;
				}
				bool bSharesRightTargetInput = false;
				for (const FString& RightTargetInputKey : KeyA.RightTargetInputKeys)
				{
					if (KeyB.RightTargetInputKeys.Contains(RightTargetInputKey))
					{
						bSharesRightTargetInput = true;
						break;
					}
				}
				if (bSharesRightTargetInput
					&& ComparePortOrderDemandSets(KeyA.ExternalSourceDemands, KeyB.ExternalSourceDemands, bDemandLess))
				{
					return bDemandLess;
				}
				if (KeyA.bHasNeighbor && KeyB.bHasNeighbor)
				{
					if (!FMath::IsNearlyEqual(KeyA.NeighborOrder, KeyB.NeighborOrder, 0.001))
					{
						return KeyA.NeighborOrder < KeyB.NeighborOrder;
					}
					if (!FMath::IsNearlyEqual(KeyA.NeighborPortFraction, KeyB.NeighborPortFraction, 0.001))
					{
						return KeyA.NeighborPortFraction < KeyB.NeighborPortFraction;
					}
				}
				return KeyA.StableIndex < KeyB.StableIndex;
			});
		}

		static void MinimizeColumnCrossings(
			TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FPortOrderDemandSet>& SourceDemands,
			const TArray<FPortOrderDemandSet>& TargetDemands,
			const TArray<FPortOrderDemandSet>* ExternalSourceDemands,
			TArray<TArray<int32>>& Columns)
		{
			for (int32 Pass = 0; Pass < 4; ++Pass)
			{
				for (int32 ColumnIndex = 1; ColumnIndex < Columns.Num(); ++ColumnIndex)
				{
					SortColumnByKnownNeighbor(Nodes, Edges, SourceDemands, TargetDemands, ExternalSourceDemands, Columns[ColumnIndex - 1], Columns[ColumnIndex], true);
				}
				for (int32 ColumnIndex = Columns.Num() - 2; ColumnIndex >= 0; --ColumnIndex)
				{
					SortColumnByKnownNeighbor(Nodes, Edges, SourceDemands, TargetDemands, ExternalSourceDemands, Columns[ColumnIndex + 1], Columns[ColumnIndex], false);
				}
			}
			for (int32 ColumnIndex = 0; ColumnIndex < Columns.Num(); ++ColumnIndex)
			{
				for (int32 Order = 0; Order < Columns[ColumnIndex].Num(); ++Order)
				{
					Nodes[Columns[ColumnIndex][Order]].Order = Order;
				}
			}
		}

		static void PrepareIslandColumnsAndOrder(
			TArray<FNode>& Nodes,
			const TArray<FEdge>& AllEdges,
			const TArray<FEdge>& ExplicitEdges,
			const TSet<int32>& RankIgnoredEdges,
			FIsland& Island,
			FDesignDiagnostics& Diagnostics)
		{
			ExtractInputChains(Nodes, AllEdges, RankIgnoredEdges, Island, Diagnostics);
			TArray<int32> CoreNodes;
			for (const int32 NodeIndex : Island.Nodes)
			{
				if (!Island.InputChainNodeSet.Contains(NodeIndex))
				{
					CoreNodes.Add(NodeIndex);
				}
			}
			if (CoreNodes.Num() == 0)
			{
				CoreNodes = Island.Nodes;
				Island.InputChains.Reset();
				Island.InputChainNodeSet.Reset();
			}
			AssignRanksInsideIsland(Nodes, ExplicitEdges, CoreNodes);
			AssignInputChainRanksAndCorridors(Nodes, Island, CoreNodes);
			Island.Columns = BuildColumns(Nodes, CoreNodes);
			const TArray<FPortOrderDemandSet> SourceDemands = ComputeSourcePortOrderDemandSets(Nodes, ExplicitEdges, CoreNodes);
			const TArray<FPortOrderDemandSet> TargetDemands = ComputeDownstreamPortOrderDemandSets(Nodes, ExplicitEdges, CoreNodes);
			const TArray<FPortOrderDemandSet> ExternalSourceDemands = ComputeExternalSourcePortOrderDemandSets(Nodes, AllEdges, ExplicitEdges, CoreNodes);
			MinimizeColumnCrossings(Nodes, ExplicitEdges, SourceDemands, TargetDemands, &ExternalSourceDemands, Island.Columns);
			Island.ColumnInternalBusLaneDemand.SetNum(FMath::Max(0, Island.Columns.Num() - 1));
			for (int32& Demand : Island.ColumnInternalBusLaneDemand)
			{
				Demand = 0;
			}
		}

		static void AssignInternalBusLaneDemand(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			TArray<FIsland>& Islands,
			const TArray<FIslandEdge>& IslandEdges)
		{
			TSet<FString> SeenGroups;
			for (const FIslandEdge& IslandEdge : IslandEdges)
			{
				if (!Edges.IsValidIndex(IslandEdge.EdgeIndex) || !Islands.IsValidIndex(IslandEdge.TargetIsland))
				{
					continue;
				}
				const FEdge& Edge = Edges[IslandEdge.EdgeIndex];
				if (!Nodes.IsValidIndex(Edge.Target))
				{
					continue;
				}
				const int32 TargetColumn = Nodes[Edge.Target].Rank;
				if (TargetColumn <= 0)
				{
					continue;
				}
				const FString GroupKey = FString::Printf(TEXT("%d|%s|%d"), IslandEdge.TargetIsland, *MakeSourceParamId(Edge), TargetColumn);
				if (SeenGroups.Contains(GroupKey))
				{
					continue;
				}
				SeenGroups.Add(GroupKey);
				FIsland& Island = Islands[IslandEdge.TargetIsland];
				const int32 CorridorIndex = TargetColumn - 1;
				if (Island.ColumnInternalBusLaneDemand.IsValidIndex(CorridorIndex))
				{
					++Island.ColumnInternalBusLaneDemand[CorridorIndex];
				}
			}
		}
