---
command: media_source_export_json
category: texture_render_target_media
title: Media
summary: File/Stream MediaSource 创建、读取、导出、回写。
aliases: []
tags: [texture_render_target_media]
status: active
---

# `media_source_export_json`

## LLM Compact

- status: active.
- purpose: File/Stream MediaSource 创建、读取、导出、回写。
- key_inputs: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 大对象或长列表优先传 `output_file`，compact 返回只保留摘要和路径。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 大对象或长列表优先传 `output_file`，compact 返回只保留摘要和路径。
- 传 `output_file` 时默认把 UE Details 属性外置为 `details` 引用；字段在相邻 `details/<group>.json#/fields[]`，并带 `property_path`、`selector`、`display_name`、`category_path`、`control_type`、`value_kind`、`availability`、`operations`、`tooltip` 和 enum `candidates`。
- 默认 authoring 根对象不返回 `coverage_report`；validate/report/raw/debug 路径才包含诊断报告。
- legacy `name` 和 `cpp_type` 不作为默认属性字段。

## Parameters

- 未在旧分册命令表中列出；以实现 schema/readback 为准。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
