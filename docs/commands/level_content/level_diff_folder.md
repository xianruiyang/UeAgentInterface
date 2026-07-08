---
command: level_diff_folder
category: level_content
title: Diff Level Folder
summary: 对比两个 Level folder package，并按 Level、Actor、Component、Property、Reference 输出 compact diff。
tags: [level_content, folder, diff, actor, properties]
status: active
---

# `level_diff_folder`

对比两个 Level folder package。该指令只读输入目录，不修改关卡或资产。输出面向 LLM 阅读，默认不返回完整文件内容。

## LLM Compact

- status: active.
- purpose: 对比两个 Level folder package，并按 Level/Actor/Component/Property/Reference 输出 compact diff。
- key_inputs: `base_folder` / `left_folder`, `target_folder` / `right_folder`, `max_changes`.
- key_returns: `schema`, `status`, `same`, `changes_total`, `truncated`, `counts_by_group`, `changes[]`.
- note: 比较单文件 Level Content JSON 与当前场景使用 `level_content_diff_json`；比较两个 folder package 使用本命令。
- note: `details/index.json` + `details/<group>.json` 按 `property_path` 比较；旧 `properties.json` 不进入正常 diff。

## Workflow Boundary

- 本命令比较两个 Level folder package 目录。
- 单文件目标 JSON 与当前场景的差异仍使用 `level_content_diff_json`。
- 两套链路都保持 active；`level_diff_folder` 覆盖的是 folder package diff，不替代单文件 Level Content diff。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Parameters
```json
{"base_folder":"tmp/base_package","target_folder":"tmp/target_package","max_changes":512}
```

- `base_folder`：必填，基准 folder package。别名：`left_folder`。
- `target_folder`：必填，目标 folder package。别名：`right_folder`。
- `max_changes`：可选，默认 `512`。限制 `changes[]` 返回条数；总数仍写入 `changes_total` 和 `counts_by_group`。

## Diff Groups

- `Level`：根级 package 文件，例如 `manifest.json`、`level.json`、`world.json`、World Settings、streaming、HLOD 等。
- `Actor`：`actors/items/<actor_id>/actor.json` 等 Actor package 文件。
- `Component`：`components/<component_id>/component.json` 等 Component package 文件。
- `Property`：Actor/Component/WorldSettings 的 grouped Details，按 `property_path` 比较。
- `Reference`：`references/assets.json` 与 `external_assets.json`。

## Returns

```json
{"schema":"uai.level_folder_diff.v1","status":"different","same":false,"base_folder":"...","target_folder":"...","files_base":7,"files_target":7,"changes_total":5,"truncated":false,"counts_by_group":{"Level":1,"Actor":1,"Component":1,"Property":1,"Reference":1},"changes":[{"group":"Property","change":"changed","path":"actors/items/a/details/Transform.json","detail":"ActorLabel"}]}
```

- `status`：`same` 或 `different`。
- `same`：没有差异时为 `true`。
- `counts_by_group`：按 `Level`、`Actor`、`Component`、`Property`、`Reference` 汇总差异。
- `changes[]`：每项包含 `group`、`change`、`path`，属性差异额外在 `detail` 写入 `property_path`。
- `truncated`：`changes[]` 达到 `max_changes` 时为 `true`。

## Recommended Flow

1. 对两个 folder package 分别调用 `level_validate_folder`。
2. 调用 `level_diff_folder`。
3. 根据 `counts_by_group` 判断影响范围。
4. 根据 `changes[].path` 和 `detail` 精确修改 folder package，不搜索源码或旧模板。

<!-- P4-001 structured-json-authoring: begin -->
## Structured JSON Authoring

- contract_type: `level`
- operation: `diff`
- implementation_status: `confirmed`
- operation_registration: `confirmed`
- command_vs_folder_json: 命令参数只负责定位资产、文件夹和执行选项；可编辑内容写入 folder JSON，不把 `asset_path`、`folder_path`、`include_raw_properties`、`dry_run` 等命令参数当作 authoring 字段。
- default_profile: `llm_authoring`；默认只暴露 public seed、container context、editable authoring 和 required readonly。
- operations: export=`level_export_folder`, apply=`level_apply_folder`, validate=`level_validate_folder`, diff=`level_diff_folder`
- included_files: `manifest.json`, `index.json`, `level.json`, `world.json`, `world_settings/summary.json`, `world_settings/details/index.json`, `streaming/index.json`, `data_layers.json`, `...(+8)`
- excluded_or_explicit_files: `actors/items/<actor_id>/properties.json`, `actors/items/<actor_id>/components/<component_id>/properties.json`, `world_settings/properties.json`, `validation/summary.json`, `feature_gates.json`, `diagnostics.json`, `editor/summary.json`, `...(+2)`
- included_field_roles: editable_authoring=2, public_seed_identity=2, required_readonly=1
- hidden_field_roles: legacy_compat=2, raw_heavy=1, readback_report=1
- editable_fields_sample: `actors/items/<actor_id>/details/<group>.json#/fields[]`, `actors/items/<actor_id>/components/<component_id>/details/<group>.json#/fields[]`, `world_settings/details/<group>.json#/fields[]`, `actors/items/<actor_id>/actor.json#/label`
- required_readonly_sample: `manifest.json#/schema`
- index_rule: `index.json` 只做导航摘要，不保存完整对象；大型集合按 `items/<id>/...` 拆分，短小内容保持合并。
- field_roles: `public_seed_identity` 使用公开 `path`/`name` seed；`required_readonly` 只用于识别、校验或差异判断，不由 apply 写入；`legacy_compat`、`debug_diagnostics`、`readback_report`、`raw_heavy`、`command_param_only` 默认隐藏。
- raw_debug_policy: `include_raw_properties`、raw/readback/debug 输出仅为显式诊断选项，默认 `llm_authoring` 不依赖这些字段。
- legacy_input: accepts=`false`；旧 properties 输入只允许出现在显式迁移、负例、错误或 debug 语境，不作为 folder authoring 输入。
- legacy_locations: `legacy/`
- legacy_fields_not_recommended: `class_path`, `object_path`
- legacy_seed_policy: 已知路径实体写 `path`；旧 `asset_path`/`class_path`/`object_path`/`component_class_path`/`legacy_path` 仅用于 legacy、debug 或 readback 兼容。
- validate_flow: 先调用 `level_validate_folder` 做只读结构校验，再 apply；validate 结果不是写入结果。
- diff_flow: 可调用 `level_diff_folder` 比较两份 folder package。
<!-- P4-001 structured-json-authoring: end -->
