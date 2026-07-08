// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Commands/Blueprint/Layout/Core/UeAgentBlueprintAutoLayoutAlgorithm.h"
#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "MaterialShared.h"

class FJsonObject;
class FJsonValue;
class SWidget;
class SGraphNode;
class SGraphPanel;
class SGraphPin;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UMaterialExpression;
class UMaterialFunction;
class UMaterialGraph;
class UObject;
class URigVMEdGraph;
struct FGeometry;

namespace UeAgentNodeGraphOps
{
	namespace Internal
	{
		struct FGraphRef
		{
			FGraphRef();

			UEdGraph* Graph;
			FString GraphKind;
			bool bSupported;
			FString UnsupportedReason;
		};

		struct FUniversalLayoutNodeRef
		{
			FUniversalLayoutNodeRef();

			UEdGraphNode* GraphNode;
			UMaterialExpression* MaterialExpression;
			bool bVirtual;
		};

		struct FUniversalLayoutModel
		{
			FUniversalLayoutModel();

			TArray<UeAgentBlueprintAutoLayout::FNode> Nodes;
			TArray<UeAgentBlueprintAutoLayout::FEdge> Edges;
			TArray<FUniversalLayoutNodeRef> NodeRefs;
			TMap<UEdGraphNode*, int32> GraphNodeToIndex;
			TMap<UMaterialExpression*, int32> ExpressionToIndex;
			int32 WritableNodeCount;
			int32 CollapsedRerouteNodeCount;
			int32 CollapsedRerouteEdgeCount;
		};

		struct FSharedGraphGeometryStats
		{
			FSharedGraphGeometryStats();

			bool bUsedRealGeometry;
			bool bOpenedEditorForGeometry;
			bool bUsedOffscreenGraphEditor;
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
			bool bRestoredGraphViewAfterCapture;
			FVector2f RestoredViewLocation;
			float RestoredZoomAmount;
			float GeometryMeasurementZoomAmount;
		};

		struct FRootMaterialPropertySpec
		{
			FRootMaterialPropertySpec();

			const TCHAR* Name;
			EMaterialProperty Property;
		};

		FString NormalizeAssetPath(const FString& InPath);
		FString ToObjectPath(const FString& InPath);
		UObject* LoadAssetObject(const FString& InPath);
		bool SaveAssetPackage(UObject* Asset, FString& OutError);
		bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, TSharedPtr<FJsonObject>& OutObj);
		bool TryGetStringFromEither(const TSharedPtr<FJsonObject>& Primary, const TSharedPtr<FJsonObject>& Secondary, const FString& Field, FString& OutValue);
		bool TryGetBoolFromEither(const TSharedPtr<FJsonObject>& Primary, const TSharedPtr<FJsonObject>& Secondary, const FString& Field, bool& OutValue);
		bool TryGetNumberFromEither(const TSharedPtr<FJsonObject>& Primary, const TSharedPtr<FJsonObject>& Secondary, const FString& Field, double& OutValue);
		bool TryGetLayoutBool(const TSharedPtr<FJsonObject>& Params, const FString& Field, bool& OutValue);
		bool HasRemovedDryRunParameter(const TSharedPtr<FJsonObject>& Params);
		void InitializePendingRealPins(
			UeAgentBlueprintAutoLayout::FNode& Node,
			const TArray<FString>& InputPins,
			const TArray<FString>& OutputPins);
		FString PinNameOrFallback(const FString& Name, const FString& Prefix, int32 Index);
		FString AdapterForAsset(const UObject* Asset);
		bool IsGraphLikelyStateMachineTopLevel(const UEdGraph* Graph);
		TSharedPtr<FJsonObject> GraphRefToJson(const FGraphRef& GraphRef);
		TArray<FGraphRef> CollectBlueprintGraphs(UBlueprint* Blueprint, bool bIncludeNestedGraphs);
		TArray<FGraphRef> CollectGenericEdGraphs(UObject* Asset);
		void AddMaterialGraphSummary(UObject* Asset, TArray<TSharedPtr<FJsonValue>>& Graphs);
		TSharedPtr<FJsonObject> BuildBlueprintLayoutParams(const TSharedPtr<FJsonObject>& Params);

