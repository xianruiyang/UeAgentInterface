// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringIdentity.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Variable.h"

namespace UeAgentBlueprintAuthoring
{
	bool BuildPinAliasLookup(const UEdGraphNode& Node,
		TMap<FString, UEdGraphPin*>& OutPins, UE::UAI::Contract::FDiagnostic& OutError)
	{
		using namespace UE::UAI::HjsonWorkspace;
		OutPins.Reset();
		OutError.Reset();
		TArray<FGraphAuthoringPinAliasInput> Inputs;
		TMap<FString, UEdGraphPin*> NativePins;
		TMap<FString, int32> Occurrences;
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (!Pin || Pin->bOrphanedPin) continue;
			const FString Name = Pin->PinName.ToString();
			const FString Id = FString::FromInt(Inputs.Num());
			Inputs.Add({Id, Name, Occurrences.FindOrAdd(Name.ToLower())++,
				Pin->Direction == EGPD_Output ? EGraphAuthoringEndpointDirection::Output
					: EGraphAuthoringEndpointDirection::Input});
			NativePins.Add(Id, Pin);
		}
		FGraphAuthoringPinAliasProjection Projection;
		FWorkspaceError Error;
		if (!TryBuildGraphAuthoringPinAliasProjection(Inputs, Projection, Error))
		{
			OutError = Error.GetDiagnostic();
			return false;
		}
		for (const auto& Entry : Projection.SourceIdByAlias)
			OutPins.Add(Entry.Key, NativePins.FindRef(Entry.Value));
		return true;
	}

	FString GraphIdentity(const UEdGraph& Graph)
	{
		return Graph.GraphGuid.IsValid()
			? TEXT("graph:") + Graph.GraphGuid.ToString(
				EGuidFormats::DigitsWithHyphensLower)
			: TEXT("graph:name:") + Graph.GetName();
	}

	FString GetBuiltinNodeId(const UEdGraphNode* Node)
	{
		if (Node && Node->IsA<UK2Node_FunctionEntry>()) return TEXT("__entry__");
		if (Node && Node->IsA<UK2Node_FunctionResult>()) return TEXT("__result__");
		if (const UK2Node_Tunnel* Tunnel = ExactCast<UK2Node_Tunnel>(Node))
		{
			bool bHasInput = false;
			bool bHasOutput = false;
			for (const UEdGraphPin* Pin : Tunnel->Pins)
			{
				if (!Pin) continue;
				bHasInput |= Pin->Direction == EGPD_Input;
				bHasOutput |= Pin->Direction == EGPD_Output;
			}
			if (bHasOutput && !bHasInput) return TEXT("__entry__");
			if (bHasInput && !bHasOutput) return TEXT("__result__");
		}
		return FString();
	}

	FString NodeReference(const UEdGraphNode* Node)
	{
		if (!Node) return FString();
		if (Node->NodeGuid.IsValid()) return Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		const FString Builtin = GetBuiltinNodeId(Node);
		return Builtin.IsEmpty() ? Node->GetName() : Builtin;
	}

	FString NodeSemanticName(const UEdGraphNode& Node)
	{
		if (const UK2Node_CallDelegate* Call = Cast<UK2Node_CallDelegate>(&Node))
			return Call->GetPropertyName().ToString();
		if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(&Node))
			if (const UEdGraph* MacroGraph = Macro->GetMacroGraph()) return MacroGraph->GetName();
		if (const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(&Node))
		{
			return Custom->CustomFunctionName.ToString();
		}
		if (const UK2Node_Event* Event = Cast<UK2Node_Event>(&Node))
		{
			return Event->EventReference.GetMemberName().ToString();
		}
		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(&Node))
		{
			if (const UFunction* Function = Call->GetTargetFunction())
			{
				return Function->GetName();
			}
			return Call->FunctionReference.GetMemberName().ToString();
		}
		if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(&Node))
		{
			return Variable->VariableReference.GetMemberName().ToString();
		}
		return Node.GetClass()->GetPathName();
	}
}
