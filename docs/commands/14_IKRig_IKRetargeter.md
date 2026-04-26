# 指令详解：IK Rig / IK Retargeter

## IK Rig

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `ik_rig_create` | 创建 IK Rig 资产 | `asset_path`；可选 `preview_skeletal_mesh`、`apply_auto_retarget_definition`、`save_after_create` |
| `ik_rig_get_info` | 读取 IK Rig 摘要 | `asset_path` |
| `ik_rig_set_solver` | 新增 / 更新 / 删除 solver | `asset_path`；新增时传 `solver_type`，更新/删除时传 `solver_index`；可选 `enabled`、`start_bone_name`、`end_bone_name`、`move_to_index`、`connect_goal_names[]`、`disconnect_goal_names[]`、`settings{...}`、`goal_settings[]`、`bone_settings[]`、`remove`、`save_after_set` |
| `ik_rig_set_preview_mesh` | 设置或清空 IK Rig preview mesh | `asset_path`、`skeletal_mesh_path` 或 `clear_preview_mesh`、`save_after_set` |
| `ik_rig_set_goal` | 新增 / 更新 / 删除 goal | `asset_path`、`goal_name`；新增/更新时传 `bone_name`；可选 `new_goal_name`、`position_alpha`、`rotation_alpha`、`position`、`rotation`、`current_transform{location/rotation/scale}`、`remove`、`save_after_set` |
| `ik_rig_set_retarget_root` | 设置 retarget root | `asset_path`、`root_bone_name`、`save_after_set` |
| `ik_rig_set_retarget_chain` | 新增 / 更新 / 删除 retarget chain | `asset_path`、`chain_name`；可选 `start_bone_name`、`end_bone_name`、`goal_name`、`new_chain_name`、`remove` |
| `ik_rig_apply_auto_retarget_definition` | 对匹配模板的骨架自动生成 retarget definition | `asset_path`、`save_after_set` |

### `ik_rig_get_info`

当前返回：
- `asset_path`
- `object_path`
- `preview_skeletal_mesh`
- `retarget_root`
- `goal_count`
- `goals[]`
- `retarget_chain_count`
- `retarget_chains[]`
- `solver_count`
- `solvers[]`

说明：
- 这一层当前重点是 IK Rig 资产结构、goal 和 retarget definition 管理，不是完整 solver 参数编辑器替代。
- `solvers[]` 当前除了基础结构，还会按 solver 类型回读：
  - `solver_kind`
  - `settings`
  - `goal_setting_count / goal_settings[]`
  - `bone_setting_count / bone_settings[]`（当前主要是 `FBIK`）

### `ik_rig_set_solver`

说明：
- 这是当前 solver 层的统一入口，避免继续拆 `add_solver / remove_solver / set_solver_enabled / set_solver_start_bone` 多条命令。
- 新增 solver 时传 `solver_type`，例如：
  - `/Script/IKRig.IKRigBodyMoverSolver`
  - `/Script/IKRig.IKRigFullBodyIKSolver`
- 当前优先支持：
  - 新增 solver
  - 删除 solver
  - 启用/禁用
  - `start_bone_name`
  - `end_bone_name`
  - 简单重排 `move_to_index`
  - `connect_goal_names[] / disconnect_goal_names[]`
- 当前已稳定开放的细粒度能力面：
  - `BodyMover`
    - `settings{...}`
    - `goal_settings[]{ influence_multiplier }`
  - `FBIK`
    - `settings{ iterations / sub_iterations / mass_multiplier / allow_stretch / root_behavior / global_pull_chain_alpha / max_angle / over_relaxation }`
    - `goal_settings[]{ chain_depth / strength_alpha / pull_chain_alpha / pin_rotation }`
    - `bone_settings[]{ rotation_stiffness / position_stiffness / use_preferred_angles / preferred_angles }`
- 其它 solver 仍未继续开放通用 settings struct 编辑。

### `ik_rig_set_goal`

说明：
- 这是 goal 生命周期与当前 goal 数据的统一入口。
- 新增/更新时 `bone_name` 仍是必填；如需改名可同时传 `new_goal_name`。
- 当前已稳定支持：
  - `position_alpha`
  - `rotation_alpha`
  - 顶层 `position / rotation`
  - `current_transform{location/rotation/scale}`
- `current_transform` 当前按“即时写入 + 即时回读 + `get_info` 摘要可见”验收，不承诺后续 rig 重初始化后的绝对值恒定不变。

