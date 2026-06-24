		struct FRootInfluenceModel
		{
			TMap<int32, TSet<int32>> OutputsByRoot;
			TSet<int32> ScoringTargets;
			TMap<FString, TArray<double>> InterfaceOrderByRootTarget;
			TMap<FString, double> WeightByRootTarget;
		};

		static double ComputeRootOrderInversionScore(
			const TArray<int32>& Order,
			const TMap<int32, TSet<int32>>& OutputsByRoot,
			const TMap<FString, TArray<double>>& InterfaceOrderByRootTarget,
			const TMap<FString, double>& WeightByRootTarget)
		{
			TSet<int32> Targets;
			for (const TPair<int32, TSet<int32>>& Pair : OutputsByRoot)
			{
				for (const int32 Target : Pair.Value)
				{
					Targets.Add(Target);
				}
			}
			double InversionScore = 0.0;
			for (const int32 Target : Targets)
			{
				TArray<int32> ParticipatingRoots;
				for (const int32 Root : Order)
				{
					const TSet<int32>* Outputs = OutputsByRoot.Find(Root);
					if (Outputs && Outputs->Contains(Target))
					{
						ParticipatingRoots.Add(Root);
					}
				}
				for (int32 A = 0; A < ParticipatingRoots.Num(); ++A)
				{
					for (int32 B = A + 1; B < ParticipatingRoots.Num(); ++B)
					{
						const FString KeyA = MakeRootTargetKey(ParticipatingRoots[A], Target);
						const FString KeyB = MakeRootTargetKey(ParticipatingRoots[B], Target);
						if (CompareInterfaceOrderTuples(
							InterfaceOrderByRootTarget.Find(KeyA),
							InterfaceOrderByRootTarget.Find(KeyB)) > 0)
						{
							InversionScore += GetRootTargetWeight(WeightByRootTarget, ParticipatingRoots[A], Target)
								* GetRootTargetWeight(WeightByRootTarget, ParticipatingRoots[B], Target);
						}
					}
				}
			}
			return InversionScore;
		}

		static double ComputeRootContinuousScoreUpperBound(
			const TArray<int32>& Prefix,
			const TArray<int32>& Remaining,
			const TSet<int32>& ScoringTargets,
			const TMap<int32, TSet<int32>>& OutputsByRoot,
			const TMap<FString, double>& WeightByRootTarget)
		{
			double UpperBound = 0.0;
			for (const int32 Target : ScoringTargets)
			{
				double ClosedScore = 0.0;
				double OpenRunWeight = 0.0;
				for (const int32 Root : Prefix)
				{
					const TSet<int32>* Outputs = OutputsByRoot.Find(Root);
					const bool bHasTarget = Outputs && Outputs->Contains(Target);
					if (bHasTarget)
					{
						OpenRunWeight += GetRootTargetWeight(WeightByRootTarget, Root, Target);
					}
					else if (OpenRunWeight > 0)
					{
						ClosedScore += OpenRunWeight * OpenRunWeight;
						OpenRunWeight = 0;
					}
				}
				double RemainingTargetWeight = 0.0;
				for (const int32 Root : Remaining)
				{
					const TSet<int32>* Outputs = OutputsByRoot.Find(Root);
					if (Outputs && Outputs->Contains(Target))
					{
						RemainingTargetWeight += GetRootTargetWeight(WeightByRootTarget, Root, Target);
					}
				}
				const double BestOpenRun = OpenRunWeight + RemainingTargetWeight;
				UpperBound += ClosedScore + BestOpenRun * BestOpenRun;
			}
			return UpperBound;
		}

		static void ScoreRootIslandOrder(
			const TArray<int32>& Order,
			const TMap<int32, TSet<int32>>& OutputsByRoot,
			const TMap<FString, TArray<double>>& InterfaceOrderByRootTarget,
			const TMap<FString, double>& WeightByRootTarget,
			double& OutContinuousScore,
			double& OutInversionScore)
		{
			TSet<int32> Targets;
			for (const TPair<int32, TSet<int32>>& Pair : OutputsByRoot)
			{
				for (const int32 Target : Pair.Value)
				{
					Targets.Add(Target);
				}
			}
			OutContinuousScore = 0.0;
			OutInversionScore = 0.0;
			for (const int32 Target : Targets)
			{
				double RunWeight = 0.0;
				TArray<int32> ParticipatingRoots;
				for (const int32 Root : Order)
				{
					const TSet<int32>* Outputs = OutputsByRoot.Find(Root);
					const bool bHasTarget = Outputs && Outputs->Contains(Target);
					if (bHasTarget)
					{
						RunWeight += GetRootTargetWeight(WeightByRootTarget, Root, Target);
						ParticipatingRoots.Add(Root);
					}
					else if (RunWeight > 0)
					{
						OutContinuousScore += RunWeight * RunWeight;
						RunWeight = 0;
					}
				}
				if (RunWeight > 0)
				{
					OutContinuousScore += RunWeight * RunWeight;
				}
				for (int32 A = 0; A < ParticipatingRoots.Num(); ++A)
				{
					for (int32 B = A + 1; B < ParticipatingRoots.Num(); ++B)
					{
						const FString KeyA = MakeRootTargetKey(ParticipatingRoots[A], Target);
						const FString KeyB = MakeRootTargetKey(ParticipatingRoots[B], Target);
						if (CompareInterfaceOrderTuples(
							InterfaceOrderByRootTarget.Find(KeyA),
							InterfaceOrderByRootTarget.Find(KeyB)) > 0)
						{
							OutInversionScore += GetRootTargetWeight(WeightByRootTarget, ParticipatingRoots[A], Target)
								* GetRootTargetWeight(WeightByRootTarget, ParticipatingRoots[B], Target);
						}
					}
				}
			}
		}

		static void GetConnectionTargetInterfaceYsForIslandEdge(
			const TArray<FIslandEdge>& IslandEdges,
			const TArray<FDeterministicIslandConnection>& Connections,
			const TMultiMap<int32, int32>& ConnectionIndicesByEdgeIndex,
			const int32 IslandEdgeIndex,
			TArray<double>& OutTargetYs)
		{
			OutTargetYs.Reset();
			if (!IslandEdges.IsValidIndex(IslandEdgeIndex))
			{
				return;
			}
			TArray<int32> ConnectionIndices;
			ConnectionIndicesByEdgeIndex.MultiFind(IslandEdges[IslandEdgeIndex].EdgeIndex, ConnectionIndices);
			for (const int32 ConnectionIndex : ConnectionIndices)
			{
				if (Connections.IsValidIndex(ConnectionIndex))
				{
					OutTargetYs.Add(Connections[ConnectionIndex].Endpoint.ActualTargetLocal.Y);
				}
			}
			if (OutTargetYs.Num() == 0)
			{
				OutTargetYs.Add(IslandEdges[IslandEdgeIndex].TargetLocal.Y);
			}
		}

		static FRootInfluenceModel BuildRootInfluenceModel(
			const TArray<FIsland>& Islands,
			const TArray<FIslandEdge>& IslandEdges,
			const TArray<FDeterministicIslandConnection>& Connections,
			const TSet<int32>& RouteOnlyIslandEdgeIndices,
			const TArray<int32>& TopoOrder,
			const TArray<int32>& Roots)
		{
			FRootInfluenceModel Model;

			TArray<TArray<int32>> OutgoingByIsland;
			TArray<TArray<int32>> IncomingByIsland;
			OutgoingByIsland.SetNum(Islands.Num());
			IncomingByIsland.SetNum(Islands.Num());
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
				OutgoingByIsland[IslandEdge.SourceIsland].Add(IslandEdgeIndex);
				IncomingByIsland[IslandEdge.TargetIsland].Add(IslandEdgeIndex);
			}

			TMultiMap<int32, int32> ConnectionIndicesByEdgeIndex;
			for (int32 ConnectionIndex = 0; ConnectionIndex < Connections.Num(); ++ConnectionIndex)
			{
				if (!Connections[ConnectionIndex].bRouteOnly)
				{
					ConnectionIndicesByEdgeIndex.Add(Connections[ConnectionIndex].Endpoint.EdgeIndex, ConnectionIndex);
				}
			}

			TArray<TMap<int32, int32>> MinDepthByIslandRoot;
			TArray<TMap<int32, TArray<double>>> EntryYsByIslandRoot;
			MinDepthByIslandRoot.SetNum(Islands.Num());
			EntryYsByIslandRoot.SetNum(Islands.Num());
			for (const int32 Root : Roots)
			{
				if (MinDepthByIslandRoot.IsValidIndex(Root))
				{
					MinDepthByIslandRoot[Root].Add(Root, 0);
				}
			}

			TArray<int32> EffectiveTopoOrder = TopoOrder;
			for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
			{
				if (!EffectiveTopoOrder.Contains(IslandIndex))
				{
					EffectiveTopoOrder.Add(IslandIndex);
				}
			}

			for (const int32 IslandIndex : EffectiveTopoOrder)
			{
				if (!Islands.IsValidIndex(IslandIndex))
				{
					continue;
				}
				const TMap<int32, int32> SourceDepths = MinDepthByIslandRoot[IslandIndex];
				if (SourceDepths.Num() == 0)
				{
					continue;
				}
				for (const int32 IslandEdgeIndex : OutgoingByIsland[IslandIndex])
				{
					const FIslandEdge& IslandEdge = IslandEdges[IslandEdgeIndex];
					const int32 TargetIsland = IslandEdge.TargetIsland;
					if (!MinDepthByIslandRoot.IsValidIndex(TargetIsland))
					{
						continue;
					}
					TArray<double> TargetYs;
					GetConnectionTargetInterfaceYsForIslandEdge(IslandEdges, Connections, ConnectionIndicesByEdgeIndex, IslandEdgeIndex, TargetYs);
					for (const TPair<int32, int32>& RootDepth : SourceDepths)
					{
						const int32 Root = RootDepth.Key;
						const int32 NewDepth = RootDepth.Value + 1;
						int32* ExistingDepth = MinDepthByIslandRoot[TargetIsland].Find(Root);
						const bool bReplaceDepth = ExistingDepth == nullptr || NewDepth < *ExistingDepth;
						const bool bSameDepth = ExistingDepth != nullptr && NewDepth == *ExistingDepth;
						if (!bReplaceDepth && !bSameDepth)
						{
							continue;
						}
						if (bReplaceDepth)
						{
							MinDepthByIslandRoot[TargetIsland].Add(Root, NewDepth);
							EntryYsByIslandRoot[TargetIsland].FindOrAdd(Root).Reset();
						}
						TArray<double>& EntryYs = EntryYsByIslandRoot[TargetIsland].FindOrAdd(Root);
						for (const double TargetY : TargetYs)
						{
							EntryYs.Add(TargetY);
						}
					}
				}
			}

			for (int32 TargetIsland = 0; TargetIsland < Islands.Num(); ++TargetIsland)
			{
				if (MinDepthByIslandRoot[TargetIsland].Num() <= 1)
				{
					continue;
				}
				TSet<int32> UnionRoots;
				TArray<TSet<int32>> IncomingRootSets;
				for (const int32 IslandEdgeIndex : IncomingByIsland[TargetIsland])
				{
					const FIslandEdge& IslandEdge = IslandEdges[IslandEdgeIndex];
					TSet<int32> IncomingRoots;
					if (MinDepthByIslandRoot.IsValidIndex(IslandEdge.SourceIsland))
					{
						for (const TPair<int32, int32>& RootDepth : MinDepthByIslandRoot[IslandEdge.SourceIsland])
						{
							IncomingRoots.Add(RootDepth.Key);
							UnionRoots.Add(RootDepth.Key);
						}
					}
					if (IncomingRoots.Num() > 0)
					{
						IncomingRootSets.Add(MoveTemp(IncomingRoots));
					}
				}
				if (UnionRoots.Num() <= 1)
				{
					continue;
				}
				bool bIncomingAlreadyCarriesFullUnion = false;
				for (const TSet<int32>& IncomingRoots : IncomingRootSets)
				{
					if (RootSetEquals(IncomingRoots, UnionRoots))
					{
						bIncomingAlreadyCarriesFullUnion = true;
						break;
					}
				}
				if (!bIncomingAlreadyCarriesFullUnion)
				{
					Model.ScoringTargets.Add(TargetIsland);
				}
			}

			for (const int32 TargetIsland : Model.ScoringTargets)
			{
				for (const TPair<int32, int32>& RootDepth : MinDepthByIslandRoot[TargetIsland])
				{
					const int32 Root = RootDepth.Key;
					Model.OutputsByRoot.FindOrAdd(Root).Add(TargetIsland);
					const FString Key = MakeRootTargetKey(Root, TargetIsland);
					Model.WeightByRootTarget.Add(Key, RootInfluenceDepthWeight(RootDepth.Value));
					TArray<double> InterfaceYs;
					if (const TArray<double>* StoredYs = EntryYsByIslandRoot[TargetIsland].Find(Root))
					{
						InterfaceYs = *StoredYs;
					}
					if (InterfaceYs.Num() == 0)
					{
						InterfaceYs.Add(IslandAabb(Islands[TargetIsland]).CenterY());
					}
					SortUniqueInterfaceYs(InterfaceYs);
					Model.InterfaceOrderByRootTarget.Add(Key, MoveTemp(InterfaceYs));
				}
			}
			return Model;
		}

		static TArray<int32> ComputeRootIslandContinuousOrder(
			const TArray<FIsland>& Islands,
			const TArray<FIslandEdge>& IslandEdges,
			const TArray<FDeterministicIslandConnection>& Connections,
			const TSet<int32>& RouteOnlyIslandEdgeIndices,
			const TArray<int32>& TopoOrder,
			FRunResult& Result)
		{
			TSet<int32> NonRouteIncomingTargets;
			for (int32 IslandEdgeIndex = 0; IslandEdgeIndex < IslandEdges.Num(); ++IslandEdgeIndex)
			{
				if (!RouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex) && Islands.IsValidIndex(IslandEdges[IslandEdgeIndex].TargetIsland))
				{
					NonRouteIncomingTargets.Add(IslandEdges[IslandEdgeIndex].TargetIsland);
				}
			}
			TArray<int32> Roots;
			for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
			{
				if (!NonRouteIncomingTargets.Contains(IslandIndex))
				{
					Roots.Add(IslandIndex);
				}
			}
			Roots.Sort([&Islands](const int32 A, const int32 B)
			{
				const FRect RectA = IslandAabb(Islands[A]);
				const FRect RectB = IslandAabb(Islands[B]);
				if (!FMath::IsNearlyEqual(RectA.Top, RectB.Top, 0.5))
				{
					return RectA.Top < RectB.Top;
				}
				return A < B;
			});

			const FRootInfluenceModel Influence = BuildRootInfluenceModel(Islands, IslandEdges, Connections, RouteOnlyIslandEdgeIndices, TopoOrder, Roots);
			const TMap<int32, TSet<int32>>& OutputsByRoot = Influence.OutputsByRoot;
			const TMap<FString, TArray<double>>& InterfaceOrderByRootTarget = Influence.InterfaceOrderByRootTarget;
			const TMap<FString, double>& WeightByRootTarget = Influence.WeightByRootTarget;
			const TSet<int32>& ScoringTargets = Influence.ScoringTargets;
			Result.DesignDiagnostics.RootIslandCount = Roots.Num();
			Result.DesignDiagnostics.RootScoringTargetCount = ScoringTargets.Num();
			Result.DesignDiagnostics.RootEquivalentGroupCount = Roots.Num();

			TArray<TArray<int32>> Components;
			TSet<int32> Unvisited;
			for (const int32 Root : Roots)
			{
				Unvisited.Add(Root);
			}
			while (Unvisited.Num() > 0)
			{
				int32 Seed = INDEX_NONE;
				for (const int32 Root : Unvisited)
				{
					Seed = Root;
					break;
				}
				TArray<int32> Queue;
				TArray<int32> Component;
				Queue.Add(Seed);
				Unvisited.Remove(Seed);
				while (Queue.Num() > 0)
				{
					const int32 Root = Queue.Pop(EAllowShrinking::No);
					Component.Add(Root);
					const TSet<int32>* Outputs = OutputsByRoot.Find(Root);
					if (!Outputs)
					{
						continue;
					}
					for (const int32 Other : Roots)
					{
						if (!Unvisited.Contains(Other))
						{
							continue;
						}
						const TSet<int32>* OtherOutputs = OutputsByRoot.Find(Other);
						if (!OtherOutputs)
						{
							continue;
						}
						bool bSharesOutput = false;
						for (const int32 Target : *Outputs)
						{
							if (OtherOutputs->Contains(Target))
							{
								bSharesOutput = true;
								break;
							}
						}
						if (bSharesOutput)
						{
							Unvisited.Remove(Other);
							Queue.Add(Other);
						}
					}
				}
				Component.Sort([&Islands](int32 A, int32 B)
				{
					const FRect RectA = IslandAabb(Islands[A]);
					const FRect RectB = IslandAabb(Islands[B]);
					if (!FMath::IsNearlyEqual(RectA.Top, RectB.Top, 0.5))
					{
						return RectA.Top < RectB.Top;
					}
					return A < B;
				});
				Components.Add(Component);
			}
			Result.DesignDiagnostics.RootShareComponentCount = Components.Num();
			Components.Sort([&Islands, &OutputsByRoot, &WeightByRootTarget](const TArray<int32>& A, const TArray<int32>& B)
			{
				auto CountComponentOutputs = [&OutputsByRoot, &WeightByRootTarget](const TArray<int32>& Component) -> double
				{
					double Count = 0.0;
					for (const int32 Root : Component)
					{
						if (const TSet<int32>* Outputs = OutputsByRoot.Find(Root))
						{
							for (const int32 Target : *Outputs)
							{
								Count += GetRootTargetWeight(WeightByRootTarget, Root, Target);
							}
						}
					}
					return Count;
				};
				const double OutputCountA = CountComponentOutputs(A);
				const double OutputCountB = CountComponentOutputs(B);
				if (!FMath::IsNearlyEqual(OutputCountA, OutputCountB, 0.001))
				{
					return OutputCountA > OutputCountB;
				}
				const int32 IslandA = A.Num() > 0 ? A[0] : INDEX_NONE;
				const int32 IslandB = B.Num() > 0 ? B[0] : INDEX_NONE;
				if (!Islands.IsValidIndex(IslandA) || !Islands.IsValidIndex(IslandB))
				{
					return IslandA < IslandB;
				}
				const FRect RectA = IslandAabb(Islands[IslandA]);
				const FRect RectB = IslandAabb(Islands[IslandB]);
				if (!FMath::IsNearlyEqual(RectA.Top, RectB.Top, 0.5))
				{
					return RectA.Top < RectB.Top;
				}
				return IslandA < IslandB;
			});

			TArray<int32> FinalOrder;
			for (const TArray<int32>& Component : Components)
			{
				TArray<int32> BestOrder = Component;
				double BestScore = -1.0;
				double BestInversion = TNumericLimits<double>::Max();
				constexpr int32 ExactRootLimit = 11;
				if (Component.Num() > ExactRootLimit)
				{
					++Result.DesignDiagnostics.RootContinuousOrderExactLimitExceeded;
				}
				else
				{
					TArray<int32> Prefix;
					TArray<int32> Remaining = Component;
					TFunction<void()> Search = [&]()
					{
						const double UpperBound = ComputeRootContinuousScoreUpperBound(Prefix, Remaining, ScoringTargets, OutputsByRoot, WeightByRootTarget);
						if (BestScore >= 0.0 && UpperBound < BestScore - 0.5)
						{
							++Result.DesignDiagnostics.RootContinuousOrderPrunedStateCount;
							return;
						}
						if (BestScore >= 0.0
							&& FMath::IsNearlyEqual(UpperBound, BestScore, 0.5)
							&& ComputeRootOrderInversionScore(Prefix, OutputsByRoot, InterfaceOrderByRootTarget, WeightByRootTarget) > BestInversion)
						{
							++Result.DesignDiagnostics.RootContinuousOrderPrunedStateCount;
							return;
						}
						if (Remaining.Num() == 0)
						{
							double Score = 0.0;
							double Inversion = 0.0;
							ScoreRootIslandOrder(Prefix, OutputsByRoot, InterfaceOrderByRootTarget, WeightByRootTarget, Score, Inversion);
							if (Score > BestScore + 0.5 || (FMath::IsNearlyEqual(Score, BestScore, 0.5) && Inversion < BestInversion))
							{
								BestScore = Score;
								BestInversion = Inversion;
								BestOrder = Prefix;
							}
							return;
						}
						TArray<int32> BranchOrder = Remaining;
						BranchOrder.Sort([&OutputsByRoot, &WeightByRootTarget, &Prefix](const int32 A, const int32 B)
						{
							double SharedWithTailA = 0.0;
							double SharedWithTailB = 0.0;
							if (Prefix.Num() > 0)
							{
								const TSet<int32>* TailOutputs = OutputsByRoot.Find(Prefix.Last());
								const TSet<int32>* OutputsA = OutputsByRoot.Find(A);
								const TSet<int32>* OutputsB = OutputsByRoot.Find(B);
								if (TailOutputs && OutputsA)
								{
									for (const int32 Target : *OutputsA)
									{
										if (TailOutputs->Contains(Target))
										{
											SharedWithTailA += GetRootTargetWeight(WeightByRootTarget, A, Target);
										}
									}
								}
								if (TailOutputs && OutputsB)
								{
									for (const int32 Target : *OutputsB)
									{
										if (TailOutputs->Contains(Target))
										{
											SharedWithTailB += GetRootTargetWeight(WeightByRootTarget, B, Target);
										}
									}
								}
							}
							if (!FMath::IsNearlyEqual(SharedWithTailA, SharedWithTailB, 0.001))
							{
								return SharedWithTailA > SharedWithTailB;
							}
							const TSet<int32>* OutputsA = OutputsByRoot.Find(A);
							const TSet<int32>* OutputsB = OutputsByRoot.Find(B);
							double OutputCountA = 0.0;
							double OutputCountB = 0.0;
							if (OutputsA)
							{
								for (const int32 Target : *OutputsA)
								{
									OutputCountA += GetRootTargetWeight(WeightByRootTarget, A, Target);
								}
							}
							if (OutputsB)
							{
								for (const int32 Target : *OutputsB)
								{
									OutputCountB += GetRootTargetWeight(WeightByRootTarget, B, Target);
								}
							}
							if (!FMath::IsNearlyEqual(OutputCountA, OutputCountB, 0.001))
							{
								return OutputCountA > OutputCountB;
							}
							return A < B;
						});
						for (const int32 Root : BranchOrder)
						{
							const int32 Index = Remaining.IndexOfByKey(Root);
							if (Index == INDEX_NONE)
							{
								continue;
							}
							Remaining.RemoveAt(Index, 1, EAllowShrinking::No);
							Prefix.Add(Root);
							Search();
							Prefix.Pop(EAllowShrinking::No);
							Remaining.Insert(Root, Index);
						}
					};
					Search();
					Result.DesignDiagnostics.RootContinuousOrderScore += FMath::Max(0.0, BestScore);
					if (BestInversion < TNumericLimits<double>::Max())
					{
						Result.DesignDiagnostics.RootContinuousOrderInversionScore += BestInversion;
					}
				}
				FinalOrder.Append(BestOrder);
			}
			return FinalOrder;
		}

		static bool IslandRectsCollideWithPlaced(
			const TArray<FRect>& CandidateRects,
			const TArray<FRect>& PlacedRects,
			const double Padding)
		{
			for (const FRect& CandidateRect : CandidateRects)
			{
				for (const FRect& PlacedRect : PlacedRects)
				{
					if (CandidateRect.Intersects(PlacedRect, Padding))
					{
						return true;
					}
				}
			}
			return false;
		}

		static TArray<FRect> BuildIslandHardRectsAt(
			const TArray<FNode>& Nodes,
			const FIsland& Island,
			const double X,
			const double Y,
			const FOptions& Options)
		{
			FIsland Temp = Island;
			Temp.X = X;
			Temp.Y = Y;
			return BuildIslandHardOccupancyRects(Nodes, Temp, Options);
		}

		static TArray<FRect> BuildIslandPlacementOccupancyRects(
			const TArray<FNode>& Nodes,
			const FIsland& Island,
			const FOptions& Options)
		{
			TArray<FRect> Rects;
			const double Padding = FMath::Max(0.0, Options.NodePadding);
			for (const int32 NodeIndex : Island.Nodes)
			{
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
			if (Rects.Num() == 0)
			{
				Rects.Add(IslandAabb(Island));
			}
			return Rects;
		}

		static TArray<FRect> BuildIslandPlacementRectsAt(
			const TArray<FNode>& Nodes,
			const FIsland& Island,
			const double X,
			const double Y,
			const FOptions& Options)
		{
			FIsland Temp = Island;
			Temp.X = X;
			Temp.Y = Y;
			return BuildIslandPlacementOccupancyRects(Nodes, Temp, Options);
		}

		static double FindNearestNonCollidingIslandY(
			const TArray<FNode>& Nodes,
			const FIsland& Island,
			const double PreferredY,
			const TArray<FRect>& PlacedRects,
			const FOptions& Options)
		{
			TArray<double> CandidateYs;
			auto AddCandidateY = [&CandidateYs](const double Y)
			{
				if (!FMath::IsFinite(Y))
				{
					return;
				}
				for (const double Existing : CandidateYs)
				{
					if (FMath::IsNearlyEqual(Existing, Y, 0.5))
					{
						return;
					}
				}
				CandidateYs.Add(SnapGraphCoordinate(Y));
			};
			AddCandidateY(PreferredY);
			FIsland LocalIsland = Island;
			LocalIsland.X = 0.0;
			LocalIsland.Y = 0.0;
			const TArray<FRect> LocalRects = BuildIslandPlacementOccupancyRects(Nodes, LocalIsland, Options);
			const double Gap = FMath::Max(Options.NodePadding, GetBusAvoidanceDistance());
			for (const FRect& PlacedRect : PlacedRects)
			{
				for (const FRect& LocalRect : LocalRects)
				{
					const double CandidateLeft = Island.X + LocalRect.Left;
					const double CandidateRight = Island.X + LocalRect.Right;
					if (!IntervalOverlaps(CandidateLeft, CandidateRight, PlacedRect.Left, PlacedRect.Right, 0.0))
					{
						continue;
					}
					AddCandidateY(PlacedRect.Top - Gap - LocalRect.Bottom);
					AddCandidateY(PlacedRect.Bottom + Gap - LocalRect.Top);
				}
			}
			CandidateYs.Sort([PreferredY](double A, double B)
			{
				const double ResidualA = FMath::Abs(A - PreferredY);
				const double ResidualB = FMath::Abs(B - PreferredY);
				if (!FMath::IsNearlyEqual(ResidualA, ResidualB, 0.5))
				{
					return ResidualA < ResidualB;
				}
				if ((A < PreferredY) != (B < PreferredY))
				{
					return A < PreferredY;
				}
				return A < B;
			});
			for (const double CandidateY : CandidateYs)
			{
				const TArray<FRect> CandidateRects = BuildIslandPlacementRectsAt(Nodes, Island, Island.X, CandidateY, Options);
				if (!IslandRectsCollideWithPlaced(CandidateRects, PlacedRects, 0.0))
				{
					return CandidateY;
				}
			}
			return SnapGraphCoordinate(PreferredY);
		}

		static void PlaceIslandsYByDeterministicTopo(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			TArray<FIsland>& Islands,
			const TArray<FIslandEdge>& IslandEdges,
			const TArray<FDeterministicIslandConnection>& Connections,
			const TSet<int32>& RouteOnlyIslandEdgeIndices,
			const TArray<int32>& ComponentByIsland,
			const TArray<int32>& TopoOrder,
			const FOptions& Options,
			FRunResult& Result,
			TArray<TArray<int32>>& OutTopologyColumns)
		{
			const double RootGap = FMath::Max(Options.NodePadding, GetBusAvoidanceDistance());
			Result.DesignDiagnostics.RootCompactVerticalGap = RootGap;
			TMap<int32, FRect> IslandLocalOccupiedBounds;
			auto GetIslandLocalPlacementBounds = [&](const int32 IslandIndex) -> FRect
			{
				if (const FRect* Existing = IslandLocalOccupiedBounds.Find(IslandIndex))
				{
					return *Existing;
				}
				FIsland LocalIsland = Islands[IslandIndex];
				LocalIsland.X = 0.0;
				LocalIsland.Y = 0.0;
				const FRect Bounds = BoundsForRects(BuildIslandPlacementOccupancyRects(Nodes, LocalIsland, Options));
				IslandLocalOccupiedBounds.Add(IslandIndex, Bounds);
				return Bounds;
			};
			TMap<int32, int32> ConnectionByEdge;
			for (int32 ConnectionIndex = 0; ConnectionIndex < Connections.Num(); ++ConnectionIndex)
			{
				ConnectionByEdge.Add(Connections[ConnectionIndex].Endpoint.EdgeIndex, ConnectionIndex);
			}

			TArray<TArray<int32>> TopologyColumns;
			TArray<uint8> AssignedToColumn;
			AssignedToColumn.Init(0, Islands.Num());
			TArray<int32> Remaining = TopoOrder;
			while (Remaining.Num() > 0)
			{
				TArray<int32> Ready;
				for (const int32 IslandIndex : Remaining)
				{
					if (!Islands.IsValidIndex(IslandIndex))
					{
						continue;
					}
					bool bReady = true;
					for (const int32 IslandEdgeIndex : Islands[IslandIndex].IncomingEdges)
					{
						if (RouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex) || !IslandEdges.IsValidIndex(IslandEdgeIndex))
						{
							continue;
						}
						const int32 SourceIsland = IslandEdges[IslandEdgeIndex].SourceIsland;
						if (!AssignedToColumn.IsValidIndex(SourceIsland) || !AssignedToColumn[SourceIsland])
						{
							bReady = false;
							break;
						}
					}
					if (bReady)
					{
						Ready.Add(IslandIndex);
					}
				}
				if (Ready.Num() == 0)
				{
					++Result.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount;
					return;
				}
				TopologyColumns.Add(Ready);
				for (const int32 IslandIndex : Ready)
				{
					if (AssignedToColumn.IsValidIndex(IslandIndex))
					{
						AssignedToColumn[IslandIndex] = 1;
					}
					Remaining.Remove(IslandIndex);
				}
			}

			TMap<int32, int32> ColumnIndexByIsland;
			TMap<int32, int32> OrderIndexByIsland;
			for (int32 OrderIndex = 0; OrderIndex < TopoOrder.Num(); ++OrderIndex)
			{
				OrderIndexByIsland.Add(TopoOrder[OrderIndex], OrderIndex);
			}
			for (int32 ColumnIndex = 0; ColumnIndex < TopologyColumns.Num(); ++ColumnIndex)
			{
				for (const int32 IslandIndex : TopologyColumns[ColumnIndex])
				{
					ColumnIndexByIsland.Add(IslandIndex, ColumnIndex);
				}
			}

			auto BuildInterfaceOrderValue = [&](
				const int32 NeighborIsland,
				const double NeighborInterfaceY,
				const double LocalInterfaceY) -> double
			{
				const int32 NeighborColumn = ColumnIndexByIsland.Contains(NeighborIsland) ? ColumnIndexByIsland[NeighborIsland] : 0;
				const int32 NeighborOrder = OrderIndexByIsland.Contains(NeighborIsland) ? OrderIndexByIsland[NeighborIsland] : NeighborIsland;
				return static_cast<double>(NeighborColumn) * 100000000.0
					+ static_cast<double>(NeighborOrder) * 100000.0
					+ NeighborInterfaceY * 10.0
					+ LocalInterfaceY * 0.01;
			};

			auto SortTopologyColumnByBidirectionalContinuity = [&](TArray<int32>& Column, const int32 ColumnIndex)
			{
				if (Column.Num() <= 1)
				{
					return;
				}

				TMap<int32, TSet<int32>> OutputsByRoot;
				TMap<FString, TArray<double>> InterfaceOrderByRootTarget;
				TMap<FString, double> WeightByRootTarget;
				TSet<int32> ScoringTargets;
				auto AddOrderingInfluence = [&](
					const int32 IslandIndex,
					const int32 TargetKey,
					const double InterfaceOrder,
					const double Weight)
				{
					OutputsByRoot.FindOrAdd(IslandIndex).Add(TargetKey);
					ScoringTargets.Add(TargetKey);
					const FString Key = MakeRootTargetKey(IslandIndex, TargetKey);
					InterfaceOrderByRootTarget.FindOrAdd(Key).Add(InterfaceOrder);
					WeightByRootTarget.FindOrAdd(Key) += FMath::Max(KINDA_SMALL_NUMBER, Weight);
				};

				for (const int32 IslandIndex : Column)
				{
					if (!Islands.IsValidIndex(IslandIndex))
					{
						continue;
					}
					for (const int32 IslandEdgeIndex : Islands[IslandIndex].IncomingEdges)
					{
						if (RouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex) || !IslandEdges.IsValidIndex(IslandEdgeIndex))
						{
							continue;
						}
						const FIslandEdge& IslandEdge = IslandEdges[IslandEdgeIndex];
						const int32* SourceColumnPtr = ColumnIndexByIsland.Find(IslandEdge.SourceIsland);
						if (!SourceColumnPtr || *SourceColumnPtr >= ColumnIndex)
						{
							continue;
						}
						const int32* ConnectionIndexPtr = ConnectionByEdge.Find(IslandEdge.EdgeIndex);
						if (!ConnectionIndexPtr || !Connections.IsValidIndex(*ConnectionIndexPtr))
						{
							continue;
						}
						if (!Edges.IsValidIndex(IslandEdge.EdgeIndex))
						{
							continue;
						}
						const FDeterministicIslandConnection& Connection = Connections[*ConnectionIndexPtr];
						const FEdge& Edge = Edges[IslandEdge.EdgeIndex];
						const int32 TargetKey = Edge.Source * 2;
						const double Weight = ColumnDistanceInfluenceWeight(FMath::Abs(ColumnIndex - *SourceColumnPtr));
						AddOrderingInfluence(
							IslandIndex,
							TargetKey,
							BuildInterfaceOrderValue(IslandEdge.SourceIsland, Connection.Endpoint.SourceLocal.Y, Connection.Endpoint.EntryLocal.Y),
							Weight);
					}
					for (const int32 IslandEdgeIndex : Islands[IslandIndex].OutgoingEdges)
					{
						if (RouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex) || !IslandEdges.IsValidIndex(IslandEdgeIndex))
						{
							continue;
						}
						const FIslandEdge& IslandEdge = IslandEdges[IslandEdgeIndex];
						const int32* TargetColumnPtr = ColumnIndexByIsland.Find(IslandEdge.TargetIsland);
						if (!TargetColumnPtr || *TargetColumnPtr <= ColumnIndex)
						{
							continue;
						}
						const int32* ConnectionIndexPtr = ConnectionByEdge.Find(IslandEdge.EdgeIndex);
						if (!ConnectionIndexPtr || !Connections.IsValidIndex(*ConnectionIndexPtr))
						{
							continue;
						}
						if (!Edges.IsValidIndex(IslandEdge.EdgeIndex))
						{
							continue;
						}
						const FDeterministicIslandConnection& Connection = Connections[*ConnectionIndexPtr];
						const FEdge& Edge = Edges[IslandEdge.EdgeIndex];
						const int32 TargetKey = Edge.Target * 2 + 1;
						const double Weight = ColumnDistanceInfluenceWeight(FMath::Abs(*TargetColumnPtr - ColumnIndex));
						AddOrderingInfluence(
							IslandIndex,
							TargetKey,
							BuildInterfaceOrderValue(IslandEdge.TargetIsland, Connection.Endpoint.EntryLocal.Y, Connection.Endpoint.SourceLocal.Y),
							Weight);
					}
				}

				if (ScoringTargets.Num() == 0)
				{
					Column.Sort([&](const int32 A, const int32 B)
					{
						const int32 OrderA = OrderIndexByIsland.Contains(A) ? OrderIndexByIsland[A] : A;
						const int32 OrderB = OrderIndexByIsland.Contains(B) ? OrderIndexByIsland[B] : B;
						return OrderA != OrderB ? OrderA < OrderB : A < B;
					});
					return;
				}

				for (TPair<FString, TArray<double>>& Pair : InterfaceOrderByRootTarget)
				{
					SortUniqueInterfaceYs(Pair.Value);
				}

				const int32 ExactLimit = 11;
				if (Column.Num() <= ExactLimit)
				{
					TArray<int32> BestOrder = Column;
					double BestScore = -1.0;
					double BestInversion = TNumericLimits<double>::Max();
					TFunction<void(TArray<int32>&, TArray<int32>&)> Search;
					Search = [&](TArray<int32>& Prefix, TArray<int32>& Open)
					{
						if (BestScore >= 0.0)
						{
							const double UpperBound = ComputeRootContinuousScoreUpperBound(Prefix, Open, ScoringTargets, OutputsByRoot, WeightByRootTarget);
							if (UpperBound < BestScore - 0.5)
							{
								return;
							}
						}
						if (Open.Num() == 0)
						{
							double Score = 0.0;
							double Inversion = 0.0;
							ScoreRootIslandOrder(Prefix, OutputsByRoot, InterfaceOrderByRootTarget, WeightByRootTarget, Score, Inversion);
							if (Score > BestScore + 0.5 || (FMath::IsNearlyEqual(Score, BestScore, 0.5) && Inversion < BestInversion))
							{
								BestScore = Score;
								BestInversion = Inversion;
								BestOrder = Prefix;
							}
							return;
						}

						TArray<int32> BranchOrder = Open;
						BranchOrder.Sort([&](const int32 A, const int32 B)
						{
							double SharedA = 0.0;
							double SharedB = 0.0;
							if (Prefix.Num() > 0)
							{
								const int32 Last = Prefix.Last();
								const TSet<int32>* LastTargets = OutputsByRoot.Find(Last);
								if (LastTargets)
								{
									if (const TSet<int32>* TargetsA = OutputsByRoot.Find(A))
									{
										for (const int32 Target : *TargetsA)
										{
											if (LastTargets->Contains(Target))
											{
												SharedA += GetRootTargetWeight(WeightByRootTarget, Last, Target)
													+ GetRootTargetWeight(WeightByRootTarget, A, Target);
											}
										}
									}
									if (const TSet<int32>* TargetsB = OutputsByRoot.Find(B))
									{
										for (const int32 Target : *TargetsB)
										{
											if (LastTargets->Contains(Target))
											{
												SharedB += GetRootTargetWeight(WeightByRootTarget, Last, Target)
													+ GetRootTargetWeight(WeightByRootTarget, B, Target);
											}
										}
									}
								}
							}
							if (!FMath::IsNearlyEqual(SharedA, SharedB, 0.5))
							{
								return SharedA > SharedB;
							}
							const int32 OrderA = OrderIndexByIsland.Contains(A) ? OrderIndexByIsland[A] : A;
							const int32 OrderB = OrderIndexByIsland.Contains(B) ? OrderIndexByIsland[B] : B;
							return OrderA != OrderB ? OrderA < OrderB : A < B;
						});

						for (const int32 Candidate : BranchOrder)
						{
							const int32 OpenIndex = Open.IndexOfByKey(Candidate);
							if (OpenIndex == INDEX_NONE)
							{
								continue;
							}
							Prefix.Add(Candidate);
							Open.RemoveAt(OpenIndex, 1, EAllowShrinking::No);
							Search(Prefix, Open);
							Open.Insert(Candidate, OpenIndex);
							Prefix.Pop(EAllowShrinking::No);
						}
					};

					TArray<int32> Prefix;
					TArray<int32> Open = Column;
					Search(Prefix, Open);
					Column = BestOrder;
				}
				else
				{
					TMap<int32, double> WeightedOrderByIsland;
					for (const int32 IslandIndex : Column)
					{
						double WeightedSum = 0.0;
						double WeightSum = 0.0;
						if (const TSet<int32>* Targets = OutputsByRoot.Find(IslandIndex))
						{
							for (const int32 Target : *Targets)
							{
								const FString Key = MakeRootTargetKey(IslandIndex, Target);
								const TArray<double>* Values = InterfaceOrderByRootTarget.Find(Key);
								const double Weight = GetRootTargetWeight(WeightByRootTarget, IslandIndex, Target);
								if (Values && Values->Num() > 0)
								{
									double ValueSum = 0.0;
									for (const double Value : *Values)
									{
										ValueSum += Value;
									}
									WeightedSum += (ValueSum / static_cast<double>(Values->Num())) * Weight;
									WeightSum += Weight;
								}
							}
						}
						if (WeightSum > 0.0)
						{
							WeightedOrderByIsland.Add(IslandIndex, WeightedSum / WeightSum);
						}
					}
					Column.Sort([&](const int32 A, const int32 B)
					{
						const double OrderA = WeightedOrderByIsland.Contains(A) ? WeightedOrderByIsland[A] : static_cast<double>(OrderIndexByIsland.Contains(A) ? OrderIndexByIsland[A] : A);
						const double OrderB = WeightedOrderByIsland.Contains(B) ? WeightedOrderByIsland[B] : static_cast<double>(OrderIndexByIsland.Contains(B) ? OrderIndexByIsland[B] : B);
						if (!FMath::IsNearlyEqual(OrderA, OrderB, 0.5))
						{
							return OrderA < OrderB;
						}
						const int32 StableA = OrderIndexByIsland.Contains(A) ? OrderIndexByIsland[A] : A;
						const int32 StableB = OrderIndexByIsland.Contains(B) ? OrderIndexByIsland[B] : B;
						return StableA != StableB ? StableA < StableB : A < B;
					});
				}
			};

			for (int32 ColumnIndex = 0; ColumnIndex < TopologyColumns.Num(); ++ColumnIndex)
			{
				SortTopologyColumnByBidirectionalContinuity(TopologyColumns[ColumnIndex], ColumnIndex);
				for (int32 OrderIndex = 0; OrderIndex < TopologyColumns[ColumnIndex].Num(); ++OrderIndex)
				{
					OrderIndexByIsland.Add(TopologyColumns[ColumnIndex][OrderIndex], OrderIndex);
				}
			}

			auto IslandHasAnyOutput = [&](const int32 IslandIndex) -> bool
			{
				if (!Islands.IsValidIndex(IslandIndex))
				{
					return false;
				}
				for (const int32 IslandEdgeIndex : Islands[IslandIndex].OutgoingEdges)
				{
					if (IslandEdges.IsValidIndex(IslandEdgeIndex))
					{
						return true;
					}
				}
				return false;
			};

			for (int32 ColumnIndex = 0; ColumnIndex < TopologyColumns.Num(); ++ColumnIndex)
			{
				bool bHasOutputColumnRight = false;
				bool bHasNoOutputColumnLeft = false;
				double OutputColumnRightX = Options.OriginX;
				double NoOutputColumnLeftX = Options.OriginX;
				for (const int32 IslandIndex : TopologyColumns[ColumnIndex])
				{
					if (!Islands.IsValidIndex(IslandIndex))
					{
						continue;
					}
					if (ColumnIndex == 0)
					{
						Islands[IslandIndex].X = Options.OriginX;
					}
					else
					{
						Islands[IslandIndex].X = ComputeIslandLeftXFromPlacedParents(
							Islands,
							IslandEdges,
							Connections,
							RouteOnlyIslandEdgeIndices,
							ConnectionByEdge,
							IslandIndex,
							Options,
							&Result,
							true);
					}
					const FRect Bounds = GetIslandLocalPlacementBounds(IslandIndex);
					if (IslandHasAnyOutput(IslandIndex))
					{
						const double RightX = Islands[IslandIndex].X + Bounds.Right;
						OutputColumnRightX = bHasOutputColumnRight ? FMath::Max(OutputColumnRightX, RightX) : RightX;
						bHasOutputColumnRight = true;
					}
					else
					{
						NoOutputColumnLeftX = bHasNoOutputColumnLeft ? FMath::Max(NoOutputColumnLeftX, Islands[IslandIndex].X) : Islands[IslandIndex].X;
						bHasNoOutputColumnLeft = true;
					}
				}
				if (bHasOutputColumnRight || bHasNoOutputColumnLeft)
				{
					for (const int32 IslandIndex : TopologyColumns[ColumnIndex])
					{
						if (!Islands.IsValidIndex(IslandIndex))
						{
							continue;
						}
						if (IslandHasAnyOutput(IslandIndex))
						{
							if (!bHasOutputColumnRight)
							{
								continue;
							}
							const FRect Bounds = GetIslandLocalPlacementBounds(IslandIndex);
							Islands[IslandIndex].X = SnapGraphCoordinate(OutputColumnRightX - Bounds.Right);
						}
						else
						{
							if (!bHasNoOutputColumnLeft)
							{
								continue;
							}
							Islands[IslandIndex].X = SnapGraphCoordinate(NoOutputColumnLeftX);
						}
					}
				}
			}

			auto ComputeColumnCompactHeight = [&](const TArray<int32>& Column) -> double
			{
				double Total = 0.0;
				int32 ValidCount = 0;
				for (const int32 IslandIndex : Column)
				{
					if (!Islands.IsValidIndex(IslandIndex))
					{
						continue;
					}
					Total += FMath::Max(1.0, GetIslandLocalPlacementBounds(IslandIndex).Height());
					++ValidCount;
				}
				if (ValidCount > 1)
				{
					Total += RootGap * static_cast<double>(ValidCount - 1);
				}
				return Total;
			};

			int32 BaseColumnIndex = INDEX_NONE;
			double BestColumnHeight = -1.0;
			for (int32 ColumnIndex = 0; ColumnIndex < TopologyColumns.Num(); ++ColumnIndex)
			{
				const double Height = ComputeColumnCompactHeight(TopologyColumns[ColumnIndex]);
				if (Height > BestColumnHeight + 0.5)
				{
					BestColumnHeight = Height;
					BaseColumnIndex = ColumnIndex;
				}
			}
			Result.DesignDiagnostics.IslandLeftwardReanchorBaseColumn = BaseColumnIndex;
			Result.DesignDiagnostics.PlacementComponentCount = FMath::Max(Result.DesignDiagnostics.PlacementComponentCount, TopologyColumns.Num());

			TArray<uint8> YPlaced;
			YPlaced.Init(0, Islands.Num());

			auto ComputePreferredTopFromPlacedParents = [&](
				const int32 IslandIndex,
				const TSet<int32>* SourceFilter,
				double& OutPreferredTop) -> bool
			{
				if (!Islands.IsValidIndex(IslandIndex))
				{
					return false;
				}
				FInterfaceBundleAnchorAccumulator Anchor;
				for (const int32 IslandEdgeIndex : Islands[IslandIndex].IncomingEdges)
				{
					if (RouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex) || !IslandEdges.IsValidIndex(IslandEdgeIndex))
					{
						continue;
					}
					const FIslandEdge& IslandEdge = IslandEdges[IslandEdgeIndex];
					if (!YPlaced.IsValidIndex(IslandEdge.SourceIsland) || !YPlaced[IslandEdge.SourceIsland])
					{
						continue;
					}
					if (SourceFilter != nullptr && SourceFilter->Num() > 0 && !SourceFilter->Contains(IslandEdge.SourceIsland))
					{
						continue;
					}
					const int32* ConnectionIndexPtr = ConnectionByEdge.Find(IslandEdge.EdgeIndex);
					if (!ConnectionIndexPtr || !Connections.IsValidIndex(*ConnectionIndexPtr) || !Islands.IsValidIndex(IslandEdge.SourceIsland))
					{
						continue;
					}
					const FDeterministicIslandConnection& Connection = Connections[*ConnectionIndexPtr];
					const double FixedInterfaceY = Islands[IslandEdge.SourceIsland].Y + Connection.Endpoint.SourceLocal.Y;
					const double AnchorOffsetY = Connection.Endpoint.EntryLocal.Y;
					Anchor.Add(FixedInterfaceY, AnchorOffsetY);
				}
				if (!Anchor.ResolvePreferredOriginY(OutPreferredTop))
				{
					return false;
				}
				return true;
			};
			auto ComputePreferredTopFromPlacedChildren = [&](
				const int32 IslandIndex,
				const TSet<int32>* TargetFilter,
				double& OutPreferredTop) -> bool
			{
				if (!Islands.IsValidIndex(IslandIndex))
				{
					return false;
				}
				FInterfaceBundleAnchorAccumulator Anchor;
				for (const int32 IslandEdgeIndex : Islands[IslandIndex].OutgoingEdges)
				{
					if (RouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex) || !IslandEdges.IsValidIndex(IslandEdgeIndex))
					{
						continue;
					}
					const FIslandEdge& IslandEdge = IslandEdges[IslandEdgeIndex];
					if (!YPlaced.IsValidIndex(IslandEdge.TargetIsland) || !YPlaced[IslandEdge.TargetIsland])
					{
						continue;
					}
					if (TargetFilter != nullptr && TargetFilter->Num() > 0 && !TargetFilter->Contains(IslandEdge.TargetIsland))
					{
						continue;
					}
					const int32* ConnectionIndexPtr = ConnectionByEdge.Find(IslandEdge.EdgeIndex);
					if (!ConnectionIndexPtr || !Connections.IsValidIndex(*ConnectionIndexPtr) || !Islands.IsValidIndex(IslandEdge.TargetIsland))
					{
						continue;
					}
					const FDeterministicIslandConnection& Connection = Connections[*ConnectionIndexPtr];
					const double FixedInterfaceY = Islands[IslandEdge.TargetIsland].Y + Connection.Endpoint.EntryLocal.Y;
					const double AnchorOffsetY = Connection.Endpoint.SourceLocal.Y;
					Anchor.Add(FixedInterfaceY, AnchorOffsetY);
				}
				if (!Anchor.ResolvePreferredOriginY(OutPreferredTop))
				{
					return false;
				}
				return true;
			};

			auto BuildCompactFallbackOriginY = [&](const TArray<int32>& Column, const int32 TargetIsland) -> double
			{
				double CursorY = Options.OriginY;
				for (const int32 IslandIndex : Column)
				{
					if (!Islands.IsValidIndex(IslandIndex))
					{
						continue;
					}
					const FRect Bounds = GetIslandLocalPlacementBounds(IslandIndex);
					if (IslandIndex == TargetIsland)
					{
						return SnapGraphCoordinate(CursorY - Bounds.Top);
					}
					CursorY += FMath::Max(1.0, Bounds.Height()) + RootGap;
				}
				return Options.OriginY;
			};

			auto PlaceBaseColumnCompact = [&](const int32 ColumnIndex)
			{
				if (!TopologyColumns.IsValidIndex(ColumnIndex))
				{
					return;
				}
				double CursorY = Options.OriginY;
				for (const int32 IslandIndex : TopologyColumns[ColumnIndex])
				{
					if (!Islands.IsValidIndex(IslandIndex))
					{
						continue;
					}
					const FRect Bounds = GetIslandLocalPlacementBounds(IslandIndex);
					Islands[IslandIndex].Y = SnapGraphCoordinate(CursorY - Bounds.Top);
					Islands[IslandIndex].PreferredY = Islands[IslandIndex].Y;
					if (YPlaced.IsValidIndex(IslandIndex))
					{
						YPlaced[IslandIndex] = 1;
					}
					CursorY += FMath::Max(1.0, Bounds.Height()) + RootGap;
				}
			};

			auto PlaceColumnAnchoredToFixed = [&](
				const int32 CurrentColumnIndex,
				const int32 FixedColumnIndex,
				const bool bAnchorFromParents)
			{
				if (!TopologyColumns.IsValidIndex(CurrentColumnIndex) || !TopologyColumns.IsValidIndex(FixedColumnIndex))
				{
					return;
				}
				const TArray<int32>& CurrentColumn = TopologyColumns[CurrentColumnIndex];
				const TArray<int32>& FixedColumn = TopologyColumns[FixedColumnIndex];
				if (CurrentColumn.Num() == 0 || FixedColumn.Num() == 0)
				{
					return;
				}
				TSet<int32> FixedColumnSet;
				for (const int32 FixedIsland : FixedColumn)
				{
					if (Islands.IsValidIndex(FixedIsland) && YPlaced.IsValidIndex(FixedIsland) && YPlaced[FixedIsland])
					{
						FixedColumnSet.Add(FixedIsland);
					}
				}
				if (FixedColumnSet.Num() == 0)
				{
					return;
				}
				TArray<FColumnItem> ColumnItems;
				ColumnItems.Reserve(CurrentColumn.Num());
				for (const int32 IslandIndex : CurrentColumn)
				{
					if (!Islands.IsValidIndex(IslandIndex))
					{
						continue;
					}
					const FRect Bounds = GetIslandLocalPlacementBounds(IslandIndex);
					double PreferredOriginY = BuildCompactFallbackOriginY(CurrentColumn, IslandIndex);
					if (bAnchorFromParents)
					{
						ComputePreferredTopFromPlacedParents(IslandIndex, &FixedColumnSet, PreferredOriginY);
					}
					else
					{
						ComputePreferredTopFromPlacedChildren(IslandIndex, &FixedColumnSet, PreferredOriginY);
					}
					FColumnItem Item;
					Item.NodeIndex = IslandIndex;
					Item.Height = FMath::Max(1.0, Bounds.Height());
					Item.PreferredTop = SnapGraphCoordinate(PreferredOriginY + Bounds.Top);
					Item.bHasPreferred = true;
					ColumnItems.Add(Item);
				}
				if (ColumnItems.Num() == 0)
				{
					return;
				}
				PlaceAnchoredIntervals(ColumnItems, RootGap);
				for (const FColumnItem& Item : ColumnItems)
				{
					const int32 IslandIndex = Item.NodeIndex;
					if (!Islands.IsValidIndex(IslandIndex))
					{
						continue;
					}
					const FRect Bounds = GetIslandLocalPlacementBounds(IslandIndex);
					Islands[IslandIndex].PreferredY = SnapGraphCoordinate(Item.PreferredTop - Bounds.Top);
					Islands[IslandIndex].Y = SnapGraphCoordinate(Item.FinalTop - Bounds.Top);
					if (YPlaced.IsValidIndex(IslandIndex))
					{
						YPlaced[IslandIndex] = 1;
					}
					++Result.DesignDiagnostics.IslandPlacementOrderCount;
				}
				++Result.DesignDiagnostics.IslandLeftwardReanchorColumnCount;
			};

			if (BaseColumnIndex != INDEX_NONE)
			{
				PlaceBaseColumnCompact(BaseColumnIndex);
				for (int32 ColumnIndex = BaseColumnIndex + 1; ColumnIndex < TopologyColumns.Num(); ++ColumnIndex)
				{
					PlaceColumnAnchoredToFixed(ColumnIndex, ColumnIndex - 1, true);
				}
				for (int32 ColumnIndex = BaseColumnIndex - 1; ColumnIndex >= 0; --ColumnIndex)
				{
					PlaceColumnAnchoredToFixed(ColumnIndex, ColumnIndex + 1, false);
				}
			}

			auto ComputeColumnWorldSpanY = [&](const TArray<int32>& Column, double& OutSpanY) -> bool
			{
				bool bHasBounds = false;
				double MinY = 0.0;
				double MaxY = 0.0;
				for (const int32 IslandIndex : Column)
				{
					if (!Islands.IsValidIndex(IslandIndex))
					{
						continue;
					}
					const FRect Bounds = GetIslandLocalPlacementBounds(IslandIndex);
					const double TopY = Islands[IslandIndex].Y + Bounds.Top;
					const double BottomY = Islands[IslandIndex].Y + Bounds.Bottom;
					if (!bHasBounds)
					{
						MinY = TopY;
						MaxY = BottomY;
						bHasBounds = true;
					}
					else
					{
						MinY = FMath::Min(MinY, TopY);
						MaxY = FMath::Max(MaxY, BottomY);
					}
				}
				if (!bHasBounds)
				{
					return false;
				}
				OutSpanY = MaxY - MinY;
				return true;
			};
			if (TopologyColumns.Num() > 0)
			{
				Result.DesignDiagnostics.RootIslandCount = TopologyColumns[0].Num();
				Result.DesignDiagnostics.RootIslandTopYCount = TopologyColumns[0].Num();
				double FirstColumnSpanY = 0.0;
				if (ComputeColumnWorldSpanY(TopologyColumns[0], FirstColumnSpanY))
				{
					Result.DesignDiagnostics.RootCompactTotalHeight = FirstColumnSpanY;
				}
			}
			OutTopologyColumns = TopologyColumns;
		}

		static bool ConnectionEndpointOffsetTrianglesHitsHardOccupancy(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const int32 EdgeIndex,
			const FVector2D& SourcePoint,
			const FVector2D& TargetPoint,
			const FOptions& Options)
		{
			if (!Edges.IsValidIndex(EdgeIndex))
			{
				return true;
			}
			FRect SourceRect;
			FRect TargetRect;
			const FRect* SourceRectPtr = TryFindNodeWorldRectInIslands(Nodes, Islands, Edges[EdgeIndex].Source, SourceRect) ? &SourceRect : nullptr;
			const FRect* TargetRectPtr = TryFindNodeWorldRectInIslands(Nodes, Islands, Edges[EdgeIndex].Target, TargetRect) ? &TargetRect : nullptr;
			TArray<TArray<FVector2D>> EndpointTriangles;
			if (!TryBuildEndpointOffsetCollisionTriangles(SourcePoint, SourceRectPtr, TargetPoint, TargetRectPtr, Options, EndpointTriangles))
			{
				return true;
			}
			const TArray<FRect> HardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[EdgeIndex]);
			return EndpointOffsetTrianglesHitsHardOccupancy(EndpointTriangles, HardRects);
		}

		static bool BoundaryEntryToActualTargetHitsHardOccupancy(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const int32 EdgeIndex,
			const FVector2D& EntryPoint,
			const FVector2D& ActualTargetPoint,
			const FOptions& Options)
		{
			if (!Edges.IsValidIndex(EdgeIndex) || FVector2D::Distance(EntryPoint, ActualTargetPoint) <= 0.5)
			{
				return false;
			}
			TArray<FVector2D> EntryToActualControlPoints;
			EntryToActualControlPoints.Add(EntryPoint);
			EntryToActualControlPoints.Add(ActualTargetPoint);
			const TArray<FRect> HardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[EdgeIndex]);
			return ControlWireHitsHardOccupancy(EntryToActualControlPoints, HardRects);
		}

		static void RefreshDeterministicConnectionWorldPoints(
			const TArray<FIsland>& Islands,
			TArray<FDeterministicIslandConnection>& Connections)
		{
			for (FDeterministicIslandConnection& Connection : Connections)
			{
				if (!Islands.IsValidIndex(Connection.Endpoint.SourceIsland)
					|| !Islands.IsValidIndex(Connection.Endpoint.TargetIsland))
				{
					continue;
				}
				const FIsland& SourceIsland = Islands[Connection.Endpoint.SourceIsland];
				const FIsland& TargetIsland = Islands[Connection.Endpoint.TargetIsland];
				Connection.SourcePoint = FVector2D(
					SourceIsland.X + Connection.Endpoint.SourceLocal.X,
					SourceIsland.Y + Connection.Endpoint.SourceLocal.Y);
				Connection.EntryPoint = FVector2D(
					TargetIsland.X + Connection.Endpoint.EntryLocal.X,
					TargetIsland.Y + Connection.Endpoint.EntryLocal.Y);
				Connection.ActualTargetPoint = FVector2D(
					TargetIsland.X + Connection.Endpoint.ActualTargetLocal.X,
					TargetIsland.Y + Connection.Endpoint.ActualTargetLocal.Y);
			}
		}

		static bool ResolveBusConnectionEntriesForCurrentPlacement(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIslandPortCandidate>& Candidates,
			const TArray<FIsland>& Islands,
			const TArray<FDeterministicInterIslandBus>& Buses,
			const FOptions& Options,
			TArray<FDeterministicIslandConnection>& Connections)
		{
			bool bAnyChanged = false;
			for (FDeterministicIslandConnection& Connection : Connections)
			{
				if (!Connection.bBus)
				{
					continue;
				}
				const int32 OldCandidateId = Connection.Endpoint.CandidateId;
				const int32 OldBoundaryEntryId = Connection.Endpoint.BoundaryEntryId;
				const int32 OldInternalBusGroupId = Connection.Endpoint.InternalBusGroupId;
				const FString OldSide = Connection.Endpoint.Side;
				const FVector2D OldEntryLocal = Connection.Endpoint.EntryLocal;
				if (Buses.IsValidIndex(Connection.BusId)
					&& Islands.IsValidIndex(Connection.Endpoint.TargetIsland))
				{
					const FIsland& TargetIsland = Islands[Connection.Endpoint.TargetIsland];
					const FDeterministicInterIslandBus& Bus = Buses[Connection.BusId];
					const FIslandPortCandidate* BestBoundaryCandidate = nullptr;
					double BestBoundarySideCost = TNumericLimits<double>::Max();
					for (const FIslandPortCandidate& Candidate : Candidates)
					{
						if (Candidate.EdgeIndex != Connection.Endpoint.EdgeIndex
							|| Candidate.SourceIsland != Connection.Endpoint.SourceIsland
							|| Candidate.TargetIsland != Connection.Endpoint.TargetIsland
							|| !IsBoundaryBusSideCandidate(Candidate.Side))
						{
							continue;
						}
						if (Connection.Endpoint.InternalBusGroupId != INDEX_NONE
							&& Candidate.InternalBusGroupId != Connection.Endpoint.InternalBusGroupId)
						{
							continue;
						}
						const double CandidateWorldY = TargetIsland.Y + Candidate.TargetLocal.Y;
						const double SideCost = FMath::Abs(CandidateWorldY - Bus.FinalY);
						if (!BestBoundaryCandidate
							|| SideCost < BestBoundarySideCost - 0.5
							|| (FMath::IsNearlyEqual(SideCost, BestBoundarySideCost, 0.5)
								&& Candidate.Id < BestBoundaryCandidate->Id))
						{
							BestBoundaryCandidate = &Candidate;
							BestBoundarySideCost = SideCost;
						}
					}
					if (BestBoundaryCandidate && BestBoundaryCandidate->Id != Connection.Endpoint.CandidateId)
					{
						ApplyEndpointCandidateToSelected(*BestBoundaryCandidate, Options, Connection.Endpoint);
					}
				}
				ResolveSelectedEndpointEntry(
					Candidates,
					Islands,
					Options,
					Connection.Endpoint,
					RequiredBusEntrySourceSpanX(Connection, Buses, Islands, Options));
				if (OldCandidateId != Connection.Endpoint.CandidateId
					|| OldBoundaryEntryId != Connection.Endpoint.BoundaryEntryId
					|| OldInternalBusGroupId != Connection.Endpoint.InternalBusGroupId
					|| OldSide != Connection.Endpoint.Side
					|| !FMath::IsNearlyEqual(OldEntryLocal.X, Connection.Endpoint.EntryLocal.X, 0.5)
					|| !FMath::IsNearlyEqual(OldEntryLocal.Y, Connection.Endpoint.EntryLocal.Y, 0.5))
				{
					bAnyChanged = true;
				}
			}
			return bAnyChanged;
		}

		static void ClassifyIslandConnections(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const TArray<FIslandPortCandidate>& Candidates,
			const TSet<int32>& RouteOnlyEdgeIndices,
			const FOptions& Options,
			TArray<FDeterministicIslandConnection>& Connections,
			FRunResult& Result,
			const bool bAllowDirectPromotion,
			const bool bRecordDiagnostics = true)
		{
			const double DirectMaxSpanX = GetDirectConnectionMaxSpanX(Options);
			TMap<FString, int32> SourceParamUseCounts;
			for (const FDeterministicIslandConnection& ExistingConnection : Connections)
			{
				if (!Edges.IsValidIndex(ExistingConnection.Endpoint.EdgeIndex))
				{
					continue;
				}
				const FString SourceParamId = MakeSourceParamId(Edges[ExistingConnection.Endpoint.EdgeIndex]);
				int32& Count = SourceParamUseCounts.FindOrAdd(SourceParamId);
				++Count;
			}
			for (FDeterministicIslandConnection& Connection : Connections)
			{
				if (!Edges.IsValidIndex(Connection.Endpoint.EdgeIndex)
					|| !Islands.IsValidIndex(Connection.Endpoint.SourceIsland)
					|| !Islands.IsValidIndex(Connection.Endpoint.TargetIsland))
				{
					continue;
				}
				const FIsland& SourceIsland = Islands[Connection.Endpoint.SourceIsland];
				const FIsland& TargetIsland = Islands[Connection.Endpoint.TargetIsland];
				const FVector2D SourcePoint(SourceIsland.X + Connection.Endpoint.SourceLocal.X, SourceIsland.Y + Connection.Endpoint.SourceLocal.Y);
				Connection.SourcePoint = SourcePoint;
				Connection.EntryPoint = FVector2D(TargetIsland.X + Connection.Endpoint.EntryLocal.X, TargetIsland.Y + Connection.Endpoint.EntryLocal.Y);
				Connection.ActualTargetPoint = FVector2D(TargetIsland.X + Connection.Endpoint.ActualTargetLocal.X, TargetIsland.Y + Connection.Endpoint.ActualTargetLocal.Y);
				Connection.bRouteOnly = RouteOnlyEdgeIndices.Contains(Connection.Endpoint.EdgeIndex);
				Connection.bDirect = false;
				Connection.bBus = false;
				Connection.BusReason.Reset();
				Connection.BusId = INDEX_NONE;
				Connection.BranchSlotIndex = 0;
				if (Connection.bRouteOnly)
				{
					Connection.bBus = true;
					Connection.BusReason = TEXT("feedback");
					if (bRecordDiagnostics)
					{
						++Result.DesignDiagnostics.InterIslandFeedbackBusConnectionCount;
					}
					continue;
				}

				const FString SourceParamId = MakeSourceParamId(Edges[Connection.Endpoint.EdgeIndex]);
				const bool bSingleTargetForSourceOutput = SourceParamUseCounts.FindRef(SourceParamId) == 1;
				const double ActualDirectSpanX = FMath::Abs(
					(Connection.ActualTargetPoint.X - Options.InputAccessOffsetX)
					- (SourcePoint.X + Options.OutputAccessOffsetX));
				const bool bActualDirectAlignmentValid =
					HasNonNegativeConnectionAlignmentShorten(SourcePoint, Connection.ActualTargetPoint, Options);
				const bool bActualDirectCollision = ConnectionEndpointOffsetTrianglesHitsHardOccupancy(
					Nodes,
					Edges,
					Islands,
					Connection.Endpoint.EdgeIndex,
					SourcePoint,
					Connection.ActualTargetPoint,
					Options);
				if (bAllowDirectPromotion
					&& bSingleTargetForSourceOutput
					&& bActualDirectAlignmentValid
					&& !bActualDirectCollision
					&& ActualDirectSpanX <= DirectMaxSpanX)
				{
					Connection.Endpoint.EntryLocal = Connection.Endpoint.ActualTargetLocal;
					Connection.bDirect = true;
					Connection.bBus = false;
					Connection.DirectSpanX = ActualDirectSpanX;
					if (bRecordDiagnostics)
					{
						++Result.DesignDiagnostics.DirectIslandConnectionCount;
					}
					continue;
				}

				const FIslandPortCandidate* BestLegal = nullptr;
				double BestLegalScore = TNumericLimits<double>::Max();
				const FIslandPortCandidate* BestFallback = nullptr;
				double BestFallbackScore = TNumericLimits<double>::Max();
				double BestSpan = 0.0;
				for (const FIslandPortCandidate& Candidate : Candidates)
				{
					if (Candidate.EdgeIndex != Connection.Endpoint.EdgeIndex)
					{
						continue;
					}
					const FVector2D EntryPoint(TargetIsland.X + Candidate.TargetLocal.X, TargetIsland.Y + Candidate.TargetLocal.Y);
					const FVector2D ActualTargetPoint(TargetIsland.X + Candidate.ActualTargetLocal.X, TargetIsland.Y + Candidate.ActualTargetLocal.Y);
					const double SpanX = FMath::Abs((EntryPoint.X - Options.InputAccessOffsetX) - (SourcePoint.X + Options.OutputAccessOffsetX));
					const bool bAlignmentValid = HasNonNegativeConnectionAlignmentShorten(SourcePoint, EntryPoint, Options);
					const bool bSourceToEntryCollision = ConnectionEndpointOffsetTrianglesHitsHardOccupancy(Nodes, Edges, Islands, Candidate.EdgeIndex, SourcePoint, EntryPoint, Options);
					const bool bEntryToActualCollision = BoundaryEntryToActualTargetHitsHardOccupancy(
						Nodes,
						Edges,
						Islands,
						Candidate.EdgeIndex,
						EntryPoint,
						ActualTargetPoint,
						Options);
					const bool bCollision = bSourceToEntryCollision || bEntryToActualCollision;
					const bool bLegal = bAlignmentValid && !bCollision && SpanX <= DirectMaxSpanX;
					const bool bEntryDiffersFromActualTarget = FVector2D::Distance(EntryPoint, ActualTargetPoint) > 0.5;
					const double BaseScore =
						(bAlignmentValid ? 0.0 : 1000000.0)
						+ (bSourceToEntryCollision ? 100000.0 : 0.0)
						+ (bEntryToActualCollision ? 1000000.0 : 0.0)
						+ FMath::Max(0.0, SpanX - DirectMaxSpanX) * 100.0
						+ (bEntryDiffersFromActualTarget ? 100.0 : 0.0)
						+ FVector2D::Distance(EntryPoint, ActualTargetPoint) * 0.01
						+ FMath::Abs(SourcePoint.Y - EntryPoint.Y)
						+ ((Candidate.Side == TEXT("left") && !bEntryToActualCollision) ? 0.0 : 10.0)
						+ static_cast<double>(Candidate.Id) * 0.001;
					if (BaseScore < BestFallbackScore)
					{
						BestFallback = &Candidate;
						BestFallbackScore = BaseScore;
					}
					if (bLegal)
					{
						const double LegalScore =
							(bAlignmentValid ? 0.0 : 1000.0)
							+ PolylineManhattanLength(BuildDirectSplinePolyline(SourcePoint, EntryPoint)) * 0.001
							+ FVector2D::Distance(EntryPoint, ActualTargetPoint) * 0.01
							+ FMath::Abs(SourcePoint.Y - EntryPoint.Y)
							+ (Candidate.Side == TEXT("left") ? 0.0 : 10.0)
							+ static_cast<double>(Candidate.Id) * 0.001;
						if (LegalScore < BestLegalScore)
						{
							BestLegal = &Candidate;
							BestLegalScore = LegalScore;
							BestSpan = SpanX;
						}
					}
				}
				const FIslandPortCandidate* Chosen = BestLegal ? BestLegal : BestFallback;
				if (Chosen)
				{
					Connection.Endpoint = MakeSelectedEndpointFromCandidate(*Chosen, Options);
					Connection.Endpoint.MinTargetDeltaX = MinTargetDeltaXForCandidate(*Chosen, Options);
					Connection.EntryPoint = FVector2D(TargetIsland.X + Chosen->TargetLocal.X, TargetIsland.Y + Chosen->TargetLocal.Y);
					Connection.ActualTargetPoint = FVector2D(TargetIsland.X + Chosen->ActualTargetLocal.X, TargetIsland.Y + Chosen->ActualTargetLocal.Y);
					Connection.DirectSpanX = BestLegal ? BestSpan : FMath::Abs(Connection.EntryPoint.X - SourcePoint.X);
				}
				Connection.bDirect = false;
				Connection.bBus = true;
				Connection.BusReason = TEXT("collision_or_long_span");
				if (bRecordDiagnostics)
				{
					++Result.DesignDiagnostics.BusIslandConnectionCount;
				}
			}
		}

		static void PromoteFinalDirectIslandConnections(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const TSet<int32>& RouteOnlyEdgeIndices,
			const FOptions& Options,
			TArray<FDeterministicIslandConnection>& Connections,
			FRunResult& Result)
		{
			Result.DesignDiagnostics.DirectIslandConnectionCount = 0;
			Result.DesignDiagnostics.BusIslandConnectionCount = 0;
			Result.DesignDiagnostics.InterIslandFeedbackBusConnectionCount = 0;

			TMap<FString, int32> SourceParamUseCounts;
			for (const FDeterministicIslandConnection& ExistingConnection : Connections)
			{
				if (!Edges.IsValidIndex(ExistingConnection.Endpoint.EdgeIndex))
				{
					continue;
				}
				const FString SourceParamId = MakeSourceParamId(Edges[ExistingConnection.Endpoint.EdgeIndex]);
				int32& Count = SourceParamUseCounts.FindOrAdd(SourceParamId);
				++Count;
			}

			const double DirectMaxSpanX = GetDirectConnectionMaxSpanX(Options);
			TSet<int32> ActiveFinalBusIds;
			for (FDeterministicIslandConnection& Connection : Connections)
			{
				if (!Edges.IsValidIndex(Connection.Endpoint.EdgeIndex)
					|| !Islands.IsValidIndex(Connection.Endpoint.SourceIsland)
					|| !Islands.IsValidIndex(Connection.Endpoint.TargetIsland))
				{
					continue;
				}

				const FIsland& SourceIsland = Islands[Connection.Endpoint.SourceIsland];
				const FIsland& TargetIsland = Islands[Connection.Endpoint.TargetIsland];
				Connection.SourcePoint = FVector2D(
					SourceIsland.X + Connection.Endpoint.SourceLocal.X,
					SourceIsland.Y + Connection.Endpoint.SourceLocal.Y);
				Connection.EntryPoint = FVector2D(
					TargetIsland.X + Connection.Endpoint.EntryLocal.X,
					TargetIsland.Y + Connection.Endpoint.EntryLocal.Y);
				Connection.ActualTargetPoint = FVector2D(
					TargetIsland.X + Connection.Endpoint.ActualTargetLocal.X,
					TargetIsland.Y + Connection.Endpoint.ActualTargetLocal.Y);
				Connection.bRouteOnly = RouteOnlyEdgeIndices.Contains(Connection.Endpoint.EdgeIndex);
				Connection.DirectReroutePoints.Reset();
				if (Connection.bRouteOnly)
				{
					Connection.bDirect = false;
					Connection.bBus = true;
					Connection.BusReason = TEXT("feedback");
					if (Connection.BusId != INDEX_NONE)
					{
						ActiveFinalBusIds.Add(Connection.BusId);
					}
					++Result.DesignDiagnostics.InterIslandFeedbackBusConnectionCount;
					continue;
				}

				const FString SourceParamId = MakeSourceParamId(Edges[Connection.Endpoint.EdgeIndex]);
				const bool bSingleTargetForSourceOutput = SourceParamUseCounts.FindRef(SourceParamId) == 1;
				const double ActualDirectSpanX = FMath::Abs(
					(Connection.ActualTargetPoint.X - Options.InputAccessOffsetX)
					- (Connection.SourcePoint.X + Options.OutputAccessOffsetX));
				const bool bActualDirectAlignmentValid =
					HasNonNegativeConnectionAlignmentShorten(Connection.SourcePoint, Connection.ActualTargetPoint, Options);
				const bool bActualDirectCollision = ConnectionEndpointOffsetTrianglesHitsHardOccupancy(
					Nodes,
					Edges,
					Islands,
					Connection.Endpoint.EdgeIndex,
					Connection.SourcePoint,
					Connection.ActualTargetPoint,
					Options);
				TArray<FVector2D> EndpointKnotControlPoints;
				TArray<FVector2D> EndpointKnotInteriorPoints;
				const bool bEndpointKnotDirectValid =
					bActualDirectCollision
					&& TryBuildDirectEndpointKnotControlPoints(
						Connection.SourcePoint,
						Connection.ActualTargetPoint,
						Options,
						EndpointKnotControlPoints,
						&EndpointKnotInteriorPoints)
					&& !ControlWireHitsHardOccupancy(
						EndpointKnotControlPoints,
						BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[Connection.Endpoint.EdgeIndex]));
				if (bSingleTargetForSourceOutput
					&& bActualDirectAlignmentValid
					&& (!bActualDirectCollision || bEndpointKnotDirectValid)
					&& ActualDirectSpanX <= DirectMaxSpanX)
				{
					Connection.Endpoint.EntryLocal = Connection.Endpoint.ActualTargetLocal;
					Connection.EntryPoint = Connection.ActualTargetPoint;
					Connection.bDirect = true;
					Connection.bBus = false;
					Connection.BusId = INDEX_NONE;
					Connection.DirectSpanX = ActualDirectSpanX;
					if (bEndpointKnotDirectValid)
					{
						Connection.DirectReroutePoints = EndpointKnotInteriorPoints;
					}
					++Result.DesignDiagnostics.DirectIslandConnectionCount;
					continue;
				}

				Connection.bDirect = false;
				Connection.bBus = true;
				Connection.DirectSpanX = ActualDirectSpanX;
				if (Connection.BusReason.IsEmpty())
				{
					Connection.BusReason = TEXT("collision_or_long_span");
				}
				if (Connection.BusId != INDEX_NONE)
				{
					ActiveFinalBusIds.Add(Connection.BusId);
				}
				++Result.DesignDiagnostics.BusIslandConnectionCount;
			}
			Result.DesignDiagnostics.InterIslandBusCount = ActiveFinalBusIds.Num();
			Result.DesignDiagnostics.ConnectionGroupCount =
				Result.DesignDiagnostics.InterIslandBusCount
				+ Result.DesignDiagnostics.DirectIslandConnectionCount;
			Result.DesignDiagnostics.RouteGroupCount = Result.DesignDiagnostics.ConnectionGroupCount;
		}

		static void AddIslandLocalBand(TArray<FRect>& Bands, const FRect& Rect)
		{
			if (Rect.Width() <= 0.5 || Rect.Height() <= 0.5)
			{
				return;
			}
			if (!FMath::IsFinite(Rect.Left)
				|| !FMath::IsFinite(Rect.Right)
				|| !FMath::IsFinite(Rect.Top)
				|| !FMath::IsFinite(Rect.Bottom))
			{
				return;
			}
			Bands.Add(Rect);
		}

		static void UnionIslandLocalBand(FRect& Bounds, bool& bAny, const FRect& Rect)
		{
			if (Rect.Width() <= 0.5 || Rect.Height() <= 0.5)
			{
				return;
			}
			if (!bAny)
			{
				Bounds = Rect;
				bAny = true;
				return;
			}
			Bounds.Left = FMath::Min(Bounds.Left, Rect.Left);
			Bounds.Top = FMath::Min(Bounds.Top, Rect.Top);
			Bounds.Right = FMath::Max(Bounds.Right, Rect.Right);
			Bounds.Bottom = FMath::Max(Bounds.Bottom, Rect.Bottom);
		}

		static TArray<FRect> BuildIslandColumnCollisionBands(
			const TArray<FNode>& Nodes,
			const FIsland& Island,
			const FOptions& Options)
		{
			TArray<FRect> Bands;
			const double Padding = FMath::Max(4.0, Options.NodePadding * 0.25);
			for (const TArray<int32>& Column : Island.Columns)
			{
				FRect ColumnBounds;
				bool bAnyColumnItem = false;
				for (const int32 NodeIndex : Column)
				{
					if (!Nodes.IsValidIndex(NodeIndex))
					{
						continue;
					}
					const FNode& Node = Nodes[NodeIndex];
					UnionIslandLocalBand(
						ColumnBounds,
						bAnyColumnItem,
						{
							Node.NewX - Padding,
							Node.NewY - Padding,
							Node.NewX + Node.Width + Padding,
							Node.NewY + Node.Height + Padding
						});
				}
				if (bAnyColumnItem)
				{
					AddIslandLocalBand(Bands, ColumnBounds);
				}
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
					AddIslandLocalBand(
						Bands,
						{
							Segment.Left - Padding,
							Segment.Top - Padding,
							Segment.Right + Padding,
							Segment.Bottom + Padding
						});
				}
			}
			if (Bands.Num() == 0)
			{
				AddIslandLocalBand(Bands, Island.LocalBounds);
			}
			return Bands;
		}

		static void AddSelectedInputChannelBands(
			const TArray<FEdge>& Edges,
			const TArray<FInternalBusGroup>& InternalBusGroups,
			const TArray<FDeterministicIslandConnection>& Connections,
			const int32 IslandIndex,
			TArray<FRect>& Bands)
		{
			const double HalfThickness = GetBusHalfThickness();
			TMap<FString, FRect> BoundaryBandByKey;
			TSet<FString> BoundaryBandHasValue;
			for (const FDeterministicIslandConnection& Connection : Connections)
			{
				if (Connection.Endpoint.TargetIsland != IslandIndex)
				{
					continue;
				}
				const FSelectedEndpointCandidate& Endpoint = Connection.Endpoint;
				FVector2D InternalEntry = Endpoint.ActualTargetLocal;
				if (InternalBusGroups.IsValidIndex(Endpoint.InternalBusGroupId)
					&& InternalBusGroups[Endpoint.InternalBusGroupId].IslandId == IslandIndex)
				{
					InternalEntry = InternalBusGroups[Endpoint.InternalBusGroupId].EntryLocal;
				}
				if (Endpoint.Side == TEXT("left"))
				{
					AddIslandLocalBand(
						Bands,
						{
							FMath::Min(Endpoint.EntryLocal.X, InternalEntry.X),
							InternalEntry.Y - HalfThickness,
							FMath::Max(Endpoint.EntryLocal.X, InternalEntry.X),
							InternalEntry.Y + HalfThickness
						});
					continue;
				}
				if (Endpoint.Side != TEXT("top") && Endpoint.Side != TEXT("bottom"))
				{
					continue;
				}
				FString SourceKey = FString::FromInt(Endpoint.EdgeIndex);
				if (Edges.IsValidIndex(Endpoint.EdgeIndex))
				{
					SourceKey = MakeSourceParamId(Edges[Endpoint.EdgeIndex]);
				}
				const FString BoundaryKey = FString::Printf(TEXT("%d|%s|%s"), IslandIndex, *SourceKey, *Endpoint.Side);
				const double BandRight = FMath::Max3(Endpoint.EntryLocal.X, InternalEntry.X, Endpoint.ActualTargetLocal.X);
				const FRect Band = {
					FMath::Min(Endpoint.EntryLocal.X, BandRight),
					Endpoint.EntryLocal.Y - HalfThickness,
					FMath::Max(Endpoint.EntryLocal.X, BandRight),
					Endpoint.EntryLocal.Y + HalfThickness
				};
				FRect& Existing = BoundaryBandByKey.FindOrAdd(BoundaryKey);
				bool bAny = BoundaryBandHasValue.Contains(BoundaryKey);
				UnionIslandLocalBand(Existing, bAny, Band);
				if (bAny)
				{
					BoundaryBandHasValue.Add(BoundaryKey);
				}
			}
			for (const TPair<FString, FRect>& Pair : BoundaryBandByKey)
			{
				if (BoundaryBandHasValue.Contains(Pair.Key))
				{
					AddIslandLocalBand(Bands, Pair.Value);
				}
			}
		}

		static void AddIslandLocalBusStructuralBands(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const FIsland& Island,
			const TSet<int32>& InterIslandEdgeIndices,
			const FOptions& Options,
			TArray<FRect>& Bands)
		{
			if (Island.bCommentIsland)
			{
				return;
			}
			const TSet<int32> InputChainEdges = BuildInputChainEdgeSet(Island);
			for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
			{
				if (InterIslandEdgeIndices.Contains(EdgeIndex) || InputChainEdges.Contains(EdgeIndex))
				{
					continue;
				}
				const FEdge& Edge = Edges[EdgeIndex];
				if (!Island.NodeSet.Contains(Edge.Source)
					|| !Island.NodeSet.Contains(Edge.Target)
					|| !Nodes.IsValidIndex(Edge.Source)
					|| !Nodes.IsValidIndex(Edge.Target))
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
				const bool bCrossesMultipleColumns = FMath::Abs(TargetColumn - SourceColumn) > 1;
				if (!bLocalColumnLineHitsSibling && !bCrossesMultipleColumns)
				{
					continue;
				}
				TArray<FVector2D> Route;
				if (bLocalColumnLineHitsSibling && !bCrossesMultipleColumns)
				{
					const double ColumnRightX = ComputeIslandColumnRightBoundaryX(Nodes, Island, LocalCollisionColumns[0], Options);
					Route = BuildSourceColumnRightBoundaryRoute(Start, End, ColumnRightX, Options);
				}
				else
				{
					const double TrunkY = SnapGraphCoordinate((Start.Y + End.Y) * 0.5);
					Route = BuildIslandInternalSharedBusRoute(Start, End, TrunkY, Options);
					if (bLocalColumnLineHitsSibling)
					{
						const double ColumnRightX = ComputeIslandColumnRightBoundaryX(Nodes, Island, LocalCollisionColumns[0], Options);
						InsertSourceColumnRightBoundaryPoint(Route, Start, ColumnRightX, Options);
					}
				}
				if (Route.Num() < 2)
				{
					continue;
				}
				TArray<FRect> RouteBands;
				AddVisibleRouteLaneReservationRects(Route, GetBusAvoidanceDistance(), Options, RouteBands);
				for (const FRect& RouteBand : RouteBands)
				{
					AddIslandLocalBand(
						Bands,
						{
							RouteBand.Left - Island.X,
							RouteBand.Top - Island.Y,
							RouteBand.Right - Island.X,
							RouteBand.Bottom - Island.Y
						});
				}
			}
		}

		static TArray<FXStripCollisionBox> BuildIslandBusCollisionBoxes(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FInternalBusGroup>& InternalBusGroups,
			const TArray<FDeterministicIslandConnection>& Connections,
			const FIsland& Island,
			const int32 IslandIndex,
			const TSet<int32>& InterIslandEdgeIndices,
			const FOptions& Options)
		{
			TArray<FRect> LocalBands = BuildIslandColumnCollisionBands(Nodes, Island, Options);
			AddSelectedInputChannelBands(Edges, InternalBusGroups, Connections, IslandIndex, LocalBands);
			AddIslandLocalBusStructuralBands(Nodes, Edges, Island, InterIslandEdgeIndices, Options, LocalBands);

			double LocalCollisionTop = TNumericLimits<double>::Max();
			double LocalCollisionBottom = -TNumericLimits<double>::Max();
			for (const FRect& Band : LocalBands)
			{
				LocalCollisionTop = FMath::Min(LocalCollisionTop, Band.Top);
				LocalCollisionBottom = FMath::Max(LocalCollisionBottom, Band.Bottom);
			}
			const double IslandCollisionCenterY =
				LocalCollisionTop != TNumericLimits<double>::Max() && LocalCollisionBottom > LocalCollisionTop + 0.5
					? Island.Y + (LocalCollisionTop + LocalCollisionBottom) * 0.5
					: IslandAabb(Island).CenterY();

			TArray<double> Events;
			auto AddEvent = [&Events](const double Value)
			{
				if (!FMath::IsFinite(Value))
				{
					return;
				}
				const double Snapped = SnapGraphCoordinate(Value);
				for (const double Existing : Events)
				{
					if (FMath::IsNearlyEqual(Existing, Snapped, 0.5))
					{
						return;
					}
				}
				Events.Add(Snapped);
			};
			AddEvent(Island.LocalBounds.Left);
			AddEvent(Island.LocalBounds.Right);
			for (const FRect& Band : LocalBands)
			{
				AddEvent(Band.Left);
				AddEvent(Band.Right);
			}
			Events.Sort();

			TArray<FXStripCollisionBox> Boxes;
			const double AvoidanceY = GetBusAvoidanceDistance();
			for (int32 EventIndex = 0; EventIndex + 1 < Events.Num(); ++EventIndex)
			{
				const double X0 = Events[EventIndex];
				const double X1 = Events[EventIndex + 1];
				if (X1 <= X0 + 0.5)
				{
					continue;
				}
				double RawTop = TNumericLimits<double>::Max();
				double RawBottom = -TNumericLimits<double>::Max();
				for (const FRect& Band : LocalBands)
				{
					if (!XStripRangeOverlaps(Band.Left, Band.Right, X0, X1, 0.0))
					{
						continue;
					}
					RawTop = FMath::Min(RawTop, Band.Top);
					RawBottom = FMath::Max(RawBottom, Band.Bottom);
				}
				if (RawTop == TNumericLimits<double>::Max())
				{
					continue;
				}
				FRect GlobalRect = {
					Island.X + X0,
					Island.Y + RawTop - AvoidanceY,
					Island.X + X1,
					Island.Y + RawBottom + AvoidanceY
				};
				if (Boxes.Num() > 0)
				{
					FXStripCollisionBox& Previous = Boxes.Last();
					if (Previous.OwnerKind == EXStripCollisionOwnerKind::Island
						&& Previous.OwnerId == IslandIndex
						&& FMath::IsNearlyEqual(Previous.Rect.Right, GlobalRect.Left, 0.5)
						&& FMath::IsNearlyEqual(Previous.Rect.Top, GlobalRect.Top, 0.5)
						&& FMath::IsNearlyEqual(Previous.Rect.Bottom, GlobalRect.Bottom, 0.5))
					{
						Previous.Rect.Right = GlobalRect.Right;
						continue;
					}
				}
				AddXStripCollisionBox(
					Boxes,
					EXStripCollisionOwnerKind::Island,
					IslandIndex,
					INDEX_NONE,
					GlobalRect,
					IslandCollisionCenterY);
			}
			if (Boxes.Num() == 0)
			{
				FRect Rect = IslandAabb(Island);
				Rect.Top -= AvoidanceY;
				Rect.Bottom += AvoidanceY;
				AddXStripCollisionBox(Boxes, EXStripCollisionOwnerKind::Island, IslandIndex, INDEX_NONE, Rect, Rect.CenterY());
			}
			return Boxes;
		}

		static TArray<FXStripCollisionBox> BuildAllIslandBusCollisionBoxes(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const TArray<FInternalBusGroup>& InternalBusGroups,
			const TArray<FDeterministicIslandConnection>& Connections,
			const FOptions& Options)
		{
			TArray<FXStripCollisionBox> Boxes;
			TSet<int32> InterIslandEdgeIndices;
			for (const FDeterministicIslandConnection& Connection : Connections)
			{
				if (Connection.Endpoint.EdgeIndex != INDEX_NONE)
				{
					InterIslandEdgeIndices.Add(Connection.Endpoint.EdgeIndex);
				}
			}
			for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
			{
				const FIsland& Island = Islands[IslandIndex];
				Boxes.Append(BuildIslandBusCollisionBoxes(
					Nodes,
					Edges,
					InternalBusGroups,
					Connections,
					Island,
					IslandIndex,
					InterIslandEdgeIndices,
					Options));
			}
			return Boxes;
		}

		static void ComputeInterIslandBusTrunkXCoverage(
			const FDeterministicIslandConnection& Connection,
			const FOptions& Options,
			double& OutXMin,
			double& OutXMax)
		{
			const double BranchSpacing = GetCrossBusBranchLaneSpacing(Options);
			const double SourceBusPointX = ComputeInputXAlignedToOutput(Connection.SourcePoint.X, Options);
			const double BranchPointX = ComputeOutputXAlignedToInput(Connection.ActualTargetPoint.X, Options)
				- FMath::Max(0, Connection.BranchSlotIndex) * BranchSpacing;
			OutXMin = SnapGraphCoordinate(FMath::Min(SourceBusPointX, BranchPointX));
			OutXMax = SnapGraphCoordinate(FMath::Max(SourceBusPointX, BranchPointX));
		}

		static FString MakeBusAttachmentPointKey(const TCHAR* Kind, const FVector2D& Point)
		{
			return FString::Printf(
				TEXT("%s:%lld:%lld"),
				Kind,
				static_cast<int64>(FMath::RoundToDouble(SnapGraphCoordinate(Point.X))),
				static_cast<int64>(FMath::RoundToDouble(SnapGraphCoordinate(Point.Y))));
		}

		static void ComputeInterIslandBusSlotBiases(
			const TArray<FDeterministicIslandConnection>& Connections,
			TArray<FDeterministicInterIslandBus>& Buses)
		{
			constexpr double NeutralToleranceY = 0.5;
			for (FDeterministicInterIslandBus& Bus : Buses)
			{
				Bus.UpperAttachmentCount = 0;
				Bus.LowerAttachmentCount = 0;
				Bus.SlotBias = 0;
				TSet<FString> UniqueInterfaces;
				auto AddInterface = [&Bus, &UniqueInterfaces](const TCHAR* Kind, const FVector2D& Point)
				{
					if (!FMath::IsFinite(Point.Y))
					{
						return;
					}
					const FString Key = MakeBusAttachmentPointKey(Kind, Point);
					if (UniqueInterfaces.Contains(Key))
					{
						return;
					}
					UniqueInterfaces.Add(Key);
					if (Point.Y < Bus.PreferredY - NeutralToleranceY)
					{
						++Bus.UpperAttachmentCount;
					}
					else if (Point.Y > Bus.PreferredY + NeutralToleranceY)
					{
						++Bus.LowerAttachmentCount;
					}
				};

				AddInterface(TEXT("source"), Bus.SourcePoint);
				for (const int32 ConnectionIndex : Bus.ConnectionIndices)
				{
					if (!Connections.IsValidIndex(ConnectionIndex))
					{
						continue;
					}
					AddInterface(TEXT("target"), ResolveBusBranchVisibleTargetPoint(Connections[ConnectionIndex]));
				}
				Bus.SlotBias = Bus.UpperAttachmentCount - Bus.LowerAttachmentCount;
			}
		}

		static double BusIntervalOverlapLength(
			const FDeterministicInterIslandBus& A,
			const FDeterministicInterIslandBus& B)
		{
			const double Left = FMath::Max(A.XMin, B.XMin);
			const double Right = FMath::Min(A.XMax, B.XMax);
			return FMath::Max(0.0, Right - Left);
		}

		static double MergeA49IslandDirectionalOffset(
			const double CurrentOffsetY,
			const double CandidateOffsetY,
			const int32 Direction)
		{
			if (Direction < 0)
			{
				return FMath::Min(CurrentOffsetY, CandidateOffsetY);
			}
			if (Direction > 0)
			{
				return FMath::Max(CurrentOffsetY, CandidateOffsetY);
			}
			return CurrentOffsetY;
		}

		static bool SignedBusSlotConflicts(
			const TArray<FDeterministicInterIslandBus>& Buses,
			const int32 BusId,
			const int32 CandidateSignedSlot,
			const int32 IgnoreBusId = INDEX_NONE)
		{
			if (!Buses.IsValidIndex(BusId))
			{
				return false;
			}
			const FDeterministicInterIslandBus& Bus = Buses[BusId];
			for (int32 OtherBusId = 0; OtherBusId < Buses.Num(); ++OtherBusId)
			{
				if (OtherBusId == BusId || OtherBusId == IgnoreBusId)
				{
					continue;
				}
				const FDeterministicInterIslandBus& OtherBus = Buses[OtherBusId];
				if (OtherBus.BundleId != Bus.BundleId || OtherBus.SlotIndex == INDEX_NONE)
				{
					continue;
				}
				if (OtherBus.SignedSlotIndex != CandidateSignedSlot)
				{
					continue;
				}
				if (IntervalOverlaps(Bus.XMin, Bus.XMax, OtherBus.XMin, OtherBus.XMax, 0.0))
				{
					return true;
				}
			}
			return false;
		}

		static double ComputeBusSlotBiasInversionCost(
			const TArray<FDeterministicInterIslandBus>& Buses,
			const int32 BusId,
			const int32 CandidateSignedSlot)
		{
			if (!Buses.IsValidIndex(BusId))
			{
				return 0.0;
			}
			const FDeterministicInterIslandBus& Bus = Buses[BusId];
			double Cost = 0.0;
			for (const FDeterministicInterIslandBus& OtherBus : Buses)
			{
				if (OtherBus.Id == BusId
					|| OtherBus.BundleId != Bus.BundleId
					|| OtherBus.SlotIndex == INDEX_NONE)
				{
					continue;
				}
				const double OverlapWeight = BusIntervalOverlapLength(Bus, OtherBus);
				if (OverlapWeight <= 0.5)
				{
					continue;
				}
				if (Bus.SlotBias > OtherBus.SlotBias && CandidateSignedSlot >= OtherBus.SignedSlotIndex)
				{
					Cost += static_cast<double>(Bus.SlotBias - OtherBus.SlotBias) * OverlapWeight;
				}
				else if (Bus.SlotBias < OtherBus.SlotBias && CandidateSignedSlot <= OtherBus.SignedSlotIndex)
				{
					Cost += static_cast<double>(OtherBus.SlotBias - Bus.SlotBias) * OverlapWeight;
				}
			}
			return Cost;
		}

		static void AssignInterIslandBusSlotsByBias(
			TArray<FDeterministicInterIslandBus>& Buses,
			FDeterministicInterIslandBusBundle& Bundle,
			FRunResult& Result,
			const bool bRecordDiagnostics)
		{
			TArray<int32> SortedBusIds = Bundle.BusIds;
			SortedBusIds.Sort([&Buses](const int32 A, const int32 B)
			{
				const FDeterministicInterIslandBus& Left = Buses[A];
				const FDeterministicInterIslandBus& Right = Buses[B];
				if (!FMath::IsNearlyEqual(Left.XMin, Right.XMin, 0.5))
				{
					return Left.XMin < Right.XMin;
				}
				const int32 LeftBiasAbs = FMath::Abs(Left.SlotBias);
				const int32 RightBiasAbs = FMath::Abs(Right.SlotBias);
				if (LeftBiasAbs != RightBiasAbs)
				{
					return LeftBiasAbs > RightBiasAbs;
				}
				if (!FMath::IsNearlyEqual(Left.PreferredY, Right.PreferredY, 0.5))
				{
					return Left.PreferredY < Right.PreferredY;
				}
				return Left.Id < Right.Id;
			});

			for (const int32 BusId : SortedBusIds)
			{
				if (!Buses.IsValidIndex(BusId))
				{
					continue;
				}
				const int32 CandidateLimit = FMath::Max(2, SortedBusIds.Num() + 1);
				int32 BestSignedSlot = 0;
				int32 BestUniqueSlotCount = TNumericLimits<int32>::Max();
				double BestBiasCost = TNumericLimits<double>::Max();
				int32 BestAbsSlot = TNumericLimits<int32>::Max();
				int32 BestBiasTie = TNumericLimits<int32>::Max();
				bool bFoundCandidate = false;

				TSet<int32> ExistingSignedSlots;
				for (const int32 OtherBusId : Bundle.BusIds)
				{
					if (Buses.IsValidIndex(OtherBusId) && Buses[OtherBusId].SlotIndex != INDEX_NONE)
					{
						ExistingSignedSlots.Add(Buses[OtherBusId].SignedSlotIndex);
					}
				}

				for (int32 AbsSlot = 0; AbsSlot <= CandidateLimit; ++AbsSlot)
				{
					TArray<int32> Candidates;
					if (AbsSlot == 0)
					{
						Candidates.Add(0);
					}
					else
					{
						Candidates.Add(-AbsSlot);
						Candidates.Add(AbsSlot);
					}
					for (const int32 CandidateSignedSlot : Candidates)
					{
						if (SignedBusSlotConflicts(Buses, BusId, CandidateSignedSlot))
						{
							continue;
						}
						TSet<int32> CandidateSlotSet = ExistingSignedSlots;
						CandidateSlotSet.Add(CandidateSignedSlot);
						const int32 UniqueSlotCount = CandidateSlotSet.Num();
						const double BiasCost = ComputeBusSlotBiasInversionCost(Buses, BusId, CandidateSignedSlot);
						const int32 AbsCandidate = FMath::Abs(CandidateSignedSlot);
						const int32 BiasTie =
							Buses[BusId].SlotBias > 0
								? CandidateSignedSlot
								: (Buses[BusId].SlotBias < 0 ? -CandidateSignedSlot : 0);

						bool bBetter = !bFoundCandidate;
						if (!bBetter)
						{
							if (UniqueSlotCount != BestUniqueSlotCount)
							{
								bBetter = UniqueSlotCount < BestUniqueSlotCount;
							}
							else if (!FMath::IsNearlyEqual(BiasCost, BestBiasCost, 0.001))
							{
								bBetter = BiasCost < BestBiasCost;
							}
							else if (AbsCandidate != BestAbsSlot)
							{
								bBetter = AbsCandidate < BestAbsSlot;
							}
							else if (BiasTie != BestBiasTie)
							{
								bBetter = BiasTie < BestBiasTie;
							}
							else if (CandidateSignedSlot != BestSignedSlot)
							{
								bBetter = CandidateSignedSlot < BestSignedSlot;
							}
						}
						if (bBetter)
						{
							bFoundCandidate = true;
							BestSignedSlot = CandidateSignedSlot;
							BestUniqueSlotCount = UniqueSlotCount;
							BestBiasCost = BiasCost;
							BestAbsSlot = AbsCandidate;
							BestBiasTie = BiasTie;
						}
					}
				}

				Buses[BusId].SignedSlotIndex = BestSignedSlot;
				Buses[BusId].SlotIndex = 0;
				if (bRecordDiagnostics)
				{
					++Result.DesignDiagnostics.InterIslandBusSlotAssignmentCount;
				}
			}

			for (int32 Pass = 0; Pass < SortedBusIds.Num(); ++Pass)
			{
				bool bSwapped = false;
				for (int32 AIndex = 0; AIndex < SortedBusIds.Num(); ++AIndex)
				{
					const int32 AId = SortedBusIds[AIndex];
					if (!Buses.IsValidIndex(AId))
					{
						continue;
					}
					for (int32 BIndex = AIndex + 1; BIndex < SortedBusIds.Num(); ++BIndex)
					{
						const int32 BId = SortedBusIds[BIndex];
						if (!Buses.IsValidIndex(BId))
						{
							continue;
						}
						FDeterministicInterIslandBus& A = Buses[AId];
						FDeterministicInterIslandBus& B = Buses[BId];
						if (A.SlotBias == B.SlotBias
							|| !IntervalOverlaps(A.XMin, A.XMax, B.XMin, B.XMax, 0.0))
						{
							continue;
						}
						const bool bInverted =
							(A.SlotBias > B.SlotBias && A.SignedSlotIndex >= B.SignedSlotIndex)
							|| (A.SlotBias < B.SlotBias && A.SignedSlotIndex <= B.SignedSlotIndex);
						if (!bInverted)
						{
							continue;
						}
						const int32 OldASlot = A.SignedSlotIndex;
						const int32 OldBSlot = B.SignedSlotIndex;
						if (SignedBusSlotConflicts(Buses, AId, OldBSlot, BId)
							|| SignedBusSlotConflicts(Buses, BId, OldASlot, AId))
						{
							continue;
						}
						A.SignedSlotIndex = OldBSlot;
						B.SignedSlotIndex = OldASlot;
						bSwapped = true;
					}
				}
				if (!bSwapped)
				{
					break;
				}
			}

			TArray<int32> UniqueSignedSlots;
			for (const int32 BusId : Bundle.BusIds)
			{
				if (!Buses.IsValidIndex(BusId))
				{
					continue;
				}
				UniqueSignedSlots.AddUnique(Buses[BusId].SignedSlotIndex);
			}
			UniqueSignedSlots.Sort();
			TMap<int32, int32> SlotIndexBySignedSlot;
			for (int32 Index = 0; Index < UniqueSignedSlots.Num(); ++Index)
			{
				SlotIndexBySignedSlot.Add(UniqueSignedSlots[Index], Index);
			}
			Bundle.MaxSlotIndex = FMath::Max(0, UniqueSignedSlots.Num() - 1);
			for (const int32 BusId : Bundle.BusIds)
			{
				if (!Buses.IsValidIndex(BusId))
				{
					continue;
				}
				if (const int32* SlotIndex = SlotIndexBySignedSlot.Find(Buses[BusId].SignedSlotIndex))
				{
					Buses[BusId].SlotIndex = *SlotIndex;
				}
				else
				{
					Buses[BusId].SlotIndex = 0;
				}
			}
		}
