---
command: asset_import_fbx_animation
category: core_level
title: `asset_import_fbx_animation`
summary: 导入 FBX 为 AnimSequence
aliases: []
tags: [core_level]
status: active
---

# `asset_import_fbx_animation`

## LLM Compact

- status: active.
- purpose: 导入 FBX 为 AnimSequence
- key_inputs: `destination_path`, `skeletal_mesh_path`, `skeleton_path`, `source_filename`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 若只给 `skeletal_mesh_path`，命令会自动解析其绑定 Skeleton。
- note: 导入结果是 `AnimSequence` 资产。
- note: 返回 `skeleton_path` 与 `imported_animation_paths`，便于后续直接接 `AnimBlueprint` / Montage / 结构化动画工作流。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 若只给 `skeletal_mesh_path`，命令会自动解析其绑定 Skeleton。
- 导入结果是 `AnimSequence` 资产。
- 返回 `skeleton_path` 与 `imported_animation_paths`，便于后续直接接 `AnimBlueprint` / Montage / 结构化动画工作流。

## Parameters

- `destination_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。
- `skeletal_mesh_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。
- `skeleton_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。
- `source_filename`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
