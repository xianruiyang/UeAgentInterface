# UAI JSON Authoring Guide

本指南定义 UeAgentInterface 结构化 JSON 的 LLM authoring 主路径。它不替代单命令文档；字段、参数、返回和支持面仍以 `commands/<category>/<command>.md`、当前插件源码、adapter/profile、实际导出和 readback/smoke 为准。

## Documentation Route

| 文档 | 内容 |
| --- | --- |
| `json-authoring/00_DirectJsonAuthoring.md` | 直接编辑 `llm_authoring` JSON、`node_catalog_search.items_tsv -> path/name seed -> apply/readback` 的统一主链路。 |
| `json-authoring/01_Workflow.md` | export -> read guide/index -> edit `llm_authoring` -> validate/dry-run -> apply -> readback 的通用流程。 |
| `json-authoring/02_CommonInputValue.md` | 通用 `value`、enum、source、只读字段和 compact JSON 规则。 |
| `json-authoring/03_OperationsAndSelectors.md` | selector、operation、新增/删除/替换结构、最小 seed。 |
| `json-authoring/04_SpaceContract.md` | transform、layout、socket、IK、graph/node/widget 坐标合同。 |
| `json-authoring/05_Validation.md` | lint、adapter、validate/dry-run、apply、readback、coverage gap 处理。 |
| `json-authoring/06_LegacyMigration.md` | 旧字段、旧结构和 deprecated 命令的迁移/废弃边界。 |
| `json-authoring/07_ImplementationConsistency.md` | docs/source/wrapper/pack/export 一致性检查入口、失败处理和搜索边界。 |
| `json-authoring/recipes/*.md` | 模块特有规则，只讲通用教程之外的差异。 |
| `commands/<category>/<command>.md` | 命令参数、返回字段、folder JSON 支持范围、validate/diff 覆盖和实现边界。 |

## Main Authoring Contract

结构化 folder 默认 profile 是 `llm_authoring`。LLM 默认直接编辑该 profile 下的 folder JSON；Python 只用于批量生成、参数化调用、adapter 调试或重复变量过多的辅助工作，不能替代 JSON 中的业务意图。

默认阅读顺序：

1. 先读 `manifest.json`、`index.json`、`references/index.json` 或同等导航文件。
2. 再按当前目标打开 `authoring/*.json`、`authoring/<collection>/index.json` 或 `graphs/index.json`。
3. 只有需要编辑某个大型集合项时，才打开 `items/<id>/...` 下的具体文件。
4. 不把大型 Actor/Component item、raw/readback-only 内容或 debug report 放进默认一次性上下文。P3 验证中 Level 已出现 30 个 actor/component item JSON 超过 16,000 bytes；这些文件必须按需读取。

`llm_authoring` 只包含：

- `public_seed_identity`
- `container_context`
- `editable_authoring`
- `required_readonly`

默认不包含：

- `legacy_compat`
- `debug_diagnostics`
- `readback_report`
- `raw_heavy`
- `command_param_only`

命令参数和 folder JSON 字段必须分开理解。`asset_path`、`folder_path`、`dry_run`、`include_raw_properties`、`clean_output_dir` 等是命令参数或显式诊断选项，不是默认可编辑 authoring 字段。

## Direct JSON Authoring First

```text
export/readback folder package
-> read manifest/index/reference first
-> edit only required llm_authoring files
-> when adding candidates, use node_catalog_search items_tsv path/name seed
-> validate/lint/dry-run when available
-> apply
-> export/readback/compile/probe
-> semantic verification
```

不要用一长串原子命令替代已有 JSON/folder workflow。新增内容默认走“最小 seed -> apply 补全 -> readback 继续编辑”：不要从 readback 复制完整对象，也不要手写 UE/UAI 会生成的 id、template、slot、pin、默认属性、diagnostics 或 derived 字段。

公开 seed 主身份只有两类：

- `path`：调用方已经知道资产、类、对象或 owner 路径时使用。
- `name`：从 `node_catalog_search` 返回的无路径候选或上下文可解析候选使用。

`full_name`、`asset_path`、`class_path`、`object_path`、`component_class_path`、`script_asset_path`、`module_script_path` 等旧字段只作 legacy/debug/readback 兼容，不是新 authoring 推荐输入。旧结构和 deprecated 命令边界见 `json-authoring/06_LegacyMigration.md`。

## Merge-First Folder Shape

- `index.json` 只做导航摘要，不保存完整对象。
- 短小内容合并进资产级或集合级 JSON。
- 只有大型集合、独立生命周期、独立 diff/apply 价值或上下文过大的内容才拆到 `items/<id>/...`。
- Graph 逻辑整体写在 `graphs/items/<graph_id>/graph.json`，包含 nodes/pins/links 或等价逻辑内容。
- Graph 视觉布局写在 `graphs/items/<graph_id>/layout.json`，只保存节点位置、注释框、折叠状态等布局信息。
- 旧平铺 `graphs/<name>.json`、旧 `widget_tree/tree.json` 和旧 properties/index 布局只允许出现在显式迁移、负例、错误或 debug/readback 语境，不是 authoring 输入。

## Details Grouped Authoring

UE Details、UObject 属性面板和资产设置面板可编辑字段统一走公共 Details authoring 层。

