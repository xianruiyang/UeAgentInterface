// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentCommandRegistry.h"
#include "Server/UeAgentHttpServer.h"

namespace
{
	const TSet<FString>& GetExplicitlyUnsupportedCommandIds()
	{
		static const TSet<FString> Unsupported{
			TEXT("begin_transaction"),
			TEXT("end_transaction"),
			TEXT("editor_console_exec"),
			TEXT("exec_batch"),
			TEXT("redo"),
			TEXT("undo"),
		};
		return Unsupported;
	}

	const TSet<FString>& GetRetiredLegacyCommandIds()
	{
		static const TSet<FString> Retired{
			TEXT("asset_delete"),
			TEXT("asset_move"),
			TEXT("asset_fixup_redirectors"),
			TEXT("sound_wave_reimport"),
			TEXT("sparse_volume_texture_reimport"),
			TEXT("static_mesh_reimport"),
			TEXT("texture_reimport"),
			TEXT("skeletal_mesh_remove_morph_target"),
			TEXT("skeletal_mesh_remove_skin_weight_profile"),
			TEXT("asset_import_fbx_animation"),
			TEXT("asset_import_fbx_skeletal_mesh"),
			TEXT("asset_import_geometry_cache"),
			TEXT("asset_import_texture"),
			TEXT("light_profile_import"),
			TEXT("sparse_volume_texture_import"),
			TEXT("sound_wave_import"),
			TEXT("localization_import_po"),
			TEXT("skeletal_mesh_import_skin_weight_profile"),
			TEXT("data_layer_delete_instance"),
			TEXT("level_content_delete_scope"),
			TEXT("pcg_cleanup"),
			TEXT("level_streaming_move_actors"),
			TEXT("level_streaming_remove"),
			TEXT("modeling_replace_actor_mesh"),
			TEXT("modeling_remove_material_slot"),
			TEXT("editor_close"),
			TEXT("editor_prepare_exit"),
			TEXT("editor_resolve_dirty_resources"),
			TEXT("audio_submix_record_start"),
			TEXT("audio_submix_record_stop"),
			TEXT("audio_submix_record_cancel"),
			TEXT("packaging_run_build"),
			TEXT("packaging_task_status"),
			TEXT("packaging_cancel_task"),
			TEXT("localization_gather_text"),
			TEXT("localization_compile_text"),
			TEXT("world_partition_resave_actors"),
			TEXT("anim_blueprint_screenshot"),
			TEXT("anim_sequence_screenshot"),
			TEXT("behavior_tree_screenshot"),
			TEXT("blueprint_screenshot"),
			TEXT("control_rig_screenshot"),
			TEXT("metasound_screenshot"),
			TEXT("screenshot_viewport"),
			TEXT("screenshot_viewport_buffer"),
			TEXT("pcg_screenshot"),
			TEXT("editor_viewport_screenshot"),
			TEXT("niagara_screenshot"),
			TEXT("sequence_screenshot"),
			TEXT("sound_cue_screenshot"),
			TEXT("state_tree_screenshot"),
			TEXT("texture_screenshot"),
			TEXT("umg_screenshot"),
			TEXT("modeling_voxel_grid_begin"),
			TEXT("modeling_voxel_grid_add_bounds"),
			TEXT("modeling_voxel_grid_remove_bounds"),
			TEXT("modeling_voxel_grid_apply"),
			TEXT("save_current_level"),
			TEXT("ik_rig_apply_folder"),
			TEXT("level_streaming_save"),
			TEXT("modeling_save_mesh_asset"),
			TEXT("level_topology_set_actor_assignments"),
			TEXT("config_apply_section"),
			TEXT("project_settings_apply_page"),
			TEXT("device_profile_apply_json"),
			TEXT("localization_apply_json"),
			TEXT("platform_config_apply_json"),
			TEXT("scalability_apply_json"),
			TEXT("string_table_apply_json"),
			TEXT("montage_add_notify"),
			TEXT("montage_add_notify_state"),
			TEXT("montage_add_notify_track"),
			TEXT("montage_add_section"),
			TEXT("montage_add_segment"),
			TEXT("montage_add_slot_track"),
			TEXT("montage_remove_notify"),
			TEXT("montage_remove_notify_track"),
			TEXT("montage_remove_section"),
			TEXT("montage_remove_segment"),
			TEXT("montage_remove_skeleton_slot"),
			TEXT("montage_remove_slot_track"),
			TEXT("montage_rename_section"),
			TEXT("montage_rename_skeleton_slot"),
			TEXT("montage_rename_slot_track"),
			TEXT("montage_set_blend_options"),
			TEXT("montage_set_next_section"),
			TEXT("montage_set_preview_mesh"),
			TEXT("montage_set_section_time"),
			TEXT("montage_set_skeleton_slot_group"),
			TEXT("montage_set_sync_group"),
			TEXT("montage_update_notify"),
			TEXT("montage_update_segment"),
			TEXT("sequence_add_actor_binding"),
			TEXT("sequence_add_bool_property_key"),
			TEXT("sequence_add_bool_property_track"),
			TEXT("sequence_add_float_property_key"),
			TEXT("sequence_add_float_property_track"),
			TEXT("sequence_add_integer_property_key"),
			TEXT("sequence_add_integer_property_track"),
			TEXT("sequence_add_property_key"),
			TEXT("sequence_add_property_track"),
			TEXT("sequence_add_skeletal_animation_section"),
			TEXT("sequence_add_skeletal_animation_track"),
			TEXT("sequence_add_transform_key"),
			TEXT("sequence_add_transform_track"),
			TEXT("sequence_add_visibility_key"),
			TEXT("sequence_add_visibility_track"),
			TEXT("sequence_remove_actor_binding"),
			TEXT("sequence_remove_skeletal_animation_section"),
			TEXT("sequence_set_level_sequence_display_rate"),
			TEXT("sequence_set_level_sequence_playback_range"),
			TEXT("sequence_update_skeletal_animation_section"),
			TEXT("anim_blueprint_add_anim_layer"),
			TEXT("anim_blueprint_add_call_function_node"),
			TEXT("anim_blueprint_add_conduit"),
			TEXT("anim_blueprint_add_custom_event_node"),
			TEXT("anim_blueprint_add_event_dispatcher"),
			TEXT("anim_blueprint_add_event_node"),
			TEXT("anim_blueprint_add_function_graph"),
			TEXT("anim_blueprint_add_macro_graph"),
			TEXT("anim_blueprint_add_node_by_class"),
			TEXT("anim_blueprint_add_state"),
			TEXT("anim_blueprint_add_state_alias"),
			TEXT("anim_blueprint_add_state_machine"),
			TEXT("anim_blueprint_add_transition"),
			TEXT("anim_blueprint_add_variable"),
			TEXT("anim_blueprint_add_variable_node"),
			TEXT("anim_blueprint_clear_entry_state"),
			TEXT("anim_blueprint_connect_pins"),
			TEXT("anim_blueprint_disconnect_pins"),
			TEXT("anim_blueprint_implement_layer_interface"),
			TEXT("anim_blueprint_remove_anim_layer"),
			TEXT("anim_blueprint_remove_layer_interface"),
			TEXT("anim_blueprint_remove_node"),
			TEXT("anim_blueprint_remove_state_machine"),
			TEXT("anim_blueprint_remove_state_node"),
			TEXT("anim_blueprint_remove_transition"),
			TEXT("anim_blueprint_remove_variable"),
			TEXT("anim_blueprint_rename_anim_layer"),
			TEXT("anim_blueprint_rename_state_machine"),
			TEXT("anim_blueprint_rename_state_node"),
			TEXT("anim_blueprint_reparent"),
			TEXT("anim_blueprint_set_cdo_property"),
			TEXT("anim_blueprint_set_entry_state"),
			TEXT("anim_blueprint_set_pin_default_value"),
			TEXT("anim_blueprint_set_preview_animation_blueprint"),
			TEXT("anim_blueprint_set_preview_mesh"),
			TEXT("anim_blueprint_set_state_alias_targets"),
			TEXT("anim_blueprint_set_state_properties"),
			TEXT("anim_blueprint_set_transition_properties"),
			TEXT("blueprint_add_call_function_node"),
			TEXT("blueprint_add_component"),
			TEXT("blueprint_add_component_bound_event"),
			TEXT("blueprint_add_custom_event_node"),
			TEXT("blueprint_add_dynamic_cast_node"),
			TEXT("blueprint_add_enhanced_input_action_event"),
			TEXT("blueprint_add_enhanced_input_add_mapping_context_node"),
			TEXT("blueprint_add_enhanced_input_local_player_subsystem_node"),
			TEXT("blueprint_add_event_dispatcher"),
			TEXT("blueprint_add_event_node"),
			TEXT("blueprint_add_function_graph"),
			TEXT("blueprint_add_macro_graph"),
			TEXT("blueprint_add_node_by_class"),
			TEXT("blueprint_add_variable"),
			TEXT("blueprint_add_variable_node"),
			TEXT("blueprint_connect_pins"),
			TEXT("blueprint_disconnect_pins"),
			TEXT("blueprint_remove_component"),
			TEXT("blueprint_remove_node"),
			TEXT("blueprint_remove_variable"),
			TEXT("blueprint_reparent"),
			TEXT("blueprint_set_cdo_property"),
			TEXT("blueprint_set_component_property"),
			TEXT("blueprint_set_pin_default_value"),
			TEXT("material_add_expression"),
			TEXT("material_connect_expression_to_property"),
			TEXT("material_connect_expressions"),
			TEXT("material_delete_expression"),
			TEXT("material_set_expression_property"),
			TEXT("material_set_instance_parent"),
			TEXT("material_set_parameter"),
			TEXT("material_set_property"),
			TEXT("material_set_scalar_parameter"),
			TEXT("material_set_static_switch_parameter"),
			TEXT("material_set_texture_parameter"),
			TEXT("material_set_vector_parameter"),
			TEXT("niagara_emitter_add_event_handler"),
			TEXT("niagara_emitter_add_renderer"),
			TEXT("niagara_emitter_clear_parent"),
			TEXT("niagara_emitter_parameter_add"),
			TEXT("niagara_emitter_parameter_remove"),
			TEXT("niagara_emitter_parameter_set"),
			TEXT("niagara_emitter_remove_event_handler"),
			TEXT("niagara_emitter_remove_renderer"),
			TEXT("niagara_emitter_set_event_handler_property"),
			TEXT("niagara_emitter_set_property"),
			TEXT("niagara_emitter_set_renderer_property"),
			TEXT("sequence_add_umg_widget_color_key"),
			TEXT("sequence_add_umg_widget_float_key"),
			TEXT("sequence_add_umg_widget_opacity_key"),
			TEXT("sequence_add_umg_widget_transform_key"),
			TEXT("sequence_add_umg_widget_translation_key"),
			TEXT("sequence_create_umg_animation"),
			TEXT("sequence_remove_umg_animation"),
			TEXT("sequence_rename_umg_animation"),
			TEXT("sequence_set_umg_animation_display_rate"),
			TEXT("sequence_set_umg_animation_playback_range"),
			TEXT("umg_add_widget"),
			TEXT("umg_bind_widget_property_to_variable"),
			TEXT("umg_remove_widget"),
			TEXT("umg_rename_widget"),
			TEXT("umg_set_slot_property"),
			TEXT("umg_set_widget_property"),
		};
		return Retired;
	}

