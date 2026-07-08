---
command: level_validate_folder
category: level_content
title: Validate Level Folder
summary: 校验 Level folder package 的结构、类路径、属性 patch 和引用目标。
tags: [level_content, folder, validate, actor, properties]
status: active
---

# `level_validate_folder`

校验 `level_export_folder` / `level_apply_folder` 使用的 Level folder package。该指令只读输入目录，不修改关卡或资产。

## LLM Compact

- status: active.
- purpose: 只读校验 Level folder package 的 schema、Actor/Component class、grouped Details 和 references。
- key_inputs: `input_folder` / `folder_path`, `check_references`.
- key_returns: `schema`, `status`, `ok`, `actors_seen`, `properties_seen`, `references_seen`, `errors`, `warnings`, `issues[]`.
- note: 校验单文件 Level Content JSON 使用 `level_content_validate_json`；校验 folder package 使用本命令。
- note: `issues[]` 用 `severity/code/path/message` 精确定位问题文件和字段。
- note: 默认校验 `details/index.json` + `details/<group>.json`；旧 `properties.json` 不再作为 authoring 输入，如存在应报告 legacy rejection 或迁移问题。

## Workflow Boundary

- 本命令校验 Level folder package 目录。
- 单文件 Level Content JSON payload 仍使用 `level_content_validate_json`。
- 两套链路都保持 active；选择依据是输入形态，而不是新旧替代关系。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Parameters
```json
{"input_folder":"tmp/uai_level_package","check_references":true}
```

- `input_folder`：必填，folder package 根目录。别名：`folder_path`。
- `check_references`：可选，默认 `true`。检查 `references/assets.json` 中的目标是否可解析。

## Checks

- `manifest.json` 可读取，schema 与 `uai.level_folder.v1` 一致。
- `actors/items/<actor_id>/actor.json` 存在且可读取。
- Actor `class_path` 可加载，并且是 `AActor` 派生类。
- Component `component.json` 可读取，`class_path` 可加载，并且是 `UActorComponent` 派生类。
- Actor/Component/WorldSettings `details/index.json` 可读取，所有 `details/<group>.json` 存在且包含 `fields[]`。
- Actor/Component/WorldSettings 旧 `properties.json` 如存在，应作为 legacy 输入问题报告，不作为 grouped Details 校验来源。
- 每个 property patch 必须有 `property_path`，并且有 `value_text` 或 `value`。
- 基于 Actor/Component class CDO 检查属性是否存在、是否可编辑或只读。
- 引用项必须有 `target_path` 或 `target_package_path`；无法解析时返回 warning。

## Returns

```json
{"schema":"uai.level_folder_validate.v1","status":"valid","folder_path":"...","ok":true,"actors_seen":1,"properties_seen":1,"references_seen":0,"errors":0,"warnings":0,"issues":[]}
```

- `status`：`valid` 或 `invalid`。
- `ok`：没有 error 时为 `true`。
- `issues[]`：每项包含 `severity`、`code`、`path`、`message`，用于定位缺文件、错误 schema、无效 class、失效引用、不可写属性等问题。

## Recommended Flow

1. 用 `node_catalog_search` 或已有导出创建 folder package。
2. 调用 `level_validate_folder`。
3. 只有 `ok=true` 时再调用 `level_apply_folder`。
4. 若失败，按 `issues[].path` 精确修正对应文件，不搜索源码或旧模板。

<!-- P4-001 structured-json-authoring: begin -->
## Structured JSON Authoring

- contract_type: `level`
- operation: `validate`
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
