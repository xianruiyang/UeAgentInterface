# UeAgentInterface 指令分册

本目录是 `Plugins/UeAgentInterface/docs/UeAgentInterface_Usage.md` 的分册版。
用途是按模块快速查某条指令的作用、关键参数和适用场景。

## 分册列表

- `01_Core_Level_Assets_Landscape.md`
- `02_Blueprint.md`
- `03_UMG.md`
- `04_StaticMesh_EnhancedInput.md`
- `05_Material.md`
- `06_Sequence.md`
- `13_AnimationAssets_Skeleton.md`
- `14_IKRig_IKRetargeter.md`
- `12_Montage.md`
- `07_Niagara_System.md`
- `08_Niagara_Emitter.md`
- `09_Niagara_StageGraph.md`
- `15_Niagara_FolderFormat.md`
- `deprecatedCommand/README.md`
- `10_Modeling.md`
- `11_AnimBlueprint.md`
- `../UeAgentInterface_Status.md`

## 使用约定

- 插件底层协议仍是 `POST /api/exec`
- 日常自动化调用优先使用 `UeAgentInterfaceCMD/dist/uai-cli.exe`
- 批量任务优先使用 `run` 或 `batch`
- 复杂属性写入统一使用 UE `ImportText` 风格的 `value_text`
- 所有走 `value_text` / JSON / 结构化 JSON 的属性回写都应返回解析与读回状态：
  - `requested_value_text`：请求写入的原始字符串
  - `applied_value_text`：UE 导入后再读回的实际字符串
  - `property_import_status`：`imported_and_read_back`、`property_not_found`、`import_failed` 或专用状态
  - `property_import_verified`：是否已完成写入后的读回
  - `property_import_error`：失败时的具体错误，如 `property_not_found` 或 `property_import_failed:<property>:<value>`
  - `value_text_exact_match` / `value_text_changed_after_import`：请求值与读回值是否完全一致
  - `cpp_type`：目标属性 C++ 类型
- `value_text_changed_after_import=true` 不一定等于失败；UE 可能会规范化大小写、对象路径或浮点格式。但对向量、颜色、枚举等结构化值，它是排查“命令成功但值被回退/归零”的关键依据。
- JSON 结构错误不能静默吞掉。通用返回字段为 `warnings[]` 或结果项内的 `json_issues[]`，每项包含 `severity/code/path/message`。`asset_apply_property_json` 的坏条目和字段错误会进入对应 `property_results[].json_issues[]`。
- Blueprint / UMG / AnimBlueprint 变量类型统一使用 `pin_category/pin_subcategory/pin_subcategory_object/container_type/value_type`。常见和中频 UE 结构体、枚举、对象/类资产可写别名，例如 `vector/rotator/transform/linearcolor/hitresult/gameplaytag/key/slatebrush/collisionchannel/actor/staticmesh/niagarasystem/userwidget`；`map` 必须写 `value_type`。
- JSON / 结构化 JSON apply 的解析失败必须可见：
  - 单文件 `json_file` 读取或语法解析失败会直接失败返回 `json_file_not_found / load_json_file_failed / json_parse_failed`。
  - 文件夹式 workflow 中，可选 JSON 文件只有不存在时可跳过；如果文件存在但读取或语法解析失败，会失败返回并带文件路径。
  - 可恢复的坏条目，例如数组元素不是 object、缺 `name/id/property_name/value_text` 等，会写入 `warnings[] / warning_count` 或 `property_results[]`，不应静默丢弃。
  - 新增或维护 JSON/结构化 JSON apply 时必须优先接入统一诊断工具，至少覆盖未知字段、字段类型不匹配、必填字段缺失、数组元素类型错误，并在 warning/error 中保留完整 JSON path。
- 以 `UeAgentInterface_Usage.md` 为完整字段定义来源，本目录只保留高频说明

## 资产编辑优先级

当前资产编辑默认遵循这条优先级：

1. 单文件 JSON：
   - 适合小型、浅层资产
   - 典型：`asset_export_property_json / asset_apply_property_json`
   - 典型：`enhanced_input_export_*_json / enhanced_input_apply_*_json`
   - 典型：`montage_export_json / montage_apply_json`
