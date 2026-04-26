bool FUeAgentHttpServer::ExecuteModelingCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;

	const TMap<FString, FString> ToolWrappers = {
		{TEXT("modeling_convert_actor_to_dynamic_mesh"), TEXT("BeginConvertMeshesTool")},
		{TEXT("modeling_duplicate_to_new_static_mesh"), TEXT("BeginDuplicateMeshesTool")},
		{TEXT("modeling_create_box"), TEXT("BeginAddBoxPrimitiveTool")},
		{TEXT("modeling_create_cylinder"), TEXT("BeginAddCylinderPrimitiveTool")},
		{TEXT("modeling_create_sphere"), TEXT("BeginAddSpherePrimitiveTool")},
		{TEXT("modeling_create_plane"), TEXT("BeginAddRectanglePrimitiveTool")},
		{TEXT("modeling_create_stairs"), TEXT("BeginAddStairsPrimitiveTool")},
		{TEXT("modeling_create_ramp"), TEXT("BeginAddRampPrimitiveTool")},
		{TEXT("modeling_create_ramp_corner"), TEXT("BeginAddRampCornerPrimitiveTool")},
		{TEXT("modeling_extrude_faces"), TEXT("BeginSelectionExtrudeTool")},
		{TEXT("modeling_inset_faces"), TEXT("BeginPolyEditTool")},
		{TEXT("modeling_bevel_edges"), TEXT("BeginPolyEditTool")},
		{TEXT("modeling_offset"), TEXT("BeginSelectionOffsetTool")},
		{TEXT("modeling_push_pull"), TEXT("BeginPolyEditTool")},
		{TEXT("modeling_mirror"), TEXT("BeginMirrorTool")},
		{TEXT("modeling_duplicate_faces"), TEXT("BeginPolyEditTool")},
		{TEXT("modeling_boolean"), TEXT("BeginMeshBooleanTool")},
		{TEXT("modeling_trim"), TEXT("BeginMeshTrimTool")},
		{TEXT("modeling_plane_cut"), TEXT("BeginPlaneCutTool")},
		{TEXT("modeling_mesh_cut"), TEXT("BeginCutMeshWithMeshTool")},
		{TEXT("modeling_voxel_boolean"), TEXT("BeginVoxelBooleanTool")},
		{TEXT("modeling_remesh"), TEXT("BeginRemeshMeshTool")},
		{TEXT("modeling_simplify"), TEXT("BeginSimplifyMeshTool")},
		{TEXT("modeling_subdivide"), TEXT("BeginSubdividePolyTool")},
		{TEXT("modeling_weld_edges"), TEXT("BeginWeldEdgesTool")},
		{TEXT("modeling_fill_holes"), TEXT("BeginHoleFillTool")},
		{TEXT("modeling_recompute_normals"), TEXT("BeginEditNormalsTool")},
		{TEXT("modeling_set_pivot"), TEXT("BeginEditPivotTool")},
		{TEXT("modeling_bake_transform"), TEXT("BeginBakeTransformTool")},
		{TEXT("modeling_align_to_world"), TEXT("BeginAlignObjectsTool")},
		{TEXT("modeling_auto_uv"), TEXT("BeginGlobalUVGenerateTool")},
		{TEXT("modeling_project_uv"), TEXT("BeginUVProjectionTool")},
		{TEXT("modeling_set_material_slot"), TEXT("BeginEditMeshMaterialsTool")},
		{TEXT("modeling_add_material_slot"), TEXT("BeginEditMeshMaterialsTool")},
		{TEXT("modeling_remove_material_slot"), TEXT("BeginEditMeshMaterialsTool")},
		{TEXT("modeling_generate_simple_collision"), TEXT("BeginSimpleCollisionEditorTool")},
		{TEXT("modeling_generate_convex_collision"), TEXT("BeginSetCollisionGeometryTool")}
	};

	if (CommandLower == TEXT("modeling_activate_mode")) return CmdModelingActivateMode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_get_selection")) return CmdModelingGetSelection(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_set_selection")) return CmdModelingSetSelection(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_set_mesh_selection_mode")) return CmdModelingSetMeshSelectionMode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_get_mesh_selection_info")) return CmdModelingGetMeshSelectionInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_clear_mesh_selection")) return CmdModelingClearMeshSelection(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_select_mesh_elements_via_screen")) return CmdModelingSelectMeshElementsViaScreen(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_select_mesh_elements_via_world_ray")) return CmdModelingSelectMeshElementsViaWorldRay(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_start_tool")) return CmdModelingStartTool(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_get_active_tool")) return CmdModelingGetActiveTool(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_get_active_tool_properties")) return CmdModelingGetActiveToolProperties(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_set_active_tool_property")) return CmdModelingSetActiveToolProperty(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_invoke_active_tool_action")) return CmdModelingInvokeActiveToolAction(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_accept_tool")) return CmdModelingAcceptTool(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_cancel_tool")) return CmdModelingCancelTool(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_save_mesh_asset")) return CmdModelingSaveMeshAsset(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_replace_actor_mesh")) return CmdModelingReplaceActorMesh(Ctx, OutData, OutError);
	if (CommandLower == TEXT("modeling_snap_to_ground")) return CmdModelingSnapToGround(Ctx, OutData, OutError);

	if (const FString* ToolIdentifier = ToolWrappers.Find(CommandLower))
	{
		FUeAgentRequestContext WrappedCtx = Ctx;
		if (!WrappedCtx.Params.IsValid())
		{
			WrappedCtx.Params = MakeShared<FJsonObject>();
		}
		WrappedCtx.Params->SetStringField(TEXT("__uai_wrapper_command"), CommandLower);
		WrappedCtx.Params->SetStringField(TEXT("tool_identifier"), *ToolIdentifier);
		if (CommandLower == TEXT("modeling_duplicate_faces"))
		{
			WrappedCtx.Params->SetStringField(TEXT("post_action"), TEXT("Duplicate"));
		}
		else if (CommandLower == TEXT("modeling_inset_faces"))
		{
			WrappedCtx.Params->SetStringField(TEXT("post_action"), TEXT("Inset"));
		}
		else if (CommandLower == TEXT("modeling_push_pull"))
		{
			WrappedCtx.Params->SetStringField(TEXT("post_action"), TEXT("PushPull"));
		}
		else if (CommandLower == TEXT("modeling_bevel_edges"))
		{
			WrappedCtx.Params->SetStringField(TEXT("post_action"), TEXT("Bevel"));
		}
		return CmdModelingStartTool(WrappedCtx, OutData, OutError);
	}

	OutError = TEXT("unknown_modeling_command");
	return false;
}

