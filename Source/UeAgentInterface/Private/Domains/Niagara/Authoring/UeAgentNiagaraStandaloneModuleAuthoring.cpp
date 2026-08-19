// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/Authoring/UeAgentNiagaraStandaloneModuleAuthoring.h"

#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraConstants.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptVariable.h"
#include "UObject/UObjectGlobals.h"

class UNiagaraNodeParameterMapSet;
namespace UE::Niagara::Wizard::Utilities
{
	NIAGARAEDITOR_API UEdGraphPin* AddWriteParameterPin(
		const FNiagaraTypeDefinition& Type,
		const FName& Name,
		UNiagaraNodeParameterMapSet* MapSetNode);
}

namespace UeAgentNiagaraStandaloneModuleAuthoring
{
namespace
{
	const FName ParameterPinSubCategory(TEXT("ParameterPin"));
	const FName DynamicAddPinSubCategory(TEXT("DynamicAddPin"));
	const FString StandaloneAssignmentMarker(TEXT("UAI:StandaloneAssignment"));
	UEdGraphPin* FindParameterMapPin(UEdGraphNode& Node, EEdGraphPinDirection Direction);

	bool IsParameterMapPin(const UEdGraphPin* Pin)
	{
		return Pin
			&& UEdGraphSchema_Niagara::PinToTypeDefinition(Pin)
				== FNiagaraTypeDefinition::GetParameterMapDef();
	}

	bool IsParameterMapSetNode(const UEdGraphNode* Node)
	{
		for (const UClass* Class = Node ? Node->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
		{
			if (Class->GetName() == TEXT("NiagaraNodeParameterMapSet")) return true;
		}
		return false;
	}

	bool IsDynamicInputOverrideNode(UEdGraphNode* Node)
	{
		if (!IsParameterMapSetNode(Node)) return false;
		UEdGraphPin* OutputPin = FindParameterMapPin(*Node, EGPD_Output);
		if (!OutputPin) return false;
		return OutputPin->LinkedTo.ContainsByPredicate([](const UEdGraphPin* LinkedPin)
		{
			const UNiagaraNodeFunctionCall* Function = LinkedPin
				? Cast<UNiagaraNodeFunctionCall>(LinkedPin->GetOwningNode())
				: nullptr;
			return Function && Function->GetCalledUsage() == ENiagaraScriptUsage::DynamicInput;
		});
	}

	UEdGraphPin* FindParameterMapPin(UEdGraphNode& Node, const EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == Direction && IsParameterMapPin(Pin)) return Pin;
		}
		return nullptr;
	}

	bool TryConnect(UNiagaraGraph& Graph, UEdGraphPin& OutputPin, UEdGraphPin& InputPin)
	{
		if (const UEdGraphSchema* Schema = Graph.GetSchema())
		{
			Schema->TryCreateConnection(&OutputPin, &InputPin);
		}
		if (!OutputPin.LinkedTo.Contains(&InputPin)) OutputPin.MakeLinkTo(&InputPin);
		return OutputPin.LinkedTo.Contains(&InputPin) && InputPin.LinkedTo.Contains(&OutputPin);
	}

	bool GetMainChain(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNode*>& OutNodes, FString* OutError = nullptr)
	{
		FString ChainError;
		const bool bSuccess = UeAgentNiagaraStageModuleAuthoring::GetParameterMapChainNodes(
			OutputNode, OutNodes, ChainError);
		if (!bSuccess && OutError) *OutError = ChainError;
		return bSuccess;
	}

	bool AssignmentVariablesMatch(
		const TArray<FNiagaraVariable>& ExistingTargets,
		const TArray<FNiagaraVariable>& RequestedTargets)
	{
		if (ExistingTargets.Num() != RequestedTargets.Num()) return false;
		for (const FNiagaraVariable& Requested : RequestedTargets)
		{
			const FString RequestedName =
				UeAgentNiagaraStageModuleAuthoring::NormalizeAssignmentTargetName(Requested.GetName().ToString());
			const bool bFound = ExistingTargets.ContainsByPredicate(
				[&Requested, &RequestedName](const FNiagaraVariable& Existing)
				{
					return Existing.GetType() == Requested.GetType()
						&& UeAgentNiagaraStageModuleAuthoring::NormalizeAssignmentTargetName(
							Existing.GetName().ToString()).Equals(RequestedName, ESearchCase::IgnoreCase);
				});
			if (!bFound) return false;
		}
		return true;
	}

