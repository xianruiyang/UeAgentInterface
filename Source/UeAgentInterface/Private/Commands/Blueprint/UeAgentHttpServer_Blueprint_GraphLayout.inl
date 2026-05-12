// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Blueprint/Layout/UeAgentBlueprintAutoLayoutAlgorithm.h"

namespace UeAgentBlueprintOps
{
	namespace
	{
		struct FGraphLayoutModel
		{
			TArray<UeAgentBlueprintAutoLayout::FNode> Nodes;
			TArray<UeAgentBlueprintAutoLayout::FEdge> Edges;
			TArray<UEdGraphNode*> GraphNodes;
			TMap<UEdGraphNode*, int32> NodeToIndex;
			TMap<FString, UEdGraphNode*> GuidToNode;
		};

		struct FGraphLayoutGeometryStats
		{
			bool bUsedRealGeometry = false;
			bool bWindowPrepared = false;
			FString GeometrySource = TEXT("real_geometry_required");
			FString Error;
			int32 NodeCount = 0;
			int32 MeasuredNodeCount = 0;
			int32 PinWidgetCount = 0;
			int32 PinGeometryAnchorCount = 0;
			int32 VisibleMissingPinAnchorCount = 0;
			int32 MissingPinAnchorCount = 0;
			int32 FocusedGeometryNodeCount = 0;
			bool bFocusedGeometryBudgetExhausted = false;
			bool bRestoreViewStateFromDocument = false;
			bool bRestoredGraphViewAfterCapture = false;
			bool bRealGeometryCaptureRequested = false;
			bool bGeometryViewCaptureAllowed = false;
			bool bFocusedGeometryCaptureAllowed = false;
			FVector2f RestoredViewLocation = FVector2f::ZeroVector;
			float RestoredZoomAmount = 1.0f;
			float GeometryMeasurementZoomAmount = 1.0f;
		};

		static bool IsLayoutRerouteNode(const UEdGraphNode* Node)
		{
			return Cast<UK2Node_Knot>(Node) != nullptr;
		}

		static double ComputeGraphEditorAccessOffsetX(const double Fallback)
		{
			const UGraphEditorSettings* Settings = GetDefault<UGraphEditorSettings>();
			if (!Settings)
			{
				return Fallback;
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
			return FMath::IsFinite(Offset) && Offset > 0.0 ? Offset : Fallback;
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
				return false;
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
				TMap<TSharedRef<SWidget>, FArrangedWidget> PanelArrangedWidgets;
				FindArrangedDescendantGeometries(GraphPanel, PanelGeometry, WidgetsToFind, PanelArrangedWidgets);
				for (const TPair<TSharedRef<SWidget>, FArrangedWidget>& Pair : PanelArrangedWidgets)
				{
					if (!ArrangedWidgets.Contains(Pair.Key))
					{
						ArrangedWidgets.Add(Pair.Key, Pair.Value);
					}
				}
			}

			const FArrangedWidget* PinGeometry = ArrangedWidgets.Find(PinWidget);
			if (!PinGeometry)
			{
				return false;
			}

			const FVector2f PinPointAbsolute = GetGraphPinConnectionPointAbsolute(
				PinGeometry->Geometry,
				Direction,
				ZoomAmount);

			if (bHasArrangedNodeGeometry)
			{
				const FVector2f NodeAbsolute = ArrangedNodeGeometry.GetAbsolutePosition();
				const FVector2f LocalGraphOffset = (PinPointAbsolute - NodeAbsolute) / ZoomAmount;
				OutAnchor = FVector2D(static_cast<double>(LocalGraphOffset.X), static_cast<double>(LocalGraphOffset.Y));
				if (IsFiniteLayoutPoint(OutAnchor))
				{
					return true;
				}
			}

			return TryConvertAbsolutePinPointToNodeAnchor(
				GraphPanel,
				GraphNodeWidget,
				NodeGraphPosition,
				PinPointAbsolute,
				OutAnchor);
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

			return TryConvertAbsolutePinPointToNodeAnchor(
				GraphPanel,
				GraphNodeWidget,
				NodeGraphPosition,
				PinConnectionPointAbsolute,
				OutAnchor);
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

		static FGraphLayoutModel CollectGraphLayoutModel(UEdGraph* Graph)
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

		static bool CaptureGraphLayoutRealGeometry(
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
			TSharedPtr<IBlueprintEditor> BlueprintEditor = UeAgentBlueprintOps::GetBlueprintEditor(Blueprint, true, OutError);
			if (!BlueprintEditor.IsValid())
			{
				OutStats.Error = OutError;
				return false;
			}
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutGeometry editor_ready elapsed_ms=%.1f"),
				(FPlatformTime::Seconds() - CaptureStartSeconds) * 1000.0);
			TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(Graph, true);
			if (!GraphEditor.IsValid())
			{
				OutError = TEXT("graph_editor_not_found_for_real_geometry");
				OutStats.Error = OutError;
				return false;
			}
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutGeometry graph_editor_ready elapsed_ms=%.1f"),
				(FPlatformTime::Seconds() - CaptureStartSeconds) * 1000.0);
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

		static int32 RemoveExistingRerouteNodes(UEdGraph* Graph)
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

		static int32 CountExistingRerouteNodes(UEdGraph* Graph)
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

