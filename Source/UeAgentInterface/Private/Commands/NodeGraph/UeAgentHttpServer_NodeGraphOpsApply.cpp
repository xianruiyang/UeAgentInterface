// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_NodeGraphOps.h"
#include "UeAgentHttpServer_NodeGraphOpsInternal.h"
#include "Commands/Blueprint/Layout/Core/UeAgentBlueprintAutoLayoutAlgorithm.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/RigVMEdGraph.h"
#include "EdGraph/RigVMEdGraphNode.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphEditor.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/Nodes/RigVMRerouteNode.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"

namespace UeAgentNodeGraphOps
{
	namespace Internal
	{
		bool ApplySharedGraphNodePosition(UEdGraphNode* GraphNode, const int32 NewX, const int32 NewY)
		{
			if (!GraphNode)
			{
				return false;
			}
			if (URigVMEdGraphNode* RigNode = Cast<URigVMEdGraphNode>(GraphNode))
			{
				URigVMController* Controller = RigNode->GetController();
				URigVMNode* ModelNode = RigNode->GetModelNode();
				if (Controller && ModelNode)
				{
					return Controller->SetNodePosition(ModelNode, FVector2D(NewX, NewY), true, false, false);
				}
			}
			GraphNode->Modify();
			GraphNode->NodePosX = NewX;
			GraphNode->NodePosY = NewY;
			return true;
		}
		UEdGraphPin* FindSharedLayoutGraphPin(
			const FUniversalLayoutModel& Model,
			const UeAgentBlueprintAutoLayout::FEdge& Edge,
			const bool bSource)
		{
			const int32 NodeIndex = bSource ? Edge.Source : Edge.Target;
			if (!Model.NodeRefs.IsValidIndex(NodeIndex))
			{
				return nullptr;
			}
			UEdGraphNode* GraphNode = Model.NodeRefs[NodeIndex].GraphNode;
			if (!GraphNode)
			{
				return nullptr;
			}
			const EEdGraphPinDirection Direction = bSource ? EGPD_Output : EGPD_Input;
			const int32 PinIndex = bSource ? Edge.SourcePinIndex : Edge.TargetPinIndex;
			const FString& PinName = bSource ? Edge.SourcePin : Edge.TargetPin;
			const TArray<UEdGraphPin*> Pins = GetSharedGraphPinsByDirection(GraphNode, Direction);
			if (Pins.IsValidIndex(PinIndex))
			{
				return Pins[PinIndex];
			}
			for (UEdGraphPin* Pin : Pins)
			{
				if (Pin && GraphPinNameString(Pin) == PinName)
				{
					return Pin;
				}
			}
			return nullptr;
		}
		bool TryGetRigVMPinPathsForEdge(
			const FUniversalLayoutModel& Model,
			const UeAgentBlueprintAutoLayout::FEdge& Edge,
			FString& OutSourcePath,
			FString& OutTargetPath)
		{
			UEdGraphPin* SourceGraphPin = FindSharedLayoutGraphPin(Model, Edge, true);
			UEdGraphPin* TargetGraphPin = FindSharedLayoutGraphPin(Model, Edge, false);
			if (!SourceGraphPin || !TargetGraphPin)
			{
				return false;
			}
			const URigVMEdGraphNode* SourceNode = Cast<URigVMEdGraphNode>(SourceGraphPin->GetOwningNode());
			const URigVMEdGraphNode* TargetNode = Cast<URigVMEdGraphNode>(TargetGraphPin->GetOwningNode());
			if (!SourceNode || !TargetNode)
			{
				return false;
			}
			const URigVMPin* SourceModelPin = SourceNode->FindModelPinFromGraphPin(SourceGraphPin);
			const URigVMPin* TargetModelPin = TargetNode->FindModelPinFromGraphPin(TargetGraphPin);
			if (!SourceModelPin || !TargetModelPin)
			{
				return false;
			}
			OutSourcePath = SourceModelPin->GetPinPath();
			OutTargetPath = TargetModelPin->GetPinPath();
			return !OutSourcePath.IsEmpty() && !OutTargetPath.IsEmpty();
		}
		FString MakeRigVMLinkPath(const FString& SourcePath, const FString& TargetPath)
		{
			return FString::Printf(TEXT("%s -> %s"), *SourcePath, *TargetPath);
		}
		int32 CountRigVMRerouteNodes(const URigVMEdGraph* RigGraph)
		{
			const URigVMGraph* ModelGraph = RigGraph ? RigGraph->GetModel() : nullptr;
			if (!ModelGraph)
			{
				return 0;
			}
			int32 Count = 0;
			for (URigVMNode* Node : ModelGraph->GetNodes())
			{
				if (Cast<URigVMRerouteNode>(Node))
				{
					++Count;
				}
			}
			return Count;
		}
		bool RemoveRigVMRerouteNodesAndRestoreSemanticLinks(
			URigVMEdGraph* RigGraph,
			const TArray<TPair<FString, FString>>& EdgePinPaths,
			FString& OutError)
		{
			URigVMGraph* ModelGraph = RigGraph ? RigGraph->GetModel() : nullptr;
			URigVMController* Controller = RigGraph ? RigGraph->GetController() : nullptr;
			if (!ModelGraph || !Controller)
			{
				OutError = TEXT("rigvm_graph_or_controller_unavailable");
				return false;
			}

			TArray<URigVMRerouteNode*> RerouteNodes;
			for (URigVMNode* Node : ModelGraph->GetNodes())
			{
				if (URigVMRerouteNode* RerouteNode = Cast<URigVMRerouteNode>(Node))
				{
					RerouteNodes.Add(RerouteNode);
				}
			}
			for (URigVMRerouteNode* RerouteNode : RerouteNodes)
			{
				if (RerouteNode && !Controller->RemoveNode(RerouteNode, true, false))
				{
					OutError = FString::Printf(TEXT("rigvm_remove_reroute_failed node=%s"), *RerouteNode->GetName());
					return false;
				}
			}
			for (const TPair<FString, FString>& EdgePath : EdgePinPaths)
			{
				if (ModelGraph->FindLink(MakeRigVMLinkPath(EdgePath.Key, EdgePath.Value)))
				{
					continue;
				}
				if (!Controller->AddLink(EdgePath.Key, EdgePath.Value, true, false))
				{
					OutError = FString::Printf(TEXT("rigvm_restore_semantic_link_failed link=%s"), *MakeRigVMLinkPath(EdgePath.Key, EdgePath.Value));
					return false;
				}
			}
			return true;
		}
		FVector2D GetRigVMRerouteNodeSpacerSize()
		{
			// Matches SGraphNodeKnot::NodeSpacerSize. Layout plans store the visible knot/control-point center.
			return FVector2D(42.0, 24.0);
		}
		FVector2D ConvertLayoutKnotControlPointToRigVMNodePosition(const FVector2D& ControlPoint)
		{
			return ControlPoint - GetRigVMRerouteNodeSpacerSize() * 0.5;
		}
		struct FRigVMRerouteControlPointAlignment
		{
			TWeakObjectPtr<URigVMRerouteNode> RerouteNode;
			FVector2D DesiredControlPoint = FVector2D::ZeroVector;
		};
		UEdGraphPin* FindRigVMRerouteVisibleGraphPin(URigVMEdGraphNode* EdNode)
		{
			if (!EdNode)
			{
				return nullptr;
			}
			int32 InputPinIndex = INDEX_NONE;
			int32 OutputPinIndex = INDEX_NONE;
			if (EdNode->ShouldDrawNodeAsControlPointOnly(InputPinIndex, OutputPinIndex)
				&& EdNode->Pins.IsValidIndex(OutputPinIndex))
			{
				return EdNode->Pins[OutputPinIndex];
			}
			for (UEdGraphPin* Pin : EdNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output && Pin->ParentPin == nullptr)
				{
					return Pin;
				}
			}
			return nullptr;
		}
		bool TryMeasureRigVMRerouteControlPoint(
			URigVMEdGraph* RigGraph,
			SGraphPanel& GraphPanel,
			URigVMRerouteNode* RerouteNode,
			FVector2D& OutControlPoint)
		{
			if (!RigGraph || !RerouteNode)
			{
				return false;
			}
			URigVMEdGraphNode* EdNode = Cast<URigVMEdGraphNode>(
				RigGraph->FindNodeForModelNodeName(RerouteNode->GetFName()));
			if (!EdNode)
			{
				return false;
			}
			TSharedPtr<SGraphNode> GraphNodeWidget = GraphPanel.GetNodeWidgetFromGuid(EdNode->NodeGuid);
			if (!GraphNodeWidget.IsValid())
			{
				GraphPanel.RefreshNode(*EdNode);
				PumpAndTickSlateForLayoutGeometry();
				GraphNodeWidget = GraphPanel.GetNodeWidgetFromGuid(EdNode->NodeGuid);
			}
			if (!GraphNodeWidget.IsValid())
			{
				return false;
			}
			UEdGraphPin* VisiblePin = FindRigVMRerouteVisibleGraphPin(EdNode);
			if (!VisiblePin)
			{
				return false;
			}
			TSharedPtr<SGraphPin> PinWidget = GraphNodeWidget->FindWidgetForPin(VisiblePin);
			if (!PinWidget.IsValid())
			{
				return false;
			}
			FVector2D Anchor = FVector2D::ZeroVector;
			if (!TryGetPinImageCenterAnchor(GraphPanel, *GraphNodeWidget, PinWidget.ToSharedRef(), Anchor))
			{
				return false;
			}
			OutControlPoint = RerouteNode->GetPosition() + Anchor;
			return IsFiniteLayoutPoint(OutControlPoint);
		}
		bool AlignRigVMRerouteControlPointsToSlateGeometry(
			URigVMEdGraph* RigGraph,
			URigVMController* Controller,
			const TArray<FRigVMRerouteControlPointAlignment>& Alignments,
			FString& OutError)
		{
			if (Alignments.Num() == 0)
			{
				return true;
			}
			if (!RigGraph || !Controller)
			{
				OutError = TEXT("rigvm_reroute_alignment_graph_or_controller_unavailable");
				return false;
			}
			if (!FSlateApplication::IsInitialized())
			{
				OutError = TEXT("rigvm_reroute_alignment_slate_not_initialized");
				return false;
			}

			FGraphAppearanceInfo AppearanceInfo;
			AppearanceInfo.CornerText = FText::FromString(TEXT("UAI RigVM reroute alignment probe"));
			TSharedPtr<SGraphEditor> GraphEditor = SNew(SGraphEditor)
				.IsEditable(false)
				.DisplayAsReadOnly(true)
				.ShowGraphStateOverlay(false)
				.Appearance(AppearanceInfo)
				.GraphToEdit(RigGraph);
			PumpAndTickSlateForLayoutGeometry();
			SGraphPanel* GraphPanel = GraphEditor.IsValid() ? GraphEditor->GetGraphPanel() : nullptr;
			if (!GraphPanel)
			{
				OutError = TEXT("rigvm_reroute_alignment_graph_panel_unavailable");
				return false;
			}

			auto RefreshLayout = [&]()
			{
				GraphPanel->Update();
				PumpAndTickSlateForLayoutGeometry();
				const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
				GraphEditor->SlatePrepass(WidgetScale);
				GraphPanel->SlatePrepass(WidgetScale);
			};

			for (const FRigVMRerouteControlPointAlignment& Alignment : Alignments)
			{
				URigVMRerouteNode* RerouteNode = Alignment.RerouteNode.Get();
				if (!RerouteNode)
				{
					OutError = TEXT("rigvm_reroute_alignment_node_lost");
					return false;
				}
				if (!IsFiniteLayoutPoint(Alignment.DesiredControlPoint))
				{
					OutError = FString::Printf(TEXT("rigvm_reroute_alignment_invalid_target node=%s"), *RerouteNode->GetName());
					return false;
				}

				URigVMEdGraphNode* EdNode = Cast<URigVMEdGraphNode>(
					RigGraph->FindNodeForModelNodeName(RerouteNode->GetFName()));
				if (!EdNode)
				{
					OutError = FString::Printf(TEXT("rigvm_reroute_alignment_ednode_not_found node=%s"), *RerouteNode->GetName());
					return false;
				}
				const FVector2D CurrentPosition = RerouteNode->GetPosition();
				GraphEditor->SetViewLocation(
					SnapViewLocationToPixelGrid(
						FVector2f(
							FMath::RoundToFloat(static_cast<float>(CurrentPosition.X - 256.0)),
							FMath::RoundToFloat(static_cast<float>(CurrentPosition.Y - 192.0))),
						1.0f),
					1.0f);
				GraphPanel->RefreshNode(*EdNode);
				RefreshLayout();
				RefreshLayout();

				FVector2D MeasuredControlPoint = FVector2D::ZeroVector;
				if (!TryMeasureRigVMRerouteControlPoint(RigGraph, *GraphPanel, RerouteNode, MeasuredControlPoint))
				{
					OutError = FString::Printf(TEXT("rigvm_reroute_alignment_measure_failed node=%s"), *RerouteNode->GetName());
					return false;
				}
				const FVector2D Delta = Alignment.DesiredControlPoint - MeasuredControlPoint;
				if (FMath::Abs(Delta.X) > 0.01 || FMath::Abs(Delta.Y) > 0.01)
				{
					if (!Controller->SetNodePosition(RerouteNode, CurrentPosition + Delta, true, true, false))
					{
						OutError = FString::Printf(TEXT("rigvm_reroute_alignment_move_failed node=%s"), *RerouteNode->GetName());
						return false;
					}
				}
			}

			double MaxResidual = 0.0;
			FString MaxResidualNodeName;
			FVector2D MaxResidualDesired = FVector2D::ZeroVector;
			FVector2D MaxResidualMeasured = FVector2D::ZeroVector;
			for (const FRigVMRerouteControlPointAlignment& Alignment : Alignments)
			{
				URigVMRerouteNode* RerouteNode = Alignment.RerouteNode.Get();
				if (!RerouteNode)
				{
					OutError = TEXT("rigvm_reroute_alignment_verify_node_lost");
					return false;
				}
				URigVMEdGraphNode* EdNode = Cast<URigVMEdGraphNode>(
					RigGraph->FindNodeForModelNodeName(RerouteNode->GetFName()));
				if (!EdNode)
				{
					OutError = FString::Printf(TEXT("rigvm_reroute_alignment_verify_ednode_not_found node=%s"), *RerouteNode->GetName());
					return false;
				}
				const FVector2D CurrentPosition = RerouteNode->GetPosition();
				GraphEditor->SetViewLocation(
					SnapViewLocationToPixelGrid(
						FVector2f(
							FMath::RoundToFloat(static_cast<float>(CurrentPosition.X - 256.0)),
							FMath::RoundToFloat(static_cast<float>(CurrentPosition.Y - 192.0))),
						1.0f),
					1.0f);
				GraphPanel->RefreshNode(*EdNode);
				RefreshLayout();

				FVector2D MeasuredControlPoint = FVector2D::ZeroVector;
				if (!TryMeasureRigVMRerouteControlPoint(RigGraph, *GraphPanel, RerouteNode, MeasuredControlPoint))
				{
					OutError = FString::Printf(TEXT("rigvm_reroute_alignment_verify_measure_failed node=%s"), *RerouteNode->GetName());
					return false;
				}
				const double Residual = FVector2D::Distance(Alignment.DesiredControlPoint, MeasuredControlPoint);
				if (Residual > MaxResidual)
				{
					MaxResidual = Residual;
					MaxResidualNodeName = RerouteNode->GetName();
					MaxResidualDesired = Alignment.DesiredControlPoint;
					MaxResidualMeasured = MeasuredControlPoint;
				}
			}
			if (MaxResidual > 0.5)
			{
				OutError = FString::Printf(
					TEXT("rigvm_reroute_alignment_residual_too_large max_residual=%.3f node=%s desired=(%.3f,%.3f) measured=(%.3f,%.3f)"),
					MaxResidual,
					*MaxResidualNodeName,
					MaxResidualDesired.X,
					MaxResidualDesired.Y,
					MaxResidualMeasured.X,
					MaxResidualMeasured.Y);
				return false;
			}
			return true;
		}
		struct FRigVMSharedReroutePoint
		{
			TWeakObjectPtr<URigVMRerouteNode> RerouteNode;
			FString ValuePinPath;
		};
		FString MakeRigVMSharedRerouteKey(
			const UeAgentBlueprintAutoLayout::FReroutePlan& Plan,
			const FVector2D& Point)
		{
			return UeAgentBlueprintAutoLayout::MakeRerouteAttachmentPointMergeKey(
				Plan.AttachmentMergeGroup,
				Point);
		}
		struct FRigVMResolvedReroutePlan
		{
			const UeAgentBlueprintAutoLayout::FReroutePlan* Plan = nullptr;
			int32 EdgeIndex = INDEX_NONE;
			const UeAgentBlueprintAutoLayout::FEdge* Edge = nullptr;
			FString SourcePath;
			FString TargetPath;
			FVector2D SourcePoint = FVector2D::ZeroVector;
			FVector2D TargetPoint = FVector2D::ZeroVector;
			TArray<FVector2D> Points;
		};
		struct FRigVMBusTopologyBranch
		{
			int32 EdgeIndex = INDEX_NONE;
			FString TargetPath;
			FVector2D BranchPoint = FVector2D::ZeroVector;
			TArray<FVector2D> SuffixPoints;
		};
		struct FRigVMBusTopology
		{
			FString Key;
			FString SourcePath;
			double TrunkY = 0.0;
			TArray<FVector2D> SourcePrefixPoints;
			TArray<FVector2D> TrunkPoints;
			TArray<FRigVMBusTopologyBranch> Branches;
		};
		FString MakeRigVMVisiblePointKey(const FString& Scope, const FVector2D& Point)
		{
			return FString::Printf(TEXT("%s|%d|%d"), *Scope, FMath::RoundToInt(Point.X), FMath::RoundToInt(Point.Y));
		}
		bool RigVMPointsNearlyEqual(const FVector2D& A, const FVector2D& B)
		{
			return FVector2D::Distance(A, B) <= 0.5;
		}
		void AddUniqueRigVMVisiblePoint(TArray<FVector2D>& Points, const FVector2D& Point)
		{
			for (const FVector2D& Existing : Points)
			{
				if (RigVMPointsNearlyEqual(Existing, Point))
				{
					return;
				}
			}
			Points.Add(FVector2D(FMath::RoundToDouble(Point.X), FMath::RoundToDouble(Point.Y)));
		}
		TArray<FVector2D> BuildRigVMResolvedRoute(const FRigVMResolvedReroutePlan& Resolved)
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
		bool EstimateRigVMBusTrunkY(const TArray<FRigVMResolvedReroutePlan>& Plans, double& OutTrunkY)
		{
			TMap<int32, double> HorizontalLengthByY;
			TMap<int32, double> WeightedYByKey;
			for (const FRigVMResolvedReroutePlan& Resolved : Plans)
			{
				const TArray<FVector2D> Route = BuildRigVMResolvedRoute(Resolved);
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
		FString MakeRigVMBusTopologyKey(
			const FUniversalLayoutModel& Model,
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
		bool BuildRigVMBusTopology(
			const FString& TopologyKey,
			const TArray<FRigVMResolvedReroutePlan>& Plans,
			FRigVMBusTopology& OutTopology)
		{
			if (Plans.Num() == 0 || Plans[0].SourcePath.IsEmpty())
			{
				return false;
			}
			double TrunkY = 0.0;
			if (!EstimateRigVMBusTrunkY(Plans, TrunkY))
			{
				return false;
			}

			OutTopology = FRigVMBusTopology();
			OutTopology.Key = TopologyKey;
			OutTopology.SourcePath = Plans[0].SourcePath;
			OutTopology.TrunkY = TrunkY;

			double BestPrefixFirstTrunkX = TNumericLimits<double>::Max();
			TArray<FVector2D> BestSourcePrefix;
			bool bAnyPlanMaterialized = false;

			for (const FRigVMResolvedReroutePlan& Resolved : Plans)
			{
				if (!Resolved.Plan || Resolved.TargetPath.IsEmpty())
				{
					continue;
				}
				const TArray<FVector2D> Route = BuildRigVMResolvedRoute(Resolved);
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
				}

				for (int32 Index = FirstTrunkIndex; Index <= LastTrunkIndex; ++Index)
				{
					if (FMath::IsNearlyEqual(Route[Index].Y, TrunkY, 0.5))
					{
						AddUniqueRigVMVisiblePoint(OutTopology.TrunkPoints, FVector2D(Route[Index].X, TrunkY));
					}
				}

				FRigVMBusTopologyBranch Branch;
				Branch.EdgeIndex = Resolved.EdgeIndex;
				Branch.TargetPath = Resolved.TargetPath;
				Branch.BranchPoint = FVector2D(Route[LastTrunkIndex].X, TrunkY);
				for (int32 Index = LastTrunkIndex + 1; Index + 1 < Route.Num(); ++Index)
				{
					if (!RigVMPointsNearlyEqual(Route[Index], Branch.BranchPoint))
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
		bool EnsureRigVMLink(
			URigVMGraph* ModelGraph,
			URigVMController* Controller,
			const FString& SourcePath,
			const FString& TargetPath,
			const TCHAR* Context,
			FString& OutError)
		{
			if (!ModelGraph || !Controller || SourcePath.IsEmpty() || TargetPath.IsEmpty())
			{
				OutError = FString::Printf(TEXT("rigvm_%s_link_context_invalid"), Context ? Context : TEXT("ensure"));
				return false;
			}
			if (ModelGraph->FindLink(MakeRigVMLinkPath(SourcePath, TargetPath)))
			{
				return true;
			}
			if (!Controller->AddLink(SourcePath, TargetPath, true, false))
			{
				OutError = FString::Printf(
					TEXT("rigvm_%s_add_link_failed link=%s"),
					Context ? Context : TEXT("ensure"),
					*MakeRigVMLinkPath(SourcePath, TargetPath));
				return false;
			}
			return true;
		}
		bool BreakRigVMLinkIfPresent(
			URigVMGraph* ModelGraph,
			URigVMController* Controller,
			const FString& SourcePath,
			const FString& TargetPath,
			const TCHAR* Context,
			FString& OutError)
		{
			if (!ModelGraph || !Controller || SourcePath.IsEmpty() || TargetPath.IsEmpty())
			{
				OutError = FString::Printf(TEXT("rigvm_%s_link_context_invalid"), Context ? Context : TEXT("break"));
				return false;
			}
			if (!ModelGraph->FindLink(MakeRigVMLinkPath(SourcePath, TargetPath)))
			{
				return true;
			}
			if (!Controller->BreakLink(SourcePath, TargetPath, true, false))
			{
				OutError = FString::Printf(
					TEXT("rigvm_%s_break_link_failed link=%s"),
					Context ? Context : TEXT("break"),
					*MakeRigVMLinkPath(SourcePath, TargetPath));
				return false;
			}
			return true;
		}
		bool AddRigVMRerouteOnExistingLink(
			URigVMGraph* ModelGraph,
			URigVMController* Controller,
			const FString& SourcePath,
			const FString& TargetPath,
			const FVector2D& ControlPoint,
			const TCHAR* Context,
			TArray<FRigVMRerouteControlPointAlignment>& CreatedAlignments,
			FString& OutValuePinPath,
			FString& OutError)
		{
			OutValuePinPath.Reset();
			if (!EnsureRigVMLink(ModelGraph, Controller, SourcePath, TargetPath, Context, OutError))
			{
				return false;
			}
			URigVMLink* Link = ModelGraph->FindLink(MakeRigVMLinkPath(SourcePath, TargetPath));
			if (!Link)
			{
				OutError = FString::Printf(TEXT("rigvm_%s_reroute_link_not_found link=%s"), Context ? Context : TEXT("add_reroute"), *MakeRigVMLinkPath(SourcePath, TargetPath));
				return false;
			}
			const FVector2D RigVMNodePosition = ConvertLayoutKnotControlPointToRigVMNodePosition(ControlPoint);
			URigVMRerouteNode* RerouteNode = Controller->AddRerouteNodeOnLink(Link, RigVMNodePosition, FString(), true, false);
			if (!RerouteNode)
			{
				OutError = FString::Printf(TEXT("rigvm_%s_add_reroute_failed link=%s"), Context ? Context : TEXT("add_reroute"), *MakeRigVMLinkPath(SourcePath, TargetPath));
				return false;
			}
			URigVMPin* ValuePin = RerouteNode->FindPin(TEXT("Value"));
			if (!ValuePin)
			{
				OutError = FString::Printf(TEXT("rigvm_%s_reroute_value_pin_missing node=%s"), Context ? Context : TEXT("add_reroute"), *RerouteNode->GetName());
				return false;
			}
			FRigVMRerouteControlPointAlignment Alignment;
			Alignment.RerouteNode = RerouteNode;
			Alignment.DesiredControlPoint = ControlPoint;
			CreatedAlignments.Add(Alignment);
			OutValuePinPath = ValuePin->GetPinPath();
			return !OutValuePinPath.IsEmpty();
		}
		bool ApplyRigVMLegacyReroutePlan(
			URigVMGraph* ModelGraph,
			URigVMController* Controller,
			const UeAgentBlueprintAutoLayout::FReroutePlan& Plan,
			const FString& SourcePath,
			const FString& TargetPath,
			TArray<FRigVMRerouteControlPointAlignment>& CreatedAlignments,
			int32& CreatedCount,
			FString& OutError)
		{
			FString CurrentSourcePath = SourcePath;
			for (const FVector2D& Point : UeAgentBlueprintAutoLayout::GetAuthoritativeReroutePoints(Plan))
			{
				FString RerouteValuePinPath;
				if (!AddRigVMRerouteOnExistingLink(
					ModelGraph,
					Controller,
					CurrentSourcePath,
					TargetPath,
					Point,
					TEXT("legacy"),
					CreatedAlignments,
					RerouteValuePinPath,
					OutError))
				{
					return false;
				}
				CurrentSourcePath = RerouteValuePinPath;
				++CreatedCount;
			}
			return EnsureRigVMLink(ModelGraph, Controller, CurrentSourcePath, TargetPath, TEXT("legacy_tail"), OutError);
		}
		bool ApplyRigVMBusTopology(
			URigVMGraph* ModelGraph,
			URigVMController* Controller,
			const FRigVMBusTopology& Topology,
			TArray<FRigVMRerouteControlPointAlignment>& CreatedAlignments,
			int32& CreatedCount,
			FString& OutError)
		{
			if (!ModelGraph || !Controller || Topology.SourcePath.IsEmpty() || Topology.TrunkPoints.Num() == 0 || Topology.Branches.Num() == 0)
			{
				OutError = TEXT("rigvm_bus_topology_invalid");
				return false;
			}

			const FString TemporaryTargetPath = Topology.Branches[0].TargetPath;
			if (TemporaryTargetPath.IsEmpty())
			{
				OutError = TEXT("rigvm_bus_topology_temp_target_missing");
				return false;
			}

			TMap<FString, FString> ValuePinPathByPoint;
			FString CurrentSourcePath = Topology.SourcePath;

			for (const FVector2D& PrefixPoint : Topology.SourcePrefixPoints)
			{
				FString RerouteValuePinPath;
				if (!AddRigVMRerouteOnExistingLink(
					ModelGraph,
					Controller,
					CurrentSourcePath,
					TemporaryTargetPath,
					PrefixPoint,
					TEXT("bus_prefix"),
					CreatedAlignments,
					RerouteValuePinPath,
					OutError))
				{
					return false;
				}
				CurrentSourcePath = RerouteValuePinPath;
				++CreatedCount;
			}

			for (const FVector2D& TrunkPoint : Topology.TrunkPoints)
			{
				FString RerouteValuePinPath;
				if (!AddRigVMRerouteOnExistingLink(
					ModelGraph,
					Controller,
					CurrentSourcePath,
					TemporaryTargetPath,
					TrunkPoint,
					TEXT("bus_trunk"),
					CreatedAlignments,
					RerouteValuePinPath,
					OutError))
				{
					return false;
				}
				ValuePinPathByPoint.Add(MakeRigVMVisiblePointKey(Topology.Key, TrunkPoint), RerouteValuePinPath);
				CurrentSourcePath = RerouteValuePinPath;
				++CreatedCount;
			}

			if (!BreakRigVMLinkIfPresent(ModelGraph, Controller, CurrentSourcePath, TemporaryTargetPath, TEXT("bus_temp_tail"), OutError))
			{
				return false;
			}

			for (const FRigVMBusTopologyBranch& Branch : Topology.Branches)
			{
				if (Branch.TargetPath.IsEmpty())
				{
					continue;
				}
				FString BranchSourcePath = ValuePinPathByPoint.FindRef(MakeRigVMVisiblePointKey(Topology.Key, Branch.BranchPoint));
				if (BranchSourcePath.IsEmpty())
				{
					OutError = FString::Printf(TEXT("rigvm_bus_branch_point_missing point=(%.3f,%.3f)"), Branch.BranchPoint.X, Branch.BranchPoint.Y);
					return false;
				}
				for (const FVector2D& SuffixPoint : Branch.SuffixPoints)
				{
					FString RerouteValuePinPath;
					if (!AddRigVMRerouteOnExistingLink(
						ModelGraph,
						Controller,
						BranchSourcePath,
						Branch.TargetPath,
						SuffixPoint,
						TEXT("bus_branch"),
						CreatedAlignments,
						RerouteValuePinPath,
						OutError))
					{
						return false;
					}
					BranchSourcePath = RerouteValuePinPath;
					++CreatedCount;
				}
				if (!EnsureRigVMLink(ModelGraph, Controller, BranchSourcePath, Branch.TargetPath, TEXT("bus_branch_tail"), OutError))
				{
					return false;
				}
			}
			return true;
		}
		int32 ApplyRigVMReroutePlans(
			URigVMEdGraph* RigGraph,
			const FUniversalLayoutModel& Model,
			const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans,
			const bool bReplaceExistingReroutes,
			FString& OutError)
		{
			URigVMGraph* ModelGraph = RigGraph ? RigGraph->GetModel() : nullptr;
			URigVMController* Controller = RigGraph ? RigGraph->GetController() : nullptr;
			if (!ModelGraph || !Controller)
			{
				OutError = TEXT("rigvm_graph_or_controller_unavailable");
				return 0;
			}

			TArray<TPair<FString, FString>> EdgePinPaths;
			EdgePinPaths.SetNum(Model.Edges.Num());
			for (int32 EdgeIndex = 0; EdgeIndex < Model.Edges.Num(); ++EdgeIndex)
			{
				FString SourcePath;
				FString TargetPath;
				if (!TryGetRigVMPinPathsForEdge(Model, Model.Edges[EdgeIndex], SourcePath, TargetPath))
				{
					OutError = FString::Printf(TEXT("rigvm_edge_pin_path_unavailable edge=%d"), EdgeIndex);
					return 0;
				}
				EdgePinPaths[EdgeIndex] = TPair<FString, FString>(SourcePath, TargetPath);
			}

			if (bReplaceExistingReroutes && !RemoveRigVMRerouteNodesAndRestoreSemanticLinks(RigGraph, EdgePinPaths, OutError))
			{
				return 0;
			}

			int32 CreatedCount = 0;
			TArray<FRigVMRerouteControlPointAlignment> CreatedAlignments;
			TMap<int32, const UeAgentBlueprintAutoLayout::FReroutePlan*> PlanByEdge;
			TMap<FString, TArray<FRigVMResolvedReroutePlan>> BusPlansByTopologyKey;
			TSet<int32> TopologyHandledEdges;

			for (const UeAgentBlueprintAutoLayout::FReroutePlan& Plan : Plans)
			{
				if (Plan.EdgeIndex == INDEX_NONE
					|| !EdgePinPaths.IsValidIndex(Plan.EdgeIndex)
					|| UeAgentBlueprintAutoLayout::GetAuthoritativeReroutePoints(Plan).Num() == 0)
				{
					continue;
				}
				PlanByEdge.Add(Plan.EdgeIndex, &Plan);
			}

			for (const TPair<int32, const UeAgentBlueprintAutoLayout::FReroutePlan*>& Pair : PlanByEdge)
			{
				if (!EdgePinPaths.IsValidIndex(Pair.Key) || !Pair.Value)
				{
					continue;
				}
				const FString& SourcePath = EdgePinPaths[Pair.Key].Key;
				const FString& TargetPath = EdgePinPaths[Pair.Key].Value;
				if (!EnsureRigVMLink(ModelGraph, Controller, SourcePath, TargetPath, TEXT("restore_planned_direct"), OutError)
					|| !BreakRigVMLinkIfPresent(ModelGraph, Controller, SourcePath, TargetPath, TEXT("planned_direct"), OutError))
				{
					return CreatedCount;
				}

				const UeAgentBlueprintAutoLayout::FReroutePlan& Plan = *Pair.Value;
				if (Plan.AttachmentMergeGroup != INDEX_NONE
					&& Plan.RouteKind != UeAgentBlueprintAutoLayout::ERouteKind::Direct
					&& Model.Edges.IsValidIndex(Plan.EdgeIndex))
				{
					const UeAgentBlueprintAutoLayout::FEdge& Edge = Model.Edges[Plan.EdgeIndex];
					FRigVMResolvedReroutePlan Resolved;
					Resolved.Plan = &Plan;
					Resolved.EdgeIndex = Plan.EdgeIndex;
					Resolved.Edge = &Edge;
					Resolved.SourcePath = SourcePath;
					Resolved.TargetPath = TargetPath;
					Resolved.SourcePoint = UeAgentBlueprintAutoLayout::GetSourcePort(Model.Nodes[Edge.Source], Edge, true);
					Resolved.TargetPoint = UeAgentBlueprintAutoLayout::GetTargetPort(Model.Nodes[Edge.Target], Edge, true);
					Resolved.Points = UeAgentBlueprintAutoLayout::GetAuthoritativeReroutePoints(Plan);
					const FString TopologyKey = MakeRigVMBusTopologyKey(Model, Plan);
					if (!TopologyKey.IsEmpty())
					{
						BusPlansByTopologyKey.FindOrAdd(TopologyKey).Add(Resolved);
					}
				}
			}

			for (const TPair<FString, TArray<FRigVMResolvedReroutePlan>>& Pair : BusPlansByTopologyKey)
			{
				FRigVMBusTopology Topology;
				if (!BuildRigVMBusTopology(Pair.Key, Pair.Value, Topology))
				{
					continue;
				}
				const int32 CreatedBefore = CreatedCount;
				if (ApplyRigVMBusTopology(ModelGraph, Controller, Topology, CreatedAlignments, CreatedCount, OutError))
				{
					for (const FRigVMResolvedReroutePlan& Resolved : Pair.Value)
					{
						TopologyHandledEdges.Add(Resolved.EdgeIndex);
					}
				}
				else
				{
					return FMath::Max(CreatedBefore, CreatedCount);
				}
			}

			for (const TPair<int32, const UeAgentBlueprintAutoLayout::FReroutePlan*>& Pair : PlanByEdge)
			{
				if (TopologyHandledEdges.Contains(Pair.Key) || !Pair.Value || !EdgePinPaths.IsValidIndex(Pair.Key))
				{
					continue;
				}
				if (!ApplyRigVMLegacyReroutePlan(
					ModelGraph,
					Controller,
					*Pair.Value,
					EdgePinPaths[Pair.Key].Key,
					EdgePinPaths[Pair.Key].Value,
					CreatedAlignments,
					CreatedCount,
					OutError))
				{
					return CreatedCount;
				}
			}

			if (!AlignRigVMRerouteControlPointsToSlateGeometry(RigGraph, Controller, CreatedAlignments, OutError))
			{
				return CreatedCount;
			}
			return CreatedCount;
		}
		int32 CountSharedPlannedRerouteKnotNodes(const TArray<UeAgentBlueprintAutoLayout::FReroutePlan>& Plans)
		{
			int32 Count = 0;
			TSet<FString> SharedKeys;
			for (const UeAgentBlueprintAutoLayout::FReroutePlan& Plan : Plans)
			{
				const TArray<FVector2D>& WritePoints = UeAgentBlueprintAutoLayout::GetAuthoritativeReroutePoints(Plan);
				for (int32 PointIndex = 0; PointIndex < WritePoints.Num(); ++PointIndex)
				{
					const FString SharedKey = MakeRigVMSharedRerouteKey(Plan, WritePoints[PointIndex]);
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
	}
}
