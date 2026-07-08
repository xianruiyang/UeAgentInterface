---
command: blueprint_screenshot
category: blueprint
title: 蓝图窗口截图（新增）
summary: 对蓝图编辑器窗口/视口/图面板截图
aliases: []
tags: [blueprint]
status: active
---

# `blueprint_screenshot`

## LLM Compact

- status: active.
- purpose: 对蓝图编辑器窗口/视口/图面板截图
- key_inputs: `asset_path`, `format`, `max_size`, `open_editor_if_needed`, `target`, `quality`, `allow_ui_mutation`, `allow_window_mutation` ...
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `format`：输出格式，例如 png/json/csv，取决于命令支持。
- `max_size`：截图或导出尺寸上限。
- `open_editor_if_needed`：资产预览命令是否允许打开编辑器；高风险 adapter 可能拒绝自动打开。
- `target`：统一目标对象；用于指定 level viewport、asset editor、资产或组件。
- `quality`：有损图片输出质量；通常用于 jpg/webp。
- `allow_ui_mutation`：布尔/开关字段；默认值以本命令契约为准。
- `allow_window_mutation`：布尔/开关字段；默认值以本命令契约为准。
- `capture_height`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `capture_width`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `graph_name`：名称字段；同名对象存在时应结合路径或 id 消歧。
- `restore_graph_view_after_capture`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
