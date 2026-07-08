#include "UeAgentHttpServer_Blueprint.h"

#include "Commands/Blueprint/UeAgentHttpServer_BlueprintOps.h"
#include "Module/UeAgentInterfaceLogger.h"

#include "AnimGraphNode_StateMachineBase.h"
#include "BlueprintEditorLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Knot.h"
#include "K2Node_Tunnel.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "ScopedTransaction.h"

bool FUeAgentHttpServer::CmdBlueprintAddEventNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString EventName;

	if (!JsonTryGetString(Ctx.Params, TEXT("event_name"), EventName) || EventName.IsEmpty())

	{

		OutError = TEXT("missing_event_name");

		return false;

	}

	FString EventClassPath = TEXT("/Script/Engine.Actor");

	JsonTryGetString(Ctx.Params, TEXT("event_class"), EventClassPath);

	UClass* EventClass = UeAgentBlueprintOps::ResolveClassByPath(EventClassPath);

	if (!EventClass)

	{

		OutError = TEXT("invalid_event_class");

		return false;

	}



	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	FString GraphName = TEXT("EventGraph");

	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);

	UEdGraph* Graph = UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, GraphName);

	if (!Graph)

	{

		OutError = TEXT("graph_not_found");

		return false;

	}



	const FName EventFName(*EventName);

	for (UEdGraphNode* Node : Graph->Nodes)

	{

		if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))

		{

			if (EventNode->EventReference.GetMemberName() == EventFName)

			{

				OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

				OutData->SetStringField(TEXT("graph_name"), Graph->GetName());

				OutData->SetStringField(TEXT("event_name"), EventName);

				OutData->SetStringField(TEXT("node_guid"), EventNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

				OutData->SetBoolField(TEXT("changed"), false);

				return true;

			}

		}

	}



	double XNum = 0.0;

	double YNum = 0.0;

	JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum);

	JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum);

	int32 NodePosY = (int32)YNum;



	UK2Node_Event* NewEventNode = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, Graph, EventFName, EventClass, NodePosY);

	if (!NewEventNode)

	{

		OutError = TEXT("add_event_node_failed");

		return false;

	}

	NewEventNode->NodePosX = (int32)XNum;

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);



	bool bCompileAfterAdd = true;

	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);

	if (bCompileAfterAdd)

	{

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

	}

	bool bSaveAfterAdd = false;

	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);

	if (bSaveAfterAdd && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))

	{

		return false;

	}



	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

	OutData->SetStringField(TEXT("graph_name"), Graph->GetName());

	OutData->SetStringField(TEXT("event_name"), EventName);

	OutData->SetStringField(TEXT("node_guid"), NewEventNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	OutData->SetBoolField(TEXT("changed"), true);

	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);

	return true;

}



bool FUeAgentHttpServer::CmdBlueprintAddCustomEventNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString EventName;

	if (!JsonTryGetString(Ctx.Params, TEXT("event_name"), EventName) || EventName.IsEmpty())

	{

		OutError = TEXT("missing_event_name");

		return false;

	}



	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	if (UK2Node_Event* ExistingNode = FBlueprintEditorUtils::FindCustomEventNode(Blueprint, FName(*EventName)))

	{

		OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

		OutData->SetStringField(TEXT("event_name"), EventName);

		OutData->SetStringField(TEXT("node_guid"), ExistingNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

		OutData->SetBoolField(TEXT("changed"), false);

		return true;

	}



	FString GraphName = TEXT("EventGraph");

	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);

	UEdGraph* Graph = UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, GraphName);

	if (!Graph)

	{

		OutError = TEXT("graph_not_found");

		return false;

	}

	double XNum = 0.0;

	double YNum = 0.0;

	JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum);

	JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum);



	UK2Node_CustomEvent* CustomEventNode = NewObject<UK2Node_CustomEvent>(Graph);

	if (!CustomEventNode)

	{

		OutError = TEXT("create_custom_event_node_failed");

		return false;

	}

	CustomEventNode->CustomFunctionName = FName(*EventName);

	CustomEventNode->CreateNewGuid();

	CustomEventNode->PostPlacedNewNode();

	CustomEventNode->SetFlags(RF_Transactional);

	CustomEventNode->NodePosX = (int32)XNum;

	CustomEventNode->NodePosY = (int32)YNum;

	CustomEventNode->AllocateDefaultPins();

	UEdGraphSchema_K2::SetNodeMetaData(CustomEventNode, FNodeMetadata::DefaultGraphNode);

	Graph->AddNode(CustomEventNode, true, false);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);



	bool bCompileAfterAdd = true;

	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);

	if (bCompileAfterAdd)

	{

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

	}

	bool bSaveAfterAdd = false;

	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);

	if (bSaveAfterAdd && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))

	{

		return false;

	}



	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

	OutData->SetStringField(TEXT("event_name"), EventName);

	OutData->SetStringField(TEXT("node_guid"), CustomEventNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	OutData->SetBoolField(TEXT("changed"), true);

	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);

	return true;

}



