---
command: pcg_graph_export_folder
category: pcg
title: 指令列表
summary: 导出 `UPCGGraph` 文件夹式 JSON
aliases: []
tags: [pcg]
status: active
---

# `pcg_graph_export_folder`

## LLM Compact

- status: active.
- purpose: 导出 `UPCGGraph` 文件夹式 JSON
- key_inputs: `asset_path`, `folder_path`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- note: PCG node settings Details 默认外置为 `settings_details`，具体字段在相邻 `details/<owner>/index.json` 与 group JSON；`graph.json` 只保留业务结构和引用。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Authoring Notes
- 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- 默认导出把节点 settings Details 外置为 `settings_details`；编辑时按 `settings_details.path` 打开分组文件，不在 `graph.json` 中展开全部属性。

<!-- P4-001 structured-json-authoring: begin -->
## Structured JSON Authoring

- contract_type: `pcg_graph`
- operation: `export`
- implementation_status: `confirmed`
- operation_registration: `confirmed`
- command_vs_folder_json: 命令参数只负责定位资产、文件夹和执行选项；可编辑内容写入 folder JSON，不把 `asset_path`、`folder_path`、`include_raw_properties`、`dry_run` 等命令参数当作 authoring 字段。
- default_profile: `llm_authoring`；默认只暴露 public seed、container context、editable authoring 和 required readonly。
- operations: export=`pcg_graph_export_folder`, apply=`pcg_graph_apply_folder`, validate=`pcg_graph_validate_folder`, diff=`pcg_graph_diff_folder`
- included_files: `manifest.json`, `asset.json`, `index.json`, `authoring/pcg_graph.json`, `authoring/graphs/items/main/graph.json`, `authoring/graphs/items/main/layout.json`, `references/index.json`, `authoring/graphs/items/main/regions/`, `authoring/graphs/items/main/details/`
- excluded_or_explicit_files: `validation/issues.json`, `debug/raw_properties.json`, `legacy/`
- included_field_roles: editable_authoring=5, public_seed_identity=1, required_readonly=1
- hidden_field_roles: legacy_compat=1, raw_heavy=1, readback_report=1
- editable_fields_sample: `asset.json#/path`, `authoring/pcg_graph.json#/graph_settings`, `authoring/pcg_graph.json#/parameters`, `authoring/graphs/items/main/graph.json#/nodes`, `authoring/graphs/items/main/graph.json#/edges`, `authoring/graphs/items/main/graph.json#/nodes[]/settings_details`, `authoring/graphs/items/main/layout.json#/nodes`
- required_readonly_sample: `asset.json#/format_version`
- index_rule: `index.json` 只做导航摘要，不保存完整对象；大型集合按 `items/<id>/...` 拆分，短小内容保持合并。
- graph_rule: `graphs/items/<graph_id>/graph.json` 保存逻辑整体；`layout.json` 只保存编辑器视觉布局。旧平铺 `graphs/<name>.json` 只作 legacy 兼容。
- details_rule: 节点 settings 的 UE Details 字段默认通过 `settings_details` 引用到 `details/<owner>/index.json` 和 group JSON；legacy inline `settings_properties[]` 不作为合法 authoring 输入。
- field_roles: `public_seed_identity` 使用公开 `path`/`name` seed；`required_readonly` 只用于识别、校验或差异判断，不由 apply 写入；`legacy_compat`、`debug_diagnostics`、`readback_report`、`raw_heavy`、`command_param_only` 默认隐藏。
- raw_debug_policy: `include_raw_properties`、raw/readback/debug 输出仅为显式诊断选项，默认 `llm_authoring` 不依赖这些字段。
- legacy_input: accepts=`false`；旧 properties 输入只允许出现在显式迁移、负例、错误或 debug 语境，不作为 folder authoring 输入。
- legacy_locations: `asset.json#/asset_path`, `graph.json`, `layout.json`
- legacy_fields_not_recommended: `asset_path`, `object_path`, `class_path`
- legacy_seed_policy: 已知路径实体写 `path`；旧 `asset_path`/`class_path`/`object_path`/`component_class_path`/`legacy_path` 仅用于 legacy、debug 或 readback 兼容。
- validate_flow: 先调用 `pcg_graph_validate_folder` 做只读结构校验，再 apply；validate 结果不是写入结果。
- diff_flow: 可调用 `pcg_graph_diff_folder` 比较两份 folder package。
<!-- P4-001 structured-json-authoring: end -->

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `folder_path`：Outliner folder 或资产 folder；具体含义以命令所在领域为准。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
