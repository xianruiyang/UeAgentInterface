---
command: editor_prepare_exit
category: core_level
title: `editor_prepare_exit`
summary: 退出前按策略保存/丢弃并请求关闭编辑器
aliases: []
tags: [core_level]
status: active
---

# `editor_prepare_exit`

## LLM Compact

- status: active.
- purpose: 退出前按策略保存/丢弃并请求关闭编辑器
- key_inputs: `close_world_browser_tabs`, `discard_all_dirty`, `discard_asset_paths`, `request_exit`, `save_asset_paths`, `unload_late_shutdown_editor_modules`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 该命令用于编译、发布或自动化结束前的推荐关闭入口：先按策略保存/丢弃，再关闭资产编辑器、关闭 WorldBrowser tab、提前卸载晚卸载风险模块，最后延迟请求退出。
- note: 若清理后仍存在 dirty 资源，会返回失败 `editor_has_unresolved_dirty_resources`，不会请求退出。
- note: 返回 `remaining_dirty_resource_count`、`closed_world_browser_tab_count`、`unloaded_late_shutdown_editor_module_count`、`unloaded_late_shutdown_editor_modules[]`。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 该命令用于编译、发布或自动化结束前的推荐关闭入口：先按策略保存/丢弃，再关闭资产编辑器、关闭 WorldBrowser tab、提前卸载晚卸载风险模块，最后延迟请求退出。
- 若清理后仍存在 dirty 资源，会返回失败 `editor_has_unresolved_dirty_resources`，不会请求退出。
- 返回 `remaining_dirty_resource_count`、`closed_world_browser_tab_count`、`unloaded_late_shutdown_editor_module_count`、`unloaded_late_shutdown_editor_modules[]`。

## Parameters

- `close_world_browser_tabs`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `discard_all_dirty`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `discard_asset_paths`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `request_exit`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `save_asset_paths`：布尔/开关字段；默认值以本命令契约为准。
- `unload_late_shutdown_editor_modules`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
