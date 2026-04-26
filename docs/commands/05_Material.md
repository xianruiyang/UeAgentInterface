# 指令详解：Material

> 废弃写入命令已迁移到 `deprecatedCommand/05_Material.md`；本分册只保留主流程、读取、导出/应用、编译、诊断，以及尚未被 JSON / 结构化 JSON 覆盖的命令。

## 资产与编译

| 指令 | 作用 | 关键参数 | 说明 |
|---|---|---|---|
| `material_create` | 创建 `UMaterial` | `asset_path` | 可选 `compile_after_create`、`save_after_create` |
| `material_instance_create` | 创建 `UMaterialInstanceConstant` | `asset_path`、`parent_material_path` | 用于参数化材质实例 |
| `material_open_editor` | 打开材质编辑器 | `asset_path` | 进入可视化编辑上下文 |
| `material_get_info` | 读取材质/材质实例信息 | `asset_path` | 返回参数、父材质、表达式摘要 |
| `material_compile` | 编译材质或刷新实例 | `asset_path` | 可选 `include_messages`、`severity_filter`、`max_messages` |
| `material_get_compile_log` | 读取编译日志 | `asset_path` | 可选 `compile_before_read`、`severity_filter`、`max_messages` |
| `material_export_folder` | 导出 `UMaterial` 到固定文件夹式 JSON 结构 | `asset_path`、可选 `clean_output_dir`、`include_validation` | 固定根目录：`Saved/UeAssetFolders/MaterialGraph` |
| `material_apply_folder` | 从固定文件夹式 JSON 结构回写 `UMaterial` | `asset_path`、可选 `create_if_missing`、`compile_after_apply`、`save_after_apply` | 第一版采用“整图重建式 apply” |
| `material_instance_export_folder` | 导出 `UMaterialInstanceConstant` 到固定文件夹式 JSON 结构 | `asset_path`、可选 `clean_output_dir`、`include_validation` | 固定根目录：`Saved/UeAssetFolders/MaterialInstance` |
| `material_instance_apply_folder` | 从固定文件夹式 JSON 结构回写 `UMaterialInstanceConstant` | `asset_path`、可选 `create_if_missing`、`clear_existing_overrides`、`compile_after_apply`、`save_after_apply` | 第一版聚焦“父材质 + 参数覆盖” |
| `material_function_export_folder` | 导出 `UMaterialFunction` 到固定文件夹式 JSON 结构 | `asset_path`、可选 `clean_output_dir`、`include_validation` | 固定根目录：`Saved/UeAssetFolders/MaterialFunction` |
| `material_function_apply_folder` | 从固定文件夹式 JSON 结构回写 `UMaterialFunction` | `asset_path`、可选 `create_if_missing`、`update_after_apply`、`save_after_apply` | 第一版采用“整图重建式 apply” |

## 属性与实例参数

> 废弃写入命令已迁移到 `deprecatedCommand/05_Material.md` 的对应章节。

`material_set_property` 返回通用属性写入观测字段：`requested_value_text`、`applied_value_text`、`property_import_status`、`property_import_verified`、`value_text_exact_match`、`value_text_changed_after_import`、`cpp_type`。

## 表达式图编辑

| 指令 | 作用 | 关键参数 | 说明 |
|---|---|---|---|
| `material_list_expressions` | 列出表达式节点 | `asset_path` | 返回名称、类、GUID、位置 |

`material_set_expression_property` 返回通用属性写入观测字段。`MaterialFunction` 这类专用对象引用写入会使用 `property_import_status=assigned_object_reference`，其余属性使用 `ImportText` 并读回实际值。

## 文件夹式编辑流程（新增）

1. `material_export_folder`
2. 在固定导出目录中编辑：
   - `asset.json`
   - `settings/material.json`
   - `parameters/interface.json`
   - `graphs/MaterialGraph.json`
   - `validation/checks.json`
3. `material_apply_folder`
4. `material_compile` / `material_get_compile_log`

说明：

- `UMaterial` 当前主编辑路径是 `material_export_folder / material_apply_folder`。
- 对属性很多的表达式节点或材质根设置，推荐先把最小图结构建出来，再 export，看 UE 导出的真实模板后继续补字段。
- 当前只覆盖 `UMaterial`，不覆盖 `Material Instance`。
- 固定导出根目录：
  - `Saved/UeAssetFolders/MaterialGraph`
- 单个资产目录按 `asset_path` 自动展开，例如：
  - `/Game/Materials/M_Door` -> `Saved/UeAssetFolders/MaterialGraph/Game/Materials/M_Door`
- `material_apply_folder` 第一版对表达式图采用“删除现有表达式，再按文件夹描述重建”的方式。
- 材质根对象左侧 `Details` 面板参数归 `settings/material.json`。
- `material_apply_folder` 返回 `warning_count` / `warnings`。根属性和表达式属性缺少 `property_name` / `value_text`、写后读回不一致等情况会进入 warnings，方便定位结构化 JSON 中的解析或规范化问题。
- Material 三个 folder apply 的可选 JSON（如 `settings/material.json`、`parameters/interface.json`、`parameters/overrides.json`、`function.json`）只有不存在时才会跳过；文件存在但读取失败或 JSON 语法解析失败会直接失败返回并带文件路径。
- `material_instance_apply_folder` 和 `material_function_apply_folder` 也返回 `warning_count / warnings`；数组条目不是 object 或缺 `id/name/property_name/value_text/parameter_type` 等关键字段时会写入 warnings，不再静默丢弃。