		static int32 CountPlannedRerouteKnotNodes(const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans)
		{
			int32 Count = 0;
			for (const UeAgentBlueprintAutoLayout::FReroutePlan& Plan : Plans)
			{
				if (Plan.EdgeIndex == INDEX_NONE || Plan.Points.Num() == 0)
				{
					continue;
				}
				Count += Plan.KnotPoints.Num() == Plan.Points.Num()
					? Plan.KnotPoints.Num()
					: Plan.Points.Num();
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

		static FVector2D GetLayoutKnotNodeSpacerSize()
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

		static int32 ApplyNodePositions(const FGraphLayoutModel& Model)
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

		static int32 ApplyReroutePlans(
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
				if (Plan.EdgeIndex != INDEX_NONE && Plan.Points.Num() > 0)
				{
					PlanByEdge.Add(Plan.EdgeIndex, &Plan);
				}
			}

			int32 CreatedKnotCount = 0;
			TMap<FString, UK2Node_Knot*> SharedKnotByMergeKey;
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

				if (bInsertRerouteNodes && bHasPlan)
				{
					UEdGraphPin* CurrentOutput = SourcePin;
					bool bFailed = false;
					const TArray<FVector2D>& KnotPoints = (*PlanPtr)->KnotPoints.Num() == (*PlanPtr)->Points.Num()
						? (*PlanPtr)->KnotPoints
						: (*PlanPtr)->Points;
					for (int32 PointIndex = 0; PointIndex < KnotPoints.Num(); ++PointIndex)
					{
						const FVector2D& Point = KnotPoints[PointIndex];
						FString SharedKnotKey;
						if ((*PlanPtr)->AttachmentMergeGroup != INDEX_NONE)
						{
							SharedKnotKey = FString::Printf(
								TEXT("%d|%d|%d|%d"),
								(*PlanPtr)->AttachmentMergeGroup,
								PointIndex,
								FMath::RoundToInt(Point.X),
								FMath::RoundToInt(Point.Y));
						}

						UK2Node_Knot* Knot = nullptr;
						if (!SharedKnotKey.IsEmpty())
						{
							if (UK2Node_Knot** ExistingKnot = SharedKnotByMergeKey.Find(SharedKnotKey))
							{
								Knot = *ExistingKnot;
							}
						}
						if (!Knot)
						{
							Knot = CreateLayoutKnot(Graph, Point);
							if (Knot && !SharedKnotKey.IsEmpty())
							{
								SharedKnotByMergeKey.Add(SharedKnotKey, Knot);
							}
							if (Knot)
							{
								++CreatedKnotCount;
							}
						}
						if (!Knot)
						{
							bFailed = true;
							break;
						}
						if (CurrentOutput && !CurrentOutput->LinkedTo.Contains(Knot->GetInputPin()))
						{
							CurrentOutput->MakeLinkTo(Knot->GetInputPin(), true);
						}
						CurrentOutput = Knot->GetOutputPin();
					}
					if (!bFailed && CurrentOutput && !CurrentOutput->LinkedTo.Contains(TargetPin))
					{
						CurrentOutput->MakeLinkTo(TargetPin, true);
					}
				}
				else if ((bRewireAllEdges || bHasPlan) && !SourcePin->LinkedTo.Contains(TargetPin))
				{
					SourcePin->MakeLinkTo(TargetPin, true);
				}
			}
			return CreatedKnotCount;
		}

		static TArray<TSharedPtr<FJsonValue>> NodesToJson(const FGraphLayoutModel& Model)
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

		static TArray<TSharedPtr<FJsonValue>> ReroutePlansToJson(const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans)
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

		static bool ApplyLayoutReportToModel(
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
}

bool FUeAgentHttpServer::CmdBlueprintLayoutGraphSelfTest(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	(void)Ctx;
	UeAgentBlueprintAutoLayout::FSelfTestResult Result = UeAgentBlueprintAutoLayout::RunSelfTest();
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("layout_self_test_version"), TEXT("exec_region_island_corridor_v2_preferred_then_directional_avoid_two_column_anchor_20260512"));
	OutData->SetNumberField(TEXT("case_count"), Result.CaseCount);
	OutData->SetNumberField(TEXT("passed_count"), Result.PassedCount);
	OutData->SetNumberField(TEXT("failed_count"), Result.FailedCount);
	OutData->SetBoolField(TEXT("all_passed"), Result.bAllPassed);
	OutData->SetArrayField(TEXT("cases"), Result.Cases);
	if (!Result.bAllPassed)
	{
		OutError = FString::Printf(TEXT("blueprint_layout_graph_self_test_failed failed=%d passed=%d"), Result.FailedCount, Result.PassedCount);
		return false;
	}
	OutError.Reset();
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintLayoutGraph(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString GraphName = TEXT("EventGraph");
	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);

