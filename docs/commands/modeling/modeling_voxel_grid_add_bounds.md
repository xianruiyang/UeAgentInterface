---
command: modeling_voxel_grid_add_bounds
category: modeling
title: Add Voxel Bounds
summary: 把 bounds 覆盖到的体素加入当前 session。
aliases: []
tags: [modeling, voxel, whitebox]
status: active
---

# `modeling_voxel_grid_add_bounds`

## LLM Compact

- status: active.
- purpose: 把 bounds 覆盖到的体素加入当前 session。
- key_inputs: `session_id`, `expected_revision`, `bounds.space`, `bounds.min`, `bounds.max`, `bounds.center`, `bounds.extent`, `max_operation_cells` ...
- key_returns: `changed_cell_count`, `occupied_cell_count`, `revision`, `bounds.cell_range`
- note: 体素网格使用 session/revision；add/remove/apply 前后都要检查 `revision`，避免覆盖旧状态。
- note: 作用：把一个 bounds 覆盖到的体素加入当前 session。
- note: 离散化规则：`min` 使用 `floor(min / cell_size)`，`max` 使用 `ceil(max / cell_size)`，语义是 cover 而不是只取中心点。
- validation: 检查 `session_id/grid_id/revision`；apply 后验证 mesh 计数、collision coverage、sidecar/anchor，必要时用空间感知或 sweep 验证。

## Authoring Notes

- 体素网格使用 session/revision；add/remove/apply 前后都要检查 `revision`，避免覆盖旧状态。
- 作用：把一个 bounds 覆盖到的体素加入当前 session。
- 离散化规则：`min` 使用 `floor(min / cell_size)`，`max` 使用 `ceil(max / cell_size)`，语义是 cover 而不是只取中心点。

## Parameters

- `session_id`：必填。
- `expected_revision`：可选，防止过期写入。
- `bounds.space`：`grid` / `world` / `component_local` 等；推荐 `grid` 或 `world`。
- `bounds.min` + `bounds.max`，或 `bounds.center` + `bounds.extent`。
- `max_operation_cells`：默认 `200000`。
- `bounds.center/extent`：bounds 的中心和半尺寸。
- `bounds.min/max`：bounds 的最小/最大角。

## Returns

- `changed_cell_count`。
- `occupied_cell_count`。
- `revision`。
- `bounds.cell_range`。

## Minimal Example

```json
{
  "command": "modeling_voxel_grid_add_bounds",
  "params": {
    "session_id": "VG_SESSION_xxx",
    "bounds": {
      "space": "grid",
      "min": [0, 0, 0],
      "max": [600, 400, 100]
    }
  }
}
```

## Validation

每次 add 只改变 session 占用集，不代表最终 mesh/collision 已更新；区域主体完成后必须调用 `modeling_voxel_grid_apply`。
