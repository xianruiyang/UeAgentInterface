---
command: level_apply_folder
category: level_content
title: Apply Level Folder
summary: 从 Level folder package 创建或更新关卡 Actor，并写回 grouped Details。
tags: [level_content, folder, apply, actor, properties]
status: active
---

# `level_apply_folder`

从 Level folder package 创建或更新关卡中的 Actor，并基于统一 Details authoring 公共层写回 Actor、Component 和 WorldSettings 属性。默认只消费 `details/index.json` + `details/<group>.json`。

## LLM Compact

- status: active.
- purpose: 从 Level folder package 创建或更新关卡 Actor，并写回 Actor/Component/WorldSettings grouped Details。
- key_inputs: `input_folder` / `folder_path`, `create_missing_actors`.
- key_returns: `schema`, `status`, `actors_seen`, `actors_created`, `actors_updated`, `actor_properties_applied`, `component_properties_applied`, `actors[]`, `warnings[]`.
- note: 本命令消费 folder package 目录，不消费 `level_content_apply_json` 的单文件 payload。
- note: 推荐先 `level_validate_folder`，必要时 `level_diff_folder`，再 `level_apply_folder`。
- note: 新 authoring 默认编辑 `details/<group>.json#/fields[]`；旧 `properties.json` 输入不再作为合法 authoring。

## Workflow Boundary

- 本命令消费 Level folder package 目录，不消费 `level_content_apply_json` 的单文件 payload。
- 小规模资源写入可继续用 `level_content_apply_json`；需要 folder package 审阅、validate、diff 或批量 Actor/Component 属性写回时使用本命令。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Parameters
```json
{"input_folder":"tmp/uai_level_package","create_missing_actors":true}
```

- `input_folder`：必填，folder package 根目录。别名：`folder_path`。
- `create_missing_actors`：可选，默认 `true`。为 `false` 时只更新已存在 Actor，不创建缺失 Actor。

## 能力边界

- 消费 `actors/items/<actor_id>/actor.json` 创建或更新 Actor。
- 依据公开 `path` seed 创建 Actor，支持 `/Script/...` native Actor 类和可加载的 Actor 派生类；旧 `class_path` 仅作为 readback 兼容字段。
- 依据 `object_path` 或 `label` 查找已存在 Actor；命中后更新 `label` 与 `folder_path`。
- 写回 `actors/items/<actor_id>/details/<group>.json` 中的 Actor 属性。
- 写回 `actors/items/<actor_id>/components/<component_id>/details/<group>.json` 中的 Component 属性。
- 写回 `world_settings/details/<group>.json` 中的 WorldSettings 属性。
- 旧 `actors/items/<actor_id>/properties.json`、Component `properties.json` 和 `world_settings/properties.json` 不再作为合法 authoring 输入。
- 属性项使用稳定 `property_path` 定位字段，优先读取 `value_text`，缺省时读取 `value`。
- 不可写、缺失、导入失败的属性不会静默吞掉，会进入 per-property result，并计入 skipped/failed 或 warnings。
- 不从 `references/assets.json` 或 `external_assets.json` 推断 Actor 类型。

## Returns

```json
{"schema":"uai.level_folder_apply.v1","status":"applied","folder_path":"...","actors_seen":1,"actors_created":1,"actors_updated":0,"actors_skipped":0,"actor_properties_applied":1,"component_properties_applied":1,"actors":[{"actor_id":"seed_actor","label":"MyActor","class_path":"/Script/Engine.PointLight","status":"created","object_path":"...","properties_apply":{"schema":"uai.uobject_properties_apply.v1","applied":0,"skipped":0,"failed":0},"components":[{"component_id":"seed_light_component","name":"PointLightComponent0","class_path":"/Script/Engine.PointLightComponent","properties_apply":{"schema":"uai.uobject_properties_apply.v1","applied":1,"skipped":0,"failed":0}}]}],"warnings":[]}
```

`actors[].status` 为 `created`、`updated` 或 `skipped`。跳过不可加载类、缺少 `path`（或 legacy `class_path`）或 spawn 失败时，原因写入 `reason` 和 `warnings`。

## 推荐流程

1. 使用 `node_catalog_search` 或等价可放置 Actor 查询取得 `items_tsv`，从 `path` / `name` 列取 seed。
2. 创建最小 folder package，并把 seed 写入 `actors/items/<actor_id>/actor.json`；新 authoring 写 `path`，不要把 `class_path` 当作主身份字段。
3. 如需改属性，优先编辑 Actor、Component 或 WorldSettings 的 `details/<group>.json#/fields[]`；只需要保留要 patch 的字段值即可。
4. 调用 `level_validate_folder`。
5. 可选调用 `level_diff_folder` 对比原 package 与目标 package。
6. 调用 `level_apply_folder`。
7. 调用 `level_export_folder` 读回，确认 Actor、Component 和目标属性已进入同一 package 结构。

<!-- P4-001 structured-json-authoring: begin -->
## Structured JSON Authoring

- contract_type: `level`
- operation: `apply`
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
