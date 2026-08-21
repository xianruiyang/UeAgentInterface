// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GraphLayout/BlueprintAutoLayout/UeAgentBlueprintAutoLayoutAlgorithm.h"
#include "Module/UeAgentInterfaceLogger.h"

namespace UeAgentBlueprintAutoLayout
{
	FRect::FRect()
		: Left(0.0)
		, Top(0.0)
		, Right(0.0)
		, Bottom(0.0)
	{
	}

	FRect::FRect(double InLeft, double InTop, double InRight, double InBottom)
		: Left(InLeft)
		, Top(InTop)
		, Right(InRight)
		, Bottom(InBottom)
	{
	}

	double FRect::Width() const
	{
		return Right - Left;
	}

	double FRect::Height() const
	{
		return Bottom - Top;
	}

	double FRect::CenterY() const
	{
		return (Top + Bottom) * 0.5;
	}

	FNode::FNode()
		: Index(INDEX_NONE)
		, Component(INDEX_NONE)
		, Rank(0)
		, Order(0)
		, Width(260.0)
		, Height(110.0)
		, OldX(0.0)
		, OldY(0.0)
		, NewX(0.0)
		, NewY(0.0)
		, bHasExecPin(false)
		, bRerouteNode(false)
		, bIsCommentNode(false)
		, bMeasuredGeometry(false)
		, CommentDepth(0)
		, InputChainTargetGroupKey(INDEX_NONE)
		, InputChainTargetGroupPadding(-1.0)
	{
	}

	FEdge::FEdge()
		: Source(INDEX_NONE)
		, Target(INDEX_NONE)
		, SourcePinIndex(INDEX_NONE)
		, TargetPinIndex(INDEX_NONE)
		, bExec(false)
	{
	}

	FEdge::FEdge(int32 InSource, int32 InTarget, const FString& InSourcePin, const FString& InTargetPin, int32 InSourcePinIndex, int32 InTargetPinIndex, bool bInExec)
		: Source(InSource)
		, Target(InTarget)
		, SourcePin(InSourcePin)
		, TargetPin(InTargetPin)
		, SourcePinIndex(InSourcePinIndex)
		, TargetPinIndex(InTargetPinIndex)
		, bExec(bInExec)
	{
	}

	FReroutePlan::FReroutePlan()
		: EdgeIndex(INDEX_NONE)
		, RouteKind(ERouteKind::Direct)
		, BusLaneIndex(0)
		, BranchLaneIndex(0)
		, AttachmentMergeGroup(INDEX_NONE)
	{
	}

	FDesignDiagnostics::FDesignDiagnostics()
		: SccCountBeforeCut(0)
		, SccFeedbackEdgeCount(0)
		, FanoutNodeCount(0)
		, FanoutImplicitEdgeCount(0)
		, CommentIslandCount(0)
		, CommentIslandNodeCount(0)
		, CommentIslandBoundaryEdgeCount(0)
		, RankIgnoredEdgeCount(0)
		, IslandCutEdgeCount(0)
		, ImplicitEdgeCount(0)
		, NodeIslandCount(0)
		, MultiSinkIslandCount(0)
		, InputChainCount(0)
		, InputChainNodeCount(0)
		, InputChainForbiddenZoneCount(0)
		, InputChainForbiddenAvoidanceCount(0)
		, InputChainCollisionRerouteCount(0)
		, InputChainCollisionRerouteRejectedCount(0)
		, IslandEdgeCount(0)
		, ImplicitEdgeInsideIslandCount(0)
		, SourceParamCount(0)
		, InternalBusGroupCount(0)
		, BoundaryEntryCount(0)
		, LeftBoundaryEntryCount(0)
		, TopBoundaryEntryCount(0)
		, BottomBoundaryEntryCount(0)
		, BoundaryBusRequiredCount(0)
		, BoundaryBusCount(0)
		, InternalAttachMergeGroupCount(0)
		, BoundaryAttachMergeGroupCount(0)
		, BoundaryEntryRejectedCandidateCount(0)
		, AnchoredColumnPlacementCount(0)
		, AnchoredColumnUnconnectedItemCount(0)
		, AnchoredColumnTotalPinResidual(0.0)
		, AnchoredColumnMaxPinResidual(0.0)
		, IslandInternalPlacementCount(0)
		, IslandInternalLeftBoundaryEntryCount(0)
		, IslandInternalMultiColumnBusCount(0)
		, IslandInternalSameColumnCollisionBusCount(0)
		, IslandInternalTotalPinResidual(0.0)
		, IslandInternalMaxPinResidual(0.0)
		, IslandLocalFrozenNodeCount(0)
		, IslandLocalMembershipMutationCount(0)
		, IslandLocalMutationCount(0)
		, IslandLocalMaxMutation(0.0)
		, EndpointCandidateCount(0)
		, PlacementGraphEdgeCount(0)
		, RouteOnlyEdgeCount(0)
		, PlacementOwnerEdgeCount(0)
		, PlacementNonOwnerEdgeCount(0)
		, SourceOrderGroupCount(0)
		, FanoutIntentGroupCount(0)
		, PlacementProposalCount(0)
		, RejectedProposalCount(0)
		, SourcePortOrderViolationCount(0)
		, InvalidForwardConnectionCount(0)
		, SelectedEndpointCandidateCount(0)
		, ConnectionGroupCount(0)
		, RouteGroupCount(0)
		, RouteKindUpgradeCount(0)
		, BranchMergeGroupCount(0)
		, CrossBusTrunkLaneCount(0)
		, CrossBusLaneCount(0)
		, RouteThroughHardOccupancyCount(0)
		, IslandConnectionTotalLength(0.0)
		, TotalRouteLength(0.0)
		, BusYVarianceTotal(0.0)
		, IslandConnectionExternalUpgradeCount(0)
		, IslandConnectionTopBottomXResidual(0.0)
		, IslandConnectionLeftYResidual(0.0)
		, PlacementComponentCount(0)
		, LaneColoringLaneCount(0)
		, RouteThroughIslandCount(0)
		, IslandTopologicalOrderCount(0)
		, IslandTopologicalResidualCycleEdgeCount(0)
		, RootIslandCount(0)
		, RootScoringTargetCount(0)
		, RootShareComponentCount(0)
		, RootEquivalentGroupCount(0)
		, RootContinuousOrderScore(0.0)
		, RootContinuousOrderInversionScore(0.0)
		, RootContinuousOrderPrunedStateCount(0)
		, RootContinuousOrderMemoHitCount(0)
		, RootContinuousOrderExactLimitExceeded(0)
		, RootCompactVerticalGap(0.0)
		, RootCompactTotalHeight(0.0)
		, RootIslandTopYCount(0)
		, IslandPlacementOrderCount(0)
		, IslandLeftwardReanchorBaseColumn(INDEX_NONE)
		, IslandLeftwardReanchorColumnCount(0)
		, DirectIslandConnectionCount(0)
		, BusIslandConnectionCount(0)
		, InterIslandFeedbackBusConnectionCount(0)
		, InterIslandBusCount(0)
		, InterIslandBusDuplicateMergeCount(0)
		, InterIslandBusBundleCount(0)
		, InterIslandBusSlotAssignmentCount(0)
		, InterIslandBusMaxSlotCount(0)
		, InterIslandBusBundleCollisionBoxCount(0)
		, IslandBusCollisionBoxCount(0)
		, IslandBandGroupCount(0)
		, TopologyColumnHorizontalPushCount(0)
		, TopologyColumnBusPullCount(0)
		, IncomingBusBranchCount(0)
		, InterIslandBusBranchXSlotCount(0)
		, IslandBusBranchRightShiftCount(0)
		, InterIslandBusBranchKnotCount(0)
		, InterIslandBusBranchKnotOmittedCount(0)
	{
	}