bool FUeAgentHttpServer::CmdModelingActivateMode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	FlushToolsContext(Context);
	OutData->SetBoolField(TEXT("mode_active"), true);
	OutData->SetStringField(TEXT("mode_id"), UModelingToolsEditorMode::EM_ModelingToolsEditorModeId.ToString());
	FUeAgentInterfaceLogger::Log(TEXT("ModelingActivateMode req=%s"), *Ctx.RequestId);
	return true;
}

bool FUeAgentHttpServer::CmdModelingGetSelection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> ActorsJson;
	if (GEditor)
	{
		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
		{
			if (AActor* Actor = Cast<AActor>(*It))
			{
				ActorsJson.Add(MakeShared<FJsonValueObject>(MakeActorSummaryJson(Actor)));
			}
		}
	}

	UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr;
	UGeometrySelectionManager::EGeometryTopologyType TopologyType = UGeometrySelectionManager::EGeometryTopologyType::Triangle;
	UGeometrySelectionManager::EGeometryElementType ElementType = UGeometrySelectionManager::EGeometryElementType::Face;
	int32 NumTargets = 0;
	bool bIsEmpty = true;
	if (SelectionManager)
	{
		SelectionManager->GetActiveSelectionInfo(TopologyType, ElementType, NumTargets, bIsEmpty);
	}

	OutData->SetArrayField(TEXT("selected_actors"), ActorsJson);
	OutData->SetStringField(TEXT("mesh_selection_element_type"), SelectionManager ? ElementTypeToString(ElementType) : TEXT("face"));
	OutData->SetStringField(TEXT("mesh_topology_mode"), SelectionManager ? TopologyModeToString(SelectionManager->GetMeshTopologyMode()) : TEXT("triangle"));
	OutData->SetNumberField(TEXT("mesh_target_count"), NumTargets);
	OutData->SetBoolField(TEXT("mesh_selection_empty"), bIsEmpty);
	return true;
}