## IK Retargeter

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `ik_retargeter_create` | 创建 IK Retargeter 资产 | `asset_path`；可选 `source_ik_rig`、`target_ik_rig`、`source_preview_mesh`、`target_preview_mesh`、`add_default_ops`、`save_after_create` |
| `ik_retargeter_get_info` | 读取 IK Retargeter 摘要 | `asset_path` |
| `ik_retargeter_set_ik_rig` | 设置 source / target IK Rig | `asset_path`、`source_or_target`、`ik_rig_path`；可选 `clear_ik_rig`、`save_after_set` |
| `ik_retargeter_set_settings` | 统一设置 retargeter global / root / chain settings | `asset_path`；可选 `global_settings{...}`、`root_settings{...}`、`target_chain_name`、`chain_settings{...}`、`reset_chain_to_default`、`save_after_set` |
| `ik_retargeter_set_pose` | 统一管理 retarget pose 生命周期与当前 pose 数据 | `asset_path`、`source_or_target`；可选 `pose_name`、`create_if_missing`、`set_current`、`duplicate_from_pose`、`rename_from_pose`、`remove`、`reset_all`、`reset_bones[]`、`root_offset`、`bone_rotation_offsets[]`、`auto_align_all`、`auto_align_bones[]`、`auto_align_method`、`snap_bone_to_ground`、`save_after_set` |
| `ik_retargeter_set_preview_mesh` | 设置 source / target preview mesh | `asset_path`、`source_or_target`、`skeletal_mesh_path`；可选 `clear_preview_mesh`、`save_after_set` |
| `ik_retargeter_auto_map_chains` | 对 retargeter op stack 自动做链映射 | `asset_path`；可选 `auto_map_type=exact/fuzzy/clear`、`force_remap`、`op_name`、`save_after_set` |
| `ik_retargeter_duplicate_and_retarget` | 批量复制并重定向动画资产 | `asset_path`、`asset_paths[]`、`output_folder`；可选 `source_mesh_path`、`target_mesh_path`、`prefix`、`suffix`、`search`、`replace`、`include_referenced_assets` |

### `ik_retargeter_create`

说明：
- 默认会补 `default ops`，这是为了让 `auto_map_chains` 和后续 retarget 流程有稳定落点。
- 如果只想建一个空 retargeter，可传 `add_default_ops=false`。

### `ik_retargeter_get_info`

当前返回：
- `source_ik_rig`
- `target_ik_rig`
- `source_preview_mesh`
- `target_preview_mesh`
- `source_retarget_pose`
- `target_retarget_pose`
- `source_pose_names[]`
- `target_pose_names[]`
- `current_source_pose`
- `current_target_pose`
- `retarget_op_count`
- `root_settings`
- `global_settings`
- `chain_mapping_ready`
- `chain_mapping_count`
- `chain_mappings[]`
- `chain_settings_count`
- `chain_settings[]`

说明：
- 由于 UE 5.6 的 chain mapping 依附 op stack，`chain_mappings[]` 当前按可解析到的 op 聚合输出，主要用于调试与结构确认。
- 在默认 op 配置下，`chain_mappings[]` 更适合作“最佳努力的诊断字段”，不应把它当成 retarget 是否可执行的唯一依据；真正的可执行性应结合 `duplicate_and_retarget` 结果验证。
- `root_settings / global_settings` 当前已经进入稳定能力面；`chain_settings[]` 已接入摘要回读，但仍更适合作诊断字段。

### `ik_retargeter_set_settings`

说明：
- 这是当前 retargeter settings 的统一入口，避免继续拆 `set_global_settings / set_root_settings / set_chain_settings / reset_chain_settings` 多条命令。
- `global_settings{...}` 与 `root_settings{...}` 当前是稳定能力面。
- `chain_settings{...}` 与 `reset_chain_to_default` 需要同时提供 `target_chain_name`。
- 当前实现完成后会直接回落到 `ik_retargeter_get_info` 的摘要返回，便于立即核对设置结果。

### `ik_retargeter_set_pose`

说明：
- 这是 retarget pose 的统一入口，避免继续拆 `create_pose / duplicate_pose / rename_pose / set_current_pose / set_pose_root_offset / set_pose_bone_rotation` 多条命令。
- 推荐工作流：
  1. `pose_name + create_if_missing=true` 创建或确保 pose 存在
  2. `set_current=true` 切到目标 pose
  3. 再写 `root_offset`、`bone_rotation_offsets[]`、`auto_align_*`
- `duplicate_from_pose` 和 `rename_from_pose` 都是对 `pose_name` 的写入型操作。
- `reset_all` 和 `reset_bones[]` 走的是 controller 的 pose reset 流程。
- `root_offset` 当前回读的是 retarget pose 上的 root translation delta；是否对运行时结果可见，还要结合具体 rig/op 配置验证。

推荐参数形式：

