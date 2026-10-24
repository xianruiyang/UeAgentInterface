#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UStateTreeEditorData;
class UStateTreeState;
struct FStateTreeEditorNode;
struct FStateTreeTransition;
struct FPropertyBindingBinding;

namespace UeAgentStateTreeAuthoring
{
	struct FStateCapture
	{
		const UStateTreeState* State = nullptr;
		const UStateTreeState* Parent = nullptr;
		int32 Order = 0;
	};

	enum class ENodePlacement : uint8
	{
		Evaluator, GlobalTask, Task, SingleTask, EnterCondition,
		Consideration, TransitionCondition, PropertyFunction
	};

	struct FNodeCapture
	{
		const FStateTreeEditorNode* Node = nullptr;
		// Empty for editor-data collections; otherwise a state, transition or binding target ID.
		FGuid OwnerId;
		ENodePlacement Placement = ENodePlacement::Task;
		int32 Order = 0;
		const UObject* NotificationOwner = nullptr;
	};

	struct FTransitionCapture
	{
		const FStateTreeTransition* Transition = nullptr;
		const UStateTreeState* State = nullptr;
		int32 Order = 0;
	};
	struct FBindingSourceCapture
	{
		FGuid Id;
		FGuid OwnerId;
		FString Name;
		FString Placement;
		const UStruct* Type = nullptr;
	};

	// Borrowed native views: consume before any mutation/reload; this is not a rollback snapshot.
	struct FTreeCapture
	{
		const UStateTreeEditorData* EditorData = nullptr;
		TArray<FStateCapture> States;
		TArray<FNodeCapture> Nodes;
		TArray<FTransitionCapture> Transitions;
		TArray<const FPropertyBindingBinding*> Bindings;
		TArray<FBindingSourceCapture> BindingSources;
	};

	UEAGENTINTERFACE_API bool CaptureHierarchy(const UStateTreeEditorData& EditorData,
		TArray<FStateCapture>& OutStates, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool CaptureTree(const UStateTreeEditorData& EditorData,
		FTreeCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError);

	// Options may supply a scoped snapshot identity for each native part. Requests borrow
	// the captured node storage; capture their values before changing any owning collection.
	UEAGENTINTERFACE_API bool AppendNodePropertyReadRequests(const FNodeCapture& Capture,
		TFunctionRef<UeAgentAuthoringState::FEditablePropertyReadOptions(const FString& Part)> OptionsForPart,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError);
}