	UeAgentBlueprintAutoLayout::FOptions Options;
	JsonTryGetNumber(Ctx.Params, TEXT("horizontal_spacing"), Options.HorizontalSpacing);
	JsonTryGetNumber(Ctx.Params, TEXT("vertical_spacing"), Options.VerticalSpacing);
	JsonTryGetNumber(Ctx.Params, TEXT("node_padding"), Options.NodePadding);
	JsonTryGetNumber(Ctx.Params, TEXT("origin_x"), Options.OriginX);
	JsonTryGetNumber(Ctx.Params, TEXT("origin_y"), Options.OriginY);
	const double GraphEditorAccessOffsetX = UeAgentBlueprintOps::ComputeGraphEditorAccessOffsetX(Options.PinDirectionBiasX);
	Options.PinDirectionBiasX = GraphEditorAccessOffsetX;
	Options.OutputAccessOffsetX = GraphEditorAccessOffsetX;
	Options.InputAccessOffsetX = GraphEditorAccessOffsetX;
	Options.AccessOffsetSource = TEXT("graph_editor_settings_compute_spline_tangent");
	if (JsonTryGetNumber(Ctx.Params, TEXT("pin_direction_bias_x"), Options.PinDirectionBiasX))
	{
		Options.OutputAccessOffsetX = Options.PinDirectionBiasX;
		Options.InputAccessOffsetX = Options.PinDirectionBiasX;
		Options.AccessOffsetSource = TEXT("legacy_param_pin_direction_bias_x");
	}
	if (JsonTryGetNumber(Ctx.Params, TEXT("output_access_offset_x"), Options.OutputAccessOffsetX))
	{
		Options.AccessOffsetSource = TEXT("param_output_access_offset_x");
	}
	if (JsonTryGetNumber(Ctx.Params, TEXT("input_access_offset_x"), Options.InputAccessOffsetX))
	{
		Options.AccessOffsetSource = Options.AccessOffsetSource == TEXT("param_output_access_offset_x")
			? TEXT("param_output_and_input_access_offset_x")
			: TEXT("param_input_access_offset_x");
	}

	FString LayoutReportFile;
	JsonTryGetString(Ctx.Params, TEXT("layout_report_file"), LayoutReportFile);

	bool bDryRun = false;
	bool bCompileAfterLayout = false;
	bool bSaveAfterLayout = false;
	bool bInsertRerouteNodes = false;
	bool bReplaceExistingRerouteNodes = false;
	bool bRequireRealGeometry = true;
	bool bUseRealGeometry = true;
	bool bAllowGeometryViewCapture = false;
	bool bAllowFocusedGeometryCapture = false;
	bool bAllowRemoveAllRerouteNodes = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_layout"), bCompileAfterLayout);
	JsonTryGetBool(Ctx.Params, TEXT("save_after_layout"), bSaveAfterLayout);
	JsonTryGetBool(Ctx.Params, TEXT("insert_reroute_nodes"), bInsertRerouteNodes);
	JsonTryGetBool(Ctx.Params, TEXT("replace_existing_reroute_nodes"), bReplaceExistingRerouteNodes);
	JsonTryGetBool(Ctx.Params, TEXT("require_real_geometry"), bRequireRealGeometry);
	JsonTryGetBool(Ctx.Params, TEXT("use_real_geometry"), bUseRealGeometry);
	JsonTryGetBool(Ctx.Params, TEXT("allow_geometry_view_capture"), bAllowGeometryViewCapture);
	JsonTryGetBool(Ctx.Params, TEXT("allow_focused_geometry_capture"), bAllowFocusedGeometryCapture);
	JsonTryGetBool(Ctx.Params, TEXT("allow_remove_all_reroute_nodes"), bAllowRemoveAllRerouteNodes);
	if (bAllowFocusedGeometryCapture)
	{
		bAllowGeometryViewCapture = true;
	}
	const bool bEffectiveReplaceExistingRerouteNodes = bReplaceExistingRerouteNodes || bInsertRerouteNodes;
	const bool bUseLayoutReport = !LayoutReportFile.IsEmpty();
	if (!bUseLayoutReport)
	{
		bUseRealGeometry = true;
		bRequireRealGeometry = true;
	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}
	UEdGraph* Graph = UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, GraphName);
	if (!Graph)
	{
		OutError = TEXT("graph_not_found");
		return false;
	}