	FMetrics::FMetrics()
		: OverlapCount(0)
		, EdgeCrossingCount(0)
		, EdgeNodeIntersectionCount(0)
		, BackwardEdgeCount(0)
		, PinOrderViolationCount(0)
		, TotalWireLength(0.0)
		, TotalNonExecWireLength(0.0)
		, LayoutWidth(0.0)
		, LayoutHeight(0.0)
		, LayoutArea(0.0)
	{
	}

	FOptions::FOptions()
		: OriginX(0.0)
		, OriginY(0.0)
		, HorizontalSpacing(300.0)
		, VerticalSpacing(140.0)
		, NodePadding(24.0)
		, OutputAccessOffsetX(0.0)
		, InputAccessOffsetX(0.0)
		, bUseRealGeometry(false)
		, MissingPinAnchorCount(0)
		, MissingMeasuredNodeGeometryCount(0)
	{
	}

	FRunResult::FRunResult()
		: Strategy(TEXT("exec_region_deterministic_inter_island_bus_bundle"))
		, bDesignComplete(false)
		, NodeCount(0)
		, EdgeCount(0)
		, IslandCount(0)
		, IslandEdgeCount(0)
		, RankCount(0)
		, MovedNodeCount(0)
		, ReroutePlanCount(0)
		, LocalBusEdgeCount(0)
		, ExternalBusEdgeCount(0)
		, AlignmentFailedUnitCount(0)
		, InvalidIslandEdgeCount(0)
		, DesignFeatureCount(0)
		, DesignFeatureCompleteCount(0)
		, DesignFeaturePartialCount(0)
		, DesignFeatureMissingCount(0)
	{
	}

	FSelfTestResult::FSelfTestResult()
		: bAllPassed(false)
		, CaseCount(0)
		, PassedCount(0)
		, FailedCount(0)
	{
	}

	const TArray<FVector2D>& GetAuthoritativeReroutePoints(const FReroutePlan& Plan)
	{
		return Plan.Points.Num() > 0 || Plan.KnotPoints.Num() == 0
			? Plan.Points
			: Plan.KnotPoints;
	}

	FString MakeRerouteAttachmentPointMergeKey(const int32 AttachmentMergeGroup, const FVector2D& Point)
	{
		if (AttachmentMergeGroup == INDEX_NONE)
		{
			return FString();
		}
		return FString::Printf(
			TEXT("%d|%d|%d"),
			AttachmentMergeGroup,
			FMath::RoundToInt(Point.X),
			FMath::RoundToInt(Point.Y));
	}

	namespace
	{
	// Keep these internal modules ordered. Later modules depend on helpers declared by earlier modules.
	// Internal data structures, geometry helpers, connection alignment, metrics.
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

		enum class EXStripCollisionOwnerKind : uint8
		{
			Unknown,
			InterIslandBusBundle,
			Island,
			InputChain
		};

		struct FXStripCollisionBox
		{
			EXStripCollisionOwnerKind OwnerKind = EXStripCollisionOwnerKind::Unknown;
			int32 OwnerId = INDEX_NONE;
			int32 SecondaryOwnerId = INDEX_NONE;
			int32 SegmentIndex = INDEX_NONE;
			bool bHasCenterYOverride = false;
			double CenterYOverride = 0.0;
			FRect Rect;

			double CenterY() const
			{
				return bHasCenterYOverride ? CenterYOverride : Rect.CenterY();
			}
		};

		static bool XStripRangeOverlaps(const double A0, const double A1, const double B0, const double B1, const double Tolerance = 0.0)
		{
			const double LeftA = FMath::Min(A0, A1);
			const double RightA = FMath::Max(A0, A1);
			const double LeftB = FMath::Min(B0, B1);
			const double RightB = FMath::Max(B0, B1);
			return RightA + Tolerance > LeftB + 0.5 && RightB + Tolerance > LeftA + 0.5;
		}

		static bool IsValidXStripCollisionBox(const FXStripCollisionBox& Box)
		{
			return Box.Rect.Width() > 0.5
				&& Box.Rect.Height() > 0.5
				&& FMath::IsFinite(Box.Rect.Left)
				&& FMath::IsFinite(Box.Rect.Right)
				&& FMath::IsFinite(Box.Rect.Top)
				&& FMath::IsFinite(Box.Rect.Bottom)
				&& (!Box.bHasCenterYOverride || FMath::IsFinite(Box.CenterYOverride));
		}

