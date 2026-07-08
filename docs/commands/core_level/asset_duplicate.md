---
command: asset_duplicate
category: core_level
title: Core / Session
summary: 复制资产到指定路径
aliases: []
tags: [core_level]
status: active
---

# `asset_duplicate`

## LLM Compact

- status: active.
- purpose: 复制资产到指定路径
- key_inputs: `destination_asset_path`, `save_after_duplicate`, `source_asset_path`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `destination_asset_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。
- `save_after_duplicate`：布尔/开关字段；默认值以本命令契约为准。
- `source_asset_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