bool FUeAgentHttpServer::CmdModelingSetSelection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ActorIds = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("actor_ids"), ActorIds) || !ActorIds)
	{
		OutError = TEXT("actor_ids_required");
		return false;
	}

	const bool bSyncGeometryTargets = !Ctx.Params->HasField(TEXT("sync_geometry_targets")) || Ctx.Params->GetBoolField(TEXT("sync_geometry_targets"));
	const TArray<AActor*> Actors = ResolveActorsByIds(World, *ActorIds);
	SelectActors(Actors);
	if (bSyncGeometryTargets)
	{
		SyncGeometryTargets(Actors, Context);
	}

	OutData->SetNumberField(TEXT("selected_actor_count"), Actors.Num());
	if (UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr)
	{
		OutData->SetBoolField(TEXT("has_active_targets"), SelectionManager->HasActiveTargets());
		OutData->SetBoolField(TEXT("has_selection"), SelectionManager->HasSelection());
	}
	FUeAgentInterfaceLogger::Log(TEXT("ModelingSetSelection req=%s count=%d sync_targets=%s has_active_targets=%s"),
		*Ctx.RequestId,
		Actors.Num(),
		bSyncGeometryTargets ? TEXT("true") : TEXT("false"),
		(Context.Mode && Context.Mode->GetSelectionManager() && Context.Mode->GetSelectionManager()->HasActiveTargets()) ? TEXT("true") : TEXT("false"));
	return true;
}

bool FUeAgentHttpServer::CmdModelingSetMeshSelectionMode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr;
	if (!SelectionManager)
	{
		OutError = TEXT("modeling_selection_manager_unavailable");
		return false;
	}

	FString ElementValue = TEXT("face");
	FString TopologyValue = TEXT("triangle");
	bool bConvertExisting = true;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("element_type"), ElementValue);
		Ctx.Params->TryGetStringField(TEXT("topology_mode"), TopologyValue);
		Ctx.Params->TryGetBoolField(TEXT("convert_existing_selection"), bConvertExisting);
	}

	const UGeometrySelectionManager::EGeometryElementType ElementType = ParseElementType(ElementValue);
	const UGeometrySelectionManager::EMeshTopologyMode TopologyMode = ParseTopologyMode(TopologyValue);
	SelectionManager->SetMeshSelectionTypeAndMode(ElementType, TopologyMode, bConvertExisting);

	OutData->SetStringField(TEXT("element_type"), ElementTypeToString(ElementType));
	OutData->SetStringField(TEXT("topology_mode"), TopologyModeToString(TopologyMode));
	OutData->SetBoolField(TEXT("convert_existing_selection"), bConvertExisting);
	return true;
}

bool FUeAgentHttpServer::CmdModelingGetMeshSelectionInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr;
	if (!SelectionManager)
	{
		OutError = TEXT("modeling_selection_manager_unavailable");
		return false;
	}

	UGeometrySelectionManager::EGeometryTopologyType TopologyType = UGeometrySelectionManager::EGeometryTopologyType::Triangle;
	UGeometrySelectionManager::EGeometryElementType ElementType = UGeometrySelectionManager::EGeometryElementType::Face;
	int32 NumTargets = 0;
	bool bIsEmpty = true;
	SelectionManager->GetActiveSelectionInfo(TopologyType, ElementType, NumTargets, bIsEmpty);

	OutData->SetStringField(TEXT("element_type"), ElementTypeToString(ElementType));
	OutData->SetStringField(TEXT("topology_mode"), TopologyModeToString(SelectionManager->GetMeshTopologyMode()));
	OutData->SetNumberField(TEXT("target_count"), NumTargets);
	OutData->SetBoolField(TEXT("is_empty"), bIsEmpty);
	OutData->SetBoolField(TEXT("has_active_targets"), SelectionManager->HasActiveTargets());
	return true;
}

bool FUeAgentHttpServer::CmdModelingClearMeshSelection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr;
	if (!SelectionManager)
	{
		OutError = TEXT("modeling_selection_manager_unavailable");
		return false;
	}

	const bool bSaveSelectionBeforeClear = [&Ctx]()
	{
		bool bValue = false;
		if (Ctx.Params.IsValid())
		{
			Ctx.Params->TryGetBoolField(TEXT("save_selection_before_clear"), bValue);
		}
		return bValue;
	}();

	SelectionManager->ClearSelection(bSaveSelectionBeforeClear);
	FlushToolsContext(Context);

	UGeometrySelectionManager::EGeometryTopologyType TopologyType = UGeometrySelectionManager::EGeometryTopologyType::Triangle;
	UGeometrySelectionManager::EGeometryElementType ElementType = UGeometrySelectionManager::EGeometryElementType::Face;
	int32 NumTargets = 0;
	bool bIsEmpty = true;
	SelectionManager->GetActiveSelectionInfo(TopologyType, ElementType, NumTargets, bIsEmpty);

	OutData->SetBoolField(TEXT("selection_cleared"), true);
	OutData->SetBoolField(TEXT("save_selection_before_clear"), bSaveSelectionBeforeClear);
	OutData->SetStringField(TEXT("element_type"), ElementTypeToString(ElementType));
	OutData->SetStringField(TEXT("topology_mode"), TopologyModeToString(SelectionManager->GetMeshTopologyMode()));
	OutData->SetNumberField(TEXT("target_count"), NumTargets);
	OutData->SetBoolField(TEXT("is_empty"), bIsEmpty);
	OutData->SetBoolField(TEXT("has_active_targets"), SelectionManager->HasActiveTargets());
	return true;
}

