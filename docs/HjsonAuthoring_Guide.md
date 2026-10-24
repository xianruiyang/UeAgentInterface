# UAI HJSON Authoring Guide

本指南定义 UeAgentInterface 结构化 HJSON 的 LLM authoring 主路径。它不替代单命令文档；字段、参数、返回和支持面仍以 `commands/<category>/<command>.md`、当前插件源码、adapter/profile、实际导出和 readback/smoke 为准。

## Documentation Route

| 文档 | 内容 |
| --- | --- |
| `hjson-authoring/00_DirectHjsonAuthoring.md` | 直接编辑 `llm_authoring` HJSON、领域 candidate identity -> HJSON binding -> apply/readback 的统一主链路。 |
| `hjson-authoring/01_Workflow.md` | export -> read guide/index -> edit `llm_authoring` -> import/apply -> readback 的通用流程。 |
| `hjson-authoring/02_CommonInputValue.md` | 通用 `value`、enum、source、只读字段和 compact HJSON 规则。 |
| `hjson-authoring/03_OperationsAndSelectors.md` | selector、operation、新增/删除/替换结构、最小 seed。 |
| `hjson-authoring/04_SpaceContract.md` | transform、layout、socket、IK、graph/node/widget 坐标合同。 |
| `hjson-authoring/05_Validation.md` | import 合同校验、apply、readback 和领域必要证据。 |
| `hjson-authoring/06_LegacyMigration.md` | 旧输入拒绝、历史归档和 deprecated 命令边界。 |
| `hjson-authoring/07_ImplementationConsistency.md` | docs/source/wrapper/pack/export 一致性检查入口、失败处理和搜索边界。 |
| `hjson-authoring/08_NiagaraApplyDomain.md` | Niagara System/Emitter/Script、stage/module/dynamic input 的统一 Apply/Export 纵切、契约与能力边界。 |
| `hjson-authoring/09_GeneratedExamples.md` | 当前 workspace 的只读最小示例、类型递归与集合创建规则。 |
| `hjson-authoring/recipes/*.md` | 模块特有规则，只讲通用教程之外的差异。 |
| `commands/<category>/<command>.md` | 命令参数、返回字段、folder HJSON 支持范围、validate/diff 覆盖和实现边界。 |

## Main Authoring Contract

结构化 authoring 默认通过 `content_graph.export_workspace` / `content_graph.import_workspace`，profile 是 `llm_authoring`。LLM 默认直接编辑该 profile 下的 HJSON workspace；Python 只用于批量生成、参数化调用、adapter 调试或重复变量过多的辅助工作，不能替代 HJSON 中的业务意图。领域专用的历史 folder command 不再作为默认入口。

落盘的 authoring、folder package、fixture、contract 和 CLI report 默认使用 HJSON。UAI HTTP 请求/响应 envelope、内存对象和 `--json-output` stdout 仍是 JSON 传输格式；历史命令名或参数名里的 `json` 只表示兼容 API 字段名，不表示 authoring 文件仍为 compact JSON。

路径兼容边界：

- 模型可见的 target、structure、item 和 graph 路径使用业务语义名；已命名图容器保留 authored name。所有重名分配共用 `X`、`X#2`、`X#3` 规则，第一项无后缀；同一真实身份的重复引用复用已有别名，仅不同条目在所属作用域内消歧，Pin 作用域为单个节点。真实名称自带的数字或 `#` 不作为自动后缀剥除；已占用名称不能覆盖。完整别名才是引用，连线和默认值都消费同一映射。含 `#` 的 HJSON key、引用及路径保留双引号，避免被解析为注释。稳定 local handle、hash、GUID、revision 和它们的映射只存在于隐藏 `.uai/workspace.hjson`，不得复制到文件名、HJSON 或 Python 参数。
- `output_file` 传 `.json` 时，authoring 导出会规范化写到同名 `.hjson`，返回的 `output_file` / `output_format` 以实际落盘为准。
- `json_file` 作为历史输入参数时，若命令已声明兼容别名，可按同名 `.hjson` 读取；不声明兼容的 HJSON authoring 文件输入必须直接传 `.hjson`。
- inline `json`、`json_text`、`curve_json`、`value_json`、response/report/runtime JSON 不属于 authoring 文件格式迁移范围。

