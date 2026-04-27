# 指令详解：Animation Assets / Skeleton

> 废弃写入命令已迁移到 `deprecatedCommand/13_AnimationAssets_Skeleton.md`；本分册只保留主流程、读取、导出/应用、编译、诊断，以及尚未被 JSON / 结构化 JSON 覆盖的命令。

## Animation Sequence

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `anim_sequence_get_info` | 读取 AnimSequence 资产摘要 | `asset_path`；可选 `include_curve_keys` |
| `anim_sequence_screenshot` | 按帧或时间截取 AnimSequence 预览图 | `asset_path`；可选 `skeletal_mesh_path`、`frame_index` 或 `time_seconds`、`format`、`quality`、`max_size`、`target=viewport` |
| `anim_sequence_set_curve` | 统一管理 AnimSequence 曲线与 key | `asset_path`、`curve_name`；可选 `curve_type=float`、`curve_json`、`keys[]`、`time_seconds/value`、`clear_existing_keys`、`remove`、`meta_data_curve`、`remove_name_from_skeleton`、`save_after_set` |
| `anim_sequence_set_bones` | 统一管理骨骼动画轨删除 | `asset_path`；可选 `remove_bone_names[]`、`include_children`、`children_excluded[]`、`exclude_children_recursively`、`remove_all_bone_animation`、`remove_virtual_bone_names[]`、`finalize_after_set`、`save_after_set` |
| `anim_sequence_set_metadata` | 统一管理 AnimSequence metadata 生命周期 | `asset_path`；可选 `metadata_class_path`、`metadata_values`、`remove`、`clear_all`、`save_after_set` |
| `anim_sequence_set_notify_track` | 新增 / 更新 / 删除 notify track | `asset_path`、`track_name`；可选 `track_color`、`remove`、`save_after_set` |
| `anim_sequence_set_notify` | 新增 / 更新 / 删除单条 notify | `asset_path`；新增时传 `time_seconds`，更新/删除时传 `notify_index`；可选 `track_name`、`notify_name`、`notify_class`、`notify_state_class`、`duration_seconds`、`tick_type`、`trigger_weight_threshold`、`notify_trigger_chance`、`notify_filter_type`、`notify_filter_lod`、`can_be_filtered_via_request`、`trigger_on_dedicated_server`、`trigger_on_follower`、`notify_color`、`remove`、`save_after_set` |
| `anim_sequence_set_sync_markers` | 批量新增 / 删除 / 清空 sync marker | `asset_path`；可选 `add_markers[]`、`remove_marker_names[]`、`remove_notify_track_names[]`、`clear_all`、`save_after_set` |

### `anim_sequence_get_info`

当前返回：
- `skeleton`
- `preview_skeletal_mesh`
- `sequence_length`
- `rate_scale`
- `num_frames`
- `interpolation_type`
- `additive_animation_type`
- `additive_base_pose_type`
- `additive_base_pose_sequence`
- `additive_base_pose_frame_index`
- `root_motion_enabled`
- `force_root_lock`
- `root_motion_lock_type`
- `retarget_source`
- `retarget_source_asset`
- `bone_compression_settings`
- `curve_compression_settings`
- `variable_frame_stripping_settings`
- `track_count`
- `track_names[]`
- `metadata_count`
- `metadata[]`
- `curve_count`
- `curves[]`
- `notifies[]`
- `notify_tracks[]`
- `sync_markers[]`
- `unique_notify_names[]`
- `unique_marker_names[]`

说明：
- `notifies[]` 当前会回读 `notify_index / notify_name / track_name / time_seconds / duration_seconds / tick_type / trigger/filter 字段 / color / class`。
- `metadata[]` 当前会回读 metadata 类与简单 `properties` 摘要。
- `notify_tracks[]` 用于快速确认 notify 和 marker 依附在哪条轨上。
- `sync_markers[]` 适合做 marker sync 自动化回读。

### `anim_sequence_screenshot`

说明：
- 用于动画选帧审查，按 `frame_index` 或 `time_seconds` 把指定 `AnimSequence` 固定到目标帧并输出图片。
- 如果动画资产没有设置预览网格，建议显式传 `skeletal_mesh_path`；命令会检查 Skeleton 兼容性。
- 当前 `target` 只稳定开放 `viewport`，实现内部使用临时预览 Actor + SceneCapture，不依赖当前关卡相机。
- 当前会创建临时补光和背景板，并只把预览角色/背景板纳入捕获，避免当前关卡地面、水面、墙体或背光污染选帧截图。
- 返回字段包含 `file_path`、`capture_mode`、`frame_index`、`time_seconds`、`num_frames`、`preview_skeletal_mesh`。

示例：