		static FXStripCollisionBox MakeXStripCollisionBox(
			const EXStripCollisionOwnerKind OwnerKind,
			const int32 OwnerId,
			const int32 SecondaryOwnerId,
			const int32 SegmentIndex,
			const FRect& Rect)
		{
			FXStripCollisionBox Box;
			Box.OwnerKind = OwnerKind;
			Box.OwnerId = OwnerId;
			Box.SecondaryOwnerId = SecondaryOwnerId;
			Box.SegmentIndex = SegmentIndex;
			Box.Rect = Rect;
			return Box;
		}

		static FXStripCollisionBox MakeXStripCollisionBox(
			const EXStripCollisionOwnerKind OwnerKind,
			const int32 OwnerId,
			const int32 SecondaryOwnerId,
			const int32 SegmentIndex,
			const FRect& Rect,
			const double CenterY)
		{
			FXStripCollisionBox Box = MakeXStripCollisionBox(OwnerKind, OwnerId, SecondaryOwnerId, SegmentIndex, Rect);
			Box.bHasCenterYOverride = true;
			Box.CenterYOverride = CenterY;
			return Box;
		}

		static bool AddXStripCollisionBox(
			TArray<FXStripCollisionBox>& Boxes,
			const EXStripCollisionOwnerKind OwnerKind,
			const int32 OwnerId,
			const int32 SecondaryOwnerId,
			const FRect& Rect)
		{
			const FXStripCollisionBox Box = MakeXStripCollisionBox(OwnerKind, OwnerId, SecondaryOwnerId, Boxes.Num(), Rect);
			if (!IsValidXStripCollisionBox(Box))
			{
				return false;
			}
			Boxes.Add(Box);
			return true;
		}

		static bool AddXStripCollisionBox(
			TArray<FXStripCollisionBox>& Boxes,
			const EXStripCollisionOwnerKind OwnerKind,
			const int32 OwnerId,
			const int32 SecondaryOwnerId,
			const FRect& Rect,
			const double CenterY)
		{
			const FXStripCollisionBox Box = MakeXStripCollisionBox(OwnerKind, OwnerId, SecondaryOwnerId, Boxes.Num(), Rect, CenterY);
			if (!IsValidXStripCollisionBox(Box))
			{
				return false;
			}
			Boxes.Add(Box);
			return true;
		}

		static bool XStripOverlapsXRange(const FXStripCollisionBox& Box, const double X0, const double X1, const double Tolerance = 0.0)
		{
			return IsValidXStripCollisionBox(Box)
				&& XStripRangeOverlaps(Box.Rect.Left, Box.Rect.Right, X0, X1, Tolerance);
		}

		static bool XStripIntersectsRect(const FXStripCollisionBox& Box, const FRect& Rect, const double Padding = 0.0)
		{
			return IsValidXStripCollisionBox(Box)
				&& Rect.Width() > 0.5
				&& Rect.Height() > 0.5
				&& Box.Rect.Intersects(Rect, Padding);
		}

		static bool XStripIntersects(const FXStripCollisionBox& A, const FXStripCollisionBox& B, const double Padding = 0.0)
		{
			return IsValidXStripCollisionBox(A)
				&& IsValidXStripCollisionBox(B)
				&& A.Rect.Intersects(B.Rect, Padding);
		}

		static bool FindLeftmostIntersectingXStripLeft(
			const FRect& Rect,
			const TArray<FXStripCollisionBox>& Boxes,
			double& OutLeft)
		{
			OutLeft = TNumericLimits<double>::Max();
			for (const FXStripCollisionBox& Box : Boxes)
			{
				if (XStripIntersectsRect(Box, Rect, 0.0))
				{
					OutLeft = FMath::Min(OutLeft, Box.Rect.Left);
				}
			}
			return OutLeft != TNumericLimits<double>::Max();
		}

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

		struct FInterfaceBundleAnchorAccumulator
		{
			bool bHasSample = false;
			double FixedMinY = 0.0;
			double FixedMaxY = 0.0;
			double AnchorMinY = 0.0;
			double AnchorMaxY = 0.0;

			void Add(const double FixedInterfaceY, const double AnchorOffsetY)
			{
				if (!bHasSample)
				{
					FixedMinY = FixedMaxY = FixedInterfaceY;
					AnchorMinY = AnchorMaxY = AnchorOffsetY;
					bHasSample = true;
					return;
				}
				FixedMinY = FMath::Min(FixedMinY, FixedInterfaceY);
				FixedMaxY = FMath::Max(FixedMaxY, FixedInterfaceY);
				AnchorMinY = FMath::Min(AnchorMinY, AnchorOffsetY);
				AnchorMaxY = FMath::Max(AnchorMaxY, AnchorOffsetY);
			}

			bool ResolvePreferredOriginY(double& OutPreferredOriginY) const
			{
				if (!bHasSample)
				{
					return false;
				}
				const double FixedCenterY = (FixedMinY + FixedMaxY) * 0.5;
				const double AnchorCenterY = (AnchorMinY + AnchorMaxY) * 0.5;
				OutPreferredOriginY = FMath::RoundToDouble(FixedCenterY - AnchorCenterY);
				return true;
			}
		};

		struct FPortOrderDemand
		{
			bool bValid = false;
			int32 GroupNode = INDEX_NONE;
			double Value = 0.0;
			int32 Distance = 0;
			double Weight = 1.0;
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
			FPortOrderDemandSet ExternalSourceDemands;
			TSet<FString> RightTargetInputKeys;
			int32 StableIndex = 0;
		};

		static double ColumnDistanceInfluenceWeight(const int32 Distance)
		{
			const int32 ClampedDistance = FMath::Max(1, Distance);
			return FMath::Pow(0.5, static_cast<double>(ClampedDistance - 1));
		}

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
			bool bSingleNodeBoundaryBus = false;
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
			bool bSingleNodeBoundaryBus = false;
		};

