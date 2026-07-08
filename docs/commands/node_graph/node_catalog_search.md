---
command: node_catalog_search
category: node_graph
title: Node Catalog Search
summary: 查询 UE palette/action menu 风格的可新增节点、模块、输入来源或可放置 Actor 候选，供 LLM 写 JSON 前取证
aliases: [placeable_actor, level_actor, actor_placeable, place_actors, actor_catalog]
tags: [node_graph, placeable_actor, level_actor, actor_placeable, place_actors, actor_catalog]
status: active
---

# `node_catalog_search`

## LLM Compact

- status: active.
- purpose: 查询 UE palette/action menu 风格的可新增节点、模块、输入来源或可放置 Actor 候选，供 LLM 写 JSON 前取证
- key_inputs: `candidate_id`, `catalog_type`, `context.asset_path`, `graph_name`, `node_guid`, `pin_guid`, `pin_name`, `filters` ...
- key_returns: `warnings`, `category_tree`, `counts`, `groups`, `items_tsv`; `items_tsv` 首行为 `name	full_name	category	tooltip	path`，`path` 是当前 seed apply 已验证的主输入列。
- note: 新增节点/模块/expression 前先查询 catalog TSV，从 `path` 或 `name` 列取 seed；写入后必须编译或读回关键 pin/input 状态。
- placeable_actor: `catalog_type=placeable_actor`，别名 `level_actor`、`actor_placeable`；TSV 的 `path` 列可直接写入 `actors/items/<actor_id>/actor.json`。
- placeable_actor_verified: 当前基础候选已通过搜索、最小创建和读回验证，包括 `PointLight`、`DirectionalLight`、`CameraActor`、`PlayerStart`、`TextRenderActor`、`StaticMeshActor`、`TriggerBox`、`Cube`。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 新增节点/模块/expression 前先查询 catalog TSV；写入后必须编译或读回关键 pin/input 状态。
- 创建关卡 Actor 前使用 `catalog_type=placeable_actor` 查询可放置 Actor 候选，优先复制 `items_tsv` 对应行的 `path` 列到 `actors/items/<actor_id>/actor.json`。
- `placeable_actor` 的别名 `level_actor`、`actor_placeable` 会归一化为主类型；正式文档和示例使用 `placeable_actor`。
- `placeable_actor` 的 TSV `path` 列适合直接生成 `actors/items/<actor_id>/actor.json`；单文件 `level_content_apply_json` 的 `resources[].class_path` 属于 legacy/small-payload 兼容路径，不作为 folder authoring 推荐示例。
- Niagara input value source 需要已有 module input 上下文，Niagara graph node 需要脚本图编辑上下文；这些 catalog seed 不是可脱离目标上下文独立 apply 的 payload。
- 已验证基础候选：`PointLight`、`DirectionalLight`、`CameraActor`、`PlayerStart`、`TextRenderActor`、`StaticMeshActor`、`TriggerBox`、`Cube`。

## Parameters

- `candidate_id`：稳定 id，用于精确定位或后续操作。
- `catalog_type`：catalog 主类型。常用值包括 `niagara_module`、`niagara_dynamic_input`、`niagara_graph_node`、`material_node`、`material_expression`、`material_function_call`、`blueprint_node`、`actor_component`、`umg_widget`、`placeable_actor`。`placeable_actor` 别名：`level_actor`、`actor_placeable`。
- `context.asset_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。
- `graph_name`：名称字段；同名对象存在时应结合路径或 id 消歧。
- `node_guid`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `pin_guid`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `pin_name`：名称字段；同名对象存在时应结合路径或 id 消歧。
- `filters`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `page`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `query.text`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `view.include_legacy_authoring`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `include_debug_fields`：布尔/开关字段；默认值以本命令契约为准。

## Returns

- `warnings`：警告摘要。
- `category_tree`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `counts`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `groups`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `items_tsv`：候选列表 TSV；首行为 `name	full_name	category	tooltip	path`。
- `items_tsv.name`：候选短名；无稳定路径候选可作为 `name` seed。
- `items_tsv.full_name`：legacy/display/readback 兼容列，不作为新 authoring 主身份。
- `items_tsv.category`：候选分类，如 `Basic`、`Lights`、`Shapes`、`Volumes`、`Cinematic`。
- `items_tsv.tooltip`：UE tooltip 摘要，保留给 LLM 判断候选含义。
- `items_tsv.path`：当前 seed apply 已验证的主输入列；有路径实体写 `path`，无路径实体可回退使用 `name`。

## Minimal Example

```python
result = node_catalog_search(
    ue,
    catalog_type="placeable_actor",
    query={"text": "Point Light"},
    page={"limit": 5},
)
header, row = result.data["items_tsv"].splitlines()[:2]
columns = dict(zip(header.split("\t"), row.split("\t")))
actor_path = columns["path"]
```

`placeable_actor` 的 folder actor 最小形状：

```json
{
  "operation": "upsert",
  "actor_id": "<actor_id>",
  "label": "<display_name>",
  "path": "/Script/Engine.PointLight",
  "folder_path": "",
  "tags": [],
  "transform": {
    "space": "ue_world",
    "space_contract": {
      "subject": "<actor_id>",
      "dimension": 3,
      "Fsemantic": "ue_world",
      "Fwrite": "ue_world",
      "Fread": "ue_world",
      "Fverify": "ue_world"
    },
    "location": [0, 0, 0],
    "rotation": [0, 0, 0],
    "scale": [1, 1, 1]
  }
}
```

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

对 `placeable_actor`，验证链路为：`node_catalog_search` -> 解析 `items_tsv` 的 `path` / `name` 列 -> 写入 `actors/items/<actor_id>/actor.json` -> `level_apply_folder` -> `level_export_folder` 读回 class、label、folder、transform。`class_path` 只作为 readback/legacy 兼容字段检查。
