---
command: asset_import_texture
category: core_level
title: `asset_import_texture`
summary: 导入外部贴图为 Texture2D
aliases: []
tags: [core_level]
status: active
---

# `asset_import_texture`

## LLM Compact

- status: active.
- purpose: 导入外部贴图为 Texture2D
- key_inputs: `destination_name`, `srgb`, `compression_settings`, `mip_gen_settings`, `lod_group`, `destination_path`, `source_filename`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 通用导入字段：`imported_object_count`、`imported_objects[]`、`primary_asset_path` 等。
- note: 该命令只负责把外部贴图正式导入为 UE Texture2D，并在导入后回读关键属性；材质图、Niagara Renderer、SubUV 行列数等仍走对应资产的 JSON / folder workflow。
- note: 枚举字段解析失败、布尔字段类型错误、扩展名不支持、UE 工厂拒绝导入都会返回硬错误，不会静默忽略。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 通用导入字段：`imported_object_count`、`imported_objects[]`、`primary_asset_path` 等。
- 该命令只负责把外部贴图正式导入为 UE Texture2D，并在导入后回读关键属性；材质图、Niagara Renderer、SubUV 行列数等仍走对应资产的 JSON / folder workflow。
- 枚举字段解析失败、布尔字段类型错误、扩展名不支持、UE 工厂拒绝导入都会返回硬错误，不会静默忽略。

## Parameters

- `destination_name`：名称字段；同名对象存在时应结合路径或 id 消歧。
- `srgb`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `compression_settings`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `mip_gen_settings`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `lod_group`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `destination_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。
- `source_filename`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