	const FString CompileStatusBefore = UeAgentBlueprintOps::BlueprintStatusToString(Blueprint->Status);
	FString CompileStatusAfter = CompileStatusBefore;
	bool bPrecompiledBeforeLayout = false;
	if (!bDryRun && bCompileAfterLayout)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		CompileStatusAfter = UeAgentBlueprintOps::BlueprintStatusToString(Blueprint->Status);
		bPrecompiledBeforeLayout = true;
	}

	auto Model = UeAgentBlueprintOps::CollectGraphLayoutModel(Graph);
	UeAgentBlueprintOps::FGraphLayoutGeometryStats GeometryStats;
	FString GeometryError;
	GeometryStats.NodeCount = Model.Nodes.Num();
	GeometryStats.GeometrySource = bUseLayoutReport ? TEXT("layout_report_file") : TEXT("real_geometry_required");
	const bool bCapturedRealGeometry = !bUseLayoutReport
		? UeAgentBlueprintOps::CaptureGraphLayoutRealGeometry(
			Blueprint,
			Graph,
			Model,
			GeometryStats,
			bAllowGeometryViewCapture,
			bAllowFocusedGeometryCapture,
			GeometryError)
		: false;
	if (!bUseLayoutReport && !bCapturedRealGeometry)
	{
		OutError = GeometryError.IsEmpty() ? TEXT("real_geometry_required_but_unavailable") : GeometryError;
		return false;
	}
	Options.bUseRealGeometry = GeometryStats.bUsedRealGeometry;
	Options.MissingPinAnchorCount = GeometryStats.MissingPinAnchorCount;
	UeAgentBlueprintAutoLayout::FRunResult RunResult;
	if (bUseLayoutReport)
	{
		GeometryStats.GeometrySource = TEXT("layout_report_file");
		if (!UeAgentBlueprintOps::ApplyLayoutReportToModel(LayoutReportFile, Model, RunResult, OutError))
		{
			return false;
		}
	}
	else
	{
		const double LayoutStartSeconds = FPlatformTime::Seconds();
		UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutAlgorithm start nodes=%d edges=%d"), Model.Nodes.Num(), Model.Edges.Num());
		if (!UeAgentBlueprintAutoLayout::RunLayout(Model.Nodes, Model.Edges, Options, RunResult))
		{
			OutError = TEXT("layout_algorithm_failed");
			return false;
		}
			UE_LOG(LogUeAgentInterface, Display, TEXT("BlueprintLayoutAlgorithm done elapsed_ms=%.1f reroutes=%d bus_contracts=%d route_through=%d hard=%d design_complete=%s"),
				(FPlatformTime::Seconds() - LayoutStartSeconds) * 1000.0,
				RunResult.ReroutePlans.Num(),
				RunResult.BusContractDiagnostics.Num(),
				RunResult.DesignDiagnostics.RouteThroughIslandCount,
				RunResult.DesignDiagnostics.RouteThroughHardOccupancyCount,
				RunResult.bDesignComplete ? TEXT("true") : TEXT("false"));
	}
	if (!bDryRun
		&& !bUseLayoutReport
		&& (RunResult.DesignDiagnostics.IslandLocalMembershipMutationCount > 0
			|| RunResult.DesignDiagnostics.IslandLocalMutationCount > 0))
	{
		OutError = TEXT("island_local_layout_mutation_blocked");
		return false;
	}

	const int32 ExistingRerouteNodeCount = UeAgentBlueprintOps::CountExistingRerouteNodes(Graph);
	const int32 PlannedRerouteKnotNodeCount = UeAgentBlueprintOps::CountPlannedRerouteKnotNodes(RunResult.ReroutePlans);
	if (!bDryRun
		&& !bUseLayoutReport
		&& bEffectiveReplaceExistingRerouteNodes
		&& ExistingRerouteNodeCount > 0
		&& PlannedRerouteKnotNodeCount == 0
		&& !bAllowRemoveAllRerouteNodes)
	{
		OutError = FString::Printf(
			TEXT("reroute_delete_all_guard_blocked existing_reroute_nodes=%d planned_reroute_knots=0; pass allow_remove_all_reroute_nodes=true only when intentionally flattening every existing reroute"),
			ExistingRerouteNodeCount);
		return false;
	}

	int32 RemovedRerouteNodeCount = 0;
	int32 CreatedRerouteNodeCount = 0;
	int32 AppliedMoveCount = 0;
	bool bChanged = false;
	bool bSaved = false;
	FString SaveError;

	if (!bDryRun)
	{
		Graph->Modify();
		Blueprint->Modify();
		if (bEffectiveReplaceExistingRerouteNodes)
		{
			RemovedRerouteNodeCount = UeAgentBlueprintOps::RemoveExistingRerouteNodes(Graph);
		}

		AppliedMoveCount = UeAgentBlueprintOps::ApplyNodePositions(Model);
		CreatedRerouteNodeCount = UeAgentBlueprintOps::ApplyReroutePlans(
			Graph,
			Model,
			RunResult.ReroutePlans,
			bInsertRerouteNodes,
			bEffectiveReplaceExistingRerouteNodes);

		bChanged = AppliedMoveCount > 0 || RemovedRerouteNodeCount > 0 || CreatedRerouteNodeCount > 0;
		if (bChanged)
		{
			Graph->NotifyGraphChanged();
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		if (bCompileAfterLayout)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			CompileStatusAfter = UeAgentBlueprintOps::BlueprintStatusToString(Blueprint->Status);
		}

		if (bSaveAfterLayout)
		{
			bSaved = UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, SaveError);
			if (!bSaved)
			{
				OutError = SaveError.IsEmpty() ? TEXT("save_failed") : SaveError;
				return false;
			}
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), AssetPathInput);
	OutData->SetStringField(TEXT("graph_name"), GraphName);
	OutData->SetStringField(TEXT("layout_strategy"), RunResult.Strategy);
	OutData->SetStringField(TEXT("selected_candidate"), TEXT("clean_rewrite"));
	OutData->SetStringField(TEXT("layout_framework"), TEXT("clean_semantic_island_multiport_layout"));
	OutData->SetStringField(TEXT("layout_report_file"), LayoutReportFile);
	OutData->SetBoolField(TEXT("layout_report_restore"), bUseLayoutReport);
	OutData->SetNumberField(TEXT("output_access_offset_x"), Options.OutputAccessOffsetX);
	OutData->SetNumberField(TEXT("input_access_offset_x"), Options.InputAccessOffsetX);
	OutData->SetNumberField(TEXT("pin_direction_bias_x"), Options.PinDirectionBiasX);
	OutData->SetStringField(TEXT("access_offset_source"), Options.AccessOffsetSource);
	OutData->SetBoolField(TEXT("require_real_geometry"), bRequireRealGeometry);
	OutData->SetBoolField(TEXT("use_real_geometry"), bUseRealGeometry);
	OutData->SetBoolField(TEXT("allow_geometry_view_capture"), bAllowGeometryViewCapture);
	OutData->SetBoolField(TEXT("allow_focused_geometry_capture"), bAllowFocusedGeometryCapture);
	OutData->SetBoolField(TEXT("real_geometry_capture_requested"), GeometryStats.bRealGeometryCaptureRequested);
	OutData->SetStringField(TEXT("geometry_source"), GeometryStats.GeometrySource);
	OutData->SetBoolField(TEXT("used_real_geometry"), GeometryStats.bUsedRealGeometry);
	OutData->SetBoolField(TEXT("geometry_window_prepared"), GeometryStats.bWindowPrepared);
	OutData->SetNumberField(TEXT("measured_node_count"), GeometryStats.MeasuredNodeCount);
	OutData->SetNumberField(TEXT("pin_widget_count"), GeometryStats.PinWidgetCount);
	OutData->SetNumberField(TEXT("pin_geometry_anchor_count"), GeometryStats.PinGeometryAnchorCount);
	OutData->SetNumberField(TEXT("missing_visible_pin_anchor_count"), GeometryStats.VisibleMissingPinAnchorCount);
		OutData->SetNumberField(TEXT("missing_pin_anchor_count"), GeometryStats.MissingPinAnchorCount);
		OutData->SetNumberField(TEXT("focused_geometry_node_count"), GeometryStats.FocusedGeometryNodeCount);
		OutData->SetBoolField(TEXT("focused_geometry_budget_exhausted"), GeometryStats.bFocusedGeometryBudgetExhausted);
		OutData->SetBoolField(TEXT("geometry_restore_view_state_from_document"), GeometryStats.bRestoreViewStateFromDocument);
		OutData->SetBoolField(TEXT("geometry_restored_graph_view_after_capture"), GeometryStats.bRestoredGraphViewAfterCapture);
		OutData->SetNumberField(TEXT("geometry_restored_view_x"), GeometryStats.RestoredViewLocation.X);
		OutData->SetNumberField(TEXT("geometry_restored_view_y"), GeometryStats.RestoredViewLocation.Y);
		OutData->SetNumberField(TEXT("geometry_restored_zoom"), GeometryStats.RestoredZoomAmount);
		OutData->SetNumberField(TEXT("geometry_measurement_zoom"), GeometryStats.GeometryMeasurementZoomAmount);
		if (!GeometryStats.Error.IsEmpty())
		{
			OutData->SetStringField(TEXT("geometry_error"), GeometryStats.Error);
		}
	OutData->SetBoolField(TEXT("layout_design_complete"), RunResult.bDesignComplete);
	OutData->SetNumberField(TEXT("layout_design_feature_count"), RunResult.DesignFeatureCount);
	OutData->SetNumberField(TEXT("layout_design_feature_complete_count"), RunResult.DesignFeatureCompleteCount);
	OutData->SetNumberField(TEXT("layout_design_feature_partial_count"), RunResult.DesignFeaturePartialCount);
	OutData->SetNumberField(TEXT("layout_design_feature_missing_count"), RunResult.DesignFeatureMissingCount);
	OutData->SetArrayField(TEXT("layout_design_feature_coverage"), RunResult.DesignFeatureCoverage);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("changed"), bChanged);
	OutData->SetBoolField(TEXT("saved"), bSaved);
	OutData->SetBoolField(TEXT("compile_after_layout"), bCompileAfterLayout);
	OutData->SetBoolField(TEXT("precompiled_before_layout"), bPrecompiledBeforeLayout);
	OutData->SetStringField(TEXT("compile_status_before"), CompileStatusBefore);
	OutData->SetStringField(TEXT("compile_status_after"), CompileStatusAfter);
	OutData->SetBoolField(TEXT("insert_reroute_nodes"), bInsertRerouteNodes);
	OutData->SetBoolField(TEXT("replace_existing_reroute_nodes"), bReplaceExistingRerouteNodes);
	OutData->SetBoolField(TEXT("effective_replace_existing_reroute_nodes"), bEffectiveReplaceExistingRerouteNodes);
	OutData->SetBoolField(TEXT("allow_remove_all_reroute_nodes"), bAllowRemoveAllRerouteNodes);
	OutData->SetNumberField(TEXT("existing_reroute_node_count"), ExistingRerouteNodeCount);
	OutData->SetNumberField(TEXT("planned_reroute_knot_node_count"), PlannedRerouteKnotNodeCount);
	const FVector2D RerouteKnotSpacer = UeAgentBlueprintOps::GetLayoutKnotNodeSpacerSize();
	OutData->SetNumberField(TEXT("reroute_knot_node_spacer_x"), RerouteKnotSpacer.X);
	OutData->SetNumberField(TEXT("reroute_knot_node_spacer_y"), RerouteKnotSpacer.Y);
	OutData->SetNumberField(TEXT("reroute_knot_control_point_to_node_offset_x"), RerouteKnotSpacer.X * 0.5);
	OutData->SetNumberField(TEXT("reroute_knot_control_point_to_node_offset_y"), RerouteKnotSpacer.Y * 0.5);
	OutData->SetNumberField(TEXT("node_count"), RunResult.NodeCount);
	OutData->SetNumberField(TEXT("edge_count"), RunResult.EdgeCount);
	OutData->SetNumberField(TEXT("moved_node_count"), RunResult.MovedNodeCount);
	OutData->SetNumberField(TEXT("applied_move_count"), AppliedMoveCount);
	OutData->SetNumberField(TEXT("removed_reroute_node_count"), RemovedRerouteNodeCount);
	OutData->SetNumberField(TEXT("created_reroute_node_count"), CreatedRerouteNodeCount);
	OutData->SetNumberField(TEXT("reroute_plan_count"), RunResult.ReroutePlanCount);
	OutData->SetNumberField(TEXT("local_bus_edge_count"), RunResult.LocalBusEdgeCount);
	OutData->SetNumberField(TEXT("external_bus_edge_count"), RunResult.ExternalBusEdgeCount);
	OutData->SetNumberField(TEXT("data_island_count"), RunResult.IslandCount);
	OutData->SetNumberField(TEXT("island_edge_count"), RunResult.IslandEdgeCount);
	OutData->SetNumberField(TEXT("rank_count"), RunResult.RankCount);
	OutData->SetNumberField(TEXT("alignment_failed_unit_count"), RunResult.AlignmentFailedUnitCount);
	OutData->SetNumberField(TEXT("invalid_island_edge_count"), RunResult.InvalidIslandEdgeCount);
	OutData->SetNumberField(TEXT("scc_count_before_cut"), RunResult.DesignDiagnostics.SccCountBeforeCut);
	OutData->SetNumberField(TEXT("scc_feedback_edge_count"), RunResult.DesignDiagnostics.SccFeedbackEdgeCount);
	OutData->SetNumberField(TEXT("fanout_node_count"), RunResult.DesignDiagnostics.FanoutNodeCount);
	OutData->SetNumberField(TEXT("fanout_implicit_edge_count"), RunResult.DesignDiagnostics.FanoutImplicitEdgeCount);
	OutData->SetNumberField(TEXT("comment_island_count"), RunResult.DesignDiagnostics.CommentIslandCount);
	OutData->SetNumberField(TEXT("comment_island_node_count"), RunResult.DesignDiagnostics.CommentIslandNodeCount);
	OutData->SetNumberField(TEXT("comment_island_boundary_edge_count"), RunResult.DesignDiagnostics.CommentIslandBoundaryEdgeCount);
	OutData->SetNumberField(TEXT("rank_ignored_edge_count"), RunResult.DesignDiagnostics.RankIgnoredEdgeCount);
	OutData->SetNumberField(TEXT("island_cut_edge_count"), RunResult.DesignDiagnostics.IslandCutEdgeCount);
		OutData->SetNumberField(TEXT("implicit_edge_count"), RunResult.DesignDiagnostics.ImplicitEdgeCount);
		OutData->SetNumberField(TEXT("node_island_count"), RunResult.DesignDiagnostics.NodeIslandCount);
		OutData->SetNumberField(TEXT("multi_sink_island_count"), RunResult.DesignDiagnostics.MultiSinkIslandCount);
		OutData->SetNumberField(TEXT("input_chain_count"), RunResult.DesignDiagnostics.InputChainCount);
		OutData->SetNumberField(TEXT("input_chain_node_count"), RunResult.DesignDiagnostics.InputChainNodeCount);
		OutData->SetNumberField(TEXT("input_chain_forbidden_zone_count"), RunResult.DesignDiagnostics.InputChainForbiddenZoneCount);
		OutData->SetNumberField(TEXT("input_chain_forbidden_avoidance_count"), RunResult.DesignDiagnostics.InputChainForbiddenAvoidanceCount);
		OutData->SetNumberField(TEXT("input_chain_collision_reroute_count"), RunResult.DesignDiagnostics.InputChainCollisionRerouteCount);
		OutData->SetNumberField(TEXT("input_chain_collision_reroute_rejected_count"), RunResult.DesignDiagnostics.InputChainCollisionRerouteRejectedCount);
		OutData->SetNumberField(TEXT("implicit_edge_inside_island_count"), RunResult.DesignDiagnostics.ImplicitEdgeInsideIslandCount);
	OutData->SetNumberField(TEXT("source_param_count"), RunResult.DesignDiagnostics.SourceParamCount);
	OutData->SetNumberField(TEXT("internal_bus_group_count"), RunResult.DesignDiagnostics.InternalBusGroupCount);
	OutData->SetNumberField(TEXT("boundary_entry_count"), RunResult.DesignDiagnostics.BoundaryEntryCount);
	OutData->SetNumberField(TEXT("left_boundary_entry_count"), RunResult.DesignDiagnostics.LeftBoundaryEntryCount);
	OutData->SetNumberField(TEXT("top_boundary_entry_count"), RunResult.DesignDiagnostics.TopBoundaryEntryCount);
	OutData->SetNumberField(TEXT("bottom_boundary_entry_count"), RunResult.DesignDiagnostics.BottomBoundaryEntryCount);
	OutData->SetNumberField(TEXT("boundary_bus_required_count"), RunResult.DesignDiagnostics.BoundaryBusRequiredCount);
	OutData->SetNumberField(TEXT("boundary_bus_count"), RunResult.DesignDiagnostics.BoundaryBusCount);
	OutData->SetNumberField(TEXT("internal_attach_merge_group_count"), RunResult.DesignDiagnostics.InternalAttachMergeGroupCount);
	OutData->SetNumberField(TEXT("boundary_attach_merge_group_count"), RunResult.DesignDiagnostics.BoundaryAttachMergeGroupCount);
	OutData->SetNumberField(TEXT("boundary_entry_rejected_candidate_count"), RunResult.DesignDiagnostics.BoundaryEntryRejectedCandidateCount);
	OutData->SetNumberField(TEXT("anchored_column_placement_count"), RunResult.DesignDiagnostics.AnchoredColumnPlacementCount);
	OutData->SetNumberField(TEXT("anchored_column_unconnected_item_count"), RunResult.DesignDiagnostics.AnchoredColumnUnconnectedItemCount);
	OutData->SetNumberField(TEXT("anchored_column_total_pin_residual"), RunResult.DesignDiagnostics.AnchoredColumnTotalPinResidual);
	OutData->SetNumberField(TEXT("anchored_column_max_pin_residual"), RunResult.DesignDiagnostics.AnchoredColumnMaxPinResidual);
	OutData->SetNumberField(TEXT("island_internal_placement_count"), RunResult.DesignDiagnostics.IslandInternalPlacementCount);
	OutData->SetNumberField(TEXT("island_internal_left_boundary_entry_count"), RunResult.DesignDiagnostics.IslandInternalLeftBoundaryEntryCount);
	OutData->SetNumberField(TEXT("island_internal_total_pin_residual"), RunResult.DesignDiagnostics.IslandInternalTotalPinResidual);
	OutData->SetNumberField(TEXT("island_internal_max_pin_residual"), RunResult.DesignDiagnostics.IslandInternalMaxPinResidual);
	OutData->SetNumberField(TEXT("island_local_frozen_node_count"), RunResult.DesignDiagnostics.IslandLocalFrozenNodeCount);
	OutData->SetNumberField(TEXT("island_local_membership_mutation_count"), RunResult.DesignDiagnostics.IslandLocalMembershipMutationCount);
	OutData->SetNumberField(TEXT("island_local_mutation_count"), RunResult.DesignDiagnostics.IslandLocalMutationCount);
	OutData->SetNumberField(TEXT("island_local_max_mutation"), RunResult.DesignDiagnostics.IslandLocalMaxMutation);
	OutData->SetNumberField(TEXT("endpoint_candidate_count"), RunResult.DesignDiagnostics.EndpointCandidateCount);
	OutData->SetNumberField(TEXT("placement_graph_edge_count"), RunResult.DesignDiagnostics.PlacementGraphEdgeCount);
	OutData->SetNumberField(TEXT("route_only_edge_count"), RunResult.DesignDiagnostics.RouteOnlyEdgeCount);
	OutData->SetNumberField(TEXT("route_only_cycle_break_edge_count"), RunResult.DesignDiagnostics.RouteOnlyCycleBreakEdgeCount);
	OutData->SetNumberField(TEXT("route_only_middle_corridor_count"), RunResult.DesignDiagnostics.RouteOnlyMiddleCorridorCount);
	OutData->SetNumberField(TEXT("route_only_outer_fallback_count"), RunResult.DesignDiagnostics.RouteOnlyOuterFallbackCount);
	OutData->SetNumberField(TEXT("source_order_group_count"), RunResult.DesignDiagnostics.SourceOrderGroupCount);
	OutData->SetNumberField(TEXT("fanout_intent_group_count"), RunResult.DesignDiagnostics.FanoutIntentGroupCount);
	OutData->SetNumberField(TEXT("placement_proposal_count"), RunResult.DesignDiagnostics.PlacementProposalCount);
	OutData->SetNumberField(TEXT("rejected_proposal_count"), RunResult.DesignDiagnostics.RejectedProposalCount);
	OutData->SetNumberField(TEXT("source_port_order_violation_count"), RunResult.DesignDiagnostics.SourcePortOrderViolationCount);
	OutData->SetNumberField(TEXT("invalid_forward_connection_count"), RunResult.DesignDiagnostics.InvalidForwardConnectionCount);
	OutData->SetNumberField(TEXT("bus_contract_count"), RunResult.DesignDiagnostics.BusContractCount);
	OutData->SetNumberField(TEXT("selected_endpoint_candidate_count"), RunResult.DesignDiagnostics.SelectedEndpointCandidateCount);
	OutData->SetNumberField(TEXT("connection_group_count"), RunResult.DesignDiagnostics.ConnectionGroupCount);
	OutData->SetNumberField(TEXT("route_group_count"), RunResult.DesignDiagnostics.RouteGroupCount);
	OutData->SetNumberField(TEXT("route_kind_upgrade_count"), RunResult.DesignDiagnostics.RouteKindUpgradeCount);
	OutData->SetNumberField(TEXT("branch_merge_group_count"), RunResult.DesignDiagnostics.BranchMergeGroupCount);
	OutData->SetNumberField(TEXT("cross_bus_trunk_lane_count"), RunResult.DesignDiagnostics.CrossBusTrunkLaneCount);
	OutData->SetNumberField(TEXT("cross_bus_lane_count"), RunResult.DesignDiagnostics.CrossBusLaneCount);
	OutData->SetNumberField(TEXT("route_through_hard_occupancy_count"), RunResult.DesignDiagnostics.RouteThroughHardOccupancyCount);
	OutData->SetNumberField(TEXT("island_connection_total_length"), RunResult.DesignDiagnostics.IslandConnectionTotalLength);
	OutData->SetNumberField(TEXT("total_route_length"), RunResult.DesignDiagnostics.TotalRouteLength);
	OutData->SetNumberField(TEXT("bus_y_variance_total"), RunResult.DesignDiagnostics.BusYVarianceTotal);
	OutData->SetNumberField(TEXT("island_connection_external_upgrade_count"), RunResult.DesignDiagnostics.IslandConnectionExternalUpgradeCount);
	OutData->SetNumberField(TEXT("island_connection_top_bottom_x_residual"), RunResult.DesignDiagnostics.IslandConnectionTopBottomXResidual);
	OutData->SetNumberField(TEXT("island_connection_left_y_residual"), RunResult.DesignDiagnostics.IslandConnectionLeftYResidual);
	OutData->SetNumberField(TEXT("placement_component_count"), RunResult.DesignDiagnostics.PlacementComponentCount);
	OutData->SetNumberField(TEXT("lane_coloring_lane_count"), RunResult.DesignDiagnostics.LaneColoringLaneCount);
	OutData->SetNumberField(TEXT("route_through_island_count"), RunResult.DesignDiagnostics.RouteThroughIslandCount);
	OutData->SetObjectField(TEXT("metrics_before"), UeAgentBlueprintAutoLayout::MetricsToJson(RunResult.MetricsBefore));
	OutData->SetObjectField(TEXT("metrics_after"), UeAgentBlueprintAutoLayout::MetricsToJson(RunResult.MetricsAfter));
	OutData->SetObjectField(TEXT("metrics_after_reroute_plan"), UeAgentBlueprintAutoLayout::MetricsToJson(RunResult.MetricsAfterReroute));
	OutData->SetNumberField(TEXT("overlap_count_after"), RunResult.MetricsAfter.OverlapCount);
	OutData->SetNumberField(TEXT("edge_crossing_count_after"), RunResult.MetricsAfter.EdgeCrossingCount);
	OutData->SetNumberField(TEXT("edge_node_intersection_count_after"), RunResult.MetricsAfter.EdgeNodeIntersectionCount);
	OutData->SetArrayField(TEXT("nodes"), UeAgentBlueprintOps::NodesToJson(Model));
	OutData->SetArrayField(TEXT("reroute_plan"), UeAgentBlueprintOps::ReroutePlansToJson(RunResult.ReroutePlans));
	OutData->SetArrayField(TEXT("island_diagnostics"), RunResult.IslandDiagnostics);
	OutData->SetArrayField(TEXT("island_edge_diagnostics"), RunResult.IslandEdgeDiagnostics);
	OutData->SetArrayField(TEXT("column_diagnostics"), RunResult.ColumnDiagnostics);
	OutData->SetArrayField(TEXT("input_chains"), RunResult.InputChainDiagnostics);
	OutData->SetArrayField(TEXT("internal_bus_groups"), RunResult.InternalBusGroupDiagnostics);
	OutData->SetArrayField(TEXT("boundary_entries"), RunResult.BoundaryEntryDiagnostics);
	OutData->SetArrayField(TEXT("boundary_buses"), RunResult.BoundaryBusDiagnostics);
	OutData->SetArrayField(TEXT("island_local_mutations"), RunResult.IslandLocalMutationDiagnostics);
	OutData->SetArrayField(TEXT("island_port_candidates"), RunResult.IslandPortCandidateDiagnostics);
	OutData->SetArrayField(TEXT("placement_proposals"), RunResult.PlacementProposalDiagnostics);
	OutData->SetArrayField(TEXT("source_order_groups"), RunResult.SourceOrderGroupDiagnostics);
	OutData->SetArrayField(TEXT("fanout_intent_groups"), RunResult.FanoutIntentGroupDiagnostics);
	OutData->SetArrayField(TEXT("selected_endpoint_candidates"), RunResult.SelectedEndpointCandidateDiagnostics);
	OutData->SetArrayField(TEXT("bus_contracts"), RunResult.BusContractDiagnostics);
	OutData->SetArrayField(TEXT("branch_merge_groups"), RunResult.BranchMergeDiagnostics);
	OutData->SetArrayField(TEXT("cross_bus_lanes"), RunResult.CrossBusLaneDiagnostics);
	OutData->SetArrayField(TEXT("corridor_routes"), RunResult.CorridorRouteDiagnostics);
	OutData->SetArrayField(TEXT("lane_assignments"), RunResult.LaneAssignmentDiagnostics);
	OutData->SetArrayField(TEXT("rejection_diagnostics"), RunResult.RejectionDiagnostics);
	return true;
}