bool FUeAgentHttpServer::CmdBlueprintAddCallFunctionNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString FunctionOwnerClassPath;

	if (!JsonTryGetString(Ctx.Params, TEXT("function_owner_class"), FunctionOwnerClassPath) || FunctionOwnerClassPath.IsEmpty())

	{

		OutError = TEXT("missing_function_owner_class");

		return false;

	}

	FString FunctionName;

	if (!JsonTryGetString(Ctx.Params, TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())

	{

		OutError = TEXT("missing_function_name");

		return false;

	}

	UClass* OwnerClass = UeAgentBlueprintOps::ResolveClassByPath(FunctionOwnerClassPath);

	if (!OwnerClass)

	{

		OutError = TEXT("invalid_function_owner_class");

		return false;

	}

	UFunction* Function = OwnerClass->FindFunctionByName(FName(*FunctionName));

	if (!Function)

	{

		OutError = TEXT("function_not_found");

		return false;

	}



	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	FString GraphName = TEXT("EventGraph");

	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);

	UEdGraph* Graph = UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, GraphName);

	if (!Graph)

	{

		OutError = TEXT("graph_not_found");

		return false;

	}

	bool bAllowUnsafeConstructionScriptPcgGeneration = false;

	JsonTryGetBool(Ctx.Params, TEXT("allow_unsafe_construction_script_pcg_generation"), bAllowUnsafeConstructionScriptPcgGeneration);

	if (!bAllowUnsafeConstructionScriptPcgGeneration
		&& UeAgentBlueprintOps::IsUserConstructionScriptGraph(Graph)
		&& UeAgentBlueprintOps::IsUnsafeConstructionScriptPcgCall(OwnerClass->GetPathName(), Function->GetName()))

	{

		if (!OutData.IsValid())

		{

			OutData = MakeShared<FJsonObject>();

		}

		OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

		OutData->SetStringField(TEXT("graph_name"), Graph->GetName());

		OutData->SetStringField(TEXT("function_owner_class"), OwnerClass->GetPathName());

		OutData->SetStringField(TEXT("function_name"), Function->GetName());

		OutData->SetBoolField(TEXT("allow_unsafe_construction_script_pcg_generation"), bAllowUnsafeConstructionScriptPcgGeneration);

		OutError = FString::Printf(
			TEXT("unsafe_construction_script_pcg_generation_blocked:%s:%s.%s"),
			*Graph->GetName(),
			*OwnerClass->GetPathName(),
			*Function->GetName());

		return false;

	}

	bool bAllowEventFunctionCall = false;
	JsonTryGetBool(Ctx.Params, TEXT("allow_event_function_call"), bAllowEventFunctionCall);
	if (!bAllowEventFunctionCall && UeAgentBlueprintOps::RequiresExplicitParentOrEventNode(Function->GetName()))
	{
		if (!OutData.IsValid())
		{
			OutData = MakeShared<FJsonObject>();
		}
		OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("graph_name"), Graph->GetName());
		OutData->SetStringField(TEXT("function_owner_class"), OwnerClass->GetPathName());
		OutData->SetStringField(TEXT("function_name"), Function->GetName());
		OutData->SetBoolField(TEXT("allow_event_function_call"), bAllowEventFunctionCall);
		OutError = FString::Printf(
			TEXT("event_function_call_requires_explicit_parent_node:%s.%s"),
			*OwnerClass->GetPathName(),
			*Function->GetName());
		return false;
	}



	double XNum = 0.0;

	double YNum = 0.0;

	JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum);

	JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum);



	UK2Node_CallFunction* CallFunctionNode = NewObject<UK2Node_CallFunction>(Graph);

	if (!CallFunctionNode)

	{

		OutError = TEXT("create_call_function_node_failed");

		return false;

	}

	CallFunctionNode->CreateNewGuid();

	CallFunctionNode->PostPlacedNewNode();

	CallFunctionNode->SetFlags(RF_Transactional);

	CallFunctionNode->NodePosX = (int32)XNum;

	CallFunctionNode->NodePosY = (int32)YNum;

	CallFunctionNode->SetFromFunction(Function);

	CallFunctionNode->AllocateDefaultPins();

	UEdGraphSchema_K2::SetNodeMetaData(CallFunctionNode, FNodeMetadata::DefaultGraphNode);

	Graph->AddNode(CallFunctionNode, true, false);



	FString NodeComment;

	if (JsonTryGetString(Ctx.Params, TEXT("node_comment"), NodeComment) && !NodeComment.IsEmpty())

	{

		CallFunctionNode->NodeComment = NodeComment;

	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);



	bool bCompileAfterAdd = true;

	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);

	if (bCompileAfterAdd)

	{

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

	}

	bool bSaveAfterAdd = false;

	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);

	if (bSaveAfterAdd && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))

	{

		return false;

	}



	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

	OutData->SetStringField(TEXT("graph_name"), Graph->GetName());

	OutData->SetStringField(TEXT("function_owner_class"), OwnerClass->GetPathName());

	OutData->SetStringField(TEXT("function_name"), Function->GetName());

	OutData->SetStringField(TEXT("node_guid"), CallFunctionNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);

	return true;

}



