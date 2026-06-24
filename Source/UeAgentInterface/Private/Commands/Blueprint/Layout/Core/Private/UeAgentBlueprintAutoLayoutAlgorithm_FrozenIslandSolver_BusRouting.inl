		static void BuildInterIslandBusesAndBundles(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			TArray<FDeterministicIslandConnection>& Connections,
			const FOptions& Options,
			const FString& BuildStageName,
			TArray<FDeterministicInterIslandBus>& OutBuses,
			TArray<FDeterministicInterIslandBusBundle>& OutBundles,
			FRunResult& Result,
			const bool bRecordDiagnostics = true)
		{
			OutBuses.Reset();
			OutBundles.Reset();
			TMap<FString, int32> BusByKey;
			for (int32 ConnectionIndex = 0; ConnectionIndex < Connections.Num(); ++ConnectionIndex)
			{
				FDeterministicIslandConnection& Connection = Connections[ConnectionIndex];
				if (!Connection.bBus || !Edges.IsValidIndex(Connection.Endpoint.EdgeIndex))
				{
					continue;
				}
				const FString Key = MakeInterIslandBusKey(Edges[Connection.Endpoint.EdgeIndex]);
				int32 BusId = INDEX_NONE;
				if (const int32* Existing = BusByKey.Find(Key))
				{
					BusId = *Existing;
					if (bRecordDiagnostics)
					{
						++Result.DesignDiagnostics.InterIslandBusDuplicateMergeCount;
					}
				}
				else
				{
					FDeterministicInterIslandBus Bus;
					Bus.Id = OutBuses.Num();
					Bus.Key = Key;
					Bus.SourcePoint = Connection.SourcePoint;
					Bus.XMin = TNumericLimits<double>::Max();
					Bus.XMax = -TNumericLimits<double>::Max();
					Bus.PreferredY = Connection.SourcePoint.Y;
					OutBuses.Add(Bus);
					BusId = Bus.Id;
					BusByKey.Add(Key, BusId);
				}
				FDeterministicInterIslandBus& Bus = OutBuses[BusId];
				Connection.BusId = BusId;
				Bus.ConnectionIndices.Add(ConnectionIndex);
				double TrunkXMin = 0.0;
				double TrunkXMax = 0.0;
				ComputeInterIslandBusTrunkXCoverage(Connection, Options, TrunkXMin, TrunkXMax);
				Bus.XMin = FMath::Min(Bus.XMin, TrunkXMin);
				Bus.XMax = FMath::Max(Bus.XMax, TrunkXMax);
				double SumY = Bus.SourcePoint.Y;
				for (const int32 ExistingConnectionIndex : Bus.ConnectionIndices)
				{
					if (Connections.IsValidIndex(ExistingConnectionIndex))
					{
						SumY += Connections[ExistingConnectionIndex].ActualTargetPoint.Y;
					}
				}
				Bus.PreferredY = SumY / static_cast<double>(Bus.ConnectionIndices.Num() + 1);
			}
			if (bRecordDiagnostics)
			{
				Result.DesignDiagnostics.InterIslandBusCount = OutBuses.Num();
				Result.DesignDiagnostics.ConnectionGroupCount = OutBuses.Num() + Result.DesignDiagnostics.DirectIslandConnectionCount;
				Result.DesignDiagnostics.RouteGroupCount = Result.DesignDiagnostics.ConnectionGroupCount;
			}

			TArray<int32> Parent;
			Parent.SetNum(OutBuses.Num());
			for (int32 Index = 0; Index < Parent.Num(); ++Index)
			{
				Parent[Index] = Index;
			}
			TFunction<int32(int32)> Find = [&](int32 Index) -> int32
			{
				while (Parent.IsValidIndex(Index) && Parent[Index] != Index)
				{
					Parent[Index] = Parent[Parent[Index]];
					Index = Parent[Index];
				}
				return Index;
			};
			auto Union = [&](int32 A, int32 B)
			{
				A = Find(A);
				B = Find(B);
				if (A != B)
				{
					Parent[B] = A;
				}
			};
			const double BundleJoinThreshold = FMath::Max(60.0, Options.VerticalSpacing * 0.75);
			TArray<TSharedPtr<FJsonValue>> MergePairDiagnostics;
			int32 CandidatePairCount = 0;
			for (int32 A = 0; A < OutBuses.Num(); ++A)
			{
				for (int32 B = A + 1; B < OutBuses.Num(); ++B)
				{
					++CandidatePairCount;
					if (IntervalOverlaps(OutBuses[A].XMin, OutBuses[A].XMax, OutBuses[B].XMin, OutBuses[B].XMax, 0.0)
						&& FMath::Abs(OutBuses[A].PreferredY - OutBuses[B].PreferredY) <= BundleJoinThreshold)
					{
						TSharedPtr<FJsonObject> MergeObj = MakeShared<FJsonObject>();
						MergeObj->SetNumberField(TEXT("bus_id_a"), OutBuses[A].Id);
						MergeObj->SetNumberField(TEXT("bus_id_b"), OutBuses[B].Id);
						MergeObj->SetStringField(TEXT("bus_key_a"), OutBuses[A].Key);
						MergeObj->SetStringField(TEXT("bus_key_b"), OutBuses[B].Key);
						MergeObj->SetNumberField(TEXT("preferred_y_a"), OutBuses[A].PreferredY);
						MergeObj->SetNumberField(TEXT("preferred_y_b"), OutBuses[B].PreferredY);
						MergeObj->SetNumberField(TEXT("preferred_y_delta"), FMath::Abs(OutBuses[A].PreferredY - OutBuses[B].PreferredY));
						MergeObj->SetNumberField(TEXT("x_min_a"), OutBuses[A].XMin);
						MergeObj->SetNumberField(TEXT("x_max_a"), OutBuses[A].XMax);
						MergeObj->SetNumberField(TEXT("x_min_b"), OutBuses[B].XMin);
						MergeObj->SetNumberField(TEXT("x_max_b"), OutBuses[B].XMax);
						MergePairDiagnostics.Add(MakeShared<FJsonValueObject>(MergeObj));
						Union(A, B);
					}
				}
			}
			TMap<int32, int32> BundleByRoot;
			for (int32 BusIndex = 0; BusIndex < OutBuses.Num(); ++BusIndex)
			{
				const int32 Root = Find(BusIndex);
				int32* BundleIdPtr = BundleByRoot.Find(Root);
				if (!BundleIdPtr)
				{
					FDeterministicInterIslandBusBundle Bundle;
					Bundle.Id = OutBundles.Num();
					OutBundles.Add(Bundle);
					BundleByRoot.Add(Root, Bundle.Id);
					BundleIdPtr = BundleByRoot.Find(Root);
				}
				FDeterministicInterIslandBusBundle& Bundle = OutBundles[*BundleIdPtr];
				Bundle.BusIds.Add(BusIndex);
				OutBuses[BusIndex].BundleId = Bundle.Id;
			}
			for (FDeterministicInterIslandBusBundle& Bundle : OutBundles)
			{
				double PreferredY = 0.0;
				double XMin = TNumericLimits<double>::Max();
				double XMax = -TNumericLimits<double>::Max();
				for (const int32 BusId : Bundle.BusIds)
				{
					if (!OutBuses.IsValidIndex(BusId))
					{
						continue;
					}
					if (OutBuses[BusId].XMin == TNumericLimits<double>::Max())
					{
						OutBuses[BusId].XMin = OutBuses[BusId].SourcePoint.X;
						OutBuses[BusId].XMax = OutBuses[BusId].SourcePoint.X;
					}
					PreferredY += OutBuses[BusId].PreferredY;
					XMin = FMath::Min(XMin, OutBuses[BusId].XMin);
					XMax = FMath::Max(XMax, OutBuses[BusId].XMax);
					OutBuses[BusId].SlotIndex = INDEX_NONE;
					OutBuses[BusId].SignedSlotIndex = 0;
					OutBuses[BusId].FinalY = OutBuses[BusId].PreferredY;
				}
				Bundle.PreferredY = PreferredY / FMath::Max(1, Bundle.BusIds.Num());
				Bundle.FinalCenterY = Bundle.PreferredY;
				Bundle.XMin = XMin == TNumericLimits<double>::Max() ? 0.0 : XMin;
				Bundle.XMax = XMax == -TNumericLimits<double>::Max() ? Bundle.XMin : XMax;
				Bundle.MaxSlotIndex = 0;
				Bundle.CollisionBoxes.Reset();
			}
			ComputeInterIslandBusSlotBiases(Connections, OutBuses);
			{
				TSharedPtr<FJsonObject> StageObj = MakeShared<FJsonObject>();
				StageObj->SetStringField(TEXT("stage"), BuildStageName);
				StageObj->SetNumberField(TEXT("sequence_index"), Result.InterIslandBusBuildDiagnostics.Num());
				StageObj->SetBoolField(TEXT("recorded_final_diagnostics"), bRecordDiagnostics);
				StageObj->SetNumberField(TEXT("bus_count"), OutBuses.Num());
				StageObj->SetNumberField(TEXT("bundle_count"), OutBundles.Num());
				StageObj->SetNumberField(TEXT("candidate_pair_count"), CandidatePairCount);
				StageObj->SetNumberField(TEXT("merged_pair_count"), MergePairDiagnostics.Num());
				StageObj->SetNumberField(TEXT("bundle_join_threshold"), BundleJoinThreshold);
				StageObj->SetArrayField(TEXT("merged_pairs"), MergePairDiagnostics);

				TArray<TSharedPtr<FJsonValue>> BusValues;
				BusValues.Reserve(OutBuses.Num());
				for (const FDeterministicInterIslandBus& Bus : OutBuses)
				{
					TSharedPtr<FJsonObject> BusObj = MakeShared<FJsonObject>();
					BusObj->SetNumberField(TEXT("bus_id"), Bus.Id);
					BusObj->SetStringField(TEXT("bus_key"), Bus.Key);
					BusObj->SetNumberField(TEXT("bundle_id"), Bus.BundleId);
					BusObj->SetNumberField(TEXT("preferred_y"), Bus.PreferredY);
					BusObj->SetNumberField(TEXT("x_min"), Bus.XMin);
					BusObj->SetNumberField(TEXT("x_max"), Bus.XMax);
					BusObj->SetObjectField(TEXT("source_point"), PointToJson(Bus.SourcePoint));
					BusObj->SetNumberField(TEXT("connection_count"), Bus.ConnectionIndices.Num());
					BusObj->SetArrayField(TEXT("connection_indices"), IntArrayToJson(Bus.ConnectionIndices));

					TArray<int32> EdgeIndices;
					TArray<TSharedPtr<FJsonValue>> ConnectionValues;
					for (const int32 ConnectionIndex : Bus.ConnectionIndices)
					{
						if (!Connections.IsValidIndex(ConnectionIndex))
						{
							continue;
						}
						const FDeterministicIslandConnection& Connection = Connections[ConnectionIndex];
						const int32 EdgeIndex = Connection.Endpoint.EdgeIndex;
						EdgeIndices.Add(EdgeIndex);
						TSharedPtr<FJsonObject> ConnectionObj = MakeShared<FJsonObject>();
						ConnectionObj->SetNumberField(TEXT("connection_index"), ConnectionIndex);
						ConnectionObj->SetNumberField(TEXT("edge_index"), EdgeIndex);
						ConnectionObj->SetNumberField(TEXT("source_island_id"), Connection.Endpoint.SourceIsland);
						ConnectionObj->SetNumberField(TEXT("target_island_id"), Connection.Endpoint.TargetIsland);
						ConnectionObj->SetStringField(TEXT("target_side"), Connection.Endpoint.Side);
						ConnectionObj->SetObjectField(TEXT("source_point"), PointToJson(Connection.SourcePoint));
						ConnectionObj->SetObjectField(TEXT("entry_point"), PointToJson(Connection.EntryPoint));
						ConnectionObj->SetObjectField(TEXT("actual_target_point"), PointToJson(Connection.ActualTargetPoint));
						if (Edges.IsValidIndex(EdgeIndex))
						{
							const FEdge& Edge = Edges[EdgeIndex];
							ConnectionObj->SetStringField(TEXT("source_pin"), Edge.SourcePin);
							ConnectionObj->SetStringField(TEXT("target_pin"), Edge.TargetPin);
							ConnectionObj->SetStringField(TEXT("source_title"), Nodes.IsValidIndex(Edge.Source) ? Nodes[Edge.Source].Title : FString());
							ConnectionObj->SetStringField(TEXT("target_title"), Nodes.IsValidIndex(Edge.Target) ? Nodes[Edge.Target].Title : FString());
						}
						ConnectionValues.Add(MakeShared<FJsonValueObject>(ConnectionObj));
					}
					BusObj->SetArrayField(TEXT("edge_indices"), IntArrayToJson(EdgeIndices));
					BusObj->SetArrayField(TEXT("connections"), ConnectionValues);
					BusValues.Add(MakeShared<FJsonValueObject>(BusObj));
				}
				StageObj->SetArrayField(TEXT("buses"), BusValues);

				TArray<TSharedPtr<FJsonValue>> BundleValues;
				BundleValues.Reserve(OutBundles.Num());
				for (const FDeterministicInterIslandBusBundle& Bundle : OutBundles)
				{
					TSharedPtr<FJsonObject> BundleObj = MakeShared<FJsonObject>();
					BundleObj->SetNumberField(TEXT("bundle_id"), Bundle.Id);
					BundleObj->SetNumberField(TEXT("bus_count"), Bundle.BusIds.Num());
					BundleObj->SetNumberField(TEXT("preferred_y"), Bundle.PreferredY);
					BundleObj->SetNumberField(TEXT("x_min"), Bundle.XMin);
					BundleObj->SetNumberField(TEXT("x_max"), Bundle.XMax);
					BundleObj->SetArrayField(TEXT("bus_ids"), IntArrayToJson(Bundle.BusIds));
					TArray<TSharedPtr<FJsonValue>> BusKeyValues;
					for (const int32 BusId : Bundle.BusIds)
					{
						if (OutBuses.IsValidIndex(BusId))
						{
							BusKeyValues.Add(MakeShared<FJsonValueString>(OutBuses[BusId].Key));
						}
					}
					BundleObj->SetArrayField(TEXT("bus_keys"), BusKeyValues);
					BundleValues.Add(MakeShared<FJsonValueObject>(BundleObj));
				}
				StageObj->SetArrayField(TEXT("bundles"), BundleValues);
				Result.InterIslandBusBuildDiagnostics.Add(MakeShared<FJsonValueObject>(StageObj));
			}
			if (bRecordDiagnostics)
			{
				Result.DesignDiagnostics.InterIslandBusBundleCount = OutBundles.Num();
			}
		}

		static void AppendInterIslandBusBuildSnapshot(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FDeterministicIslandConnection>& Connections,
			const FOptions& Options,
			const FString& StageName,
			const TArray<FDeterministicInterIslandBus>& Buses,
			const TArray<FDeterministicInterIslandBusBundle>& Bundles,
			FRunResult& Result,
			const bool bRecordDiagnostics = false)
		{
			const double BundleJoinThreshold = FMath::Max(60.0, Options.VerticalSpacing * 0.75);
			TArray<TSharedPtr<FJsonValue>> MergePairDiagnostics;
			int32 CandidatePairCount = 0;
			for (int32 A = 0; A < Buses.Num(); ++A)
			{
				for (int32 B = A + 1; B < Buses.Num(); ++B)
				{
					++CandidatePairCount;
					if (Buses[A].BundleId != INDEX_NONE && Buses[A].BundleId == Buses[B].BundleId)
					{
						TSharedPtr<FJsonObject> MergeObj = MakeShared<FJsonObject>();
						MergeObj->SetNumberField(TEXT("bus_id_a"), Buses[A].Id);
						MergeObj->SetNumberField(TEXT("bus_id_b"), Buses[B].Id);
						MergeObj->SetStringField(TEXT("bus_key_a"), Buses[A].Key);
						MergeObj->SetStringField(TEXT("bus_key_b"), Buses[B].Key);
						MergeObj->SetNumberField(TEXT("preferred_y_a"), Buses[A].PreferredY);
						MergeObj->SetNumberField(TEXT("preferred_y_b"), Buses[B].PreferredY);
						MergeObj->SetNumberField(TEXT("preferred_y_delta"), FMath::Abs(Buses[A].PreferredY - Buses[B].PreferredY));
						MergeObj->SetNumberField(TEXT("x_min_a"), Buses[A].XMin);
						MergeObj->SetNumberField(TEXT("x_max_a"), Buses[A].XMax);
						MergeObj->SetNumberField(TEXT("x_min_b"), Buses[B].XMin);
						MergeObj->SetNumberField(TEXT("x_max_b"), Buses[B].XMax);
						MergeObj->SetBoolField(TEXT("snapshot_same_bundle"), true);
						MergePairDiagnostics.Add(MakeShared<FJsonValueObject>(MergeObj));
					}
				}
			}

			TSharedPtr<FJsonObject> StageObj = MakeShared<FJsonObject>();
			StageObj->SetStringField(TEXT("stage"), StageName);
			StageObj->SetNumberField(TEXT("sequence_index"), Result.InterIslandBusBuildDiagnostics.Num());
			StageObj->SetBoolField(TEXT("recorded_final_diagnostics"), bRecordDiagnostics);
			StageObj->SetBoolField(TEXT("snapshot_only"), true);
			StageObj->SetNumberField(TEXT("bus_count"), Buses.Num());
			StageObj->SetNumberField(TEXT("bundle_count"), Bundles.Num());
			StageObj->SetNumberField(TEXT("candidate_pair_count"), CandidatePairCount);
			StageObj->SetNumberField(TEXT("merged_pair_count"), MergePairDiagnostics.Num());
			StageObj->SetNumberField(TEXT("bundle_join_threshold"), BundleJoinThreshold);
			StageObj->SetArrayField(TEXT("merged_pairs"), MergePairDiagnostics);

			TArray<TSharedPtr<FJsonValue>> BusValues;
			BusValues.Reserve(Buses.Num());
			for (const FDeterministicInterIslandBus& Bus : Buses)
			{
				TSharedPtr<FJsonObject> BusObj = MakeShared<FJsonObject>();
				BusObj->SetNumberField(TEXT("bus_id"), Bus.Id);
				BusObj->SetStringField(TEXT("bus_key"), Bus.Key);
				BusObj->SetNumberField(TEXT("bundle_id"), Bus.BundleId);
				BusObj->SetNumberField(TEXT("preferred_y"), Bus.PreferredY);
				BusObj->SetNumberField(TEXT("final_y"), Bus.FinalY);
				BusObj->SetNumberField(TEXT("slot_index"), Bus.SlotIndex);
				BusObj->SetNumberField(TEXT("signed_slot_index"), Bus.SignedSlotIndex);
				BusObj->SetNumberField(TEXT("x_min"), Bus.XMin);
				BusObj->SetNumberField(TEXT("x_max"), Bus.XMax);
				BusObj->SetObjectField(TEXT("source_point"), PointToJson(Bus.SourcePoint));
				BusObj->SetNumberField(TEXT("connection_count"), Bus.ConnectionIndices.Num());
				BusObj->SetArrayField(TEXT("connection_indices"), IntArrayToJson(Bus.ConnectionIndices));

				TArray<int32> EdgeIndices;
				TArray<TSharedPtr<FJsonValue>> ConnectionValues;
				for (const int32 ConnectionIndex : Bus.ConnectionIndices)
				{
					if (!Connections.IsValidIndex(ConnectionIndex))
					{
						continue;
					}
					const FDeterministicIslandConnection& Connection = Connections[ConnectionIndex];
					const int32 EdgeIndex = Connection.Endpoint.EdgeIndex;
					EdgeIndices.Add(EdgeIndex);
					TSharedPtr<FJsonObject> ConnectionObj = MakeShared<FJsonObject>();
					ConnectionObj->SetNumberField(TEXT("connection_index"), ConnectionIndex);
					ConnectionObj->SetNumberField(TEXT("edge_index"), EdgeIndex);
					ConnectionObj->SetNumberField(TEXT("source_island_id"), Connection.Endpoint.SourceIsland);
					ConnectionObj->SetNumberField(TEXT("target_island_id"), Connection.Endpoint.TargetIsland);
					ConnectionObj->SetStringField(TEXT("target_side"), Connection.Endpoint.Side);
					ConnectionObj->SetObjectField(TEXT("source_point"), PointToJson(Connection.SourcePoint));
					ConnectionObj->SetObjectField(TEXT("entry_point"), PointToJson(Connection.EntryPoint));
					ConnectionObj->SetObjectField(TEXT("actual_target_point"), PointToJson(Connection.ActualTargetPoint));
					ConnectionObj->SetNumberField(TEXT("branch_slot_index"), Connection.BranchSlotIndex);
					ConnectionObj->SetBoolField(TEXT("currently_bus"), Connection.bBus);
					ConnectionObj->SetBoolField(TEXT("currently_direct"), Connection.bDirect);
					if (Edges.IsValidIndex(EdgeIndex))
					{
						const FEdge& Edge = Edges[EdgeIndex];
						ConnectionObj->SetStringField(TEXT("source_pin"), Edge.SourcePin);
						ConnectionObj->SetStringField(TEXT("target_pin"), Edge.TargetPin);
						ConnectionObj->SetStringField(TEXT("source_title"), Nodes.IsValidIndex(Edge.Source) ? Nodes[Edge.Source].Title : FString());
						ConnectionObj->SetStringField(TEXT("target_title"), Nodes.IsValidIndex(Edge.Target) ? Nodes[Edge.Target].Title : FString());
					}
					ConnectionValues.Add(MakeShared<FJsonValueObject>(ConnectionObj));
				}
				BusObj->SetArrayField(TEXT("edge_indices"), IntArrayToJson(EdgeIndices));
				BusObj->SetArrayField(TEXT("connections"), ConnectionValues);
				BusValues.Add(MakeShared<FJsonValueObject>(BusObj));
			}
			StageObj->SetArrayField(TEXT("buses"), BusValues);

			TArray<TSharedPtr<FJsonValue>> BundleValues;
			BundleValues.Reserve(Bundles.Num());
			for (const FDeterministicInterIslandBusBundle& Bundle : Bundles)
			{
				TSharedPtr<FJsonObject> BundleObj = MakeShared<FJsonObject>();
				BundleObj->SetNumberField(TEXT("bundle_id"), Bundle.Id);
				BundleObj->SetNumberField(TEXT("bus_count"), Bundle.BusIds.Num());
				BundleObj->SetNumberField(TEXT("preferred_y"), Bundle.PreferredY);
				BundleObj->SetNumberField(TEXT("final_center_y"), Bundle.FinalCenterY);
				BundleObj->SetNumberField(TEXT("x_min"), Bundle.XMin);
				BundleObj->SetNumberField(TEXT("x_max"), Bundle.XMax);
				BundleObj->SetArrayField(TEXT("bus_ids"), IntArrayToJson(Bundle.BusIds));
				TArray<TSharedPtr<FJsonValue>> BusKeyValues;
				for (const int32 BusId : Bundle.BusIds)
				{
					if (Buses.IsValidIndex(BusId))
					{
						BusKeyValues.Add(MakeShared<FJsonValueString>(Buses[BusId].Key));
					}
				}
				BundleObj->SetArrayField(TEXT("bus_keys"), BusKeyValues);
				BundleValues.Add(MakeShared<FJsonValueObject>(BundleObj));
			}
			StageObj->SetArrayField(TEXT("bundles"), BundleValues);
			Result.InterIslandBusBuildDiagnostics.Add(MakeShared<FJsonValueObject>(StageObj));

			if (bRecordDiagnostics)
			{
				Result.DesignDiagnostics.InterIslandBusCount = Buses.Num();
				Result.DesignDiagnostics.ConnectionGroupCount = Buses.Num() + Result.DesignDiagnostics.DirectIslandConnectionCount;
				Result.DesignDiagnostics.RouteGroupCount = Result.DesignDiagnostics.ConnectionGroupCount;
				Result.DesignDiagnostics.InterIslandBusBundleCount = Bundles.Num();
			}
		}

		struct FInterIslandBundleGeometryPlan
		{
			TArray<FXStripCollisionBox> IslandBoxes;
			TArray<FBundleXPartition> Partitions;
			TArray<FPartitionBandConstraint> BandConstraints;
		};

		struct FPartitionIslandBandItem
		{
			int32 IslandId = INDEX_NONE;
			double Top = TNumericLimits<double>::Max();
			double Bottom = -TNumericLimits<double>::Max();
			double CenterY = 0.0;
			double CenterYSum = 0.0;
			int32 CenterYCount = 0;
		};

		static void AddUniqueSortedXEvent(TArray<double>& Events, const double Value)
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
		}

		static void RebuildInterIslandBundleCollisionBoxes(
			const TArray<FDeterministicInterIslandBus>& Buses,
			const double AvoidanceY,
			FDeterministicInterIslandBusBundle& Bundle)
		{
			Bundle.CollisionBoxes.Reset();
			TArray<double> Events;
			for (const int32 BusId : Bundle.BusIds)
			{
				if (!Buses.IsValidIndex(BusId))
				{
					continue;
				}
				AddUniqueSortedXEvent(Events, Buses[BusId].XMin);
				AddUniqueSortedXEvent(Events, Buses[BusId].XMax);
			}
			Events.Sort();
			if (Events.Num() < 2)
			{
				return;
			}
			for (int32 EventIndex = 0; EventIndex + 1 < Events.Num(); ++EventIndex)
			{
				const double X0 = Events[EventIndex];
				const double X1 = Events[EventIndex + 1];
				if (X1 <= X0 + 0.5)
				{
					continue;
				}
				double MinY = TNumericLimits<double>::Max();
				double MaxY = -TNumericLimits<double>::Max();
				const double MidX = (X0 + X1) * 0.5;
				for (const int32 BusId : Bundle.BusIds)
				{
					if (!Buses.IsValidIndex(BusId))
					{
						continue;
					}
					const FDeterministicInterIslandBus& Bus = Buses[BusId];
					if (Bus.XMin <= MidX + 0.5 && Bus.XMax >= MidX - 0.5)
					{
						MinY = FMath::Min(MinY, Bus.FinalY);
						MaxY = FMath::Max(MaxY, Bus.FinalY);
					}
				}
				if (MinY != TNumericLimits<double>::Max())
				{
					AddXStripCollisionBox(
						Bundle.CollisionBoxes,
						EXStripCollisionOwnerKind::InterIslandBusBundle,
						Bundle.Id,
						INDEX_NONE,
						{ X0, MinY - AvoidanceY, X1, MaxY + AvoidanceY },
						Bundle.FinalCenterY);
				}
			}
		}

		static void RefreshInterIslandBundleBusYAndCollision(
			TArray<FDeterministicInterIslandBus>& Buses,
			const double LanePitchY,
			const double AvoidanceY,
			FDeterministicInterIslandBusBundle& Bundle)
		{
			const double SlotCenter = static_cast<double>(Bundle.MaxSlotIndex) * 0.5;
			for (const int32 BusId : Bundle.BusIds)
			{
				if (Buses.IsValidIndex(BusId))
				{
					Buses[BusId].FinalY = SnapGraphCoordinate(Bundle.FinalCenterY + (Buses[BusId].SlotIndex - SlotCenter) * LanePitchY);
				}
			}
			RebuildInterIslandBundleCollisionBoxes(Buses, AvoidanceY, Bundle);
		}

		static void ApplyA44InterIslandBundleSlotGeometry(
			TArray<FDeterministicInterIslandBus>& Buses,
			TArray<FDeterministicInterIslandBusBundle>& Bundles,
			const double LanePitchY,
			const double AvoidanceY,
			FRunResult& Result,
			const bool bRecordDiagnostics)
		{
			for (FDeterministicInterIslandBusBundle& Bundle : Bundles)
			{
				AssignInterIslandBusSlotsByBias(Buses, Bundle, Result, bRecordDiagnostics);
				Bundle.FinalCenterY = SnapGraphCoordinate(Bundle.PreferredY);
				RefreshInterIslandBundleBusYAndCollision(Buses, LanePitchY, AvoidanceY, Bundle);
				if (bRecordDiagnostics)
				{
					Result.DesignDiagnostics.InterIslandBusMaxSlotCount = FMath::Max(Result.DesignDiagnostics.InterIslandBusMaxSlotCount, Bundle.MaxSlotIndex + 1);
					Result.DesignDiagnostics.CrossBusTrunkLaneCount = FMath::Max(Result.DesignDiagnostics.CrossBusTrunkLaneCount, Bundle.MaxSlotIndex + 1);
					Result.DesignDiagnostics.CrossBusLaneCount = FMath::Max(Result.DesignDiagnostics.CrossBusLaneCount, Bundle.MaxSlotIndex + 1);
					Result.DesignDiagnostics.LaneColoringLaneCount = FMath::Max(Result.DesignDiagnostics.LaneColoringLaneCount, Bundle.MaxSlotIndex + 1);
					Result.DesignDiagnostics.InterIslandBusBundleCollisionBoxCount += Bundle.CollisionBoxes.Num();
				}
			}
		}

		static double InterIslandBundleCenterYInPartition(
			const TArray<FDeterministicInterIslandBusBundle>& Bundles,
			const int32 BundleId,
			const double X0,
			const double X1)
		{
			if (!Bundles.IsValidIndex(BundleId))
			{
				return 0.0;
			}
			double CenterYSum = 0.0;
			int32 CenterYCount = 0;
			for (const FXStripCollisionBox& Box : Bundles[BundleId].CollisionBoxes)
			{
				if (!XStripOverlapsXRange(Box, X0, X1, 0.0))
				{
					continue;
				}
				CenterYSum += Box.CenterY();
				++CenterYCount;
			}
			return CenterYCount > 0 ? CenterYSum / static_cast<double>(CenterYCount) : 0.0;
		}

		static bool ComputeInterIslandBundleBoundsInPartition(
			const TArray<FDeterministicInterIslandBusBundle>& Bundles,
			const int32 BundleId,
			const double X0,
			const double X1,
			double& OutTop,
			double& OutBottom)
		{
			OutTop = TNumericLimits<double>::Max();
			OutBottom = -TNumericLimits<double>::Max();
			if (!Bundles.IsValidIndex(BundleId))
			{
				return false;
			}
			for (const FXStripCollisionBox& Box : Bundles[BundleId].CollisionBoxes)
			{
				if (!XStripOverlapsXRange(Box, X0, X1, 0.0))
				{
					continue;
				}
				OutTop = FMath::Min(OutTop, Box.Rect.Top);
				OutBottom = FMath::Max(OutBottom, Box.Rect.Bottom);
			}
			return OutTop != TNumericLimits<double>::Max() && OutBottom > OutTop + 0.5;
		}

		static TArray<FBundleXPartition> BuildA48InterIslandBundleXPartitions(const TArray<FDeterministicInterIslandBusBundle>& Bundles)
		{
			TArray<double> PartitionEvents;
			for (const FDeterministicInterIslandBusBundle& Bundle : Bundles)
			{
				double StartX = TNumericLimits<double>::Max();
				double EndX = -TNumericLimits<double>::Max();
				for (const FXStripCollisionBox& Box : Bundle.CollisionBoxes)
				{
					StartX = FMath::Min(StartX, Box.Rect.Left);
					EndX = FMath::Max(EndX, Box.Rect.Right);
				}
				if (StartX == TNumericLimits<double>::Max())
				{
					continue;
				}
				AddUniqueSortedXEvent(PartitionEvents, StartX);
				AddUniqueSortedXEvent(PartitionEvents, EndX);
			}
			PartitionEvents.Sort();
			TArray<FBundleXPartition> Partitions;
			for (int32 EventIndex = 0; EventIndex + 1 < PartitionEvents.Num(); ++EventIndex)
			{
				const double X0 = PartitionEvents[EventIndex];
				const double X1 = PartitionEvents[EventIndex + 1];
				if (X1 <= X0 + 0.5)
				{
					continue;
				}
				FBundleXPartition Partition;
				Partition.X0 = X0;
				Partition.X1 = X1;
				Partition.MidX = (X0 + X1) * 0.5;
				for (const FDeterministicInterIslandBusBundle& Bundle : Bundles)
				{
					bool bActiveInPartition = false;
					for (const FXStripCollisionBox& BundleBox : Bundle.CollisionBoxes)
					{
						if (XStripOverlapsXRange(BundleBox, Partition.X0, Partition.X1, 0.0))
						{
							bActiveInPartition = true;
							break;
						}
					}
					if (bActiveInPartition)
					{
						Partition.ActiveBundleIds.Add(Bundle.Id);
					}
				}
				Partition.ActiveBundleIds.Sort([&Bundles, &Partition](const int32 A, const int32 B)
				{
					if (!Bundles.IsValidIndex(A) || !Bundles.IsValidIndex(B))
					{
						return A < B;
					}
					const double CenterA = InterIslandBundleCenterYInPartition(Bundles, A, Partition.X0, Partition.X1);
					const double CenterB = InterIslandBundleCenterYInPartition(Bundles, B, Partition.X0, Partition.X1);
					if (!FMath::IsNearlyEqual(CenterA, CenterB, 0.5))
					{
						return CenterA < CenterB;
					}
					return A < B;
				});
				if (Partition.ActiveBundleIds.Num() > 0)
				{
					Partitions.Add(Partition);
				}
			}
			return Partitions;
		}

		static TArray<FPartitionIslandBandItem> BuildA48PartitionIslandItems(
			const TArray<FXStripCollisionBox>& IslandBoxes,
			const FBundleXPartition& Partition)
		{
			TArray<FPartitionIslandBandItem> Items;
			TMap<int32, int32> ItemIndexByIsland;
			for (const FXStripCollisionBox& IslandBox : IslandBoxes)
			{
				if (IslandBox.OwnerKind != EXStripCollisionOwnerKind::Island
					|| IslandBox.OwnerId == INDEX_NONE
					|| !XStripOverlapsXRange(IslandBox, Partition.X0, Partition.X1, 0.0))
				{
					continue;
				}
				if (int32* ExistingIndex = ItemIndexByIsland.Find(IslandBox.OwnerId))
				{
					FPartitionIslandBandItem& Item = Items[*ExistingIndex];
					Item.Top = FMath::Min(Item.Top, IslandBox.Rect.Top);
					Item.Bottom = FMath::Max(Item.Bottom, IslandBox.Rect.Bottom);
					Item.CenterYSum += IslandBox.CenterY();
					++Item.CenterYCount;
				}
				else
				{
					FPartitionIslandBandItem Item;
					Item.IslandId = IslandBox.OwnerId;
					Item.Top = IslandBox.Rect.Top;
					Item.Bottom = IslandBox.Rect.Bottom;
					Item.CenterYSum = IslandBox.CenterY();
					Item.CenterYCount = 1;
					ItemIndexByIsland.Add(Item.IslandId, Items.Num());
					Items.Add(Item);
				}
			}
			for (FPartitionIslandBandItem& Item : Items)
			{
				Item.CenterY = Item.CenterYCount > 0
					? Item.CenterYSum / static_cast<double>(Item.CenterYCount)
					: 0.0;
			}
			Items.Sort([](const FPartitionIslandBandItem& A, const FPartitionIslandBandItem& B)
			{
				if (!FMath::IsNearlyEqual(A.CenterY, B.CenterY, 0.5))
				{
					return A.CenterY < B.CenterY;
				}
				return A.IslandId < B.IslandId;
			});
			return Items;
		}

		static TArray<FPartitionBandConstraint> BuildA48PartitionBandConstraints(
			const TArray<FXStripCollisionBox>& IslandBoxes,
			const TArray<FDeterministicInterIslandBusBundle>& Bundles,
			const TArray<FBundleXPartition>& Partitions,
			FRunResult& Result,
			const bool bRecordDiagnostics)
		{
			TArray<FPartitionBandConstraint> BandConstraints;
			for (int32 PartitionIndex = 0; PartitionIndex < Partitions.Num(); ++PartitionIndex)
			{
				const FBundleXPartition& Partition = Partitions[PartitionIndex];
				const int32 ActiveCount = Partition.ActiveBundleIds.Num();
				if (ActiveCount <= 0)
				{
					continue;
				}
				TArray<double> ActiveBundleCenters;
				ActiveBundleCenters.Reserve(ActiveCount);
				for (const int32 BundleId : Partition.ActiveBundleIds)
				{
					ActiveBundleCenters.Add(InterIslandBundleCenterYInPartition(Bundles, BundleId, Partition.X0, Partition.X1));
				}

				const int32 BandCount = ActiveCount + 1;
				TArray<TArray<FPartitionIslandBandItem>> BandItems;
				BandItems.SetNum(BandCount);

				for (const FPartitionIslandBandItem& Item : BuildA48PartitionIslandItems(IslandBoxes, Partition))
				{
					int32 BandIndex = 0;
					while (BandIndex < ActiveCount && Item.CenterY > ActiveBundleCenters[BandIndex] + 0.5)
					{
						++BandIndex;
					}
					if (BandItems.IsValidIndex(BandIndex))
					{
						BandItems[BandIndex].Add(Item);
					}
				}

				for (int32 BandIndex = 0; BandIndex < BandCount; ++BandIndex)
				{
					if (!BandItems.IsValidIndex(BandIndex) || BandItems[BandIndex].Num() == 0)
					{
						continue;
					}
					BandItems[BandIndex].Sort([](const FPartitionIslandBandItem& A, const FPartitionIslandBandItem& B)
					{
						if (!FMath::IsNearlyEqual(A.Top, B.Top, 0.5))
						{
							return A.Top < B.Top;
						}
						if (!FMath::IsNearlyEqual(A.Bottom, B.Bottom, 0.5))
						{
							return A.Bottom < B.Bottom;
						}
						return A.IslandId < B.IslandId;
					});
					const int32 UpperBundleId = BandIndex == 0 ? INDEX_NONE : Partition.ActiveBundleIds[BandIndex - 1];
					const int32 LowerBundleId = BandIndex == ActiveCount ? INDEX_NONE : Partition.ActiveBundleIds[BandIndex];

					int32 GroupOrderInBand = 0;
					auto EmitBandGroup = [&BandConstraints, &Partition, PartitionIndex, BandIndex, &GroupOrderInBand, UpperBundleId, LowerBundleId, bRecordDiagnostics, &Result](
						const TArray<int32>& IslandIds,
						const double GroupTop,
						const double GroupBottom)
					{
						if (IslandIds.Num() == 0)
						{
							return;
						}
						FPartitionBandConstraint Constraint;
						Constraint.PartitionIndex = PartitionIndex;
						Constraint.BandIndex = BandIndex;
						Constraint.GroupOrderInBand = GroupOrderInBand++;
						Constraint.UpperBundleId = UpperBundleId;
						Constraint.LowerBundleId = LowerBundleId;
						Constraint.GroupTop = GroupTop;
						Constraint.GroupBottom = GroupBottom;
						Constraint.PartitionX0 = Partition.X0;
						Constraint.PartitionX1 = Partition.X1;
						Constraint.IslandIds = IslandIds;
						BandConstraints.Add(Constraint);
						if (bRecordDiagnostics)
						{
							++Result.DesignDiagnostics.IslandBandGroupCount;
						}
					};

					TArray<int32> CurrentIslandIds;
					double CurrentTop = TNumericLimits<double>::Max();
					double CurrentBottom = -TNumericLimits<double>::Max();
					for (const FPartitionIslandBandItem& Item : BandItems[BandIndex])
					{
						const bool bStartsNewGroup = CurrentIslandIds.Num() > 0 && Item.Top > CurrentBottom + 0.5;
						if (bStartsNewGroup)
						{
							EmitBandGroup(CurrentIslandIds, CurrentTop, CurrentBottom);
							CurrentIslandIds.Reset();
							CurrentTop = TNumericLimits<double>::Max();
							CurrentBottom = -TNumericLimits<double>::Max();
						}
						CurrentIslandIds.Add(Item.IslandId);
						CurrentTop = FMath::Min(CurrentTop, Item.Top);
						CurrentBottom = FMath::Max(CurrentBottom, Item.Bottom);
					}
					EmitBandGroup(CurrentIslandIds, CurrentTop, CurrentBottom);
				}
			}
			return BandConstraints;
		}

		static FInterIslandBundleGeometryPlan BuildA48InterIslandBundleGeometryPlan(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const TArray<FInternalBusGroup>& InternalBusGroups,
			const TArray<FDeterministicIslandConnection>& Connections,
			const TArray<FDeterministicInterIslandBus>& Buses,
			const TArray<FDeterministicInterIslandBusBundle>& Bundles,
			const FOptions& Options,
			FRunResult& Result,
			const bool bRecordDiagnostics)
		{
			FInterIslandBundleGeometryPlan Plan;
			Plan.IslandBoxes = BuildAllIslandBusCollisionBoxes(
				Nodes,
				Edges,
				Islands,
				InternalBusGroups,
				Connections,
				Options);
			if (bRecordDiagnostics)
			{
				Result.DesignDiagnostics.IslandBusCollisionBoxCount = Plan.IslandBoxes.Num();
			}
			Plan.Partitions = BuildA48InterIslandBundleXPartitions(Bundles);
			Plan.BandConstraints = BuildA48PartitionBandConstraints(Plan.IslandBoxes, Bundles, Plan.Partitions, Result, bRecordDiagnostics);
			return Plan;
		}

		static void ApplyA49InterIslandPartitionSpread(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			TArray<FIsland>& Islands,
			const TArray<FInternalBusGroup>& InternalBusGroups,
			const TArray<FDeterministicIslandConnection>& Connections,
			TArray<FDeterministicInterIslandBus>& Buses,
			TArray<FDeterministicInterIslandBusBundle>& Bundles,
			const FOptions& Options,
			FInterIslandBundleGeometryPlan& Plan,
			const double LanePitchY,
			const double AvoidanceY)
		{

			TArray<FXStripCollisionBox>& IslandBoxes = Plan.IslandBoxes;
			const TArray<FBundleXPartition>& Partitions = Plan.Partitions;
			const TArray<FPartitionBandConstraint>& BandConstraints = Plan.BandConstraints;
			auto RebuildBundleCollisionBoxes = [&Buses, AvoidanceY](FDeterministicInterIslandBusBundle& Bundle)
			{
				RebuildInterIslandBundleCollisionBoxes(Buses, AvoidanceY, Bundle);
			};
			auto ComputeBundleBoundsInPartition = [&Bundles](const int32 BundleId, const double X0, const double X1, double& OutTop, double& OutBottom) -> bool
			{
				return ComputeInterIslandBundleBoundsInPartition(Bundles, BundleId, X0, X1, OutTop, OutBottom);
			};
			auto BundleCenterYInPartition = [&Bundles](const int32 BundleId, const double X0, const double X1)
			{
				return InterIslandBundleCenterYInPartition(Bundles, BundleId, X0, X1);
			};
			TArray<TArray<int32>> ConstraintIndicesByPartition;
			ConstraintIndicesByPartition.SetNum(Partitions.Num());
			for (int32 ConstraintIndex = 0; ConstraintIndex < BandConstraints.Num(); ++ConstraintIndex)
			{
				const int32 PartitionIndex = BandConstraints[ConstraintIndex].PartitionIndex;
				if (ConstraintIndicesByPartition.IsValidIndex(PartitionIndex))
				{
					ConstraintIndicesByPartition[PartitionIndex].Add(ConstraintIndex);
				}
			}
			TArray<double> IslandA49BaseY;
			TArray<double> IslandA49PendingOffsetY;
			IslandA49BaseY.SetNum(Islands.Num());
			IslandA49PendingOffsetY.Init(0.0, Islands.Num());
			for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
			{
				IslandA49BaseY[IslandIndex] = Islands[IslandIndex].Y;
			}

			TArray<double> BundleA49BaseCenterY;
			BundleA49BaseCenterY.SetNum(Bundles.Num());
			for (int32 BundleIndex = 0; BundleIndex < Bundles.Num(); ++BundleIndex)
			{
				BundleA49BaseCenterY[BundleIndex] = Bundles[BundleIndex].FinalCenterY;
			}

			auto RebuildCurrentIslandBoxes = [&IslandBoxes, &Nodes, &Edges, &Islands, &InternalBusGroups, &Connections, &Options]()
			{
				IslandBoxes = BuildAllIslandBusCollisionBoxes(Nodes, Edges, Islands, InternalBusGroups, Connections, Options);
			};
			auto RebuildBundleGeometry = [&Bundles, &Buses, LanePitchY, &RebuildBundleCollisionBoxes](const int32 BundleId)
			{
				if (!Bundles.IsValidIndex(BundleId))
				{
					return;
				}
				FDeterministicInterIslandBusBundle& Bundle = Bundles[BundleId];
				const double SlotCenter = static_cast<double>(Bundle.MaxSlotIndex) * 0.5;
				for (const int32 BusId : Bundle.BusIds)
				{
					if (Buses.IsValidIndex(BusId))
					{
						Buses[BusId].FinalY = SnapGraphCoordinate(Bundle.FinalCenterY + (Buses[BusId].SlotIndex - SlotCenter) * LanePitchY);
					}
				}
				RebuildBundleCollisionBoxes(Bundle);
			};
			auto MoveBundleByDeltaY = [&Bundles, &RebuildBundleGeometry](const int32 BundleId, const double DeltaY)
			{
				if (!Bundles.IsValidIndex(BundleId) || FMath::Abs(DeltaY) <= 0.5)
				{
					return;
				}
				Bundles[BundleId].FinalCenterY = SnapGraphCoordinate(Bundles[BundleId].FinalCenterY + DeltaY);
				RebuildBundleGeometry(BundleId);
			};
			auto ApplyIslandA49OffsetY = [&Islands, &IslandA49BaseY, &IslandA49PendingOffsetY](const int32 IslandId, const double NewOffsetY) -> bool
			{
				if (!Islands.IsValidIndex(IslandId) || !IslandA49BaseY.IsValidIndex(IslandId) || !IslandA49PendingOffsetY.IsValidIndex(IslandId))
				{
					return false;
				}
				const double SnappedY = SnapGraphCoordinate(IslandA49BaseY[IslandId] + NewOffsetY);
				const double SnappedOffsetY = SnappedY - IslandA49BaseY[IslandId];
				if (FMath::Abs(SnappedOffsetY - IslandA49PendingOffsetY[IslandId]) <= 0.5)
				{
					return false;
				}
				IslandA49PendingOffsetY[IslandId] = SnappedOffsetY;
				Islands[IslandId].Y = SnappedY;
				return true;
			};
			auto MergeIslandA49OffsetY = [&IslandA49PendingOffsetY, &ApplyIslandA49OffsetY](const int32 IslandId, const double CandidateOffsetY, const int32 Direction) -> bool
			{
				if (!IslandA49PendingOffsetY.IsValidIndex(IslandId) || Direction == 0)
				{
					return false;
				}
				const double MergedOffsetY = MergeA49IslandDirectionalOffset(
					IslandA49PendingOffsetY[IslandId],
					CandidateOffsetY,
					Direction);
				return ApplyIslandA49OffsetY(IslandId, MergedOffsetY);
			};
			auto MoveIslandByDeltaY = [&IslandA49PendingOffsetY, &MergeIslandA49OffsetY](const int32 IslandId, const double DeltaY) -> bool
			{
				if (!IslandA49PendingOffsetY.IsValidIndex(IslandId) || FMath::Abs(DeltaY) <= 0.5)
				{
					return false;
				}
				const int32 Direction = DeltaY < 0.0 ? -1 : 1;
				const double CandidateOffsetY = IslandA49PendingOffsetY[IslandId] + DeltaY;
				return MergeIslandA49OffsetY(IslandId, CandidateOffsetY, Direction);
			};
			auto GetBundleA49OffsetY = [&Bundles, &BundleA49BaseCenterY](const int32 BundleId) -> double
			{
				if (!Bundles.IsValidIndex(BundleId) || !BundleA49BaseCenterY.IsValidIndex(BundleId))
				{
					return 0.0;
				}
				return Bundles[BundleId].FinalCenterY - BundleA49BaseCenterY[BundleId];
			};
			auto MergeBundleA49Offset = [](const double CurrentOffsetY, const double CandidateOffsetY, const int32 Direction) -> double
			{
				return MergeA49IslandDirectionalOffset(CurrentOffsetY, CandidateOffsetY, Direction);
			};
			auto ComputeIslandBoundsInPartition = [&IslandBoxes](const int32 IslandId, const double X0, const double X1, double& OutTop, double& OutBottom) -> bool
			{
				OutTop = TNumericLimits<double>::Max();
				OutBottom = -TNumericLimits<double>::Max();
				for (const FXStripCollisionBox& IslandBox : IslandBoxes)
				{
					if (IslandBox.OwnerKind != EXStripCollisionOwnerKind::Island
						|| IslandBox.OwnerId != IslandId
						|| !XStripOverlapsXRange(IslandBox, X0, X1, 0.0))
					{
						continue;
					}
					OutTop = FMath::Min(OutTop, IslandBox.Rect.Top);
					OutBottom = FMath::Max(OutBottom, IslandBox.Rect.Bottom);
				}
				return OutTop != TNumericLimits<double>::Max() && OutBottom > OutTop + 0.5;
			};
			auto ComputeIslandBoundsAcrossAllStrips = [&IslandBoxes](const int32 IslandId, double& OutTop, double& OutBottom) -> bool
			{
				OutTop = TNumericLimits<double>::Max();
				OutBottom = -TNumericLimits<double>::Max();
				for (const FXStripCollisionBox& IslandBox : IslandBoxes)
				{
					if (IslandBox.OwnerKind != EXStripCollisionOwnerKind::Island || IslandBox.OwnerId != IslandId)
					{
						continue;
					}
					OutTop = FMath::Min(OutTop, IslandBox.Rect.Top);
					OutBottom = FMath::Max(OutBottom, IslandBox.Rect.Bottom);
				}
				if (OutTop == TNumericLimits<double>::Max() || OutBottom <= OutTop + 0.5)
				{
					return false;
				}
				return true;
			};
			auto ComputeIslandCenterYAcrossAllStrips = [&IslandBoxes](const int32 IslandId, double& OutCenterY) -> bool
			{
				OutCenterY = 0.0;
				double CenterYSum = 0.0;
				int32 CenterYCount = 0;
				for (const FXStripCollisionBox& IslandBox : IslandBoxes)
				{
					if (IslandBox.OwnerKind != EXStripCollisionOwnerKind::Island || IslandBox.OwnerId != IslandId)
					{
						continue;
					}
					CenterYSum += IslandBox.CenterY();
					++CenterYCount;
				}
				if (CenterYCount <= 0)
				{
					return false;
				}
				OutCenterY = CenterYSum / static_cast<double>(CenterYCount);
				return true;
			};
			auto ComputeBandGroupBounds = [&ComputeIslandBoundsInPartition](const FPartitionBandConstraint& Constraint, double& OutTop, double& OutBottom) -> bool
			{
				OutTop = TNumericLimits<double>::Max();
				OutBottom = -TNumericLimits<double>::Max();
				for (const int32 IslandId : Constraint.IslandIds)
				{
					double IslandTop = 0.0;
					double IslandBottom = 0.0;
					if (!ComputeIslandBoundsInPartition(IslandId, Constraint.PartitionX0, Constraint.PartitionX1, IslandTop, IslandBottom))
					{
						continue;
					}
					OutTop = FMath::Min(OutTop, IslandTop);
					OutBottom = FMath::Max(OutBottom, IslandBottom);
				}
				return OutTop != TNumericLimits<double>::Max() && OutBottom > OutTop + 0.5;
			};
			struct FConfirmedYComponent
			{
				bool bBundle = false;
				int32 Id = INDEX_NONE;
			};
			auto AppendIslandRectsInPartition = [&IslandBoxes](const int32 IslandId, const FBundleXPartition& Partition, TArray<FRect>& OutRects)
			{
				for (const FXStripCollisionBox& IslandBox : IslandBoxes)
				{
					if (IslandBox.OwnerKind == EXStripCollisionOwnerKind::Island
						&& IslandBox.OwnerId == IslandId
						&& XStripOverlapsXRange(IslandBox, Partition.X0, Partition.X1, 0.0))
					{
						OutRects.Add(IslandBox.Rect);
					}
				}
			};
			auto AppendBundleRectsInPartition = [&Bundles](const int32 BundleId, const FBundleXPartition& Partition, TArray<FRect>& OutRects)
			{
				if (!Bundles.IsValidIndex(BundleId))
				{
					return;
				}
				for (const FXStripCollisionBox& BundleBox : Bundles[BundleId].CollisionBoxes)
				{
					if (XStripOverlapsXRange(BundleBox, Partition.X0, Partition.X1, 0.0))
					{
						OutRects.Add(BundleBox.Rect);
					}
				}
			};
			auto AppendComponentRectsInPartition = [&AppendIslandRectsInPartition, &AppendBundleRectsInPartition](
				const FConfirmedYComponent& Component,
				const FBundleXPartition& Partition,
				TArray<FRect>& OutRects)
			{
				if (Component.bBundle)
				{
					AppendBundleRectsInPartition(Component.Id, Partition, OutRects);
				}
				else
				{
					AppendIslandRectsInPartition(Component.Id, Partition, OutRects);
				}
			};
			auto ComputeEntityDeltaToAvoidConfirmed = [
				&AppendIslandRectsInPartition,
				&AppendBundleRectsInPartition,
				&AppendComponentRectsInPartition](
					const bool bBundle,
					const int32 EntityId,
					const FBundleXPartition& Partition,
					const TArray<FConfirmedYComponent>& ConfirmedComponents,
					const int32 Direction)
			{
				TArray<FRect> EntityRects;
				if (bBundle)
				{
					AppendBundleRectsInPartition(EntityId, Partition, EntityRects);
				}
				else
				{
					AppendIslandRectsInPartition(EntityId, Partition, EntityRects);
				}
				if (EntityRects.Num() == 0)
				{
					return 0.0;
				}
				double DeltaY = 0.0;
				for (const FRect& EntityRect : EntityRects)
				{
					for (const FConfirmedYComponent& Confirmed : ConfirmedComponents)
					{
						if (Confirmed.bBundle == bBundle && Confirmed.Id == EntityId)
						{
							continue;
						}
						TArray<FRect> ConfirmedRects;
						AppendComponentRectsInPartition(Confirmed, Partition, ConfirmedRects);
						for (const FRect& ConfirmedRect : ConfirmedRects)
						{
							if (!XStripRangeOverlaps(EntityRect.Left, EntityRect.Right, ConfirmedRect.Left, ConfirmedRect.Right, 0.0))
							{
								continue;
							}
							if (Direction < 0)
							{
								if (EntityRect.Bottom + DeltaY > ConfirmedRect.Top - 0.5)
								{
									DeltaY = FMath::Min(DeltaY, ConfirmedRect.Top - EntityRect.Bottom);
								}
							}
							else if (EntityRect.Top + DeltaY < ConfirmedRect.Bottom + 0.5)
							{
								DeltaY = FMath::Max(DeltaY, ConfirmedRect.Bottom - EntityRect.Top);
							}
						}
					}
				}
				return SnapGraphCoordinate(DeltaY);
			};
			auto IslandIntersectsCurrentBundleRange = [&Bundles, &IslandBoxes](
				const int32 IslandId,
				const int32 BundleId,
				const FBundleXPartition& Partition,
				const int32 Direction)
			{
				if (!Bundles.IsValidIndex(BundleId))
				{
					return false;
				}
				for (const FXStripCollisionBox& IslandBox : IslandBoxes)
				{
					if (IslandBox.OwnerKind != EXStripCollisionOwnerKind::Island
						|| IslandBox.OwnerId != IslandId
						|| !XStripOverlapsXRange(IslandBox, Partition.X0, Partition.X1, 0.0))
					{
						continue;
					}
					for (const FXStripCollisionBox& BundleBox : Bundles[BundleId].CollisionBoxes)
					{
						if (!XStripOverlapsXRange(BundleBox, Partition.X0, Partition.X1, 0.0)
							|| !XStripRangeOverlaps(IslandBox.Rect.Left, IslandBox.Rect.Right, BundleBox.Rect.Left, BundleBox.Rect.Right, 0.0))
						{
							continue;
						}
						if (Direction < 0)
						{
							if (IslandBox.Rect.Bottom > BundleBox.Rect.Top - 0.5)
							{
								return true;
							}
						}
						else if (IslandBox.Rect.Top < BundleBox.Rect.Bottom + 0.5)
						{
							return true;
						}
					}
				}
				return false;
			};
			auto IslandsIntersectInPartition = [&IslandBoxes](const int32 A, const int32 B, const FBundleXPartition& Partition)
			{
				if (A == B)
				{
					return false;
				}
				for (const FXStripCollisionBox& BoxA : IslandBoxes)
				{
					if (BoxA.OwnerKind != EXStripCollisionOwnerKind::Island
						|| BoxA.OwnerId != A
						|| !XStripOverlapsXRange(BoxA, Partition.X0, Partition.X1, 0.0))
					{
						continue;
					}
					for (const FXStripCollisionBox& BoxB : IslandBoxes)
					{
						if (BoxB.OwnerKind != EXStripCollisionOwnerKind::Island
							|| BoxB.OwnerId != B
							|| !XStripOverlapsXRange(BoxB, Partition.X0, Partition.X1, 0.0)
							|| !XStripRangeOverlaps(BoxA.Rect.Left, BoxA.Rect.Right, BoxB.Rect.Left, BoxB.Rect.Right, 0.0))
						{
							continue;
						}
						if (BoxA.Rect.Intersects(BoxB.Rect, 0.0))
						{
							return true;
						}
					}
				}
				return false;
			};
			auto ShiftDirectionalEntities = [
				&BandConstraints,
				&ConstraintIndicesByPartition,
				&MoveBundleByDeltaY,
				&GetBundleA49OffsetY,
				&MergeBundleA49Offset,
				&MergeIslandA49OffsetY,
				&RebuildCurrentIslandBoxes,
				&BundleCenterYInPartition](
					const int32 PartitionIndex,
					const FBundleXPartition& Partition,
					const int32 CurrentBandIndex,
					const double AnchorCenterY,
					const int32 Direction,
					const double DeltaY)
			{
				if (FMath::Abs(DeltaY) <= 0.5)
				{
					return;
				}
				TArray<int32> BundleIdsToMove;
				for (const int32 BundleId : Partition.ActiveBundleIds)
				{
					const double Center = BundleCenterYInPartition(BundleId, Partition.X0, Partition.X1);
					const bool bInDirection = Direction < 0
						? Center <= AnchorCenterY + 0.5
						: Center >= AnchorCenterY - 0.5;
					if (bInDirection)
					{
						BundleIdsToMove.Add(BundleId);
					}
				}
				for (const int32 BundleId : BundleIdsToMove)
				{
					MoveBundleByDeltaY(BundleId, DeltaY);
				}
				bool bHaveMergedBundleOffset = false;
				double MergedBundleOffsetY = 0.0;
				TMap<int32, double> MovedBundleOffsetById;
				for (const int32 BundleId : BundleIdsToMove)
				{
					const double BundleOffsetY = GetBundleA49OffsetY(BundleId);
					MovedBundleOffsetById.Add(BundleId, BundleOffsetY);
					if (!bHaveMergedBundleOffset)
					{
						MergedBundleOffsetY = BundleOffsetY;
						bHaveMergedBundleOffset = true;
					}
					else
					{
						MergedBundleOffsetY = MergeBundleA49Offset(MergedBundleOffsetY, BundleOffsetY, Direction);
					}
				}
				bool bMovedIsland = false;
				if (bHaveMergedBundleOffset)
				{
					TMap<int32, double> CandidateOffsetByIsland;
					auto AddIslandCandidate = [&CandidateOffsetByIsland, &MergeBundleA49Offset, Direction](const int32 IslandId, const double CandidateOffsetY)
					{
						if (double* ExistingOffset = CandidateOffsetByIsland.Find(IslandId))
						{
							*ExistingOffset = MergeBundleA49Offset(*ExistingOffset, CandidateOffsetY, Direction);
							return;
						}
						CandidateOffsetByIsland.Add(IslandId, CandidateOffsetY);
					};
					if (ConstraintIndicesByPartition.IsValidIndex(PartitionIndex))
					{
						for (const int32 ConstraintIndex : ConstraintIndicesByPartition[PartitionIndex])
						{
							if (!BandConstraints.IsValidIndex(ConstraintIndex))
							{
								continue;
							}
							const FPartitionBandConstraint& Constraint = BandConstraints[ConstraintIndex];
							const bool bBeyondOuterBundle = Direction < 0
								? Constraint.BandIndex < CurrentBandIndex
								: Constraint.BandIndex > CurrentBandIndex;
							if (!bBeyondOuterBundle)
							{
								continue;
							}
							const int32 AdjacentMovedBundleId = Direction < 0
								? Constraint.LowerBundleId
								: Constraint.UpperBundleId;
							const double* AdjacentBundleOffset = MovedBundleOffsetById.Find(AdjacentMovedBundleId);
							const double CandidateOffsetY = AdjacentBundleOffset ? *AdjacentBundleOffset : MergedBundleOffsetY;
							for (const int32 IslandId : Constraint.IslandIds)
							{
								AddIslandCandidate(IslandId, CandidateOffsetY);
							}
						}
					}
					for (const TPair<int32, double>& IslandCandidate : CandidateOffsetByIsland)
					{
						bMovedIsland |= MergeIslandA49OffsetY(IslandCandidate.Key, IslandCandidate.Value, Direction);
					}
				}
				if (bMovedIsland)
				{
					RebuildCurrentIslandBoxes();
				}
			};

			auto ProcessDirectionalBand = [
				&Partitions,
				&ConstraintIndicesByPartition,
				&BandConstraints,
				&Islands,
				&ComputeBundleBoundsInPartition,
				&BundleCenterYInPartition,
				&ComputeBandGroupBounds,
				&ComputeEntityDeltaToAvoidConfirmed,
				&IslandIntersectsCurrentBundleRange,
				&IslandsIntersectInPartition,
				&MoveIslandByDeltaY,
				&RebuildCurrentIslandBoxes,
				&ShiftDirectionalEntities](
					const int32 PartitionIndex,
					const int32 AnchorBundleId,
					const int32 Direction)
			{
				if (!Partitions.IsValidIndex(PartitionIndex) || AnchorBundleId == INDEX_NONE || Direction == 0)
				{
					return;
				}
				const FBundleXPartition& Partition = Partitions[PartitionIndex];
				int32 CurrentBundleId = AnchorBundleId;
				TArray<FConfirmedYComponent> ConfirmedComponents;
				FConfirmedYComponent AnchorComponent;
				AnchorComponent.bBundle = true;
				AnchorComponent.Id = AnchorBundleId;
				ConfirmedComponents.Add(AnchorComponent);
				for (int32 Safety = 0; Safety < Partition.ActiveBundleIds.Num(); ++Safety)
				{
					const int32 CurrentBundlePosition = Partition.ActiveBundleIds.IndexOfByKey(CurrentBundleId);
					if (CurrentBundlePosition == INDEX_NONE)
					{
						break;
					}
					double CurrentTop = 0.0;
					double CurrentBottom = 0.0;
					if (!ComputeBundleBoundsInPartition(CurrentBundleId, Partition.X0, Partition.X1, CurrentTop, CurrentBottom))
					{
						break;
					}
					const int32 BandIndex = Direction < 0 ? CurrentBundlePosition : CurrentBundlePosition + 1;
					const int32 UpperBundleId = BandIndex == 0 ? INDEX_NONE : Partition.ActiveBundleIds[BandIndex - 1];
					const int32 LowerBundleId = BandIndex == Partition.ActiveBundleIds.Num() ? INDEX_NONE : Partition.ActiveBundleIds[BandIndex];
					if ((Direction < 0 && LowerBundleId != CurrentBundleId)
						|| (Direction > 0 && UpperBundleId != CurrentBundleId))
					{
						break;
					}
					TArray<int32> GroupConstraintIndices;
					if (ConstraintIndicesByPartition.IsValidIndex(PartitionIndex))
					{
						for (const int32 ConstraintIndex : ConstraintIndicesByPartition[PartitionIndex])
						{
							if (!BandConstraints.IsValidIndex(ConstraintIndex))
							{
								continue;
							}
							const FPartitionBandConstraint& Constraint = BandConstraints[ConstraintIndex];
							if (Constraint.UpperBundleId == UpperBundleId && Constraint.LowerBundleId == LowerBundleId)
							{
								GroupConstraintIndices.Add(ConstraintIndex);
							}
						}
					}
					GroupConstraintIndices.Sort([&BandConstraints, Direction](const int32 A, const int32 B)
					{
						const FPartitionBandConstraint& Left = BandConstraints[A];
						const FPartitionBandConstraint& Right = BandConstraints[B];
						if (Left.GroupOrderInBand != Right.GroupOrderInBand)
						{
							return Direction < 0
								? Left.GroupOrderInBand > Right.GroupOrderInBand
								: Left.GroupOrderInBand < Right.GroupOrderInBand;
						}
						return A < B;
					});

					for (const int32 ConstraintIndex : GroupConstraintIndices)
					{
						if (!BandConstraints.IsValidIndex(ConstraintIndex))
						{
							continue;
						}
						const FPartitionBandConstraint& Constraint = BandConstraints[ConstraintIndex];
						TArray<int32> OrderedIslandIds;
						OrderedIslandIds.Reserve(Constraint.IslandIds.Num());
						if (Direction < 0)
						{
							for (int32 Index = Constraint.IslandIds.Num() - 1; Index >= 0; --Index)
							{
								OrderedIslandIds.Add(Constraint.IslandIds[Index]);
							}
						}
						else
						{
							OrderedIslandIds = Constraint.IslandIds;
						}
						TMap<int32, int32> OutwardRankByIsland;
						for (int32 Rank = 0; Rank < OrderedIslandIds.Num(); ++Rank)
						{
							OutwardRankByIsland.Add(OrderedIslandIds[Rank], Rank);
						}
						auto IsFartherFromCurrentBundle = [&OutwardRankByIsland](const int32 A, const int32 B)
						{
							const int32* RankA = OutwardRankByIsland.Find(A);
							const int32* RankB = OutwardRankByIsland.Find(B);
							if (RankA && RankB && *RankA != *RankB)
							{
								return *RankA > *RankB;
							}
							return A > B;
						};
						TSet<int32> ConfirmedIslandIds;
						TSet<int32> PendingIslandIds;
						for (const int32 IslandId : OrderedIslandIds)
						{
							if (Islands.IsValidIndex(IslandId)
								&& IslandIntersectsCurrentBundleRange(IslandId, CurrentBundleId, Partition, Direction))
							{
								PendingIslandIds.Add(IslandId);
							}
						}
						auto ConfirmIsland = [&ConfirmedIslandIds, &ConfirmedComponents, &Islands](const int32 IslandId)
						{
							if (!Islands.IsValidIndex(IslandId) || ConfirmedIslandIds.Contains(IslandId))
							{
								return;
							}
							ConfirmedIslandIds.Add(IslandId);
							FConfirmedYComponent Component;
							Component.bBundle = false;
							Component.Id = IslandId;
							ConfirmedComponents.Add(Component);
						};
						int32 ValidIslandCount = 0;
						for (const int32 IslandId : OrderedIslandIds)
						{
							if (Islands.IsValidIndex(IslandId))
							{
								++ValidIslandCount;
							}
						}
						for (int32 GroupSafety = 0; GroupSafety < ValidIslandCount * 2 + 1 && ConfirmedIslandIds.Num() < ValidIslandCount; ++GroupSafety)
						{
							if (PendingIslandIds.Num() == 0)
							{
								bool bSeededOrConfirmed = false;
								for (const int32 IslandId : OrderedIslandIds)
								{
									if (!Islands.IsValidIndex(IslandId) || ConfirmedIslandIds.Contains(IslandId))
									{
										continue;
									}
									const double DeltaY = ComputeEntityDeltaToAvoidConfirmed(false, IslandId, Partition, ConfirmedComponents, Direction);
									if (FMath::Abs(DeltaY) > 0.5)
									{
										PendingIslandIds.Add(IslandId);
									}
									else
									{
										ConfirmIsland(IslandId);
									}
									bSeededOrConfirmed = true;
									break;
								}
								if (!bSeededOrConfirmed)
								{
									break;
								}
								continue;
							}

							bool bMovedIsland = false;
							for (const int32 IslandId : OrderedIslandIds)
							{
								if (!PendingIslandIds.Contains(IslandId) || ConfirmedIslandIds.Contains(IslandId))
								{
									continue;
								}
								const double DeltaY = ComputeEntityDeltaToAvoidConfirmed(false, IslandId, Partition, ConfirmedComponents, Direction);
								bMovedIsland |= MoveIslandByDeltaY(IslandId, DeltaY);
							}
							if (bMovedIsland)
							{
								RebuildCurrentIslandBoxes();
							}

							TSet<int32> NextPendingIslandIds;
							for (int32 AIndex = 0; AIndex < OrderedIslandIds.Num(); ++AIndex)
							{
								const int32 IslandA = OrderedIslandIds[AIndex];
								if (!Islands.IsValidIndex(IslandA) || ConfirmedIslandIds.Contains(IslandA))
								{
									continue;
								}
								for (int32 BIndex = AIndex + 1; BIndex < OrderedIslandIds.Num(); ++BIndex)
								{
									const int32 IslandB = OrderedIslandIds[BIndex];
									if (!Islands.IsValidIndex(IslandB) || ConfirmedIslandIds.Contains(IslandB))
									{
										continue;
									}
									if (!PendingIslandIds.Contains(IslandA) && !PendingIslandIds.Contains(IslandB))
									{
										continue;
									}
									if (!IslandsIntersectInPartition(IslandA, IslandB, Partition))
									{
										continue;
									}
									NextPendingIslandIds.Add(IsFartherFromCurrentBundle(IslandA, IslandB) ? IslandA : IslandB);
								}
							}
							for (const int32 IslandId : OrderedIslandIds)
							{
								if (PendingIslandIds.Contains(IslandId) && !NextPendingIslandIds.Contains(IslandId))
								{
									ConfirmIsland(IslandId);
								}
							}
							PendingIslandIds = MoveTemp(NextPendingIslandIds);
						}
						for (const int32 IslandId : OrderedIslandIds)
						{
							ConfirmIsland(IslandId);
						}
					}

					const int32 OuterBundleId = Direction < 0 ? UpperBundleId : LowerBundleId;
					if (OuterBundleId == INDEX_NONE)
					{
						break;
					}
					double OuterTop = 0.0;
					double OuterBottom = 0.0;
					if (!ComputeBundleBoundsInPartition(OuterBundleId, Partition.X0, Partition.X1, OuterTop, OuterBottom))
					{
						break;
					}
					const double OuterCenter = BundleCenterYInPartition(OuterBundleId, Partition.X0, Partition.X1);
					const double BundleDeltaY = ComputeEntityDeltaToAvoidConfirmed(true, OuterBundleId, Partition, ConfirmedComponents, Direction);
					if (FMath::Abs(BundleDeltaY) > 0.5)
					{
						ShiftDirectionalEntities(PartitionIndex, Partition, BandIndex, OuterCenter, Direction, BundleDeltaY);
					}
					FConfirmedYComponent OuterComponent;
					OuterComponent.bBundle = true;
					OuterComponent.Id = OuterBundleId;
					ConfirmedComponents.Add(OuterComponent);
					CurrentBundleId = OuterBundleId;
				}
			};

			int32 StartPartitionIndex = INDEX_NONE;
			int32 MaxActiveBundleCount = 0;
			int32 MaxBandGroupCount = 0;
			for (int32 PartitionIndex = 0; PartitionIndex < Partitions.Num(); ++PartitionIndex)
			{
				if (!Partitions.IsValidIndex(PartitionIndex) || Partitions[PartitionIndex].ActiveBundleIds.Num() == 0)
				{
					continue;
				}
				const int32 ActiveBundleCount = Partitions[PartitionIndex].ActiveBundleIds.Num();
				const int32 BandGroupCount = ConstraintIndicesByPartition.IsValidIndex(PartitionIndex)
					? ConstraintIndicesByPartition[PartitionIndex].Num()
					: 0;
				if (StartPartitionIndex == INDEX_NONE
					|| ActiveBundleCount > MaxActiveBundleCount
					|| (ActiveBundleCount == MaxActiveBundleCount && BandGroupCount > MaxBandGroupCount))
				{
					StartPartitionIndex = PartitionIndex;
					MaxActiveBundleCount = ActiveBundleCount;
					MaxBandGroupCount = BandGroupCount;
				}
			}
			TArray<int32> PartitionOrder;
			if (StartPartitionIndex != INDEX_NONE)
			{
				for (int32 Distance = 0; Distance < Partitions.Num(); ++Distance)
				{
					const int32 Left = StartPartitionIndex - Distance;
					const int32 Right = StartPartitionIndex + Distance;
					if (Partitions.IsValidIndex(Left))
					{
						PartitionOrder.Add(Left);
					}
					if (Distance != 0 && Partitions.IsValidIndex(Right))
					{
						PartitionOrder.Add(Right);
					}
				}
			}
			for (const int32 PartitionIndex : PartitionOrder)
			{
				if (!Partitions.IsValidIndex(PartitionIndex) || Partitions[PartitionIndex].ActiveBundleIds.Num() == 0)
				{
					continue;
				}
				const FBundleXPartition& Partition = Partitions[PartitionIndex];
				const TArray<int32>& SortedActiveBundles = Partition.ActiveBundleIds;
				if (SortedActiveBundles.Num() == 0)
				{
					continue;
				}
				double AverageGroupCenter = 0.0;
				int32 AverageGroupCount = 0;
				if (ConstraintIndicesByPartition.IsValidIndex(PartitionIndex))
				{
					for (const int32 ConstraintIndex : ConstraintIndicesByPartition[PartitionIndex])
					{
						if (!BandConstraints.IsValidIndex(ConstraintIndex))
						{
							continue;
						}
						double GroupTop = 0.0;
						double GroupBottom = 0.0;
						if (ComputeBandGroupBounds(BandConstraints[ConstraintIndex], GroupTop, GroupBottom))
						{
							AverageGroupCenter += (GroupTop + GroupBottom) * 0.5;
							++AverageGroupCount;
						}
					}
				}
				if (AverageGroupCount > 0)
				{
					AverageGroupCenter /= static_cast<double>(AverageGroupCount);
				}
				int32 AnchorBundleId = SortedActiveBundles[SortedActiveBundles.Num() / 2];
				if (SortedActiveBundles.Num() % 2 == 0)
				{
					const int32 UpperMidBundleId = SortedActiveBundles[SortedActiveBundles.Num() / 2 - 1];
					const int32 LowerMidBundleId = SortedActiveBundles[SortedActiveBundles.Num() / 2];
					if (AverageGroupCount > 0)
					{
						const double UpperDistance = FMath::Abs(BundleCenterYInPartition(UpperMidBundleId, Partition.X0, Partition.X1) - AverageGroupCenter);
						const double LowerDistance = FMath::Abs(BundleCenterYInPartition(LowerMidBundleId, Partition.X0, Partition.X1) - AverageGroupCenter);
						AnchorBundleId = LowerDistance < UpperDistance - 0.5 ? LowerMidBundleId : UpperMidBundleId;
					}
					else
					{
						AnchorBundleId = UpperMidBundleId;
					}
				}
				ProcessDirectionalBand(PartitionIndex, AnchorBundleId, -1);
				ProcessDirectionalBand(PartitionIndex, AnchorBundleId, 1);
			}

		}


		static void FinalizeInterIslandBundleGeometryPlan(
			TArray<FDeterministicInterIslandBus>& Buses,
			TArray<FDeterministicInterIslandBusBundle>& Bundles,
			const FInterIslandBundleGeometryPlan& Plan,
			const double LanePitchY,
			const double AvoidanceY,
			FRunResult& Result,
			const bool bRecordDiagnostics)
		{
			for (FDeterministicInterIslandBusBundle& Bundle : Bundles)
			{
				const double SlotCenter = static_cast<double>(Bundle.MaxSlotIndex) * 0.5;
				for (const int32 BusId : Bundle.BusIds)
				{
					if (Buses.IsValidIndex(BusId))
					{
						Buses[BusId].FinalY = SnapGraphCoordinate(Bundle.FinalCenterY + (Buses[BusId].SlotIndex - SlotCenter) * LanePitchY);
					}
				}
				RebuildInterIslandBundleCollisionBoxes(Buses, AvoidanceY, Bundle);
			}
			if (bRecordDiagnostics)
			{
				Result.DesignDiagnostics.InterIslandBusBundleCollisionBoxCount = 0;
				for (const FDeterministicInterIslandBusBundle& Bundle : Bundles)
				{
					Result.DesignDiagnostics.InterIslandBusBundleCollisionBoxCount += Bundle.CollisionBoxes.Num();
				}
				Result.InterIslandBusDiagnostics.Reset();
				auto AddRectFields = [](const TSharedPtr<FJsonObject>& Obj, const TCHAR* Prefix, const FRect& Rect)
				{
					Obj->SetNumberField(FString::Printf(TEXT("%s_left"), Prefix), Rect.Left);
					Obj->SetNumberField(FString::Printf(TEXT("%s_top"), Prefix), Rect.Top);
					Obj->SetNumberField(FString::Printf(TEXT("%s_right"), Prefix), Rect.Right);
					Obj->SetNumberField(FString::Printf(TEXT("%s_bottom"), Prefix), Rect.Bottom);
				};
				auto AddDiagnosticObject = [&Result](const TSharedPtr<FJsonObject>& Obj)
				{
					Result.InterIslandBusDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
				};
				for (const FDeterministicInterIslandBus& Bus : Buses)
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("kind"), TEXT("bus"));
					Obj->SetNumberField(TEXT("bus_id"), Bus.Id);
					Obj->SetStringField(TEXT("bus_key"), Bus.Key);
					Obj->SetNumberField(TEXT("bundle_id"), Bus.BundleId);
					Obj->SetNumberField(TEXT("slot_index"), Bus.SlotIndex);
					Obj->SetNumberField(TEXT("signed_slot_index"), Bus.SignedSlotIndex);
					Obj->SetNumberField(TEXT("slot_bias"), Bus.SlotBias);
					Obj->SetNumberField(TEXT("upper_attachment_count"), Bus.UpperAttachmentCount);
					Obj->SetNumberField(TEXT("lower_attachment_count"), Bus.LowerAttachmentCount);
					Obj->SetNumberField(TEXT("final_y"), Bus.FinalY);
					Obj->SetNumberField(TEXT("preferred_y"), Bus.PreferredY);
					Obj->SetNumberField(TEXT("x_min"), Bus.XMin);
					Obj->SetNumberField(TEXT("x_max"), Bus.XMax);
					TArray<TSharedPtr<FJsonValue>> ConnectionValues;
					for (const int32 ConnectionIndex : Bus.ConnectionIndices)
					{
						ConnectionValues.Add(MakeShared<FJsonValueNumber>(ConnectionIndex));
					}
					Obj->SetArrayField(TEXT("connection_indices"), ConnectionValues);
					AddDiagnosticObject(Obj);
				}
				for (const FDeterministicInterIslandBusBundle& Bundle : Bundles)
				{
					for (const FXStripCollisionBox& Box : Bundle.CollisionBoxes)
					{
						TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
						Obj->SetStringField(TEXT("kind"), TEXT("bundle_box"));
						Obj->SetNumberField(TEXT("bundle_id"), Bundle.Id);
						Obj->SetNumberField(TEXT("final_center_y"), Bundle.FinalCenterY);
						Obj->SetNumberField(TEXT("preferred_y"), Bundle.PreferredY);
						Obj->SetNumberField(TEXT("box_center_y"), Box.CenterY());
						AddRectFields(Obj, TEXT("rect"), Box.Rect);
						AddDiagnosticObject(Obj);
					}
				}
				for (int32 ConstraintIndex = 0; ConstraintIndex < Plan.BandConstraints.Num(); ++ConstraintIndex)
				{
					const FPartitionBandConstraint& Constraint = Plan.BandConstraints[ConstraintIndex];
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("kind"), TEXT("partition_band_group"));
					Obj->SetNumberField(TEXT("constraint_index"), ConstraintIndex);
					Obj->SetNumberField(TEXT("partition_index"), Constraint.PartitionIndex);
					Obj->SetNumberField(TEXT("band_index"), Constraint.BandIndex);
					Obj->SetNumberField(TEXT("group_order_in_band"), Constraint.GroupOrderInBand);
					Obj->SetNumberField(TEXT("upper_bundle_id"), Constraint.UpperBundleId);
					Obj->SetNumberField(TEXT("lower_bundle_id"), Constraint.LowerBundleId);
					Obj->SetNumberField(TEXT("group_top"), Constraint.GroupTop);
					Obj->SetNumberField(TEXT("group_bottom"), Constraint.GroupBottom);
					Obj->SetNumberField(TEXT("partition_x0"), Constraint.PartitionX0);
					Obj->SetNumberField(TEXT("partition_x1"), Constraint.PartitionX1);
					TArray<TSharedPtr<FJsonValue>> IslandValues;
					for (const int32 IslandId : Constraint.IslandIds)
					{
						IslandValues.Add(MakeShared<FJsonValueNumber>(IslandId));
					}
					Obj->SetArrayField(TEXT("island_ids"), IslandValues);
					AddDiagnosticObject(Obj);
				}
				for (const FDeterministicInterIslandBusBundle& Bundle : Bundles)
				{
					for (const FXStripCollisionBox& BundleBox : Bundle.CollisionBoxes)
					{
						for (const FXStripCollisionBox& IslandBox : Plan.IslandBoxes)
						{
							if (IslandBox.OwnerKind != EXStripCollisionOwnerKind::Island)
							{
								continue;
							}
							FRect FinalIslandRect = IslandBox.Rect;
							if (!BundleBox.Rect.Intersects(FinalIslandRect, 0.0))
							{
								continue;
							}
							TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
							Obj->SetStringField(TEXT("kind"), TEXT("bundle_island_overlap"));
							Obj->SetNumberField(TEXT("bundle_id"), Bundle.Id);
							Obj->SetNumberField(TEXT("island_id"), IslandBox.OwnerId);
							Obj->SetNumberField(TEXT("bundle_box_center_y"), BundleBox.CenterY());
							Obj->SetNumberField(TEXT("island_box_center_y"), IslandBox.CenterY());
							AddRectFields(Obj, TEXT("bundle_rect"), BundleBox.Rect);
							AddRectFields(Obj, TEXT("island_rect"), FinalIslandRect);
							AddDiagnosticObject(Obj);
						}
					}
				}
			}

		}

		static void PlanInterIslandBusBundleGeometry(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			TArray<FIsland>& Islands,
			const TArray<FInternalBusGroup>& InternalBusGroups,
			const TArray<FDeterministicIslandConnection>& Connections,
			TArray<FDeterministicInterIslandBus>& Buses,
			TArray<FDeterministicInterIslandBusBundle>& Bundles,
			const FOptions& Options,
			FRunResult& Result,
			const bool bRecordDiagnostics = true,
			const bool bApplyPartitionSpread = true)
		{
			const double LanePitchY = GetBusAvoidanceDistance();
			const double AvoidanceY = GetBusAvoidanceDistance();
			ApplyA44InterIslandBundleSlotGeometry(Buses, Bundles, LanePitchY, AvoidanceY, Result, bRecordDiagnostics);
			FInterIslandBundleGeometryPlan GeometryPlan = BuildA48InterIslandBundleGeometryPlan(
				Nodes,
				Edges,
				Islands,
				InternalBusGroups,
				Connections,
				Buses,
				Bundles,
				Options,
				Result,
				bRecordDiagnostics);
			if (bApplyPartitionSpread)
			{
				ApplyA49InterIslandPartitionSpread(
					Nodes,
					Edges,
					Islands,
					InternalBusGroups,
					Connections,
					Buses,
					Bundles,
					Options,
					GeometryPlan,
					LanePitchY,
					AvoidanceY);
			}
			FinalizeInterIslandBundleGeometryPlan(Buses, Bundles, GeometryPlan, LanePitchY, AvoidanceY, Result, bRecordDiagnostics);
		}

		static void ApplyTopologyColumnPullIn(
			const TArray<FNode>& Nodes,
			TArray<FIsland>& Islands,
			const TArray<FDeterministicIslandConnection>& Connections,
			const TArray<FIslandPortCandidate>& Candidates,
			const TArray<FDeterministicInterIslandBus>& Buses,
			const TArray<TArray<int32>>& TopologyColumns,
			const FOptions& Options,
			FRunResult& Result)
		{
			(void)Candidates;
			auto BuildOtherIslandRects = [&Nodes, &Islands, &Options](const int32 MovingIsland)
			{
				TArray<FRect> OtherRects;
				for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
				{
					if (IslandIndex == MovingIsland || !Islands.IsValidIndex(IslandIndex))
					{
						continue;
					}
					OtherRects.Append(BuildIslandPlacementOccupancyRects(Nodes, Islands[IslandIndex], Options));
				}
				return OtherRects;
			};
			auto ComputeColumnWorldSpanY = [&Nodes, &Islands, &Options](const TArray<int32>& Column, double& OutSpanY) -> bool
			{
				bool bHasBounds = false;
				double Top = TNumericLimits<double>::Max();
				double Bottom = -TNumericLimits<double>::Max();
				for (const int32 IslandIndex : Column)
				{
					if (!Islands.IsValidIndex(IslandIndex))
					{
						continue;
					}
					const FRect Bounds = BoundsForRects(BuildIslandPlacementOccupancyRects(Nodes, Islands[IslandIndex], Options));
					Top = bHasBounds ? FMath::Min(Top, Bounds.Top) : Bounds.Top;
					Bottom = bHasBounds ? FMath::Max(Bottom, Bounds.Bottom) : Bounds.Bottom;
					bHasBounds = true;
				}
				if (!bHasBounds)
				{
					return false;
				}
				OutSpanY = FMath::Max(0.0, Bottom - Top);
				return true;
			};
			auto ConnectionStaysAlignedAfterMove = [&Islands, &Connections, &Options](
				const FDeterministicIslandConnection& Connection,
				const int32 MovingIsland,
				const double DeltaX) -> bool
			{
				if (!Islands.IsValidIndex(Connection.Endpoint.SourceIsland)
					|| !Islands.IsValidIndex(Connection.Endpoint.TargetIsland))
				{
					return true;
				}
				FIsland SourceIsland = Islands[Connection.Endpoint.SourceIsland];
				FIsland TargetIsland = Islands[Connection.Endpoint.TargetIsland];
				if (Connection.Endpoint.SourceIsland == MovingIsland)
				{
					SourceIsland.X = SnapGraphCoordinate(SourceIsland.X + DeltaX);
				}
				if (Connection.Endpoint.TargetIsland == MovingIsland)
				{
					TargetIsland.X = SnapGraphCoordinate(TargetIsland.X + DeltaX);
				}

				const FVector2D SourcePoint(SourceIsland.X + Connection.Endpoint.SourceLocal.X, SourceIsland.Y + Connection.Endpoint.SourceLocal.Y);
				const FVector2D ActualTargetPoint(TargetIsland.X + Connection.Endpoint.ActualTargetLocal.X, TargetIsland.Y + Connection.Endpoint.ActualTargetLocal.Y);
				return HasNonNegativeConnectionAlignmentShorten(SourcePoint, ActualTargetPoint, Options);
			};
			auto IslandConnectionsStayAlignedAfterMove = [&Connections, &ConnectionStaysAlignedAfterMove](const int32 IslandIndex, const double DeltaX) -> bool
			{
				for (const FDeterministicIslandConnection& Connection : Connections)
				{
					if (Connection.Endpoint.SourceIsland != IslandIndex && Connection.Endpoint.TargetIsland != IslandIndex)
					{
						continue;
					}
					if (!ConnectionStaysAlignedAfterMove(Connection, IslandIndex, DeltaX))
					{
						return false;
					}
				}
				return true;
			};
			auto ComputeHorizontalPullLimit = [&Connections, &Islands, &Options](
				const int32 IslandIndex,
				const int32 Direction) -> double
			{
				double Limit = TNumericLimits<double>::Max();
				for (const FDeterministicIslandConnection& Connection : Connections)
				{
					if (!Islands.IsValidIndex(Connection.Endpoint.SourceIsland)
						|| !Islands.IsValidIndex(Connection.Endpoint.TargetIsland))
					{
						continue;
					}
					const bool bMovingSourceTowardTarget = Direction > 0 && Connection.Endpoint.SourceIsland == IslandIndex;
					const bool bMovingTargetTowardSource = Direction < 0 && Connection.Endpoint.TargetIsland == IslandIndex;
					if (!bMovingSourceTowardTarget && !bMovingTargetTowardSource)
					{
						continue;
					}

					const FIsland& SourceIsland = Islands[Connection.Endpoint.SourceIsland];
					const FIsland& TargetIsland = Islands[Connection.Endpoint.TargetIsland];
					const FVector2D SourcePoint(SourceIsland.X + Connection.Endpoint.SourceLocal.X, SourceIsland.Y + Connection.Endpoint.SourceLocal.Y);
					const FVector2D ActualTargetPoint(TargetIsland.X + Connection.Endpoint.ActualTargetLocal.X, TargetIsland.Y + Connection.Endpoint.ActualTargetLocal.Y);
					const double Available = ComputeConnectionAlignmentShortenX(SourcePoint, ActualTargetPoint, Options);
					Limit = FMath::Min(Limit, Available);
				}
				if (Limit >= TNumericLimits<double>::Max() * 0.5)
				{
					return 0.0;
				}
				return FMath::Max(0.0, Limit);
			};
			auto TryMoveIslandX = [&](const int32 IslandIndex, const int32 Direction)
			{
				if (!Islands.IsValidIndex(IslandIndex) || Direction == 0)
				{
					return;
				}
				double CandidateMove = SnapGraphCoordinate(ComputeHorizontalPullLimit(IslandIndex, Direction));
				for (; CandidateMove > 0.5; CandidateMove -= GetBusAvoidanceDistance())
				{
					const double DeltaX = Direction * CandidateMove;
					if (!IslandConnectionsStayAlignedAfterMove(IslandIndex, DeltaX))
					{
						continue;
					}
					const TArray<FRect> CandidateRects = BuildIslandPlacementRectsAt(
						Nodes,
						Islands[IslandIndex],
						Islands[IslandIndex].X + DeltaX,
						Islands[IslandIndex].Y,
						Options);
					const TArray<FRect> FixedRects = BuildOtherIslandRects(IslandIndex);
					if (!IslandRectsCollideWithPlaced(CandidateRects, FixedRects, 0.0))
					{
						Islands[IslandIndex].X = SnapGraphCoordinate(Islands[IslandIndex].X + DeltaX);
						++Result.DesignDiagnostics.TopologyColumnHorizontalPushCount;
						break;
					}
				}
			};
			auto TryPullIslandTowardIncidentBus = [&](const int32 IslandIndex)
			{
				if (!Islands.IsValidIndex(IslandIndex))
				{
					return;
				}
				TArray<double> IncidentBusYs;
				for (const FDeterministicIslandConnection& Connection : Connections)
				{
					if (!Connection.bBus || !Buses.IsValidIndex(Connection.BusId))
					{
						continue;
					}
					if (Connection.Endpoint.SourceIsland == IslandIndex || Connection.Endpoint.TargetIsland == IslandIndex)
					{
						IncidentBusYs.Add(Buses[Connection.BusId].PreferredY);
					}
				}
				if (IncidentBusYs.Num() == 0)
				{
					return;
				}
				const double TargetY = MedianOf(IncidentBusYs, IslandAabb(Islands[IslandIndex]).CenterY());
				const double CurrentCenter = IslandAabb(Islands[IslandIndex]).CenterY();
				const double DesiredDelta = SnapGraphCoordinate(TargetY - CurrentCenter);
				const int32 Direction = DesiredDelta > 0.0 ? 1 : -1;
				for (double CandidateMove = FMath::Abs(DesiredDelta); CandidateMove > 0.5; CandidateMove -= GetBusAvoidanceDistance())
				{
					const double DeltaY = Direction * CandidateMove;
					const TArray<FRect> CandidateRects = BuildIslandPlacementRectsAt(
						Nodes,
						Islands[IslandIndex],
						Islands[IslandIndex].X,
						Islands[IslandIndex].Y + DeltaY,
						Options);
					const TArray<FRect> FixedRects = BuildOtherIslandRects(IslandIndex);
					if (!IslandRectsCollideWithPlaced(CandidateRects, FixedRects, 0.0))
					{
						Islands[IslandIndex].Y = SnapGraphCoordinate(Islands[IslandIndex].Y + DeltaY);
						++Result.DesignDiagnostics.TopologyColumnBusPullCount;
						break;
					}
				}
			};
			auto IslandHasAnyOutput = [&Connections](const int32 IslandIndex) -> bool
			{
				for (const FDeterministicIslandConnection& Connection : Connections)
				{
					if (Connection.Endpoint.SourceIsland == IslandIndex)
					{
						return true;
					}
				}
				return false;
			};
			auto RestoreNoOutputColumnLeftAlignment = [&]()
			{
				for (const TArray<int32>& Column : TopologyColumns)
				{
					bool bHasNoOutputLeft = false;
					double NoOutputLeftX = 0.0;
					for (const int32 IslandIndex : Column)
					{
						if (!Islands.IsValidIndex(IslandIndex) || IslandHasAnyOutput(IslandIndex))
						{
							continue;
						}
						NoOutputLeftX = bHasNoOutputLeft ? FMath::Max(NoOutputLeftX, Islands[IslandIndex].X) : Islands[IslandIndex].X;
						bHasNoOutputLeft = true;
					}
					if (!bHasNoOutputLeft)
					{
						continue;
					}
					for (const int32 IslandIndex : Column)
					{
						if (!Islands.IsValidIndex(IslandIndex) || IslandHasAnyOutput(IslandIndex))
						{
							continue;
						}
						const double DeltaX = SnapGraphCoordinate(NoOutputLeftX - Islands[IslandIndex].X);
						if (FMath::Abs(DeltaX) <= 0.5)
						{
							continue;
						}
						if (!IslandConnectionsStayAlignedAfterMove(IslandIndex, DeltaX))
						{
							continue;
						}
						const TArray<FRect> CandidateRects = BuildIslandPlacementRectsAt(
							Nodes,
							Islands[IslandIndex],
							Islands[IslandIndex].X + DeltaX,
							Islands[IslandIndex].Y,
							Options);
						const TArray<FRect> FixedRects = BuildOtherIslandRects(IslandIndex);
						if (!IslandRectsCollideWithPlaced(CandidateRects, FixedRects, 0.0))
						{
							Islands[IslandIndex].X = SnapGraphCoordinate(Islands[IslandIndex].X + DeltaX);
						}
					}
				}
			};

			int32 BaseColumnIndex = INDEX_NONE;
			double BestColumnSpanY = -1.0;
			for (int32 ColumnIndex = 0; ColumnIndex < TopologyColumns.Num(); ++ColumnIndex)
			{
				double SpanY = 0.0;
				if (ComputeColumnWorldSpanY(TopologyColumns[ColumnIndex], SpanY) && SpanY > BestColumnSpanY + 0.5)
				{
					BestColumnSpanY = SpanY;
					BaseColumnIndex = ColumnIndex;
				}
			}
			if (BaseColumnIndex == INDEX_NONE)
			{
				return;
			}

			auto ProcessColumn = [&](const TArray<int32>& Column, const int32 Direction)
			{
				for (const int32 IslandIndex : Column)
				{
					TryMoveIslandX(IslandIndex, Direction);
					TryPullIslandTowardIncidentBus(IslandIndex);
				}
			};

			for (int32 ColumnIndex = BaseColumnIndex - 1; ColumnIndex >= 0; --ColumnIndex)
			{
				ProcessColumn(TopologyColumns[ColumnIndex], 1);
			}
			for (int32 ColumnIndex = BaseColumnIndex + 1; ColumnIndex < TopologyColumns.Num(); ++ColumnIndex)
			{
				ProcessColumn(TopologyColumns[ColumnIndex], -1);
			}
			RestoreNoOutputColumnLeftAlignment();
		}

		static void ResolveBusBranchSlotsAndRightShift(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FInternalBusGroup>& InternalBusGroups,
			TArray<FIsland>& Islands,
			TArray<FDeterministicIslandConnection>& Connections,
			const TArray<FIslandPortCandidate>& Candidates,
			const TArray<FDeterministicInterIslandBus>& Buses,
			const TArray<FIslandEdge>& IslandEdges,
			const TSet<int32>& RouteOnlyIslandEdgeIndices,
			const TArray<int32>& TopoOrder,
			const FOptions& Options,
			FRunResult& Result,
			const bool bRecordDiagnostics = true,
			const bool bApplyIslandRightShift = true)
		{
			const double BranchSpacing = GetCrossBusBranchLaneSpacing(Options);
			const double IslandStripBandPadding = FMath::Max(4.0, Options.NodePadding * 0.25);
			const double IslandStripCollisionPadding = FMath::Max(0.0, Options.NodePadding - 2.0 * IslandStripBandPadding);
			TArray<double> LocalSlotShift;
			LocalSlotShift.Init(0.0, Islands.Num());
			auto IslandHasAnyOutput = [&Islands, &IslandEdges](const int32 IslandIndex) -> bool
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
			TMap<int32, TArray<int32>> NoOutputGroupsByInitialLeftX;
			for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
			{
				if (!IslandHasAnyOutput(IslandIndex))
				{
					NoOutputGroupsByInitialLeftX.FindOrAdd(FMath::RoundToInt(SnapGraphCoordinate(Islands[IslandIndex].X))).Add(IslandIndex);
				}
			}
			struct FIncomingBranchSlotItem
			{
				int32 ConnectionIndex = INDEX_NONE;
				int32 TargetIsland = INDEX_NONE;
				int32 EdgeIndex = INDEX_NONE;
				double BusY = 0.0;
				double SourceY = 0.0;
				double TargetY = 0.0;
				double RightBoundX = 0.0;
				double OutputToBusDistanceY = 0.0;
				double TargetToBusDistanceY = 0.0;
				double YMin = 0.0;
				double YMax = 0.0;
			};
			TArray<FIncomingBranchSlotItem> BranchItems;
			for (int32 ConnectionIndex = 0; ConnectionIndex < Connections.Num(); ++ConnectionIndex)
			{
				const FDeterministicIslandConnection& Connection = Connections[ConnectionIndex];
				if (!Connection.bBus
					|| !Buses.IsValidIndex(Connection.BusId)
					|| !Islands.IsValidIndex(Connection.Endpoint.SourceIsland)
					|| !Islands.IsValidIndex(Connection.Endpoint.TargetIsland))
				{
					continue;
				}
				const FIsland& SourceIsland = Islands[Connection.Endpoint.SourceIsland];
				const FIsland& TargetIsland = Islands[Connection.Endpoint.TargetIsland];
				const FVector2D SourcePoint(SourceIsland.X + Connection.Endpoint.SourceLocal.X, SourceIsland.Y + Connection.Endpoint.SourceLocal.Y);
				const FVector2D ActualTargetPoint(TargetIsland.X + Connection.Endpoint.ActualTargetLocal.X, TargetIsland.Y + Connection.Endpoint.ActualTargetLocal.Y);
				const FDeterministicInterIslandBus& Bus = Buses[Connection.BusId];
				FIncomingBranchSlotItem Item;
				Item.ConnectionIndex = ConnectionIndex;
				Item.TargetIsland = Connection.Endpoint.TargetIsland;
				Item.EdgeIndex = Connection.Endpoint.EdgeIndex;
				Item.BusY = Bus.FinalY;
				Item.SourceY = SourcePoint.Y;
				Item.TargetY = ActualTargetPoint.Y;
				Item.RightBoundX = ComputeOutputXAlignedToInput(ActualTargetPoint.X, Options);
				Item.OutputToBusDistanceY = FMath::Abs(Item.SourceY - Item.BusY);
				Item.TargetToBusDistanceY = FMath::Abs(Item.TargetY - Item.BusY);
				Item.YMin = FMath::Min(Item.BusY, Item.TargetY);
				Item.YMax = FMath::Max(Item.BusY, Item.TargetY);
				BranchItems.Add(Item);
			}

			if (BranchItems.Num() > 0)
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
						const FIncomingBranchSlotItem& Left = BranchItems[A];
						const FIncomingBranchSlotItem& Right = BranchItems[B];
						if (!IntervalOverlaps(Left.YMin, Left.YMax, Right.YMin, Right.YMax, GetBusAvoidanceDistance()))
						{
							continue;
						}
						UnionItems(A, B);
					}
				}

				TMap<int32, TArray<int32>> GroupsByRoot;
				for (int32 ItemIndex = 0; ItemIndex < BranchItems.Num(); ++ItemIndex)
				{
					GroupsByRoot.FindOrAdd(FindRoot(ItemIndex)).Add(ItemIndex);
				}

				for (TPair<int32, TArray<int32>>& Pair : GroupsByRoot)
				{
					TArray<int32>& Group = Pair.Value;
					Group.Sort([&BranchItems](const int32 A, const int32 B)
					{
						const FIncomingBranchSlotItem& Left = BranchItems[A];
						const FIncomingBranchSlotItem& Right = BranchItems[B];
						if (!FMath::IsNearlyEqual(Left.TargetToBusDistanceY, Right.TargetToBusDistanceY, 0.5))
						{
							return Left.TargetToBusDistanceY < Right.TargetToBusDistanceY;
						}
						if (!FMath::IsNearlyEqual(Left.OutputToBusDistanceY, Right.OutputToBusDistanceY, 0.5))
						{
							return Left.OutputToBusDistanceY < Right.OutputToBusDistanceY;
						}
						if (!FMath::IsNearlyEqual(Left.RightBoundX, Right.RightBoundX, 0.5))
						{
							return Left.RightBoundX > Right.RightBoundX;
						}
						if (!FMath::IsNearlyEqual(Left.TargetY, Right.TargetY, 0.5))
						{
							return Left.TargetY < Right.TargetY;
						}
						return Left.EdgeIndex < Right.EdgeIndex;
					});

					TArray<int32> AssignedSlots;
					AssignedSlots.Init(INDEX_NONE, Group.Num());
					for (int32 GroupIndex = 0; GroupIndex < Group.Num(); ++GroupIndex)
					{
						const FIncomingBranchSlotItem& Branch = BranchItems[Group[GroupIndex]];
						int32 Slot = 0;
						for (;; ++Slot)
						{
							bool bFree = true;
							for (int32 OtherGroupIndex = 0; OtherGroupIndex < GroupIndex; ++OtherGroupIndex)
							{
								if (AssignedSlots[OtherGroupIndex] != Slot)
								{
									continue;
								}
								const FIncomingBranchSlotItem& Other = BranchItems[Group[OtherGroupIndex]];
								if (IntervalOverlaps(Branch.YMin, Branch.YMax, Other.YMin, Other.YMax, GetBusAvoidanceDistance()))
								{
									bFree = false;
									break;
								}
							}
							if (bFree)
							{
								break;
							}
						}
						AssignedSlots[GroupIndex] = Slot;
						Connections[Branch.ConnectionIndex].BranchSlotIndex = Slot;
						if (LocalSlotShift.IsValidIndex(Branch.TargetIsland))
						{
							LocalSlotShift[Branch.TargetIsland] = FMath::Max(LocalSlotShift[Branch.TargetIsland], Slot * BranchSpacing);
						}
						if (bRecordDiagnostics)
						{
							Result.DesignDiagnostics.InterIslandBusBranchXSlotCount = FMath::Max(Result.DesignDiagnostics.InterIslandBusBranchXSlotCount, Slot + 1);
						}
					}
					if (bRecordDiagnostics)
					{
						Result.DesignDiagnostics.IncomingBusBranchCount += Group.Num();
					}
				}
			}

			for (FDeterministicIslandConnection& Connection : Connections)
			{
				if (!Connection.bBus)
				{
					continue;
				}
				ResolveSelectedEndpointEntry(
					Candidates,
					Islands,
					Options,
					Connection.Endpoint,
					RequiredBusEntrySourceSpanX(Connection, Buses, Islands, Options));
			}

			if (!bApplyIslandRightShift)
			{
				return;
			}

			TSet<int32> InterIslandEdgeIndices;
			for (const FDeterministicIslandConnection& Connection : Connections)
			{
				if (Connection.Endpoint.EdgeIndex != INDEX_NONE)
				{
					InterIslandEdgeIndices.Add(Connection.Endpoint.EdgeIndex);
				}
			}

			auto BuildCurrentIslandStripBoxes = [&](const int32 IslandIndex) -> TArray<FXStripCollisionBox>
			{
				if (!Islands.IsValidIndex(IslandIndex))
				{
					return {};
				}
				return BuildIslandBusCollisionBoxes(
					Nodes,
					Edges,
					InternalBusGroups,
					Connections,
					Islands[IslandIndex],
					IslandIndex,
					InterIslandEdgeIndices,
					Options);
			};

			auto ComputeRightShiftToAvoidConfirmed = [IslandStripCollisionPadding](const TArray<FXStripCollisionBox>& CurrentBoxes, const TArray<FXStripCollisionBox>& ConfirmedBoxes) -> double
			{
				constexpr double BoundaryEpsilon = 1.0;
				double RequiredShift = 0.0;
				const int32 MaxIterations = FMath::Max(16, CurrentBoxes.Num() * FMath::Max(1, ConfirmedBoxes.Num()) + 16);
				for (int32 Iteration = 0; Iteration < MaxIterations; ++Iteration)
				{
					bool bChanged = false;
					for (const FXStripCollisionBox& CurrentBaseBox : CurrentBoxes)
					{
						FXStripCollisionBox ShiftedCurrentBox = CurrentBaseBox;
						ShiftedCurrentBox.Rect.Left += RequiredShift;
						ShiftedCurrentBox.Rect.Right += RequiredShift;
						for (const FXStripCollisionBox& ConfirmedBox : ConfirmedBoxes)
						{
							if (!XStripIntersects(ShiftedCurrentBox, ConfirmedBox, IslandStripCollisionPadding))
							{
								continue;
							}
							const double CandidateShift = SnapGraphCoordinate(ConfirmedBox.Rect.Right + IslandStripCollisionPadding - CurrentBaseBox.Rect.Left + BoundaryEpsilon);
							if (CandidateShift > RequiredShift + 0.5)
							{
								RequiredShift = CandidateShift;
								bChanged = true;
							}
						}
					}
					if (!bChanged)
					{
						break;
					}
				}
				return FMath::Max(0.0, SnapGraphCoordinate(RequiredShift));
			};

			auto AppendSweptStripBoxes = [](TArray<FXStripCollisionBox>& ConfirmedBoxes, const TArray<FXStripCollisionBox>& BeforeBoxes, const double DeltaX)
			{
				if (DeltaX <= 0.5)
				{
					return;
				}
				for (const FXStripCollisionBox& BeforeBox : BeforeBoxes)
				{
					const FRect SweptRect = {
						FMath::Min(BeforeBox.Rect.Left, BeforeBox.Rect.Left + DeltaX),
						BeforeBox.Rect.Top,
						FMath::Max(BeforeBox.Rect.Right, BeforeBox.Rect.Right + DeltaX),
						BeforeBox.Rect.Bottom
					};
					AddXStripCollisionBox(
						ConfirmedBoxes,
						BeforeBox.OwnerKind,
						BeforeBox.OwnerId,
						BeforeBox.SecondaryOwnerId,
						SweptRect,
						BeforeBox.CenterY());
				}
			};

			TArray<int32> SweepOrder;
			TArray<int32> SweepTopoRank;
			TSet<int64> SweepForwardIslandEdgeKeys;
			SweepTopoRank.Init(TNumericLimits<int32>::Max(), Islands.Num());
			for (int32 OrderIndex = 0; OrderIndex < TopoOrder.Num(); ++OrderIndex)
			{
				if (SweepTopoRank.IsValidIndex(TopoOrder[OrderIndex]))
				{
					SweepTopoRank[TopoOrder[OrderIndex]] = OrderIndex;
				}
			}
			for (int32 IslandEdgeIndex = 0; IslandEdgeIndex < IslandEdges.Num(); ++IslandEdgeIndex)
			{
				if (RouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex))
				{
					continue;
				}
				const FIslandEdge& IslandEdge = IslandEdges[IslandEdgeIndex];
				if (Islands.IsValidIndex(IslandEdge.SourceIsland) && Islands.IsValidIndex(IslandEdge.TargetIsland))
				{
					SweepForwardIslandEdgeKeys.Add(MakeIslandPairKey(IslandEdge.SourceIsland, IslandEdge.TargetIsland));
				}
			}
			SweepOrder.Reserve(Islands.Num());
			for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
			{
				SweepOrder.Add(IslandIndex);
			}
			SweepOrder.Sort([&Islands, &Options, &SweepTopoRank, &SweepForwardIslandEdgeKeys](const int32 A, const int32 B)
			{
				const FRect LeftBounds = IslandAabb(Islands[A]);
				const FRect RightBounds = IslandAabb(Islands[B]);
				const bool bXOccupancyNeighbors = XStripRangeOverlaps(
					LeftBounds.Left,
					LeftBounds.Right,
					RightBounds.Left,
					RightBounds.Right,
					FMath::Max(0.0, Options.NodePadding));
				if (bXOccupancyNeighbors)
				{
					const bool bAFeedsB = SweepForwardIslandEdgeKeys.Contains(MakeIslandPairKey(A, B));
					const bool bBFeedsA = SweepForwardIslandEdgeKeys.Contains(MakeIslandPairKey(B, A));
					if (bAFeedsB != bBFeedsA)
					{
						return bAFeedsB;
					}
				}
				if (!FMath::IsNearlyEqual(LeftBounds.Left, RightBounds.Left, 0.5))
				{
					return LeftBounds.Left < RightBounds.Left;
				}
				const int32 TopoRankA = SweepTopoRank.IsValidIndex(A) ? SweepTopoRank[A] : TNumericLimits<int32>::Max();
				const int32 TopoRankB = SweepTopoRank.IsValidIndex(B) ? SweepTopoRank[B] : TNumericLimits<int32>::Max();
				if (TopoRankA != TopoRankB)
				{
					return TopoRankA < TopoRankB;
				}
				if (!FMath::IsNearlyEqual(LeftBounds.Top, RightBounds.Top, 0.5))
				{
					return LeftBounds.Top < RightBounds.Top;
				}
				return A < B;
			});

			TArray<FXStripCollisionBox> ConfirmedStripOccupancy;
			for (const int32 IslandIndex : SweepOrder)
			{
				if (!Islands.IsValidIndex(IslandIndex))
				{
					continue;
				}

				TArray<FXStripCollisionBox> CurrentBoxes = BuildCurrentIslandStripBoxes(IslandIndex);
				const double CollisionShift = ComputeRightShiftToAvoidConfirmed(CurrentBoxes, ConfirmedStripOccupancy);
				if (CollisionShift > 0.5)
				{
					Islands[IslandIndex].X = SnapGraphCoordinate(Islands[IslandIndex].X + CollisionShift);
				}

				TArray<FXStripCollisionBox> PreSelfShiftBoxes = BuildCurrentIslandStripBoxes(IslandIndex);
				const double PreSelfShiftX = Islands[IslandIndex].X;
				double SelfShift = LocalSlotShift.IsValidIndex(IslandIndex) ? LocalSlotShift[IslandIndex] : 0.0;
				for (const FDeterministicIslandConnection& Connection : Connections)
				{
					if (!Connection.bBus
						|| Connection.Endpoint.TargetIsland != IslandIndex
						|| !Islands.IsValidIndex(Connection.Endpoint.SourceIsland))
					{
						continue;
					}
					const FIsland& SourceIsland = Islands[Connection.Endpoint.SourceIsland];
					const double SourceX = SourceIsland.X + Connection.Endpoint.SourceLocal.X;
					const double RequiredTargetLeft =
						RequiredBusTargetLeftForForwardBranch(Connection, SourceX, Options);
					SelfShift = FMath::Max(SelfShift, RequiredTargetLeft - Islands[IslandIndex].X);
				}
				SelfShift = FMath::Max(0.0, SnapGraphCoordinate(SelfShift));
				if (SelfShift > 0.5)
				{
					Islands[IslandIndex].X = SnapGraphCoordinate(Islands[IslandIndex].X + SelfShift);
					if (bRecordDiagnostics)
					{
						++Result.DesignDiagnostics.IslandBusBranchRightShiftCount;
					}
				}

				TArray<FXStripCollisionBox> FinalBoxes = BuildCurrentIslandStripBoxes(IslandIndex);
				const double FinalCollisionShift = ComputeRightShiftToAvoidConfirmed(FinalBoxes, ConfirmedStripOccupancy);
				if (FinalCollisionShift > 0.5)
				{
					Islands[IslandIndex].X = SnapGraphCoordinate(Islands[IslandIndex].X + FinalCollisionShift);
					FinalBoxes = BuildCurrentIslandStripBoxes(IslandIndex);
				}

				AppendSweptStripBoxes(ConfirmedStripOccupancy, PreSelfShiftBoxes, Islands[IslandIndex].X - PreSelfShiftX);
				ConfirmedStripOccupancy.Append(FinalBoxes);
			}
			for (const TPair<int32, TArray<int32>>& Pair : NoOutputGroupsByInitialLeftX)
			{
				bool bHasTargetLeft = false;
				double TargetLeftX = 0.0;
				for (const int32 IslandIndex : Pair.Value)
				{
					if (!Islands.IsValidIndex(IslandIndex))
					{
						continue;
					}
					TargetLeftX = bHasTargetLeft ? FMath::Max(TargetLeftX, Islands[IslandIndex].X) : Islands[IslandIndex].X;
					bHasTargetLeft = true;
				}
				if (!bHasTargetLeft)
				{
					continue;
				}
				for (const int32 IslandIndex : Pair.Value)
				{
					if (Islands.IsValidIndex(IslandIndex))
					{
						Islands[IslandIndex].X = SnapGraphCoordinate(TargetLeftX);
					}
				}
			}
		}

		static TArray<FReroutePlan> MaterializeFinalDirectInterIslandPlans(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const TArray<FDeterministicIslandConnection>& Connections,
			const FOptions& Options,
			FRunResult& Result)
		{
			TArray<FReroutePlan> Plans;
			for (const FDeterministicIslandConnection& Connection : Connections)
			{
				if (!Connection.bDirect
					|| !Edges.IsValidIndex(Connection.Endpoint.EdgeIndex)
					|| !Islands.IsValidIndex(Connection.Endpoint.SourceIsland)
					|| !Islands.IsValidIndex(Connection.Endpoint.TargetIsland))
				{
					continue;
				}
				const FVector2D SourcePoint(
					Islands[Connection.Endpoint.SourceIsland].X + Connection.Endpoint.SourceLocal.X,
					Islands[Connection.Endpoint.SourceIsland].Y + Connection.Endpoint.SourceLocal.Y);
				const FVector2D ActualTargetPoint(
					Islands[Connection.Endpoint.TargetIsland].X + Connection.Endpoint.ActualTargetLocal.X,
					Islands[Connection.Endpoint.TargetIsland].Y + Connection.Endpoint.ActualTargetLocal.Y);
				TArray<FVector2D> Route;
				if (Connection.DirectReroutePoints.Num() > 0)
				{
					Route.Add(SourcePoint);
					for (const FVector2D& Point : Connection.DirectReroutePoints)
					{
						Route.Add(Point);
					}
					Route.Add(ActualTargetPoint);
				}
				else
				{
					Route = BuildDirectSplinePolyline(SourcePoint, ActualTargetPoint);
				}
				const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[Connection.Endpoint.EdgeIndex]);
				const bool bCrossesHard = ControlWireHitsHardOccupancy(Route, RouteHardRects);
				AddRouteDiagnostics(Result, Connection.Endpoint.EdgeIndex, TEXT("direct"), ERouteKind::Direct, Route, false, bCrossesHard);
				Result.DesignDiagnostics.IslandConnectionTotalLength += PolylineManhattanLength(Route);
				if (Connection.DirectReroutePoints.Num() > 0)
				{
					FReroutePlan Plan;
					Plan.EdgeIndex = Connection.Endpoint.EdgeIndex;
					Plan.Reason = TEXT("direct_endpoint_knots");
					Plan.RouteKind = ERouteKind::Direct;
					Plan.Points = Connection.DirectReroutePoints;
					FinalizeReroutePlanPoints(Plan, Options);
					Plans.Add(Plan);
				}
			}
			return Plans;
		}

		static bool CanOmitSingleNodeIslandBusBranchKnot(
			const FIsland& TargetIsland,
			const FDeterministicIslandConnection& Connection,
			const FVector2D& SourcePoint,
			const FVector2D& SourceTrunkPoint,
			const FVector2D& VisibleTargetPoint,
			const FVector2D& ActualTargetPoint,
			const FOptions& Options,
			const TArray<FRect>& RouteHardRects)
		{
			if (TargetIsland.Nodes.Num() != 1)
			{
				return false;
			}
			if (Connection.BranchSlotIndex != 0)
			{
				return false;
			}
			if (FVector2D::Distance(VisibleTargetPoint, ActualTargetPoint) > 0.5)
			{
				return false;
			}
			if (!HasNonNegativeConnectionAlignmentShorten(SourceTrunkPoint, ActualTargetPoint, Options))
			{
				return false;
			}
			TArray<FVector2D> CandidateRoute;
			CandidateRoute.Add(SourcePoint);
			CandidateRoute.Add(SourceTrunkPoint);
			CandidateRoute.Add(ActualTargetPoint);
			return !ControlWireHitsHardOccupancy(CandidateRoute, RouteHardRects);
		}

		static TArray<FReroutePlan> MaterializeInterIslandBusPlans(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const TArray<FDeterministicIslandConnection>& Connections,
			const TArray<FDeterministicInterIslandBus>& Buses,
			const FOptions& Options,
			FRunResult& Result)
		{
			TArray<FReroutePlan> Plans;
			TSet<int32> ActiveBusIds;
			auto AddDistinctPoint = [](TArray<FVector2D>& Points, const FVector2D& Point)
			{
				const FVector2D SnappedPoint = SnapGraphPoint(Point);
				if (Points.Num() == 0 || FVector2D::Distance(Points.Last(), SnappedPoint) > 0.5)
				{
					Points.Add(SnappedPoint);
				}
			};

			for (const FDeterministicIslandConnection& Connection : Connections)
			{
				if (!Connection.bBus
					|| !Edges.IsValidIndex(Connection.Endpoint.EdgeIndex)
					|| !Islands.IsValidIndex(Connection.Endpoint.SourceIsland)
					|| !Islands.IsValidIndex(Connection.Endpoint.TargetIsland)
					|| !Buses.IsValidIndex(Connection.BusId))
				{
					continue;
				}
				const FDeterministicInterIslandBus& Bus = Buses[Connection.BusId];
				ActiveBusIds.Add(Bus.Id);
				const FVector2D SourcePoint(
					Islands[Connection.Endpoint.SourceIsland].X + Connection.Endpoint.SourceLocal.X,
					Islands[Connection.Endpoint.SourceIsland].Y + Connection.Endpoint.SourceLocal.Y);
				const FVector2D ActualTargetPoint(
					Islands[Connection.Endpoint.TargetIsland].X + Connection.Endpoint.ActualTargetLocal.X,
					Islands[Connection.Endpoint.TargetIsland].Y + Connection.Endpoint.ActualTargetLocal.Y);
				const FVector2D EntryPoint(
					Islands[Connection.Endpoint.TargetIsland].X + Connection.Endpoint.EntryLocal.X,
					Islands[Connection.Endpoint.TargetIsland].Y + Connection.Endpoint.EntryLocal.Y);
				const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[Connection.Endpoint.EdgeIndex]);
				const double BranchSpacing = GetCrossBusBranchLaneSpacing(Options);
				TArray<FVector2D> Route;
				AddDistinctPoint(Route, SourcePoint);
				const FVector2D SourceTrunkPoint(
					ComputeInputXAlignedToOutput(SourcePoint.X, Options),
					Bus.FinalY);
				const FVector2D BranchPoint(
					ComputeOutputXAlignedToInput(ActualTargetPoint.X, Options)
						- FMath::Max(0, Connection.BranchSlotIndex) * BranchSpacing,
					Bus.FinalY);
				FDeterministicIslandConnection VisibleTargetConnection = Connection;
				VisibleTargetConnection.EntryPoint = EntryPoint;
				VisibleTargetConnection.ActualTargetPoint = ActualTargetPoint;
				const FVector2D VisibleTargetPoint = ResolveBusBranchVisibleTargetPoint(VisibleTargetConnection);
				const FString TargetMode = ResolveBusBranchTargetMode(VisibleTargetConnection);
				const bool bVisibleTargetIsActualTarget = FVector2D::Distance(VisibleTargetPoint, ActualTargetPoint) <= 0.5;
				const bool bMergeVisibleTargetWithActual =
					!bVisibleTargetIsActualTarget
					&& ShouldMergeBusVisibleTargetWithActualTarget(VisibleTargetConnection, VisibleTargetPoint, ActualTargetPoint);
				const bool bOmitBranchKnot = CanOmitSingleNodeIslandBusBranchKnot(
					Islands[Connection.Endpoint.TargetIsland],
					Connection,
					SourcePoint,
					SourceTrunkPoint,
					VisibleTargetPoint,
					ActualTargetPoint,
					Options,
					RouteHardRects);
				AddDistinctPoint(Route, SourceTrunkPoint);
				if (!bOmitBranchKnot)
				{
					AddDistinctPoint(Route, BranchPoint);
				}
				if (!bVisibleTargetIsActualTarget && !bMergeVisibleTargetWithActual)
				{
					AddDistinctPoint(Route, VisibleTargetPoint);
				}
				AddDistinctPoint(Route, ActualTargetPoint);
				TSharedPtr<FJsonObject> VisibleTargetObj = MakeShared<FJsonObject>();
				VisibleTargetObj->SetNumberField(TEXT("edge_index"), Connection.Endpoint.EdgeIndex);
				VisibleTargetObj->SetNumberField(TEXT("bus_id"), Bus.Id);
				VisibleTargetObj->SetStringField(TEXT("bus_key"), Bus.Key);
				VisibleTargetObj->SetNumberField(TEXT("branch_slot_index"), Connection.BranchSlotIndex);
				VisibleTargetObj->SetObjectField(TEXT("branch_point"), PointToJson(BranchPoint));
				VisibleTargetObj->SetObjectField(TEXT("visible_target_point"), PointToJson(VisibleTargetPoint));
				VisibleTargetObj->SetObjectField(TEXT("entry_point"), PointToJson(EntryPoint));
				VisibleTargetObj->SetObjectField(TEXT("actual_target_point"), PointToJson(ActualTargetPoint));
				VisibleTargetObj->SetStringField(TEXT("target_mode"), TargetMode);
				VisibleTargetObj->SetBoolField(TEXT("merged_with_actual_target"), bVisibleTargetIsActualTarget || bMergeVisibleTargetWithActual);
				VisibleTargetObj->SetBoolField(TEXT("branch_knot_omitted"), bOmitBranchKnot);
				Result.InterIslandBusBranchVisibleTargetDiagnostics.Add(MakeShared<FJsonValueObject>(VisibleTargetObj));
				TSharedPtr<FJsonObject> TargetModeObj = MakeShared<FJsonObject>();
				TargetModeObj->SetNumberField(TEXT("edge_index"), Connection.Endpoint.EdgeIndex);
				TargetModeObj->SetNumberField(TEXT("bus_id"), Bus.Id);
				TargetModeObj->SetNumberField(TEXT("branch_slot_index"), Connection.BranchSlotIndex);
				TargetModeObj->SetStringField(TEXT("target_mode"), TargetMode);
				TargetModeObj->SetBoolField(TEXT("branch_knot_omitted"), bOmitBranchKnot);
				Result.InterIslandBusBranchTargetModeDiagnostics.Add(MakeShared<FJsonValueObject>(TargetModeObj));
				TArray<FVector2D> DiagnosticRoute = Route;
				FReroutePlan Plan;
				Plan.EdgeIndex = Connection.Endpoint.EdgeIndex;
				Plan.Reason = Connection.BusReason == TEXT("feedback") ? TEXT("inter_island_feedback_bus") : TEXT("inter_island_bus_bundle");
				Plan.BusId = Bus.Key;
				Plan.RouteKind = ERouteKind::ExternalBus;
				Plan.BusLaneIndex = Bus.SlotIndex;
				Plan.BranchLaneIndex = Connection.BranchSlotIndex;
				Plan.AttachmentMergeGroup = Bus.Id;
				for (int32 PointIndex = 1; PointIndex < Route.Num(); ++PointIndex)
				{
					if (PointIndex == Route.Num() - 1 && FVector2D::Distance(Route[PointIndex], ActualTargetPoint) <= 0.5)
					{
						continue;
					}
					Plan.Points.Add(Route[PointIndex]);
				}
				FinalizeReroutePlanPoints(Plan, Options);
				Plans.Add(Plan);
				const bool bCrossesHard = ControlWireHitsHardOccupancy(DiagnosticRoute, RouteHardRects);
				AddRouteDiagnostics(Result, Connection.Endpoint.EdgeIndex, TEXT("inter_island_bus"), ERouteKind::ExternalBus, DiagnosticRoute, true, bCrossesHard);
				Result.DesignDiagnostics.IslandConnectionTotalLength += PolylineManhattanLength(DiagnosticRoute);
				++Result.DesignDiagnostics.RouteKindUpgradeCount;
				++Result.DesignDiagnostics.IslandConnectionExternalUpgradeCount;
				if (bOmitBranchKnot)
				{
					++Result.DesignDiagnostics.InterIslandBusBranchKnotOmittedCount;
				}
				else
				{
					++Result.DesignDiagnostics.InterIslandBusBranchKnotCount;
				}
			}
			Result.DesignDiagnostics.BranchMergeGroupCount = ActiveBusIds.Num();
			Result.DesignDiagnostics.RouteThroughIslandCount = Result.DesignDiagnostics.RouteThroughHardOccupancyCount;
			return Plans;
		}

		static TArray<FReroutePlan> RunDeterministicInterIslandSolver(
			TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			TArray<FIsland>& Islands,
			const TArray<FIslandEdge>& IslandEdges,
			const TArray<FInternalBusGroup>& InternalBusGroups,
			const TArray<FIslandPortCandidate>& Candidates,
			const FOptions& Options,
			FRunResult& Result)
		{
			const double SolverStartSeconds = FPlatformTime::Seconds();
			TSet<int32> RouteOnlyIslandEdgeIndices;
			TSet<int32> RouteOnlyEdgeIndices;
			TSet<int32> ForwardExemptIslandEdgeIndices;
			for (int32 IslandEdgeIndex = 0; IslandEdgeIndex < IslandEdges.Num(); ++IslandEdgeIndex)
			{
				if (IslandEdges[IslandEdgeIndex].bFeedback)
				{
					RouteOnlyIslandEdgeIndices.Add(IslandEdgeIndex);
					RouteOnlyEdgeIndices.Add(IslandEdges[IslandEdgeIndex].EdgeIndex);
					ForwardExemptIslandEdgeIndices.Add(IslandEdgeIndex);
				}
			}
			AssignIslandPlacementRanks(Islands, IslandEdges, ForwardExemptIslandEdgeIndices);
			RouteOnlyEdgeIndices.Reset();
			for (int32 IslandEdgeIndex = 0; IslandEdgeIndex < IslandEdges.Num(); ++IslandEdgeIndex)
			{
				if (RouteOnlyIslandEdgeIndices.Contains(IslandEdgeIndex))
				{
					RouteOnlyEdgeIndices.Add(IslandEdges[IslandEdgeIndex].EdgeIndex);
				}
			}
			AssignIslandPlacementRanks(Islands, IslandEdges, ForwardExemptIslandEdgeIndices);

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

			TArray<FDeterministicIslandConnection> Connections;
			Connections.Reserve(IslandEdges.Num());
			TSet<int32> SelectedEdgeIndices;
			for (const FIslandEdge& IslandEdge : IslandEdges)
			{
				if (!Edges.IsValidIndex(IslandEdge.EdgeIndex))
				{
					continue;
				}
				if (SelectedEdgeIndices.Contains(IslandEdge.EdgeIndex))
				{
					continue;
				}
				FSelectedEndpointCandidate Selected;
				if (!ChooseInitialIslandCandidate(Candidates, IslandEdge.EdgeIndex, Options, Selected))
				{
					Selected.EdgeIndex = IslandEdge.EdgeIndex;
					Selected.CandidateId = INDEX_NONE;
					Selected.SourceIsland = IslandEdge.SourceIsland;
					Selected.TargetIsland = IslandEdge.TargetIsland;
					Selected.Side = IslandEdge.TargetSide;
					Selected.SourceLocal = IslandEdge.SourceLocal;
					Selected.EntryLocal = IslandEdge.TargetLocal;
					Selected.ActualTargetLocal = IslandEdge.TargetLocal;
					Selected.SourcePortOrderKey = IslandEdge.SourceLocal.Y;
					++Result.DesignDiagnostics.RejectedProposalCount;
				}
				FDeterministicIslandConnection Connection;
				Connection.Endpoint = Selected;
				Connection.bRouteOnly = RouteOnlyEdgeIndices.Contains(IslandEdge.EdgeIndex);
				Connection.bBus = Connection.bRouteOnly;
				Connection.BusReason = Connection.bRouteOnly ? TEXT("feedback") : TEXT("");
				Connections.Add(Connection);
				SelectedEdgeIndices.Add(Selected.EdgeIndex);
			}
			Result.DesignDiagnostics.PlacementProposalCount = Connections.Num();
			Result.DesignDiagnostics.SelectedEndpointCandidateCount = Connections.Num();

			const TArray<int32> PlacementTopoOrder = BuildIslandTopoOrder(Islands, IslandEdges, RouteOnlyIslandEdgeIndices, Result);
			if (Result.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount > 0)
			{
				UE_LOG(LogUeAgentInterface, Warning, TEXT("BlueprintLayoutCore residual island cycle after legal route-only feedback edges residual=%d islands=%d island_edges=%d route_only=%d"),
					Result.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount,
					Islands.Num(),
					IslandEdges.Num(),
					RouteOnlyIslandEdgeIndices.Num());
				return {};
			}
			TArray<TArray<int32>> PlacementTopologyColumns;
			PlaceIslandsYByDeterministicTopo(Nodes, Edges, Islands, IslandEdges, Connections, RouteOnlyIslandEdgeIndices, ComponentByIsland, PlacementTopoOrder, Options, Result, PlacementTopologyColumns);
			if (Result.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount > 0)
			{
				UE_LOG(LogUeAgentInterface, Warning, TEXT("BlueprintLayoutCore island placement stopped because no topology-ready island remained residual=%d islands=%d island_edges=%d route_only=%d"),
					Result.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount,
					Islands.Num(),
					IslandEdges.Num(),
					RouteOnlyIslandEdgeIndices.Num());
				return {};
			}
			SnapIslandOffsetsToPixelGrid(Islands);
			ClassifyIslandConnections(Nodes, Edges, Islands, Candidates, RouteOnlyEdgeIndices, Options, Connections, Result, false, false);
			TArray<FDeterministicInterIslandBus> Buses;
			TArray<FDeterministicInterIslandBusBundle> Bundles;
			BuildInterIslandBusesAndBundles(Nodes, Edges, Islands, Connections, Options, TEXT("after_initial_classification"), Buses, Bundles, Result, false);
			SnapIslandOffsetsToPixelGrid(Islands);
			RefreshDeterministicConnectionWorldPoints(Islands, Connections);
			BuildInterIslandBusesAndBundles(Nodes, Edges, Islands, Connections, Options, TEXT("after_initial_snap_refresh"), Buses, Bundles, Result, false);
			const bool bApplyA43TopologyColumnPullIn = true;
			if (bApplyA43TopologyColumnPullIn)
			{
				ApplyTopologyColumnPullIn(Nodes, Islands, Connections, Candidates, Buses, PlacementTopologyColumns, Options, Result);
			}
			SnapIslandOffsetsToPixelGrid(Islands);
			ClassifyIslandConnections(Nodes, Edges, Islands, Candidates, RouteOnlyEdgeIndices, Options, Connections, Result, false, false);
			BuildInterIslandBusesAndBundles(Nodes, Edges, Islands, Connections, Options, TEXT("after_topology_pullin_reclassification"), Buses, Bundles, Result, false);
			ResolveBusConnectionEntriesForCurrentPlacement(Nodes, Edges, Candidates, Islands, Buses, Options, Connections);
			RefreshDeterministicConnectionWorldPoints(Islands, Connections);
			BuildInterIslandBusesAndBundles(Nodes, Edges, Islands, Connections, Options, TEXT("after_entry_resolution"), Buses, Bundles, Result, false);
			const bool bApplyA49PartitionSpread = true;
			const bool bApplyA51IslandRightShift = true;
			PlanInterIslandBusBundleGeometry(Nodes, Edges, Islands, InternalBusGroups, Connections, Buses, Bundles, Options, Result, true, bApplyA49PartitionSpread);
			RefreshDeterministicConnectionWorldPoints(Islands, Connections);
			ResolveBusBranchSlotsAndRightShift(Nodes, Edges, InternalBusGroups, Islands, Connections, Candidates, Buses, IslandEdges, RouteOnlyIslandEdgeIndices, PlacementTopoOrder, Options, Result, true, bApplyA51IslandRightShift);
			SnapIslandOffsetsToPixelGrid(Islands);
			ResolveBusConnectionEntriesForCurrentPlacement(Nodes, Edges, Candidates, Islands, Buses, Options, Connections);
			RefreshDeterministicConnectionWorldPoints(Islands, Connections);
			AppendInterIslandBusBuildSnapshot(Nodes, Edges, Connections, Options, TEXT("after_branch_slot_resolution"), Buses, Bundles, Result, false);
			for (int32 BoundaryBusSidePass = 0; BoundaryBusSidePass < 6; ++BoundaryBusSidePass)
			{
				const bool bBoundaryBusEntryChanged = ResolveBusConnectionEntriesForCurrentPlacement(Nodes, Edges, Candidates, Islands, Buses, Options, Connections);
				if (!bBoundaryBusEntryChanged)
				{
					break;
				}
				RefreshDeterministicConnectionWorldPoints(Islands, Connections);
				AppendInterIslandBusBuildSnapshot(Nodes, Edges, Connections, Options, FString::Printf(TEXT("boundary_bus_side_pass_%d"), BoundaryBusSidePass), Buses, Bundles, Result, false);
			}
			AppendInterIslandBusBuildSnapshot(Nodes, Edges, Connections, Options, TEXT("final_recorded_build"), Buses, Bundles, Result, true);
			RefreshDeterministicConnectionWorldPoints(Islands, Connections);
			PromoteFinalDirectIslandConnections(Nodes, Edges, Islands, RouteOnlyEdgeIndices, Options, Connections, Result);

			TArray<FSelectedEndpointCandidate> SelectedEndpoints;
			SelectedEndpoints.Reserve(Connections.Num());
			auto IsFinalConnectionForwardValid = [&Nodes, &Edges, &Islands, &Buses, &Options](const FDeterministicIslandConnection& Connection) -> bool
			{
				if (!Islands.IsValidIndex(Connection.Endpoint.SourceIsland)
					|| !Islands.IsValidIndex(Connection.Endpoint.TargetIsland)
					|| !Edges.IsValidIndex(Connection.Endpoint.EdgeIndex))
				{
					return true;
				}
				const FIsland& SourceIsland = Islands[Connection.Endpoint.SourceIsland];
				const FIsland& TargetIsland = Islands[Connection.Endpoint.TargetIsland];
				const FVector2D SourcePoint(
					SourceIsland.X + Connection.Endpoint.SourceLocal.X,
					SourceIsland.Y + Connection.Endpoint.SourceLocal.Y);
				const FVector2D ActualTargetPoint(
					TargetIsland.X + Connection.Endpoint.ActualTargetLocal.X,
					TargetIsland.Y + Connection.Endpoint.ActualTargetLocal.Y);
				const FVector2D EntryPoint(
					TargetIsland.X + Connection.Endpoint.EntryLocal.X,
					TargetIsland.Y + Connection.Endpoint.EntryLocal.Y);
				if (Connection.bDirect)
				{
					return HasNonNegativeConnectionAlignmentShorten(SourcePoint, ActualTargetPoint, Options);
				}
				if (Connection.bBus && Buses.IsValidIndex(Connection.BusId))
				{
					const FDeterministicInterIslandBus& Bus = Buses[Connection.BusId];
					const double BranchSpacing = GetCrossBusBranchLaneSpacing(Options);
					const FVector2D SourceTrunkPoint(ComputeInputXAlignedToOutput(SourcePoint.X, Options), Bus.FinalY);
					const FVector2D BranchPoint(
						ComputeOutputXAlignedToInput(ActualTargetPoint.X, Options)
							- FMath::Max(0, Connection.BranchSlotIndex) * BranchSpacing,
						Bus.FinalY);
					FDeterministicIslandConnection VisibleTargetConnection = Connection;
					VisibleTargetConnection.EntryPoint = EntryPoint;
					VisibleTargetConnection.ActualTargetPoint = ActualTargetPoint;
					const FVector2D VisibleTargetPoint = ResolveBusBranchVisibleTargetPoint(VisibleTargetConnection);
					const bool bVisibleTargetIsActualTarget = FVector2D::Distance(VisibleTargetPoint, ActualTargetPoint) <= 0.5;
					const bool bVisibleToTargetValid =
						bVisibleTargetIsActualTarget
						|| ShouldMergeBusVisibleTargetWithActualTarget(VisibleTargetConnection, VisibleTargetPoint, ActualTargetPoint)
						|| HasNonNegativeConnectionAlignmentShorten(VisibleTargetPoint, ActualTargetPoint, Options);
					const TArray<FRect> RouteHardRects = BuildRouteHardOccupancyRects(Nodes, Islands, Options, Edges[Connection.Endpoint.EdgeIndex]);
					const bool bOmitBranchKnot = CanOmitSingleNodeIslandBusBranchKnot(
						TargetIsland,
						Connection,
						SourcePoint,
						SourceTrunkPoint,
						VisibleTargetPoint,
						ActualTargetPoint,
						Options,
						RouteHardRects);
					return HasNonNegativeConnectionAlignmentShorten(SourcePoint, SourceTrunkPoint, Options)
						&& (bOmitBranchKnot
							|| (BranchPoint.X + 0.5 >= SourceTrunkPoint.X
								&& HasNonNegativeConnectionAlignmentShorten(BranchPoint, VisibleTargetPoint, Options)))
						&& bVisibleToTargetValid;
				}
				return IsCandidateForwardValid(SourceIsland, TargetIsland, Connection.Endpoint, Options);
			};
			for (FDeterministicIslandConnection& Connection : Connections)
			{
				FSelectedEndpointCandidate& Selected = Connection.Endpoint;
				if (Connection.bDirect)
				{
					Selected.EntryLocal = Selected.ActualTargetLocal;
				}
				else if (Selected.CandidateId != INDEX_NONE)
				{
					ResolveSelectedEndpointEntry(
						Candidates,
						Islands,
						Options,
						Selected,
						Connection.bBus
							? RequiredBusEntrySourceSpanX(Connection, Buses, Islands, Options)
							: 0.0);
				}
				if (Connection.bBus && Buses.IsValidIndex(Connection.BusId))
				{
					Selected.InterIslandBusId = Connection.BusId;
				}
				if (Islands.IsValidIndex(Selected.SourceIsland) && Islands.IsValidIndex(Selected.TargetIsland))
				{
					const FIsland& SourceIsland = Islands[Selected.SourceIsland];
					const FIsland& TargetIsland = Islands[Selected.TargetIsland];
					if (!Connection.bRouteOnly && !IsFinalConnectionForwardValid(Connection))
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
				SelectedEndpoints.Add(Selected);
			}

			BuildSourceOrderAndFanoutDiagnostics(Edges, SelectedEndpoints, Result);
			Result.DesignDiagnostics.SelectedEndpointCandidateCount = SelectedEndpoints.Num();
			Result.DesignDiagnostics.SourcePortOrderViolationCount = CountSourcePortOrderViolations(
				Islands,
				BuildSourcePortOrderConstraints(Islands, SelectedEndpoints, nullptr));
			for (const FSelectedEndpointCandidate& Selected : SelectedEndpoints)
			{
				AddSelectedEndpointDiagnostics(Result, Selected, Islands, Options);
			}
			TArray<FReroutePlan> Plans = MaterializeFinalDirectInterIslandPlans(Nodes, Edges, Islands, Connections, Options, Result);
			Plans.Append(MaterializeInterIslandBusPlans(Nodes, Edges, Islands, Connections, Buses, Options, Result));
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore deterministic_inter_island_solver_done elapsed_ms=%.1f selected=%d direct=%d buses=%d bundles=%d route_only=%d placement_components=%d plans=%d"),
				(FPlatformTime::Seconds() - SolverStartSeconds) * 1000.0,
				SelectedEndpoints.Num(),
				Result.DesignDiagnostics.DirectIslandConnectionCount,
				Buses.Num(),
				Bundles.Num(),
				RouteOnlyIslandEdgeIndices.Num(),
				Result.DesignDiagnostics.PlacementComponentCount,
				Plans.Num());
			return Plans;
		}