默认阅读顺序：

1. 先按当前 ExportProfile/folder contract 的 `included_files` 和教程 `read_order` 读取必需 identity/entry 导航文件；不假定一定存在 `manifest.hjson`、`index.hjson` 或 references。
2. 再按当前目标打开契约列出的 authoring unit、collection index、graph index 或直接 ref。
3. 只有需要编辑某个大型集合项时，才打开 `items/<可读语义名>/...` 下的具体文件；同名项使用导出给出的连续编号，不自行构造或抄写机器 identity。
4. 不把大型 Actor/Component item、raw/readback-only 内容或 debug report 放进默认一次性上下文。P3 验证中 Level 已出现 30 个 actor/component item HJSON 超过 16,000 bytes；这些文件必须按需读取。

新增内容或空集合的形状不明时，按 `examples/index.hjson` 定位当前业务文件对应的最小示例，
不读取整个示例库。`examples/` 是独立只读说明，复用当前类型/集合合同生成，不参与资产导入、
对象身份、业务基线或默认值应用；写入业务文件后才表达编辑意图。

`llm_authoring` 只包含：

- `public_seed_identity`
- `container_context`
- `editable_authoring`
- `required_readonly`

普通 `llm_authoring` HJSON 是模型编辑面，不是内部身份清单：

- `unit_id`、`target_identity_key`、GUID、hash、revision 和 `property_definition:<hash>` 等精确机器身份只保存在隐藏 `.uai/` manifest 与映射文件中；模型不编辑 `.uai/`。旧 workspace 不兼容此投影，必须重新 export，不能靠兼容表猜回身份。
- 已存在的 Level Actor 和 Blueprint Component 以可读 `label` / `name` 作为 map key；同名实例由当前 owner 的最短语义作用域消歧。两者新增项均把各自候选查询返回的可读 `name` 写在 row 顶层，map key 表达新实例名；`candidate` envelope 与 `operation` 由服务端恢复。Level Actor 在 `actor_component_hierarchy.hjson` 的 `actors` 中新增 row，候选来自 `world.actor_candidates`；Blueprint Component 使用组件树及组件候选查询。其他字段沿各自导出合同，不因创建名称形式一致而混用领域字段。
- 图内节点和 Pin 都使用当前图作用域下的语义 key；同名 Pin 只追加连续 occurrence。Pin 默认值直接写成 `{ type, value }`，其中 `type` 是真实标量或 struct 类型，`value` 是直接标量或递归对象，不再暴露 Pin GUID、`pin_default.<GUID>` 或 `{ pin_name, default_value }` 包装。
- 每个 UObject 的普通 `properties.hjson` 只保留一个 `categories` 编辑入口：完整 UE Category path 作为组 key，
  无类别字段归入 `General`，每组直接以属性名组织 `{type,value}`，导出可附只读 `definition` 引用。`type` 是稳定值类型，完整属性约束/候选按 `definition` 查所属 `definitions.hjson`；不能用基础类型名代替定义引用。Category 只出现一次并只负责导航；隐藏
  manifest 持有精确 property unit/pointer，旧 flat `properties` workspace 必须重新 export。

默认不包含：

- `legacy_compat`
- `debug_diagnostics`
- `readback_report`
- `raw_heavy`
- `command_param_only`

命令参数和 folder HJSON 字段必须分开理解。`asset_path`、`folder_path`、`dry_run`、`include_raw_properties`、`clean_output_dir` 等是命令参数或显式诊断选项，不是默认可编辑 authoring 字段。

## Direct HJSON Authoring First

```text
export/readback folder package
-> follow ExportProfile included_files/tutorial read_order
-> edit only required llm_authoring files
-> when adding content, query the exact owning domain candidate command
-> copy the exact candidate projection allowed by that domain into its HJSON
-> import/apply
-> export/readback/compile/probe
-> semantic verification
```

不要用一长串原子命令替代 `content_graph` HJSON workspace。新增内容默认走“最小 seed -> apply 补全 -> readback 继续编辑”：不要从 readback 复制完整对象，也不要手写 UE/UAI 会生成的 id、template、slot、pin、默认属性、diagnostics 或 derived 字段。入口收敛不按命令数量清退 typed operations；仍有独立消费者或尚未被 HJSON 覆盖的合同继续由其正式 owner 承担，例如 `docs/active/t51_niagara_command_migration.md` 登记的 T51 Niagara typed commands。

