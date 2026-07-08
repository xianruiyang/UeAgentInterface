// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_NodeGraphOps.h"
#include "UeAgentHttpServer_NodeGraphOpsInternal.h"
#include "Commands/Blueprint/Layout/Core/UeAgentBlueprintAutoLayoutAlgorithm.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphEditor.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace UeAgentNodeGraphOps
{
	namespace Internal
	{
		void PumpAndTickSlateForLayoutGeometry()
		{
			if (!FSlateApplication::IsInitialized())
			{
				return;
			}
			FSlateApplication& SlateApp = FSlateApplication::Get();
			SlateApp.PumpMessages();
			if (!SlateApp.IsTicking())
			{
				SlateApp.Tick(ESlateTickType::All);
			}
		}

		bool IsFiniteLayoutPoint(const FVector2D& Point)
		{
			return FMath::IsFinite(Point.X) && FMath::IsFinite(Point.Y);
		}
		bool IsUsableSlateGeometry(const FGeometry& Geometry)
		{
			const FVector2f Size = Geometry.GetLocalSize();
			const FVector2f AbsolutePosition = Geometry.GetAbsolutePosition();
			return Size.X > 0.5f
				&& Size.Y > 0.5f
				&& FMath::IsFinite(AbsolutePosition.X)
				&& FMath::IsFinite(AbsolutePosition.Y);
		}
		bool TryGetUsableWidgetGeometry(const SWidget& Widget, FGeometry& OutGeometry)
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
		void SetMeasuredPinAnchor(
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
		bool TryGetMeasuredPinAnchor(
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
		int32 CountMeasuredPinAnchors(const UeAgentBlueprintAutoLayout::FNode& Node, const EEdGraphPinDirection Direction)
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
		FVector2f SnapViewLocationToPixelGrid(const FVector2f& Location, const float ZoomAmount)
		{
			const float SafeZoom = FMath::Max(ZoomAmount, 0.01f);
			return FVector2f(
				FMath::RoundToFloat(Location.X * SafeZoom) / SafeZoom,
				FMath::RoundToFloat(Location.Y * SafeZoom) / SafeZoom);
		}
		void FindArrangedDescendantGeometries(
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
		FVector2f GetGraphPinConnectionPointAbsolute(
			const FGeometry& PinGeometry,
			const EEdGraphPinDirection Direction,
			const float ZoomAmount)
		{
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
		bool TryGetCachedPinNodeOffsetY(const SGraphPin& PinWidget, double& OutNodeOffsetY)
		{
			// Match the data used by SGraphPanel's built-in alignment code:
			// NodePosY + SGraphPin::GetNodeOffset().Y. In minimized/headless paths the
			// widget may not have ticked yet, and UE leaves this cache at 0; treat that
			// as unavailable and fall back to arranged geometry.
			const FVector2f CachedPinNodeOffset = UE::Slate::CastToVector2f(PinWidget.GetNodeOffset());
			if (!FMath::IsFinite(CachedPinNodeOffset.Y) || FMath::Abs(CachedPinNodeOffset.Y) <= 0.5f)
			{
				return false;
			}
			OutNodeOffsetY = static_cast<double>(CachedPinNodeOffset.Y);
			return true;
		}
		bool TryConvertAbsolutePinPointToNodeAnchor(
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
		bool TryGetArrangedPinAnchor(
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
		bool TryGetTickSpacePinAnchor(
			SGraphPanel& GraphPanel,
			SGraphNode& GraphNodeWidget,
			const TSharedRef<SGraphPin>& PinWidget,
			const FVector2D& NodeGraphPosition,
			const EEdGraphPinDirection Direction,
			FVector2D& OutAnchor)
		{
			return TryGetArrangedPinAnchor(GraphPanel, GraphNodeWidget, PinWidget, NodeGraphPosition, Direction, OutAnchor);
		}
		bool OpenEditorForGeometryIfNeeded(UObject* Asset, bool& bOpenedEditor)
		{
			bOpenedEditor = false;
			if (!Asset || !GEditor)
			{
				return false;
			}
			if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				if (!AssetEditorSubsystem->FindEditorForAsset(Asset, false))
				{
					if (!AssetEditorSubsystem->OpenEditorForAsset(Asset))
					{
						return false;
					}
					bOpenedEditor = true;
				}
				PumpAndTickSlateForLayoutGeometry();
				PumpAndTickSlateForLayoutGeometry();
				return true;
			}
			return false;
		}
		bool CaptureSharedGraphRealGeometry(
			UObject* Asset,
			UEdGraph* Graph,
			FUniversalLayoutModel& Model,
			FSharedGraphGeometryStats& OutStats,
			FString& OutError,
			const bool bOpenAssetEditorForGeometry)
		{
			OutStats = FSharedGraphGeometryStats();
			OutStats.NodeCount = Model.Nodes.Num();
			OutStats.GeometrySource = TEXT("slate_graph_editor_bounds_and_arranged_pin_geometry");
			OutStats.GeometryMeasurementZoomAmount = 1.0f;

			if (!Asset || !Graph)
			{
				OutError = TEXT("asset_or_graph_missing_for_real_geometry");
				OutStats.Error = OutError;
				return false;
			}
			if (!FSlateApplication::IsInitialized())
			{
				OutError = TEXT("slate_not_initialized_for_real_geometry");
				OutStats.Error = OutError;
				return false;
			}

			bool bOpenedAssetEditor = false;
			if (bOpenAssetEditorForGeometry)
			{
				OpenEditorForGeometryIfNeeded(Asset, bOpenedAssetEditor);
			}

			TSharedPtr<SGraphEditor> OwnedGeometryGraphEditor;
			FGraphAppearanceInfo AppearanceInfo;
			AppearanceInfo.CornerText = FText::FromString(TEXT("UAI real geometry probe"));
			OwnedGeometryGraphEditor = SNew(SGraphEditor)
				.IsEditable(false)
				.DisplayAsReadOnly(true)
				.ShowGraphStateOverlay(false)
				.Appearance(AppearanceInfo)
				.GraphToEdit(Graph);
			TSharedPtr<SGraphEditor> GraphEditor = OwnedGeometryGraphEditor;
			OutStats.bOpenedEditorForGeometry = bOpenedAssetEditor;
			OutStats.bUsedOffscreenGraphEditor = true;
			PumpAndTickSlateForLayoutGeometry();
			SGraphPanel* GraphPanel = GraphEditor->GetGraphPanel();
			if (!GraphPanel)
			{
				OutError = TEXT("graph_panel_not_found_for_real_geometry");
				OutStats.Error = OutError;
				return false;
			}

			FVector2f SavedViewLocation = FVector2f::ZeroVector;
			float SavedZoom = 1.0f;
			GraphEditor->GetViewLocation(SavedViewLocation, SavedZoom);
			OutStats.RestoredViewLocation = SavedViewLocation;
			OutStats.RestoredZoomAmount = SavedZoom;

			auto RestoreGraphView = [&]()
			{
				GraphEditor->SetViewLocation(SnapViewLocationToPixelGrid(SavedViewLocation, SavedZoom), SavedZoom);
				OutStats.bRestoredGraphViewAfterCapture = true;
				PumpAndTickSlateForLayoutGeometry();
			};

			auto RefreshGraphPanelLayout = [&](const int32 FrameCount)
			{
				for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
				{
					GraphPanel->Update();
					PumpAndTickSlateForLayoutGeometry();
					const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
					GraphEditor->SlatePrepass(WidgetScale);
					GraphPanel->SlatePrepass(WidgetScale);
				}
			};

			GraphEditor->SetViewLocation(SnapViewLocationToPixelGrid(SavedViewLocation, 1.0f), 1.0f);
			for (const FUniversalLayoutNodeRef& Ref : Model.NodeRefs)
			{
				if (Ref.GraphNode)
				{
					GraphPanel->RefreshNode(*Ref.GraphNode);
				}
			}
			RefreshGraphPanelLayout(3);

			for (UeAgentBlueprintAutoLayout::FNode& Node : Model.Nodes)
			{
				Node.InputPinAnchorValid.Init(0, Node.InputPinAnchors.Num());
				Node.OutputPinAnchorValid.Init(0, Node.OutputPinAnchors.Num());
				Node.bMeasuredGeometry = false;
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

			auto CaptureNodeSize = [&](UeAgentBlueprintAutoLayout::FNode& LayoutNode, UEdGraphNode* GraphNode) -> bool
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
				auto MakeMeasuredSize = [](const double Width, const double Height)
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
				const FMeasuredNodeSize ChosenSize = DesiredSize.bValid ? DesiredSize : BoundsSize;
				if (!ChosenSize.bValid)
				{
					LayoutNode.bMeasuredGeometry = false;
					return false;
				}
				LayoutNode.Width = FMath::Clamp(ChosenSize.Width, 1.0, 5000.0);
				LayoutNode.Height = FMath::Clamp(ChosenSize.Height, 1.0, 5000.0);
				LayoutNode.bMeasuredGeometry = true;
				return true;
			};

			auto CapturePinAnchorsForNode = [&](UeAgentBlueprintAutoLayout::FNode& LayoutNode, UEdGraphNode* GraphNode, const bool bCountPinWidgets)
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
					if (!IsVisibleSharedGraphPin(Pin) || (Pin->Direction != EGPD_Input && Pin->Direction != EGPD_Output))
					{
						continue;
					}
					const int32 PinIndex = GetSharedGraphPinIndexInDirection(Pin);
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
					const FVector2f WidgetNodeGraphPosition2f = GraphNodeWidget->GetPosition2f();
					const FVector2D WidgetNodeGraphPosition(
						static_cast<double>(WidgetNodeGraphPosition2f.X),
						static_cast<double>(WidgetNodeGraphPosition2f.Y));
					FVector2D Anchor = FVector2D::ZeroVector;
					if (TryGetTickSpacePinAnchor(*GraphPanel, *GraphNodeWidget, PinWidget.ToSharedRef(), WidgetNodeGraphPosition, Pin->Direction, Anchor))
					{
						SetMeasuredPinAnchor(LayoutNode, Pin->Direction, PinIndex, Anchor);
						++OutStats.PinGeometryAnchorCount;
					}
				}
			};

			for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
			{
				UEdGraphNode* GraphNode = Model.NodeRefs.IsValidIndex(NodeIndex) ? Model.NodeRefs[NodeIndex].GraphNode : nullptr;
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

			constexpr double MaxFocusedCaptureSeconds = 30.0;
			constexpr int32 MaxFocusedCapturePasses = 2;
			const double FocusedStartSeconds = FPlatformTime::Seconds();
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
			for (int32 PassIndex = 0; PassIndex < MaxFocusedCapturePasses; ++PassIndex)
			{
				const int32 MissingAtPassStart = CountAllMissingUsedPinAnchors();
				if (MissingAtPassStart <= 0)
				{
					break;
				}
				for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
				{
					if ((FPlatformTime::Seconds() - FocusedStartSeconds) > MaxFocusedCaptureSeconds)
					{
						OutStats.bFocusedGeometryBudgetExhausted = true;
						break;
					}
					UEdGraphNode* GraphNode = Model.NodeRefs.IsValidIndex(NodeIndex) ? Model.NodeRefs[NodeIndex].GraphNode : nullptr;
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
					GraphEditor->SetViewLocation(SnapViewLocationToPixelGrid(FocusedViewLocation, 1.0f), 1.0f);
					++OutStats.FocusedGeometryNodeCount;
					RefreshGraphPanelLayout(2);
					CaptureNodeSize(Model.Nodes[NodeIndex], GraphNode);
					CapturePinAnchorsForNode(Model.Nodes[NodeIndex], GraphNode, false);
				}
				if (OutStats.bFocusedGeometryBudgetExhausted || CountAllMissingUsedPinAnchors() >= MissingAtPassStart)
				{
					break;
				}
			}

			OutStats.MissingPinAnchorCount = 0;
			OutStats.VisibleMissingPinAnchorCount = 0;
			int32 MissingNodeGeometryCount = 0;
			TArray<FString> MissingNodeGeometrySamples;
			TArray<FString> MissingUsedPinAnchorSamples;
			for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
			{
				const UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[NodeIndex];
				OutStats.VisibleMissingPinAnchorCount += Node.InputPinNames.Num() - CountMeasuredPinAnchors(Node, EGPD_Input);
				OutStats.VisibleMissingPinAnchorCount += Node.OutputPinNames.Num() - CountMeasuredPinAnchors(Node, EGPD_Output);
				if (!Node.bMeasuredGeometry)
				{
					++MissingNodeGeometryCount;
					if (MissingNodeGeometrySamples.Num() < 8)
					{
						UEdGraphNode* GraphNode = Model.NodeRefs.IsValidIndex(NodeIndex) ? Model.NodeRefs[NodeIndex].GraphNode : nullptr;
						const FString GraphNodeName = GraphNode ? GraphNode->GetName() : TEXT("<null>");
						const FString ClassName = GraphNode ? GraphNode->GetClass()->GetName() : TEXT("<null>");
						MissingNodeGeometrySamples.Add(FString::Printf(
							TEXT("%d:%s:%s:%s"),
							NodeIndex,
							*Node.Title,
							*ClassName,
							*GraphNodeName));
					}
				}
			}
			for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
			{
				OutStats.MissingPinAnchorCount += CountMissingUsedPinAnchors(NodeIndex, EGPD_Input);
				OutStats.MissingPinAnchorCount += CountMissingUsedPinAnchors(NodeIndex, EGPD_Output);
				const UeAgentBlueprintAutoLayout::FNode& Node = Model.Nodes[NodeIndex];
				auto AddMissingUsedPinSamples = [&](const EEdGraphPinDirection Direction)
				{
					const TSet<int32>& UsedPins = Direction == EGPD_Input ? UsedInputPinIndices[NodeIndex] : UsedOutputPinIndices[NodeIndex];
					const TArray<FString>& PinNames = Direction == EGPD_Input ? Node.InputPinNames : Node.OutputPinNames;
					for (const int32 PinIndex : UsedPins)
					{
						FVector2D Anchor = FVector2D::ZeroVector;
						if (TryGetMeasuredPinAnchor(Node, Direction, PinIndex, Anchor))
						{
							continue;
						}
						if (MissingUsedPinAnchorSamples.Num() >= 12)
						{
							return;
						}
						const FString PinName = PinNames.IsValidIndex(PinIndex) ? PinNames[PinIndex] : FString::Printf(TEXT("pin_%d"), PinIndex);
						MissingUsedPinAnchorSamples.Add(FString::Printf(
							TEXT("%d:%s:%s:%d:%s"),
							NodeIndex,
							*Node.Title,
							Direction == EGPD_Input ? TEXT("input") : TEXT("output"),
							PinIndex,
							*PinName));
					}
				};
				AddMissingUsedPinSamples(EGPD_Input);
				AddMissingUsedPinSamples(EGPD_Output);
			}
			OutStats.bUsedRealGeometry = MissingNodeGeometryCount == 0 && OutStats.MissingPinAnchorCount == 0;
			OutStats.GeometrySource = OutStats.bUsedRealGeometry
				? TEXT("slate_graph_editor_bounds_and_arranged_pin_geometry")
				: TEXT("incomplete_slate_real_pin_geometry");
			if (!OutStats.bUsedRealGeometry)
			{
				OutError = FString::Printf(
					TEXT("real_geometry_incomplete nodes=%d/%d missing_node_geometry=%d pin_widgets=%d pin_geometry=%d missing_used_pin_anchors=%d missing_visible_pin_anchors=%d"),
					OutStats.MeasuredNodeCount,
					OutStats.NodeCount,
					MissingNodeGeometryCount,
					OutStats.PinWidgetCount,
					OutStats.PinGeometryAnchorCount,
					OutStats.MissingPinAnchorCount,
					OutStats.VisibleMissingPinAnchorCount);
				if (OutStats.bFocusedGeometryBudgetExhausted)
				{
					OutError += FString::Printf(TEXT(" focused_capture_budget_exhausted focused_nodes=%d"), OutStats.FocusedGeometryNodeCount);
				}
				if (MissingNodeGeometrySamples.Num() > 0)
				{
					OutError += FString::Printf(TEXT(" missing_node_samples=[%s]"), *FString::Join(MissingNodeGeometrySamples, TEXT(";")));
				}
				if (MissingUsedPinAnchorSamples.Num() > 0)
				{
					OutError += FString::Printf(TEXT(" missing_used_pin_samples=[%s]"), *FString::Join(MissingUsedPinAnchorSamples, TEXT(";")));
				}
				OutStats.Error = OutError;
			}
			else
			{
				OutError.Reset();
			}
			RestoreGraphView();
			return OutStats.bUsedRealGeometry;
		}
		bool TryGetPinImageCenterAnchor(
			SGraphPanel& GraphPanel,
			SGraphNode& GraphNodeWidget,
			const TSharedRef<SGraphPin>& PinWidget,
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

			TSharedRef<SWidget> AnchorWidget = StaticCastSharedRef<SWidget>(PinWidget);
			if (TSharedPtr<SWidget> PinImage = PinWidget->GetPinImageWidget())
			{
				AnchorWidget = PinImage.ToSharedRef();
			}

			TSet<TSharedRef<SWidget>> WidgetsToFind;
			WidgetsToFind.Add(AnchorWidget);

			TMap<TSharedRef<SWidget>, FArrangedWidget> ArrangedWidgets;
			const auto ViewOffset = GraphPanel.GetViewOffset();
			const FVector2f NodePanelOffset(
				GraphNodeWidget.GetPosition2f().X - static_cast<float>(ViewOffset.X),
				GraphNodeWidget.GetPosition2f().Y - static_cast<float>(ViewOffset.Y));
			const FArrangedWidget ArrangedNode = PanelGeometry.MakeChild(
				GraphNodeWidget.AsShared(),
				NodePanelOffset,
				GraphNodeWidget.GetDesiredSize(),
				ZoomAmount);
			FindArrangedDescendantGeometries(GraphNodeWidget, ArrangedNode.Geometry, WidgetsToFind, ArrangedWidgets);
			if (ArrangedWidgets.Num() < WidgetsToFind.Num())
			{
				FindArrangedDescendantGeometries(GraphPanel, PanelGeometry, WidgetsToFind, ArrangedWidgets);
			}

			const FArrangedWidget* AnchorGeometry = ArrangedWidgets.Find(AnchorWidget);
			FGeometry FallbackAnchorGeometry;
			if (!AnchorGeometry && TryGetUsableWidgetGeometry(AnchorWidget.Get(), FallbackAnchorGeometry))
			{
				ArrangedWidgets.Add(AnchorWidget, FArrangedWidget(AnchorWidget, FallbackAnchorGeometry));
				AnchorGeometry = ArrangedWidgets.Find(AnchorWidget);
			}
			if (!AnchorGeometry)
			{
				return false;
			}

			const FVector2f AnchorCenterAbsolute =
				AnchorGeometry->Geometry.AbsolutePosition + AnchorGeometry->Geometry.GetDrawSize() * 0.5f;
			const FVector2f NodeAbsolute = ArrangedNode.Geometry.GetAbsolutePosition();
			const FVector2f LocalGraphOffset = (AnchorCenterAbsolute - NodeAbsolute) / ZoomAmount;
			OutAnchor = FVector2D(static_cast<double>(LocalGraphOffset.X), static_cast<double>(LocalGraphOffset.Y));
			return IsFiniteLayoutPoint(OutAnchor);
		}
	}
}
