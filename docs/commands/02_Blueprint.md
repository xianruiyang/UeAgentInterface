# 指令详解：Blueprint

> 废弃写入命令已迁移到 `deprecatedCommand/02_Blueprint.md`；本分册只保留主流程、读取、导出/应用、编译、诊断，以及尚未被 JSON / 结构化 JSON 覆盖的命令。

## 资产与编译

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `blueprint_create` | 创建 Blueprint 资产 | `asset_path`、`parent_class`、`compile_after_create`、`open_editor`、`save_after_create` |
| `blueprint_compile` | 编译 Blueprint | `asset_path`、`include_messages`、`severity_filter`、`max_messages`、`save_after_compile` |
| `blueprint_get_compile_log` | 触发编译并读取编译日志 | `asset_path`、`severity_filter`、`max_messages`、`save_after_compile` |
| `blueprint_get_info` | 读取 Blueprint 基础信息 | `asset_path` |
| `blueprint_list_graphs` | 列出 UberGraph / 函数图 / 宏图 / 委托图 | `asset_path` |
| `blueprint_export_folder` | 导出 Actor Blueprint 到固定文件夹式 JSON 结构 | `asset_path`、可选 `clean_output_dir`、`include_validation` |
| `blueprint_apply_folder` | 从固定文件夹式 JSON 结构回写 Actor Blueprint | `asset_path`、可选 `create_if_missing`、`compile_after_apply`、`save_after_apply` |

- `blueprint_compile` 只有在 `include_messages=true` 时才会回传过滤后的编译消息数组。
- `blueprint_list_graphs` 返回 `graphs[]` 和 `graph_count`，适合做“新增函数图/宏图/委托图后”的结构校验。
- `blueprint_export_folder` / `blueprint_apply_folder` 当前只覆盖 `Actor Blueprint`。
- 固定导出根目录为：`Saved/UeAssetFolders/ActorBlueprint`
- 单个资产的文件夹路径按 `asset_path` 自动展开，例如：
  - `/Game/Blueprints/BP_Door`
  - `Saved/UeAssetFolders/ActorBlueprint/Game/Blueprints/BP_Door`
- `blueprint_apply_folder` 当前对支持的图采用“保留内建入口节点，重建其余节点”的方式应用。
- `function_graph / macro_graph` 的边定义可引用保留节点 ID：
  - `__entry__`
  - `__result__`

## 组件（SCS）

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `blueprint_inspect_components` | 查看 SCS 组件树 | `asset_path` |

## 图与节点

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `blueprint_inspect_nodes` | 列出图节点与引脚 | `asset_path`、`graph_name`、`limit_per_graph`、`include_pins` |

- 当前节点创建能力集中在：标准事件、自定义事件、函数调用、变量节点，以及按类创建的通用非结构节点。
- `graph_name` 现在既可以传图名，也可以直接传 `graph_path`；对子图、状态机子图、Transition Rule 图优先建议传 `graph_path`，避免重名歧义。
- `blueprint_add_node_by_class` 适合 `K2Node_ExecutionSequence` 这类普通图节点；`Event / CustomEvent / CallFunction / Variable / ComponentBoundEvent` 以及会生成附属子图的结构节点，仍应走专用命令。

## 变量 / 函数 / 宏 / 委托

> 废弃写入命令已迁移到 `deprecatedCommand/02_Blueprint.md` 的对应章节。

- `blueprint_add_variable` 的常用高级类型参数：
  - `pin_subcategory`：主要用于 `PC_Real` 的 `float/double` 区分。
  - `pin_subcategory_object`：对象、类、结构体等需要附带类型对象时使用。
  - `container_type`：支持 `array/set/map`。
  - `instance_editable=true`：把变量改成实例可编辑。
- `WidgetBlueprint` 本质上也是 `Blueprint`，因此 UI 变量、事件图逻辑仍然可以走 `blueprint_add_variable`、`blueprint_add_custom_event_node` 等命令。

## 图视图控制（新增）

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `blueprint_graph_get_view` | 获取指定图的平移/缩放 | `asset_path`、`graph_name`、`open_editor_if_needed` |
| `blueprint_graph_set_view` | 设置指定图的平移/缩放 | `asset_path`、`graph_name`、`view_x`、`view_y`、`zoom`、`open_editor_if_needed` |

- `graph_name` 默认 `EventGraph`。
- `blueprint_graph_set_view` 至少需要传入 `view_x/view_y/zoom` 三者之一。

## 蓝图预览视口控制（新增）

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `blueprint_viewport_get_camera` | 获取蓝图预览视口相机 | `asset_path`、`open_editor_if_needed` |
| `blueprint_viewport_set_camera` | 设置蓝图预览视口相机 | `asset_path`、`location`、`rotation`、`fov`、`open_editor_if_needed` |

