namespace UeAgentBlueprintOps

{

	static FName NormalizePinCategoryName(const FString& InCategory)

	{

		const FString Cat = InCategory.TrimStartAndEnd();

		if (Cat.IsEmpty())

		{

			return NAME_None;

		}

		if (Cat.Equals(TEXT("bool"), ESearchCase::IgnoreCase) || Cat.Equals(TEXT("boolean"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Boolean;

		}

		if (Cat.Equals(TEXT("int"), ESearchCase::IgnoreCase) || Cat.Equals(TEXT("integer"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Int;

		}

		if (Cat.Equals(TEXT("int64"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Int64;

		}

		if (Cat.Equals(TEXT("byte"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Byte;

		}

		if (Cat.Equals(TEXT("float"), ESearchCase::IgnoreCase) || Cat.Equals(TEXT("double"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Real;

		}

		if (Cat.Equals(TEXT("name"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Name;

		}

		if (Cat.Equals(TEXT("string"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_String;

		}

		if (Cat.Equals(TEXT("text"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Text;

		}

		if (Cat.Equals(TEXT("struct"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Struct;

		}

		if (Cat.Equals(TEXT("object"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Object;

		}

		if (Cat.Equals(TEXT("class"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Class;

		}

		if (Cat.Equals(TEXT("delegate"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Delegate;

		}

		if (Cat.Equals(TEXT("multicastdelegate"), ESearchCase::IgnoreCase) || Cat.Equals(TEXT("mcdelegate"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_MCDelegate;

		}

		return FName(*Cat);

	}



	static FName NormalizeRealPinSubCategoryName(const FString& InSubCategory)

	{

		const FString SubCategory = InSubCategory.TrimStartAndEnd();

		if (SubCategory.IsEmpty())

		{

			return NAME_None;

		}

		if (SubCategory.Equals(TEXT("float"), ESearchCase::IgnoreCase) || SubCategory.Equals(TEXT("single"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Float;

		}

		if (SubCategory.Equals(TEXT("double"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Double;

		}

		return FName(*SubCategory);

	}



	static EPinContainerType ParsePinContainerType(const FString& InContainerType)

	{

		const FString ContainerType = InContainerType.TrimStartAndEnd();

		if (ContainerType.Equals(TEXT("array"), ESearchCase::IgnoreCase))

		{

			return EPinContainerType::Array;

		}

		if (ContainerType.Equals(TEXT("set"), ESearchCase::IgnoreCase))

		{

			return EPinContainerType::Set;

		}

		if (ContainerType.Equals(TEXT("map"), ESearchCase::IgnoreCase))

		{

			return EPinContainerType::Map;

		}

		return EPinContainerType::None;

	}



	static UObject* ResolvePinSubCategoryObject(const FString& InPath)

	{

		if (InPath.TrimStartAndEnd().IsEmpty())

		{

			return nullptr;

		}

		if (UClass* AsClass = ResolveClassByPath(InPath))

		{

			return AsClass;

		}

		return LoadAssetObject(InPath);

	}

}



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

	UEdGraphPin* FromPin = UeAgentBlueprintOps::FindNodePinByName(FromNode, FromPinName);

	UEdGraphPin* ToPin = UeAgentBlueprintOps::FindNodePinByName(ToNode, ToPinName);

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

	PinType.PinCategory = UeAgentBlueprintOps::NormalizePinCategoryName(PinCategoryText);

	if (PinType.PinCategory.IsNone())

	{

		OutError = TEXT("invalid_pin_category");

		return false;

	}

	FString PinSubCategoryText;

	JsonTryGetString(Ctx.Params, TEXT("pin_subcategory"), PinSubCategoryText);

	PinSubCategoryText.TrimStartAndEndInline();

	if (!PinSubCategoryText.IsEmpty())

	{

		PinType.PinSubCategory = UeAgentBlueprintOps::NormalizeRealPinSubCategoryName(PinSubCategoryText);

	}



	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real)

	{

		// UE5 的 PC_Real 变量必须带 float/double 子类别，否则编译期会触发断言崩溃。
		if (PinType.PinSubCategory.IsNone())
		{

			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;

		}

		if (PinType.PinSubCategory != UEdGraphSchema_K2::PC_Float && PinType.PinSubCategory != UEdGraphSchema_K2::PC_Double)

		{

			OutError = TEXT("invalid_real_pin_subcategory");

			return false;

		}

	}



	FString PinSubCategoryObjectPath;

	JsonTryGetString(Ctx.Params, TEXT("pin_subcategory_object"), PinSubCategoryObjectPath);

	if (!PinSubCategoryObjectPath.TrimStartAndEnd().IsEmpty())

	{

		UObject* SubCategoryObject = UeAgentBlueprintOps::ResolvePinSubCategoryObject(PinSubCategoryObjectPath);

		if (!SubCategoryObject)

		{

			OutError = TEXT("invalid_pin_subcategory_object");

			return false;

		}

		PinType.PinSubCategoryObject = SubCategoryObject;

	}



	FString ContainerTypeText;

	JsonTryGetString(Ctx.Params, TEXT("container_type"), ContainerTypeText);

	PinType.ContainerType = UeAgentBlueprintOps::ParsePinContainerType(ContainerTypeText);



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



bool FUeAgentHttpServer::CmdBlueprintAddComponentBoundEvent(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString ComponentName;

	if (!JsonTryGetString(Ctx.Params, TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())

	{

		OutError = TEXT("missing_component_name");

		return false;

	}

	FString DelegatePropertyName;

	if (!JsonTryGetString(Ctx.Params, TEXT("delegate_property_name"), DelegatePropertyName) || DelegatePropertyName.IsEmpty())

	{

		OutError = TEXT("missing_delegate_property_name");

		return false;

	}

	FString DelegateOwnerClassPath = TEXT("/Script/Engine.PrimitiveComponent");

	JsonTryGetString(Ctx.Params, TEXT("delegate_owner_class"), DelegateOwnerClassPath);

	UClass* DelegateOwnerClass = UeAgentBlueprintOps::ResolveClassByPath(DelegateOwnerClassPath);

	if (!DelegateOwnerClass)

	{

		OutError = TEXT("invalid_delegate_owner_class");

		return false;

	}



	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	if (!Blueprint->SkeletonGeneratedClass)

	{

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

	}

	FObjectProperty* ComponentProperty = FindFProperty<FObjectProperty>(Blueprint->SkeletonGeneratedClass, FName(*ComponentName));

	if (!ComponentProperty)

	{

		OutError = TEXT("component_property_not_found");

		return false;

	}

	if (const UK2Node_ComponentBoundEvent* ExistingNode = FKismetEditorUtilities::FindBoundEventForComponent(Blueprint, FName(*DelegatePropertyName), FName(*ComponentName)))

	{

		OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

		OutData->SetStringField(TEXT("component_name"), ComponentName);

		OutData->SetStringField(TEXT("delegate_property_name"), DelegatePropertyName);

		OutData->SetStringField(TEXT("node_guid"), ExistingNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

		OutData->SetBoolField(TEXT("changed"), false);

		return true;

	}



	FKismetEditorUtilities::CreateNewBoundEventForClass(DelegateOwnerClass, FName(*DelegatePropertyName), Blueprint, ComponentProperty);

	const UK2Node_ComponentBoundEvent* CreatedNode = FKismetEditorUtilities::FindBoundEventForComponent(Blueprint, FName(*DelegatePropertyName), FName(*ComponentName));

	if (!CreatedNode)

	{

		OutError = TEXT("create_component_bound_event_failed");

		return false;

	}

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

	OutData->SetStringField(TEXT("component_name"), ComponentName);

	OutData->SetStringField(TEXT("delegate_property_name"), DelegatePropertyName);

	OutData->SetStringField(TEXT("node_guid"), CreatedNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	OutData->SetBoolField(TEXT("changed"), true);

	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);

	return true;

}

bool FUeAgentHttpServer::CmdBlueprintAddEnhancedInputActionEvent(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString InputActionPath;

	if (!JsonTryGetString(Ctx.Params, TEXT("input_action"), InputActionPath) || InputActionPath.IsEmpty())

	{

		OutError = TEXT("missing_input_action");

		return false;

	}

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

	UInputAction* InputAction = Cast<UInputAction>(UeAgentBlueprintOps::LoadAssetObject(InputActionPath));

	if (!InputAction)

	{

		OutError = TEXT("input_action_not_found");

		return false;

	}

	TArray<UK2Node_EnhancedInputAction*> ExistingNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, ExistingNodes);
	for (UK2Node_EnhancedInputAction* ExistingNode : ExistingNodes)
	{
		if (ExistingNode && ExistingNode->GetGraph() == Graph && ExistingNode->InputAction == InputAction)
		{
			OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
			OutData->SetStringField(TEXT("graph_name"), Graph->GetName());
			OutData->SetStringField(TEXT("graph_path"), Graph->GetPathName());
			OutData->SetStringField(TEXT("input_action"), InputAction->GetPathName());
			OutData->SetStringField(TEXT("node_guid"), ExistingNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			OutData->SetBoolField(TEXT("changed"), false);
			return true;
		}
	}

	double XNum = 0.0;
	double YNum = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum);
	JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum);

	Blueprint->Modify();
	Graph->Modify();

	UK2Node_EnhancedInputAction* NewNode = NewObject<UK2Node_EnhancedInputAction>(Graph);
	if (!NewNode)
	{
		OutError = TEXT("create_enhanced_input_action_node_failed");
		return false;
	}

	NewNode->InputAction = InputAction;
	Graph->AddNode(NewNode, true, false);
	NewNode->CreateNewGuid();
	NewNode->PostPlacedNewNode();
	NewNode->SetFlags(RF_Transactional);
	NewNode->NodePosX = (int32)XNum;
	NewNode->NodePosY = (int32)YNum;
	NewNode->AllocateDefaultPins();
	NewNode->PostReconstructNode();
	UEdGraphSchema_K2::SetNodeMetaData(NewNode, FNodeMetadata::DefaultGraphNode);

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
	OutData->SetStringField(TEXT("input_action"), InputAction->GetPathName());
	OutData->SetStringField(TEXT("node_guid"), NewNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);

	return true;

}

bool FUeAgentHttpServer::CmdBlueprintAddEnhancedInputLocalPlayerSubsystemNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

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

	UFunction* Function = USubsystemBlueprintLibrary::StaticClass()->FindFunctionByName(TEXT("GetLocalPlayerSubSystemFromPlayerController"));

	if (!Function)

	{

		OutError = TEXT("enhanced_input_local_player_subsystem_function_not_found");

		return false;

	}

	double XNum = 0.0;
	double YNum = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum);
	JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum);

	Blueprint->Modify();
	Graph->Modify();

	UK2Node_CallFunction* CallFunctionNode = NewObject<UK2Node_CallFunction>(Graph);

	if (!CallFunctionNode)

	{

		OutError = TEXT("create_enhanced_input_local_player_subsystem_node_failed");

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

	UEdGraphPin* ClassPin = UeAgentBlueprintOps::FindNodePinByName(CallFunctionNode, TEXT("Class"));
	if (!ClassPin)
	{
		OutError = TEXT("enhanced_input_local_player_subsystem_class_pin_not_found");
		return false;
	}

	if (const UEdGraphSchema* Schema = Graph->GetSchema())
	{
		Schema->TrySetDefaultObject(*ClassPin, UEnhancedInputLocalPlayerSubsystem::StaticClass());
	}
	else
	{
		ClassPin->DefaultObject = UEnhancedInputLocalPlayerSubsystem::StaticClass();
		ClassPin->DefaultValue = UEnhancedInputLocalPlayerSubsystem::StaticClass()->GetPathName();
	}
	CallFunctionNode->ReconstructNode();

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
	OutData->SetStringField(TEXT("graph_path"), Graph->GetPathName());
	OutData->SetStringField(TEXT("function_owner_class"), USubsystemBlueprintLibrary::StaticClass()->GetPathName());
	OutData->SetStringField(TEXT("function_name"), Function->GetName());
	OutData->SetStringField(TEXT("configured_class"), UEnhancedInputLocalPlayerSubsystem::StaticClass()->GetPathName());
	OutData->SetStringField(TEXT("node_guid"), CallFunctionNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);

	return true;

}

bool FUeAgentHttpServer::CmdBlueprintAddEnhancedInputAddMappingContextNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString MappingContextPath;

	if (!JsonTryGetString(Ctx.Params, TEXT("mapping_context"), MappingContextPath) || MappingContextPath.IsEmpty())

	{

		OutError = TEXT("missing_mapping_context");

		return false;

	}

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

	UInputMappingContext* MappingContext = Cast<UInputMappingContext>(UeAgentBlueprintOps::LoadAssetObject(MappingContextPath));

	if (!MappingContext)

	{

		OutError = TEXT("mapping_context_not_found");

		return false;

	}

	UFunction* Function = UEnhancedInputLocalPlayerSubsystem::StaticClass()->FindFunctionByName(TEXT("AddMappingContext"));

	if (!Function)

	{

		OutError = TEXT("enhanced_input_add_mapping_context_function_not_found");

		return false;

	}

	double PriorityNum = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("priority"), PriorityNum);
	const FString DefaultOptionsValue = TEXT("(bIgnoreAllPressedKeysUntilRelease=True,bForceImmediately=False,bNotifyUserSettings=False)");
	FString OptionsDefaultValue = DefaultOptionsValue;
	JsonTryGetString(Ctx.Params, TEXT("options_default_value"), OptionsDefaultValue);

	double XNum = 0.0;
	double YNum = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum);
	JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum);

	Blueprint->Modify();
	Graph->Modify();

	UK2Node_CallFunction* CallFunctionNode = NewObject<UK2Node_CallFunction>(Graph);

	if (!CallFunctionNode)

	{

		OutError = TEXT("create_enhanced_input_add_mapping_context_node_failed");

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

	UEdGraphPin* MappingContextPin = UeAgentBlueprintOps::FindNodePinByName(CallFunctionNode, TEXT("MappingContext"));
	UEdGraphPin* PriorityPin = UeAgentBlueprintOps::FindNodePinByName(CallFunctionNode, TEXT("Priority"));
	UEdGraphPin* OptionsPin = UeAgentBlueprintOps::FindNodePinByName(CallFunctionNode, TEXT("Options"));
	if (!MappingContextPin || !PriorityPin || !OptionsPin)
	{
		OutError = TEXT("enhanced_input_add_mapping_context_pin_not_found");
		return false;
	}

	if (const UEdGraphSchema* Schema = Graph->GetSchema())
	{
		Schema->TrySetDefaultObject(*MappingContextPin, MappingContext);
		Schema->TrySetDefaultValue(*PriorityPin, LexToString(FMath::RoundToInt(PriorityNum)));
		Schema->TrySetDefaultValue(*OptionsPin, OptionsDefaultValue);
	}
	else
	{
		MappingContextPin->DefaultObject = MappingContext;
		MappingContextPin->DefaultValue = MappingContext->GetPathName();
		PriorityPin->DefaultValue = LexToString(FMath::RoundToInt(PriorityNum));
		OptionsPin->DefaultValue = OptionsDefaultValue;
	}
	CallFunctionNode->ReconstructNode();

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
	OutData->SetStringField(TEXT("graph_path"), Graph->GetPathName());
	OutData->SetStringField(TEXT("function_owner_class"), UEnhancedInputLocalPlayerSubsystem::StaticClass()->GetPathName());
	OutData->SetStringField(TEXT("function_name"), Function->GetName());
	OutData->SetStringField(TEXT("mapping_context"), MappingContext->GetPathName());
	OutData->SetNumberField(TEXT("priority"), FMath::RoundToInt(PriorityNum));
	OutData->SetStringField(TEXT("options_default_value"), OptionsDefaultValue);
	OutData->SetStringField(TEXT("node_guid"), CallFunctionNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);

	return true;

}

bool FUeAgentHttpServer::CmdBlueprintAddDynamicCastNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString TargetClassPath;

	if (!JsonTryGetString(Ctx.Params, TEXT("target_class"), TargetClassPath) || TargetClassPath.IsEmpty())

	{

		OutError = TEXT("missing_target_class");

		return false;

	}

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

	UClass* TargetClass = UeAgentBlueprintOps::ResolveClassByPath(TargetClassPath);

	if (!TargetClass || !TargetClass->IsChildOf(UObject::StaticClass()))

	{

		OutError = TEXT("invalid_target_class");

		return false;

	}

	bool bPureCast = true;
	JsonTryGetBool(Ctx.Params, TEXT("pure"), bPureCast);

	double XNum = 0.0;
	double YNum = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum);
	JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum);

	Blueprint->Modify();
	Graph->Modify();

	UK2Node_DynamicCast* NewNode = NewObject<UK2Node_DynamicCast>(Graph);
	if (!NewNode)
	{
		OutError = TEXT("create_dynamic_cast_node_failed");
		return false;
	}

	NewNode->TargetType = TargetClass;
	Graph->AddNode(NewNode, true, false);
	NewNode->CreateNewGuid();
	NewNode->PostPlacedNewNode();
	NewNode->SetFlags(RF_Transactional);
	NewNode->NodePosX = (int32)XNum;
	NewNode->NodePosY = (int32)YNum;
	NewNode->AllocateDefaultPins();
	NewNode->SetPurity(bPureCast);
	NewNode->PostReconstructNode();
	UEdGraphSchema_K2::SetNodeMetaData(NewNode, FNodeMetadata::DefaultGraphNode);

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
	OutData->SetStringField(TEXT("target_class"), TargetClass->GetPathName());
	OutData->SetStringField(TEXT("node_guid"), NewNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("pure"), bPureCast);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);

	return true;

}



bool FUeAgentHttpServer::CmdBlueprintAddFunctionGraph(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString FunctionName;

	if (!JsonTryGetString(Ctx.Params, TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())

	{

		OutError = TEXT("missing_function_name");

		return false;

	}



	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	if (UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, FunctionName))

	{

		OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

		OutData->SetStringField(TEXT("graph_name"), FunctionName);

		OutData->SetBoolField(TEXT("changed"), false);

		return true;

	}



	UEdGraph* NewGraph = UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, FunctionName);

	if (!NewGraph)

	{

		OutError = TEXT("add_function_graph_failed");

		return false;

	}

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

	OutData->SetStringField(TEXT("graph_name"), NewGraph->GetName());

	OutData->SetStringField(TEXT("graph_path"), NewGraph->GetPathName());

	OutData->SetBoolField(TEXT("changed"), true);

	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);

	return true;

}



bool FUeAgentHttpServer::CmdBlueprintAddMacroGraph(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString MacroName;

	if (!JsonTryGetString(Ctx.Params, TEXT("macro_name"), MacroName) || MacroName.IsEmpty())

	{

		OutError = TEXT("missing_macro_name");

		return false;

	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	for (UEdGraph* Graph : Blueprint->MacroGraphs)

	{

		if (Graph && Graph->GetName().Equals(MacroName, ESearchCase::IgnoreCase))

		{

			OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

			OutData->SetStringField(TEXT("graph_name"), Graph->GetName());

			OutData->SetStringField(TEXT("graph_path"), Graph->GetPathName());

			OutData->SetBoolField(TEXT("changed"), false);

			return true;

		}

	}



	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*MacroName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

	if (!NewGraph)

	{

		OutError = TEXT("create_macro_graph_failed");

		return false;

	}

	FBlueprintEditorUtils::AddMacroGraph(Blueprint, NewGraph, true, nullptr);

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

	OutData->SetStringField(TEXT("graph_name"), NewGraph->GetName());

	OutData->SetStringField(TEXT("graph_path"), NewGraph->GetPathName());

	OutData->SetBoolField(TEXT("changed"), true);

	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);

	return true;

}



bool FUeAgentHttpServer::CmdBlueprintAddEventDispatcher(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString DispatcherName;

	if (!JsonTryGetString(Ctx.Params, TEXT("dispatcher_name"), DispatcherName) || DispatcherName.IsEmpty())

	{

		OutError = TEXT("missing_dispatcher_name");

		return false;

	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}



	const FName DispatcherFName(*DispatcherName);

	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, DispatcherFName) != INDEX_NONE)

	{

		OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

		OutData->SetStringField(TEXT("dispatcher_name"), DispatcherName);

		OutData->SetBoolField(TEXT("changed"), false);

		return true;

	}



	FEdGraphPinType DelegatePinType;

	DelegatePinType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;

	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, DispatcherFName, DelegatePinType, FString()))

	{

		OutError = TEXT("add_event_dispatcher_failed");

		return false;

	}

	FBlueprintEditorUtils::ConformDelegateSignatureGraphs(Blueprint);

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

	OutData->SetStringField(TEXT("dispatcher_name"), DispatcherName);

	OutData->SetBoolField(TEXT("changed"), true);

	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);

	return true;

}



bool FUeAgentHttpServer::CmdBlueprintReparent(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString ParentClassPath;

	if (!JsonTryGetString(Ctx.Params, TEXT("parent_class"), ParentClassPath) || ParentClassPath.IsEmpty())

	{

		OutError = TEXT("missing_parent_class");

		return false;

	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	UClass* NewParentClass = UeAgentBlueprintOps::ResolveClassByPath(ParentClassPath);

	if (!NewParentClass)

	{

		OutError = TEXT("invalid_parent_class");

		return false;

	}

	UBlueprintEditorLibrary::ReparentBlueprint(Blueprint, NewParentClass);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);



	bool bCompileAfterReparent = true;

	JsonTryGetBool(Ctx.Params, TEXT("compile_after_reparent"), bCompileAfterReparent);

	if (bCompileAfterReparent)

	{

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

	}

	bool bSaveAfterReparent = false;

	JsonTryGetBool(Ctx.Params, TEXT("save_after_reparent"), bSaveAfterReparent);

	if (bSaveAfterReparent && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))

	{

		return false;

	}



	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

	OutData->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));

	OutData->SetBoolField(TEXT("saved"), bSaveAfterReparent);

	return true;

}



bool FUeAgentHttpServer::CmdBlueprintRemoveComponent(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString ComponentName;

	if (!JsonTryGetString(Ctx.Params, TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())

	{

		OutError = TEXT("missing_component_name");

		return false;

	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;

	if (!SCS)

	{

		OutError = TEXT("blueprint_has_no_scs");

		return false;

	}

	USCS_Node* TargetNode = UeAgentBlueprintOps::FindSCSNodeByAnyName(SCS, ComponentName);

	if (!TargetNode)

	{

		OutError = TEXT("component_not_found");

		return false;

	}

	SCS->RemoveNodeAndPromoteChildren(TargetNode);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);



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

	OutData->SetStringField(TEXT("component_name"), ComponentName);

	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);

	return true;

}



bool FUeAgentHttpServer::CmdBlueprintSetCdoProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const

{

	FString AssetPathInput;

	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())

	{

		OutError = TEXT("missing_asset_path");

		return false;

	}

	FString PropertyName;

	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())

	{

		OutError = TEXT("missing_property_name");

		return false;

	}

	FString ValueText;

	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))

	{

		OutError = TEXT("missing_value_text");

		return false;

	}



	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);

	if (!Blueprint)

	{

		OutError = TEXT("blueprint_not_found");

		return false;

	}

	if (!Blueprint->GeneratedClass)

	{

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

	}

	UObject* Cdo = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;

	if (!Cdo)

	{

		OutError = TEXT("cdo_not_found");

		return false;

	}



	FProperty* Property = nullptr;

	void* ValuePtr = nullptr;

	if (!UeAgentBlueprintOps::ResolvePropertyPath(Cdo, PropertyName, Property, ValuePtr) || !Property || !ValuePtr)

	{

		OutError = TEXT("property_not_found");

		return false;

	}

	if (!Property->ImportText_Direct(*ValueText, ValuePtr, Cdo, PPF_None))

	{
		const FString ImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyName, *ValueText);

		OutData->SetStringField(TEXT("property_name"), PropertyName);

		OutData->SetStringField(TEXT("value_text"), ValueText);

		SetPropertyImportResultFields(OutData, Property, ValueText, TEXT(""), TEXT("import_failed"), ImportError);

		OutError = ImportError;

		return false;

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



	if (bCompileAfterSet)

	{

		Cdo = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;

		Property = nullptr;

		ValuePtr = nullptr;

		if (!Cdo || !UeAgentBlueprintOps::ResolvePropertyPath(Cdo, PropertyName, Property, ValuePtr) || !Property || !ValuePtr)

		{

			OutError = TEXT("property_not_found_after_compile");

			return false;

		}

	}



	FString ExportedValue;

	Property->ExportTextItem_Direct(ExportedValue, ValuePtr, ValuePtr, Cdo, PPF_None);

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());

	OutData->SetStringField(TEXT("property_name"), PropertyName);

	OutData->SetStringField(TEXT("value_text"), ValueText);

	OutData->SetStringField(TEXT("applied_value_text"), ExportedValue);

	SetPropertyImportResultFields(OutData, Property, ValueText, ExportedValue, TEXT("imported_and_read_back"));

	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);

	return true;

}