```json
{
  "asset_path": "/Game/Characters/Paladin/Animations/sword_and_shield_180_turn__2_",
  "skeletal_mesh_path": "/Game/Characters/Paladin/Mesh/Paladin_WProp_J_Nordstrom",
  "frame_index": 24,
  "format": "png",
  "max_size": 1280,
  "target": "viewport"
}
```

### `anim_sequence_set_curve`

说明：
- 这是当前曲线管理的统一入口，避免继续拆 `add_float_curve / remove_curve / add_curve_key` 多条命令。
- 当前稳定开放 `curve_type=float`。
- 推荐优先使用 `curve_json`，格式为 `ue_agent_interface.curve.v1` 的 float 曲线结构；写入前会校验未知字段、缺 key 值、重复时间、非法插值/切线/外推模式，并在 `json_issues[]` 返回。
- 单 key 写入可直接传顶层 `time_seconds` + `value`。
- 多 key 写入走 `keys[]`。
- `clear_existing_keys=true` 会先清掉该曲线现有 key，再重建当前输入。

推荐参数形式：

```json
{
  "asset_path": "/Game/Anim/A_Idle_Copy",
  "curve_name": "StageCurve",
  "curve_type": "float",
  "curve_json": {
    "schema": "ue_agent_interface.curve.v1",
    "curve_kind": "float",
    "storage": "animation_raw_curve",
    "carrier_cpp_type": "FRichCurve",
    "channels": {
      "value": {
        "keys": [
          { "time": 0.10, "value": 1.0 },
          { "time": 0.20, "value": 2.0 }
        ]
      }
    }
  },
  "save_after_set": false
}
```

### `anim_sequence_set_bones`

说明：
- 这是当前骨骼动画轨处理的统一入口，避免继续拆 `remove_bone_track / remove_all_bone_tracks / remove_virtual_bones` 多条命令。
- 当前稳定面主要是“按骨骼名删除动画轨”。
- `include_children=true` 时会走递归删除；如需要排除部分子骨骼，可配合 `children_excluded[]` 与 `exclude_children_recursively`。
- `remove_virtual_bone_names[]` 当前也已接入，但由于它会影响关联 skeleton，使用时应明确知道目标序列绑定的 skeleton。

推荐参数形式：

```json
{
  "asset_path": "/Game/Anim/A_Idle_Copy",
  "remove_bone_names": ["pelvis"],
  "include_children": false,
  "finalize_after_set": true,
  "save_after_set": false
}
```

### `anim_sequence_set_metadata`

说明：
- 这是 AnimSequence metadata 生命周期的统一入口，避免继续拆 `add_metadata / remove_metadata / clear_metadata` 多条命令。
- 传 `metadata_class_path` 且不带 `remove` 时，会“如果不存在则添加”，默认不重复创建同类 metadata。
- `metadata_values` 可用于同一条命令内直接写简单 metadata 属性。
- 传 `remove=true` 时，按类删除该序列上的全部同类 metadata。
- `clear_all=true` 时清空该序列上的全部 metadata。
- metadata 类必须是具体的 `UAnimMetaData` 派生类，不能是抽象基类。

推荐参数形式：

```json
{
  "asset_path": "/Game/Anim/A_Idle_Copy",
  "metadata_class_path": "/Script/GptProjectTest.GptAnimMetaTag",
  "metadata_values": {
    "tag": "Stage27Tag",
    "note": "Stage27Note"
  },
  "save_after_set": false
}
```

> 废弃写入命令已迁移到 `deprecatedCommand/13_AnimationAssets_Skeleton.md` 的对应章节。
### `anim_sequence_set_notify`

说明：
- 这是 AnimSequence notify 本体的统一入口，避免继续拆 `add_notify / update_notify / remove_notify / add_notify_state` 多条命令。
- 不传 `notify_class / notify_state_class` 时，会创建一条普通命名 notify。
- 传 `notify_state_class` 时，必须同时提供 `duration_seconds`。
- 新增时如 `track_name` 不存在，命令会先补齐 notify track。
- 当前优先支持 notify 结构、时间、轨道、颜色和常见触发/过滤字段；不继续扩到自定义 notify 对象 payload 属性编辑。

推荐参数形式：

```json
{
  "asset_path": "/Game/Anim/A_Idle_Copy",
  "track_name": "Markers",
  "time_seconds": 0.2,
  "notify_name": "FootstepL",
  "notify_color": { "r": 0.9, "g": 0.3, "b": 0.2, "a": 1.0 },
  "notify_filter_type": "lod",
  "notify_filter_lod": 1,
  "save_after_set": false
}
```

### `anim_sequence_set_sync_markers`

推荐参数形式：

```json
{
  "asset_path": "/Game/Anim/A_Idle_Copy",
  "add_markers": [
    { "marker_name": "LeftPlant", "time_seconds": 0.10, "notify_track_name": "Markers" },
    { "marker_name": "RightPlant", "time_seconds": 0.42, "notify_track_name": "Markers" }
  ],
  "save_after_set": false
}
```

