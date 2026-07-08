---
command: physics_asset_apply_folder
category: physics_baseline
title: 指令总览
summary: 创建或更新 PhysicsAsset 基础 bodies/constraints/profiles
aliases: []
tags: [physics_baseline]
status: active
---

# `physics_asset_apply_folder`

## LLM Compact

- status: active.
- purpose: 创建或更新 PhysicsAsset 基础 bodies/constraints/profiles
- key_inputs: `folder_path`, `allow_destructive_replace`, `backup`, `create_if_missing`, `save_after_apply`
- key_returns: `asset_created`, `body_results[]`, `constraint_results[]`, `profile_results[]`, `readback`
- behavior: 写入命令；必须明确目标范围、默认值和写后读回验收。
- note: JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- validation: apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Authoring Notes
- JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。

<!-- P4-001 structured-json-authoring: begin -->
## Structured JSON Authoring

- contract_type: `physics_asset`
- operation: `apply`
- implementation_status: `confirmed`
- operation_registration: `confirmed`
- command_vs_folder_json: 命令参数只负责定位资产、文件夹和执行选项；可编辑内容写入 folder JSON，不把 `asset_path`、`folder_path`、`include_raw_properties`、`dry_run` 等命令参数当作 authoring 字段。
- default_profile: `llm_authoring`；默认只暴露 public seed、container context、editable authoring 和 required readonly。
- operations: export=`physics_asset_export_folder`, apply=`physics_asset_apply_folder`, validate=`physics_asset_validate_folder`, diff=`missing`
- included_files: `manifest.json`, `asset.json`, `index.json`, `authoring/physics_asset.json`, `authoring/bodies/index.json`, `authoring/bodies/items/<body_id>/body.json`, `authoring/constraints.json`, `authoring/profiles.json`, `...(+3)`
- excluded_or_explicit_files: `validation/issues.json`, `debug/raw_body_setup.json`, `legacy/`
- included_field_roles: container_context=1, editable_authoring=4, public_seed_identity=1, required_readonly=1
- hidden_field_roles: legacy_compat=1, raw_heavy=1, readback_report=1
- editable_fields_sample: `asset.json#/path`, `authoring/physics_asset.json#/`, `authoring/bodies/items/<body_id>/body.json#/shapes`, `authoring/constraints.json#/constraints`, `authoring/profiles.json#/profiles`
- required_readonly_sample: `asset.json#/format_version`, `authoring/bodies/index.json#/bodies`
- index_rule: `index.json` 只做导航摘要，不保存完整对象；大型集合按 `items/<id>/...` 拆分，短小内容保持合并。
- field_roles: `public_seed_identity` 使用公开 `path`/`name` seed；`required_readonly` 只用于识别、校验或差异判断，不由 apply 写入；`legacy_compat`、`debug_diagnostics`、`readback_report`、`raw_heavy`、`command_param_only` 默认隐藏。
- raw_debug_policy: `include_raw_properties`、raw/readback/debug 输出仅为显式诊断选项，默认 `llm_authoring` 不依赖这些字段。
- legacy_input: accepts=`false`；旧 properties 输入只允许出现在显式迁移、负例、错误或 debug 语境，不作为 folder authoring 输入。
- legacy_locations: `asset.json#/asset_path`, `bodies.json`, `constraints.json`, `profiles.json`
- legacy_fields_not_recommended: `asset_path`, `object_path`, `class_path`
- legacy_seed_policy: 已知路径实体写 `path`；旧 `asset_path`/`class_path`/`object_path`/`component_class_path`/`legacy_path` 仅用于 legacy、debug 或 readback 兼容。
- validate_flow: 先调用 `physics_asset_validate_folder` 做只读结构校验，再 apply；validate 结果不是写入结果。
- diff_flow: 当前 contract 未注册专用 `*_diff_folder`；不要声称 diff 覆盖完整，必要时用 readback 后结构化比较。
<!-- P4-001 structured-json-authoring: end -->

## Parameters

- `folder_path`：Outliner folder 或资产 folder；具体含义以命令所在领域为准。
- `allow_destructive_replace`：布尔/开关字段；默认值以本命令契约为准。
- `backup`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `create_if_missing`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `save_after_apply`：布尔/开关字段；默认值以本命令契约为准。

## Returns

- `asset_created`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `body_results[]`：body_results 列表摘要；大列表应写入 output/artifact 或 debug。
- `constraint_results[]`：constraint_results 列表摘要；大列表应写入 output/artifact 或 debug。
- `profile_results[]`：profile_results 列表摘要；大列表应写入 output/artifact 或 debug。
- `readback`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。
