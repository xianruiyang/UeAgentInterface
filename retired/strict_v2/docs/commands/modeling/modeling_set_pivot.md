---
command: modeling_set_pivot
category: modeling
title: `modeling_set_pivot`
summary: modeling_set_pivot command contract.
aliases: []
tags: [modeling]
status: active
---

# `modeling_set_pivot`

## LLM Compact

- status: active.
- purpose: modeling_set_pivot command contract.
- key_inputs: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 枢轴写入必须明确目标是资产还是场景组件；写后验证 pivot before/after，并确认 geometry/collision preserved。
- note: 传入 `preset` / `pivot_preset` / `custom_point` 等高层字段时，UAI 先做参数归一化，再调用合适后端。
- note: 未传高层字段时，保持兼容旧行为，仍作为 `BeginEditPivotTool` 的 raw wrapper，可继续传 `tool_properties`、`post_action` 和 `accept`。
- note: 触发条件：目标 Actor 解析到唯一 `UDynamicMeshComponent`，并传入 `preset` 或 `custom_point`。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 枢轴写入必须明确目标是资产还是场景组件；写后验证 pivot before/after，并确认 geometry/collision preserved。
- 传入 `preset` / `pivot_preset` / `custom_point` 等高层字段时，UAI 先做参数归一化，再调用合适后端。
- 未传高层字段时，保持兼容旧行为，仍作为 `BeginEditPivotTool` 的 raw wrapper，可继续传 `tool_properties`、`post_action` 和 `accept`。
- 触发条件：目标 Actor 解析到唯一 `UDynamicMeshComponent`，并传入 `preset` 或 `custom_point`。
- 空间语义：输入点为 component local 或 world；写入后通过平移 mesh 顶点并移动组件 world location 保持可见几何不动。
- 返回重点：`pivot_backend=dynamic_mesh_component`、`target`、`frame.bounds_space=component_local`、`pivot_point_component_local`、`pivot_before`、`pivot_after`、`geometry_preserved`、`world_bounds_origin_delta`、`world_bounds_extent_delta`、`collision_preserved`。
- 普通 `UDynamicMeshComponent` 使用当前 DynamicMesh 顶点平移后端；旧 static voxel session、
  sidecar anchor 和跨请求 grid map 已退出 active。`editor.session` 的 voxel policy 只持有
  operation-owned transient actor，不作为本 authoring command 的持久目标。

## Parameters

- 未在旧分册命令表中列出；以实现 schema/readback 为准。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