说明：
- `remove_marker_names[]`：按 marker 名批量删除。
- `remove_notify_track_names[]`：按 notify track 批量删该轨上的 marker。
- `clear_all=true`：清空该序列全部 sync marker。
- 如果 `add_markers[].notify_track_name` 不存在，命令会先补对应 notify track。

## Skeleton

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `skeleton_get_info` | 读取 Skeleton 摘要 | `asset_path` |
| `skeleton_list_bones` | 列出 Reference Skeleton 骨骼层级 | `asset_path` |
| `skeleton_set_compatible_skeletons` | 批量管理 compatible skeleton 与相关开关 | `asset_path`；可选 `set_compatible_skeleton_paths[]`、`add_compatible_skeleton_paths[]`、`remove_compatible_skeleton_paths[]`、`clear_all`、`use_retarget_modes_from_compatible_skeleton`、`save_after_set` |
| `skeleton_set_preview_mesh` | 设置或清空 Skeleton 预览网格 | `asset_path`、`skeletal_mesh_path` 或 `clear_preview_mesh`、`save_after_set` |
| `skeleton_set_socket` | 新增 / 更新 / 删除 socket | `asset_path`、`socket_name`；新增/更新时可传 `bone_name`、`relative_location/rotation/scale`；删除时传 `remove=true` |
| `skeleton_set_virtual_bone` | 新增 / 删除 virtual bone | `asset_path`；新增时传 `source_bone_name/target_bone_name`、可选 `virtual_bone_name`；删除时传 `virtual_bone_name` 和 `remove=true` |

### `skeleton_get_info`

当前返回：
- `preview_skeletal_mesh`
- `root_bone_name`
- `bone_count`
- `socket_count`
- `virtual_bone_count`
- `slot_group_count`
- `compatible_skeleton_count`
- `slot_groups[]`
- `sockets[]`
- `virtual_bones[]`
- `compatible_skeletons[]`
- `use_retarget_modes_from_compatible_skeleton`
- `animation_notify_names[]`

说明：
- `compatible_skeletons[]` 和 `use_retarget_modes_from_compatible_skeleton` 适合做 skeleton 级重定向兼容关系回读。
- `slot_groups[]` 和 `animation_notify_names[]` 方便跟 Montage / Sequence 做联动检查。

### `skeleton_set_compatible_skeletons`

说明：
- 这是 skeleton 兼容关系的统一入口，避免继续拆 `add/remove/clear/set_use_retarget_modes` 多条命令。
- `set_compatible_skeleton_paths[]` 表示“整体替换”；`add_*` / `remove_*` 表示增量修改。
- 不允许把自己加为 compatible skeleton。

### `skeleton_set_socket`

说明：
- 不传 `remove` 时，命令会“存在则更新，不存在则创建”。
- 如果只想改偏移，不需要重建 socket。

### `skeleton_set_virtual_bone`

说明：
- 不传 `virtual_bone_name` 时，会让 UE 自动生成标准名字。
- 删除时按 `virtual_bone_name` 删除。

## 当前边界

- `AnimSequence` 当前已经覆盖资产级设置、压缩 / 剥帧设置、float 曲线、骨骼轨删除、metadata 生命周期、普通 notify、notify track、sync marker 和摘要回读。
- 官方 `AnimationBlueprintLibrary` 还存在 `vector / transform` 曲线相关 API，但基于当前 UE 5.6 数据模型实测，它们不够稳定，暂不作为正式稳定命令面对外暴露。
- 还没有继续扩到自定义 notify / notify state 对象的 payload 属性编辑，也没有扩到 `vector / transform` 曲线和更完整的压缩策略管理。
- `Skeleton` 当前聚焦 preview mesh、compatible skeleton、socket、virtual bone 和槽组/通知名回读，没有扩到更细粒度 retarget profile 编辑。

## 2026-04-21 增量更新

- `anim_sequence_set_metadata` 现在支持可选 `metadata_values`
- `anim_sequence_get_info.metadata[]` 现在会回读 `properties`

当前已稳定验证的 metadata 属性编辑范围：

- `FName`
- `FString`
- `FText`
- `bool`
- 数值类型

推荐写法：

```json
{
  "asset_path": "/Game/Anim/A_Idle_Copy",
  "metadata_class_path": "/Script/GptProjectTest.GptAnimMetaTag",
  "metadata_values": {
    "tag": "Stage27Tag",
    "note": "Stage27Note"
  },
  "save_after_set": false
}
```

说明：

- 属性名匹配会忽略大小写、下划线、连字符和空格差异。
- 当前不扩到嵌套 struct / array / object reference。