## `Material Instance` 文件夹式编辑流程（新增）

1. `material_instance_export_folder`
2. 在固定导出目录中编辑：
   - `asset.json`
   - `parent.json`
   - `parameters/overrides.json`
   - `validation/checks.json`
3. `material_instance_apply_folder`
4. `material_compile` / `material_get_compile_log`

说明：

- `UMaterialInstanceConstant` 当前主编辑路径是 `material_instance_export_folder / material_instance_apply_folder`
- 对参数覆盖很多的实例，推荐先只放最小 `parent + 少量 overrides`，再 export，看 UE 当前导出的真实 overrides 结构后继续补全
- 当前只覆盖 `UMaterialInstanceConstant`
- 固定导出根目录：
  - `Saved/UeAssetFolders/MaterialInstance`
- 单个资产目录按 `asset_path` 自动展开，例如：
  - `/Game/Materials/MI_Door` -> `Saved/UeAssetFolders/MaterialInstance/Game/Materials/MI_Door`
- 当前 profile 聚焦：
  - `parent.json`：父材质链
  - `parameters/overrides.json`：参数 overrides 真源
- `material_instance_apply_folder` 默认：
  - `clear_existing_overrides=true`
  - 先清旧 overrides，再按文件夹真源重建

`parameters/overrides.json` 当前统一使用 `parameters[]`，每项包含：

- `name`
- `parameter_type`
- 对应值字段：
  - `scalar` -> `value`
  - `vector` -> `value:{r,g,b,a}`
  - `texture` -> `texture_path`
  - `static_switch` -> `value`
  - `static_component_mask` -> `value:{r,g,b,a}`

## `material_get_info`

当前会返回这些高频参数数组：

- `scalar_parameters[]`
- `vector_parameters[]`
- `texture_parameters[]`
- `static_switch_parameters[]`
- `static_component_mask_parameters[]`

其中 `static_component_mask_parameters[]` 额外包含：

- `name`
- `r/g/b/a`
- `expression_guid`
- `resolved`

说明：

- 对 `UMaterial`，返回父材质默认值。
- 对 `UMaterialInstanceConstant`，优先返回实例静态参数覆盖；若实例未覆盖，则回落到父材质默认值。

> 废弃写入命令已迁移到 `deprecatedCommand/05_Material.md` 的对应章节。
## 当前边界

- `material_set_parameter` 当前统一的是实例参数写入，不覆盖材质图表达式编辑。
- `static_component_mask` 目前走实例静态参数覆盖路径，适合 `MIC` 自动化，不是通用材质图 authoring 替代。
- `material_set_parameter` 与旧的 `material_set_scalar/vector/texture/static_switch_parameter` 均保留为兼容入口，完整 authoring 优先走 `material_instance_export_folder / material_instance_apply_folder`。
- `material_export_folder / material_apply_folder` 当前只覆盖 `UMaterial`，不覆盖 `UMaterialInstanceConstant` 和 `UMaterialFunction`。
- `material_instance_export_folder / material_instance_apply_folder` 当前只覆盖 `UMaterialInstanceConstant`，不覆盖 `UMaterialFunction` / `MaterialFunctionInstance`。
- `material_function_export_folder / material_function_apply_folder` 当前只覆盖 `UMaterialFunction`，不覆盖 `MaterialFunctionInstance / Material Layer / Material Layer Blend`。
- `parameters/interface.json` 当前主要服务于参数接口摘要与 `ParameterName / Group / SortPriority` 回写，不是独立于图拓扑的唯一真源。

## `Material Function` 文件夹式编辑流程（新增）

1. `material_function_export_folder`
2. 在固定导出目录中编辑：
   - `asset.json`
   - `function.json`
   - `graphs/MaterialFunctionGraph.json`
   - `validation/checks.json`
3. `material_function_apply_folder`

说明：

- `UMaterialFunction` 当前主编辑路径是 `material_function_export_folder / material_function_apply_folder`
- 推荐同样采用“先最小图结构 -> export -> 补字段 -> apply”的方式，而不是第一次就把函数图所有表达式属性写满
- 当前只覆盖 `UMaterialFunction`
- 固定导出根目录：
  - `Saved/UeAssetFolders/MaterialFunction`
- 单个资产目录按 `asset_path` 自动展开，例如：
  - `/Game/Materials/MF_Waves` -> `Saved/UeAssetFolders/MaterialFunction/Game/Materials/MF_Waves`
- 当前 profile 聚焦：
  - `function.json`：函数级基本属性，如 `description / expose_to_library`
  - `graphs/MaterialFunctionGraph.json`：表达式图真源
- `material_function_apply_folder` 第一版：
  - 先删除现有表达式，再按文件夹描述重建
  - 完成后调用 `UpdateMaterialFunction`