- 这两个命令操作的是 Blueprint 编辑器的组件预览视口（SCS Viewport），不是关卡主视口。

## 蓝图窗口截图（新增）

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `blueprint_screenshot` | 对蓝图编辑器窗口/视口/图面板截图 | `asset_path`、`target`、`graph_name`、`format`、`quality`、`max_size`、`open_editor_if_needed` |

`target` 支持：
- `viewport`：组件预览视口
- `graph`：指定 `graph_name` 的图面板
- `event_graph`：EventGraph 图面板
- `window`：蓝图编辑器所在窗口

返回字段补充：
- `capture_mode`：截图来源模式。常见值：
  - `viewport_window_crop`：直接从 Blueprint 窗口裁切预览视口区域；
  - `viewport_readpixels_fallback`：窗口裁切失败后，退回到视口像素读取；
  - `window_fallback`：视口相关截图失败后，退回到蓝图窗口截图；
  - `slate_widget`：普通 Slate Widget 截图（如 graph/window 目标）。

## 类级操作

> 废弃写入命令已迁移到 `deprecatedCommand/02_Blueprint.md` 的对应章节。

### 属性写入返回

`blueprint_set_component_property`、`blueprint_set_cdo_property` 以及 `blueprint_apply_folder` 中的组件/节点属性回写，都会暴露写入观测信息。原子命令返回 `requested_value_text`、`applied_value_text`、`property_import_status`、`property_import_verified`、`value_text_exact_match`、`value_text_changed_after_import`、`cpp_type`；文件夹式回写会把缺少 `property_name` / `value_text`、`ImportText` 失败、写后读回不一致等情况写入 `warning_count / warnings`。

`blueprint_apply_folder` 的可选文件（例如 `members/variables.json`、`members/delegates.json`、`components/tree.json`、`members/defaults.json`）只有不存在时才会跳过；如果文件存在但读取失败或 JSON 语法解析失败，会直接失败返回并带文件路径。数组条目不是 object 或缺 `name/property_name/value_text` 等关键字段时会进入 `warning_count / warnings[]`。

## 常见流程

1. `blueprint_create`
2. `blueprint_export_folder`
3. 编辑结构化目录中的 `members/*.json`、`components/tree.json`、`graphs/*.json`
4. `blueprint_apply_folder`
5. `blueprint_compile`
6. `save_asset`

## 文件夹式编辑流程（新增）

1. `blueprint_export_folder`
2. 在固定导出目录中编辑：
   - `asset.json`
   - `members/*.json`
   - `components/tree.json`
   - `graphs/*.json`
3. `blueprint_apply_folder`
4. `blueprint_compile` / `blueprint_get_compile_log`

说明：

- 这套模式适合中型以上 Blueprint 的结构化编辑。
- 当前它就是 `Actor Blueprint` 的主 authoring 工作流；上表中已标记的原子写入命令只做 bootstrap、探针验证和局部修补。
- 当前推荐把它当成“上层 authoring 格式 + 底层命令施工”的工作流，而不是整包覆盖重建器。
- 当某类对象属性面很大时，推荐不要第一次就把 `properties[]` 写满。
  更推荐：
  1. 先在 `components/tree.json` 或 `graphs/*.json` 里只写最小骨架
  2. `blueprint_apply_folder`
  3. `blueprint_export_folder`
  4. 以导出的真实 JSON 为模板补全属性
  5. 再次 `blueprint_apply_folder`
- `graphs/*.json` 当前稳定支持的基础 `node_type` 至少包括：
  - `event`
  - `custom_event`
  - `call_function`
  - `variable_node`
  - `component_bound_event`
  - `node_by_class`

`custom_event` 的最小结构示例：

```json
{
  "id": "custom_start",
  "node_type": "custom_event",
  "event_name": "StartRound",
  "pos": { "x": 0, "y": 180 }
}
```

`component` 的推荐最小骨架示例：

```json
{
  "name": "CameraBoom",
  "class": "/Script/Engine.SpringArmComponent",
  "parent": "DefaultSceneRoot",
  "properties": []
}
```

对这类组件，推荐先 apply 让 UE 建好真实模板，再 export 看当前可用属性，再继续补 `properties[]`。

## 2026-04-22 更新

- `blueprint_export_folder / blueprint_apply_folder` 的 `graphs/*.json` 现已支持以下专用 `node_type`：
  - `custom_event`
  - `enhanced_input_action_event`
  - `dynamic_cast`
  - `enhanced_input_get_local_player_subsystem`
  - `enhanced_input_add_mapping_context`
- `blueprint_set_cdo_property` 现可沿对象子属性路径继续解析，因此可直接写入 `CharacterMovement.*` 默认值。