	bool InsertNodeAtIndex(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraNode& Node,
		const int32 TargetIndex,
		FString& OutError)
	{
		UNiagaraGraph* Graph = OutputNode.GetNiagaraGraph();
		UEdGraphPin* NodeInput = FindParameterMapPin(Node, EGPD_Input);
		UEdGraphPin* NodeOutput = FindParameterMapPin(Node, EGPD_Output);
		if (!Graph || !NodeInput || !NodeOutput)
		{
			OutError = TEXT("niagara_standalone_assignment_node_invalid");
			return false;
		}

		TArray<UNiagaraNode*> Chain;
		if (!GetMainChain(OutputNode, Chain, &OutError)) return false;
		Chain.Remove(&Node);
		TArray<UNiagaraNode*> AssignmentNodes;
		GetAssignmentNodes(OutputNode, AssignmentNodes);
		AssignmentNodes.Remove(&Node);
		const int32 InsertIndex = FMath::Clamp(TargetIndex, 0, AssignmentNodes.Num());
		UEdGraphNode* NextNode = &OutputNode;
		if (InsertIndex < AssignmentNodes.Num())
		{
			const int32 NextAssignmentChainIndex = Chain.IndexOfByKey(AssignmentNodes[InsertIndex]);
			if (NextAssignmentChainIndex == INDEX_NONE)
			{
				OutError = TEXT("niagara_standalone_assignment_insert_target_missing");
				return false;
			}
			int32 NextGroupStartIndex = NextAssignmentChainIndex;
			while (NextGroupStartIndex > 0
				&& !AssignmentNodes.Contains(Chain[NextGroupStartIndex - 1]))
			{
				--NextGroupStartIndex;
			}
			NextNode = Chain[NextGroupStartIndex];
		}
		UEdGraphPin* NextInput = FindParameterMapPin(*NextNode, EGPD_Input);
		if (!NextInput || NextInput->LinkedTo.Num() != 1)
		{
			OutError = TEXT("niagara_standalone_assignment_insert_point_invalid");
			return false;
		}
		UEdGraphPin* PreviousOutput = NextInput->LinkedTo[0];
		if (!PreviousOutput || !IsParameterMapPin(PreviousOutput))
		{
			OutError = TEXT("niagara_standalone_assignment_previous_output_invalid");
			return false;
		}

		NodeInput->BreakAllPinLinks(true);
		NodeOutput->BreakAllPinLinks(true);
		NextInput->BreakAllPinLinks(true);
		if (!TryConnect(*Graph, *PreviousOutput, *NodeInput)
			|| !TryConnect(*Graph, *NodeOutput, *NextInput))
		{
			OutError = TEXT("niagara_standalone_assignment_link_failed");
			return false;
		}
		Node.NodePosX = (PreviousOutput->GetOwningNode()->NodePosX + NextNode->NodePosX) / 2;
		Node.NodePosY = NextNode->NodePosY;
		Graph->NotifyGraphChanged();
		return true;
	}

