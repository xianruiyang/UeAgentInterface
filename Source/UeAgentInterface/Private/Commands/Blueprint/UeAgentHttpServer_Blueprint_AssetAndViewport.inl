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
	if (CommandLower == TEXT("blueprint_layout_graph_self_test")) return CmdBlueprintLayoutGraphSelfTest(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_layout_graph")) return CmdBlueprintLayoutGraph(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blueprint_layout_all_graphs"))
	{
		return UeAgentBlueprintOps::ExecuteBlueprintLayoutAllGraphsCommand(
			Ctx,
			OutData,
			OutError,
			[this](const FUeAgentRequestContext& GraphCtx, TSharedPtr<FJsonObject>& GraphData, FString& GraphError)
			{
				return CmdBlueprintLayoutGraph(GraphCtx, GraphData, GraphError);
			});
	}
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

	FVector2f PersistedViewLocation(0.0f, 0.0f);
	float PersistedZoomAmount = 1.0f;
	const bool bHasPersistedViewState = UeAgentBlueprintOps::TryGetBlueprintGraphDocumentViewState(
		Blueprint,
		Graph,
		PersistedViewLocation,
		PersistedZoomAmount);
	const FVector2f PersistedPixelResidual = UeAgentGraphViewUtils::ViewLocationPixelResidual(PersistedViewLocation, PersistedZoomAmount);
	const bool bLiveDiffersFromPersisted =
		bHasPersistedViewState
		&& (!FMath::IsNearlyEqual(ViewLocation.X, PersistedViewLocation.X, 0.01f)
			|| !FMath::IsNearlyEqual(ViewLocation.Y, PersistedViewLocation.Y, 0.01f)
			|| !FMath::IsNearlyEqual(ZoomAmount, PersistedZoomAmount, 0.0001f));

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("graph_name"), Graph->GetName());
	OutData->SetNumberField(TEXT("view_x"), ViewLocation.X);
	OutData->SetNumberField(TEXT("view_y"), ViewLocation.Y);
	OutData->SetNumberField(TEXT("zoom"), ZoomAmount);
	OutData->SetBoolField(TEXT("view_pixel_aligned"), UeAgentGraphViewUtils::IsViewLocationPixelAligned(ViewLocation, ZoomAmount));
	const FVector2f PixelResidual = UeAgentGraphViewUtils::ViewLocationPixelResidual(ViewLocation, ZoomAmount);
	OutData->SetNumberField(TEXT("screen_pixel_x"), ViewLocation.X * ZoomAmount);
	OutData->SetNumberField(TEXT("screen_pixel_y"), ViewLocation.Y * ZoomAmount);
	OutData->SetNumberField(TEXT("screen_pixel_residual_x"), PixelResidual.X);
	OutData->SetNumberField(TEXT("screen_pixel_residual_y"), PixelResidual.Y);
	OutData->SetBoolField(TEXT("persisted_view_found"), bHasPersistedViewState);
	OutData->SetNumberField(TEXT("persisted_view_x"), PersistedViewLocation.X);
	OutData->SetNumberField(TEXT("persisted_view_y"), PersistedViewLocation.Y);
	OutData->SetNumberField(TEXT("persisted_zoom"), PersistedZoomAmount);
	OutData->SetBoolField(TEXT("persisted_view_pixel_aligned"), bHasPersistedViewState && UeAgentGraphViewUtils::IsViewLocationPixelAligned(PersistedViewLocation, PersistedZoomAmount));
	OutData->SetNumberField(TEXT("persisted_screen_pixel_x"), PersistedViewLocation.X * PersistedZoomAmount);
	OutData->SetNumberField(TEXT("persisted_screen_pixel_y"), PersistedViewLocation.Y * PersistedZoomAmount);
	OutData->SetNumberField(TEXT("persisted_screen_pixel_residual_x"), bHasPersistedViewState ? PersistedPixelResidual.X : 0.0f);
	OutData->SetNumberField(TEXT("persisted_screen_pixel_residual_y"), bHasPersistedViewState ? PersistedPixelResidual.Y : 0.0f);
	OutData->SetBoolField(TEXT("live_view_differs_from_persisted"), bLiveDiffersFromPersisted);
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
	bool bSnapToPixelGrid = true;
	JsonTryGetBool(Ctx.Params, TEXT("snap_to_pixel_grid"), bSnapToPixelGrid);
	bool bUpdateDocumentViewState = true;
	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("update_document_view_state"), bUpdateDocumentViewState);
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (!bHasViewX && !bHasViewY && !bHasZoom)
	{
		OutError = TEXT("missing_view_or_zoom");
		return false;
	}

	const float NewZoom = (float)FMath::Clamp(ZoomAmount, 0.05, 64.0);
	FVector2f NewViewLocation((float)ViewX, (float)ViewY);
	if (bSnapToPixelGrid)
	{
		NewViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(NewViewLocation, NewZoom);
	}
	GraphEditor->SetViewLocation(NewViewLocation, NewZoom);
	GraphEditor->GetViewLocation(CurrentViewLocation, CurrentZoomAmount);
	if (bSnapToPixelGrid && !UeAgentGraphViewUtils::IsViewLocationPixelAligned(CurrentViewLocation, CurrentZoomAmount))
	{
		CurrentViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(CurrentViewLocation, CurrentZoomAmount);
		GraphEditor->SetViewLocation(CurrentViewLocation, CurrentZoomAmount);
		GraphEditor->GetViewLocation(CurrentViewLocation, CurrentZoomAmount);
	}
	FVector2f PersistedViewLocation = CurrentViewLocation;
	if (bSnapToPixelGrid)
	{
		PersistedViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(PersistedViewLocation, CurrentZoomAmount);
	}

	bool bUpdatedDocumentViewState = false;
	int32 RemovedDocumentViewStateEntryCount = 0;
	if (bUpdateDocumentViewState)
	{
		if (bSaveAfterSet)
		{
			Blueprint->Modify();
		}
		bUpdatedDocumentViewState = UeAgentBlueprintOps::PersistBlueprintGraphDocumentViewState(
			Blueprint,
			Graph,
			PersistedViewLocation,
			CurrentZoomAmount,
			&RemovedDocumentViewStateEntryCount);
	}
	FVector2f EffectivePersistedViewLocation(0.0f, 0.0f);
	float EffectivePersistedZoomAmount = 1.0f;
	bool bHasPersistedViewState = false;
	if (bUpdatedDocumentViewState)
	{
		EffectivePersistedViewLocation = PersistedViewLocation;
		EffectivePersistedZoomAmount = CurrentZoomAmount;
		bHasPersistedViewState = true;
	}
	else
	{
		bHasPersistedViewState = UeAgentBlueprintOps::TryGetBlueprintGraphDocumentViewState(
			Blueprint,
			Graph,
			EffectivePersistedViewLocation,
			EffectivePersistedZoomAmount);
	}
	if (bSaveAfterSet && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("graph_name"), Graph->GetName());
	OutData->SetNumberField(TEXT("view_x"), CurrentViewLocation.X);
	OutData->SetNumberField(TEXT("view_y"), CurrentViewLocation.Y);
	OutData->SetNumberField(TEXT("zoom"), CurrentZoomAmount);
	OutData->SetBoolField(TEXT("persisted_view_found"), bHasPersistedViewState);
	OutData->SetNumberField(TEXT("persisted_view_x"), EffectivePersistedViewLocation.X);
	OutData->SetNumberField(TEXT("persisted_view_y"), EffectivePersistedViewLocation.Y);
	OutData->SetNumberField(TEXT("persisted_zoom"), EffectivePersistedZoomAmount);
	OutData->SetBoolField(TEXT("snap_to_pixel_grid"), bSnapToPixelGrid);
	OutData->SetBoolField(TEXT("view_pixel_aligned"), UeAgentGraphViewUtils::IsViewLocationPixelAligned(CurrentViewLocation, CurrentZoomAmount));
	OutData->SetBoolField(TEXT("persisted_view_pixel_aligned"), bHasPersistedViewState && UeAgentGraphViewUtils::IsViewLocationPixelAligned(EffectivePersistedViewLocation, EffectivePersistedZoomAmount));
	OutData->SetBoolField(TEXT("update_document_view_state"), bUpdateDocumentViewState);
	OutData->SetBoolField(TEXT("updated_document_view_state"), bUpdatedDocumentViewState);
	OutData->SetNumberField(TEXT("removed_document_view_state_entry_count"), RemovedDocumentViewStateEntryCount);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	OutData->SetNumberField(TEXT("screen_pixel_x"), CurrentViewLocation.X * CurrentZoomAmount);
	OutData->SetNumberField(TEXT("screen_pixel_y"), CurrentViewLocation.Y * CurrentZoomAmount);
	const FVector2f PixelResidual = UeAgentGraphViewUtils::ViewLocationPixelResidual(CurrentViewLocation, CurrentZoomAmount);
	const FVector2f PersistedPixelResidual = UeAgentGraphViewUtils::ViewLocationPixelResidual(EffectivePersistedViewLocation, EffectivePersistedZoomAmount);
	OutData->SetNumberField(TEXT("screen_pixel_residual_x"), PixelResidual.X);
	OutData->SetNumberField(TEXT("screen_pixel_residual_y"), PixelResidual.Y);
	OutData->SetNumberField(TEXT("persisted_screen_pixel_x"), EffectivePersistedViewLocation.X * EffectivePersistedZoomAmount);
	OutData->SetNumberField(TEXT("persisted_screen_pixel_y"), EffectivePersistedViewLocation.Y * EffectivePersistedZoomAmount);
	OutData->SetNumberField(TEXT("persisted_screen_pixel_residual_x"), bHasPersistedViewState ? PersistedPixelResidual.X : 0.0f);
	OutData->SetNumberField(TEXT("persisted_screen_pixel_residual_y"), bHasPersistedViewState ? PersistedPixelResidual.Y : 0.0f);
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

	FString Target = TEXT("viewport");
	JsonTryGetString(Ctx.Params, TEXT("target"), Target);
	Target = Target.TrimStartAndEnd().ToLower();
	const bool bGraphTarget = Target == TEXT("eventgraph")
		|| Target == TEXT("event_graph")
		|| Target == TEXT("graph")
		|| Target.StartsWith(TEXT("graph:"));

	bool bAllowUiMutation = !bGraphTarget;
	JsonTryGetBool(Ctx.Params, TEXT("allow_ui_mutation"), bAllowUiMutation);
	bool bAllowWindowMutation = bAllowUiMutation;
	JsonTryGetBool(Ctx.Params, TEXT("allow_window_mutation"), bAllowWindowMutation);
	if (bAllowUiMutation)
	{
		bAllowWindowMutation = true;
	}

	bool bOpenEditorIfNeeded = bAllowUiMutation;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);
	if (bOpenEditorIfNeeded && !bAllowUiMutation)
	{
		OutError = TEXT("open_editor_if_needed_requires_allow_ui_mutation");
		return false;
	}

	TSharedPtr<IBlueprintEditor> BlueprintEditor = UeAgentBlueprintOps::GetBlueprintEditor(Blueprint, bOpenEditorIfNeeded, OutError);
	if (!BlueprintEditor.IsValid())
	{
		return false;
	}

	FString GraphName = TEXT("EventGraph");
	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);
	bool bRestoreGraphViewAfterCapture = true;
	JsonTryGetBool(Ctx.Params, TEXT("restore_graph_view_after_capture"), bRestoreGraphViewAfterCapture);

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
	TSharedPtr<SGraphEditor> GraphEditorToRestore;
	UEdGraph* GraphToRestore = nullptr;
	bool bRestoredGraphViewAfterCapture = false;
	bool bWindowMutatedForCapture = false;
	bool bPreparedWindowForCapture = false;
	FVector2f GraphViewBeforeCapture(0.0f, 0.0f);
	float GraphZoomBeforeCapture = 1.0f;
	FVector2f GraphViewAfterCapture(0.0f, 0.0f);
	float GraphZoomAfterCapture = 1.0f;
	bool bCapturedGraphViewBefore = false;
	bool bCapturedGraphViewAfter = false;
	FString GraphViewRestoreMode = TEXT("none");
	auto RestoreGraphViewAfterCapture = [&]() -> bool
	{
		if (!bRestoreGraphViewAfterCapture || !GraphToRestore || !GraphEditorToRestore.IsValid())
		{
			return false;
		}
		if (!bAllowUiMutation && bCapturedGraphViewBefore)
		{
			float RestoreZoomAmount = UeAgentGraphViewUtils::SafeGraphZoomAmount(GraphZoomBeforeCapture);
			FVector2f RestoreViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(
				GraphViewBeforeCapture,
				RestoreZoomAmount);
			GraphEditorToRestore->SetViewLocation(RestoreViewLocation, RestoreZoomAmount);
			GraphEditorToRestore->GetViewLocation(RestoreViewLocation, RestoreZoomAmount);
			GraphViewRestoreMode = TEXT("pre_capture");
			FUeAgentInterfaceLogger::Log(
				TEXT("CmdBlueprintScreenshot restored graph view to pre-capture state asset=%s graph=%s view=(%.3f,%.3f) zoom=%.6f"),
				*Blueprint->GetOutermost()->GetName(),
				*GraphToRestore->GetName(),
				RestoreViewLocation.X,
				RestoreViewLocation.Y,
				RestoreZoomAmount);
			return true;
		}
		FVector2f RestoredViewLocation(0.0f, 0.0f);
		float RestoredZoomAmount = 1.0f;
		const bool bRestored = UeAgentBlueprintOps::RestoreBlueprintGraphEditorToDocumentViewState(
			Blueprint,
			GraphToRestore,
			GraphEditorToRestore,
			&RestoredViewLocation,
			&RestoredZoomAmount);
		if (bRestored)
		{
			FUeAgentInterfaceLogger::Log(
				TEXT("CmdBlueprintScreenshot restored graph view after capture asset=%s graph=%s view=(%.3f,%.3f) zoom=%.6f"),
				*Blueprint->GetOutermost()->GetName(),
				*GraphToRestore->GetName(),
				RestoredViewLocation.X,
				RestoredViewLocation.Y,
				RestoredZoomAmount);
			GraphViewRestoreMode = TEXT("document");
		}
		return bRestored;
	};
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
		TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(Graph, bAllowUiMutation);
		if (!GraphEditor.IsValid())
		{
			OutError = TEXT("graph_editor_not_found");
			return false;
		}
		GraphToRestore = Graph;
		GraphEditorToRestore = GraphEditor;
		GraphEditor->GetViewLocation(GraphViewBeforeCapture, GraphZoomBeforeCapture);
		GraphZoomBeforeCapture = UeAgentGraphViewUtils::SafeGraphZoomAmount(GraphZoomBeforeCapture);
		bCapturedGraphViewBefore = true;
		FString WindowError;
		BlueprintWindowForShot = UeAgentBlueprintOps::ResolveBlueprintEditorWindow(BlueprintEditor, WindowError);
		if (BlueprintWindowForShot.IsValid() && bAllowWindowMutation)
		{
			bool bWindowMutatedThisPass = false;
			bPreparedWindowForCapture = true;
			if (!UeAgentBlueprintOps::PrepareSlateWindowForCapture(BlueprintWindowForShot, OutError, bAllowWindowMutation, &bWindowMutatedThisPass))
			{
				bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
				bRestoredGraphViewAfterCapture = RestoreGraphViewAfterCapture();
				return false;
			}
			bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
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
		TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(Graph, bAllowUiMutation);
		if (!GraphEditor.IsValid())
		{
			OutError = TEXT("graph_editor_not_found");
			return false;
		}
		GraphToRestore = Graph;
		GraphEditorToRestore = GraphEditor;
		GraphEditor->GetViewLocation(GraphViewBeforeCapture, GraphZoomBeforeCapture);
		GraphZoomBeforeCapture = UeAgentGraphViewUtils::SafeGraphZoomAmount(GraphZoomBeforeCapture);
		bCapturedGraphViewBefore = true;
		FString WindowError;
		BlueprintWindowForShot = UeAgentBlueprintOps::ResolveBlueprintEditorWindow(BlueprintEditor, WindowError);
		if (BlueprintWindowForShot.IsValid() && bAllowWindowMutation)
		{
			bool bWindowMutatedThisPass = false;
			bPreparedWindowForCapture = true;
			if (!UeAgentBlueprintOps::PrepareSlateWindowForCapture(BlueprintWindowForShot, OutError, bAllowWindowMutation, &bWindowMutatedThisPass))
			{
				bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
				bRestoredGraphViewAfterCapture = RestoreGraphViewAfterCapture();
				return false;
			}
			bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
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
		bool bWindowMutatedThisPass = false;
		bPreparedWindowForCapture = true;
		if (!UeAgentBlueprintOps::PrepareSlateWindowForCapture(BlueprintWindowForShot, OutError, bAllowWindowMutation, &bWindowMutatedThisPass))
		{
			bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
			return false;
		}
		bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
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
	FString CaptureMode = TEXT("slate_widget_offscreen");
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
			FString WindowPrepareError;
			bool bWindowMutatedThisPass = false;
			bPreparedWindowForCapture = true;
			if (!UeAgentBlueprintOps::PrepareSlateWindowForCapture(BlueprintWindowForShot, WindowPrepareError, bAllowWindowMutation, &bWindowMutatedThisPass))
			{
				FUeAgentInterfaceLogger::Log(TEXT("CmdBlueprintScreenshot viewport window not renderable before capture: %s"), *WindowPrepareError);
			}
			bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
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

		bool bWindowMutatedThisPass = false;
		bShotOk = UeAgentBlueprintOps::ScreenshotBlueprintWindowCropWidget(
			BlueprintEditor,
			CropWidget,
			Pixels,
			ShotSize,
			OutError,
			bAllowWindowMutation,
			&bWindowMutatedThisPass);
		bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
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
			else
			{
				bWindowMutatedThisPass = false;
				if (UeAgentBlueprintOps::ScreenshotBlueprintWindow(
					BlueprintEditor,
					Pixels,
					ShotSize,
					FallbackError,
					bAllowWindowMutation,
					&bWindowMutatedThisPass))
				{
					bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
					bShotOk = true;
					CaptureMode = TEXT("window_offscreen_fallback");
					OutError.Reset();
				}
				else
				{
					bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
					OutError = FallbackError.IsEmpty() ? CropError : FallbackError;
				}
			}
		}
		else
		{
			CaptureMode = TEXT("viewport_window_crop_offscreen");
		}
	}
	else
	{
		if (EffectiveTarget == TEXT("graph") && !bAllowUiMutation)
		{
			CaptureMode = TEXT("graph_widget_offscreen_noninvasive");
			bShotOk = UeAgentBlueprintOps::ScreenshotSlateWidgetNonInvasive(WidgetToShot.ToSharedRef(), Pixels, ShotSize, OutError);
		}
		else
		{
			CaptureMode = EffectiveTarget == TEXT("graph") ? TEXT("graph_widget_offscreen_mutating") : TEXT("slate_widget_offscreen");
			bShotOk = UeAgentBlueprintOps::ScreenshotSlateWidget(WidgetToShot.ToSharedRef(), Pixels, ShotSize, OutError);
		}
	}
	if (EffectiveTarget == TEXT("graph"))
	{
		bRestoredGraphViewAfterCapture = RestoreGraphViewAfterCapture();
		if (GraphEditorToRestore.IsValid())
		{
			GraphEditorToRestore->GetViewLocation(GraphViewAfterCapture, GraphZoomAfterCapture);
			GraphZoomAfterCapture = UeAgentGraphViewUtils::SafeGraphZoomAmount(GraphZoomAfterCapture);
			bCapturedGraphViewAfter = true;
		}
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
	OutData->SetStringField(TEXT("legacy_backbuffer_capture"), TEXT("disabled"));
	OutData->SetBoolField(TEXT("allow_ui_mutation"), bAllowUiMutation);
	OutData->SetBoolField(TEXT("allow_window_mutation"), bAllowWindowMutation);
	OutData->SetBoolField(TEXT("prepared_window_for_capture"), bPreparedWindowForCapture);
	OutData->SetBoolField(TEXT("window_mutated_for_capture"), bWindowMutatedForCapture);
	OutData->SetBoolField(TEXT("capture_window_resolved"), BlueprintWindowForShot.IsValid());
	if (BlueprintWindowForShot.IsValid())
	{
		OutData->SetBoolField(TEXT("capture_window_visible_after"), BlueprintWindowForShot->IsVisible());
		OutData->SetBoolField(TEXT("capture_window_minimized_after"), BlueprintWindowForShot->IsWindowMinimized());
	}
	OutData->SetBoolField(TEXT("restore_graph_view_after_capture"), bRestoreGraphViewAfterCapture);
	OutData->SetBoolField(TEXT("restored_graph_view_after_capture"), bRestoredGraphViewAfterCapture);
	OutData->SetStringField(TEXT("graph_view_restore_mode"), GraphViewRestoreMode);
	if (EffectiveTarget == TEXT("graph"))
	{
		OutData->SetBoolField(TEXT("captured_graph_view_before"), bCapturedGraphViewBefore);
		OutData->SetBoolField(TEXT("captured_graph_view_after"), bCapturedGraphViewAfter);
		OutData->SetNumberField(TEXT("graph_view_before_x"), GraphViewBeforeCapture.X);
		OutData->SetNumberField(TEXT("graph_view_before_y"), GraphViewBeforeCapture.Y);
		OutData->SetNumberField(TEXT("graph_zoom_before"), GraphZoomBeforeCapture);
		OutData->SetNumberField(TEXT("graph_view_after_x"), GraphViewAfterCapture.X);
		OutData->SetNumberField(TEXT("graph_view_after_y"), GraphViewAfterCapture.Y);
		OutData->SetNumberField(TEXT("graph_zoom_after"), GraphZoomAfterCapture);
		const bool bGraphViewChangedAfterCapture = bCapturedGraphViewBefore
			&& bCapturedGraphViewAfter
			&& (!FMath::IsNearlyEqual(GraphViewBeforeCapture.X, GraphViewAfterCapture.X, 0.01f)
				|| !FMath::IsNearlyEqual(GraphViewBeforeCapture.Y, GraphViewAfterCapture.Y, 0.01f)
				|| !FMath::IsNearlyEqual(GraphZoomBeforeCapture, GraphZoomAfterCapture, 0.0001f));
		OutData->SetBoolField(TEXT("graph_view_changed_after_capture"), bGraphViewChangedAfterCapture);
		const FVector2f GraphViewBeforeResidual = UeAgentGraphViewUtils::ViewLocationPixelResidual(
			GraphViewBeforeCapture,
			GraphZoomBeforeCapture);
		const FVector2f GraphViewAfterResidual = UeAgentGraphViewUtils::ViewLocationPixelResidual(
			GraphViewAfterCapture,
			GraphZoomAfterCapture);
		OutData->SetNumberField(TEXT("graph_view_before_pixel_residual_x"), GraphViewBeforeResidual.X);
		OutData->SetNumberField(TEXT("graph_view_before_pixel_residual_y"), GraphViewBeforeResidual.Y);
		OutData->SetNumberField(TEXT("graph_view_after_pixel_residual_x"), GraphViewAfterResidual.X);
		OutData->SetNumberField(TEXT("graph_view_after_pixel_residual_y"), GraphViewAfterResidual.Y);
	}
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetNumberField(TEXT("width"), ResizedSize.X);
	OutData->SetNumberField(TEXT("height"), ResizedSize.Y);
	OutData->SetNumberField(TEXT("bytes"), (double)Bytes);
	return true;
}