多数公开 seed 以两类字段表达主身份：

- `path`：调用方已经知道资产、类、对象或 owner 路径时使用。
- 可读 `name`：使用当前领域 candidate command 返回的名称；只有同名时才附带消除歧义所需的短语义 `select` 字段。

Blueprint node 与 Component 不共享文件形状。Blueprint graph node 服从当前 graph 导出的
`nodes` map：新 row 的 key 是本地可读 alias，row 写 `candidate:{name, select?}` 和可选
`pos: {x, y}`，不得显式写机器身份；服务端从 alias 与 desired-state diff
派生 node identity 和 create/update/remove。ordinary component-tree 的新 Component row 则把可读 `name`
直接写在顶层，以 map key 表达实例名，并可选填写 `parent`，不得再写显式 `operation` 或嵌套
`candidate`。服务端在写前从同一原生 owner 重枚举并恢复 action/class/asset override 等内部身份。
候选查询只返回可读名称或最短作用域别名；同名时只附带消除歧义所需的短语义 `select`，不会公开
function/class/spawner/signature 或其他机器身份。
同批新增Blueprint成员时，旧资产候选列表尚不包含新成员动作；缺失的候选（保留原name和select）交给已有
scratch预检，在应用成员、编译并刷新原生动作后精确解析。未知或歧义候选仍在持久写入前失败，
不要求拆分变量与节点提交或重启Editor。普通Blueprint、AnimBP与WidgetBP共享此入口及成员→编译刷新→节点创建顺序；direct document的candidate节点类型也由共同mutation派生。新图自身的创建/查询顺序仍遵守所属领域合同。

字段是否可写只由当前 CommandSpec 或 folder contract 决定；folder authoring 不把历史 identity alias 自动迁移为新 target。旧结构拒绝和 deprecated 命令边界见 `hjson-authoring/06_LegacyMigration.md`。

## Merge-First Folder Shape

- `index.hjson` 只做导航摘要，不保存完整对象。
- 短小内容合并进资产级或集合级 HJSON。
- 只有大型集合、独立生命周期、独立 diff/apply 价值或上下文过大的内容才拆到 `items/<id>/...`。
- 支持 merge-first folder contract 的 Graph 把逻辑写在 `graphs/items/<graph_id>/graph.hjson`，把视觉布局写在同项的 `layout.hjson`。
- 当前 Blueprint graph 是明确例外：`content_graph` export 会生成 `graphs/<local-alias>.hjson`，authoring 必须编辑本次导出的原文件并按同一 workspace 回灌；不要擅自搬到 `graphs/items/`，也不要从历史 workspace 复制平铺文件。
- 其它旧平铺 graph、旧 `widget_tree/tree.hjson` 和旧 properties/index 布局只允许出现在显式迁移、负例、错误或 debug/readback 语境，不是 authoring 输入。

### Blueprint Graph Pin Interface And Variable Pin Groups

Blueprint graph 的可写 desired state 与当前节点实例接口分离，但仍由同一 capture revision 和同一原子 export plan 持有：

- `graphs/<local-alias>.hjson` 是唯一可写 graph 入口；有可用节点接口时，它包含相对 `interface_ref: <local-alias>.interfaces.hjson`。
- `graphs/<local-alias>.interfaces.hjson` 是同次 export 生成的只读 companion。其 `nodes` 使用主图完全相同的 node alias，并为每个当前非 orphan Pin 给出 `alias`、`direction`、`kind`、公共 `type`、`default_editable` 及适用的可见性/连接限制。公开查询的 `pin_interface.pins[].alias` 与导出接口共用同一投影，直接作为连接端点的 Pin 部分；空原生名称、重名和已有数字后缀均由公共投影处理，内部 occurrence 不进入普通接口，不由调用者推算。
- companion 的 `pin_groups[]` 描述当前节点实例的可变 Pin 组能力、数量和成员；它不是静态 node-class catalog，也不是第二状态源。不得编辑 companion、复制旧 companion，或从节点类型记忆猜 Pin；每次都消费当前 workspace 的 `interface_ref`。
- 主图 node 的 `pin_groups` 是可写目标数量 object。当前首批正式 key 只有 Sequence 的 `execution_outputs`（最少 2）与 Make Array 的 `inputs`（最少 1）；其他动态节点没有当前 export capability 时必须停止并报告缺口，不能类推这些 key。
- 可变组数量、受影响的 Pin 默认值与连接可在同一次编辑意图中提交；服务端在同一 prepared mutation 中重建真实接口，再解析端点与类型。不能仅因为发生 resize 就要求模型拆成两次 import。
- 缩小组时同次期望连接必须与最终接口一致，不能保留指向已移除 Pin 的关系或依赖重建时静默丢线。尚不知道新接口时可先应用并重新 export 获取信息；这是按需发现，不是固定两阶段协议。
- 旧 Sequence 专用字段 `exec_output_pin_count` 已退出 authoring 合同，输入时明确拒绝；不要保留双写兼容。


