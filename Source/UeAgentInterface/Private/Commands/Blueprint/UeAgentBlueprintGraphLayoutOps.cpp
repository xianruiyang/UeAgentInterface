// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Blueprint/UeAgentBlueprintGraphLayoutOps.h"

#include "Commands/Blueprint/UeAgentHttpServer_BlueprintOps.h"
#include "Commands/UeAgentGraphViewUtils.h"
#include "BlueprintEditor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphEditor.h"
#include "GraphEditorSettings.h"
#include "K2Node_Knot.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/AppStyle.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"

namespace UeAgentBlueprintOps
{
	FGraphLayoutModel::FGraphLayoutModel() = default;

	FGraphLayoutGeometryStats::FGraphLayoutGeometryStats()
		: bUsedRealGeometry(false)
		, bWindowPrepared(false)
		, GeometrySource(TEXT("real_geometry_required"))
		, NodeCount(0)
		, MeasuredNodeCount(0)
		, PinWidgetCount(0)
		, PinGeometryAnchorCount(0)
		, VisibleMissingPinAnchorCount(0)
		, MissingPinAnchorCount(0)
		, FocusedGeometryNodeCount(0)
		, bFocusedGeometryBudgetExhausted(false)
		, bRestoreViewStateFromDocument(false)
		, bRestoredGraphViewAfterCapture(false)
		, bRealGeometryCaptureRequested(false)
		, bGeometryViewCaptureAllowed(false)
		, bFocusedGeometryCaptureAllowed(false)
		, bUsedOffscreenGraphEditor(false)
		, RestoredViewLocation(FVector2f::ZeroVector)
		, RestoredZoomAmount(1.0f)
		, GeometryMeasurementZoomAmount(1.0f)
	{
	}

	FBlueprintLayoutGraphTarget::FBlueprintLayoutGraphTarget()
		: Graph(nullptr)
	{
	}



	static bool IsLayoutRerouteNode(const UEdGraphNode* Node)
	{
		return Cast<UK2Node_Knot>(Node) != nullptr;
	}