2. 文件夹式结构化 JSON：
   - 适合 Blueprint / UMG / AnimBlueprint / Material 这类图或树结构资产
   - 典型：`*_export_folder / *_apply_folder`
   - 当前已新增：`sequence_export_folder / sequence_apply_folder`
   - 当前已新增：`niagara_export_folder / niagara_apply_folder`
   - 当前已新增：`niagara_emitter_export_folder / niagara_emitter_apply_folder`
   - 当前已新增：`niagara_script_export_folder / niagara_script_apply_folder`
   - Niagara 当前已覆盖 `NiagaraSystem / NiagaraEmitter / NiagaraScript` 三个完整 folder profile；System / Emitter apply 会随返回携带 Stack 感叹号信息，严格验收以 `warnings`、`stack_error_count` 和 `validation/coverage_report.json` 为依据
3. 原子命令：
   - 当同一资产字段或结构已经能被单文件 JSON / 文件夹式结构化 JSON 表达时，对应写入型原子命令统一标记为 **Deprecated for authoring**
   - 保留用途只包括 bootstrap、读取/探针验证、迁移脚本、schema 边界字段和失败后的定点补修

结论：

- 文档中的资产编辑不再默认推荐“逐条命令手搓”，而是优先推荐 `JSON / 结构化 JSON` 工作流。
- 已有 JSON / 结构化 JSON 主流程覆盖的零散写入命令保留但 deprecated for authoring；完整 authoring 必须走“创建最小骨架 -> 导出 -> 写基础结构 -> apply -> 再导出补全 -> 修改参数 -> 再 apply”的 JSON 工作流。
- 读取、创建最小资产、打开编辑器、编译、截图、导出、应用、dirty 处理和诊断命令不属于这类废弃写入命令。
- 暂无 JSON / 结构化 JSON profile 的领域不强行标废弃，例如 Modeling、IK Rig / IK Retargeter、Level Actor 放置、NavMesh、普通视口控制等。

废弃写入命令明细已抽到 `deprecatedCommand/`，覆盖面包括：

- Actor Blueprint：组件树、成员、图节点/连线、pin 默认值、CDO 默认值、reparent 等可进入 `blueprint_apply_folder` 的写入命令。
- WidgetBlueprint / UMG：控件树、Widget/Slot 属性、属性绑定，以及可进入 `umg_apply_folder` 的 UMG 动画写入命令。
- EnhancedInput：`InputAction / InputMappingContext` 的属性、映射增删和清空命令，主流程改用 `enhanced_input_apply_*_json`。
- StaticMesh / Animation Asset 通用属性：能由 `asset_apply_property_json` 表达的纯属性写入命令，例如 `static_mesh_set_property`、`anim_sequence_set_settings`。
- Material / Material Instance / Material Function：属性、参数、表达式图节点和连线写入命令，主流程改用对应 folder workflow。
- Level Sequence：播放设置、binding、track、key、section 等可进入 `sequence_apply_folder` 的写入命令。
- Montage：播放/混合/sync、slot、segment、section、notify、Skeleton slot/group 等可进入 `montage_apply_json` 的写入命令。
- AnimBlueprint：成员、逻辑图、Anim Layer、Layer Interface、State Machine、State/Transition、预览配置等可进入 `anim_blueprint_apply_folder` 的写入命令。
- Niagara：System、Emitter、Renderer、Event、Parameter、Stage、Module、Node、ModuleInput 等可进入 Niagara folder workflow 的写入命令。

## 通用方法论

对于“属性面很大、很难一开始就写全”的对象，当前推荐统一采用：

`bootstrap -> export -> refine -> apply`

即：

1. 先建最小骨架
2. 先让 UE 生成真实对象
3. 再导出 JSON / 结构化 JSON
4. 在导出模板上补全高价值属性
5. 再回写

这样做的目的：

- 不靠记忆硬猜属性名和 `value_text`
- 让后续编辑建立在 UE 已经确认可用的真实模板上
- 减少“第一次就想把所有属性写完”导致的返工

按对象复杂度选格式：

- 小对象：单文件 JSON
- 大对象：文件夹式结构化 JSON

## 当前同步状态（2026-04-23，已按实现复核）

本轮已按命令路由实现做过代码审计：

- 命令名层面，分册文档与实现当前无缺项。
- 若某条命令近期有增量扩展，优先以对应分册中的主表和“增量更新”说明为准。

