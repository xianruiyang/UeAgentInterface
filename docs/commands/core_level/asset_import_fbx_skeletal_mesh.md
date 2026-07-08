---
command: asset_import_fbx_skeletal_mesh
category: core_level
title: `asset_import_fbx_skeletal_mesh`
summary: 导入 FBX 为 Skeletal Mesh
aliases: []
tags: [core_level]
status: active
---

# `asset_import_fbx_skeletal_mesh`

## LLM Compact

- status: active.
- purpose: 导入 FBX 为 Skeletal Mesh
- key_inputs: `destination_path`, `skeleton_path/import_materials/import_textures/create_physics_asset/import_animations/import_morph_targets/update_skeleton_reference_pose/validate_after_import/expected_*`, `source_filename`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: Morph Target 导入验收不要只看 `ok=true`。必须检查 `import_morph_targets=true`、`morph_target_count>0`、`morph_targets_imported[]` 包含预期名称，再调用 `skeletal_mesh_get_morph_targets`、`skeletal_mesh_validate_morph_targets` 和 `skeletal_mesh_preview_morph_target` 做读回与 transient component 设置验证。
- note: 通过 FBX 工厂自动导入 Skeletal Mesh。
- note: 返回 `imported_object_paths`，并按类型拆出 `skeletal_mesh_asset_paths / skeleton_asset_paths / physics_asset_paths`；`primary_asset_path` 是本次导入的代表资产。
- note: 返回角色变形相关字段：`skeletal_mesh_asset_path`、`morph_targets_imported[]`、`skin_weight_profiles_imported[]`、`lod_count`、`material_slot_count`、`section_count`、`morph_target_count`、`skin_weight_profile_count`、`validation_issues[]`、`validation_error_count`、`validation_passed`、`validation_report`。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- Morph Target 导入验收不要只看 `ok=true`。必须检查 `import_morph_targets=true`、`morph_target_count>0`、`morph_targets_imported[]` 包含预期名称，再调用 `skeletal_mesh_get_morph_targets`、`skeletal_mesh_validate_morph_targets` 和 `skeletal_mesh_preview_morph_target` 做读回与 transient component 设置验证。
- 通过 FBX 工厂自动导入 Skeletal Mesh。
- 返回 `imported_object_paths`，并按类型拆出 `skeletal_mesh_asset_paths / skeleton_asset_paths / physics_asset_paths`；`primary_asset_path` 是本次导入的代表资产。
- 返回角色变形相关字段：`skeletal_mesh_asset_path`、`morph_targets_imported[]`、`skin_weight_profiles_imported[]`、`lod_count`、`material_slot_count`、`section_count`、`morph_target_count`、`skin_weight_profile_count`、`validation_issues[]`、`validation_error_count`、`validation_passed`、`validation_report`。
- 如果 `validate_after_import=true` 且 `expected_morph_targets[]` 中任何名称未导入，命令返回失败，错误为 `fbx_skeletal_mesh_import_validation_failed`，不会把“导入了 mesh 但没有 Morph Target”伪装成成功。
- 如果源文件不是 `.fbx`，返回 `source_file_is_not_fbx`；文件不存在返回 `source_file_not_found`；`destination_path` 不是合法 `/Game/...` 包路径时返回 `invalid_destination_path`。
- 适合先导入角色网格，再把动作批量导入到同一 Skeleton。

## Parameters

- `destination_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。
- `skeleton_path/import_materials/import_textures/create_physics_asset/import_animations/import_morph_targets/update_skeleton_reference_pose/validate_after_import/expected_*`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `source_filename`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
