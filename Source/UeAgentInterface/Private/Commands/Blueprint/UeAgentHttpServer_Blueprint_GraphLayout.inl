namespace UeAgentBlueprintOps
{
	struct FBlueprintLayoutRect
	{
		double Left = 0.0;
		double Top = 0.0;
		double Right = 0.0;
		double Bottom = 0.0;

		bool Intersects(const FBlueprintLayoutRect& Other, const double Padding = 0.0) const
		{
			constexpr double Epsilon = 0.5;
			return !(Right + Padding <= Other.Left + Epsilon
				|| Other.Right + Padding <= Left + Epsilon
				|| Bottom + Padding <= Other.Top + Epsilon
				|| Other.Bottom + Padding <= Top + Epsilon);
		}
	};

	struct FBlueprintLayoutNode
	{
		UEdGraphNode* Node = nullptr;
		FString Guid;
		FString Title;
		int32 Index = INDEX_NONE;
		int32 Component = INDEX_NONE;
		int32 Rank = 0;
		int32 Order = 0;
		int32 InputPinCount = 0;
		int32 OutputPinCount = 0;
		double Width = 260.0;
		double Height = 110.0;
		double OldX = 0.0;
		double OldY = 0.0;
		double NewX = 0.0;
		double NewY = 0.0;
		bool bRerouteNode = false;
		bool bHasMeasuredGeometry = false;
		TArray<FVector2D> InputPinAnchors;
		TArray<FVector2D> OutputPinAnchors;
		TArray<uint8> InputPinAnchorValid;
		TArray<uint8> OutputPinAnchorValid;
	};

	struct FBlueprintLayoutEdge
	{
		int32 Source = INDEX_NONE;
		int32 Target = INDEX_NONE;
		FString SourcePin;
		FString TargetPin;
		int32 SourcePinIndex = INDEX_NONE;
		int32 TargetPinIndex = INDEX_NONE;
		bool bExec = false;
	};

	enum class EBlueprintLayoutRouteKind : uint8
	{
		Direct,
		LocalBus,
		ExternalBus,
		SharedFanoutBus
	};

	struct FBlueprintLayoutReroutePlan
	{
		int32 RawEdgeIndex = INDEX_NONE;
		FVector2D FirstPoint = FVector2D::ZeroVector;
		FVector2D SecondPoint = FVector2D::ZeroVector;
		TArray<FVector2D> ReroutePoints;
		FString Reason;
		FString FanoutGroupId;
		FString BusId;
		EBlueprintLayoutRouteKind RouteKind = EBlueprintLayoutRouteKind::LocalBus;
		int32 BusLaneIndex = 0;
		FString BusLaneSide;
	};

	struct FBlueprintLayoutGeometryStats
	{
		bool bUsedRealGeometry = false;
		bool bWindowPrepared = false;
		FString GeometrySource = TEXT("estimated_fallback");
		FString Error;
		int32 NodeCount = 0;
		int32 MeasuredNodeCount = 0;
		int32 PinWidgetCount = 0;
		int32 PinGeometryAnchorCount = 0;
		int32 PinOffsetAnchorCount = 0;
		int32 MissingPinAnchorCount = 0;
	};

	struct FBlueprintLayoutAlignmentStats
	{
		int32 PinAlignedCompactionMoveCount = 0;
		int32 FinalSingleConsumerPinAlignmentCandidateCount = 0;
		int32 FinalSingleConsumerPinAlignmentMoveCount = 0;
		int32 DirectLeafExecInputDockingCandidateCount = 0;
		int32 DirectLeafExecInputDockingMoveCount = 0;
		int32 ExecInputExpressionIslandCandidateCount = 0;
		int32 ExecInputExpressionIslandMoveCount = 0;
		int32 SharedExecInputFanoutDockingCandidateCount = 0;
		int32 SharedExecInputFanoutDockingMoveCount = 0;
	};

	struct FBlueprintLayoutMetrics
	{
		int32 OverlapCount = 0;
		int32 EdgeCrossingCount = 0;
		int32 EdgeNodeIntersectionCount = 0;
		int32 BackwardEdgeCount = 0;
		int32 BackwardDataEdgeCount = 0;
		int32 BackwardExecEdgeCount = 0;
		int32 PinOrderViolationCount = 0;
		double TotalWireLength = 0.0;
		double TotalNonExecWireLength = 0.0;
		double BackwardWireLength = 0.0;
		double BackwardDataWireLength = 0.0;
		double BackwardExecWireLength = 0.0;
		double PinOrderViolationDistance = 0.0;
		double LayoutWidth = 0.0;
		double LayoutHeight = 0.0;
		double LayoutArea = 0.0;
		double LayoutAspectRatio = 1.0;
		double LayoutAspectRatioPenalty = 0.0;
	};

	struct FBlueprintLayoutLayerNode
	{
		int32 NodeIndex = INDEX_NONE;
		double Key = 0.0;
		TArray<int32> Signature;
	};

	struct FBlueprintLayoutVerticalInterval
	{
		double Top = 0.0;
		double Bottom = 0.0;
	};

	static FString LayoutRouteKindToString(const EBlueprintLayoutRouteKind RouteKind)
	{
		switch (RouteKind)
		{
		case EBlueprintLayoutRouteKind::Direct:
			return TEXT("direct");
		case EBlueprintLayoutRouteKind::LocalBus:
			return TEXT("local_bus");
		case EBlueprintLayoutRouteKind::ExternalBus:
			return TEXT("external_bus");
		case EBlueprintLayoutRouteKind::SharedFanoutBus:
			return TEXT("local_bus");
		default:
			return TEXT("local_bus");
		}
	}

	static EBlueprintLayoutRouteKind InferLayoutRouteKindFromReason(const FString& Reason)
	{
		if (Reason.Contains(TEXT("_external_"), ESearchCase::IgnoreCase)
			|| Reason.Contains(TEXT("external_"), ESearchCase::IgnoreCase))
		{
			return EBlueprintLayoutRouteKind::ExternalBus;
		}
		return EBlueprintLayoutRouteKind::LocalBus;
	}

	static bool IsRerouteBlueprintNode(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return false;
		}
		const FString ClassName = Node->GetClass() ? Node->GetClass()->GetName() : FString();
		return ClassName.Contains(TEXT("K2Node_Knot")) || ClassName.Contains(TEXT("EdGraphNode_CommentReroute"));
	}

	static bool IsCompactBlueprintValueNode(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return false;
		}
		const FString ClassName = Node->GetClass() ? Node->GetClass()->GetName() : FString();
		return ClassName.Contains(TEXT("K2Node_VariableGet"))
			|| ClassName.Contains(TEXT("K2Node_Self"))
			|| ClassName.Contains(TEXT("K2Node_Literal"))
			|| ClassName.Contains(TEXT("K2Node_EnumLiteral"))
			|| ClassName.Contains(TEXT("K2Node_TemporaryVariable"));
	}

	static bool IsExecPin(const UEdGraphPin* Pin)
	{
		return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	}

	static bool IsVisibleLayoutPin(const UEdGraphPin* Pin)
	{
		if (!Pin || Pin->bHidden)
		{
			return false;
		}
		const UEdGraphNode* Node = Pin->GetOwningNodeUnchecked();
		if (Pin->bAdvancedView
			&& Node
			&& Node->AdvancedPinDisplay == ENodeAdvancedPins::Hidden
			&& Pin->LinkedTo.Num() == 0)
		{
			return false;
		}
		return true;
	}

	static int32 CountVisiblePinsInDirection(const UEdGraphNode* Node, const EEdGraphPinDirection Direction)
	{
		int32 Count = 0;
		if (!Node)
		{
			return Count;
		}
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction && IsVisibleLayoutPin(Pin))
			{
				++Count;
			}
		}
		return Count;
	}

	static bool NodeHasAnyExecPin(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return false;
		}
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (IsExecPin(Pin))
			{
				return true;
			}
		}
		return false;
	}

	static bool IsSmallPureBlueprintFunctionNode(const UEdGraphNode* Node, const int32 InputPins, const int32 OutputPins)
	{
		if (!Node)
		{
			return false;
		}
		const FString ClassName = Node->GetClass() ? Node->GetClass()->GetName() : FString();
		const int32 PinRows = FMath::Max(InputPins, OutputPins);
		return ClassName.Contains(TEXT("K2Node_CallFunction"))
			&& !NodeHasAnyExecPin(Node)
			&& PinRows <= 2
			&& InputPins + OutputPins <= 3;
	}

	static bool CompareStableNodeIdentity(const TArray<FBlueprintLayoutNode>& Nodes, const int32 A, const int32 B)
	{
		if (!Nodes.IsValidIndex(A) || !Nodes.IsValidIndex(B))
		{
			return A < B;
		}
		if (!FMath::IsNearlyEqual(Nodes[A].OldY, Nodes[B].OldY, 1.0))
		{
			return Nodes[A].OldY < Nodes[B].OldY;
		}
		if (!FMath::IsNearlyEqual(Nodes[A].OldX, Nodes[B].OldX, 1.0))
		{
			return Nodes[A].OldX < Nodes[B].OldX;
		}
		const int32 TitleCompare = Nodes[A].Title.Compare(Nodes[B].Title, ESearchCase::IgnoreCase);
		if (TitleCompare != 0)
		{
			return TitleCompare < 0;
		}
		return Nodes[A].Guid < Nodes[B].Guid;
	}

	static double EstimateNodeWidth(UEdGraphNode* Node, const double DefaultWidth)
	{
		if (!Node)
		{
			return DefaultWidth;
		}
		if (IsRerouteBlueprintNode(Node))
		{
			return 24.0;
		}
		const FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		const int32 InputPins = CountVisiblePinsInDirection(Node, EGPD_Input);
		const int32 OutputPins = CountVisiblePinsInDirection(Node, EGPD_Output);
		if (IsCompactBlueprintValueNode(Node))
		{
			const double NaturalWidth = 112.0 + static_cast<double>(Title.Len()) * 6.0;
			double Width = FMath::Max(128.0, NaturalWidth);
			if (Node->NodeWidth > 20)
			{
				Width = FMath::Min(static_cast<double>(Node->NodeWidth), Width);
			}
			return FMath::Clamp(Width, 120.0, 280.0);
		}
		if (IsSmallPureBlueprintFunctionNode(Node, InputPins, OutputPins))
		{
			const double NaturalWidth = 132.0 + static_cast<double>(Title.Len()) * 5.5;
			double Width = FMath::Max(156.0, NaturalWidth);
			if (Node->NodeWidth > 20)
			{
				Width = FMath::Min(static_cast<double>(Node->NodeWidth), Width);
			}
			return FMath::Clamp(Width, 140.0, 260.0);
		}
		double Width = Node->NodeWidth > 20 ? static_cast<double>(Node->NodeWidth) : DefaultWidth;
		Width = FMath::Max(Width, 150.0 + static_cast<double>(Title.Len()) * 7.0);
		return FMath::Clamp(Width, 140.0, 900.0);
	}

	static double EstimateNodeHeight(UEdGraphNode* Node, const double DefaultHeight)
	{
		if (!Node)
		{
			return DefaultHeight;
		}
		if (IsRerouteBlueprintNode(Node))
		{
			return 24.0;
		}
		const int32 InputPins = CountVisiblePinsInDirection(Node, EGPD_Input);
		const int32 OutputPins = CountVisiblePinsInDirection(Node, EGPD_Output);
		const int32 PinRows = FMath::Max(InputPins, OutputPins);
		if (IsCompactBlueprintValueNode(Node))
		{
			double Height = 34.0 + static_cast<double>(FMath::Max(0, PinRows - 1)) * 18.0;
			if (Node->NodeHeight > 20)
			{
				Height = FMath::Min(static_cast<double>(Node->NodeHeight), Height);
			}
			return FMath::Clamp(Height, 34.0, 72.0);
		}
		if (IsSmallPureBlueprintFunctionNode(Node, InputPins, OutputPins))
		{
			double Height = 56.0 + static_cast<double>(FMath::Max(0, PinRows - 1)) * 22.0;
			if (Node->NodeHeight > 20)
			{
				Height = FMath::Min(static_cast<double>(Node->NodeHeight), Height);
			}
			return FMath::Clamp(Height, 52.0, 96.0);
		}
		if (Node->NodeHeight > 20)
		{
			return FMath::Clamp(static_cast<double>(Node->NodeHeight), 70.0, 1200.0);
		}
		double Height = FMath::Max(DefaultHeight, 68.0 + static_cast<double>(PinRows) * 24.0);
		if (!Node->NodeComment.IsEmpty())
		{
			Height += 24.0;
		}
		return FMath::Clamp(Height, 70.0, 1200.0);
	}

	static int32 CountPinsInDirection(const UEdGraphNode* Node, const EEdGraphPinDirection Direction)
	{
		return CountVisiblePinsInDirection(Node, Direction);
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
		int32 VisualIndex = 0;
		for (const UEdGraphPin* CandidatePin : Node->Pins)
		{
			if (!CandidatePin || CandidatePin->Direction != Pin->Direction || !IsVisibleLayoutPin(CandidatePin))
			{
				continue;
			}
			if (CandidatePin == Pin)
			{
				return VisualIndex;
			}
			++VisualIndex;
		}
		return INDEX_NONE;
	}

	static void ResetMeasuredPinAnchors(FBlueprintLayoutNode& Node)
	{
		Node.InputPinAnchors.SetNum(Node.InputPinCount);
		Node.OutputPinAnchors.SetNum(Node.OutputPinCount);
		Node.InputPinAnchorValid.SetNumZeroed(Node.InputPinCount);
		Node.OutputPinAnchorValid.SetNumZeroed(Node.OutputPinCount);
	}

	static bool IsFiniteLayoutPoint(const FVector2D& Point)
	{
		return FMath::IsFinite(Point.X) && FMath::IsFinite(Point.Y);
	}

	static void SetMeasuredPinAnchor(
		FBlueprintLayoutNode& Node,
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
		const FBlueprintLayoutNode& Node,
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

	static int32 CountMeasuredPinAnchors(const FBlueprintLayoutNode& Node, const EEdGraphPinDirection Direction)
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

	static bool IsUsableSlateGeometry(const FGeometry& Geometry)
	{
		const FVector2f Size = Geometry.GetLocalSize();
		const FVector2f AbsolutePosition = Geometry.GetAbsolutePosition();
		return Size.X > 0.5f
			&& Size.Y > 0.5f
			&& FMath::IsFinite(AbsolutePosition.X)
			&& FMath::IsFinite(AbsolutePosition.Y);
	}

	static bool TryGetTickSpacePinAnchor(
		SGraphPanel& GraphPanel,
		const SGraphNode& GraphNodeWidget,
		const TSharedRef<SGraphPin>& PinWidget,
		const FVector2D& NodeGraphPosition,
		const EEdGraphPinDirection Direction,
		FVector2D& OutAnchor)
	{
		FVector2f PinCenterAbsolute = FVector2f::ZeroVector;
		bool bHasPinCenter = false;

		if (const TSharedPtr<SWidget> PinImageWidget = PinWidget->GetPinImageWidget())
		{
			const FGeometry& PinImageGeometry = PinImageWidget->GetTickSpaceGeometry();
			if (IsUsableSlateGeometry(PinImageGeometry))
			{
				PinCenterAbsolute = PinImageGeometry.GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f));
				bHasPinCenter = true;
			}
		}

		if (!bHasPinCenter)
		{
			const FGeometry& PinGeometry = PinWidget->GetTickSpaceGeometry();
			if (!IsUsableSlateGeometry(PinGeometry))
			{
				return false;
			}
			PinCenterAbsolute = Direction == EGPD_Input
				? UE::Slate::CastToVector2f(FGeometryHelper::VerticalMiddleLeftOf(PinGeometry))
				: UE::Slate::CastToVector2f(FGeometryHelper::VerticalMiddleRightOf(PinGeometry));
		}

		const FGeometry& NodeGeometry = GraphNodeWidget.GetTickSpaceGeometry();
		if (IsUsableSlateGeometry(NodeGeometry))
		{
			const float ZoomAmount = FMath::Max(GraphPanel.GetZoomAmount(), 0.01f);
			const FVector2f NodeAbsolute = NodeGeometry.GetAbsolutePosition();
			const FVector2f LocalGraphOffset = (PinCenterAbsolute - NodeAbsolute) / ZoomAmount;
			OutAnchor = FVector2D(
				static_cast<double>(LocalGraphOffset.X),
				static_cast<double>(LocalGraphOffset.Y));
			if (IsFiniteLayoutPoint(OutAnchor))
			{
				return true;
			}
		}

		const FVector2f PinCenterPanel = GraphPanel.GetTickSpaceGeometry().AbsoluteToLocal(PinCenterAbsolute);
		const FVector2f PinCenterGraph = UE::Slate::CastToVector2f(GraphPanel.PanelCoordToGraphCoord(PinCenterPanel));
		OutAnchor = FVector2D(
			static_cast<double>(PinCenterGraph.X) - NodeGraphPosition.X,
			static_cast<double>(PinCenterGraph.Y) - NodeGraphPosition.Y);
		return IsFiniteLayoutPoint(OutAnchor);
	}

	static bool CaptureBlueprintLayoutGeometry(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		TArray<FBlueprintLayoutNode>& LayoutNodes,
		FBlueprintLayoutGeometryStats& OutStats,
		FString& OutError)
	{
		OutStats = FBlueprintLayoutGeometryStats();
		OutStats.NodeCount = LayoutNodes.Num();

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

		TSharedPtr<IBlueprintEditor> BlueprintEditor = GetBlueprintEditor(Blueprint, true, OutError);
		if (!BlueprintEditor.IsValid())
		{
			OutStats.Error = OutError;
			return false;
		}

		TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(Graph, true);
		if (!GraphEditor.IsValid())
		{
			OutError = TEXT("graph_editor_not_found_for_real_geometry");
			OutStats.Error = OutError;
			return false;
		}

		SGraphPanel* GraphPanel = GraphEditor->GetGraphPanel();
		if (!GraphPanel)
		{
			OutError = TEXT("graph_panel_not_found_for_real_geometry");
			OutStats.Error = OutError;
			return false;
		}

		FString WindowError;
		const TSharedPtr<SWindow> BlueprintWindow = ResolveBlueprintEditorWindow(BlueprintEditor, WindowError);
		if (BlueprintWindow.IsValid())
		{
			if (!PrepareSlateWindowForCapture(BlueprintWindow, OutError))
			{
				OutStats.Error = OutError;
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

		auto RefreshGraphPanelGeometry = [&]()
		{
			for (int32 FrameIndex = 0; FrameIndex < 2; ++FrameIndex)
			{
				GraphPanel->Update();
				PumpAndTickSlateForCapture();
				const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
				GraphEditor->SlatePrepass(WidgetScale);
				GraphPanel->SlatePrepass(WidgetScale);
				if (BlueprintWindow.IsValid())
				{
					FSlateApplication::Get().ForceRedrawWindow(BlueprintWindow.ToSharedRef());
					PumpAndTickSlateForCapture();
				}
			}
		};
		RefreshGraphPanelGeometry();

		TSet<FGuid> WarmedNodePinOffsets;
		auto WarmGraphNodePinOffsets = [&WarmedNodePinOffsets](const TSharedRef<SGraphNode>& GraphNodeWidget)
		{
			UEdGraphNode* Node = GraphNodeWidget->GetNodeObj();
			if (!Node || WarmedNodePinOffsets.Contains(Node->NodeGuid))
			{
				return;
			}
			TArray<FColor> WarmPixels;
			FIntPoint WarmSize(0, 0);
			FString WarmError;
			(void)ScreenshotSlateWidget(StaticCastSharedRef<SWidget>(GraphNodeWidget), WarmPixels, WarmSize, WarmError);
			WarmedNodePinOffsets.Add(Node->NodeGuid);
		};
		for (const FBlueprintLayoutNode& LayoutNode : LayoutNodes)
		{
			if (LayoutNode.Node)
			{
				GraphPanel->RefreshNode(*LayoutNode.Node);
			}
		}
		RefreshGraphPanelGeometry();

		auto CapturePinAnchorsForNode = [&](FBlueprintLayoutNode& LayoutNode, const bool bCountPinWidgets)
		{
			if (!LayoutNode.Node)
			{
				return;
			}

			TSharedPtr<SGraphNode> GraphNodeWidget = GraphPanel->GetNodeWidgetFromGuid(LayoutNode.Node->NodeGuid);
			if (!GraphNodeWidget.IsValid())
			{
				return;
			}
			WarmGraphNodePinOffsets(GraphNodeWidget.ToSharedRef());

			for (UEdGraphPin* Pin : LayoutNode.Node->Pins)
			{
				if (!Pin || !IsVisibleLayoutPin(Pin) || (Pin->Direction != EGPD_Input && Pin->Direction != EGPD_Output))
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
				const FVector2D WidgetNodeGraphPosition(
					static_cast<double>(WidgetNodeGraphPosition2f.X),
					static_cast<double>(WidgetNodeGraphPosition2f.Y));
				if (TryGetTickSpacePinAnchor(
					*GraphPanel,
					*GraphNodeWidget,
					PinWidget.ToSharedRef(),
					WidgetNodeGraphPosition,
					Pin->Direction,
					Anchor))
				{
					SetMeasuredPinAnchor(LayoutNode, Pin->Direction, PinIndex, Anchor);
					++OutStats.PinGeometryAnchorCount;
					continue;
				}

				const auto NodeOffset = PinWidget->GetNodeOffset();
				const FVector2D CachedAnchor(
					static_cast<double>(NodeOffset.X),
					static_cast<double>(NodeOffset.Y));
				if (IsFiniteLayoutPoint(CachedAnchor) && !CachedAnchor.IsNearlyZero())
				{
					SetMeasuredPinAnchor(LayoutNode, Pin->Direction, PinIndex, CachedAnchor);
					++OutStats.PinOffsetAnchorCount;
				}
			}
		};

		for (FBlueprintLayoutNode& LayoutNode : LayoutNodes)
		{
			ResetMeasuredPinAnchors(LayoutNode);
			if (!LayoutNode.Node)
			{
				continue;
			}

			FSlateRect Bounds;
			bool bMeasuredNode = false;
			if (GraphEditor->GetBoundsForNode(LayoutNode.Node, Bounds, 0.0f))
			{
				const double Width = static_cast<double>(Bounds.Right - Bounds.Left);
				const double Height = static_cast<double>(Bounds.Bottom - Bounds.Top);
				if (Width > 1.0 && Height > 1.0)
				{
					LayoutNode.Width = FMath::Clamp(Width, 1.0, 5000.0);
					LayoutNode.Height = FMath::Clamp(Height, 1.0, 5000.0);
					bMeasuredNode = true;
				}
			}

			TSharedPtr<SGraphNode> GraphNodeWidget = GraphPanel->GetNodeWidgetFromGuid(LayoutNode.Node->NodeGuid);
			if (!bMeasuredNode && GraphNodeWidget.IsValid())
			{
				const FVector2D DesiredSize = GraphNodeWidget->GetDesiredSize();
				if (DesiredSize.X > 1.0 && DesiredSize.Y > 1.0)
				{
					LayoutNode.Width = FMath::Clamp(DesiredSize.X, 1.0, 5000.0);
					LayoutNode.Height = FMath::Clamp(DesiredSize.Y, 1.0, 5000.0);
					bMeasuredNode = true;
				}
			}
			LayoutNode.bHasMeasuredGeometry = bMeasuredNode;
			if (bMeasuredNode)
			{
				++OutStats.MeasuredNodeCount;
			}

			CapturePinAnchorsForNode(LayoutNode, true);
		}

		if (BlueprintWindow.IsValid())
		{
			for (FBlueprintLayoutNode& LayoutNode : LayoutNodes)
			{
				const int32 MissingForNode =
					(LayoutNode.InputPinCount - CountMeasuredPinAnchors(LayoutNode, EGPD_Input))
					+ (LayoutNode.OutputPinCount - CountMeasuredPinAnchors(LayoutNode, EGPD_Output));
				if (MissingForNode <= 0)
				{
					continue;
				}

				const FVector2f FocusedViewLocation(
					FMath::RoundToFloat(static_cast<float>(LayoutNode.OldX - 240.0)),
					FMath::RoundToFloat(static_cast<float>(LayoutNode.OldY - 160.0)));
				GraphEditor->SetViewLocation(FocusedViewLocation, 1.0f);
				RefreshGraphPanelGeometry();
				CapturePinAnchorsForNode(LayoutNode, false);
			}
		}

		OutStats.MissingPinAnchorCount = 0;
		for (const FBlueprintLayoutNode& LayoutNode : LayoutNodes)
		{
			OutStats.MissingPinAnchorCount += LayoutNode.InputPinCount - CountMeasuredPinAnchors(LayoutNode, EGPD_Input);
			OutStats.MissingPinAnchorCount += LayoutNode.OutputPinCount - CountMeasuredPinAnchors(LayoutNode, EGPD_Output);
		}

		const bool bComplete = OutStats.MeasuredNodeCount == OutStats.NodeCount
			&& OutStats.MissingPinAnchorCount == 0;
		OutStats.bUsedRealGeometry = bComplete;
		OutStats.GeometrySource = bComplete ? TEXT("slate_graph_editor_bounds_and_arranged_pin_geometry") : TEXT("partial_slate_graph_editor");
		if (!bComplete)
		{
			OutError = FString::Printf(
				TEXT("real_geometry_incomplete nodes=%d/%d pin_widgets=%d pin_geometry=%d pin_offsets=%d missing_pin_anchors=%d"),
				OutStats.MeasuredNodeCount,
				OutStats.NodeCount,
				OutStats.PinWidgetCount,
				OutStats.PinGeometryAnchorCount,
				OutStats.PinOffsetAnchorCount,
				OutStats.MissingPinAnchorCount);
			OutStats.Error = OutError;
			return false;
		}

		OutError.Reset();
		return true;
	}

	static bool NodeHasExecPin(const FBlueprintLayoutNode& Node)
	{
		if (!Node.Node)
		{
			return false;
		}
		for (const UEdGraphPin* Pin : Node.Node->Pins)
		{
			if (IsExecPin(Pin))
			{
				return true;
			}
		}
		return false;
	}

	static FBlueprintLayoutRect MakeNodeRect(const FBlueprintLayoutNode& Node, const bool bUseNewPosition)
	{
		const double X = bUseNewPosition ? Node.NewX : Node.OldX;
		const double Y = bUseNewPosition ? Node.NewY : Node.OldY;
		FBlueprintLayoutRect Rect;
		Rect.Left = X;
		Rect.Top = Y;
		Rect.Right = X + Node.Width;
		Rect.Bottom = Y + Node.Height;
		return Rect;
	}

	static double GetPinYOffset(const FBlueprintLayoutNode& Node, const EEdGraphPinDirection Direction, const int32 PinIndex)
	{
		FVector2D MeasuredAnchor = FVector2D::ZeroVector;
		if (TryGetMeasuredPinAnchor(Node, Direction, PinIndex, MeasuredAnchor))
		{
			return MeasuredAnchor.Y;
		}
		const int32 Count = FMath::Max(1, Direction == EGPD_Input ? Node.InputPinCount : Node.OutputPinCount);
		if (Node.bRerouteNode)
		{
			return Node.Height * 0.5;
		}
		if (IsCompactBlueprintValueNode(Node.Node))
		{
			const double TopPadding = FMath::Clamp(Node.Height * 0.18, 8.0, 16.0);
			const double BottomPadding = FMath::Clamp(Node.Height * 0.12, 6.0, 14.0);
			const double Span = FMath::Max(1.0, Node.Height - TopPadding - BottomPadding);
			if (Count <= 1 || PinIndex == INDEX_NONE)
			{
				return Node.Height * 0.5;
			}
			const double Fraction = static_cast<double>(FMath::Clamp(PinIndex, 0, Count - 1)) / static_cast<double>(Count - 1);
			return TopPadding + Span * Fraction;
		}
		if (IsSmallPureBlueprintFunctionNode(Node.Node, Node.InputPinCount, Node.OutputPinCount))
		{
			if (Count <= 1 || PinIndex == INDEX_NONE)
			{
				return Node.Height * 0.5;
			}
			const double TopPadding = FMath::Clamp(Node.Height * 0.32, 18.0, 28.0);
			const double BottomPadding = FMath::Clamp(Node.Height * 0.18, 10.0, 20.0);
			const double Span = FMath::Max(1.0, Node.Height - TopPadding - BottomPadding);
			const double Fraction = static_cast<double>(FMath::Clamp(PinIndex, 0, Count - 1)) / static_cast<double>(Count - 1);
			return TopPadding + Span * Fraction;
		}
		const double TopPadding = FMath::Clamp(Node.Height * 0.22, 24.0, 42.0);
		const double BottomPadding = FMath::Clamp(Node.Height * 0.16, 18.0, 34.0);
		const double Span = FMath::Max(1.0, Node.Height - TopPadding - BottomPadding);
		if (Count <= 1 || PinIndex == INDEX_NONE)
		{
			return TopPadding + Span * 0.5;
		}
		const double Fraction = static_cast<double>(FMath::Clamp(PinIndex, 0, Count - 1)) / static_cast<double>(Count - 1);
		return TopPadding + Span * Fraction;
	}

	static double GetSourcePinFraction(const FBlueprintLayoutNode& Node, const FBlueprintLayoutEdge& Edge)
	{
		const int32 Count = FMath::Max(1, Node.OutputPinCount);
		return (static_cast<double>(FMath::Max(0, Edge.SourcePinIndex)) + 1.0) / (static_cast<double>(Count) + 1.0);
	}

	static double GetTargetPinFraction(const FBlueprintLayoutNode& Node, const FBlueprintLayoutEdge& Edge)
	{
		const int32 Count = FMath::Max(1, Node.InputPinCount);
		return (static_cast<double>(FMath::Max(0, Edge.TargetPinIndex)) + 1.0) / (static_cast<double>(Count) + 1.0);
	}

	static FVector2D GetEdgeSourcePoint(const TArray<FBlueprintLayoutNode>& Nodes, const FBlueprintLayoutEdge& Edge, const bool bUseNewPosition)
	{
		if (!Nodes.IsValidIndex(Edge.Source))
		{
			return FVector2D::ZeroVector;
		}
		const FBlueprintLayoutNode& Node = Nodes[Edge.Source];
		const FBlueprintLayoutRect Rect = MakeNodeRect(Node, bUseNewPosition);
		FVector2D MeasuredAnchor = FVector2D::ZeroVector;
		if (TryGetMeasuredPinAnchor(Node, EGPD_Output, Edge.SourcePinIndex, MeasuredAnchor))
		{
			const double Y = bUseNewPosition ? Node.NewY : Node.OldY;
			// Pin widget offsets give reliable row alignment, but output X can sit inside the node.
			return FVector2D(Rect.Right, Y + MeasuredAnchor.Y);
		}
		return FVector2D(Rect.Right, Rect.Top + GetPinYOffset(Node, EGPD_Output, Edge.SourcePinIndex));
	}

	static FVector2D GetEdgeTargetPoint(const TArray<FBlueprintLayoutNode>& Nodes, const FBlueprintLayoutEdge& Edge, const bool bUseNewPosition)
	{
		if (!Nodes.IsValidIndex(Edge.Target))
		{
			return FVector2D::ZeroVector;
		}
		const FBlueprintLayoutNode& Node = Nodes[Edge.Target];
		const FBlueprintLayoutRect Rect = MakeNodeRect(Node, bUseNewPosition);
		FVector2D MeasuredAnchor = FVector2D::ZeroVector;
		if (TryGetMeasuredPinAnchor(Node, EGPD_Input, Edge.TargetPinIndex, MeasuredAnchor))
		{
			const double Y = bUseNewPosition ? Node.NewY : Node.OldY;
			// Keep the measured pin row while anchoring inputs to the left node boundary.
			return FVector2D(Rect.Left, Y + MeasuredAnchor.Y);
		}
		return FVector2D(Rect.Left, Rect.Top + GetPinYOffset(Node, EGPD_Input, Edge.TargetPinIndex));
	}

	static void BuildIncidentEdgeLists(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		TArray<TArray<int32>>& OutIncomingEdges,
		TArray<TArray<int32>>& OutOutgoingEdges)
	{
		OutIncomingEdges.Empty();
		OutOutgoingEdges.Empty();
		OutIncomingEdges.SetNum(Nodes.Num());
		OutOutgoingEdges.SetNum(Nodes.Num());
		for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
		{
			const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
			if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
			{
				continue;
			}
			OutOutgoingEdges[Edge.Source].Add(EdgeIndex);
			OutIncomingEdges[Edge.Target].Add(EdgeIndex);
		}
	}

	static FString MakeLayoutEdgeKey(const FBlueprintLayoutEdge& Edge)
	{
		return FString::Printf(
			TEXT("%d:%s:%d>%d:%s:%d:%d"),
			Edge.Source,
			*Edge.SourcePin,
			Edge.SourcePinIndex,
			Edge.Target,
			*Edge.TargetPin,
			Edge.TargetPinIndex,
			Edge.bExec ? 1 : 0);
	}

	static void AddUniqueLayoutEdge(TArray<FBlueprintLayoutEdge>& OutEdges, TSet<FString>& EdgeKeys, const FBlueprintLayoutEdge& Edge)
	{
		const FString Key = MakeLayoutEdgeKey(Edge);
		if (EdgeKeys.Contains(Key))
		{
			return;
		}
		EdgeKeys.Add(Key);
		OutEdges.Add(Edge);
	}

	static TArray<FBlueprintLayoutEdge> BuildRerouteCollapsedEdges(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& RawEdges,
		int32& OutRerouteNodeCount)
	{
		OutRerouteNodeCount = 0;
		for (const FBlueprintLayoutNode& Node : Nodes)
		{
			if (Node.bRerouteNode)
			{
				++OutRerouteNodeCount;
			}
		}
		if (OutRerouteNodeCount == 0)
		{
			return RawEdges;
		}

		TArray<TArray<int32>> IncomingEdges;
		TArray<TArray<int32>> OutgoingEdges;
		BuildIncidentEdgeLists(Nodes, RawEdges, IncomingEdges, OutgoingEdges);

		TArray<FBlueprintLayoutEdge> CollapsedEdges;
		TSet<FString> CollapsedKeys;
		TFunction<void(const FBlueprintLayoutEdge&, int32, bool, TSet<int32>&)> FollowReroute;
		FollowReroute = [&](
			const FBlueprintLayoutEdge& SeedEdge,
			const int32 RerouteIndex,
			const bool bPathExec,
			TSet<int32>& VisitedReroutes)
		{
			if (!Nodes.IsValidIndex(RerouteIndex)
				|| !Nodes[RerouteIndex].bRerouteNode
				|| !OutgoingEdges.IsValidIndex(RerouteIndex)
				|| VisitedReroutes.Contains(RerouteIndex))
			{
				return;
			}
			VisitedReroutes.Add(RerouteIndex);
			for (const int32 OutEdgeIndex : OutgoingEdges[RerouteIndex])
			{
				if (!RawEdges.IsValidIndex(OutEdgeIndex))
				{
					continue;
				}
				const FBlueprintLayoutEdge& OutEdge = RawEdges[OutEdgeIndex];
				if (!Nodes.IsValidIndex(OutEdge.Target))
				{
					continue;
				}
				if (Nodes[OutEdge.Target].bRerouteNode)
				{
					FollowReroute(SeedEdge, OutEdge.Target, bPathExec || OutEdge.bExec, VisitedReroutes);
					continue;
				}

				FBlueprintLayoutEdge CollapsedEdge;
				CollapsedEdge.Source = SeedEdge.Source;
				CollapsedEdge.Target = OutEdge.Target;
				CollapsedEdge.SourcePin = SeedEdge.SourcePin;
				CollapsedEdge.TargetPin = OutEdge.TargetPin;
				CollapsedEdge.SourcePinIndex = SeedEdge.SourcePinIndex;
				CollapsedEdge.TargetPinIndex = OutEdge.TargetPinIndex;
				CollapsedEdge.bExec = bPathExec || OutEdge.bExec;
				AddUniqueLayoutEdge(CollapsedEdges, CollapsedKeys, CollapsedEdge);
			}
			VisitedReroutes.Remove(RerouteIndex);
		};

		for (const FBlueprintLayoutEdge& RawEdge : RawEdges)
		{
			if (!Nodes.IsValidIndex(RawEdge.Source)
				|| !Nodes.IsValidIndex(RawEdge.Target)
				|| Nodes[RawEdge.Source].bRerouteNode)
			{
				continue;
			}
			if (!Nodes[RawEdge.Target].bRerouteNode)
			{
				AddUniqueLayoutEdge(CollapsedEdges, CollapsedKeys, RawEdge);
				continue;
			}
			TSet<int32> VisitedReroutes;
			FollowReroute(RawEdge, RawEdge.Target, RawEdge.bExec, VisitedReroutes);
		}
		return CollapsedEdges;
	}

	static int32 RemoveRerouteNodesAndReconnect(UEdGraph* Graph)
	{
		if (!Graph || !Graph->GetSchema())
		{
			return 0;
		}
		const UEdGraphSchema* Schema = Graph->GetSchema();
		TArray<UEdGraphNode*> RerouteNodes;
		TSet<UEdGraphNode*> RerouteNodeSet;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsRerouteBlueprintNode(Node))
			{
				RerouteNodes.Add(Node);
				RerouteNodeSet.Add(Node);
			}
		}
		if (RerouteNodes.Num() == 0)
		{
			return 0;
		}

		TFunction<void(UEdGraphPin*, TSet<UEdGraphNode*>&, TArray<UEdGraphPin*>&)> CollectTerminalTargets;
		CollectTerminalTargets = [&](UEdGraphPin* OutputPin, TSet<UEdGraphNode*>& VisitedReroutes, TArray<UEdGraphPin*>& OutTargets)
		{
			if (!OutputPin)
			{
				return;
			}
			for (UEdGraphPin* LinkedPin : OutputPin->LinkedTo)
			{
				if (!LinkedPin)
				{
					continue;
				}
				UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
				if (!LinkedNode)
				{
					continue;
				}
				if (RerouteNodeSet.Contains(LinkedNode))
				{
					if (VisitedReroutes.Contains(LinkedNode))
					{
						continue;
					}
					VisitedReroutes.Add(LinkedNode);
					if (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(LinkedNode))
					{
						CollectTerminalTargets(Knot->GetOutputPin(), VisitedReroutes, OutTargets);
					}
					continue;
				}
				if (LinkedPin->Direction == EGPD_Input)
				{
					OutTargets.AddUnique(LinkedPin);
				}
			}
		};

		for (UEdGraphNode* Node : RerouteNodes)
		{
			UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node);
			if (!Knot || !Knot->GetInputPin() || !Knot->GetOutputPin())
			{
				continue;
			}
			TArray<UEdGraphPin*> SourcePins = Knot->GetInputPin()->LinkedTo;
			for (UEdGraphPin* SourcePin : SourcePins)
			{
				if (!SourcePin || SourcePin->Direction != EGPD_Output)
				{
					continue;
				}
				UEdGraphNode* SourceNode = SourcePin->GetOwningNode();
				if (!SourceNode || RerouteNodeSet.Contains(SourceNode))
				{
					continue;
				}
				TArray<UEdGraphPin*> TerminalTargets;
				TSet<UEdGraphNode*> VisitedReroutes;
				VisitedReroutes.Add(Node);
				CollectTerminalTargets(Knot->GetOutputPin(), VisitedReroutes, TerminalTargets);
				for (UEdGraphPin* TargetPin : TerminalTargets)
				{
					if (TargetPin && !SourcePin->LinkedTo.Contains(TargetPin))
					{
						Schema->TryCreateConnection(SourcePin, TargetPin);
					}
				}
			}
		}

		int32 RemovedCount = 0;
		for (UEdGraphNode* Node : RerouteNodes)
		{
			if (!Node || !Graph->Nodes.Contains(Node))
			{
				continue;
			}
			Node->Modify();
			Node->BreakAllNodeLinks();
			Graph->RemoveNode(Node);
			++RemovedCount;
		}
		return RemovedCount;
	}

	static FBlueprintLayoutRect MakeLayoutBounds(const TArray<FBlueprintLayoutNode>& Nodes, const TArray<int32>& NodeIndices, const bool bUseNewPosition)
	{
		FBlueprintLayoutRect Bounds;
		bool bHasBounds = false;
		for (const int32 NodeIndex : NodeIndices)
		{
			if (!Nodes.IsValidIndex(NodeIndex))
			{
				continue;
			}
			const FBlueprintLayoutRect Rect = MakeNodeRect(Nodes[NodeIndex], bUseNewPosition);
			if (!bHasBounds)
			{
				Bounds = Rect;
				bHasBounds = true;
				continue;
			}
			Bounds.Left = FMath::Min(Bounds.Left, Rect.Left);
			Bounds.Top = FMath::Min(Bounds.Top, Rect.Top);
			Bounds.Right = FMath::Max(Bounds.Right, Rect.Right);
			Bounds.Bottom = FMath::Max(Bounds.Bottom, Rect.Bottom);
		}
		return Bounds;
	}

	static double Orientation(const FVector2D& A, const FVector2D& B, const FVector2D& C)
	{
		return (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
	}

	static bool SegmentIntersectsSegment(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FVector2D& D)
	{
		const double O1 = Orientation(A, B, C);
		const double O2 = Orientation(A, B, D);
		const double O3 = Orientation(C, D, A);
		const double O4 = Orientation(C, D, B);
		return O1 * O2 < -KINDA_SMALL_NUMBER && O3 * O4 < -KINDA_SMALL_NUMBER;
	}

	static bool SegmentIntersectsRect(const FVector2D& A, const FVector2D& B, const FBlueprintLayoutRect& Rect)
	{
		if ((A.X >= Rect.Left && A.X <= Rect.Right && A.Y >= Rect.Top && A.Y <= Rect.Bottom)
			|| (B.X >= Rect.Left && B.X <= Rect.Right && B.Y >= Rect.Top && B.Y <= Rect.Bottom))
		{
			return true;
		}
		const FVector2D TL(Rect.Left, Rect.Top);
		const FVector2D TR(Rect.Right, Rect.Top);
		const FVector2D BR(Rect.Right, Rect.Bottom);
		const FVector2D BL(Rect.Left, Rect.Bottom);
		return SegmentIntersectsSegment(A, B, TL, TR)
			|| SegmentIntersectsSegment(A, B, TR, BR)
			|| SegmentIntersectsSegment(A, B, BR, BL)
			|| SegmentIntersectsSegment(A, B, BL, TL);
	}

	static TArray<FVector2D> BuildDefaultEdgePolyline(const TArray<FBlueprintLayoutNode>& Nodes, const FBlueprintLayoutEdge& Edge, const bool bUseNewPosition)
	{
		TArray<FVector2D> Points;
		const FVector2D Start = GetEdgeSourcePoint(Nodes, Edge, bUseNewPosition);
		const FVector2D End = GetEdgeTargetPoint(Nodes, Edge, bUseNewPosition);
		const double MidX = Start.X <= End.X ? (Start.X + End.X) * 0.5 : FMath::Max(Start.X, End.X) + 180.0;
		Points.Add(Start);
		Points.Add(FVector2D(MidX, Start.Y));
		Points.Add(FVector2D(MidX, End.Y));
		Points.Add(End);
		return Points;
	}

	static TArray<FVector2D> BuildReroutePlanPolyline(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const FBlueprintLayoutEdge& Edge,
		const FBlueprintLayoutReroutePlan* Plan,
		const bool bUseNewPosition)
	{
		if (!Plan)
		{
			return BuildDefaultEdgePolyline(Nodes, Edge, bUseNewPosition);
		}
		TArray<FVector2D> Points;
		Points.Add(GetEdgeSourcePoint(Nodes, Edge, bUseNewPosition));
		if (Plan->ReroutePoints.Num() > 0)
		{
			for (const FVector2D& Point : Plan->ReroutePoints)
			{
				Points.Add(Point);
			}
		}
		else
		{
			Points.Add(Plan->FirstPoint);
			Points.Add(Plan->SecondPoint);
		}
		Points.Add(GetEdgeTargetPoint(Nodes, Edge, bUseNewPosition));
		return Points;
	}

	static bool HasConflictingSharedTargetOrder(
		const TArray<FBlueprintLayoutEdge>& Edges,
		const int32 SourceA,
		const int32 SourceB,
		const bool bExec);

	static bool HasConflictingSharedSourceOrder(
		const TArray<FBlueprintLayoutEdge>& Edges,
		const int32 TargetA,
		const int32 TargetB,
		const bool bExec);

	static bool IsRoutedProducerDataIslandPinOrderPair(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const FBlueprintLayoutEdge& EdgeA,
		const FBlueprintLayoutEdge& EdgeB)
	{
		if (EdgeA.bExec
			|| EdgeB.bExec
			|| EdgeA.Target != EdgeB.Target
			|| !Nodes.IsValidIndex(EdgeA.Source)
			|| !Nodes.IsValidIndex(EdgeB.Source)
			|| !Nodes.IsValidIndex(EdgeA.Target))
		{
			return false;
		}
		const FBlueprintLayoutNode& TargetNode = Nodes[EdgeA.Target];
		if (NodeHasExecPin(TargetNode) || IsCompactBlueprintValueNode(TargetNode.Node))
		{
			return false;
		}
		const bool bAExecSource = NodeHasExecPin(Nodes[EdgeA.Source]);
		const bool bBExecSource = NodeHasExecPin(Nodes[EdgeB.Source]);
		const bool bACompactSource = IsCompactBlueprintValueNode(Nodes[EdgeA.Source].Node);
		const bool bBCompactSource = IsCompactBlueprintValueNode(Nodes[EdgeB.Source].Node);
		return (bAExecSource && bBCompactSource) || (bBExecSource && bACompactSource);
	}

	static int32 CountPinOrderViolations(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const bool bUseNewPosition,
		double* OutDistance = nullptr)
	{
		int32 Violations = 0;
		double Distance = 0.0;
		for (int32 A = 0; A < Edges.Num(); ++A)
		{
			if (!Nodes.IsValidIndex(Edges[A].Source) || !Nodes.IsValidIndex(Edges[A].Target))
			{
				continue;
			}
			const FVector2D StartA = GetEdgeSourcePoint(Nodes, Edges[A], bUseNewPosition);
			const FVector2D EndA = GetEdgeTargetPoint(Nodes, Edges[A], bUseNewPosition);
			for (int32 B = A + 1; B < Edges.Num(); ++B)
			{
				if (!Nodes.IsValidIndex(Edges[B].Source)
					|| !Nodes.IsValidIndex(Edges[B].Target)
					|| Edges[A].bExec != Edges[B].bExec)
				{
					continue;
				}
				const FVector2D StartB = GetEdgeSourcePoint(Nodes, Edges[B], bUseNewPosition);
				const FVector2D EndB = GetEdgeTargetPoint(Nodes, Edges[B], bUseNewPosition);
				if (Edges[A].Source == Edges[B].Source
					&& Edges[A].Target != Edges[B].Target
					&& Edges[A].SourcePinIndex != INDEX_NONE
					&& Edges[B].SourcePinIndex != INDEX_NONE
					&& !HasConflictingSharedSourceOrder(Edges, Edges[A].Target, Edges[B].Target, Edges[A].bExec))
				{
					const double SourcePinDelta = static_cast<double>(Edges[A].SourcePinIndex - Edges[B].SourcePinIndex);
					const double TargetYDelta = EndA.Y - EndB.Y;
					if (FMath::Abs(SourcePinDelta) > 0.5 && FMath::Abs(TargetYDelta) > 4.0 && SourcePinDelta * TargetYDelta < 0.0)
					{
						++Violations;
						Distance += FMath::Abs(TargetYDelta);
					}
				}
				if (Edges[A].Target == Edges[B].Target
					&& Edges[A].Source != Edges[B].Source
					&& Edges[A].TargetPinIndex != INDEX_NONE
					&& Edges[B].TargetPinIndex != INDEX_NONE
					&& !IsRoutedProducerDataIslandPinOrderPair(Nodes, Edges[A], Edges[B])
					&& !HasConflictingSharedTargetOrder(Edges, Edges[A].Source, Edges[B].Source, Edges[A].bExec))
				{
					const double TargetPinDelta = static_cast<double>(Edges[A].TargetPinIndex - Edges[B].TargetPinIndex);
					const double SourceYDelta = StartA.Y - StartB.Y;
					if (FMath::Abs(TargetPinDelta) > 0.5 && FMath::Abs(SourceYDelta) > 4.0 && TargetPinDelta * SourceYDelta < 0.0)
					{
						++Violations;
						Distance += FMath::Abs(SourceYDelta);
					}
				}
			}
		}
		if (OutDistance)
		{
			*OutDistance = Distance;
		}
		return Violations;
	}

	static bool HasConflictingSharedTargetOrder(
		const TArray<FBlueprintLayoutEdge>& Edges,
		const int32 SourceA,
		const int32 SourceB,
		const bool bExec)
	{
		int32 Direction = 0;
		for (const FBlueprintLayoutEdge& EdgeA : Edges)
		{
			if (EdgeA.Source != SourceA || EdgeA.bExec != bExec || EdgeA.TargetPinIndex == INDEX_NONE)
			{
				continue;
			}
			for (const FBlueprintLayoutEdge& EdgeB : Edges)
			{
				if (EdgeB.Source != SourceB
					|| EdgeB.bExec != bExec
					|| EdgeB.Target != EdgeA.Target
					|| EdgeB.TargetPinIndex == INDEX_NONE
					|| EdgeB.TargetPinIndex == EdgeA.TargetPinIndex)
				{
					continue;
				}
				const int32 ThisDirection = EdgeA.TargetPinIndex < EdgeB.TargetPinIndex ? -1 : 1;
				if (Direction != 0 && Direction != ThisDirection)
				{
					return true;
				}
				Direction = ThisDirection;
			}
		}
		return false;
	}

	static bool HasConflictingSharedSourceOrder(
		const TArray<FBlueprintLayoutEdge>& Edges,
		const int32 TargetA,
		const int32 TargetB,
		const bool bExec)
	{
		int32 Direction = 0;
		for (const FBlueprintLayoutEdge& EdgeA : Edges)
		{
			if (EdgeA.Target != TargetA || EdgeA.bExec != bExec || EdgeA.SourcePinIndex == INDEX_NONE)
			{
				continue;
			}
			for (const FBlueprintLayoutEdge& EdgeB : Edges)
			{
				if (EdgeB.Target != TargetB
					|| EdgeB.bExec != bExec
					|| EdgeB.Source != EdgeA.Source
					|| EdgeB.SourcePinIndex == INDEX_NONE
					|| EdgeB.SourcePinIndex == EdgeA.SourcePinIndex)
				{
					continue;
				}
				const int32 ThisDirection = EdgeA.SourcePinIndex < EdgeB.SourcePinIndex ? -1 : 1;
				if (Direction != 0 && Direction != ThisDirection)
				{
					return true;
				}
				Direction = ThisDirection;
			}
		}
		return false;
	}

	static FBlueprintLayoutMetrics EvaluateLayoutMetrics(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const bool bUseNewPosition,
		const double NodePadding)
	{
		FBlueprintLayoutMetrics Metrics;
		TArray<int32> NonRerouteNodes;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (!Nodes[NodeIndex].bRerouteNode)
			{
				NonRerouteNodes.Add(NodeIndex);
			}
		}
		for (int32 A = 0; A < NonRerouteNodes.Num(); ++A)
		{
			const FBlueprintLayoutRect RectA = MakeNodeRect(Nodes[NonRerouteNodes[A]], bUseNewPosition);
			for (int32 B = A + 1; B < NonRerouteNodes.Num(); ++B)
			{
				const FBlueprintLayoutRect RectB = MakeNodeRect(Nodes[NonRerouteNodes[B]], bUseNewPosition);
				if (RectA.Intersects(RectB, NodePadding))
				{
					++Metrics.OverlapCount;
				}
			}
		}

		for (const FBlueprintLayoutEdge& Edge : Edges)
		{
			if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
			{
				continue;
			}
			const TArray<FVector2D> Points = BuildDefaultEdgePolyline(Nodes, Edge, bUseNewPosition);
			for (int32 SegmentIndex = 0; SegmentIndex + 1 < Points.Num(); ++SegmentIndex)
			{
				const double Length = FMath::Abs(Points[SegmentIndex + 1].X - Points[SegmentIndex].X)
					+ FMath::Abs(Points[SegmentIndex + 1].Y - Points[SegmentIndex].Y);
				Metrics.TotalWireLength += Length;
				if (!Edge.bExec)
				{
					Metrics.TotalNonExecWireLength += Length;
				}
			}
			const FVector2D Start = Points[0];
			const FVector2D End = Points.Last();
			if (Start.X > End.X + 1.0)
			{
				++Metrics.BackwardEdgeCount;
				Metrics.BackwardWireLength += Start.X - End.X;
				if (Edge.bExec)
				{
					++Metrics.BackwardExecEdgeCount;
					Metrics.BackwardExecWireLength += Start.X - End.X;
				}
				else
				{
					++Metrics.BackwardDataEdgeCount;
					Metrics.BackwardDataWireLength += Start.X - End.X;
				}
			}

			for (const int32 NodeIndex : NonRerouteNodes)
			{
				if (NodeIndex == Edge.Source || NodeIndex == Edge.Target)
				{
					continue;
				}
				const FBlueprintLayoutRect NodeRect = MakeNodeRect(Nodes[NodeIndex], bUseNewPosition);
				for (int32 SegmentIndex = 0; SegmentIndex + 1 < Points.Num(); ++SegmentIndex)
				{
					if (SegmentIntersectsRect(Points[SegmentIndex], Points[SegmentIndex + 1], NodeRect))
					{
						++Metrics.EdgeNodeIntersectionCount;
						break;
					}
				}
			}
		}

		for (int32 A = 0; A < Edges.Num(); ++A)
		{
			if (!Nodes.IsValidIndex(Edges[A].Source) || !Nodes.IsValidIndex(Edges[A].Target))
			{
				continue;
			}
			const TArray<FVector2D> PointsA = BuildDefaultEdgePolyline(Nodes, Edges[A], bUseNewPosition);
			for (int32 B = A + 1; B < Edges.Num(); ++B)
			{
				if (!Nodes.IsValidIndex(Edges[B].Source)
					|| !Nodes.IsValidIndex(Edges[B].Target)
					|| Edges[A].Source == Edges[B].Source
					|| Edges[A].Target == Edges[B].Target
					|| Edges[A].Source == Edges[B].Target
					|| Edges[A].Target == Edges[B].Source)
				{
					continue;
				}
				const TArray<FVector2D> PointsB = BuildDefaultEdgePolyline(Nodes, Edges[B], bUseNewPosition);
				for (int32 SegmentA = 0; SegmentA + 1 < PointsA.Num(); ++SegmentA)
				{
					for (int32 SegmentB = 0; SegmentB + 1 < PointsB.Num(); ++SegmentB)
					{
						if (SegmentIntersectsSegment(PointsA[SegmentA], PointsA[SegmentA + 1], PointsB[SegmentB], PointsB[SegmentB + 1]))
						{
							++Metrics.EdgeCrossingCount;
						}
					}
				}
			}
		}

		Metrics.PinOrderViolationCount = CountPinOrderViolations(Nodes, Edges, bUseNewPosition, &Metrics.PinOrderViolationDistance);
		const FBlueprintLayoutRect Bounds = MakeLayoutBounds(Nodes, NonRerouteNodes, bUseNewPosition);
		Metrics.LayoutWidth = FMath::Max(0.0, Bounds.Right - Bounds.Left);
		Metrics.LayoutHeight = FMath::Max(0.0, Bounds.Bottom - Bounds.Top);
		Metrics.LayoutArea = Metrics.LayoutWidth * Metrics.LayoutHeight;
		Metrics.LayoutAspectRatio = Metrics.LayoutWidth / FMath::Max(1.0, Metrics.LayoutHeight);
		if (Metrics.LayoutAspectRatio < 0.2)
		{
			Metrics.LayoutAspectRatioPenalty = 0.2 - Metrics.LayoutAspectRatio;
		}
		else if (Metrics.LayoutAspectRatio > 5.0)
		{
			Metrics.LayoutAspectRatioPenalty = Metrics.LayoutAspectRatio - 5.0;
		}
		return Metrics;
	}

	static FBlueprintLayoutMetrics EvaluateLayoutMetricsWithReroutePlans(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<FBlueprintLayoutReroutePlan>& Plans,
		const bool bUseNewPosition,
		const double NodePadding)
	{
		TMap<int32, FBlueprintLayoutReroutePlan> PlanByEdge;
		for (const FBlueprintLayoutReroutePlan& Plan : Plans)
		{
			if (Edges.IsValidIndex(Plan.RawEdgeIndex))
			{
				PlanByEdge.Add(Plan.RawEdgeIndex, Plan);
			}
		}

		FBlueprintLayoutMetrics Metrics;
		TArray<int32> NonRerouteNodes;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (Nodes.IsValidIndex(NodeIndex) && !Nodes[NodeIndex].bRerouteNode)
			{
				NonRerouteNodes.Add(NodeIndex);
			}
		}

		for (int32 A = 0; A < NonRerouteNodes.Num(); ++A)
		{
			const FBlueprintLayoutRect RectA = MakeNodeRect(Nodes[NonRerouteNodes[A]], bUseNewPosition);
			for (int32 B = A + 1; B < NonRerouteNodes.Num(); ++B)
			{
				const FBlueprintLayoutRect RectB = MakeNodeRect(Nodes[NonRerouteNodes[B]], bUseNewPosition);
				if (RectA.Intersects(RectB, NodePadding))
				{
					++Metrics.OverlapCount;
				}
			}
		}

		TArray<TArray<FVector2D>> EdgePolylines;
		EdgePolylines.SetNum(Edges.Num());
		for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
		{
			if (!Edges.IsValidIndex(EdgeIndex)
				|| !Nodes.IsValidIndex(Edges[EdgeIndex].Source)
				|| !Nodes.IsValidIndex(Edges[EdgeIndex].Target))
			{
				continue;
			}
			const FBlueprintLayoutReroutePlan* Plan = PlanByEdge.Find(EdgeIndex);
			EdgePolylines[EdgeIndex] = BuildReroutePlanPolyline(Nodes, Edges[EdgeIndex], Plan, bUseNewPosition);
			const TArray<FVector2D>& Points = EdgePolylines[EdgeIndex];
			for (int32 SegmentIndex = 0; SegmentIndex + 1 < Points.Num(); ++SegmentIndex)
			{
				const double Length = FMath::Abs(Points[SegmentIndex + 1].X - Points[SegmentIndex].X)
					+ FMath::Abs(Points[SegmentIndex + 1].Y - Points[SegmentIndex].Y);
				Metrics.TotalWireLength += Length;
				if (!Edges[EdgeIndex].bExec)
				{
					Metrics.TotalNonExecWireLength += Length;
				}
			}
			const FVector2D Start = Points[0];
			const FVector2D End = Points.Last();
			if (Start.X > End.X + 1.0)
			{
				++Metrics.BackwardEdgeCount;
				Metrics.BackwardWireLength += Start.X - End.X;
				if (Edges[EdgeIndex].bExec)
				{
					++Metrics.BackwardExecEdgeCount;
					Metrics.BackwardExecWireLength += Start.X - End.X;
				}
				else
				{
					++Metrics.BackwardDataEdgeCount;
					Metrics.BackwardDataWireLength += Start.X - End.X;
				}
			}

			for (const int32 NodeIndex : NonRerouteNodes)
			{
				if (NodeIndex == Edges[EdgeIndex].Source || NodeIndex == Edges[EdgeIndex].Target)
				{
					continue;
				}
				const FBlueprintLayoutRect NodeRect = MakeNodeRect(Nodes[NodeIndex], bUseNewPosition);
				for (int32 SegmentIndex = 0; SegmentIndex + 1 < Points.Num(); ++SegmentIndex)
				{
					if (SegmentIntersectsRect(Points[SegmentIndex], Points[SegmentIndex + 1], NodeRect))
					{
						++Metrics.EdgeNodeIntersectionCount;
						break;
					}
				}
			}
		}

		for (int32 A = 0; A < Edges.Num(); ++A)
		{
			if (!Nodes.IsValidIndex(Edges[A].Source) || !Nodes.IsValidIndex(Edges[A].Target))
			{
				continue;
			}
			const TArray<FVector2D>& PointsA = EdgePolylines[A];
			for (int32 B = A + 1; B < Edges.Num(); ++B)
			{
				if (!Nodes.IsValidIndex(Edges[B].Source)
					|| !Nodes.IsValidIndex(Edges[B].Target)
					|| Edges[A].Source == Edges[B].Source
					|| Edges[A].Target == Edges[B].Target
					|| Edges[A].Source == Edges[B].Target
					|| Edges[A].Target == Edges[B].Source)
				{
					continue;
				}
				const TArray<FVector2D>& PointsB = EdgePolylines[B];
				for (int32 SegmentA = 0; SegmentA + 1 < PointsA.Num(); ++SegmentA)
				{
					for (int32 SegmentB = 0; SegmentB + 1 < PointsB.Num(); ++SegmentB)
					{
						if (SegmentIntersectsSegment(PointsA[SegmentA], PointsA[SegmentA + 1], PointsB[SegmentB], PointsB[SegmentB + 1]))
						{
							++Metrics.EdgeCrossingCount;
						}
					}
				}
			}
		}

		Metrics.PinOrderViolationCount = CountPinOrderViolations(Nodes, Edges, bUseNewPosition, &Metrics.PinOrderViolationDistance);
		const FBlueprintLayoutRect Bounds = MakeLayoutBounds(Nodes, NonRerouteNodes, bUseNewPosition);
		Metrics.LayoutWidth = FMath::Max(0.0, Bounds.Right - Bounds.Left);
		Metrics.LayoutHeight = FMath::Max(0.0, Bounds.Bottom - Bounds.Top);
		Metrics.LayoutArea = Metrics.LayoutWidth * Metrics.LayoutHeight;
		Metrics.LayoutAspectRatio = Metrics.LayoutWidth / FMath::Max(1.0, Metrics.LayoutHeight);
		if (Metrics.LayoutAspectRatio < 0.2)
		{
			Metrics.LayoutAspectRatioPenalty = 0.2 - Metrics.LayoutAspectRatio;
		}
		else if (Metrics.LayoutAspectRatio > 5.0)
		{
			Metrics.LayoutAspectRatioPenalty = Metrics.LayoutAspectRatio - 5.0;
		}
		return Metrics;
	}

	static double ScoreLayoutCandidate(const FBlueprintLayoutMetrics& Metrics)
	{
		return Metrics.OverlapCount * 1000000000.0
			+ Metrics.PinOrderViolationCount * 20000000.0
			+ Metrics.BackwardDataEdgeCount * 12000000.0
			+ Metrics.EdgeCrossingCount * 1000000.0
			+ Metrics.EdgeNodeIntersectionCount * 100000.0
			+ Metrics.LayoutArea * 0.00005
			+ Metrics.LayoutAspectRatioPenalty * 10000.0
			+ Metrics.TotalWireLength;
	}

	static TSharedPtr<FJsonObject> LayoutMetricsToJson(const FBlueprintLayoutMetrics& Metrics)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("overlap_count"), Metrics.OverlapCount);
		Obj->SetNumberField(TEXT("edge_crossing_count"), Metrics.EdgeCrossingCount);
		Obj->SetNumberField(TEXT("edge_node_intersection_count"), Metrics.EdgeNodeIntersectionCount);
		Obj->SetNumberField(TEXT("backward_edge_count"), Metrics.BackwardEdgeCount);
		Obj->SetNumberField(TEXT("backward_data_edge_count"), Metrics.BackwardDataEdgeCount);
		Obj->SetNumberField(TEXT("backward_exec_edge_count"), Metrics.BackwardExecEdgeCount);
		Obj->SetNumberField(TEXT("pin_order_violation_count"), Metrics.PinOrderViolationCount);
		Obj->SetNumberField(TEXT("total_wire_manhattan_length"), Metrics.TotalWireLength);
		Obj->SetNumberField(TEXT("total_non_exec_wire_manhattan_length"), Metrics.TotalNonExecWireLength);
		Obj->SetNumberField(TEXT("backward_wire_manhattan_length"), Metrics.BackwardWireLength);
		Obj->SetNumberField(TEXT("backward_data_wire_manhattan_length"), Metrics.BackwardDataWireLength);
		Obj->SetNumberField(TEXT("backward_exec_wire_manhattan_length"), Metrics.BackwardExecWireLength);
		Obj->SetNumberField(TEXT("pin_order_violation_distance"), Metrics.PinOrderViolationDistance);
		Obj->SetNumberField(TEXT("layout_width"), Metrics.LayoutWidth);
		Obj->SetNumberField(TEXT("layout_height"), Metrics.LayoutHeight);
		Obj->SetNumberField(TEXT("layout_area"), Metrics.LayoutArea);
		Obj->SetNumberField(TEXT("layout_aspect_ratio"), Metrics.LayoutAspectRatio);
		Obj->SetNumberField(TEXT("layout_aspect_ratio_penalty"), Metrics.LayoutAspectRatioPenalty);
		return Obj;
	}

	static TArray<TArray<int32>> BuildConnectedComponents(const TArray<FBlueprintLayoutNode>& Nodes, const TArray<FBlueprintLayoutEdge>& Edges)
	{
		TArray<TArray<int32>> Adjacency;
		Adjacency.SetNum(Nodes.Num());
		for (const FBlueprintLayoutEdge& Edge : Edges)
		{
			if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
			{
				continue;
			}
			Adjacency[Edge.Source].Add(Edge.Target);
			Adjacency[Edge.Target].Add(Edge.Source);
		}

		TArray<bool> Visited;
		Visited.Init(false, Nodes.Num());
		TArray<TArray<int32>> Components;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (Visited[NodeIndex] || Nodes[NodeIndex].bRerouteNode)
			{
				continue;
			}
			TArray<int32> Component;
			TArray<int32> Stack;
			Stack.Add(NodeIndex);
			Visited[NodeIndex] = true;
			while (Stack.Num() > 0)
			{
				const int32 Current = Stack.Pop(EAllowShrinking::No);
				Component.Add(Current);
				for (const int32 Next : Adjacency[Current])
				{
					if (Nodes.IsValidIndex(Next) && !Visited[Next] && !Nodes[Next].bRerouteNode)
					{
						Visited[Next] = true;
						Stack.Add(Next);
					}
				}
			}
			Component.Sort([&Nodes](const int32 A, const int32 B)
			{
				return CompareStableNodeIdentity(Nodes, A, B);
			});
			Components.Add(Component);
		}
		Components.Sort([&Nodes](const TArray<int32>& A, const TArray<int32>& B)
		{
			const int32 AFirst = A.Num() > 0 ? A[0] : INDEX_NONE;
			const int32 BFirst = B.Num() > 0 ? B[0] : INDEX_NONE;
			return CompareStableNodeIdentity(Nodes, AFirst, BFirst);
		});
		return Components;
	}

	static int32 CompareIntegerSignatures(const TArray<int32>& A, const TArray<int32>& B)
	{
		const int32 Count = FMath::Min(A.Num(), B.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (A[Index] != B[Index])
			{
				return A[Index] < B[Index] ? -1 : 1;
			}
		}
		if (A.Num() != B.Num())
		{
			return A.Num() < B.Num() ? -1 : 1;
		}
		return 0;
	}

	static double MedianValue(TArray<double> Values)
	{
		if (Values.Num() == 0)
		{
			return 0.0;
		}
		Values.Sort();
		const int32 Mid = Values.Num() / 2;
		if (Values.Num() % 2 == 1)
		{
			return Values[Mid];
		}
		return (Values[Mid - 1] + Values[Mid]) * 0.5;
	}

	static double LayoutManhattanDistance(const FVector2D& A, const FVector2D& B)
	{
		return FMath::Abs(A.X - B.X) + FMath::Abs(A.Y - B.Y);
	}

	static void AddUniqueLayoutValue(TArray<double>& Values, const double Value, const double Tolerance = 8.0)
	{
		if (!FMath::IsFinite(Value))
		{
			return;
		}
		for (const double Existing : Values)
		{
			if (FMath::IsNearlyEqual(Existing, Value, Tolerance))
			{
				return;
			}
		}
		Values.Add(Value);
	}

	static bool FindSharedFanoutTrunkY(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const double TrunkStartX,
		const double TrunkEndX,
		const double SourceY,
		const TArray<double>& TargetYs,
		const double HorizontalSpacing,
		const double NodePadding,
		double& OutTrunkY)
	{
		if (TargetYs.Num() == 0 || TrunkEndX <= TrunkStartX + 24.0)
		{
			return false;
		}

		const double TargetMedianY = MedianValue(TargetYs);
		double MinTargetY = TNumericLimits<double>::Max();
		double MaxTargetY = -TNumericLimits<double>::Max();
		for (const double TargetY : TargetYs)
		{
			MinTargetY = FMath::Min(MinTargetY, TargetY);
			MaxTargetY = FMath::Max(MaxTargetY, TargetY);
		}

		const double SearchPadding = FMath::Clamp(HorizontalSpacing * 0.42, 140.0, 300.0);
		const double SearchTop = FMath::Min(SourceY, MinTargetY) - SearchPadding;
		const double SearchBottom = FMath::Max(SourceY, MaxTargetY) + SearchPadding;
		const double Clearance = FMath::Clamp(NodePadding * 1.6, 36.0, 96.0);

		TArray<double> CandidateYs;
		AddUniqueLayoutValue(CandidateYs, TargetMedianY);
		AddUniqueLayoutValue(CandidateYs, (MinTargetY + MaxTargetY) * 0.5);
		AddUniqueLayoutValue(CandidateYs, SourceY);
		for (const double TargetY : TargetYs)
		{
			AddUniqueLayoutValue(CandidateYs, TargetY);
		}

		TArray<FBlueprintLayoutVerticalInterval> Intervals;
		for (const FBlueprintLayoutNode& Node : Nodes)
		{
			if (Node.bRerouteNode)
			{
				continue;
			}
			const FBlueprintLayoutRect Rect = MakeNodeRect(Node, true);
			if (Rect.Right < TrunkStartX - Clearance || Rect.Left > TrunkEndX + Clearance)
			{
				continue;
			}
			FBlueprintLayoutVerticalInterval Interval;
			Interval.Top = Rect.Top - Clearance;
			Interval.Bottom = Rect.Bottom + Clearance;
			Intervals.Add(Interval);
			AddUniqueLayoutValue(CandidateYs, Interval.Top - Clearance * 0.5);
			AddUniqueLayoutValue(CandidateYs, Interval.Bottom + Clearance * 0.5);
		}
		Intervals.Sort([](const FBlueprintLayoutVerticalInterval& A, const FBlueprintLayoutVerticalInterval& B)
		{
			if (!FMath::IsNearlyEqual(A.Top, B.Top, 1.0))
			{
				return A.Top < B.Top;
			}
			return A.Bottom < B.Bottom;
		});

		double CursorBottom = SearchTop;
		for (const FBlueprintLayoutVerticalInterval& Interval : Intervals)
		{
			if (Interval.Top > CursorBottom + Clearance)
			{
				AddUniqueLayoutValue(CandidateYs, (CursorBottom + Interval.Top) * 0.5);
			}
			CursorBottom = FMath::Max(CursorBottom, Interval.Bottom);
		}
		if (SearchBottom > CursorBottom + Clearance)
		{
			AddUniqueLayoutValue(CandidateYs, (CursorBottom + SearchBottom) * 0.5);
		}

		double BestScore = TNumericLimits<double>::Max();
		double BestY = TargetMedianY;
		int32 BestHardHits = TNumericLimits<int32>::Max();
		for (const double CandidateY : CandidateYs)
		{
			if (CandidateY < SearchTop || CandidateY > SearchBottom)
			{
				continue;
			}

			int32 HardHits = 0;
			int32 SoftHits = 0;
			double MinClearance = TNumericLimits<double>::Max();
			for (const FBlueprintLayoutNode& Node : Nodes)
			{
				if (Node.bRerouteNode)
				{
					continue;
				}
				const FBlueprintLayoutRect Rect = MakeNodeRect(Node, true);
				if (Rect.Right < TrunkStartX || Rect.Left > TrunkEndX)
				{
					continue;
				}
				if (CandidateY >= Rect.Top && CandidateY <= Rect.Bottom)
				{
					++HardHits;
				}
				if (CandidateY >= Rect.Top - Clearance && CandidateY <= Rect.Bottom + Clearance)
				{
					++SoftHits;
				}
				MinClearance = FMath::Min(
					MinClearance,
					FMath::Min(FMath::Abs(CandidateY - Rect.Top), FMath::Abs(CandidateY - Rect.Bottom)));
			}
			if (MinClearance == TNumericLimits<double>::Max())
			{
				MinClearance = Clearance;
			}
			const double Score = HardHits * 1000000.0
				+ SoftHits * 50000.0
				+ FMath::Abs(CandidateY - TargetMedianY)
				+ FMath::Abs(CandidateY - SourceY) * 0.18
				- FMath::Min(MinClearance, Clearance) * 12.0;
			if (HardHits < BestHardHits || (HardHits == BestHardHits && Score < BestScore))
			{
				BestHardHits = HardHits;
				BestScore = Score;
				BestY = CandidateY;
			}
		}

		if (BestHardHits > 0)
		{
			return false;
		}
		OutTrunkY = BestY;
		return true;
	}

	static int32 CountAdjacentLayerCrossings(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<int32>& LeftLayer,
		const TArray<int32>& RightLayer)
	{
		TMap<int32, int32> LeftOrder;
		TMap<int32, int32> RightOrder;
		for (int32 Index = 0; Index < LeftLayer.Num(); ++Index)
		{
			LeftOrder.Add(LeftLayer[Index], Index);
		}
		for (int32 Index = 0; Index < RightLayer.Num(); ++Index)
		{
			RightOrder.Add(RightLayer[Index], Index);
		}

		struct FAnchorEdge
		{
			double Left = 0.0;
			double Right = 0.0;
		};
		TArray<FAnchorEdge> AnchorEdges;
		for (const FBlueprintLayoutEdge& Edge : Edges)
		{
			const int32* SourceOrder = LeftOrder.Find(Edge.Source);
			const int32* TargetOrder = RightOrder.Find(Edge.Target);
			if (!SourceOrder || !TargetOrder || !Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
			{
				continue;
			}
			FAnchorEdge Anchor;
			Anchor.Left = static_cast<double>(*SourceOrder) + GetSourcePinFraction(Nodes[Edge.Source], Edge);
			Anchor.Right = static_cast<double>(*TargetOrder) + GetTargetPinFraction(Nodes[Edge.Target], Edge);
			AnchorEdges.Add(Anchor);
		}

		int32 Crossings = 0;
		for (int32 A = 0; A < AnchorEdges.Num(); ++A)
		{
			for (int32 B = A + 1; B < AnchorEdges.Num(); ++B)
			{
				const double LeftDelta = AnchorEdges[A].Left - AnchorEdges[B].Left;
				const double RightDelta = AnchorEdges[A].Right - AnchorEdges[B].Right;
				if (FMath::Abs(LeftDelta) > KINDA_SMALL_NUMBER
					&& FMath::Abs(RightDelta) > KINDA_SMALL_NUMBER
					&& LeftDelta * RightDelta < 0.0)
				{
					++Crossings;
				}
			}
		}
		return Crossings;
	}

	static int32 CountLayerNeighborCrossings(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& Layers,
		const int32 LayerIndex)
	{
		int32 Count = 0;
		if (Layers.IsValidIndex(LayerIndex - 1))
		{
			Count += CountAdjacentLayerCrossings(Nodes, Edges, Layers[LayerIndex - 1], Layers[LayerIndex]);
		}
		if (Layers.IsValidIndex(LayerIndex + 1))
		{
			Count += CountAdjacentLayerCrossings(Nodes, Edges, Layers[LayerIndex], Layers[LayerIndex + 1]);
		}
		return Count;
	}

	static TArray<int32> BuildPortSignatureForNode(
		const int32 NodeIndex,
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<int32>& EdgeIndices,
		const bool bUseOutgoingTargetPins)
	{
		TArray<int32> Signature;
		for (const int32 EdgeIndex : EdgeIndices)
		{
			if (!Edges.IsValidIndex(EdgeIndex))
			{
				continue;
			}
			const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
			if (bUseOutgoingTargetPins && Edge.Source == NodeIndex)
			{
				Signature.Add(FMath::Max(0, Edge.TargetPinIndex));
			}
			else if (!bUseOutgoingTargetPins && Edge.Target == NodeIndex)
			{
				Signature.Add(FMath::Max(0, Edge.SourcePinIndex));
			}
		}
		Signature.Sort();
		return Signature;
	}

	static void UpdateLayerOrders(TArray<FBlueprintLayoutNode>& Nodes, const TArray<int32>& Layer)
	{
		for (int32 Order = 0; Order < Layer.Num(); ++Order)
		{
			if (Nodes.IsValidIndex(Layer[Order]))
			{
				Nodes[Layer[Order]].Order = Order;
			}
		}
	}

	static void SortLayerByNeighborMedian(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		TArray<int32>& Layer,
		const bool bSortByIncoming)
	{
		TArray<FBlueprintLayoutLayerNode> SortItems;
		SortItems.Reserve(Layer.Num());
		for (const int32 NodeIndex : Layer)
		{
			FBlueprintLayoutLayerNode Item;
			Item.NodeIndex = NodeIndex;
			TArray<double> Keys;
			TArray<int32> LocalEdgeIndices;
			const TArray<int32>& IncidentEdges = bSortByIncoming ? IncomingEdges[NodeIndex] : OutgoingEdges[NodeIndex];
			for (const int32 EdgeIndex : IncidentEdges)
			{
				if (!Edges.IsValidIndex(EdgeIndex))
				{
					continue;
				}
				const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
				if (bSortByIncoming)
				{
					if (!Nodes.IsValidIndex(Edge.Source) || Nodes[Edge.Source].Rank >= Nodes[NodeIndex].Rank)
					{
						continue;
					}
					Keys.Add(static_cast<double>(Nodes[Edge.Source].Order) + GetSourcePinFraction(Nodes[Edge.Source], Edge));
					LocalEdgeIndices.Add(EdgeIndex);
				}
				else
				{
					if (!Nodes.IsValidIndex(Edge.Target) || Nodes[Edge.Target].Rank <= Nodes[NodeIndex].Rank)
					{
						continue;
					}
					Keys.Add(static_cast<double>(Nodes[Edge.Target].Order) + GetTargetPinFraction(Nodes[Edge.Target], Edge));
					LocalEdgeIndices.Add(EdgeIndex);
				}
			}
			Item.Key = Keys.Num() > 0 ? MedianValue(Keys) : static_cast<double>(Nodes[NodeIndex].Order);
			Item.Signature = BuildPortSignatureForNode(NodeIndex, Nodes, Edges, LocalEdgeIndices, !bSortByIncoming);
			SortItems.Add(Item);
		}

		SortItems.Sort([&Nodes](const FBlueprintLayoutLayerNode& A, const FBlueprintLayoutLayerNode& B)
		{
			if (!FMath::IsNearlyEqual(A.Key, B.Key, 0.0001))
			{
				return A.Key < B.Key;
			}
			const int32 SignatureCompare = CompareIntegerSignatures(A.Signature, B.Signature);
			if (SignatureCompare != 0)
			{
				return SignatureCompare < 0;
			}
			return CompareStableNodeIdentity(Nodes, A.NodeIndex, B.NodeIndex);
		});

		Layer.Reset();
		for (const FBlueprintLayoutLayerNode& Item : SortItems)
		{
			Layer.Add(Item.NodeIndex);
		}
		UpdateLayerOrders(Nodes, Layer);
	}

	static void RefineLayerByAdjacentTranspose(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		TArray<TArray<int32>>& Layers,
		const int32 LayerIndex)
	{
		if (!Layers.IsValidIndex(LayerIndex) || Layers[LayerIndex].Num() < 2)
		{
			return;
		}
		bool bChanged = true;
		int32 Guard = 0;
		while (bChanged && Guard++ < Layers[LayerIndex].Num() * 4)
		{
			bChanged = false;
			for (int32 Index = 0; Index + 1 < Layers[LayerIndex].Num(); ++Index)
			{
				const int32 BeforeCrossings = CountLayerNeighborCrossings(Nodes, Edges, Layers, LayerIndex);
				Layers[LayerIndex].Swap(Index, Index + 1);
				UpdateLayerOrders(Nodes, Layers[LayerIndex]);
				const int32 AfterCrossings = CountLayerNeighborCrossings(Nodes, Edges, Layers, LayerIndex);
				if (AfterCrossings < BeforeCrossings)
				{
					bChanged = true;
				}
				else
				{
					Layers[LayerIndex].Swap(Index, Index + 1);
					UpdateLayerOrders(Nodes, Layers[LayerIndex]);
				}
			}
		}
	}

	static void AssignRanksForComponent(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const TArray<int32>& Component)
	{
		TSet<int32> ComponentSet;
		for (const int32 NodeIndex : Component)
		{
			ComponentSet.Add(NodeIndex);
			if (Nodes.IsValidIndex(NodeIndex))
			{
				Nodes[NodeIndex].Rank = 0;
			}
		}

		TMap<int32, int32> InDegree;
		for (const int32 NodeIndex : Component)
		{
			InDegree.Add(NodeIndex, 0);
		}
		for (const FBlueprintLayoutEdge& Edge : Edges)
		{
			if (ComponentSet.Contains(Edge.Source) && ComponentSet.Contains(Edge.Target) && Edge.Source != Edge.Target)
			{
				InDegree.FindOrAdd(Edge.Target) += 1;
			}
		}

		TArray<int32> Queue;
		for (const int32 NodeIndex : Component)
		{
			const int32* Degree = InDegree.Find(NodeIndex);
			if (Degree && *Degree == 0)
			{
				Queue.Add(NodeIndex);
			}
		}
		Queue.Sort([&Nodes](const int32 A, const int32 B)
		{
			return CompareStableNodeIdentity(Nodes, A, B);
		});

		TArray<int32> TopologicalOrder;
		TSet<int32> Emitted;
		while (Queue.Num() > 0)
		{
			const int32 Current = Queue[0];
			Queue.RemoveAt(0, 1, EAllowShrinking::No);
			if (Emitted.Contains(Current))
			{
				continue;
			}
			Emitted.Add(Current);
			TopologicalOrder.Add(Current);
			if (!OutgoingEdges.IsValidIndex(Current))
			{
				continue;
			}
			for (const int32 EdgeIndex : OutgoingEdges[Current])
			{
				if (!Edges.IsValidIndex(EdgeIndex) || !ComponentSet.Contains(Edges[EdgeIndex].Target))
				{
					continue;
				}
				int32& Degree = InDegree.FindOrAdd(Edges[EdgeIndex].Target);
				Degree = FMath::Max(0, Degree - 1);
				if (Degree == 0)
				{
					Queue.Add(Edges[EdgeIndex].Target);
					Queue.Sort([&Nodes](const int32 A, const int32 B)
					{
						return CompareStableNodeIdentity(Nodes, A, B);
					});
				}
			}
		}
		for (const int32 NodeIndex : Component)
		{
			if (!Emitted.Contains(NodeIndex))
			{
				TopologicalOrder.Add(NodeIndex);
			}
		}

		TMap<int32, int32> TopoPosition;
		for (int32 Index = 0; Index < TopologicalOrder.Num(); ++Index)
		{
			TopoPosition.Add(TopologicalOrder[Index], Index);
		}

		for (const int32 NodeIndex : TopologicalOrder)
		{
			if (!OutgoingEdges.IsValidIndex(NodeIndex))
			{
				continue;
			}
			for (const int32 EdgeIndex : OutgoingEdges[NodeIndex])
			{
				if (!Edges.IsValidIndex(EdgeIndex) || !ComponentSet.Contains(Edges[EdgeIndex].Target))
				{
					continue;
				}
				const int32* SourceTopo = TopoPosition.Find(Edges[EdgeIndex].Source);
				const int32* TargetTopo = TopoPosition.Find(Edges[EdgeIndex].Target);
				if (!SourceTopo || !TargetTopo || *SourceTopo >= *TargetTopo)
				{
					continue;
				}
				Nodes[Edges[EdgeIndex].Target].Rank = FMath::Max(Nodes[Edges[EdgeIndex].Target].Rank, Nodes[Edges[EdgeIndex].Source].Rank + 1);
			}
		}

		for (int32 Pass = 0; Pass < Component.Num(); ++Pass)
		{
			bool bChanged = false;
			for (int32 ReverseIndex = TopologicalOrder.Num() - 1; ReverseIndex >= 0; --ReverseIndex)
			{
				const int32 NodeIndex = TopologicalOrder[ReverseIndex];
				if (!Nodes.IsValidIndex(NodeIndex)
					|| NodeHasExecPin(Nodes[NodeIndex])
					|| !OutgoingEdges.IsValidIndex(NodeIndex))
				{
					continue;
				}
				int32 MinTargetRank = TNumericLimits<int32>::Max();
				for (const int32 EdgeIndex : OutgoingEdges[NodeIndex])
				{
					if (Edges.IsValidIndex(EdgeIndex) && !Edges[EdgeIndex].bExec && ComponentSet.Contains(Edges[EdgeIndex].Target))
					{
						MinTargetRank = FMath::Min(MinTargetRank, Nodes[Edges[EdgeIndex].Target].Rank);
					}
				}
				if (MinTargetRank == TNumericLimits<int32>::Max())
				{
					continue;
				}
				int32 MinAllowedRank = 0;
				if (IncomingEdges.IsValidIndex(NodeIndex))
				{
					for (const int32 EdgeIndex : IncomingEdges[NodeIndex])
					{
						if (Edges.IsValidIndex(EdgeIndex) && ComponentSet.Contains(Edges[EdgeIndex].Source))
						{
							MinAllowedRank = FMath::Max(MinAllowedRank, Nodes[Edges[EdgeIndex].Source].Rank + 1);
						}
					}
				}
				const int32 DesiredRank = FMath::Max(MinAllowedRank, MinTargetRank - 1);
				if (DesiredRank != Nodes[NodeIndex].Rank)
				{
					Nodes[NodeIndex].Rank = DesiredRank;
					bChanged = true;
				}
			}
			if (!bChanged)
			{
				break;
			}
		}
	}

	static TArray<TArray<int32>> BuildLayersForComponent(const TArray<FBlueprintLayoutNode>& Nodes, const TArray<int32>& Component)
	{
		int32 MaxRank = 0;
		for (const int32 NodeIndex : Component)
		{
			if (Nodes.IsValidIndex(NodeIndex))
			{
				MaxRank = FMath::Max(MaxRank, Nodes[NodeIndex].Rank);
			}
		}
		TArray<TArray<int32>> Layers;
		Layers.SetNum(MaxRank + 1);
		for (const int32 NodeIndex : Component)
		{
			if (Nodes.IsValidIndex(NodeIndex))
			{
				Layers[Nodes[NodeIndex].Rank].Add(NodeIndex);
			}
		}
		for (TArray<int32>& Layer : Layers)
		{
			Layer.Sort([&Nodes](const int32 A, const int32 B)
			{
				return CompareStableNodeIdentity(Nodes, A, B);
			});
		}
		return Layers;
	}

	static void MinimizeCrossingsWithPortConstraints(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		TArray<TArray<int32>>& Layers,
		const int32 MaxIterations)
	{
		for (TArray<int32>& Layer : Layers)
		{
			UpdateLayerOrders(Nodes, Layer);
		}
		for (int32 Iteration = 0; Iteration < FMath::Max(1, MaxIterations); ++Iteration)
		{
			for (int32 LayerIndex = 1; LayerIndex < Layers.Num(); ++LayerIndex)
			{
				SortLayerByNeighborMedian(Nodes, Edges, IncomingEdges, OutgoingEdges, Layers[LayerIndex], true);
				RefineLayerByAdjacentTranspose(Nodes, Edges, Layers, LayerIndex);
			}
			for (int32 LayerIndex = Layers.Num() - 2; LayerIndex >= 0; --LayerIndex)
			{
				SortLayerByNeighborMedian(Nodes, Edges, IncomingEdges, OutgoingEdges, Layers[LayerIndex], false);
				RefineLayerByAdjacentTranspose(Nodes, Edges, Layers, LayerIndex);
			}
		}
	}

	static void PlaceLayerByDesiredY(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<int32>& Layer,
		const TArray<double>& DesiredTops,
		const double MinimumTop,
		const double NodePadding)
	{
		double CursorY = MinimumTop;
		for (int32 Index = 0; Index < Layer.Num(); ++Index)
		{
			const int32 NodeIndex = Layer[Index];
			if (!Nodes.IsValidIndex(NodeIndex))
			{
				continue;
			}
			const double DesiredTop = DesiredTops.IsValidIndex(Index) ? DesiredTops[Index] : CursorY;
			Nodes[NodeIndex].NewY = FMath::Max(CursorY, DesiredTop);
			CursorY = Nodes[NodeIndex].NewY + Nodes[NodeIndex].Height + NodePadding;
		}
	}

	static double ComputeDesiredTopFromPorts(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<int32>& IncidentEdges,
		const int32 NodeIndex)
	{
		TArray<double> DesiredTops;
		for (const int32 EdgeIndex : IncidentEdges)
		{
			if (!Edges.IsValidIndex(EdgeIndex))
			{
				continue;
			}
			const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
			if (Edge.Source == NodeIndex && Nodes.IsValidIndex(Edge.Target))
			{
				const double TargetPinY = GetEdgeTargetPoint(Nodes, Edge, true).Y;
				DesiredTops.Add(TargetPinY - GetPinYOffset(Nodes[NodeIndex], EGPD_Output, Edge.SourcePinIndex));
				if (Edge.bExec)
				{
					DesiredTops.Add(TargetPinY - GetPinYOffset(Nodes[NodeIndex], EGPD_Output, Edge.SourcePinIndex));
				}
			}
			else if (Edge.Target == NodeIndex && Nodes.IsValidIndex(Edge.Source))
			{
				const double SourcePinY = GetEdgeSourcePoint(Nodes, Edge, true).Y;
				DesiredTops.Add(SourcePinY - GetPinYOffset(Nodes[NodeIndex], EGPD_Input, Edge.TargetPinIndex));
				if (Edge.bExec)
				{
					DesiredTops.Add(SourcePinY - GetPinYOffset(Nodes[NodeIndex], EGPD_Input, Edge.TargetPinIndex));
				}
			}
		}
		return DesiredTops.Num() > 0 ? MedianValue(DesiredTops) : Nodes[NodeIndex].NewY;
	}

	static TArray<int32> CollectIncidentEdgesForPinAlignment(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const int32 NodeIndex)
	{
		TArray<int32> AllIncidentEdges;
		TArray<int32> ExecIncidentEdges;
		auto AppendEdges = [&](const TArray<int32>& EdgeIndices)
		{
			for (const int32 EdgeIndex : EdgeIndices)
			{
				if (!Edges.IsValidIndex(EdgeIndex))
				{
					continue;
				}
				AllIncidentEdges.Add(EdgeIndex);
				if (Edges[EdgeIndex].bExec)
				{
					ExecIncidentEdges.Add(EdgeIndex);
				}
			}
		};
		if (IncomingEdges.IsValidIndex(NodeIndex))
		{
			AppendEdges(IncomingEdges[NodeIndex]);
		}
		if (OutgoingEdges.IsValidIndex(NodeIndex))
		{
			AppendEdges(OutgoingEdges[NodeIndex]);
		}
		if (Nodes.IsValidIndex(NodeIndex) && NodeHasExecPin(Nodes[NodeIndex]) && ExecIncidentEdges.Num() > 0)
		{
			return ExecIncidentEdges;
		}
		return AllIncidentEdges;
	}

	static void PlaceComponentLayers(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const TArray<TArray<int32>>& Layers,
		const double OriginX,
		const double OriginY,
		const double HorizontalSpacing,
		const double NodePadding,
		int32& OutRankCount)
	{
		TArray<double> ColumnWidths;
		ColumnWidths.Init(0.0, Layers.Num());
		for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); ++LayerIndex)
		{
			for (const int32 NodeIndex : Layers[LayerIndex])
			{
				if (Nodes.IsValidIndex(NodeIndex))
				{
					ColumnWidths[LayerIndex] = FMath::Max(ColumnWidths[LayerIndex], Nodes[NodeIndex].Width);
				}
			}
		}

		double ColumnX = OriginX;
		for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); ++LayerIndex)
		{
			for (const int32 NodeIndex : Layers[LayerIndex])
			{
				if (Nodes.IsValidIndex(NodeIndex))
				{
					Nodes[NodeIndex].NewX = ColumnX;
				}
			}
			ColumnX += FMath::Max(1.0, ColumnWidths[LayerIndex]) + HorizontalSpacing;
		}

		for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); ++LayerIndex)
		{
			double CursorY = OriginY;
			for (const int32 NodeIndex : Layers[LayerIndex])
			{
				if (Nodes.IsValidIndex(NodeIndex))
				{
					Nodes[NodeIndex].NewY = CursorY;
					CursorY += Nodes[NodeIndex].Height + NodePadding;
				}
			}
		}

		for (int32 Pass = 0; Pass < 10; ++Pass)
		{
			for (int32 Direction = 0; Direction < 2; ++Direction)
			{
				const bool bForward = Direction == 0;
				for (int32 LayerLoop = 0; LayerLoop < Layers.Num(); ++LayerLoop)
				{
					const int32 LayerIndex = bForward ? LayerLoop : Layers.Num() - 1 - LayerLoop;
					TArray<double> DesiredTops;
					for (const int32 NodeIndex : Layers[LayerIndex])
					{
						const TArray<int32> IncidentEdges = CollectIncidentEdgesForPinAlignment(
							Nodes,
							Edges,
							IncomingEdges,
							OutgoingEdges,
							NodeIndex);
						DesiredTops.Add(ComputeDesiredTopFromPorts(Nodes, Edges, IncidentEdges, NodeIndex));
					}
					PlaceLayerByDesiredY(Nodes, Layers[LayerIndex], DesiredTops, OriginY, NodePadding);
				}
			}
		}
		OutRankCount = FMath::Max(OutRankCount, Layers.Num());
	}

	static int32 ResolveGlobalOverlaps(TArray<FBlueprintLayoutNode>& Nodes, const double NodePadding);

	static TSet<int32> FindFinalLogicConsumers(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const int32 StartNode)
	{
		TSet<int32> Consumers;
		TSet<int32> Visited;
		TArray<int32> Stack;
		Stack.Add(StartNode);
		while (Stack.Num() > 0)
		{
			const int32 NodeIndex = Stack.Pop(EAllowShrinking::No);
			if (!Nodes.IsValidIndex(NodeIndex) || Visited.Contains(NodeIndex))
			{
				continue;
			}
			Visited.Add(NodeIndex);
			if (NodeIndex != StartNode && NodeHasExecPin(Nodes[NodeIndex]))
			{
				Consumers.Add(NodeIndex);
				continue;
			}
			if (!OutgoingEdges.IsValidIndex(NodeIndex))
			{
				continue;
			}
			for (const int32 EdgeIndex : OutgoingEdges[NodeIndex])
			{
				if (!Edges.IsValidIndex(EdgeIndex) || Edges[EdgeIndex].bExec)
				{
					continue;
				}
				Stack.Add(Edges[EdgeIndex].Target);
			}
		}
		return Consumers;
	}

	static void DockSingleConsumerDataSubgraphs(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const double NodePadding,
		const bool bAllowPureTargets = false)
	{
		TMap<int32, TArray<int32>> IncomingDockEdgesByTarget;
		TMap<int32, TSet<int32>> CachedConsumerSets;
		auto GetConsumers = [&](const int32 NodeIndex) -> TSet<int32>
		{
			if (const TSet<int32>* Existing = CachedConsumerSets.Find(NodeIndex))
			{
				return *Existing;
			}
			TSet<int32> Consumers = FindFinalLogicConsumers(Nodes, Edges, OutgoingEdges, NodeIndex);
			CachedConsumerSets.Add(NodeIndex, Consumers);
			return Consumers;
		};
		auto CountDirectDataConsumers = [&](const int32 NodeIndex) -> int32
		{
			if (!OutgoingEdges.IsValidIndex(NodeIndex))
			{
				return 0;
			}
			int32 Count = 0;
			for (const int32 EdgeIndex : OutgoingEdges[NodeIndex])
			{
				if (Edges.IsValidIndex(EdgeIndex)
					&& !Edges[EdgeIndex].bExec
					&& Nodes.IsValidIndex(Edges[EdgeIndex].Target)
					&& !Nodes[Edges[EdgeIndex].Target].bRerouteNode)
				{
					++Count;
				}
			}
			return Count;
		};

		for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
		{
			const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
			if (Edge.bExec
				|| !Nodes.IsValidIndex(Edge.Source)
				|| !Nodes.IsValidIndex(Edge.Target)
				|| Nodes[Edge.Source].bRerouteNode
				|| Nodes[Edge.Target].bRerouteNode
				|| NodeHasExecPin(Nodes[Edge.Source])
				|| !IsCompactBlueprintValueNode(Nodes[Edge.Source].Node))
			{
				continue;
			}

			const bool bTargetIsFinalLogicConsumer = NodeHasExecPin(Nodes[Edge.Target]);
			if (!bTargetIsFinalLogicConsumer && !bAllowPureTargets)
			{
				continue;
			}
			const TSet<int32> SourceConsumers = GetConsumers(Edge.Source);
			if (bTargetIsFinalLogicConsumer)
			{
				if (SourceConsumers.Num() != 1 || !SourceConsumers.Contains(Edge.Target))
				{
					continue;
				}
			}
			else if (CountDirectDataConsumers(Edge.Source) != 1)
			{
				continue;
			}

			IncomingDockEdgesByTarget.FindOrAdd(Edge.Target).Add(EdgeIndex);
		}

		for (int32 Pass = 0; Pass < 4; ++Pass)
		{
			for (TPair<int32, TArray<int32>>& Pair : IncomingDockEdgesByTarget)
			{
				const int32 TargetIndex = Pair.Key;
				if (!Nodes.IsValidIndex(TargetIndex))
				{
					continue;
				}
				TArray<int32> DockEdges = Pair.Value;
				DockEdges.Sort([&](const int32 A, const int32 B)
				{
					const FBlueprintLayoutEdge& EdgeA = Edges[A];
					const FBlueprintLayoutEdge& EdgeB = Edges[B];
					if (EdgeA.TargetPinIndex != EdgeB.TargetPinIndex)
					{
						return EdgeA.TargetPinIndex < EdgeB.TargetPinIndex;
					}
					return CompareStableNodeIdentity(Nodes, EdgeA.Source, EdgeB.Source);
				});

				TSet<int32> MovedSources;
				struct FSidecarPlacement
				{
					int32 SourceIndex = INDEX_NONE;
					int32 EdgeIndex = INDEX_NONE;
					double DesiredY = 0.0;
					double NewX = 0.0;
				};
				TArray<FSidecarPlacement> Placements;
				for (const int32 EdgeIndex : DockEdges)
				{
					if (!Edges.IsValidIndex(EdgeIndex))
					{
						continue;
					}
					const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
					if (!Nodes.IsValidIndex(Edge.Source) || MovedSources.Contains(Edge.Source))
					{
						continue;
					}
					const FBlueprintLayoutNode& SourceNode = Nodes[Edge.Source];
					const FBlueprintLayoutNode& TargetNode = Nodes[TargetIndex];
					const double LocalGap = NodeHasExecPin(TargetNode) ? 92.0 : 72.0;
					const double DesiredX = TargetNode.NewX - SourceNode.Width - LocalGap;
					const double DesiredY = GetEdgeTargetPoint(Nodes, Edge, true).Y
						- GetPinYOffset(SourceNode, EGPD_Output, Edge.SourcePinIndex);
					FSidecarPlacement Placement;
					Placement.SourceIndex = Edge.Source;
					Placement.EdgeIndex = EdgeIndex;
					Placement.DesiredY = DesiredY;
					Placement.NewX = DesiredX;
					Placements.Add(Placement);
					MovedSources.Add(Edge.Source);
				}
				double CursorY = -TNumericLimits<double>::Max();
				for (FSidecarPlacement& Placement : Placements)
				{
					if (!Nodes.IsValidIndex(Placement.SourceIndex))
					{
						continue;
					}
					FBlueprintLayoutNode& SourceNode = Nodes[Placement.SourceIndex];
					SourceNode.NewX = Placement.NewX;
					if (CursorY > -TNumericLimits<double>::Max() * 0.5)
					{
						SourceNode.NewY = FMath::Max(Placement.DesiredY, CursorY);
					}
					else
					{
						SourceNode.NewY = Placement.DesiredY;
					}
					CursorY = SourceNode.NewY + SourceNode.Height + FMath::Max(18.0, NodePadding * 0.75);
				}
			}
			ResolveGlobalOverlaps(Nodes, NodePadding);
		}
	}

	static void ResolveColumnOverlaps(TArray<FBlueprintLayoutNode>& Nodes, const double NodePadding)
	{
		TMap<int32, TArray<int32>> Columns;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (!Nodes.IsValidIndex(NodeIndex) || Nodes[NodeIndex].bRerouteNode)
			{
				continue;
			}
			const int32 ColumnKey = FMath::RoundToInt(Nodes[NodeIndex].NewX / 16.0);
			Columns.FindOrAdd(ColumnKey).Add(NodeIndex);
		}
		for (TPair<int32, TArray<int32>>& Pair : Columns)
		{
			TArray<int32>& ColumnNodes = Pair.Value;
			ColumnNodes.Sort([&Nodes](const int32 A, const int32 B)
			{
				if (!FMath::IsNearlyEqual(Nodes[A].NewY, Nodes[B].NewY, 1.0))
				{
					return Nodes[A].NewY < Nodes[B].NewY;
				}
				return CompareStableNodeIdentity(Nodes, A, B);
			});
			double CursorY = -TNumericLimits<double>::Max();
			for (const int32 NodeIndex : ColumnNodes)
			{
				if (!Nodes.IsValidIndex(NodeIndex))
				{
					continue;
				}
				if (CursorY > -TNumericLimits<double>::Max() * 0.5)
				{
					Nodes[NodeIndex].NewY = FMath::Max(Nodes[NodeIndex].NewY, CursorY);
				}
				CursorY = Nodes[NodeIndex].NewY + Nodes[NodeIndex].Height + NodePadding;
			}
		}
	}

	static int32 ResolveGlobalOverlaps(TArray<FBlueprintLayoutNode>& Nodes, const double NodePadding)
	{
		TArray<int32> OrderedNodes;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (!Nodes.IsValidIndex(NodeIndex) || Nodes[NodeIndex].bRerouteNode)
			{
				continue;
			}
			OrderedNodes.Add(NodeIndex);
		}
		OrderedNodes.Sort([&Nodes](const int32 A, const int32 B)
		{
			if (!FMath::IsNearlyEqual(Nodes[A].NewY, Nodes[B].NewY, 1.0))
			{
				return Nodes[A].NewY < Nodes[B].NewY;
			}
			if (!FMath::IsNearlyEqual(Nodes[A].NewX, Nodes[B].NewX, 1.0))
			{
				return Nodes[A].NewX < Nodes[B].NewX;
			}
			return CompareStableNodeIdentity(Nodes, A, B);
		});

		int32 MoveCount = 0;
		for (int32 Index = 0; Index < OrderedNodes.Num(); ++Index)
		{
			const int32 NodeIndex = OrderedNodes[Index];
			if (!Nodes.IsValidIndex(NodeIndex))
			{
				continue;
			}
			bool bMovedThisNode = true;
			int32 Guard = 0;
			while (bMovedThisNode && Guard++ < OrderedNodes.Num())
			{
				bMovedThisNode = false;
				FBlueprintLayoutRect NodeRect = MakeNodeRect(Nodes[NodeIndex], true);
				for (int32 PrevIndex = 0; PrevIndex < Index; ++PrevIndex)
				{
					const int32 OtherIndex = OrderedNodes[PrevIndex];
					if (!Nodes.IsValidIndex(OtherIndex))
					{
						continue;
					}
					const FBlueprintLayoutRect OtherRect = MakeNodeRect(Nodes[OtherIndex], true);
					if (NodeRect.Intersects(OtherRect, NodePadding))
					{
						Nodes[NodeIndex].NewY = OtherRect.Bottom + NodePadding;
						NodeRect = MakeNodeRect(Nodes[NodeIndex], true);
						bMovedThisNode = true;
						++MoveCount;
					}
				}
			}
		}
		return MoveCount;
	}

	static int32 ResolveNonExecOverlapsPreservingExec(TArray<FBlueprintLayoutNode>& Nodes, const double NodePadding)
	{
		int32 MoveCount = 0;
		auto IsMovable = [&Nodes](const int32 NodeIndex)
		{
			return Nodes.IsValidIndex(NodeIndex)
				&& !Nodes[NodeIndex].bRerouteNode
				&& !NodeHasExecPin(Nodes[NodeIndex]);
		};
		auto MeasureOverlapPenaltyAtY = [&Nodes, NodePadding](const int32 MovingNodeIndex, const double CandidateY)
		{
			if (!Nodes.IsValidIndex(MovingNodeIndex))
			{
				return TNumericLimits<double>::Max();
			}

			FBlueprintLayoutNode CandidateNode = Nodes[MovingNodeIndex];
			CandidateNode.NewY = CandidateY;
			const FBlueprintLayoutRect CandidateRect = MakeNodeRect(CandidateNode, true);
			double Penalty = 0.0;
			for (int32 OtherIndex = 0; OtherIndex < Nodes.Num(); ++OtherIndex)
			{
				if (OtherIndex == MovingNodeIndex
					|| !Nodes.IsValidIndex(OtherIndex)
					|| Nodes[OtherIndex].bRerouteNode)
				{
					continue;
				}
				const FBlueprintLayoutRect OtherRect = MakeNodeRect(Nodes[OtherIndex], true);
				if (!CandidateRect.Intersects(OtherRect, NodePadding))
				{
					continue;
				}

				const double OverlapWidth = FMath::Max(
					0.0,
					FMath::Min(CandidateRect.Right + NodePadding, OtherRect.Right + NodePadding)
					- FMath::Max(CandidateRect.Left, OtherRect.Left));
				const double OverlapHeight = FMath::Max(
					0.0,
					FMath::Min(CandidateRect.Bottom + NodePadding, OtherRect.Bottom + NodePadding)
					- FMath::Max(CandidateRect.Top, OtherRect.Top));
				Penalty += 1000000.0 + OverlapWidth * OverlapHeight;
			}
			return Penalty;
		};
		auto AddCandidateY = [](TArray<double>& CandidateYs, const double CandidateY)
		{
			if (!FMath::IsFinite(CandidateY))
			{
				return;
			}
			for (const double ExistingY : CandidateYs)
			{
				if (FMath::IsNearlyEqual(ExistingY, CandidateY, 0.5))
				{
					return;
				}
			}
			CandidateYs.Add(CandidateY);
		};
		auto FindBestResolvedY = [&Nodes, NodePadding, &MeasureOverlapPenaltyAtY, &AddCandidateY](
			const int32 MovingNodeIndex,
			const int32 FixedNodeIndex,
			double& OutBestY)
		{
			if (!Nodes.IsValidIndex(MovingNodeIndex) || !Nodes.IsValidIndex(FixedNodeIndex))
			{
				return false;
			}

			TArray<double> CandidateYs;
			const double CurrentY = Nodes[MovingNodeIndex].NewY;
			AddCandidateY(CandidateYs, CurrentY);

			const FBlueprintLayoutRect MovingRect = MakeNodeRect(Nodes[MovingNodeIndex], true);
			const FBlueprintLayoutRect FixedRect = MakeNodeRect(Nodes[FixedNodeIndex], true);
			AddCandidateY(CandidateYs, FixedRect.Top - Nodes[MovingNodeIndex].Height - NodePadding);
			AddCandidateY(CandidateYs, FixedRect.Bottom + NodePadding);

			for (int32 OtherIndex = 0; OtherIndex < Nodes.Num(); ++OtherIndex)
			{
				if (OtherIndex == MovingNodeIndex
					|| !Nodes.IsValidIndex(OtherIndex)
					|| Nodes[OtherIndex].bRerouteNode)
				{
					continue;
				}
				const FBlueprintLayoutRect OtherRect = MakeNodeRect(Nodes[OtherIndex], true);
				const bool bXCouldOverlap = !(MovingRect.Right + NodePadding <= OtherRect.Left + 0.5
					|| OtherRect.Right + NodePadding <= MovingRect.Left + 0.5);
				if (!bXCouldOverlap)
				{
					continue;
				}
				AddCandidateY(CandidateYs, OtherRect.Top - Nodes[MovingNodeIndex].Height - NodePadding);
				AddCandidateY(CandidateYs, OtherRect.Bottom + NodePadding);
			}

			const double CurrentPenalty = MeasureOverlapPenaltyAtY(MovingNodeIndex, CurrentY);
			double BestY = CurrentY;
			double BestPenalty = CurrentPenalty;
			double BestScore = CurrentPenalty;
			const double PreferredSideY = ((MovingRect.Top + MovingRect.Bottom) * 0.5 < (FixedRect.Top + FixedRect.Bottom) * 0.5)
				? FixedRect.Top - Nodes[MovingNodeIndex].Height - NodePadding
				: FixedRect.Bottom + NodePadding;
			for (const double CandidateY : CandidateYs)
			{
				const double Penalty = MeasureOverlapPenaltyAtY(MovingNodeIndex, CandidateY);
				const double Score = Penalty
					+ FMath::Abs(CandidateY - CurrentY) * 0.01
					+ FMath::Abs(CandidateY - PreferredSideY) * 0.001;
				if (Score < BestScore - 0.5
					|| (Penalty < BestPenalty - 0.5 && FMath::IsNearlyEqual(Score, BestScore, 0.5)))
				{
					BestScore = Score;
					BestPenalty = Penalty;
					BestY = CandidateY;
				}
			}

			if (BestPenalty < CurrentPenalty - 0.5
				&& !FMath::IsNearlyEqual(BestY, CurrentY, 0.5))
			{
				OutBestY = BestY;
				return true;
			}
			return false;
		};
		for (int32 Pass = 0; Pass < 12; ++Pass)
		{
			bool bMovedThisPass = false;
			for (int32 A = 0; A < Nodes.Num(); ++A)
			{
				if (!Nodes.IsValidIndex(A) || Nodes[A].bRerouteNode)
				{
					continue;
				}
				for (int32 B = A + 1; B < Nodes.Num(); ++B)
				{
					if (!Nodes.IsValidIndex(B) || Nodes[B].bRerouteNode)
					{
						continue;
					}
					const FBlueprintLayoutRect RectA = MakeNodeRect(Nodes[A], true);
					const FBlueprintLayoutRect RectB = MakeNodeRect(Nodes[B], true);
					if (!RectA.Intersects(RectB, NodePadding))
					{
						continue;
					}

					const bool bAMovable = IsMovable(A);
					const bool bBMovable = IsMovable(B);
					if (!bAMovable && !bBMovable)
					{
						continue;
					}

					int32 MoveIndex = INDEX_NONE;
					int32 FixedIndex = INDEX_NONE;
					if (bAMovable && !bBMovable)
					{
						MoveIndex = A;
						FixedIndex = B;
					}
					else if (!bAMovable && bBMovable)
					{
						MoveIndex = B;
						FixedIndex = A;
					}
					else
					{
						MoveIndex = RectA.Top <= RectB.Top ? B : A;
						FixedIndex = RectA.Top <= RectB.Top ? A : B;
					}

					if (!Nodes.IsValidIndex(MoveIndex) || !Nodes.IsValidIndex(FixedIndex))
					{
						continue;
					}
					double DesiredY = Nodes[MoveIndex].NewY;
					if (!FindBestResolvedY(MoveIndex, FixedIndex, DesiredY))
					{
						continue;
					}
					if (!FMath::IsNearlyEqual(Nodes[MoveIndex].NewY, DesiredY, 0.5))
					{
						Nodes[MoveIndex].NewY = DesiredY;
						++MoveCount;
						bMovedThisPass = true;
					}
				}
			}
			if (!bMovedThisPass)
			{
				break;
			}
		}
		return MoveCount;
	}

	static int32 ApplyTopDownPinAlignedCompaction(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const double HorizontalSpacing,
		const double NodePadding)
	{
		struct FPinAlignedColumn
		{
			int32 Component = INDEX_NONE;
			double Left = 0.0;
			double Right = 0.0;
			TArray<int32> NodeIndices;
		};

		TArray<FPinAlignedColumn> Columns;
		TArray<int32> SortableNodes;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (Nodes.IsValidIndex(NodeIndex) && !Nodes[NodeIndex].bRerouteNode && !NodeHasExecPin(Nodes[NodeIndex]))
			{
				SortableNodes.Add(NodeIndex);
			}
		}
		SortableNodes.Sort([&Nodes](const int32 A, const int32 B)
		{
			if (Nodes[A].Component != Nodes[B].Component)
			{
				return Nodes[A].Component < Nodes[B].Component;
			}
			if (!FMath::IsNearlyEqual(Nodes[A].NewX, Nodes[B].NewX, 1.0))
			{
				return Nodes[A].NewX < Nodes[B].NewX;
			}
			return CompareStableNodeIdentity(Nodes, A, B);
		});

		const double LanePadding = FMath::Max(8.0, NodePadding * 0.35);
		for (const int32 NodeIndex : SortableNodes)
		{
			const double NodeLeft = Nodes[NodeIndex].NewX - LanePadding;
			const double NodeRight = Nodes[NodeIndex].NewX + Nodes[NodeIndex].Width + LanePadding;
			int32 ColumnIndex = INDEX_NONE;
			for (int32 ExistingIndex = 0; ExistingIndex < Columns.Num(); ++ExistingIndex)
			{
				FPinAlignedColumn& Column = Columns[ExistingIndex];
				if (Column.Component == Nodes[NodeIndex].Component
					&& NodeLeft <= Column.Right
					&& NodeRight >= Column.Left)
				{
					ColumnIndex = ExistingIndex;
					break;
				}
			}
			if (ColumnIndex == INDEX_NONE)
			{
				FPinAlignedColumn Column;
				Column.Component = Nodes[NodeIndex].Component;
				Column.Left = NodeLeft;
				Column.Right = NodeRight;
				Column.NodeIndices.Add(NodeIndex);
				Columns.Add(Column);
				continue;
			}

			Columns[ColumnIndex].Left = FMath::Min(Columns[ColumnIndex].Left, NodeLeft);
			Columns[ColumnIndex].Right = FMath::Max(Columns[ColumnIndex].Right, NodeRight);
			Columns[ColumnIndex].NodeIndices.Add(NodeIndex);
			bool bMergedColumn = true;
			while (bMergedColumn)
			{
				bMergedColumn = false;
				for (int32 OtherIndex = Columns.Num() - 1; OtherIndex >= 0; --OtherIndex)
				{
					if (OtherIndex == ColumnIndex
						|| Columns[OtherIndex].Component != Columns[ColumnIndex].Component
						|| Columns[OtherIndex].Left > Columns[ColumnIndex].Right
						|| Columns[OtherIndex].Right < Columns[ColumnIndex].Left)
					{
						continue;
					}
					Columns[ColumnIndex].Left = FMath::Min(Columns[ColumnIndex].Left, Columns[OtherIndex].Left);
					Columns[ColumnIndex].Right = FMath::Max(Columns[ColumnIndex].Right, Columns[OtherIndex].Right);
					Columns[ColumnIndex].NodeIndices.Append(Columns[OtherIndex].NodeIndices);
					Columns.RemoveAt(OtherIndex, 1, EAllowShrinking::No);
					if (OtherIndex < ColumnIndex)
					{
						--ColumnIndex;
					}
					bMergedColumn = true;
					break;
				}
			}
		}

		int32 MoveCount = 0;
		const double MaxSinglePassShift = FMath::Clamp(HorizontalSpacing * 0.32, 72.0, 120.0);
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			bool bMovedThisPass = false;
			for (FPinAlignedColumn& Column : Columns)
			{
				if (Column.NodeIndices.Num() == 0)
				{
					continue;
				}

				TMap<int32, double> DesiredTopByNode;
				for (const int32 NodeIndex : Column.NodeIndices)
				{
					if (!Nodes.IsValidIndex(NodeIndex))
					{
						continue;
					}
					const TArray<int32> IncidentEdges = CollectIncidentEdgesForPinAlignment(
						Nodes,
						Edges,
						IncomingEdges,
						OutgoingEdges,
						NodeIndex);
					const double DesiredTop = IncidentEdges.Num() > 0
						? ComputeDesiredTopFromPorts(Nodes, Edges, IncidentEdges, NodeIndex)
						: Nodes[NodeIndex].NewY;
					DesiredTopByNode.Add(NodeIndex, DesiredTop);
				}

				auto GetDesiredTop = [&DesiredTopByNode, &Nodes](const int32 NodeIndex)
				{
					if (const double* FoundDesiredTop = DesiredTopByNode.Find(NodeIndex))
					{
						return *FoundDesiredTop;
					}
					return Nodes.IsValidIndex(NodeIndex) ? Nodes[NodeIndex].NewY : 0.0;
				};

				auto GetClampedDesiredTop = [&GetDesiredTop, &Nodes, MaxSinglePassShift](const int32 NodeIndex)
				{
					if (!Nodes.IsValidIndex(NodeIndex))
					{
						return 0.0;
					}
					return FMath::Clamp(
						GetDesiredTop(NodeIndex),
						Nodes[NodeIndex].NewY - MaxSinglePassShift,
						Nodes[NodeIndex].NewY + MaxSinglePassShift);
				};

				Column.NodeIndices.Sort([&Nodes, &GetDesiredTop](const int32 A, const int32 B)
				{
					const double DesiredTopA = GetDesiredTop(A);
					const double DesiredTopB = GetDesiredTop(B);
					if (!FMath::IsNearlyEqual(DesiredTopA, DesiredTopB, 1.0))
					{
						return DesiredTopA < DesiredTopB;
					}
					if (!FMath::IsNearlyEqual(Nodes[A].NewY, Nodes[B].NewY, 1.0))
					{
						return Nodes[A].NewY < Nodes[B].NewY;
					}
					if (Nodes[A].Rank != Nodes[B].Rank)
					{
						return Nodes[A].Rank < Nodes[B].Rank;
					}
					if (Nodes[A].Order != Nodes[B].Order)
					{
						return Nodes[A].Order < Nodes[B].Order;
					}
					return CompareStableNodeIdentity(Nodes, A, B);
				});

				double CursorY = GetClampedDesiredTop(Column.NodeIndices[0]);
				for (const int32 NodeIndex : Column.NodeIndices)
				{
					if (!Nodes.IsValidIndex(NodeIndex))
					{
						continue;
					}
					const double DesiredTop = GetClampedDesiredTop(NodeIndex);
					const double NewTop = FMath::Max(CursorY, DesiredTop);
					if (!FMath::IsNearlyEqual(NewTop, Nodes[NodeIndex].NewY, 0.5))
					{
						Nodes[NodeIndex].NewY = NewTop;
						++MoveCount;
						bMovedThisPass = true;
					}
					CursorY = Nodes[NodeIndex].NewY + Nodes[NodeIndex].Height + NodePadding;
				}
			}
			if (!bMovedThisPass)
			{
				break;
			}
		}
		return MoveCount;
	}

	static bool WouldNodeOverlapAt(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const int32 NodeIndex,
		const double CandidateX,
		const double CandidateY,
		const double NodePadding)
	{
		if (!Nodes.IsValidIndex(NodeIndex))
		{
			return true;
		}
		FBlueprintLayoutNode CandidateNode = Nodes[NodeIndex];
		CandidateNode.NewX = CandidateX;
		CandidateNode.NewY = CandidateY;
		const FBlueprintLayoutRect CandidateRect = MakeNodeRect(CandidateNode, true);
		for (int32 OtherIndex = 0; OtherIndex < Nodes.Num(); ++OtherIndex)
		{
			if (OtherIndex == NodeIndex
				|| !Nodes.IsValidIndex(OtherIndex)
				|| Nodes[OtherIndex].bRerouteNode)
			{
				continue;
			}
			const FBlueprintLayoutRect OtherRect = MakeNodeRect(Nodes[OtherIndex], true);
			if (CandidateRect.Intersects(OtherRect, NodePadding))
			{
				return true;
			}
		}
		return false;
	}

	static bool IsMovableFinalAlignmentDataNode(const FBlueprintLayoutNode& Node)
	{
		if (Node.bRerouteNode || NodeHasExecPin(Node))
		{
			return false;
		}
		return IsCompactBlueprintValueNode(Node.Node)
			|| IsSmallPureBlueprintFunctionNode(Node.Node, Node.InputPinCount, Node.OutputPinCount);
	}

	static int32 CountNonExecOutgoingEdges(
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const int32 NodeIndex)
	{
		if (!OutgoingEdges.IsValidIndex(NodeIndex))
		{
			return 0;
		}
		int32 Count = 0;
		for (const int32 EdgeIndex : OutgoingEdges[NodeIndex])
		{
			if (Edges.IsValidIndex(EdgeIndex) && !Edges[EdgeIndex].bExec)
			{
				++Count;
			}
		}
		return Count;
	}

	static bool IsFinalSingleConsumerPinAlignmentSource(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const int32 NodeIndex,
		const int32 EdgeIndex)
	{
		if (!Nodes.IsValidIndex(NodeIndex)
			|| Nodes[NodeIndex].bRerouteNode
			|| NodeHasExecPin(Nodes[NodeIndex])
			|| !OutgoingEdges.IsValidIndex(NodeIndex)
			|| !Edges.IsValidIndex(EdgeIndex)
			|| Edges[EdgeIndex].bExec)
		{
			return false;
		}

		int32 NonExecOutgoingCount = 0;
		for (const int32 OutgoingEdgeIndex : OutgoingEdges[NodeIndex])
		{
			if (Edges.IsValidIndex(OutgoingEdgeIndex) && !Edges[OutgoingEdgeIndex].bExec)
			{
				++NonExecOutgoingCount;
				if (OutgoingEdgeIndex != EdgeIndex)
				{
					return false;
				}
			}
		}
		if (NonExecOutgoingCount != 1)
		{
			return false;
		}

		int32 NonExecIncomingCount = 0;
		if (IncomingEdges.IsValidIndex(NodeIndex))
		{
			for (const int32 IncomingEdgeIndex : IncomingEdges[NodeIndex])
			{
				if (Edges.IsValidIndex(IncomingEdgeIndex) && !Edges[IncomingEdgeIndex].bExec)
				{
					++NonExecIncomingCount;
				}
			}
		}

		const bool bCompactValue = IsCompactBlueprintValueNode(Nodes[NodeIndex].Node);
		const bool bSmallPureFunction = IsSmallPureBlueprintFunctionNode(
			Nodes[NodeIndex].Node,
			Nodes[NodeIndex].InputPinCount,
			Nodes[NodeIndex].OutputPinCount);
		if (!bCompactValue && !bSmallPureFunction)
		{
			return false;
		}
		return bCompactValue || NonExecIncomingCount <= 3;
	}

	static void CollectMovableFinalAlignmentDataIsland(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const int32 NodeIndex,
		TSet<int32>& Visited,
		TArray<int32>& OutNodeIndices)
	{
		if (!Nodes.IsValidIndex(NodeIndex)
			|| Visited.Contains(NodeIndex)
			|| OutNodeIndices.Num() >= 12
			|| !IsMovableFinalAlignmentDataNode(Nodes[NodeIndex]))
		{
			return;
		}

		Visited.Add(NodeIndex);
		OutNodeIndices.Add(NodeIndex);
		if (!IncomingEdges.IsValidIndex(NodeIndex))
		{
			return;
		}

		for (const int32 IncomingEdgeIndex : IncomingEdges[NodeIndex])
		{
			if (!Edges.IsValidIndex(IncomingEdgeIndex) || Edges[IncomingEdgeIndex].bExec)
			{
				continue;
			}
			const int32 SourceIndex = Edges[IncomingEdgeIndex].Source;
			if (!Nodes.IsValidIndex(SourceIndex)
				|| CountNonExecOutgoingEdges(Edges, OutgoingEdges, SourceIndex) != 1
				|| !IsMovableFinalAlignmentDataNode(Nodes[SourceIndex]))
			{
				continue;
			}
			CollectMovableFinalAlignmentDataIsland(
				Nodes,
				Edges,
				IncomingEdges,
				OutgoingEdges,
				SourceIndex,
				Visited,
				OutNodeIndices);
		}
	}

	static bool WouldMoveNodeGroupOverlapAtDelta(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<int32>& GroupNodeIndices,
		const double DeltaY,
		const double NodePadding)
	{
		TSet<int32> GroupSet;
		for (const int32 NodeIndex : GroupNodeIndices)
		{
			if (Nodes.IsValidIndex(NodeIndex))
			{
				GroupSet.Add(NodeIndex);
			}
		}
		if (GroupSet.Num() == 0)
		{
			return true;
		}

		for (const int32 NodeIndex : GroupSet)
		{
			FBlueprintLayoutNode CandidateNode = Nodes[NodeIndex];
			CandidateNode.NewY += DeltaY;
			const FBlueprintLayoutRect CandidateRect = MakeNodeRect(CandidateNode, true);
			for (int32 OtherIndex = 0; OtherIndex < Nodes.Num(); ++OtherIndex)
			{
				if (!Nodes.IsValidIndex(OtherIndex)
					|| Nodes[OtherIndex].bRerouteNode
					|| GroupSet.Contains(OtherIndex))
				{
					continue;
				}
				const FBlueprintLayoutRect OtherRect = MakeNodeRect(Nodes[OtherIndex], true);
				if (CandidateRect.Intersects(OtherRect, NodePadding))
				{
					return true;
				}
			}
		}
		return false;
	}

	static double MeasureMoveNodeGroupOverlapPenaltyAtDelta(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<int32>& GroupNodeIndices,
		const double DeltaX,
		const double DeltaY,
		const double NodePadding)
	{
		TSet<int32> GroupSet;
		for (const int32 NodeIndex : GroupNodeIndices)
		{
			if (Nodes.IsValidIndex(NodeIndex))
			{
				GroupSet.Add(NodeIndex);
			}
		}
		if (GroupSet.Num() == 0)
		{
			return TNumericLimits<double>::Max();
		}

		double Penalty = 0.0;
		for (const int32 NodeIndex : GroupSet)
		{
			FBlueprintLayoutNode CandidateNode = Nodes[NodeIndex];
			CandidateNode.NewX += DeltaX;
			CandidateNode.NewY += DeltaY;
			const FBlueprintLayoutRect CandidateRect = MakeNodeRect(CandidateNode, true);
			for (int32 OtherIndex = 0; OtherIndex < Nodes.Num(); ++OtherIndex)
			{
				if (!Nodes.IsValidIndex(OtherIndex) || Nodes[OtherIndex].bRerouteNode)
				{
					continue;
				}

				FBlueprintLayoutNode OtherNode = Nodes[OtherIndex];
				if (GroupSet.Contains(OtherIndex))
				{
					if (OtherIndex <= NodeIndex)
					{
						continue;
					}
					OtherNode.NewX += DeltaX;
					OtherNode.NewY += DeltaY;
				}
				const FBlueprintLayoutRect OtherRect = MakeNodeRect(OtherNode, true);
				if (!CandidateRect.Intersects(OtherRect, NodePadding))
				{
					continue;
				}

				const double OverlapWidth = FMath::Max(
					0.0,
					FMath::Min(CandidateRect.Right + NodePadding, OtherRect.Right + NodePadding)
					- FMath::Max(CandidateRect.Left, OtherRect.Left));
				const double OverlapHeight = FMath::Max(
					0.0,
					FMath::Min(CandidateRect.Bottom + NodePadding, OtherRect.Bottom + NodePadding)
					- FMath::Max(CandidateRect.Top, OtherRect.Top));
				Penalty += 1000000.0 + OverlapWidth * OverlapHeight;
			}
		}
		return Penalty;
	}

	static double MeasureMoveNodeGroupExecCorridorPenaltyAtDelta(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<int32>& GroupNodeIndices,
		const double DeltaX,
		const double DeltaY,
		const double NodePadding)
	{
		TSet<int32> GroupSet;
		for (const int32 NodeIndex : GroupNodeIndices)
		{
			if (Nodes.IsValidIndex(NodeIndex))
			{
				GroupSet.Add(NodeIndex);
			}
		}
		if (GroupSet.Num() == 0)
		{
			return TNumericLimits<double>::Max();
		}

		double Penalty = 0.0;
		for (const int32 NodeIndex : GroupSet)
		{
			FBlueprintLayoutNode CandidateNode = Nodes[NodeIndex];
			CandidateNode.NewX += DeltaX;
			CandidateNode.NewY += DeltaY;
			const FBlueprintLayoutRect CandidateRect = MakeNodeRect(CandidateNode, true);
			for (const FBlueprintLayoutEdge& Edge : Edges)
			{
				if (!Edge.bExec
					|| !Nodes.IsValidIndex(Edge.Source)
					|| !Nodes.IsValidIndex(Edge.Target)
					|| Nodes[Edge.Source].bRerouteNode
					|| Nodes[Edge.Target].bRerouteNode
					|| GroupSet.Contains(Edge.Source)
					|| GroupSet.Contains(Edge.Target))
				{
					continue;
				}

				const FVector2D SourcePoint = GetEdgeSourcePoint(Nodes, Edge, true);
				const FVector2D TargetPoint = GetEdgeTargetPoint(Nodes, Edge, true);
				const double CorridorLeft = FMath::Min(SourcePoint.X, TargetPoint.X);
				const double CorridorRight = FMath::Max(SourcePoint.X, TargetPoint.X);
				if (CorridorRight - CorridorLeft < 1.0)
				{
					continue;
				}

				const double CorridorTop = FMath::Min(SourcePoint.Y, TargetPoint.Y) - FMath::Max(12.0, NodePadding * 0.5);
				const double CorridorBottom = FMath::Max(SourcePoint.Y, TargetPoint.Y) + FMath::Max(12.0, NodePadding * 0.5);
				const bool bXIntersects = CandidateRect.Right + NodePadding > CorridorLeft
					&& CandidateRect.Left < CorridorRight + NodePadding;
				const bool bYIntersects = CandidateRect.Bottom + NodePadding > CorridorTop
					&& CandidateRect.Top < CorridorBottom + NodePadding;
				if (!bXIntersects || !bYIntersects)
				{
					continue;
				}

				const double OverlapWidth = FMath::Max(
					0.0,
					FMath::Min(CandidateRect.Right + NodePadding, CorridorRight + NodePadding)
					- FMath::Max(CandidateRect.Left, CorridorLeft));
				const double OverlapHeight = FMath::Max(
					0.0,
					FMath::Min(CandidateRect.Bottom + NodePadding, CorridorBottom + NodePadding)
					- FMath::Max(CandidateRect.Top, CorridorTop));
				Penalty += 2500.0 + OverlapWidth * OverlapHeight * 0.5;
			}
		}
		return Penalty;
	}

	static int32 ApplyFinalSingleConsumerPinAlignment(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const double NodePadding,
		int32* OutCandidateCount = nullptr)
	{
		if (OutCandidateCount)
		{
			*OutCandidateCount = 0;
		}
		TArray<int32> CandidateEdgeIndices;
		for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
		{
			if (!Edges.IsValidIndex(EdgeIndex)
				|| Edges[EdgeIndex].bExec
				|| !Nodes.IsValidIndex(Edges[EdgeIndex].Source)
				|| !Nodes.IsValidIndex(Edges[EdgeIndex].Target)
				|| Nodes[Edges[EdgeIndex].Target].bRerouteNode
				|| !IsFinalSingleConsumerPinAlignmentSource(Nodes, Edges, IncomingEdges, OutgoingEdges, Edges[EdgeIndex].Source, EdgeIndex))
			{
				continue;
			}
			CandidateEdgeIndices.Add(EdgeIndex);
		}
		if (OutCandidateCount)
		{
			*OutCandidateCount = CandidateEdgeIndices.Num();
		}

		CandidateEdgeIndices.Sort([&Nodes, &Edges](const int32 A, const int32 B)
		{
			const FBlueprintLayoutEdge& EdgeA = Edges[A];
			const FBlueprintLayoutEdge& EdgeB = Edges[B];
			const double TargetPinYA = GetEdgeTargetPoint(Nodes, EdgeA, true).Y;
			const double TargetPinYB = GetEdgeTargetPoint(Nodes, EdgeB, true).Y;
			if (!FMath::IsNearlyEqual(TargetPinYA, TargetPinYB, 1.0))
			{
				return TargetPinYA < TargetPinYB;
			}
			return CompareStableNodeIdentity(Nodes, EdgeA.Source, EdgeB.Source);
		});

		int32 MoveCount = 0;
		for (const int32 EdgeIndex : CandidateEdgeIndices)
		{
			if (!Edges.IsValidIndex(EdgeIndex))
			{
				continue;
			}
			const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
			if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
			{
				continue;
			}
			const double DesiredSourceTop = GetEdgeTargetPoint(Nodes, Edge, true).Y
				- GetPinYOffset(Nodes[Edge.Source], EGPD_Output, Edge.SourcePinIndex);
			const double DeltaY = DesiredSourceTop - Nodes[Edge.Source].NewY;
			if (FMath::IsNearlyEqual(DeltaY, 0.0, 0.5)
				|| FMath::Abs(DeltaY) > 520.0)
			{
				continue;
			}

			TArray<int32> MoveGroup;
			TSet<int32> VisitedGroupNodes;
			CollectMovableFinalAlignmentDataIsland(
				Nodes,
				Edges,
				IncomingEdges,
				OutgoingEdges,
				Edge.Source,
				VisitedGroupNodes,
				MoveGroup);
			if (MoveGroup.Num() == 0
				|| WouldMoveNodeGroupOverlapAtDelta(Nodes, MoveGroup, DeltaY, NodePadding))
			{
				continue;
			}
			for (const int32 NodeIndex : MoveGroup)
			{
				if (Nodes.IsValidIndex(NodeIndex))
				{
					Nodes[NodeIndex].NewY += DeltaY;
					++MoveCount;
				}
			}
		}
		return MoveCount;
	}

	static int32 EnforcePortOrderHardConstraints(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const double NodePadding)
	{
		int32 MoveCount = 0;
		auto EnforceAbove = [&Nodes, &MoveCount, NodePadding](
			const int32 UpperNodeIndex,
			const int32 LowerNodeIndex,
			const double UpperPinOffset,
			const double LowerPinOffset)
		{
			if (!Nodes.IsValidIndex(UpperNodeIndex)
				|| !Nodes.IsValidIndex(LowerNodeIndex)
				|| UpperNodeIndex == LowerNodeIndex
				|| Nodes[UpperNodeIndex].bRerouteNode
				|| Nodes[LowerNodeIndex].bRerouteNode)
			{
				return;
			}
			const double UpperPinY = Nodes[UpperNodeIndex].NewY + UpperPinOffset;
			const double LowerPinY = Nodes[LowerNodeIndex].NewY + LowerPinOffset;
			const double RequiredLowerPinY = UpperPinY + FMath::Max(24.0, NodePadding);
			if (LowerPinY < RequiredLowerPinY)
			{
				Nodes[LowerNodeIndex].NewY += RequiredLowerPinY - LowerPinY;
				++MoveCount;
			}
		};

		for (int32 A = 0; A < Edges.Num(); ++A)
		{
			if (!Nodes.IsValidIndex(Edges[A].Source) || !Nodes.IsValidIndex(Edges[A].Target))
			{
				continue;
			}
			for (int32 B = A + 1; B < Edges.Num(); ++B)
			{
				if (!Nodes.IsValidIndex(Edges[B].Source)
					|| !Nodes.IsValidIndex(Edges[B].Target)
					|| Edges[A].bExec != Edges[B].bExec)
				{
					continue;
				}
				if (Edges[A].Target == Edges[B].Target
					&& Edges[A].Source != Edges[B].Source
					&& Edges[A].TargetPinIndex != INDEX_NONE
					&& Edges[B].TargetPinIndex != INDEX_NONE
					&& Edges[A].TargetPinIndex != Edges[B].TargetPinIndex
					&& !HasConflictingSharedTargetOrder(Edges, Edges[A].Source, Edges[B].Source, Edges[A].bExec))
				{
					const bool bAAbove = Edges[A].TargetPinIndex < Edges[B].TargetPinIndex;
					const FBlueprintLayoutEdge& UpperEdge = bAAbove ? Edges[A] : Edges[B];
					const FBlueprintLayoutEdge& LowerEdge = bAAbove ? Edges[B] : Edges[A];
					EnforceAbove(
						UpperEdge.Source,
						LowerEdge.Source,
						GetPinYOffset(Nodes[UpperEdge.Source], EGPD_Output, UpperEdge.SourcePinIndex),
						GetPinYOffset(Nodes[LowerEdge.Source], EGPD_Output, LowerEdge.SourcePinIndex));
				}
				if (Edges[A].Source == Edges[B].Source
					&& Edges[A].Target != Edges[B].Target
					&& Edges[A].SourcePinIndex != INDEX_NONE
					&& Edges[B].SourcePinIndex != INDEX_NONE
					&& Edges[A].SourcePinIndex != Edges[B].SourcePinIndex
					&& !HasConflictingSharedSourceOrder(Edges, Edges[A].Target, Edges[B].Target, Edges[A].bExec))
				{
					const bool bAAbove = Edges[A].SourcePinIndex < Edges[B].SourcePinIndex;
					const FBlueprintLayoutEdge& UpperEdge = bAAbove ? Edges[A] : Edges[B];
					const FBlueprintLayoutEdge& LowerEdge = bAAbove ? Edges[B] : Edges[A];
					EnforceAbove(
						UpperEdge.Target,
						LowerEdge.Target,
						GetPinYOffset(Nodes[UpperEdge.Target], EGPD_Input, UpperEdge.TargetPinIndex),
						GetPinYOffset(Nodes[LowerEdge.Target], EGPD_Input, LowerEdge.TargetPinIndex));
				}
			}
		}
		ResolveColumnOverlaps(Nodes, NodePadding);
		ResolveGlobalOverlaps(Nodes, NodePadding);
		return MoveCount;
	}

	static int32 StraightenLinearExecChains(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const double NodePadding)
	{
		TArray<int32> ExecIncomingCount;
		TArray<int32> ExecOutgoingCount;
		TArray<int32> SingleExecIncomingEdge;
		TArray<int32> SingleExecOutgoingEdge;
		ExecIncomingCount.Init(0, Nodes.Num());
		ExecOutgoingCount.Init(0, Nodes.Num());
		SingleExecIncomingEdge.Init(INDEX_NONE, Nodes.Num());
		SingleExecOutgoingEdge.Init(INDEX_NONE, Nodes.Num());

		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (!Nodes.IsValidIndex(NodeIndex) || Nodes[NodeIndex].bRerouteNode || !NodeHasExecPin(Nodes[NodeIndex]))
			{
				continue;
			}
			if (IncomingEdges.IsValidIndex(NodeIndex))
			{
				for (const int32 EdgeIndex : IncomingEdges[NodeIndex])
				{
					if (Edges.IsValidIndex(EdgeIndex) && Edges[EdgeIndex].bExec)
					{
						SingleExecIncomingEdge[NodeIndex] = EdgeIndex;
						++ExecIncomingCount[NodeIndex];
					}
				}
			}
			if (OutgoingEdges.IsValidIndex(NodeIndex))
			{
				for (const int32 EdgeIndex : OutgoingEdges[NodeIndex])
				{
					if (Edges.IsValidIndex(EdgeIndex) && Edges[EdgeIndex].bExec)
					{
						SingleExecOutgoingEdge[NodeIndex] = EdgeIndex;
						++ExecOutgoingCount[NodeIndex];
					}
				}
			}
		}

		TArray<int32> StartNodes;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (!Nodes.IsValidIndex(NodeIndex)
				|| Nodes[NodeIndex].bRerouteNode
				|| !NodeHasExecPin(Nodes[NodeIndex]))
			{
				continue;
			}
			if (ExecOutgoingCount.IsValidIndex(NodeIndex) && ExecOutgoingCount[NodeIndex] > 1 && OutgoingEdges.IsValidIndex(NodeIndex))
			{
				for (const int32 EdgeIndex : OutgoingEdges[NodeIndex])
				{
					if (!Edges.IsValidIndex(EdgeIndex) || !Edges[EdgeIndex].bExec)
					{
						continue;
					}
					const int32 TargetIndex = Edges[EdgeIndex].Target;
					if (Nodes.IsValidIndex(TargetIndex) && NodeHasExecPin(Nodes[TargetIndex]))
					{
						StartNodes.AddUnique(TargetIndex);
					}
				}
				continue;
			}
			const int32 IncomingCount = ExecIncomingCount.IsValidIndex(NodeIndex) ? ExecIncomingCount[NodeIndex] : 0;
			const int32 OutgoingCount = ExecOutgoingCount.IsValidIndex(NodeIndex) ? ExecOutgoingCount[NodeIndex] : 0;
			if (OutgoingCount == 1 && IncomingCount != 1)
			{
				StartNodes.AddUnique(NodeIndex);
				continue;
			}
			if (IncomingCount == 1 && SingleExecIncomingEdge.IsValidIndex(NodeIndex) && Edges.IsValidIndex(SingleExecIncomingEdge[NodeIndex]))
			{
				const int32 SourceIndex = Edges[SingleExecIncomingEdge[NodeIndex]].Source;
				const int32 SourceOutgoingCount = ExecOutgoingCount.IsValidIndex(SourceIndex) ? ExecOutgoingCount[SourceIndex] : 0;
				if (SourceOutgoingCount != 1)
				{
					StartNodes.AddUnique(NodeIndex);
				}
			}
		}

		StartNodes.Sort([&Nodes](const int32 A, const int32 B)
		{
			if (Nodes[A].Rank != Nodes[B].Rank)
			{
				return Nodes[A].Rank < Nodes[B].Rank;
			}
			if (!FMath::IsNearlyEqual(Nodes[A].NewY, Nodes[B].NewY, 1.0))
			{
				return Nodes[A].NewY < Nodes[B].NewY;
			}
			return CompareStableNodeIdentity(Nodes, A, B);
		});

		TSet<int32> ClaimedNodes;
		int32 MoveCount = 0;
		for (const int32 StartNode : StartNodes)
		{
			if (!Nodes.IsValidIndex(StartNode) || ClaimedNodes.Contains(StartNode))
			{
				continue;
			}

			TArray<int32> ChainNodes;
			TArray<int32> IncomingExecEdgeForNode;
			TSet<int32> ChainSet;
			int32 Current = StartNode;
			while (Nodes.IsValidIndex(Current)
				&& NodeHasExecPin(Nodes[Current])
				&& !Nodes[Current].bRerouteNode
				&& !ClaimedNodes.Contains(Current)
				&& !ChainSet.Contains(Current))
			{
				ChainNodes.Add(Current);
				ChainSet.Add(Current);
				IncomingExecEdgeForNode.Add(SingleExecIncomingEdge.IsValidIndex(Current) ? SingleExecIncomingEdge[Current] : INDEX_NONE);

				if (!ExecOutgoingCount.IsValidIndex(Current)
					|| ExecOutgoingCount[Current] != 1
					|| !SingleExecOutgoingEdge.IsValidIndex(Current)
					|| !Edges.IsValidIndex(SingleExecOutgoingEdge[Current]))
				{
					break;
				}

				const int32 NextNode = Edges[SingleExecOutgoingEdge[Current]].Target;
				if (!Nodes.IsValidIndex(NextNode)
					|| Nodes[NextNode].bRerouteNode
					|| !NodeHasExecPin(Nodes[NextNode])
					|| !ExecIncomingCount.IsValidIndex(NextNode)
					|| ExecIncomingCount[NextNode] != 1)
				{
					break;
				}
				Current = NextNode;
			}

			if (ChainNodes.Num() < 2)
			{
				continue;
			}

			double AnchorPinY = Nodes[ChainNodes[0]].NewY;
			if (IncomingExecEdgeForNode.IsValidIndex(0) && Edges.IsValidIndex(IncomingExecEdgeForNode[0]))
			{
				const FBlueprintLayoutEdge& IncomingEdge = Edges[IncomingExecEdgeForNode[0]];
				const int32 SourceIndex = IncomingEdge.Source;
				const int32 SourceOutgoingCount = ExecOutgoingCount.IsValidIndex(SourceIndex) ? ExecOutgoingCount[SourceIndex] : 0;
				if (SourceOutgoingCount != 1 && Nodes.IsValidIndex(SourceIndex))
				{
					AnchorPinY = Nodes[SourceIndex].NewY + GetPinYOffset(Nodes[SourceIndex], EGPD_Output, IncomingEdge.SourcePinIndex);
				}
				else
				{
					AnchorPinY = Nodes[ChainNodes[0]].NewY + GetPinYOffset(Nodes[ChainNodes[0]], EGPD_Input, IncomingEdge.TargetPinIndex);
				}
			}
			else if (SingleExecOutgoingEdge.IsValidIndex(ChainNodes[0]) && Edges.IsValidIndex(SingleExecOutgoingEdge[ChainNodes[0]]))
			{
				const FBlueprintLayoutEdge& OutgoingEdge = Edges[SingleExecOutgoingEdge[ChainNodes[0]]];
				AnchorPinY = Nodes[ChainNodes[0]].NewY + GetPinYOffset(Nodes[ChainNodes[0]], EGPD_Output, OutgoingEdge.SourcePinIndex);
			}

			TArray<double> DesiredTops;
			for (int32 ChainIndex = 0; ChainIndex < ChainNodes.Num(); ++ChainIndex)
			{
				const int32 NodeIndex = ChainNodes[ChainIndex];
				double DesiredTop = Nodes[NodeIndex].NewY;
				const int32 IncomingEdgeIndex = IncomingExecEdgeForNode.IsValidIndex(ChainIndex) ? IncomingExecEdgeForNode[ChainIndex] : INDEX_NONE;
				if (Edges.IsValidIndex(IncomingEdgeIndex))
				{
					DesiredTop = AnchorPinY - GetPinYOffset(Nodes[NodeIndex], EGPD_Input, Edges[IncomingEdgeIndex].TargetPinIndex);
				}
				else if (SingleExecOutgoingEdge.IsValidIndex(NodeIndex) && Edges.IsValidIndex(SingleExecOutgoingEdge[NodeIndex]))
				{
					DesiredTop = AnchorPinY - GetPinYOffset(Nodes[NodeIndex], EGPD_Output, Edges[SingleExecOutgoingEdge[NodeIndex]].SourcePinIndex);
				}
				DesiredTops.Add(DesiredTop);
			}

			double ShiftY = 0.0;
			for (int32 Guard = 0; Guard < 24; ++Guard)
			{
				bool bShifted = false;
				for (int32 ChainIndex = 0; ChainIndex < ChainNodes.Num(); ++ChainIndex)
				{
					const int32 NodeIndex = ChainNodes[ChainIndex];
					if (!Nodes.IsValidIndex(NodeIndex) || !DesiredTops.IsValidIndex(ChainIndex))
					{
						continue;
					}
					FBlueprintLayoutNode CandidateNode = Nodes[NodeIndex];
					CandidateNode.NewY = DesiredTops[ChainIndex] + ShiftY;
					const FBlueprintLayoutRect CandidateRect = MakeNodeRect(CandidateNode, true);
					for (int32 OtherIndex = 0; OtherIndex < Nodes.Num(); ++OtherIndex)
					{
						if (!Nodes.IsValidIndex(OtherIndex)
							|| ChainSet.Contains(OtherIndex)
							|| Nodes[OtherIndex].bRerouteNode)
						{
							continue;
						}
						const FBlueprintLayoutRect OtherRect = MakeNodeRect(Nodes[OtherIndex], true);
						if (CandidateRect.Intersects(OtherRect, NodePadding))
						{
							const double RequiredShift = OtherRect.Bottom + NodePadding - CandidateRect.Top;
							if (RequiredShift > ShiftY + 0.5)
							{
								ShiftY = RequiredShift;
								bShifted = true;
							}
						}
					}
				}
				if (!bShifted)
				{
					break;
				}
			}

			for (int32 ChainIndex = 0; ChainIndex < ChainNodes.Num(); ++ChainIndex)
			{
				const int32 NodeIndex = ChainNodes[ChainIndex];
				if (!Nodes.IsValidIndex(NodeIndex) || !DesiredTops.IsValidIndex(ChainIndex))
				{
					continue;
				}
				const double NewTop = DesiredTops[ChainIndex] + ShiftY;
				if (!FMath::IsNearlyEqual(Nodes[NodeIndex].NewY, NewTop, 0.5))
				{
					Nodes[NodeIndex].NewY = NewTop;
					++MoveCount;
				}
				ClaimedNodes.Add(NodeIndex);
			}
		}
		return MoveCount;
	}

	static TArray<int32> GetSortedExecOutgoingEdges(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const int32 NodeIndex)
	{
		TArray<int32> Result;
		if (!OutgoingEdges.IsValidIndex(NodeIndex))
		{
			return Result;
		}
		for (const int32 EdgeIndex : OutgoingEdges[NodeIndex])
		{
			if (Edges.IsValidIndex(EdgeIndex) && Edges[EdgeIndex].bExec && Nodes.IsValidIndex(Edges[EdgeIndex].Target))
			{
				Result.Add(EdgeIndex);
			}
		}
		Result.Sort([&Edges, &Nodes](const int32 A, const int32 B)
		{
			const FBlueprintLayoutEdge& EdgeA = Edges[A];
			const FBlueprintLayoutEdge& EdgeB = Edges[B];
			if (EdgeA.SourcePinIndex != EdgeB.SourcePinIndex)
			{
				return EdgeA.SourcePinIndex < EdgeB.SourcePinIndex;
			}
			const int32 PinCompare = EdgeA.SourcePin.Compare(EdgeB.SourcePin, ESearchCase::IgnoreCase);
			if (PinCompare != 0)
			{
				return PinCompare < 0;
			}
			return CompareStableNodeIdentity(Nodes, EdgeA.Target, EdgeB.Target);
		});
		return Result;
	}

	static double GetExecInputOffsetForLane(const FBlueprintLayoutNode& Node, const int32 IncomingEdgeIndex, const TArray<FBlueprintLayoutEdge>& Edges)
	{
		if (Edges.IsValidIndex(IncomingEdgeIndex))
		{
			return GetPinYOffset(Node, EGPD_Input, Edges[IncomingEdgeIndex].TargetPinIndex);
		}
		return GetPinYOffset(Node, EGPD_Output, 0);
	}

	static void AssignControlExecLanesRecursive(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const int32 NodeIndex,
		const int32 IncomingExecEdge,
		const double LaneY,
		const double HorizontalSpacing,
		const double BranchLaneGap,
		TSet<int32>& Visiting,
		TSet<int32>& PlacedExecNodes,
		int32& MoveCount)
	{
		if (!Nodes.IsValidIndex(NodeIndex)
			|| Nodes[NodeIndex].bRerouteNode
			|| !NodeHasExecPin(Nodes[NodeIndex])
			|| Visiting.Contains(NodeIndex))
		{
			return;
		}
		const TArray<int32> ExecOutEdges = GetSortedExecOutgoingEdges(Nodes, Edges, OutgoingEdges, NodeIndex);
		double NodeLaneOffset = GetExecInputOffsetForLane(Nodes[NodeIndex], IncomingExecEdge, Edges);
		if (!Edges.IsValidIndex(IncomingExecEdge) && ExecOutEdges.Num() > 0 && Edges.IsValidIndex(ExecOutEdges[0]))
		{
			NodeLaneOffset = GetPinYOffset(Nodes[NodeIndex], EGPD_Output, Edges[ExecOutEdges[0]].SourcePinIndex);
		}
		if (PlacedExecNodes.Contains(NodeIndex))
		{
			const double CurrentPinY = Nodes[NodeIndex].NewY + NodeLaneOffset;
			const double BlendedLaneY = (CurrentPinY + LaneY) * 0.5;
			const double NewY = BlendedLaneY - NodeLaneOffset;
			if (!FMath::IsNearlyEqual(Nodes[NodeIndex].NewY, NewY, 0.5))
			{
				Nodes[NodeIndex].NewY = NewY;
				++MoveCount;
			}
			return;
		}

		Visiting.Add(NodeIndex);
		PlacedExecNodes.Add(NodeIndex);
		const double NewY = LaneY - NodeLaneOffset;
		if (!FMath::IsNearlyEqual(Nodes[NodeIndex].NewY, NewY, 0.5))
		{
			Nodes[NodeIndex].NewY = NewY;
			++MoveCount;
		}

		if (ExecOutEdges.Num() == 1)
		{
			const int32 TargetIndex = Edges[ExecOutEdges[0]].Target;
			if (Nodes.IsValidIndex(TargetIndex))
			{
				const double RequiredX = Nodes[NodeIndex].NewX + Nodes[NodeIndex].Width + FMath::Clamp(HorizontalSpacing * 0.65, 220.0, 520.0);
				if (!PlacedExecNodes.Contains(TargetIndex) && !FMath::IsNearlyEqual(Nodes[TargetIndex].NewX, RequiredX, 0.5))
				{
					Nodes[TargetIndex].NewX = RequiredX;
					++MoveCount;
				}
			}
			AssignControlExecLanesRecursive(
				Nodes,
				Edges,
				IncomingEdges,
				OutgoingEdges,
				TargetIndex,
				ExecOutEdges[0],
				LaneY,
				HorizontalSpacing,
				BranchLaneGap,
				Visiting,
				PlacedExecNodes,
				MoveCount);
		}
		else if (ExecOutEdges.Num() > 1)
		{
			const double Center = (static_cast<double>(ExecOutEdges.Num()) - 1.0) * 0.5;
			for (int32 ChildOrder = 0; ChildOrder < ExecOutEdges.Num(); ++ChildOrder)
			{
				const int32 EdgeIndex = ExecOutEdges[ChildOrder];
				const int32 TargetIndex = Edges[EdgeIndex].Target;
				if (Nodes.IsValidIndex(TargetIndex))
				{
					const double RequiredX = Nodes[NodeIndex].NewX + Nodes[NodeIndex].Width + FMath::Clamp(HorizontalSpacing * 0.85, 280.0, 660.0);
					if (!PlacedExecNodes.Contains(TargetIndex) && !FMath::IsNearlyEqual(Nodes[TargetIndex].NewX, RequiredX, 0.5))
					{
						Nodes[TargetIndex].NewX = RequiredX;
						++MoveCount;
					}
				}
				const double ChildLaneY = LaneY + (static_cast<double>(ChildOrder) - Center) * BranchLaneGap;
				AssignControlExecLanesRecursive(
					Nodes,
					Edges,
					IncomingEdges,
					OutgoingEdges,
					TargetIndex,
					EdgeIndex,
					ChildLaneY,
					HorizontalSpacing,
					BranchLaneGap * 0.82,
					Visiting,
					PlacedExecNodes,
					MoveCount);
			}
		}
		Visiting.Remove(NodeIndex);
	}

	static int32 ApplyControlExecLaneRestore(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const double HorizontalSpacing,
		const bool bAnchorRootToSingleChild)
	{
		TArray<int32> ExecRoots;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (!Nodes.IsValidIndex(NodeIndex) || Nodes[NodeIndex].bRerouteNode || !NodeHasExecPin(Nodes[NodeIndex]))
			{
				continue;
			}
			int32 IncomingExecCount = 0;
			if (IncomingEdges.IsValidIndex(NodeIndex))
			{
				for (const int32 EdgeIndex : IncomingEdges[NodeIndex])
				{
					if (Edges.IsValidIndex(EdgeIndex) && Edges[EdgeIndex].bExec)
					{
						++IncomingExecCount;
					}
				}
			}
			const TArray<int32> OutExecEdges = GetSortedExecOutgoingEdges(Nodes, Edges, OutgoingEdges, NodeIndex);
			if (IncomingExecCount == 0 && OutExecEdges.Num() > 0)
			{
				ExecRoots.Add(NodeIndex);
			}
		}
		ExecRoots.Sort([&Nodes](const int32 A, const int32 B)
		{
			if (Nodes[A].Component != Nodes[B].Component)
			{
				return Nodes[A].Component < Nodes[B].Component;
			}
			if (!FMath::IsNearlyEqual(Nodes[A].NewY, Nodes[B].NewY, 1.0))
			{
				return Nodes[A].NewY < Nodes[B].NewY;
			}
			return CompareStableNodeIdentity(Nodes, A, B);
		});

		int32 MoveCount = 0;
		TSet<int32> RestoredExecNodes;
		for (const int32 RootIndex : ExecRoots)
		{
			if (!Nodes.IsValidIndex(RootIndex))
			{
				continue;
			}
			double LaneY = Nodes[RootIndex].NewY + GetPinYOffset(Nodes[RootIndex], EGPD_Output, 0);
			const TArray<int32> RootOutExecEdges = GetSortedExecOutgoingEdges(Nodes, Edges, OutgoingEdges, RootIndex);
			if (bAnchorRootToSingleChild && RootOutExecEdges.Num() == 1)
			{
				const int32 TargetIndex = Edges[RootOutExecEdges[0]].Target;
				if (Nodes.IsValidIndex(TargetIndex))
				{
					LaneY = Nodes[TargetIndex].NewY + GetExecInputOffsetForLane(Nodes[TargetIndex], RootOutExecEdges[0], Edges);
				}
			}
			TSet<int32> Visiting;
			AssignControlExecLanesRecursive(
				Nodes,
				Edges,
				IncomingEdges,
				OutgoingEdges,
				RootIndex,
				INDEX_NONE,
				LaneY,
				HorizontalSpacing,
				FMath::Clamp(HorizontalSpacing * 1.35, 360.0, 720.0),
				Visiting,
				RestoredExecNodes,
				MoveCount);
		}
		return MoveCount;
	}

	static int32 ApplyConsumerDataAttachmentLayout(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const double HorizontalSpacing,
		const double NodePadding)
	{
		int32 MoveCount = 0;
		TArray<int32> DataNodes;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (Nodes.IsValidIndex(NodeIndex) && !Nodes[NodeIndex].bRerouteNode && !NodeHasExecPin(Nodes[NodeIndex]))
			{
				DataNodes.Add(NodeIndex);
			}
		}
		DataNodes.Sort([&Nodes](const int32 A, const int32 B)
		{
			if (!FMath::IsNearlyEqual(Nodes[A].NewX, Nodes[B].NewX, 1.0))
			{
				return Nodes[A].NewX > Nodes[B].NewX;
			}
			if (!FMath::IsNearlyEqual(Nodes[A].NewY, Nodes[B].NewY, 1.0))
			{
				return Nodes[A].NewY < Nodes[B].NewY;
			}
			return CompareStableNodeIdentity(Nodes, A, B);
		});

		for (int32 Pass = 0; Pass < 6; ++Pass)
		{
			for (const int32 NodeIndex : DataNodes)
			{
				if (!Nodes.IsValidIndex(NodeIndex) || !OutgoingEdges.IsValidIndex(NodeIndex))
				{
					continue;
				}
				double MinTargetX = TNumericLimits<double>::Max();
				double SumTargetY = 0.0;
				double WeightSum = 0.0;
				int32 ConsumerCount = 0;
				int32 MeaningfulConsumerCount = 0;
				bool bHasNonExecConsumer = false;
				bool bFeedsExecConsumer = false;
				for (const int32 EdgeIndex : OutgoingEdges[NodeIndex])
				{
					if (!Edges.IsValidIndex(EdgeIndex) || Edges[EdgeIndex].bExec || !Nodes.IsValidIndex(Edges[EdgeIndex].Target))
					{
						continue;
					}
					const FBlueprintLayoutNode& TargetNode = Nodes[Edges[EdgeIndex].Target];
					if (TargetNode.bRerouteNode)
					{
						continue;
					}
					++MeaningfulConsumerCount;
					bHasNonExecConsumer = bHasNonExecConsumer || !NodeHasExecPin(TargetNode);
				}
				const bool bMixedLocalAndExecConsumers = MeaningfulConsumerCount > 1 && bHasNonExecConsumer;
				for (const int32 EdgeIndex : OutgoingEdges[NodeIndex])
				{
					if (!Edges.IsValidIndex(EdgeIndex) || Edges[EdgeIndex].bExec || !Nodes.IsValidIndex(Edges[EdgeIndex].Target))
					{
						continue;
					}
					const FBlueprintLayoutNode& TargetNode = Nodes[Edges[EdgeIndex].Target];
					if (TargetNode.bRerouteNode)
					{
						continue;
					}
					const FVector2D TargetPoint = GetEdgeTargetPoint(Nodes, Edges[EdgeIndex], true);
					const double Weight = NodeHasExecPin(TargetNode)
						? (bMixedLocalAndExecConsumers ? 0.85 : 1.8)
						: 1.0;
					MinTargetX = FMath::Min(MinTargetX, TargetNode.NewX);
					SumTargetY += TargetPoint.Y * Weight;
					WeightSum += Weight;
					++ConsumerCount;
					bFeedsExecConsumer = bFeedsExecConsumer || NodeHasExecPin(TargetNode);
				}
				if (ConsumerCount == 0 || WeightSum <= 0.0 || MinTargetX == TNumericLimits<double>::Max())
				{
					continue;
				}

				const double Gap = IsCompactBlueprintValueNode(Nodes[NodeIndex].Node)
					? FMath::Clamp(HorizontalSpacing * 0.30, 80.0, 150.0)
					: FMath::Clamp(HorizontalSpacing * 0.48, 140.0, 260.0);
				double DesiredX = MinTargetX - Nodes[NodeIndex].Width - Gap;
				double DesiredY = SumTargetY / WeightSum - GetPinYOffset(Nodes[NodeIndex], EGPD_Output, 0);

				if (ConsumerCount > 1)
				{
					DesiredX -= FMath::Clamp(HorizontalSpacing * 0.18, 48.0, 140.0);
				}
				if (!bFeedsExecConsumer && !IsCompactBlueprintValueNode(Nodes[NodeIndex].Node) && Nodes[NodeIndex].InputPinCount >= 3)
				{
					DesiredY -= FMath::Clamp(NodePadding * 2.0, 56.0, 140.0);
				}
				if (!FMath::IsNearlyEqual(Nodes[NodeIndex].NewX, DesiredX, 0.5)
					|| !FMath::IsNearlyEqual(Nodes[NodeIndex].NewY, DesiredY, 0.5))
				{
					Nodes[NodeIndex].NewX = DesiredX;
					Nodes[NodeIndex].NewY = DesiredY;
					++MoveCount;
				}
			}
		}
		return MoveCount;
	}

	static bool HasNonExecIncomingDataEdge(
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const int32 NodeIndex)
	{
		if (!IncomingEdges.IsValidIndex(NodeIndex))
		{
			return false;
		}
		for (const int32 EdgeIndex : IncomingEdges[NodeIndex])
		{
			if (Edges.IsValidIndex(EdgeIndex) && !Edges[EdgeIndex].bExec)
			{
				return true;
			}
		}
		return false;
	}

	static int32 ApplySettledFanoutProducerDocking(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const double HorizontalSpacing,
		const double NodePadding)
	{
		int32 MoveCount = 0;
		const double CompactInputGap = FMath::Clamp(HorizontalSpacing * 0.22, 82.0, 160.0);
		for (int32 Pass = 0; Pass < 4; ++Pass)
		{
			bool bMovedThisPass = false;
			TArray<int32> DataNodes;
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				if (Nodes.IsValidIndex(NodeIndex)
					&& !Nodes[NodeIndex].bRerouteNode
					&& !NodeHasExecPin(Nodes[NodeIndex])
					&& OutgoingEdges.IsValidIndex(NodeIndex))
				{
					DataNodes.Add(NodeIndex);
				}
			}
			DataNodes.Sort([&Nodes](const int32 A, const int32 B)
			{
				if (!FMath::IsNearlyEqual(Nodes[A].NewX, Nodes[B].NewX, 1.0))
				{
					return Nodes[A].NewX > Nodes[B].NewX;
				}
				if (!FMath::IsNearlyEqual(Nodes[A].NewY, Nodes[B].NewY, 1.0))
				{
					return Nodes[A].NewY < Nodes[B].NewY;
				}
				return CompareStableNodeIdentity(Nodes, A, B);
			});

			for (const int32 NodeIndex : DataNodes)
			{
				if (!Nodes.IsValidIndex(NodeIndex)
					|| Nodes[NodeIndex].InputPinCount > 2
					|| !OutgoingEdges.IsValidIndex(NodeIndex))
				{
					continue;
				}

				double MinTargetX = TNumericLimits<double>::Max();
				double MaxTargetX = -TNumericLimits<double>::Max();
				double TargetYSum = 0.0;
				double WeightSum = 0.0;
				int32 PureConsumerCount = 0;
				for (const int32 EdgeIndex : OutgoingEdges[NodeIndex])
				{
					if (!Edges.IsValidIndex(EdgeIndex)
						|| Edges[EdgeIndex].bExec
						|| !Nodes.IsValidIndex(Edges[EdgeIndex].Target)
						|| Nodes[Edges[EdgeIndex].Target].bRerouteNode
						|| NodeHasExecPin(Nodes[Edges[EdgeIndex].Target]))
					{
						continue;
					}
					const FBlueprintLayoutNode& TargetNode = Nodes[Edges[EdgeIndex].Target];
					const FVector2D TargetPoint = GetEdgeTargetPoint(Nodes, Edges[EdgeIndex], true);
					MinTargetX = FMath::Min(MinTargetX, TargetNode.NewX);
					MaxTargetX = FMath::Max(MaxTargetX, TargetNode.NewX);
					TargetYSum += TargetPoint.Y;
					WeightSum += 1.0;
					++PureConsumerCount;
				}
				if (PureConsumerCount < 2
					|| WeightSum <= 0.0
					|| MinTargetX == TNumericLimits<double>::Max()
					|| MaxTargetX == -TNumericLimits<double>::Max())
				{
					continue;
				}

				const FVector2D CurrentSourcePoint = FVector2D(
					Nodes[NodeIndex].NewX + Nodes[NodeIndex].Width,
					Nodes[NodeIndex].NewY + GetPinYOffset(Nodes[NodeIndex], EGPD_Output, 0));
				const double FanoutSpan = MaxTargetX - CurrentSourcePoint.X;
				if (FanoutSpan < FMath::Max(420.0, HorizontalSpacing * 1.35))
				{
					continue;
				}

				const bool bSmallPureNode = IsSmallPureBlueprintFunctionNode(Nodes[NodeIndex].Node, Nodes[NodeIndex].InputPinCount, Nodes[NodeIndex].OutputPinCount);
				const double Gap = IsCompactBlueprintValueNode(Nodes[NodeIndex].Node)
					? FMath::Clamp(HorizontalSpacing * 0.30, 80.0, 150.0)
					: (bSmallPureNode
						? FMath::Clamp(HorizontalSpacing * 0.34, 96.0, 170.0)
						: FMath::Clamp(HorizontalSpacing * 0.48, 140.0, 260.0));
				const double FanoutSlack = (IsCompactBlueprintValueNode(Nodes[NodeIndex].Node) || bSmallPureNode)
					? 0.0
					: FMath::Clamp(HorizontalSpacing * 0.18, 48.0, 140.0);
				double DesiredX = MinTargetX - Nodes[NodeIndex].Width - Gap - FanoutSlack;
				if (IncomingEdges.IsValidIndex(NodeIndex))
				{
					double MaxInputRight = -TNumericLimits<double>::Max();
					for (const int32 InEdgeIndex : IncomingEdges[NodeIndex])
					{
						if (Edges.IsValidIndex(InEdgeIndex)
							&& !Edges[InEdgeIndex].bExec
							&& Nodes.IsValidIndex(Edges[InEdgeIndex].Source)
							&& !Nodes[Edges[InEdgeIndex].Source].bRerouteNode)
						{
							MaxInputRight = FMath::Max(MaxInputRight, Nodes[Edges[InEdgeIndex].Source].NewX + Nodes[Edges[InEdgeIndex].Source].Width);
						}
					}
					if (MaxInputRight > -TNumericLimits<double>::Max() * 0.5)
					{
						DesiredX = FMath::Max(DesiredX, MaxInputRight + CompactInputGap);
					}
				}
				const double DesiredY = TargetYSum / WeightSum - GetPinYOffset(Nodes[NodeIndex], EGPD_Output, 0);
				if (!FMath::IsNearlyEqual(Nodes[NodeIndex].NewX, DesiredX, 0.5)
					|| !FMath::IsNearlyEqual(Nodes[NodeIndex].NewY, DesiredY, 0.5))
				{
					Nodes[NodeIndex].NewX = DesiredX;
					Nodes[NodeIndex].NewY = DesiredY;
					++MoveCount;
					bMovedThisPass = true;
				}

				if (!IncomingEdges.IsValidIndex(NodeIndex))
				{
					continue;
				}
				double CompactInputCursorY = -TNumericLimits<double>::Max();
				for (const int32 InEdgeIndex : IncomingEdges[NodeIndex])
				{
					if (!Edges.IsValidIndex(InEdgeIndex)
						|| Edges[InEdgeIndex].bExec
						|| !Nodes.IsValidIndex(Edges[InEdgeIndex].Source)
						|| Nodes[Edges[InEdgeIndex].Source].bRerouteNode
						|| NodeHasExecPin(Nodes[Edges[InEdgeIndex].Source])
						|| !IsCompactBlueprintValueNode(Nodes[Edges[InEdgeIndex].Source].Node)
						|| HasNonExecIncomingDataEdge(Edges, IncomingEdges, Edges[InEdgeIndex].Source))
					{
						continue;
					}
					const int32 SourceIndex = Edges[InEdgeIndex].Source;
					const double SourceX = Nodes[NodeIndex].NewX - Nodes[SourceIndex].Width - CompactInputGap;
					double SourceY = GetEdgeTargetPoint(Nodes, Edges[InEdgeIndex], true).Y
						- GetPinYOffset(Nodes[SourceIndex], EGPD_Output, Edges[InEdgeIndex].SourcePinIndex);
					if (CompactInputCursorY > -TNumericLimits<double>::Max() * 0.5)
					{
						SourceY = FMath::Max(SourceY, CompactInputCursorY);
					}
					CompactInputCursorY = SourceY + Nodes[SourceIndex].Height + FMath::Max(18.0, NodePadding * 0.75);
					if (!FMath::IsNearlyEqual(Nodes[SourceIndex].NewX, SourceX, 0.5)
						|| !FMath::IsNearlyEqual(Nodes[SourceIndex].NewY, SourceY, 0.5))
					{
						Nodes[SourceIndex].NewX = SourceX;
						Nodes[SourceIndex].NewY = SourceY;
						++MoveCount;
						bMovedThisPass = true;
					}
				}
			}
			if (!bMovedThisPass)
			{
				break;
			}
			ResolveGlobalOverlaps(Nodes, NodePadding);
		}
		return MoveCount;
	}

	static int32 ApplyProducerAnchoredDataIslandLayout(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const double HorizontalSpacing,
		const double NodePadding)
	{
		int32 MoveCount = 0;
		const double LocalInputGap = FMath::Clamp(HorizontalSpacing * 0.22, 82.0, 160.0);
		const double ProducerGap = FMath::Clamp(HorizontalSpacing * 0.45, 140.0, 260.0);
		const double LongIslandSpan = FMath::Max(920.0, HorizontalSpacing * 3.0);
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (!Nodes.IsValidIndex(NodeIndex)
				|| Nodes[NodeIndex].bRerouteNode
				|| NodeHasExecPin(Nodes[NodeIndex])
				|| IsCompactBlueprintValueNode(Nodes[NodeIndex].Node)
				|| !IncomingEdges.IsValidIndex(NodeIndex)
				|| !OutgoingEdges.IsValidIndex(NodeIndex))
			{
				continue;
			}

			TArray<int32> InputEdges;
			double MaxSignificantInputRight = -TNumericLimits<double>::Max();
			double MaxExecDataSourceRight = -TNumericLimits<double>::Max();
			double DesiredTopSum = 0.0;
			double DesiredTopWeight = 0.0;
			for (const int32 EdgeIndex : IncomingEdges[NodeIndex])
			{
				if (!Edges.IsValidIndex(EdgeIndex)
					|| Edges[EdgeIndex].bExec
					|| !Nodes.IsValidIndex(Edges[EdgeIndex].Source)
					|| Nodes[Edges[EdgeIndex].Source].bRerouteNode)
				{
					continue;
				}
				InputEdges.Add(EdgeIndex);
				const int32 SourceIndex = Edges[EdgeIndex].Source;
				const FVector2D SourcePoint = GetEdgeSourcePoint(Nodes, Edges[EdgeIndex], true);
				const double DesiredTop = SourcePoint.Y - GetPinYOffset(Nodes[NodeIndex], EGPD_Input, Edges[EdgeIndex].TargetPinIndex);
				const bool bSourceIsLeafValue = IsCompactBlueprintValueNode(Nodes[SourceIndex].Node)
					&& !HasNonExecIncomingDataEdge(Edges, IncomingEdges, SourceIndex);
				const double Weight = bSourceIsLeafValue ? 0.45 : 1.0;
				DesiredTopSum += DesiredTop * Weight;
				DesiredTopWeight += Weight;
				if (NodeHasExecPin(Nodes[SourceIndex]))
				{
					MaxExecDataSourceRight = FMath::Max(MaxExecDataSourceRight, Nodes[SourceIndex].NewX + Nodes[SourceIndex].Width);
				}
				else if (!bSourceIsLeafValue)
				{
					MaxSignificantInputRight = FMath::Max(MaxSignificantInputRight, Nodes[SourceIndex].NewX + Nodes[SourceIndex].Width);
				}
			}
			if (InputEdges.Num() < 2 || DesiredTopWeight <= 0.0)
			{
				continue;
			}

			double MinConsumerX = TNumericLimits<double>::Max();
			bool bFeedsExecConsumer = false;
			for (const int32 EdgeIndex : OutgoingEdges[NodeIndex])
			{
				if (!Edges.IsValidIndex(EdgeIndex)
					|| Edges[EdgeIndex].bExec
					|| !Nodes.IsValidIndex(Edges[EdgeIndex].Target)
					|| Nodes[Edges[EdgeIndex].Target].bRerouteNode)
				{
					continue;
				}
				MinConsumerX = FMath::Min(MinConsumerX, Nodes[Edges[EdgeIndex].Target].NewX);
				bFeedsExecConsumer = bFeedsExecConsumer || NodeHasExecPin(Nodes[Edges[EdgeIndex].Target]);
			}
			if (!bFeedsExecConsumer || MinConsumerX == TNumericLimits<double>::Max())
			{
				continue;
			}

			if (MaxSignificantInputRight == -TNumericLimits<double>::Max())
			{
				MaxSignificantInputRight = Nodes[NodeIndex].NewX;
			}
			const double ConsumerSideX = MinConsumerX - Nodes[NodeIndex].Width - ProducerGap;
			const double ProducerSideX = MaxSignificantInputRight + ProducerGap;
			if (ConsumerSideX - ProducerSideX < LongIslandSpan)
			{
				continue;
			}

			double DesiredX = FMath::Lerp(ProducerSideX, ConsumerSideX, 0.22);
			if (MaxExecDataSourceRight > -TNumericLimits<double>::Max())
			{
				DesiredX = FMath::Max(DesiredX, MaxExecDataSourceRight + LocalInputGap);
			}
			DesiredX = FMath::Min(DesiredX, ConsumerSideX);
			const double DesiredY = DesiredTopSum / DesiredTopWeight;
			if (!FMath::IsNearlyEqual(Nodes[NodeIndex].NewX, DesiredX, 0.5)
				|| !FMath::IsNearlyEqual(Nodes[NodeIndex].NewY, DesiredY, 0.5))
			{
				Nodes[NodeIndex].NewX = DesiredX;
				Nodes[NodeIndex].NewY = DesiredY;
				++MoveCount;
			}

			TArray<int32> CompactInputEdges;
			for (const int32 EdgeIndex : InputEdges)
			{
				const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
				if (Nodes.IsValidIndex(Edge.Source)
					&& IsCompactBlueprintValueNode(Nodes[Edge.Source].Node)
					&& !NodeHasExecPin(Nodes[Edge.Source]))
				{
					CompactInputEdges.Add(EdgeIndex);
				}
			}
			CompactInputEdges.Sort([&Edges](const int32 A, const int32 B)
			{
				if (Edges[A].TargetPinIndex != Edges[B].TargetPinIndex)
				{
					return Edges[A].TargetPinIndex < Edges[B].TargetPinIndex;
				}
				return Edges[A].SourcePinIndex < Edges[B].SourcePinIndex;
			});
			double CursorY = -TNumericLimits<double>::Max();
			for (const int32 EdgeIndex : CompactInputEdges)
			{
				const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
				if (!Nodes.IsValidIndex(Edge.Source))
				{
					continue;
				}
				const double InputX = Nodes[NodeIndex].NewX - Nodes[Edge.Source].Width - LocalInputGap;
				double InputY = GetEdgeTargetPoint(Nodes, Edge, true).Y
					- GetPinYOffset(Nodes[Edge.Source], EGPD_Output, Edge.SourcePinIndex);
				if (CursorY > -TNumericLimits<double>::Max() * 0.5)
				{
					InputY = FMath::Max(InputY, CursorY);
				}
				CursorY = InputY + Nodes[Edge.Source].Height + FMath::Max(18.0, NodePadding * 0.75);
				if (!FMath::IsNearlyEqual(Nodes[Edge.Source].NewX, InputX, 0.5)
					|| !FMath::IsNearlyEqual(Nodes[Edge.Source].NewY, InputY, 0.5))
				{
					Nodes[Edge.Source].NewX = InputX;
					Nodes[Edge.Source].NewY = InputY;
					++MoveCount;
				}
			}

			TArray<int32> SignificantInputEdges;
			for (const int32 EdgeIndex : InputEdges)
			{
				const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
				if (Nodes.IsValidIndex(Edge.Source)
					&& !NodeHasExecPin(Nodes[Edge.Source])
					&& !IsCompactBlueprintValueNode(Nodes[Edge.Source].Node)
					&& !Nodes[Edge.Source].bRerouteNode)
				{
					SignificantInputEdges.Add(EdgeIndex);
				}
			}
			SignificantInputEdges.Sort([&Edges](const int32 A, const int32 B)
			{
				if (Edges[A].TargetPinIndex != Edges[B].TargetPinIndex)
				{
					return Edges[A].TargetPinIndex < Edges[B].TargetPinIndex;
				}
				return Edges[A].SourcePinIndex < Edges[B].SourcePinIndex;
			});

			double SignificantCursorY = -TNumericLimits<double>::Max();
			for (const int32 EdgeIndex : SignificantInputEdges)
			{
				const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
				if (!Nodes.IsValidIndex(Edge.Source))
				{
					continue;
				}
				const double ProducerChainGap = FMath::Clamp(HorizontalSpacing * 2.45, 620.0, 860.0);
				const double DesiredSourceX = Nodes[NodeIndex].NewX - Nodes[Edge.Source].Width - ProducerChainGap;
				if (DesiredSourceX <= Nodes[Edge.Source].NewX + 0.5)
				{
					continue;
				}
				double DesiredSourceY = GetEdgeTargetPoint(Nodes, Edge, true).Y
					- GetPinYOffset(Nodes[Edge.Source], EGPD_Output, Edge.SourcePinIndex);
				if (SignificantCursorY > -TNumericLimits<double>::Max() * 0.5)
				{
					DesiredSourceY = FMath::Max(DesiredSourceY, SignificantCursorY);
				}
				SignificantCursorY = DesiredSourceY + Nodes[Edge.Source].Height + FMath::Max(24.0, NodePadding);
				Nodes[Edge.Source].NewX = DesiredSourceX;
				Nodes[Edge.Source].NewY = DesiredSourceY;
				++MoveCount;
			}
		}
		return MoveCount;
	}

	static int32 ResolveBackwardDataEdgesForPureNodes(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const double HorizontalSpacing)
	{
		int32 MoveCount = 0;
		const double MinDataStep = FMath::Clamp(HorizontalSpacing * 0.38, 110.0, 190.0);
		for (int32 Pass = 0; Pass < 8; ++Pass)
		{
			bool bMovedThisPass = false;
			for (const FBlueprintLayoutEdge& Edge : Edges)
			{
				if (Edge.bExec
					|| !Nodes.IsValidIndex(Edge.Source)
					|| !Nodes.IsValidIndex(Edge.Target)
					|| Nodes[Edge.Source].bRerouteNode
					|| Nodes[Edge.Target].bRerouteNode
					|| NodeHasExecPin(Nodes[Edge.Target]))
				{
					continue;
				}

				const FVector2D Start = GetEdgeSourcePoint(Nodes, Edge, true);
				const FVector2D End = GetEdgeTargetPoint(Nodes, Edge, true);
				if (Start.X <= End.X - 1.0)
				{
					continue;
				}

				const double RequiredTargetX = Nodes[Edge.Source].NewX + Nodes[Edge.Source].Width + MinDataStep;
				if (RequiredTargetX > Nodes[Edge.Target].NewX + 0.5)
				{
					Nodes[Edge.Target].NewX = RequiredTargetX;
					++MoveCount;
					bMovedThisPass = true;
				}
			}
			if (!bMovedThisPass)
			{
				break;
			}
		}
		return MoveCount;
	}

	static int32 ResolveBackwardDataEdgesIntoExecTargets(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const double HorizontalSpacing)
	{
		int32 MoveCount = 0;
		const double MinDataToExecStep = FMath::Clamp(HorizontalSpacing * 0.12, 36.0, 90.0);
		for (int32 Pass = 0; Pass < 4; ++Pass)
		{
			bool bMovedThisPass = false;
			for (const FBlueprintLayoutEdge& Edge : Edges)
			{
				if (Edge.bExec
					|| !Nodes.IsValidIndex(Edge.Source)
					|| !Nodes.IsValidIndex(Edge.Target)
					|| Nodes[Edge.Source].bRerouteNode
					|| Nodes[Edge.Target].bRerouteNode
					|| !NodeHasExecPin(Nodes[Edge.Target]))
				{
					continue;
				}

				const FVector2D Start = GetEdgeSourcePoint(Nodes, Edge, true);
				const FVector2D End = GetEdgeTargetPoint(Nodes, Edge, true);
				if (Start.X <= End.X - 1.0)
				{
					continue;
				}

				const double RequiredTargetX = Start.X + MinDataToExecStep;
				if (RequiredTargetX > Nodes[Edge.Target].NewX + 0.5)
				{
					Nodes[Edge.Target].NewX = RequiredTargetX;
					++MoveCount;
					bMovedThisPass = true;
				}
			}
			if (!bMovedThisPass)
			{
				break;
			}
		}
		return MoveCount;
	}

	static bool IsDirectLeafDataDockingSource(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const int32 SourceIndex)
	{
		if (!Nodes.IsValidIndex(SourceIndex)
			|| Nodes[SourceIndex].bRerouteNode
			|| NodeHasExecPin(Nodes[SourceIndex]))
		{
			return false;
		}

		int32 IncomingDataCount = 0;
		if (IncomingEdges.IsValidIndex(SourceIndex))
		{
			for (const int32 EdgeIndex : IncomingEdges[SourceIndex])
			{
				if (Edges.IsValidIndex(EdgeIndex)
					&& !Edges[EdgeIndex].bExec
					&& Nodes.IsValidIndex(Edges[EdgeIndex].Source)
					&& !Nodes[Edges[EdgeIndex].Source].bRerouteNode)
				{
					++IncomingDataCount;
				}
			}
		}
		if (IncomingDataCount > 0)
		{
			return false;
		}

		int32 OutgoingDataCount = 0;
		if (OutgoingEdges.IsValidIndex(SourceIndex))
		{
			for (const int32 EdgeIndex : OutgoingEdges[SourceIndex])
			{
				if (Edges.IsValidIndex(EdgeIndex)
					&& !Edges[EdgeIndex].bExec
					&& Nodes.IsValidIndex(Edges[EdgeIndex].Target)
					&& !Nodes[Edges[EdgeIndex].Target].bRerouteNode)
				{
					++OutgoingDataCount;
				}
			}
		}
		if (OutgoingDataCount != 1)
		{
			return false;
		}

		const bool bCompactValue = IsCompactBlueprintValueNode(Nodes[SourceIndex].Node);
		const bool bLeafPureValue = Nodes[SourceIndex].InputPinCount == 0
			&& Nodes[SourceIndex].OutputPinCount <= 2
			&& Nodes[SourceIndex].Height <= 90.0;
		return bCompactValue || bLeafPureValue;
	}

	static bool WouldDockedNodeOverlap(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const int32 MovingNodeIndex,
		const int32 TargetNodeIndex,
		const double DesiredX,
		const double DesiredY,
		const double Padding)
	{
		if (!Nodes.IsValidIndex(MovingNodeIndex))
		{
			return true;
		}
		FBlueprintLayoutNode CandidateNode = Nodes[MovingNodeIndex];
		CandidateNode.NewX = DesiredX;
		CandidateNode.NewY = DesiredY;
		const FBlueprintLayoutRect CandidateRect = MakeNodeRect(CandidateNode, true);
		for (int32 OtherIndex = 0; OtherIndex < Nodes.Num(); ++OtherIndex)
		{
			if (OtherIndex == MovingNodeIndex
				|| OtherIndex == TargetNodeIndex
				|| !Nodes.IsValidIndex(OtherIndex)
				|| Nodes[OtherIndex].bRerouteNode)
			{
				continue;
			}
			const FBlueprintLayoutRect OtherRect = MakeNodeRect(Nodes[OtherIndex], true);
			if (CandidateRect.Intersects(OtherRect, Padding))
			{
				return true;
			}
		}
		return false;
	}

	static int32 DockDirectLeafDataInputsToExecTargets(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const double HorizontalSpacing,
		const double NodePadding,
		int32* OutCandidateCount = nullptr)
	{
		TArray<int32> CandidateEdges;
		for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
		{
			const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
			if (Edge.bExec
				|| !Nodes.IsValidIndex(Edge.Source)
				|| !Nodes.IsValidIndex(Edge.Target)
				|| Nodes[Edge.Source].bRerouteNode
				|| Nodes[Edge.Target].bRerouteNode
				|| !NodeHasExecPin(Nodes[Edge.Target])
				|| !IsDirectLeafDataDockingSource(Nodes, Edges, IncomingEdges, OutgoingEdges, Edge.Source))
			{
				continue;
			}
			CandidateEdges.Add(EdgeIndex);
		}
		if (OutCandidateCount)
		{
			*OutCandidateCount = CandidateEdges.Num();
		}
		CandidateEdges.Sort([&Nodes, &Edges](const int32 A, const int32 B)
		{
			const FBlueprintLayoutEdge& EdgeA = Edges[A];
			const FBlueprintLayoutEdge& EdgeB = Edges[B];
			if (!FMath::IsNearlyEqual(Nodes[EdgeA.Target].NewX, Nodes[EdgeB.Target].NewX, 1.0))
			{
				return Nodes[EdgeA.Target].NewX < Nodes[EdgeB.Target].NewX;
			}
			const double TargetYA = GetEdgeTargetPoint(Nodes, EdgeA, true).Y;
			const double TargetYB = GetEdgeTargetPoint(Nodes, EdgeB, true).Y;
			if (!FMath::IsNearlyEqual(TargetYA, TargetYB, 1.0))
			{
				return TargetYA < TargetYB;
			}
			return CompareStableNodeIdentity(Nodes, EdgeA.Source, EdgeB.Source);
		});

		int32 MoveCount = 0;
		const double OverlapPadding = FMath::Max(0.0, NodePadding);
		for (const int32 EdgeIndex : CandidateEdges)
		{
			const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
			if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
			{
				continue;
			}

			const double Gap = IsCompactBlueprintValueNode(Nodes[Edge.Source].Node)
				? FMath::Clamp(HorizontalSpacing * 0.24, 72.0, 130.0)
				: FMath::Clamp(HorizontalSpacing * 0.30, 90.0, 160.0);
			const double DesiredX = Nodes[Edge.Target].NewX - Nodes[Edge.Source].Width - Gap;
			const double DesiredY = GetEdgeTargetPoint(Nodes, Edge, true).Y
				- GetPinYOffset(Nodes[Edge.Source], EGPD_Output, Edge.SourcePinIndex);
			if (WouldDockedNodeOverlap(Nodes, Edge.Source, Edge.Target, DesiredX, DesiredY, OverlapPadding))
			{
				continue;
			}
			if (!FMath::IsNearlyEqual(Nodes[Edge.Source].NewX, DesiredX, 0.5)
				|| !FMath::IsNearlyEqual(Nodes[Edge.Source].NewY, DesiredY, 0.5))
			{
				Nodes[Edge.Source].NewX = DesiredX;
				Nodes[Edge.Source].NewY = DesiredY;
				++MoveCount;
			}
		}
		return MoveCount;
	}

	static int32 DockExecInputExpressionIslandsToExecTargets(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const double HorizontalSpacing,
		const double NodePadding,
		int32* OutCandidateCount = nullptr)
	{
		TArray<int32> CandidateEdges;
		for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
		{
			const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
			if (Edge.bExec
				|| !Nodes.IsValidIndex(Edge.Source)
				|| !Nodes.IsValidIndex(Edge.Target)
				|| Nodes[Edge.Source].bRerouteNode
				|| Nodes[Edge.Target].bRerouteNode
				|| NodeHasExecPin(Nodes[Edge.Source])
				|| !NodeHasExecPin(Nodes[Edge.Target])
				|| IsDirectLeafDataDockingSource(Nodes, Edges, IncomingEdges, OutgoingEdges, Edge.Source)
				|| !HasNonExecIncomingDataEdge(Edges, IncomingEdges, Edge.Source)
				|| !IsFinalSingleConsumerPinAlignmentSource(Nodes, Edges, IncomingEdges, OutgoingEdges, Edge.Source, EdgeIndex))
			{
				continue;
			}
			CandidateEdges.Add(EdgeIndex);
		}
		if (OutCandidateCount)
		{
			*OutCandidateCount = CandidateEdges.Num();
		}
		CandidateEdges.Sort([&Nodes, &Edges](const int32 A, const int32 B)
		{
			const FBlueprintLayoutEdge& EdgeA = Edges[A];
			const FBlueprintLayoutEdge& EdgeB = Edges[B];
			if (!FMath::IsNearlyEqual(Nodes[EdgeA.Target].NewX, Nodes[EdgeB.Target].NewX, 1.0))
			{
				return Nodes[EdgeA.Target].NewX < Nodes[EdgeB.Target].NewX;
			}
			const double TargetPinYA = GetEdgeTargetPoint(Nodes, EdgeA, true).Y;
			const double TargetPinYB = GetEdgeTargetPoint(Nodes, EdgeB, true).Y;
			if (!FMath::IsNearlyEqual(TargetPinYA, TargetPinYB, 1.0))
			{
				return TargetPinYA < TargetPinYB;
			}
			return CompareStableNodeIdentity(Nodes, EdgeA.Source, EdgeB.Source);
		});

		int32 MoveCount = 0;
		for (const int32 EdgeIndex : CandidateEdges)
		{
			if (!Edges.IsValidIndex(EdgeIndex))
			{
				continue;
			}
			const FBlueprintLayoutEdge& Edge = Edges[EdgeIndex];
			if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
			{
				continue;
			}

			TArray<int32> MoveGroup;
			TSet<int32> VisitedGroupNodes;
			CollectMovableFinalAlignmentDataIsland(
				Nodes,
				Edges,
				IncomingEdges,
				OutgoingEdges,
				Edge.Source,
				VisitedGroupNodes,
				MoveGroup);
			if (MoveGroup.Num() < 2)
			{
				continue;
			}

			const double RootGap = IsCompactBlueprintValueNode(Nodes[Edge.Source].Node)
				? FMath::Clamp(HorizontalSpacing * 0.24, 72.0, 130.0)
				: FMath::Clamp(HorizontalSpacing * 0.34, 100.0, 180.0);
			const double DesiredRootX = Nodes[Edge.Target].NewX - Nodes[Edge.Source].Width - RootGap;
			const double PinAlignedRootY = GetEdgeTargetPoint(Nodes, Edge, true).Y
				- GetPinYOffset(Nodes[Edge.Source], EGPD_Output, Edge.SourcePinIndex);
			const double SideGap = FMath::Clamp(HorizontalSpacing * 0.30, 96.0, 180.0);

			TArray<double> CandidateRootYs;
			auto AddCandidateRootY = [&CandidateRootYs](const double CandidateY)
			{
				if (!FMath::IsFinite(CandidateY))
				{
					return;
				}
				for (const double ExistingY : CandidateRootYs)
				{
					if (FMath::IsNearlyEqual(ExistingY, CandidateY, 0.5))
					{
						return;
					}
				}
				CandidateRootYs.Add(CandidateY);
			};
			AddCandidateRootY(PinAlignedRootY);
			AddCandidateRootY(Nodes[Edge.Target].NewY - Nodes[Edge.Source].Height - SideGap);
			AddCandidateRootY(Nodes[Edge.Target].NewY + Nodes[Edge.Target].Height + SideGap);

			double BestDeltaX = 0.0;
			double BestDeltaY = 0.0;
			double BestPenalty = TNumericLimits<double>::Max();
			double BestScore = TNumericLimits<double>::Max();
			for (const double CandidateRootY : CandidateRootYs)
			{
				const double DeltaX = DesiredRootX - Nodes[Edge.Source].NewX;
				const double DeltaY = CandidateRootY - Nodes[Edge.Source].NewY;
				const double OverlapPenalty = MeasureMoveNodeGroupOverlapPenaltyAtDelta(
					Nodes,
					MoveGroup,
					DeltaX,
					DeltaY,
					NodePadding);
				const double ExecCorridorPenalty = MeasureMoveNodeGroupExecCorridorPenaltyAtDelta(
					Nodes,
					Edges,
					MoveGroup,
					DeltaX,
					DeltaY,
					NodePadding);
				const double PinResidual = FMath::Abs(CandidateRootY - PinAlignedRootY);
				const double Score = OverlapPenalty
					+ ExecCorridorPenalty
					+ PinResidual * 8.0
					+ FMath::Abs(DeltaY) * 0.01
					+ FMath::Abs(DeltaX) * 0.005;
				if (Score < BestScore - 0.5)
				{
					BestScore = Score;
					BestPenalty = OverlapPenalty;
					BestDeltaX = DeltaX;
					BestDeltaY = DeltaY;
				}
			}

			if (BestPenalty >= 1000000.0
				|| (FMath::IsNearlyEqual(BestDeltaX, 0.0, 0.5) && FMath::IsNearlyEqual(BestDeltaY, 0.0, 0.5)))
			{
				continue;
			}

			for (const int32 NodeIndex : MoveGroup)
			{
				if (Nodes.IsValidIndex(NodeIndex))
				{
					Nodes[NodeIndex].NewX += BestDeltaX;
					Nodes[NodeIndex].NewY += BestDeltaY;
					++MoveCount;
				}
			}
		}
		return MoveCount;
	}

	static int32 DockSharedExecInputFanoutDataIslandsToExecTargets(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const double HorizontalSpacing,
		const double NodePadding,
		int32* OutCandidateCount = nullptr)
	{
		TArray<int32> CandidateSources;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (!Nodes.IsValidIndex(NodeIndex)
				|| Nodes[NodeIndex].bRerouteNode
				|| NodeHasExecPin(Nodes[NodeIndex])
				|| !OutgoingEdges.IsValidIndex(NodeIndex)
				|| !IsMovableFinalAlignmentDataNode(Nodes[NodeIndex]))
			{
				continue;
			}

			TSet<int32> ExecConsumerTargets;
			for (const int32 EdgeIndex : OutgoingEdges[NodeIndex])
			{
				if (!Edges.IsValidIndex(EdgeIndex)
					|| Edges[EdgeIndex].bExec
					|| !Nodes.IsValidIndex(Edges[EdgeIndex].Target)
					|| Nodes[Edges[EdgeIndex].Target].bRerouteNode
					|| !NodeHasExecPin(Nodes[Edges[EdgeIndex].Target]))
				{
					continue;
				}
				ExecConsumerTargets.Add(Edges[EdgeIndex].Target);
			}
			if (ExecConsumerTargets.Num() < 2)
			{
				continue;
			}
			CandidateSources.Add(NodeIndex);
		}
		if (OutCandidateCount)
		{
			*OutCandidateCount = CandidateSources.Num();
		}
		CandidateSources.Sort([&Nodes](const int32 A, const int32 B)
		{
			if (!FMath::IsNearlyEqual(Nodes[A].NewX, Nodes[B].NewX, 1.0))
			{
				return Nodes[A].NewX < Nodes[B].NewX;
			}
			if (!FMath::IsNearlyEqual(Nodes[A].NewY, Nodes[B].NewY, 1.0))
			{
				return Nodes[A].NewY < Nodes[B].NewY;
			}
			return CompareStableNodeIdentity(Nodes, A, B);
		});

		int32 MoveCount = 0;
		for (const int32 SourceIndex : CandidateSources)
		{
			if (!Nodes.IsValidIndex(SourceIndex) || !OutgoingEdges.IsValidIndex(SourceIndex))
			{
				continue;
			}

			TArray<int32> FanoutEdges;
			TArray<double> TargetPinYs;
			for (const int32 EdgeIndex : OutgoingEdges[SourceIndex])
			{
				if (!Edges.IsValidIndex(EdgeIndex)
					|| Edges[EdgeIndex].bExec
					|| !Nodes.IsValidIndex(Edges[EdgeIndex].Target)
					|| Nodes[Edges[EdgeIndex].Target].bRerouteNode
					|| !NodeHasExecPin(Nodes[Edges[EdgeIndex].Target]))
				{
					continue;
				}
				FanoutEdges.Add(EdgeIndex);
				TargetPinYs.Add(GetEdgeTargetPoint(Nodes, Edges[EdgeIndex], true).Y);
			}
			if (FanoutEdges.Num() < 2 || TargetPinYs.Num() < 2)
			{
				continue;
			}
			TargetPinYs.Sort();
			const int32 MiddleIndex = TargetPinYs.Num() / 2;
			const double MedianTargetPinY = (TargetPinYs.Num() % 2) == 0
				? (TargetPinYs[MiddleIndex - 1] + TargetPinYs[MiddleIndex]) * 0.5
				: TargetPinYs[MiddleIndex];

			TArray<int32> MoveGroup;
			TSet<int32> VisitedGroupNodes;
			CollectMovableFinalAlignmentDataIsland(
				Nodes,
				Edges,
				IncomingEdges,
				OutgoingEdges,
				SourceIndex,
				VisitedGroupNodes,
				MoveGroup);
			if (MoveGroup.Num() == 0)
			{
				continue;
			}

			const int32 SourcePinIndex = Edges[FanoutEdges[0]].SourcePinIndex;
			const double PinAlignedRootY = MedianTargetPinY - GetPinYOffset(Nodes[SourceIndex], EGPD_Output, SourcePinIndex);
			const double SideGap = FMath::Clamp(HorizontalSpacing * 0.36, 96.0, 190.0);

			TArray<double> CandidateRootYs;
			auto AddCandidateRootY = [&CandidateRootYs](const double CandidateY)
			{
				if (!FMath::IsFinite(CandidateY))
				{
					return;
				}
				for (const double ExistingY : CandidateRootYs)
				{
					if (FMath::IsNearlyEqual(ExistingY, CandidateY, 0.5))
					{
						return;
					}
				}
				CandidateRootYs.Add(CandidateY);
			};
			AddCandidateRootY(PinAlignedRootY);
			AddCandidateRootY(PinAlignedRootY - SideGap);
			AddCandidateRootY(PinAlignedRootY + SideGap);
			AddCandidateRootY(Nodes[SourceIndex].NewY);

			double BestDeltaY = 0.0;
			double BestPenalty = TNumericLimits<double>::Max();
			double BestScore = TNumericLimits<double>::Max();
			for (const double CandidateRootY : CandidateRootYs)
			{
				const double DeltaY = CandidateRootY - Nodes[SourceIndex].NewY;
				const double OverlapPenalty = MeasureMoveNodeGroupOverlapPenaltyAtDelta(
					Nodes,
					MoveGroup,
					0.0,
					DeltaY,
					NodePadding);
				const double CandidateSourcePinY = CandidateRootY + GetPinYOffset(Nodes[SourceIndex], EGPD_Output, SourcePinIndex);
				double FanoutResidual = 0.0;
				for (const double TargetPinY : TargetPinYs)
				{
					FanoutResidual += FMath::Abs(CandidateSourcePinY - TargetPinY);
				}
				FanoutResidual /= static_cast<double>(TargetPinYs.Num());
				const double Score = OverlapPenalty
					+ FMath::Abs(CandidateRootY - PinAlignedRootY) * 10.0
					+ FanoutResidual * 0.15
					+ FMath::Abs(DeltaY) * 0.005;
				if (Score < BestScore - 0.5)
				{
					BestScore = Score;
					BestPenalty = OverlapPenalty;
					BestDeltaY = DeltaY;
				}
			}

			if (BestPenalty >= 1000000.0 || FMath::IsNearlyEqual(BestDeltaY, 0.0, 0.5))
			{
				continue;
			}

			for (const int32 NodeIndex : MoveGroup)
			{
				if (Nodes.IsValidIndex(NodeIndex))
				{
					Nodes[NodeIndex].NewY += BestDeltaY;
					++MoveCount;
				}
			}
		}
		return MoveCount;
	}

	static int32 ResolveBackwardExecEdges(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const double HorizontalSpacing)
	{
		int32 MoveCount = 0;
		const double MinExecStep = FMath::Clamp(HorizontalSpacing * 0.65, 220.0, 520.0);
		for (int32 Pass = 0; Pass < 8; ++Pass)
		{
			bool bMovedThisPass = false;
			for (const FBlueprintLayoutEdge& Edge : Edges)
			{
				if (!Edge.bExec
					|| !Nodes.IsValidIndex(Edge.Source)
					|| !Nodes.IsValidIndex(Edge.Target)
					|| Nodes[Edge.Source].bRerouteNode
					|| Nodes[Edge.Target].bRerouteNode)
				{
					continue;
				}

				const double RequiredTargetX = Nodes[Edge.Source].NewX + Nodes[Edge.Source].Width + MinExecStep;
				if (Nodes[Edge.Target].NewX >= RequiredTargetX - 0.5)
				{
					continue;
				}

				if (RequiredTargetX > Nodes[Edge.Target].NewX + 0.5)
				{
					Nodes[Edge.Target].NewX = RequiredTargetX;
					++MoveCount;
					bMovedThisPass = true;
				}
			}
			if (!bMovedThisPass)
			{
				break;
			}
		}
		return MoveCount;
	}

	static int32 EnforceSharedTargetDataSourceOrder(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const double NodePadding)
	{
		int32 MoveCount = 0;
		const double MinPinLaneGap = FMath::Clamp(NodePadding * 0.85, 18.0, 48.0);
		auto IsSingleDirectCompactDataSource = [&](const int32 NodeIndex) -> bool
		{
			if (!Nodes.IsValidIndex(NodeIndex)
				|| !IsCompactBlueprintValueNode(Nodes[NodeIndex].Node)
				|| !OutgoingEdges.IsValidIndex(NodeIndex))
			{
				return false;
			}
			int32 DirectDataConsumerCount = 0;
			for (const int32 EdgeIndex : OutgoingEdges[NodeIndex])
			{
				if (Edges.IsValidIndex(EdgeIndex)
					&& !Edges[EdgeIndex].bExec
					&& Nodes.IsValidIndex(Edges[EdgeIndex].Target)
					&& !Nodes[Edges[EdgeIndex].Target].bRerouteNode)
				{
					++DirectDataConsumerCount;
				}
			}
			return DirectDataConsumerCount == 1;
		};
		for (int32 Pass = 0; Pass < 8; ++Pass)
		{
			bool bMovedThisPass = false;
			for (int32 TargetIndex = 0; TargetIndex < Nodes.Num(); ++TargetIndex)
			{
				if (!Nodes.IsValidIndex(TargetIndex)
					|| Nodes[TargetIndex].bRerouteNode
					|| !IncomingEdges.IsValidIndex(TargetIndex))
				{
					continue;
				}

				TArray<int32> DataInEdges;
				for (const int32 EdgeIndex : IncomingEdges[TargetIndex])
				{
					if (Edges.IsValidIndex(EdgeIndex)
						&& !Edges[EdgeIndex].bExec
						&& Nodes.IsValidIndex(Edges[EdgeIndex].Source)
						&& !Nodes[Edges[EdgeIndex].Source].bRerouteNode)
					{
						DataInEdges.Add(EdgeIndex);
					}
				}
				if (DataInEdges.Num() < 2)
				{
					continue;
				}
				bool bProducerAnchoredDataIsland = !NodeHasExecPin(Nodes[TargetIndex])
					&& !IsCompactBlueprintValueNode(Nodes[TargetIndex].Node)
					&& DataInEdges.Num() >= 3;
				bool bRestrictProducerIslandToCompactInputs = false;
				if (bProducerAnchoredDataIsland && OutgoingEdges.IsValidIndex(TargetIndex))
				{
					bool bFeedsExecConsumer = false;
					for (const int32 OutEdgeIndex : OutgoingEdges[TargetIndex])
					{
						if (Edges.IsValidIndex(OutEdgeIndex)
							&& !Edges[OutEdgeIndex].bExec
							&& Nodes.IsValidIndex(Edges[OutEdgeIndex].Target)
							&& NodeHasExecPin(Nodes[Edges[OutEdgeIndex].Target]))
						{
							bFeedsExecConsumer = true;
							break;
						}
					}
					if (bFeedsExecConsumer)
					{
						bRestrictProducerIslandToCompactInputs = true;
					}
				}

				DataInEdges.Sort([&Edges, &Nodes](const int32 A, const int32 B)
				{
					const FBlueprintLayoutEdge& EdgeA = Edges[A];
					const FBlueprintLayoutEdge& EdgeB = Edges[B];
					if (EdgeA.TargetPinIndex != EdgeB.TargetPinIndex)
					{
						return EdgeA.TargetPinIndex < EdgeB.TargetPinIndex;
					}
					if (!FMath::IsNearlyEqual(Nodes[EdgeA.Source].NewY, Nodes[EdgeB.Source].NewY, 1.0))
					{
						return Nodes[EdgeA.Source].NewY < Nodes[EdgeB.Source].NewY;
					}
					return CompareStableNodeIdentity(Nodes, EdgeA.Source, EdgeB.Source);
				});

				for (int32 EdgeOrder = 1; EdgeOrder < DataInEdges.Num(); ++EdgeOrder)
				{
					const FBlueprintLayoutEdge& UpperEdge = Edges[DataInEdges[EdgeOrder - 1]];
					const FBlueprintLayoutEdge& LowerEdge = Edges[DataInEdges[EdgeOrder]];
					if (!Nodes.IsValidIndex(UpperEdge.Source) || !Nodes.IsValidIndex(LowerEdge.Source))
					{
						continue;
					}
					if (HasConflictingSharedTargetOrder(Edges, UpperEdge.Source, LowerEdge.Source, false))
					{
						continue;
					}
					if (bRestrictProducerIslandToCompactInputs
						&& !IsSingleDirectCompactDataSource(UpperEdge.Source)
						&& !IsSingleDirectCompactDataSource(LowerEdge.Source))
					{
						continue;
					}

					const double UpperSourceY = GetEdgeSourcePoint(Nodes, UpperEdge, true).Y;
					const double LowerSourceY = GetEdgeSourcePoint(Nodes, LowerEdge, true).Y;
					const double RequiredLowerY = UpperSourceY + MinPinLaneGap;
					if (LowerSourceY >= RequiredLowerY - 0.5)
					{
						continue;
					}

					const bool bCanMoveLower = !NodeHasExecPin(Nodes[LowerEdge.Source]);
					const bool bCanMoveUpper = !NodeHasExecPin(Nodes[UpperEdge.Source]);
					if (bCanMoveLower)
					{
						Nodes[LowerEdge.Source].NewY += RequiredLowerY - LowerSourceY;
						++MoveCount;
						bMovedThisPass = true;
					}
					else if (bCanMoveUpper)
					{
						Nodes[UpperEdge.Source].NewY -= RequiredLowerY - LowerSourceY;
						++MoveCount;
						bMovedThisPass = true;
					}
				}
			}
			if (!bMovedThisPass)
			{
				break;
			}
		}
		return MoveCount;
	}

	static int32 ApplyControlTreeLayoutRefinement(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& IncomingEdges,
		const TArray<TArray<int32>>& OutgoingEdges,
		const double HorizontalSpacing,
		const double NodePadding,
		FBlueprintLayoutAlignmentStats* OutAlignmentStats = nullptr)
	{
		int32 MoveCount = 0;
		TArray<int32> ExecRoots;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (!Nodes.IsValidIndex(NodeIndex) || Nodes[NodeIndex].bRerouteNode || !NodeHasExecPin(Nodes[NodeIndex]))
			{
				continue;
			}
			int32 IncomingExecCount = 0;
			if (IncomingEdges.IsValidIndex(NodeIndex))
			{
				for (const int32 EdgeIndex : IncomingEdges[NodeIndex])
				{
					if (Edges.IsValidIndex(EdgeIndex) && Edges[EdgeIndex].bExec)
					{
						++IncomingExecCount;
					}
				}
			}
			const TArray<int32> OutExecEdges = GetSortedExecOutgoingEdges(Nodes, Edges, OutgoingEdges, NodeIndex);
			if (IncomingExecCount == 0 && OutExecEdges.Num() > 0)
			{
				ExecRoots.Add(NodeIndex);
			}
		}
		ExecRoots.Sort([&Nodes](const int32 A, const int32 B)
		{
			if (Nodes[A].Component != Nodes[B].Component)
			{
				return Nodes[A].Component < Nodes[B].Component;
			}
			if (!FMath::IsNearlyEqual(Nodes[A].NewY, Nodes[B].NewY, 1.0))
			{
				return Nodes[A].NewY < Nodes[B].NewY;
			}
			return CompareStableNodeIdentity(Nodes, A, B);
		});

		TSet<int32> PlacedExecNodes;
		TMap<int32, double> NextLaneByComponent;
		TMap<int32, double> LeftByComponent;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (!Nodes.IsValidIndex(NodeIndex) || Nodes[NodeIndex].bRerouteNode)
			{
				continue;
			}
			double& Left = LeftByComponent.FindOrAdd(Nodes[NodeIndex].Component);
			if (Left == 0.0)
			{
				Left = Nodes[NodeIndex].NewX;
			}
			else
			{
				Left = FMath::Min(Left, Nodes[NodeIndex].NewX);
			}
		}
		for (const int32 RootIndex : ExecRoots)
		{
			if (!Nodes.IsValidIndex(RootIndex) || PlacedExecNodes.Contains(RootIndex))
			{
				continue;
			}
			const double ComponentLeft = LeftByComponent.Contains(Nodes[RootIndex].Component)
				? LeftByComponent[Nodes[RootIndex].Component]
				: Nodes[RootIndex].NewX;
			const double RootX = ComponentLeft + FMath::Clamp(HorizontalSpacing * 1.6, 480.0, 760.0);
			if (!FMath::IsNearlyEqual(Nodes[RootIndex].NewX, RootX, 0.5))
			{
				Nodes[RootIndex].NewX = RootX;
				++MoveCount;
			}
			double& NextLaneY = NextLaneByComponent.FindOrAdd(Nodes[RootIndex].Component);
			if (NextLaneY == 0.0)
			{
				NextLaneY = Nodes[RootIndex].NewY + GetPinYOffset(Nodes[RootIndex], EGPD_Output, 0);
			}
			const double RootLaneY = FMath::Max(NextLaneY, Nodes[RootIndex].NewY + GetPinYOffset(Nodes[RootIndex], EGPD_Output, 0));
			TSet<int32> Visiting;
			AssignControlExecLanesRecursive(
				Nodes,
				Edges,
				IncomingEdges,
				OutgoingEdges,
				RootIndex,
				INDEX_NONE,
				RootLaneY,
				HorizontalSpacing,
				FMath::Clamp(HorizontalSpacing * 1.35, 360.0, 720.0),
				Visiting,
				PlacedExecNodes,
				MoveCount);
			double SubtreeBottom = RootLaneY;
			for (const int32 PlacedIndex : PlacedExecNodes)
			{
				if (Nodes.IsValidIndex(PlacedIndex) && Nodes[PlacedIndex].Component == Nodes[RootIndex].Component)
				{
					SubtreeBottom = FMath::Max(SubtreeBottom, Nodes[PlacedIndex].NewY + Nodes[PlacedIndex].Height);
				}
			}
			NextLaneY = SubtreeBottom + FMath::Clamp(HorizontalSpacing * 0.72, 220.0, 420.0);
		}

		MoveCount += ApplyConsumerDataAttachmentLayout(Nodes, Edges, IncomingEdges, OutgoingEdges, HorizontalSpacing, NodePadding);
		MoveCount += ApplyProducerAnchoredDataIslandLayout(Nodes, Edges, IncomingEdges, OutgoingEdges, HorizontalSpacing, NodePadding);
		MoveCount += ResolveBackwardDataEdgesForPureNodes(Nodes, Edges, HorizontalSpacing);
		MoveCount += ResolveBackwardDataEdgesIntoExecTargets(Nodes, Edges, HorizontalSpacing);
		MoveCount += DockExecInputExpressionIslandsToExecTargets(Nodes, Edges, IncomingEdges, OutgoingEdges, HorizontalSpacing, NodePadding);
		MoveCount += ResolveBackwardDataEdgesForPureNodes(Nodes, Edges, HorizontalSpacing);
		MoveCount += ResolveBackwardDataEdgesIntoExecTargets(Nodes, Edges, HorizontalSpacing);
		MoveCount += EnforceSharedTargetDataSourceOrder(Nodes, Edges, IncomingEdges, OutgoingEdges, NodePadding);
		MoveCount += ResolveBackwardExecEdges(Nodes, Edges, HorizontalSpacing);
		for (int32 Pass = 0; Pass < 3; ++Pass)
		{
			const int32 OverlapMoves = ResolveGlobalOverlaps(Nodes, NodePadding);
			MoveCount += OverlapMoves;
			TSet<int32> DummyVisiting;
			TSet<int32> RealignedExecNodes;
			for (const int32 RootIndex : ExecRoots)
			{
				if (!Nodes.IsValidIndex(RootIndex))
				{
					continue;
				}
				const double LaneY = Nodes[RootIndex].NewY + GetPinYOffset(Nodes[RootIndex], EGPD_Output, 0);
				AssignControlExecLanesRecursive(
					Nodes,
					Edges,
					IncomingEdges,
					OutgoingEdges,
					RootIndex,
					INDEX_NONE,
					LaneY,
					HorizontalSpacing,
					FMath::Clamp(HorizontalSpacing * 1.35, 360.0, 720.0),
					DummyVisiting,
					RealignedExecNodes,
					MoveCount);
			}
			DockSingleConsumerDataSubgraphs(Nodes, Edges, OutgoingEdges, NodePadding);
			MoveCount += ApplyProducerAnchoredDataIslandLayout(Nodes, Edges, IncomingEdges, OutgoingEdges, HorizontalSpacing, NodePadding);
			MoveCount += ResolveBackwardDataEdgesForPureNodes(Nodes, Edges, HorizontalSpacing);
			MoveCount += ResolveBackwardDataEdgesIntoExecTargets(Nodes, Edges, HorizontalSpacing);
			MoveCount += DockExecInputExpressionIslandsToExecTargets(Nodes, Edges, IncomingEdges, OutgoingEdges, HorizontalSpacing, NodePadding);
			MoveCount += ResolveBackwardDataEdgesForPureNodes(Nodes, Edges, HorizontalSpacing);
			MoveCount += ResolveBackwardDataEdgesIntoExecTargets(Nodes, Edges, HorizontalSpacing);
			MoveCount += EnforceSharedTargetDataSourceOrder(Nodes, Edges, IncomingEdges, OutgoingEdges, NodePadding);
			MoveCount += ResolveBackwardExecEdges(Nodes, Edges, HorizontalSpacing);
			if (OverlapMoves == 0)
			{
				break;
			}
		}
		// Producer-island passes can move pure consumers after the first data docking pass.
		// Re-dock only settled one-to-many data producers so fanout sources do not remain stranded.
		MoveCount += DockSharedExecInputFanoutDataIslandsToExecTargets(Nodes, Edges, IncomingEdges, OutgoingEdges, HorizontalSpacing, NodePadding);
		MoveCount += ApplySettledFanoutProducerDocking(Nodes, Edges, IncomingEdges, OutgoingEdges, HorizontalSpacing, NodePadding);
		MoveCount += ResolveBackwardDataEdgesForPureNodes(Nodes, Edges, HorizontalSpacing);
		MoveCount += ResolveBackwardDataEdgesIntoExecTargets(Nodes, Edges, HorizontalSpacing);
		MoveCount += EnforceSharedTargetDataSourceOrder(Nodes, Edges, IncomingEdges, OutgoingEdges, NodePadding);
		const int32 PinAlignedCompactionMoves = ApplyTopDownPinAlignedCompaction(Nodes, Edges, IncomingEdges, OutgoingEdges, HorizontalSpacing, NodePadding);
		MoveCount += PinAlignedCompactionMoves;
		if (OutAlignmentStats)
		{
			OutAlignmentStats->PinAlignedCompactionMoveCount += PinAlignedCompactionMoves;
		}
		for (int32 Pass = 0; Pass < 4; ++Pass)
		{
			const int32 OverlapMoves = ResolveGlobalOverlaps(Nodes, NodePadding);
			MoveCount += OverlapMoves;
			if (OverlapMoves == 0)
			{
				break;
			}
		}
		MoveCount += ApplyControlExecLaneRestore(Nodes, Edges, IncomingEdges, OutgoingEdges, HorizontalSpacing, true);
		MoveCount += DockSharedExecInputFanoutDataIslandsToExecTargets(Nodes, Edges, IncomingEdges, OutgoingEdges, HorizontalSpacing, NodePadding);
		MoveCount += DockExecInputExpressionIslandsToExecTargets(Nodes, Edges, IncomingEdges, OutgoingEdges, HorizontalSpacing, NodePadding);
		MoveCount += ResolveBackwardDataEdgesForPureNodes(Nodes, Edges, HorizontalSpacing);
		MoveCount += ResolveBackwardDataEdgesIntoExecTargets(Nodes, Edges, HorizontalSpacing);
		MoveCount += EnforceSharedTargetDataSourceOrder(Nodes, Edges, IncomingEdges, OutgoingEdges, NodePadding);
		return MoveCount;
	}

	static void ApplyPortConstraintLayeredLayout(
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const TArray<TArray<int32>>& Components,
		const double OriginX,
		const double OriginY,
		const double HorizontalSpacing,
		const double VerticalSpacing,
		const double NodePadding,
		const int32 MaxIterations,
		int32& OutRankCount,
		FBlueprintLayoutAlignmentStats* OutAlignmentStats = nullptr)
	{
		OutRankCount = 0;
		if (OutAlignmentStats)
		{
			*OutAlignmentStats = FBlueprintLayoutAlignmentStats();
		}
		TArray<TArray<int32>> IncomingEdges;
		TArray<TArray<int32>> OutgoingEdges;
		BuildIncidentEdgeLists(Nodes, Edges, IncomingEdges, OutgoingEdges);

		double ComponentY = OriginY;
		for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
		{
			const TArray<int32>& Component = Components[ComponentIndex];
			if (Component.Num() == 0)
			{
				continue;
			}
			for (const int32 NodeIndex : Component)
			{
				if (Nodes.IsValidIndex(NodeIndex))
				{
					Nodes[NodeIndex].Component = ComponentIndex;
				}
			}
			AssignRanksForComponent(Nodes, Edges, IncomingEdges, OutgoingEdges, Component);
			TArray<TArray<int32>> Layers = BuildLayersForComponent(Nodes, Component);
			MinimizeCrossingsWithPortConstraints(Nodes, Edges, IncomingEdges, OutgoingEdges, Layers, MaxIterations);
			PlaceComponentLayers(
				Nodes,
				Edges,
				IncomingEdges,
				OutgoingEdges,
				Layers,
				OriginX,
				ComponentY,
				HorizontalSpacing,
				NodePadding,
				OutRankCount);

			TArray<int32> ComponentNodes;
			for (const TArray<int32>& Layer : Layers)
			{
				ComponentNodes.Append(Layer);
			}
			const FBlueprintLayoutRect Bounds = MakeLayoutBounds(Nodes, ComponentNodes, true);
			ComponentY = Bounds.Bottom + FMath::Max(VerticalSpacing, NodePadding * 5.0);
		}

		for (int32 Pass = 0; Pass < 12; ++Pass)
		{
			const int32 MoveCount = EnforcePortOrderHardConstraints(Nodes, Edges, NodePadding);
			if (MoveCount == 0)
			{
				break;
			}
		}
		DockSingleConsumerDataSubgraphs(Nodes, Edges, OutgoingEdges, NodePadding);
		for (int32 Pass = 0; Pass < 4; ++Pass)
		{
			const int32 MoveCount = EnforcePortOrderHardConstraints(Nodes, Edges, NodePadding);
			if (MoveCount == 0)
			{
				break;
			}
		}
		StraightenLinearExecChains(Nodes, Edges, IncomingEdges, OutgoingEdges, NodePadding);
		ApplyControlTreeLayoutRefinement(Nodes, Edges, IncomingEdges, OutgoingEdges, HorizontalSpacing, NodePadding, OutAlignmentStats);
		for (int32 Pass = 0; Pass < 4; ++Pass)
		{
			const int32 MoveCount = ResolveGlobalOverlaps(Nodes, NodePadding);
			if (MoveCount == 0)
			{
				break;
			}
		}
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			DockSingleConsumerDataSubgraphs(Nodes, Edges, OutgoingEdges, NodePadding, true);
			EnforceSharedTargetDataSourceOrder(Nodes, Edges, IncomingEdges, OutgoingEdges, NodePadding);
			const int32 MoveCount = ResolveGlobalOverlaps(Nodes, NodePadding);
			if (MoveCount == 0)
			{
				break;
			}
		}
		int32 FinalSingleConsumerCandidateCount = 0;
		const int32 FinalSingleConsumerMoves = ApplyFinalSingleConsumerPinAlignment(
			Nodes,
			Edges,
			IncomingEdges,
			OutgoingEdges,
			NodePadding,
			&FinalSingleConsumerCandidateCount);
		if (OutAlignmentStats)
		{
			OutAlignmentStats->FinalSingleConsumerPinAlignmentCandidateCount = FinalSingleConsumerCandidateCount;
			OutAlignmentStats->FinalSingleConsumerPinAlignmentMoveCount = FinalSingleConsumerMoves;
		}
		(void)ApplyControlExecLaneRestore(Nodes, Edges, IncomingEdges, OutgoingEdges, HorizontalSpacing, false);
		(void)ResolveBackwardExecEdges(Nodes, Edges, HorizontalSpacing);
		int32 ExecInputExpressionIslandCandidateCount = 0;
		const int32 ExecInputExpressionIslandMoveCount = DockExecInputExpressionIslandsToExecTargets(
			Nodes,
			Edges,
			IncomingEdges,
			OutgoingEdges,
			HorizontalSpacing,
			NodePadding,
			&ExecInputExpressionIslandCandidateCount);
		if (OutAlignmentStats)
		{
			OutAlignmentStats->ExecInputExpressionIslandCandidateCount = ExecInputExpressionIslandCandidateCount;
			OutAlignmentStats->ExecInputExpressionIslandMoveCount = ExecInputExpressionIslandMoveCount;
		}
		int32 SharedExecInputFanoutCandidateCount = 0;
		const int32 SharedExecInputFanoutMoveCount = DockSharedExecInputFanoutDataIslandsToExecTargets(
			Nodes,
			Edges,
			IncomingEdges,
			OutgoingEdges,
			HorizontalSpacing,
			NodePadding,
			&SharedExecInputFanoutCandidateCount);
		if (OutAlignmentStats)
		{
			OutAlignmentStats->SharedExecInputFanoutDockingCandidateCount = SharedExecInputFanoutCandidateCount;
			OutAlignmentStats->SharedExecInputFanoutDockingMoveCount = SharedExecInputFanoutMoveCount;
		}
		int32 DirectLeafDockingCandidateCount = 0;
		const int32 DirectLeafDockingMoveCount = DockDirectLeafDataInputsToExecTargets(
			Nodes,
			Edges,
			IncomingEdges,
			OutgoingEdges,
			HorizontalSpacing,
			NodePadding,
			&DirectLeafDockingCandidateCount);
		if (OutAlignmentStats)
		{
			OutAlignmentStats->DirectLeafExecInputDockingCandidateCount = DirectLeafDockingCandidateCount;
			OutAlignmentStats->DirectLeafExecInputDockingMoveCount = DirectLeafDockingMoveCount;
		}
		(void)ResolveBackwardDataEdgesForPureNodes(Nodes, Edges, HorizontalSpacing);
		(void)ResolveBackwardDataEdgesIntoExecTargets(Nodes, Edges, HorizontalSpacing);
		(void)EnforceSharedTargetDataSourceOrder(Nodes, Edges, IncomingEdges, OutgoingEdges, NodePadding);
		(void)ResolveNonExecOverlapsPreservingExec(Nodes, NodePadding);
		int32 PostSharedExecInputFanoutCandidateCount = 0;
		const int32 PostSharedExecInputFanoutMoveCount = DockSharedExecInputFanoutDataIslandsToExecTargets(
			Nodes,
			Edges,
			IncomingEdges,
			OutgoingEdges,
			HorizontalSpacing,
			NodePadding,
			&PostSharedExecInputFanoutCandidateCount);
		int32 PostExecInputExpressionIslandCandidateCount = 0;
		const int32 PostExecInputExpressionIslandMoveCount = DockExecInputExpressionIslandsToExecTargets(
			Nodes,
			Edges,
			IncomingEdges,
			OutgoingEdges,
			HorizontalSpacing,
			NodePadding,
			&PostExecInputExpressionIslandCandidateCount);
		int32 PostDirectLeafDockingCandidateCount = 0;
		const int32 PostDirectLeafDockingMoveCount = DockDirectLeafDataInputsToExecTargets(
			Nodes,
			Edges,
			IncomingEdges,
			OutgoingEdges,
			HorizontalSpacing,
			NodePadding,
			&PostDirectLeafDockingCandidateCount);
		if (OutAlignmentStats)
		{
			OutAlignmentStats->SharedExecInputFanoutDockingCandidateCount = FMath::Max(
				OutAlignmentStats->SharedExecInputFanoutDockingCandidateCount,
				PostSharedExecInputFanoutCandidateCount);
			OutAlignmentStats->SharedExecInputFanoutDockingMoveCount += PostSharedExecInputFanoutMoveCount;
			OutAlignmentStats->ExecInputExpressionIslandCandidateCount = FMath::Max(
				OutAlignmentStats->ExecInputExpressionIslandCandidateCount,
				PostExecInputExpressionIslandCandidateCount);
			OutAlignmentStats->ExecInputExpressionIslandMoveCount += PostExecInputExpressionIslandMoveCount;
			OutAlignmentStats->DirectLeafExecInputDockingCandidateCount = FMath::Max(
				OutAlignmentStats->DirectLeafExecInputDockingCandidateCount,
				PostDirectLeafDockingCandidateCount);
			OutAlignmentStats->DirectLeafExecInputDockingMoveCount += PostDirectLeafDockingMoveCount;
		}
	}

	static UEdGraphPin* FindLayoutGraphPin(
		UEdGraphNode* Node,
		const FString& PinName,
		const EEdGraphPinDirection Direction,
		const int32 PinIndex)
	{
		if (!Node)
		{
			return nullptr;
		}
		UEdGraphPin* FallbackPin = nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != Direction || Pin->PinName.ToString() != PinName)
			{
				continue;
			}
			if (!FallbackPin)
			{
				FallbackPin = Pin;
			}
			if (PinIndex == INDEX_NONE || GetPinIndexInDirection(Pin) == PinIndex)
			{
				return Pin;
			}
		}
		return FallbackPin;
	}

	static int32 CountPolylineNodeIntersections(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const FBlueprintLayoutEdge& Edge,
		const TArray<FVector2D>& Points)
	{
		int32 Count = 0;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (!Nodes.IsValidIndex(NodeIndex)
				|| Nodes[NodeIndex].bRerouteNode
				|| NodeIndex == Edge.Source
				|| NodeIndex == Edge.Target)
			{
				continue;
			}
			const FBlueprintLayoutRect Rect = MakeNodeRect(Nodes[NodeIndex], true);
			for (int32 SegmentIndex = 0; SegmentIndex + 1 < Points.Num(); ++SegmentIndex)
			{
				if (SegmentIntersectsRect(Points[SegmentIndex], Points[SegmentIndex + 1], Rect))
				{
					++Count;
					break;
				}
			}
		}
		return Count;
	}

	static TArray<int32> CollectPolylineHitNodeIndices(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const FBlueprintLayoutEdge& Edge,
		const TArray<FVector2D>& Points,
		const int32 MaxHits = 8)
	{
		TArray<int32> HitNodeIndices;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (!Nodes.IsValidIndex(NodeIndex)
				|| Nodes[NodeIndex].bRerouteNode
				|| NodeIndex == Edge.Source
				|| NodeIndex == Edge.Target)
			{
				continue;
			}
			const FBlueprintLayoutRect Rect = MakeNodeRect(Nodes[NodeIndex], true);
			for (int32 SegmentIndex = 0; SegmentIndex + 1 < Points.Num(); ++SegmentIndex)
			{
				if (SegmentIntersectsRect(Points[SegmentIndex], Points[SegmentIndex + 1], Rect))
				{
					HitNodeIndices.Add(NodeIndex);
					break;
				}
			}
			if (HitNodeIndices.Num() >= MaxHits)
			{
				break;
			}
		}
		return HitNodeIndices;
	}

	static int32 CountPolylineEdgeCrossings(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const int32 RoutedEdgeIndex,
		const TArray<FVector2D>& RoutedPoints)
	{
		if (!Edges.IsValidIndex(RoutedEdgeIndex))
		{
			return 0;
		}
		const FBlueprintLayoutEdge& RoutedEdge = Edges[RoutedEdgeIndex];
		int32 Count = 0;
		for (int32 OtherIndex = 0; OtherIndex < Edges.Num(); ++OtherIndex)
		{
			if (OtherIndex == RoutedEdgeIndex || !Edges.IsValidIndex(OtherIndex))
			{
				continue;
			}
			const FBlueprintLayoutEdge& OtherEdge = Edges[OtherIndex];
			if (!Nodes.IsValidIndex(OtherEdge.Source)
				|| !Nodes.IsValidIndex(OtherEdge.Target)
				|| OtherEdge.Source == RoutedEdge.Source
				|| OtherEdge.Target == RoutedEdge.Target
				|| OtherEdge.Source == RoutedEdge.Target
				|| OtherEdge.Target == RoutedEdge.Source)
			{
				continue;
			}

			const TArray<FVector2D> OtherPoints = BuildDefaultEdgePolyline(Nodes, OtherEdge, true);
			for (int32 SegmentA = 0; SegmentA + 1 < RoutedPoints.Num(); ++SegmentA)
			{
				for (int32 SegmentB = 0; SegmentB + 1 < OtherPoints.Num(); ++SegmentB)
				{
					if (SegmentIntersectsSegment(
						RoutedPoints[SegmentA],
						RoutedPoints[SegmentA + 1],
						OtherPoints[SegmentB],
						OtherPoints[SegmentB + 1]))
					{
						++Count;
					}
				}
			}
		}
		return Count;
	}

	static int32 CountPolylineEdgeCrossingsWithPlanMap(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& Edges,
		const int32 RoutedEdgeIndex,
		const TArray<FVector2D>& RoutedPoints,
		const TMap<int32, FBlueprintLayoutReroutePlan>& PlanByEdge)
	{
		if (!Edges.IsValidIndex(RoutedEdgeIndex))
		{
			return 0;
		}
		const FBlueprintLayoutEdge& RoutedEdge = Edges[RoutedEdgeIndex];
		int32 Count = 0;
		for (int32 OtherIndex = 0; OtherIndex < Edges.Num(); ++OtherIndex)
		{
			if (OtherIndex == RoutedEdgeIndex || !Edges.IsValidIndex(OtherIndex))
			{
				continue;
			}
			const FBlueprintLayoutEdge& OtherEdge = Edges[OtherIndex];
			if (!Nodes.IsValidIndex(OtherEdge.Source)
				|| !Nodes.IsValidIndex(OtherEdge.Target)
				|| OtherEdge.Source == RoutedEdge.Source
				|| OtherEdge.Target == RoutedEdge.Target
				|| OtherEdge.Source == RoutedEdge.Target
				|| OtherEdge.Target == RoutedEdge.Source)
			{
				continue;
			}

			const FBlueprintLayoutReroutePlan* ExistingPlan = PlanByEdge.Find(OtherIndex);
			const TArray<FVector2D> OtherPoints = BuildReroutePlanPolyline(Nodes, OtherEdge, ExistingPlan, true);
			for (int32 SegmentA = 0; SegmentA + 1 < RoutedPoints.Num(); ++SegmentA)
			{
				for (int32 SegmentB = 0; SegmentB + 1 < OtherPoints.Num(); ++SegmentB)
				{
					if (SegmentIntersectsSegment(
						RoutedPoints[SegmentA],
						RoutedPoints[SegmentA + 1],
						OtherPoints[SegmentB],
						OtherPoints[SegmentB + 1]))
					{
						++Count;
					}
				}
			}
		}
		return Count;
	}

	static FVector2D SnapLayoutBusPoint(const FVector2D& Point)
	{
		constexpr double Grid = 8.0;
		return FVector2D(
			FMath::GridSnap(Point.X, Grid),
			FMath::GridSnap(Point.Y, Grid));
	}

	static void RefreshReroutePlanEndpointFields(FBlueprintLayoutReroutePlan& Plan)
	{
		if (Plan.ReroutePoints.Num() > 0)
		{
			Plan.FirstPoint = Plan.ReroutePoints[0];
		}
		if (Plan.ReroutePoints.Num() > 1)
		{
			Plan.SecondPoint = Plan.ReroutePoints[1];
		}
	}

	static void CompactConsecutiveReroutePoints(FBlueprintLayoutReroutePlan& Plan)
	{
		TArray<FVector2D> CleanPoints;
		for (const FVector2D& RawPoint : Plan.ReroutePoints)
		{
			const FVector2D Point = SnapLayoutBusPoint(RawPoint);
			if (CleanPoints.Num() == 0
				|| LayoutManhattanDistance(CleanPoints.Last(), Point) > 10.0)
			{
				CleanPoints.Add(Point);
			}
		}
		Plan.ReroutePoints = MoveTemp(CleanPoints);
		RefreshReroutePlanEndpointFields(Plan);
	}

	static int32 MergeNearbyBusTrunkPoints(TArray<FBlueprintLayoutReroutePlan>& Plans, const double HorizontalSpacing)
	{
		struct FBusPointRef
		{
			int32 PlanIndex = INDEX_NONE;
			int32 PointIndex = INDEX_NONE;
			FVector2D Point = FVector2D::ZeroVector;
		};

		TMap<FString, TArray<FBusPointRef>> PointsByBus;
		for (int32 PlanIndex = 0; PlanIndex < Plans.Num(); ++PlanIndex)
		{
			if (!Plans.IsValidIndex(PlanIndex)
				|| Plans[PlanIndex].BusId.IsEmpty()
				|| Plans[PlanIndex].ReroutePoints.Num() < 2)
			{
				continue;
			}
			for (int32 PointIndex = 0; PointIndex < 2; ++PointIndex)
			{
				FBusPointRef Ref;
				Ref.PlanIndex = PlanIndex;
				Ref.PointIndex = PointIndex;
				Ref.Point = Plans[PlanIndex].ReroutePoints[PointIndex];
				PointsByBus.FindOrAdd(Plans[PlanIndex].BusId).Add(Ref);
			}
		}

		int32 MergeCount = 0;
		const double MergeDistance = FMath::Clamp(HorizontalSpacing * 0.35, 80.0, 160.0);
		const double LaneTolerance = FMath::Clamp(HorizontalSpacing * 0.12, 24.0, 44.0);
		for (TPair<FString, TArray<FBusPointRef>>& Pair : PointsByBus)
		{
			TArray<FBusPointRef>& BusPoints = Pair.Value;
			BusPoints.Sort([](const FBusPointRef& A, const FBusPointRef& B)
			{
				if (!FMath::IsNearlyEqual(A.Point.Y, B.Point.Y, 1.0))
				{
					return A.Point.Y < B.Point.Y;
				}
				return A.Point.X < B.Point.X;
			});

			for (int32 StartIndex = 0; StartIndex < BusPoints.Num();)
			{
				int32 EndIndex = StartIndex + 1;
				double SumX = BusPoints[StartIndex].Point.X;
				double SumY = BusPoints[StartIndex].Point.Y;
				while (EndIndex < BusPoints.Num()
					&& FMath::Abs(BusPoints[EndIndex].Point.Y - BusPoints[StartIndex].Point.Y) <= LaneTolerance
					&& FMath::Abs(BusPoints[EndIndex].Point.X - BusPoints[EndIndex - 1].Point.X) <= MergeDistance)
				{
					SumX += BusPoints[EndIndex].Point.X;
					SumY += BusPoints[EndIndex].Point.Y;
					++EndIndex;
				}
				const int32 ClusterCount = EndIndex - StartIndex;
				if (ClusterCount > 1)
				{
					const FVector2D Center = SnapLayoutBusPoint(FVector2D(SumX / ClusterCount, SumY / ClusterCount));
					for (int32 Index = StartIndex; Index < EndIndex; ++Index)
					{
						const FBusPointRef& Ref = BusPoints[Index];
						if (Plans.IsValidIndex(Ref.PlanIndex) && Plans[Ref.PlanIndex].ReroutePoints.IsValidIndex(Ref.PointIndex))
						{
							Plans[Ref.PlanIndex].ReroutePoints[Ref.PointIndex] = Center;
							++MergeCount;
						}
					}
				}
				StartIndex = EndIndex;
			}
		}

		for (FBlueprintLayoutReroutePlan& Plan : Plans)
		{
			RefreshReroutePlanEndpointFields(Plan);
		}
		return MergeCount;
	}

	static bool LayoutIntervalsOverlap(const double ATop, const double ABottom, const double BTop, const double BBottom, const double Tolerance)
	{
		return FMath::Max(ATop, BTop) <= FMath::Min(ABottom, BBottom) + Tolerance;
	}

	static int32 StaggerOverlappingBusBranches(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& RawEdges,
		TArray<FBlueprintLayoutReroutePlan>& Plans,
		const double HorizontalSpacing)
	{
		struct FBranchRecord
		{
			int32 PlanIndex = INDEX_NONE;
			double Top = 0.0;
			double Bottom = 0.0;
			double OriginalX = 0.0;
			double MinX = 0.0;
			double MaxX = 0.0;
			double TargetY = 0.0;
			FString TargetGuid;
		};

		TMap<FString, TArray<FBranchRecord>> BranchesByBus;
		for (int32 PlanIndex = 0; PlanIndex < Plans.Num(); ++PlanIndex)
		{
			if (!Plans.IsValidIndex(PlanIndex)
				|| Plans[PlanIndex].BusId.IsEmpty()
				|| Plans[PlanIndex].ReroutePoints.Num() < 2
				|| !RawEdges.IsValidIndex(Plans[PlanIndex].RawEdgeIndex))
			{
				continue;
			}
			const FBlueprintLayoutEdge& Edge = RawEdges[Plans[PlanIndex].RawEdgeIndex];
			if (!Nodes.IsValidIndex(Edge.Target))
			{
				continue;
			}
			const FVector2D End = GetEdgeTargetPoint(Nodes, Edge, true);
			const double TrunkY = Plans[PlanIndex].ReroutePoints[1].Y;
			FBranchRecord Record;
			Record.PlanIndex = PlanIndex;
			Record.Top = FMath::Min(TrunkY, End.Y);
			Record.Bottom = FMath::Max(TrunkY, End.Y);
			Record.OriginalX = Plans[PlanIndex].ReroutePoints[1].X;
			Record.MinX = Plans[PlanIndex].ReroutePoints[0].X + FMath::Clamp(HorizontalSpacing * 0.38, 96.0, 180.0);
			Record.MaxX = End.X - FMath::Clamp(HorizontalSpacing * 0.20, 64.0, 120.0);
			Record.TargetY = End.Y;
			Record.TargetGuid = Nodes[Edge.Target].Guid;
			BranchesByBus.FindOrAdd(Plans[PlanIndex].BusId).Add(Record);
		}

		int32 StaggerCount = 0;
		const double BranchGap = FMath::Clamp(HorizontalSpacing * 0.18, 48.0, 90.0);
		const double IntervalTolerance = FMath::Clamp(HorizontalSpacing * 0.10, 20.0, 36.0);
		for (TPair<FString, TArray<FBranchRecord>>& Pair : BranchesByBus)
		{
			TArray<FBranchRecord>& Branches = Pair.Value;
			if (Branches.Num() < 2)
			{
				continue;
			}
			Branches.Sort([](const FBranchRecord& A, const FBranchRecord& B)
			{
				if (!FMath::IsNearlyEqual(A.TargetY, B.TargetY, 1.0))
				{
					return A.TargetY < B.TargetY;
				}
				if (!FMath::IsNearlyEqual(A.OriginalX, B.OriginalX, 1.0))
				{
					return A.OriginalX < B.OriginalX;
				}
				return A.TargetGuid < B.TargetGuid;
			});

			TArray<FBranchRecord> Placed;
			TMap<int32, int32> LaneByPlan;
			for (const FBranchRecord& Branch : Branches)
			{
				int32 Lane = 0;
				while (true)
				{
					bool bLaneConflict = false;
					for (const FBranchRecord& Other : Placed)
					{
						const int32* OtherLane = LaneByPlan.Find(Other.PlanIndex);
						if (OtherLane
							&& *OtherLane == Lane
							&& LayoutIntervalsOverlap(Branch.Top, Branch.Bottom, Other.Top, Other.Bottom, IntervalTolerance))
						{
							bLaneConflict = true;
							break;
						}
					}
					if (!bLaneConflict)
					{
						break;
					}
					++Lane;
				}
				LaneByPlan.Add(Branch.PlanIndex, Lane);
				Placed.Add(Branch);
			}

			for (const FBranchRecord& Branch : Branches)
			{
				const int32 Lane = LaneByPlan.FindRef(Branch.PlanIndex);
				if (Lane <= 0 || !Plans.IsValidIndex(Branch.PlanIndex) || Plans[Branch.PlanIndex].ReroutePoints.Num() < 2)
				{
					continue;
				}
				const int32 Side = (Lane % 2 == 1) ? 1 : -1;
				const int32 Step = (Lane + 1) / 2;
				double DesiredX = Branch.OriginalX + static_cast<double>(Side * Step) * BranchGap;
				if (Branch.MinX < Branch.MaxX)
				{
					DesiredX = FMath::Clamp(DesiredX, Branch.MinX, Branch.MaxX);
				}
				if (FMath::IsNearlyEqual(DesiredX, Branch.OriginalX, 1.0))
				{
					continue;
				}
				FBlueprintLayoutReroutePlan& Plan = Plans[Branch.PlanIndex];
				const double OldBranchX = Plan.ReroutePoints[1].X;
				Plan.ReroutePoints[1].X = DesiredX;
				for (int32 PointIndex = 2; PointIndex < Plan.ReroutePoints.Num(); ++PointIndex)
				{
					if (FMath::Abs(Plan.ReroutePoints[PointIndex].X - OldBranchX) <= 8.0)
					{
						Plan.ReroutePoints[PointIndex].X = DesiredX;
					}
				}
				++StaggerCount;
			}
		}

		for (FBlueprintLayoutReroutePlan& Plan : Plans)
		{
			RefreshReroutePlanEndpointFields(Plan);
		}
		return StaggerCount;
	}

	static int32 PlanExternalBusLanes(
		const TArray<FBlueprintLayoutNode>& Nodes,
		TArray<FBlueprintLayoutReroutePlan>& Plans,
		const double HorizontalSpacing)
	{
		struct FExternalBusRecord
		{
			FString BusId;
			TArray<int32> PlanIndices;
			double Left = TNumericLimits<double>::Max();
			double Right = -TNumericLimits<double>::Max();
			double DesiredY = 0.0;
			bool bUpper = true;
			int32 LaneIndex = 0;
			double LaneY = 0.0;
		};

		TArray<int32> NonRerouteNodeIndices;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (Nodes.IsValidIndex(NodeIndex) && !Nodes[NodeIndex].bRerouteNode)
			{
				NonRerouteNodeIndices.Add(NodeIndex);
			}
		}
		const FBlueprintLayoutRect GraphBounds = MakeLayoutBounds(Nodes, NonRerouteNodeIndices, true);
		const double GraphCenterY = (GraphBounds.Top + GraphBounds.Bottom) * 0.5;

		TMap<FString, FExternalBusRecord> RecordsByBus;
		for (int32 PlanIndex = 0; PlanIndex < Plans.Num(); ++PlanIndex)
		{
			if (!Plans.IsValidIndex(PlanIndex)
				|| Plans[PlanIndex].RouteKind != EBlueprintLayoutRouteKind::ExternalBus
				|| Plans[PlanIndex].ReroutePoints.Num() < 2)
			{
				continue;
			}

			FBlueprintLayoutReroutePlan& Plan = Plans[PlanIndex];
			const FString BusKey = Plan.BusId.IsEmpty()
				? FString::Printf(TEXT("external_plan_%d"), PlanIndex)
				: Plan.BusId;
			const FVector2D First = Plan.ReroutePoints[0];
			const FVector2D Second = Plan.ReroutePoints[1];
			const double TrunkY = (First.Y + Second.Y) * 0.5;

			FExternalBusRecord& Record = RecordsByBus.FindOrAdd(BusKey);
			if (Record.PlanIndices.Num() == 0)
			{
				Record.BusId = BusKey;
				Record.DesiredY = TrunkY;
				Record.bUpper = TrunkY <= GraphCenterY;
			}
			else
			{
				Record.DesiredY = (Record.DesiredY * static_cast<double>(Record.PlanIndices.Num()) + TrunkY)
					/ static_cast<double>(Record.PlanIndices.Num() + 1);
				Record.bUpper = Record.DesiredY <= GraphCenterY;
			}
			Record.PlanIndices.Add(PlanIndex);
			Record.Left = FMath::Min(Record.Left, FMath::Min(First.X, Second.X));
			Record.Right = FMath::Max(Record.Right, FMath::Max(First.X, Second.X));
		}

		TArray<FExternalBusRecord> Records;
		RecordsByBus.GenerateValueArray(Records);
		if (Records.Num() == 0)
		{
			return 0;
		}

		TArray<int32> UpperRecords;
		TArray<int32> LowerRecords;
		for (int32 RecordIndex = 0; RecordIndex < Records.Num(); ++RecordIndex)
		{
			if (Records[RecordIndex].bUpper)
			{
				UpperRecords.Add(RecordIndex);
			}
			else
			{
				LowerRecords.Add(RecordIndex);
			}
		}

		const double LaneGap = FMath::Clamp(HorizontalSpacing * 0.16, 48.0, 84.0);
		const double ProjectionTolerance = FMath::Clamp(HorizontalSpacing * 0.05, 12.0, 28.0);

		auto AssignSideLanes = [&Records, LaneGap, ProjectionTolerance](TArray<int32>& SideRecords, const bool bUpper)
		{
			SideRecords.Sort([&Records](const int32 A, const int32 B)
			{
				const FExternalBusRecord& RecordA = Records[A];
				const FExternalBusRecord& RecordB = Records[B];
				if (!FMath::IsNearlyEqual(RecordA.Left, RecordB.Left, 1.0))
				{
					return RecordA.Left < RecordB.Left;
				}
				if (!FMath::IsNearlyEqual(RecordA.Right, RecordB.Right, 1.0))
				{
					return RecordA.Right < RecordB.Right;
				}
				return RecordA.BusId < RecordB.BusId;
			});

			TArray<double> LaneLastRight;
			TArray<int32> AssignedRecords;
			for (const int32 RecordIndex : SideRecords)
			{
				FExternalBusRecord& Record = Records[RecordIndex];
				int32 LaneIndex = 0;
				for (; LaneIndex < LaneLastRight.Num(); ++LaneIndex)
				{
					if (Record.Left > LaneLastRight[LaneIndex] + ProjectionTolerance)
					{
						break;
					}
				}
				if (LaneIndex == LaneLastRight.Num())
				{
					LaneLastRight.Add(-TNumericLimits<double>::Max());
				}
				LaneLastRight[LaneIndex] = FMath::Max(LaneLastRight[LaneIndex], Record.Right);
				Record.LaneIndex = LaneIndex;
				const double Direction = bUpper ? -1.0 : 1.0;
				Record.LaneY = Record.DesiredY + Direction * static_cast<double>(LaneIndex) * LaneGap;
				bool bConflicts = true;
				while (bConflicts)
				{
					bConflicts = false;
					for (const int32 AssignedIndex : AssignedRecords)
					{
						if (!Records.IsValidIndex(AssignedIndex))
						{
							continue;
						}
						const FExternalBusRecord& Assigned = Records[AssignedIndex];
						const bool bOverlapsX = Record.Left <= Assigned.Right + ProjectionTolerance
							&& Assigned.Left <= Record.Right + ProjectionTolerance;
						if (bOverlapsX && FMath::Abs(Record.LaneY - Assigned.LaneY) < LaneGap * 0.72)
						{
							Record.LaneY += Direction * LaneGap;
							bConflicts = true;
							break;
						}
					}
				}
				AssignedRecords.Add(RecordIndex);
			}
		};

		AssignSideLanes(UpperRecords, true);
		AssignSideLanes(LowerRecords, false);

		int32 AdjustedCount = 0;
		for (const FExternalBusRecord& Record : Records)
		{
			const FVector2D SnappedLanePoint = SnapLayoutBusPoint(FVector2D(0.0, Record.LaneY));
			const double LaneY = SnappedLanePoint.Y;
			for (const int32 PlanIndex : Record.PlanIndices)
			{
				if (!Plans.IsValidIndex(PlanIndex) || Plans[PlanIndex].ReroutePoints.Num() < 2)
				{
					continue;
				}
				FBlueprintLayoutReroutePlan& Plan = Plans[PlanIndex];
				const double OldTrunkY = (Plan.ReroutePoints[0].Y + Plan.ReroutePoints[1].Y) * 0.5;
				if (!FMath::IsNearlyEqual(OldTrunkY, LaneY, 1.0))
				{
					++AdjustedCount;
				}
				Plan.ReroutePoints[0].Y = LaneY;
				Plan.ReroutePoints[1].Y = LaneY;
				for (int32 PointIndex = 2; PointIndex < Plan.ReroutePoints.Num(); ++PointIndex)
				{
					if (FMath::Abs(Plan.ReroutePoints[PointIndex].Y - OldTrunkY) <= 8.0)
					{
						Plan.ReroutePoints[PointIndex].Y = LaneY;
					}
				}
				Plan.BusLaneIndex = Record.LaneIndex;
				Plan.BusLaneSide = Record.bUpper ? TEXT("upper") : TEXT("lower");
				RefreshReroutePlanEndpointFields(Plan);
			}
		}
		return AdjustedCount;
	}

	static int32 RemoveTrivialLocalBusPlans(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& RawEdges,
		TArray<FBlueprintLayoutReroutePlan>& Plans,
		const double HorizontalSpacing)
	{
		TMap<FString, int32> BusCounts;
		for (const FBlueprintLayoutReroutePlan& Plan : Plans)
		{
			if (!Plan.BusId.IsEmpty())
			{
				++BusCounts.FindOrAdd(Plan.BusId);
			}
		}

		int32 RemovedCount = 0;
		Plans.RemoveAll([&](const FBlueprintLayoutReroutePlan& Plan)
		{
			if (Plan.RouteKind != EBlueprintLayoutRouteKind::LocalBus
				|| !RawEdges.IsValidIndex(Plan.RawEdgeIndex)
				|| Plan.ReroutePoints.Num() < 2)
			{
				return false;
			}
			const int32 BusCount = Plan.BusId.IsEmpty() ? 1 : BusCounts.FindRef(Plan.BusId);
			if (BusCount > 1)
			{
				return false;
			}
			const FBlueprintLayoutEdge& Edge = RawEdges[Plan.RawEdgeIndex];
			if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
			{
				return false;
			}
			const FVector2D Start = GetEdgeSourcePoint(Nodes, Edge, true);
			const FVector2D End = GetEdgeTargetPoint(Nodes, Edge, true);
			const double Dx = End.X - Start.X;
			const double Dy = End.Y - Start.Y;
			const bool bBackward = Dx < -1.0;
			const double MiddleDistance = LayoutManhattanDistance(Plan.ReroutePoints[0], Plan.ReroutePoints.Last());
			if (!bBackward && MiddleDistance < FMath::Max(160.0, HorizontalSpacing * 0.70))
			{
				++RemovedCount;
				return true;
			}
			const bool bShortEnough = FMath::Abs(Dx) + FMath::Abs(Dy) < FMath::Max(760.0, HorizontalSpacing * 2.6);
			const bool bDirectHitsNodes = CountPolylineNodeIntersections(Nodes, Edge, BuildDefaultEdgePolyline(Nodes, Edge, true)) > 0;
			if (!bBackward && bShortEnough && !bDirectHitsNodes)
			{
				++RemovedCount;
				return true;
			}
			return false;
		});
		return RemovedCount;
	}

	static void FinalizeRerouteBusPlans(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& RawEdges,
		TArray<FBlueprintLayoutReroutePlan>& Plans,
		const double HorizontalSpacing,
		const double NodePadding)
	{
		for (FBlueprintLayoutReroutePlan& Plan : Plans)
		{
			CompactConsecutiveReroutePoints(Plan);
		}
		MergeNearbyBusTrunkPoints(Plans, HorizontalSpacing);
		StaggerOverlappingBusBranches(Nodes, RawEdges, Plans, HorizontalSpacing);
		for (FBlueprintLayoutReroutePlan& Plan : Plans)
		{
			CompactConsecutiveReroutePoints(Plan);
		}
		RemoveTrivialLocalBusPlans(Nodes, RawEdges, Plans, HorizontalSpacing);
		TMap<FString, int32> BusCounts;
		for (const FBlueprintLayoutReroutePlan& Plan : Plans)
		{
			if (!Plan.BusId.IsEmpty())
			{
				++BusCounts.FindOrAdd(Plan.BusId);
			}
		}
		Plans.RemoveAll([NodePadding, &BusCounts](const FBlueprintLayoutReroutePlan& Plan)
		{
			if (Plan.ReroutePoints.Num() < 2)
			{
				return true;
			}
			const int32 BusCount = Plan.BusId.IsEmpty() ? 1 : BusCounts.FindRef(Plan.BusId);
			if (BusCount > 1)
			{
				return false;
			}
			const double MiddleDistance = LayoutManhattanDistance(Plan.ReroutePoints[0], Plan.ReroutePoints.Last());
			return MiddleDistance < FMath::Max(72.0, NodePadding * 2.0);
		});
		for (FBlueprintLayoutReroutePlan& Plan : Plans)
		{
			RefreshReroutePlanEndpointFields(Plan);
		}
	}

	static TArray<FBlueprintLayoutReroutePlan> BuildRerouteInsertionPlan(
		const TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& RawEdges,
		const double HorizontalSpacing,
		const double NodePadding,
		const int32 MaxInsertedEdges = 64)
	{
		TArray<FBlueprintLayoutReroutePlan> Plans;
		TSet<int32> PlannedEdges;
		TMap<int32, FBlueprintLayoutReroutePlan> PlannedRouteByEdge;
		struct FPlanAlternativeSet
		{
			int32 RawEdgeIndex = INDEX_NONE;
			TArray<FBlueprintLayoutReroutePlan> Candidates;
		};
		TArray<FPlanAlternativeSet> AlternativeSets;

		TArray<int32> NonRerouteNodeIndices;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (Nodes.IsValidIndex(NodeIndex) && !Nodes[NodeIndex].bRerouteNode)
			{
				NonRerouteNodeIndices.Add(NodeIndex);
			}
		}
		auto ComputeLocalExternalBusYs = [&Nodes, &NonRerouteNodeIndices, HorizontalSpacing](
			const double SpanLeft,
			const double SpanRight,
			const TArray<double>& AnchorYs,
			double& OutUpperY,
			double& OutLowerY) -> bool
		{
			if (AnchorYs.Num() == 0 || SpanRight <= SpanLeft)
			{
				return false;
			}

			double LocalTop = TNumericLimits<double>::Max();
			double LocalBottom = -TNumericLimits<double>::Max();
			for (const double AnchorY : AnchorYs)
			{
				LocalTop = FMath::Min(LocalTop, AnchorY);
				LocalBottom = FMath::Max(LocalBottom, AnchorY);
			}
			if (LocalTop == TNumericLimits<double>::Max())
			{
				return false;
			}

			const double SpanPad = FMath::Clamp(HorizontalSpacing * 0.20, 64.0, 140.0);
			const double VerticalWindowPad = FMath::Clamp(HorizontalSpacing * 0.82, 220.0, 460.0);
			const double WindowTop = LocalTop - VerticalWindowPad;
			const double WindowBottom = LocalBottom + VerticalWindowPad;
			for (const int32 NodeIndex : NonRerouteNodeIndices)
			{
				if (!Nodes.IsValidIndex(NodeIndex))
				{
					continue;
				}
				const FBlueprintLayoutRect Rect = MakeNodeRect(Nodes[NodeIndex], true);
				const bool bOverlapsX = Rect.Right >= SpanLeft - SpanPad && Rect.Left <= SpanRight + SpanPad;
				const bool bNearAnchorBand = Rect.Bottom >= WindowTop && Rect.Top <= WindowBottom;
				if (!bOverlapsX || !bNearAnchorBand)
				{
					continue;
				}
				LocalTop = FMath::Min(LocalTop, Rect.Top);
				LocalBottom = FMath::Max(LocalBottom, Rect.Bottom);
			}

			const double CorridorGap = FMath::Clamp(HorizontalSpacing * 0.46, 150.0, 300.0);
			OutUpperY = LocalTop - CorridorGap;
			OutLowerY = LocalBottom + CorridorGap;
			return IsFiniteLayoutPoint(FVector2D(SpanLeft, OutUpperY))
				&& IsFiniteLayoutPoint(FVector2D(SpanRight, OutLowerY));
		};

		TMap<FString, TArray<int32>> FanoutGroups;
		for (int32 EdgeIndex = 0; EdgeIndex < RawEdges.Num(); ++EdgeIndex)
		{
			const FBlueprintLayoutEdge& Edge = RawEdges[EdgeIndex];
			if (!Nodes.IsValidIndex(Edge.Source)
				|| !Nodes.IsValidIndex(Edge.Target)
				|| Nodes[Edge.Source].bRerouteNode
				|| Nodes[Edge.Target].bRerouteNode)
			{
				continue;
			}
			const FString Key = FString::Printf(TEXT("%d:%s:%d"), Edge.Source, *Edge.SourcePin, Edge.SourcePinIndex);
			FanoutGroups.FindOrAdd(Key).Add(EdgeIndex);
		}

		for (const TPair<FString, TArray<int32>>& Pair : FanoutGroups)
		{
			if (Plans.Num() >= MaxInsertedEdges || Pair.Value.Num() < 2)
			{
				continue;
			}
			FVector2D FirstSourcePoint = FVector2D::ZeroVector;
			bool bHasSource = false;
			bool bHasExecFanout = false;
			for (const int32 EdgeIndex : Pair.Value)
			{
				const FBlueprintLayoutEdge& Edge = RawEdges[EdgeIndex];
				const FVector2D Start = GetEdgeSourcePoint(Nodes, Edge, true);
				FirstSourcePoint = Start;
				bHasSource = true;
				if (Edge.bExec)
				{
					bHasExecFanout = true;
					break;
				}
			}
			if (!bHasSource || bHasExecFanout)
			{
				continue;
			}
			const double RemoteTargetThreshold = FMath::Max(360.0, HorizontalSpacing * 1.35);
			TArray<int32> RemoteFanoutEdges;
			for (const int32 EdgeIndex : Pair.Value)
			{
				if (!RawEdges.IsValidIndex(EdgeIndex))
				{
					continue;
				}
				const FVector2D End = GetEdgeTargetPoint(Nodes, RawEdges[EdgeIndex], true);
				if (End.X - FirstSourcePoint.X >= RemoteTargetThreshold)
				{
					RemoteFanoutEdges.Add(EdgeIndex);
				}
			}
			TArray<int32> BusFanoutEdges = RemoteFanoutEdges;
			if (BusFanoutEdges.Num() < 2)
			{
				continue;
			}
			double MinTargetX = TNumericLimits<double>::Max();
			double MaxTargetX = -TNumericLimits<double>::Max();
			double MinTargetY = TNumericLimits<double>::Max();
			double MaxTargetY = -TNumericLimits<double>::Max();
			TArray<double> TargetYs;
			for (const int32 EdgeIndex : BusFanoutEdges)
			{
				if (!RawEdges.IsValidIndex(EdgeIndex))
				{
					continue;
				}
				const FBlueprintLayoutEdge& Edge = RawEdges[EdgeIndex];
				const FVector2D End = GetEdgeTargetPoint(Nodes, Edge, true);
				MinTargetX = FMath::Min(MinTargetX, End.X);
				MaxTargetX = FMath::Max(MaxTargetX, End.X);
				MinTargetY = FMath::Min(MinTargetY, End.Y);
				MaxTargetY = FMath::Max(MaxTargetY, End.Y);
				TargetYs.Add(End.Y);
			}
			if (TargetYs.Num() < 2)
			{
				continue;
			}
			const double FanoutDistance = MaxTargetX - FirstSourcePoint.X;
			const bool bExternalFanoutCandidate = FanoutDistance > FMath::Max(1320.0, HorizontalSpacing * 4.8)
				&& (BusFanoutEdges.Num() >= 3 || MaxTargetY - MinTargetY > 260.0);
			const bool bLongFanout = FanoutDistance > FMath::Max(560.0, HorizontalSpacing * 2.0);
			const bool bTallFanout = MaxTargetY - MinTargetY > 220.0;
			const bool bWorthSharedFanout = BusFanoutEdges.Num() >= 3
				? (bLongFanout || bTallFanout || FanoutDistance > FMath::Max(520.0, HorizontalSpacing * 1.8))
				: ((bLongFanout && bTallFanout) || FanoutDistance > FMath::Max(720.0, HorizontalSpacing * 2.5));
			if (!bWorthSharedFanout)
			{
				continue;
			}
			const double ExitX = FirstSourcePoint.X + FMath::Clamp(HorizontalSpacing * 0.28, 96.0, 220.0);
			const double EntryGap = FMath::Clamp(HorizontalSpacing * 0.36, 120.0, 220.0);
			const double MinBranchSpan = FMath::Clamp(HorizontalSpacing * 0.60, 180.0, 320.0);
			const double MinEndpointStub = FMath::Clamp(HorizontalSpacing * 0.20, 64.0, 120.0);
			double TrunkEndX = ExitX + MinBranchSpan;
			TMap<int32, double> BranchXByEdge;
			for (const int32 EdgeIndex : BusFanoutEdges)
			{
				if (!RawEdges.IsValidIndex(EdgeIndex))
				{
					continue;
				}
				const FVector2D End = GetEdgeTargetPoint(Nodes, RawEdges[EdgeIndex], true);
				const double BranchX = FMath::Max(ExitX + MinBranchSpan, End.X - EntryGap);
				if (End.X - BranchX < MinEndpointStub)
				{
					continue;
				}
				BranchXByEdge.Add(EdgeIndex, BranchX);
				TrunkEndX = FMath::Max(TrunkEndX, BranchX);
			}
			BusFanoutEdges.RemoveAll([&BranchXByEdge](const int32 EdgeIndex)
			{
				return !BranchXByEdge.Contains(EdgeIndex);
			});
			if (BusFanoutEdges.Num() < 2)
			{
				continue;
			}
			TargetYs.Reset();
			MinTargetY = TNumericLimits<double>::Max();
			MaxTargetY = -TNumericLimits<double>::Max();
			for (const int32 EdgeIndex : BusFanoutEdges)
			{
				if (!RawEdges.IsValidIndex(EdgeIndex))
				{
					continue;
				}
				const FVector2D End = GetEdgeTargetPoint(Nodes, RawEdges[EdgeIndex], true);
				MinTargetY = FMath::Min(MinTargetY, End.Y);
				MaxTargetY = FMath::Max(MaxTargetY, End.Y);
				TargetYs.Add(End.Y);
			}
			if (TargetYs.Num() < 2)
			{
				continue;
			}
			double TrunkY = 0.0;
			bool bExternalFanout = false;
			if (!FindSharedFanoutTrunkY(Nodes, ExitX, TrunkEndX, FirstSourcePoint.Y, TargetYs, HorizontalSpacing, NodePadding, TrunkY))
			{
				if (!bExternalFanoutCandidate)
				{
					continue;
				}
				TArray<double> FanoutAnchorYs = TargetYs;
				FanoutAnchorYs.Add(FirstSourcePoint.Y);
				double UpperExternalY = 0.0;
				double LowerExternalY = 0.0;
				if (!ComputeLocalExternalBusYs(ExitX, TrunkEndX, FanoutAnchorYs, UpperExternalY, LowerExternalY))
				{
					continue;
				}
				const double FanoutCenterY = (MinTargetY + MaxTargetY + FirstSourcePoint.Y) / 3.0;
				const double UpperExcursion = FMath::Abs(FanoutCenterY - UpperExternalY);
				const double LowerExcursion = FMath::Abs(FanoutCenterY - LowerExternalY);
				TrunkY = UpperExcursion <= LowerExcursion ? UpperExternalY : LowerExternalY;
				bExternalFanout = true;
			}
			const FString FanoutId = FString::Printf(TEXT("%s_fanout_%s"), bExternalFanout ? TEXT("external") : TEXT("local"), *Pair.Key);
			TArray<int32> SortedEdges = BusFanoutEdges;
			SortedEdges.Sort([&](const int32 A, const int32 B)
			{
				const FVector2D EndA = GetEdgeTargetPoint(Nodes, RawEdges[A], true);
				const FVector2D EndB = GetEdgeTargetPoint(Nodes, RawEdges[B], true);
				const double BranchXA = BranchXByEdge.FindRef(A);
				const double BranchXB = BranchXByEdge.FindRef(B);
				if (!FMath::IsNearlyEqual(BranchXA, BranchXB, 1.0))
				{
					return BranchXA < BranchXB;
				}
				if (!FMath::IsNearlyEqual(EndA.Y, EndB.Y, 1.0))
				{
					return EndA.Y < EndB.Y;
				}
				return RawEdges[A].TargetPinIndex < RawEdges[B].TargetPinIndex;
			});
			for (int32 LocalIndex = 0; LocalIndex < SortedEdges.Num() && Plans.Num() < MaxInsertedEdges; ++LocalIndex)
			{
				const int32 EdgeIndex = SortedEdges[LocalIndex];
				if (PlannedEdges.Contains(EdgeIndex))
				{
					continue;
				}
				const FBlueprintLayoutEdge& Edge = RawEdges[EdgeIndex];
				const FVector2D End = GetEdgeTargetPoint(Nodes, Edge, true);
				const double BranchX = BranchXByEdge.FindRef(EdgeIndex);
				FBlueprintLayoutReroutePlan Plan;
				Plan.RawEdgeIndex = EdgeIndex;
				Plan.FirstPoint = FVector2D(ExitX, TrunkY);
				Plan.SecondPoint = FVector2D(BranchX, TrunkY);
				Plan.ReroutePoints = {
					Plan.FirstPoint,
					Plan.SecondPoint
				};
				const double BranchVerticalDelta = FMath::Abs(End.Y - TrunkY);
				const double BranchEndpointStub = End.X - BranchX;
				const double MinBranchGuideVerticalDelta = FMath::Max(420.0, HorizontalSpacing * 1.6);
				const double MinBranchGuideEndpointStub = FMath::Max(260.0, HorizontalSpacing * 1.05);
				if (BranchVerticalDelta >= MinBranchGuideVerticalDelta
					&& BranchEndpointStub >= MinBranchGuideEndpointStub)
				{
					Plan.ReroutePoints.Add(FVector2D(BranchX, End.Y));
				}
				Plan.Reason = bExternalFanout ? TEXT("fanout_external_bus") : TEXT("fanout_local_bus");
				Plan.FanoutGroupId = FanoutId;
				Plan.BusId = FanoutId;
				Plan.RouteKind = bExternalFanout ? EBlueprintLayoutRouteKind::ExternalBus : EBlueprintLayoutRouteKind::LocalBus;
				Plans.Add(Plan);
				PlannedEdges.Add(EdgeIndex);
				PlannedRouteByEdge.Add(EdgeIndex, Plan);
			}
		}

		for (int32 EdgeIndex = 0; EdgeIndex < RawEdges.Num() && Plans.Num() < MaxInsertedEdges; ++EdgeIndex)
		{
			if (PlannedEdges.Contains(EdgeIndex))
			{
				continue;
			}
			const FBlueprintLayoutEdge& Edge = RawEdges[EdgeIndex];
			if (!Nodes.IsValidIndex(Edge.Source)
				|| !Nodes.IsValidIndex(Edge.Target)
				|| Nodes[Edge.Source].bRerouteNode
				|| Nodes[Edge.Target].bRerouteNode)
			{
				continue;
			}
			const FVector2D Start = GetEdgeSourcePoint(Nodes, Edge, true);
			const FVector2D End = GetEdgeTargetPoint(Nodes, Edge, true);
			const double Dx = End.X - Start.X;
			const double Dy = End.Y - Start.Y;
			const bool bBackward = Dx < -1.0;
			const bool bLongForward = Dx > FMath::Max(880.0, HorizontalSpacing * 3.0);
			TArray<FVector2D> DirectPoints = BuildDefaultEdgePolyline(Nodes, Edge, true);
			const bool bHitsNodes = CountPolylineNodeIntersections(Nodes, Edge, DirectPoints) > 0;
			const bool bExecHitsNodes = Edge.bExec && bHitsNodes;
			const bool bDataHitsNodes = !Edge.bExec && bHitsNodes;
			const bool bAllowBackwardExecBus = !Edge.bExec
				|| FMath::Abs(Dx) > FMath::Max(620.0, HorizontalSpacing * 2.15)
				|| bExecHitsNodes;
			if (Edge.bExec && !bBackward)
			{
				continue;
			}
			if (Edge.bExec && bBackward && !bAllowBackwardExecBus)
			{
				continue;
			}
			if (!bBackward && !bLongForward && !bExecHitsNodes && !bDataHitsNodes)
			{
				continue;
			}
			if (Edge.bExec
				&& !bBackward
				&& !bExecHitsNodes
				&& !(bLongForward && FMath::Abs(Dy) > FMath::Max(360.0, HorizontalSpacing * 1.2)))
			{
				continue;
			}

			struct FCorridorCandidate
			{
				FVector2D First;
				FVector2D Second;
				TArray<FVector2D> ReroutePoints;
				int32 Intersections = 0;
				int32 EdgeCrossings = 0;
				int32 Priority = 0;
				double Length = 0.0;
				double MiddleStraightnessDelta = 0.0;
				double MaxSegmentLength = 0.0;
				FString Reason;
				EBlueprintLayoutRouteKind RouteKind = EBlueprintLayoutRouteKind::LocalBus;
			};
			TArray<FCorridorCandidate> Candidates;
			auto AddCandidatePoints = [&](const TArray<FVector2D>& InputReroutePoints, const FString& Reason, const int32 Priority)
			{
				TArray<FVector2D> CleanReroutePoints;
				for (const FVector2D& Point : InputReroutePoints)
				{
					if (CleanReroutePoints.Num() == 0
						|| FMath::Abs(CleanReroutePoints.Last().X - Point.X) + FMath::Abs(CleanReroutePoints.Last().Y - Point.Y) > 8.0)
					{
						CleanReroutePoints.Add(Point);
					}
				}
				if (CleanReroutePoints.Num() < 2)
				{
					return;
				}
				const double EndpointProximity = FMath::Clamp(HorizontalSpacing * 0.18, 56.0, 110.0);
				if (LayoutManhattanDistance(Start, CleanReroutePoints[0]) < EndpointProximity
					|| LayoutManhattanDistance(End, CleanReroutePoints.Last()) < EndpointProximity)
				{
					return;
				}
				const double MiddleDistance = FMath::Abs(CleanReroutePoints.Last().X - CleanReroutePoints[0].X)
					+ FMath::Abs(CleanReroutePoints.Last().Y - CleanReroutePoints[0].Y);
				if (MiddleDistance < FMath::Clamp(HorizontalSpacing * 0.24, 72.0, 140.0))
				{
					return;
				}
				TArray<FVector2D> Points;
				Points.Add(Start);
				for (const FVector2D& Point : CleanReroutePoints)
				{
					Points.Add(Point);
				}
				Points.Add(End);
				FCorridorCandidate Candidate;
				Candidate.First = CleanReroutePoints[0];
				Candidate.Second = CleanReroutePoints.Last();
				Candidate.ReroutePoints = CleanReroutePoints;
				Candidate.Intersections = CountPolylineNodeIntersections(Nodes, Edge, Points);
				Candidate.EdgeCrossings = CountPolylineEdgeCrossingsWithPlanMap(Nodes, RawEdges, EdgeIndex, Points, PlannedRouteByEdge);
				Candidate.Priority = Priority;
				for (int32 SegmentIndex = 0; SegmentIndex + 1 < Points.Num(); ++SegmentIndex)
				{
					const double SegmentLength = FMath::Abs(Points[SegmentIndex + 1].X - Points[SegmentIndex].X)
						+ FMath::Abs(Points[SegmentIndex + 1].Y - Points[SegmentIndex].Y);
					Candidate.Length += SegmentLength;
					Candidate.MaxSegmentLength = FMath::Max(Candidate.MaxSegmentLength, SegmentLength);
				}
				Candidate.MiddleStraightnessDelta = TNumericLimits<double>::Max();
				for (int32 SegmentIndex = 1; SegmentIndex + 1 < Points.Num(); ++SegmentIndex)
				{
					Candidate.MiddleStraightnessDelta = FMath::Min(
						Candidate.MiddleStraightnessDelta,
						FMath::Min(
							FMath::Abs(Points[SegmentIndex + 1].X - Points[SegmentIndex].X),
							FMath::Abs(Points[SegmentIndex + 1].Y - Points[SegmentIndex].Y)));
				}
				if (Candidate.MiddleStraightnessDelta == TNumericLimits<double>::Max())
				{
					Candidate.MiddleStraightnessDelta = 0.0;
				}
				Candidate.Reason = Reason;
				Candidate.RouteKind = InferLayoutRouteKindFromReason(Reason);
				Candidates.Add(Candidate);
			};
			auto AddCandidate = [&](const FVector2D& First, const FVector2D& Second, const FString& Reason, const int32 Priority)
			{
				AddCandidatePoints({ First, Second }, Reason, Priority);
			};

			const double ExitGap = FMath::Clamp(HorizontalSpacing * 0.28, 96.0, 220.0);
			const double EntryGap = FMath::Clamp(HorizontalSpacing * 0.22, 80.0, 180.0);
			const double ForwardDx = FMath::Max(1.0, Dx);
			const double SourceBendX = Start.X + FMath::Min(ExitGap, FMath::Max(36.0, ForwardDx * 0.35));
			const double TargetBendX = End.X - FMath::Min(EntryGap, FMath::Max(36.0, ForwardDx * 0.35));
			const bool bHasUsableForwardCorridorSpan = TargetBendX > SourceBendX + FMath::Clamp(HorizontalSpacing * 0.16, 44.0, 96.0);
			const bool bEligibleForExternalBus = !Edge.bExec
				&& !bBackward
				&& bLongForward
				&& bHasUsableForwardCorridorSpan
				&& (bHitsNodes || FMath::Abs(Dy) > FMath::Max(360.0, HorizontalSpacing * 1.2));
			const bool bEligibleForBackwardBus = bBackward
				&& (bHitsNodes || FMath::Abs(Dx) > FMath::Max(620.0, HorizontalSpacing * 2.15));
			if (bEligibleForBackwardBus)
			{
				const double BusX = FMath::Max(Start.X, End.X) + FMath::Clamp(HorizontalSpacing * 0.55, 180.0, 360.0);
				AddCandidate(FVector2D(BusX, Start.Y), FVector2D(BusX, End.Y), TEXT("backward_wire"), 0);
			}
			if (!bBackward)
			{
				if (bEligibleForExternalBus)
				{
					TArray<double> AnchorYs = { Start.Y, End.Y };
					double UpperBusY = 0.0;
					double LowerBusY = 0.0;
					if (ComputeLocalExternalBusYs(SourceBendX, TargetBendX, AnchorYs, UpperBusY, LowerBusY))
					{
						const double MaxExternalExcursion = FMath::Clamp(HorizontalSpacing * 1.95, 520.0, 980.0);
						const double TopAnchorY = FMath::Min(Start.Y, End.Y);
						const double BottomAnchorY = FMath::Max(Start.Y, End.Y);
						const TArray<FVector2D> UpperBusPoints = {
							FVector2D(SourceBendX, UpperBusY),
							FVector2D(TargetBendX, UpperBusY)
						};
						const TArray<FVector2D> LowerBusPoints = {
							FVector2D(SourceBendX, LowerBusY),
							FVector2D(TargetBendX, LowerBusY)
						};
						if (FMath::Abs(TopAnchorY - UpperBusY) <= MaxExternalExcursion)
						{
							AddCandidatePoints(
								UpperBusPoints,
								TEXT("data_external_upper_bus_corridor"),
								8);
						}
						if (FMath::Abs(LowerBusY - BottomAnchorY) <= MaxExternalExcursion)
						{
							AddCandidatePoints(
								LowerBusPoints,
								TEXT("data_external_lower_bus_corridor"),
								10);
						}
					}
				}

				if (!Edge.bExec && (bLongForward || bHitsNodes))
				{
					const TArray<int32> EndpointNodes = { Edge.Source, Edge.Target };
					const FBlueprintLayoutRect EndpointBounds = MakeLayoutBounds(Nodes, EndpointNodes, true);
					const double LocalLaneGap = FMath::Clamp(HorizontalSpacing * 0.24, 72.0, 150.0);
					const bool bNearlyFlatDataLane = FMath::Abs(Dy) <= FMath::Max(140.0, HorizontalSpacing * 0.45);
					if (bLongForward && bNearlyFlatDataLane && bHasUsableForwardCorridorSpan)
					{
						const double InlineY = (Start.Y + End.Y) * 0.5;
						AddCandidate(
							FVector2D(SourceBendX, InlineY),
							FVector2D(TargetBendX, InlineY),
							TEXT("data_inline_lane"),
							bHitsNodes ? -3 : -2);
					}

					const double LocalUpperY = FMath::Min(Start.Y, End.Y) - LocalLaneGap;
					const double LocalLowerY = FMath::Max(Start.Y, End.Y) + LocalLaneGap;
					if (bHasUsableForwardCorridorSpan)
					{
						AddCandidate(
							FVector2D(SourceBendX, LocalUpperY),
							FVector2D(TargetBendX, LocalUpperY),
							TEXT("data_local_upper_corridor"),
							-1);
						AddCandidate(
							FVector2D(SourceBendX, LocalLowerY),
							FVector2D(TargetBendX, LocalLowerY),
							TEXT("data_local_lower_corridor"),
							1);
					}

					double BlockingTop = EndpointBounds.Top;
					double BlockingBottom = EndpointBounds.Bottom;
					const double SpanLeft = FMath::Min(Start.X, End.X);
					const double SpanRight = FMath::Max(Start.X, End.X);
					const double DirectLanePadding = FMath::Clamp(HorizontalSpacing * 0.18, 54.0, 110.0);
					const double DirectTop = FMath::Min(Start.Y, End.Y) - DirectLanePadding;
					const double DirectBottom = FMath::Max(Start.Y, End.Y) + DirectLanePadding;
					for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
					{
						if (!Nodes.IsValidIndex(NodeIndex)
							|| NodeIndex == Edge.Source
							|| NodeIndex == Edge.Target
							|| Nodes[NodeIndex].bRerouteNode)
						{
							continue;
						}
						const FBlueprintLayoutRect Rect = MakeNodeRect(Nodes[NodeIndex], true);
						const bool bOverlapsX = Rect.Right >= SpanLeft && Rect.Left <= SpanRight;
						const bool bNearDirectLane = Rect.Bottom >= DirectTop && Rect.Top <= DirectBottom;
						if (!bOverlapsX || !bNearDirectLane)
						{
							continue;
						}
						BlockingTop = FMath::Min(BlockingTop, Rect.Top);
						BlockingBottom = FMath::Max(BlockingBottom, Rect.Bottom);
					}
					const double CorridorGap = FMath::Clamp(HorizontalSpacing * 0.45, 140.0, 280.0);
					const double UpperY = BlockingTop - CorridorGap;
					const double LowerY = BlockingBottom + CorridorGap;
					if (bHasUsableForwardCorridorSpan)
					{
						AddCandidate(FVector2D(SourceBendX, UpperY), FVector2D(TargetBendX, UpperY), TEXT("data_upper_corridor"), 0);
						AddCandidate(FVector2D(SourceBendX, LowerY), FVector2D(TargetBendX, LowerY), TEXT("data_lower_corridor"), 2);
					}
				}
			}

			Candidates.RemoveAll([bEligibleForExternalBus](const FCorridorCandidate& Candidate)
			{
				return !bEligibleForExternalBus
					&& Candidate.RouteKind == EBlueprintLayoutRouteKind::ExternalBus;
			});

			Candidates.Sort([](const FCorridorCandidate& A, const FCorridorCandidate& B)
			{
				const int32 ATopologicalCost = A.EdgeCrossings * 10 + A.Intersections;
				const int32 BTopologicalCost = B.EdgeCrossings * 10 + B.Intersections;
				if (ATopologicalCost != BTopologicalCost)
				{
					return ATopologicalCost < BTopologicalCost;
				}
				if (A.EdgeCrossings != B.EdgeCrossings)
				{
					return A.EdgeCrossings < B.EdgeCrossings;
				}
				if (A.Intersections != B.Intersections)
				{
					return A.Intersections < B.Intersections;
				}
				if (A.Priority != B.Priority)
				{
					return A.Priority < B.Priority;
				}
				const bool bAOrthogonalBus = A.MiddleStraightnessDelta < 8.0;
				const bool bBOrthogonalBus = B.MiddleStraightnessDelta < 8.0;
				if (bAOrthogonalBus != bBOrthogonalBus)
				{
					return bAOrthogonalBus;
				}
				if (!FMath::IsNearlyEqual(A.MaxSegmentLength, B.MaxSegmentLength, 8.0))
				{
					return A.MaxSegmentLength < B.MaxSegmentLength;
				}
				return A.Length < B.Length;
			});
			if (Candidates.Num() > 0)
			{
				FPlanAlternativeSet Alternatives;
				Alternatives.RawEdgeIndex = EdgeIndex;
				const int32 CandidateLimit = Candidates.Num();
				for (int32 CandidateIndex = 0; CandidateIndex < CandidateLimit; ++CandidateIndex)
				{
					FBlueprintLayoutReroutePlan CandidatePlan;
					CandidatePlan.RawEdgeIndex = EdgeIndex;
					CandidatePlan.FirstPoint = Candidates[CandidateIndex].First;
					CandidatePlan.SecondPoint = Candidates[CandidateIndex].Second;
					CandidatePlan.ReroutePoints = Candidates[CandidateIndex].ReroutePoints;
					CandidatePlan.Reason = Candidates[CandidateIndex].Reason;
					CandidatePlan.RouteKind = Candidates[CandidateIndex].RouteKind;
					if (CandidatePlan.RouteKind == EBlueprintLayoutRouteKind::ExternalBus)
					{
						CandidatePlan.BusId = FString::Printf(TEXT("external_%d_%d"), EdgeIndex, CandidateIndex);
					}
					else if (CandidatePlan.RouteKind == EBlueprintLayoutRouteKind::LocalBus)
					{
						CandidatePlan.BusId = FString::Printf(TEXT("local_%d_%d"), EdgeIndex, CandidateIndex);
					}
					Alternatives.Candidates.Add(CandidatePlan);
				}
				Plans.Add(Alternatives.Candidates[0]);
				PlannedRouteByEdge.Add(EdgeIndex, Alternatives.Candidates[0]);
				AlternativeSets.Add(Alternatives);
			}
		}
		if (AlternativeSets.Num() > 0)
		{
			auto EvaluatePlanScore = [&Nodes, &RawEdges, NodePadding](const TArray<FBlueprintLayoutReroutePlan>& CandidatePlans)
			{
				return ScoreLayoutCandidate(EvaluateLayoutMetricsWithReroutePlans(Nodes, RawEdges, CandidatePlans, true, NodePadding));
			};
			TArray<FBlueprintLayoutReroutePlan> StableSeedPlans = Plans;
			for (const FPlanAlternativeSet& Alternatives : AlternativeSets)
			{
				int32 PlanIndex = INDEX_NONE;
				for (int32 Index = 0; Index < StableSeedPlans.Num(); ++Index)
				{
					if (StableSeedPlans[Index].RawEdgeIndex == Alternatives.RawEdgeIndex)
					{
						PlanIndex = Index;
						break;
					}
				}
				if (PlanIndex == INDEX_NONE)
				{
					continue;
				}
				for (const FBlueprintLayoutReroutePlan& CandidatePlan : Alternatives.Candidates)
				{
					if (!CandidatePlan.Reason.Contains(TEXT("_hit_")) && !CandidatePlan.Reason.Contains(TEXT("_external_")))
					{
						StableSeedPlans[PlanIndex] = CandidatePlan;
						break;
					}
				}
			}
			Plans = StableSeedPlans;
			double CurrentScore = EvaluatePlanScore(Plans);
			auto RefinePlans = [&Plans, &AlternativeSets, &EvaluatePlanScore, &CurrentScore](const bool bAllowHitAvoidanceCandidates)
			{
				for (int32 Pass = 0; Pass < 5; ++Pass)
				{
					bool bImproved = false;
					for (const FPlanAlternativeSet& Alternatives : AlternativeSets)
					{
						int32 PlanIndex = INDEX_NONE;
						for (int32 Index = 0; Index < Plans.Num(); ++Index)
						{
							if (Plans[Index].RawEdgeIndex == Alternatives.RawEdgeIndex)
							{
								PlanIndex = Index;
								break;
							}
						}
						if (PlanIndex == INDEX_NONE)
						{
							continue;
						}
						FBlueprintLayoutReroutePlan BestPlan = Plans[PlanIndex];
						double BestScore = CurrentScore;
						for (const FBlueprintLayoutReroutePlan& CandidatePlan : Alternatives.Candidates)
						{
							if (!bAllowHitAvoidanceCandidates
								&& (CandidatePlan.Reason.Contains(TEXT("_hit_")) || CandidatePlan.Reason.Contains(TEXT("_external_"))))
							{
								continue;
							}
							TArray<FBlueprintLayoutReroutePlan> TrialPlans = Plans;
							TrialPlans[PlanIndex] = CandidatePlan;
							const double TrialScore = EvaluatePlanScore(TrialPlans);
							if (TrialScore + 1.0 < BestScore)
							{
								BestScore = TrialScore;
								BestPlan = CandidatePlan;
							}
						}
						if (BestScore + 1.0 < CurrentScore)
						{
							Plans[PlanIndex] = BestPlan;
							CurrentScore = BestScore;
							bImproved = true;
						}
					}
					if (!bImproved)
					{
						break;
					}
				}
			};
			RefinePlans(false);
			RefinePlans(true);
		}
		PlanExternalBusLanes(Nodes, Plans, HorizontalSpacing);
		FinalizeRerouteBusPlans(Nodes, RawEdges, Plans, HorizontalSpacing, NodePadding);
		return Plans;
	}

	static UK2Node_Knot* CreateLayoutKnot(UEdGraph* Graph, const FVector2D& Point)
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
		Knot->NodePosX = FMath::RoundToInt(Point.X);
		Knot->NodePosY = FMath::RoundToInt(Point.Y);
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

	struct FLayoutKnotAnchorRequest
	{
		UK2Node_Knot* Knot = nullptr;
		FVector2D DesiredAnchor = FVector2D::ZeroVector;
	};

	static bool AlignLayoutKnotsToDesiredPinAnchors(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TArray<FLayoutKnotAnchorRequest>& Requests)
	{
		if (!Blueprint || !Graph || Requests.Num() == 0 || !FSlateApplication::IsInitialized())
		{
			return false;
		}

		FString Error;
		TSharedPtr<IBlueprintEditor> BlueprintEditor = GetBlueprintEditor(Blueprint, true, Error);
		if (!BlueprintEditor.IsValid())
		{
			return false;
		}
		TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(Graph, true);
		if (!GraphEditor.IsValid())
		{
			return false;
		}
		SGraphPanel* GraphPanel = GraphEditor->GetGraphPanel();
		if (!GraphPanel)
		{
			return false;
		}

		Graph->NotifyGraphChanged();
		FString WindowError;
		const TSharedPtr<SWindow> BlueprintWindow = ResolveBlueprintEditorWindow(BlueprintEditor, WindowError);
		if (BlueprintWindow.IsValid())
		{
			if (!PrepareSlateWindowForCapture(BlueprintWindow, Error))
			{
				return false;
			}
		}
		else
		{
			FSlateApplication::Get().PumpMessages();
			const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
			GraphEditor->SlatePrepass(WidgetScale);
			GraphPanel->SlatePrepass(WidgetScale);
		}
		GraphPanel->Update();
		FSlateApplication::Get().PumpMessages();
		const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
		GraphEditor->SlatePrepass(WidgetScale);
		GraphPanel->SlatePrepass(WidgetScale);

		bool bAlignedAll = true;
		for (const FLayoutKnotAnchorRequest& Request : Requests)
		{
			if (!Request.Knot || !Request.Knot->GetInputPin() || !IsFiniteLayoutPoint(Request.DesiredAnchor))
			{
				bAlignedAll = false;
				continue;
			}

			TSharedPtr<SGraphNode> KnotWidget = GraphPanel->GetNodeWidgetFromGuid(Request.Knot->NodeGuid);
			if (!KnotWidget.IsValid())
			{
				bAlignedAll = false;
				continue;
			}
			TSharedPtr<SGraphPin> PinWidget = KnotWidget->FindWidgetForPin(Request.Knot->GetInputPin());
			if (!PinWidget.IsValid() && Request.Knot->GetOutputPin())
			{
				PinWidget = KnotWidget->FindWidgetForPin(Request.Knot->GetOutputPin());
			}
			if (!PinWidget.IsValid())
			{
				bAlignedAll = false;
				continue;
			}

			const auto NodeOffset = PinWidget->GetNodeOffset();
			const FVector2D Offset(static_cast<double>(NodeOffset.X), static_cast<double>(NodeOffset.Y));
			if (!IsFiniteLayoutPoint(Offset))
			{
				bAlignedAll = false;
				continue;
			}
			Request.Knot->NodePosX = FMath::RoundToInt(Request.DesiredAnchor.X - Offset.X);
			Request.Knot->NodePosY = FMath::RoundToInt(Request.DesiredAnchor.Y - Offset.Y);
		}
		if (bAlignedAll)
		{
			Graph->NotifyGraphChanged();
		}
		return bAlignedAll;
	}

	static void RemoveLayoutKnots(UEdGraph* Graph, const TArray<UK2Node_Knot*>& Knots)
	{
		for (UK2Node_Knot* Knot : Knots)
		{
			if (Knot && Graph)
			{
				Knot->BreakAllNodeLinks();
				Graph->RemoveNode(Knot);
			}
		}
	}

	static bool ConnectPinsIfNeeded(const UEdGraphSchema* Schema, UEdGraphPin* OutputPin, UEdGraphPin* InputPin)
	{
		if (!Schema || !OutputPin || !InputPin)
		{
			return false;
		}
		if (OutputPin->LinkedTo.Contains(InputPin))
		{
			return true;
		}
		return Schema->TryCreateConnection(OutputPin, InputPin);
	}

	static int32 ApplySharedFanoutBusPlans(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const UEdGraphSchema* Schema,
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& RawEdges,
		const TArray<FBlueprintLayoutReroutePlan>& Plans,
		TSet<int32>& OutHandledRawEdges)
	{
		if (!Blueprint || !Graph || !Schema)
		{
			return 0;
		}

		TMap<FString, TArray<int32>> PlansByBus;
		for (int32 PlanIndex = 0; PlanIndex < Plans.Num(); ++PlanIndex)
		{
			const FBlueprintLayoutReroutePlan& Plan = Plans[PlanIndex];
			if (Plan.BusId.IsEmpty() || Plan.FanoutGroupId.IsEmpty())
			{
				continue;
			}
			PlansByBus.FindOrAdd(Plan.BusId).Add(PlanIndex);
		}

		int32 InsertedEdgeCount = 0;
		for (const TPair<FString, TArray<int32>>& Pair : PlansByBus)
		{
			if (Pair.Value.Num() < 2)
			{
				continue;
			}
			const FBlueprintLayoutReroutePlan& FirstPlan = Plans[Pair.Value[0]];
			if (!RawEdges.IsValidIndex(FirstPlan.RawEdgeIndex) || FirstPlan.ReroutePoints.Num() < 2)
			{
				continue;
			}
			const FBlueprintLayoutEdge& FirstEdge = RawEdges[FirstPlan.RawEdgeIndex];
			if (!Nodes.IsValidIndex(FirstEdge.Source))
			{
				continue;
			}
			UEdGraphPin* SourcePin = FindLayoutGraphPin(Nodes[FirstEdge.Source].Node, FirstEdge.SourcePin, EGPD_Output, FirstEdge.SourcePinIndex);
			if (!SourcePin)
			{
				continue;
			}

			struct FSharedFanoutBranch
			{
				const FBlueprintLayoutReroutePlan* Plan = nullptr;
				UEdGraphPin* TargetPin = nullptr;
				FVector2D BranchPoint = FVector2D::ZeroVector;
				TArray<FVector2D> BranchReroutePoints;
			};

			TArray<UEdGraphPin*> TargetPins;
			TArray<FSharedFanoutBranch> Branches;
			bool bAllTargetsLinked = true;
			for (const int32 PlanIndex : Pair.Value)
			{
				if (!Plans.IsValidIndex(PlanIndex))
				{
					continue;
				}
				const FBlueprintLayoutReroutePlan& Plan = Plans[PlanIndex];
				if (!RawEdges.IsValidIndex(Plan.RawEdgeIndex))
				{
					continue;
				}
				const FBlueprintLayoutEdge& Edge = RawEdges[Plan.RawEdgeIndex];
				if (Edge.Source != FirstEdge.Source
					|| !Edge.SourcePin.Equals(FirstEdge.SourcePin, ESearchCase::CaseSensitive)
					|| Edge.SourcePinIndex != FirstEdge.SourcePinIndex
					|| !Nodes.IsValidIndex(Edge.Target))
				{
					bAllTargetsLinked = false;
					break;
				}
				UEdGraphPin* TargetPin = FindLayoutGraphPin(Nodes[Edge.Target].Node, Edge.TargetPin, EGPD_Input, Edge.TargetPinIndex);
				if (!TargetPin || !SourcePin->LinkedTo.Contains(TargetPin))
				{
					bAllTargetsLinked = false;
					break;
				}
				if (Plan.ReroutePoints.Num() < 2)
				{
					bAllTargetsLinked = false;
					break;
				}
				FSharedFanoutBranch Branch;
				Branch.Plan = &Plan;
				Branch.TargetPin = TargetPin;
				Branch.BranchPoint = Plan.ReroutePoints[1];
				for (int32 PointIndex = 2; PointIndex < Plan.ReroutePoints.Num(); ++PointIndex)
				{
					Branch.BranchReroutePoints.Add(Plan.ReroutePoints[PointIndex]);
				}
				TargetPins.Add(TargetPin);
				Branches.Add(Branch);
			}
			if (!bAllTargetsLinked || Branches.Num() < 2 || FirstPlan.ReroutePoints.Num() < 1)
			{
				continue;
			}

			TArray<UK2Node_Knot*> CreatedKnots;
			TArray<FLayoutKnotAnchorRequest> TrunkAnchorRequests;
			TArray<FVector2D> TrunkPoints;
			TrunkPoints.Add(FirstPlan.ReroutePoints[0]);
			TArray<FVector2D> SortedBranchPoints;
			for (const FSharedFanoutBranch& Branch : Branches)
			{
				bool bAlreadyAdded = false;
				for (const FVector2D& ExistingPoint : SortedBranchPoints)
				{
					if (FMath::Abs(ExistingPoint.X - Branch.BranchPoint.X) + FMath::Abs(ExistingPoint.Y - Branch.BranchPoint.Y) <= 8.0)
					{
						bAlreadyAdded = true;
						break;
					}
				}
				if (!bAlreadyAdded)
				{
					SortedBranchPoints.Add(Branch.BranchPoint);
				}
			}
			SortedBranchPoints.Sort([](const FVector2D& A, const FVector2D& B)
			{
				if (!FMath::IsNearlyEqual(A.X, B.X, 1.0))
				{
					return A.X < B.X;
				}
				return A.Y < B.Y;
			});
			for (const FVector2D& BranchPoint : SortedBranchPoints)
			{
				if (FMath::Abs(BranchPoint.X - TrunkPoints[0].X) + FMath::Abs(BranchPoint.Y - TrunkPoints[0].Y) > 8.0)
				{
					TrunkPoints.Add(BranchPoint);
				}
			}

			bool bCreatedTrunk = true;
			for (const FVector2D& TrunkPoint : TrunkPoints)
			{
				UK2Node_Knot* TrunkKnot = CreateLayoutKnot(Graph, TrunkPoint);
				if (!TrunkKnot)
				{
					bCreatedTrunk = false;
					break;
				}
				CreatedKnots.Add(TrunkKnot);
				FLayoutKnotAnchorRequest AnchorRequest;
				AnchorRequest.Knot = TrunkKnot;
				AnchorRequest.DesiredAnchor = TrunkPoint;
				TrunkAnchorRequests.Add(AnchorRequest);
			}
			if (!bCreatedTrunk || CreatedKnots.Num() != TrunkPoints.Num())
			{
				RemoveLayoutKnots(Graph, CreatedKnots);
				continue;
			}
			if (!AlignLayoutKnotsToDesiredPinAnchors(Blueprint, Graph, TrunkAnchorRequests))
			{
				RemoveLayoutKnots(Graph, CreatedKnots);
				continue;
			}
			auto FindTrunkKnotForPoint = [&TrunkPoints, &CreatedKnots](const FVector2D& Point) -> UK2Node_Knot*
			{
				for (int32 Index = 0; Index < TrunkPoints.Num() && Index < CreatedKnots.Num(); ++Index)
				{
					if (FMath::Abs(TrunkPoints[Index].X - Point.X) + FMath::Abs(TrunkPoints[Index].Y - Point.Y) <= 8.0)
					{
						return CreatedKnots[Index];
					}
				}
				return nullptr;
			};

			for (UEdGraphPin* TargetPin : TargetPins)
			{
				SourcePin->BreakLinkTo(TargetPin);
			}
			bool bConnected = ConnectPinsIfNeeded(Schema, SourcePin, CreatedKnots[0]->GetInputPin());
			for (int32 TrunkIndex = 1; bConnected && TrunkIndex < CreatedKnots.Num(); ++TrunkIndex)
			{
				bConnected = ConnectPinsIfNeeded(Schema, CreatedKnots[TrunkIndex - 1]->GetOutputPin(), CreatedKnots[TrunkIndex]->GetInputPin());
			}
			for (const FSharedFanoutBranch& Branch : Branches)
			{
				if (!bConnected)
				{
					break;
				}
				UK2Node_Knot* BranchTrunkKnot = FindTrunkKnotForPoint(Branch.BranchPoint);
				if (!BranchTrunkKnot)
				{
					bConnected = false;
					break;
				}
				UEdGraphPin* CurrentOutputPin = BranchTrunkKnot->GetOutputPin();
				for (const FVector2D& BranchReroutePoint : Branch.BranchReroutePoints)
				{
					UK2Node_Knot* BranchKnot = CreateLayoutKnot(Graph, BranchReroutePoint);
					if (!BranchKnot)
					{
						bConnected = false;
						break;
					}
					CreatedKnots.Add(BranchKnot);
					FLayoutKnotAnchorRequest BranchAnchorRequest;
					BranchAnchorRequest.Knot = BranchKnot;
					BranchAnchorRequest.DesiredAnchor = BranchReroutePoint;
					TArray<FLayoutKnotAnchorRequest> BranchAnchorRequests;
					BranchAnchorRequests.Add(BranchAnchorRequest);
					if (!AlignLayoutKnotsToDesiredPinAnchors(Blueprint, Graph, BranchAnchorRequests))
					{
						bConnected = false;
						break;
					}
					if (!ConnectPinsIfNeeded(Schema, CurrentOutputPin, BranchKnot->GetInputPin()))
					{
						bConnected = false;
						break;
					}
					CurrentOutputPin = BranchKnot->GetOutputPin();
				}
				bConnected = bConnected && ConnectPinsIfNeeded(Schema, CurrentOutputPin, Branch.TargetPin);
			}
			if (!bConnected)
			{
				RemoveLayoutKnots(Graph, CreatedKnots);
				for (UEdGraphPin* TargetPin : TargetPins)
				{
					Schema->TryCreateConnection(SourcePin, TargetPin);
				}
				continue;
			}
			for (const FSharedFanoutBranch& Branch : Branches)
			{
				if (Branch.Plan)
				{
					OutHandledRawEdges.Add(Branch.Plan->RawEdgeIndex);
					++InsertedEdgeCount;
				}
			}
		}
		return InsertedEdgeCount;
	}

	static int32 ApplyRerouteInsertionPlan(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		TArray<FBlueprintLayoutNode>& Nodes,
		const TArray<FBlueprintLayoutEdge>& RawEdges,
		const TArray<FBlueprintLayoutReroutePlan>& Plans)
	{
		if (!Blueprint || !Graph || Plans.Num() == 0)
		{
			return 0;
		}
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			return 0;
		}

		int32 InsertedEdgeCount = 0;
		TSet<int32> HandledRawEdges;
		InsertedEdgeCount += ApplySharedFanoutBusPlans(Blueprint, Graph, Schema, Nodes, RawEdges, Plans, HandledRawEdges);
		for (const FBlueprintLayoutReroutePlan& Plan : Plans)
		{
			if (HandledRawEdges.Contains(Plan.RawEdgeIndex))
			{
				continue;
			}
			if (!RawEdges.IsValidIndex(Plan.RawEdgeIndex))
			{
				continue;
			}
			const FBlueprintLayoutEdge& Edge = RawEdges[Plan.RawEdgeIndex];
			if (!Nodes.IsValidIndex(Edge.Source) || !Nodes.IsValidIndex(Edge.Target))
			{
				continue;
			}
			UEdGraphPin* SourcePin = FindLayoutGraphPin(Nodes[Edge.Source].Node, Edge.SourcePin, EGPD_Output, Edge.SourcePinIndex);
			UEdGraphPin* TargetPin = FindLayoutGraphPin(Nodes[Edge.Target].Node, Edge.TargetPin, EGPD_Input, Edge.TargetPinIndex);
			if (!SourcePin || !TargetPin || !SourcePin->LinkedTo.Contains(TargetPin))
			{
				continue;
			}

			TArray<FVector2D> PointsToInsert = Plan.ReroutePoints;
			if (PointsToInsert.Num() == 0)
			{
				PointsToInsert = { Plan.FirstPoint, Plan.SecondPoint };
			}
			if (PointsToInsert.Num() < 2)
			{
				continue;
			}

			TArray<UK2Node_Knot*> InsertedKnots;
			TArray<FLayoutKnotAnchorRequest> AnchorRequests;
			bool bCreatedAllKnots = true;
			for (const FVector2D& Point : PointsToInsert)
			{
				UK2Node_Knot* Knot = CreateLayoutKnot(Graph, Point);
				if (!Knot)
				{
					bCreatedAllKnots = false;
					break;
				}
				InsertedKnots.Add(Knot);
				FLayoutKnotAnchorRequest AnchorRequest;
				AnchorRequest.Knot = Knot;
				AnchorRequest.DesiredAnchor = Point;
				AnchorRequests.Add(AnchorRequest);
			}
			if (!bCreatedAllKnots || InsertedKnots.Num() != PointsToInsert.Num())
			{
				RemoveLayoutKnots(Graph, InsertedKnots);
				continue;
			}
			if (!AlignLayoutKnotsToDesiredPinAnchors(Blueprint, Graph, AnchorRequests))
			{
				RemoveLayoutKnots(Graph, InsertedKnots);
				continue;
			}

			SourcePin->BreakLinkTo(TargetPin);
			bool bConnected = true;
			UEdGraphPin* CurrentOutputPin = SourcePin;
			for (UK2Node_Knot* Knot : InsertedKnots)
			{
				if (!Knot || !Knot->GetInputPin() || !Knot->GetOutputPin() || !CurrentOutputPin)
				{
					bConnected = false;
					break;
				}
				bConnected = Schema->TryCreateConnection(CurrentOutputPin, Knot->GetInputPin());
				if (!bConnected)
				{
					break;
				}
				CurrentOutputPin = Knot->GetOutputPin();
			}
			bConnected = bConnected && CurrentOutputPin && Schema->TryCreateConnection(CurrentOutputPin, TargetPin);
			if (!bConnected)
			{
				RemoveLayoutKnots(Graph, InsertedKnots);
				Schema->TryCreateConnection(SourcePin, TargetPin);
				continue;
			}
			++InsertedEdgeCount;
		}
		return InsertedEdgeCount;
	}
}

bool FUeAgentHttpServer::CmdBlueprintLayoutGraph(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	const double CommandStartTime = FPlatformTime::Seconds();
	auto LogLayoutStage = [CommandStartTime](const TCHAR* Stage, const int32 NodeCount = INDEX_NONE, const int32 EdgeCount = INDEX_NONE)
	{
		UE_LOG(
			LogUeAgentInterface,
			Display,
			TEXT("blueprint_layout_graph stage=%s elapsed=%.3fs nodes=%d edges=%d"),
			Stage,
			FPlatformTime::Seconds() - CommandStartTime,
			NodeCount,
			EdgeCount);
	};
	LogLayoutStage(TEXT("start"));

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString GraphName = TEXT("EventGraph");
	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);

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

	bool bRestoreEditorView = true;
	bool bSnapRestoredEditorViewToPixelGrid = true;
	JsonTryGetBool(Ctx.Params, TEXT("restore_editor_view"), bRestoreEditorView);
	JsonTryGetBool(Ctx.Params, TEXT("snap_restored_editor_view_to_pixel_grid"), bSnapRestoredEditorViewToPixelGrid);
	bool bCapturedEditorView = false;
	bool bRestoredEditorView = false;
	bool bAllowPersistRestoredEditorViewState = false;
	bool bPersistedRestoredEditorViewState = false;
	int32 RemovedRestoredEditorViewStateEntryCount = 0;
	FVector2f CapturedEditorViewLocation(0.0f, 0.0f);
	float CapturedEditorViewZoom = 1.0f;
	auto CaptureEditorViewState = [&]()
	{
		if (!bRestoreEditorView || bCapturedEditorView || !FSlateApplication::IsInitialized())
		{
			return;
		}
		FString ViewError;
		TSharedPtr<IBlueprintEditor> BlueprintEditor = UeAgentBlueprintOps::GetBlueprintEditor(Blueprint, true, ViewError);
		if (!BlueprintEditor.IsValid())
		{
			return;
		}
		TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(Graph, true);
		if (!GraphEditor.IsValid())
		{
			return;
		}
		GraphEditor->GetViewLocation(CapturedEditorViewLocation, CapturedEditorViewZoom);
		bCapturedEditorView = true;
	};
	auto RestoreEditorViewState = [&]()
	{
		if (!bRestoreEditorView || !bCapturedEditorView || bRestoredEditorView || !FSlateApplication::IsInitialized())
		{
			return;
		}
		FString ViewError;
		TSharedPtr<IBlueprintEditor> BlueprintEditor = UeAgentBlueprintOps::GetBlueprintEditor(Blueprint, true, ViewError);
		if (!BlueprintEditor.IsValid())
		{
			return;
		}
		TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(Graph, true);
		if (!GraphEditor.IsValid())
		{
			return;
		}
		FVector2f RestoreViewLocation = CapturedEditorViewLocation;
		if (bSnapRestoredEditorViewToPixelGrid)
		{
			RestoreViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(
				RestoreViewLocation,
				CapturedEditorViewZoom);
		}
		GraphEditor->SetViewLocation(RestoreViewLocation, CapturedEditorViewZoom);
		if (bAllowPersistRestoredEditorViewState)
		{
			Blueprint->Modify();
			bPersistedRestoredEditorViewState = UeAgentBlueprintOps::PersistBlueprintGraphDocumentViewState(
				Blueprint,
				Graph,
				RestoreViewLocation,
				CapturedEditorViewZoom,
				&RemovedRestoredEditorViewStateEntryCount);
		}
		bRestoredEditorView = true;
	};
	CaptureEditorViewState();

	double DefaultNodeWidth = 300.0;
	double DefaultNodeHeight = 120.0;
	double HorizontalSpacing = 260.0;
	double VerticalSpacing = 120.0;
	double NodePadding = 28.0;
	double OriginX = 0.0;
	double OriginY = 0.0;
	double MaxIterationsNumber = 16.0;
	JsonTryGetNumber(Ctx.Params, TEXT("default_node_width"), DefaultNodeWidth);
	JsonTryGetNumber(Ctx.Params, TEXT("default_node_height"), DefaultNodeHeight);
	JsonTryGetNumber(Ctx.Params, TEXT("horizontal_spacing"), HorizontalSpacing);
	JsonTryGetNumber(Ctx.Params, TEXT("vertical_spacing"), VerticalSpacing);
	JsonTryGetNumber(Ctx.Params, TEXT("node_padding"), NodePadding);
	JsonTryGetNumber(Ctx.Params, TEXT("origin_x"), OriginX);
	JsonTryGetNumber(Ctx.Params, TEXT("origin_y"), OriginY);
	JsonTryGetNumber(Ctx.Params, TEXT("max_iterations"), MaxIterationsNumber);
	DefaultNodeWidth = FMath::Clamp(DefaultNodeWidth, 120.0, 1000.0);
	DefaultNodeHeight = FMath::Clamp(DefaultNodeHeight, 70.0, 1000.0);
	HorizontalSpacing = FMath::Clamp(HorizontalSpacing, 80.0, 2000.0);
	VerticalSpacing = FMath::Clamp(VerticalSpacing, 40.0, 2000.0);
	NodePadding = FMath::Clamp(NodePadding, 0.0, 400.0);
	const int32 MaxIterations = FMath::Clamp(FMath::RoundToInt(MaxIterationsNumber), 1, 32);

	bool bDryRun = false;
	bool bCompileAfterLayout = false;
	bool bSaveAfterLayout = false;
	bool bInsertRerouteNodes = false;
	bool bReplaceExistingRerouteNodes = false;
	bool bRequireRealGeometry = true;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_layout"), bCompileAfterLayout);
	JsonTryGetBool(Ctx.Params, TEXT("save_after_layout"), bSaveAfterLayout);
	JsonTryGetBool(Ctx.Params, TEXT("insert_reroute_nodes"), bInsertRerouteNodes);
	JsonTryGetBool(Ctx.Params, TEXT("replace_existing_reroute_nodes"), bReplaceExistingRerouteNodes);
	JsonTryGetBool(Ctx.Params, TEXT("require_real_geometry"), bRequireRealGeometry);
	bAllowPersistRestoredEditorViewState = !bDryRun && bSaveAfterLayout;
	bool bEmitPinAlignmentProbes = false;
	double PinAlignmentProbeLimitNumber = 16.0;
	FString PinAlignmentProbeSourceTitle;
	FString PinAlignmentProbeSourcePin;
	FString PinAlignmentProbeTargetTitle;
	FString PinAlignmentProbeTargetPin;
	JsonTryGetBool(Ctx.Params, TEXT("emit_pin_alignment_probes"), bEmitPinAlignmentProbes);
	JsonTryGetNumber(Ctx.Params, TEXT("pin_alignment_probe_limit"), PinAlignmentProbeLimitNumber);
	JsonTryGetString(Ctx.Params, TEXT("pin_alignment_probe_source_title"), PinAlignmentProbeSourceTitle);
	JsonTryGetString(Ctx.Params, TEXT("pin_alignment_probe_source_pin"), PinAlignmentProbeSourcePin);
	JsonTryGetString(Ctx.Params, TEXT("pin_alignment_probe_target_title"), PinAlignmentProbeTargetTitle);
	JsonTryGetString(Ctx.Params, TEXT("pin_alignment_probe_target_pin"), PinAlignmentProbeTargetPin);
	const int32 PinAlignmentProbeLimit = FMath::Clamp(FMath::RoundToInt(PinAlignmentProbeLimitNumber), 0, 200);

	int32 RemovedExistingRerouteNodeCount = 0;
	if (!bDryRun && bReplaceExistingRerouteNodes)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "BlueprintLayoutReplaceRerouteNodes", "Replace Blueprint Layout Reroute Nodes"));
		Blueprint->Modify();
		Graph->Modify();
		RemovedExistingRerouteNodeCount = UeAgentBlueprintOps::RemoveRerouteNodesAndReconnect(Graph);
		if (RemovedExistingRerouteNodeCount > 0)
		{
			Graph->NotifyGraphChanged();
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}
	}

	TArray<UeAgentBlueprintOps::FBlueprintLayoutNode> LayoutNodes;
	TMap<UEdGraphNode*, int32> NodeToIndex;
	LayoutNodes.Reserve(Graph->Nodes.Num());
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		UeAgentBlueprintOps::FBlueprintLayoutNode LayoutNode;
		LayoutNode.Node = Node;
		LayoutNode.Guid = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		LayoutNode.Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		LayoutNode.Index = LayoutNodes.Num();
		LayoutNode.bRerouteNode = UeAgentBlueprintOps::IsRerouteBlueprintNode(Node);
		LayoutNode.Width = UeAgentBlueprintOps::EstimateNodeWidth(Node, DefaultNodeWidth);
		LayoutNode.Height = UeAgentBlueprintOps::EstimateNodeHeight(Node, DefaultNodeHeight);
		LayoutNode.InputPinCount = UeAgentBlueprintOps::CountPinsInDirection(Node, EGPD_Input);
		LayoutNode.OutputPinCount = UeAgentBlueprintOps::CountPinsInDirection(Node, EGPD_Output);
		LayoutNode.OldX = Node->NodePosX;
		LayoutNode.OldY = Node->NodePosY;
		LayoutNode.NewX = Node->NodePosX;
		LayoutNode.NewY = Node->NodePosY;
		NodeToIndex.Add(Node, LayoutNode.Index);
		LayoutNodes.Add(LayoutNode);
	}

	UeAgentBlueprintOps::FBlueprintLayoutGeometryStats GeometryStats;
	FString GeometryError;
	const bool bCapturedRealGeometry = UeAgentBlueprintOps::CaptureBlueprintLayoutGeometry(
		Blueprint,
		Graph,
		LayoutNodes,
		GeometryStats,
		GeometryError);
	if (!bCapturedRealGeometry && bRequireRealGeometry)
	{
		RestoreEditorViewState();
		OutError = GeometryError.IsEmpty() ? TEXT("real_geometry_required_but_unavailable") : GeometryError;
		return false;
	}
	LogLayoutStage(TEXT("geometry_captured"), GeometryStats.MeasuredNodeCount, GeometryStats.PinGeometryAnchorCount + GeometryStats.PinOffsetAnchorCount);

	TArray<UeAgentBlueprintOps::FBlueprintLayoutEdge> RawLayoutEdges;
	TSet<FString> EdgeKeys;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		const int32* SourceIndexPtr = NodeToIndex.Find(Node);
		if (!Node || !SourceIndexPtr)
		{
			continue;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				UEdGraphNode* TargetNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				const int32* TargetIndexPtr = TargetNode ? NodeToIndex.Find(TargetNode) : nullptr;
				if (!TargetIndexPtr || *TargetIndexPtr == *SourceIndexPtr)
				{
					continue;
				}
				UeAgentBlueprintOps::FBlueprintLayoutEdge Edge;
				Edge.Source = *SourceIndexPtr;
				Edge.Target = *TargetIndexPtr;
				Edge.SourcePin = Pin->PinName.ToString();
				Edge.TargetPin = LinkedPin ? LinkedPin->PinName.ToString() : FString();
				Edge.SourcePinIndex = UeAgentBlueprintOps::GetPinIndexInDirection(Pin);
				Edge.TargetPinIndex = UeAgentBlueprintOps::GetPinIndexInDirection(LinkedPin);
				Edge.bExec = UeAgentBlueprintOps::IsExecPin(Pin) || UeAgentBlueprintOps::IsExecPin(LinkedPin);
				const FString Key = UeAgentBlueprintOps::MakeLayoutEdgeKey(Edge);
				if (EdgeKeys.Contains(Key))
				{
					continue;
				}
				EdgeKeys.Add(Key);
				RawLayoutEdges.Add(Edge);
			}
		}
	}
	LogLayoutStage(TEXT("graph_built"), LayoutNodes.Num(), RawLayoutEdges.Num());

	const UeAgentBlueprintOps::FBlueprintLayoutMetrics BeforeMetrics =
		UeAgentBlueprintOps::EvaluateLayoutMetrics(LayoutNodes, RawLayoutEdges, false, NodePadding);

	int32 RerouteNodeCount = 0;
	TArray<UeAgentBlueprintOps::FBlueprintLayoutEdge> LayoutEdges =
		UeAgentBlueprintOps::BuildRerouteCollapsedEdges(LayoutNodes, RawLayoutEdges, RerouteNodeCount);
	TArray<TArray<int32>> ConnectedComponents =
		UeAgentBlueprintOps::BuildConnectedComponents(LayoutNodes, LayoutEdges);

	int32 RankCount = 0;
	UeAgentBlueprintOps::FBlueprintLayoutAlignmentStats AlignmentStats;
	UeAgentBlueprintOps::ApplyPortConstraintLayeredLayout(
		LayoutNodes,
		LayoutEdges,
		ConnectedComponents,
		OriginX,
		OriginY,
		HorizontalSpacing,
		VerticalSpacing,
		NodePadding,
		MaxIterations,
		RankCount,
		&AlignmentStats);
	const UeAgentBlueprintOps::FBlueprintLayoutMetrics AfterMetrics =
		UeAgentBlueprintOps::EvaluateLayoutMetrics(LayoutNodes, LayoutEdges, true, NodePadding);
	const double FinalScore = UeAgentBlueprintOps::ScoreLayoutCandidate(AfterMetrics);
	LogLayoutStage(TEXT("layout_done"), LayoutNodes.Num(), LayoutEdges.Num());

	const TArray<UeAgentBlueprintOps::FBlueprintLayoutEdge>& RerouteCandidateEdges =
		(bDryRun && bReplaceExistingRerouteNodes) ? LayoutEdges : RawLayoutEdges;
	const bool bReroutePlanUsesCollapsedEdges = &RerouteCandidateEdges == &LayoutEdges;
	const TArray<UeAgentBlueprintOps::FBlueprintLayoutReroutePlan> RerouteInsertionPlan =
		UeAgentBlueprintOps::BuildRerouteInsertionPlan(LayoutNodes, RerouteCandidateEdges, HorizontalSpacing, NodePadding);
	const UeAgentBlueprintOps::FBlueprintLayoutMetrics ReroutePlanMetrics =
		UeAgentBlueprintOps::EvaluateLayoutMetricsWithReroutePlans(
			LayoutNodes,
			RerouteCandidateEdges,
			RerouteInsertionPlan,
			true,
			NodePadding);
	const double ReroutePlanScore = UeAgentBlueprintOps::ScoreLayoutCandidate(ReroutePlanMetrics);
	int32 InsertedRerouteEdgeCount = 0;

	int32 MovedNodeCount = 0;
	TArray<TSharedPtr<FJsonValue>> NodeResults;
	for (const UeAgentBlueprintOps::FBlueprintLayoutNode& LayoutNode : LayoutNodes)
	{
		const bool bMoved = !FMath::IsNearlyEqual(LayoutNode.OldX, LayoutNode.NewX, 0.5)
			|| !FMath::IsNearlyEqual(LayoutNode.OldY, LayoutNode.NewY, 0.5);
		if (bMoved)
		{
			++MovedNodeCount;
		}

		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_guid"), LayoutNode.Guid);
		NodeObj->SetStringField(TEXT("title"), LayoutNode.Title);
		NodeObj->SetStringField(TEXT("class"), LayoutNode.Node ? LayoutNode.Node->GetClass()->GetPathName() : FString());
		NodeObj->SetNumberField(TEXT("rank"), LayoutNode.Rank);
		NodeObj->SetNumberField(TEXT("order"), LayoutNode.Order);
		NodeObj->SetNumberField(TEXT("component"), LayoutNode.Component);
		NodeObj->SetNumberField(TEXT("width"), LayoutNode.Width);
		NodeObj->SetNumberField(TEXT("height"), LayoutNode.Height);
		NodeObj->SetNumberField(TEXT("estimated_width"), LayoutNode.Width);
		NodeObj->SetNumberField(TEXT("estimated_height"), LayoutNode.Height);
		NodeObj->SetBoolField(TEXT("measured_geometry"), LayoutNode.bHasMeasuredGeometry);
		NodeObj->SetNumberField(TEXT("input_pin_count"), LayoutNode.InputPinCount);
		NodeObj->SetNumberField(TEXT("output_pin_count"), LayoutNode.OutputPinCount);
		NodeObj->SetNumberField(TEXT("input_pin_anchor_count"), UeAgentBlueprintOps::CountMeasuredPinAnchors(LayoutNode, EGPD_Input));
		NodeObj->SetNumberField(TEXT("output_pin_anchor_count"), UeAgentBlueprintOps::CountMeasuredPinAnchors(LayoutNode, EGPD_Output));
		NodeObj->SetBoolField(TEXT("has_exec_pin"), UeAgentBlueprintOps::NodeHasExecPin(LayoutNode));
		NodeObj->SetBoolField(TEXT("is_reroute_node"), LayoutNode.bRerouteNode);
		NodeObj->SetNumberField(TEXT("old_pos_x"), LayoutNode.OldX);
		NodeObj->SetNumberField(TEXT("old_pos_y"), LayoutNode.OldY);
		NodeObj->SetNumberField(TEXT("new_pos_x"), LayoutNode.NewX);
		NodeObj->SetNumberField(TEXT("new_pos_y"), LayoutNode.NewY);
		NodeObj->SetBoolField(TEXT("moved"), bMoved);
		NodeResults.Add(MakeShared<FJsonValueObject>(NodeObj));
	}

	TArray<TSharedPtr<FJsonValue>> BackwardEdgeResults;
	for (const UeAgentBlueprintOps::FBlueprintLayoutEdge& Edge : LayoutEdges)
	{
		if (!LayoutNodes.IsValidIndex(Edge.Source) || !LayoutNodes.IsValidIndex(Edge.Target))
		{
			continue;
		}
		const FVector2D Start = UeAgentBlueprintOps::GetEdgeSourcePoint(LayoutNodes, Edge, true);
		const FVector2D End = UeAgentBlueprintOps::GetEdgeTargetPoint(LayoutNodes, Edge, true);
		if (Start.X <= End.X + 1.0)
		{
			continue;
		}
		TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
		EdgeObj->SetStringField(TEXT("source_title"), LayoutNodes[Edge.Source].Title);
		EdgeObj->SetStringField(TEXT("target_title"), LayoutNodes[Edge.Target].Title);
		EdgeObj->SetStringField(TEXT("source_pin"), Edge.SourcePin);
		EdgeObj->SetStringField(TEXT("target_pin"), Edge.TargetPin);
		EdgeObj->SetBoolField(TEXT("exec_edge"), Edge.bExec);
		EdgeObj->SetBoolField(TEXT("source_has_exec_pin"), UeAgentBlueprintOps::NodeHasExecPin(LayoutNodes[Edge.Source]));
		EdgeObj->SetBoolField(TEXT("target_has_exec_pin"), UeAgentBlueprintOps::NodeHasExecPin(LayoutNodes[Edge.Target]));
		EdgeObj->SetNumberField(TEXT("source_x"), Start.X);
		EdgeObj->SetNumberField(TEXT("target_x"), End.X);
		EdgeObj->SetNumberField(TEXT("backward_dx"), Start.X - End.X);
		BackwardEdgeResults.Add(MakeShared<FJsonValueObject>(EdgeObj));
	}

	TArray<TSharedPtr<FJsonValue>> PinOrderViolationResults;
	for (int32 A = 0; A < LayoutEdges.Num(); ++A)
	{
		if (!LayoutNodes.IsValidIndex(LayoutEdges[A].Source) || !LayoutNodes.IsValidIndex(LayoutEdges[A].Target))
		{
			continue;
		}
		const UeAgentBlueprintOps::FBlueprintLayoutEdge& EdgeA = LayoutEdges[A];
		const FVector2D StartA = UeAgentBlueprintOps::GetEdgeSourcePoint(LayoutNodes, EdgeA, true);
		const FVector2D EndA = UeAgentBlueprintOps::GetEdgeTargetPoint(LayoutNodes, EdgeA, true);
		for (int32 B = A + 1; B < LayoutEdges.Num(); ++B)
		{
			if (!LayoutNodes.IsValidIndex(LayoutEdges[B].Source)
				|| !LayoutNodes.IsValidIndex(LayoutEdges[B].Target)
				|| EdgeA.bExec != LayoutEdges[B].bExec)
			{
				continue;
			}
			const UeAgentBlueprintOps::FBlueprintLayoutEdge& EdgeB = LayoutEdges[B];
			const FVector2D StartB = UeAgentBlueprintOps::GetEdgeSourcePoint(LayoutNodes, EdgeB, true);
			const FVector2D EndB = UeAgentBlueprintOps::GetEdgeTargetPoint(LayoutNodes, EdgeB, true);
			if (EdgeA.Source == EdgeB.Source
				&& EdgeA.Target != EdgeB.Target
				&& EdgeA.SourcePinIndex != INDEX_NONE
				&& EdgeB.SourcePinIndex != INDEX_NONE
				&& !UeAgentBlueprintOps::HasConflictingSharedSourceOrder(LayoutEdges, EdgeA.Target, EdgeB.Target, EdgeA.bExec))
			{
				const double SourcePinDelta = static_cast<double>(EdgeA.SourcePinIndex - EdgeB.SourcePinIndex);
				const double TargetYDelta = EndA.Y - EndB.Y;
				if (FMath::Abs(SourcePinDelta) > 0.5 && FMath::Abs(TargetYDelta) > 4.0 && SourcePinDelta * TargetYDelta < 0.0)
				{
					TSharedPtr<FJsonObject> ViolationObj = MakeShared<FJsonObject>();
					ViolationObj->SetStringField(TEXT("kind"), TEXT("shared_source"));
					ViolationObj->SetStringField(TEXT("source_guid"), LayoutNodes[EdgeA.Source].Guid);
					ViolationObj->SetStringField(TEXT("source_title"), LayoutNodes[EdgeA.Source].Title);
					ViolationObj->SetStringField(TEXT("target_a_guid"), LayoutNodes[EdgeA.Target].Guid);
					ViolationObj->SetStringField(TEXT("target_a_title"), LayoutNodes[EdgeA.Target].Title);
					ViolationObj->SetStringField(TEXT("target_b_guid"), LayoutNodes[EdgeB.Target].Guid);
					ViolationObj->SetStringField(TEXT("target_b_title"), LayoutNodes[EdgeB.Target].Title);
					ViolationObj->SetStringField(TEXT("source_pin_a"), EdgeA.SourcePin);
					ViolationObj->SetStringField(TEXT("source_pin_b"), EdgeB.SourcePin);
					ViolationObj->SetStringField(TEXT("target_pin_a"), EdgeA.TargetPin);
					ViolationObj->SetStringField(TEXT("target_pin_b"), EdgeB.TargetPin);
					ViolationObj->SetNumberField(TEXT("source_pin_index_a"), EdgeA.SourcePinIndex);
					ViolationObj->SetNumberField(TEXT("source_pin_index_b"), EdgeB.SourcePinIndex);
					ViolationObj->SetNumberField(TEXT("target_y_delta"), TargetYDelta);
					PinOrderViolationResults.Add(MakeShared<FJsonValueObject>(ViolationObj));
				}
			}
			if (EdgeA.Target == EdgeB.Target
				&& EdgeA.Source != EdgeB.Source
				&& EdgeA.TargetPinIndex != INDEX_NONE
				&& EdgeB.TargetPinIndex != INDEX_NONE
				&& !UeAgentBlueprintOps::IsRoutedProducerDataIslandPinOrderPair(LayoutNodes, EdgeA, EdgeB)
				&& !UeAgentBlueprintOps::HasConflictingSharedTargetOrder(LayoutEdges, EdgeA.Source, EdgeB.Source, EdgeA.bExec))
			{
				const double TargetPinDelta = static_cast<double>(EdgeA.TargetPinIndex - EdgeB.TargetPinIndex);
				const double SourceYDelta = StartA.Y - StartB.Y;
				if (FMath::Abs(TargetPinDelta) > 0.5 && FMath::Abs(SourceYDelta) > 4.0 && TargetPinDelta * SourceYDelta < 0.0)
				{
					TSharedPtr<FJsonObject> ViolationObj = MakeShared<FJsonObject>();
					ViolationObj->SetStringField(TEXT("kind"), TEXT("shared_target"));
					ViolationObj->SetStringField(TEXT("source_a_guid"), LayoutNodes[EdgeA.Source].Guid);
					ViolationObj->SetStringField(TEXT("source_a_title"), LayoutNodes[EdgeA.Source].Title);
					ViolationObj->SetStringField(TEXT("source_b_guid"), LayoutNodes[EdgeB.Source].Guid);
					ViolationObj->SetStringField(TEXT("source_b_title"), LayoutNodes[EdgeB.Source].Title);
					ViolationObj->SetStringField(TEXT("target_guid"), LayoutNodes[EdgeA.Target].Guid);
					ViolationObj->SetStringField(TEXT("target_title"), LayoutNodes[EdgeA.Target].Title);
					ViolationObj->SetStringField(TEXT("source_pin_a"), EdgeA.SourcePin);
					ViolationObj->SetStringField(TEXT("source_pin_b"), EdgeB.SourcePin);
					ViolationObj->SetStringField(TEXT("target_pin_a"), EdgeA.TargetPin);
					ViolationObj->SetStringField(TEXT("target_pin_b"), EdgeB.TargetPin);
					ViolationObj->SetNumberField(TEXT("target_pin_index_a"), EdgeA.TargetPinIndex);
					ViolationObj->SetNumberField(TEXT("target_pin_index_b"), EdgeB.TargetPinIndex);
					ViolationObj->SetNumberField(TEXT("source_y_delta"), SourceYDelta);
					PinOrderViolationResults.Add(MakeShared<FJsonValueObject>(ViolationObj));
				}
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> PinAlignmentProbeResults;
	if (bEmitPinAlignmentProbes && PinAlignmentProbeLimit > 0)
	{
		auto MatchesProbeText = [](const FString& Value, const FString& Filter) -> bool
		{
			return Filter.IsEmpty() || Value.Contains(Filter, ESearchCase::IgnoreCase);
		};
		for (int32 EdgeIndex = 0; EdgeIndex < LayoutEdges.Num() && PinAlignmentProbeResults.Num() < PinAlignmentProbeLimit; ++EdgeIndex)
		{
			const UeAgentBlueprintOps::FBlueprintLayoutEdge& Edge = LayoutEdges[EdgeIndex];
			if (!LayoutNodes.IsValidIndex(Edge.Source) || !LayoutNodes.IsValidIndex(Edge.Target))
			{
				continue;
			}
			const UeAgentBlueprintOps::FBlueprintLayoutNode& SourceNode = LayoutNodes[Edge.Source];
			const UeAgentBlueprintOps::FBlueprintLayoutNode& TargetNode = LayoutNodes[Edge.Target];
			if (!MatchesProbeText(SourceNode.Title, PinAlignmentProbeSourceTitle)
				|| !MatchesProbeText(Edge.SourcePin, PinAlignmentProbeSourcePin)
				|| !MatchesProbeText(TargetNode.Title, PinAlignmentProbeTargetTitle)
				|| !MatchesProbeText(Edge.TargetPin, PinAlignmentProbeTargetPin))
			{
				continue;
			}

			FVector2D SourceMeasuredAnchor = FVector2D::ZeroVector;
			FVector2D TargetMeasuredAnchor = FVector2D::ZeroVector;
			const bool bSourceMeasuredAnchor = UeAgentBlueprintOps::TryGetMeasuredPinAnchor(
				SourceNode,
				EGPD_Output,
				Edge.SourcePinIndex,
				SourceMeasuredAnchor);
			const bool bTargetMeasuredAnchor = UeAgentBlueprintOps::TryGetMeasuredPinAnchor(
				TargetNode,
				EGPD_Input,
				Edge.TargetPinIndex,
				TargetMeasuredAnchor);
			const double SourcePinOffsetY = UeAgentBlueprintOps::GetPinYOffset(SourceNode, EGPD_Output, Edge.SourcePinIndex);
			const double TargetPinOffsetY = UeAgentBlueprintOps::GetPinYOffset(TargetNode, EGPD_Input, Edge.TargetPinIndex);
			const FVector2D OldSourcePoint = UeAgentBlueprintOps::GetEdgeSourcePoint(LayoutNodes, Edge, false);
			const FVector2D OldTargetPoint = UeAgentBlueprintOps::GetEdgeTargetPoint(LayoutNodes, Edge, false);
			const FVector2D NewSourcePoint = UeAgentBlueprintOps::GetEdgeSourcePoint(LayoutNodes, Edge, true);
			const FVector2D NewTargetPoint = UeAgentBlueprintOps::GetEdgeTargetPoint(LayoutNodes, Edge, true);
			const double DesiredSourceTopY = TargetNode.NewY + TargetPinOffsetY - SourcePinOffsetY;
			const double SourceFormulaPinY = DesiredSourceTopY + SourcePinOffsetY;
			const double SourceFormulaResidualY = SourceFormulaPinY - NewTargetPoint.Y;
			const double DesiredTargetTopY = SourceNode.NewY + SourcePinOffsetY - TargetPinOffsetY;
			const double TargetFormulaPinY = DesiredTargetTopY + TargetPinOffsetY;
			const double TargetFormulaResidualY = NewSourcePoint.Y - TargetFormulaPinY;
			const double ActualResidualY = NewSourcePoint.Y - NewTargetPoint.Y;

			TSharedPtr<FJsonObject> ProbeObj = MakeShared<FJsonObject>();
			ProbeObj->SetNumberField(TEXT("edge_index"), EdgeIndex);
			ProbeObj->SetBoolField(TEXT("exec_edge"), Edge.bExec);
			ProbeObj->SetStringField(TEXT("source_guid"), SourceNode.Guid);
			ProbeObj->SetStringField(TEXT("source_title"), SourceNode.Title);
			ProbeObj->SetStringField(TEXT("source_pin"), Edge.SourcePin);
			ProbeObj->SetNumberField(TEXT("source_pin_index"), Edge.SourcePinIndex);
			ProbeObj->SetStringField(TEXT("target_guid"), TargetNode.Guid);
			ProbeObj->SetStringField(TEXT("target_title"), TargetNode.Title);
			ProbeObj->SetStringField(TEXT("target_pin"), Edge.TargetPin);
			ProbeObj->SetNumberField(TEXT("target_pin_index"), Edge.TargetPinIndex);
			ProbeObj->SetBoolField(TEXT("source_measured_anchor"), bSourceMeasuredAnchor);
			ProbeObj->SetBoolField(TEXT("target_measured_anchor"), bTargetMeasuredAnchor);
			ProbeObj->SetNumberField(TEXT("source_pin_offset_y"), SourcePinOffsetY);
			ProbeObj->SetNumberField(TEXT("target_pin_offset_y"), TargetPinOffsetY);
			ProbeObj->SetNumberField(TEXT("source_old_top_y"), SourceNode.OldY);
			ProbeObj->SetNumberField(TEXT("target_old_top_y"), TargetNode.OldY);
			ProbeObj->SetNumberField(TEXT("source_new_top_y"), SourceNode.NewY);
			ProbeObj->SetNumberField(TEXT("target_new_top_y"), TargetNode.NewY);
			ProbeObj->SetNumberField(TEXT("source_pin_y_before"), OldSourcePoint.Y);
			ProbeObj->SetNumberField(TEXT("target_pin_y_before"), OldTargetPoint.Y);
			ProbeObj->SetNumberField(TEXT("residual_y_before"), OldSourcePoint.Y - OldTargetPoint.Y);
			ProbeObj->SetNumberField(TEXT("source_pin_y_after_layout"), NewSourcePoint.Y);
			ProbeObj->SetNumberField(TEXT("target_pin_y_after_layout"), NewTargetPoint.Y);
			ProbeObj->SetNumberField(TEXT("actual_residual_y_after_layout"), ActualResidualY);
			ProbeObj->SetNumberField(TEXT("actual_abs_residual_y_after_layout"), FMath::Abs(ActualResidualY));
			ProbeObj->SetNumberField(TEXT("desired_source_top_y_for_target_pin"), DesiredSourceTopY);
			ProbeObj->SetNumberField(TEXT("source_delta_y_to_align_target"), DesiredSourceTopY - SourceNode.NewY);
			ProbeObj->SetNumberField(TEXT("source_formula_pin_y"), SourceFormulaPinY);
			ProbeObj->SetNumberField(TEXT("source_formula_residual_y"), SourceFormulaResidualY);
			ProbeObj->SetNumberField(TEXT("desired_target_top_y_for_source_pin"), DesiredTargetTopY);
			ProbeObj->SetNumberField(TEXT("target_delta_y_to_align_source"), DesiredTargetTopY - TargetNode.NewY);
			ProbeObj->SetNumberField(TEXT("target_formula_pin_y"), TargetFormulaPinY);
			ProbeObj->SetNumberField(TEXT("target_formula_residual_y"), TargetFormulaResidualY);
			ProbeObj->SetBoolField(TEXT("alignment_formula_passed"), FMath::Abs(SourceFormulaResidualY) <= 0.01 && FMath::Abs(TargetFormulaResidualY) <= 0.01);
			ProbeObj->SetBoolField(TEXT("layout_currently_aligned"), FMath::Abs(ActualResidualY) <= 1.0);
			PinAlignmentProbeResults.Add(MakeShared<FJsonValueObject>(ProbeObj));
		}
	}

	TArray<TSharedPtr<FJsonValue>> ReroutePlanResults;
	TArray<TSharedPtr<FJsonValue>> BusCorridorResults;
	int32 DirectRouteCount = 0;
	int32 LocalBusRouteCount = 0;
	int32 ExternalBusRouteCount = 0;
	int32 SharedFanoutBusRouteCount = 0;
	TSet<FString> EmittedBusIds;
	for (const UeAgentBlueprintOps::FBlueprintLayoutReroutePlan& Plan : RerouteInsertionPlan)
	{
		if (!RerouteCandidateEdges.IsValidIndex(Plan.RawEdgeIndex))
		{
			continue;
		}
		const UeAgentBlueprintOps::FBlueprintLayoutEdge& Edge = RerouteCandidateEdges[Plan.RawEdgeIndex];
		if (!LayoutNodes.IsValidIndex(Edge.Source) || !LayoutNodes.IsValidIndex(Edge.Target))
		{
			continue;
		}
		TSharedPtr<FJsonObject> PlanObj = MakeShared<FJsonObject>();
		PlanObj->SetNumberField(TEXT("raw_edge_index"), Plan.RawEdgeIndex);
		PlanObj->SetStringField(TEXT("source_guid"), LayoutNodes[Edge.Source].Guid);
		PlanObj->SetStringField(TEXT("source_title"), LayoutNodes[Edge.Source].Title);
		PlanObj->SetStringField(TEXT("target_guid"), LayoutNodes[Edge.Target].Guid);
		PlanObj->SetStringField(TEXT("target_title"), LayoutNodes[Edge.Target].Title);
		PlanObj->SetStringField(TEXT("source_pin"), Edge.SourcePin);
		PlanObj->SetStringField(TEXT("target_pin"), Edge.TargetPin);
		PlanObj->SetBoolField(TEXT("exec_edge"), Edge.bExec);
		PlanObj->SetStringField(TEXT("reason"), Plan.Reason);
		PlanObj->SetStringField(TEXT("route_kind"), UeAgentBlueprintOps::LayoutRouteKindToString(Plan.RouteKind));
		PlanObj->SetStringField(TEXT("bus_id"), Plan.BusId);
		PlanObj->SetStringField(TEXT("fanout_group_id"), Plan.FanoutGroupId);
		PlanObj->SetNumberField(TEXT("bus_lane_index"), Plan.BusLaneIndex);
		PlanObj->SetStringField(TEXT("bus_lane_side"), Plan.BusLaneSide);
		PlanObj->SetNumberField(TEXT("first_x"), Plan.FirstPoint.X);
		PlanObj->SetNumberField(TEXT("first_y"), Plan.FirstPoint.Y);
		PlanObj->SetNumberField(TEXT("second_x"), Plan.SecondPoint.X);
		PlanObj->SetNumberField(TEXT("second_y"), Plan.SecondPoint.Y);
		TArray<TSharedPtr<FJsonValue>> ReroutePointResults;
		TArray<FVector2D> PlanPoints = Plan.ReroutePoints;
		if (PlanPoints.Num() == 0)
		{
			PlanPoints = { Plan.FirstPoint, Plan.SecondPoint };
		}
		for (const FVector2D& Point : PlanPoints)
		{
			TSharedPtr<FJsonObject> PointObj = MakeShared<FJsonObject>();
			PointObj->SetNumberField(TEXT("x"), Point.X);
			PointObj->SetNumberField(TEXT("y"), Point.Y);
			ReroutePointResults.Add(MakeShared<FJsonValueObject>(PointObj));
		}
		PlanObj->SetNumberField(TEXT("point_count"), ReroutePointResults.Num());
		PlanObj->SetArrayField(TEXT("points"), ReroutePointResults);
		ReroutePlanResults.Add(MakeShared<FJsonValueObject>(PlanObj));

		switch (Plan.RouteKind)
		{
		case UeAgentBlueprintOps::EBlueprintLayoutRouteKind::Direct:
			++DirectRouteCount;
			break;
		case UeAgentBlueprintOps::EBlueprintLayoutRouteKind::ExternalBus:
			++ExternalBusRouteCount;
			break;
		case UeAgentBlueprintOps::EBlueprintLayoutRouteKind::SharedFanoutBus:
			++LocalBusRouteCount;
			break;
		case UeAgentBlueprintOps::EBlueprintLayoutRouteKind::LocalBus:
		default:
			++LocalBusRouteCount;
			break;
		}

		if (!Plan.BusId.IsEmpty() && !EmittedBusIds.Contains(Plan.BusId))
		{
			EmittedBusIds.Add(Plan.BusId);
			TSharedPtr<FJsonObject> BusObj = MakeShared<FJsonObject>();
			BusObj->SetStringField(TEXT("bus_id"), Plan.BusId);
			BusObj->SetStringField(TEXT("route_kind"), UeAgentBlueprintOps::LayoutRouteKindToString(Plan.RouteKind));
			BusObj->SetStringField(TEXT("reason"), Plan.Reason);
			BusObj->SetNumberField(TEXT("bus_lane_index"), Plan.BusLaneIndex);
			BusObj->SetStringField(TEXT("bus_lane_side"), Plan.BusLaneSide);
			double BusTrunkStartX = PlanPoints.Num() > 0 ? PlanPoints[0].X : Plan.FirstPoint.X;
			double BusTrunkY = PlanPoints.Num() > 0 ? PlanPoints[0].Y : Plan.FirstPoint.Y;
			double BusTrunkEndX = PlanPoints.Num() > 1 ? PlanPoints[1].X : Plan.SecondPoint.X;
			int32 EdgeCountForBus = 0;
			TArray<TSharedPtr<FJsonValue>> BusEdgeIndices;
			for (const UeAgentBlueprintOps::FBlueprintLayoutReroutePlan& OtherPlan : RerouteInsertionPlan)
			{
				if (OtherPlan.BusId.Equals(Plan.BusId, ESearchCase::CaseSensitive))
				{
					++EdgeCountForBus;
					BusEdgeIndices.Add(MakeShared<FJsonValueNumber>(OtherPlan.RawEdgeIndex));
					TArray<FVector2D> OtherPlanPoints = OtherPlan.ReroutePoints;
					if (OtherPlanPoints.Num() == 0)
					{
						OtherPlanPoints = { OtherPlan.FirstPoint, OtherPlan.SecondPoint };
					}
					if (OtherPlanPoints.Num() > 0)
					{
						BusTrunkStartX = FMath::Min(BusTrunkStartX, OtherPlanPoints[0].X);
						BusTrunkY = OtherPlanPoints[0].Y;
					}
					if (OtherPlanPoints.Num() > 1)
					{
						BusTrunkEndX = FMath::Max(BusTrunkEndX, OtherPlanPoints[1].X);
					}
				}
			}
			BusObj->SetNumberField(TEXT("trunk_start_x"), BusTrunkStartX);
			BusObj->SetNumberField(TEXT("trunk_y"), BusTrunkY);
			BusObj->SetNumberField(TEXT("trunk_end_x"), BusTrunkEndX);
			BusObj->SetNumberField(TEXT("edge_count"), EdgeCountForBus);
			BusObj->SetArrayField(TEXT("raw_edge_indices"), BusEdgeIndices);
			BusCorridorResults.Add(MakeShared<FJsonValueObject>(BusObj));
		}
	}

	if (!bDryRun && (MovedNodeCount > 0 || (bInsertRerouteNodes && RerouteInsertionPlan.Num() > 0)))
	{
		const FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "BlueprintLayoutGraph", "Layout Blueprint Graph"));
		Blueprint->Modify();
		Graph->Modify();
		for (const UeAgentBlueprintOps::FBlueprintLayoutNode& LayoutNode : LayoutNodes)
		{
			if (!LayoutNode.Node)
			{
				continue;
			}
			LayoutNode.Node->Modify();
			LayoutNode.Node->NodePosX = FMath::RoundToInt(LayoutNode.NewX);
			LayoutNode.Node->NodePosY = FMath::RoundToInt(LayoutNode.NewY);
		}
		if (bInsertRerouteNodes)
		{
			InsertedRerouteEdgeCount = UeAgentBlueprintOps::ApplyRerouteInsertionPlan(Blueprint, Graph, LayoutNodes, RawLayoutEdges, RerouteInsertionPlan);
		}
		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
	RestoreEditorViewState();

	if (!bDryRun && bCompileAfterLayout)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (!bDryRun && bSaveAfterLayout && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> AfterMetricsJson = UeAgentBlueprintOps::LayoutMetricsToJson(AfterMetrics);
	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("graph_name"), Graph->GetName());
	OutData->SetStringField(TEXT("graph_path"), Graph->GetPathName());
	OutData->SetStringField(TEXT("layout_strategy"), TEXT("rule_driven_port_constraint_layered_layout_v2"));
	OutData->SetStringField(TEXT("selected_candidate"), TEXT("port_constraint_layered_layout"));
	OutData->SetBoolField(TEXT("require_real_geometry"), bRequireRealGeometry);
	OutData->SetBoolField(TEXT("used_real_geometry"), GeometryStats.bUsedRealGeometry);
	OutData->SetStringField(TEXT("geometry_source"), GeometryStats.GeometrySource);
	OutData->SetBoolField(TEXT("geometry_window_prepared"), GeometryStats.bWindowPrepared);
	OutData->SetNumberField(TEXT("measured_node_count"), GeometryStats.MeasuredNodeCount);
	OutData->SetNumberField(TEXT("pin_widget_count"), GeometryStats.PinWidgetCount);
	OutData->SetNumberField(TEXT("pin_geometry_anchor_count"), GeometryStats.PinGeometryAnchorCount);
	OutData->SetNumberField(TEXT("pin_offset_anchor_count"), GeometryStats.PinOffsetAnchorCount);
	OutData->SetNumberField(TEXT("missing_pin_anchor_count"), GeometryStats.MissingPinAnchorCount);
	if (!GeometryStats.Error.IsEmpty())
	{
		OutData->SetStringField(TEXT("geometry_error"), GeometryStats.Error);
	}
	OutData->SetNumberField(TEXT("node_count"), LayoutNodes.Num());
	OutData->SetNumberField(TEXT("edge_count"), LayoutEdges.Num());
	OutData->SetNumberField(TEXT("raw_edge_count"), RawLayoutEdges.Num());
	OutData->SetNumberField(TEXT("reroute_node_count"), RerouteNodeCount);
	OutData->SetNumberField(TEXT("component_count"), ConnectedComponents.Num());
	OutData->SetNumberField(TEXT("rank_count"), RankCount);
	OutData->SetBoolField(TEXT("node_alignment_flow_enabled"), true);
	OutData->SetNumberField(TEXT("pin_aligned_compaction_move_count"), AlignmentStats.PinAlignedCompactionMoveCount);
	OutData->SetNumberField(TEXT("final_single_consumer_pin_alignment_candidate_count"), AlignmentStats.FinalSingleConsumerPinAlignmentCandidateCount);
	OutData->SetNumberField(TEXT("final_single_consumer_pin_alignment_move_count"), AlignmentStats.FinalSingleConsumerPinAlignmentMoveCount);
	OutData->SetNumberField(TEXT("direct_leaf_exec_input_docking_candidate_count"), AlignmentStats.DirectLeafExecInputDockingCandidateCount);
	OutData->SetNumberField(TEXT("direct_leaf_exec_input_docking_move_count"), AlignmentStats.DirectLeafExecInputDockingMoveCount);
	OutData->SetNumberField(TEXT("exec_input_expression_island_candidate_count"), AlignmentStats.ExecInputExpressionIslandCandidateCount);
	OutData->SetNumberField(TEXT("exec_input_expression_island_move_count"), AlignmentStats.ExecInputExpressionIslandMoveCount);
	OutData->SetNumberField(TEXT("shared_exec_input_fanout_docking_candidate_count"), AlignmentStats.SharedExecInputFanoutDockingCandidateCount);
	OutData->SetNumberField(TEXT("shared_exec_input_fanout_docking_move_count"), AlignmentStats.SharedExecInputFanoutDockingMoveCount);
	OutData->SetNumberField(TEXT("moved_node_count"), MovedNodeCount);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("insert_reroute_nodes"), bInsertRerouteNodes);
	OutData->SetBoolField(TEXT("replace_existing_reroute_nodes"), bReplaceExistingRerouteNodes);
	OutData->SetBoolField(TEXT("restore_editor_view"), bRestoreEditorView);
	OutData->SetBoolField(TEXT("editor_view_captured"), bCapturedEditorView);
	OutData->SetBoolField(TEXT("editor_view_restored"), bRestoredEditorView);
	OutData->SetBoolField(TEXT("snap_restored_editor_view_to_pixel_grid"), bSnapRestoredEditorViewToPixelGrid);
	OutData->SetBoolField(TEXT("persist_restored_editor_view_state"), bAllowPersistRestoredEditorViewState);
	OutData->SetBoolField(TEXT("persisted_restored_editor_view_state"), bPersistedRestoredEditorViewState);
	OutData->SetNumberField(TEXT("removed_restored_editor_view_state_entry_count"), RemovedRestoredEditorViewStateEntryCount);
	if (bCapturedEditorView)
	{
		OutData->SetNumberField(TEXT("captured_editor_view_x"), CapturedEditorViewLocation.X);
		OutData->SetNumberField(TEXT("captured_editor_view_y"), CapturedEditorViewLocation.Y);
		OutData->SetNumberField(TEXT("captured_editor_view_zoom"), CapturedEditorViewZoom);
	}
	OutData->SetNumberField(TEXT("removed_existing_reroute_node_count"), RemovedExistingRerouteNodeCount);
	OutData->SetNumberField(TEXT("reroute_plan_count"), RerouteInsertionPlan.Num());
	OutData->SetStringField(TEXT("reroute_plan_edge_source"), bReroutePlanUsesCollapsedEdges ? TEXT("collapsed_edges") : TEXT("raw_edges"));
	OutData->SetNumberField(TEXT("reroute_plan_edge_count"), RerouteCandidateEdges.Num());
	OutData->SetNumberField(TEXT("reroute_inserted_edge_count"), InsertedRerouteEdgeCount);
	TSharedPtr<FJsonObject> RouteKindCounts = MakeShared<FJsonObject>();
	RouteKindCounts->SetNumberField(TEXT("direct"), DirectRouteCount);
	RouteKindCounts->SetNumberField(TEXT("local_bus"), LocalBusRouteCount);
	RouteKindCounts->SetNumberField(TEXT("local_deflection"), LocalBusRouteCount);
	RouteKindCounts->SetNumberField(TEXT("external_bus"), ExternalBusRouteCount);
	RouteKindCounts->SetNumberField(TEXT("shared_fanout_bus"), SharedFanoutBusRouteCount);
	OutData->SetObjectField(TEXT("route_kind_counts"), RouteKindCounts);
	OutData->SetNumberField(TEXT("local_bus_edge_count"), LocalBusRouteCount);
	OutData->SetNumberField(TEXT("external_bus_edge_count"), ExternalBusRouteCount);
	OutData->SetNumberField(TEXT("shared_fanout_bus_edge_count"), SharedFanoutBusRouteCount);
	OutData->SetNumberField(TEXT("orphan_local_reroute_count"), 0);
	OutData->SetBoolField(TEXT("changed"), !bDryRun && (MovedNodeCount > 0 || InsertedRerouteEdgeCount > 0 || RemovedExistingRerouteNodeCount > 0));
	OutData->SetBoolField(TEXT("saved"), !bDryRun && bSaveAfterLayout);
	OutData->SetNumberField(TEXT("overlap_count_before"), BeforeMetrics.OverlapCount);
	OutData->SetNumberField(TEXT("overlap_count_after"), AfterMetrics.OverlapCount);
	OutData->SetNumberField(TEXT("edge_crossing_count_before"), BeforeMetrics.EdgeCrossingCount);
	OutData->SetNumberField(TEXT("edge_crossing_count_after"), AfterMetrics.EdgeCrossingCount);
	OutData->SetNumberField(TEXT("edge_node_intersection_count_before"), BeforeMetrics.EdgeNodeIntersectionCount);
	OutData->SetNumberField(TEXT("edge_node_intersection_count_after"), AfterMetrics.EdgeNodeIntersectionCount);
	OutData->SetNumberField(TEXT("total_wire_manhattan_length_before"), BeforeMetrics.TotalWireLength);
	OutData->SetNumberField(TEXT("total_wire_manhattan_length_after"), AfterMetrics.TotalWireLength);
	OutData->SetNumberField(TEXT("total_non_exec_wire_manhattan_length_before"), BeforeMetrics.TotalNonExecWireLength);
	OutData->SetNumberField(TEXT("total_non_exec_wire_manhattan_length_after"), AfterMetrics.TotalNonExecWireLength);
	OutData->SetObjectField(TEXT("metrics_before"), UeAgentBlueprintOps::LayoutMetricsToJson(BeforeMetrics));
	OutData->SetObjectField(TEXT("metrics_after"), AfterMetricsJson);
	OutData->SetObjectField(TEXT("metrics_after_reroute_plan"), UeAgentBlueprintOps::LayoutMetricsToJson(ReroutePlanMetrics));
	OutData->SetNumberField(TEXT("layout_score"), FinalScore);
	OutData->SetNumberField(TEXT("layout_score_after_reroute_plan"), ReroutePlanScore);
	auto EmitLegacyCandidateMetrics = [&OutData](const TCHAR* CandidateName, const UeAgentBlueprintOps::FBlueprintLayoutMetrics& Metrics, const double Score)
	{
		OutData->SetObjectField(FString::Printf(TEXT("metrics_%s_candidate"), CandidateName), UeAgentBlueprintOps::LayoutMetricsToJson(Metrics));
		OutData->SetNumberField(FString::Printf(TEXT("score_%s_candidate"), CandidateName), Score);
	};
	EmitLegacyCandidateMetrics(TEXT("exec_dominant"), AfterMetrics, FinalScore);
	EmitLegacyCandidateMetrics(TEXT("exec_dominant_centered"), AfterMetrics, FinalScore);
	EmitLegacyCandidateMetrics(TEXT("dependency_dominant"), AfterMetrics, FinalScore);
	EmitLegacyCandidateMetrics(TEXT("dependency_dominant_refined"), AfterMetrics, FinalScore);
	EmitLegacyCandidateMetrics(TEXT("dependency_dominant_centered"), AfterMetrics, FinalScore);
	EmitLegacyCandidateMetrics(TEXT("existing_refined"), AfterMetrics, FinalScore);
	EmitLegacyCandidateMetrics(TEXT("existing_semantic_port_constraint"), AfterMetrics, FinalScore);
	EmitLegacyCandidateMetrics(TEXT("semantic_port_constraint"), AfterMetrics, FinalScore);
	OutData->SetArrayField(TEXT("nodes"), NodeResults);
	OutData->SetArrayField(TEXT("backward_edges_after"), BackwardEdgeResults);
	OutData->SetArrayField(TEXT("pin_order_violations_after"), PinOrderViolationResults);
	if (bEmitPinAlignmentProbes)
	{
		OutData->SetNumberField(TEXT("pin_alignment_probe_count"), PinAlignmentProbeResults.Num());
		OutData->SetArrayField(TEXT("pin_alignment_probes"), PinAlignmentProbeResults);
	}
	OutData->SetArrayField(TEXT("bus_corridors"), BusCorridorResults);
	OutData->SetArrayField(TEXT("reroute_plan"), ReroutePlanResults);
	return true;
}