以下指令已同步到分册文档：

- `get_world_state`
- `exec_batch`
- `begin_transaction`
- `end_transaction`
- `undo`
- `redo`
- `save_current_level`
- `level_get_actor_transform`
- `level_set_actor_transform`
- `level_set_actor_location`
- `level_set_actor_rotation`
- `level_set_actor_scale`
- `list_actors`
- `spawn_actor`
- `destroy_actor`
- `level_duplicate_actor`
- `actor_list_components`
- `level_list_actor_components`
- `level_get_actor_property`
- `level_get_component_property`
- `actor_set_property`
- `level_set_actor_property`
- `component_set_property`
- `level_set_component_property`
- `level_get_selection`
- `level_set_selection`
- `level_set_actor_folder`
- `level_add_actor_tag`
- `level_spawn_wall_with_opening`
- `level_mark_probe`
- `level_generate_probes`
- `viewport_get_camera`
- `viewport_set_camera`
- `viewport_set_realtime`
- `viewport_set_game_view`
- `viewport_focus_actor`
- `viewport_frame_selection`
- `viewport_frame_actors`
- `viewport_frame_actor`
- `viewport_frame_folder`
- `viewport_focus_actor_safe`
- `viewport_deproject_screen_to_world`
- `viewport_trace_screen_point`
- `navmesh_build`
- `navmesh_project_point`
- `navmesh_find_path`
- `navmesh_spawn_bounds_volume`
- `level_validate_connectivity`
- `level_trace_world_ray`
- `level_sweep_capsule`
- `level_sweep_capsule_path`
- `level_check_overlaps`
- `level_snap_to_surface`
- `screenshot_viewport`
- `screenshot_viewport_buffer`
- `mesh_get_closest_vertex`
- `mesh_get_vertex_world_position`
- `level_destroy_folder_actors`
- `level_cleanup_empty_folders`
- `level_align_actor_vertex_to_vertex`
- `level_align_actor_by_bounds`
- `level_align_face_to_face`
- `viewport_pick_actor_at_screen`
- `viewport_select_actor_at_screen`
- `level_get_nearby_actor_obbs`
- `editor_get_open_assets`
- `open_asset_editor`
- `save_asset`
- `asset_duplicate`
- `asset_import_fbx_skeletal_mesh`
- `asset_import_fbx_animation`
- `asset_export_property_json`
- `asset_apply_property_json`
- `editor_list_dirty_resources`
- `editor_resolve_dirty_resources`
- `editor_close`
- `editor_prepare_exit`
- `static_mesh_get_bounds`
- `static_mesh_get_local_corners`
- `static_mesh_set_material_slot`
- `static_mesh_set_collision_boxes`
- `static_mesh_set_collision_spheres`
- `static_mesh_set_collision_capsules`
- `static_mesh_add_socket`
- `static_mesh_update_socket`
- `static_mesh_remove_socket`
- `modeling_activate_mode`
- `modeling_get_selection`
- `modeling_set_selection`
- `modeling_set_mesh_selection_mode`
- `modeling_get_mesh_selection_info`
- `modeling_clear_mesh_selection`
- `modeling_select_mesh_elements_via_screen`
- `modeling_select_mesh_elements_via_world_ray`
- `modeling_start_tool`
- `modeling_get_active_tool`
- `modeling_get_active_tool_properties`
- `modeling_set_active_tool_property`
- `modeling_invoke_active_tool_action`
- `modeling_accept_tool`
- `modeling_cancel_tool`
- `modeling_save_mesh_asset`
- `modeling_replace_actor_mesh`
- `modeling_snap_to_ground`
- `modeling_convert_actor_to_dynamic_mesh`
- `modeling_duplicate_to_new_static_mesh`
- `modeling_create_box`
- `modeling_create_cylinder`
- `modeling_create_sphere`
- `modeling_create_plane`
- `modeling_create_stairs`
- `modeling_create_ramp`
- `modeling_create_ramp_corner`
- `modeling_extrude_faces`
- `modeling_inset_faces`
- `modeling_bevel_edges`
- `modeling_offset`
- `modeling_push_pull`
- `modeling_mirror`
- `modeling_duplicate_faces`
- `modeling_boolean`
- `modeling_trim`
- `modeling_plane_cut`
- `modeling_mesh_cut`
- `modeling_voxel_boolean`
- `modeling_remesh`
- `modeling_simplify`
- `modeling_subdivide`
- `modeling_weld_edges`
- `modeling_fill_holes`
- `modeling_recompute_normals`
- `modeling_set_pivot`
- `modeling_bake_transform`
- `modeling_align_to_world`
- `modeling_auto_uv`
- `modeling_project_uv`
- `modeling_set_material_slot`
- `modeling_add_material_slot`
- `modeling_remove_material_slot`
- `modeling_generate_simple_collision`
- `modeling_generate_convex_collision`
- `material_set_parameter`
- `anim_blueprint_create`
- `anim_blueprint_create_layer_interface`
- `anim_blueprint_open_editor`
- `anim_blueprint_compile`
- `anim_blueprint_get_compile_log`
- `anim_blueprint_export_folder`
- `anim_blueprint_apply_folder`
- `anim_blueprint_get_info`
- `anim_blueprint_list_graphs`
- `anim_blueprint_list_state_machines`
- `anim_blueprint_list_anim_layers`
- `anim_blueprint_list_layer_interfaces`
- `anim_blueprint_implement_layer_interface`
- `anim_blueprint_remove_layer_interface`
- `anim_blueprint_add_anim_layer`
- `anim_blueprint_rename_anim_layer`
- `anim_blueprint_remove_anim_layer`
- `anim_blueprint_add_state_machine`
- `anim_blueprint_rename_state_machine`
- `anim_blueprint_remove_state_machine`
- `anim_blueprint_set_entry_state`
- `anim_blueprint_clear_entry_state`
- `anim_blueprint_add_state`
- `anim_blueprint_set_state_properties`
- `anim_blueprint_add_conduit`
- `anim_blueprint_add_state_alias`
- `anim_blueprint_rename_state_node`
- `anim_blueprint_remove_state_node`
- `anim_blueprint_set_state_alias_targets`
- `anim_blueprint_add_transition`
- `anim_blueprint_set_transition_properties`
- `anim_blueprint_remove_transition`
- `anim_blueprint_inspect_nodes`
- `anim_blueprint_add_event_node`
- `anim_blueprint_add_custom_event_node`
- `anim_blueprint_add_node_by_class`
- `anim_blueprint_add_call_function_node`
- `anim_blueprint_add_variable_node`
- `anim_blueprint_connect_pins`
- `anim_blueprint_disconnect_pins`
- `anim_blueprint_set_pin_default_value`
- `anim_blueprint_remove_node`
- `anim_blueprint_add_variable`
- `anim_blueprint_remove_variable`
- `anim_blueprint_add_function_graph`
- `anim_blueprint_add_macro_graph`
- `anim_blueprint_add_event_dispatcher`
- `anim_blueprint_graph_get_view`
- `anim_blueprint_graph_set_view`
- `anim_blueprint_viewport_get_camera`
- `anim_blueprint_viewport_set_camera`
- `anim_blueprint_screenshot`
- `anim_blueprint_set_preview_mesh`
- `anim_blueprint_set_preview_animation_blueprint`
- `anim_blueprint_reparent`
- `anim_blueprint_set_cdo_property`
- `anim_sequence_get_info`
- `anim_sequence_screenshot`
- `anim_sequence_set_settings`
- `anim_sequence_set_preview_mesh`
- `anim_sequence_set_curve`
- `anim_sequence_set_bones`
- `anim_sequence_set_metadata`
- `anim_sequence_set_notify`
- `anim_sequence_set_notify_track`
- `anim_sequence_set_sync_markers`
- `skeleton_get_info`
- `skeleton_list_bones`
- `skeleton_set_compatible_skeletons`
- `skeleton_set_preview_mesh`
- `skeleton_set_socket`
- `skeleton_set_virtual_bone`
- `ik_rig_create`
- `ik_rig_get_info`
- `ik_rig_set_preview_mesh`
- `ik_rig_set_goal`
- `ik_rig_set_retarget_root`
- `ik_rig_set_retarget_chain`
- `ik_rig_apply_auto_retarget_definition`
- `ik_retargeter_create`
- `ik_retargeter_get_info`
- `ik_retargeter_set_ik_rig`
- `ik_retargeter_set_settings`
- `ik_retargeter_set_pose`
- `ik_retargeter_set_preview_mesh`
- `ik_retargeter_auto_map_chains`
- `ik_retargeter_duplicate_and_retarget`
- `montage_list_montages`
- `montage_create`
- `montage_open_editor`
- `montage_get_info`
- `montage_set_preview_mesh`
- `montage_set_blend_options`
- `montage_set_sync_group`
- `montage_add_slot_track`
- `montage_rename_slot_track`
- `montage_remove_slot_track`
- `montage_add_segment`
- `montage_update_segment`
- `montage_remove_segment`
- `montage_add_section`
- `montage_rename_section`
- `montage_set_section_time`
- `montage_remove_section`
- `montage_set_next_section`
- `montage_add_notify_track`
- `montage_remove_notify_track`
- `montage_add_notify`
- `montage_add_notify_state`
- `montage_update_notify`
- `montage_remove_notify`
- `montage_list_skeleton_slots`
- `montage_set_skeleton_slot_group`
- `montage_rename_skeleton_slot`
- `montage_remove_skeleton_slot`

