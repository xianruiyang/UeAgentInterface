---
command: editor_viewport_set_preview_state
category: editor_viewport
title: 命令表
summary: 设置 adapter 专属预览状态
aliases: []
tags: [editor_viewport]
status: active
---

# `editor_viewport_set_preview_state`

## LLM Compact

- status: active.
- purpose: 设置 adapter 专属预览状态
- key_inputs: `preview_state`
- key_returns: `preview_state`, `preview_state_applied`
- note: 统一覆盖 Level viewport 与支持的资产预览窗口；target 的 `scope/asset_type/asset_path/viewport_role` 用于选择 adapter。
- note: 不同 adapter 的 camera/focus 使用各自 preview world，不得与 Level world 或资产局部空间混用。
- note: 设置结果也应被 `editor_viewport_screenshot` 以及 Niagara/Animation/Sequence 等序列帧输出消费。
- note: 用途：设置 adapter 专属预览状态。它与 camera/show flags 分离，避免把 Niagara 定帧、动画时间、材质预览体等状态混进 `visual_options`。
- validation: 读回 target/camera/visual_options/preview_state；截图检查 `file_path` 与 `pixel_signal.nonblank`，再结合目标状态验证。

## Authoring Notes

- 统一覆盖 Level viewport 与支持的资产预览窗口；target 的 `scope/asset_type/asset_path/viewport_role` 用于选择 adapter。
- 不同 adapter 的 camera/focus 使用各自 preview world，不得与 Level world 或资产局部空间混用。
- 设置结果也应被 `editor_viewport_screenshot` 以及 Niagara/Animation/Sequence 等序列帧输出消费。
- 用途：设置 adapter 专属预览状态。它与 camera/show flags 分离，避免把 Niagara 定帧、动画时间、材质预览体等状态混进 `visual_options`。
- 非空 `preview_state` 返回 `editor_viewport_preview_state_not_supported`。
- 只读 `preview_state` 会报告 asset path、LOD 和选择摘要。
- 只读 `preview_state` 会报告 asset、skeleton、preview mesh、animation / anim blueprint 路径、context、reference pose 和选择摘要。
- 只读 `preview_state.context="ControlRigBlueprint"`。
- 只读 `preview_state.preview_mesh_path`。
- ControlRig 专属运行、求解、控制器选择或 RigVM 调试状态不在当前 `editor_viewport_set_preview_state` 范围内。

## Parameters

- `preview_state`：adapter 专属预览状态；序列帧/截图应继承统一 preview_state 语义。

## Returns

- `preview_state`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `preview_state_applied`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读回 target/camera/visual_options/preview_state；截图检查 `file_path` 与 `pixel_signal.nonblank`，再结合目标状态验证。
