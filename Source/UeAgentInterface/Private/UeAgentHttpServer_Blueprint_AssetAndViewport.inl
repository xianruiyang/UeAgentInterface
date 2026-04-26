bool FUeAgentHttpServer::ExecuteBlueprintCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("blueprint_create")) return CmdBlueprintCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_compile")) return CmdBlueprintCompile(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_get_compile_log")) return CmdBlueprintGetCompileLog(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_get_info")) return CmdBlueprintGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_list_graphs")) return CmdBlueprintListGraphs(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_export_folder")) return CmdBlueprintExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_apply_folder")) return CmdBlueprintApplyFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_inspect_components")) return CmdBlueprintInspectComponents(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_inspect_nodes")) return CmdBlueprintInspectNodes(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_component")) return CmdBlueprintAddComponent(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_set_component_property")) return CmdBlueprintSetComponentProperty(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_event_node")) return CmdBlueprintAddEventNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_custom_event_node")) return CmdBlueprintAddCustomEventNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_node_by_class")) return CmdBlueprintAddNodeByClass(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_call_function_node")) return CmdBlueprintAddCallFunctionNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_connect_pins")) return CmdBlueprintConnectPins(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_disconnect_pins")) return CmdBlueprintDisconnectPins(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_set_pin_default_value")) return CmdBlueprintSetPinDefaultValue(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_remove_node")) return CmdBlueprintRemoveNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_variable")) return CmdBlueprintAddVariable(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_remove_variable")) return CmdBlueprintRemoveVariable(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_variable_node")) return CmdBlueprintAddVariableNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_component_bound_event")) return CmdBlueprintAddComponentBoundEvent(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_enhanced_input_action_event")) return CmdBlueprintAddEnhancedInputActionEvent(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_enhanced_input_local_player_subsystem_node")) return CmdBlueprintAddEnhancedInputLocalPlayerSubsystemNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_enhanced_input_add_mapping_context_node")) return CmdBlueprintAddEnhancedInputAddMappingContextNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_dynamic_cast_node")) return CmdBlueprintAddDynamicCastNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_function_graph")) return CmdBlueprintAddFunctionGraph(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_macro_graph")) return CmdBlueprintAddMacroGraph(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_add_event_dispatcher")) return CmdBlueprintAddEventDispatcher(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_graph_get_view")) return CmdBlueprintGraphGetView(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_graph_set_view")) return CmdBlueprintGraphSetView(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_viewport_get_camera")) return CmdBlueprintViewportGetCamera(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_viewport_set_camera")) return CmdBlueprintViewportSetCamera(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_screenshot")) return CmdBlueprintScreenshot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_reparent")) return CmdBlueprintReparent(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_remove_component")) return CmdBlueprintRemoveComponent(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_set_cdo_property")) return CmdBlueprintSetCdoProperty(Ctx, OutData, OutError);

	OutError = TEXT("unknown_blueprint_command");
	return false;
}