	using FExactCommandInvoker = bool (*)(const FUeAgentHttpServer*, const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, TArray<TSharedPtr<FJsonValue>>&, FString&);
	using FPrefixedCommandInvoker = bool (*)(const FUeAgentHttpServer*, const FString&, const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&);
	using FExactNoCtxConstMethod = bool (FUeAgentHttpServer::*)(TSharedPtr<FJsonObject>&, FString&) const;
	using FExactWithCtxConstMethod = bool (FUeAgentHttpServer::*)(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&) const;
	using FExactWithCtxArtifactsConstMethod = bool (FUeAgentHttpServer::*)(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, TArray<TSharedPtr<FJsonValue>>&, FString&) const;
	using FExactWithCtxMutableMethod = bool (FUeAgentHttpServer::*)(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&);
	using FPrefixedConstMethod = bool (FUeAgentHttpServer::*)(const FString&, const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&) const;

	struct FExactCommandEntry
	{
		const TCHAR* Name;
		FExactCommandInvoker Invoker;
	};

	struct FPrefixedCommandEntry
	{
		const TCHAR* Prefix;
		FPrefixedCommandInvoker Invoker;
	};

	struct FUeAgentHttpServerCommandAccess
	{
		template<FExactNoCtxConstMethod Method>
		static bool InvokeExactNoCtx(const FUeAgentHttpServer* Server, const FUeAgentRequestContext&, TSharedPtr<FJsonObject>& OutDataRef, TArray<TSharedPtr<FJsonValue>>&, FString& OutErrRef)
		{
			return (Server->*Method)(OutDataRef, OutErrRef);
		}

