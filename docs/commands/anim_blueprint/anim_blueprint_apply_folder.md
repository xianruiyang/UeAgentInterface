---
command: anim_blueprint_apply_folder
category: anim_blueprint
title: `anim_blueprint_apply_folder`
summary: 从结构化文件夹回写
aliases: []
tags: [anim_blueprint]
status: active
---

# `anim_blueprint_apply_folder`

## LLM Compact

- status: active.
- purpose: 从结构化文件夹回写
- key_inputs: `asset_path`, `compile_after_apply`, `create_if_missing`, `save_after_apply`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- behavior: 写入命令；必须明确目标范围、默认值和写后读回验收。
- note: JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- note: 结构修改默认优先走这条文件夹式回写，而不是逐条节点命令手搓。
- note: 先写最小结构
- note: apply 生成真实资产
- validation: apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Authoring Notes
- JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- 结构修改默认优先走这条文件夹式回写，而不是逐条节点命令手搓。
- 先写最小结构
- apply 生成真实资产
- export 看真实图/结构 JSON
- 再补更多字段
- 编译 / 保存
- 默认 `compile_after_apply=true`。apply 会收集编译日志并返回 `compile_status`、`compile_error_count`、`compile_warning_count`、`compile_has_error`、`compile_messages[]`；如果有编译 error 或 Blueprint 状态为 `BS_Error`，命令失败返回 `compile_failed_after_apply`，并且不会执行最终 `save_after_apply`。
- 普通逻辑图和成员层当前复用 `blueprint_apply_folder` 代理回写。
- 对已存在资产，`target_skeleton / template` 目前只在创建时生效；若与现有资产不一致，apply 会给 warning，不会强改现有资产。

<!-- P4-001 structured-json-authoring: begin -->
## Structured JSON Authoring

- contract_type: `anim_blueprint`
- operation: `apply`
- implementation_status: `confirmed`
- operation_registration: `confirmed`
- command_vs_folder_json: 命令参数只负责定位资产、文件夹和执行选项；可编辑内容写入 folder JSON，不把 `asset_path`、`folder_path`、`include_raw_properties`、`dry_run` 等命令参数当作 authoring 字段。
- default_profile: `llm_authoring`；默认只暴露 public seed、container context、editable authoring 和 required readonly。
- operations: export=`anim_blueprint_export_folder`, apply=`anim_blueprint_apply_folder`, validate=`missing`, diff=`missing`
- included_files: `asset.json`, `settings/anim_blueprint.json`, `members/variables.json`, `members/delegates.json`, `members/defaults.json`, `layer_interfaces/interfaces.json`, `anim_layers/layers.json`, `state_machines/<state_machine>.json`, `...(+3)`
- excluded_or_explicit_files: `graphs/<graph_name>.json`, `validation/checks.json`
- included_field_roles: editable_authoring=7, public_seed_identity=1, required_readonly=1
- hidden_field_roles: legacy_compat=2
- editable_fields_sample: `asset.json#/path`, `settings/anim_blueprint.json#/`, `layer_interfaces/interfaces.json#/interfaces`, `anim_layers/layers.json#/layers`, `state_machines/<state_machine>.json#/`, `graphs/items/<graph_id>/graph.json#/nodes`, `graphs/items/<graph_id>/graph.json#/edges`, `graphs/items/<graph_id>/layout.json#/nodes`
- required_readonly_sample: `asset.json#/format_version`
- index_rule: `index.json` 只做导航摘要，不保存完整对象；大型集合按 `items/<id>/...` 拆分，短小内容保持合并。
- graph_rule: `graphs/items/<graph_id>/graph.json` 保存逻辑整体；`layout.json` 只保存编辑器视觉布局。旧平铺 `graphs/<name>.json` 只作 legacy 兼容。
- field_roles: `public_seed_identity` 使用公开 `path`/`name` seed；`required_readonly` 只用于识别、校验或差异判断，不由 apply 写入；`legacy_compat`、`debug_diagnostics`、`readback_report`、`raw_heavy`、`command_param_only` 默认隐藏。
- raw_debug_policy: raw/readback/debug 字段默认不进入 `llm_authoring`；需要排查时改用 debug/readback/legacy profile 或命令显式选项。
- legacy_input: accepts=`false`；旧 properties 输入只允许出现在显式迁移、负例、错误或 debug 语境，不作为 folder authoring 输入。
- legacy_locations: `graphs/<graph_name>.json`, `asset.json#/asset_path`
- legacy_fields_not_recommended: `asset_path`, `graphs/<graph_name>.json`
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