bool FUeAgentHttpServer::CmdModelingSelectMeshElementsViaScreen(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr;
	if (!SelectionManager)
	{
		OutError = TEXT("modeling_selection_manager_unavailable");
		return false;
	}
	if (!SelectionManager->HasActiveTargets())
	{
		OutError = TEXT("modeling_selection_targets_empty");
		return false;
	}

	FVector2D ScreenPoint(0.0f, 0.0f);
	if (!ResolveViewportScreenPoint(Context.ViewportClient, Ctx.Params, ScreenPoint, OutError))
	{
		return false;
	}

	FRay3d WorldRay;
	if (!DeprojectViewportScreenPoint(Context, ScreenPoint, WorldRay, OutError))
	{
		return false;
	}

	FGeometrySelectionUpdateConfig UpdateConfig;
	FString ChangeTypeText = TEXT("replace");
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("change_type"), ChangeTypeText);
	}
	UpdateConfig.ChangeType = ParseSelectionChangeType(ChangeTypeText);

	const bool bClearOnMiss = [&Ctx, &UpdateConfig]()
	{
		bool bValue = (UpdateConfig.ChangeType == EGeometrySelectionChangeType::Replace);
		if (Ctx.Params.IsValid())
		{
			Ctx.Params->TryGetBoolField(TEXT("clear_on_miss"), bValue);
		}
		return bValue;
	}();

	FGeometrySelectionUpdateResult Result;
	SelectionManager->UpdateSelectionViaRaycast(WorldRay, UpdateConfig, Result);
	if (Result.bSelectionMissed && bClearOnMiss)
	{
		SelectionManager->ClearSelection();
	}
	FlushToolsContext(Context);

	OutData->SetObjectField(TEXT("screen_point"), VecToJson(FVector(ScreenPoint.X, ScreenPoint.Y, 0.0)));
	OutData->SetObjectField(TEXT("world_origin"), VecToJson((FVector)WorldRay.Origin));
	OutData->SetObjectField(TEXT("world_direction"), VecToJson((FVector)WorldRay.Direction));
	OutData->SetBoolField(TEXT("clear_on_miss"), bClearOnMiss);
	FillSelectionUpdateJson(SelectionManager, UpdateConfig, Result, OutData);
	return true;
}

bool FUeAgentHttpServer::CmdModelingSelectMeshElementsViaWorldRay(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentModelingOps;
	OutData = MakeShared<FJsonObject>();

	FModelingContext Context;
	if (!GetModelingContext(Context, OutError))
	{
		return false;
	}

	UGeometrySelectionManager* SelectionManager = Context.Mode ? Context.Mode->GetSelectionManager() : nullptr;
	if (!SelectionManager)
	{
		OutError = TEXT("modeling_selection_manager_unavailable");
		return false;
	}
	if (!SelectionManager->HasActiveTargets())
	{
		OutError = TEXT("modeling_selection_targets_empty");
		return false;
	}

	FRay3d WorldRay;
	if (!ResolveWorldRayFromParams(Ctx.Params, WorldRay, OutError))
	{
		return false;
	}

	FGeometrySelectionUpdateConfig UpdateConfig;
	FString ChangeTypeText = TEXT("replace");
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("change_type"), ChangeTypeText);
	}
	UpdateConfig.ChangeType = ParseSelectionChangeType(ChangeTypeText);

	const bool bClearOnMiss = [&Ctx, &UpdateConfig]()
	{
		bool bValue = (UpdateConfig.ChangeType == EGeometrySelectionChangeType::Replace);
		if (Ctx.Params.IsValid())
		{
			Ctx.Params->TryGetBoolField(TEXT("clear_on_miss"), bValue);
		}
		return bValue;
	}();

	FGeometrySelectionUpdateResult Result;
	SelectionManager->UpdateSelectionViaRaycast(WorldRay, UpdateConfig, Result);
	if (Result.bSelectionMissed && bClearOnMiss)
	{
		SelectionManager->ClearSelection();
	}
	FlushToolsContext(Context);

	OutData->SetObjectField(TEXT("world_origin"), VecToJson((FVector)WorldRay.Origin));
	OutData->SetObjectField(TEXT("world_direction"), VecToJson((FVector)WorldRay.Direction));
	OutData->SetBoolField(TEXT("clear_on_miss"), bClearOnMiss);
	FillSelectionUpdateJson(SelectionManager, UpdateConfig, Result, OutData);
	return true;
}