bool FUeAgentHttpServer::CmdBlueprintConnectPins(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString GraphName = TEXT("EventGraph");

	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);



	FString FromNodeGuid;

	FString FromPinName;

	FString ToNodeGuid;

	FString ToPinName;

	if (!JsonTryGetString(Ctx.Params, TEXT("from_node_guid"), FromNodeGuid) || FromNodeGuid.IsEmpty())

	{

		OutError = TEXT("missing_from_node_guid");

		return false;

	}

	if (!JsonTryGetString(Ctx.Params, TEXT("from_pin"), FromPinName) || FromPinName.IsEmpty())

	{

		OutError = TEXT("missing_from_pin");

		return false;

	}

	if (!JsonTryGetString(Ctx.Params, TEXT("to_node_guid"), ToNodeGuid) || ToNodeGuid.IsEmpty())

	{

		OutError = TEXT("missing_to_node_guid");

		return false;

	}

	if (!JsonTryGetString(Ctx.Params, TEXT("to_pin"), ToPinName) || ToPinName.IsEmpty())

	{

		OutError = TEXT("missing_to_pin");

		return false;

	}



	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	UEdGraph* Graph = UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, GraphName);

	if (!Graph)

	{

		OutError = TEXT("graph_not_found");

		return false;

	}

	UEdGraphNode* FromNode = UeAgentBlueprintOps::FindGraphNodeByGuid(Graph, FromNodeGuid);

	UEdGraphNode* ToNode = UeAgentBlueprintOps::FindGraphNodeByGuid(Graph, ToNodeGuid);

	if (!FromNode || !ToNode)

	{

		OutError = TEXT("node_not_found");

		return false;

	}

	const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
	auto FindPinByNameOrSplitParent = [K2Schema](UEdGraphNode* Node, const FString& PinName) -> UEdGraphPin*
	{
		if (!Node)
		{
			return nullptr;
		}
		if (UEdGraphPin* Pin = UeAgentBlueprintOps::FindNodePinByName(Node, PinName))
		{
			return Pin;
		}
		if (!K2Schema)
		{
			return nullptr;
		}
		for (UEdGraphPin* CandidatePin : Node->Pins)
		{
			if (!CandidatePin || CandidatePin->SubPins.Num() > 0)
			{
				continue;
			}
			const FString CandidateName = CandidatePin->GetName();
			if (!PinName.StartsWith(CandidateName + TEXT("_"), ESearchCase::CaseSensitive))
			{
				continue;
			}
			if (!K2Schema->CanSplitStructPin(*CandidatePin))
			{
				continue;
			}
			K2Schema->SplitPin(CandidatePin, false);
			if (UEdGraphPin* SplitPin = UeAgentBlueprintOps::FindNodePinByName(Node, PinName))
			{
				return SplitPin;
			}
		}
		return nullptr;
	};

	UEdGraphPin* FromPin = FindPinByNameOrSplitParent(FromNode, FromPinName);

	UEdGraphPin* ToPin = FindPinByNameOrSplitParent(ToNode, ToPinName);

	if (!FromPin || !ToPin)

	{

		OutError = TEXT("pin_not_found");

		return false;

	}



	const UEdGraphSchema* Schema = Graph->GetSchema();

	if (!Schema || !Schema->TryCreateConnection(FromPin, ToPin))

	{

		OutError = TEXT("connect_pins_failed");

		return false;

	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);



	bool bCompileAfterConnect = true;

	JsonTryGetBool(Ctx.Params, TEXT("compile_after_connect"), bCompileAfterConnect);

	if (bCompileAfterConnect)

	{

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

	}

	bool bSaveAfterConnect = false;

	JsonTryGetBool(Ctx.Params, TEXT("save_after_connect"), bSaveAfterConnect);

	if (bSaveAfterConnect && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))

	{

		return false;

	}



	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

	OutData->SetStringField(TEXT("graph_name"), Graph->GetName());

	OutData->SetStringField(TEXT("from_node_guid"), FromNodeGuid);

	OutData->SetStringField(TEXT("from_pin"), FromPinName);

	OutData->SetStringField(TEXT("to_node_guid"), ToNodeGuid);

	OutData->SetStringField(TEXT("to_pin"), ToPinName);

	OutData->SetBoolField(TEXT("saved"), bSaveAfterConnect);

	return true;

}