		struct FSelectedEndpointCandidate
		{
			int32 EdgeIndex = INDEX_NONE;
			int32 CandidateId = INDEX_NONE;
			int32 SourceIsland = INDEX_NONE;
			int32 TargetIsland = INDEX_NONE;
			int32 BoundaryEntryId = INDEX_NONE;
			int32 InternalBusGroupId = INDEX_NONE;
			FString Side = TEXT("left");
			FVector2D SourceLocal = FVector2D::ZeroVector;
			FVector2D EntryLocal = FVector2D::ZeroVector;
			FVector2D ActualTargetLocal = FVector2D::ZeroVector;
			int32 ConnectionGroupId = INDEX_NONE;
			int32 InterIslandBusId = INDEX_NONE;
			double PrimaryResidual = 0.0;
			double MinTargetDeltaX = 0.0;
			double SourcePortOrderKey = 0.0;
			bool bPlacementOwner = false;
			bool bSingleNodeBoundaryBus = false;
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

		static double BusPathAccessClearanceX(const FOptions& Options)
		{
			return EndpointAccessClearanceX(Options) * 2.0;
		}

		static double GetFallbackInterfaceRowSpacing(const FOptions& Options)
		{
			return FMath::Max(Options.NodePadding * 1.5, 36.0);
		}

		static constexpr double BusAvoidanceDistance = 20.0;

		static constexpr double GetBusAvoidanceDistance()
		{
			return BusAvoidanceDistance;
		}

		static constexpr double GetBusTrunkLaneSpacing()
		{
			return GetBusAvoidanceDistance();
		}

		static double GetCrossBusBranchLaneSpacing(const FOptions& Options)
		{
			(void)Options;
			return GetBusAvoidanceDistance();
		}

