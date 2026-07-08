---
command: niagara_screenshot
category: niagara_system
title: `niagara_screenshot`
summary: 截取 Niagara 资产编辑器界面，支持从 0 连续推进的序列帧总览图
aliases: []
tags: [niagara_system]
status: active
---

# `niagara_screenshot`

## LLM Compact

- status: active.
- purpose: 截取 Niagara 资产编辑器界面，支持从 0 连续推进的序列帧总览图
- key_inputs: `asset_path`, `format`, `max_size`, `open_editor_if_needed`, `preview_state`, `target`, `visual_options`, `camera_distance_scale` ...
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 序列帧、预览截图和资产预览应沿用统一 `visual_options` 与 `preview_state` 语义。
- note: 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- note: 用于打开或复用 Niagara System / Emitter / Script 编辑器，并把编辑器界面或预览画面截图写入 UAI artifacts `Shots` 目录。该命令不运行 PIE / game。
- note: 序列帧模式支持统一 `visual_options` / `preview_state` 嵌套对象：`visual_options` 可填充 `target`、`format`、`quality`、`max_size`、`width`、`height`、frame sheet 输出设置、`frame_capture_renderer` / `renderer`、`camera_fov`、`camera_distance`、`camera_distance_scale`、`camera_target_z_offset`、`camera_height_offset`、`camera_side_offset`、`wait_for_render_dependencies` 和 warmup capture 设置；`preview_state` 可填充 `target_frame` / `frame_index` / `frame` / `pause_at_frame`、`start_frame_index`、`count`、`frame_interval`、`frames[]`、`tick_delta_seconds` / `preview_tick_delta_seconds`、`advance_mode`、`restore_interactive_preview`、`reset`、`pause`。嵌套对象只填充缺失的顶层字段；若顶层字段已存在则顶层字段胜出，并在返回 `unified_sequence_frame_options.conflicts` 中记录。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 序列帧、预览截图和资产预览应沿用统一 `visual_options` 与 `preview_state` 语义。
- 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- 用于打开或复用 Niagara System / Emitter / Script 编辑器，并把编辑器界面或预览画面截图写入 UAI artifacts `Shots` 目录。该命令不运行 PIE / game。
- 序列帧模式支持统一 `visual_options` / `preview_state` 嵌套对象：`visual_options` 可填充 `target`、`format`、`quality`、`max_size`、`width`、`height`、frame sheet 输出设置、`frame_capture_renderer` / `renderer`、`camera_fov`、`camera_distance`、`camera_distance_scale`、`camera_target_z_offset`、`camera_height_offset`、`camera_side_offset`、`wait_for_render_dependencies` 和 warmup capture 设置；`preview_state` 可填充 `target_frame` / `frame_index` / `frame` / `pause_at_frame`、`start_frame_index`、`count`、`frame_interval`、`frames[]`、`tick_delta_seconds` / `preview_tick_delta_seconds`、`advance_mode`、`restore_interactive_preview`、`reset`、`pause`。嵌套对象只填充缺失的顶层字段；若顶层字段已存在则顶层字段胜出，并在返回 `unified_sequence_frame_options.conflicts` 中记录。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `format`：输出格式，例如 png/json/csv，取决于命令支持。
- `max_size`：截图或导出尺寸上限。
- `open_editor_if_needed`：资产预览命令是否允许打开编辑器；高风险 adapter 可能拒绝自动打开。
- `preview_state`：adapter 专属预览状态；序列帧/截图应继承统一 preview_state 语义。
- `target`：统一目标对象；用于指定 level viewport、asset editor、资产或组件。
- `visual_options`：视图模式/overlay/show flags；截图和序列帧也应消费该对象。
- `camera_distance_scale`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `camera_fov`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `count`：数量摘要。
- `create_frame_sheet`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `frame_capture_renderer`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `frame_interval`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `frame_sheet_label_scale`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `frame_sheet_max_size`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `frame_sheet_padding`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `preview_tick_delta_seconds`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `start_frame_index`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `target_frame`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `write_individual_frames`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
