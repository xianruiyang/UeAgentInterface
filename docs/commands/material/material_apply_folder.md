---
command: material_apply_folder
category: material
title: 资产与编译
summary: 从固定文件夹式 JSON 结构回写 `UMaterial`
aliases: []
tags: [material]
status: active
---

# `material_apply_folder`

## LLM Compact

- status: active.
- purpose: 从固定文件夹式 JSON 结构回写 `UMaterial`
- key_inputs: `asset_path`, `compile_after_apply`, `create_if_missing`, `save_after_apply`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- behavior: 写入命令；必须明确目标范围、默认值和写后读回验收。
- note: JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- note: 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- validation: apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Authoring Notes
- JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。

<!-- P4-001 structured-json-authoring: begin -->
## Structured JSON Authoring

- contract_type: `material`
- operation: `apply`
- implementation_status: `audit_only`
- operation_registration: `confirmed`
- command_vs_folder_json: 命令参数只负责定位资产、文件夹和执行选项；可编辑内容写入 folder JSON，不把 `asset_path`、`folder_path`、`include_raw_properties`、`dry_run` 等命令参数当作 authoring 字段。
- default_profile: `llm_authoring`；默认只暴露 public seed、container context、editable authoring 和 required readonly。
- operations: export=`material_export_folder`, apply=`material_apply_folder`, validate=`missing`, diff=`missing`
- included_files: `manifest.json`, `asset.json`, `index.json`, `authoring/material.json`, `authoring/graphs/items/main/graph.json`, `authoring/graphs/items/main/layout.json`, `references/index.json`, `authoring/parameters/index.json`, `...(+1)`
- excluded_or_explicit_files: `validation/issues.json`, `debug/raw_properties.json`, `legacy/`
- included_field_roles: editable_authoring=5, public_seed_identity=1, required_readonly=1
- hidden_field_roles: legacy_compat=1, raw_heavy=1, readback_report=1
- editable_fields_sample: `asset.json#/path`, `authoring/material.json#/`, `authoring/parameters/interface.json#/parameters`, `authoring/graphs/items/main/graph.json#/nodes`, `authoring/graphs/items/main/graph.json#/edges`, `authoring/graphs/items/main/layout.json#/nodes`
- required_readonly_sample: `asset.json#/format_version`
- index_rule: `index.json` 只做导航摘要，不保存完整对象；大型集合按 `items/<id>/...` 拆分，短小内容保持合并。
- graph_rule: `graphs/items/<graph_id>/graph.json` 保存逻辑整体；`layout.json` 只保存编辑器视觉布局。旧平铺 `graphs/<name>.json` 只作 legacy 兼容。
- field_roles: `public_seed_identity` 使用公开 `path`/`name` seed；`required_readonly` 只用于识别、校验或差异判断，不由 apply 写入；`legacy_compat`、`debug_diagnostics`、`readback_report`、`raw_heavy`、`command_param_only` 默认隐藏。
- raw_debug_policy: `include_raw_properties`、raw/readback/debug 输出仅为显式诊断选项，默认 `llm_authoring` 不依赖这些字段。
- legacy_input: accepts=`false`；旧 properties 输入只允许出现在显式迁移、负例、错误或 debug 语境，不作为 folder authoring 输入。
- legacy_locations: `asset.json#/asset_path`, `graphs/MaterialGraph.json`, `settings/material.json`, `parameters/interface.json`
- legacy_fields_not_recommended: `asset_path`, `object_path`, `class_path`
- legacy_seed_policy: 已知路径实体写 `path`；旧 `asset_path`/`class_path`/`object_path`/`component_class_path`/`legacy_path` 仅用于 legacy、debug 或 readback 兼容。
- validate_flow: 当前 contract 未注册专用 `*_validate_folder`；不要声称 validate 覆盖完整，需用 export/readback/consistency gate 或命令返回字段交叉验证。
- diff_flow: 当前 contract 未注册专用 `*_diff_folder`；不要声称 diff 覆盖完整，必要时用 readback 后结构化比较。
<!-- P4-001 structured-json-authoring: end -->

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `compile_after_apply`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `create_if_missing`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `save_after_apply`：布尔/开关字段；默认值以本命令契约为准。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。
