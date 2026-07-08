---
command: project_settings_export_page
category: project_settings_config
title: Project Settings
summary: 导出某个 Settings 页的字段 schema 与当前值
aliases: []
tags: [project_settings_config]
status: active
---

# `project_settings_export_page`

## LLM Compact

- status: active.
- purpose: 导出某个 Settings 页的字段 schema 与当前值
- key_inputs: `output_file`, `container`, `category`, `section`, `include_defaults`, `include_hidden_properties`, `include_metadata`, `include_non_editable` ...
- key_returns: `schema`, `capabilities`, `field_schema_hash`, `fields{}`, `page_value_hash`, `section_info{}` ...
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 默认字段项使用 `property_path`、`selector`、`display_name`、`category_path`、`control_type`、`value_kind`、`availability`、`operations`、`tooltip` 和 `candidates`。
- 默认不返回 `raw_config`、`raw_config_file` 或 `generated_at`；只有显式 `include_raw_config=true` 才写出 raw config 诊断内容。
- legacy `name`、`cpp_type`、`enum_options` 不作为默认 authoring 字段。

## Parameters

- `output_file`：输出 artifact 路径；大结果应写文件，不要内联到 compact data。
- `container`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `category`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `section`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `include_defaults`：布尔/开关字段；默认值以本命令契约为准。
- `include_hidden_properties`：布尔/开关字段；默认值以本命令契约为准。
- `include_metadata`：布尔/开关字段；默认值以本命令契约为准。
- `include_non_editable`：布尔/开关字段；默认值以本命令契约为准。
- `include_raw_config`：布尔/开关字段；默认值以本命令契约为准。
- `output_directory`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `output_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。
- `section_id`：稳定 id，用于精确定位或后续操作。
- `write_file`：文件路径字段；长 JSON/结果优先走文件。

## Returns

- `schema`：返回 payload schema。
- `capabilities`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `field_schema_hash`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `fields{}`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `page_value_hash`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `raw_config`：仅 `include_raw_config=true` 时返回；属于 raw/debug 诊断内容，不是默认 authoring 字段。
- `raw_config_file`：仅 `include_raw_config=true` 且写文件时返回；属于 raw/debug 诊断内容。
- `section_info{}`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `settings_object`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `values{}`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
