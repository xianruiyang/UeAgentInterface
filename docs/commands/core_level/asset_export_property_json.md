---
command: asset_export_property_json
category: core_level
title: `asset_export_property_json`
summary: 导出资产属性为 JSON
aliases: []
tags: [core_level]
status: active
---

# `asset_export_property_json`

## LLM Compact

- status: active.
- purpose: 导出资产属性为 JSON
- key_inputs: `asset_path`, `output_file`, `property_names[]`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 这是当前“小型浅层资产”的主工作流；能被 `properties[]` 表达的逐条纯属性 setter 已迁移到对应 `deprecatedCommand` 分册。
- note: 对字段较多、但仍适合单文件 JSON 的资产，推荐先最小创建，再导出属性 JSON，当作真实模板继续补全。
- note: 传 `output_file` 时，默认把 UE Details 属性外置为 `details` 引用和相邻 `details/index.json` + group JSON；旧 inline `properties[]` 仅用于显式迁移或诊断识别。
- note: 如果某个默认属性在当前资产上不存在，会落到 `missing_properties[]`，不会中断整条导出。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 这是当前“小型浅层资产”的主工作流；能被 `properties[]` 表达的逐条纯属性 setter 已迁移到对应 `deprecatedCommand` 分册。
- 对字段较多、但仍适合单文件 JSON 的资产，推荐先最小创建，再导出属性 JSON，当作真实模板继续补全。
- 传 `output_file` 时，默认编辑相邻 `details/<group>.json#/fields[]`；命令响应只保留 compact 摘要和路径。
- 如果某个默认属性在当前资产上不存在，会落到 `missing_properties[]`，不会中断整条导出。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `output_file`：输出 artifact 路径；大结果应写文件，不要内联到 compact data。
- `property_names[]`：property_names 列表。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