bool FUeAgentHttpServer::CmdBlueprintCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = UeAgentBlueprintOps::NormalizeAssetPath(AssetPathInput);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("invalid_asset_name");
		return false;
	}

	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	if (UeAgentBlueprintOps::AssetExists(ObjectPath))
	{
		OutError = TEXT("asset_already_exists");
		return false;
	}

	FString ParentClassPath = TEXT("/Script/Engine.Actor");
	JsonTryGetString(Ctx.Params, TEXT("parent_class"), ParentClassPath);
	UClass* ParentClass = UeAgentBlueprintOps::ResolveClassByPath(ParentClassPath);
	if (!ParentClass || !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
	{
		OutError = TEXT("invalid_parent_class");
		return false;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = TEXT("create_package_failed");
		return false;
	}

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		*AssetName,
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("UeAgentInterface")));
	if (!Blueprint)
	{
		OutError = TEXT("create_blueprint_failed");
		return false;
	}

	FAssetRegistryModule::AssetCreated(Blueprint);
	Blueprint->MarkPackageDirty();
	Package->MarkPackageDirty();

	bool bCompileAfterCreate = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_create"), bCompileAfterCreate);
	if (bCompileAfterCreate)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
		{
			AssetEditorSubsystem->OpenEditorForAsset(Blueprint);
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), PackageName);
	OutData->SetStringField(TEXT("object_path"), Blueprint->GetPathName());
	OutData->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("status"), UeAgentBlueprintOps::BlueprintStatusToString(Blueprint->Status));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintCompile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}

	FCompilerResultsLog CompilerLog;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);

	FString SeverityFilterText = TEXT("all");
	JsonTryGetString(Ctx.Params, TEXT("severity_filter"), SeverityFilterText);
	UeAgentBlueprintOps::ECompilerMessageSeverityFilter SeverityFilter = UeAgentBlueprintOps::ECompilerMessageSeverityFilter::All;
	if (!UeAgentBlueprintOps::ParseCompilerSeverityFilter(SeverityFilterText, SeverityFilter))
	{
		OutError = TEXT("invalid_severity_filter");
		return false;
	}

	int32 MaxMessages = 200;
	double MaxMessagesNumber = static_cast<double>(MaxMessages);
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_messages"), MaxMessagesNumber))
	{
		MaxMessages = FMath::Clamp(FMath::RoundToInt(MaxMessagesNumber), 1, 2000);
	}

	bool bIncludeMessages = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_messages"), bIncludeMessages);

	bool bSaveAfterCompile = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_compile"), bSaveAfterCompile);
	if (bSaveAfterCompile && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("status"), UeAgentBlueprintOps::BlueprintStatusToString(Blueprint->Status));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCompile);
	OutData->SetNumberField(TEXT("error_count"), CompilerLog.NumErrors);
	OutData->SetNumberField(TEXT("warning_count"), CompilerLog.NumWarnings);
	OutData->SetNumberField(TEXT("message_total_count"), CompilerLog.Messages.Num());
	OutData->SetBoolField(TEXT("has_error"), CompilerLog.NumErrors > 0 || Blueprint->Status == BS_Error);

	if (bIncludeMessages)
	{
		TArray<TSharedPtr<FJsonValue>> Messages;
		int32 FilteredErrorCount = 0;
		int32 FilteredWarningCount = 0;
		int32 FilteredInfoCount = 0;
		UeAgentBlueprintOps::BuildCompilerLogJson(CompilerLog, SeverityFilter, MaxMessages, Messages, FilteredErrorCount, FilteredWarningCount, FilteredInfoCount);

		OutData->SetStringField(TEXT("severity_filter"), SeverityFilterText);
		OutData->SetNumberField(TEXT("max_messages"), MaxMessages);
		OutData->SetNumberField(TEXT("filtered_error_count"), FilteredErrorCount);
		OutData->SetNumberField(TEXT("filtered_warning_count"), FilteredWarningCount);
		OutData->SetNumberField(TEXT("filtered_info_count"), FilteredInfoCount);
		OutData->SetNumberField(TEXT("filtered_message_count"), FilteredErrorCount + FilteredWarningCount + FilteredInfoCount);
		OutData->SetArrayField(TEXT("messages"), Messages);
	}

	return true;
}

