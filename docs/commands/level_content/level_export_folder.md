---
command: level_export_folder
category: level_content
title: Export Level Folder
summary: 将当前关卡导出为文件夹式结构化 Level package。
tags: [level_content, folder, export, actor]
status: active
---

# `level_export_folder`

将当前关卡导出为文件夹式结构化 Level package。该导出用于 LLM 分块阅读、后续 `level_apply_folder` / validate / diff 工作流，不把所有内容压进单个 JSON。

## LLM Compact

- status: active.
- purpose: 将当前关卡导出为多文件 Level folder package，便于 LLM 分块读取、审阅、修改和 diff。
- key_inputs: `output_folder`, `overwrite`.
- key_returns: `schema`, `status`, `folder_path`, `level_package`, `files_written`, `directories_created`.
- note: 推荐链路为 `level_export_folder` -> 编辑 folder package -> `level_validate_folder` -> `level_diff_folder` -> `level_apply_folder`。
- note: 单文件小规模场景写入仍用 `level_content_apply_json`；不要把 Level Content JSON payload 当成 folder package。
- note: 默认 `llm_authoring` 下 Actor、Component、WorldSettings 的 UE Details 字段写入统一 `details/index.json` + `details/<group>.json`，入口 JSON 只做概要和导航。

## Workflow Boundary

- 本命令是多文件 folder workflow 的入口，推荐链路为：`level_export_folder` -> 编辑 folder package -> `level_validate_folder` -> `level_diff_folder` -> `level_apply_folder`。
- 单文件、小规模场景写入仍使用 `level_content_apply_json`；不要把 `level_content_apply_json` 的 payload 当成 folder package。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Parameters
```json
{"output_folder":"tmp/uai_level_package","overwrite":true}
```

- `output_folder`：可选，导出根目录。为空时使用 Saved 下的默认目录。别名：`output_dir`。
- `overwrite`：可选，默认 `true`。

## 主要产物

- 根文件：`README.md`、`manifest.json`、`index.json`、`level.json`、`world.json`。
- Actor：`actors/folders.json`、`actors/index.json`、`actors/items/<actor_id>/actor.json`、`actors/items/<actor_id>/details/index.json`、`actors/items/<actor_id>/details/<group>.json`、`components/<component_id>/...`。
- Component：`actors/items/<actor_id>/components/<component_id>/component.json`、`components/<component_id>/details/index.json`、`components/<component_id>/details/<group>.json`。
- WorldSettings：`world_settings/summary.json`、`world_settings/details/index.json`、`world_settings/details/<group>.json`。
- Debug-only：`feature_gates.json`、`diagnostics.json`、`editor/summary.json`、`level_blueprint/summary.json` 仅 debug profile 输出。
- Legacy/split：显式 split、`legacy`、`readback` 或 `debug` profile 可继续输出 `properties.json`。
- Topology：`streaming/index.json`、`data_layers.json`、`world_partition.json`、`hlod.json`。
- 引用：`references/assets.json`、`external_assets.json`。

## Returns

```json
{"schema":"uai.level_folder.v1","status":"exported","folder_path":"...","level_package":"...","files_written":18,"directories_created":9}
```

`external_assets.json` 只记录外部资产引用摘要，不内联目标资产；`/Script/...` native class 不作为外部资产列出。

## Minimal Example

```python
from uai_core import UaiCore
from uai_core.commands.level_export_folder import level_export_folder

ue = UaiCore.from_config(require_token=True)
result = level_export_folder(
    ue,
    output_folder="tmp/uai_level_package",
    overwrite=True,
)
result.raise_for_failure()
folder_path = result.data["folder_path"]
```

导出后优先读取：

- `manifest.json`：package schema。
- `actors/index.json`：Actor 检索入口。
- `actors/items/<actor_id>/actor.json`：Actor 身份、transform 摘要、Component 索引和 `details` 引用。
- `actors/items/<actor_id>/details/index.json`：Actor Details 分组索引。
- `actors/items/<actor_id>/components/<component_id>/component.json`：Component 身份、attachment 摘要和 `details` 引用。
- `actors/items/<actor_id>/components/<component_id>/details/index.json`：Component Details 分组索引。
- `world_settings/summary.json`：WorldSettings 身份和 `details` 引用。
- `references/assets.json`：引用摘要。

<!-- P4-001 structured-json-authoring: begin -->
## Structured JSON Authoring

- contract_type: `level`
- operation: `export`
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