## Details Grouped Authoring

UE Details、UObject 属性面板和资产设置面板可编辑字段统一走公共 Details authoring 层。

- 本节的 `details/index.hjson` + group files 是显式 Details authoring 载体，不是 semantic workspace 的
  `properties.hjson`。后者只在同一文件用 `categories` 分组，并继续由隐藏 manifest 恢复 property unit；
  两者不得互转为第二编辑入口。
- 默认导出不在入口 HJSON 内展开 `properties[]`、`settings_properties[]` 等 Details 描述数组；入口只保留身份、业务结构和 `details` / `*_details` 引用。
- 只有 owning domain 的当前导出或 detail 明确允许时，创建 seed 才能使用结构正确的 inline `properties[]` / `*_properties[]` 写初始值。当前 ordinary Blueprint component-tree 不在首批 row 混入 properties；先用顶层 `name|id` 创建并重新 export，再编辑新导出的 property units。
- 支持 `describe` 的候选查询直接返回同一创建格式的 `description.properties[]`，保留原生字段类型和值；选取需要的 descriptor 并编辑 `value` 即可。公共 Details 读取区分缺省字段与显式错误载荷，错误 descriptor 独立汇总，导入在原始 HJSON 节点上绑定文件、指针和行列；普通属性文件的 `categories` 分组布局不变。
- 同一 owner 只能有一个属性来源：inline 字段与对应 `details` / `*_details` ref 同时存在时，在修改资产前返回 `inline_grouped_details_conflict`，不覆盖、不合并、不猜优先级。
- 分组文件写入相邻 `details/<owner_scope>/index.hjson` 和同目录 group 文件；Level Actor/Component/WorldSettings 可直接使用对象目录下的 `details/index.hjson`。
- `details` / `*_details` ref 里的 `category` 只是阅读和调试 metadata，不参与目录分层；不要创建 `details/<category>/<owner_scope>` 这类旧路径。
- owner scope 会经过共享文件名 stem 清洗，避免 `|`、冒号、空格等字符污染 Windows 路径。
- `details/index.hjson` 是导航和完整性摘要；实际可编辑字段在 group 文件的 `fields[]` 中。
- group field 默认保留 `property_path`、`display_name`、`kind` 和 canonical `value`；字段受禁用或编辑条件约束时保留 `availability`，有编辑意义时保留 `tooltip` 和 enum/object/class `candidates`。
- grouped Details authoring 不输出 `category_path` 或复数 `operations`；分组由文件和 `details/index.hjson` 表达。它们只允许作为 debug/readback 证据，不是 authoring 输入。
- 展示型 `value_text` 只在 debug/readback profile，或确实无法提供可靠结构化 `value` 时保留。
- 为减少重复，默认不保留和 canonical `value` 等价的展示副本，例如 `value_text`、`default_value_text`；authoring apply 只读取 `value`，不从非法或缺失的 `value` 回退到文本。
- `raw_properties`、`readonly_properties`、`missing_properties`、`unsupported_properties` 是诊断或只读材料，不会被公共层自动当作可编辑 Details 外置。
- apply/validate 对 inline seed 和 grouped Details 使用同一公共解析、冲突检查和属性导入链路；diff 和已有 owner 的持续编辑仍以导出的 grouped Details 为稳定来源。
- 只返回 response、没有稳定输出文件边界的命令仍返回 inline JSON；它们必须走同一 compact writer，但不会伪造 folder `details/` 文件。

