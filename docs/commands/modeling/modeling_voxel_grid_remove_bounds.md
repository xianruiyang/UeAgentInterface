---
command: modeling_voxel_grid_remove_bounds
category: modeling
title: 从体素网格 session 中剔除 bounds 覆盖到的 cell
summary: 从体素网格 session 中剔除 bounds 覆盖到的 cell
aliases: []
tags: [modeling]
status: active
---

# `modeling_voxel_grid_remove_bounds`

## LLM Compact

- status: active.
- purpose: 从体素网格 session 中剔除 bounds 覆盖到的 cell
- key_inputs: `bounds.center`, `extent`, `bounds.min`, `max`, `bounds.space`, `expected_revision`, `session_id`
- key_returns: `bounds.cell_range`, `changed_cell_count`, `occupied_cell_count`, `revision`
- note: 体素网格使用 session/revision；add/remove/apply 前后都要检查 `revision`，避免覆盖旧状态。
- note: 作用：从当前 session 剔除一个 bounds 覆盖到的体素。
- note: 参数与 `modeling_voxel_grid_add_bounds` 相同。
- validation: 检查 `session_id/grid_id/revision`；apply 后验证 mesh 计数、collision coverage、sidecar/anchor，必要时用空间感知或 sweep 验证。

## Authoring Notes

- 体素网格使用 session/revision；add/remove/apply 前后都要检查 `revision`，避免覆盖旧状态。
- 作用：从当前 session 剔除一个 bounds 覆盖到的体素。
- 参数与 `modeling_voxel_grid_add_bounds` 相同。

## Parameters

- `bounds.center/extent`：bounds 的中心和半尺寸。
- `bounds.min/max`：bounds 的最小/最大角。
- `bounds.space`：bounds 所在空间；写入前必须按命令文档确认，不能混用 world/local。
- `expected_revision`：乐观并发校验；revision 不匹配时应停止并重新读回。
- `session_id`：编辑 session id；后续 add/remove/apply 必须使用同一 session。

## Returns

- `bounds.cell_range`：本次体素 bounds 覆盖到的 cell 范围。
- `changed_cell_count`：本次新增/剔除的体素 cell 数。
- `occupied_cell_count`：当前 session 占用 cell 总数。
- `revision`：session 或资源 revision，用于后续并发保护。

## Validation

检查 `session_id/grid_id/revision`；apply 后验证 mesh 计数、collision coverage、sidecar/anchor，必要时用空间感知或 sweep 验证。
