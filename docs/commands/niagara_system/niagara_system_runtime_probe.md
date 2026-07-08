---
command: niagara_system_runtime_probe
category: niagara_system
title: `niagara_system_runtime_probe`
summary: 读取 System 预览组件运行统计
aliases: []
tags: [niagara_system]
status: active
---

# `niagara_system_runtime_probe`

## LLM Compact

- status: active.
- purpose: 读取 System 预览组件运行统计
- key_inputs: `asset_path`, `advance_mode`, `tick_count`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- note: 读取结果时不要只看最终快照。对 Collision Event、Death Event、短 lifetime 水花/火花这类瞬时粒子，最终帧可能已经归零，应优先看 `summary.emitter_peaks[]`。
- note: 推荐新流程是 `niagara_preview_advance` 推进一次并暂停，然后用 `sample_mode=current_preview` 只读；不要让 screenshot 和 probe 分别重复推进同一帧。
- note: Ray Trace Collision / Event Handler 场景依赖预览世界和碰撞上下文。若重置后的 probe 与界面观察不一致，先对比 `component_source`、`component_world`、`reset_preview`、`advance_mode`，再判断资产是否有问题。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 新增节点/模块/expression 前先查询 catalog/seed；写入后必须编译或读回关键 pin/input 状态。
- 读取结果时不要只看最终快照。对 Collision Event、Death Event、短 lifetime 水花/火花这类瞬时粒子，最终帧可能已经归零，应优先看 `summary.emitter_peaks[]`。
- 推荐新流程是 `niagara_preview_advance` 推进一次并暂停，然后用 `sample_mode=current_preview` 只读；不要让 screenshot 和 probe 分别重复推进同一帧。
- Ray Trace Collision / Event Handler 场景依赖预览世界和碰撞上下文。若重置后的 probe 与界面观察不一致，先对比 `component_source`、`component_world`、`reset_preview`、`advance_mode`，再判断资产是否有问题。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `advance_mode`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `tick_count`：数量摘要。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
