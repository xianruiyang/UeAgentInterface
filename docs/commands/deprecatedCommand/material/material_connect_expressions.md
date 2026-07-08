---
command: material_connect_expressions
category: material
title: 表达式图编辑
summary: **Deprecated for authoring**：连接节点到节点
aliases: []
tags: [material, deprecated]
status: deprecated
deprecated_scope: authoring
deprecated_reason: **Deprecated for authoring**：连接节点到节点
deprecated_replacement: []
---

# `material_connect_expressions`

## LLM Compact

- status: deprecated.
- purpose: **Deprecated for authoring**：连接节点到节点
- key_inputs: `asset_path`, `from_expression*`, `to_expression*`
- key_returns: `graphs/*.json`
- note: 生命周期：废弃指令只用于兼容、迁移、诊断或修复；新 authoring 不应选用。
- note: 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- validation: 确认迁移目标可覆盖当前用途；只有兼容/迁移/诊断/修复场景才继续使用，并记录原因。

## Authoring Notes

- 生命周期：废弃指令只用于兼容、迁移、诊断或修复；新 authoring 不应选用。
- 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `from_expression*`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `to_expression*`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `graphs/*.json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

确认迁移目标可覆盖当前用途；只有兼容/迁移/诊断/修复场景才继续使用，并记录原因。