bool FUeAgentHttpServer::CmdBlueprintDisconnectPins(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString GraphName = TEXT("EventGraph");

	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);



	FString FromNodeGuid;

	FString FromPinName;

	if (!JsonTryGetString(Ctx.Params, TEXT("from_node_guid"), FromNodeGuid) || FromNodeGuid.IsEmpty())

	{

		OutError = TEXT("missing_from_node_guid");

		return false;

	}

	if (!JsonTryGetString(Ctx.Params, TEXT("from_pin"), FromPinName) || FromPinName.IsEmpty())

	{

		OutError = TEXT("missing_from_pin");

		return false;

	}

	FString ToNodeGuid;

	FString ToPinName;

	const bool bHasSpecificTarget =
		JsonTryGetString(Ctx.Params, TEXT("to_node_guid"), ToNodeGuid) && !ToNodeGuid.IsEmpty()
		&& JsonTryGetString(Ctx.Params, TEXT("to_pin"), ToPinName) && !ToPinName.IsEmpty();



	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	UEdGraph* Graph = UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, GraphName);

	if (!Graph)

	{

		OutError = TEXT("graph_not_found");

		return false;

	}

	UEdGraphNode* FromNode = UeAgentBlueprintOps::FindGraphNodeByGuid(Graph, FromNodeGuid);

	if (!FromNode)

	{

		OutError = TEXT("from_node_not_found");

		return false;

	}

	UEdGraphPin* FromPin = UeAgentBlueprintOps::FindNodePinByName(FromNode, FromPinName);

	if (!FromPin)

	{

		OutError = TEXT("from_pin_not_found");

		return false;

	}

	int32 BrokenLinkCount = 0;

	if (bHasSpecificTarget)

	{

		UEdGraphNode* ToNode = UeAgentBlueprintOps::FindGraphNodeByGuid(Graph, ToNodeGuid);

		if (!ToNode)

		{

			OutError = TEXT("to_node_not_found");

			return false;

		}

		UEdGraphPin* ToPin = UeAgentBlueprintOps::FindNodePinByName(ToNode, ToPinName);

		if (!ToPin)

		{

			OutError = TEXT("to_pin_not_found");

			return false;

		}

		if (!FromPin->LinkedTo.Contains(ToPin))

		{

			OutError = TEXT("pin_link_not_found");

			return false;

		}

		FromPin->Modify();

		ToPin->Modify();

		FromPin->BreakLinkTo(ToPin);

		BrokenLinkCount = 1;

	}

	else

	{

		BrokenLinkCount = FromPin->LinkedTo.Num();

		if (BrokenLinkCount == 0)

		{

			OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

			OutData->SetStringField(TEXT("graph_name"), Graph->GetName());

			OutData->SetStringField(TEXT("from_node_guid"), FromNodeGuid);

			OutData->SetStringField(TEXT("from_pin"), FromPinName);

			OutData->SetNumberField(TEXT("broken_link_count"), 0);

			OutData->SetBoolField(TEXT("changed"), false);

			return true;

		}

		for (UEdGraphPin* LinkedPin : FromPin->LinkedTo)

		{

			if (LinkedPin)

			{

				LinkedPin->Modify();

			}

		}

		FromPin->Modify();

		FromPin->BreakAllPinLinks();

	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);



	bool bCompileAfterDisconnect = true;

	JsonTryGetBool(Ctx.Params, TEXT("compile_after_disconnect"), bCompileAfterDisconnect);

	if (bCompileAfterDisconnect)

	{

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

	}

	bool bSaveAfterDisconnect = false;

	JsonTryGetBool(Ctx.Params, TEXT("save_after_disconnect"), bSaveAfterDisconnect);

	if (bSaveAfterDisconnect && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))

	{

		return false;

	}



	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

	OutData->SetStringField(TEXT("graph_name"), Graph->GetName());

	OutData->SetStringField(TEXT("from_node_guid"), FromNodeGuid);

	OutData->SetStringField(TEXT("from_pin"), FromPinName);

	if (bHasSpecificTarget)

	{

		OutData->SetStringField(TEXT("to_node_guid"), ToNodeGuid);

		OutData->SetStringField(TEXT("to_pin"), ToPinName);

	}

	OutData->SetNumberField(TEXT("broken_link_count"), BrokenLinkCount);

	OutData->SetBoolField(TEXT("changed"), BrokenLinkCount > 0);

	OutData->SetBoolField(TEXT("saved"), bSaveAfterDisconnect);

	return true;

}