- 默认导出不在入口 JSON 内展开 `properties[]`、`settings_properties[]` 等 Details 描述数组；入口只保留身份、业务结构和 `details` / `*_details` 引用。
- 分组文件写入相邻 `details/<owner_or_field>/index.json` 和 `details/<owner_or_field>/<group>.json`；Level Actor/Component/WorldSettings 可直接使用对象目录下的 `details/index.json`。
- `details/index.json` 是导航和完整性摘要；实际可编辑字段在 group 文件的 `fields[]` 中。
- group field 保留 `property_path`、`selector`、`display_name`、`category_path`、`control_type`、`value_kind`、`value`、`availability`、`operations`；有编辑意义时保留 `tooltip` 和 enum/object/class `candidates`。
- 为减少重复，默认不保留和结构化 `value` 等价的展示副本，例如 `value_text`、`default_value_text`；旧输入只允许由显式迁移工具、reader 诊断或负例测试识别，不作为 authoring 输入。
- `raw_properties`、`readonly_properties`、`missing_properties`、`unsupported_properties` 是诊断或只读材料，不会被公共层自动当作可编辑 Details 外置。
- apply/validate/diff 读取新 grouped Details 时会先恢复为命令内部结构；旧 inline `properties[]` 不作为合法 authoring 输入，如出现应进入显式迁移、负例、错误或 debug/readback 语境。
- 只返回 response、没有稳定输出文件边界的命令仍返回 inline JSON；它们必须走同一 compact writer，但不会伪造 folder `details/` 文件。

## Field Rules

1. 已知路径实体用 `path`，无路径候选用 `name`。不要让 LLM 手填 `full_name` 或细分旧路径字段。
2. 可编辑属性或等价编辑项的主身份使用 `property_path` 或域内等价 selector；legacy `name` 只作为显式迁移、debug 或 readback 识别字段，不作为默认导出字段。
3. `required_readonly` 字段可以留在 `llm_authoring`，但只用于识别、校验、约束或差异判断，不由 apply 写入。
4. 普通值使用结构化 `value` 或域内 `value_text`；enum/choice 必须带 `candidates`，LLM 只能写候选值。
5. 每个可编辑项默认应包含 `selector`、`display_name`、`category_path`、`control_type`、`value_kind`、`availability`、`operations`；有说明和候选时保留 `tooltip`、`candidates`。
6. `operation` 只用于 add/remove/replace/clear/rename/set_binding 等非普通值意图。
7. 不写 derived count、export timestamp、file path、compile report、runtime probe、coverage report、roundtrip status 或 raw enum options。
8. 不把 `raw_properties`、`readonly_properties`、`missing_properties` 或 `unsupported_properties` 当成通用可写面；这些字段不作为 apply 输入。
9. unknown/unsupported operation 必须在 adapter 或 validate 阶段失败，不得静默丢弃用户意图。

## Coverage Boundaries

当前专用 validate/diff 覆盖不是完整矩阵。

- 这些类型当前缺 dedicated `*_validate_folder`：`blueprint`、`umg`、`anim_blueprint`、`material`、`material_instance`、`material_function`、`niagara_system`、`niagara_emitter`、`niagara_script`、`sequence`。
- 上述类型也缺 dedicated `*_diff_folder`。
- AI 行为树相关既有 folder 文档存在，但 contract operation registration 仍为 `missing`，不得写成闭环 authoring 能力。

缺 dedicated validate/diff 时，不能声称 full coverage；改用 export/readback、command response、authoring consistency gate 或结构化比较补验证。

实现一致性入口、失败 issue 字段、command toolchain audit 和不得宽泛搜索源码/旧文档的规则见 `json-authoring/07_ImplementationConsistency.md`。

## Standard Loop

1. 导出或读取固定 fixture。
2. 读取导航文件，按需打开目标 `llm_authoring` 文件。
3. 只编辑 authoring 文件，不改 readback/debug/legacy。
4. 新增候选时解析 `node_catalog_search.items_tsv`，优先使用 `path` 列，无路径候选使用 `name` 列。
5. 运行 lint、validate 或 dry-run；没有 dedicated validate 时记录 coverage gap。
6. apply。
7. export/readback，比较语义结果。
8. 对支持编译的域检查 compile/stack issue。
9. 记录 warning、dirty resources、process state 和剩余边界。

普通导出、apply、readback、smoke 复用当前 UE Editor，不因为普通验证完成而关闭用户已有 UE。只有需要重新编译插件/UE C++ 且 DLL 被占用时，才先处理 dirty resource，然后安全关闭 UE 构建。

## Verification Requirements

| Case | Required check |
| --- | --- |
| Value edit | command response plus readback exact or normalized value check. |
| Graph edit | node/link count, supported node type, layout if edited, compile/validation report when available, readback graph. |
| Niagara edit | stack issues, compile log, active branch/mode, runtime or preview probe when visual behavior matters. |
| Space edit | same subject, same frame, same state readback; visual check when layout or rendering matters. |
| Destructive edit | explicit destructive gate, dry-run or plan first, readback after apply. |
| Long-tail config | validate/diff/dry-run when available; do not treat packaging/config output as persistent asset state unless command reports it. |
