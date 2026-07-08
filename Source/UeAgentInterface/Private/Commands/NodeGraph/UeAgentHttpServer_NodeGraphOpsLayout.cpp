// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_NodeGraphOps.h"
#include "UeAgentHttpServer_NodeGraphOpsInternal.h"
#include "Commands/Blueprint/Layout/Core/UeAgentBlueprintAutoLayoutAlgorithm.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/RigVMEdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "RigVMCore/RigVMExecuteContext.h"
#include "RigVMModel/RigVMPin.h"

namespace UeAgentNodeGraphOps
{
	namespace Internal
	{
		UEdGraph* ResolveGenericGraph(UObject* Asset, const FString& GraphRef)
		{
			const TArray<FGraphRef> Graphs = CollectGenericEdGraphs(Asset);
			for (const FGraphRef& Ref : Graphs)
			{
				if (!Ref.Graph)
				{
					continue;
				}
				if (Ref.Graph->GetName().Equals(GraphRef, ESearchCase::IgnoreCase)
					|| Ref.Graph->GetPathName().Equals(GraphRef, ESearchCase::IgnoreCase))
				{
					return Ref.Graph;
				}
			}
			if (Graphs.Num() == 1 && GraphRef.IsEmpty())
			{
				return Graphs[0].Graph;
			}
			return nullptr;
		}
		bool IsExecLikeGraphPin(const UEdGraphPin* Pin)
		{
			if (!Pin)
			{
				return false;
			}
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->PinType.PinCategory == FName(TEXT("exec")))
			{
				return true;
			}
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				if (const UScriptStruct* ScriptStruct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()))
				{
					if (ScriptStruct->IsChildOf(FRigVMExecutePin::StaticStruct()))
					{
						return true;
					}
				}
			}
			if (const URigVMEdGraphNode* RigNode = Cast<URigVMEdGraphNode>(Pin->GetOwningNode()))
			{
				if (const URigVMPin* ModelPin = RigNode->FindModelPinFromGraphPin(Pin))
				{
					return ModelPin->IsExecuteContext();
				}
			}
			return false;
		}
		bool GetSharedReroutePassThroughPins(UEdGraphNode* Node, UEdGraphPin*& OutInputPin, UEdGraphPin*& OutOutputPin)
		{
			OutInputPin = nullptr;
			OutOutputPin = nullptr;
			if (!Node)
			{
				return false;
			}
			int32 InputPinIndex = INDEX_NONE;
			int32 OutputPinIndex = INDEX_NONE;
			if (Node->ShouldDrawNodeAsControlPointOnly(InputPinIndex, OutputPinIndex))
			{
				if (!Node->Pins.IsValidIndex(InputPinIndex) || !Node->Pins.IsValidIndex(OutputPinIndex))
				{
					return false;
				}
				OutInputPin = Node->Pins[InputPinIndex];
				OutOutputPin = Node->Pins[OutputPinIndex];
				return OutInputPin && OutOutputPin;
			}

			const URigVMEdGraphNode* RigNode = Cast<URigVMEdGraphNode>(Node);
			const FString NodeName = Node->GetName();
			const FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			const FString ModelPath = RigNode ? RigNode->GetModelNodePath() : FString();
			const bool bLooksLikeRigVMReroute =
				NodeName.StartsWith(TEXT("RerouteNode_"))
				|| Title.StartsWith(TEXT("RerouteNode_"))
				|| ModelPath.StartsWith(TEXT("RerouteNode_"));
			if (!bLooksLikeRigVMReroute)
			{
				return false;
			}

			TArray<UEdGraphPin*> CandidateInputs;
			TArray<UEdGraphPin*> CandidateOutputs;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->ParentPin != nullptr)
				{
					continue;
				}
				if (Pin->Direction == EGPD_Input)
				{
					CandidateInputs.Add(Pin);
				}
				else if (Pin->Direction == EGPD_Output)
				{
					CandidateOutputs.Add(Pin);
				}
			}
			if (CandidateInputs.Num() != 1 || CandidateOutputs.Num() != 1)
			{
				return false;
			}
			OutInputPin = CandidateInputs[0];
			OutOutputPin = CandidateOutputs[0];
			const bool bValuePassThrough =
				OutInputPin->PinName == OutOutputPin->PinName
				|| OutInputPin->PinName == TEXT("Value")
				|| OutOutputPin->PinName == TEXT("Value");
			if (!bValuePassThrough || (OutInputPin->LinkedTo.Num() == 0 && OutOutputPin->LinkedTo.Num() == 0))
			{
				OutInputPin = nullptr;
				OutOutputPin = nullptr;
				return false;
			}
			return OutInputPin && OutOutputPin;
		}
		bool IsSharedLayoutRerouteNode(UEdGraphNode* Node)
		{
			UEdGraphPin* InputPin = nullptr;
			UEdGraphPin* OutputPin = nullptr;
			return GetSharedReroutePassThroughPins(Node, InputPin, OutputPin);
		}
		void CollectTerminalSharedLinkedPins(UEdGraphPin* LinkedPin, TArray<UEdGraphPin*>& OutPins, TSet<UEdGraphPin*>& VisitedPins)
		{
			if (!LinkedPin || VisitedPins.Contains(LinkedPin))
			{
				return;
			}
			VisitedPins.Add(LinkedPin);

			UEdGraphNode* Owner = LinkedPin->GetOwningNode();
			UEdGraphPin* RerouteInput = nullptr;
			UEdGraphPin* RerouteOutput = nullptr;
			if (!GetSharedReroutePassThroughPins(Owner, RerouteInput, RerouteOutput))
			{
				OutPins.Add(LinkedPin);
				return;
			}

			UEdGraphPin* NextPin = nullptr;
			if (LinkedPin == RerouteInput)
			{
				NextPin = RerouteOutput;
			}
			else if (LinkedPin == RerouteOutput)
			{
				NextPin = RerouteInput;
			}
			if (!NextPin)
			{
				OutPins.Add(LinkedPin);
				return;
			}
			for (UEdGraphPin* NextLinked : NextPin->LinkedTo)
			{
				CollectTerminalSharedLinkedPins(NextLinked, OutPins, VisitedPins);
			}
		}
		bool IsVisibleSharedGraphPin(const UEdGraphPin* Pin)
		{
			if (!Pin)
			{
				return false;
			}
			return !Pin->bHidden || Pin->LinkedTo.Num() > 0 || IsExecLikeGraphPin(Pin);
		}
		FString GraphPinNameString(const UEdGraphPin* Pin)
		{
			return PinNameOrFallback(Pin ? Pin->GetName() : FString(), TEXT("Pin_"), 0);
		}
		TArray<UEdGraphPin*> GetSharedGraphPinsByDirection(UEdGraphNode* Node, EEdGraphPinDirection Direction)
		{
			TArray<UEdGraphPin*> OutPins;
			if (!Node)
			{
				return OutPins;
			}
			TSet<const UEdGraphPin*> SeenPins;
			TFunction<void(UEdGraphPin*)> VisitPin = [&](UEdGraphPin* Pin)
			{
				if (!Pin || SeenPins.Contains(Pin))
				{
					return;
				}
				SeenPins.Add(Pin);
				if (IsVisibleSharedGraphPin(Pin) && Pin->Direction == Direction)
				{
					OutPins.Add(Pin);
				}
				for (UEdGraphPin* SubPin : Pin->SubPins)
				{
					VisitPin(SubPin);
				}
			};
			for (UEdGraphPin* Pin : Node->Pins)
			{
				VisitPin(Pin);
			}
			return OutPins;
		}
		int32 GetSharedGraphPinIndexInDirection(const UEdGraphPin* Pin)
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
			const TArray<UEdGraphPin*> DirectionPins = GetSharedGraphPinsByDirection(const_cast<UEdGraphNode*>(Node), Pin->Direction);
			for (int32 Index = 0; Index < DirectionPins.Num(); ++Index)
			{
				if (DirectionPins[Index] == Pin)
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}
		void CollectSharedGraphNodePins(UEdGraphNode* GraphNode, UeAgentBlueprintAutoLayout::FNode& OutNode)
		{
			TArray<FString> InputPins;
			TArray<FString> OutputPins;
			const TArray<UEdGraphPin*> Inputs = GetSharedGraphPinsByDirection(GraphNode, EGPD_Input);
			const TArray<UEdGraphPin*> Outputs = GetSharedGraphPinsByDirection(GraphNode, EGPD_Output);
			for (int32 Index = 0; Index < Inputs.Num(); ++Index)
			{
				InputPins.Add(PinNameOrFallback(GraphPinNameString(Inputs[Index]), TEXT("Input_"), Index));
				if (IsExecLikeGraphPin(Inputs[Index]))
				{
					OutNode.bHasExecPin = true;
				}
			}
			for (int32 Index = 0; Index < Outputs.Num(); ++Index)
			{
				OutputPins.Add(PinNameOrFallback(GraphPinNameString(Outputs[Index]), TEXT("Output_"), Index));
				if (IsExecLikeGraphPin(Outputs[Index]))
				{
					OutNode.bHasExecPin = true;
				}
			}
			InitializePendingRealPins(OutNode, InputPins, OutputPins);
		}
		FUniversalLayoutModel BuildGenericSharedLayoutModel(UEdGraph* Graph)
		{
			FUniversalLayoutModel Model;
			if (!Graph)
			{
				return Model;
			}

			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				if (!GraphNode)
				{
					continue;
				}
				if (IsSharedLayoutRerouteNode(GraphNode))
				{
					++Model.CollapsedRerouteNodeCount;
					continue;
				}

				UeAgentBlueprintAutoLayout::FNode Node;
				Node.Index = Model.Nodes.Num();
				Node.Guid = GraphNode->NodeGuid.IsValid()
					? GraphNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)
					: GraphNode->GetPathName();
				Node.Title = GraphNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
				if (Node.Title.IsEmpty())
				{
					Node.Title = GraphNode->GetName();
				}
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
				CollectSharedGraphNodePins(GraphNode, Node);

				Model.GraphNodeToIndex.Add(GraphNode, Node.Index);
				FUniversalLayoutNodeRef Ref;
				Ref.GraphNode = GraphNode;
				Model.NodeRefs.Add(Ref);
				Model.Nodes.Add(Node);
			}
			Model.WritableNodeCount = Model.Nodes.Num();

			TSet<FString> EdgeKeys;
			for (UEdGraphNode* SourceNode : Graph->Nodes)
			{
				const int32* SourceIndexPtr = Model.GraphNodeToIndex.Find(SourceNode);
				if (!SourceIndexPtr)
				{
					continue;
				}
				const TArray<UEdGraphPin*> SourcePins = GetSharedGraphPinsByDirection(SourceNode, EGPD_Output);
				for (UEdGraphPin* SourcePin : SourcePins)
				{
					for (UEdGraphPin* LinkedPin : SourcePin->LinkedTo)
					{
						TArray<UEdGraphPin*> TerminalPins;
						TSet<UEdGraphPin*> VisitedPins;
						CollectTerminalSharedLinkedPins(LinkedPin, TerminalPins, VisitedPins);
						for (UEdGraphPin* TerminalPin : TerminalPins)
						{
							if (!TerminalPin || TerminalPin->Direction != EGPD_Input)
							{
								continue;
							}
							UEdGraphNode* TargetNode = TerminalPin->GetOwningNode();
							const int32* TargetIndexPtr = Model.GraphNodeToIndex.Find(TargetNode);
							if (!TargetIndexPtr)
							{
								continue;
							}

							UeAgentBlueprintAutoLayout::FEdge Edge;
							Edge.Source = *SourceIndexPtr;
							Edge.Target = *TargetIndexPtr;
							Edge.SourcePin = GraphPinNameString(SourcePin);
							Edge.TargetPin = GraphPinNameString(TerminalPin);
							Edge.SourcePinIndex = GetSharedGraphPinIndexInDirection(SourcePin);
							Edge.TargetPinIndex = GetSharedGraphPinIndexInDirection(TerminalPin);
							if (Edge.SourcePinIndex == INDEX_NONE || Edge.TargetPinIndex == INDEX_NONE)
							{
								continue;
							}
							Edge.bExec = IsExecLikeGraphPin(SourcePin) || IsExecLikeGraphPin(TerminalPin);
							const FString EdgeKey = FString::Printf(
								TEXT("%d:%d:%s:%s:%d:%d"),
								Edge.Source,
								Edge.Target,
								*Edge.SourcePin,
								*Edge.TargetPin,
								Edge.SourcePinIndex,
								Edge.TargetPinIndex);
							if (!EdgeKeys.Contains(EdgeKey))
							{
								EdgeKeys.Add(EdgeKey);
								if (TerminalPin != LinkedPin)
								{
									++Model.CollapsedRerouteEdgeCount;
								}
								Model.Edges.Add(Edge);
							}
						}
					}
				}
			}

			return Model;
		}
		bool RunSharedBlueprintLayoutCore(
			FUniversalLayoutModel& Model,
			const TSharedPtr<FJsonObject>& Params,
			UeAgentBlueprintAutoLayout::FOptions& OutOptions,
			UeAgentBlueprintAutoLayout::FRunResult& OutRunResult,
			FString& OutError)
		{
			if (!ParseSharedLayoutOptions(Params, OutOptions, OutError))
			{
				return false;
			}
			ApplyDefaultOriginFromWritableNodes(Model, Params, OutOptions);
			if (!UeAgentBlueprintAutoLayout::RunLayout(Model.Nodes, Model.Edges, OutOptions, OutRunResult))
			{
				OutError = TEXT("shared_blueprint_layout_core_failed");
				return false;
			}
			return true;
		}
	}
}