## 2026-04-24 增量补齐的 Niagara 诊断命令

- `niagara_get_stack_issues`
  - 分册：`07_Niagara_System.md`、`08_Niagara_Emitter.md`
  - 用途：读取 Niagara Stack 面板里的红色错误、黄色警告和信息提示；返回 `stack_issue_icon_kind`、`stack_issue_tooltip_summary` 和 emitter 编译/状态提示字段。对 UI 红色感叹号，优先使用 `prefer_existing_view_model=true` 与 `open_editor_if_needed=true`
- `niagara_refresh_system`
  - 分册：`07_Niagara_System.md`
  - 用途：在 UAI 写入 Niagara System / Emitter / Stage / Module 后刷新 overview、emitter execution order、cached traversal data 和 Niagara ViewModel；用于处理“手动新增/删除 emitter 后 System 才恢复运行”的缓存/执行图过期问题。`niagara_compile_system` 对 System 默认会先执行同类刷新。
- `niagara_system_runtime_probe`
  - 分册：`07_Niagara_System.md`
  - 用途：默认推进 Niagara 编辑器预览组件并返回 System / Emitter 执行状态、粒子数和生成数；`sample_mode=current_preview` 可只读当前暂停预览状态；不启动 PIE / game 窗口。
- `niagara_preview_advance`
  - 分册：`07_Niagara_System.md`
  - 用途：从 0 或当前预览状态连续 tick 到目标帧并暂停，返回 `preview_state_token`；后续 `niagara_system_runtime_probe(sample_mode=current_preview)` 与 `niagara_screenshot(capture_mode=current_preview)` 使用该 token 只读同一状态。

