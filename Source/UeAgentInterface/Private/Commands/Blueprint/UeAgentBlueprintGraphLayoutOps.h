// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commands/Blueprint/Layout/Core/UeAgentBlueprintAutoLayoutAlgorithm.h"

class FJsonObject;
class FJsonValue;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;

namespace UeAgentBlueprintOps
{
	struct FGraphLayoutModel
	{
		FGraphLayoutModel();

		TArray<UeAgentBlueprintAutoLayout::FNode> Nodes;
		TArray<UeAgentBlueprintAutoLayout::FEdge> Edges;
		TArray<UEdGraphNode*> GraphNodes;
		TMap<UEdGraphNode*, int32> NodeToIndex;
		TMap<FString, UEdGraphNode*> GuidToNode;
	};

	struct FGraphLayoutGeometryStats
	{
		FGraphLayoutGeometryStats();

		bool bUsedRealGeometry;
		bool bWindowPrepared;
		FString GeometrySource;
		FString Error;
		int32 NodeCount;
		int32 MeasuredNodeCount;
		int32 PinWidgetCount;
		int32 PinGeometryAnchorCount;
		int32 VisibleMissingPinAnchorCount;
		int32 MissingPinAnchorCount;
		int32 FocusedGeometryNodeCount;
		bool bFocusedGeometryBudgetExhausted;
		bool bRestoreViewStateFromDocument;
		bool bRestoredGraphViewAfterCapture;
		bool bRealGeometryCaptureRequested;
		bool bGeometryViewCaptureAllowed;
		bool bFocusedGeometryCaptureAllowed;
		bool bUsedOffscreenGraphEditor;
		FVector2f RestoredViewLocation;
		float RestoredZoomAmount;
		float GeometryMeasurementZoomAmount;
	};

	struct FBlueprintLayoutGraphTarget
	{
		FBlueprintLayoutGraphTarget();

		UEdGraph* Graph;
		FString GraphType;
	};

	int32 CountLayoutSemanticNodes(const UEdGraph* Graph);
	bool CollectBlueprintLayoutTargets(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Params,
		TArray<FBlueprintLayoutGraphTarget>& OutTargets,
		FString& OutError);
	bool TryComputeGraphEditorAccessOffsetX(double& OutOffset);
	FGraphLayoutModel CollectGraphLayoutModel(UEdGraph* Graph);
	bool CaptureGraphLayoutRealGeometry(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		FGraphLayoutModel& Model,
		FGraphLayoutGeometryStats& OutStats,
		bool bAllowGeometryViewCapture,
		bool bAllowFocusedGeometryCapture,
		FString& OutError);
	bool ApplyLayoutReportToModel(
		const FString& ReportFile,
		FGraphLayoutModel& Model,
		UeAgentBlueprintAutoLayout::FRunResult& OutRun,
		FString& OutError);
	int32 RemoveExistingRerouteNodes(UEdGraph* Graph);
	int32 CountExistingRerouteNodes(UEdGraph* Graph);
	int32 CountPlannedRerouteKnotNodes(const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans);
	int32 ApplyNodePositions(const FGraphLayoutModel& Model);
	int32 ApplyReroutePlans(
		UEdGraph* Graph,
		const FGraphLayoutModel& Model,
		const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans,
		bool bInsertRerouteNodes,
		bool bRewireAllEdges);
	FVector2D GetLayoutKnotNodeSpacerSize();
	TArray<TSharedPtr<FJsonValue>> NodesToJson(const FGraphLayoutModel& Model);
	TArray<TSharedPtr<FJsonValue>> ReroutePlansToJson(const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans);
}