bool FUeAgentHttpServer::CmdBlueprintSetPinDefaultValue(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString GraphName = TEXT("EventGraph");

	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);

	FString NodeGuid;

	FString PinName;

	FString DefaultValue;

	if (!JsonTryGetString(Ctx.Params, TEXT("node_guid"), NodeGuid) || NodeGuid.IsEmpty())

	{

		OutError = TEXT("missing_node_guid");

		return false;

	}

	if (!JsonTryGetString(Ctx.Params, TEXT("pin_name"), PinName) || PinName.IsEmpty())

	{

		OutError = TEXT("missing_pin_name");

		return false;

	}

	if (!JsonTryGetString(Ctx.Params, TEXT("default_value"), DefaultValue))

	{

		OutError = TEXT("missing_default_value");

		return false;

	}



	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	UEdGraph* Graph = UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, GraphName);

	if (!Graph)

	{

		OutError = TEXT("graph_not_found");

		return false;

	}

	UEdGraphNode* Node = UeAgentBlueprintOps::FindGraphNodeByGuid(Graph, NodeGuid);

	if (!Node)

	{

		OutError = TEXT("node_not_found");

		return false;

	}

	UEdGraphPin* Pin = UeAgentBlueprintOps::FindNodePinByName(Node, PinName);

	if (!Pin)

	{

		OutError = TEXT("pin_not_found");

		return false;

	}

	if (const UEdGraphSchema* Schema = Graph->GetSchema())

	{

		Schema->TrySetDefaultValue(*Pin, DefaultValue);

	}

	else

	{

		Pin->DefaultValue = DefaultValue;

	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);



	bool bCompileAfterSet = true;

	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);

	if (bCompileAfterSet)

	{

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

	}

	bool bSaveAfterSet = false;

	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);

	if (bSaveAfterSet && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))

	{

		return false;

	}



	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

	OutData->SetStringField(TEXT("node_guid"), NodeGuid);

	OutData->SetStringField(TEXT("pin_name"), PinName);

	OutData->SetStringField(TEXT("default_value"), Pin->DefaultValue);

	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);

	return true;

}



bool FUeAgentHttpServer::CmdBlueprintRemoveNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString GraphName = TEXT("EventGraph");

	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);

	FString NodeGuid;

	if (!JsonTryGetString(Ctx.Params, TEXT("node_guid"), NodeGuid) || NodeGuid.IsEmpty())

	{

		OutError = TEXT("missing_node_guid");

		return false;

	}



	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	UEdGraph* Graph = UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, GraphName);

	if (!Graph)

	{

		OutError = TEXT("graph_not_found");

		return false;

	}

	UEdGraphNode* Node = UeAgentBlueprintOps::FindGraphNodeByGuid(Graph, NodeGuid);

	if (!Node)

	{

		OutError = TEXT("node_not_found");

		return false;

	}



	Graph->Modify();

	Graph->RemoveNode(Node);

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);



	bool bCompileAfterRemove = true;

	JsonTryGetBool(Ctx.Params, TEXT("compile_after_remove"), bCompileAfterRemove);

	if (bCompileAfterRemove)

	{

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

	}

	bool bSaveAfterRemove = false;

	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);

	if (bSaveAfterRemove && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))

	{

		return false;

	}



	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

	OutData->SetStringField(TEXT("graph_name"), Graph->GetName());

	OutData->SetStringField(TEXT("node_guid"), NodeGuid);

	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);

	return true;

}



