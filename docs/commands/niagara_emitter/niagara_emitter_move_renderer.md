---
command: niagara_emitter_move_renderer
category: niagara_emitter
title: niagara_emitter_move_renderer
summary: **原子/兼容**：编辑 Niagara System/Emitter 的 emitter、renderer、event handler、parameter 或属性
aliases: []
tags: [niagara_emitter]
status: active
---

# `niagara_emitter_move_renderer`

## LLM Compact

- status: active.
- purpose: **原子/兼容**：编辑 Niagara System/Emitter 的 emitter、renderer、event handler、parameter 或属性
- key_inputs: `asset_path`, `emitter_name`, `input_name`, `module_name`, `value_text`
- key_returns: `errors`, `success`, `warnings`
- note: 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `emitter_name`：名称字段；同名对象存在时应结合路径或 id 消歧。
- `input_name`：名称字段；同名对象存在时应结合路径或 id 消歧。
- `module_name`：名称字段；同名对象存在时应结合路径或 id 消歧。
- `value_text`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `errors`：错误摘要。
- `success`：命令业务是否成功；仍需按返回字段读回验收。
- `warnings`：警告摘要。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
