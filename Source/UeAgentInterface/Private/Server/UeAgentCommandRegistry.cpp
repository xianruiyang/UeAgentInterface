// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentCommandRegistry.h"
#include "Server/UeAgentHttpServer.h"

namespace
{
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
				{ TEXT("exec_batch"), &InvokeExactWithCtxArtifacts<&FUeAgentHttpServer::ExecuteBatchCommands> },
#include "Server/Generated/UeAgentCommandRegistry_NodeGraphExact.h"
				{ TEXT("get_world_state"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdGetWorldState> },
				{ TEXT("list_actors"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdListActors> },
				{ TEXT("actor_list_components"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdActorListComponents> },
				{ TEXT("level_list_actor_components"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdActorListComponents> },
				{ TEXT("level_get_actor_property"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGetActorProperty> },
				{ TEXT("actor_set_property"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdActorSetProperty> },
				{ TEXT("level_set_actor_property"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdActorSetProperty> },
				{ TEXT("level_get_component_property"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGetComponentProperty> },
				{ TEXT("component_set_property"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdComponentSetProperty> },
				{ TEXT("level_set_component_property"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdComponentSetProperty> },
				{ TEXT("level_set_skeletal_mesh_morph_target"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetSkeletalMeshMorphTarget> },
				{ TEXT("level_set_skeletal_animation_pose"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetSkeletalAnimationPose> },
				{ TEXT("spawn_actor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdSpawnActor> },
				{ TEXT("level_spawn_wall_with_opening"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSpawnWallWithOpening> },
				{ TEXT("destroy_actor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdDestroyActor> },
				{ TEXT("level_mark_probe"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelMarkProbe> },
				{ TEXT("level_generate_probes"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGenerateProbes> },
				{ TEXT("level_destroy_folder_actors"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelDestroyFolderActors> },
				{ TEXT("level_cleanup_empty_folders"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelCleanupEmptyFolders> },
				{ TEXT("level_attach_actor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelAttachActor> },
				{ TEXT("level_detach_actor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelDetachActor> },
				{ TEXT("level_get_selection"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdLevelGetSelection> },
				{ TEXT("level_set_selection"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetSelection> },
				{ TEXT("level_duplicate_actor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelDuplicateActor> },
				{ TEXT("level_set_actor_folder"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetActorFolder> },
				{ TEXT("level_add_actor_tag"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelAddActorTag> },
				{ TEXT("level_get_actor_transform"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGetActorTransform> },
				{ TEXT("level_set_actor_transform"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetActorTransform> },
				{ TEXT("probe_actor_transform_candidates"), &InvokeExactWithCtxArtifacts<&FUeAgentHttpServer::CmdProbeActorTransformCandidates> },
				{ TEXT("probe_camera_pixel_scale"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdProbeCameraPixelScale> },
				{ TEXT("probe_ik_goal_candidates"), &InvokeExactWithCtxArtifacts<&FUeAgentHttpServer::CmdProbeIKGoalCandidates> },
				{ TEXT("probe_ik_goal_review_frames"), &InvokeExactWithCtxArtifacts<&FUeAgentHttpServer::CmdProbeIKGoalReviewFrames> },
				{ TEXT("probe_ik_goal_apply_candidate"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdProbeIKGoalApplyCandidate> },
				{ TEXT("level_set_actor_location"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetActorLocation> },
				{ TEXT("level_set_actor_rotation"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetActorRotation> },
				{ TEXT("level_set_actor_scale"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetActorScale> },
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
				{ TEXT("viewport_get_camera"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdViewportGetCamera> },
				{ TEXT("viewport_get_warnings"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportGetWarnings> },
				{ TEXT("viewport_set_camera"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportSetCamera> },
				{ TEXT("viewport_set_realtime"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportSetRealtime> },
				{ TEXT("viewport_set_game_view"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportSetGameView> },
				{ TEXT("viewport_get_editor_interaction_state"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdViewportGetEditorInteractionState> },
				{ TEXT("viewport_restore_editor_interaction_state"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportRestoreEditorInteractionState> },
				{ TEXT("viewport_list_view_modes"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdViewportListViewModes> },
				{ TEXT("viewport_focus_actor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportFocusActor> },
				{ TEXT("viewport_focus_actor_safe"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportFocusActorSafe> },
				{ TEXT("viewport_frame_actor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportFrameActor> },
				{ TEXT("viewport_frame_selection"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportFrameSelection> },
				{ TEXT("viewport_frame_actors"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportFrameActors> },
				{ TEXT("viewport_frame_folder"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportFrameFolder> },
				{ TEXT("viewport_deproject_screen_to_world"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportDeprojectScreenToWorld> },
				{ TEXT("viewport_trace_screen_point"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportTraceScreenPoint> },
				{ TEXT("viewport_pick_actor_at_screen"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportPickActorAtScreen> },
				{ TEXT("viewport_select_actor_at_screen"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportSelectActorAtScreen> },
				{ TEXT("level_get_nearby_actor_obbs"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGetNearbyActorObbs> },
				{ TEXT("level_get_nearby_mesh_shapes"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGetNearbyMeshShapes> },
				{ TEXT("level_validate_connectivity"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelValidateConnectivity> },
				{ TEXT("navmesh_build"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdNavmeshBuild> },
				{ TEXT("navmesh_project_point"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdNavmeshProjectPoint> },
				{ TEXT("navmesh_find_path"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdNavmeshFindPath> },
				{ TEXT("navmesh_spawn_bounds_volume"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdNavmeshSpawnBoundsVolume> },
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
				{ TEXT("screenshot_viewport"), &InvokeExactWithCtxArtifacts<&FUeAgentHttpServer::CmdScreenshotViewport> },
				{ TEXT("screenshot_viewport_buffer"), &InvokeExactWithCtxArtifacts<&FUeAgentHttpServer::CmdScreenshotViewportBuffer> },
				{ TEXT("save_current_level"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdSaveCurrentLevel> },
				{ TEXT("level_create_blank"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelCreateBlank> },
				{ TEXT("level_open"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelOpen> },
				{ TEXT("begin_transaction"), &InvokeExactMutableWithCtx<&FUeAgentHttpServer::CmdBeginTransaction> },
				{ TEXT("end_transaction"), &InvokeExactMutableWithCtx<&FUeAgentHttpServer::CmdEndTransaction> },
				{ TEXT("undo"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdUndo> },
				{ TEXT("redo"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdRedo> },
				{ TEXT("editor_get_open_assets"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdEditorGetOpenAssets> },
				{ TEXT("open_asset_editor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdOpenAssetEditor> },
				{ TEXT("save_asset"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdSaveAsset> },
				{ TEXT("asset_duplicate"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetDuplicate> },
				{ TEXT("asset_move"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetMove> },
				{ TEXT("asset_delete"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetDelete> },
				{ TEXT("asset_fixup_redirectors"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetFixupRedirectors> },
				{ TEXT("asset_reference_graph"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetReferenceGraph> },
				{ TEXT("asset_import_texture"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetImportTexture> },
				{ TEXT("asset_import_fbx_skeletal_mesh"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetImportFbxSkeletalMesh> },
				{ TEXT("asset_import_fbx_animation"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetImportFbxAnimation> },
				{ TEXT("asset_import_geometry_cache"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetImportGeometryCache> },
				{ TEXT("asset_export_property_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetExportPropertyJson> },
				{ TEXT("asset_apply_property_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetApplyPropertyJson> },
				{ TEXT("curve_export_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdCurveExportJson> },
				{ TEXT("curve_apply_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdCurveApplyJson> },
				{ TEXT("editor_console_exec"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdEditorConsoleExec> },
				{ TEXT("editor_list_dirty_resources"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdEditorListDirtyResources> },
				{ TEXT("editor_resolve_dirty_resources"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdEditorResolveDirtyResources> },
				{ TEXT("editor_close"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdEditorClose> },
				{ TEXT("editor_prepare_exit"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdEditorPrepareExit> },
				{ TEXT("landscape_create"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLandscapeCreate> },
				{ TEXT("landscape_raise_circle"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLandscapeRaiseCircle> },
				{ TEXT("project_settings_export_index"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdProjectSettingsExportIndex> },
				{ TEXT("project_settings_export_page"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdProjectSettingsExportPage> },
				{ TEXT("project_settings_validate_page"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdProjectSettingsValidatePage> },
				{ TEXT("project_settings_apply_page"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdProjectSettingsApplyPage> },
				{ TEXT("project_settings_diff_page"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdProjectSettingsDiffPage> },
				{ TEXT("config_export_section"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdConfigExportSection> },
				{ TEXT("config_apply_section"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdConfigApplySection> },
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
		return FUeAgentHttpServerCommandAccess::FindExactCommandInvoker(CommandLower);
	}

	FPrefixedCommandInvoker FindPrefixedCommandInvoker(const FString& CommandLower)
	{
		return FUeAgentHttpServerCommandAccess::FindPrefixedCommandInvoker(CommandLower);
	}
}
