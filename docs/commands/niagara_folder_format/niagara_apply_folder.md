---
command: niagara_apply_folder
category: niagara_folder_format
title: 指令列表
summary: 从文件夹式 JSON 回写 `UNiagaraSystem`
aliases: []
tags: [niagara_folder_format]
status: active
---

# `niagara_apply_folder`

## LLM Compact

- status: active.
- purpose: 从文件夹式 JSON 回写 `UNiagaraSystem`
- key_inputs: `asset_path`, `folder_path`, `strict`, `apply_referenced_emitters`, `create_if_missing`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- behavior: 写入命令；必须明确目标范围、默认值和写后读回验收。
- note: JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- note: folder authoring 以导出/最小 authoring/readback 为主；新增节点、组件或模块先用 catalog/seed，再按读回修补。
- note: 常规 apply 字段：`system_properties_applied`、`user_parameters_applied`、`system_stages_applied`、`emitter_handles_applied`、`emitter_properties_applied`、`post_refresh_data_interfaces_applied`、`system_refresh`、`warnings`、`warning_count`。
- note: 编译字段：`compile_after_apply`、`force_compile`、`wait_for_complete`、`compilation_requested`、`compilation_complete`、`has_outstanding_compilation_requests`、`outstanding_after_stack_issue_read`、`post_stack_compilation_requested`、`post_stack_compilation_complete`、`compile_log_read_ok`、`compile_log_read_error`、`compile_has_error`、`compile_log`。apply 会先编译，再读取 Stack issue；如果 ViewModel/Stack 读回让 System 再次进入 outstanding 状态，会立刻再强制编译一次并重读 compile log。最终等待后仍有 outstanding compile request 时才返回 `compile_incomplete_after_apply`；compile log 读取失败时返回 `compile_log_read_failed_after_apply`；`compile_log.has_error=true` 时返回 `compile_failed_after_apply`。这些失败都保留 `saved=false`，不会执行最终保存。
- validation: apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Authoring Notes
- JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- folder authoring 以导出/最小 authoring/readback 为主；新增节点、组件或模块先用 catalog/seed，再按读回修补。
- 常规 apply 字段：`system_properties_applied`、`user_parameters_applied`、`system_stages_applied`、`emitter_handles_applied`、`emitter_properties_applied`、`post_refresh_data_interfaces_applied`、`system_refresh`、`warnings`、`warning_count`。
- 编译字段：`compile_after_apply`、`force_compile`、`wait_for_complete`、`compilation_requested`、`compilation_complete`、`has_outstanding_compilation_requests`、`outstanding_after_stack_issue_read`、`post_stack_compilation_requested`、`post_stack_compilation_complete`、`compile_log_read_ok`、`compile_log_read_error`、`compile_has_error`、`compile_log`。apply 会先编译，再读取 Stack issue；如果 ViewModel/Stack 读回让 System 再次进入 outstanding 状态，会立刻再强制编译一次并重读 compile log。最终等待后仍有 outstanding compile request 时才返回 `compile_incomplete_after_apply`；compile log 读取失败时返回 `compile_log_read_failed_after_apply`；`compile_log.has_error=true` 时返回 `compile_failed_after_apply`。这些失败都保留 `saved=false`，不会执行最终保存。
- Stack 诊断字段：`stack_issue_report`、`stack_issues`、`stack_scopes`、`stack_total_issue_count`、`stack_error_count`、`stack_warning_count`、`stack_info_count`、`has_stack_errors`、`stack_issue_view_model_source`。
- 预览恢复字段：`restore_preview_after_apply`、`preview_restore`。`preview_restore.restored=true` 且 `preview_restore.is_paused=false` 表示 apply 结束后已恢复 Niagara 编辑器预览；即使随后因编译、Stack 或 strict 门禁返回失败，该字段也会尽量随已填充的 `data` 返回，便于确认没有把 UI 预览留在暂停态。
- 当 `strict=true` 且 Stack error 非空时，命令失败返回 `strict_apply_has_stack_errors`，同时返回具体 `stack_issues[] / stack_scopes[]`。

<!-- P4-001 structured-json-authoring: begin -->
## Structured JSON Authoring

- contract_type: `niagara_system`
- operation: `apply`
- implementation_status: `audit_only`
- operation_registration: `confirmed`
- command_vs_folder_json: 命令参数只负责定位资产、文件夹和执行选项；可编辑内容写入 folder JSON，不把 `asset_path`、`folder_path`、`include_raw_properties`、`dry_run` 等命令参数当作 authoring 字段。
- default_profile: `llm_authoring`；默认只暴露 public seed、container context、editable authoring 和 required readonly。
- operations: export=`niagara_export_folder`, apply=`niagara_apply_folder`, validate=`missing`, diff=`missing`
- included_files: `manifest.json`, `asset.json`, `index.json`, `authoring/system.json`, `authoring/scripts/items/SystemSpawn/graph.json`, `authoring/scripts/items/SystemSpawn/layout.json`, `authoring/scripts/items/SystemUpdate/graph.json`, `authoring/scripts/items/SystemUpdate/layout.json`, `...(+3)`
- excluded_or_explicit_files: `validation/issues.json`, `debug/raw_properties.json`, `legacy/`
- included_field_roles: editable_authoring=4, public_seed_identity=1, required_readonly=1
- hidden_field_roles: legacy_compat=1, raw_heavy=1, readback_report=1
- editable_fields_sample: `asset.json#/path`, `authoring/system.json#/`, `authoring/scripts/items/SystemSpawn/graph.json#/nodes`, `authoring/scripts/items/SystemUpdate/graph.json#/nodes`, `authoring/emitter_overrides/index.json#/emitters`
- required_readonly_sample: `asset.json#/format_version`
- index_rule: `index.json` 只做导航摘要，不保存完整对象；大型集合按 `items/<id>/...` 拆分，短小内容保持合并。
- field_roles: `public_seed_identity` 使用公开 `path`/`name` seed；`required_readonly` 只用于识别、校验或差异判断，不由 apply 写入；`legacy_compat`、`debug_diagnostics`、`readback_report`、`raw_heavy`、`command_param_only` 默认隐藏。
- raw_debug_policy: `include_raw_properties`、raw/readback/debug 输出仅为显式诊断选项，默认 `llm_authoring` 不依赖这些字段。
- legacy_input: accepts=`false`；旧 properties 输入只允许出现在显式迁移、负例、错误或 debug 语境，不作为 folder authoring 输入。
- legacy_locations: `asset.json#/asset_path`, `settings/system.json`, `emitters/`
- legacy_fields_not_recommended: `asset_path`, `object_path`, `class_path`
- legacy_seed_policy: 已知路径实体写 `path`；旧 `asset_path`/`class_path`/`object_path`/`component_class_path`/`legacy_path` 仅用于 legacy、debug 或 readback 兼容。
- validate_flow: 当前 contract 未注册专用 `*_validate_folder`；不要声称 validate 覆盖完整，需用 export/readback/consistency gate 或命令返回字段交叉验证。
- diff_flow: 当前 contract 未注册专用 `*_diff_folder`；不要声称 diff 覆盖完整，必要时用 readback 后结构化比较。
<!-- P4-001 structured-json-authoring: end -->

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `folder_path`：Outliner folder 或资产 folder；具体含义以命令所在领域为准。
- `strict`：严格校验模式。
- `apply_referenced_emitters`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `create_if_missing`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。