		static double GetBusHalfThickness()
		{
			return FMath::Max(0.5, GetBusAvoidanceDistance() * 0.05);
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

		static FRect OffsetGraphRect(const FRect& Rect, const FVector2D& Offset)
		{
			FRect Result = Rect;
			Result.Left += Offset.X;
			Result.Right += Offset.X;
			Result.Top += Offset.Y;
			Result.Bottom += Offset.Y;
			return Result;
		}

		static FRect MakeNodeRectWithOffset(const FNode& Node, const FVector2D& Offset, const bool bUseNewPosition)
		{
			return OffsetGraphRect(MakeNodeRect(Node, bUseNewPosition), Offset);
		}

		static bool TryBuildEscapedEndpointAabb(
			const FVector2D& OutputPoint,
			const FRect* OutputNodeRect,
			const FVector2D& InputPoint,
			const FRect* InputNodeRect,
			const FOptions& Options,
			FRect& OutBounds)
		{
			FVector2D EscapedOutput(OutputPoint.X + Options.OutputAccessOffsetX, OutputPoint.Y);
			FVector2D EscapedInput(InputPoint.X - Options.InputAccessOffsetX, InputPoint.Y);
			constexpr double NodeEscapeEpsilon = 0.5;
			if (OutputNodeRect)
			{
				EscapedOutput.X = FMath::Max(EscapedOutput.X, OutputNodeRect->Right + NodeEscapeEpsilon);
			}
			if (InputNodeRect)
			{
				EscapedInput.X = FMath::Min(EscapedInput.X, InputNodeRect->Left - NodeEscapeEpsilon);
			}
			if (!FMath::IsFinite(EscapedOutput.X)
				|| !FMath::IsFinite(EscapedOutput.Y)
				|| !FMath::IsFinite(EscapedInput.X)
				|| !FMath::IsFinite(EscapedInput.Y))
			{
				return false;
			}
			OutBounds.Left = FMath::Min(EscapedOutput.X, EscapedInput.X);
			OutBounds.Top = FMath::Min(EscapedOutput.Y, EscapedInput.Y);
			OutBounds.Right = FMath::Max(EscapedOutput.X, EscapedInput.X);
			OutBounds.Bottom = FMath::Max(EscapedOutput.Y, EscapedInput.Y);
			constexpr double LineTolerance = 0.5;
			if (OutBounds.Right - OutBounds.Left < LineTolerance * 2.0)
			{
				OutBounds.Left -= LineTolerance;
				OutBounds.Right += LineTolerance;
			}
			if (OutBounds.Bottom - OutBounds.Top < LineTolerance * 2.0)
			{
				OutBounds.Top -= LineTolerance;
				OutBounds.Bottom += LineTolerance;
			}
			return true;
		}

		static bool TryBuildEndpointOffsetCollisionTriangles(
			const FVector2D& OutputPoint,
			const FRect* OutputNodeRect,
			const FVector2D& InputPoint,
			const FRect* InputNodeRect,
			const FOptions& Options,
			TArray<TArray<FVector2D>>& OutTriangles)
		{
			OutTriangles.Reset();
			FVector2D EscapedOutput(OutputPoint.X + Options.OutputAccessOffsetX, OutputPoint.Y);
			FVector2D EscapedInput(InputPoint.X - Options.InputAccessOffsetX, InputPoint.Y);
			constexpr double NodeEscapeEpsilon = 0.5;
			if (OutputNodeRect)
			{
				EscapedOutput.X = FMath::Max(EscapedOutput.X, OutputNodeRect->Right + NodeEscapeEpsilon);
			}
			if (InputNodeRect)
			{
				EscapedInput.X = FMath::Min(EscapedInput.X, InputNodeRect->Left - NodeEscapeEpsilon);
			}
			if (!FMath::IsFinite(OutputPoint.X)
				|| !FMath::IsFinite(OutputPoint.Y)
				|| !FMath::IsFinite(InputPoint.X)
				|| !FMath::IsFinite(InputPoint.Y)
				|| !FMath::IsFinite(EscapedOutput.X)
				|| !FMath::IsFinite(EscapedOutput.Y)
				|| !FMath::IsFinite(EscapedInput.X)
				|| !FMath::IsFinite(EscapedInput.Y))
			{
				return false;
			}
			const FVector2D MidPoint =
				OutputPoint * 0.125
				+ EscapedOutput * 0.375
				+ EscapedInput * 0.375
				+ InputPoint * 0.125;
			if (!FMath::IsFinite(MidPoint.X) || !FMath::IsFinite(MidPoint.Y))
			{
				return false;
			}
			OutTriangles.SetNum(2);
			OutTriangles[0].Add(OutputPoint);
			OutTriangles[0].Add(EscapedOutput);
			OutTriangles[0].Add(MidPoint);
			OutTriangles[1].Add(MidPoint);
			OutTriangles[1].Add(EscapedInput);
			OutTriangles[1].Add(InputPoint);
			return true;
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

		static bool PointOnSegment2D(const FVector2D& Point, const FVector2D& A, const FVector2D& B, const double Tolerance = 0.5)
		{
			const double Cross =
				(Point.Y - A.Y) * (B.X - A.X)
				- (Point.X - A.X) * (B.Y - A.Y);
			if (FMath::Abs(Cross) > Tolerance)
			{
				return false;
			}
			return Point.X >= FMath::Min(A.X, B.X) - Tolerance
				&& Point.X <= FMath::Max(A.X, B.X) + Tolerance
				&& Point.Y >= FMath::Min(A.Y, B.Y) - Tolerance
				&& Point.Y <= FMath::Max(A.Y, B.Y) + Tolerance;
		}

		static bool PointInPolygon2D(const FVector2D& Point, const TArray<FVector2D>& Polygon)
		{
			if (Polygon.Num() < 3)
			{
				return false;
			}
			bool bInside = false;
			for (int32 Index = 0, PreviousIndex = Polygon.Num() - 1; Index < Polygon.Num(); PreviousIndex = Index++)
			{
				const FVector2D& A = Polygon[PreviousIndex];
				const FVector2D& B = Polygon[Index];
				if (PointOnSegment2D(Point, A, B))
				{
					return true;
				}
				const bool bCrossesScanline = (A.Y > Point.Y) != (B.Y > Point.Y);
				if (!bCrossesScanline)
				{
					continue;
				}
				const double IntersectX = (B.X - A.X) * (Point.Y - A.Y) / (B.Y - A.Y) + A.X;
				if (Point.X < IntersectX)
				{
					bInside = !bInside;
				}
			}
			return bInside;
		}

		static FRect BoundsForPolygonPoints(const TArray<FVector2D>& Points)
		{
			FRect Bounds;
			Bounds.Left = TNumericLimits<double>::Max();
			Bounds.Top = TNumericLimits<double>::Max();
			Bounds.Right = -TNumericLimits<double>::Max();
			Bounds.Bottom = -TNumericLimits<double>::Max();
			for (const FVector2D& Point : Points)
			{
				Bounds.Left = FMath::Min(Bounds.Left, Point.X);
				Bounds.Top = FMath::Min(Bounds.Top, Point.Y);
				Bounds.Right = FMath::Max(Bounds.Right, Point.X);
				Bounds.Bottom = FMath::Max(Bounds.Bottom, Point.Y);
			}
			constexpr double LineTolerance = 0.5;
			if (Bounds.Right - Bounds.Left < LineTolerance * 2.0)
			{
				Bounds.Left -= LineTolerance;
				Bounds.Right += LineTolerance;
			}
			if (Bounds.Bottom - Bounds.Top < LineTolerance * 2.0)
			{
				Bounds.Top -= LineTolerance;
				Bounds.Bottom += LineTolerance;
			}
			return Bounds;
		}

		static bool PolygonIntersectsRect(const TArray<FVector2D>& Polygon, const FRect& Rect)
		{
			if (Polygon.Num() < 3 || Rect.Width() <= 0.5 || Rect.Height() <= 0.5)
			{
				return false;
			}
			if (!BoundsForPolygonPoints(Polygon).Intersects(Rect, 0.0))
			{
				return false;
			}
			auto PointInRect = [&Rect](const FVector2D& Point)
			{
				return Point.X >= Rect.Left && Point.X <= Rect.Right && Point.Y >= Rect.Top && Point.Y <= Rect.Bottom;
			};
			for (const FVector2D& Point : Polygon)
			{
				if (PointInRect(Point))
				{
					return true;
				}
			}
			const TArray<FVector2D> RectCorners = {
				FVector2D(Rect.Left, Rect.Top),
				FVector2D(Rect.Right, Rect.Top),
				FVector2D(Rect.Right, Rect.Bottom),
				FVector2D(Rect.Left, Rect.Bottom)
			};
			for (const FVector2D& Corner : RectCorners)
			{
				if (PointInPolygon2D(Corner, Polygon))
				{
					return true;
				}
			}
			for (int32 Index = 0; Index < Polygon.Num(); ++Index)
			{
				const FVector2D& A = Polygon[Index];
				const FVector2D& B = Polygon[(Index + 1) % Polygon.Num()];
				if (SegmentIntersectsRect(A, B, Rect))
				{
					return true;
				}
			}
			return false;
		}

		static bool EndpointOffsetTrianglesIntersectsRect(const TArray<TArray<FVector2D>>& Triangles, const FRect& Rect)
		{
			for (const TArray<FVector2D>& Triangle : Triangles)
			{
				if (PolygonIntersectsRect(Triangle, Rect))
				{
					return true;
				}
			}
			return false;
		}

		static bool FindLeftmostEndpointOffsetTrianglesIntersectingXStripLeft(
			const TArray<TArray<FVector2D>>& Triangles,
			const TArray<FXStripCollisionBox>& Boxes,
			double& OutLeft)
		{
			OutLeft = TNumericLimits<double>::Max();
			for (const FXStripCollisionBox& Box : Boxes)
			{
				if (IsValidXStripCollisionBox(Box) && EndpointOffsetTrianglesIntersectsRect(Triangles, Box.Rect))
				{
					OutLeft = FMath::Min(OutLeft, Box.Rect.Left);
				}
			}
			return OutLeft != TNumericLimits<double>::Max();
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

	// Cycle cutting, island extraction, input-chain detection, rank and column ordering.
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

	// Anchored column placement, dense packing, input-chain placement, island-local layout.
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

	// Target input entry candidates and island boundary bus model.
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

	// Frozen island placement, endpoint candidates, bus topology, and route diagnostics.
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

		// Branch slot/right-shift and final plan materialization live in a separate private include.
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

	static TArray<int32> SortedIntSetValues(const TSet<int32>& Values)
	{
		TArray<int32> Out = Values.Array();
		Out.Sort();
		return Out;
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	static TArray<TSharedPtr<FJsonValue>> EdgeSetToJson(
		const TArray<FNode>& Nodes,
		const TArray<FEdge>& Edges,
		const TSet<int32>& EdgeIndices,
		const TSet<int32>& RankIgnoredEdges,
		const TSet<int32>& FeedbackEdges,
		const TSet<int32>& FanoutEdges,
		const TSet<int32>& IslandCutEdges)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const int32 EdgeIndex : SortedIntSetValues(EdgeIndices))
		{
			if (!Edges.IsValidIndex(EdgeIndex))
			{
				continue;
			}
			const FEdge& Edge = Edges[EdgeIndex];
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("edge_index"), EdgeIndex);
			Obj->SetNumberField(TEXT("source_node_index"), Edge.Source);
			Obj->SetNumberField(TEXT("target_node_index"), Edge.Target);
			Obj->SetStringField(TEXT("source_title"), Nodes.IsValidIndex(Edge.Source) ? Nodes[Edge.Source].Title : FString());
			Obj->SetStringField(TEXT("target_title"), Nodes.IsValidIndex(Edge.Target) ? Nodes[Edge.Target].Title : FString());
			Obj->SetStringField(TEXT("source_pin"), Edge.SourcePin);
			Obj->SetStringField(TEXT("target_pin"), Edge.TargetPin);
			Obj->SetStringField(TEXT("source_param_id"), MakeSourceParamId(Edge));
			Obj->SetBoolField(TEXT("exec"), Edge.bExec);
			Obj->SetBoolField(TEXT("rank_ignored"), RankIgnoredEdges.Contains(EdgeIndex));
			Obj->SetBoolField(TEXT("feedback_cut"), FeedbackEdges.Contains(EdgeIndex));
			Obj->SetBoolField(TEXT("fanout_cut"), FanoutEdges.Contains(EdgeIndex));
			Obj->SetBoolField(TEXT("island_cut"), IslandCutEdges.Contains(EdgeIndex));
			Out.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Out;
	}

	static void AppendIslandPartitionStageDiagnostics(
		FRunResult& Result,
		const FString& StageName,
		const TArray<FNode>& Nodes,
		const TArray<FEdge>& Edges,
		const TSet<int32>& IslandCutEdges,
		const TSet<int32>& RankIgnoredEdges,
		const TSet<int32>& FeedbackEdges,
		const TSet<int32>& FanoutEdges,
		const TArray<int32>* CommentGroupByNode)
	{
		const TArray<TArray<int32>> Groups = BuildWeakIslands(Nodes, Edges, IslandCutEdges, CommentGroupByNode);
		TArray<int32> IslandByNode;
		IslandByNode.Init(INDEX_NONE, Nodes.Num());
		for (int32 IslandIndex = 0; IslandIndex < Groups.Num(); ++IslandIndex)
		{
			for (const int32 NodeIndex : Groups[IslandIndex])
			{
				if (IslandByNode.IsValidIndex(NodeIndex))
				{
					IslandByNode[NodeIndex] = IslandIndex;
				}
			}
		}

		TSet<int32> CrossingCutEdges;
		for (const int32 EdgeIndex : IslandCutEdges)
		{
			if (!Edges.IsValidIndex(EdgeIndex))
			{
				continue;
			}
			const FEdge& Edge = Edges[EdgeIndex];
			const int32 SourceIsland = IslandByNode.IsValidIndex(Edge.Source) ? IslandByNode[Edge.Source] : INDEX_NONE;
			const int32 TargetIsland = IslandByNode.IsValidIndex(Edge.Target) ? IslandByNode[Edge.Target] : INDEX_NONE;
			if (SourceIsland != INDEX_NONE && TargetIsland != INDEX_NONE && SourceIsland != TargetIsland)
			{
				CrossingCutEdges.Add(EdgeIndex);
			}
		}

		TSharedPtr<FJsonObject> StageObj = MakeShared<FJsonObject>();
		StageObj->SetStringField(TEXT("stage"), StageName);
		StageObj->SetNumberField(TEXT("island_count"), Groups.Num());
		StageObj->SetNumberField(TEXT("island_cut_edge_count"), IslandCutEdges.Num());
		StageObj->SetNumberField(TEXT("rank_ignored_edge_count"), RankIgnoredEdges.Num());
		StageObj->SetNumberField(TEXT("feedback_edge_count"), FeedbackEdges.Num());
		StageObj->SetNumberField(TEXT("fanout_edge_count"), FanoutEdges.Num());
		StageObj->SetNumberField(TEXT("crossing_cut_edge_count"), CrossingCutEdges.Num());
		StageObj->SetArrayField(TEXT("island_cut_edge_indices"), IntArrayToJson(SortedIntSetValues(IslandCutEdges)));
		StageObj->SetArrayField(TEXT("rank_ignored_edge_indices"), IntArrayToJson(SortedIntSetValues(RankIgnoredEdges)));
		StageObj->SetArrayField(TEXT("feedback_edge_indices"), IntArrayToJson(SortedIntSetValues(FeedbackEdges)));
		StageObj->SetArrayField(TEXT("fanout_edge_indices"), IntArrayToJson(SortedIntSetValues(FanoutEdges)));
		StageObj->SetArrayField(TEXT("crossing_cut_edges"), EdgeSetToJson(Nodes, Edges, CrossingCutEdges, RankIgnoredEdges, FeedbackEdges, FanoutEdges, IslandCutEdges));

		TArray<TSharedPtr<FJsonValue>> IslandValues;
		IslandValues.Reserve(Groups.Num());
		for (int32 IslandIndex = 0; IslandIndex < Groups.Num(); ++IslandIndex)
		{
			const TArray<int32>& Group = Groups[IslandIndex];
			TArray<FString> Titles;
			Titles.Reserve(Group.Num());
			bool bHasExec = false;
			bool bCommentIsland = false;
			int32 CommentGroup = INDEX_NONE;
			for (const int32 NodeIndex : Group)
			{
				if (!Nodes.IsValidIndex(NodeIndex))
				{
					continue;
				}
				Titles.Add(Nodes[NodeIndex].Title);
				bHasExec = bHasExec || Nodes[NodeIndex].bHasExecPin;
				if (CommentGroupByNode && CommentGroupByNode->IsValidIndex(NodeIndex) && (*CommentGroupByNode)[NodeIndex] != INDEX_NONE)
				{
					bCommentIsland = true;
					CommentGroup = (*CommentGroupByNode)[NodeIndex];
				}
			}

			TSharedPtr<FJsonObject> IslandObj = MakeShared<FJsonObject>();
			IslandObj->SetNumberField(TEXT("island_id"), IslandIndex);
			IslandObj->SetNumberField(TEXT("node_count"), Group.Num());
			IslandObj->SetBoolField(TEXT("has_exec_node"), bHasExec);
			IslandObj->SetBoolField(TEXT("comment_island"), bCommentIsland);
			IslandObj->SetNumberField(TEXT("comment_group"), CommentGroup);
			IslandObj->SetArrayField(TEXT("node_indices"), IntArrayToJson(Group));
			IslandObj->SetArrayField(TEXT("node_titles"), StringArrayToJson(Titles));
			IslandValues.Add(MakeShared<FJsonValueObject>(IslandObj));
		}
		StageObj->SetArrayField(TEXT("islands"), IslandValues);
		Result.IslandPartitionStageDiagnostics.Add(MakeShared<FJsonValueObject>(StageObj));
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
		TSet<int32> FeedbackOnlyIslandCutEdges;
		for (const int32 EdgeIndex : FeedbackEdges)
		{
			if (Edges.IsValidIndex(EdgeIndex))
			{
				FeedbackOnlyIslandCutEdges.Add(EdgeIndex);
			}
		}
		AppendIslandPartitionStageDiagnostics(
			OutResult,
			TEXT("after_scc_feedback_cut"),
			Nodes,
			Edges,
			FeedbackOnlyIslandCutEdges,
			RankIgnoredEdges,
			FeedbackEdges,
			FanoutEdges,
			&CommentGroupByNode);

		TMap<int32, TArray<int32>> EdgesBySourceNode;
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
			if (IsSameCommentIslandGroup(CommentGroupByNode, Edge))
			{
				continue;
			}
			EdgesBySourceNode.FindOrAdd(Edge.Source).Add(EdgeIndex);
		}
		TSet<int32> FanoutNodes;
		for (const TPair<int32, TArray<int32>>& Pair : EdgesBySourceNode)
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
				RankIgnoredEdges.Add(EdgeIndex);
				IslandCutEdges.Add(EdgeIndex);
				FanoutEdges.Add(EdgeIndex);
				FanoutNodes.Add(Edges[EdgeIndex].Source);
			}
		}
		TSet<int32> FeedbackAndFanoutIslandCutEdges = IslandCutEdges;
		for (const int32 EdgeIndex : FeedbackEdges)
		{
			if (Edges.IsValidIndex(EdgeIndex))
			{
				FeedbackAndFanoutIslandCutEdges.Add(EdgeIndex);
			}
		}
		AppendIslandPartitionStageDiagnostics(
			OutResult,
			TEXT("after_fanout_cut_before_comment_boundary"),
			Nodes,
			Edges,
			FeedbackAndFanoutIslandCutEdges,
			RankIgnoredEdges,
			FeedbackEdges,
			FanoutEdges,
			&CommentGroupByNode);
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
		for (const int32 EdgeIndex : FeedbackEdges)
		{
			if (Edges.IsValidIndex(EdgeIndex))
			{
				IslandCutEdges.Add(EdgeIndex);
			}
		}
		AppendIslandPartitionStageDiagnostics(
			OutResult,
			TEXT("after_all_island_cuts_before_final_islands"),
			Nodes,
			Edges,
			IslandCutEdges,
			RankIgnoredEdges,
			FeedbackEdges,
			FanoutEdges,
			&CommentGroupByNode);
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
		AppendIslandPartitionStageDiagnostics(
			OutResult,
			TEXT("final_weak_connected_islands"),
			Nodes,
			Edges,
			IslandCutEdges,
			RankIgnoredEdges,
			FeedbackEdges,
			FanoutEdges,
			&CommentGroupByNode);
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
			const bool bFeedbackEdge = FeedbackEdges.Contains(EdgeIndex);
			if (!SourceIslandPtr || !TargetIslandPtr || (*SourceIslandPtr == *TargetIslandPtr && !bFeedbackEdge))
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
			IslandEdge.bFeedback = bFeedbackEdge;
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
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore inter_island_solver_start elapsed_ms=%.1f"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0);
		OutResult.ReroutePlans = RunDeterministicInterIslandSolver(
			Nodes,
			Edges,
			Islands,
			IslandEdges,
			InternalBusGroups,
			IslandPortCandidates,
			Options,
			OutResult);
		if (OutResult.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount > 0)
		{
			UE_LOG(LogUeAgentInterface, Warning, TEXT("BlueprintLayoutCore failed residual island cycle count=%d"),
				OutResult.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount);
			return false;
		}
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutCore inter_island_solver_done elapsed_ms=%.1f reroutes=%d inter_island_buses=%d selected_endpoints=%d"),
			(FPlatformTime::Seconds() - LayoutTraceStartSeconds) * 1000.0,
			OutResult.ReroutePlans.Num(),
			OutResult.DesignDiagnostics.InterIslandBusCount,
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
		CompleteIf(TEXT("inter_island_topological_x"), OutResult.DesignDiagnostics.IslandEdgeCount == 0 || (OutResult.DesignDiagnostics.PlacementGraphEdgeCount + OutResult.DesignDiagnostics.RouteOnlyEdgeCount == OutResult.DesignDiagnostics.IslandEdgeCount && OutResult.DesignDiagnostics.IslandTopologicalOrderCount == OutResult.DesignDiagnostics.NodeIslandCount && OutResult.DesignDiagnostics.InvalidForwardConnectionCount == 0), OutResult.DesignDiagnostics.IslandTopologicalOrderCount > 0, FString::Printf(TEXT("topo_order=%d islands=%d placement_edges=%d route_only=%d residual_cycles=%d invalid_forward=%d"), OutResult.DesignDiagnostics.IslandTopologicalOrderCount, OutResult.DesignDiagnostics.NodeIslandCount, OutResult.DesignDiagnostics.PlacementGraphEdgeCount, OutResult.DesignDiagnostics.RouteOnlyEdgeCount, OutResult.DesignDiagnostics.IslandTopologicalResidualCycleEdgeCount, OutResult.DesignDiagnostics.InvalidForwardConnectionCount));
		CompleteIf(TEXT("root_continuous_y_order"), OutResult.DesignDiagnostics.RootIslandCount == 0 || (OutResult.DesignDiagnostics.RootIslandTopYCount == OutResult.DesignDiagnostics.RootIslandCount && OutResult.DesignDiagnostics.RootContinuousOrderExactLimitExceeded == 0), OutResult.DesignDiagnostics.RootIslandTopYCount > 0, FString::Printf(TEXT("roots=%d placed=%d components=%d score=%.1f inversion=%.1f exact_limit=%d"), OutResult.DesignDiagnostics.RootIslandCount, OutResult.DesignDiagnostics.RootIslandTopYCount, OutResult.DesignDiagnostics.RootShareComponentCount, OutResult.DesignDiagnostics.RootContinuousOrderScore, OutResult.DesignDiagnostics.RootContinuousOrderInversionScore, OutResult.DesignDiagnostics.RootContinuousOrderExactLimitExceeded));
		CompleteIf(TEXT("remaining_island_topological_y"), OutResult.DesignDiagnostics.NodeIslandCount == 0 || OutResult.DesignDiagnostics.RootIslandTopYCount + OutResult.DesignDiagnostics.IslandPlacementOrderCount >= OutResult.DesignDiagnostics.NodeIslandCount, OutResult.DesignDiagnostics.IslandPlacementOrderCount > 0, FString::Printf(TEXT("root_placed=%d remaining_placed=%d islands=%d"), OutResult.DesignDiagnostics.RootIslandTopYCount, OutResult.DesignDiagnostics.IslandPlacementOrderCount, OutResult.DesignDiagnostics.NodeIslandCount));
		CompleteIf(TEXT("inter_island_direct_bus_classification"), OutResult.DesignDiagnostics.IslandEdgeCount == 0 || OutResult.DesignDiagnostics.DirectIslandConnectionCount + OutResult.DesignDiagnostics.BusIslandConnectionCount + OutResult.DesignDiagnostics.InterIslandFeedbackBusConnectionCount >= OutResult.DesignDiagnostics.SelectedEndpointCandidateCount, OutResult.DesignDiagnostics.SelectedEndpointCandidateCount > 0, FString::Printf(TEXT("selected=%d direct=%d bus=%d feedback_bus=%d"), OutResult.DesignDiagnostics.SelectedEndpointCandidateCount, OutResult.DesignDiagnostics.DirectIslandConnectionCount, OutResult.DesignDiagnostics.BusIslandConnectionCount, OutResult.DesignDiagnostics.InterIslandFeedbackBusConnectionCount));
		CompleteIf(TEXT("source_order_fanout_route_groups"), OutResult.DesignDiagnostics.IslandEdgeCount == 0 || (OutResult.DesignDiagnostics.SourceOrderGroupCount > 0 && OutResult.DesignDiagnostics.RouteGroupCount >= 0), OutResult.DesignDiagnostics.SourceOrderGroupCount > 0, FString::Printf(TEXT("source_order_groups=%d fanout_groups=%d route_groups=%d"), OutResult.DesignDiagnostics.SourceOrderGroupCount, OutResult.DesignDiagnostics.FanoutIntentGroupCount, OutResult.DesignDiagnostics.RouteGroupCount));
		CompleteIf(TEXT("inter_island_bus_bundle_planning"), OutResult.DesignDiagnostics.InterIslandBusCount == 0 || (OutResult.DesignDiagnostics.InterIslandBusBundleCount > 0 && OutResult.DesignDiagnostics.InterIslandBusSlotAssignmentCount >= OutResult.DesignDiagnostics.InterIslandBusCount && OutResult.DesignDiagnostics.RouteThroughIslandCount == 0), OutResult.DesignDiagnostics.InterIslandBusCount > 0, FString::Printf(TEXT("buses=%d bundles=%d slots=%d max_slot=%d collision_boxes=%d island_boxes=%d route_through_island=%d"), OutResult.DesignDiagnostics.InterIslandBusCount, OutResult.DesignDiagnostics.InterIslandBusBundleCount, OutResult.DesignDiagnostics.InterIslandBusSlotAssignmentCount, OutResult.DesignDiagnostics.InterIslandBusMaxSlotCount, OutResult.DesignDiagnostics.InterIslandBusBundleCollisionBoxCount, OutResult.DesignDiagnostics.IslandBusCollisionBoxCount, OutResult.DesignDiagnostics.RouteThroughIslandCount));
		CompleteIf(TEXT("inter_island_branch_slot_planning"), OutResult.DesignDiagnostics.IncomingBusBranchCount == 0 || (OutResult.DesignDiagnostics.InterIslandBusBranchXSlotCount > 0 && OutResult.DesignDiagnostics.InterIslandBusBranchKnotCount + OutResult.DesignDiagnostics.InterIslandBusBranchKnotOmittedCount >= OutResult.DesignDiagnostics.IncomingBusBranchCount), OutResult.DesignDiagnostics.IncomingBusBranchCount > 0, FString::Printf(TEXT("branches=%d x_slots=%d right_shift=%d knots=%d omitted=%d"), OutResult.DesignDiagnostics.IncomingBusBranchCount, OutResult.DesignDiagnostics.InterIslandBusBranchXSlotCount, OutResult.DesignDiagnostics.IslandBusBranchRightShiftCount, OutResult.DesignDiagnostics.InterIslandBusBranchKnotCount, OutResult.DesignDiagnostics.InterIslandBusBranchKnotOmittedCount));
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
			&& OutResult.DesignDiagnostics.RouteThroughIslandCount == 0;

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
}
