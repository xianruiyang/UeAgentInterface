---
command: niagara_system_list_module_inputs
category: niagara_stage_graph
title: 读取 System stage module 的真实 input
summary: 读取 System stage module 的真实 input
aliases: []
tags: [niagara_stage_graph]
status: active
---

# `niagara_system_list_module_inputs`

## LLM Compact

- status: active.
- purpose: 读取 System stage module 的真实 input
- key_inputs: `asset_path`, `module_node_guid`
- key_returns: `inputs`, `module_node_guid`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `module_node_guid`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `inputs`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `module_node_guid`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
