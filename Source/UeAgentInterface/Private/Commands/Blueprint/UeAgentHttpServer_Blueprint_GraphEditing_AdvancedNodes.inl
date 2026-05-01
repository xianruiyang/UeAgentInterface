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
