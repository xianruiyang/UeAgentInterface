---
command: niagara_emitter_apply_folder
category: niagara_emitter
title: Emitter 属性
summary: 从文件夹式 JSON 回写 standalone Emitter
aliases: []
tags: [niagara_emitter]
status: active
---

# `niagara_emitter_apply_folder`

## LLM Compact

- status: active.
- purpose: 从文件夹式 JSON 回写 standalone Emitter
- key_inputs: `folder_path`, `strict`, `create_if_missing`, `emitter_asset_path`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- behavior: 写入命令；必须明确目标范围、默认值和写后读回验收。
- note: JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- note: 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- note: 当 `folder_path` 指向 `niagara_export_folder` 导出的 System 内嵌 emitter 子目录时，该子目录可能没有 `asset.json`；此时只要请求显式提供 `emitter_asset_path`，`niagara_emitter_apply_folder` 会把该子目录内容回写到指定 standalone Emitter。
- note: 若既没有 `asset.json`，也没有显式 `emitter_asset_path` / `asset_path`，命令会失败并返回 `missing_emitter_asset_path_and_asset_json`。
- validation: apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Authoring Notes
- JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- 当 `folder_path` 指向 `niagara_export_folder` 导出的 System 内嵌 emitter 子目录时，该子目录可能没有 `asset.json`；此时只要请求显式提供 `emitter_asset_path`，`niagara_emitter_apply_folder` 会把该子目录内容回写到指定 standalone Emitter。
- 若既没有 `asset.json`，也没有显式 `emitter_asset_path` / `asset_path`，命令会失败并返回 `missing_emitter_asset_path_and_asset_json`。
- 从 System 内嵌 emitter 子目录回写到 standalone Emitter 时，assignment 模块会被重建为目标资产自己的本地节点，并保留模块启用状态和 input default；这能避免 standalone Emitter 保存时引用 `/Game/...NS_System:EmbeddedEmitter...` 私有对象。
- 当来源文件夹包含完整 `stages/index.json` 时，apply 会把目标 Emitter 的动态 stage 集合同步到来源：来源不存在的 Event Handler / Simulation Stage 会被裁掉，防止旧 event stage、旧 module stack 或旧速度模块残留。
- 当来源文件夹包含 `event_generators.json` 时，apply 会先清空目标 Emitter 四类脚本上的 Event Generator 数组，再按 JSON 重建；空数组会清除旧 DeathEvent/CollisionEvent generator，避免同一数据集被旧 generator 和新 module 重复写入。
- 对 UE 内置 `CollisionEvent` / `DeathEvent` generator，apply 只会在同一 owner stage 里存在对应 `GenerateCollisionEvent` / `GenerateDeathEvent` module 时重建；若导出文件夹残留 orphan generator，会返回 `event_generator_skipped_missing_stage_module` warning，提示先修正 stage/module 结构。
- 返回值包含 `stack_issue_report`、`stack_issues`、`stack_scopes`、`stack_error_count` 等字段；这与单独调用 `niagara_get_stack_issues` 的数据结构一致。
- 返回值包含 `compile_after_apply`、`force_compile`、`wait_for_complete`、`compilation_requested`、`compilation_complete`、`compile_log_read_ok`、`compile_log_read_error`、`compile_has_error`、`compile_log`。compile log 读取失败时返回 `compile_log_read_failed_after_apply`；`compile_log.has_error=true` 时返回 `compile_failed_after_apply`，且不会保存。

<!-- P4-001 structured-json-authoring: begin -->
## Structured JSON Authoring

- contract_type: `niagara_emitter`
- operation: `apply`
- implementation_status: `audit_only`
- operation_registration: `confirmed`
- command_vs_folder_json: 命令参数只负责定位资产、文件夹和执行选项；可编辑内容写入 folder JSON，不把 `asset_path`、`folder_path`、`include_raw_properties`、`dry_run` 等命令参数当作 authoring 字段。
- default_profile: `llm_authoring`；默认只暴露 public seed、container context、editable authoring 和 required readonly。
- operations: export=`niagara_emitter_export_folder`, apply=`niagara_emitter_apply_folder`, validate=`missing`, diff=`missing`
- included_files: `manifest.json`, `asset.json`, `index.json`, `authoring/emitter.json`, `authoring/stacks.json`, `authoring/renderers.json`, `authoring/events.json`, `authoring/data_interfaces.json`, `...(+3)`
- excluded_or_explicit_files: `validation/issues.json`, `debug/raw_properties.json`, `legacy/`
- included_field_roles: editable_authoring=5, public_seed_identity=1, required_readonly=1
- hidden_field_roles: legacy_compat=1, raw_heavy=1, readback_report=1
- editable_fields_sample: `asset.json#/path`, `authoring/emitter.json#/`, `authoring/stacks.json#/stacks`, `authoring/renderers.json#/renderers`, `authoring/events.json#/events`, `authoring/data_interfaces.json#/data_interfaces`
- required_readonly_sample: `asset.json#/format_version`
- index_rule: `index.json` 只做导航摘要，不保存完整对象；大型集合按 `items/<id>/...` 拆分，短小内容保持合并。
- field_roles: `public_seed_identity` 使用公开 `path`/`name` seed；`required_readonly` 只用于识别、校验或差异判断，不由 apply 写入；`legacy_compat`、`debug_diagnostics`、`readback_report`、`raw_heavy`、`command_param_only` 默认隐藏。
- raw_debug_policy: `include_raw_properties`、raw/readback/debug 输出仅为显式诊断选项，默认 `llm_authoring` 不依赖这些字段。
- legacy_input: accepts=`false`；旧 properties 输入只允许出现在显式迁移、负例、错误或 debug 语境，不作为 folder authoring 输入。
- legacy_locations: `asset.json#/asset_path`, `emitter.json`, `renderers.json`
- legacy_fields_not_recommended: `asset_path`, `object_path`, `class_path`
- legacy_seed_policy: 已知路径实体写 `path`；旧 `asset_path`/`class_path`/`object_path`/`component_class_path`/`legacy_path` 仅用于 legacy、debug 或 readback 兼容。
- validate_flow: 当前 contract 未注册专用 `*_validate_folder`；不要声称 validate 覆盖完整，需用 export/readback/consistency gate 或命令返回字段交叉验证。
- diff_flow: 当前 contract 未注册专用 `*_diff_folder`；不要声称 diff 覆盖完整，必要时用 readback 后结构化比较。
<!-- P4-001 structured-json-authoring: end -->

## Parameters

- `folder_path`：Outliner folder 或资产 folder；具体含义以命令所在领域为准。
- `strict`：严格校验模式。
- `create_if_missing`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `emitter_asset_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。
