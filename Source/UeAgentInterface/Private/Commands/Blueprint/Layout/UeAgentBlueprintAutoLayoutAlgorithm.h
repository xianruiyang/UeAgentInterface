// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UeAgentBlueprintAutoLayout
{
	struct FRect
	{
		double Left = 0.0;
		double Top = 0.0;
		double Right = 0.0;
		double Bottom = 0.0;

		double Width() const { return Right - Left; }
		double Height() const { return Bottom - Top; }
		double CenterY() const { return (Top + Bottom) * 0.5; }
		bool Intersects(const FRect& Other, double Padding = 0.0) const;
	};

	struct FNode
	{
		FString Guid;
		FString Title;
		FString ClassName;
		int32 Index = INDEX_NONE;
		int32 Component = INDEX_NONE;
		int32 Rank = 0;
		int32 Order = 0;
		double Width = 260.0;
		double Height = 110.0;
		double OldX = 0.0;
		double OldY = 0.0;
		double NewX = 0.0;
		double NewY = 0.0;
		bool bHasExecPin = false;
		bool bRerouteNode = false;
		bool bIsCommentNode = false;
		bool bMeasuredGeometry = false;
		int32 CommentDepth = 0;
		TArray<FString> InputPinNames;
		TArray<FString> OutputPinNames;
		TArray<FVector2D> InputPinAnchors;
		TArray<FVector2D> OutputPinAnchors;
		TArray<uint8> InputPinAnchorValid;
		TArray<uint8> OutputPinAnchorValid;
		int32 InputChainTargetGroupKey = INDEX_NONE;
		double InputChainTargetGroupPadding = -1.0;
	};

	struct FEdge
	{
		int32 Source = INDEX_NONE;
		int32 Target = INDEX_NONE;
		FString SourcePin;
		FString TargetPin;
		int32 SourcePinIndex = INDEX_NONE;
		int32 TargetPinIndex = INDEX_NONE;
		bool bExec = false;
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
		int32 EdgeIndex = INDEX_NONE;
		FString Reason;
		FString BusId;
		ERouteKind RouteKind = ERouteKind::Direct;
		int32 BusLaneIndex = 0;
		int32 BranchLaneIndex = 0;
		int32 AttachmentMergeGroup = INDEX_NONE;
		// Points are visible reroute control-point positions in graph coordinates.
		TArray<FVector2D> Points;
		// Compatibility mirror for older reports. Reroute points are visible graph positions.
		TArray<FVector2D> KnotPoints;
		TArray<EReroutePointRole> PointRoles;
	};

	struct FDesignDiagnostics
	{
		int32 SccCountBeforeCut = 0;
		int32 SccFeedbackEdgeCount = 0;
		int32 FanoutNodeCount = 0;
		int32 FanoutImplicitEdgeCount = 0;
		int32 CommentIslandCount = 0;
		int32 CommentIslandNodeCount = 0;
		int32 CommentIslandBoundaryEdgeCount = 0;
		int32 RankIgnoredEdgeCount = 0;
		int32 IslandCutEdgeCount = 0;
		int32 ImplicitEdgeCount = 0;
		int32 NodeIslandCount = 0;
		int32 MultiSinkIslandCount = 0;
		int32 InputChainCount = 0;
		int32 InputChainNodeCount = 0;
		int32 InputChainForbiddenZoneCount = 0;
		int32 InputChainForbiddenAvoidanceCount = 0;
		int32 InputChainCollisionRerouteCount = 0;
		int32 InputChainCollisionRerouteRejectedCount = 0;
		int32 IslandEdgeCount = 0;
		int32 ImplicitEdgeInsideIslandCount = 0;
		int32 SourceParamCount = 0;
		int32 InternalBusGroupCount = 0;
		int32 BoundaryEntryCount = 0;
		int32 LeftBoundaryEntryCount = 0;
		int32 TopBoundaryEntryCount = 0;
		int32 BottomBoundaryEntryCount = 0;
		int32 BoundaryBusRequiredCount = 0;
		int32 BoundaryBusCount = 0;
		int32 InternalAttachMergeGroupCount = 0;
		int32 BoundaryAttachMergeGroupCount = 0;
		int32 BoundaryEntryRejectedCandidateCount = 0;
		int32 AnchoredColumnPlacementCount = 0;
		int32 AnchoredColumnUnconnectedItemCount = 0;
		double AnchoredColumnTotalPinResidual = 0.0;
		double AnchoredColumnMaxPinResidual = 0.0;
		int32 IslandInternalPlacementCount = 0;
		int32 IslandInternalLeftBoundaryEntryCount = 0;
		double IslandInternalTotalPinResidual = 0.0;
		double IslandInternalMaxPinResidual = 0.0;
		int32 IslandLocalFrozenNodeCount = 0;
		int32 IslandLocalMembershipMutationCount = 0;
		int32 IslandLocalMutationCount = 0;
		double IslandLocalMaxMutation = 0.0;
		int32 EndpointCandidateCount = 0;
		int32 PlacementGraphEdgeCount = 0;
		int32 RouteOnlyEdgeCount = 0;
		int32 RouteOnlyCycleBreakEdgeCount = 0;
		int32 PlacementOwnerEdgeCount = 0;
		int32 PlacementNonOwnerEdgeCount = 0;
		int32 RouteOnlyMiddleCorridorCount = 0;
		int32 RouteOnlyOuterFallbackCount = 0;
		int32 SourceOrderGroupCount = 0;
		int32 FanoutIntentGroupCount = 0;
		int32 PlacementProposalCount = 0;
		int32 RejectedProposalCount = 0;
		int32 SourcePortOrderViolationCount = 0;
		int32 InvalidForwardConnectionCount = 0;
		int32 BusContractCount = 0;
		int32 SelectedEndpointCandidateCount = 0;
		int32 ConnectionGroupCount = 0;
		int32 RouteGroupCount = 0;
		int32 RouteKindUpgradeCount = 0;
		int32 BranchMergeGroupCount = 0;
		int32 CrossBusTrunkLaneCount = 0;
		int32 CrossBusLaneCount = 0;
		int32 RouteThroughHardOccupancyCount = 0;
		double IslandConnectionTotalLength = 0.0;
		double TotalRouteLength = 0.0;
		double BusYVarianceTotal = 0.0;
		int32 IslandConnectionExternalUpgradeCount = 0;
		double IslandConnectionTopBottomXResidual = 0.0;
		double IslandConnectionLeftYResidual = 0.0;
		int32 PlacementComponentCount = 0;
		int32 LaneColoringLaneCount = 0;
		int32 RouteThroughIslandCount = 0;
	};

	struct FMetrics
	{
		int32 OverlapCount = 0;
		int32 EdgeCrossingCount = 0;
		int32 EdgeNodeIntersectionCount = 0;
		int32 BackwardEdgeCount = 0;
		int32 PinOrderViolationCount = 0;
		double TotalWireLength = 0.0;
		double TotalNonExecWireLength = 0.0;
		double LayoutWidth = 0.0;
		double LayoutHeight = 0.0;
		double LayoutArea = 0.0;
	};

	struct FOptions
	{
		double OriginX = 0.0;
		double OriginY = 0.0;
		double HorizontalSpacing = 300.0;
		double VerticalSpacing = 140.0;
		double NodePadding = 24.0;
		// Deprecated compatibility alias. Command entry maps it to both access offsets.
		double PinDirectionBiasX = 24.0;
		double OutputAccessOffsetX = 24.0;
		double InputAccessOffsetX = 24.0;
		FString AccessOffsetSource = TEXT("legacy_pin_direction_bias_x");
		bool bUseRealGeometry = false;
		int32 MissingPinAnchorCount = 0;
	};

	struct FRunResult
	{
		FString Strategy = TEXT("exec_region_island_corridor_v2");
		bool bDesignComplete = false;
		int32 NodeCount = 0;
		int32 EdgeCount = 0;
		int32 IslandCount = 0;
		int32 IslandEdgeCount = 0;
		int32 RankCount = 0;
		int32 MovedNodeCount = 0;
		int32 ReroutePlanCount = 0;
		int32 LocalBusEdgeCount = 0;
		int32 ExternalBusEdgeCount = 0;
		int32 AlignmentFailedUnitCount = 0;
		int32 InvalidIslandEdgeCount = 0;
		int32 DesignFeatureCount = 0;
		int32 DesignFeatureCompleteCount = 0;
		int32 DesignFeaturePartialCount = 0;
		int32 DesignFeatureMissingCount = 0;
		FMetrics MetricsBefore;
		FMetrics MetricsAfter;
		FMetrics MetricsAfterReroute;
		FDesignDiagnostics DesignDiagnostics;
		TArray<FReroutePlan> ReroutePlans;
		TArray<TSharedPtr<FJsonValue>> DesignFeatureCoverage;
		TArray<TSharedPtr<FJsonValue>> IslandDiagnostics;
		TArray<TSharedPtr<FJsonValue>> IslandEdgeDiagnostics;
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
		TArray<TSharedPtr<FJsonValue>> BusContractDiagnostics;
		TArray<TSharedPtr<FJsonValue>> BranchMergeDiagnostics;
		TArray<TSharedPtr<FJsonValue>> CrossBusLaneDiagnostics;
		TArray<TSharedPtr<FJsonValue>> CorridorRouteDiagnostics;
		TArray<TSharedPtr<FJsonValue>> LaneAssignmentDiagnostics;
		TArray<TSharedPtr<FJsonValue>> RejectionDiagnostics;
	};

	struct FSelfTestResult
	{
		bool bAllPassed = false;
		int32 CaseCount = 0;
		int32 PassedCount = 0;
		int32 FailedCount = 0;
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
