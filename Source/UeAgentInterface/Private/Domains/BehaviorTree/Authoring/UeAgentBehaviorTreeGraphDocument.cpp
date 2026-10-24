#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeGraphDocument.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringGraphLinks.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "AIGraphTypes.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeNativeGraph.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeAuthoringState.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringDirectApply.h"

namespace UeAgentBehaviorTreeAuthoring
{
	namespace
	{
		bool Fail(UE::UAI::Contract::FDiagnostic& Error, const TCHAR* Code, const FString& Reason,
			const FString& Path = FString(), TSharedPtr<FJsonValue> Actual = nullptr, TSharedPtr<FJsonValue> Expected = nullptr)
		{
			Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("behavior_tree_authoring"));
			Error.Source.InternalJsonPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			if (Error.ActualValue || Error.ExpectedValue) Error.ActualValuePresent = Error.ActualValue.IsValid();
			if (Error.ExpectedValue) Error.ExpectedValuePresent = true;
			return false;
		}

		bool NativeFail(UE::UAI::Contract::FDiagnostic& Error, const TCHAR* Code, const FString& Reason,
			const FString& Context = FString())
		{
			Fail(Error, Code, Reason);
			Error.SourceKind = TEXT("native_result");
			Error.InternalContext = Context;
			return false;
		}

		FString NodePath(const FString& Identity)
		{
			return TEXT("/nodes/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Identity);
		}

		bool ForwardAt(UE::UAI::Contract::FDiagnostic& Error, const FString& Path)
		{
			UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, Path);
			return false;
		}

