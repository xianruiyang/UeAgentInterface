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

			auto AddEntry = [&](FInternalBusGroup& Group, const FString& Side, const FVector2D& Point, bool bContinuousSegment)
			{
				if (!Islands.IsValidIndex(Group.IslandId))
				{
					return;
				}
				const FIsland& Island = Islands[Group.IslandId];
				FBoundaryEntry Entry;
				Entry.Id = OutBoundaryEntries.Num();
				Entry.IslandId = Group.IslandId;
				Entry.InternalBusGroupId = Group.Id;
				Entry.SourceParamId = Group.SourceParamId;
				Entry.Side = Side;
				Entry.LocalPoint = Point;
				Entry.bContinuousSegment = bContinuousSegment;
				if (Side == TEXT("top") || Side == TEXT("bottom"))
				{
					if (bContinuousSegment)
					{
						Entry.AllowedMinX = 0.0;
						Entry.AllowedMaxX = FMath::Clamp(Point.X, 0.0, FMath::Max(0.0, Island.LocalBounds.Width()));
					}
					else
					{
						Entry.AllowedMinX = FMath::Clamp(Point.X, 0.0, FMath::Max(0.0, Island.LocalBounds.Width()));
						Entry.AllowedMaxX = Entry.AllowedMinX;
					}
					Entry.LocalPoint.X = Entry.AllowedMaxX;
				}
				else
				{
					Entry.AllowedMinX = Point.X;
					Entry.AllowedMaxX = Point.X;
				}
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
					const double LeadGap = Options.HorizontalSpacing * 0.20;
					const double BoundaryX = bMultipleInternalGroups
						? FMath::Clamp(Group.EntryLocal.X - LeadGap, 0.0, FMath::Max(0.0, Island.LocalBounds.Width()))
						: FMath::Clamp(Group.EntryLocal.X, 0.0, FMath::Max(0.0, Island.LocalBounds.Width()));
					const double LeftBoundaryX = FMath::Min(-Options.HorizontalSpacing * 0.22, Group.EntryLocal.X);
					const FVector2D LeftBoundaryPoint(LeftBoundaryX, Group.EntryLocal.Y);
					if (Group.bExec || (!bMultipleInternalGroups && Group.bOnlyLeftmostColumn))
					{
						AddEntry(Group, TEXT("left"), LeftBoundaryPoint, false);
					}
					else
					{
						AddEntry(Group, TEXT("left"), LeftBoundaryPoint, false);
						const FVector2D TopPoint(BoundaryX, -Options.VerticalSpacing * 0.30);
						const FVector2D BottomPoint(BoundaryX, Island.LocalBounds.Height() + Options.VerticalSpacing * 0.30);
						if (bMultipleInternalGroups)
						{
							AddEntry(Group, TEXT("top"), TopPoint, true);
							AddEntry(Group, TEXT("bottom"), BottomPoint, true);
						}
						else
						{
							AddEntry(Group, TEXT("top"), TopPoint, false);
							AddEntry(Group, TEXT("bottom"), BottomPoint, false);
							AddEntry(Group, TEXT("top"), TopPoint, true);
							AddEntry(Group, TEXT("bottom"), BottomPoint, true);
						}
					}
				}
			}

			TMap<int32, TArray<int32>> LeftEntriesByIsland;
			for (int32 EntryIndex = 0; EntryIndex < OutBoundaryEntries.Num(); ++EntryIndex)
			{
				if (OutBoundaryEntries[EntryIndex].Side == TEXT("left"))
				{
					LeftEntriesByIsland.FindOrAdd(OutBoundaryEntries[EntryIndex].IslandId).Add(EntryIndex);
				}
			}
			for (TPair<int32, TArray<int32>>& Pair : LeftEntriesByIsland)
			{
				TArray<int32>& Entries = Pair.Value;
				if (Entries.Num() <= 1)
				{
					continue;
				}
				double OriginalMinY = TNumericLimits<double>::Max();
				double OriginalMaxY = -TNumericLimits<double>::Max();
				for (const int32 EntryIndex : Entries)
				{
					OriginalMinY = FMath::Min(OriginalMinY, OutBoundaryEntries[EntryIndex].LocalPoint.Y);
					OriginalMaxY = FMath::Max(OriginalMaxY, OutBoundaryEntries[EntryIndex].LocalPoint.Y);
				}
				Entries.Sort([&OutBoundaryEntries](int32 A, int32 B)
				{
					if (!FMath::IsNearlyEqual(OutBoundaryEntries[A].LocalPoint.Y, OutBoundaryEntries[B].LocalPoint.Y, 1.0))
					{
						return OutBoundaryEntries[A].LocalPoint.Y < OutBoundaryEntries[B].LocalPoint.Y;
					}
					return A < B;
				});
				double CursorY = -TNumericLimits<double>::Max();
				const double MinEntryGap = FMath::Max(18.0, Options.NodePadding * 0.75);
				for (const int32 EntryIndex : Entries)
				{
					FBoundaryEntry& Entry = OutBoundaryEntries[EntryIndex];
					Entry.LocalPoint.Y = FMath::Max(Entry.LocalPoint.Y, CursorY + MinEntryGap);
					CursorY = Entry.LocalPoint.Y;
				}
				const double NewCenter = (OutBoundaryEntries[Entries[0]].LocalPoint.Y + OutBoundaryEntries[Entries.Last()].LocalPoint.Y) * 0.5;
				const double OldCenter = (OriginalMinY + OriginalMaxY) * 0.5;
				const double Shift = OldCenter - NewCenter;
				for (const int32 EntryIndex : Entries)
				{
					FBoundaryEntry& Entry = OutBoundaryEntries[EntryIndex];
					Entry.LocalPoint.Y += Shift;
					if (OutInternalBusGroups.IsValidIndex(Entry.InternalBusGroupId))
					{
						OutInternalBusGroups[Entry.InternalBusGroupId].EntryLocal = Entry.LocalPoint;
					}
				}
			}

			for (const TPair<FString, TArray<int32>>& Pair : GroupsByBoundaryBusKey)
			{
				if (Pair.Value.Num() <= 1)
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
				Obj->SetBoolField(TEXT("continuous_segment"), Entry.bContinuousSegment);
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
					Candidate.bContinuousSegment = Entry.bContinuousSegment;
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
					Obj->SetBoolField(TEXT("continuous_segment"), Candidate.bContinuousSegment);
					Result.IslandPortCandidateDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}
			Result.DesignDiagnostics.EndpointCandidateCount = OutCandidates.Num();
		}