## Structured HJSON Navigation And Formatting

- 当前公共 authoring 方言固定为 `uai-hjson-authoring/1`，由 C++ 服务端公共 codec 解释。SDK 传递文本和托管 workspace，不维护客户端第二套解析语义；不得把通用第三方 HJSON parser 的宽松行为当作 UAI authoring 合同。
- 公共 formatter 的紧凑对象/数组字符串加引号；输入 parser 接受合法逐行裸字符串，例如 `name: Create Points Grid`、`type: CustomDefine[]`，保留内部空格、标点、引号和注释样文本，排除尾随空白。裸字符串延续到行末；数字、布尔值和 null 在逗号、闭合边界或注释前仍作为标量。字符串需要同行后续字段时使用双引号。canonical 输出可以更保守地加引号，输出偏好不等于输入拒绝规则；第三方 parser 不能替代服务端精确数值合同。
- 公共文件入口只接受 object root。整数在 AST 中保留任意精度十进制文本；有限浮点按 IEEE-754 binary64 语义输出最短可回读表示，并保留 `-0.0` 与整数形浮点的类型。`NaN`、`Infinity`、`-Infinity` 在读取和写出两侧都拒绝。
- canonical writer 固定 key 排序、二空格缩进、LF 换行与结尾单换行；writer 写出的文档必须能被同方言 reader 重新读取。普通 SDK 消费者无需在 Python 中重建 canonical bytes 或完整性信息。
- 方言支持三单引号 multiline string。开始分隔符后只能有空白和换行；结束分隔符单独占行；非空内容缩进不得小于结束分隔符缩进。canonical writer 可以把 multiline 值写成带转义的双引号字符串，不要求保留原始文本布局。
- parser、writer 和结构扫描使用显式 frame/task stack，并沿完整输入推进。
- Object Apply 与 PropertyValueCodec 按真实反射类型递归处理 UObject、UStruct、array、set、map、fixed array 和 optional；循环保护依据当前 active object identity path。
- create/replace 的缺失值只从 UE factory、constructor、CDO/archetype/template、`UScriptStruct::InitializeStruct`、graph schema 或真实 staged object 获得。provider shorthand 先解码到 typed property storage，再从该 storage canonical encode/readback；多态 owned object 没有 existing object、显式兼容 class 或唯一可靠 class source 时返回 `owned_object_class_source_required`，不猜基类、第一候选或业务默认。
- parse/write/IO 失败必须包含 `dialect`、`phase`、稳定 `code`、`pointer`、`line`、`column` 与 source file（适用时）。LF、CRLF、CR 的位置计算一致；禁止用自由文本匹配替代稳定错误字段。
- 公共文件写入使用同目录临时文件，完成 UTF-8 写入和 flush 后重新 parse，再以 replace 发布；任一步失败都清理临时文件并保持既有 final 不变。C++ 调用方应使用 `TryWrite*` / `SaveObject` 获取失败，不应把 legacy `Write*` 返回的空字符串当成功。
- folder HJSON 按 ExportProfile `included_files`、教程 `read_order` 和实际 ref 导航到目标 authoring unit 与必要 owner context；不存在的 manifest/index/references 不得由调用方补造。默认不读取 sibling、raw、readonly、debug、coverage 或完整 readback。
- 普通编辑通过 `content_graph.export_workspace(...)` 取得 workspace handle，只修改其可见 HJSON，再调用 `workspace.import_changes()`；服务端负责方言解析和验证。已有单文件使用 `content_graph.import_file(...)`，已有 HJSON/JSON 文本使用 `content_graph.import_inline(target, document)`；`document` 是字符串。完整 SDK 用法见 [PYTHON_SDK_AND_CLI.md](../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md)。
- 小范围人工修改可以直接编辑目标 HJSON，但写后必须重新 parse；批量或跨文件修改必须使用结构化 parser/writer，不使用正则、全局字符串替换或手工转义。
- 公共路径 resolver 负责 owner scope、Details group 和 ref 的目录映射；不得重复拼接 category、owner 或 section，不能生成重复中间目录。
- directory workspace load 使用单次 stat visitor 完整枚举，`enumeration.entries_visited` 与 `enumeration.bytes_visited` 只报告观察量，不参与拒绝。路径越界、reparse、无效 stat、未知文件和大小写冲突仍按各自真实错误失败。
- Windows workspace 路径在读写前通过 handle final path 重新执行 root containment。workspace root 或内部 reparse directory 默认拒绝；junction 指向 root 外返回 `workspace_reparse_escape`，无法可信解析返回 `workspace_reparse_resolution_failed`。该规则同时适用于 directory/single-file load、reference、copy 和尚不存在目标的 write/export 路径。
- 共享 Export writer 不直接逐文件修改 final。它先完成完整 plan 和路径/冲突/token 检查，再在 final 同一父目录建立 same-volume staging，写入并重新 parse 全部 HJSON；发布时以 journal 记录 `final -> backup -> staging -> final`，任一步失败恢复旧 final。single-file 也走同一 staging/backup/journal 语义。
- 非空 final 必须带有效 `.uai-export-owner`（single-file 使用相邻 owner sidecar）；记录包含稳定 owner UUID、contract/profile、workspace kind、final path 和格式版本。无 ownership 的非空目录、越界路径或 reparse 目标在创建、删除、copy 或发布前拒绝。
- directory Export 生成的根级 `.uai-export-owner` 是 publisher metadata，不是 authoring unit。Workspace Loader 在未知文件检查以及文件数/总字节预算计数前只忽略这个精确根级 marker；其他未知文件、子目录中的同名文件和伪造 sibling 仍按 contract 拒绝。因而共享 Export 产生的 workspace 可以原样作为 `content_graph.import_workspace` 的 directory source。
- 成功的共享 `content_graph.export_workspace` 默认只向模型返回 `{"status":"succeeded"}`；workspace 路径、entry、publisher transaction receipt 及其余完整性字段留在隐藏 machine manifest 与内部验证中。verification/readback 调用方不得把 authoring input 当 readback 输出，也不得重新创建已废弃的 `runtimeLogs/` 或 `reports/`。
- HJSON artifact 不能只凭路径存在判定成功。内部 verification 必须绑定当前 request、live server session 与调用时间，并验证 project-contained root、相对 entry、完整且无额外文件的集合、规范化 size/hash、逐文件 HJSON parse、profile、asset identity、contract、same-volume publisher 与 workspace validation；这些字段不进入默认模型结果。
- 刷新输入目录时，publisher 收据按实际来源区分生成文件与保留的用户文件。旧生成分片即使已脱离当前引用，也属于待替换/退出集合；不能仅用当前 `content_ref`、`properties_ref` 或 `interface_ref` 判断历史文件归属。用户额外文件在连续刷新后仍保持用户归属。旧收据缺少分类时保守保留未知文件，只允许与新规范文件同路径且逐字节相同的内容合并，不按后缀推断可删除文件。
- 只有一次提交完整 plan 的 exporter 才能声称整个 workspace 原子发布；复合 exporter若仍分多次提交 fragment，必须先在领域层聚合为单一 plan。
- Apply workspace 的 `reference_policy.external_references` 当前唯一 active 值是 `reject`。历史 `contract_allowlist` 没有完整 allowed-root、semantic scope 和 read/write 权限实现，因此解析或注册时返回 `apply_workspace_external_reference_policy_unimplemented:contract_allowlist`；不得把该值当成已支持能力。
- save-required Apply 使用 request-scoped durable journal。包从磁盘加载时，operation transaction backend 与默认 package stager 都会在发布前释放 package loader；staging 或 publish 失败必须从 backup 恢复并确认 hash，不能因 `.uasset` Linker 锁把已恢复状态报告为可安全重试。
- durable publish 只有在 provider commit 与 completed journal 均成功后才清除已持久化 package 的 dirty 标记；commit 失败或回滚路径恢复进入事务前的 dirty 状态。同一 `request_id` 与相同 fingerprint 的重放从 completed receipt 解析，不重复 mutation 或改写包/receipt；相同 ID 配不同 fingerprint 必须拒绝。
- compact writer 对 object 递归计算有效字段数：scalar 计 1；不含 object 的 list 在所属 object 中计 1 且列表同行；嵌套 object 的字段递归累计。有效字段数 `<= 6` 且所有子值可同行时 object 同行，否则折行。含 object 的 list 不作为普通同行列表。
- 字符串是否引用由 writer 的 HJSON 语法安全判断。注释符、引号、反斜杠、冒号、边界符、换行或 Unicode 不得因同行输出而改变值；重要写入执行 parse -> write -> parse round-trip。