		UClass* ResolveClass(const FString& Path, UClass* Base)
		{
			UClass* Class = FSoftClassPath(Path).TryLoadClass<UObject>();
			return Class && Class->IsChildOf(Base)
				&& !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists) ? Class : nullptr;
		}

		UEdGraphPin* ResolvePin(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Endpoint,
			EEdGraphPinDirection Direction, UE::UAI::Contract::FDiagnostic& OutError)
		{
			using namespace UE::UAI::HjsonWorkspace;
			FString Name;
			if (!Node)
			{
				NativeFail(OutError, TEXT("behavior_tree_graph_pin_unresolved"), TEXT("The connection's native graph node is unavailable."));
				return nullptr;
			}
			if (!Endpoint || !Endpoint->TryGetStringField(TEXT("pin"), Name))
			{
				Fail(OutError, TEXT("behavior_tree_graph_pin_unresolved"), TEXT("A connection endpoint requires a pin name string."), TEXT("/pin"),
					Endpoint ? Endpoint->TryGetField(TEXT("pin")) : nullptr, MakeShared<FJsonValueString>(TEXT("pin name string")));
				return nullptr;
			}
			double Occurrence = 0;
			const bool bHasOccurrence = Endpoint->HasField(TEXT("pin_occurrence"));
			Endpoint->TryGetNumberField(TEXT("pin_occurrence"), Occurrence);
			if (!bHasOccurrence)
			{
				TArray<FGraphAuthoringPinAliasInput> Inputs;
				TMap<FString, int32> Occurrences;
				for (int32 Index = 0; Index < Node->Pins.Num(); ++Index)
				{
					const auto* Pin = Node->Pins[Index];
					if (!Pin) continue;
					const FString Key = FString::FromInt(Pin->Direction) + TEXT(":") + Pin->PinName.ToString();
					Inputs.Add({FString::FromInt(Index), Pin->PinName.ToString(), Occurrences.FindOrAdd(Key)++,
						Pin->Direction == EGPD_Input ? EGraphAuthoringEndpointDirection::Input : EGraphAuthoringEndpointDirection::Output});
				}
				FGraphAuthoringPinAliasProjection Projection;
				FWorkspaceError ProjectionError;
				if (!TryBuildGraphAuthoringPinAliasProjection(Inputs, Projection, ProjectionError))
				{
					OutError = ProjectionError.GetDiagnostic();
					OutError.SourceKind = TEXT("native_result");
					return nullptr;
				}
				const FString* Index = Projection.SourceIdByAlias.Find(Name);
				UEdGraphPin* Pin = Index ? Node->Pins[FCString::Atoi(**Index)] : nullptr;
				if (Pin && Pin->Direction == Direction) return Pin;
			}
			else
			{
				int32 Seen = 0;
				for (UEdGraphPin* Pin : Node->Pins)
					if (Pin && Pin->Direction == Direction && Pin->PinName.ToString() == Name && Seen++ == Occurrence) return Pin;
			}
			auto Requested = MakeShared<FJsonObject>();
			Requested->SetStringField(TEXT("pin"), Name);
			if (bHasOccurrence) Requested->SetNumberField(TEXT("pin_occurrence"), Occurrence);
			Requested->SetStringField(TEXT("direction"), Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			Fail(OutError, TEXT("behavior_tree_graph_pin_unresolved"), TEXT("No native pin matches the requested name, occurrence and direction."),
				TEXT("/pin"), MakeShared<FJsonValueObject>(Requested), MakeShared<FJsonValueString>(TEXT("available pin in the requested direction")));
			return nullptr;
		}

	}

	static bool ValidateGraphContents(const TSharedPtr<FJsonValue>& Value, bool bOwned, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!Value.IsValid() || Value->Type != EJson::Object)
			return NativeFail(OutError, TEXT("behavior_tree_graph_document_invalid"), TEXT("The prepared Behavior Tree graph document is missing or is not an object."));
		const auto Document = Value->AsObject();
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		if (!Document->TryGetObjectField(TEXT("nodes"), Nodes) || !Nodes || !Nodes->IsValid())
			return NativeFail(OutError, TEXT("behavior_tree_graph_shape_invalid"), TEXT("The prepared graph has no object-valued node collection."));
		if (!Document->TryGetArrayField(TEXT("connections"), Edges) || !Edges)
			return NativeFail(OutError, TEXT("behavior_tree_graph_shape_invalid"), TEXT("The prepared graph has no array-valued connection collection."));
		for (const auto& Pair : (*Nodes)->Values)
		{
			const FString Path = NodePath(Pair.Key);
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
				return NativeFail(OutError, TEXT("behavior_tree_graph_node_invalid"), TEXT("A prepared graph node is missing or is not an object."), Pair.Key);
			const auto Node = Pair.Value->AsObject();
			if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Node,
				{{TEXT("injected"), EJson::Boolean}, {TEXT("parent"), EJson::String}}, OutError,
				FString())) return ForwardAt(OutError, Path);
			FGuid Guid;
			bool bInjected = false;
			Node->TryGetBoolField(TEXT("injected"), bInjected);
			FString NativeGuid, IdentityParent;
			if (bInjected)
			{
				if (bOwned) return NativeFail(OutError, TEXT("behavior_tree_graph_injected_identity_invalid"), TEXT("An injected node cannot be restored inside an owned decorator graph."), Pair.Key);
				if (!Node->TryGetStringField(TEXT("native_guid"), NativeGuid) || !FGuid::Parse(NativeGuid, Guid) || !Guid.IsValid())
					return NativeFail(OutError, TEXT("behavior_tree_graph_injected_identity_invalid"), TEXT("The captured injected node has no valid native GUID."), Pair.Key);
				if (!Node->TryGetStringField(TEXT("parent"), IdentityParent)
					|| Pair.Key != IdentityParent + TEXT("/") + Guid.ToString(EGuidFormats::DigitsWithHyphensLower))
					return NativeFail(OutError, TEXT("behavior_tree_graph_injected_identity_invalid"), TEXT("The injected node identity does not match its captured parent and native GUID."), Pair.Key);
			}
			else if (Pair.Key != TEXT("node:root") && (!Pair.Key.StartsWith(TEXT("node:"))
				|| !FGuid::Parse(Pair.Key.Mid(5), Guid) || !Guid.IsValid()))
				return NativeFail(OutError, TEXT("behavior_tree_graph_node_identity_invalid"), TEXT("The captured node identity is not a valid node GUID reference."), Pair.Key);
			FString NodeClass, PayloadClass, Parent;
			if (!Node->TryGetStringField(TEXT("node_class"), NodeClass)
				|| !ResolveClass(NodeClass, bOwned ? UEdGraphNode::StaticClass() : UBehaviorTreeGraphNode::StaticClass()))
				return NativeFail(OutError, TEXT("behavior_tree_graph_node_class_invalid"), TEXT("The prepared node class cannot be loaded as a concrete graph-node class for this graph."), Pair.Key + TEXT(": ") + NodeClass);
			const bool bComposite = NodeClass == UBehaviorTreeGraphNode_CompositeDecorator::StaticClass()->GetPathName();
			const bool bLogic = bOwned && NodeClass == TEXT("/Script/BehaviorTreeEditor.BehaviorTreeDecoratorGraphNode_Logic");
			if (bOwned && !bLogic && NodeClass != TEXT("/Script/BehaviorTreeEditor.BehaviorTreeDecoratorGraphNode_Decorator"))
				return NativeFail(OutError, TEXT("behavior_tree_owned_node_class_invalid"), TEXT("An owned decorator graph only supports native decorator and logic node classes."), NodeClass);
			if (bLogic)
			{
				FString Logic;
				int32 Inputs = 0;
				if (!Node->TryGetStringField(TEXT("logic"), Logic)
					|| (Logic != TEXT("Sink") && Logic != TEXT("And") && Logic != TEXT("Or") && Logic != TEXT("Not")))
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("behavior_tree_owned_logic_invalid"), TEXT("/logic"),
						MakeShared<FJsonValueString>(TEXT("Sink, And, Or, or Not")), Node->TryGetField(TEXT("logic")), TEXT("behavior_tree_authoring"), TEXT("The decorator logic mode is missing or unsupported."));
					OutError.ActualValuePresent = Node->HasField(TEXT("logic"));
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Node, TEXT("logic"));
					return ForwardAt(OutError, Path);
				}
				if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Node, TEXT("input_count"), Inputs, OutError)) return ForwardAt(OutError, Path + TEXT("/input_count"));
				if (!Node->HasField(TEXT("input_count")) || Inputs > MAX_uint16
					|| ((Logic == TEXT("And") || Logic == TEXT("Or")) ? Inputs < 2 : Inputs != 1))
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("behavior_tree_owned_inputs_invalid"), TEXT("/input_count"),
						MakeShared<FJsonValueString>((Logic == TEXT("And") || Logic == TEXT("Or")) ? TEXT("integer in [2, 65535]") : TEXT("1")),
						Node->TryGetField(TEXT("input_count")), TEXT("behavior_tree_authoring"), TEXT("The input count does not match this decorator logic mode."));
					OutError.ActualValuePresent = Node->HasField(TEXT("input_count"));
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Node, TEXT("input_count"));
					return ForwardAt(OutError, Path);
				}
			}
			if (Pair.Key == TEXT("node:root") && NodeClass != UBehaviorTreeGraphNode_Root::StaticClass()->GetPathName())
				return NativeFail(OutError, TEXT("behavior_tree_graph_root_class_invalid"), TEXT("The graph root does not have the native Behavior Tree root class."), NodeClass);
			if (Pair.Key != TEXT("node:root") && !bComposite && !bLogic
				&& (!Node->TryGetStringField(TEXT("payload_class"), PayloadClass)
					|| !ResolveClass(PayloadClass, bOwned ? UBTDecorator::StaticClass() : UBTNode::StaticClass())))
				return NativeFail(OutError, TEXT("behavior_tree_graph_payload_class_invalid"), TEXT("The prepared node payload cannot be loaded as a concrete Behavior Tree node or decorator for this graph."), Pair.Key + TEXT(": ") + PayloadClass);
			Node->TryGetStringField(TEXT("parent"), Parent);
			if ((bOwned && !Parent.IsEmpty()) || (bComposite && Parent.IsEmpty()))
				return Fail(OutError, TEXT("behavior_tree_graph_parent_invalid"), bOwned
					? TEXT("A node inside an owned decorator graph cannot have an attachment parent.")
					: TEXT("A composite decorator requires a parent node."), Path + TEXT("/parent"));
			if (!Parent.IsEmpty() && (Parent == Pair.Key || !(*Nodes)->HasField(Parent)))
				return Fail(OutError, TEXT("behavior_tree_graph_parent_invalid"), Parent == Pair.Key
					? TEXT("A node cannot be its own parent.") : TEXT("The referenced parent node is absent from this graph."), Path + TEXT("/parent"));
			if (!Parent.IsEmpty())
			{
				const auto* ParentValue = (*Nodes)->Values.Find(Parent);
				FString ParentClass, ParentParent;
				if (!ParentValue || !ParentValue->IsValid() || (*ParentValue)->Type != EJson::Object
					|| !(*ParentValue)->AsObject()->TryGetStringField(TEXT("node_class"), ParentClass)
					|| (!ResolveClass(ParentClass, UBehaviorTreeGraphNode_Composite::StaticClass())
						&& !ResolveClass(ParentClass, UBehaviorTreeGraphNode_Task::StaticClass()))
					|| ((*ParentValue)->AsObject()->TryGetStringField(TEXT("parent"), ParentParent) && !ParentParent.IsEmpty()))
					return Fail(OutError, TEXT("behavior_tree_graph_attachment_parent_invalid"), TEXT("An attachment parent must be a top-level composite or task node."), Path + TEXT("/parent"));
				if (!bComposite && !ResolveClass(PayloadClass, UBTDecorator::StaticClass()) && !ResolveClass(PayloadClass, UBTService::StaticClass()))
					return NativeFail(OutError, TEXT("behavior_tree_graph_attachment_type_invalid"), TEXT("An attachment payload must be a decorator or service."), PayloadClass);
				for (const TCHAR* Field : {TEXT("order"), TEXT("subnode_order")})
				{
					int32 Order = 0;
					if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Node, Field, Order, OutError)) return ForwardAt(OutError, Path + TEXT("/") + Field);
					if (Order < 0)
						return Fail(OutError, TEXT("behavior_tree_graph_attachment_order_invalid"), TEXT("An attachment order must be a nonnegative integer."),
							Path + TEXT("/") + Field, Node->TryGetField(Field), MakeShared<FJsonValueString>(TEXT("nonnegative integer")));
				}
			}
			const TSharedPtr<FJsonObject>* Position = nullptr;
			if (!Node->TryGetObjectField(TEXT("pos"), Position) || !Position || !Position->IsValid())
				return Fail(OutError, TEXT("behavior_tree_graph_position_invalid"), TEXT("The node position must be an object with x and y coordinates."), Path + TEXT("/pos"),
					Node->TryGetField(TEXT("pos")), MakeShared<FJsonValueString>(TEXT("position object")));
			for (const TCHAR* Axis : {TEXT("x"), TEXT("y")})
			{
				double Coordinate = 0;
				if (!(*Position)->TryGetNumberField(Axis, Coordinate) || !FMath::IsFinite(Coordinate) || Coordinate < MIN_int32 || Coordinate > MAX_int32)
					return Fail(OutError, TEXT("behavior_tree_graph_position_invalid"), TEXT("A graph coordinate must be a finite number in the signed 32-bit range."),
						Path + TEXT("/pos/") + Axis, (*Position)->TryGetField(Axis), MakeShared<FJsonValueString>(TEXT("finite number in [-2147483648, 2147483647]")));
			}
			for (const TCHAR* Field : {TEXT("properties"), TEXT("property_changes")})
			{
				TSharedPtr<FJsonObject> Properties;
				if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Node, Field, Properties, OutError)) return ForwardAt(OutError, Path + TEXT("/") + Field);
			}
		}
		if (bOwned)
		{
			int32 Sinks = 0;
			for (const auto& Pair : (*Nodes)->Values)
			{
				FString Logic;
				Sinks += Pair.Value->AsObject()->TryGetStringField(TEXT("logic"), Logic) && Logic == TEXT("Sink");
			}
			if (Sinks != 1) return Fail(OutError, TEXT("behavior_tree_owned_sink_invalid"), TEXT("An owned decorator graph must contain exactly one sink node."),
				TEXT("/nodes"), MakeShared<FJsonValueNumber>(Sinks), MakeShared<FJsonValueNumber>(1));
		}
		for (int32 EdgeIndex = 0; EdgeIndex < Edges->Num(); ++EdgeIndex)
		{
			const auto& EdgeValue = (*Edges)[EdgeIndex];
			const FString Path = TEXT("/connections/") + FString::FromInt(EdgeIndex);
			if (!EdgeValue.IsValid() || EdgeValue->Type != EJson::Object)
				return NativeFail(OutError, TEXT("behavior_tree_graph_connection_invalid"), TEXT("A prepared graph connection is missing or is not an object."), Path);
			for (const TCHAR* Field : {TEXT("from"), TEXT("to")})
			{
				const TSharedPtr<FJsonObject>* Endpoint = nullptr;
				FString Node, Pin;
				int32 Occurrence = 0;
				const FString EndpointPath = Path + TEXT("/") + Field;
				if (!EdgeValue->AsObject()->TryGetObjectField(Field, Endpoint) || !Endpoint || !Endpoint->IsValid())
					return NativeFail(OutError, TEXT("behavior_tree_graph_endpoint_invalid"), TEXT("The prepared connection endpoint is not an object."), EndpointPath);
				if (!(*Endpoint)->TryGetStringField(TEXT("node"), Node) || !(*Nodes)->HasField(Node))
					return Fail(OutError, TEXT("behavior_tree_graph_endpoint_invalid"), TEXT("The connection endpoint does not identify an available graph node."), EndpointPath + TEXT("/node"));
				if (!(*Endpoint)->TryGetStringField(TEXT("pin"), Pin))
					return Fail(OutError, TEXT("behavior_tree_graph_endpoint_invalid"), TEXT("The connection endpoint requires a pin name string."), EndpointPath + TEXT("/pin"),
						(*Endpoint)->TryGetField(TEXT("pin")), MakeShared<FJsonValueString>(TEXT("pin name string")));
				if (!UeAgentAuthoringJsonFields::ReadInt32Field(**Endpoint, TEXT("pin_occurrence"), Occurrence, OutError)) return ForwardAt(OutError, EndpointPath + TEXT("/pin_occurrence"));
				if (Occurrence < 0)
					return Fail(OutError, TEXT("behavior_tree_graph_pin_occurrence_invalid"), TEXT("A pin occurrence must be a nonnegative integer."), EndpointPath + TEXT("/pin_occurrence"),
						(*Endpoint)->TryGetField(TEXT("pin_occurrence")), MakeShared<FJsonValueString>(TEXT("nonnegative integer")));
			}
		}
		return true;
	}

	bool ValidateGraphDocument(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!ValidateGraphContents(Value, false, OutError)) return false;
		const auto Document = Value->AsObject();
		const auto Nodes = Document->GetObjectField(TEXT("nodes"));
		const TSharedPtr<FJsonObject>* Owned = nullptr;
		if (Document->HasField(TEXT("owned_graphs")) && !Document->TryGetObjectField(TEXT("owned_graphs"), Owned))
			return NativeFail(OutError, TEXT("behavior_tree_owned_graphs_invalid"), TEXT("The prepared owned-graph collection is not an object."));
		TSet<FString> Owners;
		if (Owned) for (const auto& Pair : (*Owned)->Values)
		{
			if (!ValidateGraphContents(Pair.Value, true, OutError)) return ForwardAt(OutError,
				TEXT("/owned_graphs/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key));
			FString Owner;
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (!Pair.Value->AsObject()->TryGetStringField(TEXT("owner"), Owner)
				|| Pair.Key != TEXT("decorator:") + Owner || Owners.Contains(Owner)
				|| !Nodes->TryGetObjectField(Owner, Node)
				|| (*Node)->GetStringField(TEXT("node_class")) != UBehaviorTreeGraphNode_CompositeDecorator::StaticClass()->GetPathName())
				return NativeFail(OutError, TEXT("behavior_tree_owned_graph_owner_invalid"), TEXT("An owned graph must identify one unique, matching composite decorator owner."), Pair.Key);
			Owners.Add(Owner);
		}
		for (const auto& Pair : Nodes->Values)
			if (Pair.Value->AsObject()->GetStringField(TEXT("node_class")) == UBehaviorTreeGraphNode_CompositeDecorator::StaticClass()->GetPathName()
				&& !Owners.Contains(Pair.Key)) return NativeFail(OutError, TEXT("behavior_tree_owned_graph_missing"), TEXT("A composite decorator node has no corresponding owned graph."), Pair.Key);
		return true;
	}

	static TSharedRef<FJsonObject> ReadGraphContentsDocument(const FGraphContents& Capture)
	{
		const auto Document = MakeShared<FJsonObject>();
		const auto Nodes = MakeShared<FJsonObject>();
		TMap<FString, const UEdGraphNode*> NativeNodes;
		for (const FNodeCapture& Captured : Capture.Nodes)
		{
			NativeNodes.Add(Captured.Id, Captured.NativeNode);
			const auto Node = Captured.State;
			Node->RemoveField(TEXT("pins"));
			Node->SetStringField(TEXT("payload_class"), Captured.PayloadClass);
			Node->SetNumberField(TEXT("subnode_order"), Captured.Order);
			if (Node->HasField(TEXT("logic")))
			{
				int32 InputCount = 0;
				for (const UEdGraphPin* Pin : Captured.NativeNode->Pins) InputCount += Pin && Pin->Direction == EGPD_Input;
				Node->SetNumberField(TEXT("input_count"), InputCount);
			}
			Nodes->SetObjectField(Captured.Id, Node);
		}
		TArray<TSharedPtr<FJsonValue>> Connections;
		for (const FConnectionCapture& Captured : Capture.Connections)
		{
			const auto Edge = MakeShared<FJsonObject>();
			auto AddEndpoint = [&](const TCHAR* Field, const FString& Id, const TCHAR* IndexField) {
				const UEdGraphNode* Node = NativeNodes.FindChecked(Id);
				const int32 Index = Captured.State->GetIntegerField(IndexField);
				const UEdGraphPin* Pin = Node->Pins[Index];
				int32 Occurrence = 0;
				for (int32 I = 0; I < Index; ++I)
					if (Node->Pins[I] && Node->Pins[I]->Direction == Pin->Direction && Node->Pins[I]->PinName == Pin->PinName) ++Occurrence;
				const auto Endpoint = MakeShared<FJsonObject>();
				Endpoint->SetStringField(TEXT("node"), Id);
				Endpoint->SetStringField(TEXT("pin"), Pin->PinName.ToString());
				Endpoint->SetNumberField(TEXT("pin_occurrence"), Occurrence);
				Edge->SetObjectField(Field, Endpoint);
			};
			AddEndpoint(TEXT("from"), Captured.SourceId, TEXT("source_pin"));
			AddEndpoint(TEXT("to"), Captured.TargetId, TEXT("target_pin"));
			Connections.Add(MakeShared<FJsonValueObject>(Edge));
		}
		Document->SetObjectField(TEXT("nodes"), Nodes);
		Document->SetArrayField(TEXT("connections"), MoveTemp(Connections));
		return Document;
	}

	bool ReadGraphDocument(UBehaviorTree& Tree, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FGraphCapture Capture;
		if (!CaptureGraph(Tree, Capture, OutError)) return false;
		const auto Document = ReadGraphContentsDocument(Capture);
		const auto OwnedGraphs = MakeShared<FJsonObject>();
		for (const FOwnedGraphCapture& Captured : Capture.OwnedGraphs)
		{
			const auto Graph = ReadGraphContentsDocument(Captured);
			Graph->SetStringField(TEXT("owner"), Captured.OwnerId);
			Graph->SetStringField(TEXT("name"), Captured.NativeGraph->GetName());
			Graph->SetStringField(TEXT("guid"), Captured.NativeGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			OwnedGraphs->SetObjectField(Captured.Id, Graph);
		}
		Document->SetObjectField(TEXT("owned_graphs"), OwnedGraphs);
		Document->SetStringField(TEXT("blackboard"), Tree.BlackboardAsset ? Tree.BlackboardAsset->GetPathName() : FString());
		OutValue = MakeShared<FJsonValueObject>(Document);
		return true;
	}

	bool BuildGraphDocumentFromMutations(UBehaviorTree& Tree, const TArray<FMutation>& Mutations,
		TSharedPtr<FJsonObject>& OutDocument, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutDocument.Reset();
		TSharedPtr<FJsonValue> Before;
		if (!ReadGraphDocument(Tree, Before, OutError)) return false;
		const auto Document = Before->AsObject();
		const auto Nodes = Document->GetObjectField(TEXT("nodes"));
		TMap<FString, const UEdGraphNode*> NativeNodes;
		FGraphCapture Capture;
		if (!CaptureGraph(Tree, Capture, OutError)) return false;
		for (const FNodeCapture& Node : Capture.Nodes) NativeNodes.Add(Node.Id, Node.NativeNode);
		for (const FMutation& Mutation : Mutations)
		{
			if (Mutation.Kind == EMutationKind::BlackboardBinding)
			{
				Document->SetStringField(TEXT("blackboard"), Mutation.ObjectRef);
				continue;
			}
			if (Mutation.Kind == EMutationKind::Connection) continue;
			if (Nodes->HasField(Mutation.Identity))
				return NativeFail(OutError, TEXT("behavior_tree_graph_node_already_exists"), TEXT("The requested node identity already exists in the captured graph."), Mutation.Identity);
			const bool bRoot = Mutation.Kind == EMutationKind::Root;
			UClass* NodeClass = bRoot ? UBehaviorTreeGraphNode_Root::StaticClass() : Mutation.Node.GraphNodeClass;
			if (!NodeClass || !NodeClass->IsChildOf(UBehaviorTreeGraphNode::StaticClass())
				|| (!bRoot && !Mutation.Node.PayloadClass))
				return NativeFail(OutError, TEXT("behavior_tree_graph_node_class_invalid"), TEXT("The requested mutation has no compatible graph-node class or required payload class."), Mutation.Identity);
			const auto Node = MakeShared<FJsonObject>();
			Node->SetStringField(TEXT("node_class"), NodeClass->GetPathName());
			Node->SetStringField(TEXT("payload_class"), bRoot ? NodeClass->GetPathName() : Mutation.Node.PayloadClass->GetPathName());
			Node->SetStringField(TEXT("parent"), Mutation.ParentId);
			Node->SetStringField(TEXT("candidate_name"), Mutation.Node.CandidateName);
			Node->SetStringField(TEXT("candidate_id"), Mutation.Node.CandidateId);
			const auto Select = MakeShared<FJsonObject>();
			for (const auto& Field : Mutation.Node.CandidateSelect) Select->SetStringField(Field.Key, Field.Value);
			Node->SetObjectField(TEXT("candidate_select"), Select);
			const auto Position = MakeShared<FJsonObject>();
			Position->SetNumberField(TEXT("x"), Mutation.Node.Location.X);
			Position->SetNumberField(TEXT("y"), Mutation.Node.Location.Y);
			Node->SetObjectField(TEXT("pos"), Position);
			Nodes->SetObjectField(Mutation.Identity, Node);
			// The old input uses direction-relative indices. Obtain their names from native pin allocation.
			UBehaviorTreeGraphNode* Template = NewObject<UBehaviorTreeGraphNode>(GetTransientPackage(), NodeClass);
			Template->AllocateDefaultPins();
			NativeNodes.Add(Mutation.Identity, Template);
		}
		auto Endpoint = [&](const FString& Id, EEdGraphPinDirection Direction, int32 DirectionIndex,
			TSharedPtr<FJsonObject>& OutEndpoint) {
			const UEdGraphNode* Node = NativeNodes.FindRef(Id);
			if (!Node) return NativeFail(OutError, TEXT("behavior_tree_graph_endpoint_invalid"), TEXT("The connection mutation references an unavailable native node."), Id);
			if (DirectionIndex < 0) return Fail(OutError, TEXT("behavior_tree_graph_endpoint_invalid"), TEXT("A direction-relative pin index cannot be negative."),
				TEXT("/pin"), MakeShared<FJsonValueNumber>(DirectionIndex), MakeShared<FJsonValueString>(TEXT("nonnegative pin index")));
			int32 Index = 0;
			TMap<FName, int32> Occurrences;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != Direction) continue;
				const int32 Occurrence = Occurrences.FindOrAdd(Pin->PinName)++;
				if (Index++ != DirectionIndex) continue;
				OutEndpoint = MakeShared<FJsonObject>();
				OutEndpoint->SetStringField(TEXT("node"), Id);
				OutEndpoint->SetStringField(TEXT("pin"), Pin->PinName.ToString());
				OutEndpoint->SetNumberField(TEXT("pin_occurrence"), Occurrence);
				return true;
			}
			return Fail(OutError, TEXT("behavior_tree_graph_endpoint_invalid"), TEXT("No pin exists at the requested index in the requested direction."),
				TEXT("/pin"), MakeShared<FJsonValueNumber>(DirectionIndex), MakeShared<FJsonValueString>(Direction == EGPD_Input ? TEXT("available input pin index") : TEXT("available output pin index")));
		};
		TArray<TSharedPtr<FJsonValue>> Connections = Document->GetArrayField(TEXT("connections"));
		for (const FMutation& Mutation : Mutations)
		{
			if (Mutation.Kind != EMutationKind::Connection) continue;
			TSharedPtr<FJsonObject> From, To;
			const FString Path = TEXT("/connections/") + FString::FromInt(Connections.Num());
			if (!Endpoint(Mutation.FromId, EGPD_Output, Mutation.FromIndex, From)) return ForwardAt(OutError, Path + TEXT("/from"));
			if (!Endpoint(Mutation.ToId, EGPD_Input, Mutation.ToIndex, To)) return ForwardAt(OutError, Path + TEXT("/to"));
			const auto Edge = MakeShared<FJsonObject>();
			Edge->SetObjectField(TEXT("from"), From);
			Edge->SetObjectField(TEXT("to"), To);
			Connections.Add(MakeShared<FJsonValueObject>(Edge));
		}
		Document->SetArrayField(TEXT("connections"), MoveTemp(Connections));
		if (!ValidateGraphDocument(Before, OutError)) return false;
		OutDocument = Document;
		return true;
	}

	bool BuildDefaultOwnedGraphDocument(const FString& OwnerId, TSharedPtr<FJsonObject>& OutDocument, UE::UAI::Contract::FDiagnostic& OutError)
	{
		TStrongObjectPtr<UBehaviorTreeGraph> Graph(NewObject<UBehaviorTreeGraph>(GetTransientPackage(), NAME_None, RF_Transient));
		auto* Owner = NewObject<UBehaviorTreeGraphNode_CompositeDecorator>(Graph.Get(), NAME_None, RF_Transient);
		Owner->PostPlacedNewNode();
		FGraphContents Capture;
		if (!Owner->BoundGraph) return NativeFail(OutError, TEXT("behavior_tree_owned_default_graph_missing"), TEXT("The native composite decorator did not create its default owned graph."), OwnerId);
		if (!CaptureGraphContents(*Owner->BoundGraph, Capture, OutError)) return false;
		OutDocument = ReadGraphContentsDocument(Capture);
		OutDocument->SetStringField(TEXT("owner"), OwnerId);
		OutDocument->SetStringField(TEXT("name"), Owner->BoundGraph->GetName());
		OutDocument->SetStringField(TEXT("guid"), Owner->BoundGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		return ValidateGraphContents(MakeShared<FJsonValueObject>(OutDocument), true, OutError);
	}

	static bool ApplyOwnedGraphDocument(UBehaviorTree& Tree, UBehaviorTreeGraphNode_CompositeDecorator& Owner,
		const FString& GraphId, const TSharedPtr<FJsonObject>& Document, UE::UAI::Contract::FDiagnostic& OutError,
		TArray<UeAgentDetailsAuthoringService::FScopedPropertyIntent>* OutPropertyIntents)
	{
		UEdGraph* Graph = Owner.BoundGraph;
		if (!Graph) return NativeFail(OutError, TEXT("behavior_tree_owned_graph_missing"), TEXT("The decorator owner has no bound native graph."));
		const auto Desired = Document->GetObjectField(TEXT("nodes"));
		TMap<FString, UEdGraphNode*> Nodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) return NativeFail(OutError, TEXT("behavior_tree_owned_node_missing"), TEXT("The native decorator graph contains a missing node."));
			Nodes.Add(TEXT("node:") + Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower), Node);
		}
		for (const auto& Pair : Desired->Values)
		{
			const auto Spec = Pair.Value->AsObject();
			UClass* Class = ResolveClass(Spec->GetStringField(TEXT("node_class")), UEdGraphNode::StaticClass());
			if (UEdGraphNode* Existing = Nodes.FindRef(Pair.Key); Existing && Existing->GetClass() != Class)
				return NativeFail(OutError, TEXT("behavior_tree_owned_node_type_changed"), TEXT("The native decorator node class differs from the requested class."), Pair.Key);
		}
		Graph->Modify();
		for (const auto& Pair : Nodes)
		{
			Pair.Value->Modify();
			for (UEdGraphPin* Pin : Pair.Value->Pins) if (Pin) Pin->BreakAllPinLinks();
			if (!Desired->HasField(Pair.Key)) Pair.Value->DestroyNode();
		}
		for (const auto& Pair : Desired->Values)
		{
			const auto Spec = Pair.Value->AsObject();
			UEdGraphNode* Node = Nodes.FindRef(Pair.Key);
			const bool bNew = Node == nullptr;
			FString CandidateName;
			Spec->TryGetStringField(TEXT("candidate_name"), CandidateName);
			const bool bNativeCreate = bNew && !CandidateName.IsEmpty();
			if (bNew)
			{
				UClass* Class = ResolveClass(Spec->GetStringField(TEXT("node_class")), UEdGraphNode::StaticClass());
				if (bNativeCreate)
				{
					using namespace UE::UAI::QueryOwners::AiBehavior;
					FAiBehaviorCandidate Candidate;
					FAiBehaviorCandidateQueryError Error;
					TMap<FString, FString> Select;
					const TSharedPtr<FJsonObject>* Fields = nullptr;
					if (Spec->TryGetObjectField(TEXT("candidate_select"), Fields))
						for (const auto& Field : (*Fields)->Values) Select.Add(Field.Key, Field.Value->AsString());
					const auto Position = Spec->GetObjectField(TEXT("pos"));
					const FAiBehaviorCandidateContext Context{nullptr, FString(), Graph};
					if (!TryPerformAiGraphCandidate(Tree, CandidateName, FString(),
						FVector2f(Position->GetNumberField(TEXT("x")), Position->GetNumberField(TEXT("y"))),
						Node, Candidate, Error, Select, Context))
					{
						OutError = MoveTemp(Error);
						return ForwardAt(OutError, NodePath(Pair.Key)
							+ (OutError.PropertyPath == TEXT("x") || OutError.PropertyPath == TEXT("y") ? TEXT("/pos") : TEXT("/candidate")));
					}
					if (!Node || Node->GetClass() != Class) return NativeFail(OutError, TEXT("behavior_tree_owned_node_type_changed"), TEXT("The native decorator node class differs from the requested class."));
				}
				else
				{
					Node = NewObject<UEdGraphNode>(Graph, Class, NAME_None, RF_Transactional);
					Graph->AddNode(Node, false, false);
				}
				FGuid::Parse(Pair.Key.Mid(5), Node->NodeGuid);
				Nodes.Add(Pair.Key, Node);
			}
			FString Logic;
			if (Spec->TryGetStringField(TEXT("logic"), Logic))
			{
				FByteProperty* Property = FindFProperty<FByteProperty>(Node->GetClass(), TEXT("LogicMode"));
				int64 Mode = 0;
				if (!UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(Property ? Property->Enum : nullptr, Logic, Mode))
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("behavior_tree_owned_logic_invalid"), TEXT("/logic"),
						MakeShared<FJsonValueString>(Property && Property->Enum ? Property->Enum->GetPathName() : TEXT("native LogicMode enum")),
						MakeShared<FJsonValueString>(Logic), TEXT("behavior_tree_authoring"), Property && Property->Enum
							? TEXT("The selected logic mode is not a member of the native enum.") : TEXT("This node has no native LogicMode enum property."));
					OutError.bRetrySafe = false;
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Spec, TEXT("logic"));
					return ForwardAt(OutError, NodePath(Pair.Key));
				}
				Property->SetPropertyValue_InContainer(Node, static_cast<uint8>(Mode));
			}
			else if (bNew && !bNativeCreate)
			{
				UClass* PayloadClass = ResolveClass(Spec->GetStringField(TEXT("payload_class")), UBTDecorator::StaticClass());
				FStructProperty* Property = FindFProperty<FStructProperty>(Node->GetClass(), TEXT("ClassData"));
				if (!Property || Property->Struct != FGraphNodeClassData::StaticStruct())
					return NativeFail(OutError, TEXT("behavior_tree_owned_class_data_missing"), TEXT("The decorator node has no compatible native ClassData property."), Pair.Key);
				const FGraphNodeClassData Data(PayloadClass, FString());
				Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Node), &Data);
			}
			if (bNew && !bNativeCreate)
			{
				Node->PostPlacedNewNode();
				Node->AllocateDefaultPins();
			}
			if (!Logic.IsEmpty())
			{
				TArray<UEdGraphPin*> Inputs;
				for (UEdGraphPin* Pin : Node->Pins) if (Pin && Pin->Direction == EGPD_Input) Inputs.Add(Pin);
				const int32 Count = Spec->GetIntegerField(TEXT("input_count"));
				if (Inputs.IsEmpty()) return NativeFail(OutError, TEXT("behavior_tree_owned_input_template_missing"), TEXT("The native logic node has no input pin to use as a pin template."));
				// The native logic node's non-exported AddInputPin/RemoveInputPin use
				// these public graph primitives. Keep the native allocated pin type/name.
				while (Inputs.Num() < Count)
					Inputs.Add(Node->CreatePin(EGPD_Input, Inputs[0]->PinType, Inputs[0]->PinName));
				while (Inputs.Num() > Count) Node->RemovePin(Inputs.Pop(EAllowShrinking::No));
			}
			const auto Position = Spec->GetObjectField(TEXT("pos"));
			Node->NodePosX = Position->GetIntegerField(TEXT("x"));
			Node->NodePosY = Position->GetIntegerField(TEXT("y"));
			UObject* PropertyObject = const_cast<UObject*>(GetNodePropertyObject(*Node));
			if (Logic.IsEmpty() && !PropertyObject) return NativeFail(OutError, TEXT("behavior_tree_owned_payload_missing"), TEXT("The native decorator node has no property payload object."), Pair.Key);
			if (!PropertyObject && (Spec->HasField(TEXT("properties")) || Spec->HasField(TEXT("property_changes"))))
				return NativeFail(OutError, TEXT("behavior_tree_properties_target_missing"), TEXT("The node has property edits but no native object that owns those properties."), Pair.Key);
			if (PropertyObject)
			{
				if (PropertyObject->GetClass()->GetPathName() != Spec->GetStringField(TEXT("payload_class")))
					return NativeFail(OutError, TEXT("behavior_tree_owned_payload_type_changed"), TEXT("The native decorator payload class differs from the requested payload class."), Pair.Key);
				UeAgentAuthoringObjectApply::FOptions Options;
				Options.bAllowDomainAuthorizedOwners = true;
				UeAgentAuthoringDirectApply::FResult Result;
				TSharedPtr<FJsonObject> Properties, Changes;
				if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Spec, TEXT("properties"), Properties, OutError))
					return ForwardAt(OutError, NodePath(Pair.Key) + TEXT("/properties"));
				if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Spec, TEXT("property_changes"), Changes, OutError))
					return ForwardAt(OutError, NodePath(Pair.Key) + TEXT("/property_changes"));
				if (Properties)
				{
					if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(PropertyObject, Properties, Options, Result)) { OutError = Result.Diagnostic; return ForwardAt(OutError, NodePath(Pair.Key) + TEXT("/properties")); }
					if (OutPropertyIntents) UeAgentDetailsAuthoringService::AppendScopedObjectPropertyIntents(
						Result.PropertyIntents, NodePropertySemanticId, TEXT("node_id"), Pair.Key, *OutPropertyIntents,
						{{TEXT("graph_name"), GraphId}});
				}
				Options.bCompactValues = true;
				if (Changes)
				{
					if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(PropertyObject, Changes, Options, Result)) { OutError = Result.Diagnostic; return ForwardAt(OutError, NodePath(Pair.Key) + TEXT("/property_changes")); }
					if (OutPropertyIntents) UeAgentDetailsAuthoringService::AppendScopedObjectPropertyIntents(
						Result.PropertyIntents, NodePropertySemanticId, TEXT("node_id"), Pair.Key, *OutPropertyIntents,
						{{TEXT("graph_name"), GraphId}});
				}
			}
		}
		UeAgentAuthoringGraphLinks::FIntent ConnectionIntent;
		int32 ConnectionIndex = 0;
		for (const auto& Edge : Document->GetArrayField(TEXT("connections")))
		{
			const auto From = Edge->AsObject()->GetObjectField(TEXT("from"));
			const auto To = Edge->AsObject()->GetObjectField(TEXT("to"));
			UEdGraphPin* Source = ResolvePin(Nodes.FindRef(From->GetStringField(TEXT("node"))), From, EGPD_Output, OutError);
			if (!Source) return ForwardAt(OutError, TEXT("/connections/") + FString::FromInt(ConnectionIndex) + TEXT("/from"));
			UEdGraphPin* Target = ResolvePin(Nodes.FindRef(To->GetStringField(TEXT("node"))), To, EGPD_Input, OutError);
			if (!Target) return ForwardAt(OutError, TEXT("/connections/") + FString::FromInt(ConnectionIndex) + TEXT("/to"));
			if (!Graph->GetSchema()) return NativeFail(OutError, TEXT("behavior_tree_owned_connection_rejected"), TEXT("The native graph has no schema for connecting pins."));
			if (!Graph->GetSchema()->TryCreateConnection(Source, Target))
				return Fail(OutError, TEXT("behavior_tree_owned_connection_rejected"), TEXT("The native graph schema rejected the connection. ")
					+ Graph->GetSchema()->CanCreateConnection(Source, Target).Message.ToString(), TEXT("/connections/") + FString::FromInt(ConnectionIndex));
			ConnectionIntent.Expect(Source, Target, true, TEXT("/connections/") + FString::FromInt(ConnectionIndex++));
		}
		FString Guid;
		if (Document->TryGetStringField(TEXT("guid"), Guid)) FGuid::Parse(Guid, Graph->GraphGuid);
		Owner.OnInnerGraphChanged();
		ConnectionIntent.Observe();
		return true;
	}

	bool ApplyGraphDocument(UBehaviorTree& Tree, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError)
	{
		return ApplyGraphDocument(Tree, Value, OutError, nullptr);
	}

	bool ApplyGraphDocument(UBehaviorTree& Tree, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError,
		TArray<UeAgentDetailsAuthoringService::FScopedPropertyIntent>* OutPropertyIntents)
	{
		if (OutPropertyIntents) OutPropertyIntents->Reset();
		if (!ValidateGraphDocument(Value, OutError)) return false;
		const auto Document = Value->AsObject();
		UBlackboardData* DesiredBlackboard = Tree.BlackboardAsset;
		FString Blackboard;
		if (Document->TryGetStringField(TEXT("blackboard"), Blackboard))
		{
			DesiredBlackboard = Blackboard.IsEmpty() ? nullptr : LoadObject<UBlackboardData>(nullptr, *Blackboard);
			if (!Blackboard.IsEmpty() && !DesiredBlackboard) return Fail(OutError, TEXT("behavior_tree_graph_blackboard_unresolved"), TEXT("The blackboard reference cannot be loaded as a Blackboard Data asset."), TEXT("/blackboard"), MakeShared<FJsonValueString>(Blackboard), MakeShared<FJsonValueString>(TEXT("loadable Blackboard Data asset reference")));
		}
		// Headless loads do not receive the editor's initialization that hydrates
		// subtree preview caches. The requested binding is published after native
		// structure creation, before selectors and dependent properties are applied.
		if (auto* Graph = Cast<UBehaviorTreeGraph>(Tree.BTGraph)) Graph->Initialize();
		const auto Desired = Document->GetObjectField(TEXT("nodes"));
		FGraphCapture Before;
		if (!CaptureGraph(Tree, Before, OutError)) return false;
		TMap<FString, UBehaviorTreeGraphNode*> Nodes;
		for (const FNodeCapture& Captured : Before.Nodes)
			Nodes.Add(Captured.Id, const_cast<UBehaviorTreeGraphNode*>(Cast<UBehaviorTreeGraphNode>(Captured.NativeNode)));
		TArray<FMutation> Creates;
		for (const auto& Pair : Desired->Values)
		{
			const auto Node = Pair.Value->AsObject();
			UClass* NodeClass = ResolveClass(Node->GetStringField(TEXT("node_class")), UBehaviorTreeGraphNode::StaticClass());
			UClass* PayloadClass = Pair.Key == TEXT("node:root") ? nullptr
				: ResolveClass(Node->GetStringField(TEXT("payload_class")), UBTNode::StaticClass());
			if (UBehaviorTreeGraphNode* const* Existing = Nodes.Find(Pair.Key))
			{
				if (!*Existing || (*Existing)->GetClass() != NodeClass
					|| (PayloadClass && (!(*Existing)->NodeInstance || (*Existing)->NodeInstance->GetClass() != PayloadClass)))
					return NativeFail(OutError, TEXT("behavior_tree_graph_existing_type_changed"), TEXT("An existing graph node or payload no longer has the requested class."), Pair.Key);
				continue;
			}
			FMutation& Create = Creates.AddDefaulted_GetRef();
			Create.Identity = Pair.Key;
			Node->TryGetStringField(TEXT("parent"), Create.ParentId);
			Create.Kind = Pair.Key == TEXT("node:root") ? EMutationKind::Root
				: Create.ParentId.IsEmpty() ? EMutationKind::Node : EMutationKind::Attachment;
			Create.Node.GraphNodeClass = NodeClass;
			Create.Node.PayloadClass = PayloadClass;
			Node->TryGetBoolField(TEXT("injected"), Create.Node.bInjected);
			FString NativeGuid;
			if (!Node->TryGetStringField(TEXT("native_guid"), NativeGuid)) NativeGuid = Pair.Key.Mid(5);
			FGuid::Parse(NativeGuid, Create.Node.NodeGuid);
			Node->TryGetStringField(TEXT("candidate_name"), Create.Node.CandidateName);
			Node->TryGetStringField(TEXT("candidate_id"), Create.Node.CandidateId);
			const TSharedPtr<FJsonObject>* Select = nullptr;
			if (Node->TryGetObjectField(TEXT("candidate_select"), Select) && Select && Select->IsValid())
			{
				for (const auto& Field : (*Select)->Values)
				{
					FString SelectionValue;
					if (!Field.Value.IsValid() || !Field.Value->TryGetString(SelectionValue))
						return Fail(OutError, TEXT("behavior_tree_graph_candidate_select_invalid"), TEXT("Each candidate selection value must be a string."), NodePath(Pair.Key) + TEXT("/candidate_select/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Field.Key), Field.Value, MakeShared<FJsonValueString>(TEXT("string")));
					Create.Node.CandidateSelect.Add(Field.Key, SelectionValue);
				}
			}
			const auto Pos = Node->GetObjectField(TEXT("pos"));
			Create.Node.Location = FVector2f(Pos->GetNumberField(TEXT("x")), Pos->GetNumberField(TEXT("y")));
		}
		FString Revision;
		if (!Creates.IsEmpty() && !ApplyMutations(Tree, Creates, Revision, OutError, false))
			return false;
		FGraphCapture Current;
		if (!CaptureGraph(Tree, Current, OutError)) return false;
		Nodes.Reset();
		TMap<FString, FString> CurrentParents;
		for (const FNodeCapture& Captured : Current.Nodes)
		{
			Nodes.Add(Captured.Id, const_cast<UBehaviorTreeGraphNode*>(Cast<UBehaviorTreeGraphNode>(Captured.NativeNode)));
			CurrentParents.Add(Captured.Id, Captured.ParentId);
		}
		for (const FNodeCapture& Captured : Current.Nodes)
		{
			UBehaviorTreeGraphNode* Node = Nodes.FindChecked(Captured.Id);
			if (!Node) return NativeFail(OutError, TEXT("behavior_tree_graph_node_unsupported"), TEXT("The captured node is not a supported native Behavior Tree graph node."), Captured.Id);
			if (!Desired->HasField(Captured.Id))
			{
				Node->Modify();
				if (const auto* Composite = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Node))
					if (Tree.BTGraph) Tree.BTGraph->SubGraphs.Remove(Composite->BoundGraph);
				if (auto* Parent = Nodes.FindRef(Captured.ParentId)) Parent->RemoveSubNode(Node);
				Node->ParentNode = nullptr;
				Node->DestroyNode();
				Nodes.Remove(Captured.Id);
			}
		}
		// Root placement can select an unrelated loaded Blackboard. Publish the
		// authored binding after structural defaults, and synchronize the Root
		// projection so later native editor notifications cannot restore an old one.
		BindBlackboard(Tree, DesiredBlackboard);
		for (const auto& Pair : Desired->Values)
		{
			UBehaviorTreeGraphNode* Node = Nodes.FindRef(Pair.Key);
			if (!Node) return NativeFail(OutError, TEXT("behavior_tree_graph_node_missing"), TEXT("A desired Behavior Tree node is missing after native graph reconstruction."), Pair.Key);
			const auto Spec = Pair.Value->AsObject();
			FString ParentId;
			Spec->TryGetStringField(TEXT("parent"), ParentId);
			UBehaviorTreeGraphNode* Parent = ParentId.IsEmpty() ? nullptr : Nodes.FindRef(ParentId);
			UBehaviorTreeGraphNode* OldParent = Nodes.FindRef(CurrentParents.FindRef(Pair.Key));
			if (OldParent != Parent)
			{
				Node->Modify();
				if (OldParent) OldParent->RemoveSubNode(Node);
				if (Parent)
				{
					Parent->Modify();
					Parent->InsertSubNodeAt(Node, 0);
				}
			}
			Node->ParentNode = Parent;
			Node->Modify();
			bool bInjected = false;
			Spec->TryGetBoolField(TEXT("injected"), bInjected);
			Node->bInjectedNode = bInjected;
			if (bInjected) Node->bIsReadOnly = true;
			const auto Pos = Spec->GetObjectField(TEXT("pos"));
			Node->NodePosX = Pos->GetIntegerField(TEXT("x"));
			Node->NodePosY = Pos->GetIntegerField(TEXT("y"));
			UObject* PropertyObject = const_cast<UObject*>(GetNodePropertyObject(*Node));
			if (!PropertyObject && (Spec->HasField(TEXT("properties")) || Spec->HasField(TEXT("property_changes"))))
				return NativeFail(OutError, TEXT("behavior_tree_graph_properties_target_missing"), TEXT("The node has property edits but no native object that owns those properties."), Pair.Key);
			const TSharedPtr<FJsonObject>* SnapshotProperties = nullptr;
			if (PropertyObject && Spec->TryGetObjectField(TEXT("properties"), SnapshotProperties))
			{
				UeAgentAuthoringObjectApply::FOptions Options;
				Options.bAllowDomainAuthorizedOwners = true;
				UeAgentAuthoringDirectApply::FResult Result;
				if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(PropertyObject, *SnapshotProperties, Options, Result)) { OutError = Result.Diagnostic; return ForwardAt(OutError, NodePath(Pair.Key) + TEXT("/properties")); }
				if (OutPropertyIntents) UeAgentDetailsAuthoringService::AppendScopedObjectPropertyIntents(
					Result.PropertyIntents, NodePropertySemanticId, TEXT("node_id"), Pair.Key, *OutPropertyIntents,
					{{TEXT("graph_name"), TEXT("main")}});
			}
			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if (Spec->TryGetArrayField(TEXT("property_changes"), Properties) && Properties && !Properties->IsEmpty())
			{
				if (!PropertyObject) return NativeFail(OutError, TEXT("behavior_tree_graph_properties_target_missing"), TEXT("The node has property edits but no native object that owns those properties."), Pair.Key);
				UeAgentAuthoringObjectApply::FOptions Options;
				Options.bAllowDomainAuthorizedOwners = true;
				Options.bCompactValues = true;
				UeAgentAuthoringDirectApply::FResult Result;
				if (!UeAgentAuthoringDirectApply::ApplyObjectPatch(PropertyObject, *Properties, Options, Result))
					{ OutError = MoveTemp(Result.Diagnostic); return ForwardAt(OutError, NodePath(Pair.Key) + TEXT("/property_changes")); }
				if (OutPropertyIntents) UeAgentDetailsAuthoringService::AppendScopedObjectPropertyIntents(
					Result.PropertyIntents, NodePropertySemanticId, TEXT("node_id"), Pair.Key, *OutPropertyIntents,
					{{TEXT("graph_name"), TEXT("main")}});
			}
		}
		// The native generic list and typed execution lists have independent ordering.
		for (const auto& Pair : Nodes)
		{
			UBehaviorTreeGraphNode* Parent = Pair.Value;
			if (Parent->SubNodes.IsEmpty()) continue;
			Parent->Modify();
			auto OrderOf = [&](const UAIGraphNode& Child, const TCHAR* Field) {
				FString Id;
				for (const auto& Candidate : Nodes) if (Candidate.Value == &Child) { Id = Candidate.Key; break; }
				double Order = MAX_int32;
				const TSharedPtr<FJsonObject>* Spec = nullptr;
				if (Desired->TryGetObjectField(Id, Spec) && Spec && Spec->IsValid()) (*Spec)->TryGetNumberField(Field, Order);
				return Order;
			};
			Parent->SubNodes.StableSort([&](const TObjectPtr<UAIGraphNode>& Left, const TObjectPtr<UAIGraphNode>& Right) {
				return OrderOf(*Left, TEXT("subnode_order")) < OrderOf(*Right, TEXT("subnode_order"));
			});
			Parent->Decorators.StableSort([&](const TObjectPtr<UBehaviorTreeGraphNode>& Left, const TObjectPtr<UBehaviorTreeGraphNode>& Right) {
				return OrderOf(*Left, TEXT("order")) < OrderOf(*Right, TEXT("order"));
			});
			Parent->Services.StableSort([&](const TObjectPtr<UBehaviorTreeGraphNode>& Left, const TObjectPtr<UBehaviorTreeGraphNode>& Right) {
				return OrderOf(*Left, TEXT("order")) < OrderOf(*Right, TEXT("order"));
			});
		}
		for (const auto& Pair : Nodes)
			for (UEdGraphPin* Pin : Pair.Value->Pins) if (Pin) Pin->BreakAllPinLinks();
		UeAgentAuthoringGraphLinks::FIntent ConnectionIntent;
		int32 ConnectionIndex = 0;
		for (const auto& Edge : Document->GetArrayField(TEXT("connections")))
		{
			const auto From = Edge->AsObject()->GetObjectField(TEXT("from"));
			const auto To = Edge->AsObject()->GetObjectField(TEXT("to"));
			UEdGraphPin* Source = ResolvePin(Nodes.FindRef(From->GetStringField(TEXT("node"))), From, EGPD_Output, OutError);
			if (!Source) return ForwardAt(OutError, TEXT("/connections/") + FString::FromInt(ConnectionIndex) + TEXT("/from"));
			UEdGraphPin* Target = ResolvePin(Nodes.FindRef(To->GetStringField(TEXT("node"))), To, EGPD_Input, OutError);
			if (!Target) return ForwardAt(OutError, TEXT("/connections/") + FString::FromInt(ConnectionIndex) + TEXT("/to"));
			const UEdGraphSchema* Schema = Tree.BTGraph ? Tree.BTGraph->GetSchema() : nullptr;
			if (!Schema) return NativeFail(OutError, TEXT("behavior_tree_graph_connection_rejected"), TEXT("The native graph has no schema for connecting pins."));
			if (!Schema->TryCreateConnection(Source, Target))
				return Fail(OutError, TEXT("behavior_tree_graph_connection_rejected"), TEXT("The native graph schema rejected the connection. ")
					+ Schema->CanCreateConnection(Source, Target).Message.ToString(), TEXT("/connections/") + FString::FromInt(ConnectionIndex));
			ConnectionIntent.Expect(Source, Target, true, TEXT("/connections/") + FString::FromInt(ConnectionIndex++));
		}
		const TSharedPtr<FJsonObject>* Owned = nullptr;
		if (Document->TryGetObjectField(TEXT("owned_graphs"), Owned))
			for (const auto& Pair : (*Owned)->Values)
			{
				const auto Spec = Pair.Value->AsObject();
				auto* Owner = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Nodes.FindRef(Spec->GetStringField(TEXT("owner"))));
				const bool bCreatedOwner = Creates.ContainsByPredicate([&](const FMutation& Create) {
					return !Create.Node.CandidateName.IsEmpty() && Create.Identity == Spec->GetStringField(TEXT("owner")); });
				if (Owner && bCreatedOwner)
				{
					const auto DefaultNodes = Spec->GetObjectField(TEXT("nodes"));
					if (!Owner->BoundGraph || Owner->BoundGraph->Nodes.Num() != 1 || DefaultNodes->Values.Num() != 1)
						return NativeFail(OutError, TEXT("behavior_tree_owned_default_graph_mismatch"), TEXT("The newly created decorator graph must contain exactly one default sink node."), Pair.Key);
					const auto Default = DefaultNodes->Values.CreateConstIterator();
					UEdGraphNode* Native = Owner->BoundGraph->Nodes[0];
					const auto* Logic = Native ? FindFProperty<FByteProperty>(Native->GetClass(), TEXT("LogicMode")) : nullptr;
					if (!Logic || !Logic->Enum || Logic->Enum->GetNameStringByValue(Logic->GetPropertyValue_InContainer(Native)) != TEXT("Sink")
						|| Default.Value()->AsObject()->GetStringField(TEXT("logic")) != TEXT("Sink"))
						return NativeFail(OutError, TEXT("behavior_tree_owned_default_graph_mismatch"), TEXT("The newly created decorator graph must contain exactly one default sink node."), Pair.Key);
					FGuid::Parse(Default.Key().Mid(5), Native->NodeGuid);
				}
				if (!Owner) return NativeFail(OutError, TEXT("behavior_tree_owned_graph_owner_missing"), TEXT("The desired decorator graph has no compatible native owner."), Pair.Key);
				if (!ApplyOwnedGraphDocument(Tree, *Owner, Pair.Key, Spec, OutError, OutPropertyIntents))
					return ForwardAt(OutError, TEXT("/owned_graphs/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key));
			}
		if (!Nodes.IsEmpty() && !SettleGraph(Tree, Revision, OutError)) return false;
		if (Nodes.IsEmpty()) Tree.RootNode = nullptr;
		ConnectionIntent.Observe();
		Tree.MarkPackageDirty();
		return true;
	}
}
