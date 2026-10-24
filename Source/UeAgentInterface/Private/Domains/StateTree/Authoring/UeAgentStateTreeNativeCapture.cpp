#include "Domains/StateTree/Authoring/UeAgentStateTreeNativeCapture.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreeState.h"
#include "StateTreeSchema.h"
#include "StateTreeExecutionTypes.h"

namespace UeAgentStateTreeAuthoring
{
	namespace
	{
		FString NodeCollection(ENodePlacement Placement)
		{
			switch (Placement) {
			case ENodePlacement::Evaluator: return TEXT("Evaluators");
			case ENodePlacement::GlobalTask: return TEXT("GlobalTasks");
			case ENodePlacement::Task: return TEXT("Tasks");
			case ENodePlacement::SingleTask: return TEXT("SingleTask");
			case ENodePlacement::EnterCondition: return TEXT("EnterConditions");
			case ENodePlacement::Consideration: return TEXT("Considerations");
			case ENodePlacement::TransitionCondition: return TEXT("Transition.Conditions");
			case ENodePlacement::PropertyFunction: return TEXT("PropertyBindings.PropertyFunction");
			}
			return FString::FromInt(static_cast<int32>(Placement));
		}
		UE::UAI::Contract::FDiagnostic CaptureFailure(const TCHAR* Code, const FString& Reason,
			const UObject* Owner, const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("state_tree_capture"));
			Error.SourceKind = TEXT("native_result"); Error.PropertyPath = Path;
			if (Owner) Error.TargetPath = Owner->GetPathName();
			Error.ActualValuePresent = Actual.IsValid(); Error.ActualValue = MoveTemp(Actual); Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}
	}

	bool CaptureHierarchy(const UStateTreeEditorData& EditorData, TArray<FStateCapture>& OutStates, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutStates.Reset();
		OutError.Reset();
		TArray<FStateCapture> States, Pending;
		TSet<const UStateTreeState*> Visited;
		TSet<FGuid> Identities;
		for (int32 Index = EditorData.SubTrees.Num() - 1; Index >= 0; --Index)
			Pending.Add({EditorData.SubTrees[Index], nullptr, Index});
		while (!Pending.IsEmpty())
		{
			const FStateCapture Entry = Pending.Pop(EAllowShrinking::No);
			if (!Entry.State || Visited.Contains(Entry.State) || !Entry.State->ID.IsValid()
				|| Identities.Contains(Entry.State->ID))
			{
				OutError = CaptureFailure(TEXT("state_tree_hierarchy_invalid"),
					!Entry.State ? TEXT("The native hierarchy contains a null state.")
						: Visited.Contains(Entry.State) ? TEXT("The same native state occurs more than once in the hierarchy.")
						: !Entry.State->ID.IsValid() ? TEXT("The native state has no valid identity.")
						: TEXT("Different native states share the same identity."),
					&EditorData, Entry.Parent ? Entry.Parent->GetPathName(&EditorData) + FString::Printf(TEXT(".Children[%d]"), Entry.Order)
						: FString::Printf(TEXT("SubTrees[%d]"), Entry.Order),
					MakeShared<FJsonValueString>(Entry.State ? Entry.State->Name.ToString() : TEXT("null state")),
					MakeShared<FJsonValueString>(TEXT("non-null state with unique occurrence and valid unique identity")));
				if (Entry.State) OutError.SubjectId = Entry.State->ID.ToString(EGuidFormats::DigitsWithHyphensLower);
				return false;
			}
			Visited.Add(Entry.State);
			Identities.Add(Entry.State->ID);
			States.Add(Entry);
			for (int32 Index = Entry.State->Children.Num() - 1; Index >= 0; --Index)
				Pending.Add({Entry.State->Children[Index], Entry.State, Index});
		}
		OutStates = MoveTemp(States);
		return true;
	}

	bool CaptureTree(const UStateTreeEditorData& EditorData, FTreeCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutCapture = {};
		OutError.Reset();
		FTreeCapture Capture;
		Capture.EditorData = &EditorData;
		TSet<FGuid> Identities;
		auto AddIdentity = [&](const FGuid& Id, const TCHAR* Code, const UObject* Owner, const FString& Path) {
			if (!Id.IsValid() || Identities.Contains(Id)) {
				OutError = CaptureFailure(Code, !Id.IsValid() ? TEXT("The native item has no valid identity.")
					: TEXT("The native item reuses an identity already captured in this StateTree."),
					Owner, Path, MakeShared<FJsonValueString>(!Id.IsValid() ? TEXT("invalid identity") : TEXT("duplicate identity")),
					MakeShared<FJsonValueString>(TEXT("valid unique identity")));
				OutError.SubjectId = Id.ToString(EGuidFormats::DigitsWithHyphensLower); return false;
			}
			Identities.Add(Id);
			return true;
		};
		auto AddNode = [&](const FStateTreeEditorNode& Node, const FGuid& OwnerId,
			ENodePlacement Placement, int32 Order, const UObject* NotificationOwner) {
			if (!AddIdentity(Node.ID, TEXT("state_tree_node_identity_invalid"), NotificationOwner,
				NodeCollection(Placement) + FString::Printf(TEXT("[%d].ID"), Order))) return false;
			Capture.Nodes.Add({&Node, OwnerId, Placement, Order, NotificationOwner});
			return true;
		};
		auto AddSource = [&](const FGuid& SourceId, const FGuid& OwnerId, const FString& Name,
			const TCHAR* Placement, const UStruct* Type) {
			if (!Type) {
				OutError = CaptureFailure(TEXT("state_tree_binding_source_type_missing"),
					TEXT("The native binding source has no structure or class type."), &EditorData,
					FString(Placement) + TEXT(".") + Name, MakeShared<FJsonValueBoolean>(false),
					MakeShared<FJsonValueString>(TEXT("structure or class type"))); return false;
			}
			if (!AddIdentity(SourceId, TEXT("state_tree_binding_source_identity_invalid"), &EditorData,
				FString(Placement) + TEXT(".") + Name + TEXT(".ID"))) return false;
			Capture.BindingSources.Add({SourceId, OwnerId, Name, Placement, Type});
			return true;
		};
		auto AddNodes = [&](const TArray<FStateTreeEditorNode>& Nodes, const FGuid& OwnerId,
			ENodePlacement Placement, const UObject* NotificationOwner) {
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
				if (!AddNode(Nodes[Index], OwnerId, Placement, Index, NotificationOwner)) return false;
			return true;
		};
		if (!AddIdentity(EditorData.GetRootParametersGuid(), TEXT("state_tree_parameter_identity_invalid"),
			&EditorData, TEXT("RootParametersGuid"))) return false;
		if (EditorData.Schema)
			for (const auto& Context : EditorData.Schema->GetContextDataDescs())
				if (!AddSource(Context.ID, {}, Context.Name.ToString(), TEXT("context"), Context.Struct)) return false;
		if (!AddNodes(EditorData.Evaluators, {}, ENodePlacement::Evaluator, &EditorData)
			|| !AddNodes(EditorData.GlobalTasks, {}, ENodePlacement::GlobalTask, &EditorData)) return false;

		if (!CaptureHierarchy(EditorData, Capture.States, OutError)) return false;
		for (const FStateCapture& Entry : Capture.States)
		{
			const UStateTreeState& State = *Entry.State;
			if (!AddIdentity(State.ID, TEXT("state_tree_state_identity_invalid"), &State, TEXT("ID"))) return false;
			if (!AddIdentity(State.Parameters.ID, TEXT("state_tree_parameter_identity_invalid"), &State, TEXT("Parameters.ID"))) return false;
			if (State.bHasRequiredEventToEnter && !AddSource(State.GetEventID(), State.ID,
				State.Name.ToString() + TEXT(" Enter Event"), TEXT("state_event"), FStateTreeEvent::StaticStruct())) return false;
			if (!AddNodes(State.Tasks, State.ID, ENodePlacement::Task, &State)
				|| !AddNodes(State.EnterConditions, State.ID, ENodePlacement::EnterCondition, &State)
				|| !AddNodes(State.Considerations, State.ID, ENodePlacement::Consideration, &State)) return false;
			// Empty SingleTask is an optional slot, not a missing persisted node.
			if (State.SingleTask.Node.IsValid() || State.SingleTask.Instance.IsValid()
				|| State.SingleTask.InstanceObject || State.SingleTask.ID.IsValid())
				if (!AddNode(State.SingleTask, State.ID, ENodePlacement::SingleTask, 0, &State)) return false;
			for (int32 Index = 0; Index < State.Transitions.Num(); ++Index)
			{
				const FStateTreeTransition& Transition = State.Transitions[Index];
				if (!AddIdentity(Transition.ID, TEXT("state_tree_transition_identity_invalid"), &State,
					FString::Printf(TEXT("Transitions[%d].ID"), Index))) return false;
				Capture.Transitions.Add({&Transition, &State, Index});
				if (Transition.Trigger == EStateTreeTransitionTrigger::OnEvent && !AddSource(Transition.GetEventID(), Transition.ID,
					State.Name.ToString() + TEXT(" Transition Event"), TEXT("transition_event"), FStateTreeEvent::StaticStruct())) return false;
				if (!AddNodes(Transition.Conditions, Transition.ID, ENodePlacement::TransitionCondition, &State)) return false;
			}
		}

		// VisitAllNodes also prepares temporary event values; use persistent bindings for read-only capture.
		EditorData.EditorBindings.ForEachBinding([&](const FPropertyBindingBinding& Binding) {
			if (!OutError.IsEmpty()) return;
			Capture.Bindings.Add(&Binding);
			const FConstStructView Function = Binding.GetPropertyFunctionNode();
			if (const FStateTreeEditorNode* Node = Function.GetPtr<const FStateTreeEditorNode>())
				AddNode(*Node, Binding.GetTargetPath().GetStructID(), ENodePlacement::PropertyFunction, Capture.Bindings.Num() - 1, &EditorData);
		});
		if (!OutError.IsEmpty()) return false;
		OutCapture = MoveTemp(Capture);
		return true;
	}

	bool AppendNodePropertyReadRequests(const FNodeCapture& Capture,
		TFunctionRef<UeAgentAuthoringState::FEditablePropertyReadOptions(const FString& Part)> OptionsForPart,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!Capture.Node || !Capture.Node->ID.IsValid() || !Capture.NotificationOwner)
		{
			const auto Facts = MakeShared<FJsonObject>();
			Facts->SetBoolField(TEXT("node_present"), Capture.Node != nullptr);
			Facts->SetBoolField(TEXT("valid_identity"), Capture.Node && Capture.Node->ID.IsValid());
			Facts->SetBoolField(TEXT("notification_owner_present"), Capture.NotificationOwner != nullptr);
			OutError = CaptureFailure(TEXT("state_tree_node_property_binding_invalid"),
				!Capture.Node ? TEXT("The native property-read capture has no node.")
					: !Capture.Node->ID.IsValid() ? TEXT("The native property-read node has no valid identity.")
					: TEXT("The native property-read node has no notification owner."),
				Capture.NotificationOwner, NodeCollection(Capture.Placement) + FString::Printf(TEXT("[%d]"), Capture.Order),
				MakeShared<FJsonValueObject>(Facts), MakeShared<FJsonValueString>(TEXT("node with valid identity and notification owner")));
			return false;
		}
		const FStateTreeEditorNode& Node = *Capture.Node;
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		const FString NodeId = Node.ID.ToString(EGuidFormats::DigitsWithHyphensLower);
		auto AppendStruct = [&](const FInstancedStruct& Value, const TCHAR* Part) {
			if (!Value.IsValid()) return true;
			// This identity scopes the borrowed struct root; it is not an UObject lookup path.
			const FString Scope = Capture.NotificationOwner->GetPathName() + TEXT(".node:") + NodeId + TEXT(".") + Part;
			auto Options = OptionsForPart(Part);
			if (FString(Part) == TEXT("node")) Options.ExcludedProperties.Add(TEXT("Name"));
			return UeAgentAuthoringState::AppendEditableStructPropertyReadRequests(
				const_cast<UScriptStruct*>(Value.GetScriptStruct()), Value.GetMemory(), Scope,
				Capture.NotificationOwner, Options, Requests, OutError);
		};
		if (!AppendStruct(Node.Node, TEXT("node")) || !AppendStruct(Node.Instance, TEXT("instance"))) return false;
		if (Node.InstanceObject && !UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
			Node.InstanceObject, OptionsForPart(TEXT("instance_object")), Requests, OutError)) return false;
		if (Capture.Placement == ENodePlacement::PropertyFunction)
			for (auto& Request : Requests) Request.Context.Add(TEXT("state_tree_property_function"), TEXT("true"));
		OutRequests.Append(MoveTemp(Requests));
		return true;
	}
}