```json
{
  "asset_path": "/Game/IK/RTG_PlayerToEnemy",
  "source_or_target": "target",
  "pose_name": "EnemyPoseA",
  "create_if_missing": true,
  "set_current": true,
  "bone_rotation_offsets": [
    {
      "bone_name": "pelvis",
      "rotation_offset": { "pitch": 5.0, "yaw": 10.0, "roll": 15.0 }
    }
  ],
  "save_after_set": false
}
```

### `ik_retargeter_duplicate_and_retarget`

推荐参数形式：

```json
{
  "asset_path": "/Game/IK/RTG_PlayerToEnemy",
  "asset_paths": [
    "/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle"
  ],
  "source_mesh_path": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP",
  "target_mesh_path": "/Game/Test/TutorialTPP_Target",
  "output_folder": "/Game/Test/Retargeted",
  "prefix": "RTG_",
  "include_referenced_assets": false
}
```

说明：
- 这条命令当前走 UE 5.6 的 batch retarget 流程，适合“复制一批动画资产并落到目标目录”。
- `output_folder` 必填，便于稳定追踪这次 retarget 的新资产。
- `source_mesh_path / target_mesh_path` 不传时，会回退到 retargeter 里当前的 source / target preview mesh。

## 当前边界

- `IK Rig` 当前已经支持 `BodyMover / FBIK` 的一部分 solver 细粒度属性、goal settings 和 bone settings，但还不是全 solver / 全约束编辑器替代。
- `IK Retargeter` 当前已经覆盖主流程、pose 生命周期与 pose 偏移数据管理，但不继续扩到完整 op 级细节编辑。
- 对 op 级 chain settings / 单链显式 mapping / 每个 op 的专用 settings，当前仍以稳定性优先；`global_settings/root_settings` 已稳定，`chain_settings[]` 仍主要作为诊断层能力。

## 2026-04-21 增量更新

新增命令：

- `ik_retargeter_set_settings`

当前统一入口支持：

- `global_settings{...}`
- `root_settings{...}`
- `target_chain_name + chain_settings{...}`
- `reset_chain_to_default`

`ik_retargeter_get_info` 新增回读：

- `root_settings`
- `global_settings`
- `chain_settings_count`
- `chain_settings[]`

当前稳定结论：

- `global_settings/root_settings` 已完成 smoke 与 live 验证。
- `chain_settings[]` 已接入摘要回读，但在 UE 5.6 当前最小 retargeter 资产流实测里经常是空数组，暂不应视为当前稳定能力面。

## 2026-04-21 增量更新（Stage29）

- `ik_rig_set_solver` 现在支持：
  - `connect_goal_names[]`
  - `disconnect_goal_names[]`
  - `settings{...}`
  - `goal_settings[]`
- `ik_rig_get_info.solvers[]` 现在会回读：
  - `solver_kind`
  - `settings`
  - `goal_setting_count`
  - `goal_settings[]`

当前已稳定验收的 solver 细粒度能力面：

- `BodyMover` solver settings
- `BodyMover` goal 连接
- `BodyMover` `InfluenceMultiplier`

当前仍未扩到其他 solver 的细粒度 settings。

## 2026-04-21 增量更新（Stage30）

- `ik_rig_set_goal` 现在支持：
  - `position_alpha`
  - `rotation_alpha`
- 返回已补：
  - `bone_name`
  - `position_alpha`
  - `rotation_alpha`

当前已稳定验收：

- `goal` 的目标骨骼写入
- `goal` 的 `PositionAlpha`
- `goal` 的 `RotationAlpha`

## 2026-04-21 增量更新（Stage31）

- `ik_rig_set_goal` 现在支持：
  - `position`
  - `rotation`
  - `current_transform{location/rotation/scale}`
- 返回已补：
  - `current_location`
  - `current_rotation`
  - `current_scale`
  - `current_transform`

当前稳定验收口径：

- `current transform` 的立即回读
- `ik_rig_get_info.goals[]` 摘要字段存在

说明：

- 这层当前不按“后续所有 rig 重初始化之后仍保持绝对值不变”来验收。
- 当前更合理的稳定口径是“命令即时写入 + 即时回读 + `get_info` 摘要可见”。

## 2026-04-21 增量更新（Stage32）

- `ik_rig_set_solver` 现在也稳定支持 `FBIK` 的基础细粒度设置：
  - `settings{ iterations / sub_iterations / mass_multiplier / allow_stretch / root_behavior / global_pull_chain_alpha / max_angle / over_relaxation }`
  - `goal_settings[]{ chain_depth / strength_alpha / pull_chain_alpha / pin_rotation }`
  - `bone_settings[]{ rotation_stiffness / position_stiffness / use_preferred_angles / preferred_angles }`
- `ik_rig_get_info.solvers[]` 已补 `FBIK` 的 `settings / goal_settings / bone_settings` 摘要回读。
