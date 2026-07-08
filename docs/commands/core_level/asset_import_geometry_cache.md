---
command: asset_import_geometry_cache
category: core_level
title: `asset_import_geometry_cache`
summary: 导入 Geometry Cache / Alembic
aliases: []
tags: [core_level]
status: active
---

# `asset_import_geometry_cache`

## LLM Compact

- status: active.
- purpose: 导入 Geometry Cache / Alembic
- key_inputs: `destination_path`, `source_file`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 通过 UE 自动导入管线导入缓存资产。
- note: 返回 `imported_assets[]`、`imported_asset_count` 和 `plugin_status`。
- note: Geometry Cache 内部逐帧顶点数据不通过 JSON 编辑；导入后使用 `geometry_cache_get_info` 与 `geometry_cache_validate_against_skeletal_mesh` 做验证。详见 `18_Deformer_MLDeformer_GeometryCache.md`。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 通过 UE 自动导入管线导入缓存资产。
- 返回 `imported_assets[]`、`imported_asset_count` 和 `plugin_status`。
- Geometry Cache 内部逐帧顶点数据不通过 JSON 编辑；导入后使用 `geometry_cache_get_info` 与 `geometry_cache_validate_against_skeletal_mesh` 做验证。详见 `18_Deformer_MLDeformer_GeometryCache.md`。

## Parameters

- `destination_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。
- `source_file`：文件路径字段；长 JSON/结果优先走文件。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