		bool ParseSharedLayoutOptions(const TSharedPtr<FJsonObject>& Params, UeAgentBlueprintAutoLayout::FOptions& OutOptions, FString& OutError);
		void ApplyDefaultOriginFromWritableNodes(
			const FUniversalLayoutModel& Model,
			const TSharedPtr<FJsonObject>& Params,
			UeAgentBlueprintAutoLayout::FOptions& Options);
		TSharedPtr<FJsonObject> SharedLayoutNodeToJson(
			const UeAgentBlueprintAutoLayout::FNode& Node,
			const FUniversalLayoutNodeRef& Ref);
		TArray<TSharedPtr<FJsonValue>> SharedLayoutEdgesToJson(const FUniversalLayoutModel& Model);
		TArray<TSharedPtr<FJsonValue>> SharedReroutePlansToJson(const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans);
		void AddSharedLayoutRunFields(const TSharedPtr<FJsonObject>& OutData, const UeAgentBlueprintAutoLayout::FRunResult& RunResult);
		void AddSharedGeometryFields(const TSharedPtr<FJsonObject>& OutData, const FSharedGraphGeometryStats& GeometryStats);

		UMaterialGraph* ResolveMaterialSharedGraph(UObject* Asset, FString& OutError);
		UEdGraph* ResolveGenericGraph(UObject* Asset, const FString& GraphRef);
		FUniversalLayoutModel BuildGenericSharedLayoutModel(UEdGraph* Graph);
		bool CaptureSharedGraphRealGeometry(
			UObject* Asset,
			UEdGraph* Graph,
			FUniversalLayoutModel& Model,
			FSharedGraphGeometryStats& OutStats,
			FString& OutError,
			bool bOpenAssetEditorForGeometry);
		bool RunSharedBlueprintLayoutCore(
			FUniversalLayoutModel& Model,
			const TSharedPtr<FJsonObject>& Params,
			UeAgentBlueprintAutoLayout::FOptions& OutOptions,
			UeAgentBlueprintAutoLayout::FRunResult& OutRunResult,
			FString& OutError);
		bool OpenEditorForGeometryIfNeeded(UObject* Asset, bool& bOpenedEditor);
		bool TryGetUsableWidgetGeometry(const SWidget& Widget, FGeometry& OutGeometry);
		void PumpAndTickSlateForLayoutGeometry();
		FVector2f SnapViewLocationToPixelGrid(const FVector2f& Location, float ZoomAmount);
		bool IsFiniteLayoutPoint(const FVector2D& Point);
		bool TryGetPinImageCenterAnchor(
			SGraphPanel& GraphPanel,
			SGraphNode& GraphNodeWidget,
			const TSharedRef<SGraphPin>& PinWidget,
			FVector2D& OutAnchor);
		void SetMeasuredPinAnchor(
			UeAgentBlueprintAutoLayout::FNode& Node,
			EEdGraphPinDirection Direction,
			int32 PinIndex,
			const FVector2D& Anchor);
		bool TryGetMeasuredPinAnchor(
			const UeAgentBlueprintAutoLayout::FNode& Node,
			EEdGraphPinDirection Direction,
			int32 PinIndex,
			FVector2D& OutAnchor);
		int32 CountMeasuredPinAnchors(const UeAgentBlueprintAutoLayout::FNode& Node, EEdGraphPinDirection Direction);
		bool IsExecLikeGraphPin(const UEdGraphPin* Pin);
		bool IsVisibleSharedGraphPin(const UEdGraphPin* Pin);
		FString GraphPinNameString(const UEdGraphPin* Pin);
		TArray<UEdGraphPin*> GetSharedGraphPinsByDirection(UEdGraphNode* Node, EEdGraphPinDirection Direction);
		int32 GetSharedGraphPinIndexInDirection(const UEdGraphPin* Pin);
		bool ApplySharedGraphNodePosition(UEdGraphNode* GraphNode, int32 NewX, int32 NewY);
		int32 CountRigVMRerouteNodes(const URigVMEdGraph* RigGraph);
		int32 ApplyRigVMReroutePlans(
			URigVMEdGraph* RigGraph,
			const FUniversalLayoutModel& Model,
			const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans,
			bool bReplaceExistingReroutes,
			FString& OutError);
		int32 CountSharedPlannedRerouteKnotNodes(const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans);

		bool LayoutMaterialExpressionGraph(
			UObject* Asset,
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutData,
			FString& OutError);
		bool LayoutGenericEdGraph(
			UObject* Asset,
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutData,
			FString& OutError);
	}
}
