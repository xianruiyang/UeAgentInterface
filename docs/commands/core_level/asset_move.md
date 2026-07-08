---
command: asset_move
category: core_level
title: Core / Session
summary: 通过 UE AssetTools 移动或重命名资产
aliases: []
tags: [core_level]
status: active
---

# `asset_move`

## LLM Compact

- status: active.
- purpose: 通过 UE AssetTools 移动或重命名资产
- key_inputs: `asset_path`, `dry_run`, `destination_asset_path`, `destination_folder`, `destination_name`, `save_after_move`, `source_asset_path`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `dry_run`：只计划/报告不写入；删除类命令通常默认 true，执行删除需显式 false。
- `destination_asset_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。
- `destination_folder`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `destination_name`：名称字段；同名对象存在时应结合路径或 id 消歧。
- `save_after_move`：布尔/开关字段；默认值以本命令契约为准。
- `source_asset_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