	bool AddTargetPin(UNiagaraNode& Node, const FNiagaraVariable& Target, FString& OutError)
	{
		UNiagaraGraph* Graph = Node.GetNiagaraGraph();
		const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
		if (!Graph || !Schema || !Target.IsValid())
		{
			OutError = TEXT("niagara_standalone_assignment_target_invalid");
			return false;
		}

		UNiagaraScriptVariable* ExistingScriptVariable = Graph->GetScriptVariable(Target.GetName());
		if (ExistingScriptVariable && ExistingScriptVariable->Variable.GetType() != Target.GetType())
		{
			OutError = TEXT("niagara_standalone_assignment_target_type_conflict:")
				+ Target.GetName().ToString();
			return false;
		}
		if (!ExistingScriptVariable)
		{
			// UE's native path registers the variable in the graph before converting the add pin.
			// Without this registration, a structurally valid Module Script can compile while its
			// Parameter Map Set writes are culled from the callable module signature.
			UEdGraphPin* Pin = UE::Niagara::Wizard::Utilities::AddWriteParameterPin(
				Target.GetType(),
				Target.GetName(),
				reinterpret_cast<UNiagaraNodeParameterMapSet*>(&Node));
			if (!Pin || Pin->PinName != Target.GetName())
			{
				OutError = TEXT("niagara_standalone_assignment_target_pin_create_failed:")
					+ Target.GetName().ToString();
				return false;
			}
			const FString DefaultValue = FNiagaraConstants::GetAttributeDefaultValue(Target);
			Pin->AutogeneratedDefaultValue = DefaultValue;
			Pin->DefaultValue = DefaultValue;
			return true;
		}

		// AddWriteParameterPin makes new names unique. When a graph intentionally writes the
		// same variable more than once, preserve the existing registered identity instead.
		int32 InsertIndex = INDEX_NONE;
		for (int32 PinIndex = 0; PinIndex < Node.Pins.Num(); ++PinIndex)
		{
			const UEdGraphPin* Pin = Node.Pins[PinIndex];
			if (Pin && Pin->PinType.PinSubCategory == DynamicAddPinSubCategory)
			{
				InsertIndex = PinIndex;
				break;
			}
		}
		UEdGraphPin* Pin = Node.CreatePin(
			EGPD_Input,
			Schema->TypeDefinitionToPinType(Target.GetType()),
			Target.GetName(),
			InsertIndex);
		if (!Pin)
		{
			OutError = TEXT("niagara_standalone_assignment_target_pin_create_failed:") + Target.GetName().ToString();
			return false;
		}
		Pin->PinType.PinSubCategory = ParameterPinSubCategory;
		Pin->PinFriendlyName = FText::AsCultureInvariant(Target.GetName().ToString());
		Pin->PersistentGuid = FGuid::NewGuid();
		const FString DefaultValue = FNiagaraConstants::GetAttributeDefaultValue(Target);
		Pin->AutogeneratedDefaultValue = DefaultValue;
		Pin->DefaultValue = DefaultValue;
		return true;
	}

