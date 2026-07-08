---
command: sequence_apply_folder
category: sequence
title: `sequence_apply_folder` 的 `control_rig` 轨道
summary: 从文件夹式 JSON 子集回写
aliases: []
tags: [sequence]
status: active
---

# `sequence_apply_folder`

## LLM Compact

- status: active.
- purpose: 从文件夹式 JSON 子集回写
- key_inputs: `asset_path`, `strict`, `auto_force_skeletal_custom_mode_for_control_rig`, `create_if_missing`, `fail_on_partial_roundtrip`, `fail_on_structural_diff`, `fail_on_unsupported_tracks`, `fail_on_warnings` ...
- key_returns: `Saved`, `UeAssetFolders`, `LevelSequence`
- behavior: 写入命令；必须明确目标范围、默认值和写后读回验收。
- note: JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- note: 每个 key 需要 `control` 或 `control_name`，并需要 `time_seconds` 或 `frame`。`frame` 使用序列 display rate。
- note: 支持的 control 类型：`float/scale_float`、`bool`、`integer/enum`、`vector2d`、`position`、`scale`、`rotator`、`transform/euler_transform/transform_no_scale`。
- note: 当前 apply 会把多个 `sections[]` 的 key 合并到一个 Control Rig section，并返回 warning；需要多 section 精确语义时应先小步验证。
- validation: apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Authoring Notes
- JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- 每个 key 需要 `control` 或 `control_name`，并需要 `time_seconds` 或 `frame`。`frame` 使用序列 display rate。
- 支持的 control 类型：`float/scale_float`、`bool`、`integer/enum`、`vector2d`、`position`、`scale`、`rotator`、`transform/euler_transform/transform_no_scale`。
- 当前 apply 会把多个 `sections[]` 的 key 合并到一个 Control Rig section，并返回 warning；需要多 section 精确语义时应先小步验证。

<!-- P4-001 structured-json-authoring: begin -->
## Structured JSON Authoring

- contract_type: `sequence`
- operation: `apply`
- implementation_status: `audit_only`
- operation_registration: `confirmed`
- command_vs_folder_json: 命令参数只负责定位资产、文件夹和执行选项；可编辑内容写入 folder JSON，不把 `asset_path`、`folder_path`、`include_raw_properties`、`dry_run` 等命令参数当作 authoring 字段。
- default_profile: `llm_authoring`；默认只暴露 public seed、container context、editable authoring 和 required readonly。
- operations: export=`sequence_export_folder`, apply=`sequence_apply_folder`, validate=`missing`, diff=`missing`
- included_files: `manifest.json`, `asset.json`, `index.json`, `authoring/sequence.json`, `authoring/folders.json`, `authoring/bindings/index.json`, `authoring/bindings/items/<binding_id>/binding.json`, `references/index.json`, `...(+2)`
- excluded_or_explicit_files: `validation/issues.json`, `debug/raw_properties.json`, `legacy/`
- included_field_roles: container_context=1, editable_authoring=3, public_seed_identity=1, required_readonly=1
- hidden_field_roles: legacy_compat=1, raw_heavy=1, readback_report=1
- editable_fields_sample: `asset.json#/path`, `authoring/sequence.json#/`, `authoring/folders.json#/folders`, `authoring/bindings/items/<binding_id>/binding.json#/`
- required_readonly_sample: `asset.json#/format_version`, `authoring/bindings/index.json#/bindings`
- index_rule: `index.json` 只做导航摘要，不保存完整对象；大型集合按 `items/<id>/...` 拆分，短小内容保持合并。
- field_roles: `public_seed_identity` 使用公开 `path`/`name` seed；`required_readonly` 只用于识别、校验或差异判断，不由 apply 写入；`legacy_compat`、`debug_diagnostics`、`readback_report`、`raw_heavy`、`command_param_only` 默认隐藏。
- raw_debug_policy: `include_raw_properties`、raw/readback/debug 输出仅为显式诊断选项，默认 `llm_authoring` 不依赖这些字段。
- legacy_input: accepts=`false`；旧 properties 输入只允许出现在显式迁移、负例、错误或 debug 语境，不作为 folder authoring 输入。
- legacy_locations: `asset.json#/asset_path`, `settings.json`, `bindings/`
- legacy_fields_not_recommended: `asset_path`, `object_path`, `class_path`
- legacy_seed_policy: 已知路径实体写 `path`；旧 `asset_path`/`class_path`/`object_path`/`component_class_path`/`legacy_path` 仅用于 legacy、debug 或 readback 兼容。
- validate_flow: 当前 contract 未注册专用 `*_validate_folder`；不要声称 validate 覆盖完整，需用 export/readback/consistency gate 或命令返回字段交叉验证。
- diff_flow: 当前 contract 未注册专用 `*_diff_folder`；不要声称 diff 覆盖完整，必要时用 readback 后结构化比较。
<!-- P4-001 structured-json-authoring: end -->

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `strict`：严格校验模式。
- `auto_force_skeletal_custom_mode_for_control_rig`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `create_if_missing`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `fail_on_partial_roundtrip`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `fail_on_structural_diff`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `fail_on_unsupported_tracks`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `fail_on_warnings`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `replace_mode`：布尔/开关字段；默认值以本命令契约为准。
- `save_after_apply`：布尔/开关字段；默认值以本命令契约为准。
- `structural_diff_max_entries`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `verify_after_apply`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `Saved`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `UeAssetFolders`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `LevelSequence`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。
