---
command: level_content_query_json
category: level_content
title: Query Level Content JSON
summary: 查询当前世界中的场景内容，并按 Level Content JSON 投影返回。
aliases: [level_actor_export_json]
tags: [level_content, query, scene, whitebox]
status: active
---

# `level_content_query_json`

## LLM Compact

- status: active.
- purpose: 查询当前世界中的场景内容，并按 Level Content JSON 投影返回。
- key_inputs: `selection`, `projection`, `output_file`, `actor_only`
- key_returns: `schema`, `world`, `resources[]`, `resource_count`, `organization`, `feature_gates`
- behavior: 只读查询类命令；大结果应写入 `output_file`。
- note: 大对象或长列表优先传 `output_file`，文件使用 `structured_compact` JSON；compact 返回只保留摘要、路径和 `output_format`。
- note: Level Content JSON 描述场景实例最终态；复杂资产本体仍走 Blueprint/Material/Niagara 等专用 authoring。
- validation: 写操作后用 query/snapshot 读回 label、folder、transform、tags、components/collision；删除先 dry-run 再执行。

## Authoring Notes

- 大对象或长列表优先传 `output_file`，文件使用 `structured_compact` JSON；compact 返回只保留摘要、路径和 `output_format`。
- Level Content JSON 描述场景实例最终态；复杂资产本体仍走 Blueprint/Material/Niagara 等专用 authoring。

## Parameters

- `selection`：支持 `folder`、`selected`、`all` 等模式；folder 查询配合 `folder_path` / `folder_root`。
- `projection`：控制返回字段；局部白盒通常只取 actor、folder、transform、tags、components 摘要。
- `output_file`：需要完整结果时写出到文件，文件使用 `structured_compact` JSON；返回体保留摘要、计数、world、feature_gates、`output_file` 和 `output_format`，不再返回完整 `resources[]`。
- `actor_only`：Actor-only 兼容模式；新流程通常不需要手动开启。

## Returns

- `schema` / `world`：Level Content JSON 版本与世界信息。
- `resources[]`：资源摘要。
- `resource_count`：资源数量。
- `organization` / `feature_gates`：组织语义与功能门控。
- `output_file` / `output_format`：完整导出文件路径与实际落盘格式；传入 `output_file` 时返回。

## Minimal Example

```json
{
  "command": "level_content_query_json",
  "params": {
    "selection": { "mode": "folder", "folder_path": "UAI/WB_WhiteBox01", "include_child_folders": true },
    "projection": { "include_transform": true, "include_components": "summary" },
    "output_file": "tmp/uai_params/wb_query.json"
  }
}
```

## Validation

搭建前用于确认目标 folder 是否已有内容；写入后用于确认新增/删除对象数量、folder、label 和 transform 是否符合任务产出。需要展开 `resources[]` 时读取 `output_file`。
