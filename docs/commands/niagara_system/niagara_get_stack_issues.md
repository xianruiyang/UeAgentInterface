---
command: niagara_get_stack_issues
category: niagara_system
title: `niagara_get_stack_issues`
summary: 读取 Niagara Stack 红/黄/信息提示
aliases: []
tags: [niagara_system]
status: active
---

# `niagara_get_stack_issues`

## LLM Compact

- status: active.
- purpose: 读取 Niagara Stack 红/黄/信息提示
- key_inputs: `asset_path`, `open_editor_if_needed`, `severity_filter`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- note: 编译字段：`compile_before_read`、`force_compile`、`wait_for_complete`、`compilation_requested`、`compilation_complete`、`has_outstanding_compilation_requests`
- note: Emitter scope 额外包含 `emitter_latest_compile_status`、`emitter_handle_error_text`、`emitter_handle_error_color`、`emitter_handle_error_visibility`，用于区分 Stack issue 图标和 emitter 编译/状态提示
- note: System scope 额外包含 `system_latest_compile_status`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- 编译字段：`compile_before_read`、`force_compile`、`wait_for_complete`、`compilation_requested`、`compilation_complete`、`has_outstanding_compilation_requests`
- Emitter scope 额外包含 `emitter_latest_compile_status`、`emitter_handle_error_text`、`emitter_handle_error_color`、`emitter_handle_error_visibility`，用于区分 Stack issue 图标和 emitter 编译/状态提示
- System scope 额外包含 `system_latest_compile_status`
- 要尽量匹配 UI 里鼠标悬停看到的红色感叹号文本，建议使用 `prefer_existing_view_model=true` 且 `open_editor_if_needed=true`。
- 未打开编辑器时命令会退回 data-processing ViewModel；这能读取部分 issue，但可能漏掉 UI-only / live Stack 状态。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `open_editor_if_needed`：资产预览命令是否允许打开编辑器；高风险 adapter 可能拒绝自动打开。
- `severity_filter`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