bool FUeAgentHttpServer::CmdBlueprintGetCompileLog(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}

	FString SeverityFilterText = TEXT("all");
	JsonTryGetString(Ctx.Params, TEXT("severity_filter"), SeverityFilterText);
	UeAgentBlueprintOps::ECompilerMessageSeverityFilter SeverityFilter = UeAgentBlueprintOps::ECompilerMessageSeverityFilter::All;
	if (!UeAgentBlueprintOps::ParseCompilerSeverityFilter(SeverityFilterText, SeverityFilter))
	{
		OutError = TEXT("invalid_severity_filter");
		return false;
	}

	int32 MaxMessages = 200;
	double MaxMessagesNumber = static_cast<double>(MaxMessages);
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_messages"), MaxMessagesNumber))
	{
		MaxMessages = FMath::Clamp(FMath::RoundToInt(MaxMessagesNumber), 1, 2000);
	}

	bool bSaveAfterCompile = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_compile"), bSaveAfterCompile);

	FCompilerResultsLog CompilerLog;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);

	if (bSaveAfterCompile && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Messages;
	int32 FilteredErrorCount = 0;
	int32 FilteredWarningCount = 0;
	int32 FilteredInfoCount = 0;
	UeAgentBlueprintOps::BuildCompilerLogJson(CompilerLog, SeverityFilter, MaxMessages, Messages, FilteredErrorCount, FilteredWarningCount, FilteredInfoCount);

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("status"), UeAgentBlueprintOps::BlueprintStatusToString(Blueprint->Status));
	OutData->SetStringField(TEXT("severity_filter"), SeverityFilterText);
	OutData->SetNumberField(TEXT("max_messages"), MaxMessages);
	OutData->SetNumberField(TEXT("message_total_count"), CompilerLog.Messages.Num());
	OutData->SetNumberField(TEXT("filtered_error_count"), FilteredErrorCount);
	OutData->SetNumberField(TEXT("filtered_warning_count"), FilteredWarningCount);
	OutData->SetNumberField(TEXT("filtered_info_count"), FilteredInfoCount);
	OutData->SetNumberField(TEXT("filtered_message_count"), FilteredErrorCount + FilteredWarningCount + FilteredInfoCount);
	OutData->SetNumberField(TEXT("error_count"), CompilerLog.NumErrors);
	OutData->SetNumberField(TEXT("warning_count"), CompilerLog.NumWarnings);
	OutData->SetBoolField(TEXT("has_error"), CompilerLog.NumErrors > 0 || Blueprint->Status == BS_Error);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCompile);
	OutData->SetArrayField(TEXT("messages"), Messages);
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Blueprint->GetPathName());
	OutData->SetStringField(TEXT("name"), Blueprint->GetName());
	OutData->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("status"), UeAgentBlueprintOps::BlueprintStatusToString(Blueprint->Status));
	OutData->SetBoolField(TEXT("is_data_only"), FBlueprintEditorUtils::IsDataOnlyBlueprint(Blueprint));
	OutData->SetBoolField(TEXT("is_dirty"), Blueprint->GetOutermost()->IsDirty());
	OutData->SetNumberField(TEXT("new_variable_count"), Blueprint->NewVariables.Num());
	OutData->SetNumberField(TEXT("ubergraph_count"), Blueprint->UbergraphPages.Num());
	OutData->SetNumberField(TEXT("function_graph_count"), Blueprint->FunctionGraphs.Num());
	OutData->SetNumberField(TEXT("macro_graph_count"), Blueprint->MacroGraphs.Num());
	OutData->SetNumberField(TEXT("delegate_graph_count"), Blueprint->DelegateSignatureGraphs.Num());
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintListGraphs(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Graphs;
	auto AppendGraphs = [&Graphs](const TArray<UEdGraph*>& InGraphs, const FString& TypeName)
	{
		for (UEdGraph* Graph : InGraphs)
		{
			if (!Graph)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("graph_name"), Graph->GetName());
			Item->SetStringField(TEXT("graph_path"), Graph->GetPathName());
			Item->SetStringField(TEXT("graph_type"), TypeName);
			Graphs.Add(MakeShared<FJsonValueObject>(Item));
		}
	};
	AppendGraphs(Blueprint->UbergraphPages, TEXT("ubergraph"));
	AppendGraphs(Blueprint->FunctionGraphs, TEXT("function"));
	AppendGraphs(Blueprint->MacroGraphs, TEXT("macro"));
	AppendGraphs(Blueprint->DelegateSignatureGraphs, TEXT("delegate"));

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetArrayField(TEXT("graphs"), Graphs);
	OutData->SetNumberField(TEXT("graph_count"), Graphs.Num());
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintGraphGetView(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}

	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);
	TSharedPtr<IBlueprintEditor> BlueprintEditor = UeAgentBlueprintOps::GetBlueprintEditor(Blueprint, bOpenEditorIfNeeded, OutError);
	if (!BlueprintEditor.IsValid())
	{
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

	TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(Graph, true);
	if (!GraphEditor.IsValid())
	{
		OutError = TEXT("graph_editor_not_found");
		return false;
	}

	FVector2f ViewLocation(0.0f, 0.0f);
	float ZoomAmount = 1.0f;
	GraphEditor->GetViewLocation(ViewLocation, ZoomAmount);

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("graph_name"), Graph->GetName());
	OutData->SetNumberField(TEXT("view_x"), ViewLocation.X);
	OutData->SetNumberField(TEXT("view_y"), ViewLocation.Y);
	OutData->SetNumberField(TEXT("zoom"), ZoomAmount);
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintGraphSetView(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}

	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);
	TSharedPtr<IBlueprintEditor> BlueprintEditor = UeAgentBlueprintOps::GetBlueprintEditor(Blueprint, bOpenEditorIfNeeded, OutError);
	if (!BlueprintEditor.IsValid())
	{
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

	TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(Graph, true);
	if (!GraphEditor.IsValid())
	{
		OutError = TEXT("graph_editor_not_found");
		return false;
	}

	FVector2f CurrentViewLocation(0.0f, 0.0f);
	float CurrentZoomAmount = 1.0f;
	GraphEditor->GetViewLocation(CurrentViewLocation, CurrentZoomAmount);

	double ViewX = CurrentViewLocation.X;
	double ViewY = CurrentViewLocation.Y;
	double ZoomAmount = CurrentZoomAmount;
	const bool bHasViewX = JsonTryGetNumber(Ctx.Params, TEXT("view_x"), ViewX);
	const bool bHasViewY = JsonTryGetNumber(Ctx.Params, TEXT("view_y"), ViewY);
	const bool bHasZoom = JsonTryGetNumber(Ctx.Params, TEXT("zoom"), ZoomAmount);
	if (!bHasViewX && !bHasViewY && !bHasZoom)
	{
		OutError = TEXT("missing_view_or_zoom");
		return false;
	}

	const float NewZoom = (float)FMath::Clamp(ZoomAmount, 0.05, 64.0);
	GraphEditor->SetViewLocation(FVector2f((float)ViewX, (float)ViewY), NewZoom);
	GraphEditor->GetViewLocation(CurrentViewLocation, CurrentZoomAmount);

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("graph_name"), Graph->GetName());
	OutData->SetNumberField(TEXT("view_x"), CurrentViewLocation.X);
	OutData->SetNumberField(TEXT("view_y"), CurrentViewLocation.Y);
	OutData->SetNumberField(TEXT("zoom"), CurrentZoomAmount);
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintViewportGetCamera(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}

	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);
	TSharedPtr<IBlueprintEditor> BlueprintEditor = UeAgentBlueprintOps::GetBlueprintEditor(Blueprint, bOpenEditorIfNeeded, OutError);
	if (!BlueprintEditor.IsValid())
	{
		return false;
	}

	TSharedPtr<SEditorViewport> ViewportWidget = UeAgentBlueprintOps::ResolveBlueprintViewportWidget(BlueprintEditor, OutError);
	if (!ViewportWidget.IsValid())
	{
		return false;
	}

	const TSharedPtr<FEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
	if (!ViewportClient.IsValid())
	{
		OutError = TEXT("blueprint_viewport_client_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("location"), VecToJson(ViewportClient->GetViewLocation()));
	OutData->SetObjectField(TEXT("rotation"), RotToJson(ViewportClient->GetViewRotation()));
	OutData->SetNumberField(TEXT("fov"), ViewportClient->ViewFOV);
	OutData->SetBoolField(TEXT("realtime"), ViewportClient->IsRealtime());
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintViewportSetCamera(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}

	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);
	TSharedPtr<IBlueprintEditor> BlueprintEditor = UeAgentBlueprintOps::GetBlueprintEditor(Blueprint, bOpenEditorIfNeeded, OutError);
	if (!BlueprintEditor.IsValid())
	{
		return false;
	}

	TSharedPtr<SEditorViewport> ViewportWidget = UeAgentBlueprintOps::ResolveBlueprintViewportWidget(BlueprintEditor, OutError);
	if (!ViewportWidget.IsValid())
	{
		return false;
	}

	const TSharedPtr<FEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
	if (!ViewportClient.IsValid())
	{
		OutError = TEXT("blueprint_viewport_client_not_found");
		return false;
	}

	FVector Location = ViewportClient->GetViewLocation();
	FRotator Rotation = ViewportClient->GetViewRotation();
	JsonTryGetVector(Ctx.Params, TEXT("location"), Location);
	JsonTryGetRotator(Ctx.Params, TEXT("rotation"), Rotation);

	double FovValue = ViewportClient->ViewFOV;
	if (JsonTryGetNumber(Ctx.Params, TEXT("fov"), FovValue))
	{
		ViewportClient->ViewFOV = (float)FMath::Clamp(FovValue, 5.0, 170.0);
	}

	ViewportClient->SetViewLocation(Location);
	ViewportClient->SetViewRotation(Rotation);
	ViewportClient->Invalidate();

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("location"), VecToJson(ViewportClient->GetViewLocation()));
	OutData->SetObjectField(TEXT("rotation"), RotToJson(ViewportClient->GetViewRotation()));
	OutData->SetNumberField(TEXT("fov"), ViewportClient->ViewFOV);
	OutData->SetBoolField(TEXT("realtime"), ViewportClient->IsRealtime());
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	if (!Settings || !Settings->bEnableScreenshots)
	{
		OutError = TEXT("screenshots_disabled");
		return false;
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}

	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);
	TSharedPtr<IBlueprintEditor> BlueprintEditor = UeAgentBlueprintOps::GetBlueprintEditor(Blueprint, bOpenEditorIfNeeded, OutError);
	if (!BlueprintEditor.IsValid())
	{
		return false;
	}

	FString Target = TEXT("viewport");
	JsonTryGetString(Ctx.Params, TEXT("target"), Target);
	Target = Target.TrimStartAndEnd().ToLower();

	FString GraphName = TEXT("EventGraph");
	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);

	FString Format = TEXT("jpg");
	int32 Quality = 85;
	int32 MaxSize = 1024;
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	double QualityNum = (double)Quality;
	if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), QualityNum))
	{
		Quality = FMath::Clamp((int32)QualityNum, 1, 100);
	}
	double MaxSizeNum = (double)MaxSize;
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MaxSizeNum))
	{
		MaxSize = FMath::Clamp((int32)MaxSizeNum, 64, 8192);
	}
	Format = Format.ToLower();
	if (Format == TEXT("jpeg"))
	{
		Format = TEXT("jpg");
	}
	if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
	{
		OutError = TEXT("unsupported_format");
		return false;
	}

	TSharedPtr<SWidget> WidgetToShot;
	TSharedPtr<SEditorViewport> ViewportWidgetForShot;
	TSharedPtr<SWidget> ViewportSceneWidgetForShot;
	TSharedPtr<SWindow> BlueprintWindowForShot;
	FString EffectiveTarget = Target;
	if (Target == TEXT("viewport") || Target == TEXT("components"))
	{
		ViewportWidgetForShot = UeAgentBlueprintOps::ResolveBlueprintViewportWidget(BlueprintEditor, OutError);
		if (!ViewportWidgetForShot.IsValid())
		{
			return false;
		}
		WidgetToShot = ViewportWidgetForShot;
		ViewportSceneWidgetForShot = UeAgentBlueprintOps::ResolveBlueprintViewportSceneWidget(ViewportWidgetForShot.ToSharedRef());
		EffectiveTarget = TEXT("viewport");
	}
	else if (Target == TEXT("eventgraph") || Target == TEXT("event_graph"))
	{
		UEdGraph* Graph = UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, TEXT("EventGraph"));
		if (!Graph)
		{
			OutError = TEXT("graph_not_found");
			return false;
		}
		TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(Graph, true);
		if (!GraphEditor.IsValid())
		{
			OutError = TEXT("graph_editor_not_found");
			return false;
		}
		WidgetToShot = GraphEditor;
		GraphName = Graph->GetName();
		EffectiveTarget = TEXT("graph");
	}
	else if (Target == TEXT("graph") || Target.StartsWith(TEXT("graph:")))
	{
		if (Target.StartsWith(TEXT("graph:")))
		{
			GraphName = Target.Mid(6).TrimStartAndEnd();
		}

		UEdGraph* Graph = UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, GraphName);
		if (!Graph)
		{
			OutError = TEXT("graph_not_found");
			return false;
		}
		TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(Graph, true);
		if (!GraphEditor.IsValid())
		{
			OutError = TEXT("graph_editor_not_found");
			return false;
		}
		WidgetToShot = GraphEditor;
		GraphName = Graph->GetName();
		EffectiveTarget = TEXT("graph");
	}
	else if (Target == TEXT("window"))
	{
		BlueprintWindowForShot = UeAgentBlueprintOps::ResolveBlueprintEditorWindow(BlueprintEditor, OutError);
		if (!BlueprintWindowForShot.IsValid())
		{
			return false;
		}
		WidgetToShot = BlueprintWindowForShot;
		EffectiveTarget = TEXT("window");
	}
	else
	{
		OutError = TEXT("invalid_target");
		return false;
	}

	if (!WidgetToShot.IsValid())
	{
		OutError = TEXT("target_widget_not_found");
		return false;
	}

	TArray<FColor> Pixels;
	FIntPoint ShotSize(0, 0);
	bool bShotOk = false;
	FString CaptureMode = TEXT("slate_widget");
	if (EffectiveTarget == TEXT("viewport") && ViewportWidgetForShot.IsValid())
	{
		UeAgentBlueprintOps::PrepareBlueprintViewportForCapture(BlueprintEditor, Blueprint, ViewportWidgetForShot);

		if (!BlueprintWindowForShot.IsValid())
		{
			FString WindowError;
			BlueprintWindowForShot = UeAgentBlueprintOps::ResolveBlueprintEditorWindow(BlueprintEditor, WindowError);
			if (!BlueprintWindowForShot.IsValid() && !WindowError.IsEmpty())
			{
				FUeAgentInterfaceLogger::Log(TEXT("CmdBlueprintScreenshot viewport window resolve failed before capture: %s"), *WindowError);
			}
		}
		if (BlueprintWindowForShot.IsValid())
		{
			UeAgentBlueprintOps::PrepareSlateWindowForCapture(BlueprintWindowForShot);
		}

		const TSharedRef<SWidget> CropWidget = ViewportSceneWidgetForShot.IsValid() ? ViewportSceneWidgetForShot.ToSharedRef() : WidgetToShot.ToSharedRef();
		if (ViewportSceneWidgetForShot.IsValid())
		{
			const FVector2D SceneWidgetSize = ViewportSceneWidgetForShot->GetTickSpaceGeometry().GetAbsoluteSize();
			FUeAgentInterfaceLogger::Log(
				TEXT("CmdBlueprintScreenshot viewport crop_widget=%s scene_widget_abs_size=(%.1f,%.1f)"),
				*ViewportSceneWidgetForShot->GetTypeAsString(),
				SceneWidgetSize.X,
				SceneWidgetSize.Y);
		}
		else
		{
			FUeAgentInterfaceLogger::Log(TEXT("CmdBlueprintScreenshot viewport crop_widget=fallback_editor_viewport"));
		}

		bShotOk = UeAgentBlueprintOps::ScreenshotBlueprintWindowCropWidget(BlueprintEditor, CropWidget, Pixels, ShotSize, OutError);
		if (!bShotOk)
		{
			const FString CropError = OutError;
			FUeAgentInterfaceLogger::Log(TEXT("CmdBlueprintScreenshot viewport window-crop failed: %s, fallback to read-pixels"), *OutError);
			FString FallbackError;
			if (UeAgentBlueprintOps::ScreenshotEditorViewportPixels(ViewportWidgetForShot.ToSharedRef(), Pixels, ShotSize, FallbackError))
			{
				bShotOk = true;
				CaptureMode = TEXT("viewport_readpixels_fallback");
				OutError.Reset();
			}
			else if (UeAgentBlueprintOps::ScreenshotBlueprintWindow(BlueprintEditor, Pixels, ShotSize, FallbackError))
			{
				bShotOk = true;
				CaptureMode = TEXT("window_fallback");
				OutError.Reset();
			}
			else
			{
				OutError = FallbackError.IsEmpty() ? CropError : FallbackError;
			}
		}
		else
		{
			CaptureMode = TEXT("viewport_window_crop");
		}
	}
	else
	{
		bShotOk = UeAgentBlueprintOps::ScreenshotSlateWidget(WidgetToShot.ToSharedRef(), Pixels, ShotSize, OutError);
	}

	if (!bShotOk)
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdBlueprintScreenshot failed target=%s asset=%s error=%s"), *EffectiveTarget, *Blueprint->GetOutermost()->GetName(), *OutError);
		return false;
	}

	TArray<FColor> ResizedPixels;
	FIntPoint ResizedSize(0, 0);
	if (!ResizePixelsMaxSize(Pixels, ShotSize, MaxSize, ResizedPixels, ResizedSize, OutError))
	{
		return false;
	}

	const FString OutPath = MakeShotPath(Format);
	if (!WriteCompressedImage(ResizedPixels, ResizedSize, Format, Quality, OutPath, OutError))
	{
		return false;
	}

	const int64 Bytes = IFileManager::Get().FileSize(*OutPath);
	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetStringField(TEXT("target"), EffectiveTarget);
	OutData->SetStringField(TEXT("graph_name"), EffectiveTarget == TEXT("graph") ? GraphName : TEXT(""));
	OutData->SetStringField(TEXT("capture_mode"), CaptureMode);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetNumberField(TEXT("width"), ResizedSize.X);
	OutData->SetNumberField(TEXT("height"), ResizedSize.Y);
	OutData->SetNumberField(TEXT("bytes"), (double)Bytes);
	return true;
}

