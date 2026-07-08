---
command: modeling_voxel_grid_apply
category: modeling
title: Apply Voxel Grid
summary: 把 session 内占用体素生成到 DynamicMeshComponent，并生成贴合占用体素的 box collision。
aliases: []
tags: [modeling, voxel, collision, whitebox]
status: active
---

# `modeling_voxel_grid_apply`

## LLM Compact

- status: active.
- purpose: 把 session 内占用体素生成到 DynamicMeshComponent，并生成贴合占用体素的 box collision。
- key_inputs: `session_id`, `expected_revision`, `mesh_build`, `collision.mode`, `collision.max_boxes`, `collision.profile_name`, `keep_session`
- key_returns: `output_type`, `mesh.*`, `collision.box_count`, `collision.coverage_matches_occupied_cells`, `metadata.sidecar_path`, `metadata.anchor`
- note: 体素网格使用 session/revision；add/remove/apply 前后都要检查 `revision`，避免覆盖旧状态。
- note: 默认生成暴露面 DynamicMeshActor 和 exact-cover 合并 box collision；默认材质应沿用 CubeGrid component default，不自行创建 checker 材质。
- note: 作用：把 session 内占用体素生成到 `DynamicMeshComponent`，并生成可碰撞的 box 组件。
- note: 无显式材质且组件当前只有默认/旧默认材质时，默认清空 `DynamicMeshComponent.BaseMaterials[0]`，与真实 CubeGrid 输出的 `(None)` 材质 slot 保持一致；UAI 不创建材质、不创建材质实例、不自行模拟 checker 参数。
- validation: 检查 `session_id/grid_id/revision`；apply 后验证 mesh 计数、collision coverage、sidecar/anchor，必要时用空间感知或 sweep 验证。

## Authoring Notes

- 体素网格使用 session/revision；add/remove/apply 前后都要检查 `revision`，避免覆盖旧状态。
- 默认生成暴露面 DynamicMeshActor 和 exact-cover 合并 box collision；默认材质应沿用 CubeGrid component default，不自行创建 checker 材质。
- 作用：把 session 内占用体素生成到 `DynamicMeshComponent`，并生成可碰撞的 box 组件。
- 无显式材质且组件当前只有默认/旧默认材质时，默认清空 `DynamicMeshComponent.BaseMaterials[0]`，与真实 CubeGrid 输出的 `(None)` 材质 slot 保持一致；UAI 不创建材质、不创建材质实例、不自行模拟 checker 参数。
- 旧版 UAI 体素默认写入的 `/MeshModelingToolsetExp/Materials/SimpleBaseMaterial`，以及曾被误用为默认的 `/MeshModelingToolsetExp/Materials/InProgressMaterial`、`/MeshModelingToolsetExp/Materials/CheckerMaterial`，都会被视为可迁移默认值；再次 apply 且未显式传材质时会清空为 CubeGrid component default。
- 编辑已有体素 actor 且组件已有真实自定义材质时，默认保留现有材质；需要强制替换时传 `material_path`。
- 默认碰撞不是一个包住整体的 actor/world/mesh AABB。
- 默认碰撞是对占用 cell 做 exact-cover 合并后的多个 `UBoxComponent`。
- 只有所有占用 cell 本身构成完整长方体时，结果才可能退化为 1 个 box。
- 返回 `collision.coverage_matches_occupied_cells=true` 时，表示生成 box 覆盖 cell 数与当前占用 cell 数一致。

## Parameters

- `session_id`：必填。
- `expected_revision`：可选。
- `mesh_build`：当前支持 `exposed_faces`。
- `collision.mode`：`merged_box_components` / `exact_box_components` / `none`，默认 `merged_box_components`。
- `collision.max_boxes`：默认 `2048`。
- `collision.profile_name`：常用 `BlockAll`。
- `keep_session`：是否保留 session。

## Returns

- `output_type`。
- `mesh.*`：mesh 生成摘要。
- `collision.box_count`。
- `collision.coverage_matches_occupied_cells`。
- `metadata.sidecar_path`。
- `metadata.anchor`。

## Minimal Example

```json
{
  "command": "modeling_voxel_grid_apply",
  "params": {
    "session_id": "VG_SESSION_xxx",
    "collision": {
      "mode": "merged_box_components",
      "max_boxes": 2048,
      "profile_name": "BlockAll"
    }
  }
}
```

## Validation

apply 后用 `level_get_nearby_mesh_shapes` 确认体素 collision shape 可被空间感知读取；可玩连接还要用 `level_sweep_capsule_path` 验证通行。
