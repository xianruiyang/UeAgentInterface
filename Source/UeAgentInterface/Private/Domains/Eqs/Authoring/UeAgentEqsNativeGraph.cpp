#include "Domains/Eqs/Authoring/UeAgentEqsNativeGraph.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "AIGraphNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQueryGraph.h"
#include "EnvironmentQueryGraphNode_Option.h"
#include "EnvironmentQueryGraphNode_Root.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAIGraphOwnership.h"

namespace UeAgentEqsAuthoring
{
	namespace
	{
		FString NodeId(const UEdGraphNode& Node)
		{
			return Node.IsA<UEnvironmentQueryGraphNode_Root>() ? TEXT("node:root")
				: TEXT("node:") + Node.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		}

		FString ObjectName(const UObject* Object)
		{
			return Object ? Object->GetPathName() : TEXT("missing");
		}

		UE::UAI::Contract::FDiagnostic NativeFact(const TCHAR* Code, const FString& Reason,
			const UObject& Target, const FString& Path, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected, const UEdGraphNode* Node = nullptr)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("eqs_capture"));
			Error.TargetPath = Target.GetPathName();
			Error.PropertyPath = Path;
			Error.SemanticPointer = Path;
			Error.SourceKind = TEXT("native_result");
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			if (Node)
			{
				Error.CandidateName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
				Error.ActualEvidence = MakeShared<FJsonValueString>(NodeId(*Node));
			}
			return Error;
		}

	}

	const UObject* GetNodePropertyObject(const UEdGraphNode& Node)
	{
		const auto* AiNode = Cast<UAIGraphNode>(&Node);
		if (!AiNode) return nullptr;
		if (const auto* Option = Cast<UEnvQueryOption>(AiNode->NodeInstance)) return Option->Generator;
		return Cast<UEnvQueryTest>(AiNode->NodeInstance);
	}

	bool CaptureNodeState(const UEdGraphNode& Node, TSharedPtr<FJsonObject>& OutState, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		OutState = MakeShared<FJsonObject>();
		OutState->SetStringField(TEXT("id"), NodeId(Node));
		OutState->SetStringField(TEXT("name"), Node.GetNodeTitle(ENodeTitleType::ListView).ToString());
		OutState->SetStringField(TEXT("node_class"), Node.GetClass()->GetPathName());
		const auto Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), Node.NodePosX);
		Position->SetNumberField(TEXT("y"), Node.NodePosY);
		OutState->SetObjectField(TEXT("pos"), Position);
		const UObject* Payload = GetNodePropertyObject(Node);
		OutState->SetStringField(TEXT("class_path"), Payload ? Payload->GetClass()->GetPathName() : Node.GetClass()->GetPathName());
		if (Node.IsA<UEnvironmentQueryGraphNode_Root>()) OutState->SetStringField(TEXT("role"), TEXT("root"));
		else if (!Payload)
		{
			OutError = NativeFact(TEXT("eqs_node_payload_missing"),
				FString::Printf(TEXT("EQS node of class '%s' has no generator or test payload."), *Node.GetClass()->GetPathName()),
				Node, TEXT("node.payload.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Node);
			return false;
		}
		if (const auto* Test = Cast<UEnvQueryTest>(Payload))
		{
			const FBoolProperty* Enabled = FindFProperty<FBoolProperty>(Node.GetClass(), TEXT("bTestEnabled"));
			const auto Ownership = UeAgentAuthoringAIGraphOwnership::Capture(Node);
			if (Ownership.ConflictingParent)
			{
				TArray<TSharedPtr<FJsonValue>> Owners;
				Owners.Add(MakeShared<FJsonValueString>(Ownership.Parent->GetPathName()));
				Owners.Add(MakeShared<FJsonValueString>(Ownership.ConflictingParent->GetPathName()));
				OutError = NativeFact(TEXT("eqs_test_owner_ambiguous"),
					TEXT("The EQS test belongs to more than one option node."), Node, TEXT("node.option_owners"),
					MakeShared<FJsonValueArray>(MoveTemp(Owners)), MakeShared<FJsonValueString>(TEXT("one option owner")), &Node);
				return false;
			}
			const auto* Parent = Cast<UEnvironmentQueryGraphNode_Option>(Ownership.Parent);
			if (!Enabled || !Parent)
			{
				OutError = NativeFact(TEXT("eqs_test_owner_invalid"), !Enabled
					? TEXT("The EQS test node has no bTestEnabled boolean property.")
					: TEXT("The EQS test has no owning option node."), Node,
					!Enabled ? TEXT("node.bTestEnabled.available") : TEXT("node.option_owner.available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Node);
				return false;
			}
			OutState->SetBoolField(TEXT("enabled"), Enabled->GetPropertyValue_InContainer(&Node));
			OutState->SetStringField(TEXT("parent"), NodeId(*Parent));
			OutState->SetStringField(TEXT("attachment_kind"), TEXT("test"));
			OutState->SetNumberField(TEXT("order"), Ownership.SubnodeIndex);
		}
		TArray<TSharedPtr<FJsonValue>> Pins;
		TMap<FString, int32> Occurrences;
		for (int32 PinIndex = 0; PinIndex < Node.Pins.Num(); ++PinIndex)
		{
			const UEdGraphPin* Pin = Node.Pins[PinIndex];
			if (!Pin)
			{
				OutError = NativeFact(TEXT("eqs_pin_missing"), TEXT("The EQS node contains a null pin."),
					Node, FString::Printf(TEXT("node.pins/%d"), PinIndex),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Node);
				return false;
			}
			const auto State = MakeShared<FJsonObject>();
			const FString Direction = Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output");
			State->SetStringField(TEXT("name"), Pin->PinName.ToString());
			State->SetStringField(TEXT("direction"), Direction);
			State->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
			State->SetNumberField(TEXT("occurrence"), Occurrences.FindOrAdd(Direction + TEXT(":") + Pin->PinName.ToString())++);
			Pins.Add(MakeShared<FJsonValueObject>(State));
		}
		const auto Interface = MakeShared<FJsonObject>();
		Interface->SetArrayField(TEXT("pins"), MoveTemp(Pins));
		OutState->SetObjectField(TEXT("pin_interface"), Interface);
		return true;
	}

	bool CaptureGraph(const UEnvQuery& Query, FGraphCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutCapture = {};
		OutError.Reset();
		const auto* Graph = Cast<UEnvironmentQueryGraph>(Query.EdGraph);
		if (!Graph)
		{
			if (Query.GetOptions().IsEmpty()) return true;
			OutError = NativeFact(TEXT("eqs_editor_graph_missing"),
				FString::Printf(TEXT("The EQS asset has %d options but no EnvironmentQuery editor graph."), Query.GetOptions().Num()),
				Query, TEXT("editor_graph.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (Graph->GetOuter() != &Query)
		{
			OutError = NativeFact(TEXT("eqs_graph_owner_invalid"), TEXT("The EQS editor graph belongs to another owner."),
				Query, TEXT("editor_graph.owner"), MakeShared<FJsonValueString>(ObjectName(Graph->GetOuter())),
				MakeShared<FJsonValueString>(Query.GetPathName()));
			return false;
		}
		TMap<const UEdGraphNode*, FString> IdByNode;
		TSet<FString> Ids;
		auto Add = [&](const UEdGraphNode* Node, const FString& Parent, int32 Order, const FString& Path) -> bool
		{
			if (!Node || Node->GetGraph() != Graph || !Node->NodeGuid.IsValid())
			{
				OutError = NativeFact(TEXT("eqs_node_identity_invalid"), !Node ? TEXT("The EQS graph contains a null node.")
					: Node->GetGraph() != Graph ? TEXT("The EQS node belongs to another graph.")
					: TEXT("The EQS node has an invalid node GUID."), Query, Path,
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Node);
				if (Node && Node->GetGraph() != Graph)
				{
					OutError.ActualValue = MakeShared<FJsonValueString>(ObjectName(Node->GetGraph()));
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Graph->GetPathName());
				}
				return false;
			}
			const FString Id = NodeId(*Node);
			if (Ids.Contains(Id) || IdByNode.Contains(Node))
			{
				OutError = NativeFact(TEXT("eqs_node_identity_duplicate"), IdByNode.Contains(Node)
					? TEXT("The same native EQS node was captured more than once.")
					: TEXT("Two native EQS nodes share a node identity."), Query, Path,
					MakeShared<FJsonValueString>(Node->GetPathName()), MakeShared<FJsonValueString>(TEXT("unique native node identity")), Node);
				for (const auto& Existing : IdByNode)
					if (Existing.Value == Id)
					{
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("identity distinct from ") + Existing.Key->GetPathName());
						break;
					}
				return false;
			}
			const auto* AiNode = Cast<UAIGraphNode>(Node);
			const auto* Option = AiNode ? Cast<UEnvQueryOption>(AiNode->NodeInstance) : nullptr;
			const auto* Test = AiNode ? Cast<UEnvQueryTest>(AiNode->NodeInstance) : nullptr;
			if (Parent.IsEmpty())
			{
				if (!Node->IsA<UEnvironmentQueryGraphNode_Root>() && (!Node->IsA<UEnvironmentQueryGraphNode_Option>()
					|| !Option || Option->GetOuter() != &Query || !Option->Generator || Option->Generator->GetOuter() != &Query))
				{
					const FString Reason = !Node->IsA<UEnvironmentQueryGraphNode_Option>()
						? TEXT("A top-level EQS node must be a root or option node.")
						: !Option ? TEXT("The EQS option node has no option payload.")
						: Option->GetOuter() != &Query ? TEXT("The EQS option payload belongs to another asset.")
						: !Option->Generator ? TEXT("The EQS option payload has no generator.")
						: TEXT("The EQS generator belongs to another asset.");
					OutError = NativeFact(TEXT("eqs_option_owner_invalid"), Reason, Query, Path,
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Node);
					if (!Node->IsA<UEnvironmentQueryGraphNode_Option>())
					{
						OutError.ActualValue = MakeShared<FJsonValueString>(Node->GetClass()->GetPathName());
						OutError.ExpectedValue = MakeShared<FJsonValueString>(UEnvironmentQueryGraphNode_Option::StaticClass()->GetPathName());
					}
					else if (Option && Option->GetOuter() != &Query)
					{
						OutError.ActualValue = MakeShared<FJsonValueString>(ObjectName(Option->GetOuter()));
						OutError.ExpectedValue = MakeShared<FJsonValueString>(Query.GetPathName());
					}
					else if (Option && Option->Generator && Option->Generator->GetOuter() != &Query)
					{
						OutError.ActualValue = MakeShared<FJsonValueString>(ObjectName(Option->Generator->GetOuter()));
						OutError.ExpectedValue = MakeShared<FJsonValueString>(Query.GetPathName());
					}
					return false;
				}
			}
			else if (!Test || Test->GetOuter() != &Query)
			{
				OutError = NativeFact(TEXT("eqs_test_owner_invalid"), Test
					? TEXT("The EQS test payload belongs to another asset.")
					: TEXT("The EQS option subnode has no test payload."), Query, Path,
					Test ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ObjectName(Test->GetOuter())))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
					Test ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Query.GetPathName()))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)), Node);
				return false;
			}
			Ids.Add(Id);
			IdByNode.Add(Node, Id);
			OutCapture.Nodes.Add({Id, Parent, Order, Node});
			return true;
		};
		for (int32 GraphIndex = 0; GraphIndex < Graph->Nodes.Num(); ++GraphIndex)
		{
			const UEdGraphNode* Node = Graph->Nodes[GraphIndex];
			const FString Path = FString::Printf(TEXT("graph.nodes/%d"), GraphIndex);
			if (!Add(Node, FString(), 0, Path)) return false;
			if (const auto* Option = Cast<UEnvironmentQueryGraphNode_Option>(Node))
				for (int32 Index = 0; Index < Option->SubNodes.Num(); ++Index)
					if (!Add(Option->SubNodes[Index], NodeId(*Option), Index,
						Path + FString::Printf(TEXT("/subnodes/%d"), Index))) return false;
		}
		for (const auto& Pair : IdByNode)
			for (int32 Index = 0; Index < Pair.Key->Pins.Num(); ++Index)
			{
				const UEdGraphPin* Pin = Pair.Key->Pins[Index];
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				for (int32 LinkIndex = 0; LinkIndex < Pin->LinkedTo.Num(); ++LinkIndex)
				{
					const UEdGraphPin* Linked = Pin->LinkedTo[LinkIndex];
					const FString* Target = Linked ? IdByNode.Find(Linked->GetOwningNode()) : nullptr;
					const int32 TargetPin = Linked ? Linked->GetOwningNode()->Pins.IndexOfByKey(Linked) : INDEX_NONE;
					if (!Target || TargetPin == INDEX_NONE)
					{
						auto Actual = MakeShared<FJsonObject>();
						Actual->SetStringField(TEXT("source_pin"), Pin->PinName.ToString());
						Actual->SetNumberField(TEXT("source_pin_index"), Index);
						Actual->SetBoolField(TEXT("linked_pin_present"), Linked != nullptr);
						Actual->SetBoolField(TEXT("target_node_captured"), Target != nullptr);
						Actual->SetNumberField(TEXT("target_pin_index"), TargetPin);
						if (Linked)
						{
							Actual->SetStringField(TEXT("target_node"), ObjectName(Linked->GetOwningNode()));
							Actual->SetStringField(TEXT("target_pin"), Linked->PinName.ToString());
							Actual->SetNumberField(TEXT("target_pin_count"), Linked->GetOwningNode()->Pins.Num());
						}
						OutError = NativeFact(TEXT("eqs_connection_target_invalid"), !Target
							? TEXT("The EQS output link does not identify a captured target node.")
							: TEXT("The EQS linked pin is absent from its owning node's pin array."),
							*Pair.Key, FString::Printf(TEXT("node.pins/%d/links/%d"), Index, LinkIndex),
							MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("captured target node and valid pin index")), Pair.Key);
						return false;
					}
					OutCapture.Connections.Add({FString::Printf(TEXT("edge:%s:%d:%s:%d"), *Pair.Value, Index, **Target, TargetPin),
						Pair.Value, *Target, Index, TargetPin});
				}
			}
		OutCapture.Nodes.Sort([](const FNodeCapture& A, const FNodeCapture& B) { return A.Id < B.Id; });
		OutCapture.Connections.Sort([](const FConnectionCapture& A, const FConnectionCapture& B) { return A.Id < B.Id; });
		return true;
	}
}