## 2026-04-21 增量补齐的索引命令

以下命令已经在实现与分册中存在，但此前未同步到本索引：

### Blueprint

- `blueprint_create`
- `blueprint_compile`
- `blueprint_get_compile_log`
- `blueprint_get_info`
- `blueprint_list_graphs`
- `blueprint_export_folder`
- `blueprint_apply_folder`
- `blueprint_inspect_components`
- `blueprint_inspect_nodes`
- `blueprint_add_component`
- `blueprint_set_component_property`
- `blueprint_add_event_node`
- `blueprint_add_custom_event_node`
- `blueprint_add_node_by_class`
- `blueprint_add_call_function_node`
- `blueprint_connect_pins`
- `blueprint_disconnect_pins`
- `blueprint_set_pin_default_value`
- `blueprint_remove_node`
- `blueprint_add_variable`
- `blueprint_remove_variable`
- `blueprint_add_variable_node`
- `blueprint_add_component_bound_event`
- `blueprint_add_function_graph`
- `blueprint_add_macro_graph`
- `blueprint_add_event_dispatcher`
- `blueprint_graph_get_view`
- `blueprint_graph_set_view`
- `blueprint_viewport_get_camera`
- `blueprint_viewport_set_camera`
- `blueprint_screenshot`
- `blueprint_reparent`
- `blueprint_remove_component`
- `blueprint_set_cdo_property`

### UMG

- `umg_create_widget_blueprint`
- `umg_compile`
- `umg_get_compile_log`
- `umg_get_info`
- `umg_export_folder`
- `umg_apply_folder`
- `umg_add_widget`
- `umg_remove_widget`
- `umg_set_widget_property`
- `umg_set_slot_property`
- `umg_rename_widget`
- `umg_bind_widget_property_to_variable`

