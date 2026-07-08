---
command: material_create
category: material
title: 资产与编译
summary: 创建 `UMaterial`
aliases: []
tags: [material]
status: active
---

# `material_create`

## LLM Compact

- status: active.
- purpose: 创建 `UMaterial`
- key_inputs: `asset_path`
- key_returns: `compile_after_create`, `save_after_create`
- note: 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。

## Returns

- `compile_after_create`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `save_after_create`：布尔/开关字段；默认值以本命令契约为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
