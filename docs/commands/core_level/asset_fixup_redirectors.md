---
command: asset_fixup_redirectors
category: core_level
title: Core / Session
summary: 删除资产移动后已无引用的 `ObjectRedirector`
aliases: []
tags: [core_level]
status: active
---

# `asset_fixup_redirectors`

## LLM Compact

- status: active.
- purpose: 删除资产移动后已无引用的 `ObjectRedirector`
- key_inputs: `asset_path`, `dry_run`, `folder_path`, `folder_paths`, `recursive`, `asset_paths`, `fail_on_blocked`
- key_returns: `redirector_still_referenced`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `dry_run`：只计划/报告不写入；删除类命令通常默认 true，执行删除需显式 false。
- `folder_path`：Outliner folder 或资产 folder；具体含义以命令所在领域为准。
- `folder_paths`：多个 folder 路径。
- `recursive`：是否包含子 folder/子对象。
- `asset_paths`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `fail_on_blocked`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `redirector_still_referenced`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