		template<FExactWithCtxConstMethod Method>
		static bool InvokeExactWithCtx(const FUeAgentHttpServer* Server, const FUeAgentRequestContext& CommandCtx, TSharedPtr<FJsonObject>& OutDataRef, TArray<TSharedPtr<FJsonValue>>&, FString& OutErrRef)
		{
			return (Server->*Method)(CommandCtx, OutDataRef, OutErrRef);
		}

		template<FExactWithCtxArtifactsConstMethod Method>
		static bool InvokeExactWithCtxArtifacts(const FUeAgentHttpServer* Server, const FUeAgentRequestContext& CommandCtx, TSharedPtr<FJsonObject>& OutDataRef, TArray<TSharedPtr<FJsonValue>>& OutArtifactsRef, FString& OutErrRef)
		{
			return (Server->*Method)(CommandCtx, OutDataRef, OutArtifactsRef, OutErrRef);
		}

		template<FExactWithCtxMutableMethod Method>
		static bool InvokeExactMutableWithCtx(const FUeAgentHttpServer* Server, const FUeAgentRequestContext& CommandCtx, TSharedPtr<FJsonObject>& OutDataRef, TArray<TSharedPtr<FJsonValue>>&, FString& OutErrRef)
		{
			return (const_cast<FUeAgentHttpServer*>(Server)->*Method)(CommandCtx, OutDataRef, OutErrRef);
		}

