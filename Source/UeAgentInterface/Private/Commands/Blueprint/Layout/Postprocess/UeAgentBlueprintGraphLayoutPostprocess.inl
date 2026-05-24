namespace UeAgentBlueprintOps
{
	namespace
	{
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