### StaticMesh / EnhancedInput

- `static_mesh_open_editor`
- `static_mesh_get_info`
- `static_mesh_set_preview_view`
- `static_mesh_set_property`
- `enhanced_input_create_action`
- `enhanced_input_set_action_property`
- `enhanced_input_get_action_info`
- `enhanced_input_export_action_json`
- `enhanced_input_apply_action_json`
- `enhanced_input_create_mapping_context`
- `enhanced_input_set_mapping_context_property`
- `enhanced_input_get_mapping_context_info`
- `enhanced_input_add_mapping`
- `enhanced_input_remove_mapping`
- `enhanced_input_clear_mappings`
- `enhanced_input_export_mapping_context_json`
- `enhanced_input_apply_mapping_context_json`

### Material

- `material_create`
- `material_open_editor`
- `material_get_info`
- `material_set_property`
- `material_compile`
- `material_get_compile_log`
- `material_export_folder`
- `material_apply_folder`
- `material_instance_export_folder`
- `material_instance_apply_folder`
- `material_function_export_folder`
- `material_function_apply_folder`
- `material_add_expression`
- `material_delete_expression`
- `material_list_expressions`
- `material_connect_expressions`
- `material_connect_expression_to_property`
- `material_set_expression_property`
- `material_instance_create`
- `material_set_instance_parent`
- `material_set_scalar_parameter`
- `material_set_vector_parameter`
- `material_set_texture_parameter`
- `material_set_static_switch_parameter`

### Sequence

- `sequence_list_level_sequences`
- `sequence_create_level_sequence`
- `sequence_open_level_sequence`
- `sequence_get_level_sequence_info`
- `sequence_set_level_sequence_playback_range`
- `sequence_set_level_sequence_display_rate`
- `sequence_add_actor_binding`
- `sequence_remove_actor_binding`
- `sequence_add_property_track`
- `sequence_add_property_key`
- `sequence_add_visibility_track`
- `sequence_add_visibility_key`
- `sequence_add_bool_property_track`
- `sequence_add_bool_property_key`
- `sequence_add_float_property_track`
- `sequence_add_float_property_key`
- `sequence_add_integer_property_track`
- `sequence_add_integer_property_key`
- `sequence_add_transform_track`
- `sequence_add_transform_key`
- `sequence_add_skeletal_animation_track`
- `sequence_add_skeletal_animation_section`
- `sequence_update_skeletal_animation_section`
- `sequence_remove_skeletal_animation_section`
- `sequence_list_umg_animations`
- `sequence_get_umg_animation_info`
- `sequence_create_umg_animation`
- `sequence_rename_umg_animation`
- `sequence_remove_umg_animation`
- `sequence_set_umg_animation_playback_range`
- `sequence_set_umg_animation_display_rate`
- `sequence_add_umg_widget_transform_key`
- `sequence_add_umg_widget_translation_key`
- `sequence_add_umg_widget_opacity_key`
- `sequence_add_umg_widget_color_key`

### IK Rig

- `ik_rig_set_solver`

## 当前白盒迭代建议

- 白盒重建前，优先按 `folder_path` 调用 `level_destroy_folder_actors` 清空上一轮生成物，而不是依赖脚本侧保留的 actor 列表逐个删除。
- 批量创建 `modeling_create_box / cylinder / sphere / plane / stairs` 时，优先在创建参数里直接提供 `folder_path`，减少额外 `level_set_actor_folder` 往返。
- 大量白盒构建优先使用 `batch`，避免几十次到上百次独立 `exec` 带来的进程与 HTTP 往返开销。

## 推荐查阅顺序

- 想查关卡、视口、对齐、事务：看 `01_Core_Level_Assets_Landscape.md`
- 想查 Blueprint / AnimBlueprint / UMG / Montage：看 `02_Blueprint.md`、`11_AnimBlueprint.md`、`03_UMG.md`、`12_Montage.md`
- 想查 StaticMesh / EnhancedInput：看 `04_StaticMesh_EnhancedInput.md`
- 想查材质、Sequencer、Niagara：看对应编号分册
- 想查 Modeling Mode：看 `10_Modeling.md`