bool FUeAgentHttpServer::CmdBlueprintAddVariable(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString VariableName;

	if (!JsonTryGetString(Ctx.Params, TEXT("variable_name"), VariableName) || VariableName.IsEmpty())

	{

		OutError = TEXT("missing_variable_name");

		return false;

	}

	FString PinCategoryText;

	if (!JsonTryGetString(Ctx.Params, TEXT("pin_category"), PinCategoryText) || PinCategoryText.IsEmpty())

	{

		OutError = TEXT("missing_pin_category");

		return false;

	}



	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}



	const FName VariableFName(*VariableName);

	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableFName) != INDEX_NONE)

	{

		OutError = TEXT("variable_already_exists");

		return false;

	}



	FEdGraphPinType PinType;

	if (!UeAgentBlueprintOps::BuildPinTypeFromJsonObject(Ctx.Params, PinType, OutError))

	{

		return false;

	}



	FString DefaultValue;

	JsonTryGetString(Ctx.Params, TEXT("default_value"), DefaultValue);

	FUeAgentInterfaceLogger::Log(

		TEXT("BlueprintAddVariable request asset=%s var=%s pin_category=%s pin_subcategory=%s default=%s"),

		*Blueprint->GetOutermost()->GetName(),

		*VariableName,

		*PinType.PinCategory.ToString(),

		*PinType.PinSubCategory.ToString(),

		*DefaultValue);

	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableFName, PinType, DefaultValue))

	{

		OutError = TEXT("add_member_variable_failed");

		return false;

	}



	bool bInstanceEditable = false;

	JsonTryGetBool(Ctx.Params, TEXT("instance_editable"), bInstanceEditable);

	if (bInstanceEditable)

	{

		FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VariableFName, false);

	}



	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	bool bCompileAfterAdd = true;

	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);

	if (bCompileAfterAdd)

	{

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

	}

	bool bSaveAfterAdd = true;

	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);

	if (bSaveAfterAdd && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))

	{

		return false;

	}



	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

	OutData->SetStringField(TEXT("variable_name"), VariableName);

	OutData->SetStringField(TEXT("pin_category"), PinType.PinCategory.ToString());

	OutData->SetStringField(TEXT("pin_subcategory"), PinType.PinSubCategory.ToString());

	if (const UObject* SubCategoryObject = PinType.PinSubCategoryObject.Get())

	{

		OutData->SetStringField(TEXT("pin_subcategory_object"), SubCategoryObject->GetPathName());

	}

	const FString ContainerTypeText = UeAgentBlueprintOps::PinContainerTypeToString(PinType.ContainerType);

	if (!ContainerTypeText.IsEmpty())

	{

		OutData->SetStringField(TEXT("container_type"), ContainerTypeText);

	}

	if (PinType.ContainerType == EPinContainerType::Map && !PinType.PinValueType.TerminalCategory.IsNone())

	{

		TSharedPtr<FJsonObject> ValueTypeObj = MakeShared<FJsonObject>();

		ValueTypeObj->SetStringField(TEXT("pin_category"), PinType.PinValueType.TerminalCategory.ToString());

		if (!PinType.PinValueType.TerminalSubCategory.IsNone())

		{

			ValueTypeObj->SetStringField(TEXT("pin_subcategory"), PinType.PinValueType.TerminalSubCategory.ToString());

		}

		if (const UObject* ValueSubCategoryObject = PinType.PinValueType.TerminalSubCategoryObject.Get())

		{

			ValueTypeObj->SetStringField(TEXT("pin_subcategory_object"), ValueSubCategoryObject->GetPathName());

		}

		OutData->SetObjectField(TEXT("value_type"), ValueTypeObj);

	}

	OutData->SetBoolField(TEXT("instance_editable"), bInstanceEditable);

	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);

	return true;

}