## Field Rules

1. 已知路径实体用 `path`，无路径候选用 `name`。不要让 LLM 手填 `full_name` 或细分旧路径字段。
2. 普通 workspace 属性通过导出给出的 Category 与属性 key 定位；其他领域按其实际导出的可读 selector 定位。不要把底层 descriptor 的 `property_path` 补进每个普通属性 row。
3. `required_readonly` 字段可以留在 `llm_authoring`，但只用于识别、校验、约束或差异判断，不由 apply 写入。
4. UObject 属性复用 `uai.uobject_properties.v2` 值 codec；canonical shape、精确数值类型、struct 递归和容器内层类型规则见 `UObjectPropertyValueV2.md`。显式 descriptor 的 enum/choice 通过 `candidates`/`candidates_ref` 提供选项；普通 workspace 的候选按其 `definition` 查定义目录，编辑者只写候选值，不把候选表复制进属性 row。
5. 普通 UObject 属性 row 使用 `{type,value}`，导出可附只读 `definition`，编辑现有值时保留类型与引用。不强制携带 `property_path`、`kind` 或重复名称。显式 Details descriptor 载体的字段规则只适用于该载体，不得覆盖普通 workspace 形状；领域条目的可读 selector 按真实合同保留。
6. `operation` 只用于 add/remove/replace/clear/rename/set_binding 等非普通值意图。
7. 不写 derived count、export timestamp、file path、compile report、runtime probe、coverage report、roundtrip status 或 raw enum options。
8. 不把 `raw_properties`、`readonly_properties`、`missing_properties` 或 `unsupported_properties` 当成通用可写面；这些字段不作为 apply 输入。
9. unknown/unsupported operation 必须在 adapter 或 validate 阶段失败，不得静默丢弃用户意图。

