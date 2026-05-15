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

		struct FBlueprintLayoutGraphTarget
		{
			UEdGraph* Graph = nullptr;
			FString GraphType;
		};

		static bool IsLayoutRerouteNode(const UEdGraphNode* Node)
		{
			return Cast<UK2Node_Knot>(Node) != nullptr;
		}

		static int32 CountLayoutSemanticNodes(const UEdGraph* Graph)
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

		static bool CollectBlueprintLayoutTargets(
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

		static bool TryComputeGraphEditorAccessOffsetX(double& OutOffset)
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

	}
}