bool FUeAgentHttpServer::CmdBlueprintRemoveVariable(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString VariableName;

	if (!JsonTryGetString(Ctx.Params, TEXT("variable_name"), VariableName) || VariableName.IsEmpty())

	{

		OutError = TEXT("missing_variable_name");

		return false;

	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}



	const FName VariableFName(*VariableName);

	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableFName) == INDEX_NONE)

	{

		OutError = TEXT("variable_not_found");

		return false;

	}

	FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VariableFName);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);



	bool bCompileAfterRemove = true;

	JsonTryGetBool(Ctx.Params, TEXT("compile_after_remove"), bCompileAfterRemove);

	if (bCompileAfterRemove)

	{

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

	}

	bool bSaveAfterRemove = true;

	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);

	if (bSaveAfterRemove && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))

	{

		return false;

	}



	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

	OutData->SetStringField(TEXT("variable_name"), VariableName);

	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);

	return true;

}

bool FUeAgentHttpServer::CmdBlueprintAddNodeByClass(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{
	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString NodeClassPath;

	if (!JsonTryGetString(Ctx.Params, TEXT("node_class"), NodeClassPath) || NodeClassPath.TrimStartAndEnd().IsEmpty())

	{

		OutError = TEXT("missing_node_class");

		return false;

	}

	NodeClassPath = NodeClassPath.TrimStartAndEnd();

	FString GraphName = TEXT("EventGraph");

	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);
	FString GraphPath;
	if (JsonTryGetString(Ctx.Params, TEXT("graph_path"), GraphPath) && !GraphPath.TrimStartAndEnd().IsEmpty())
	{
		GraphName = GraphPath.TrimStartAndEnd();
	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	UEdGraph* Graph = UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, GraphName);

	if (!Graph)

	{

		OutError = TEXT("graph_not_found");

		return false;

	}

	UClass* NodeClass = UeAgentBlueprintOps::ResolveClassByPath(NodeClassPath);

	if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()))

	{

		OutError = TEXT("invalid_node_class");

		return false;

	}

	if (NodeClass->HasAnyClassFlags(CLASS_Abstract))

	{

		OutError = TEXT("node_class_is_abstract");

		return false;

	}

	if (NodeClass->IsChildOf(UK2Node_Event::StaticClass()) ||
		NodeClass->IsChildOf(UK2Node_CustomEvent::StaticClass()) ||
		NodeClass->IsChildOf(UK2Node_CallFunction::StaticClass()) ||
		NodeClass->IsChildOf(UK2Node_Variable::StaticClass()) ||
		NodeClass->IsChildOf(UK2Node_ComponentBoundEvent::StaticClass()) ||
		NodeClass->IsChildOf(UAnimGraphNode_StateMachineBase::StaticClass()))

	{

		OutError = TEXT("specialized_node_requires_specific_command");

		return false;

	}

	double XNum = 0.0;

	double YNum = 0.0;

	JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum);

	JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum);

	Blueprint->Modify();

	Graph->Modify();

	UEdGraphNode* GenericNode = NewObject<UEdGraphNode>(Graph, NodeClass);

	if (!GenericNode)

	{

		OutError = TEXT("create_node_failed");

		return false;

	}

	Graph->AddNode(GenericNode, true, false);

	GenericNode->CreateNewGuid();

	GenericNode->PostPlacedNewNode();

	GenericNode->SetFlags(RF_Transactional);

	GenericNode->NodePosX = (int32)XNum;

	GenericNode->NodePosY = (int32)YNum;

	GenericNode->AllocateDefaultPins();

	double ExecOutputPinCountNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("exec_output_pin_count"), ExecOutputPinCountNumber))
	{
		const int32 DesiredExecOutputPinCount = FMath::Max(0, static_cast<int32>(ExecOutputPinCountNumber));
		if (DesiredExecOutputPinCount > 0)
		{
			if (UK2Node_ExecutionSequence* SequenceNode = Cast<UK2Node_ExecutionSequence>(GenericNode))
			{
				auto CountExecOutputPins = [&SequenceNode]() -> int32
				{
					int32 Count = 0;
					for (UEdGraphPin* Pin : SequenceNode->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
						{
							++Count;
						}
					}
					return Count;
				};
				while (CountExecOutputPins() < DesiredExecOutputPinCount)
				{
					SequenceNode->AddInputPin();
				}
			}
		}
	}

	UEdGraphSchema_K2::SetNodeMetaData(GenericNode, FNodeMetadata::DefaultGraphNode);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	bool bCompileAfterAdd = true;

	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);

	if (bCompileAfterAdd)

	{

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

	}

	bool bSaveAfterAdd = false;

	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);

	if (bSaveAfterAdd && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))

	{

		return false;

	}

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

	OutData->SetStringField(TEXT("graph_name"), Graph->GetName());
	OutData->SetStringField(TEXT("graph_path"), Graph->GetPathName());

	OutData->SetStringField(TEXT("node_guid"), GenericNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	OutData->SetStringField(TEXT("node_class"), GenericNode->GetClass()->GetPathName());

	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);

	return true;

}