## Coverage Boundaries

普通 workspace 的解析、类型/引用检查、变更规划和应用由统一 import 入口负责。缺少某类资产专属
`*_validate_folder` 或 `*_diff_folder` 不构成公共编辑能力缺口，也不要求恢复这些旧入口。
支持范围由实际目标的导出、当前输入合同和真实应用/读回共同确定；注册、文档存在或一次返回成功
都不能替代对应行为的验证。新增、删除、持久化和失败恢复各自只声明已证范围。

MediaPlayer 的可编辑设置使用同一 workspace 流程，在实际导出的 `categories` 中修改 `Loop`、
`PlayOnOpen` 等字段，再 `import_changes()` 并独立导出核对。创建新 MediaPlayer 时先查询
`asset.factory_candidates("asset.object.create", query="MediaPlayer")`；若返回同名创建方式
消歧，使用其公开 `creation` selector。无交互的空播放器可选返回的 `constructor`；`factory` 需要
交互时会明确拒绝。设置编辑不等同于打开媒体、播放或捕获，也不要求创建配套 MediaTexture。

实现一致性入口、失败 issue 字段、command toolchain audit 和不得宽泛搜索源码/旧文档的规则见 `hjson-authoring/07_ImplementationConsistency.md`。

## Standard Loop

1. 导出或读取固定 fixture。
2. 读取导航文件，按需打开目标 `llm_authoring` 文件。
3. 只编辑 authoring 文件，不改 readback/debug/legacy。
4. 新增内容时调用对应领域 candidate command；已迁移的领域只写
   `candidate:{name, select?}`，其中 `select` 只包含消除歧义所需的短语义字段；外层 context 不混入
   candidate。尚未迁移的领域严格按当前合同处理并记录缺口，不从显示文本猜 identity。
5. 按当前公开合同使用必要的预检；普通 workspace 的验证由 import 完成，不以 dedicated validate 为固定前置。
6. 调用 workspace 的 `import_changes()`，检查公开结果。
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
| Destructive edit | explicit destructive request, dry-run or plan first, readback after apply. |
| Long-tail config | apply through its owning command and read back the affected config state; do not treat packaging/config output as persistent asset state unless command reports it. |
