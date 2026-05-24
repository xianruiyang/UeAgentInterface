// Copyright Epic Games, Inc. All Rights Reserved.
// Island input-entry model: SourceParam -> InternalBusGroup -> BoundaryEntry -> EndpointCandidate.

		static void BuildInputEntryAndPortModel(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const TArray<FIslandEdge>& IslandEdges,
			const FOptions& Options,
			FRunResult& Result,
			TArray<FInternalBusGroup>& OutInternalBusGroups,
			TArray<FBoundaryEntry>& OutBoundaryEntries,
			TArray<FBoundaryBus>& OutBoundaryBuses,
			TArray<FIslandPortCandidate>& OutCandidates)
		{
			TMap<FString, int32> GroupByKey;
			TMap<FString, TArray<int32>> GroupsByBoundaryBusKey;
			TSet<FString> SourceParams;
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

				const FString SourceParamId = MakeSourceParamId(Edge);
				SourceParams.Add(SourceParamId);
				const int32 TargetColumn = Nodes[Edge.Target].Rank;
				const FString GroupKey = FString::Printf(TEXT("%d|%s|%d"), IslandEdge.TargetIsland, *SourceParamId, TargetColumn);
				int32 GroupIndex = INDEX_NONE;
				if (const int32* Existing = GroupByKey.Find(GroupKey))
				{
					GroupIndex = *Existing;
				}
				else
				{
					FInternalBusGroup Group;
					Group.Id = OutInternalBusGroups.Num();
					Group.IslandId = IslandEdge.TargetIsland;
					Group.SourceParamId = SourceParamId;
					Group.TargetColumn = TargetColumn;
					Group.bExec = Edge.bExec;
					const FIsland& TargetIsland = Islands[IslandEdge.TargetIsland];
					int32 MinColumn = TNumericLimits<int32>::Max();
					for (const int32 NodeIndex : TargetIsland.Nodes)
					{
						if (Nodes.IsValidIndex(NodeIndex))
						{
							MinColumn = FMath::Min(MinColumn, Nodes[NodeIndex].Rank);
						}
					}
					Group.bOnlyLeftmostColumn = TargetColumn <= MinColumn;
					GroupIndex = Group.Id;
					GroupByKey.Add(GroupKey, GroupIndex);
					GroupsByBoundaryBusKey.FindOrAdd(FString::Printf(TEXT("%d|%s"), IslandEdge.TargetIsland, *SourceParamId)).Add(GroupIndex);
					OutInternalBusGroups.Add(Group);
				}

				FInternalBusGroup& Group = OutInternalBusGroups[GroupIndex];
				Group.EdgeIndices.Add(IslandEdge.EdgeIndex);
				Group.TargetNodes.AddUnique(Edge.Target);
				Group.TargetLocalPorts.Add(GetTargetPort(Nodes[Edge.Target], Edge, true));
			}

			Result.DesignDiagnostics.SourceParamCount = SourceParams.Num();
			Result.DesignDiagnostics.InternalBusGroupCount = OutInternalBusGroups.Num();

			for (FInternalBusGroup& Group : OutInternalBusGroups)
			{
				TArray<double> TargetYs;
				for (const FVector2D& LocalPort : Group.TargetLocalPorts)
				{
					TargetYs.Add(LocalPort.Y);
				}
				const FIsland& Island = Islands[Group.IslandId];
				const double MedianY = MedianOf(TargetYs, Island.LocalBounds.CenterY());
				double TargetColumnLeft = TNumericLimits<double>::Max();
				double PreviousColumnRight = -TNumericLimits<double>::Max();
				for (const int32 NodeIndex : Island.Nodes)
				{
					if (!Nodes.IsValidIndex(NodeIndex))
					{
						continue;
					}
					if (Nodes[NodeIndex].Rank == Group.TargetColumn)
					{
						TargetColumnLeft = FMath::Min(TargetColumnLeft, Nodes[NodeIndex].NewX);
					}
					else if (Nodes[NodeIndex].Rank == Group.TargetColumn - 1)
					{
						PreviousColumnRight = FMath::Max(PreviousColumnRight, Nodes[NodeIndex].NewX + Nodes[NodeIndex].Width);
					}
				}
				double EntryX = -Options.HorizontalSpacing * 0.22;
				if (TargetColumnLeft < TNumericLimits<double>::Max() * 0.5)
				{
					if (PreviousColumnRight > -TNumericLimits<double>::Max() * 0.5)
					{
						EntryX = (PreviousColumnRight + TargetColumnLeft) * 0.5;
					}
					else
					{
						EntryX = TargetColumnLeft - Options.HorizontalSpacing * 0.22;
					}
				}
				double MaxOutputEntryX = TNumericLimits<double>::Max();
				for (const FVector2D& LocalPort : Group.TargetLocalPorts)
				{
					MaxOutputEntryX = FMath::Min(MaxOutputEntryX, ComputeOutputXAlignedToInput(LocalPort.X, Options));
				}
				if (MaxOutputEntryX < TNumericLimits<double>::Max() * 0.5)
				{
					EntryX = FMath::Min(EntryX, MaxOutputEntryX);
				}
				Group.EntryLocal = FVector2D(EntryX, MedianY);
			}

			TSet<int32> GroupsInSingleNodeBoundaryBus;
			for (const TPair<FString, TArray<int32>>& Pair : GroupsByBoundaryBusKey)
			{
				TSet<int32> UniqueTargetNodes;
				for (const int32 GroupIndex : Pair.Value)
				{
					if (!OutInternalBusGroups.IsValidIndex(GroupIndex))
					{
						continue;
					}
					for (const int32 TargetNodeIndex : OutInternalBusGroups[GroupIndex].TargetNodes)
					{
						UniqueTargetNodes.Add(TargetNodeIndex);
					}
				}
				if (UniqueTargetNodes.Num() == 1)
				{
					for (const int32 GroupIndex : Pair.Value)
					{
						GroupsInSingleNodeBoundaryBus.Add(GroupIndex);
					}
				}
			}

			auto ComputeEntryAllowedMaxX = [&Options](const FInternalBusGroup& Group, const double DefaultX, const bool bUseDirectLikeRightLimit) -> double
			{
				double RightMostX = TNumericLimits<double>::Max();
				for (const FVector2D& LocalPort : Group.TargetLocalPorts)
				{
					const bool bSameRowDirectLeft =
						Group.TargetLocalPorts.Num() == 1
						&& FMath::IsNearlyEqual(Group.EntryLocal.X, LocalPort.X, 0.5)
						&& FMath::IsNearlyEqual(Group.EntryLocal.Y, LocalPort.Y, 0.5);
					const double MaxEntryX = (bSameRowDirectLeft || bUseDirectLikeRightLimit)
						? LocalPort.X
						: ComputeOutputXAlignedToInput(LocalPort.X, Options);
					RightMostX = FMath::Min(RightMostX, MaxEntryX);
				}
				if (RightMostX >= TNumericLimits<double>::Max() * 0.5)
				{
					return DefaultX;
				}
				return FMath::Max(DefaultX, RightMostX);
			};

			auto AddEntry = [&](FInternalBusGroup& Group, const FString& Side, const FVector2D& Point)
			{
				if (!Islands.IsValidIndex(Group.IslandId))
				{
					return;
				}
				FBoundaryEntry Entry;
				Entry.Id = OutBoundaryEntries.Num();
				Entry.IslandId = Group.IslandId;
				Entry.InternalBusGroupId = Group.Id;
				Entry.SourceParamId = Group.SourceParamId;
				Entry.Side = Side;
				Entry.LocalPoint = Point;
				Entry.AllowedMinX = Point.X;
				Entry.bSingleNodeBoundaryBus = Side != TEXT("left") && GroupsInSingleNodeBoundaryBus.Contains(Group.Id);
				Entry.AllowedMaxX = ComputeEntryAllowedMaxX(Group, Point.X, Entry.bSingleNodeBoundaryBus);
				OutBoundaryEntries.Add(Entry);
				if (Side == TEXT("left"))
				{
					++Result.DesignDiagnostics.LeftBoundaryEntryCount;
					++Result.DesignDiagnostics.IslandInternalLeftBoundaryEntryCount;
				}
				else if (Side == TEXT("top"))
				{
					++Result.DesignDiagnostics.TopBoundaryEntryCount;
				}
				else if (Side == TEXT("bottom"))
				{
					++Result.DesignDiagnostics.BottomBoundaryEntryCount;
				}
			};

			auto IslandLocalNodeRect = [&Nodes](const int32 NodeIndex) -> FRect
			{
				if (!Nodes.IsValidIndex(NodeIndex))
				{
					return {};
				}
				const FNode& Node = Nodes[NodeIndex];
				return {
					Node.NewX,
					Node.NewY,
					Node.NewX + Node.Width,
					Node.NewY + Node.Height
				};
			};

			auto DirectLeftEntryHitsIslandNode = [&](const FInternalBusGroup& Group, const FIsland& Island, const FVector2D& LeftPoint, const FVector2D& EntryPoint) -> bool
			{
				const double HalfThickness = GetBusHalfThickness();
				FRect SegmentRect;
				SegmentRect.Left = FMath::Min(LeftPoint.X, EntryPoint.X);
				SegmentRect.Right = FMath::Max(LeftPoint.X, EntryPoint.X);
				SegmentRect.Top = FMath::Min(LeftPoint.Y, EntryPoint.Y) - HalfThickness;
				SegmentRect.Bottom = FMath::Max(LeftPoint.Y, EntryPoint.Y) + HalfThickness;
				for (const int32 NodeIndex : Island.Nodes)
				{
					if (!Nodes.IsValidIndex(NodeIndex))
					{
						continue;
					}
					if (Group.TargetNodes.Contains(NodeIndex))
					{
						continue;
					}
					if (SegmentRect.Intersects(IslandLocalNodeRect(NodeIndex), 0.0))
					{
						return true;
					}
				}
				return false;
			};

			auto CanUseDirectLeftEntry = [&](const FInternalBusGroup& Group, const FIsland& Island, const FVector2D& LeftPoint, const FVector2D& TargetInterfacePoint) -> bool
			{
				if (!FMath::IsNearlyEqual(LeftPoint.Y, TargetInterfacePoint.Y, 0.5))
				{
					return false;
				}
				if (LeftPoint.X > TargetInterfacePoint.X + 0.5)
				{
					return false;
				}
				return !DirectLeftEntryHitsIslandNode(Group, Island, LeftPoint, TargetInterfacePoint);
			};

			TSet<int32> GroupsRequiringBoundaryBus;
			for (const TPair<FString, TArray<int32>>& Pair : GroupsByBoundaryBusKey)
			{
				const bool bMultipleInternalGroups = Pair.Value.Num() > 1;
				for (const int32 GroupIndex : Pair.Value)
				{
					if (!OutInternalBusGroups.IsValidIndex(GroupIndex))
					{
						continue;
					}
					FInternalBusGroup& Group = OutInternalBusGroups[GroupIndex];
					if (!Islands.IsValidIndex(Group.IslandId))
					{
						continue;
					}
					const FIsland& Island = Islands[Group.IslandId];
					const double LeftBoundaryX = -Options.HorizontalSpacing * 0.22;
					const bool bSingleIslandInputInterface =
						!bMultipleInternalGroups
						&& Group.EdgeIndices.Num() == 1
						&& Group.TargetLocalPorts.Num() == 1;
					const FVector2D TargetInterfacePoint = bSingleIslandInputInterface
						? Group.TargetLocalPorts[0]
						: Group.EntryLocal;
					const FVector2D LeftBoundaryPoint(LeftBoundaryX, TargetInterfacePoint.Y);
					const bool bDirectLeftValid = !bMultipleInternalGroups
						&& bSingleIslandInputInterface
						&& CanUseDirectLeftEntry(Group, Island, LeftBoundaryPoint, TargetInterfacePoint);
					if (bDirectLeftValid)
					{
						Group.EntryLocal = TargetInterfacePoint;
						AddEntry(Group, TEXT("left"), LeftBoundaryPoint);
					}
					else
					{
						GroupsRequiringBoundaryBus.Add(Group.Id);
						const FVector2D TopPoint(LeftBoundaryX, -Options.VerticalSpacing * 0.30);
						const FVector2D BottomPoint(LeftBoundaryX, Island.LocalBounds.Height() + Options.VerticalSpacing * 0.30);
						AddEntry(Group, TEXT("top"), TopPoint);
						AddEntry(Group, TEXT("bottom"), BottomPoint);
					}
				}
			}

			for (const TPair<FString, TArray<int32>>& Pair : GroupsByBoundaryBusKey)
			{
				bool bBoundaryBusRequired = Pair.Value.Num() > 1;
				for (const int32 GroupIndex : Pair.Value)
				{
					if (GroupsRequiringBoundaryBus.Contains(GroupIndex))
					{
						bBoundaryBusRequired = true;
						break;
					}
				}
				if (!bBoundaryBusRequired)
				{
					continue;
				}
				++Result.DesignDiagnostics.BoundaryBusRequiredCount;
				FBoundaryBus Bus;
				Bus.Id = OutBoundaryBuses.Num();
				Bus.InternalBusGroupIds = Pair.Value;
				if (OutInternalBusGroups.IsValidIndex(Pair.Value[0]))
				{
					Bus.IslandId = OutInternalBusGroups[Pair.Value[0]].IslandId;
					Bus.SourceParamId = OutInternalBusGroups[Pair.Value[0]].SourceParamId;
				}
				for (int32 EntryIndex = 0; EntryIndex < OutBoundaryEntries.Num(); ++EntryIndex)
				{
					if (Pair.Value.Contains(OutBoundaryEntries[EntryIndex].InternalBusGroupId))
					{
						Bus.BoundaryEntryIds.Add(EntryIndex);
					}
				}
				OutBoundaryBuses.Add(Bus);
			}

			Result.DesignDiagnostics.BoundaryEntryCount = OutBoundaryEntries.Num();
			Result.DesignDiagnostics.BoundaryBusCount = OutBoundaryBuses.Num();
			Result.DesignDiagnostics.BoundaryAttachMergeGroupCount = OutBoundaryBuses.Num();
			Result.DesignDiagnostics.InternalAttachMergeGroupCount = OutInternalBusGroups.Num();
			Result.DesignDiagnostics.IslandInternalLeftBoundaryEntryCount = Result.DesignDiagnostics.LeftBoundaryEntryCount;

			for (const FInternalBusGroup& Group : OutInternalBusGroups)
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetNumberField(TEXT("internal_bus_group_id"), Group.Id);
				Obj->SetNumberField(TEXT("island_id"), Group.IslandId);
				Obj->SetStringField(TEXT("source_param_id"), Group.SourceParamId);
				Obj->SetNumberField(TEXT("target_column"), Group.TargetColumn);
				Obj->SetNumberField(TEXT("edge_count"), Group.EdgeIndices.Num());
				Obj->SetBoolField(TEXT("only_leftmost_column"), Group.bOnlyLeftmostColumn);
				Obj->SetBoolField(TEXT("exec_edge_group"), Group.bExec);
				Obj->SetObjectField(TEXT("entry_local"), PointToJson(Group.EntryLocal));
				Obj->SetArrayField(TEXT("edge_indices"), IntArrayToJson(Group.EdgeIndices));
				Result.InternalBusGroupDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
			}
			for (const FBoundaryEntry& Entry : OutBoundaryEntries)
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetNumberField(TEXT("boundary_entry_id"), Entry.Id);
				Obj->SetNumberField(TEXT("island_id"), Entry.IslandId);
				Obj->SetNumberField(TEXT("internal_bus_group_id"), Entry.InternalBusGroupId);
				Obj->SetStringField(TEXT("source_param_id"), Entry.SourceParamId);
				Obj->SetStringField(TEXT("side"), Entry.Side);
				Obj->SetObjectField(TEXT("local_point"), PointToJson(Entry.LocalPoint));
				Obj->SetNumberField(TEXT("allowed_min_x"), Entry.AllowedMinX);
				Obj->SetNumberField(TEXT("allowed_max_x"), Entry.AllowedMaxX);
				Obj->SetBoolField(TEXT("single_node_boundary_bus"), Entry.bSingleNodeBoundaryBus);
				Result.BoundaryEntryDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
			}
			for (const FBoundaryBus& Bus : OutBoundaryBuses)
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetNumberField(TEXT("boundary_bus_id"), Bus.Id);
				Obj->SetNumberField(TEXT("island_id"), Bus.IslandId);
				Obj->SetStringField(TEXT("source_param_id"), Bus.SourceParamId);
				Obj->SetArrayField(TEXT("internal_bus_group_ids"), IntArrayToJson(Bus.InternalBusGroupIds));
				Obj->SetArrayField(TEXT("boundary_entry_ids"), IntArrayToJson(Bus.BoundaryEntryIds));
				Result.BoundaryBusDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
			}

			TMultiMap<int32, int32> EntriesByGroup;
			for (const FBoundaryEntry& Entry : OutBoundaryEntries)
			{
				EntriesByGroup.Add(Entry.InternalBusGroupId, Entry.Id);
			}
			TMap<int32, int32> GroupByEdge;
			for (const FInternalBusGroup& Group : OutInternalBusGroups)
			{
				for (const int32 EdgeIndex : Group.EdgeIndices)
				{
					GroupByEdge.Add(EdgeIndex, Group.Id);
				}
			}
			for (const FIslandEdge& IslandEdge : IslandEdges)
			{
				const int32* GroupIdPtr = GroupByEdge.Find(IslandEdge.EdgeIndex);
				if (!GroupIdPtr)
				{
					continue;
				}
				TArray<int32> EntryIds;
				EntriesByGroup.MultiFind(*GroupIdPtr, EntryIds);
				EntryIds.Sort();
				for (const int32 EntryId : EntryIds)
				{
					if (!OutBoundaryEntries.IsValidIndex(EntryId))
					{
						continue;
					}
					const FBoundaryEntry& Entry = OutBoundaryEntries[EntryId];
					FIslandPortCandidate Candidate;
					Candidate.Id = OutCandidates.Num();
					Candidate.EdgeIndex = IslandEdge.EdgeIndex;
					Candidate.SourceIsland = IslandEdge.SourceIsland;
					Candidate.TargetIsland = IslandEdge.TargetIsland;
					Candidate.BoundaryEntryId = Entry.Id;
					Candidate.InternalBusGroupId = Entry.InternalBusGroupId;
					Candidate.Side = Entry.Side;
					Candidate.SourceLocal = IslandEdge.SourceLocal;
					Candidate.TargetLocal = Entry.LocalPoint;
					Candidate.ActualTargetLocal = IslandEdge.TargetLocal;
					Candidate.AllowedMinX = Entry.AllowedMinX;
					Candidate.AllowedMaxX = Entry.AllowedMaxX;
					Candidate.bSingleNodeBoundaryBus = Entry.bSingleNodeBoundaryBus;
					OutCandidates.Add(Candidate);

					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetNumberField(TEXT("candidate_id"), Candidate.Id);
					Obj->SetNumberField(TEXT("edge_index"), Candidate.EdgeIndex);
					Obj->SetNumberField(TEXT("source_island_id"), Candidate.SourceIsland);
					Obj->SetNumberField(TEXT("target_island_id"), Candidate.TargetIsland);
					Obj->SetNumberField(TEXT("boundary_entry_id"), Candidate.BoundaryEntryId);
					Obj->SetNumberField(TEXT("internal_bus_group_id"), Candidate.InternalBusGroupId);
					Obj->SetStringField(TEXT("side"), Candidate.Side);
					Obj->SetObjectField(TEXT("source_local"), PointToJson(Candidate.SourceLocal));
					Obj->SetObjectField(TEXT("entry_local"), PointToJson(Candidate.TargetLocal));
					Obj->SetObjectField(TEXT("actual_target_local"), PointToJson(Candidate.ActualTargetLocal));
					Obj->SetNumberField(TEXT("allowed_min_x"), Candidate.AllowedMinX);
					Obj->SetNumberField(TEXT("allowed_max_x"), Candidate.AllowedMaxX);
					Obj->SetBoolField(TEXT("single_node_boundary_bus"), Candidate.bSingleNodeBoundaryBus);
					Result.IslandPortCandidateDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}
			Result.DesignDiagnostics.EndpointCandidateCount = OutCandidates.Num();
		}