		template<FPrefixedConstMethod Method>
		static bool InvokePrefixed(const FUeAgentHttpServer* Server, const FString& CommandLower, const FUeAgentRequestContext& CommandCtx, TSharedPtr<FJsonObject>& OutDataRef, FString& OutErrRef)
		{
			return (Server->*Method)(CommandLower, CommandCtx, OutDataRef, OutErrRef);
		}

		static FExactCommandInvoker FindExactCommandInvoker(const FString& CommandLower)
		{
			static const FExactCommandEntry GExactCommandEntries[] = {
#include "Server/Generated/UeAgentCommandRegistry_NodeGraphExact.h"
				{ TEXT("get_world_state"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdGetWorldState> },
				{ TEXT("list_actors"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdListActors> },
				{ TEXT("actor_list_components"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdActorListComponents> },
				{ TEXT("level_list_actor_components"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdActorListComponents> },
				{ TEXT("level_get_actor_property"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGetActorProperty> },
				{ TEXT("level_get_component_property"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGetComponentProperty> },
				{ TEXT("level_set_skeletal_mesh_morph_target"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetSkeletalMeshMorphTarget> },
				{ TEXT("level_set_skeletal_animation_pose"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetSkeletalAnimationPose> },
				{ TEXT("level_spawn_wall_with_opening"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSpawnWallWithOpening> },
				{ TEXT("level_get_selection"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdLevelGetSelection> },
				{ TEXT("level_set_selection"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetSelection> },
				{ TEXT("level_get_actor_transform"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGetActorTransform> },
				{ TEXT("probe_actor_transform_candidates"), &InvokeExactWithCtxArtifacts<&FUeAgentHttpServer::CmdProbeActorTransformCandidates> },
				{ TEXT("probe_camera_pixel_scale"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdProbeCameraPixelScale> },
				{ TEXT("probe_ik_goal_candidates"), &InvokeExactWithCtxArtifacts<&FUeAgentHttpServer::CmdProbeIKGoalCandidates> },
				{ TEXT("probe_ik_goal_review_frames"), &InvokeExactWithCtxArtifacts<&FUeAgentHttpServer::CmdProbeIKGoalReviewFrames> },
				{ TEXT("probe_ik_goal_apply_candidate"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdProbeIKGoalApplyCandidate> },
				{ TEXT("level_content_query_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelContentQueryJson> },
				{ TEXT("level_content_validate_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelContentValidateJson> },
				{ TEXT("level_content_plan_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelContentPlanJson> },
				{ TEXT("level_content_apply_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelContentApplyJson> },
				{ TEXT("level_content_diff_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelContentDiffJson> },
				{ TEXT("level_content_snapshot_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelContentSnapshotJson> },
				{ TEXT("level_content_delete_scope"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelContentDeleteScope> },
				{ TEXT("level_content_merge_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelContentMergeJson> },
				{ TEXT("level_content_repair_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelContentRepairJson> },
				{ TEXT("level_export_folder"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelExportFolder> },
				{ TEXT("level_apply_folder"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelApplyFolder> },
				{ TEXT("level_validate_folder"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelValidateFolder> },
				{ TEXT("level_diff_folder"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelDiffFolder> },
				{ TEXT("level_actor_export_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelActorExportJson> },
				{ TEXT("level_actor_validate_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelActorValidateJson> },
				{ TEXT("level_actor_apply_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelActorApplyJson> },
				{ TEXT("level_actor_diff_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelActorDiffJson> },
				{ TEXT("level_actor_snapshot_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelActorSnapshotJson> },
				{ TEXT("viewport_get_warnings"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportGetWarnings> },
				{ TEXT("viewport_set_realtime"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportSetRealtime> },
				{ TEXT("viewport_set_game_view"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportSetGameView> },
				{ TEXT("viewport_get_editor_interaction_state"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdViewportGetEditorInteractionState> },
				{ TEXT("viewport_restore_editor_interaction_state"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportRestoreEditorInteractionState> },
				{ TEXT("viewport_list_view_modes"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdViewportListViewModes> },
				{ TEXT("viewport_frame_folder"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportFrameFolder> },
				{ TEXT("viewport_deproject_screen_to_world"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportDeprojectScreenToWorld> },
				{ TEXT("viewport_trace_screen_point"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportTraceScreenPoint> },
				{ TEXT("viewport_pick_actor_at_screen"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportPickActorAtScreen> },
				{ TEXT("viewport_select_actor_at_screen"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportSelectActorAtScreen> },
				{ TEXT("level_get_nearby_mesh_shapes"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGetNearbyMeshShapes> },
				{ TEXT("level_validate_connectivity"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelValidateConnectivity> },
				{ TEXT("navmesh_build"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdNavmeshBuild> },
				{ TEXT("navmesh_project_point"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdNavmeshProjectPoint> },
				{ TEXT("navmesh_find_path"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdNavmeshFindPath> },
				{ TEXT("level_trace_world_ray"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelTraceWorldRay> },
				{ TEXT("level_sweep_capsule"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSweepCapsule> },
				{ TEXT("level_sweep_capsule_path"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSweepCapsulePath> },
				{ TEXT("level_check_overlaps"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelCheckOverlaps> },
				{ TEXT("level_snap_to_surface"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSnapToSurface> },
				{ TEXT("mesh_get_closest_vertex"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdMeshGetClosestVertex> },
				{ TEXT("mesh_get_vertex_world_position"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdMeshGetVertexWorldPosition> },
				{ TEXT("level_align_actor_vertex_to_vertex"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelAlignActorVertexToVertex> },
				{ TEXT("level_align_actor_by_bounds"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelAlignActorByBounds> },
				{ TEXT("level_align_face_to_face"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelAlignFaceToFace> },
				{ TEXT("level_create_blank"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelCreateBlank> },
				{ TEXT("level_open"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelOpen> },
				{ TEXT("editor_get_open_assets"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdEditorGetOpenAssets> },
				{ TEXT("open_asset_editor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdOpenAssetEditor> },
				{ TEXT("save_asset"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdSaveAsset> },
				{ TEXT("asset_duplicate"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetDuplicate> },
				{ TEXT("asset_move"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetMove> },
				{ TEXT("asset_fixup_redirectors"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetFixupRedirectors> },
				{ TEXT("asset_reference_graph"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetReferenceGraph> },
				{ TEXT("asset_export_property_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetExportPropertyJson> },
				{ TEXT("asset_apply_property_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetApplyPropertyJson> },
				{ TEXT("curve_export_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdCurveExportJson> },
				{ TEXT("curve_apply_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdCurveApplyJson> },
				{ TEXT("editor_list_dirty_resources"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdEditorListDirtyResources> },
				{ TEXT("landscape_create"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLandscapeCreate> },
				{ TEXT("landscape_raise_circle"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLandscapeRaiseCircle> },
				{ TEXT("project_settings_export_index"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdProjectSettingsExportIndex> },
				{ TEXT("project_settings_export_page"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdProjectSettingsExportPage> },
				{ TEXT("project_settings_validate_page"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdProjectSettingsValidatePage> },
				{ TEXT("project_settings_diff_page"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdProjectSettingsDiffPage> },
				{ TEXT("config_export_section"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdConfigExportSection> },
			};

			for (const FExactCommandEntry& Entry : GExactCommandEntries)
			{
				if (CommandLower == Entry.Name)
				{
					return Entry.Invoker;
				}
			}
			return nullptr;
		}

		static FPrefixedCommandInvoker FindPrefixedCommandInvoker(const FString& CommandLower)
		{
			if (GetRetiredLegacyCommandIds().Contains(CommandLower))
			{
				return nullptr;
			}
			static const FPrefixedCommandEntry GPrefixedCommandEntries[] = {
				{ TEXT("blueprint_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteBlueprintCommand> },
				{ TEXT("anim_blueprint_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAnimBlueprintCommand> },
				{ TEXT("montage_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteMontageCommand> },
				{ TEXT("umg_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteUmgCommand> },
				{ TEXT("static_mesh_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteStaticMeshCommand> },
				{ TEXT("enhanced_input_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteEnhancedInputCommand> },
				{ TEXT("material_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteMaterialCommand> },
				{ TEXT("sequence_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteSequenceCommand> },
				{ TEXT("anim_sequence_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAnimSequenceCommand> },
				{ TEXT("blendspace_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteBlendSpaceCommand> },
				{ TEXT("skeleton_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteSkeletonCommand> },
				{ TEXT("skeletal_mesh_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteSkeletalMeshCommand> },
				{ TEXT("ik_rig_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteIKRigCommand> },
				{ TEXT("ik_retargeter_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteIKRetargeterCommand> },
				{ TEXT("retarget_batch_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteRetargetBatchCommand> },
				{ TEXT("control_rig_shape_library_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteControlRigShapeLibraryCommand> },
				{ TEXT("control_rig_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteControlRigCommand> },
				{ TEXT("geometry_cache_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteGeometryCacheCommand> },
				{ TEXT("deformer_graph_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteDeformerGraphCommand> },
				{ TEXT("deformer_source_library_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteDeformerSourceLibraryCommand> },
				{ TEXT("mesh_deformer_collection_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteMeshDeformerCollectionCommand> },
				{ TEXT("ml_deformer_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteMLDeformerCommand> },
				{ TEXT("niagara_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteNiagaraCommand> },
				{ TEXT("pcg_"), &InvokePrefixed<&FUeAgentHttpServer::ExecutePCGCommand> },
				{ TEXT("modeling_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteModelingCommand> },
				{ TEXT("editor_viewport_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteEditorViewportCommand> },
				{ TEXT("blackboard_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAICommand> },
				{ TEXT("behavior_tree_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAICommand> },
				{ TEXT("bt_node_blueprint_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAICommand> },
				{ TEXT("state_tree_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAICommand> },
				{ TEXT("eqs_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAICommand> },
				{ TEXT("ai_perception_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAICommand> },
				{ TEXT("navigation_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAICommand> },
				{ TEXT("smart_object_definition_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAICommand> },
				{ TEXT("smart_object_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAICommand> },
				{ TEXT("ai_behavior_stack_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAICommand> },
				{ TEXT("level_topology_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteLevelTopologyCommand> },
				{ TEXT("level_streaming_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteLevelTopologyCommand> },
				{ TEXT("data_layer_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteLevelTopologyCommand> },
				{ TEXT("world_partition_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteLevelTopologyCommand> },
				{ TEXT("hlod_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteLevelTopologyCommand> },
				{ TEXT("sound_wave_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("sound_cue_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("metasound_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("sound_attenuation_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("sound_concurrency_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("sound_class_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("sound_mix_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("sound_submix_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("audio_effect_preset_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("audio_component_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("audio_volume_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("ambient_sound_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("audio_runtime_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("audio_submix_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("audio_stop_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAudioCommand> },
				{ TEXT("runtime_virtual_texture_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("virtual_texture_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("texture_cube_array_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("texture_array_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("texture_cube_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("volume_texture_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("texture_graph_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("texture_collection_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("texture_usage_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("data_asset_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteDataDrivenCommand> },
				{ TEXT("primary_asset_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteDataDrivenCommand> },
				{ TEXT("data_table_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteDataDrivenCommand> },
				{ TEXT("physical_material_"), &InvokePrefixed<&FUeAgentHttpServer::ExecutePhysicsCommand> },
				{ TEXT("physics_constraint_"), &InvokePrefixed<&FUeAgentHttpServer::ExecutePhysicsCommand> },
				{ TEXT("physics_asset_"), &InvokePrefixed<&FUeAgentHttpServer::ExecutePhysicsCommand> },
				{ TEXT("physics_component_"), &InvokePrefixed<&FUeAgentHttpServer::ExecutePhysicsCommand> },
				{ TEXT("physics_runtime_"), &InvokePrefixed<&FUeAgentHttpServer::ExecutePhysicsCommand> },
				{ TEXT("physics_capabilities_"), &InvokePrefixed<&FUeAgentHttpServer::ExecutePhysicsCommand> },
				{ TEXT("localization_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteProductionPipelineCommand> },
				{ TEXT("string_table_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteProductionPipelineCommand> },
				{ TEXT("packaging_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteProductionPipelineCommand> },
				{ TEXT("target_platform_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteProductionPipelineCommand> },
				{ TEXT("platform_config_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteProductionPipelineCommand> },
				{ TEXT("device_profile_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteProductionPipelineCommand> },
				{ TEXT("scalability_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteProductionPipelineCommand> },
				{ TEXT("platform_profile_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteProductionPipelineCommand> },
				{ TEXT("texture_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("render_target_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("scene_capture_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("media_source_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("media_player_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("media_texture_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("media_runtime_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("sparse_volume_texture_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("subuv_animation_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("paper_sprite_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("paper_flipbook_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("paper_tileset_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("paper_tilemap_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("paper2d_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
				{ TEXT("light_profile_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteTextureRenderTargetCommand> },
			};

			for (const FPrefixedCommandEntry& Entry : GPrefixedCommandEntries)
			{
				if (CommandLower.StartsWith(Entry.Prefix))
				{
					return Entry.Invoker;
				}
			}
			return nullptr;
		}
	};
}

namespace UeAgentCommandRegistry
{
	FExactCommandInvoker FindExactCommandInvoker(const FString& CommandLower)
	{
		if (GetRetiredLegacyCommandIds().Contains(CommandLower))
		{
			return nullptr;
		}
		return FUeAgentHttpServerCommandAccess::FindExactCommandInvoker(CommandLower);
	}

	FPrefixedCommandInvoker FindPrefixedCommandInvoker(const FString& CommandLower)
	{
		if (CommandLower.Contains(TEXT("."))
			|| GetRetiredLegacyCommandIds().Contains(CommandLower))
		{
			return nullptr;
		}
		return FUeAgentHttpServerCommandAccess::FindPrefixedCommandInvoker(CommandLower);
	}

	bool IsExplicitlyUnsupportedCommand(const FString& CommandLower)
	{
		return GetExplicitlyUnsupportedCommandIds().Contains(CommandLower)
			|| GetRetiredLegacyCommandIds().Contains(CommandLower);
	}
}
