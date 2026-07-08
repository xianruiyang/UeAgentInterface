---
command: modeling_voxel_grid_begin
category: modeling
title: Begin Editable Voxel Grid
summary: 开始创建或编辑一个可编辑体素网格 session。
aliases: []
tags: [modeling, voxel, whitebox]
status: active
---

# `modeling_voxel_grid_begin`

## LLM Compact

- status: active.
- purpose: 开始创建或编辑一个可编辑体素网格 session。
- key_inputs: `mode`, `session_id`, `grid_id`, `actor_id`, `label`, `folder_path`, `grid.cell_size`, `grid.origin` ...
- key_returns: `session_id`, `grid_id`, `actor_id`, `output_type`, `revision`, `sidecar_path`, `anchor`
- note: 体素网格使用 session/revision；add/remove/apply 前后都要检查 `revision`，避免覆盖旧状态。
- note: 作用：开始创建或编辑一个体素网格 session。
- note: 创建模式下 Actor object name 会在目标 Level outer 下生成唯一名；重复 `grid_id`、旧同名 Actor 或刚删除的 pending-kill 残留不得导致 UE `SpawnActor` assert 崩溃。稳定身份以 `grid_id`、Actor tag 和返回的 `actor_id` 为准，不以固定 object name 为准。
- validation: 检查 `session_id/grid_id/revision`；apply 后验证 mesh 计数、collision coverage、sidecar/anchor，必要时用空间感知或 sweep 验证。

## Authoring Notes

- 体素网格使用 session/revision；add/remove/apply 前后都要检查 `revision`，避免覆盖旧状态。
- 作用：开始创建或编辑一个体素网格 session。
- 创建模式下 Actor object name 会在目标 Level outer 下生成唯一名；重复 `grid_id`、旧同名 Actor 或刚删除的 pending-kill 残留不得导致 UE `SpawnActor` assert 崩溃。稳定身份以 `grid_id`、Actor tag 和返回的 `actor_id` 为准，不以固定 object name 为准。

## Parameters

- `mode`：`create` / `edit`，默认 `create`。
- `session_id`：可选，不传自动生成。
- `grid_id`：可选；新建自动生成，编辑可从 Actor tag 读取。
- `actor_id`：编辑已有体素 Actor 时必填。
- `label`：Actor label。
- `folder_path`：Outliner folder。
- `grid.cell_size`：格子尺寸 cm。
- `grid.origin` / `grid.rotation`：grid 到 world 的稳定变换。
- `anchor`：可选 pivot/anchor 设置。

## Returns

- `session_id` / `grid_id` / `actor_id`。
- `output_type`。
- `revision`。
- `sidecar_path`。
- `anchor`。

## Minimal Example

```json
{
  "command": "modeling_voxel_grid_begin",
  "params": {
    "mode": "create",
    "label": "WB_A01_MainBody",
    "folder_path": "UAI/WB_WhiteBox01/Homestay/A01",
    "grid": {
      "cell_size": 100,
      "origin": [0, 0, 0],
      "rotation": { "pitch": 0, "yaw": 0, "roll": 0 }
    }
  }
}
```

## Validation

begin 后只说明 session 建立成功；必须继续 add/remove/apply 并读回 actor、collision 和 sidecar 才算白盒主体生成完成。
