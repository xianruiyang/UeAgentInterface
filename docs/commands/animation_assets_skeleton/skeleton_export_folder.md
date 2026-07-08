---
command: skeleton_export_folder
category: animation_assets_skeleton
title: Skeleton
summary: 导出 Skeleton 文件夹式结构化 JSON
aliases: []
tags: [animation_assets_skeleton]
status: active
---

# `skeleton_export_folder`

## LLM Compact

- status: active.
- purpose: 导出 Skeleton 文件夹式结构化 JSON
- key_inputs: `asset_path`, `folder_path`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Authoring Notes
- 按本命令参数、返回字段和写后读回规则执行。

<!-- P4-001 structured-json-authoring: begin -->
## Structured JSON Authoring

- contract_type: `skeleton`
- operation: `export`
- implementation_status: `confirmed`
- operation_registration: `confirmed`
- command_vs_folder_json: 命令参数只负责定位资产、文件夹和执行选项；可编辑内容写入 folder JSON，不把 `asset_path`、`folder_path`、`include_raw_properties`、`dry_run` 等命令参数当作 authoring 字段。
- default_profile: `llm_authoring`；默认只暴露 public seed、container context、editable authoring 和 required readonly。
- operations: export=`skeleton_export_folder`, apply=`skeleton_apply_folder`, validate=`skeleton_validate_folder`, diff=`missing`
- included_files: `manifest.json`, `asset.json`, `index.json`, `authoring/skeleton.json`, `references/index.json`, `authoring/bones/index.json`, `authoring/sockets/index.json`
- excluded_or_explicit_files: `validation/issues.json`, `debug/raw_reference_pose.json`, `legacy/`
- included_field_roles: container_context=1, editable_authoring=2, public_seed_identity=1, required_readonly=1
- hidden_field_roles: legacy_compat=1, raw_heavy=1, readback_report=1
- editable_fields_sample: `asset.json#/path`, `authoring/skeleton.json#/`, `authoring/sockets/index.json#/sockets`
- required_readonly_sample: `asset.json#/format_version`, `authoring/bones/index.json#/bones`
- index_rule: `index.json` 只做导航摘要，不保存完整对象；大型集合按 `items/<id>/...` 拆分，短小内容保持合并。
- field_roles: `public_seed_identity` 使用公开 `path`/`name` seed；`required_readonly` 只用于识别、校验或差异判断，不由 apply 写入；`legacy_compat`、`debug_diagnostics`、`readback_report`、`raw_heavy`、`command_param_only` 默认隐藏。
- raw_debug_policy: `include_raw_properties`、raw/readback/debug 输出仅为显式诊断选项，默认 `llm_authoring` 不依赖这些字段。
- legacy_input: accepts=`false`；旧 properties 输入只允许出现在显式迁移、负例、错误或 debug 语境，不作为 folder authoring 输入。
- legacy_locations: `asset.json#/asset_path`, `skeleton.json`, `bones.json`, `sockets.json`
- legacy_fields_not_recommended: `asset_path`, `object_path`, `class_path`
- legacy_seed_policy: 已知路径实体写 `path`；旧 `asset_path`/`class_path`/`object_path`/`component_class_path`/`legacy_path` 仅用于 legacy、debug 或 readback 兼容。
- validate_flow: 先调用 `skeleton_validate_folder` 做只读结构校验，再 apply；validate 结果不是写入结果。
- diff_flow: 当前 contract 未注册专用 `*_diff_folder`；不要声称 diff 覆盖完整，必要时用 readback 后结构化比较。
<!-- P4-001 structured-json-authoring: end -->

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `folder_path`：Outliner folder 或资产 folder；具体含义以命令所在领域为准。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
