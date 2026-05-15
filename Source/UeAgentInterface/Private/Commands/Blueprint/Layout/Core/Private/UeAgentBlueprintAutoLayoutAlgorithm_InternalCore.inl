// Copyright Epic Games, Inc. All Rights Reserved.
// Internal data structures, geometry helpers, connection alignment, metrics.
		struct FInputChain
		{
			int32 Id = INDEX_NONE;
			int32 TargetNode = INDEX_NONE;
			int32 TargetPinIndex = INDEX_NONE;
			int32 TargetEdgeIndex = INDEX_NONE;
			TArray<int32> Nodes;
			TArray<int32> EdgeIndices;
			int32 TargetRank = 0;
			int32 CorridorIndex = INDEX_NONE;
			double PreferredCenterY = 0.0;
			FRect LocalBounds;
			TArray<FRect> SegmentBounds;
			int32 ForbiddenZoneCount = 0;
			int32 ForbiddenAvoidanceCount = 0;
		};

		struct FInputChainForbiddenZone
		{
			double Top = 0.0;
			double Bottom = 0.0;
			int32 EdgeIndex = INDEX_NONE;
			int32 PinIndex = INDEX_NONE;
			bool bOutputPin = false;
		};

		struct FIsland
		{
			int32 Id = INDEX_NONE;
			TArray<int32> Nodes;
			TSet<int32> NodeSet;
			int32 Rank = 0;
			int32 Order = 0;
			double X = 0.0;
			double Y = 0.0;
			double PreferredY = 0.0;
			FRect LocalBounds;
			bool bHasExec = false;
			bool bCommentIsland = false;
			int32 CommentNodeIndex = INDEX_NONE;
			int32 CommentGroup = INDEX_NONE;
			TArray<int32> IncomingEdges;
			TArray<int32> OutgoingEdges;
			TArray<TArray<int32>> Columns;
			TArray<int32> ColumnInternalBusLaneDemand;
			TArray<double> ColumnWidths;
			TArray<double> ColumnGaps;
			int32 BaseColumn = 0;
			TArray<FInputChain> InputChains;
			TSet<int32> InputChainNodeSet;
		};

		struct FIslandEdge
		{
			int32 EdgeIndex = INDEX_NONE;
			int32 SourceIsland = INDEX_NONE;
			int32 TargetIsland = INDEX_NONE;
			FVector2D SourceLocal = FVector2D::ZeroVector;
			FVector2D TargetLocal = FVector2D::ZeroVector;
			FString TargetSide = TEXT("left");
			bool bFeedback = false;
			bool bFanout = false;
			bool bCommentBoundary = false;
		};

		struct FColumnItem
		{
			int32 NodeIndex = INDEX_NONE;
			double PreferredTop = 0.0;
			bool bHasPreferred = false;
			double TempTop = 0.0;
			double Height = 0.0;
			double FinalTop = 0.0;
		};

		struct FPortOrderDemand
		{
			bool bValid = false;
			int32 GroupNode = INDEX_NONE;
			double Value = 0.0;
			int32 Distance = 0;
		};

		struct FPortOrderDemandSet
		{
			TArray<FPortOrderDemand> Demands;
		};

		struct FColumnSortKey
		{
			bool bHasNeighbor = false;
			double NeighborOrder = 0.0;
			double NeighborPortFraction = 0.0;
			FPortOrderDemandSet SourceDemands;
			FPortOrderDemandSet TargetDemands;
			int32 StableIndex = 0;
		};

		struct FInternalBusGroup
		{
			int32 Id = INDEX_NONE;
			int32 IslandId = INDEX_NONE;
			FString SourceParamId;
			int32 TargetColumn = 0;
			TArray<int32> EdgeIndices;
			TArray<int32> TargetNodes;
			TArray<FVector2D> TargetLocalPorts;
			FVector2D EntryLocal = FVector2D::ZeroVector;
			bool bOnlyLeftmostColumn = false;
			bool bExec = false;
		};

		struct FBoundaryEntry
		{
			int32 Id = INDEX_NONE;
			int32 IslandId = INDEX_NONE;
			int32 InternalBusGroupId = INDEX_NONE;
			FString SourceParamId;
			FString Side = TEXT("left");
			FVector2D LocalPoint = FVector2D::ZeroVector;
			double AllowedMinX = 0.0;
			double AllowedMaxX = 0.0;
			bool bContinuousSegment = false;
		};

		struct FBoundaryBus
		{
			int32 Id = INDEX_NONE;
			int32 IslandId = INDEX_NONE;
			FString SourceParamId;
			TArray<int32> InternalBusGroupIds;
			TArray<int32> BoundaryEntryIds;
		};

		struct FIslandPortCandidate
		{
			int32 Id = INDEX_NONE;
			int32 EdgeIndex = INDEX_NONE;
			int32 SourceIsland = INDEX_NONE;
			int32 TargetIsland = INDEX_NONE;
			int32 BoundaryEntryId = INDEX_NONE;
			int32 InternalBusGroupId = INDEX_NONE;
			FString Side = TEXT("left");
			FVector2D SourceLocal = FVector2D::ZeroVector;
			FVector2D TargetLocal = FVector2D::ZeroVector;
			FVector2D ActualTargetLocal = FVector2D::ZeroVector;
			double AllowedMinX = 0.0;
			double AllowedMaxX = 0.0;
			bool bContinuousSegment = false;
		};

		struct FSelectedEndpointCandidate
		{
			int32 EdgeIndex = INDEX_NONE;
			int32 CandidateId = INDEX_NONE;
			int32 SourceIsland = INDEX_NONE;
			int32 TargetIsland = INDEX_NONE;
			FString Side = TEXT("left");
			FVector2D SourceLocal = FVector2D::ZeroVector;
			FVector2D EntryLocal = FVector2D::ZeroVector;
			FVector2D ActualTargetLocal = FVector2D::ZeroVector;
			int32 ConnectionGroupId = INDEX_NONE;
			int32 BusContractId = INDEX_NONE;
			double PrimaryResidual = 0.0;
			double MinTargetDeltaX = 0.0;
			double SourcePortOrderKey = 0.0;
			bool bPlacementOwner = false;
		};

		struct FPlacementProposal
		{
			int32 Id = INDEX_NONE;
			int32 EdgeIndex = INDEX_NONE;
			int32 CandidateId = INDEX_NONE;
			int32 SourceIsland = INDEX_NONE;
			int32 TargetIsland = INDEX_NONE;
			FString Side = TEXT("left");
			FVector2D SourceLocal = FVector2D::ZeroVector;
			FVector2D EntryLocal = FVector2D::ZeroVector;
			FVector2D ActualTargetLocal = FVector2D::ZeroVector;
			double RequiredTargetDeltaX = 0.0;
			double PreferredTargetY = 0.0;
			double Score = 0.0;
			bool bRouteOnly = false;
		};

		struct FBusContract
		{
			int32 Id = INDEX_NONE;
			FString SourceParamId;
			ERouteKind RouteKind = ERouteKind::Direct;
			double TrunkY = 0.0;
			double XMin = 0.0;
			double XMax = 0.0;
			double BranchYMin = 0.0;
			double BranchYMax = 0.0;
			double OutputToTrunkDistanceY = 0.0;
			int32 BusLaneIndex = 0;
			int32 BranchLaneIndex = 0;
			bool bSecondary = false;
			TArray<int32> EdgeIndices;
			TArray<double> BranchXs;
		};

		struct FRouteGroup
		{
			int32 Id = INDEX_NONE;
			FString SourceParamId;
			TArray<int32> SelectedEndpointIndices;
			TArray<int32> EdgeIndices;
			double IdealBusY = 0.0;
			ERouteKind RouteKind = ERouteKind::Direct;
		};

		struct FIslandLocalLayoutSnapshot
		{
			TMap<int32, FVector2D> NodeLocalPositions;
			TMap<int32, int32> NodeIslandIds;
			TMap<int32, TArray<int32>> IslandNodeLists;
			int32 FrozenNodeCount = 0;
		};

		static double SnapGraphCoordinate(const double Value)
		{
			return FMath::IsFinite(Value) ? FMath::RoundToDouble(Value) : Value;
		}

		static FVector2D SnapGraphPoint(const FVector2D& Point)
		{
			return FVector2D(SnapGraphCoordinate(Point.X), SnapGraphCoordinate(Point.Y));
		}

		struct FCorridorRoute
		{
			int32 EdgeIndex = INDEX_NONE;
			FString Side = TEXT("left");
			FString RouteKind = TEXT("direct");
			TArray<FVector2D> Points;
			bool bExternalUpgrade = false;
			bool bCrossesIsland = false;
		};

		struct FLaneAssignment
		{
			int32 EdgeIndex = INDEX_NONE;
			FString Orientation = TEXT("horizontal");
			int32 LaneIndex = 0;
			FString CorridorId;
		};

		static TSharedPtr<FJsonObject> RectToJson(const FRect& Rect)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("left"), Rect.Left);
			Obj->SetNumberField(TEXT("top"), Rect.Top);
			Obj->SetNumberField(TEXT("right"), Rect.Right);
			Obj->SetNumberField(TEXT("bottom"), Rect.Bottom);
			Obj->SetNumberField(TEXT("width"), Rect.Width());
			Obj->SetNumberField(TEXT("height"), Rect.Height());
			return Obj;
		}

		static TArray<TSharedPtr<FJsonValue>> IntArrayToJson(const TArray<int32>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const int32 Value : Values)
			{
				Out.Add(MakeShared<FJsonValueNumber>(Value));
			}
			return Out;
		}

		static TSharedPtr<FJsonObject> PointToJson(const FVector2D& Point)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("x"), Point.X);
			Obj->SetNumberField(TEXT("y"), Point.Y);
			return Obj;
		}

		static TArray<TSharedPtr<FJsonValue>> PointArrayToJson(const TArray<FVector2D>& Points)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const FVector2D& Point : Points)
			{
				Out.Add(MakeShared<FJsonValueObject>(PointToJson(Point)));
			}
			return Out;
		}

		static TArray<TSharedPtr<FJsonValue>> RectArrayToJson(const TArray<FRect>& Rects)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const FRect& Rect : Rects)
			{
				Out.Add(MakeShared<FJsonValueObject>(RectToJson(Rect)));
			}
			return Out;
		}

		static FIslandLocalLayoutSnapshot FreezeIslandLocalLayout(
			const TArray<FNode>& Nodes,
			const TArray<FIsland>& Islands,
			FRunResult& Result)
		{
			FIslandLocalLayoutSnapshot Snapshot;
			for (const FIsland& Island : Islands)
			{
				TArray<int32>& FrozenNodes = Snapshot.IslandNodeLists.FindOrAdd(Island.Id);
				for (const int32 NodeIndex : Island.Nodes)
				{
					if (!Nodes.IsValidIndex(NodeIndex))
					{
						continue;
					}
					FrozenNodes.Add(NodeIndex);
					Snapshot.NodeLocalPositions.Add(NodeIndex, FVector2D(Nodes[NodeIndex].NewX, Nodes[NodeIndex].NewY));
					Snapshot.NodeIslandIds.Add(NodeIndex, Island.Id);
					++Snapshot.FrozenNodeCount;
				}
			}
			Result.DesignDiagnostics.IslandLocalFrozenNodeCount = Snapshot.FrozenNodeCount;
			return Snapshot;
		}

		static void AddIslandLocalMutationDiagnostic(
			FRunResult& Result,
			const FString& Stage,
			const int32 IslandId,
			const int32 NodeIndex,
			const FString& Reason,
			const FVector2D& Expected,
			const FVector2D& Actual,
			const double Delta)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("stage"), Stage);
			Obj->SetNumberField(TEXT("island_id"), IslandId);
			Obj->SetNumberField(TEXT("node_index"), NodeIndex);
			Obj->SetStringField(TEXT("reason"), Reason);
			Obj->SetObjectField(TEXT("expected_local"), PointToJson(Expected));
			Obj->SetObjectField(TEXT("actual_local"), PointToJson(Actual));
			Obj->SetNumberField(TEXT("delta"), Delta);
			Result.IslandLocalMutationDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
		}

		static bool ValidateIslandLocalLayout(
			const TArray<FNode>& Nodes,
			const TArray<FIsland>& Islands,
			const FIslandLocalLayoutSnapshot& Snapshot,
			const bool bNodesAreGlobal,
			const FString& Stage,
			FRunResult& Result)
		{
			bool bOk = true;
			for (const FIsland& Island : Islands)
			{
				const TArray<int32>* FrozenNodes = Snapshot.IslandNodeLists.Find(Island.Id);
				if (!FrozenNodes || FrozenNodes->Num() != Island.Nodes.Num())
				{
					++Result.DesignDiagnostics.IslandLocalMembershipMutationCount;
					bOk = false;
					AddIslandLocalMutationDiagnostic(
						Result,
						Stage,
						Island.Id,
						INDEX_NONE,
						TEXT("island_node_membership_changed"),
						FVector2D::ZeroVector,
						FVector2D::ZeroVector,
						0.0);
					continue;
				}
				for (int32 Index = 0; Index < Island.Nodes.Num(); ++Index)
				{
					const int32 NodeIndex = Island.Nodes[Index];
					if (!FrozenNodes->IsValidIndex(Index) || (*FrozenNodes)[Index] != NodeIndex)
					{
						++Result.DesignDiagnostics.IslandLocalMembershipMutationCount;
						bOk = false;
						AddIslandLocalMutationDiagnostic(
							Result,
							Stage,
							Island.Id,
							NodeIndex,
							TEXT("island_node_order_changed"),
							FVector2D::ZeroVector,
							FVector2D::ZeroVector,
							0.0);
						continue;
					}
					if (!Nodes.IsValidIndex(NodeIndex))
					{
						continue;
					}
					const FVector2D* ExpectedLocal = Snapshot.NodeLocalPositions.Find(NodeIndex);
					const int32* ExpectedIslandId = Snapshot.NodeIslandIds.Find(NodeIndex);
					if (!ExpectedLocal || !ExpectedIslandId || *ExpectedIslandId != Island.Id)
					{
						++Result.DesignDiagnostics.IslandLocalMembershipMutationCount;
						bOk = false;
						AddIslandLocalMutationDiagnostic(
							Result,
							Stage,
							Island.Id,
							NodeIndex,
							TEXT("island_node_snapshot_missing"),
							FVector2D::ZeroVector,
							FVector2D(Nodes[NodeIndex].NewX, Nodes[NodeIndex].NewY),
							0.0);
						continue;
					}

					FVector2D ActualLocal(Nodes[NodeIndex].NewX, Nodes[NodeIndex].NewY);
					if (bNodesAreGlobal)
					{
						ActualLocal.X -= Island.X;
						ActualLocal.Y -= Island.Y;
					}
					const double Delta = (ActualLocal - *ExpectedLocal).Size();
					if (Delta > 0.5)
					{
						++Result.DesignDiagnostics.IslandLocalMutationCount;
						Result.DesignDiagnostics.IslandLocalMaxMutation = FMath::Max(Result.DesignDiagnostics.IslandLocalMaxMutation, Delta);
						bOk = false;
						AddIslandLocalMutationDiagnostic(
							Result,
							Stage,
							Island.Id,
							NodeIndex,
							TEXT("island_local_position_changed"),
							*ExpectedLocal,
							ActualLocal,
							Delta);
					}
				}
			}
			return bOk;
		}

		static void AddDesignFeature(FRunResult& Result, const FString& Key, const FString& Status, const FString& Detail)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("key"), Key);
			Obj->SetStringField(TEXT("status"), Status);
			Obj->SetStringField(TEXT("detail"), Detail);
			Result.DesignFeatureCoverage.Add(MakeShared<FJsonValueObject>(Obj));
			++Result.DesignFeatureCount;
			if (Status == TEXT("complete"))
			{
				++Result.DesignFeatureCompleteCount;
			}
			else if (Status == TEXT("partial"))
			{
				++Result.DesignFeaturePartialCount;
			}
			else
			{
				++Result.DesignFeatureMissingCount;
			}
		}

		static FString MakeSourceParamId(const FEdge& Edge)
		{
			return FString::Printf(TEXT("%d:%s:%d"), Edge.Source, *Edge.SourcePin, Edge.SourcePinIndex);
		}

		static double MedianOf(TArray<double> Values, double Fallback)
		{
			if (Values.Num() == 0)
			{
				return Fallback;
			}
			Values.Sort();
			return Values[Values.Num() / 2];
		}

		static FOptions NormalizeOptions(FOptions Options)
		{
			Options.OutputAccessOffsetX = FMath::Max(0.0, Options.OutputAccessOffsetX);
			Options.InputAccessOffsetX = FMath::Max(0.0, Options.InputAccessOffsetX);
			Options.PinDirectionBiasX = FMath::Max(
				0.0,
				Options.PinDirectionBiasX > 0.0
					? Options.PinDirectionBiasX
					: (Options.OutputAccessOffsetX + Options.InputAccessOffsetX) * 0.5);
			if (Options.AccessOffsetSource.IsEmpty())
			{
				Options.AccessOffsetSource = TEXT("unset_access_offsets");
			}
			return Options;
		}

		static double EndpointAccessClearanceX(const FOptions& Options)
		{
			return Options.OutputAccessOffsetX + Options.InputAccessOffsetX;
		}

		static double GetFallbackInterfaceRowSpacing(const FOptions& Options)
		{
			return FMath::Max(Options.NodePadding * 1.5, 36.0);
		}

		static constexpr double GetBusTrunkLaneSpacing()
		{
			return 20.0;
		}

		static double GetCrossBusBranchLaneSpacing(const FOptions& Options)
		{
			(void)Options;
			return GetBusTrunkLaneSpacing();
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

		static double EstimateInterfaceRowSpacingForNodes(
			const TArray<FNode>& Nodes,
			const TArray<int32>& NodeIndices,
			const FOptions& Options)
		{
			TArray<double> Spacings;
			for (const int32 NodeIndex : NodeIndices)
			{
				if (!Nodes.IsValidIndex(NodeIndex))
				{
					continue;
				}
				const FNode& Node = Nodes[NodeIndex];
				AddAdjacentInterfaceRowSpacings(Node.InputPinAnchors, Node.InputPinAnchorValid, Spacings);
				AddAdjacentInterfaceRowSpacings(Node.OutputPinAnchors, Node.OutputPinAnchorValid, Spacings);
			}
			if (Spacings.Num() == 0)
			{
				return GetFallbackInterfaceRowSpacing(Options);
			}
			Spacings.Sort();
			return FMath::Max(GetFallbackInterfaceRowSpacing(Options), Spacings[Spacings.Num() / 2]);
		}

		static double ComputeConnectionAlignmentShortenX(
			const FVector2D& OutputPoint,
			const FVector2D& InputPoint,
			double OutputOffsetX,
			double InputOffsetX)
		{
			return (InputPoint.X - OutputPoint.X) - (OutputOffsetX + InputOffsetX);
		}

		static double ComputeConnectionAlignmentShortenX(
			const FVector2D& OutputPoint,
			const FVector2D& InputPoint,
			const FOptions& Options)
		{
			return ComputeConnectionAlignmentShortenX(OutputPoint, InputPoint, Options.OutputAccessOffsetX, Options.InputAccessOffsetX);
		}

		static double ComputeInputXAlignedToOutput(const double OutputX, const FOptions& Options)
		{
			return OutputX + EndpointAccessClearanceX(Options);
		}

		static double ComputeOutputXAlignedToInput(const double InputX, const FOptions& Options)
		{
			return InputX - EndpointAccessClearanceX(Options);
		}

		static bool HasNonNegativeConnectionAlignmentShorten(
			const FVector2D& OutputPoint,
			const FVector2D& InputPoint,
			const FOptions& Options,
			const double Tolerance = 0.5)
		{
			return ComputeConnectionAlignmentShortenX(OutputPoint, InputPoint, Options) >= -FMath::Abs(Tolerance);
		}

		static double MinConnectionAlignmentShortenX(
			const TArray<FVector2D>& Points,
			const FOptions& Options)
		{
			double MinShorten = TNumericLimits<double>::Max();
			for (int32 PointIndex = 0; PointIndex + 1 < Points.Num(); ++PointIndex)
			{
				MinShorten = FMath::Min(MinShorten, ComputeConnectionAlignmentShortenX(Points[PointIndex], Points[PointIndex + 1], Options));
			}
			return Points.Num() >= 2 ? MinShorten : 0.0;
		}

		static bool HasNonNegativeConnectionAlignmentShortenForEverySegment(
			const TArray<FVector2D>& Points,
			const FOptions& Options,
			const double Tolerance = 0.5)
		{
			for (int32 PointIndex = 0; PointIndex + 1 < Points.Num(); ++PointIndex)
			{
				if (!HasNonNegativeConnectionAlignmentShorten(Points[PointIndex], Points[PointIndex + 1], Options, Tolerance))
				{
					return false;
				}
			}
			return true;
		}

		static double ComputeMinInputXForNonNegativeShorten(const double OutputX, const FOptions& Options)
		{
			return ComputeInputXAlignedToOutput(OutputX, Options);
		}

		static double ComputeMaxOutputXForNonNegativeShorten(const double InputX, const FOptions& Options)
		{
			return ComputeOutputXAlignedToInput(InputX, Options);
		}

		static double ClampInputXToNonNegativeShorten(
			const double DesiredInputX,
			const double OutputX,
			const double AllowedMinX,
			const double AllowedMaxX,
			const FOptions& Options)
		{
			const double MinAlignedInputX = ComputeMinInputXForNonNegativeShorten(OutputX, Options);
			const double MinX = FMath::Max(AllowedMinX, MinAlignedInputX);
			if (MinX > AllowedMaxX)
			{
				return MinX;
			}
			return FMath::Clamp(DesiredInputX, MinX, AllowedMaxX);
		}

		static double ClampOutputXToNonNegativeShorten(
			const double DesiredOutputX,
			const double InputX,
			const double AllowedMinX,
			const double AllowedMaxX,
			const FOptions& Options)
		{
			const double MaxAlignedOutputX = ComputeMaxOutputXForNonNegativeShorten(InputX, Options);
			const double MaxX = FMath::Min(AllowedMaxX, MaxAlignedOutputX);
			if (AllowedMinX > MaxX)
			{
				return MaxX;
			}
			return FMath::Clamp(DesiredOutputX, AllowedMinX, MaxX);
		}

		static double ComputeTargetOriginDeltaXForAlignedConnection(
			const FVector2D& SourceLocalOutputPoint,
			const FVector2D& TargetLocalInputPoint,
			const FOptions& Options)
		{
			return ComputeInputXAlignedToOutput(SourceLocalOutputPoint.X, Options) - TargetLocalInputPoint.X;
		}

		static void FinalizeReroutePlanPoints(FReroutePlan& Plan, const FOptions& Options)
		{
			(void)Options;
			Plan.PointRoles.Reset();
			Plan.KnotPoints.Reset();
			Plan.PointRoles.Reserve(Plan.Points.Num());
			Plan.KnotPoints.Reserve(Plan.Points.Num());
			for (int32 PointIndex = 0; PointIndex < Plan.Points.Num(); ++PointIndex)
			{
				Plan.Points[PointIndex] = SnapGraphPoint(Plan.Points[PointIndex]);
				Plan.PointRoles.Add(EReroutePointRole::Auto);
				Plan.KnotPoints.Add(Plan.Points[PointIndex]);
			}
		}

		static void SnapReroutePlansToPixelGrid(TArray<FReroutePlan>& Plans)
		{
			for (FReroutePlan& Plan : Plans)
			{
				for (FVector2D& Point : Plan.Points)
				{
					Point = SnapGraphPoint(Point);
				}
				Plan.KnotPoints.Reset();
				Plan.KnotPoints.Reserve(Plan.Points.Num());
				for (const FVector2D& Point : Plan.Points)
				{
					Plan.KnotPoints.Add(Point);
				}
			}
		}

		static bool IsValidNodeIndex(const TArray<FNode>& Nodes, int32 Index)
		{
			return Nodes.IsValidIndex(Index) && !Nodes[Index].bRerouteNode;
		}

		static double PairClearancePadding(const FNode& A, const FNode& B, double DefaultPadding)
		{
			if (A.InputChainTargetGroupKey != INDEX_NONE
				&& A.InputChainTargetGroupKey == B.InputChainTargetGroupKey
				&& A.InputChainTargetGroupPadding >= 0.0
				&& B.InputChainTargetGroupPadding >= 0.0)
			{
				return FMath::Min(DefaultPadding, FMath::Max(A.InputChainTargetGroupPadding, B.InputChainTargetGroupPadding));
			}
			return DefaultPadding;
		}

		static double NodeTop(const FNode& Node, bool bUseNewPosition)
		{
			return bUseNewPosition ? Node.NewY : Node.OldY;
		}

		static double NodeLeft(const FNode& Node, bool bUseNewPosition)
		{
			return bUseNewPosition ? Node.NewX : Node.OldX;
		}

		static FVector2D GetInputAnchor(const FNode& Node, int32 PinIndex)
		{
			if (Node.InputPinAnchors.IsValidIndex(PinIndex)
				&& Node.InputPinAnchorValid.IsValidIndex(PinIndex)
				&& Node.InputPinAnchorValid[PinIndex] != 0)
			{
				return Node.InputPinAnchors[PinIndex];
			}
			ensureMsgf(false, TEXT("Blueprint layout missing real input pin anchor node=%s pin_index=%d"), *Node.Guid, PinIndex);
			return FVector2D::ZeroVector;
		}

		static FVector2D GetOutputAnchor(const FNode& Node, int32 PinIndex)
		{
			if (Node.OutputPinAnchors.IsValidIndex(PinIndex)
				&& Node.OutputPinAnchorValid.IsValidIndex(PinIndex)
				&& Node.OutputPinAnchorValid[PinIndex] != 0)
			{
				return Node.OutputPinAnchors[PinIndex];
			}
			ensureMsgf(false, TEXT("Blueprint layout missing real output pin anchor node=%s pin_index=%d"), *Node.Guid, PinIndex);
			return FVector2D::ZeroVector;
		}

		static FRect BoundsForNodes(const TArray<FNode>& Nodes, const TArray<int32>& NodeIndices)
		{
			FRect Bounds;
			bool bAny = false;
			for (const int32 NodeIndex : NodeIndices)
			{
				if (!IsValidNodeIndex(Nodes, NodeIndex))
				{
					continue;
				}
				const FRect Rect = MakeNodeRect(Nodes[NodeIndex], true);
				if (!bAny)
				{
					Bounds = Rect;
					bAny = true;
				}
				else
				{
					Bounds.Left = FMath::Min(Bounds.Left, Rect.Left);
					Bounds.Top = FMath::Min(Bounds.Top, Rect.Top);
					Bounds.Right = FMath::Max(Bounds.Right, Rect.Right);
					Bounds.Bottom = FMath::Max(Bounds.Bottom, Rect.Bottom);
				}
			}
			return Bounds;
		}

		static TArray<FRect> BuildInputChainOwnSegmentBounds(const TArray<FNode>& Nodes, const FInputChain& Chain)
		{
			TArray<FRect> Segments;
			for (const int32 NodeIndex : Chain.Nodes)
			{
				if (Nodes.IsValidIndex(NodeIndex))
				{
					Segments.Add(MakeNodeRect(Nodes[NodeIndex], true));
				}
			}
			return Segments;
		}

		static FRect BoundsForRects(const TArray<FRect>& Rects)
		{
			FRect Bounds;
			bool bAny = false;
			for (const FRect& Rect : Rects)
			{
				if (!bAny)
				{
					Bounds = Rect;
					bAny = true;
					continue;
				}
				Bounds.Left = FMath::Min(Bounds.Left, Rect.Left);
				Bounds.Top = FMath::Min(Bounds.Top, Rect.Top);
				Bounds.Right = FMath::Max(Bounds.Right, Rect.Right);
				Bounds.Bottom = FMath::Max(Bounds.Bottom, Rect.Bottom);
			}
			return Bounds;
		}

		static void NormalizeIslandLocalLayoutToPixelGrid(TArray<FNode>& Nodes, FIsland& Island)
		{
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			const double OriginX = Island.LocalBounds.Left;
			const double OriginY = Island.LocalBounds.Top;
			for (const int32 NodeIndex : Island.Nodes)
			{
				if (!Nodes.IsValidIndex(NodeIndex))
				{
					continue;
				}
				Nodes[NodeIndex].NewX = SnapGraphCoordinate(Nodes[NodeIndex].NewX - OriginX);
				Nodes[NodeIndex].NewY = SnapGraphCoordinate(Nodes[NodeIndex].NewY - OriginY);
			}

			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			const double SnappedOriginX = Island.LocalBounds.Left;
			const double SnappedOriginY = Island.LocalBounds.Top;
			if (!FMath::IsNearlyZero(SnappedOriginX, 0.5) || !FMath::IsNearlyZero(SnappedOriginY, 0.5))
			{
				for (const int32 NodeIndex : Island.Nodes)
				{
					if (!Nodes.IsValidIndex(NodeIndex))
					{
						continue;
					}
					Nodes[NodeIndex].NewX = SnapGraphCoordinate(Nodes[NodeIndex].NewX - SnappedOriginX);
					Nodes[NodeIndex].NewY = SnapGraphCoordinate(Nodes[NodeIndex].NewY - SnappedOriginY);
				}
			}
			Island.LocalBounds = BoundsForNodes(Nodes, Island.Nodes);
			for (FInputChain& Chain : Island.InputChains)
			{
				Chain.SegmentBounds = BuildInputChainOwnSegmentBounds(Nodes, Chain);
				TArray<FRect> BoundsRects = Chain.SegmentBounds;
				if (BoundsRects.Num() > 0)
				{
					Chain.LocalBounds = BoundsForRects(BoundsRects);
				}
			}
		}

		static void BuildIncidentLists(const TArray<FNode>& Nodes, const TArray<FEdge>& Edges, TArray<TArray<int32>>& Incoming, TArray<TArray<int32>>& Outgoing)
		{
			Incoming.SetNum(Nodes.Num());
			Outgoing.SetNum(Nodes.Num());
			for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
			{
				const FEdge& Edge = Edges[EdgeIndex];
				if (!IsValidNodeIndex(Nodes, Edge.Source) || !IsValidNodeIndex(Nodes, Edge.Target))
				{
					continue;
				}
				Outgoing[Edge.Source].Add(EdgeIndex);
				Incoming[Edge.Target].Add(EdgeIndex);
			}
		}

		static bool SegmentIntersectsRect(FVector2D A, FVector2D B, const FRect& Rect)
		{
			const double MinX = FMath::Min(A.X, B.X);
			const double MaxX = FMath::Max(A.X, B.X);
			const double MinY = FMath::Min(A.Y, B.Y);
			const double MaxY = FMath::Max(A.Y, B.Y);
			if (MaxX < Rect.Left || Rect.Right < MinX || MaxY < Rect.Top || Rect.Bottom < MinY)
			{
				return false;
			}
			auto PointInRect = [&Rect](const FVector2D& Point)
			{
				return Point.X >= Rect.Left && Point.X <= Rect.Right && Point.Y >= Rect.Top && Point.Y <= Rect.Bottom;
			};
			if (PointInRect(A) || PointInRect(B))
			{
				return true;
			}
			if (FMath::Abs(A.X - B.X) < 0.5)
			{
				return A.X >= Rect.Left && A.X <= Rect.Right && MaxY >= Rect.Top && MinY <= Rect.Bottom;
			}
			if (FMath::Abs(A.Y - B.Y) < 0.5)
			{
				return A.Y >= Rect.Top && A.Y <= Rect.Bottom && MaxX >= Rect.Left && MinX <= Rect.Right;
			}
			auto LocalOrientation = [](const FVector2D P, const FVector2D Q, const FVector2D R)
			{
				const double V = (Q.Y - P.Y) * (R.X - Q.X) - (Q.X - P.X) * (R.Y - Q.Y);
				if (FMath::Abs(V) < 0.5)
				{
					return 0;
				}
				return V > 0.0 ? 1 : 2;
			};
			auto Crosses = [&LocalOrientation](const FVector2D P1, const FVector2D P2, const FVector2D Q1, const FVector2D Q2)
			{
				const int32 O1 = LocalOrientation(P1, P2, Q1);
				const int32 O2 = LocalOrientation(P1, P2, Q2);
				const int32 O3 = LocalOrientation(Q1, Q2, P1);
				const int32 O4 = LocalOrientation(Q1, Q2, P2);
				return O1 != O2 && O3 != O4;
			};
			const FVector2D TopLeft(Rect.Left, Rect.Top);
			const FVector2D TopRight(Rect.Right, Rect.Top);
			const FVector2D BottomRight(Rect.Right, Rect.Bottom);
			const FVector2D BottomLeft(Rect.Left, Rect.Bottom);
			return Crosses(A, B, TopLeft, TopRight)
				|| Crosses(A, B, TopRight, BottomRight)
				|| Crosses(A, B, BottomRight, BottomLeft)
				|| Crosses(A, B, BottomLeft, TopLeft);
		}

		static TArray<FVector2D> BuildDirectSplinePolyline(const FVector2D& Start, const FVector2D& End)
		{
			TArray<FVector2D> Points;
			Points.Add(Start);
			const double Dx = End.X - Start.X;
			const double Tangent = FMath::Clamp(FMath::Abs(Dx) * 0.45, 70.0, 420.0);
			const double Direction = Dx >= 0.0 ? 1.0 : -1.0;
			const FVector2D ControlA(Start.X + Direction * Tangent, Start.Y);
			const FVector2D ControlB(End.X - Direction * Tangent, End.Y);
			for (int32 Step = 1; Step < 8; ++Step)
			{
				const double T = static_cast<double>(Step) / 8.0;
				const double U = 1.0 - T;
				const FVector2D Point =
					Start * (U * U * U)
					+ ControlA * (3.0 * U * U * T)
					+ ControlB * (3.0 * U * T * T)
					+ End * (T * T * T);
				Points.Add(Point);
			}
			Points.Add(End);
			return Points;
		}

		static int32 Orientation(const FVector2D A, const FVector2D B, const FVector2D C)
		{
			const double V = (B.Y - A.Y) * (C.X - B.X) - (B.X - A.X) * (C.Y - B.Y);
			if (FMath::Abs(V) < 0.5)
			{
				return 0;
			}
			return V > 0.0 ? 1 : 2;
		}

		static bool SegmentsCross(const FVector2D A, const FVector2D B, const FVector2D C, const FVector2D D)
		{
			const int32 O1 = Orientation(A, B, C);
			const int32 O2 = Orientation(A, B, D);
			const int32 O3 = Orientation(C, D, A);
			const int32 O4 = Orientation(C, D, B);
			return O1 != O2 && O3 != O4;
		}

		static TArray<FVector2D> BuildPolylineForEdgeIndex(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			int32 EdgeIndex,
			const TArray<FReroutePlan>* Plans,
			bool bUseNewPosition)
		{
			TArray<FVector2D> Points;
			if (!Edges.IsValidIndex(EdgeIndex))
			{
				return Points;
			}
			const FEdge& Edge = Edges[EdgeIndex];
			if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
			{
				return Points;
			}
			Points.Add(GetSourcePort(Nodes[Edge.Source], Edge, bUseNewPosition));
			bool bAddedPlanPoints = false;
			if (Plans)
			{
				for (const FReroutePlan& Plan : *Plans)
				{
					if (Plan.EdgeIndex == EdgeIndex)
					{
						for (const FVector2D& Point : Plan.Points)
						{
							Points.Add(Point);
						}
						bAddedPlanPoints = Plan.Points.Num() > 0;
						break;
					}
				}
			}
			Points.Add(GetTargetPort(Nodes[Edge.Target], Edge, bUseNewPosition));
			if (!bAddedPlanPoints && Points.Num() == 2)
			{
				return BuildDirectSplinePolyline(Points[0], Points[1]);
			}
			return Points;
		}

		static FMetrics EvaluateMetrics(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FReroutePlan>* Plans,
			bool bUseNewPosition,
			double Padding,
			const TSet<int32>* BackwardExemptEdges = nullptr)
		{
			FMetrics Metrics;
			TArray<int32> RealNodes;
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				if (IsValidNodeIndex(Nodes, NodeIndex) && !Nodes[NodeIndex].bIsCommentNode)
				{
					RealNodes.Add(NodeIndex);
				}
			}
			for (int32 A = 0; A < RealNodes.Num(); ++A)
			{
				const FRect RectA = MakeNodeRect(Nodes[RealNodes[A]], bUseNewPosition);
				for (int32 B = A + 1; B < RealNodes.Num(); ++B)
				{
					const double PairPadding = PairClearancePadding(Nodes[RealNodes[A]], Nodes[RealNodes[B]], Padding);
					if (RectA.Intersects(MakeNodeRect(Nodes[RealNodes[B]], bUseNewPosition), PairPadding))
					{
						++Metrics.OverlapCount;
					}
				}
			}

			TArray<TArray<FVector2D>> Polylines;
			Polylines.SetNum(Edges.Num());
			for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
			{
				if (!IsValidNodeIndex(Nodes, Edges[EdgeIndex].Source) || !IsValidNodeIndex(Nodes, Edges[EdgeIndex].Target))
				{
					continue;
				}
				Polylines[EdgeIndex] = BuildPolylineForEdgeIndex(Nodes, Edges, EdgeIndex, Plans, bUseNewPosition);
				const FVector2D Start = Polylines[EdgeIndex][0];
				const FVector2D End = Polylines[EdgeIndex].Last();
				if (Start.X > End.X + 1.0 && (!BackwardExemptEdges || !BackwardExemptEdges->Contains(EdgeIndex)))
				{
					++Metrics.BackwardEdgeCount;
				}
				for (int32 PointIndex = 0; PointIndex + 1 < Polylines[EdgeIndex].Num(); ++PointIndex)
				{
					const double Len = FMath::Abs(Polylines[EdgeIndex][PointIndex + 1].X - Polylines[EdgeIndex][PointIndex].X)
						+ FMath::Abs(Polylines[EdgeIndex][PointIndex + 1].Y - Polylines[EdgeIndex][PointIndex].Y);
					Metrics.TotalWireLength += Len;
					if (!Edges[EdgeIndex].bExec)
					{
						Metrics.TotalNonExecWireLength += Len;
					}
				}
			}

			for (int32 EdgeA = 0; EdgeA < Edges.Num(); ++EdgeA)
			{
				if (Polylines[EdgeA].Num() < 2)
				{
					continue;
				}
				for (int32 EdgeB = EdgeA + 1; EdgeB < Edges.Num(); ++EdgeB)
				{
					if (Polylines[EdgeB].Num() < 2)
					{
						continue;
					}
					bool bCrossed = false;
					for (int32 A = 0; A + 1 < Polylines[EdgeA].Num() && !bCrossed; ++A)
					{
						for (int32 B = 0; B + 1 < Polylines[EdgeB].Num(); ++B)
						{
							if (SegmentsCross(Polylines[EdgeA][A], Polylines[EdgeA][A + 1], Polylines[EdgeB][B], Polylines[EdgeB][B + 1]))
							{
								++Metrics.EdgeCrossingCount;
								bCrossed = true;
								break;
							}
						}
					}
				}
				for (const int32 NodeIndex : RealNodes)
				{
					if (NodeIndex == Edges[EdgeA].Source || NodeIndex == Edges[EdgeA].Target)
					{
						continue;
					}
					const FRect Rect = MakeNodeRect(Nodes[NodeIndex], bUseNewPosition);
					for (int32 SegmentIndex = 0; SegmentIndex + 1 < Polylines[EdgeA].Num(); ++SegmentIndex)
					{
						if (SegmentIntersectsRect(Polylines[EdgeA][SegmentIndex], Polylines[EdgeA][SegmentIndex + 1], Rect))
						{
							++Metrics.EdgeNodeIntersectionCount;
							break;
						}
					}
				}
			}

			const FRect Bounds = BoundsForNodes(Nodes, RealNodes);
			Metrics.LayoutWidth = Bounds.Width();
			Metrics.LayoutHeight = Bounds.Height();
			Metrics.LayoutArea = Metrics.LayoutWidth * Metrics.LayoutHeight;
			return Metrics;
		}

		static void AppendEdgeNodeIntersectionDiagnostics(
			const TArray<FNode>& Nodes,
			const TArray<FEdge>& Edges,
			const TArray<FReroutePlan>* Plans,
			const bool bUseNewPosition,
			const FString& Stage,
			FRunResult& Result,
			const int32 MaxDiagnostics = 32)
		{
			int32 Added = 0;
			TArray<int32> RealNodes;
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				if (IsValidNodeIndex(Nodes, NodeIndex) && !Nodes[NodeIndex].bIsCommentNode)
				{
					RealNodes.Add(NodeIndex);
				}
			}

			for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
			{
				if (Added >= MaxDiagnostics
					|| !Edges.IsValidIndex(EdgeIndex)
					|| !IsValidNodeIndex(Nodes, Edges[EdgeIndex].Source)
					|| !IsValidNodeIndex(Nodes, Edges[EdgeIndex].Target))
				{
					continue;
				}

				const TArray<FVector2D> Polyline = BuildPolylineForEdgeIndex(Nodes, Edges, EdgeIndex, Plans, bUseNewPosition);
				if (Polyline.Num() < 2)
				{
					continue;
				}

				for (const int32 NodeIndex : RealNodes)
				{
					if (NodeIndex == Edges[EdgeIndex].Source || NodeIndex == Edges[EdgeIndex].Target)
					{
						continue;
					}
					const FRect Rect = MakeNodeRect(Nodes[NodeIndex], bUseNewPosition);
					for (int32 SegmentIndex = 0; SegmentIndex + 1 < Polyline.Num(); ++SegmentIndex)
					{
						if (!SegmentIntersectsRect(Polyline[SegmentIndex], Polyline[SegmentIndex + 1], Rect))
						{
							continue;
						}

						TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
						Obj->SetStringField(TEXT("kind"), TEXT("edge_node_intersection"));
						Obj->SetStringField(TEXT("stage"), Stage);
						Obj->SetNumberField(TEXT("edge_index"), EdgeIndex);
						Obj->SetBoolField(TEXT("exec"), Edges[EdgeIndex].bExec);
						Obj->SetNumberField(TEXT("source_node"), Edges[EdgeIndex].Source);
						Obj->SetStringField(TEXT("source_title"), Nodes[Edges[EdgeIndex].Source].Title);
						Obj->SetStringField(TEXT("source_pin"), Edges[EdgeIndex].SourcePin);
						Obj->SetNumberField(TEXT("target_node"), Edges[EdgeIndex].Target);
						Obj->SetStringField(TEXT("target_title"), Nodes[Edges[EdgeIndex].Target].Title);
						Obj->SetStringField(TEXT("target_pin"), Edges[EdgeIndex].TargetPin);
						Obj->SetNumberField(TEXT("blocking_node"), NodeIndex);
						Obj->SetStringField(TEXT("blocking_title"), Nodes[NodeIndex].Title);
						Obj->SetObjectField(TEXT("blocking_rect"), RectToJson(Rect));
						Obj->SetNumberField(TEXT("segment_index"), SegmentIndex);
						Obj->SetArrayField(TEXT("polyline"), PointArrayToJson(Polyline));
						Result.RejectionDiagnostics.Add(MakeShared<FJsonValueObject>(Obj));
						++Added;
						break;
					}
					if (Added >= MaxDiagnostics)
					{
						break;
					}
				}
			}
		}
