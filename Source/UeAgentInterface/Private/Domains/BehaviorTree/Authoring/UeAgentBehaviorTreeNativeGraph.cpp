#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeNativeGraph.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "DomainAdapters/PcgAiNodeGraph/UeAgentPcgAiNodeGraphNativeHook.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAIGraphOwnership.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringGraphLinks.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UeAgentBehaviorTreeAuthoring
{
	namespace
	{
		FString NodeId(const UEdGraphNode& Node)
		{
			return Node.IsA<UBehaviorTreeGraphNode_Root>() ? TEXT("node:root")
				: TEXT("node:") + Node.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		}

		UE::UAI::Contract::FDiagnostic NativeFailure(const TCHAR* Code, const TCHAR* Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const TCHAR* Stage = TEXT("behavior_tree_capture"))
		{
			auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path, Expected, Actual, Stage, Reason);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.bRetrySafe = false;
			return Diagnostic;
		}

		TSharedPtr<FJsonValue> ObjectType(const UObject* Object)
		{
			if (Object) return MakeShared<FJsonValueString>(Object->GetClass()->GetPathName());
			return MakeShared<FJsonValueNull>();
		}

		TSharedPtr<FJsonValue> ConnectionFact(const FString& FromId, int32 FromIndex, const FString& ToId, int32 ToIndex)
		{
			auto Fact = MakeShared<FJsonObject>();
			Fact->SetStringField(TEXT("source_node"), FromId);
			Fact->SetNumberField(TEXT("source_pin"), FromIndex);
			Fact->SetStringField(TEXT("target_node"), ToId);
			Fact->SetNumberField(TEXT("target_pin"), ToIndex);
			return MakeShared<FJsonValueObject>(Fact);
		}

		UEdGraphNode* FindNode(UBehaviorTreeGraph& Graph, const FString& Id)
		{
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (!Node) continue;
				if (NodeId(*Node) == Id) return Node;
			}
			return nullptr;
		}

		UEdGraphPin* FindPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, int32 DirectionIndex)
		{
			if (!Node) return nullptr;
			int32 Index = 0;
			for (UEdGraphPin* Pin : Node->Pins)
				if (Pin && Pin->Direction == Direction && Index++ == DirectionIndex) return Pin;
			return nullptr;
		}
	}

	void BindBlackboard(UBehaviorTree& Tree, UBlackboardData* Blackboard)
	{
		Tree.Modify();
		Tree.BlackboardAsset = Blackboard;
		if (auto* Graph = Cast<UBehaviorTreeGraph>(Tree.BTGraph))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
				if (auto* Root = Cast<UBehaviorTreeGraphNode_Root>(Node))
				{
					Root->Modify();
					Root->BlackboardAsset = Blackboard;
				}
			Graph->UpdateBlackboardChange();
		}
	}

	bool ApplyMutations(UBehaviorTree& Tree, const TArray<FMutation>& Mutations,
		FString& OutRevision, UE::UAI::Contract::FDiagnostic& OutError, const bool bSettle)
	{
		OutRevision.Reset();
		OutError.Reset();
		UBlackboardData* DesiredBlackboard = Tree.BlackboardAsset;
		bool bExplicitBlackboard = false;
		auto Fail = [&](UE::UAI::Contract::FDiagnostic Diagnostic) { OutError = MoveTemp(Diagnostic); return false; };
		UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Tree.BTGraph);
		if (!Graph)
		{
			Graph = NewObject<UBehaviorTreeGraph>(&Tree, TEXT("BehaviorTreeGraph"), RF_Transactional);
			if (!Graph) return Fail(NativeFailure(TEXT("graph_compound_behavior_tree_graph_create_failed"), TEXT("Unreal did not create the behavior tree graph."),
				TEXT("/graph"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(UBehaviorTreeGraph::StaticClass()->GetPathName()), TEXT("behavior_tree_apply")));
			Tree.BTGraph = Graph;
			Graph->OnCreated();
			Graph->Initialize();
		}
		for (const FMutation& Mutation : Mutations)
		{
			if (Mutation.Kind == EMutationKind::Root)
			{
				auto* Node = NewObject<UBehaviorTreeGraphNode_Root>(Graph);
				Node->CreateNewGuid();
				Node->PostPlacedNewNode();
				Node->AllocateDefaultPins();
				Graph->AddNode(Node, false, false);
				// A new Root chooses an editor default in PostPlacedNewNode. It
				// must not replace an existing or explicitly requested binding.
				if (!DesiredBlackboard && !bExplicitBlackboard) DesiredBlackboard = Tree.BlackboardAsset;
			}
			else if (Mutation.Kind == EMutationKind::Node)
			{
				UBehaviorTreeGraphNode* Node = nullptr;
				if (!CreateNode(Tree, *Graph, Mutation.Node, Node, OutError)) return false;
			}
			else if (Mutation.Kind == EMutationKind::BlackboardBinding)
			{
				auto* Blackboard = LoadObject<UBlackboardData>(nullptr, *Mutation.ObjectRef);
				if (!Blackboard) return Fail(NativeFailure(TEXT("graph_compound_behavior_tree_blackboard_unresolved"), TEXT("The blackboard binding could not be loaded as a BlackboardData asset."),
					TEXT("/blackboard"), MakeShared<FJsonValueString>(Mutation.ObjectRef), MakeShared<FJsonValueString>(UBlackboardData::StaticClass()->GetPathName()), TEXT("behavior_tree_apply")));
				DesiredBlackboard = Blackboard;
				bExplicitBlackboard = true;
			}
		}
		for (const FMutation& Mutation : Mutations)
		{
			if (Mutation.Kind != EMutationKind::Attachment) continue;
			auto* Owner = Cast<UBehaviorTreeGraphNode>(FindNode(*Graph, Mutation.ParentId));
			if (!Owner) return Fail(NativeFailure(TEXT("graph_compound_behavior_tree_subnode_owner_invalid"), TEXT("The attachment parent does not resolve to a behavior tree graph node."),
				TEXT("/parent"), MakeShared<FJsonValueString>(Mutation.ParentId), MakeShared<FJsonValueString>(TEXT("existing behavior tree graph node")), TEXT("behavior_tree_apply")));
			UBehaviorTreeGraphNode* Node = nullptr;
			if (!Mutation.Node.CandidateName.IsEmpty() || !Mutation.Node.CandidateId.IsEmpty())
			{
				using namespace UE::UAI::QueryOwners::AiBehavior;
				FAiBehaviorCandidate Candidate;
				FAiBehaviorCandidateQueryError Error;
				const FAiBehaviorCandidateContext Context{Owner,
					Mutation.Node.PayloadClass && Mutation.Node.PayloadClass->IsChildOf<UBTService>() ? TEXT("service") : TEXT("decorator")};
				UEdGraphNode* Created = nullptr;
				if (!TryPerformAiGraphCandidate(Tree, Mutation.Node.CandidateName, Mutation.Node.CandidateId,
					Mutation.Node.Location, Created, Candidate, Error, Mutation.Node.CandidateSelect, Context))
					return Fail(Error);
				Node = Cast<UBehaviorTreeGraphNode>(Created);
				if (!Node) return Fail(NativeFailure(TEXT("graph_compound_behavior_tree_subnode_create_failed"), TEXT("The candidate action did not return a behavior tree graph node."),
					TEXT("/attachment/node_class"), ObjectType(Created), MakeShared<FJsonValueString>(UBehaviorTreeGraphNode::StaticClass()->GetPathName()), TEXT("behavior_tree_apply")));
			}
			else
			{
				// Complete typed snapshots also restore objects no longer present in the current menu.
				Node = NewObject<UBehaviorTreeGraphNode>(Graph, Mutation.Node.GraphNodeClass);
				if (Mutation.Node.PayloadClass)
					Node->NodeInstance = NewObject<UBTNode>(&Tree, Mutation.Node.PayloadClass, NAME_None, RF_Transactional);
				if (!Node->NodeInstance && !Node->IsA<UBehaviorTreeGraphNode_CompositeDecorator>())
					return Fail(NativeFailure(TEXT("graph_compound_behavior_tree_subnode_create_failed"), TEXT("The attachment node has no native payload."),
						TEXT("/attachment/payload_type"), MakeShared<FJsonValueNull>(),
						MakeShared<FJsonValueString>(Mutation.Node.PayloadClass ? Mutation.Node.PayloadClass->GetPathName() : TEXT("BTNode payload")), TEXT("behavior_tree_apply")));
				Owner->AddSubNode(Node, Graph);
			}
			Node->NodeGuid = Mutation.Node.NodeGuid;
			Node->bInjectedNode = Mutation.Node.bInjected;
			if (Mutation.Node.bInjected) Node->bIsReadOnly = true;
		}
		// Root placement and attachment creation must finish before the single
		// binding is published to both native representations.
		BindBlackboard(Tree, DesiredBlackboard);
		UeAgentAuthoringGraphLinks::FIntent ConnectionIntent;
		for (const FMutation& Mutation : Mutations)
		{
			if (Mutation.Kind != EMutationKind::Connection) continue;
			UEdGraphPin* From = FindPin(FindNode(*Graph, Mutation.FromId), EGPD_Output, Mutation.FromIndex);
			UEdGraphPin* To = FindPin(FindNode(*Graph, Mutation.ToId), EGPD_Input, Mutation.ToIndex);
			if (!From || !To) return Fail(NativeFailure(TEXT("graph_compound_behavior_tree_pin_invalid"),
				!From ? TEXT("The source output pin could not be resolved.") : TEXT("The target input pin could not be resolved."),
				TEXT("/connection"), ConnectionFact(Mutation.FromId, Mutation.FromIndex, Mutation.ToId, Mutation.ToIndex),
				MakeShared<FJsonValueString>(TEXT("existing source output and target input pins")), TEXT("behavior_tree_apply")));
			const UEdGraphSchema* Schema = Graph->GetSchema();
			if (!Schema || !Schema->TryCreateConnection(From, To))
				return Fail(NativeFailure(TEXT("graph_compound_behavior_tree_connection_rejected"),
					!Schema ? TEXT("The behavior tree graph has no schema for creating a connection.") : TEXT("The native graph schema rejected the connection."),
					TEXT("/connection"), ConnectionFact(Mutation.FromId, Mutation.FromIndex, Mutation.ToId, Mutation.ToIndex),
					MakeShared<FJsonValueString>(TEXT("connection accepted by the native graph schema")), TEXT("behavior_tree_apply")));
			ConnectionIntent.Expect(From, To, true, FString());
		}
		if (bSettle && !SettleGraph(Tree, OutRevision, OutError)) return false;
		ConnectionIntent.Observe();
		return true;
	}

	bool SettleGraph(UBehaviorTree& Tree, FString& OutRevision, UE::UAI::Contract::FDiagnostic& OutError)
	{
		using namespace UE::UAI::DomainAdapters::PcgAiNodeGraph;
		// Match the editor's BehaviorAsset/save refresh before rebuilding execution data.
		// Reflection applies this to its isolated proposed tree before the live lifecycle.
		if (auto* Graph = Cast<UBehaviorTreeGraph>(Tree.BTGraph)) Graph->UpdateInjectedNodes();
		FPcgAiNodeGraphNativeHook NativeHook;
		FPcgAiNodeGraphNativeMutationReceipt ApplyReceipt;
		FPcgAiNodeGraphBarrierReceipt BarrierReceipt;
		FPcgAiNodeGraphTopology Topology;
		FString Evidence;
		FPcgAiNodeGraphAdapterError Error;
		const bool bCompiled = NativeHook.TryCompileAndCaptureBehaviorTree(Tree, ApplyReceipt, BarrierReceipt,
			Topology, OutRevision, Evidence, Error);
		if (!bCompiled || !BarrierReceipt.IsComplete() || BarrierReceipt.GetErrorCount() != 0)
		{
			if (!Error.IsEmpty()) OutError = Error;
			else
			{
				auto Fact = MakeShared<FJsonObject>();
				Fact->SetBoolField(TEXT("compile_succeeded"), bCompiled);
				Fact->SetBoolField(TEXT("complete"), BarrierReceipt.IsComplete());
				Fact->SetNumberField(TEXT("error_count"), BarrierReceipt.GetErrorCount());
				OutError = NativeFailure(TEXT("graph_compound_behavior_tree_compile_failed"),
					!bCompiled ? TEXT("The native compile-and-capture operation failed without a diagnostic.")
						: !BarrierReceipt.IsComplete() ? TEXT("The native compile barrier did not complete.") : TEXT("The native compile barrier reported errors."),
					TEXT("/compile"), MakeShared<FJsonValueObject>(Fact), MakeShared<FJsonValueString>(TEXT("successful compilation and complete barrier with zero errors")), TEXT("behavior_tree_apply"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Evidence);
			}
			return false;
		}
		return true;
	}

	const UObject* GetNodePropertyObject(const UEdGraphNode& Node)
	{
		if (Node.IsA<UBehaviorTreeGraphNode_CompositeDecorator>()) return &Node;
		// Inner decorator graph nodes are not exported C++ classes. Their reflected
		// NodeInstance is the same native property object used by the editor.
		const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Node.GetClass(), TEXT("NodeInstance"));
		return Property ? Property->GetObjectPropertyValue_InContainer(&Node) : nullptr;
	}

	bool CaptureNode(const UEdGraphNode& NativeNode, FNodeCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutCapture = {};
		OutError.Reset();
		const UEdGraphNode* Node = &NativeNode;
		OutCapture.NativeNode = Node;
		OutCapture.Id = NodeId(*Node);
		OutCapture.State = MakeShared<FJsonObject>();
		if (Node->IsA<UBehaviorTreeGraphNode_Root>()) OutCapture.State->SetStringField(TEXT("role"), TEXT("root"));
		OutCapture.State->SetStringField(TEXT("name"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		OutCapture.State->SetStringField(TEXT("node_class"), Node->GetClass()->GetPathName());
		const auto Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), Node->NodePosX);
		Position->SetNumberField(TEXT("y"), Node->NodePosY);
		OutCapture.State->SetObjectField(TEXT("pos"), Position);
		const UObject* Payload = GetNodePropertyObject(*Node);
		OutCapture.PayloadClass = Payload ? Payload->GetClass()->GetPathName() : Node->GetClass()->GetPathName();
		if (Node->GetGraph() && Node->GetGraph()->IsA<UBehaviorTreeDecoratorGraph>())
		{
			if (!Node->GetGraph()->bEditable) OutCapture.State->SetBoolField(TEXT("read_only"), true);
			if (const FByteProperty* Logic = FindFProperty<FByteProperty>(Node->GetClass(), TEXT("LogicMode")))
			{
				const uint8 Mode = Logic->GetPropertyValue_InContainer(Node);
				if (!Logic->Enum || !Logic->Enum->IsValidEnumValue(Mode))
				{
					OutError = NativeFailure(TEXT("behavior_tree_decorator_logic_invalid"),
						!Logic->Enum ? TEXT("The decorator LogicMode property has no enum definition.") : TEXT("The decorator LogicMode value is not declared by its enum."),
						TEXT("/logic"), MakeShared<FJsonValueNumber>(Mode), MakeShared<FJsonValueString>(Logic->Enum ? Logic->Enum->GetPathName() : TEXT("declared LogicMode enum value")));
					OutError.InternalContext = Node->GetPathName();
					return false;
				}
				OutCapture.State->SetStringField(TEXT("logic"), Logic->Enum->GetNameStringByValue(Mode));
			}
		}
		if (Payload)
		{
			UeAgentDetailsAuthoringService::FCapturePolicy Policy;
			UeAgentAuthoringState::FStateSnapshot PropertiesSnapshot;
			TSharedPtr<FJsonObject> Properties;
			if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(
				Payload, Policy, PropertiesSnapshot, Properties, OutError)) return false;
			PropertiesSnapshot.ReleaseNativeValues();
			OutCapture.State->SetObjectField(TEXT("properties"), Properties);
		}
		if (const UBehaviorTreeGraphNode* BehaviorNode = Cast<UBehaviorTreeGraphNode>(Node))
		{
			OutCapture.State->SetBoolField(TEXT("injected"), BehaviorNode->bInjectedNode != 0);
			const auto Ownership = UeAgentAuthoringAIGraphOwnership::Capture(*Node);
			if (Ownership.ConflictingParent)
			{
				auto Fact = MakeShared<FJsonObject>();
				Fact->SetStringField(TEXT("attachment"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
				Fact->SetStringField(TEXT("first_owner"), Ownership.Parent->GetNodeTitle(ENodeTitleType::ListView).ToString());
				Fact->SetStringField(TEXT("second_owner"), Ownership.ConflictingParent->GetNodeTitle(ENodeTitleType::ListView).ToString());
				OutError = NativeFailure(TEXT("behavior_tree_capture_attachment_owner_ambiguous"), TEXT("The same attachment is registered under more than one parent node."),
					TEXT("/attachment/parent"), MakeShared<FJsonValueObject>(Fact), MakeShared<FJsonValueString>(TEXT("one parent per attachment")));
				OutError.InternalContext = Node->GetPathName();
				return false;
			}
			const auto* Parent = Cast<UBehaviorTreeGraphNode>(Ownership.Parent);
			if (Parent)
			{
				OutCapture.ParentId = NodeId(*Parent);
				if (BehaviorNode->bInjectedNode)
				{
					// Native subtree previews retain the source Guid; ownership supplies its scope.
					OutCapture.Id = OutCapture.ParentId
						+ TEXT("/") + Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
					OutCapture.State->SetStringField(TEXT("native_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
					OutCapture.State->SetBoolField(TEXT("read_only"), true);
				}
				const int32 DecoratorIndex = Parent->Decorators.IndexOfByKey(BehaviorNode);
				const int32 ServiceIndex = Parent->Services.IndexOfByKey(BehaviorNode);
				if ((DecoratorIndex == INDEX_NONE) == (ServiceIndex == INDEX_NONE))
				{
					auto Fact = MakeShared<FJsonObject>();
					Fact->SetStringField(TEXT("attachment"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
					Fact->SetNumberField(TEXT("decorator_index"), DecoratorIndex);
					Fact->SetNumberField(TEXT("service_index"), ServiceIndex);
					OutError = NativeFailure(TEXT("behavior_tree_capture_attachment_membership_invalid"),
						DecoratorIndex == INDEX_NONE ? TEXT("The attachment belongs to neither the parent's decorator list nor its service list.")
							: TEXT("The attachment belongs to both the parent's decorator list and its service list."),
						TEXT("/attachment/membership"), MakeShared<FJsonValueObject>(Fact), MakeShared<FJsonValueString>(TEXT("membership in exactly one attachment list")));
					OutError.InternalContext = Node->GetPathName();
					return false;
				}
				OutCapture.State->SetStringField(TEXT("attachment_kind"), DecoratorIndex != INDEX_NONE ? TEXT("decorator") : TEXT("service"));
				OutCapture.State->SetNumberField(TEXT("order"), DecoratorIndex != INDEX_NONE ? DecoratorIndex : ServiceIndex);
			}
		}
		if (!OutCapture.ParentId.IsEmpty()) OutCapture.State->SetStringField(TEXT("parent"), OutCapture.ParentId);
		TArray<TSharedPtr<FJsonValue>> Pins;
		for (int32 Index = 0; Index < Node->Pins.Num(); ++Index)
		{
			const UEdGraphPin* Pin = Node->Pins[Index];
			if (!Pin) continue;
			const auto State = MakeShared<FJsonObject>();
			State->SetNumberField(TEXT("index"), Index);
			State->SetStringField(TEXT("name"), Pin->PinName.ToString());
			State->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			State->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
			Pins.Add(MakeShared<FJsonValueObject>(State));
		}
		OutCapture.State->SetArrayField(TEXT("pins"), Pins);
		return true;
	}

	bool CaptureGraphContents(const UEdGraph& NativeGraph, FGraphContents& OutCapture, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutCapture = {};
		OutError.Reset();
		const UEdGraph* Graph = &NativeGraph;

		struct FPendingNode { const UEdGraphNode* Node; FString ParentId; int32 Order; };
		TArray<FPendingNode> Pending;
		for (int32 Index = Graph->Nodes.Num() - 1; Index >= 0; --Index)
			Pending.Add({Graph->Nodes[Index], FString(), Index});
		TMap<const UEdGraphNode*, FString> IdByNode;
		TSet<FString> Ids;
		while (!Pending.IsEmpty())
		{
			const FPendingNode Current = Pending.Pop(EAllowShrinking::No);
			const UEdGraphNode* Node = Current.Node;
			if (!Node || IdByNode.Contains(Node))
			{
				auto Fact = MakeShared<FJsonObject>();
				Fact->SetStringField(TEXT("parent"), Current.ParentId);
				Fact->SetNumberField(TEXT("index"), Current.Order);
				Fact->SetField(TEXT("node_class"), ObjectType(Node));
				OutError = NativeFailure(TEXT("behavior_tree_capture_node_ownership_invalid"),
					!Node ? TEXT("The native graph node list contains a null entry.") : TEXT("The same native node is reachable through multiple ownership entries."),
					Current.ParentId.IsEmpty() ? TEXT("/nodes") : TEXT("/subnodes"), MakeShared<FJsonValueObject>(Fact), MakeShared<FJsonValueString>(TEXT("one nonnull node per ownership entry")));
				return false;
			}
			const bool bRoot = Node->IsA<UBehaviorTreeGraphNode_Root>();
			if (!bRoot && !Node->NodeGuid.IsValid())
			{
				OutError = NativeFailure(TEXT("behavior_tree_capture_node_identity_invalid"), TEXT("A non-root graph node has no valid native GUID."),
					TEXT("/node/identity"), ObjectType(Node), MakeShared<FJsonValueString>(TEXT("valid node GUID")));
				OutError.InternalContext = Node->GetPathName();
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Node->NodeGuid.ToString());
				return false;
			}
			FNodeCapture& Captured = OutCapture.Nodes.AddDefaulted_GetRef();
			if (!CaptureNode(*Node, Captured, OutError)) return false;
			const FString Id = Captured.Id;
			if (Ids.Contains(Id))
			{
				OutError = NativeFailure(TEXT("behavior_tree_capture_node_identity_duplicate"), TEXT("Two captured nodes resolve to the same document identity."),
					TEXT("/node/identity"), MakeShared<FJsonValueString>(Id), MakeShared<FJsonValueString>(TEXT("unique node identity")));
				return false;
			}
			Ids.Add(Id);
			IdByNode.Add(Node, Id);
			Captured.Order = Current.Order;
			const UAIGraphNode* AiNode = Cast<UAIGraphNode>(Node);
			if (AiNode)
				for (int32 Index = AiNode->SubNodes.Num() - 1; Index >= 0; --Index)
					Pending.Add({AiNode->SubNodes[Index], Id, Index});
		}
		for (const auto& Pair : IdByNode)
		{
			const UEdGraphNode* Node = Pair.Key;
			for (int32 Index = 0; Index < Node->Pins.Num(); ++Index)
			{
				const UEdGraphPin* Pin = Node->Pins[Index];
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					const FString* TargetId = Linked ? IdByNode.Find(Linked->GetOwningNode()) : nullptr;
					const int32 TargetIndex = Linked ? Linked->GetOwningNode()->Pins.IndexOfByKey(Linked) : INDEX_NONE;
					if (!TargetId || TargetIndex == INDEX_NONE)
					{
						OutError = NativeFailure(TEXT("behavior_tree_capture_connection_target_invalid"),
							!Linked ? TEXT("An output pin contains a null linked pin.")
								: !TargetId ? TEXT("The linked pin belongs to a node outside the captured graph.") : TEXT("The linked pin is absent from its owner's pin list."),
							TEXT("/connection"), ConnectionFact(Pair.Value, Index, TargetId ? *TargetId : FString(), TargetIndex),
							MakeShared<FJsonValueString>(TEXT("linked pin registered on a captured node")));
						return false;
					}
					FConnectionCapture& Connection = OutCapture.Connections.AddDefaulted_GetRef();
					Connection.Id = FString::Printf(TEXT("edge:%s:%d:%s:%d"), *Pair.Value, Index, **TargetId, TargetIndex);
					Connection.SourceId = Pair.Value;
					Connection.TargetId = *TargetId;
					Connection.State = MakeShared<FJsonObject>();
					Connection.State->SetNumberField(TEXT("source_pin"), Index);
					Connection.State->SetNumberField(TEXT("target_pin"), TargetIndex);
				}
			}
		}
		OutCapture.Connections.Sort([](const FConnectionCapture& A, const FConnectionCapture& B) { return A.Id < B.Id; });
		return true;
	}

	bool CaptureGraph(const UBehaviorTree& Tree, FGraphCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutCapture = {};
		OutError.Reset();
		const UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Tree.BTGraph);
		if (!Graph)
		{
			if (!Tree.RootNode) return true;
			OutError = NativeFailure(TEXT("behavior_tree_capture_graph_missing"), TEXT("The behavior tree has a runtime root node but no BehaviorTreeGraph for authoring capture."),
				TEXT("/graph"), ObjectType(Tree.BTGraph), MakeShared<FJsonValueString>(UBehaviorTreeGraph::StaticClass()->GetPathName()));
			return false;
		}
		if (!CaptureGraphContents(*Graph, OutCapture, OutError)) return false;
		TSet<const UEdGraph*> Seen;
		for (const FNodeCapture& Node : OutCapture.Nodes)
		{
			const auto* Composite = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Node.NativeNode);
			if (!Composite) continue;
			const UEdGraph* Bound = Composite->BoundGraph;
			int32 Registrations = 0;
			for (const UEdGraph* SubGraph : Graph->SubGraphs) Registrations += SubGraph == Bound;
			if (!Bound || Bound->GetOuter() != Composite || !Bound->IsA<UBehaviorTreeDecoratorGraph>()
				|| Seen.Contains(Bound) || Registrations != (Composite->bInjectedNode ? 0 : 1)
				|| (Composite->bInjectedNode && Bound->bEditable))
			{
				auto Fact = MakeShared<FJsonObject>();
				Fact->SetStringField(TEXT("owner"), Node.Id);
				Fact->SetField(TEXT("graph_class"), ObjectType(Bound));
				Fact->SetNumberField(TEXT("registration_count"), Registrations);
				Fact->SetBoolField(TEXT("injected"), Composite->bInjectedNode != 0);
				if (Bound) Fact->SetBoolField(TEXT("editable"), Bound->bEditable);
				const TCHAR* Reason = !Bound ? TEXT("The composite decorator has no bound graph.")
					: Bound->GetOuter() != Composite ? TEXT("The bound graph is owned by a different native object.")
					: !Bound->IsA<UBehaviorTreeDecoratorGraph>() ? TEXT("The bound graph is not a BehaviorTreeDecoratorGraph.")
					: Seen.Contains(Bound) ? TEXT("The bound graph is shared by multiple composite decorators.")
					: Registrations != (Composite->bInjectedNode ? 0 : 1) ? TEXT("The bound graph registration count does not match the owner's injected state.")
					: TEXT("An injected composite decorator's bound graph must be read-only.");
				OutError = NativeFailure(TEXT("behavior_tree_bound_graph_owner_invalid"), Reason, TEXT("/bound_graph"),
					MakeShared<FJsonValueObject>(Fact), MakeShared<FJsonValueString>(TEXT("uniquely owned decorator graph; injected: zero registrations and read-only; otherwise: one registration")));
				OutError.InternalContext = Composite->GetPathName();
				return false;
			}
			Seen.Add(Bound);
			FOwnedGraphCapture& Owned = OutCapture.OwnedGraphs.AddDefaulted_GetRef();
			Owned.Id = TEXT("decorator:") + Node.Id;
			Owned.OwnerId = Node.Id;
			Owned.NativeGraph = Bound;
			if (!CaptureGraphContents(*Bound, Owned, OutError)) return false;
		}
		return true;
	}

	bool CreateNode(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph,
		const FNodeCreateRequest& Request, UBehaviorTreeGraphNode*& OutNode,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutNode = nullptr;
		OutError.Reset();
		if (!Request.CandidateName.IsEmpty() || !Request.CandidateId.IsEmpty())
		{
			UE::UAI::QueryOwners::AiBehavior::FAiBehaviorCandidate Candidate;
			UE::UAI::QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError Error;
			UEdGraphNode* Created = nullptr;
			const bool bCreated = UE::UAI::QueryOwners::AiBehavior::TryPerformAiGraphCandidate(
				Tree, Request.CandidateName, Request.CandidateId, Request.Location,
				Created, Candidate, Error, Request.CandidateSelect);
			OutNode = Cast<UBehaviorTreeGraphNode>(Created);
			if (!bCreated || !OutNode || !OutNode->IsA(Request.GraphNodeClass)
				|| !OutNode->NodeInstance || OutNode->NodeInstance->GetClass() != Request.PayloadClass)
			{
				if (!Error.IsEmpty()) OutError = Error;
				else
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("action_succeeded"), bCreated);
					Actual->SetField(TEXT("node_class"), ObjectType(Created));
					Actual->SetField(TEXT("payload_type"), ObjectType(OutNode ? OutNode->NodeInstance.Get() : nullptr));
					auto Expected = MakeShared<FJsonObject>();
					Expected->SetStringField(TEXT("node_class"), Request.GraphNodeClass ? Request.GraphNodeClass->GetPathName() : FString());
					Expected->SetStringField(TEXT("payload_type"), Request.PayloadClass ? Request.PayloadClass->GetPathName() : FString());
					const TCHAR* Reason = !bCreated ? TEXT("The native candidate action failed without a diagnostic.")
						: !OutNode ? TEXT("The native candidate action did not return a behavior tree graph node.")
						: !OutNode->IsA(Request.GraphNodeClass) ? TEXT("The created graph node does not have the requested node class.")
						: !OutNode->NodeInstance ? TEXT("The created graph node has no payload instance.") : TEXT("The created payload class differs from the requested payload class.");
					OutError = NativeFailure(TEXT("graph_compound_behavior_tree_native_action_failed"), Reason,
						TEXT("/created_node"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), TEXT("behavior_tree_apply"));
					OutError.CandidateName = Request.CandidateName;
				}
				return false;
			}
			OutNode->Modify();
			OutNode->NodeGuid = Request.NodeGuid;
			return true;
		}

		OutNode = NewObject<UBehaviorTreeGraphNode>(&Graph, Request.GraphNodeClass);
		OutNode->NodeInstance = NewObject<UBTNode>(&Tree, Request.PayloadClass, NAME_None, RF_Transactional);
		if (!OutNode->NodeInstance)
		{
			OutError = NativeFailure(TEXT("graph_compound_behavior_tree_payload_create_failed"), TEXT("The Behavior Tree node payload could not be created."),
				TEXT("/created_node/payload_type"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(Request.PayloadClass ? Request.PayloadClass->GetPathName() : TEXT("BTNode payload")), TEXT("behavior_tree_apply"));
			return false;
		}
		OutNode->PostPlacedNewNode();
		OutNode->NodeGuid = Request.NodeGuid;
		OutNode->AllocateDefaultPins();
		Graph.AddNode(OutNode, false, false);
		return true;
	}
}
