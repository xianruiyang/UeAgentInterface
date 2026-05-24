// Copyright Epic Games, Inc. All Rights Reserved.
// Anchored column placement, dense packing, input-chain placement, island-local layout.

		static double GetNodeSlotHeight(const TArray<FNode>& Nodes, const TArray<double>* NodeSlotHeights, int32 NodeIndex)
		{
			const auto PixelSafeExtent = [](const double Value)
			{
				return FMath::IsFinite(Value) ? FMath::CeilToDouble(Value) : Value;
			};
			if (NodeSlotHeights && NodeSlotHeights->IsValidIndex(NodeIndex))
			{
				return PixelSafeExtent(FMath::Max(Nodes[NodeIndex].Height, (*NodeSlotHeights)[NodeIndex]));
			}
			return PixelSafeExtent(Nodes[NodeIndex].Height);
		}

		static double GetNodeSlotTopOffset(const TArray<double>* NodeSlotTopOffsets, int32 NodeIndex)
		{
			if (NodeSlotTopOffsets && NodeSlotTopOffsets->IsValidIndex(NodeIndex))
			{
				const double Offset = FMath::Max(0.0, (*NodeSlotTopOffsets)[NodeIndex]);
				return FMath::IsFinite(Offset) ? FMath::CeilToDouble(Offset) : Offset;
			}
			return 0.0;
		}

		static double StackHeight(const TArray<FNode>& Nodes, const TArray<int32>& Column, double Padding, const TArray<double>* NodeSlotHeights = nullptr)
		{
			double Height = 0.0;
			for (int32 Index = 0; Index < Column.Num(); ++Index)
			{
				Height += GetNodeSlotHeight(Nodes, NodeSlotHeights, Column[Index]);
				if (Index + 1 < Column.Num())
				{
					Height += Padding;
				}
			}
			return Height;
		}

		static void PlaceDenseColumn(
			TArray<FNode>& Nodes,
			const TArray<int32>& Column,
			double X,
			double Top,
			double Padding,
			const TArray<double>* NodeSlotHeights = nullptr,
			const TArray<double>* NodeSlotTopOffsets = nullptr)
		{
			double Cursor = SnapGraphCoordinate(Top);
			for (const int32 NodeIndex : Column)
			{
				Nodes[NodeIndex].NewX = X;
				Nodes[NodeIndex].NewY = SnapGraphCoordinate(Cursor + GetNodeSlotTopOffset(NodeSlotTopOffsets, NodeIndex));
				Cursor = FMath::CeilToDouble(Cursor + GetNodeSlotHeight(Nodes, NodeSlotHeights, NodeIndex) + Padding);
			}
		}

		static void PlaceAnchoredIntervals(TArray<FColumnItem>& Items, const double Padding)
		{
			if (Items.Num() == 0)
			{
				return;
			}

			double CompactHeight = 0.0;
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				Items[Index].TempTop = CompactHeight;
				CompactHeight += Items[Index].Height;
				if (Index + 1 < Items.Num())
				{
					CompactHeight += Padding;
				}
			}

			const double Center = CompactHeight * 0.5;
			int32 BaseIndex = 0;
			double BestDistance = TNumericLimits<double>::Max();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				const double ItemCenter = Items[Index].TempTop + Items[Index].Height * 0.5;
				const double Distance = FMath::Abs(ItemCenter - Center);
				if (Distance < BestDistance)
				{
					BestDistance = Distance;
					BaseIndex = Index;
				}
			}
			if (!Items[BaseIndex].bHasPreferred)
			{
				Items[BaseIndex].PreferredTop = Items[BaseIndex].TempTop - Center;
			}
			Items[BaseIndex].FinalTop = SnapGraphCoordinate(Items[BaseIndex].PreferredTop);

			for (int32 Index = BaseIndex - 1; Index >= 0; --Index)
			{
				const double MaxAllowedTop = Items[Index + 1].FinalTop - Padding - Items[Index].Height;
				double CandidateTop = Items[Index].bHasPreferred
					? SnapGraphCoordinate(Items[Index].PreferredTop)
					: FMath::FloorToDouble(MaxAllowedTop);
				if (CandidateTop > MaxAllowedTop)
				{
					CandidateTop = FMath::FloorToDouble(MaxAllowedTop);
				}
				Items[Index].FinalTop = CandidateTop;
			}
			for (int32 Index = BaseIndex + 1; Index < Items.Num(); ++Index)
			{
				const double MinAllowedTop = Items[Index - 1].FinalTop + Items[Index - 1].Height + Padding;
				double CandidateTop = Items[Index].bHasPreferred
					? SnapGraphCoordinate(Items[Index].PreferredTop)
					: FMath::CeilToDouble(MinAllowedTop);
				if (CandidateTop < MinAllowedTop)
				{
					CandidateTop = FMath::CeilToDouble(MinAllowedTop);
				}
				Items[Index].FinalTop = CandidateTop;
			}
		}

		static void PlaceInputChainAnchoredIntervals(TArray<FColumnItem>& Items, const double Padding)
		{
			PlaceAnchoredIntervals(Items, Padding);
		}

		static double InputChainForbiddenZoneHalfHeight(const FOptions& Options)
		{
			return FMath::Clamp(Options.NodePadding * 0.35, 6.0, 12.0);
		}

		static bool IntervalOverlapsForbiddenZone(
			const double Top,
			const double Height,
			const FInputChainForbiddenZone& Zone,
			const double Padding)
		{
			const double Bottom = Top + Height;
			return Top < Zone.Bottom + Padding && Bottom > Zone.Top - Padding;
		}

		static bool IntervalOverlapsAnyForbiddenZone(
			const double Top,
			const double Height,
			const TArray<FInputChainForbiddenZone>& Zones,
			const double Padding)
		{
			for (const FInputChainForbiddenZone& Zone : Zones)
			{
				if (IntervalOverlapsForbiddenZone(Top, Height, Zone, Padding))
				{
					return true;
				}
			}
			return false;
		}

		static bool AnchoredItemsOverlapForbiddenZones(
			const TArray<FColumnItem>& Items,
			const TArray<double>& Tops,
			const TArray<FInputChainForbiddenZone>& Zones,
			const double Padding)
		{
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				if (!Tops.IsValidIndex(Index) || IntervalOverlapsAnyForbiddenZone(Tops[Index], Items[Index].Height, Zones, Padding))
				{
					return true;
				}
			}
			return false;
		}

		static double MoveIntervalDownOutOfForbiddenZones(
			const double Top,
			const double Height,
			const TArray<FInputChainForbiddenZone>& Zones,
			const double Padding)
		{
			double Candidate = SnapGraphCoordinate(Top);
			for (int32 Iteration = 0; Iteration < Zones.Num() + 1; ++Iteration)
			{
				bool bMoved = false;
				for (const FInputChainForbiddenZone& Zone : Zones)
				{
					if (IntervalOverlapsForbiddenZone(Candidate, Height, Zone, Padding))
					{
						Candidate = SnapGraphCoordinate(Zone.Bottom + Padding);
						bMoved = true;
					}
				}
				if (!bMoved)
				{
					break;
				}
			}
			return Candidate;
		}

		static double MoveIntervalUpOutOfForbiddenZones(
			const double Top,
			const double Height,
			const TArray<FInputChainForbiddenZone>& Zones,
			const double Padding)
		{
			double Candidate = SnapGraphCoordinate(Top);
			for (int32 Iteration = 0; Iteration < Zones.Num() + 1; ++Iteration)
			{
				bool bMoved = false;
				for (int32 ZoneIndex = Zones.Num() - 1; ZoneIndex >= 0; --ZoneIndex)
				{
					const FInputChainForbiddenZone& Zone = Zones[ZoneIndex];
					if (IntervalOverlapsForbiddenZone(Candidate, Height, Zone, Padding))
					{
						Candidate = SnapGraphCoordinate(Zone.Top - Padding - Height);
						bMoved = true;
					}
				}
				if (!bMoved)
				{
					break;
				}
			}
			return Candidate;
		}

		static void PlaceAnchoredIntervalsAvoidingForbiddenZones(
			TArray<FColumnItem>& Items,
			const double Padding,
			TArray<FInputChainForbiddenZone> Zones,
			int32* OutAvoidanceCount = nullptr)
		{
			PlaceInputChainAnchoredIntervals(Items, Padding);
			if (Items.Num() == 0 || Zones.Num() == 0)
			{
				return;
			}

			Zones.Sort([](const FInputChainForbiddenZone& A, const FInputChainForbiddenZone& B)
			{
				if (!FMath::IsNearlyEqual(A.Top, B.Top, 0.001))
				{
					return A.Top < B.Top;
				}
				return A.Bottom < B.Bottom;
			});

			TArray<double> OriginalTops;
			OriginalTops.Reserve(Items.Num());
			for (const FColumnItem& Item : Items)
			{
				OriginalTops.Add(Item.FinalTop);
			}

			if (!AnchoredItemsOverlapForbiddenZones(Items, OriginalTops, Zones, Padding))
			{
				return;
			}

			double CompactHeight = 0.0;
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				Items[Index].TempTop = CompactHeight;
				CompactHeight += Items[Index].Height;
				if (Index + 1 < Items.Num())
				{
					CompactHeight += Padding;
				}
			}

			const double Center = CompactHeight * 0.5;
			int32 BaseIndex = 0;
			double BestDistance = TNumericLimits<double>::Max();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				const double ItemCenter = Items[Index].TempTop + Items[Index].Height * 0.5;
				const double Distance = FMath::Abs(ItemCenter - Center);
				if (Distance < BestDistance)
				{
					BestDistance = Distance;
					BaseIndex = Index;
				}
			}
			if (!Items[BaseIndex].bHasPreferred)
			{
				Items[BaseIndex].PreferredTop = Items[BaseIndex].TempTop - Center;
			}

			TArray<double> CandidateTops;
			CandidateTops.SetNum(Items.Num());
			double BaseTop = SnapGraphCoordinate(Items[BaseIndex].PreferredTop);
			if (IntervalOverlapsAnyForbiddenZone(BaseTop, Items[BaseIndex].Height, Zones, Padding))
			{
				const double UpTop = MoveIntervalUpOutOfForbiddenZones(BaseTop, Items[BaseIndex].Height, Zones, Padding);
				const double DownTop = MoveIntervalDownOutOfForbiddenZones(BaseTop, Items[BaseIndex].Height, Zones, Padding);
				BaseTop = FMath::Abs(UpTop - BaseTop) <= FMath::Abs(DownTop - BaseTop) ? UpTop : DownTop;
			}
			CandidateTops[BaseIndex] = BaseTop;
			if (Items.Num() == 1)
			{
				Items[0].FinalTop = BaseTop;
				if (OutAvoidanceCount && OriginalTops.IsValidIndex(0) && !FMath::IsNearlyEqual(OriginalTops[0], Items[0].FinalTop, 0.5))
				{
					++(*OutAvoidanceCount);
				}
				return;
			}

			for (int32 Index = BaseIndex - 1; Index >= 0; --Index)
			{
				const double MaxAllowedTop = CandidateTops[Index + 1] - Padding - Items[Index].Height;
				double CandidateTop = Items[Index].bHasPreferred
					? SnapGraphCoordinate(Items[Index].PreferredTop)
					: FMath::FloorToDouble(MaxAllowedTop);
				if (CandidateTop > MaxAllowedTop)
				{
					CandidateTop = FMath::FloorToDouble(MaxAllowedTop);
				}
				CandidateTop = MoveIntervalUpOutOfForbiddenZones(CandidateTop, Items[Index].Height, Zones, Padding);
				if (CandidateTop > MaxAllowedTop)
				{
					CandidateTop = MoveIntervalUpOutOfForbiddenZones(FMath::FloorToDouble(MaxAllowedTop), Items[Index].Height, Zones, Padding);
				}
				CandidateTops[Index] = CandidateTop;
			}
			for (int32 Index = BaseIndex + 1; Index < Items.Num(); ++Index)
			{
				const double MinAllowedTop = CandidateTops[Index - 1] + Items[Index - 1].Height + Padding;
				double CandidateTop = Items[Index].bHasPreferred
					? SnapGraphCoordinate(Items[Index].PreferredTop)
					: FMath::CeilToDouble(MinAllowedTop);
				if (CandidateTop < MinAllowedTop)
				{
					CandidateTop = FMath::CeilToDouble(MinAllowedTop);
				}
				CandidateTop = MoveIntervalDownOutOfForbiddenZones(CandidateTop, Items[Index].Height, Zones, Padding);
				if (CandidateTop < MinAllowedTop)
				{
					CandidateTop = MoveIntervalDownOutOfForbiddenZones(FMath::CeilToDouble(MinAllowedTop), Items[Index].Height, Zones, Padding);
				}
				CandidateTops[Index] = CandidateTop;
			}

			const bool bCandidateValid = [&]()
			{
				if (AnchoredItemsOverlapForbiddenZones(Items, CandidateTops, Zones, Padding))
				{
					return false;
				}
				for (int32 Index = 1; Index < Items.Num(); ++Index)
				{
					if (CandidateTops[Index] < CandidateTops[Index - 1] + Items[Index - 1].Height + Padding - 0.5)
					{
						return false;
					}
				}
				return true;
			}();

			if (bCandidateValid)
			{
				for (int32 Index = 0; Index < Items.Num(); ++Index)
				{
					Items[Index].FinalTop = CandidateTops[Index];
				}
				if (OutAvoidanceCount)
				{
					for (int32 Index = 0; Index < Items.Num(); ++Index)
					{
						if (OriginalTops.IsValidIndex(Index) && !FMath::IsNearlyEqual(OriginalTops[Index], Items[Index].FinalTop, 0.5))
						{
							++(*OutAvoidanceCount);
						}
					}
				}
				return;
			}

			return;
		}

		static bool ComputeAnchoredPreferredSlotTopForNode(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TSet<int32>& KnownSet,
			const int32 NodeIndex,
			const TArray<double>* NodeSlotTopOffsets,
			double& OutPreferredTop,
			int32* OutConnectionCount = nullptr)
		{
			if (!Nodes.IsValidIndex(NodeIndex))
			{
				return false;
			}

			TArray<double> KnownYs;
			TArray<double> SelfOffsets;
			const double SlotTopOffset = GetNodeSlotTopOffset(NodeSlotTopOffsets, NodeIndex);
			for (const FEdge& Edge : Edges)
			{
				if (KnownSet.Contains(Edge.Source) && Edge.Target == NodeIndex && Nodes.IsValidIndex(Edge.Source))
				{
					KnownYs.Add(GetSourcePort(Nodes[Edge.Source], Edge, true).Y);
					SelfOffsets.Add(SlotTopOffset + GetInputAnchor(Nodes[NodeIndex], Edge.TargetPinIndex).Y);
				}
				else if (KnownSet.Contains(Edge.Target) && Edge.Source == NodeIndex && Nodes.IsValidIndex(Edge.Target))
				{
					KnownYs.Add(GetTargetPort(Nodes[Edge.Target], Edge, true).Y);
					SelfOffsets.Add(SlotTopOffset + GetOutputAnchor(Nodes[NodeIndex], Edge.SourcePinIndex).Y);
				}
			}
			if (OutConnectionCount)
			{
				*OutConnectionCount = KnownYs.Num();
			}
			if (KnownYs.Num() == 0 || SelfOffsets.Num() == 0)
			{
				return false;
			}

			KnownYs.Sort();
			SelfOffsets.Sort();
			const double KnownAnchor = (KnownYs[0] + KnownYs.Last()) * 0.5;
			const double SelfAnchorOffset = (SelfOffsets[0] + SelfOffsets.Last()) * 0.5;
			OutPreferredTop = KnownAnchor - SelfAnchorOffset;
			return FMath::IsFinite(OutPreferredTop);
		}

		struct FWeightedKnownSet
		{
			int32 ColumnIndex = INDEX_NONE;
			double Weight = 1.0;
			TSet<int32> Nodes;
		};

		static void AddWeightedKnownSetForColumn(
			const TArray<TArray<int32>>& Columns,
			const int32 ColumnIndex,
			const double Weight,
			TArray<FWeightedKnownSet>& OutKnownSets)
		{
			if (!Columns.IsValidIndex(ColumnIndex))
			{
				return;
			}
			FWeightedKnownSet KnownSet;
			KnownSet.ColumnIndex = ColumnIndex;
			KnownSet.Weight = FMath::Max(0.0, Weight);
			for (const int32 NodeIndex : Columns[ColumnIndex])
			{
				KnownSet.Nodes.Add(NodeIndex);
			}
			if (KnownSet.Nodes.Num() > 0 && KnownSet.Weight > 0.0)
			{
				OutKnownSets.Add(MoveTemp(KnownSet));
			}
		}

		static TArray<TSharedPtr<FJsonValue>> WeightedKnownSetColumnsToJson(const TArray<FWeightedKnownSet>& KnownSets)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FWeightedKnownSet& KnownSet : KnownSets)
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetNumberField(TEXT("column_index"), KnownSet.ColumnIndex);
				Obj->SetNumberField(TEXT("weight"), KnownSet.Weight);
				Obj->SetNumberField(TEXT("node_count"), KnownSet.Nodes.Num());
				Values.Add(MakeShared<FJsonValueObject>(Obj));
			}
			return Values;
		}

		static bool ComputeWeightedAnchoredPreferredSlotTopForNode(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FWeightedKnownSet>& KnownSets,
			const int32 NodeIndex,
			const TArray<double>* NodeSlotTopOffsets,
			double& OutPreferredTop,
			int32* OutKnownGroupCount = nullptr,
			int32* OutConnectionCount = nullptr)
		{
			double WeightedTopSum = 0.0;
			double WeightSum = 0.0;
			int32 KnownGroupCount = 0;
			int32 ConnectionCount = 0;
			for (const FWeightedKnownSet& KnownSet : KnownSets)
			{
				double GroupPreferredTop = 0.0;
				int32 GroupConnectionCount = 0;
				if (!ComputeAnchoredPreferredSlotTopForNode(Nodes, Edges, KnownSet.Nodes, NodeIndex, NodeSlotTopOffsets, GroupPreferredTop, &GroupConnectionCount))
				{
					continue;
				}
				const double Weight = FMath::Max(0.0, KnownSet.Weight);
				if (Weight <= 0.0)
				{
					continue;
				}
				WeightedTopSum += GroupPreferredTop * Weight;
				WeightSum += Weight;
				++KnownGroupCount;
				ConnectionCount += GroupConnectionCount;
			}
			if (OutKnownGroupCount)
			{
				*OutKnownGroupCount = KnownGroupCount;
			}
			if (OutConnectionCount)
			{
				*OutConnectionCount = ConnectionCount;
			}
			if (WeightSum <= 0.0)
			{
				return false;
			}
			OutPreferredTop = WeightedTopSum / WeightSum;
			return FMath::IsFinite(OutPreferredTop);
		}

		static double InputChainTargetGroupGap(const FOptions& Options)
		{
			return FMath::Clamp(Options.NodePadding * 0.125, 0.0, 6.0);
		}

		static FRect MakeNodeRectAt(const FNode& Node, const FVector2D& Position)
		{
			FRect Bounds;
			Bounds.Left = Position.X;
			Bounds.Top = Position.Y;
			Bounds.Right = Position.X + Node.Width;
			Bounds.Bottom = Position.Y + Node.Height;
			return Bounds;
		}

		static FRect OffsetRect(const FRect& Rect, double DeltaX, double DeltaY)
		{
			FRect Out = Rect;
			Out.Left += DeltaX;
			Out.Right += DeltaX;
			Out.Top += DeltaY;
			Out.Bottom += DeltaY;
			return Out;
		}

		static void UnionRect(FRect& Bounds, bool& bAny, const FRect& Rect)
		{
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

		static TMap<int32, TArray<int32>> BuildInputChainsByTargetNode(const FIsland& Island)
		{
			TMap<int32, TArray<int32>> ChainsByTargetNode;
			for (int32 ChainIndex = 0; ChainIndex < Island.InputChains.Num(); ++ChainIndex)
			{
				const FInputChain& Chain = Island.InputChains[ChainIndex];
				ChainsByTargetNode.FindOrAdd(Chain.TargetNode).Add(ChainIndex);
			}
			return ChainsByTargetNode;
		}

		static TSet<int32> BuildInputChainEdgeSet(const FIsland& Island)
		{
			TSet<int32> EdgeSet;
			for (const FInputChain& Chain : Island.InputChains)
			{
				if (Chain.TargetEdgeIndex != INDEX_NONE)
				{
					EdgeSet.Add(Chain.TargetEdgeIndex);
				}
				for (const int32 EdgeIndex : Chain.EdgeIndices)
				{
					if (EdgeIndex != INDEX_NONE)
					{
						EdgeSet.Add(EdgeIndex);
					}
				}
			}
			return EdgeSet;
		}

		static TArray<FInputChainForbiddenZone> BuildInputChainForbiddenZonesForTarget(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& AllEdges,
			const FIsland& Island,
			const int32 TargetNode,
			const FVector2D& TargetPosition,
			const FOptions& Options)
		{
			TArray<FInputChainForbiddenZone> Zones;
			if (!Nodes.IsValidIndex(TargetNode))
			{
				return Zones;
			}
			const TSet<int32> InputChainEdges = BuildInputChainEdgeSet(Island);
			TSet<FString> UsedPinKeys;
			const double HalfHeight = InputChainForbiddenZoneHalfHeight(Options);
			for (int32 EdgeIndex = 0; EdgeIndex < AllEdges.Num(); ++EdgeIndex)
			{
				if (InputChainEdges.Contains(EdgeIndex))
				{
					continue;
				}
				const FEdge& Edge = AllEdges[EdgeIndex];
				if (Edge.Target == TargetNode && Edge.TargetPinIndex != INDEX_NONE)
				{
					const FString PinKey = FString::Printf(TEXT("I:%d"), Edge.TargetPinIndex);
					if (UsedPinKeys.Contains(PinKey))
					{
						continue;
					}
					UsedPinKeys.Add(PinKey);
					const double PinY = TargetPosition.Y + GetInputAnchor(Nodes[TargetNode], Edge.TargetPinIndex).Y;
					FInputChainForbiddenZone Zone;
					Zone.Top = PinY - HalfHeight;
					Zone.Bottom = PinY + HalfHeight;
					Zone.EdgeIndex = EdgeIndex;
					Zone.PinIndex = Edge.TargetPinIndex;
					Zone.bOutputPin = false;
					Zones.Add(Zone);
				}
			}
			Zones.Sort([](const FInputChainForbiddenZone& A, const FInputChainForbiddenZone& B)
			{
				if (!FMath::IsNearlyEqual(A.Top, B.Top, 0.001))
				{
					return A.Top < B.Top;
				}
				return A.Bottom < B.Bottom;
			});
			return Zones;
		}

		static FRect EstimateAttachedInputChainsRelativeToTarget(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& AllEdges,
			const FIsland& Island,
			const TArray<int32>& ChainIndices,
			const TMap<int32, TArray<int32>>& ChainsByTargetNode,
			const FOptions& Options,
			const double Padding)
		{
			TFunction<FRect(int32, const FVector2D&, TSet<int32>&)> EstimateTargetGroup;
			TFunction<FRect(int32, const FVector2D&, TSet<int32>&)> EstimateChainSubtree;

			EstimateChainSubtree = [&](int32 ChainIndex, const FVector2D& TargetPosition, TSet<int32>& VisitingChains) -> FRect
			{
				FRect Bounds;
				bool bAny = false;
				if (!Island.InputChains.IsValidIndex(ChainIndex) || VisitingChains.Contains(ChainIndex))
				{
					return Bounds;
				}
				const FInputChain& Chain = Island.InputChains[ChainIndex];
				if (!Nodes.IsValidIndex(Chain.TargetNode)
					|| !AllEdges.IsValidIndex(Chain.TargetEdgeIndex)
					|| Chain.Nodes.Num() == 0)
				{
					return Bounds;
				}
				VisitingChains.Add(ChainIndex);
				const FEdge& TargetEdge = AllEdges[Chain.TargetEdgeIndex];
				FVector2D NextInputPort = TargetPosition + GetInputAnchor(Nodes[Chain.TargetNode], TargetEdge.TargetPinIndex);
				for (int32 ChainNodePosition = Chain.Nodes.Num() - 1; ChainNodePosition >= 0; --ChainNodePosition)
				{
					if (!Chain.EdgeIndices.IsValidIndex(ChainNodePosition))
					{
						continue;
					}
					const int32 NodeIndex = Chain.Nodes[ChainNodePosition];
					if (!Nodes.IsValidIndex(NodeIndex))
					{
						continue;
					}
					const FEdge& EdgeToNext = AllEdges[Chain.EdgeIndices[ChainNodePosition]];
					const FVector2D OutputAnchor = GetOutputAnchor(Nodes[NodeIndex], EdgeToNext.SourcePinIndex);
					const FVector2D NodePosition(
						ComputeOutputXAlignedToInput(NextInputPort.X, Options) - OutputAnchor.X,
						NextInputPort.Y - OutputAnchor.Y);
					UnionRect(Bounds, bAny, MakeNodeRectAt(Nodes[NodeIndex], NodePosition));
					if (ChainsByTargetNode.Contains(NodeIndex))
					{
						const FRect ChildBounds = EstimateTargetGroup(NodeIndex, NodePosition, VisitingChains);
						UnionRect(Bounds, bAny, ChildBounds);
					}
					if (ChainNodePosition > 0 && Chain.EdgeIndices.IsValidIndex(ChainNodePosition - 1))
					{
						const FEdge& PreviousEdge = AllEdges[Chain.EdgeIndices[ChainNodePosition - 1]];
						NextInputPort = NodePosition + GetInputAnchor(Nodes[NodeIndex], PreviousEdge.TargetPinIndex);
					}
				}
				VisitingChains.Remove(ChainIndex);
				return Bounds;
			};

			EstimateTargetGroup = [&](int32 TargetNode, const FVector2D& TargetPosition, TSet<int32>& VisitingChains) -> FRect
			{
				FRect Bounds;
				bool bAny = false;
				const TArray<int32>* TargetChains = ChainsByTargetNode.Find(TargetNode);
				if (!TargetChains)
				{
					return Bounds;
				}
				TArray<int32> SortedChains = *TargetChains;
				SortedChains.Sort([&Island](int32 A, int32 B)
				{
					const FInputChain& ChainA = Island.InputChains[A];
					const FInputChain& ChainB = Island.InputChains[B];
					if (ChainA.TargetPinIndex != ChainB.TargetPinIndex)
					{
						return ChainA.TargetPinIndex < ChainB.TargetPinIndex;
					}
					return ChainA.Id < ChainB.Id;
				});
				TArray<FRect> ChainBounds;
				TArray<FColumnItem> Items;
				for (const int32 ChainIndex : SortedChains)
				{
					const FRect SubtreeBounds = EstimateChainSubtree(ChainIndex, TargetPosition, VisitingChains);
					FColumnItem Item;
					Item.NodeIndex = ChainIndex;
					Item.Height = SubtreeBounds.Height();
					Item.PreferredTop = SubtreeBounds.Top;
					Item.bHasPreferred = true;
					Items.Add(Item);
					ChainBounds.Add(SubtreeBounds);
				}
				const TArray<FInputChainForbiddenZone> Zones = BuildInputChainForbiddenZonesForTarget(
					Nodes,
					AllEdges,
					Island,
					TargetNode,
					TargetPosition,
					Options);
				PlaceAnchoredIntervalsAvoidingForbiddenZones(Items, Padding, Zones);
				for (int32 ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
				{
					const FColumnItem& Item = Items[ItemIndex];
					if (!ChainBounds.IsValidIndex(ItemIndex))
					{
						continue;
					}
					const FRect ShiftedBounds = OffsetRect(ChainBounds[ItemIndex], 0.0, Item.FinalTop - ChainBounds[ItemIndex].Top);
					UnionRect(Bounds, bAny, ShiftedBounds);
				}
				return Bounds;
			};

			TSet<int32> VisitingChains;
			TArray<FColumnItem> Items;
			TArray<FRect> ItemBounds;
			for (const int32 ChainIndex : ChainIndices)
			{
				FRect ChainBounds = EstimateChainSubtree(ChainIndex, FVector2D::ZeroVector, VisitingChains);
				FColumnItem Item;
				Item.NodeIndex = ChainIndex;
				Item.Height = ChainBounds.Height();
				Item.PreferredTop = ChainBounds.Top;
				Item.bHasPreferred = true;
				Items.Add(Item);
				ItemBounds.Add(ChainBounds);
			}
			int32 TargetNode = INDEX_NONE;
			if (ChainIndices.Num() > 0 && Island.InputChains.IsValidIndex(ChainIndices[0]))
			{
				TargetNode = Island.InputChains[ChainIndices[0]].TargetNode;
			}
			const TArray<FInputChainForbiddenZone> Zones = BuildInputChainForbiddenZonesForTarget(
				Nodes,
				AllEdges,
				Island,
				TargetNode,
				FVector2D::ZeroVector,
				Options);
			PlaceAnchoredIntervalsAvoidingForbiddenZones(Items, Padding, Zones);
			FRect Bounds;
			bool bAny = false;
			for (int32 ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
			{
				if (!ItemBounds.IsValidIndex(ItemIndex))
				{
					continue;
				}
				const FRect ShiftedBounds = OffsetRect(ItemBounds[ItemIndex], 0.0, Items[ItemIndex].FinalTop - ItemBounds[ItemIndex].Top);
				UnionRect(Bounds, bAny, ShiftedBounds);
			}
			return Bounds;
		}

		static void AppendAnchoredColumnItemDiagnostic(
			TArray<TSharedPtr<FJsonValue>>* OutDiagnostics,
			const FString& Stage,
			const int32 IslandId,
			const int32 TargetColumnIndex,
			const TArray<FWeightedKnownSet>& KnownSets,
			const FColumnItem& Item,
			const TArray<FNode>& Nodes,
			const int32 KnownGroupCount,
			const int32 ConnectionCount)
		{
			if (!OutDiagnostics || !Nodes.IsValidIndex(Item.NodeIndex))
			{
				return;
			}
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("stage"), Stage);
			Obj->SetNumberField(TEXT("island_id"), IslandId);
			Obj->SetNumberField(TEXT("target_column"), TargetColumnIndex);
			Obj->SetNumberField(TEXT("node_index"), Item.NodeIndex);
			Obj->SetStringField(TEXT("node_title"), Nodes[Item.NodeIndex].Title);
			Obj->SetBoolField(TEXT("has_preferred"), Item.bHasPreferred);
			Obj->SetNumberField(TEXT("preferred_top"), Item.PreferredTop);
			Obj->SetNumberField(TEXT("final_top"), Item.FinalTop);
			Obj->SetNumberField(TEXT("residual"), Item.bHasPreferred ? FMath::Abs(Item.FinalTop - Item.PreferredTop) : 0.0);
			Obj->SetNumberField(TEXT("known_group_count"), KnownGroupCount);
			Obj->SetNumberField(TEXT("known_connection_count"), ConnectionCount);
			Obj->SetArrayField(TEXT("known_columns"), WeightedKnownSetColumnsToJson(KnownSets));
			OutDiagnostics->Add(MakeShared<FJsonValueObject>(Obj));
		}

		static void PlaceAnchoredColumnByKnownSets(
			TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FWeightedKnownSet>& KnownSets,
			const TArray<int32>& TargetColumn,
			double X,
			double Padding,
			FDesignDiagnostics* Diagnostics,
			const TArray<double>* NodeSlotHeights = nullptr,
			const TArray<double>* NodeSlotTopOffsets = nullptr,
			const FString& Stage = TEXT("initial"),
			const int32 IslandId = INDEX_NONE,
			const int32 TargetColumnIndex = INDEX_NONE,
			TArray<TSharedPtr<FJsonValue>>* OutItemDiagnostics = nullptr)
		{
			if (TargetColumn.Num() == 0)
			{
				return;
			}
			TArray<FColumnItem> Items;
			TArray<int32> KnownGroupCounts;
			TArray<int32> ConnectionCounts;
			double Temp = 0.0;
			for (const int32 NodeIndex : TargetColumn)
			{
				FColumnItem Item;
				Item.NodeIndex = NodeIndex;
				Item.TempTop = Temp;
				Item.Height = GetNodeSlotHeight(Nodes, NodeSlotHeights, NodeIndex);
				Temp += Item.Height + Padding;
				int32 KnownGroupCount = 0;
				int32 ConnectionCount = 0;
				if (ComputeWeightedAnchoredPreferredSlotTopForNode(Nodes, Edges, KnownSets, NodeIndex, NodeSlotTopOffsets, Item.PreferredTop, &KnownGroupCount, &ConnectionCount))
				{
					Item.bHasPreferred = true;
				}
				else if (Diagnostics)
				{
					++Diagnostics->AnchoredColumnUnconnectedItemCount;
				}
				Items.Add(Item);
				KnownGroupCounts.Add(KnownGroupCount);
				ConnectionCounts.Add(ConnectionCount);
			}

			PlaceAnchoredIntervals(Items, Padding);
			for (int32 ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
			{
				const FColumnItem& Item = Items[ItemIndex];
				if (!Nodes.IsValidIndex(Item.NodeIndex))
				{
					continue;
				}
				Nodes[Item.NodeIndex].NewX = X;
				Nodes[Item.NodeIndex].NewY = Item.FinalTop + GetNodeSlotTopOffset(NodeSlotTopOffsets, Item.NodeIndex);
				AppendAnchoredColumnItemDiagnostic(
					OutItemDiagnostics,
					Stage,
					IslandId,
					TargetColumnIndex,
					KnownSets,
					Item,
					Nodes,
					KnownGroupCounts.IsValidIndex(ItemIndex) ? KnownGroupCounts[ItemIndex] : 0,
					ConnectionCounts.IsValidIndex(ItemIndex) ? ConnectionCounts[ItemIndex] : 0);
			}
			if (Diagnostics)
			{
				++Diagnostics->AnchoredColumnPlacementCount;
				for (const FColumnItem& Item : Items)
				{
					if (!Item.bHasPreferred || !Nodes.IsValidIndex(Item.NodeIndex))
					{
						continue;
					}
					const double Residual = FMath::Abs(Item.FinalTop - Item.PreferredTop);
					Diagnostics->AnchoredColumnTotalPinResidual += Residual;
					Diagnostics->AnchoredColumnMaxPinResidual = FMath::Max(Diagnostics->AnchoredColumnMaxPinResidual, Residual);
					Diagnostics->IslandInternalTotalPinResidual += Residual;
					Diagnostics->IslandInternalMaxPinResidual = FMath::Max(Diagnostics->IslandInternalMaxPinResidual, Residual);
				}
			}
		}

		static void PlaceAnchoredColumn(
			TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<int32>& KnownColumn,
			const TArray<int32>& TargetColumn,
			double X,
			double Padding,
			FDesignDiagnostics* Diagnostics,
			const TArray<double>* NodeSlotHeights = nullptr,
			const TArray<double>* NodeSlotTopOffsets = nullptr)
		{
			TArray<FWeightedKnownSet> KnownSets;
			FWeightedKnownSet KnownSet;
			KnownSet.ColumnIndex = INDEX_NONE;
			KnownSet.Weight = 1.0;
			for (const int32 NodeIndex : KnownColumn)
			{
				KnownSet.Nodes.Add(NodeIndex);
			}
			if (KnownSet.Nodes.Num() > 0)
			{
				KnownSets.Add(MoveTemp(KnownSet));
			}
			PlaceAnchoredColumnByKnownSets(Nodes, Edges, KnownSets, TargetColumn, X, Padding, Diagnostics, NodeSlotHeights, NodeSlotTopOffsets);
		}

		static double EstimateInterfaceRowSpacingForColumnCorridor(
			const TArray<FNode>& Nodes,
			const TArray<TArray<int32>>& Columns,
			const int32 CorridorIndex,
			const FOptions& Options)
		{
			TArray<int32> CorridorNodes;
			if (Columns.IsValidIndex(CorridorIndex))
			{
				CorridorNodes.Append(Columns[CorridorIndex]);
			}
			if (Columns.IsValidIndex(CorridorIndex + 1))
			{
				CorridorNodes.Append(Columns[CorridorIndex + 1]);
			}
			return EstimateInterfaceRowSpacingForNodes(Nodes, CorridorNodes, Options);
		}

		static double ComputeCompactIslandColumnGap(
			const TArray<FNode>& Nodes,
			const TArray<TArray<int32>>& Columns,
			const int32 CorridorIndex,
			const int32 BusLaneDemand,
			const double InputChainWidthDemand,
			const FOptions& Options,
			double* OutBaseGap = nullptr,
			double* OutBusLanePitch = nullptr,
			double* OutBusGapDemand = nullptr)
		{
			const double EndpointClearance = SnapGraphCoordinate(EndpointAccessClearanceX(Options));
			const double BranchXAvoidanceDistance = SnapGraphCoordinate(FMath::Max(
				EstimateInterfaceRowSpacingForColumnCorridor(Nodes, Columns, CorridorIndex, Options),
				EndpointClearance * 0.5));
			const double BusGapDemand = SnapGraphCoordinate(FMath::Min(
				EndpointClearance,
				BranchXAvoidanceDistance * (1.0 + static_cast<double>(FMath::Max(0, BusLaneDemand)))));
			if (OutBaseGap)
			{
				*OutBaseGap = EndpointClearance;
			}
			if (OutBusLanePitch)
			{
				*OutBusLanePitch = BranchXAvoidanceDistance;
			}
			if (OutBusGapDemand)
			{
				*OutBusGapDemand = BusGapDemand;
			}
			return SnapGraphCoordinate(FMath::Max(BusGapDemand, InputChainWidthDemand));
		}

		static FRect BoundsForInputChainSubtree(
			const TArray<FNode>& Nodes,
			const FIsland& Island,
			const TMap<int32, TArray<int32>>& ChainsByTargetNode,
			const int32 ChainIndex,
			TSet<int32>& VisitingChains)
		{
			FRect Bounds;
			bool bAny = false;
			if (!Island.InputChains.IsValidIndex(ChainIndex) || VisitingChains.Contains(ChainIndex))
			{
				return Bounds;
			}
			VisitingChains.Add(ChainIndex);
			const FInputChain& Chain = Island.InputChains[ChainIndex];
			for (const int32 NodeIndex : Chain.Nodes)
			{
				if (!Nodes.IsValidIndex(NodeIndex))
				{
					continue;
				}
				UnionRect(Bounds, bAny, MakeNodeRect(Nodes[NodeIndex], true));
				if (const TArray<int32>* ChildChains = ChainsByTargetNode.Find(NodeIndex))
				{
					for (const int32 ChildChainIndex : *ChildChains)
					{
						const FRect ChildBounds = BoundsForInputChainSubtree(Nodes, Island, ChainsByTargetNode, ChildChainIndex, VisitingChains);
						UnionRect(Bounds, bAny, ChildBounds);
					}
				}
			}
			VisitingChains.Remove(ChainIndex);
			return Bounds;
		}

		static void OffsetInputChainSubtree(
			TArray<FNode>& Nodes,
			FIsland& Island,
			const TMap<int32, TArray<int32>>& ChainsByTargetNode,
			const int32 ChainIndex,
			const double DeltaY,
			TSet<int32>& VisitingChains)
		{
			if (!Island.InputChains.IsValidIndex(ChainIndex) || VisitingChains.Contains(ChainIndex))
			{
				return;
			}
			VisitingChains.Add(ChainIndex);
			FInputChain& Chain = Island.InputChains[ChainIndex];
			for (const int32 NodeIndex : Chain.Nodes)
			{
				if (Nodes.IsValidIndex(NodeIndex))
				{
					Nodes[NodeIndex].NewY += DeltaY;
				}
				if (const TArray<int32>* ChildChains = ChainsByTargetNode.Find(NodeIndex))
				{
					for (const int32 ChildChainIndex : *ChildChains)
					{
						OffsetInputChainSubtree(Nodes, Island, ChainsByTargetNode, ChildChainIndex, DeltaY, VisitingChains);
					}
				}
			}
			Chain.LocalBounds.Top += DeltaY;
			Chain.LocalBounds.Bottom += DeltaY;
			for (FRect& Segment : Chain.SegmentBounds)
			{
				Segment.Top += DeltaY;
				Segment.Bottom += DeltaY;
			}
			Chain.PreferredCenterY += DeltaY;
			VisitingChains.Remove(ChainIndex);
		}

		static void MarkInputChainSubtreeGroup(
			TArray<FNode>& Nodes,
			FIsland& Island,
			const TMap<int32, TArray<int32>>& ChainsByTargetNode,
			const int32 ChainIndex,
			const int32 MetricsGroupKey,
			const double Padding,
			TSet<int32>& VisitingChains)
		{
			if (!Island.InputChains.IsValidIndex(ChainIndex) || VisitingChains.Contains(ChainIndex))
			{
				return;
			}
			VisitingChains.Add(ChainIndex);
			const FInputChain& Chain = Island.InputChains[ChainIndex];
			for (const int32 NodeIndex : Chain.Nodes)
			{
				if (Nodes.IsValidIndex(NodeIndex))
				{
					Nodes[NodeIndex].InputChainTargetGroupKey = MetricsGroupKey;
					Nodes[NodeIndex].InputChainTargetGroupPadding = Padding;
				}
				if (const TArray<int32>* ChildChains = ChainsByTargetNode.Find(NodeIndex))
				{
					for (const int32 ChildChainIndex : *ChildChains)
					{
						MarkInputChainSubtreeGroup(Nodes, Island, ChainsByTargetNode, ChildChainIndex, MetricsGroupKey, Padding, VisitingChains);
					}
				}
			}
			VisitingChains.Remove(ChainIndex);
		}

		static void PlaceInputChains(
			TArray<FNode>& Nodes,
			const TArray<FEdge>& AllEdges,
			FIsland& Island,
			const FOptions& Options,
			const TArray<double>& NodeSlotHeights,
			FRunResult& Result)
		{
			(void)NodeSlotHeights;
			if (Island.InputChains.Num() == 0)
			{
				return;
			}

			struct FTargetChainGroup
			{
				int32 Id = INDEX_NONE;
				int32 TargetNode = INDEX_NONE;
				int32 CorridorIndex = INDEX_NONE;
				TArray<int32> ChainIndices;
				double PreferredCenterY = 0.0;
				FRect Bounds;
			};

			const TMap<int32, TArray<int32>> ChainsByTargetNode = BuildInputChainsByTargetNode(Island);
			TArray<int32> TargetGroupByChain;
			TargetGroupByChain.Init(INDEX_NONE, Island.InputChains.Num());
			TArray<FTargetChainGroup> TargetGroups;
			TMap<FString, int32> TargetGroupByKey;
			for (int32 ChainIndex = 0; ChainIndex < Island.InputChains.Num(); ++ChainIndex)
			{
				const FInputChain& Chain = Island.InputChains[ChainIndex];
				if (!Nodes.IsValidIndex(Chain.TargetNode))
				{
					continue;
				}
				const FString GroupKey = FString::Printf(TEXT("%d|%d"), Chain.TargetNode, Chain.CorridorIndex);
				int32 GroupIndex = INDEX_NONE;
				if (const int32* ExistingGroup = TargetGroupByKey.Find(GroupKey))
				{
					GroupIndex = *ExistingGroup;
				}
				else
				{
					FTargetChainGroup Group;
					Group.Id = TargetGroups.Num();
					Group.TargetNode = Chain.TargetNode;
					Group.CorridorIndex = Chain.CorridorIndex;
					GroupIndex = Group.Id;
					TargetGroupByKey.Add(GroupKey, GroupIndex);
					TargetGroups.Add(Group);
				}
				TargetGroups[GroupIndex].ChainIndices.Add(ChainIndex);
				TargetGroupByChain[ChainIndex] = GroupIndex;
			}

			const double IntraTargetChainGap = InputChainTargetGroupGap(Options);
			TSet<int32> PlacedTargets;
			TSet<int32> ActiveTargets;
			TSet<int32> ActiveChains;

			TFunction<void(int32)> PlaceAttachedChainsForTarget;
			TFunction<void(int32)> PlaceChainSubtree;

			PlaceChainSubtree = [&](const int32 ChainIndex)
			{
				if (!Island.InputChains.IsValidIndex(ChainIndex) || ActiveChains.Contains(ChainIndex))
				{
					return;
				}
				FInputChain& Chain = Island.InputChains[ChainIndex];
				if (!Nodes.IsValidIndex(Chain.TargetNode) || !AllEdges.IsValidIndex(Chain.TargetEdgeIndex) || Chain.Nodes.Num() == 0)
				{
					return;
				}
				ActiveChains.Add(ChainIndex);
				const FEdge& TargetEdge = AllEdges[Chain.TargetEdgeIndex];
				FVector2D NextInputPort = GetTargetPort(Nodes[Chain.TargetNode], TargetEdge, true);
				Chain.PreferredCenterY = NextInputPort.Y;
				for (int32 ChainNodePosition = Chain.Nodes.Num() - 1; ChainNodePosition >= 0; --ChainNodePosition)
				{
					if (!Chain.EdgeIndices.IsValidIndex(ChainNodePosition))
					{
						continue;
					}
					const int32 NodeIndex = Chain.Nodes[ChainNodePosition];
					if (!Nodes.IsValidIndex(NodeIndex))
					{
						continue;
					}
					const FEdge& EdgeToNext = AllEdges[Chain.EdgeIndices[ChainNodePosition]];
					const FVector2D OutputAnchor = GetOutputAnchor(Nodes[NodeIndex], EdgeToNext.SourcePinIndex);
					Nodes[NodeIndex].NewX = ComputeOutputXAlignedToInput(NextInputPort.X, Options) - OutputAnchor.X;
					Nodes[NodeIndex].NewY = NextInputPort.Y - OutputAnchor.Y;
					Nodes[NodeIndex].Rank = FMath::Max(0, Chain.TargetRank - 1);
					Nodes[NodeIndex].Order = Nodes[Chain.TargetNode].Order;
					PlaceAttachedChainsForTarget(NodeIndex);
					if (ChainNodePosition > 0 && Chain.EdgeIndices.IsValidIndex(ChainNodePosition - 1))
					{
						const FEdge& PreviousEdge = AllEdges[Chain.EdgeIndices[ChainNodePosition - 1]];
						NextInputPort = GetTargetPort(Nodes[NodeIndex], PreviousEdge, true);
					}
				}
				TSet<int32> VisitingBounds;
				Chain.LocalBounds = BoundsForInputChainSubtree(Nodes, Island, ChainsByTargetNode, ChainIndex, VisitingBounds);
				Chain.SegmentBounds = BuildInputChainOwnSegmentBounds(Nodes, Chain);
				ActiveChains.Remove(ChainIndex);
			};

			PlaceAttachedChainsForTarget = [&](const int32 TargetNode)
			{
				if (PlacedTargets.Contains(TargetNode) || ActiveTargets.Contains(TargetNode))
				{
					return;
				}
				const TArray<int32>* TargetChains = ChainsByTargetNode.Find(TargetNode);
				if (!TargetChains || TargetChains->Num() == 0)
				{
					return;
				}
				ActiveTargets.Add(TargetNode);
				TArray<int32> ChainIndices = *TargetChains;
				ChainIndices.Sort([&Island](int32 A, int32 B)
				{
					const FInputChain& ChainA = Island.InputChains[A];
					const FInputChain& ChainB = Island.InputChains[B];
					if (ChainA.TargetPinIndex != ChainB.TargetPinIndex)
					{
						return ChainA.TargetPinIndex < ChainB.TargetPinIndex;
					}
					return ChainA.Id < ChainB.Id;
				});
				TArray<FColumnItem> ChainItems;
				ChainItems.Reserve(ChainIndices.Num());
				for (const int32 ChainIndex : ChainIndices)
				{
					PlaceChainSubtree(ChainIndex);
					if (!Island.InputChains.IsValidIndex(ChainIndex))
					{
						continue;
					}
					const FInputChain& Chain = Island.InputChains[ChainIndex];
					FColumnItem Item;
					Item.NodeIndex = ChainIndex;
					Item.Height = Chain.LocalBounds.Height();
					Item.PreferredTop = Chain.LocalBounds.Top;
					Item.bHasPreferred = true;
					ChainItems.Add(Item);
				}
				const TArray<FInputChainForbiddenZone> Zones = BuildInputChainForbiddenZonesForTarget(
					Nodes,
					AllEdges,
					Island,
					TargetNode,
					FVector2D(Nodes[TargetNode].NewX, Nodes[TargetNode].NewY),
					Options);
				int32 AvoidanceCount = 0;
				PlaceAnchoredIntervalsAvoidingForbiddenZones(ChainItems, IntraTargetChainGap, Zones, &AvoidanceCount);
				Result.DesignDiagnostics.InputChainForbiddenZoneCount += Zones.Num();
				Result.DesignDiagnostics.InputChainForbiddenAvoidanceCount += AvoidanceCount;
				const int32 GroupIndex = TargetGroupByChain.IsValidIndex(ChainItems.Num() > 0 ? ChainItems[0].NodeIndex : INDEX_NONE)
					? TargetGroupByChain[ChainItems[0].NodeIndex]
					: INDEX_NONE;
				const int32 MetricsGroupKey = (Island.Id + 1) * 100000 + FMath::Max(0, GroupIndex);
				for (const FColumnItem& Item : ChainItems)
				{
					const int32 ChainIndex = Item.NodeIndex;
					if (!Island.InputChains.IsValidIndex(ChainIndex))
					{
						continue;
					}
					FInputChain& Chain = Island.InputChains[ChainIndex];
					Chain.ForbiddenZoneCount = Zones.Num();
					Chain.ForbiddenAvoidanceCount = AvoidanceCount;
					const double DeltaY = Item.FinalTop - Chain.LocalBounds.Top;
					TSet<int32> VisitingOffset;
					OffsetInputChainSubtree(Nodes, Island, ChainsByTargetNode, ChainIndex, DeltaY, VisitingOffset);
					TSet<int32> VisitingGroup;
					MarkInputChainSubtreeGroup(Nodes, Island, ChainsByTargetNode, ChainIndex, MetricsGroupKey, IntraTargetChainGap, VisitingGroup);
				}
				PlacedTargets.Add(TargetNode);
				ActiveTargets.Remove(TargetNode);
			};

			for (const FInputChain& Chain : Island.InputChains)
			{
				if (Nodes.IsValidIndex(Chain.TargetNode) && !Island.InputChainNodeSet.Contains(Chain.TargetNode))
				{
					PlaceAttachedChainsForTarget(Chain.TargetNode);
				}
			}

			for (int32 ChainIndex = 0; ChainIndex < Island.InputChains.Num(); ++ChainIndex)
			{
				const FInputChain& Chain = Island.InputChains[ChainIndex];
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetNumberField(TEXT("island_id"), Island.Id);
				Obj->SetNumberField(TEXT("chain_id"), Chain.Id);
				Obj->SetNumberField(TEXT("target_node"), Chain.TargetNode);
				Obj->SetNumberField(TEXT("target_pin_index"), Chain.TargetPinIndex);
				Obj->SetNumberField(TEXT("target_rank"), Chain.TargetRank);
				Obj->SetNumberField(TEXT("corridor_index"), Chain.CorridorIndex);
				Obj->SetNumberField(TEXT("node_count"), Chain.Nodes.Num());
				Obj->SetNumberField(TEXT("target_group_id"), TargetGroupByChain.IsValidIndex(ChainIndex) ? TargetGroupByChain[ChainIndex] : INDEX_NONE);
				Obj->SetNumberField(TEXT("forbidden_zone_count"), Chain.ForbiddenZoneCount);
				Obj->SetNumberField(TEXT("forbidden_avoidance_count"), Chain.ForbiddenAvoidanceCount);
				Obj->SetObjectField(TEXT("local_bounds"), RectToJson(Chain.LocalBounds));
				Obj->SetArrayField(TEXT("segment_bounds"), RectArrayToJson(Chain.SegmentBounds));
				Obj->SetArrayField(TEXT("node_indices"), IntArrayToJson(Chain.Nodes));
				Result.InputChainDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}

		static FRect OffsetRectXY(const FRect& Rect, const double DeltaX, const double DeltaY)
		{
			FRect Result = Rect;
			Result.Left += DeltaX;
			Result.Right += DeltaX;
			Result.Top += DeltaY;
			Result.Bottom += DeltaY;
			return Result;
		}

		static void AppendInputChainCollisionReroutePlans(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FIsland>& Islands,
			const FOptions& Options,
			TArray<FReroutePlan>& Plans,
			FRunResult& Result,
			const TSet<int32>* ProtectedEdgeIndices = nullptr)
		{
			TArray<int32> NodeIsland;
			NodeIsland.Init(INDEX_NONE, Nodes.Num());
			TSet<int32> InputChainEdges;
			TArray<FXStripCollisionBox> CollisionSegments;
			for (const FIsland& Island : Islands)
			{
				for (const int32 NodeIndex : Island.Nodes)
				{
					if (NodeIsland.IsValidIndex(NodeIndex))
					{
						NodeIsland[NodeIndex] = Island.Id;
					}
				}
				const TSet<int32> IslandInputChainEdges = BuildInputChainEdgeSet(Island);
				for (const int32 EdgeIndex : IslandInputChainEdges)
				{
					InputChainEdges.Add(EdgeIndex);
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
						AddXStripCollisionBox(
							CollisionSegments,
							EXStripCollisionOwnerKind::InputChain,
							Island.Id,
							Chain.Id,
							OffsetRectXY(Segment, Island.X, Island.Y));
					}
				}
			}

			if (CollisionSegments.Num() == 0)
			{
				return;
			}

			auto NodeIslandOffset = [&Islands, &NodeIsland](const int32 NodeIndex) -> FVector2D
			{
				if (!NodeIsland.IsValidIndex(NodeIndex))
				{
					return FVector2D::ZeroVector;
				}
				const int32 IslandId = NodeIsland[NodeIndex];
				for (const FIsland& Island : Islands)
				{
					if (Island.Id == IslandId)
					{
						return FVector2D(Island.X, Island.Y);
					}
				}
				return FVector2D::ZeroVector;
			};

			auto HasPlanForEdge = [&Plans](const int32 EdgeIndex) -> bool
			{
				for (const FReroutePlan& Plan : Plans)
				{
					if (Plan.EdgeIndex == EdgeIndex)
					{
						return true;
					}
				}
				return false;
			};

			for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
			{
				if (ProtectedEdgeIndices && ProtectedEdgeIndices->Contains(EdgeIndex))
				{
					continue;
				}
				if (InputChainEdges.Contains(EdgeIndex))
				{
					continue;
				}
				if (HasPlanForEdge(EdgeIndex))
				{
					continue;
				}
				const FEdge& Edge = Edges[EdgeIndex];
				if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
				{
					continue;
				}
				const FVector2D SourcePoint = GetSourcePort(Nodes[Edge.Source], Edge, true) + NodeIslandOffset(Edge.Source);
				const FVector2D TargetPoint = GetTargetPort(Nodes[Edge.Target], Edge, true) + NodeIslandOffset(Edge.Target);
				const FRect SourceRect = MakeNodeRectWithOffset(Nodes[Edge.Source], NodeIslandOffset(Edge.Source), true);
				const FRect TargetRect = MakeNodeRectWithOffset(Nodes[Edge.Target], NodeIslandOffset(Edge.Target), true);
				TArray<TArray<FVector2D>> EndpointTriangles;
				if (!TryBuildEndpointOffsetCollisionTriangles(SourcePoint, &SourceRect, TargetPoint, &TargetRect, Options, EndpointTriangles))
				{
					continue;
				}

				double GuardX = TNumericLimits<double>::Max();
				if (!FindLeftmostEndpointOffsetTrianglesIntersectingXStripLeft(EndpointTriangles, CollisionSegments, GuardX))
				{
					continue;
				}

				const double MinAlignedGuardX = ComputeInputXAlignedToOutput(SourcePoint.X, Options);
				const double MaxAlignedGuardX = ComputeOutputXAlignedToInput(TargetPoint.X, Options);
				GuardX = SnapGraphCoordinate(FMath::Max(GuardX, MinAlignedGuardX));
				if (GuardX > MaxAlignedGuardX + 0.5)
				{
					++Result.DesignDiagnostics.InputChainCollisionRerouteRejectedCount;
					continue;
				}
				const FVector2D GuardPoint(GuardX, TargetPoint.Y);
				if (!HasNonNegativeConnectionAlignmentShorten(SourcePoint, GuardPoint, Options)
					|| !HasNonNegativeConnectionAlignmentShorten(GuardPoint, TargetPoint, Options))
				{
					++Result.DesignDiagnostics.InputChainCollisionRerouteRejectedCount;
					continue;
				}

				FReroutePlan Plan;
				Plan.EdgeIndex = EdgeIndex;
				Plan.Reason = TEXT("input_chain_collision_guard");
				Plan.BusId = TEXT("input_chain_collision_guard");
				Plan.RouteKind = ERouteKind::LocalBus;
				Plan.Points.Add(GuardPoint);
				FinalizeReroutePlanPoints(Plan, Options);
				Plans.Add(Plan);
				++Result.DesignDiagnostics.InputChainCollisionRerouteCount;
			}
		}

		static void FreezeCommentIslandLocalLayout(
			TArray<FNode>& Nodes,
			FIsland& Island,
			FRunResult& Result)
		{
			double OriginX = TNumericLimits<double>::Max();
			double OriginY = TNumericLimits<double>::Max();
			if (Nodes.IsValidIndex(Island.CommentNodeIndex))
			{
				OriginX = Nodes[Island.CommentNodeIndex].OldX;
				OriginY = Nodes[Island.CommentNodeIndex].OldY;
			}
			else
			{
				for (const int32 NodeIndex : Island.Nodes)
				{
					if (!Nodes.IsValidIndex(NodeIndex))
					{
						continue;
					}
					OriginX = FMath::Min(OriginX, Nodes[NodeIndex].OldX);
					OriginY = FMath::Min(OriginY, Nodes[NodeIndex].OldY);
				}
			}
			if (OriginX == TNumericLimits<double>::Max() || OriginY == TNumericLimits<double>::Max())
			{
				OriginX = 0.0;
				OriginY = 0.0;
			}

			Island.X = SnapGraphCoordinate(OriginX);
			Island.Y = SnapGraphCoordinate(OriginY);
			Island.PreferredY = Island.Y;
			Island.Columns.Reset();
			Island.Columns.Add(Island.Nodes);
			Island.ColumnWidths.Reset();
			Island.ColumnWidths.Add(0.0);
			Island.ColumnGaps.Reset();
			Island.ColumnInternalBusLaneDemand.Reset();
			Island.InputChains.Reset();
			Island.InputChainNodeSet.Reset();
			for (int32 Order = 0; Order < Island.Nodes.Num(); ++Order)
			{
				const int32 NodeIndex = Island.Nodes[Order];
				if (!Nodes.IsValidIndex(NodeIndex))
				{
					continue;
				}
				Nodes[NodeIndex].Rank = 0;
				Nodes[NodeIndex].Order = Order;
				Nodes[NodeIndex].NewX = SnapGraphCoordinate(Nodes[NodeIndex].OldX - OriginX);
				Nodes[NodeIndex].NewY = SnapGraphCoordinate(Nodes[NodeIndex].OldY - OriginY);
				Island.ColumnWidths[0] = FMath::Max(Island.ColumnWidths[0], Nodes[NodeIndex].Width);
			}
			NormalizeIslandLocalLayoutToPixelGrid(Nodes, Island);
			++Result.DesignDiagnostics.IslandInternalPlacementCount;

			TSharedPtr<FJsonObject> ColumnObj = MakeShared<FJsonObject>();
			ColumnObj->SetNumberField(TEXT("island_id"), Island.Id);
			ColumnObj->SetBoolField(TEXT("comment_island"), true);
			ColumnObj->SetNumberField(TEXT("comment_node_index"), Island.CommentNodeIndex);
			ColumnObj->SetNumberField(TEXT("node_count"), Island.Nodes.Num());
			ColumnObj->SetObjectField(TEXT("local_bounds"), RectToJson(Island.LocalBounds));
			ColumnObj->SetArrayField(TEXT("node_indices"), IntArrayToJson(Island.Nodes));
			Result.ColumnDiagnostics.Add(MakeShared<FJsonValueObject>(ColumnObj));
		}

		static void LayoutIslandInternally(
			TArray<FNode>& Nodes,
			const TArray<FEdge>& AllEdges,
			const TArray<FEdge>& ExplicitEdges,
			const TSet<int32>& RankIgnoredEdges,
			FIsland& Island,
			const FOptions& Options,
			FRunResult& Result)
		{
			if (Island.Columns.Num() == 0)
			{
				PrepareIslandColumnsAndOrder(Nodes, AllEdges, ExplicitEdges, RankIgnoredEdges, Island, Result.DesignDiagnostics);
			}
			TArray<TArray<int32>>& Columns = Island.Columns;
			Island.ColumnWidths.Reset();
			Island.ColumnWidths.SetNum(Columns.Num());
			for (int32 ColumnIndex = 0; ColumnIndex < Columns.Num(); ++ColumnIndex)
			{
				for (const int32 NodeIndex : Columns[ColumnIndex])
				{
					Island.ColumnWidths[ColumnIndex] = FMath::Max(Island.ColumnWidths[ColumnIndex], Nodes[NodeIndex].Width);
				}
			}
			Island.ColumnGaps.Reset();
			Island.ColumnGaps.SetNum(FMath::Max(0, Columns.Num() - 1));
			TArray<double> InputChainWidthDemand;
			InputChainWidthDemand.SetNum(Island.ColumnGaps.Num());
			TArray<double> ColumnGapBaseDemand;
			ColumnGapBaseDemand.SetNum(Island.ColumnGaps.Num());
			TArray<double> ColumnGapBusLanePitch;
			ColumnGapBusLanePitch.SetNum(Island.ColumnGaps.Num());
			TArray<double> ColumnGapBusDemand;
			ColumnGapBusDemand.SetNum(Island.ColumnGaps.Num());
			for (double& Demand : InputChainWidthDemand)
			{
				Demand = 0.0;
			}
			for (double& Demand : ColumnGapBaseDemand)
			{
				Demand = 0.0;
			}
			for (double& Demand : ColumnGapBusLanePitch)
			{
				Demand = 0.0;
			}
			for (double& Demand : ColumnGapBusDemand)
			{
				Demand = 0.0;
			}
			TArray<double> NodeSlotHeights;
			NodeSlotHeights.SetNum(Nodes.Num());
			TArray<double> NodeSlotTopOffsets;
			NodeSlotTopOffsets.SetNum(Nodes.Num());
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				NodeSlotHeights[NodeIndex] = Nodes[NodeIndex].Height;
				NodeSlotTopOffsets[NodeIndex] = 0.0;
			}
			const double ChainNodeVerticalGap = InputChainTargetGroupGap(Options);
			TArray<int32> CoreNodesForChainRoots;
			for (const TArray<int32>& Column : Columns)
			{
				for (const int32 NodeIndex : Column)
				{
					CoreNodesForChainRoots.Add(NodeIndex);
				}
			}
			AssignInputChainRanksAndCorridors(Nodes, Island, CoreNodesForChainRoots);
			const TMap<int32, TArray<int32>> ChainsByTargetNode = BuildInputChainsByTargetNode(Island);
			TMap<FString, TArray<int32>> InputChainIndicesByTargetGroup;
			for (FInputChain& Chain : Island.InputChains)
			{
				if (!Nodes.IsValidIndex(Chain.TargetNode))
				{
					continue;
				}
				const FString TargetGroupKey = FString::Printf(TEXT("%d|%d"), Chain.TargetNode, Chain.CorridorIndex);
				InputChainIndicesByTargetGroup.FindOrAdd(TargetGroupKey).Add(Chain.Id);
			}
			TMap<int32, FRect> PackedInputChainBoundsByTarget;
			for (const TPair<FString, TArray<int32>>& Pair : InputChainIndicesByTargetGroup)
			{
				FString TargetText;
				FString CorridorText;
				if (Pair.Key.Split(TEXT("|"), &TargetText, &CorridorText))
				{
					const int32 TargetNode = FCString::Atoi(*TargetText);
					const int32 CorridorIndex = FCString::Atoi(*CorridorText);
					if (Nodes.IsValidIndex(TargetNode))
					{
						const FRect PackedGroupBounds = EstimateAttachedInputChainsRelativeToTarget(
							Nodes,
							AllEdges,
							Island,
							Pair.Value,
							ChainsByTargetNode,
							Options,
							ChainNodeVerticalGap);
						if (InputChainWidthDemand.IsValidIndex(CorridorIndex))
						{
							const double RequiredCorridorWidth = FMath::Max(0.0, -PackedGroupBounds.Left) + Options.NodePadding;
							InputChainWidthDemand[CorridorIndex] = FMath::Max(InputChainWidthDemand[CorridorIndex], RequiredCorridorWidth);
						}
						if (FRect* ExistingBounds = PackedInputChainBoundsByTarget.Find(TargetNode))
						{
							ExistingBounds->Top = FMath::Min(ExistingBounds->Top, PackedGroupBounds.Top);
							ExistingBounds->Bottom = FMath::Max(ExistingBounds->Bottom, PackedGroupBounds.Bottom);
							ExistingBounds->Left = FMath::Min(ExistingBounds->Left, PackedGroupBounds.Left);
							ExistingBounds->Right = FMath::Max(ExistingBounds->Right, PackedGroupBounds.Right);
						}
						else
						{
							PackedInputChainBoundsByTarget.Add(TargetNode, PackedGroupBounds);
						}
					}
				}
			}
			for (const TPair<int32, FRect>& Pair : PackedInputChainBoundsByTarget)
			{
				const int32 TargetNode = Pair.Key;
				if (!Nodes.IsValidIndex(TargetNode))
				{
					continue;
				}
				const FRect& PackedBounds = Pair.Value;
				const double SlotTopOffset = FMath::Max(0.0, -PackedBounds.Top);
				const double RequiredHeight = FMath::Max(
					SlotTopOffset + Nodes[TargetNode].Height,
					SlotTopOffset + PackedBounds.Bottom);
				NodeSlotTopOffsets[TargetNode] = FMath::Max(NodeSlotTopOffsets[TargetNode], SlotTopOffset);
				NodeSlotHeights[TargetNode] = FMath::Max(NodeSlotHeights[TargetNode], RequiredHeight);
			}
			TArray<double> ColumnX;
			ColumnX.SetNum(Columns.Num());
			for (int32 ColumnIndex = 1; ColumnIndex < Columns.Num(); ++ColumnIndex)
			{
				const int32 BusLaneDemand = Island.ColumnInternalBusLaneDemand.IsValidIndex(ColumnIndex - 1)
					? Island.ColumnInternalBusLaneDemand[ColumnIndex - 1]
					: 0;
				const double ChainReservedGap = InputChainWidthDemand.IsValidIndex(ColumnIndex - 1)
					? InputChainWidthDemand[ColumnIndex - 1]
					: 0.0;
				double BaseGap = 0.0;
				double BusLanePitch = 0.0;
				double BusGapDemand = 0.0;
				Island.ColumnGaps[ColumnIndex - 1] = ComputeCompactIslandColumnGap(
					Nodes,
					Columns,
					ColumnIndex - 1,
					BusLaneDemand,
					ChainReservedGap,
					Options,
					&BaseGap,
					&BusLanePitch,
					&BusGapDemand);
				if (ColumnGapBaseDemand.IsValidIndex(ColumnIndex - 1))
				{
					ColumnGapBaseDemand[ColumnIndex - 1] = BaseGap;
				}
				if (ColumnGapBusLanePitch.IsValidIndex(ColumnIndex - 1))
				{
					ColumnGapBusLanePitch[ColumnIndex - 1] = BusLanePitch;
				}
				if (ColumnGapBusDemand.IsValidIndex(ColumnIndex - 1))
				{
					ColumnGapBusDemand[ColumnIndex - 1] = BusGapDemand;
				}
				ColumnX[ColumnIndex] = ColumnX[ColumnIndex - 1] + Island.ColumnWidths[ColumnIndex - 1] + Island.ColumnGaps[ColumnIndex - 1];
			}

			int32 BaseColumn = 0;
			double BestHeight = -1.0;
			for (int32 ColumnIndex = 0; ColumnIndex < Columns.Num(); ++ColumnIndex)
			{
				const double Height = StackHeight(Nodes, Columns[ColumnIndex], Options.NodePadding, &NodeSlotHeights);
				if (Height > BestHeight)
				{
					BestHeight = Height;
					BaseColumn = ColumnIndex;
				}
			}
			Island.BaseColumn = BaseColumn;
			PlaceDenseColumn(Nodes, Columns[BaseColumn], ColumnX[BaseColumn], 0.0, Options.NodePadding, &NodeSlotHeights, &NodeSlotTopOffsets);
			TArray<TSharedPtr<FJsonValue>> AnchoredItemDiagnostics;
			for (int32 ColumnIndex = BaseColumn + 1; ColumnIndex < Columns.Num(); ++ColumnIndex)
			{
				TArray<FWeightedKnownSet> KnownSets;
				AddWeightedKnownSetForColumn(Columns, ColumnIndex - 1, 1.0, KnownSets);
				PlaceAnchoredColumnByKnownSets(
					Nodes,
					ExplicitEdges,
					KnownSets,
					Columns[ColumnIndex],
					ColumnX[ColumnIndex],
					Options.NodePadding,
					&Result.DesignDiagnostics,
					&NodeSlotHeights,
					&NodeSlotTopOffsets,
					TEXT("initial_right_propagation"),
					Island.Id,
					ColumnIndex,
					&AnchoredItemDiagnostics);
			}
			for (int32 ColumnIndex = BaseColumn - 1; ColumnIndex >= 0; --ColumnIndex)
			{
				TArray<FWeightedKnownSet> KnownSets;
				AddWeightedKnownSetForColumn(Columns, ColumnIndex + 1, 1.0, KnownSets);
				PlaceAnchoredColumnByKnownSets(
					Nodes,
					ExplicitEdges,
					KnownSets,
					Columns[ColumnIndex],
					ColumnX[ColumnIndex],
					Options.NodePadding,
					&Result.DesignDiagnostics,
					&NodeSlotHeights,
					&NodeSlotTopOffsets,
					TEXT("initial_left_propagation"),
					Island.Id,
					ColumnIndex,
					&AnchoredItemDiagnostics);
			}
			PlaceInputChains(Nodes, AllEdges, Island, Options, NodeSlotHeights, Result);
			++Result.DesignDiagnostics.IslandInternalPlacementCount;

			NormalizeIslandLocalLayoutToPixelGrid(Nodes, Island);

			TSharedPtr<FJsonObject> ColumnObj = MakeShared<FJsonObject>();
			ColumnObj->SetNumberField(TEXT("island_id"), Island.Id);
			ColumnObj->SetNumberField(TEXT("column_count"), Columns.Num());
			ColumnObj->SetNumberField(TEXT("base_column"), BaseColumn);
			ColumnObj->SetNumberField(TEXT("height"), Island.LocalBounds.Height());
			ColumnObj->SetNumberField(TEXT("anchored_column_placement_count"), FMath::Max(0, Columns.Num() - 1));
			ColumnObj->SetArrayField(TEXT("anchored_column_item_diagnostics"), AnchoredItemDiagnostics);
			TArray<TSharedPtr<FJsonValue>> Widths;
			for (const double Width : Island.ColumnWidths)
			{
				Widths.Add(MakeShared<FJsonValueNumber>(Width));
			}
			ColumnObj->SetArrayField(TEXT("column_widths"), Widths);
			TArray<TSharedPtr<FJsonValue>> Gaps;
			for (const double Gap : Island.ColumnGaps)
			{
				Gaps.Add(MakeShared<FJsonValueNumber>(Gap));
			}
			ColumnObj->SetArrayField(TEXT("column_gaps"), Gaps);
			TArray<TSharedPtr<FJsonValue>> BaseGapValues;
			for (const double Gap : ColumnGapBaseDemand)
			{
				BaseGapValues.Add(MakeShared<FJsonValueNumber>(Gap));
			}
			ColumnObj->SetArrayField(TEXT("column_gap_base_demands"), BaseGapValues);
			TArray<TSharedPtr<FJsonValue>> BusLanePitchValues;
			for (const double Pitch : ColumnGapBusLanePitch)
			{
				BusLanePitchValues.Add(MakeShared<FJsonValueNumber>(Pitch));
			}
			ColumnObj->SetArrayField(TEXT("column_gap_bus_lane_pitches"), BusLanePitchValues);
			TArray<TSharedPtr<FJsonValue>> BusGapDemandValues;
			for (const double Demand : ColumnGapBusDemand)
			{
				BusGapDemandValues.Add(MakeShared<FJsonValueNumber>(Demand));
			}
			ColumnObj->SetArrayField(TEXT("column_gap_bus_demands"), BusGapDemandValues);
			TArray<TSharedPtr<FJsonValue>> InputChainWidthDemandValues;
			for (const double Demand : InputChainWidthDemand)
			{
				InputChainWidthDemandValues.Add(MakeShared<FJsonValueNumber>(Demand));
			}
			ColumnObj->SetArrayField(TEXT("input_chain_width_demands"), InputChainWidthDemandValues);
			ColumnObj->SetArrayField(TEXT("internal_bus_lane_demands"), IntArrayToJson(Island.ColumnInternalBusLaneDemand));
			ColumnObj->SetNumberField(TEXT("input_chain_count"), Island.InputChains.Num());
			ColumnObj->SetNumberField(TEXT("input_chain_node_count"), Island.InputChainNodeSet.Num());
			Result.ColumnDiagnostics.Add(MakeShared<FJsonValueObject>(ColumnObj));
		}