	bool EnsureTargetRegistered(UNiagaraNode& Node, const FNiagaraVariable& Target, FString& OutError)
	{
		UNiagaraGraph* Graph = Node.GetNiagaraGraph();
		if (!Graph || !Target.IsValid())
		{
			OutError = TEXT("niagara_standalone_assignment_target_invalid");
			return false;
		}
		if (UNiagaraScriptVariable* Existing = Graph->GetScriptVariable(Target.GetName()))
		{
			if (Existing->Variable.GetType() == Target.GetType()) return true;
			OutError = TEXT("niagara_standalone_assignment_target_type_conflict:")
				+ Target.GetName().ToString();
			return false;
		}

		struct FDetachedTargetPin
		{
			int32 Index = INDEX_NONE;
			UEdGraphPin* Pin = nullptr;
		};
		TArray<FDetachedTargetPin> DetachedPins;
		for (int32 PinIndex = Node.Pins.Num() - 1; PinIndex >= 0; --PinIndex)
		{
			UEdGraphPin* Pin = Node.Pins[PinIndex];
			if (!Pin || Pin->Direction != EGPD_Input || IsParameterMapPin(Pin)
				|| Pin->PinType.PinSubCategory == DynamicAddPinSubCategory) continue;
			const FNiagaraVariable PinVariable = UEdGraphSchema_Niagara::PinToNiagaraVariable(Pin);
			if (PinVariable.GetName() != Target.GetName()) continue;
			DetachedPins.Add({ PinIndex, Pin });
			Node.Pins.RemoveAt(PinIndex);
		}
		auto RestoreDetachedPins = [&]()
		{
			DetachedPins.Sort([](const FDetachedTargetPin& Left, const FDetachedTargetPin& Right)
			{
				return Left.Index < Right.Index;
			});
			for (const FDetachedTargetPin& Detached : DetachedPins)
			{
				Node.Pins.Insert(Detached.Pin, FMath::Clamp(Detached.Index, 0, Node.Pins.Num()));
			}
			Graph->NotifyGraphChanged();
		};
		Graph->NotifyGraphChanged();
		UEdGraphPin* RegistrationPin = UE::Niagara::Wizard::Utilities::AddWriteParameterPin(
			Target.GetType(),
			Target.GetName(),
			reinterpret_cast<UNiagaraNodeParameterMapSet*>(&Node));
		if (!RegistrationPin || RegistrationPin->PinName != Target.GetName())
		{
			if (RegistrationPin) Node.RemovePin(RegistrationPin);
			RestoreDetachedPins();
			OutError = TEXT("niagara_standalone_assignment_target_registration_failed:")
				+ Target.GetName().ToString();
			return false;
		}
		Node.RemovePin(RegistrationPin);
		RestoreDetachedPins();
		UNiagaraScriptVariable* Registered = Graph->GetScriptVariable(Target.GetName());
		if (!Registered || Registered->Variable.GetType() != Target.GetType())
		{
			OutError = TEXT("niagara_standalone_assignment_target_registration_failed:")
				+ Target.GetName().ToString();
			return false;
		}
		return true;
	}
}

	bool IsStandaloneModuleStage(const UeAgentNiagaraStageModuleAuthoring::FResolvedStage& Stage)
	{
		const UNiagaraScript* Script = Cast<UNiagaraScript>(Stage.Asset);
		return Script
			&& Script->GetUsage() == ENiagaraScriptUsage::Module
			&& Stage.Usage == ENiagaraScriptUsage::Module;
	}

	void GetAssignmentNodes(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNode*>& OutNodes)
	{
		TArray<UNiagaraNode*> Chain;
		FString IgnoredError;
		if (!GetMainChain(OutputNode, Chain, &IgnoredError))
		{
			OutNodes.Reset();
			return;
		}
		TSet<const UNiagaraNode*> LegacyHelperNodes;
		bool bHasDownstreamLegacyFunction = false;
		for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
		{
			UNiagaraNode* ChainNode = Chain[Index];
			if (Cast<UNiagaraNodeFunctionCall>(ChainNode))
			{
				bHasDownstreamLegacyFunction = true;
				continue;
			}
			if ((bHasDownstreamLegacyFunction || IsDynamicInputOverrideNode(ChainNode))
				&& ChainNode
				&& ChainNode->NodeComment != StandaloneAssignmentMarker)
			{
				LegacyHelperNodes.Add(ChainNode);
			}
		}
		OutNodes = Chain.FilterByPredicate([&LegacyHelperNodes](const UNiagaraNode* Node)
		{
			if (!IsParameterMapSetNode(Node)) return false;
			TArray<FNiagaraVariable> Targets;
			GetAssignmentTargets(*Node, Targets);
			return !Targets.IsEmpty()
				&& (Node->NodeComment == StandaloneAssignmentMarker || !LegacyHelperNodes.Contains(Node));
		});
	}

	bool ResolveAssignmentNode(
		UNiagaraNodeOutput& OutputNode,
		const FGuid& NodeGuid,
		UNiagaraNode*& OutNode,
		int32& OutIndex,
		FString& OutError)
	{
		OutNode = nullptr;
		OutIndex = INDEX_NONE;
		TArray<UNiagaraNode*> Nodes;
		GetAssignmentNodes(OutputNode, Nodes);
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index] && Nodes[Index]->NodeGuid == NodeGuid)
			{
				OutNode = Nodes[Index];
				OutIndex = Index;
				return true;
			}
		}
		OutError = TEXT("module_selector_not_found");
		return false;
	}

	bool ResolveLegacyAssignmentNodeByTargets(
		UNiagaraNodeOutput& OutputNode,
		const TArray<FNiagaraVariable>& RequestedTargets,
		UNiagaraNodeAssignment*& OutNode,
		int32& OutIndex,
		FString& OutError)
	{
		OutNode = nullptr;
		OutIndex = INDEX_NONE;
		OutError.Reset();
		TArray<UNiagaraNodeFunctionCall*> Modules;
		UeAgentNiagaraStageModuleAuthoring::GetModules(OutputNode, Modules);
		TArray<int32> Matches;
		for (int32 Index = 0; Index < Modules.Num(); ++Index)
		{
			UNiagaraNodeAssignment* Assignment = Cast<UNiagaraNodeAssignment>(Modules[Index]);
			if (!Assignment) continue;
			if (AssignmentVariablesMatch(Assignment->GetAssignmentTargets(), RequestedTargets))
			{
				Matches.Add(Index);
			}
		}
		if (Matches.IsEmpty())
		{
			OutError = TEXT("module_selector_not_found");
			return false;
		}
		if (Matches.Num() != 1)
		{
			OutError = TEXT("niagara_standalone_legacy_assignment_targets_ambiguous");
			return false;
		}
		OutIndex = Matches[0];
		OutNode = Cast<UNiagaraNodeAssignment>(Modules[OutIndex]);
		return OutNode != nullptr;
	}

	UNiagaraNode* AddAssignmentNode(
		UNiagaraNodeOutput& OutputNode,
		const TArray<FNiagaraVariable>& AssignmentTargets,
		const int32 TargetIndex,
		FString& OutError)
	{
		OutError.Reset();
		UNiagaraGraph* Graph = OutputNode.GetNiagaraGraph();
		if (!Graph || AssignmentTargets.IsEmpty())
		{
			OutError = TEXT("niagara_standalone_assignment_targets_missing");
			return nullptr;
		}
		TArray<UNiagaraNode*> Chain;
		if (!GetMainChain(OutputNode, Chain, &OutError)) return nullptr;
		UNiagaraNode* Node = nullptr;
		for (UNiagaraNode* Candidate : Chain)
		{
			if (!IsParameterMapSetNode(Candidate)) continue;
			TArray<FNiagaraVariable> ExistingTargets;
			GetAssignmentTargets(*Candidate, ExistingTargets);
			if (ExistingTargets.IsEmpty())
			{
				Node = Candidate;
				break;
			}
		}
		const bool bReusingSeedNode = Node != nullptr;
		if (!Node)
		{
			UClass* SetNodeClass = StaticLoadClass(
				UNiagaraNode::StaticClass(),
				nullptr,
				TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
			Node = SetNodeClass
				? NewObject<UNiagaraNode>(Graph, SetNodeClass, NAME_None, RF_Transactional)
				: nullptr;
		}
		if (!Node)
		{
			OutError = TEXT("niagara_standalone_assignment_node_create_failed");
			return nullptr;
		}
		if (!bReusingSeedNode)
		{
			Node->CreateNewGuid();
			Node->AllocateDefaultPins();
			Graph->AddNode(Node, true, false);
		}
		Node->NodeComment = StandaloneAssignmentMarker;
		Node->bCommentBubbleVisible = false;
		Node->bCommentBubblePinned = false;
		for (const FNiagaraVariable& Target : AssignmentTargets)
		{
			if (!AddTargetPin(*Node, Target, OutError))
			{
				if (!bReusingSeedNode) Graph->RemoveNode(Node);
				return nullptr;
			}
		}
		if (!bReusingSeedNode
			&& !InsertNodeAtIndex(OutputNode, *Node, TargetIndex == INDEX_NONE ? MAX_int32 : TargetIndex, OutError))
		{
			Graph->RemoveNode(Node);
			return nullptr;
		}
		return Node;
	}

	bool MoveAssignmentNode(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraNode& Node,
		const int32 TargetIndex,
		int32& OutNewIndex,
		FString& OutError)
	{
		OutNewIndex = INDEX_NONE;
		TArray<UNiagaraNode*> Nodes;
		GetAssignmentNodes(OutputNode, Nodes);
		const int32 CurrentIndex = Nodes.IndexOfByKey(&Node);
		if (CurrentIndex == INDEX_NONE)
		{
			OutError = TEXT("stage_module_not_found");
			return false;
		}
		const int32 ClampedIndex = FMath::Clamp(TargetIndex, 0, FMath::Max(0, Nodes.Num() - 1));
		if (CurrentIndex == ClampedIndex)
		{
			OutNewIndex = CurrentIndex;
			return true;
		}

		UEdGraphPin* Input = FindParameterMapPin(Node, EGPD_Input);
		UEdGraphPin* Output = FindParameterMapPin(Node, EGPD_Output);
		if (!Input || Input->LinkedTo.Num() != 1 || !Output || Output->LinkedTo.Num() != 1)
		{
			OutError = TEXT("niagara_standalone_assignment_links_invalid");
			return false;
		}
		UEdGraphPin* PreviousOutput = Input->LinkedTo[0];
		UEdGraphPin* NextInput = Output->LinkedTo[0];
		UNiagaraGraph* Graph = OutputNode.GetNiagaraGraph();
		if (!Graph)
		{
			OutError = TEXT("niagara_standalone_assignment_graph_missing");
			return false;
		}
		Input->BreakAllPinLinks(true);
		Output->BreakAllPinLinks(true);
		if (!TryConnect(*Graph, *PreviousOutput, *NextInput)
			|| !InsertNodeAtIndex(OutputNode, Node, ClampedIndex, OutError))
		{
			return false;
		}
		GetAssignmentNodes(OutputNode, Nodes);
		OutNewIndex = Nodes.IndexOfByKey(&Node);
		return OutNewIndex != INDEX_NONE;
	}

	bool RemoveAssignmentNode(UNiagaraNodeOutput& OutputNode, UNiagaraNode& Node, FString& OutError)
	{
		OutError.Reset();
		UNiagaraGraph* Graph = OutputNode.GetNiagaraGraph();
		UEdGraphPin* Input = FindParameterMapPin(Node, EGPD_Input);
		UEdGraphPin* Output = FindParameterMapPin(Node, EGPD_Output);
		if (!Graph || !Input || Input->LinkedTo.Num() != 1 || !Output || Output->LinkedTo.Num() != 1)
		{
			OutError = TEXT("niagara_standalone_assignment_links_invalid");
			return false;
		}
		UEdGraphPin* PreviousOutput = Input->LinkedTo[0];
		UEdGraphPin* NextInput = Output->LinkedTo[0];
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || IsParameterMapPin(Pin)
				|| Pin->PinType.PinSubCategory == DynamicAddPinSubCategory) continue;
			FString ClearError;
			if (!UeAgentNiagaraStageModuleAuthoring::ClearInputValueNodesForPin(*Pin, ClearError))
			{
				OutError = ClearError;
				return false;
			}
		}
		Input->BreakAllPinLinks(true);
		Output->BreakAllPinLinks(true);
		if (!TryConnect(*Graph, *PreviousOutput, *NextInput))
		{
			OutError = TEXT("niagara_standalone_assignment_remove_relink_failed");
			return false;
		}
		Graph->RemoveNode(&Node);
		Graph->NotifyGraphChanged();
		return true;
	}

	void GetAssignmentTargets(const UNiagaraNode& Node, TArray<FNiagaraVariable>& OutTargets)
	{
		OutTargets.Reset();
		for (const UEdGraphPin* Pin : Node.Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || IsParameterMapPin(Pin)
				|| Pin->PinType.PinSubCategory == DynamicAddPinSubCategory) continue;
			const FNiagaraVariable Variable = UEdGraphSchema_Niagara::PinToNiagaraVariable(Pin);
			if (Variable.IsValid()) OutTargets.Add(Variable);
		}
	}

	bool EnsureAssignmentTargetsRegistered(UNiagaraNode& Node, FString& OutError)
	{
		TArray<FNiagaraVariable> Targets;
		GetAssignmentTargets(Node, Targets);
		for (const FNiagaraVariable& Target : Targets)
		{
			if (!EnsureTargetRegistered(Node, Target, OutError)) return false;
		}
		return true;
	}

	bool AssignmentTargetsMatch(const UNiagaraNode& Node, const TArray<FNiagaraVariable>& RequestedTargets)
	{
		TArray<FNiagaraVariable> ExistingTargets;
		GetAssignmentTargets(Node, ExistingTargets);
		return AssignmentVariablesMatch(ExistingTargets, RequestedTargets);
	}

	UEdGraphPin* FindAssignmentTargetPin(
		UNiagaraNode& Node,
		const FString& InputName,
		FNiagaraVariable& OutVariable)
	{
		OutVariable = FNiagaraVariable();
		FString NormalizedName = InputName.TrimStartAndEnd();
		if (NormalizedName.StartsWith(TEXT("Module."), ESearchCase::IgnoreCase))
		{
			NormalizedName.RightChopInline(7);
		}
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || IsParameterMapPin(Pin)
				|| Pin->PinType.PinSubCategory == DynamicAddPinSubCategory) continue;
			if (!Pin->PinName.ToString().Equals(NormalizedName, ESearchCase::IgnoreCase)) continue;
			OutVariable = UEdGraphSchema_Niagara::PinToNiagaraVariable(Pin);
			return OutVariable.IsValid() ? Pin : nullptr;
		}
		return nullptr;
	}
}