bool FUeAgentHttpServer::CmdBlueprintAddVariableNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString VariableName;

	if (!JsonTryGetString(Ctx.Params, TEXT("variable_name"), VariableName) || VariableName.IsEmpty())

	{

		OutError = TEXT("missing_variable_name");

		return false;

	}

	FString GraphName = TEXT("EventGraph");

	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);

	FString MemberParentClassPath;
	JsonTryGetString(Ctx.Params, TEXT("member_parent_class"), MemberParentClassPath);
	MemberParentClassPath.TrimStartAndEndInline();

	FString NodeType = TEXT("get");

	JsonTryGetString(Ctx.Params, TEXT("node_type"), NodeType);

	const bool bIsSetNode = NodeType.Equals(TEXT("set"), ESearchCase::IgnoreCase);

	if (!bIsSetNode && !NodeType.Equals(TEXT("get"), ESearchCase::IgnoreCase))

	{

		OutError = TEXT("invalid_node_type");

		return false;

	}



	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	const FName VariableFName(*VariableName);
	UClass* ExternalMemberParentClass = nullptr;
	const FProperty* ExternalProperty = nullptr;
	if (!MemberParentClassPath.IsEmpty())
	{
		ExternalMemberParentClass = UeAgentBlueprintOps::ResolveClassByPath(MemberParentClassPath);
		if (!ExternalMemberParentClass)
		{
			OutError = TEXT("invalid_member_parent_class");
			return false;
		}

		ExternalProperty = FindFProperty<FProperty>(ExternalMemberParentClass, VariableFName);
		if (!ExternalProperty)
		{
			OutError = TEXT("variable_not_found");
			return false;
		}
	}
	else if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableFName) == INDEX_NONE &&
		(!Blueprint->SkeletonGeneratedClass || FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass, VariableFName) == nullptr))
	{

		OutError = TEXT("variable_not_found");

		return false;

	}

	UEdGraph* Graph = UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, GraphName);

	if (!Graph)

	{

		OutError = TEXT("graph_not_found");

		return false;

	}



	double XNum = 0.0;

	double YNum = 0.0;

	JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum);

	JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum);



	UK2Node_Variable* VariableNode = bIsSetNode ? static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableSet>(Graph))

		: static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableGet>(Graph));

	if (!VariableNode)

	{

		OutError = TEXT("create_variable_node_failed");

		return false;

	}
	if (ExternalProperty && ExternalMemberParentClass)
	{
		VariableNode->SetFromProperty(ExternalProperty, false, ExternalMemberParentClass);
	}
	else
	{
		VariableNode->VariableReference.SetSelfMember(VariableFName);
	}

	VariableNode->CreateNewGuid();

	VariableNode->PostPlacedNewNode();

	VariableNode->SetFlags(RF_Transactional);

	VariableNode->NodePosX = (int32)XNum;

	VariableNode->NodePosY = (int32)YNum;

	VariableNode->AllocateDefaultPins();

	bool bPure = true;
	JsonTryGetBool(Ctx.Params, TEXT("pure"), bPure);
	if (!bIsSetNode)
	{
		if (UK2Node_VariableGet* VariableGetNode = Cast<UK2Node_VariableGet>(VariableNode))
		{
			VariableGetNode->SetPurity(bPure);
		}
	}

	UEdGraphSchema_K2::SetNodeMetaData(VariableNode, FNodeMetadata::DefaultGraphNode);

	Graph->AddNode(VariableNode, true, false);

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);



	bool bCompileAfterAdd = true;

	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);

	if (bCompileAfterAdd)

	{

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

	}

	bool bSaveAfterAdd = false;

	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);

	if (bSaveAfterAdd && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))

	{

		return false;

	}



	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

	OutData->SetStringField(TEXT("graph_name"), Graph->GetName());

	OutData->SetStringField(TEXT("node_guid"), VariableNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	OutData->SetStringField(TEXT("variable_name"), VariableName);

	if (ExternalMemberParentClass)
	{
		OutData->SetStringField(TEXT("member_parent_class"), ExternalMemberParentClass->GetPathName());
	}

	OutData->SetStringField(TEXT("node_type"), bIsSetNode ? TEXT("set") : TEXT("get"));

	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);

	return true;

}