	int32 CountLayoutSemanticNodes(const UEdGraph* Graph)
	{
		if (!Graph)
		{
			return 0;
		}
		int32 Count = 0;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && !IsLayoutRerouteNode(Node))
			{
				++Count;
			}
		}
		return Count;
	}

	static FString GetBlueprintGraphType(const UBlueprint* Blueprint, const UEdGraph* Graph)
	{
		if (!Blueprint || !Graph)
		{
			return TEXT("unknown");
		}
		auto ContainsGraph = [Graph](const TArray<UEdGraph*>& Graphs) -> bool
		{
			for (const UEdGraph* Candidate : Graphs)
			{
				if (Candidate == Graph)
				{
					return true;
				}
			}
			return false;
		};
		if (ContainsGraph(Blueprint->UbergraphPages))
		{
			return TEXT("ubergraph");
		}
		if (ContainsGraph(Blueprint->FunctionGraphs))
		{
			return TEXT("function");
		}
		if (ContainsGraph(Blueprint->MacroGraphs))
		{
			return TEXT("macro");
		}
		if (ContainsGraph(Blueprint->DelegateSignatureGraphs))
		{
			return TEXT("delegate");
		}
		return TEXT("nested");
	}

	static void AddUniqueLayoutTarget(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FString& RequestedGraphType,
		TArray<FBlueprintLayoutGraphTarget>& OutTargets,
		TSet<UEdGraph*>& SeenGraphs)
	{
		if (!Graph || SeenGraphs.Contains(Graph))
		{
			return;
		}
		SeenGraphs.Add(Graph);
		FBlueprintLayoutGraphTarget Target;
		Target.Graph = Graph;
		Target.GraphType = RequestedGraphType.IsEmpty() ? GetBlueprintGraphType(Blueprint, Graph) : RequestedGraphType;
		OutTargets.Add(Target);
	}

	static bool AppendExplicitLayoutTarget(
		UBlueprint* Blueprint,
		const FString& GraphRef,
		TArray<FBlueprintLayoutGraphTarget>& OutTargets,
		TSet<UEdGraph*>& SeenGraphs,
		FString& OutError)
	{
		FString CleanGraphRef = GraphRef;
		CleanGraphRef.TrimStartAndEndInline();
		if (CleanGraphRef.IsEmpty())
		{
			return true;
		}
		UEdGraph* Graph = ResolveBlueprintGraph(Blueprint, CleanGraphRef);
		if (!Graph)
		{
			OutError = FString::Printf(TEXT("graph_not_found:%s"), *CleanGraphRef);
			return false;
		}
		AddUniqueLayoutTarget(Blueprint, Graph, GetBlueprintGraphType(Blueprint, Graph), OutTargets, SeenGraphs);
		return true;
	}

	static bool AppendExplicitLayoutTargetValue(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonValue>& Value,
		TArray<FBlueprintLayoutGraphTarget>& OutTargets,
		TSet<UEdGraph*>& SeenGraphs,
		FString& OutError)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			return true;
		}
		if (Value->Type == EJson::String)
		{
			return AppendExplicitLayoutTarget(Blueprint, Value->AsString(), OutTargets, SeenGraphs, OutError);
		}
		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				return true;
			}
			FString GraphRef;
			if (!Obj->TryGetStringField(TEXT("graph_path"), GraphRef))
			{
				Obj->TryGetStringField(TEXT("graph_name"), GraphRef);
			}
			if (GraphRef.IsEmpty())
			{
				Obj->TryGetStringField(TEXT("name"), GraphRef);
			}
			return AppendExplicitLayoutTarget(Blueprint, GraphRef, OutTargets, SeenGraphs, OutError);
		}
		OutError = TEXT("invalid_graph_selector");
		return false;
	}

	bool CollectBlueprintLayoutTargets(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Params,
		TArray<FBlueprintLayoutGraphTarget>& OutTargets,
		FString& OutError)
	{
		OutTargets.Reset();
		TSet<UEdGraph*> SeenGraphs;
		if (!Blueprint)
		{
			OutError = TEXT("blueprint_not_found");
			return false;
		}

		bool bHasExplicitSelectors = false;
		FString SingleGraphName;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("graph_name"), SingleGraphName) && !SingleGraphName.TrimStartAndEnd().IsEmpty())
		{
			bHasExplicitSelectors = true;
			if (!AppendExplicitLayoutTarget(Blueprint, SingleGraphName, OutTargets, SeenGraphs, OutError))
			{
				return false;
			}
		}
		if (Params.IsValid() && Params->TryGetStringField(TEXT("graph_path"), SingleGraphName) && !SingleGraphName.TrimStartAndEnd().IsEmpty())
		{
			bHasExplicitSelectors = true;
			if (!AppendExplicitLayoutTarget(Blueprint, SingleGraphName, OutTargets, SeenGraphs, OutError))
			{
				return false;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* GraphNameValues = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(TEXT("graph_names"), GraphNameValues) && GraphNameValues)
		{
			bHasExplicitSelectors = true;
			for (const TSharedPtr<FJsonValue>& Value : *GraphNameValues)
			{
				if (!AppendExplicitLayoutTargetValue(Blueprint, Value, OutTargets, SeenGraphs, OutError))
				{
					return false;
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* GraphValues = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(TEXT("graphs"), GraphValues) && GraphValues)
		{
			bHasExplicitSelectors = true;
			for (const TSharedPtr<FJsonValue>& Value : *GraphValues)
			{
				if (!AppendExplicitLayoutTargetValue(Blueprint, Value, OutTargets, SeenGraphs, OutError))
				{
					return false;
				}
			}
		}

		if (bHasExplicitSelectors)
		{
			return true;
		}

		bool bIncludeUbergraphs = true;
		bool bIncludeFunctionGraphs = true;
		bool bIncludeMacroGraphs = true;
		bool bIncludeDelegateGraphs = false;
		bool bIncludeNestedGraphs = false;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(TEXT("include_ubergraphs"), bIncludeUbergraphs);
			Params->TryGetBoolField(TEXT("include_function_graphs"), bIncludeFunctionGraphs);
			Params->TryGetBoolField(TEXT("include_macro_graphs"), bIncludeMacroGraphs);
			Params->TryGetBoolField(TEXT("include_delegate_graphs"), bIncludeDelegateGraphs);
			Params->TryGetBoolField(TEXT("include_nested_graphs"), bIncludeNestedGraphs);
		}

		auto AppendGraphArray = [&](const TArray<UEdGraph*>& Graphs, const FString& GraphType)
		{
			for (UEdGraph* Graph : Graphs)
			{
				AddUniqueLayoutTarget(Blueprint, Graph, GraphType, OutTargets, SeenGraphs);
			}
		};
		if (bIncludeUbergraphs)
		{
			AppendGraphArray(Blueprint->UbergraphPages, TEXT("ubergraph"));
		}
		if (bIncludeFunctionGraphs)
		{
			AppendGraphArray(Blueprint->FunctionGraphs, TEXT("function"));
		}
		if (bIncludeMacroGraphs)
		{
			AppendGraphArray(Blueprint->MacroGraphs, TEXT("macro"));
		}
		if (bIncludeDelegateGraphs)
		{
			AppendGraphArray(Blueprint->DelegateSignatureGraphs, TEXT("delegate"));
		}
		if (bIncludeNestedGraphs)
		{
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			for (UEdGraph* Graph : AllGraphs)
			{
				AddUniqueLayoutTarget(Blueprint, Graph, GetBlueprintGraphType(Blueprint, Graph), OutTargets, SeenGraphs);
			}
		}
		return true;
	}

	bool TryComputeGraphEditorAccessOffsetX(double& OutOffset)
	{
		OutOffset = 0.0;
		const UGraphEditorSettings* Settings = GetDefault<UGraphEditorSettings>();
		if (!Settings)
		{
			return false;
		}
		const float ReferenceDeltaX = FMath::Max(
			1.0f,
			FMath::Abs(Settings->PaddingTowardsNodeEdge) * 2.0f
			+ FMath::Abs(Settings->PaddingLeftOfOutput)
			+ FMath::Abs(Settings->PaddingRightOfInput));
		const FVector2f Tangent = Settings->ComputeSplineTangent(
			FVector2f::ZeroVector,
			FVector2f(ReferenceDeltaX, 0.0f));
		const double Offset = FMath::Abs(static_cast<double>(Tangent.X));
		if (!FMath::IsFinite(Offset) || Offset <= 0.0)
		{
			return false;
		}
		OutOffset = Offset;
		return true;
	}

	static bool IsExecPin(const UEdGraphPin* Pin)
	{
		return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	}

	static bool IsVisibleLayoutPin(const UEdGraphPin* Pin)
	{
		if (!Pin)
		{
			return false;
		}
		if (Pin->ParentPin)
		{
			return false;
		}
		return !Pin->bHidden || Pin->LinkedTo.Num() > 0 || IsExecPin(Pin);
	}

	static FString PinNameString(const UEdGraphPin* Pin)
	{
		return Pin ? Pin->GetName() : FString();
	}

	static int32 GetPinIndexInDirection(const UEdGraphPin* Pin)
	{
		if (!Pin)
		{
			return INDEX_NONE;
		}
		const UEdGraphNode* Node = Pin->GetOwningNode();
		if (!Node)
		{
			return INDEX_NONE;
		}
		int32 Index = 0;
		for (const UEdGraphPin* Candidate : Node->Pins)
		{
			if (!IsVisibleLayoutPin(Candidate) || Candidate->Direction != Pin->Direction)
			{
				continue;
			}
			if (Candidate == Pin)
			{
				return Index;
			}
			++Index;
		}
		return INDEX_NONE;
	}

	static TArray<UEdGraphPin*> GetLayoutPinsByDirection(UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		TArray<UEdGraphPin*> OutPins;
		if (!Node)
		{
			return OutPins;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (IsVisibleLayoutPin(Pin) && Pin->Direction == Direction)
			{
				OutPins.Add(Pin);
			}
		}
		return OutPins;
	}

	static void CollectNodePinsForLayout(UEdGraphNode* GraphNode, UeAgentBlueprintAutoLayout::FNode& OutNode)
	{
		const TArray<UEdGraphPin*> InputPins = GetLayoutPinsByDirection(GraphNode, EGPD_Input);
		const TArray<UEdGraphPin*> OutputPins = GetLayoutPinsByDirection(GraphNode, EGPD_Output);

		for (UEdGraphPin* Pin : InputPins)
		{
			OutNode.InputPinNames.Add(PinNameString(Pin));
			if (IsExecPin(Pin))
			{
				OutNode.bHasExecPin = true;
			}
		}
		for (UEdGraphPin* Pin : OutputPins)
		{
			OutNode.OutputPinNames.Add(PinNameString(Pin));
			if (IsExecPin(Pin))
			{
				OutNode.bHasExecPin = true;
			}
		}

		OutNode.InputPinAnchors.Init(FVector2D::ZeroVector, InputPins.Num());
		OutNode.OutputPinAnchors.Init(FVector2D::ZeroVector, OutputPins.Num());
		OutNode.InputPinAnchorValid.Init(0, InputPins.Num());
		OutNode.OutputPinAnchorValid.Init(0, OutputPins.Num());
	}

	static bool IsFiniteLayoutPoint(const FVector2D& Point)
	{
		return FMath::IsFinite(Point.X) && FMath::IsFinite(Point.Y);
	}

	static bool IsUsableSlateGeometry(const FGeometry& Geometry)
	{
		const FVector2f Size = Geometry.GetLocalSize();
		const FVector2f AbsolutePosition = Geometry.GetAbsolutePosition();
		return Size.X > 0.5f
			&& Size.Y > 0.5f
			&& FMath::IsFinite(AbsolutePosition.X)
			&& FMath::IsFinite(AbsolutePosition.Y);
	}

	static bool TryGetUsableWidgetGeometry(const SWidget& Widget, FGeometry& OutGeometry)
	{
		const FGeometry TickGeometry = Widget.GetTickSpaceGeometry();
		if (IsUsableSlateGeometry(TickGeometry))
		{
			OutGeometry = TickGeometry;
			return true;
		}
		const FGeometry CachedGeometry = Widget.GetCachedGeometry();
		if (IsUsableSlateGeometry(CachedGeometry))
		{
			OutGeometry = CachedGeometry;
			return true;
		}
		return false;
	}

	static void SetMeasuredPinAnchor(
		UeAgentBlueprintAutoLayout::FNode& Node,
		const EEdGraphPinDirection Direction,
		const int32 PinIndex,
		const FVector2D& Anchor)
	{
		if (PinIndex == INDEX_NONE || !IsFiniteLayoutPoint(Anchor))
		{
			return;
		}
		if (Direction == EGPD_Input)
		{
			if (Node.InputPinAnchors.IsValidIndex(PinIndex) && Node.InputPinAnchorValid.IsValidIndex(PinIndex))
			{
				Node.InputPinAnchors[PinIndex] = Anchor;
				Node.InputPinAnchorValid[PinIndex] = 1;
			}
		}
		else if (Direction == EGPD_Output)
		{
			if (Node.OutputPinAnchors.IsValidIndex(PinIndex) && Node.OutputPinAnchorValid.IsValidIndex(PinIndex))
			{
				Node.OutputPinAnchors[PinIndex] = Anchor;
				Node.OutputPinAnchorValid[PinIndex] = 1;
			}
		}
	}

	static bool TryGetMeasuredPinAnchor(
		const UeAgentBlueprintAutoLayout::FNode& Node,
		const EEdGraphPinDirection Direction,
		const int32 PinIndex,
		FVector2D& OutAnchor)
	{
		if (PinIndex == INDEX_NONE)
		{
			return false;
		}
		if (Direction == EGPD_Input)
		{
			if (Node.InputPinAnchors.IsValidIndex(PinIndex)
				&& Node.InputPinAnchorValid.IsValidIndex(PinIndex)
				&& Node.InputPinAnchorValid[PinIndex] != 0)
			{
				OutAnchor = Node.InputPinAnchors[PinIndex];
				return true;
			}
		}
		else if (Direction == EGPD_Output)
		{
			if (Node.OutputPinAnchors.IsValidIndex(PinIndex)
				&& Node.OutputPinAnchorValid.IsValidIndex(PinIndex)
				&& Node.OutputPinAnchorValid[PinIndex] != 0)
			{
				OutAnchor = Node.OutputPinAnchors[PinIndex];
				return true;
			}
		}
		return false;
	}

	static int32 CountMeasuredPinAnchors(const UeAgentBlueprintAutoLayout::FNode& Node, const EEdGraphPinDirection Direction)
	{
		const TArray<uint8>& Valid = Direction == EGPD_Input ? Node.InputPinAnchorValid : Node.OutputPinAnchorValid;
		int32 Count = 0;
		for (const uint8 bValid : Valid)
		{
			if (bValid != 0)
			{
				++Count;
			}
		}
		return Count;
	}

	static bool TryConvertAbsolutePinPointToNodeAnchor(
		SGraphPanel& GraphPanel,
		const SGraphNode& GraphNodeWidget,
		const FVector2D& NodeGraphPosition,
		const FVector2f& PinPointAbsolute,
		FVector2D& OutAnchor)
	{
		FGeometry NodeGeometry;
		if (TryGetUsableWidgetGeometry(GraphNodeWidget, NodeGeometry))
		{
			const float ZoomAmount = FMath::Max(GraphPanel.GetZoomAmount(), 0.01f);
			const FVector2f NodeAbsolute = NodeGeometry.GetAbsolutePosition();
			const FVector2f LocalGraphOffset = (PinPointAbsolute - NodeAbsolute) / ZoomAmount;
			OutAnchor = FVector2D(static_cast<double>(LocalGraphOffset.X), static_cast<double>(LocalGraphOffset.Y));
			if (IsFiniteLayoutPoint(OutAnchor))
			{
				return true;
			}
		}

		FGeometry PanelGeometry;
		if (!TryGetUsableWidgetGeometry(GraphPanel, PanelGeometry))
		{
			PanelGeometry = FGeometry::MakeRoot(FVector2f(4096.0f, 4096.0f), FSlateLayoutTransform(1.0f));
		}
		const FVector2f PinCenterPanel = PanelGeometry.AbsoluteToLocal(PinPointAbsolute);
		const FVector2f PinCenterGraph = GraphPanel.PanelCoordToGraphCoord(PinCenterPanel);
		OutAnchor = FVector2D(
			static_cast<double>(PinCenterGraph.X) - NodeGraphPosition.X,
			static_cast<double>(PinCenterGraph.Y) - NodeGraphPosition.Y);
		return IsFiniteLayoutPoint(OutAnchor);
	}

	static void FindArrangedDescendantGeometries(
		const SWidget& Widget,
		const FGeometry& WidgetGeometry,
		const TSet<TSharedRef<SWidget>>& WidgetsToFind,
		TMap<TSharedRef<SWidget>, FArrangedWidget>& OutResult)
	{
		FArrangedChildren ArrangedChildren(EVisibility::Visible);
		Widget.ArrangeChildren(WidgetGeometry, ArrangedChildren, true);
		for (int32 ChildIndex = 0; ChildIndex < ArrangedChildren.Num(); ++ChildIndex)
		{
			const FArrangedWidget& Child = ArrangedChildren[ChildIndex];
			if (WidgetsToFind.Contains(Child.Widget))
			{
				OutResult.Add(Child.Widget, Child);
			}
		}
		if (OutResult.Num() == WidgetsToFind.Num())
		{
			return;
		}
		for (int32 ChildIndex = 0; ChildIndex < ArrangedChildren.Num(); ++ChildIndex)
		{
			const FArrangedWidget& Child = ArrangedChildren[ChildIndex];
			FindArrangedDescendantGeometries(Child.Widget.Get(), Child.Geometry, WidgetsToFind, OutResult);
			if (OutResult.Num() == WidgetsToFind.Num())
			{
				return;
			}
		}
	}

	static FVector2f GetGraphPinConnectionPointAbsolute(
		const FGeometry& PinGeometry,
		const EEdGraphPinDirection Direction,
		const float ZoomAmount)
	{
		// Match FConnectionDrawingPolicy::DrawSplineWithArrow(StartGeom, EndGeom).
		constexpr float StartFudgeX = 4.0f;
		constexpr float EndFudgeX = 4.0f;
		const FSlateBrush* ArrowImage = FAppStyle::GetBrush(TEXT("Graph.Arrow"));
		const float ArrowRadiusX = ArrowImage
			? ArrowImage->ImageSize.X * ZoomAmount * 0.5f
			: 0.0f;
		const FVector2f DrawSize = PinGeometry.GetDrawSize();
		const float CenterY = PinGeometry.AbsolutePosition.Y + DrawSize.Y * 0.5f;
		if (Direction == EGPD_Input)
		{
			return FVector2f(
				PinGeometry.AbsolutePosition.X - (ArrowRadiusX - EndFudgeX),
				CenterY);
		}
		return FVector2f(
			PinGeometry.AbsolutePosition.X + DrawSize.X - StartFudgeX,
			CenterY);
	}

	static bool TryGetCachedPinNodeOffsetY(const SGraphPin& PinWidget, double& OutNodeOffsetY)
	{
		// Match SGraphPanel's native alignment rule:
		// NodePosY + SGraphPin::GetNodeOffset().Y. Minimized automation can leave
		// this cache at 0 before the pin widget ticks, so 0 is treated as missing.
		const FVector2f CachedPinNodeOffset = UE::Slate::CastToVector2f(PinWidget.GetNodeOffset());
		if (!FMath::IsFinite(CachedPinNodeOffset.Y) || FMath::Abs(CachedPinNodeOffset.Y) <= 0.5f)
		{
			return false;
		}
		OutNodeOffsetY = static_cast<double>(CachedPinNodeOffset.Y);
		return true;
	}

	static bool TryGetArrangedPinAnchor(
		SGraphPanel& GraphPanel,
		SGraphNode& GraphNodeWidget,
		const TSharedRef<SGraphPin>& PinWidget,
		const FVector2D& NodeGraphPosition,
		const EEdGraphPinDirection Direction,
		FVector2D& OutAnchor)
	{
		FGeometry PanelGeometry;
		if (!TryGetUsableWidgetGeometry(GraphPanel, PanelGeometry))
		{
			PanelGeometry = FGeometry::MakeRoot(FVector2f(4096.0f, 4096.0f), FSlateLayoutTransform(1.0f));
		}

		const float ZoomAmount = FMath::Max(GraphPanel.GetZoomAmount(), 0.01f);
		const float WidgetScale = FSlateApplication::IsInitialized()
			? FSlateApplication::Get().GetApplicationScale()
			: 1.0f;
		GraphNodeWidget.SlatePrepass(WidgetScale * ZoomAmount);

		TSet<TSharedRef<SWidget>> WidgetsToFind;
		WidgetsToFind.Add(PinWidget);

		TMap<TSharedRef<SWidget>, FArrangedWidget> ArrangedWidgets;
		FGeometry ArrangedNodeGeometry;
		bool bHasArrangedNodeGeometry = false;
		{
			const auto ViewOffset = GraphPanel.GetViewOffset();
			const FVector2f NodePanelOffset(
				GraphNodeWidget.GetPosition2f().X - static_cast<float>(ViewOffset.X),
				GraphNodeWidget.GetPosition2f().Y - static_cast<float>(ViewOffset.Y));
			const FArrangedWidget ArrangedNode = PanelGeometry.MakeChild(
				GraphNodeWidget.AsShared(),
				NodePanelOffset,
				GraphNodeWidget.GetDesiredSize(),
				ZoomAmount);
			ArrangedNodeGeometry = ArrangedNode.Geometry;
			bHasArrangedNodeGeometry = true;
			FindArrangedDescendantGeometries(GraphNodeWidget, ArrangedNode.Geometry, WidgetsToFind, ArrangedWidgets);
		}

		if (ArrangedWidgets.Num() < WidgetsToFind.Num())
		{
			FindArrangedDescendantGeometries(GraphPanel, PanelGeometry, WidgetsToFind, ArrangedWidgets);
		}

		const FArrangedWidget* PinGeometry = ArrangedWidgets.Find(PinWidget);
		FGeometry FallbackPinGeometry;
		if (!PinGeometry && TryGetUsableWidgetGeometry(PinWidget.Get(), FallbackPinGeometry))
		{
			ArrangedWidgets.Add(PinWidget, FArrangedWidget(PinWidget, FallbackPinGeometry));
			PinGeometry = ArrangedWidgets.Find(PinWidget);
		}
		if (!PinGeometry)
		{
			return false;
		}
		auto TryComputeArrangedNodeOffsetY = [&ArrangedNodeGeometry, bHasArrangedNodeGeometry](
			const FGeometry& Geometry,
			double& OutNodeOffsetY) -> bool
		{
			if (!bHasArrangedNodeGeometry)
			{
				return false;
			}
			const float PinScale = FMath::Max(Geometry.Scale, 0.01f);
			const float NodeScale = FMath::Max(ArrangedNodeGeometry.Scale, 0.01f);
			const FVector2f NodeOffset = Geometry.AbsolutePosition / PinScale - ArrangedNodeGeometry.AbsolutePosition / NodeScale;
			const float CenterY = NodeOffset.Y + Geometry.Size.Y * 0.5f;
			if (!FMath::IsFinite(CenterY))
			{
				return false;
			}
			OutNodeOffsetY = static_cast<double>(CenterY);
			return true;
		};

		const FVector2f PinPointAbsolute = GetGraphPinConnectionPointAbsolute(
			PinGeometry->Geometry,
			Direction,
			ZoomAmount);
		double PinNodeOffsetY = 0.0;
		if (!TryGetCachedPinNodeOffsetY(PinWidget.Get(), PinNodeOffsetY)
			&& !TryComputeArrangedNodeOffsetY(PinGeometry->Geometry, PinNodeOffsetY))
		{
			return false;
		}

		if (bHasArrangedNodeGeometry)
		{
			const FVector2f NodeAbsolute = ArrangedNodeGeometry.GetAbsolutePosition();
			const FVector2f LocalGraphOffset = (PinPointAbsolute - NodeAbsolute) / ZoomAmount;
			OutAnchor = FVector2D(static_cast<double>(LocalGraphOffset.X), PinNodeOffsetY);
			if (IsFiniteLayoutPoint(OutAnchor))
			{
				return true;
			}
		}

		FVector2D ConvertedAnchor;
		if (TryConvertAbsolutePinPointToNodeAnchor(
			GraphPanel,
			GraphNodeWidget,
			NodeGraphPosition,
			PinPointAbsolute,
			ConvertedAnchor))
		{
			ConvertedAnchor.Y = PinNodeOffsetY;
			OutAnchor = ConvertedAnchor;
			return true;
		}
		return false;
	}

	static bool TryGetTickSpacePinAnchor(
		SGraphPanel& GraphPanel,
		SGraphNode& GraphNodeWidget,
		const TSharedRef<SGraphPin>& PinWidget,
		const FVector2D& NodeGraphPosition,
		const EEdGraphPinDirection Direction,
		FVector2D& OutAnchor)
	{
		if (TryGetArrangedPinAnchor(GraphPanel, GraphNodeWidget, PinWidget, NodeGraphPosition, Direction, OutAnchor))
		{
			return true;
		}

		FGeometry PinGeometry;
		if (!TryGetUsableWidgetGeometry(PinWidget.Get(), PinGeometry))
		{
			return false;
		}
		const float ZoomAmount = FMath::Max(GraphPanel.GetZoomAmount(), 0.01f);
		const FVector2f PinConnectionPointAbsolute = GetGraphPinConnectionPointAbsolute(
			PinGeometry,
			Direction,
			ZoomAmount);

		FVector2D ConvertedAnchor;
		if (!TryConvertAbsolutePinPointToNodeAnchor(
			GraphPanel,
			GraphNodeWidget,
			NodeGraphPosition,
			PinConnectionPointAbsolute,
			ConvertedAnchor))
		{
			return false;
		}
		double PinNodeOffsetY = 0.0;
		if (!TryGetCachedPinNodeOffsetY(PinWidget.Get(), PinNodeOffsetY))
		{
			return false;
		}
		ConvertedAnchor.Y = PinNodeOffsetY;
		OutAnchor = ConvertedAnchor;
		return true;
	}

	static UEdGraphPin* FindLayoutPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, int32 PinIndex, const FString& PinName)
	{
		if (!Node)
		{
			return nullptr;
		}
		UEdGraphPin* NameFallback = nullptr;
		int32 Index = 0;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!IsVisibleLayoutPin(Pin) || Pin->Direction != Direction)
			{
				continue;
			}
			if (Index == PinIndex)
			{
				return Pin;
			}
			if (!NameFallback && PinNameString(Pin) == PinName)
			{
				NameFallback = Pin;
			}
			++Index;
		}
		return NameFallback;
	}

	static void CollectTerminalLinkedPins(UEdGraphPin* LinkedPin, TArray<UEdGraphPin*>& OutPins, TSet<UEdGraphPin*>& VisitedPins)
	{
		if (!LinkedPin || VisitedPins.Contains(LinkedPin))
		{
			return;
		}
		VisitedPins.Add(LinkedPin);
		UEdGraphNode* Owner = LinkedPin->GetOwningNode();
		UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Owner);
		if (!Knot)
		{
			OutPins.Add(LinkedPin);
			return;
		}

		UEdGraphPin* NextPin = nullptr;
		if (LinkedPin == Knot->GetInputPin())
		{
			NextPin = Knot->GetOutputPin();
		}
		else if (LinkedPin == Knot->GetOutputPin())
		{
			NextPin = Knot->GetInputPin();
		}
		if (!NextPin)
		{
			return;
		}
		for (UEdGraphPin* NextLinked : NextPin->LinkedTo)
		{
			CollectTerminalLinkedPins(NextLinked, OutPins, VisitedPins);
		}
	}

	FGraphLayoutModel CollectGraphLayoutModel(UEdGraph* Graph)
	{
		FGraphLayoutModel Model;
		if (!Graph)
		{
			return Model;
		}

		for (UEdGraphNode* GraphNode : Graph->Nodes)
		{
			if (!GraphNode || IsLayoutRerouteNode(GraphNode))
			{
				continue;
			}
			UeAgentBlueprintAutoLayout::FNode Node;
			Node.Index = Model.Nodes.Num();
			Node.Guid = GraphNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
			Node.Title = GraphNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
			Node.ClassName = GraphNode->GetClass()->GetName();
			Node.OldX = GraphNode->NodePosX;
			Node.OldY = GraphNode->NodePosY;
			Node.NewX = Node.OldX;
			Node.NewY = Node.OldY;
			if (const UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(GraphNode))
			{
				Node.bIsCommentNode = true;
				Node.CommentDepth = CommentNode->CommentDepth;
			}
			CollectNodePinsForLayout(GraphNode, Node);

			Model.NodeToIndex.Add(GraphNode, Node.Index);
			Model.GuidToNode.Add(Node.Guid, GraphNode);
			Model.GraphNodes.Add(GraphNode);
			Model.Nodes.Add(Node);
		}

		TSet<FString> EdgeKeys;
		for (UEdGraphNode* SourceNode : Model.GraphNodes)
		{
			const int32* SourceIndexPtr = Model.NodeToIndex.Find(SourceNode);
			if (!SourceIndexPtr)
			{
				continue;
			}
			for (UEdGraphPin* SourcePin : SourceNode->Pins)
			{
				if (!IsVisibleLayoutPin(SourcePin) || SourcePin->Direction != EGPD_Output)
				{
					continue;
				}
				for (UEdGraphPin* LinkedPin : SourcePin->LinkedTo)
				{
					TArray<UEdGraphPin*> TerminalPins;
					TSet<UEdGraphPin*> VisitedPins;
					CollectTerminalLinkedPins(LinkedPin, TerminalPins, VisitedPins);
					for (UEdGraphPin* TargetPin : TerminalPins)
					{
						if (!TargetPin || TargetPin->Direction != EGPD_Input)
						{
							continue;
						}
						UEdGraphNode* TargetNode = TargetPin->GetOwningNode();
						const int32* TargetIndexPtr = Model.NodeToIndex.Find(TargetNode);
						if (!TargetIndexPtr)
						{
							continue;
						}
						const FString EdgeKey = FString::Printf(
							TEXT("%d:%d:%s:%s:%d:%d"),
							*SourceIndexPtr,
							*TargetIndexPtr,
							*PinNameString(SourcePin),
							*PinNameString(TargetPin),
							GetPinIndexInDirection(SourcePin),
							GetPinIndexInDirection(TargetPin));
						if (EdgeKeys.Contains(EdgeKey))
						{
							continue;
						}
						EdgeKeys.Add(EdgeKey);

						UeAgentBlueprintAutoLayout::FEdge Edge;
						Edge.Source = *SourceIndexPtr;
						Edge.Target = *TargetIndexPtr;
						Edge.SourcePin = PinNameString(SourcePin);
						Edge.TargetPin = PinNameString(TargetPin);
						Edge.SourcePinIndex = GetPinIndexInDirection(SourcePin);
						Edge.TargetPinIndex = GetPinIndexInDirection(TargetPin);
						Edge.bExec = IsExecPin(SourcePin) || IsExecPin(TargetPin);
						Model.Edges.Add(Edge);
					}
				}
			}
		}
		return Model;
	}

	bool CaptureGraphLayoutRealGeometry(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		FGraphLayoutModel& Model,
		FGraphLayoutGeometryStats& OutStats,
		const bool bAllowGeometryViewCapture,
		const bool bAllowFocusedGeometryCapture,
		FString& OutError)
	{
		OutStats = FGraphLayoutGeometryStats();
		OutStats.NodeCount = Model.Nodes.Num();
		OutStats.bRealGeometryCaptureRequested = true;
		OutStats.bGeometryViewCaptureAllowed = bAllowGeometryViewCapture;
		OutStats.bFocusedGeometryCaptureAllowed = bAllowFocusedGeometryCapture;
		if (!Blueprint || !Graph)
		{
			OutError = TEXT("blueprint_or_graph_missing");
			OutStats.Error = OutError;
			return false;
		}
		if (!FSlateApplication::IsInitialized())
		{
			OutError = TEXT("slate_not_initialized_for_real_geometry");
			OutStats.Error = OutError;
			return false;
		}

		const double CaptureStartSeconds = FPlatformTime::Seconds();
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutGeometry start asset=%s graph=%s nodes=%d edges=%d"),
			*Blueprint->GetPathName(),
			*Graph->GetName(),
			Model.Nodes.Num(),
			Model.Edges.Num());
		TSharedPtr<IBlueprintEditor> BlueprintEditor;
		TSharedPtr<SGraphEditor> OwnedGeometryGraphEditor;
		TSharedPtr<SGraphEditor> GraphEditor;
		const bool bUseLiveGraphEditorForGeometry = bAllowGeometryViewCapture || bAllowFocusedGeometryCapture;
		if (bUseLiveGraphEditorForGeometry)
		{
			BlueprintEditor = UeAgentBlueprintOps::GetBlueprintEditor(Blueprint, true, OutError);
			if (!BlueprintEditor.IsValid())
			{
				OutStats.Error = OutError;
				return false;
			}
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutGeometry editor_ready elapsed_ms=%.1f"),
				(FPlatformTime::Seconds() - CaptureStartSeconds) * 1000.0);
			GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(Graph, true);
			if (!GraphEditor.IsValid())
			{
				OutError = TEXT("graph_editor_not_found_for_real_geometry");
				OutStats.Error = OutError;
				return false;
			}
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutGeometry live_graph_editor_ready elapsed_ms=%.1f"),
				(FPlatformTime::Seconds() - CaptureStartSeconds) * 1000.0);
		}
		else
		{
			OwnedGeometryGraphEditor = UeAgentBlueprintOps::CreateOffscreenGraphEditorForCapture(
				Graph,
				TEXT("UAI real geometry probe"),
				OutError);
			if (!OwnedGeometryGraphEditor.IsValid())
			{
				OutStats.Error = OutError;
				return false;
			}
			GraphEditor = OwnedGeometryGraphEditor;
			OutStats.bUsedOffscreenGraphEditor = true;
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutGeometry offscreen_graph_editor_ready elapsed_ms=%.1f"),
				(FPlatformTime::Seconds() - CaptureStartSeconds) * 1000.0);
		}
		SGraphPanel* GraphPanel = GraphEditor->GetGraphPanel();
		if (!GraphPanel)
		{
			OutError = TEXT("graph_panel_not_found_for_real_geometry");
			OutStats.Error = OutError;
			return false;
		}
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutGeometry graph_panel_ready elapsed_ms=%.1f"),
			(FPlatformTime::Seconds() - CaptureStartSeconds) * 1000.0);

		FVector2f SavedViewLocation(0.0f, 0.0f);
		float SavedZoom = 1.0f;
		const bool bUseDocumentViewStateForRestore = UeAgentBlueprintOps::TryGetBlueprintGraphDocumentViewState(
			Blueprint,
			Graph,
			SavedViewLocation,
			SavedZoom);
		OutStats.bRestoreViewStateFromDocument = bUseDocumentViewStateForRestore;
		if (!bUseDocumentViewStateForRestore)
		{
			GraphEditor->GetViewLocation(SavedViewLocation, SavedZoom);
			SavedViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(SavedViewLocation, SavedZoom);
		}
		auto RestoreGraphView = [&]()
		{
			if (bUseDocumentViewStateForRestore)
			{
				OutStats.bRestoredGraphViewAfterCapture = UeAgentBlueprintOps::RestoreBlueprintGraphEditorToDocumentViewState(
					Blueprint,
					Graph,
					GraphEditor,
					&OutStats.RestoredViewLocation,
					&OutStats.RestoredZoomAmount);
				if (OutStats.bRestoredGraphViewAfterCapture)
				{
					if (FSlateApplication::IsInitialized())
					{
						FSlateApplication::Get().PumpMessages();
					}
					return;
				}
			}
			FVector2f RestoreViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(SavedViewLocation, SavedZoom);
			GraphEditor->SetViewLocation(RestoreViewLocation, SavedZoom);
			float ActualZoom = SavedZoom;
			GraphEditor->GetViewLocation(RestoreViewLocation, ActualZoom);
			if (!UeAgentGraphViewUtils::IsViewLocationPixelAligned(RestoreViewLocation, ActualZoom))
			{
				RestoreViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(RestoreViewLocation, ActualZoom);
				GraphEditor->SetViewLocation(RestoreViewLocation, ActualZoom);
			}
			OutStats.bRestoredGraphViewAfterCapture = true;
			OutStats.RestoredViewLocation = RestoreViewLocation;
			OutStats.RestoredZoomAmount = ActualZoom;
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().PumpMessages();
			}
		};

		FString WindowError;
		const TSharedPtr<SWindow> BlueprintWindow = UeAgentBlueprintOps::ResolveBlueprintEditorWindow(BlueprintEditor, WindowError);
		if (BlueprintWindow.IsValid() && bAllowGeometryViewCapture)
		{
			if (!UeAgentBlueprintOps::PrepareSlateWindowForCapture(BlueprintWindow, OutError))
			{
				OutStats.Error = OutError;
				RestoreGraphView();
				return false;
			}
			OutStats.bWindowPrepared = true;
		}
		else
		{
			FSlateApplication::Get().PumpMessages();
			const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
			GraphEditor->SlatePrepass(WidgetScale);
			GraphPanel->SlatePrepass(WidgetScale);
		}
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutGeometry initial_prepass_done elapsed_ms=%.1f"),
			(FPlatformTime::Seconds() - CaptureStartSeconds) * 1000.0);

		constexpr float CanonicalGeometryMeasurementZoom = 1.0f;
		OutStats.GeometryMeasurementZoomAmount = CanonicalGeometryMeasurementZoom;
		{
			const FVector2f MeasurementViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(
				SavedViewLocation,
				CanonicalGeometryMeasurementZoom);
			GraphEditor->SetViewLocation(MeasurementViewLocation, CanonicalGeometryMeasurementZoom);
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().PumpMessages();
			}
		}

		auto RefreshGraphPanelGeometry = [&](const int32 FrameCount)
		{
			for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
			{
				GraphPanel->Update();
				UeAgentBlueprintOps::PumpAndTickSlateForCapture();
				const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
				GraphEditor->SlatePrepass(WidgetScale);
				GraphPanel->SlatePrepass(WidgetScale);
				if (BlueprintWindow.IsValid())
				{
					if (bAllowGeometryViewCapture)
					{
						FSlateApplication::Get().ForceRedrawWindow(BlueprintWindow.ToSharedRef());
						UeAgentBlueprintOps::PumpAndTickSlateForCapture();
					}
				}
			}
		};
		auto RefreshGraphPanelLayoutOnly = [&](const int32 FrameCount)
		{
			for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
			{
				GraphPanel->Update();
				UeAgentBlueprintOps::PumpAndTickSlateForCapture();
				const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
				GraphEditor->SlatePrepass(WidgetScale);
				GraphPanel->SlatePrepass(WidgetScale);
			}
		};

		for (UEdGraphNode* GraphNode : Model.GraphNodes)
		{
			if (GraphNode)
			{
				GraphPanel->RefreshNode(*GraphNode);
			}
		}
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutGeometry refresh_nodes_done elapsed_ms=%.1f"),
			(FPlatformTime::Seconds() - CaptureStartSeconds) * 1000.0);
		RefreshGraphPanelGeometry(2);
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutGeometry refresh_geometry_done elapsed_ms=%.1f"),
			(FPlatformTime::Seconds() - CaptureStartSeconds) * 1000.0);

		for (UeAgentBlueprintAutoLayout::FNode& Node : Model.Nodes)
		{
			Node.InputPinAnchorValid.Init(0, Node.InputPinAnchors.Num());
			Node.OutputPinAnchorValid.Init(0, Node.OutputPinAnchors.Num());
		}

		TArray<TSet<int32>> UsedInputPinIndices;
		TArray<TSet<int32>> UsedOutputPinIndices;
		UsedInputPinIndices.SetNum(Model.Nodes.Num());
		UsedOutputPinIndices.SetNum(Model.Nodes.Num());
		for (const UeAgentBlueprintAutoLayout::FEdge& Edge : Model.Edges)
		{
			if (UsedOutputPinIndices.IsValidIndex(Edge.Source) && Edge.SourcePinIndex != INDEX_NONE)
			{
				UsedOutputPinIndices[Edge.Source].Add(Edge.SourcePinIndex);
			}
			if (UsedInputPinIndices.IsValidIndex(Edge.Target) && Edge.TargetPinIndex != INDEX_NONE)
			{
				UsedInputPinIndices[Edge.Target].Add(Edge.TargetPinIndex);
			}
		}
		auto CountMissingUsedPinAnchors = [&](
			const int32 NodeIndex,
			const EEdGraphPinDirection Direction) -> int32
		{
			if (!Model.Nodes.IsValidIndex(NodeIndex))
			{
				return 0;
			}
			const UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[NodeIndex];
			const TSet<int32>& UsedPins = Direction == EGPD_Input ? UsedInputPinIndices[NodeIndex] : UsedOutputPinIndices[NodeIndex];
			int32 Missing = 0;
			for (const int32 PinIndex : UsedPins)
			{
				FVector2D Anchor = FVector2D::ZeroVector;
				if (!TryGetMeasuredPinAnchor(Node, Direction, PinIndex, Anchor))
				{
					++Missing;
				}
			}
			return Missing;
		};

		auto CapturePinAnchorsForNode = [&](UeAgentBlueprintAutoLayout::FNode& LayoutNode, UEdGraphNode* GraphNode, bool bCountPinWidgets)
		{
			if (!GraphNode)
			{
				return;
			}
			TSharedPtr<SGraphNode> GraphNodeWidget = GraphPanel->GetNodeWidgetFromGuid(GraphNode->NodeGuid);
			if (!GraphNodeWidget.IsValid())
			{
				return;
			}
			for (UEdGraphPin* Pin : GraphNode->Pins)
			{
				if (!IsVisibleLayoutPin(Pin) || (Pin->Direction != EGPD_Input && Pin->Direction != EGPD_Output))
				{
					continue;
				}
				const int32 PinIndex = GetPinIndexInDirection(Pin);
				if (PinIndex == INDEX_NONE)
				{
					continue;
				}
				FVector2D ExistingAnchor = FVector2D::ZeroVector;
				if (TryGetMeasuredPinAnchor(LayoutNode, Pin->Direction, PinIndex, ExistingAnchor))
				{
					continue;
				}
				TSharedPtr<SGraphPin> PinWidget = GraphNodeWidget->FindWidgetForPin(Pin);
				if (!PinWidget.IsValid())
				{
					continue;
				}
				if (bCountPinWidgets)
				{
					++OutStats.PinWidgetCount;
				}
				FVector2D Anchor = FVector2D::ZeroVector;
				const FVector2f WidgetNodeGraphPosition2f = GraphNodeWidget->GetPosition2f();
				const FVector2D WidgetNodeGraphPosition(static_cast<double>(WidgetNodeGraphPosition2f.X), static_cast<double>(WidgetNodeGraphPosition2f.Y));
				if (TryGetTickSpacePinAnchor(*GraphPanel, *GraphNodeWidget, PinWidget.ToSharedRef(), WidgetNodeGraphPosition, Pin->Direction, Anchor))
				{
					SetMeasuredPinAnchor(LayoutNode, Pin->Direction, PinIndex, Anchor);
					++OutStats.PinGeometryAnchorCount;
					continue;
				}
			}
		};

		auto CaptureNodeSize = [&](UeAgentBlueprintAutoLayout::FNode& LayoutNode, UEdGraphNode* GraphNode)
		{
			if (!GraphNode)
			{
				return false;
			}
			struct FMeasuredNodeSize
			{
				bool bValid = false;
				double Width = 0.0;
				double Height = 0.0;
			};
			auto MakeMeasuredSize = [](double Width, double Height)
			{
				FMeasuredNodeSize Size;
				if (FMath::IsFinite(Width) && FMath::IsFinite(Height) && Width > 1.0 && Height > 1.0)
				{
					Size.bValid = true;
					Size.Width = Width;
					Size.Height = Height;
				}
				return Size;
			};
			auto ChooseMeasuredSize = [](const FMeasuredNodeSize& DesiredSize, const FMeasuredNodeSize& BoundsSize)
			{
				return DesiredSize.bValid ? DesiredSize : BoundsSize;
			};
			FMeasuredNodeSize DesiredSize;
			TSharedPtr<SGraphNode> GraphNodeWidget = GraphPanel->GetNodeWidgetFromGuid(GraphNode->NodeGuid);
			if (GraphNodeWidget.IsValid())
			{
				const FVector2D Desired = GraphNodeWidget->GetDesiredSize();
				DesiredSize = MakeMeasuredSize(Desired.X, Desired.Y);
			}
			FMeasuredNodeSize BoundsSize;
			FSlateRect Bounds;
			if (GraphEditor->GetBoundsForNode(GraphNode, Bounds, 0.0f))
			{
				BoundsSize = MakeMeasuredSize(
					static_cast<double>(Bounds.Right - Bounds.Left),
					static_cast<double>(Bounds.Bottom - Bounds.Top));
			}
			const FMeasuredNodeSize ChosenSize = ChooseMeasuredSize(DesiredSize, BoundsSize);
			const bool bMeasuredNode = ChosenSize.bValid;
			if (bMeasuredNode)
			{
				LayoutNode.Width = FMath::Clamp(ChosenSize.Width, 1.0, 5000.0);
				LayoutNode.Height = FMath::Clamp(ChosenSize.Height, 1.0, 5000.0);
			}
			LayoutNode.bMeasuredGeometry = bMeasuredNode;
			return bMeasuredNode;
		};

		for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
		{
			UEdGraphNode* GraphNode = Model.GraphNodes.IsValidIndex(NodeIndex) ? Model.GraphNodes[NodeIndex] : nullptr;
			if (!GraphNode)
			{
				continue;
			}
			if (CaptureNodeSize(Model.Nodes[NodeIndex], GraphNode))
			{
				++OutStats.MeasuredNodeCount;
			}
			CapturePinAnchorsForNode(Model.Nodes[NodeIndex], GraphNode, true);
		}
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutGeometry initial_capture_done elapsed_ms=%.1f measured_nodes=%d pin_widgets=%d anchors=%d"),
			(FPlatformTime::Seconds() - CaptureStartSeconds) * 1000.0,
			OutStats.MeasuredNodeCount,
			OutStats.PinWidgetCount,
			OutStats.PinGeometryAnchorCount);

		const double FocusedCaptureStartSeconds = FPlatformTime::Seconds();
		constexpr double MaxFocusedCaptureSeconds = 120.0;
		constexpr int32 MaxFocusedCapturePasses = 2;
		auto CountAllMissingUsedPinAnchors = [&]()
		{
			int32 Missing = 0;
			for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
			{
				Missing += CountMissingUsedPinAnchors(NodeIndex, EGPD_Input);
				Missing += CountMissingUsedPinAnchors(NodeIndex, EGPD_Output);
			}
			return Missing;
		};
		const bool bRunFocusedLayoutProbe = bAllowFocusedGeometryCapture;
		if (bRunFocusedLayoutProbe)
		{
			for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
			{
				if ((FPlatformTime::Seconds() - FocusedCaptureStartSeconds) > MaxFocusedCaptureSeconds)
				{
					OutStats.bFocusedGeometryBudgetExhausted = true;
					break;
				}
				UEdGraphNode* GraphNode = Model.GraphNodes.IsValidIndex(NodeIndex) ? Model.GraphNodes[NodeIndex] : nullptr;
				if (!GraphNode)
				{
					continue;
				}
				const int32 MissingForNode =
					CountMissingUsedPinAnchors(NodeIndex, EGPD_Input)
					+ CountMissingUsedPinAnchors(NodeIndex, EGPD_Output);
				if (MissingForNode <= 0 && Model.Nodes[NodeIndex].bMeasuredGeometry)
				{
					continue;
				}
				const FVector2f FocusedViewLocation(
					FMath::RoundToFloat(static_cast<float>(Model.Nodes[NodeIndex].OldX - 320.0)),
					FMath::RoundToFloat(static_cast<float>(Model.Nodes[NodeIndex].OldY - 220.0)));
				GraphEditor->SetViewLocation(UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(FocusedViewLocation, 1.0f), 1.0f);
				++OutStats.FocusedGeometryNodeCount;
				RefreshGraphPanelGeometry(2);
				CaptureNodeSize(Model.Nodes[NodeIndex], GraphNode);
				CapturePinAnchorsForNode(Model.Nodes[NodeIndex], GraphNode, false);
			}
			for (int32 PassIndex = 0; PassIndex < MaxFocusedCapturePasses; ++PassIndex)
			{
				const int32 MissingAtPassStart = CountAllMissingUsedPinAnchors();
				if (MissingAtPassStart <= 0)
				{
					break;
				}
				for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
				{
					if ((FPlatformTime::Seconds() - FocusedCaptureStartSeconds) > MaxFocusedCaptureSeconds)
					{
						OutStats.bFocusedGeometryBudgetExhausted = true;
						break;
					}
					UEdGraphNode* GraphNode = Model.GraphNodes.IsValidIndex(NodeIndex) ? Model.GraphNodes[NodeIndex] : nullptr;
					if (!GraphNode)
					{
						continue;
					}
					const int32 MissingForNode =
						CountMissingUsedPinAnchors(NodeIndex, EGPD_Input)
						+ CountMissingUsedPinAnchors(NodeIndex, EGPD_Output);
					if (MissingForNode <= 0)
					{
						continue;
					}
					const FVector2f FocusedViewLocation(
						FMath::RoundToFloat(static_cast<float>(Model.Nodes[NodeIndex].OldX - 320.0)),
						FMath::RoundToFloat(static_cast<float>(Model.Nodes[NodeIndex].OldY - 220.0)));
					GraphEditor->SetViewLocation(UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(FocusedViewLocation, 1.0f), 1.0f);
					++OutStats.FocusedGeometryNodeCount;
					RefreshGraphPanelGeometry(2);
					CaptureNodeSize(Model.Nodes[NodeIndex], GraphNode);
					CapturePinAnchorsForNode(Model.Nodes[NodeIndex], GraphNode, false);
				}
				if (OutStats.bFocusedGeometryBudgetExhausted || CountAllMissingUsedPinAnchors() >= MissingAtPassStart)
				{
					break;
				}
			}
		}

		OutStats.MissingPinAnchorCount = 0;
		OutStats.VisibleMissingPinAnchorCount = 0;
		for (const UeAgentBlueprintAutoLayout::FNode& Node : Model.Nodes)
		{
			OutStats.VisibleMissingPinAnchorCount += Node.InputPinNames.Num() - CountMeasuredPinAnchors(Node, EGPD_Input);
			OutStats.VisibleMissingPinAnchorCount += Node.OutputPinNames.Num() - CountMeasuredPinAnchors(Node, EGPD_Output);
		}
		for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
		{
			OutStats.MissingPinAnchorCount += CountMissingUsedPinAnchors(NodeIndex, EGPD_Input);
			OutStats.MissingPinAnchorCount += CountMissingUsedPinAnchors(NodeIndex, EGPD_Output);
		}
		OutStats.bUsedRealGeometry = OutStats.MeasuredNodeCount == OutStats.NodeCount && OutStats.MissingPinAnchorCount == 0;
		OutStats.GeometrySource = OutStats.bUsedRealGeometry
			? TEXT("slate_graph_editor_bounds_and_arranged_pin_geometry")
			: TEXT("incomplete_slate_real_pin_geometry");
		if (!OutStats.bUsedRealGeometry)
		{
			OutError = FString::Printf(
				TEXT("real_geometry_incomplete nodes=%d/%d pin_widgets=%d pin_geometry=%d missing_used_pin_anchors=%d missing_visible_pin_anchors=%d"),
				OutStats.MeasuredNodeCount,
				OutStats.NodeCount,
				OutStats.PinWidgetCount,
				OutStats.PinGeometryAnchorCount,
				OutStats.MissingPinAnchorCount,
				OutStats.VisibleMissingPinAnchorCount);
			if (OutStats.bFocusedGeometryBudgetExhausted)
			{
				OutError += FString::Printf(TEXT(" focused_capture_budget_exhausted focused_nodes=%d"), OutStats.FocusedGeometryNodeCount);
			}
			OutStats.Error = OutError;
		}
		else
		{
			OutError.Reset();
		}
		RestoreGraphView();
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutGeometry done elapsed_ms=%.1f used=%s missing_used=%d visible_missing=%d"),
			(FPlatformTime::Seconds() - CaptureStartSeconds) * 1000.0,
			OutStats.bUsedRealGeometry ? TEXT("true") : TEXT("false"),
			OutStats.MissingPinAnchorCount,
			OutStats.VisibleMissingPinAnchorCount);
		return OutStats.bUsedRealGeometry;
	}

	int32 RemoveExistingRerouteNodes(UEdGraph* Graph)
	{
		if (!Graph)
		{
			return 0;
		}
		TArray<UEdGraphNode*> RerouteNodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsLayoutRerouteNode(Node))
			{
				RerouteNodes.Add(Node);
			}
		}
		if (RerouteNodes.Num() == 0)
		{
			return 0;
		}
		Graph->Modify();
		for (UEdGraphNode* Node : RerouteNodes)
		{
			if (!Node)
			{
				continue;
			}
			Node->Modify();
			Node->BreakAllNodeLinks();
			Graph->RemoveNode(Node);
		}
		return RerouteNodes.Num();
	}

	int32 CountExistingRerouteNodes(UEdGraph* Graph)
	{
		if (!Graph)
		{
			return 0;
		}
		int32 Count = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsLayoutRerouteNode(Node))
			{
				++Count;
			}
		}
		return Count;
	}

	int32 CountPlannedRerouteKnotNodes(const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans)
	{
		int32 Count = 0;
		TSet<FString> SharedKeys;
		for (const UeAgentBlueprintAutoLayout::FReroutePlan& Plan : Plans)
		{
			const TArray<FVector2D>& WritePoints = UeAgentBlueprintAutoLayout::GetAuthoritativeReroutePoints(Plan);
			if (Plan.EdgeIndex == INDEX_NONE || WritePoints.Num() == 0)
			{
				continue;
			}
			for (const FVector2D& Point : WritePoints)
			{
				const FString SharedKey = UeAgentBlueprintAutoLayout::MakeRerouteAttachmentPointMergeKey(
					Plan.AttachmentMergeGroup,
					Point);
				if (!SharedKey.IsEmpty())
				{
					if (SharedKeys.Contains(SharedKey))
					{
						continue;
					}
					SharedKeys.Add(SharedKey);
				}
				++Count;
			}
		}
		return Count;
	}

	static UEdGraphPin* FindModelPin(const FGraphLayoutModel& Model, const UeAgentBlueprintAutoLayout::FEdge& Edge, bool bSource)
	{
		const int32 NodeIndex = bSource ? Edge.Source : Edge.Target;
		if (!Model.Nodes.IsValidIndex(NodeIndex))
		{
			return nullptr;
		}
		UEdGraphNode* const* NodePtr = Model.GuidToNode.Find(Model.Nodes[NodeIndex].Guid);
		if (!NodePtr || !*NodePtr)
		{
			return nullptr;
		}
		return FindLayoutPin(
			*NodePtr,
			bSource ? EGPD_Output : EGPD_Input,
			bSource ? Edge.SourcePinIndex : Edge.TargetPinIndex,
			bSource ? Edge.SourcePin : Edge.TargetPin);
	}

	FVector2D GetLayoutKnotNodeSpacerSize()
	{
		// Matches SGraphNodeKnot::NodeSpacerSize and UEdGraphSchema_K2 reroute creation.
		return FVector2D(42.0, 24.0);
	}

	static FVector2D ConvertLayoutKnotControlPointToNodeTopLeft(const FVector2D& ControlPoint)
	{
		return ControlPoint - GetLayoutKnotNodeSpacerSize() * 0.5;
	}

	static UK2Node_Knot* CreateLayoutKnot(UEdGraph* Graph, const FVector2D& ControlPoint)
	{
		if (!Graph)
		{
			return nullptr;
		}
		UK2Node_Knot* Knot = NewObject<UK2Node_Knot>(Graph);
		if (!Knot)
		{
			return nullptr;
		}
		Knot->CreateNewGuid();
		const FVector2D NodeTopLeft = ConvertLayoutKnotControlPointToNodeTopLeft(ControlPoint);
		Knot->NodePosX = FMath::RoundToInt(NodeTopLeft.X);
		Knot->NodePosY = FMath::RoundToInt(NodeTopLeft.Y);
		Graph->AddNode(Knot, true, false);
		Knot->PostPlacedNewNode();
		Knot->AllocateDefaultPins();
		if (!Knot->GetInputPin() || !Knot->GetOutputPin())
		{
			Knot->BreakAllNodeLinks();
			Graph->RemoveNode(Knot);
			return nullptr;
		}
		return Knot;
	}

	int32 ApplyNodePositions(const FGraphLayoutModel& Model)
	{
		int32 MoveCount = 0;
		for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
		{
			if (!Model.Nodes.IsValidIndex(NodeIndex) || !Model.GraphNodes.IsValidIndex(NodeIndex))
			{
				continue;
			}
			UEdGraphNode* GraphNode = Model.GraphNodes[NodeIndex];
			const UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[NodeIndex];
			if (!GraphNode)
			{
				continue;
			}
			const int32 NewX = FMath::RoundToInt(Node.NewX);
			const int32 NewY = FMath::RoundToInt(Node.NewY);
			if (GraphNode->NodePosX == NewX && GraphNode->NodePosY == NewY)
			{
				continue;
			}
			GraphNode->Modify();
			GraphNode->NodePosX = NewX;
			GraphNode->NodePosY = NewY;
			++MoveCount;
		}
		return MoveCount;
	}

	struct FK2ResolvedReroutePlan
	{
		const UeAgentBlueprintAutoLayout::FReroutePlan* Plan = nullptr;
		int32 EdgeIndex = INDEX_NONE;
		const UeAgentBlueprintAutoLayout::FEdge* Edge = nullptr;
		UEdGraphPin* SourcePin = nullptr;
		UEdGraphPin* TargetPin = nullptr;
		FVector2D SourcePoint = FVector2D::ZeroVector;
		FVector2D TargetPoint = FVector2D::ZeroVector;
		TArray<FVector2D> Points;
	};

	struct FK2BusTopologyBranch
	{
		int32 EdgeIndex = INDEX_NONE;
		UEdGraphPin* TargetPin = nullptr;
		FVector2D BranchPoint = FVector2D::ZeroVector;
		TArray<FVector2D> SuffixPoints;
	};

	struct FK2BusTopology
	{
		FString Key;
		UEdGraphPin* SourcePin = nullptr;
		double TrunkY = 0.0;
		TArray<FVector2D> SourcePrefixPoints;
		FVector2D SourceTrunkPoint = FVector2D::ZeroVector;
		TArray<FVector2D> TrunkPoints;
		TArray<FK2BusTopologyBranch> Branches;
	};

	static FString MakeVisiblePointKey(const FString& Scope, const FVector2D& Point)
	{
		return FString::Printf(TEXT("%s|%d|%d"), *Scope, FMath::RoundToInt(Point.X), FMath::RoundToInt(Point.Y));
	}

	static bool PointsNearlyEqual(const FVector2D& A, const FVector2D& B)
	{
		return FVector2D::Distance(A, B) <= 0.5;
	}

	static void AddUniqueVisiblePoint(TArray<FVector2D>& Points, const FVector2D& Point)
	{
		for (const FVector2D& Existing : Points)
		{
			if (PointsNearlyEqual(Existing, Point))
			{
				return;
			}
		}
		Points.Add(FVector2D(FMath::RoundToDouble(Point.X), FMath::RoundToDouble(Point.Y)));
	}

	static TArray<FVector2D> BuildResolvedRoute(const FK2ResolvedReroutePlan& Resolved)
	{
		TArray<FVector2D> Route;
		Route.Reserve(Resolved.Points.Num() + 2);
		Route.Add(Resolved.SourcePoint);
		for (const FVector2D& Point : Resolved.Points)
		{
			Route.Add(Point);
		}
		Route.Add(Resolved.TargetPoint);
		return Route;
	}

	static bool EstimateBusTrunkY(const TArray<FK2ResolvedReroutePlan>& Plans, double& OutTrunkY)
	{
		TMap<int32, double> HorizontalLengthByY;
		TMap<int32, double> WeightedYByKey;
		for (const FK2ResolvedReroutePlan& Resolved : Plans)
		{
			const TArray<FVector2D> Route = BuildResolvedRoute(Resolved);
			for (int32 Index = 0; Index + 1 < Route.Num(); ++Index)
			{
				const FVector2D& A = Route[Index];
				const FVector2D& B = Route[Index + 1];
				if (!FMath::IsNearlyEqual(A.Y, B.Y, 0.5))
				{
					continue;
				}
				const double Length = FMath::Abs(B.X - A.X);
				if (Length <= 0.5)
				{
					continue;
				}
				const double SegmentY = (A.Y + B.Y) * 0.5;
				const int32 Key = FMath::RoundToInt(SegmentY);
				HorizontalLengthByY.FindOrAdd(Key) += Length;
				WeightedYByKey.FindOrAdd(Key) += SegmentY * Length;
			}
		}

		int32 BestKey = INDEX_NONE;
		double BestLength = -1.0;
		for (const TPair<int32, double>& Pair : HorizontalLengthByY)
		{
			if (Pair.Value > BestLength + 0.5
				|| (FMath::IsNearlyEqual(Pair.Value, BestLength, 0.5) && (BestKey == INDEX_NONE || Pair.Key < BestKey)))
			{
				BestKey = Pair.Key;
				BestLength = Pair.Value;
			}
		}
		if (BestKey == INDEX_NONE || BestLength <= 0.5)
		{
			return false;
		}
		OutTrunkY = WeightedYByKey.Contains(BestKey)
			? WeightedYByKey[BestKey] / BestLength
			: static_cast<double>(BestKey);
		OutTrunkY = FMath::RoundToDouble(OutTrunkY);
		return true;
	}

	static FString MakeBusTopologyKey(
		const FGraphLayoutModel& Model,
		const UeAgentBlueprintAutoLayout::FReroutePlan& Plan)
	{
		if (!Model.Edges.IsValidIndex(Plan.EdgeIndex))
		{
			return FString();
		}
		const UeAgentBlueprintAutoLayout::FEdge& Edge = Model.Edges[Plan.EdgeIndex];
		if (Plan.RouteKind == UeAgentBlueprintAutoLayout::ERouteKind::ExternalBus)
		{
			return FString::Printf(
				TEXT("external_source=%d:%s:%d"),
				Edge.Source,
				*Edge.SourcePin,
				Edge.SourcePinIndex);
		}
		return FString::Printf(
			TEXT("%s|merge=%d|source=%d:%s:%d"),
			*Plan.BusId,
			Plan.AttachmentMergeGroup,
			Edge.Source,
			*Edge.SourcePin,
			Edge.SourcePinIndex);
	}

	static bool BuildK2BusTopology(
		const FString& TopologyKey,
		const TArray<FK2ResolvedReroutePlan>& Plans,
		FK2BusTopology& OutTopology)
	{
		if (Plans.Num() == 0 || !Plans[0].SourcePin)
		{
			return false;
		}
		double TrunkY = 0.0;
		if (!EstimateBusTrunkY(Plans, TrunkY))
		{
			return false;
		}

		OutTopology = FK2BusTopology();
		OutTopology.Key = TopologyKey;
		OutTopology.SourcePin = Plans[0].SourcePin;
		OutTopology.TrunkY = TrunkY;

		double BestPrefixFirstTrunkX = TNumericLimits<double>::Max();
		TArray<FVector2D> BestSourcePrefix;
		FVector2D BestSourceTrunkPoint = FVector2D::ZeroVector;
		bool bAnyPlanMaterialized = false;

		for (const FK2ResolvedReroutePlan& Resolved : Plans)
		{
			if (!Resolved.Plan || !Resolved.TargetPin)
			{
				continue;
			}
			const TArray<FVector2D> Route = BuildResolvedRoute(Resolved);
			int32 FirstTrunkIndex = INDEX_NONE;
			int32 LastTrunkIndex = INDEX_NONE;
			for (int32 Index = 1; Index + 1 < Route.Num(); ++Index)
			{
				if (FMath::IsNearlyEqual(Route[Index].Y, TrunkY, 0.5))
				{
					if (FirstTrunkIndex == INDEX_NONE)
					{
						FirstTrunkIndex = Index;
					}
					LastTrunkIndex = Index;
				}
			}
			if (FirstTrunkIndex == INDEX_NONE || LastTrunkIndex == INDEX_NONE)
			{
				return false;
			}

			TArray<FVector2D> SourcePrefix;
			for (int32 Index = 1; Index < FirstTrunkIndex; ++Index)
			{
				SourcePrefix.Add(Route[Index]);
			}
			if (Route[FirstTrunkIndex].X < BestPrefixFirstTrunkX)
			{
				BestPrefixFirstTrunkX = Route[FirstTrunkIndex].X;
				BestSourcePrefix = SourcePrefix;
				BestSourceTrunkPoint = FVector2D(Route[FirstTrunkIndex].X, TrunkY);
			}

			for (int32 Index = FirstTrunkIndex; Index <= LastTrunkIndex; ++Index)
			{
				if (FMath::IsNearlyEqual(Route[Index].Y, TrunkY, 0.5))
				{
					AddUniqueVisiblePoint(OutTopology.TrunkPoints, FVector2D(Route[Index].X, TrunkY));
				}
			}

			FK2BusTopologyBranch Branch;
			Branch.EdgeIndex = Resolved.EdgeIndex;
			Branch.TargetPin = Resolved.TargetPin;
			Branch.BranchPoint = FVector2D(Route[LastTrunkIndex].X, TrunkY);
			for (int32 Index = LastTrunkIndex + 1; Index + 1 < Route.Num(); ++Index)
			{
				if (!PointsNearlyEqual(Route[Index], Branch.BranchPoint))
				{
					Branch.SuffixPoints.Add(Route[Index]);
				}
			}
			OutTopology.Branches.Add(Branch);
			bAnyPlanMaterialized = true;
		}

		if (!bAnyPlanMaterialized || OutTopology.TrunkPoints.Num() == 0 || OutTopology.Branches.Num() == 0)
		{
			return false;
		}
		OutTopology.SourcePrefixPoints = BestSourcePrefix;
		OutTopology.SourceTrunkPoint = BestSourceTrunkPoint;
		OutTopology.TrunkPoints.Sort([](const FVector2D& A, const FVector2D& B)
		{
			if (!FMath::IsNearlyEqual(A.X, B.X, 0.5))
			{
				return A.X < B.X;
			}
			return A.Y < B.Y;
		});
		return true;
	}

	static UK2Node_Knot* GetOrCreateTopologyKnot(
		UEdGraph* Graph,
		const FString& ScopeKey,
		const FVector2D& Point,
		TMap<FString, UK2Node_Knot*>& KnotByKey,
		int32& CreatedKnotCount)
	{
		const FString Key = MakeVisiblePointKey(ScopeKey, Point);
		if (UK2Node_Knot** Existing = KnotByKey.Find(Key))
		{
			return *Existing;
		}
		UK2Node_Knot* Knot = CreateLayoutKnot(Graph, Point);
		if (Knot)
		{
			KnotByKey.Add(Key, Knot);
			++CreatedKnotCount;
		}
		return Knot;
	}

	static void EnsureK2Link(UEdGraphPin* SourceOutput, UEdGraphPin* TargetInput)
	{
		if (SourceOutput && TargetInput && !SourceOutput->LinkedTo.Contains(TargetInput))
		{
			SourceOutput->MakeLinkTo(TargetInput, true);
		}
	}

	static bool ApplyK2LegacyReroutePlan(
		UEdGraph* Graph,
		const UeAgentBlueprintAutoLayout::FReroutePlan& Plan,
		UEdGraphPin* SourcePin,
		UEdGraphPin* TargetPin,
		TMap<FString, UK2Node_Knot*>& KnotByKey,
		int32& CreatedKnotCount)
	{
		UEdGraphPin* CurrentOutput = SourcePin;
		const FString ScopeKey = Plan.AttachmentMergeGroup != INDEX_NONE
			? FString::Printf(TEXT("legacy|%s|merge=%d"), *Plan.BusId, Plan.AttachmentMergeGroup)
			: FString::Printf(TEXT("legacy_edge=%d"), Plan.EdgeIndex);
		const TArray<FVector2D>& KnotPoints = UeAgentBlueprintAutoLayout::GetAuthoritativeReroutePoints(Plan);
		for (const FVector2D& Point : KnotPoints)
		{
			UK2Node_Knot* Knot = GetOrCreateTopologyKnot(Graph, ScopeKey, Point, KnotByKey, CreatedKnotCount);
			if (!Knot)
			{
				return false;
			}
			EnsureK2Link(CurrentOutput, Knot->GetInputPin());
			CurrentOutput = Knot->GetOutputPin();
		}
		EnsureK2Link(CurrentOutput, TargetPin);
		return true;
	}

	static bool ApplyK2BusTopology(
		UEdGraph* Graph,
		const FK2BusTopology& Topology,
		TMap<FString, UK2Node_Knot*>& KnotByKey,
		int32& CreatedKnotCount)
	{
		if (!Graph || !Topology.SourcePin || Topology.TrunkPoints.Num() == 0)
		{
			return false;
		}

		UEdGraphPin* CurrentOutput = Topology.SourcePin;
		for (const FVector2D& PrefixPoint : Topology.SourcePrefixPoints)
		{
			UK2Node_Knot* PrefixKnot = GetOrCreateTopologyKnot(Graph, Topology.Key, PrefixPoint, KnotByKey, CreatedKnotCount);
			if (!PrefixKnot)
			{
				return false;
			}
			EnsureK2Link(CurrentOutput, PrefixKnot->GetInputPin());
			CurrentOutput = PrefixKnot->GetOutputPin();
		}

		TMap<FString, UK2Node_Knot*> TrunkKnotByPoint;
		for (const FVector2D& TrunkPoint : Topology.TrunkPoints)
		{
			UK2Node_Knot* TrunkKnot = GetOrCreateTopologyKnot(Graph, Topology.Key, TrunkPoint, KnotByKey, CreatedKnotCount);
			if (!TrunkKnot)
			{
				return false;
			}
			TrunkKnotByPoint.Add(MakeVisiblePointKey(Topology.Key, TrunkPoint), TrunkKnot);
		}

		UK2Node_Knot* FirstTrunkKnot = TrunkKnotByPoint.FindRef(MakeVisiblePointKey(Topology.Key, Topology.TrunkPoints[0]));
		UK2Node_Knot* SourceTrunkKnot = TrunkKnotByPoint.FindRef(MakeVisiblePointKey(Topology.Key, Topology.SourceTrunkPoint));
		if (!SourceTrunkKnot)
		{
			SourceTrunkKnot = FirstTrunkKnot;
		}
		if (!SourceTrunkKnot)
		{
			return false;
		}
		EnsureK2Link(CurrentOutput, SourceTrunkKnot->GetInputPin());
		int32 SourceTrunkIndex = INDEX_NONE;
		for (int32 Index = 0; Index < Topology.TrunkPoints.Num(); ++Index)
		{
			if (PointsNearlyEqual(Topology.TrunkPoints[Index], Topology.SourceTrunkPoint))
			{
				SourceTrunkIndex = Index;
				break;
			}
		}
		if (SourceTrunkIndex == INDEX_NONE)
		{
			SourceTrunkIndex = 0;
		}
		for (int32 Index = SourceTrunkIndex; Index + 1 < Topology.TrunkPoints.Num(); ++Index)
		{
			UK2Node_Knot* PrevKnot = TrunkKnotByPoint.FindRef(MakeVisiblePointKey(Topology.Key, Topology.TrunkPoints[Index]));
			UK2Node_Knot* NextKnot = TrunkKnotByPoint.FindRef(MakeVisiblePointKey(Topology.Key, Topology.TrunkPoints[Index + 1]));
			if (!PrevKnot || !NextKnot)
			{
				return false;
			}
			EnsureK2Link(PrevKnot->GetOutputPin(), NextKnot->GetInputPin());
		}
		for (int32 Index = SourceTrunkIndex; Index > 0; --Index)
		{
			UK2Node_Knot* PrevKnot = TrunkKnotByPoint.FindRef(MakeVisiblePointKey(Topology.Key, Topology.TrunkPoints[Index]));
			UK2Node_Knot* NextKnot = TrunkKnotByPoint.FindRef(MakeVisiblePointKey(Topology.Key, Topology.TrunkPoints[Index - 1]));
			if (!PrevKnot || !NextKnot)
			{
				return false;
			}
			EnsureK2Link(PrevKnot->GetOutputPin(), NextKnot->GetInputPin());
		}

		for (const FK2BusTopologyBranch& Branch : Topology.Branches)
		{
			UK2Node_Knot* BranchKnot = TrunkKnotByPoint.FindRef(MakeVisiblePointKey(Topology.Key, Branch.BranchPoint));
			if (!BranchKnot || !Branch.TargetPin)
			{
				return false;
			}
			UEdGraphPin* BranchOutput = BranchKnot->GetOutputPin();
			for (const FVector2D& SuffixPoint : Branch.SuffixPoints)
			{
				UK2Node_Knot* SuffixKnot = GetOrCreateTopologyKnot(Graph, Topology.Key, SuffixPoint, KnotByKey, CreatedKnotCount);
				if (!SuffixKnot)
				{
					return false;
				}
				EnsureK2Link(BranchOutput, SuffixKnot->GetInputPin());
				BranchOutput = SuffixKnot->GetOutputPin();
			}
			EnsureK2Link(BranchOutput, Branch.TargetPin);
		}
		return true;
	}

	int32 ApplyReroutePlans(
		UEdGraph* Graph,
		const FGraphLayoutModel& Model,
		const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans,
		bool bInsertRerouteNodes,
		bool bRewireAllEdges)
	{
		if (!Graph)
		{
			return 0;
		}
		TMap<int32, const UeAgentBlueprintAutoLayout::FReroutePlan*> PlanByEdge;
		for (const UeAgentBlueprintAutoLayout::FReroutePlan& Plan : Plans)
		{
			if (Plan.EdgeIndex != INDEX_NONE
				&& UeAgentBlueprintAutoLayout::GetAuthoritativeReroutePoints(Plan).Num() > 0)
			{
				PlanByEdge.Add(Plan.EdgeIndex, &Plan);
			}
		}

		int32 CreatedKnotCount = 0;
		TMap<FString, UK2Node_Knot*> KnotByTopologyKey;
		TMap<FString, TArray<FK2ResolvedReroutePlan>> BusPlansByTopologyKey;
		TSet<int32> TopologyHandledEdges;

		for (int32 EdgeIndex = 0; EdgeIndex < Model.Edges.Num(); ++EdgeIndex)
		{
			const UeAgentBlueprintAutoLayout::FEdge& Edge = Model.Edges[EdgeIndex];
			UEdGraphPin* SourcePin = FindModelPin(Model, Edge, true);
			UEdGraphPin* TargetPin = FindModelPin(Model, Edge, false);
			if (!SourcePin || !TargetPin)
			{
				continue;
			}
			const UeAgentBlueprintAutoLayout::FReroutePlan* const* PlanPtr = PlanByEdge.Find(EdgeIndex);
			const bool bHasPlan = PlanPtr && *PlanPtr;
			if (bRewireAllEdges || bHasPlan)
			{
				SourcePin->BreakLinkTo(TargetPin, true);
			}
			if (!bInsertRerouteNodes)
			{
				if ((bRewireAllEdges || bHasPlan) && !SourcePin->LinkedTo.Contains(TargetPin))
				{
					SourcePin->MakeLinkTo(TargetPin, true);
				}
				continue;
			}
			if (bHasPlan
				&& (*PlanPtr)->AttachmentMergeGroup != INDEX_NONE
				&& (*PlanPtr)->RouteKind != UeAgentBlueprintAutoLayout::ERouteKind::Direct)
			{
				FK2ResolvedReroutePlan Resolved;
				Resolved.Plan = *PlanPtr;
				Resolved.EdgeIndex = EdgeIndex;
				Resolved.Edge = &Edge;
				Resolved.SourcePin = SourcePin;
				Resolved.TargetPin = TargetPin;
				Resolved.SourcePoint = UeAgentBlueprintAutoLayout::GetSourcePort(Model.Nodes[Edge.Source], Edge, true);
				Resolved.TargetPoint = UeAgentBlueprintAutoLayout::GetTargetPort(Model.Nodes[Edge.Target], Edge, true);
				Resolved.Points = UeAgentBlueprintAutoLayout::GetAuthoritativeReroutePoints(**PlanPtr);
				const FString TopologyKey = MakeBusTopologyKey(Model, **PlanPtr);
				if (!TopologyKey.IsEmpty())
				{
					BusPlansByTopologyKey.FindOrAdd(TopologyKey).Add(Resolved);
				}
			}
		}

		for (const TPair<FString, TArray<FK2ResolvedReroutePlan>>& Pair : BusPlansByTopologyKey)
		{
			FK2BusTopology Topology;
			if (!BuildK2BusTopology(Pair.Key, Pair.Value, Topology))
			{
				continue;
			}
			if (ApplyK2BusTopology(Graph, Topology, KnotByTopologyKey, CreatedKnotCount))
			{
				for (const FK2ResolvedReroutePlan& Resolved : Pair.Value)
				{
					TopologyHandledEdges.Add(Resolved.EdgeIndex);
				}
			}
		}

		for (int32 EdgeIndex = 0; EdgeIndex < Model.Edges.Num(); ++EdgeIndex)
		{
			if (TopologyHandledEdges.Contains(EdgeIndex))
			{
				continue;
			}
			const UeAgentBlueprintAutoLayout::FEdge& Edge = Model.Edges[EdgeIndex];
			UEdGraphPin* SourcePin = FindModelPin(Model, Edge, true);
			UEdGraphPin* TargetPin = FindModelPin(Model, Edge, false);
			if (!SourcePin || !TargetPin)
			{
				continue;
			}
			const UeAgentBlueprintAutoLayout::FReroutePlan* const* PlanPtr = PlanByEdge.Find(EdgeIndex);
			const bool bHasPlan = PlanPtr && *PlanPtr;
			if (bHasPlan)
			{
				ApplyK2LegacyReroutePlan(Graph, **PlanPtr, SourcePin, TargetPin, KnotByTopologyKey, CreatedKnotCount);
			}
			else if (bRewireAllEdges && !SourcePin->LinkedTo.Contains(TargetPin))
			{
				SourcePin->MakeLinkTo(TargetPin, true);
			}
		}
		return CreatedKnotCount;
	}

	TArray<TSharedPtr<FJsonValue>> NodesToJson(const FGraphLayoutModel& Model)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const UeAgentBlueprintAutoLayout::FNode& Node : Model.Nodes)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("guid"), Node.Guid);
			Obj->SetStringField(TEXT("title"), Node.Title);
			Obj->SetStringField(TEXT("class_name"), Node.ClassName);
			Obj->SetNumberField(TEXT("index"), Node.Index);
			Obj->SetNumberField(TEXT("rank"), Node.Rank);
			Obj->SetNumberField(TEXT("order"), Node.Order);
			Obj->SetNumberField(TEXT("component"), Node.Component);
			Obj->SetNumberField(TEXT("old_pos_x"), Node.OldX);
			Obj->SetNumberField(TEXT("old_pos_y"), Node.OldY);
			Obj->SetNumberField(TEXT("new_pos_x"), Node.NewX);
			Obj->SetNumberField(TEXT("new_pos_y"), Node.NewY);
			Obj->SetNumberField(TEXT("width"), Node.Width);
			Obj->SetNumberField(TEXT("height"), Node.Height);
			Obj->SetBoolField(TEXT("moved"), FMath::Abs(Node.NewX - Node.OldX) > 0.5 || FMath::Abs(Node.NewY - Node.OldY) > 0.5);
			Obj->SetBoolField(TEXT("has_exec_pin"), Node.bHasExecPin);
			Obj->SetBoolField(TEXT("is_comment_node"), Node.bIsCommentNode);
			Obj->SetNumberField(TEXT("comment_depth"), Node.CommentDepth);
			Obj->SetBoolField(TEXT("measured_geometry"), Node.bMeasuredGeometry);
			Obj->SetNumberField(TEXT("input_pin_anchor_count"), CountMeasuredPinAnchors(Node, EGPD_Input));
			Obj->SetNumberField(TEXT("output_pin_anchor_count"), CountMeasuredPinAnchors(Node, EGPD_Output));
			Out.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> ReroutePlansToJson(const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const UeAgentBlueprintAutoLayout::FReroutePlan& Plan : Plans)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("edge_index"), Plan.EdgeIndex);
			Obj->SetStringField(TEXT("reason"), Plan.Reason);
			Obj->SetStringField(TEXT("bus_id"), Plan.BusId);
			Obj->SetStringField(TEXT("route_kind"), UeAgentBlueprintAutoLayout::RouteKindToString(Plan.RouteKind));
			Obj->SetNumberField(TEXT("bus_lane_index"), Plan.BusLaneIndex);
			Obj->SetNumberField(TEXT("branch_lane_index"), Plan.BranchLaneIndex);
			Obj->SetNumberField(TEXT("attachment_merge_group"), Plan.AttachmentMergeGroup);

			TArray<TSharedPtr<FJsonValue>> Points;
			for (const FVector2D& Point : Plan.Points)
			{
				TSharedPtr<FJsonObject> PointObj = MakeShared<FJsonObject>();
				PointObj->SetNumberField(TEXT("x"), Point.X);
				PointObj->SetNumberField(TEXT("y"), Point.Y);
				Points.Add(MakeShared<FJsonValueObject>(PointObj));
			}
			Obj->SetArrayField(TEXT("points"), Points);
			TArray<TSharedPtr<FJsonValue>> KnotPoints;
			const TArray<FVector2D>& WritePoints = Plan.KnotPoints.Num() == Plan.Points.Num() ? Plan.KnotPoints : Plan.Points;
			for (const FVector2D& Point : WritePoints)
			{
				TSharedPtr<FJsonObject> PointObj = MakeShared<FJsonObject>();
				PointObj->SetNumberField(TEXT("x"), Point.X);
				PointObj->SetNumberField(TEXT("y"), Point.Y);
				KnotPoints.Add(MakeShared<FJsonValueObject>(PointObj));
			}
			Obj->SetArrayField(TEXT("knot_points"), KnotPoints);
			TArray<TSharedPtr<FJsonValue>> PointRoles;
			for (const UeAgentBlueprintAutoLayout::EReroutePointRole Role : Plan.PointRoles)
			{
				PointRoles.Add(MakeShared<FJsonValueString>(UeAgentBlueprintAutoLayout::ReroutePointRoleToString(Role)));
			}
			Obj->SetArrayField(TEXT("point_roles"), PointRoles);
			Out.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Out;
	}

	static UeAgentBlueprintAutoLayout::ERouteKind LayoutRouteKindFromString(const FString& Value)
	{
		if (Value.Equals(TEXT("local_bus"), ESearchCase::IgnoreCase))
		{
			return UeAgentBlueprintAutoLayout::ERouteKind::LocalBus;
		}
		if (Value.Equals(TEXT("external_bus"), ESearchCase::IgnoreCase))
		{
			return UeAgentBlueprintAutoLayout::ERouteKind::ExternalBus;
		}
		return UeAgentBlueprintAutoLayout::ERouteKind::Direct;
	}

	static bool TryGetLayoutReportDataObject(const TSharedPtr<FJsonObject>& Root, TSharedPtr<FJsonObject>& OutData)
	{
		if (!Root.IsValid())
		{
			return false;
		}
		if (Root->HasTypedField<EJson::Array>(TEXT("nodes")))
		{
			OutData = Root;
			return true;
		}

		const TSharedPtr<FJsonObject>* ResponseObjPtr = nullptr;
		if (Root->TryGetObjectField(TEXT("response"), ResponseObjPtr) && ResponseObjPtr && ResponseObjPtr->IsValid())
		{
			const TSharedPtr<FJsonObject>& ResponseObj = *ResponseObjPtr;
			const TSharedPtr<FJsonObject>* ResponseDataObjPtr = nullptr;
			if (ResponseObj->TryGetObjectField(TEXT("data"), ResponseDataObjPtr) && ResponseDataObjPtr && ResponseDataObjPtr->IsValid())
			{
				const TSharedPtr<FJsonObject>& ResponseDataObj = *ResponseDataObjPtr;
				if (ResponseDataObj->HasTypedField<EJson::Array>(TEXT("nodes")))
				{
					OutData = ResponseDataObj;
					return true;
				}
				const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
				if (ResponseDataObj->TryGetArrayField(TEXT("results"), ResultsArray) && ResultsArray)
				{
					for (const TSharedPtr<FJsonValue>& ResultValue : *ResultsArray)
					{
						const TSharedPtr<FJsonObject>* ResultObjPtr = nullptr;
						if (!ResultValue.IsValid() || !ResultValue->TryGetObject(ResultObjPtr) || !ResultObjPtr || !ResultObjPtr->IsValid())
						{
							continue;
						}
						const TSharedPtr<FJsonObject>* ResultDataObjPtr = nullptr;
						if ((*ResultObjPtr)->TryGetObjectField(TEXT("data"), ResultDataObjPtr)
							&& ResultDataObjPtr
							&& ResultDataObjPtr->IsValid()
							&& (*ResultDataObjPtr)->HasTypedField<EJson::Array>(TEXT("nodes")))
						{
							OutData = *ResultDataObjPtr;
							return true;
						}
					}
				}
			}
		}
		return false;
	}

	static FString ResolveProjectRelativeFilePath(const FString& FilePath)
	{
		if (FPaths::IsRelative(FilePath))
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), FilePath);
		}
		return FilePath;
	}

	bool ApplyLayoutReportToModel(
		const FString& ReportFile,
		FGraphLayoutModel& Model,
		UeAgentBlueprintAutoLayout::FRunResult& OutRun,
		FString& OutError)
	{
		const FString FullReportPath = ResolveProjectRelativeFilePath(ReportFile);
		FString ReportText;
		if (!FFileHelper::LoadFileToString(ReportText, *FullReportPath))
		{
			OutError = FString::Printf(TEXT("layout_report_file_load_failed:%s"), *FullReportPath);
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReportText);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = FString::Printf(TEXT("layout_report_file_parse_failed:%s"), *FullReportPath);
			return false;
		}

		TSharedPtr<FJsonObject> DataObj;
		if (!TryGetLayoutReportDataObject(Root, DataObj) || !DataObj.IsValid())
		{
			OutError = TEXT("layout_report_data_not_found");
			return false;
		}

		TMap<FString, int32> GuidToModelIndex;
		for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
		{
			GuidToModelIndex.Add(Model.Nodes[NodeIndex].Guid.ToUpper(), NodeIndex);
		}

		const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
		if (!DataObj->TryGetArrayField(TEXT("nodes"), NodesArray) || !NodesArray)
		{
			OutError = TEXT("layout_report_nodes_missing");
			return false;
		}

		int32 RestoredNodeCount = 0;
		int32 MissingNodeCount = 0;
		int32 ActualMovedNodeCount = 0;
		for (const TSharedPtr<FJsonValue>& NodeValue : *NodesArray)
		{
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& NodeObj = *NodeObjPtr;
			FString Guid;
			double NewX = 0.0;
			double NewY = 0.0;
			if (!NodeObj->TryGetStringField(TEXT("guid"), Guid)
				|| !NodeObj->TryGetNumberField(TEXT("new_pos_x"), NewX)
				|| !NodeObj->TryGetNumberField(TEXT("new_pos_y"), NewY))
			{
				continue;
			}
			const int32* ModelIndex = GuidToModelIndex.Find(Guid.ToUpper());
			if (!ModelIndex || !Model.Nodes.IsValidIndex(*ModelIndex))
			{
				++MissingNodeCount;
				continue;
			}
			UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[*ModelIndex];
			if (!FMath::IsNearlyEqual(Node.OldX, NewX, 0.5) || !FMath::IsNearlyEqual(Node.OldY, NewY, 0.5))
			{
				++ActualMovedNodeCount;
			}
			Node.NewX = NewX;
			Node.NewY = NewY;
			++RestoredNodeCount;
		}
		if (RestoredNodeCount == 0)
		{
			OutError = TEXT("layout_report_no_matching_nodes");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* RerouteArray = nullptr;
		if (!DataObj->TryGetArrayField(TEXT("reroute_plan"), RerouteArray))
		{
			DataObj->TryGetArrayField(TEXT("reroute_plans"), RerouteArray);
		}
		if (RerouteArray)
		{
			for (const TSharedPtr<FJsonValue>& PlanValue : *RerouteArray)
			{
				const TSharedPtr<FJsonObject>* PlanObjPtr = nullptr;
				if (!PlanValue.IsValid() || !PlanValue->TryGetObject(PlanObjPtr) || !PlanObjPtr || !PlanObjPtr->IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject>& PlanObj = *PlanObjPtr;
				UeAgentBlueprintAutoLayout::FReroutePlan Plan;
				double EdgeIndexNumber = 0.0;
				if (PlanObj->TryGetNumberField(TEXT("edge_index"), EdgeIndexNumber))
				{
					Plan.EdgeIndex = FMath::RoundToInt(EdgeIndexNumber);
				}
				PlanObj->TryGetStringField(TEXT("reason"), Plan.Reason);
				PlanObj->TryGetStringField(TEXT("bus_id"), Plan.BusId);
				FString RouteKindValue;
				if (PlanObj->TryGetStringField(TEXT("route_kind"), RouteKindValue))
				{
					Plan.RouteKind = LayoutRouteKindFromString(RouteKindValue);
				}
				double IntFieldNumber = 0.0;
				if (PlanObj->TryGetNumberField(TEXT("bus_lane_index"), IntFieldNumber))
				{
					Plan.BusLaneIndex = FMath::RoundToInt(IntFieldNumber);
				}
				if (PlanObj->TryGetNumberField(TEXT("branch_lane_index"), IntFieldNumber))
				{
					Plan.BranchLaneIndex = FMath::RoundToInt(IntFieldNumber);
				}
				if (PlanObj->TryGetNumberField(TEXT("attachment_merge_group"), IntFieldNumber))
				{
					Plan.AttachmentMergeGroup = FMath::RoundToInt(IntFieldNumber);
				}

				auto ReadPointArray = [](const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, TArray<FVector2D>& OutPoints)
				{
					const TArray<TSharedPtr<FJsonValue>>* PointsArray = nullptr;
					if (!Obj.IsValid() || !Obj->TryGetArrayField(FieldName, PointsArray) || !PointsArray)
					{
						return;
					}
					for (const TSharedPtr<FJsonValue>& PointValue : *PointsArray)
					{
						const TSharedPtr<FJsonObject>* PointObjPtr = nullptr;
						if (!PointValue.IsValid() || !PointValue->TryGetObject(PointObjPtr) || !PointObjPtr || !PointObjPtr->IsValid())
						{
							continue;
						}
						double X = 0.0;
						double Y = 0.0;
						if ((*PointObjPtr)->TryGetNumberField(TEXT("x"), X) && (*PointObjPtr)->TryGetNumberField(TEXT("y"), Y))
						{
							OutPoints.Add(FVector2D(X, Y));
						}
					}
				};
				ReadPointArray(PlanObj, TEXT("points"), Plan.Points);
				ReadPointArray(PlanObj, TEXT("knot_points"), Plan.KnotPoints);
				if (Plan.KnotPoints.Num() == Plan.Points.Num() && Plan.KnotPoints.Num() > 0)
				{
					Plan.Points = Plan.KnotPoints;
				}
				Plan.KnotPoints.Reset();
				Plan.PointRoles.Reset();
				Plan.KnotPoints.Reserve(Plan.Points.Num());
				Plan.PointRoles.Reserve(Plan.Points.Num());
				for (const FVector2D& Point : Plan.Points)
				{
					Plan.KnotPoints.Add(Point);
					Plan.PointRoles.Add(UeAgentBlueprintAutoLayout::EReroutePointRole::Auto);
				}
				if (Model.Edges.IsValidIndex(Plan.EdgeIndex) && Plan.Points.Num() > 0)
				{
					OutRun.ReroutePlans.Add(Plan);
				}
			}
		}

		OutRun.Strategy = TEXT("layout_report_restore");
		OutRun.bDesignComplete = MissingNodeCount == 0;
		OutRun.NodeCount = Model.Nodes.Num();
		OutRun.EdgeCount = Model.Edges.Num();
		OutRun.MovedNodeCount = ActualMovedNodeCount;
		OutRun.ReroutePlanCount = OutRun.ReroutePlans.Num();
		for (const UeAgentBlueprintAutoLayout::FReroutePlan& Plan : OutRun.ReroutePlans)
		{
			if (Plan.RouteKind == UeAgentBlueprintAutoLayout::ERouteKind::LocalBus)
			{
				++OutRun.LocalBusEdgeCount;
			}
			else if (Plan.RouteKind == UeAgentBlueprintAutoLayout::ERouteKind::ExternalBus)
			{
				++OutRun.ExternalBusEdgeCount;
			}
		}
		return true;
	}
}
