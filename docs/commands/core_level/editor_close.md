---
command: editor_close
category: core_level
title: `editor_close`
summary: 关闭编辑器；若仍有未处理脏资源则失败并返回清单
aliases: []
tags: [core_level]
status: active
---

# `editor_close`

## LLM Compact

- status: active.
- purpose: 关闭编辑器；若仍有未处理脏资源则失败并返回清单
- key_inputs: `close_all_asset_editors`, `close_world_browser_tabs`, `request_exit`, `unload_late_shutdown_editor_modules`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 该命令不会替调用方做保存/丢弃判断。
- note: 若仍存在未处理脏资源，会返回失败 `editor_has_unresolved_dirty_resources`，并在 `data.dirty_resources` 中给出完整清单。
- note: 只有当脏资源全部处理完后，才会允许关闭编辑器。
- note: 返回 `closed_world_browser_tab_count`、`unloaded_late_shutdown_editor_module_count`、`unloaded_late_shutdown_editor_modules[]`，用于确认退出前清理是否执行。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 该命令不会替调用方做保存/丢弃判断。
- 若仍存在未处理脏资源，会返回失败 `editor_has_unresolved_dirty_resources`，并在 `data.dirty_resources` 中给出完整清单。
- 只有当脏资源全部处理完后，才会允许关闭编辑器。
- 返回 `closed_world_browser_tab_count`、`unloaded_late_shutdown_editor_module_count`、`unloaded_late_shutdown_editor_modules[]`，用于确认退出前清理是否执行。

## Parameters

- `close_all_asset_editors`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `close_world_browser_tabs`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `request_exit`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `unload_late_shutdown_editor_modules`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
