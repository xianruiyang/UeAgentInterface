// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/Authoring/UeAgentNiagaraStandaloneModuleAuthoring.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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

	UE::UAI::Contract::FDiagnostic AssignmentFailure(
		const TCHAR* Code, const FString& Reason, const UNiagaraNode& Node,
		const FString& Field, const TSharedPtr<FJsonValue>& Actual,
		const TSharedPtr<FJsonValue>& Expected)
	{
		auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("niagara_authoring"));
		Error.SourceKind = TEXT("native_result");
		Error.CandidateName = Node.GetName();
		Error.InternalContext = Node.GetPathName();
		Error.PropertyPath = Field;
		Error.ActualValue = Actual;
		Error.ActualValuePresent = Actual.IsValid();
		Error.ExpectedValue = Expected;
		Error.ExpectedValuePresent = Expected.IsValid();
		return Error;
	}

	UE::UAI::Contract::FDiagnostic AssignmentLinksFailure(
		const UNiagaraNode& Node, const UEdGraphPin* Input, const UEdGraphPin* Output)
	{
		const bool bInputFailure = !Input || Input->LinkedTo.Num() != 1;
		const UEdGraphPin* Pin = bInputFailure ? Input : Output;
		return AssignmentFailure(TEXT("niagara_standalone_assignment_links_invalid"),
			Pin ? TEXT("Assignment parameter-map pin must have exactly one connection.")
				: TEXT("Assignment parameter-map pin is missing."),
			Node, bInputFailure
				? (Pin ? TEXT("input.link_count") : TEXT("input.available"))
				: (Pin ? TEXT("output.link_count") : TEXT("output.available")),
			Pin ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Pin->LinkedTo.Num()))
				: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
			Pin ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(1))
				: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)));
	}

	TSharedPtr<FJsonValue> AssignmentTargetValue(const FNiagaraVariable& Target)
	{
		auto Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("name"), Target.GetName().ToString());
		Value->SetStringField(TEXT("type"), Target.GetType().GetName());
		return MakeShared<FJsonValueObject>(Value);
	}

	UE::UAI::Contract::FDiagnostic AssignmentTargetSelectionFailure(
		const TCHAR* Code, const FString& Reason, const UNiagaraNode& OutputNode,
		const TArray<FNiagaraVariable>& Targets, const int32 MatchCount)
	{
		TArray<TSharedPtr<FJsonValue>> RequestedValues;
		for (const FNiagaraVariable& Target : Targets)
		{
			RequestedValues.Add(AssignmentTargetValue(Target));
		}
		auto Expected = MakeShared<FJsonObject>();
		Expected->SetNumberField(TEXT("match_count"), 1);
		Expected->SetArrayField(TEXT("assignment_targets"), RequestedValues);
		auto Actual = MakeShared<FJsonObject>();
		Actual->SetNumberField(TEXT("match_count"), MatchCount);
		return AssignmentFailure(Code, Reason, OutputNode, TEXT("assignment_selection"),
			MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
	}
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

	bool GetMainChain(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNode*>& OutNodes, UE::UAI::Contract::FDiagnostic& OutError)
	{
		return UeAgentNiagaraStageModuleAuthoring::GetParameterMapChainNodes(
			OutputNode, OutNodes, OutError);
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		UNiagaraGraph* Graph = OutputNode.GetNiagaraGraph();
		UEdGraphPin* NodeInput = FindParameterMapPin(Node, EGPD_Input);
		UEdGraphPin* NodeOutput = FindParameterMapPin(Node, EGPD_Output);
		if (!Graph || !NodeInput || !NodeOutput)
		{
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_node_invalid"),
				!Graph ? TEXT("Assignment insertion requires an available Niagara graph.")
					: TEXT("Assignment insertion requires both parameter-map input and output pins."),
				Node, !Graph ? TEXT("graph.available") : !NodeInput ? TEXT("input.available") : TEXT("output.available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}

		TArray<UNiagaraNode*> Chain;
		if (!GetMainChain(OutputNode, Chain, OutError)) return false;
		Chain.Remove(&Node);
		TArray<UNiagaraNode*> AssignmentNodes;
		if (!GetAssignmentNodes(OutputNode, AssignmentNodes, OutError)) return false;
		AssignmentNodes.Remove(&Node);
		const int32 InsertIndex = FMath::Clamp(TargetIndex, 0, AssignmentNodes.Num());
		UEdGraphNode* NextNode = &OutputNode;
		if (InsertIndex < AssignmentNodes.Num())
		{
			const int32 NextAssignmentChainIndex = Chain.IndexOfByKey(AssignmentNodes[InsertIndex]);
			if (NextAssignmentChainIndex == INDEX_NONE)
			{
				OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_insert_target_missing"),
					TEXT("The assignment selected as the insertion target is absent from the parameter-map chain."),
					*AssignmentNodes[InsertIndex], TEXT("chain_index"), MakeShared<FJsonValueNumber>(NextAssignmentChainIndex),
					MakeShared<FJsonValueString>(TEXT("a node index in the parameter-map chain")));
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
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_insert_point_invalid"),
				NextInput ? TEXT("The insertion target parameter-map input must have exactly one connection.")
					: TEXT("The insertion target has no parameter-map input."),
				Node, NextInput ? TEXT("insertion_target.input.link_count") : TEXT("insertion_target.input.available"),
				NextInput ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(NextInput->LinkedTo.Num()))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
				NextInput ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(1))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)));
			OutError.CandidateName = NextNode->GetName();
			return false;
		}
		UEdGraphPin* PreviousOutput = NextInput->LinkedTo[0];
		if (!PreviousOutput || !IsParameterMapPin(PreviousOutput))
		{
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_previous_output_invalid"),
				PreviousOutput ? TEXT("The insertion target is connected to a pin that is not a parameter map.")
					: TEXT("The insertion target connection has no upstream output pin."),
				Node, TEXT("insertion_target.upstream_pin_type"),
				PreviousOutput ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(UEdGraphSchema_Niagara::PinToTypeDefinition(PreviousOutput).GetName()))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
				MakeShared<FJsonValueString>(FNiagaraTypeDefinition::GetParameterMapDef().GetName()));
			OutError.CandidateName = NextNode->GetName();
			return false;
		}

		NodeInput->BreakAllPinLinks(true);
		NodeOutput->BreakAllPinLinks(true);
		NextInput->BreakAllPinLinks(true);
		const bool bPreviousConnected = TryConnect(*Graph, *PreviousOutput, *NodeInput);
		const bool bNextConnected = bPreviousConnected && TryConnect(*Graph, *NodeOutput, *NextInput);
		if (!bPreviousConnected || !bNextConnected)
		{
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_link_failed"),
				!bPreviousConnected ? TEXT("Failed to connect the previous parameter-map output to the assignment input.")
					: TEXT("Failed to connect the assignment output to the next parameter-map input."),
				Node, !bPreviousConnected ? TEXT("input.connected") : TEXT("output.connected"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		Node.NodePosX = (PreviousOutput->GetOwningNode()->NodePosX + NextNode->NodePosX) / 2;
		Node.NodePosY = NextNode->NodePosY;
		Graph->NotifyGraphChanged();
		return true;
	}

	bool AddTargetPin(UNiagaraNode& Node, const FNiagaraVariable& Target, UE::UAI::Contract::FDiagnostic& OutError)
	{
		UNiagaraGraph* Graph = Node.GetNiagaraGraph();
		const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
		if (!Graph || !Schema || !Target.IsValid())
		{
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_target_invalid"),
				!Graph ? TEXT("Assignment target pin creation requires a Niagara graph.")
					: !Schema ? TEXT("The Niagara graph schema is unavailable.")
					: TEXT("The assignment target must have a valid name and Niagara type."),
				Node, !Graph ? TEXT("graph.available") : !Schema ? TEXT("schema.available") : TEXT("target"),
				!Graph || !Schema ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)) : AssignmentTargetValue(Target),
				!Graph || !Schema ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("a nonempty parameter name and valid Niagara type"))));
			OutError.CandidateName = Target.GetName().ToString();
			return false;
		}

		UNiagaraScriptVariable* ExistingScriptVariable = Graph->GetScriptVariable(Target.GetName());
		if (ExistingScriptVariable && ExistingScriptVariable->Variable.GetType() != Target.GetType())
		{
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_target_type_conflict"),
				TEXT("The requested assignment type differs from the type registered for this parameter."),
				Node, TEXT("target.type"), MakeShared<FJsonValueString>(Target.GetType().GetName()),
				MakeShared<FJsonValueString>(ExistingScriptVariable->Variable.GetType().GetName()));
			OutError.CandidateName = Target.GetName().ToString();
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
				OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_target_pin_create_failed"),
					Pin ? TEXT("Native assignment pin creation returned a different parameter name.")
						: TEXT("Native assignment pin creation returned no pin."),
					Node, TEXT("target.pin_name"),
					Pin ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Pin->PinName.ToString()))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
					MakeShared<FJsonValueString>(Target.GetName().ToString()));
				OutError.CandidateName = Target.GetName().ToString();
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
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_target_pin_create_failed"),
				TEXT("The graph node could not create the assignment input pin."),
				Node, TEXT("target.pin_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.CandidateName = Target.GetName().ToString();
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

	bool EnsureTargetRegistered(UNiagaraNode& Node, const FNiagaraVariable& Target, UE::UAI::Contract::FDiagnostic& OutError)
	{
		UNiagaraGraph* Graph = Node.GetNiagaraGraph();
		if (!Graph || !Target.IsValid())
		{
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_target_invalid"),
				!Graph ? TEXT("Assignment target registration requires a Niagara graph.")
					: TEXT("The assignment target must have a valid name and Niagara type."),
				Node, !Graph ? TEXT("graph.available") : TEXT("target"),
				!Graph ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)) : AssignmentTargetValue(Target),
				!Graph ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("a nonempty parameter name and valid Niagara type"))));
			OutError.CandidateName = Target.GetName().ToString();
			return false;
		}
		if (UNiagaraScriptVariable* Existing = Graph->GetScriptVariable(Target.GetName()))
		{
			if (Existing->Variable.GetType() == Target.GetType()) return true;
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_target_type_conflict"),
				TEXT("The requested assignment type differs from the type registered for this parameter."),
				Node, TEXT("target.type"), MakeShared<FJsonValueString>(Target.GetType().GetName()),
				MakeShared<FJsonValueString>(Existing->Variable.GetType().GetName()));
			OutError.CandidateName = Target.GetName().ToString();
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
			const TSharedPtr<FJsonValue> ActualName = RegistrationPin
				? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(RegistrationPin->PinName.ToString()))
				: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
			if (RegistrationPin) Node.RemovePin(RegistrationPin);
			RestoreDetachedPins();
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_target_registration_failed"),
				TEXT("Native assignment registration did not create a pin with the requested parameter name."),
				Node, TEXT("target.pin_name"), ActualName, MakeShared<FJsonValueString>(Target.GetName().ToString()));
			OutError.CandidateName = Target.GetName().ToString();
			return false;
		}
		Node.RemovePin(RegistrationPin);
		RestoreDetachedPins();
		UNiagaraScriptVariable* Registered = Graph->GetScriptVariable(Target.GetName());
		if (!Registered || Registered->Variable.GetType() != Target.GetType())
		{
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_target_registration_failed"),
				Registered ? TEXT("The registered assignment parameter has a different type.")
					: TEXT("Native pin creation did not register the assignment parameter in the graph."),
				Node, TEXT("target.registered_type"),
				Registered ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Registered->Variable.GetType().GetName()))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
				MakeShared<FJsonValueString>(Target.GetType().GetName()));
			OutError.CandidateName = Target.GetName().ToString();
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

	bool GetAssignmentNodes(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNode*>& OutNodes, UE::UAI::Contract::FDiagnostic& OutError)
	{
		TArray<UNiagaraNode*> Chain;
		if (!GetMainChain(OutputNode, Chain, OutError))
		{
			OutNodes.Reset();
			return false;
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
		return true;
	}

	bool ResolveAssignmentNode(
		UNiagaraNodeOutput& OutputNode,
		const FGuid& NodeGuid,
		UNiagaraNode*& OutNode,
		int32& OutIndex,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutNode = nullptr;
		OutIndex = INDEX_NONE;
		TArray<UNiagaraNode*> Nodes;
		if (!GetAssignmentNodes(OutputNode, Nodes, OutError)) return false;
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index] && Nodes[Index]->NodeGuid == NodeGuid)
			{
				OutNode = Nodes[Index];
				OutIndex = Index;
				return true;
			}
		}
		OutError = AssignmentFailure(TEXT("module_selector_not_found"),
			TEXT("No standalone assignment in this stage matches the requested node selector."),
			OutputNode, TEXT("selector.match_count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1));
		OutError.SubjectId = NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		return false;
	}

	bool ResolveLegacyAssignmentNodeByTargets(
		UNiagaraNodeOutput& OutputNode,
		const TArray<FNiagaraVariable>& RequestedTargets,
		UNiagaraNodeAssignment*& OutNode,
		int32& OutIndex,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutNode = nullptr;
		OutIndex = INDEX_NONE;
		OutError.Reset();
		TArray<UNiagaraNodeFunctionCall*> Modules;
		if (!UeAgentNiagaraStageModuleAuthoring::GetModules(OutputNode, Modules, OutError)) return false;
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
			OutError = AssignmentTargetSelectionFailure(TEXT("module_selector_not_found"),
				TEXT("No legacy assignment matches the requested parameter names and types."),
				OutputNode, RequestedTargets, 0);
			return false;
		}
		if (Matches.Num() != 1)
		{
			OutError = AssignmentTargetSelectionFailure(TEXT("niagara_standalone_legacy_assignment_targets_ambiguous"),
				TEXT("More than one legacy assignment matches the requested parameter names and types."),
				OutputNode, RequestedTargets, Matches.Num());
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		UNiagaraGraph* Graph = OutputNode.GetNiagaraGraph();
		if (!Graph || AssignmentTargets.IsEmpty())
		{
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_targets_missing"),
				!Graph ? TEXT("Assignment creation requires an available Niagara graph.")
					: TEXT("Assignment creation requires at least one target parameter."),
				OutputNode, !Graph ? TEXT("graph.available") : TEXT("assignment_targets.count"),
				!Graph ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(AssignmentTargets.Num())),
				!Graph ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("at least one target"))));
			return nullptr;
		}
		TArray<UNiagaraNode*> Chain;
		if (!GetMainChain(OutputNode, Chain, OutError)) return nullptr;
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
		UClass* SetNodeClass = nullptr;
		if (!Node)
		{
			SetNodeClass = StaticLoadClass(
				UNiagaraNode::StaticClass(),
				nullptr,
				TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
			Node = SetNodeClass
				? NewObject<UNiagaraNode>(Graph, SetNodeClass, NAME_None, RF_Transactional)
				: nullptr;
		}
		if (!Node)
		{
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_node_create_failed"),
				SetNodeClass ? TEXT("Could not allocate the standalone parameter-map assignment node.")
					: TEXT("The Niagara parameter-map assignment node class could not be loaded."),
				OutputNode, SetNodeClass ? TEXT("assignment_node.created") : TEXT("assignment_node.class_available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutNewIndex = INDEX_NONE;
		TArray<UNiagaraNode*> Nodes;
		if (!GetAssignmentNodes(OutputNode, Nodes, OutError)) return false;
		const int32 CurrentIndex = Nodes.IndexOfByKey(&Node);
		if (CurrentIndex == INDEX_NONE)
		{
			OutError = AssignmentFailure(TEXT("stage_module_not_found"),
				TEXT("The assignment to move is absent from this stage's assignment list."),
				Node, TEXT("assignment_index"), MakeShared<FJsonValueNumber>(CurrentIndex),
				MakeShared<FJsonValueString>(TEXT("an existing assignment index")));
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
			OutError = AssignmentLinksFailure(Node, Input, Output);
			return false;
		}
		UEdGraphPin* PreviousOutput = Input->LinkedTo[0];
		UEdGraphPin* NextInput = Output->LinkedTo[0];
		UNiagaraGraph* Graph = OutputNode.GetNiagaraGraph();
		if (!Graph)
		{
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_graph_missing"),
				TEXT("Assignment movement requires an available Niagara graph."),
				Node, TEXT("graph.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		Input->BreakAllPinLinks(true);
		Output->BreakAllPinLinks(true);
		if (!TryConnect(*Graph, *PreviousOutput, *NextInput))
		{
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_move_relink_failed"),
				TEXT("Could not reconnect the parameter-map chain after detaching the assignment for movement."),
				Node, TEXT("chain.connected"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (!InsertNodeAtIndex(OutputNode, Node, ClampedIndex, OutError)) return false;
		if (!GetAssignmentNodes(OutputNode, Nodes, OutError)) return false;
		OutNewIndex = Nodes.IndexOfByKey(&Node);
		if (OutNewIndex == INDEX_NONE)
		{
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_move_readback_failed"),
				TEXT("The moved assignment is absent from the resulting stage assignment list."),
				Node, TEXT("assignment_index"), MakeShared<FJsonValueNumber>(OutNewIndex),
				MakeShared<FJsonValueNumber>(ClampedIndex));
			return false;
		}
		return true;
	}

	UNiagaraNode* ReplaceAssignmentNode(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraNode& Node,
		const TArray<FNiagaraVariable>& AssignmentTargets,
		const int32 TargetIndex,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		UNiagaraGraph* Graph = OutputNode.GetNiagaraGraph();
		const FGuid NodeGuid = Node.NodeGuid;
		if (!Graph)
		{
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_graph_missing"),
				TEXT("Assignment replacement requires an available Niagara graph."),
				Node, TEXT("graph.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return nullptr;
		}
		if (!RemoveAssignmentNode(OutputNode, Node, OutError)) return nullptr;

		// Clear this assignment's owned value nodes before changing parameter types.
		// UE owns graph-wide parameter identity, reference pins and metadata. Rebuilding
		// the whole stage would discard unrelated assignments and require a full input.
		for (const FNiagaraVariable& Target : AssignmentTargets)
		{
			const UNiagaraScriptVariable* Existing = Graph->GetScriptVariable(Target.GetName());
			if (Existing && Existing->Variable.GetType() != Target.GetType())
			{
				const TArray<FNiagaraVariable> VariablesToChange{Existing->Variable};
				Graph->ChangeParameterType(VariablesToChange, Target.GetType(), false);
			}
		}
		UNiagaraNode* Replacement = AddAssignmentNode(OutputNode, AssignmentTargets, TargetIndex, OutError);
		if (Replacement) Replacement->NodeGuid = NodeGuid;
		return Replacement;
	}

	bool RemoveAssignmentNode(UNiagaraNodeOutput& OutputNode, UNiagaraNode& Node, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		UNiagaraGraph* Graph = OutputNode.GetNiagaraGraph();
		UEdGraphPin* Input = FindParameterMapPin(Node, EGPD_Input);
		UEdGraphPin* Output = FindParameterMapPin(Node, EGPD_Output);
		if (!Graph || !Input || Input->LinkedTo.Num() != 1 || !Output || Output->LinkedTo.Num() != 1)
		{
			OutError = !Graph
				? AssignmentFailure(TEXT("niagara_standalone_assignment_links_invalid"),
					TEXT("Assignment removal requires an available Niagara graph."),
					Node, TEXT("graph.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true))
				: AssignmentLinksFailure(Node, Input, Output);
			return false;
		}
		UEdGraphPin* PreviousOutput = Input->LinkedTo[0];
		UEdGraphPin* NextInput = Output->LinkedTo[0];
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || IsParameterMapPin(Pin)
				|| Pin->PinType.PinSubCategory == DynamicAddPinSubCategory) continue;
			UE::UAI::Contract::FDiagnostic ClearError;
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
			OutError = AssignmentFailure(TEXT("niagara_standalone_assignment_remove_relink_failed"),
				TEXT("Could not reconnect the parameter-map chain after removing the assignment links."),
				Node, TEXT("chain.connected"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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

	bool EnsureAssignmentTargetsRegistered(UNiagaraNode& Node, UE::UAI::Contract::FDiagnostic& OutError)
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
