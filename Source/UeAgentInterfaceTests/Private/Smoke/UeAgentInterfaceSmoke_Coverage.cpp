// Copyright Epic Games, Inc. All Rights Reserved.

#include "Containers/Set.h"
#include "Misc/AutomationTest.h"

namespace
{
	struct FUaiSmokeCoverageDebtEntry
	{
		const TCHAR* Command;
		const TCHAR* Owner;
		const TCHAR* PreviousStatus;
		const TCHAR* PreviousPriority;
	};

	static const FUaiSmokeCoverageDebtEntry GUaiSmokeCoverageDebtLedger[] =
	{
		{ TEXT("actor_list_components"), TEXT("Core.LevelViewportModeling"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("actor_set_property"), TEXT("Core.LevelViewportModeling"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("anim_blueprint_add_event_dispatcher"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("anim_blueprint_add_event_node"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("anim_blueprint_add_macro_graph"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("anim_blueprint_graph_get_view"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("anim_blueprint_graph_set_view"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("anim_blueprint_open_editor"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("anim_blueprint_remove_node"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("anim_blueprint_remove_variable"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("anim_blueprint_screenshot"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("anim_blueprint_viewport_get_camera"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("anim_blueprint_viewport_set_camera"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("anim_sequence_screenshot"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("asset_export_property_json"), TEXT("Core.LevelViewportModeling"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("asset_import_fbx_animation"), TEXT("Core.LevelViewportModeling"), TEXT("needs_fixture_smoke"), TEXT("P1") },
		{ TEXT("asset_import_fbx_skeletal_mesh"), TEXT("Core.LevelViewportModeling"), TEXT("needs_fixture_smoke"), TEXT("P1") },
		{ TEXT("asset_import_geometry_cache"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_fixture_smoke"), TEXT("P1") },
		{ TEXT("asset_import_texture"), TEXT("Core.LevelViewportModeling"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("behavior_tree_open_editor"), TEXT("AIBehaviorStack"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("behavior_tree_screenshot"), TEXT("AIBehaviorStack"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("blendspace_apply_json"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("blendspace_get_info"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("blendspace_preview_sample"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("blueprint_add_component_bound_event"), TEXT("BlueprintUmg"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("blueprint_add_dynamic_cast_node"), TEXT("BlueprintUmg"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("blueprint_add_enhanced_input_action_event"), TEXT("BlueprintUmg"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("blueprint_add_enhanced_input_add_mapping_context_node"), TEXT("BlueprintUmg"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("blueprint_add_enhanced_input_local_player_subsystem_node"), TEXT("BlueprintUmg"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("blueprint_add_event_node"), TEXT("BlueprintUmg"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("blueprint_add_variable_node"), TEXT("BlueprintUmg"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("blueprint_disconnect_pins"), TEXT("BlueprintUmg"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("blueprint_get_compile_log"), TEXT("BlueprintUmg"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("blueprint_graph_get_view"), TEXT("BlueprintUmg"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("blueprint_graph_set_view"), TEXT("BlueprintUmg"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("blueprint_reparent"), TEXT("BlueprintUmg"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("blueprint_screenshot"), TEXT("BlueprintUmg"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("blueprint_set_cdo_property"), TEXT("BlueprintUmg"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("blueprint_viewport_get_camera"), TEXT("BlueprintUmg"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("blueprint_viewport_set_camera"), TEXT("BlueprintUmg"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("component_set_property"), TEXT("Core.LevelViewportModeling"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("control_rig_apply_folder"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("control_rig_bake_to_animation"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("control_rig_bake_to_control_rig"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("control_rig_compile"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("control_rig_create"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("control_rig_export_folder"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("control_rig_get_compile_log"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("control_rig_get_info"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("control_rig_graph_get_view"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("control_rig_graph_set_view"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("control_rig_open_editor"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("control_rig_runtime_probe"), TEXT("AnimationIKNiagara"), TEXT("needs_runtime_smoke"), TEXT("P1") },
		{ TEXT("control_rig_screenshot"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("control_rig_shape_library_apply_json"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("control_rig_shape_library_create"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("control_rig_shape_library_export_json"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("control_rig_shape_library_get_info"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("control_rig_shape_library_validate_json"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("control_rig_validate_folder"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("control_rig_viewport_get_camera"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("control_rig_viewport_set_camera"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("curve_apply_json"), TEXT("Core.LevelViewportModeling"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("curve_export_json"), TEXT("Core.LevelViewportModeling"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("deformer_graph_apply_folder"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("deformer_graph_compile"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("deformer_graph_get_compile_log"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("deformer_graph_get_info"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("deformer_source_library_create"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("deformer_source_library_export_json"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("deformer_source_library_validate_json"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("editor_get_open_assets"), TEXT("Core.LevelViewportModeling"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("editor_prepare_exit"), TEXT("Core.LevelViewportModeling"), TEXT("needs_isolated_lifecycle_smoke"), TEXT("P2") },
		{ TEXT("enhanced_input_clear_mappings"), TEXT("MaterialStaticDeformerInput"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("enhanced_input_set_action_property"), TEXT("MaterialStaticDeformerInput"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("enhanced_input_set_mapping_context_property"), TEXT("MaterialStaticDeformerInput"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("geometry_cache_validate_against_skeletal_mesh"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("ik_retargeter_auto_align_pose"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("ik_rig_preview_solve"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("ik_rig_set_preview_mesh"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("level_add_actor_tag"), TEXT("Core.LevelViewportModeling"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("level_align_actor_by_bounds"), TEXT("Core.LevelViewportModeling"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("level_align_actor_vertex_to_vertex"), TEXT("Core.LevelViewportModeling"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("level_align_face_to_face"), TEXT("Core.LevelViewportModeling"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("level_attach_actor"), TEXT("Core.LevelViewportModeling"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("level_cleanup_empty_folders"), TEXT("Core.LevelViewportModeling"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("level_detach_actor"), TEXT("Core.LevelViewportModeling"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("level_generate_probes"), TEXT("Core.LevelViewportModeling"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("level_get_actor_property"), TEXT("Core.LevelViewportModeling"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("level_get_component_property"), TEXT("Core.LevelViewportModeling"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("level_get_selection"), TEXT("Core.LevelViewportModeling"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("level_list_actor_components"), TEXT("Core.LevelViewportModeling"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("level_set_actor_property"), TEXT("Core.LevelViewportModeling"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("level_set_actor_scale"), TEXT("Core.LevelViewportModeling"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("level_set_component_property"), TEXT("Core.LevelViewportModeling"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("level_set_skeletal_mesh_morph_target"), TEXT("Core.LevelViewportModeling"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("level_spawn_wall_with_opening"), TEXT("Core.LevelViewportModeling"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("material_connect_expressions"), TEXT("MaterialStaticDeformerInput"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("material_get_compile_log"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("material_open_editor"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("material_set_instance_parent"), TEXT("MaterialStaticDeformerInput"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("material_set_property"), TEXT("MaterialStaticDeformerInput"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("material_set_scalar_parameter"), TEXT("MaterialStaticDeformerInput"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("material_set_texture_parameter"), TEXT("MaterialStaticDeformerInput"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("material_set_vector_parameter"), TEXT("MaterialStaticDeformerInput"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("mesh_deformer_collection_apply_json"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("mesh_deformer_collection_create"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("mesh_deformer_collection_export_json"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("mesh_get_closest_vertex"), TEXT("Core.LevelViewportModeling"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("mesh_get_vertex_world_position"), TEXT("Core.LevelViewportModeling"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("ml_deformer_apply_json"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("ml_deformer_create"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("ml_deformer_export_json"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("ml_deformer_get_info"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("ml_deformer_get_training_log"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_runtime_smoke"), TEXT("P1") },
		{ TEXT("ml_deformer_validate_json"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P0") },
		{ TEXT("modeling_accept_tool"), TEXT("LevelViewportModeling"), TEXT("needs_isolated_lifecycle_smoke"), TEXT("P2") },
		{ TEXT("modeling_bevel_edges"), TEXT("LevelViewportModeling"), TEXT("generated_catalog_triage"), TEXT("P3") },
		{ TEXT("modeling_cancel_tool"), TEXT("LevelViewportModeling"), TEXT("needs_isolated_lifecycle_smoke"), TEXT("P2") },
		{ TEXT("modeling_clear_mesh_selection"), TEXT("LevelViewportModeling"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("modeling_duplicate_faces"), TEXT("LevelViewportModeling"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("modeling_get_active_tool_properties"), TEXT("LevelViewportModeling"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("modeling_inset_faces"), TEXT("LevelViewportModeling"), TEXT("generated_catalog_triage"), TEXT("P3") },
		{ TEXT("modeling_invoke_active_tool_action"), TEXT("LevelViewportModeling"), TEXT("generated_catalog_triage"), TEXT("P3") },
		{ TEXT("modeling_push_pull"), TEXT("LevelViewportModeling"), TEXT("generated_catalog_triage"), TEXT("P3") },
		{ TEXT("modeling_replace_actor_mesh"), TEXT("LevelViewportModeling"), TEXT("generated_catalog_triage"), TEXT("P3") },
		{ TEXT("modeling_save_mesh_asset"), TEXT("LevelViewportModeling"), TEXT("needs_isolated_lifecycle_smoke"), TEXT("P2") },
		{ TEXT("modeling_select_mesh_elements_via_screen"), TEXT("LevelViewportModeling"), TEXT("generated_catalog_triage"), TEXT("P3") },
		{ TEXT("modeling_select_mesh_elements_via_world_ray"), TEXT("LevelViewportModeling"), TEXT("generated_catalog_triage"), TEXT("P3") },
		{ TEXT("modeling_set_active_tool_property"), TEXT("LevelViewportModeling"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("modeling_set_mesh_selection_mode"), TEXT("LevelViewportModeling"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("modeling_set_selection"), TEXT("LevelViewportModeling"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("modeling_snap_to_ground"), TEXT("LevelViewportModeling"), TEXT("generated_catalog_triage"), TEXT("P3") },
		{ TEXT("modeling_start_tool"), TEXT("LevelViewportModeling"), TEXT("generated_catalog_triage"), TEXT("P3") },
		{ TEXT("montage_open_editor"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("navmesh_spawn_bounds_volume"), TEXT("Core.LevelViewportModeling"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("niagara_apply_stack_issue_fix"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("niagara_compile_system"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("niagara_duplicate_asset"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("niagara_emitter_add_event_handler"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_add_simulation_stage"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_add_stage_module"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_add_stage_node"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_clear_module_input"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_clear_parent"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_connect_stage_nodes"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_disconnect_stage_nodes"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_get_event_handler_property"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_emitter_get_property"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_emitter_get_renderer_property"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_emitter_get_stage_node_property"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_emitter_list_event_handlers"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_emitter_list_module_inputs"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_emitter_list_stage_modules"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_emitter_list_stage_nodes"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_emitter_list_stages"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_emitter_move_renderer"), TEXT("AnimationIKNiagara"), TEXT("generated_catalog_triage"), TEXT("P3") },
		{ TEXT("niagara_emitter_move_stage_module"), TEXT("AnimationIKNiagara"), TEXT("generated_catalog_triage"), TEXT("P3") },
		{ TEXT("niagara_emitter_parameter_add"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("niagara_emitter_parameter_get"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_emitter_parameter_list"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_emitter_parameter_remove"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("niagara_emitter_parameter_set"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("niagara_emitter_remove_event_handler"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_remove_renderer"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_remove_stage"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_remove_stage_module"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_remove_stage_node"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_set_event_handler_property"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_set_module_input"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_set_property"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_set_renderer_property"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_set_stage_module_enabled"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_set_stage_node_property"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_emitter_set_stage_property"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_get_compile_log"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_get_stack_issues"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_list_assets"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_list_module_library"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_open_editor"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("niagara_preview_advance"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("niagara_refresh_system"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("niagara_screenshot"), TEXT("AnimationIKNiagara"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("niagara_set_property"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_system_get_property"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_system_list_emitters"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_system_list_module_inputs"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_system_list_stage_modules"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_system_move_emitter"), TEXT("AnimationIKNiagara"), TEXT("generated_catalog_triage"), TEXT("P3") },
		{ TEXT("niagara_system_remove_emitter"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_system_runtime_probe"), TEXT("AnimationIKNiagara"), TEXT("needs_runtime_smoke"), TEXT("P1") },
		{ TEXT("niagara_system_set_emitter_enabled"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_system_set_emitter_version"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_system_set_module_input"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("niagara_user_parameter_get"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_user_parameter_list"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("niagara_user_parameter_remove"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("niagara_user_parameter_set"), TEXT("AnimationIKNiagara"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("open_asset_editor"), TEXT("Core.LevelViewportModeling"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("redo"), TEXT("Core"), TEXT("needs_isolated_lifecycle_smoke"), TEXT("P2") },
		{ TEXT("save_asset"), TEXT("Core.LevelViewportModeling"), TEXT("needs_isolated_lifecycle_smoke"), TEXT("P2") },
		{ TEXT("save_current_level"), TEXT("Core.LevelViewportModeling"), TEXT("needs_isolated_lifecycle_smoke"), TEXT("P2") },
		{ TEXT("screenshot_viewport"), TEXT("Core.LevelViewportModeling"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("screenshot_viewport_buffer"), TEXT("Core.LevelViewportModeling"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("sequence_add_umg_widget_float_key"), TEXT("Sequence"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("sequence_add_umg_widget_translation_key"), TEXT("Sequence"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("sequence_open_level_sequence"), TEXT("Sequence"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("skeletal_mesh_get_morph_targets"), TEXT("AnimationIKNiagara"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("skeletal_mesh_remove_morph_target"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("skeletal_mesh_remove_skin_weight_profile"), TEXT("AnimationIKNiagara"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("state_tree_open_editor"), TEXT("AIBehaviorStack"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("state_tree_screenshot"), TEXT("AIBehaviorStack"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("static_mesh_build"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("static_mesh_get_bounds"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("static_mesh_get_local_corners"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_readonly_smoke"), TEXT("P2") },
		{ TEXT("static_mesh_open_editor"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("static_mesh_reimport"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("static_mesh_set_collision_boxes"), TEXT("MaterialStaticDeformerInput"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("static_mesh_set_preview_view"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("static_mesh_set_property"), TEXT("MaterialStaticDeformerInput"), TEXT("indirect_workflow_candidate"), TEXT("P2") },
		{ TEXT("static_mesh_validate_geometry"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("static_mesh_validate_uvs"), TEXT("MaterialStaticDeformerInput"), TEXT("needs_direct_smoke"), TEXT("P1") },
		{ TEXT("undo"), TEXT("Core"), TEXT("needs_isolated_lifecycle_smoke"), TEXT("P2") },
		{ TEXT("viewport_focus_actor"), TEXT("Core.LevelViewportModeling"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("viewport_focus_actor_safe"), TEXT("Core.LevelViewportModeling"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("viewport_frame_actors"), TEXT("Core.LevelViewportModeling"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("viewport_frame_selection"), TEXT("Core.LevelViewportModeling"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("viewport_set_game_view"), TEXT("Core.LevelViewportModeling"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("viewport_set_realtime"), TEXT("Core.LevelViewportModeling"), TEXT("needs_ui_smoke"), TEXT("P1") },
		{ TEXT("viewport_trace_screen_point"), TEXT("Core.LevelViewportModeling"), TEXT("needs_ui_smoke"), TEXT("P1") },
	};

	static bool IsAllowedCoverageDebtStatus(const FString& Status)
	{
		return Status == TEXT("needs_direct_smoke")
			|| Status == TEXT("needs_runtime_smoke")
			|| Status == TEXT("needs_fixture_smoke")
			|| Status == TEXT("needs_ui_smoke")
			|| Status == TEXT("needs_isolated_lifecycle_smoke")
			|| Status == TEXT("needs_readonly_smoke")
			|| Status == TEXT("indirect_workflow_candidate")
			|| Status == TEXT("generated_catalog_triage");
	}

	static bool IsAllowedCoverageDebtPriority(const FString& Priority)
	{
		return Priority == TEXT("P0") || Priority == TEXT("P1") || Priority == TEXT("P2") || Priority == TEXT("P3");
	}

	static bool IsKnownSingleWordCommand(const FString& Command)
	{
		return Command == TEXT("redo") || Command == TEXT("undo");
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSmokeCoverageLedgerTest,
	"GptProjectTest.UeAgentInterface.Smoke.CoverageLedger",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentInterfaceSmokeCoverageLedgerTest::RunTest(const FString& Parameters)
{
	TSet<FString> SeenCommands;
	TestTrue(TEXT("coverage debt ledger is not empty"), UE_ARRAY_COUNT(GUaiSmokeCoverageDebtLedger) > 0);

	for (const FUaiSmokeCoverageDebtEntry& Entry : GUaiSmokeCoverageDebtLedger)
	{
		const FString Command(Entry.Command);
		const FString Owner(Entry.Owner);
		const FString PreviousStatus(Entry.PreviousStatus);
		const FString PreviousPriority(Entry.PreviousPriority);

		TestFalse(FString::Printf(TEXT("command is not empty: %s"), Entry.Command), Command.IsEmpty());
		TestTrue(FString::Printf(TEXT("command is a UAI command token: %s"), *Command), Command.Contains(TEXT("_")) || IsKnownSingleWordCommand(Command));
		TestFalse(FString::Printf(TEXT("owner is not empty for %s"), *Command), Owner.IsEmpty());
		TestTrue(FString::Printf(TEXT("known previous smoke status for %s"), *Command), IsAllowedCoverageDebtStatus(PreviousStatus));
		TestTrue(FString::Printf(TEXT("known previous smoke priority for %s"), *Command), IsAllowedCoverageDebtPriority(PreviousPriority));

		const bool bDuplicate = SeenCommands.Contains(Command);
		TestFalse(FString::Printf(TEXT("coverage ledger has no duplicate command: %s"), *Command), bDuplicate);
		if (!bDuplicate)
		{
			SeenCommands.Add(Command);
		}
	}

	return true;
}
