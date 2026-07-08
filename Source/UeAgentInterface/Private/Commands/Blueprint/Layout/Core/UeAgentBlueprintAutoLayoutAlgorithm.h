// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UeAgentBlueprintAutoLayout
{
	struct FRect
	{
		FRect();
		FRect(double InLeft, double InTop, double InRight, double InBottom);

		double Left;
		double Top;
		double Right;
		double Bottom;

		double Width() const;
		double Height() const;
		double CenterY() const;
		bool Intersects(const FRect& Other, double Padding = 0.0) const;
	};

	struct FNode
	{
		FNode();

		FString Guid;
		FString Title;
		FString ClassName;
		int32 Index;
		int32 Component;
		int32 Rank;
		int32 Order;
		double Width;
		double Height;
		double OldX;
		double OldY;
		double NewX;
		double NewY;
		bool bHasExecPin;
		bool bRerouteNode;
		bool bIsCommentNode;
		bool bMeasuredGeometry;
		int32 CommentDepth;
		TArray<FString> InputPinNames;
		TArray<FString> OutputPinNames;
		TArray<FVector2D> InputPinAnchors;
		TArray<FVector2D> OutputPinAnchors;
		TArray<uint8> InputPinAnchorValid;
		TArray<uint8> OutputPinAnchorValid;
		int32 InputChainTargetGroupKey;
		double InputChainTargetGroupPadding;
	};

	struct FEdge
	{
		FEdge();
		FEdge(int32 InSource, int32 InTarget, const FString& InSourcePin, const FString& InTargetPin, int32 InSourcePinIndex, int32 InTargetPinIndex, bool bInExec);

		int32 Source;
		int32 Target;
		FString SourcePin;
		FString TargetPin;
		int32 SourcePinIndex;
		int32 TargetPinIndex;
		bool bExec;
	};

	enum class ERouteKind : uint8
	{
		Direct,
		LocalBus,
		ExternalBus
	};

	enum class EReroutePointRole : uint8
	{
		Auto,
		TargetInput,
		SourceOutput
	};

	struct FReroutePlan
	{
		FReroutePlan();

		int32 EdgeIndex;
		FString Reason;
		FString BusId;
		ERouteKind RouteKind;
		int32 BusLaneIndex;
		int32 BranchLaneIndex;
		int32 AttachmentMergeGroup;
		// Points are visible reroute control-point positions in graph coordinates.
		TArray<FVector2D> Points;
		// Compatibility mirror for older reports. Reroute points are visible graph positions.
		TArray<FVector2D> KnotPoints;
		TArray<EReroutePointRole> PointRoles;
	};

	const TArray<FVector2D>& GetAuthoritativeReroutePoints(const FReroutePlan& Plan);

	FString MakeRerouteAttachmentPointMergeKey(const int32 AttachmentMergeGroup, const FVector2D& Point);

	struct FDesignDiagnostics
	{
		FDesignDiagnostics();

		int32 SccCountBeforeCut;
		int32 SccFeedbackEdgeCount;
		int32 FanoutNodeCount;
		int32 FanoutImplicitEdgeCount;
		int32 CommentIslandCount;
		int32 CommentIslandNodeCount;
		int32 CommentIslandBoundaryEdgeCount;
		int32 RankIgnoredEdgeCount;
		int32 IslandCutEdgeCount;
		int32 ImplicitEdgeCount;
		int32 NodeIslandCount;
		int32 MultiSinkIslandCount;
		int32 InputChainCount;
		int32 InputChainNodeCount;
		int32 InputChainForbiddenZoneCount;
		int32 InputChainForbiddenAvoidanceCount;
		int32 InputChainCollisionRerouteCount;
		int32 InputChainCollisionRerouteRejectedCount;
		int32 IslandEdgeCount;
		int32 ImplicitEdgeInsideIslandCount;
		int32 SourceParamCount;
		int32 InternalBusGroupCount;
		int32 BoundaryEntryCount;
		int32 LeftBoundaryEntryCount;
		int32 TopBoundaryEntryCount;
		int32 BottomBoundaryEntryCount;
		int32 BoundaryBusRequiredCount;
		int32 BoundaryBusCount;
		int32 InternalAttachMergeGroupCount;
		int32 BoundaryAttachMergeGroupCount;
		int32 BoundaryEntryRejectedCandidateCount;
		int32 AnchoredColumnPlacementCount;
		int32 AnchoredColumnUnconnectedItemCount;
		double AnchoredColumnTotalPinResidual;
		double AnchoredColumnMaxPinResidual;
		int32 IslandInternalPlacementCount;
		int32 IslandInternalLeftBoundaryEntryCount;
		int32 IslandInternalMultiColumnBusCount;
		int32 IslandInternalSameColumnCollisionBusCount;
		double IslandInternalTotalPinResidual;
		double IslandInternalMaxPinResidual;
		int32 IslandLocalFrozenNodeCount;
		int32 IslandLocalMembershipMutationCount;
		int32 IslandLocalMutationCount;
		double IslandLocalMaxMutation;
		int32 EndpointCandidateCount;
		int32 PlacementGraphEdgeCount;
		int32 RouteOnlyEdgeCount;
		int32 PlacementOwnerEdgeCount;
		int32 PlacementNonOwnerEdgeCount;
		int32 SourceOrderGroupCount;
		int32 FanoutIntentGroupCount;
		int32 PlacementProposalCount;
		int32 RejectedProposalCount;
		int32 SourcePortOrderViolationCount;
		int32 InvalidForwardConnectionCount;
		int32 SelectedEndpointCandidateCount;
		int32 ConnectionGroupCount;
		int32 RouteGroupCount;
		int32 RouteKindUpgradeCount;
		int32 BranchMergeGroupCount;
		int32 CrossBusTrunkLaneCount;
		int32 CrossBusLaneCount;
		int32 RouteThroughHardOccupancyCount;
		double IslandConnectionTotalLength;
		double TotalRouteLength;
		double BusYVarianceTotal;
		int32 IslandConnectionExternalUpgradeCount;
		double IslandConnectionTopBottomXResidual;
		double IslandConnectionLeftYResidual;
		int32 PlacementComponentCount;
		int32 LaneColoringLaneCount;
		int32 RouteThroughIslandCount;
		int32 IslandTopologicalOrderCount;
		int32 IslandTopologicalResidualCycleEdgeCount;
		int32 RootIslandCount;
		int32 RootScoringTargetCount;
		int32 RootShareComponentCount;
		int32 RootEquivalentGroupCount;
		double RootContinuousOrderScore;
		double RootContinuousOrderInversionScore;
		int32 RootContinuousOrderPrunedStateCount;
		int32 RootContinuousOrderMemoHitCount;
		int32 RootContinuousOrderExactLimitExceeded;
		double RootCompactVerticalGap;
		double RootCompactTotalHeight;
		int32 RootIslandTopYCount;
		int32 IslandPlacementOrderCount;
		int32 IslandLeftwardReanchorBaseColumn;
		int32 IslandLeftwardReanchorColumnCount;
		int32 DirectIslandConnectionCount;
		int32 BusIslandConnectionCount;
		int32 InterIslandFeedbackBusConnectionCount;
		int32 InterIslandBusCount;
		int32 InterIslandBusDuplicateMergeCount;
		int32 InterIslandBusBundleCount;
		int32 InterIslandBusSlotAssignmentCount;
		int32 InterIslandBusMaxSlotCount;
		int32 InterIslandBusBundleCollisionBoxCount;
		int32 IslandBusCollisionBoxCount;
		int32 IslandBandGroupCount;
		int32 TopologyColumnHorizontalPushCount;
		int32 TopologyColumnBusPullCount;
		int32 IncomingBusBranchCount;
		int32 InterIslandBusBranchXSlotCount;
		int32 IslandBusBranchRightShiftCount;
		int32 InterIslandBusBranchKnotCount;
		int32 InterIslandBusBranchKnotOmittedCount;
	};

	struct FMetrics
	{
		FMetrics();

		int32 OverlapCount;
		int32 EdgeCrossingCount;
		int32 EdgeNodeIntersectionCount;
		int32 BackwardEdgeCount;
		int32 PinOrderViolationCount;
		double TotalWireLength;
		double TotalNonExecWireLength;
		double LayoutWidth;
		double LayoutHeight;
		double LayoutArea;
	};

	struct FOptions
	{
		FOptions();

		double OriginX;
		double OriginY;
		double HorizontalSpacing;
		double VerticalSpacing;
		double NodePadding;
		double OutputAccessOffsetX;
		double InputAccessOffsetX;
		FString AccessOffsetSource;
		bool bUseRealGeometry;
		int32 MissingPinAnchorCount;
		int32 MissingMeasuredNodeGeometryCount;
	};

	struct FRunResult
	{
		FRunResult();

		FString Strategy;
		bool bDesignComplete;
		int32 NodeCount;
		int32 EdgeCount;
		int32 IslandCount;
		int32 IslandEdgeCount;
		int32 RankCount;
		int32 MovedNodeCount;
		int32 ReroutePlanCount;
		int32 LocalBusEdgeCount;
		int32 ExternalBusEdgeCount;
		int32 AlignmentFailedUnitCount;
		int32 InvalidIslandEdgeCount;
		int32 DesignFeatureCount;
		int32 DesignFeatureCompleteCount;
		int32 DesignFeaturePartialCount;
		int32 DesignFeatureMissingCount;
		FMetrics MetricsBefore;
		FMetrics MetricsAfter;
		FMetrics MetricsAfterReroute;
		FDesignDiagnostics DesignDiagnostics;
		TArray<FReroutePlan> ReroutePlans;
		TArray<TSharedPtr<FJsonValue>> DesignFeatureCoverage;
		TArray<TSharedPtr<FJsonValue>> IslandDiagnostics;
		TArray<TSharedPtr<FJsonValue>> IslandEdgeDiagnostics;
		TArray<TSharedPtr<FJsonValue>> IslandPartitionStageDiagnostics;
		TArray<TSharedPtr<FJsonValue>> ColumnDiagnostics;
		TArray<TSharedPtr<FJsonValue>> InputChainDiagnostics;
		TArray<TSharedPtr<FJsonValue>> InternalBusGroupDiagnostics;
		TArray<TSharedPtr<FJsonValue>> BoundaryEntryDiagnostics;
		TArray<TSharedPtr<FJsonValue>> BoundaryBusDiagnostics;
		TArray<TSharedPtr<FJsonValue>> IslandLocalMutationDiagnostics;
		TArray<TSharedPtr<FJsonValue>> IslandPortCandidateDiagnostics;
		TArray<TSharedPtr<FJsonValue>> PlacementProposalDiagnostics;
		TArray<TSharedPtr<FJsonValue>> SourceOrderGroupDiagnostics;
		TArray<TSharedPtr<FJsonValue>> FanoutIntentGroupDiagnostics;
		TArray<TSharedPtr<FJsonValue>> SelectedEndpointCandidateDiagnostics;
		TArray<TSharedPtr<FJsonValue>> InterIslandBusBuildDiagnostics;
		TArray<TSharedPtr<FJsonValue>> InterIslandBusDiagnostics;
		TArray<TSharedPtr<FJsonValue>> InterIslandBusBranchVisibleTargetDiagnostics;
		TArray<TSharedPtr<FJsonValue>> InterIslandBusBranchTargetModeDiagnostics;
		TArray<TSharedPtr<FJsonValue>> BranchMergeDiagnostics;
		TArray<TSharedPtr<FJsonValue>> CrossBusLaneDiagnostics;
		TArray<TSharedPtr<FJsonValue>> RouteDiagnostics;
		TArray<TSharedPtr<FJsonValue>> LaneAssignmentDiagnostics;
		TArray<TSharedPtr<FJsonValue>> RejectionDiagnostics;
	};

	struct FSelfTestResult
	{
		FSelfTestResult();

		bool bAllPassed;
		int32 CaseCount;
		int32 PassedCount;
		int32 FailedCount;
		TArray<TSharedPtr<FJsonValue>> Cases;
	};

	FString RouteKindToString(ERouteKind Kind);
	FRect MakeNodeRect(const FNode& Node, bool bUseNewPosition);
	FString ReroutePointRoleToString(EReroutePointRole Role);
	FVector2D GetSourcePort(const FNode& Node, const FEdge& Edge, bool bUseNewPosition);
	FVector2D GetTargetPort(const FNode& Node, const FEdge& Edge, bool bUseNewPosition);
	bool RunLayout(TArray<FNode>& Nodes, const TArray<FEdge>& Edges, const FOptions& Options, FRunResult& OutResult);
	FSelfTestResult RunSelfTest();
	TSharedPtr<FJsonObject> MetricsToJson(const FMetrics& Metrics);
}
