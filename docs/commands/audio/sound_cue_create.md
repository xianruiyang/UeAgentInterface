---
command: sound_cue_create
category: audio
title: SoundCue
summary: 创建空 SoundCue
aliases: []
tags: [audio]
status: active
---

# `sound_cue_create`

## LLM Compact

- status: active.
- purpose: 创建空 SoundCue
- key_inputs: `asset_path`, `replace_existing`, `save_after_create`
- key_returns: `asset_path`, `created_asset`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `replace_existing`：布尔/开关字段；默认值以本命令契约为准。
- `save_after_create`：布尔/开关字段；默认值以本命令契约为准。

## Returns

- `asset_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。
- `created_asset`：资产操作结果摘要。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
