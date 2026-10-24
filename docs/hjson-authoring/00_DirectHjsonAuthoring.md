# Direct HJSON Authoring And Structured Apply Chain

本页描述普通 `llm_authoring` workspace 的编辑流程。实际字段以本次公开导出和所属领域合同为准；
SDK 用法见 [PYTHON_SDK_AND_CLI.md](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md)，
公共字段与生命周期见 [HjsonAuthoring_Guide.md](../HjsonAuthoring_Guide.md)。

## 主链路

1. 选择真实资产，用公开 `client.content_graph.export_workspace(target, name="current", profile_id="llm_authoring")`
   取得 workspace；先检查 `workspace.result.ok`。
2. 只读取 `workspace.path` 下本次需要的普通 HJSON，保留原文件组织及导出的可读引用。
3. 修改现有属性时只编辑属性 row 的 `value`，保留导出的 `type` 和可选 `definition`；空集合和新增结构先按需查 `examples/index.hjson` 对应条目，再查询所属领域 candidate，将返回的可读名称
   写到该领域导出合同的位置。候选查询不替代创建、关系、布局或属性意图。
4. 调用 `workspace.import_changes()`，检查 `Result.ok`；失败时读取公开错误，停止依赖失败结果。
5. 成功导入且 `workspace_refresh.status=refreshed` 后，直接读取当前刷新目录或对应 Query，核对目标结果与非目标保全。
   持久化、compile/settle、运行时或视觉证据按实际行为选择，authoring 输入不作为读回。

普通编辑不需要领域专属 validate/diff 命令作为前置。服务端公共 import 负责解析、身份/类型/引用检查、
变更规划、领域应用和必要持久读回。Python 可以生成文本或调用 SDK，但不复制上述业务过程。
生成示例的阅读与编辑边界见 [Generated Examples](09_GeneratedExamples.md)；示例不是需要导入的模板文件。

### Editor / Project Settings

`project.settings` 的 index/page 返回设置页的 `settings_object_path`，可作为同一
`content_graph.export_workspace` / `open_workspace(...).import_changes()` 的目标。
已登记 Settings 容器（包括 Editor / Project）中的原生配置对象按公共 `core.property` 导出；只修改导出的 `categories` 属性值，
不编辑 ini 或制造配置专用 HJSON。Localization 的结构化目标集合继续由原适配器负责。

导入复用公共属性解析、差量编辑及原子文件保存，触发 UE 原生属性和设置页修改通知。配置键取原生
`ConsoleVariable` metadata，并沿用设置类的 Default/GlobalUser/ProjectUser/普通 Config 保存作用域。
未改配置保留；持久读回从实际配置文件加载，并刷新当前配置缓存。
`settings_effects` 逐项返回 `property`、`restart_required` 和 `status`；有 CVar 的属性同时返回
`console_variable`、`configured_value`、`effective_value`。`applied_live` 是实时值已匹配；
`native_change_notified` 只证明原生通知已完成，不推断所有子系统立即重建；`saved_restart_required`
沿用 UE 原生重启标记；`saved_live_value_differs` 表示当前 CVar 被其他优先级覆盖或尚未应用。
只含自定义界面、没有配置 UObject 的页面不生成虚假的属性 workspace。

## 身份、属性与来源

- 已有对象使用导出的可读 key/ref；同名对象按导出给出的作用域与编号区分，不自行发明编号。
- GUID、hash、revision、unit identity 和对应映射由隐藏 workspace 管理；不读取或编辑 `.uai`，
  不把机器身份写进 HJSON、文件名或 Python 参数。
- UObject 普通属性在同一文件的 `categories` 内，以属性名组织 `{type,value}`，导出可附只读 `definition`。
  约束/候选/元素模板按 `definition` 查所属定义目录，类型与引用的查找规则见 [公共属性合同](02_CommonInputValue.md#semantic-workspace-category-grouping)；不要把底层 `property_path/kind` descriptor 补进普通 row。
- 已有字段省略表示保持。新建对象按公开字段合同的默认值和必填项初始化；null、空集合、缺省不同。
  集合内部删除和完整期望集合的含义见 [03_OperationsAndSelectors.md](03_OperationsAndSelectors.md)。
- 目录 workspace 使用原隐藏基线。没有该基线的 single-file/inline 只用于当前合同允许的 missing
  初始化，不用于绕过已有对象的并发校验。`import_file`/`import_inline` 的参数使用当前公开 SDK 签名；
  不手工搬运绝对 workspace_path 或机器 expectation。

## 新增结构与候选

查询返回的最小可读 `name` 选择原生创建能力；只有返回同名 refine 时才附加最短 `select`。
实例名、parent、graph、stage、position、属性和连线属于编辑意图，不混入 candidate。
显示 tooltip/category、类名猜测或完整查询 envelope 都不能代替公开候选。

不同领域的文件形状由各自导出决定，不能把一个领域的 `candidate` 包装推广到所有对象。
例如 ordinary Blueprint component-tree 在 `components` map 中用 key 表达实例名，row 顶层填写名称：

```hjson
components: {
  DoorLight: {
    name: "Point Light"
    parent: DefaultSceneRoot
  }
}
```

此片段只在当前导出的 component-tree 位置使用；候选必须由本次查询确认。
新建 row 可携带已知可写字段的内联 `properties`，导入后归回普通 property units；字段未知时先最小创建再读取导出。
省略 `parent` 会挂到当前根；显式 `parent: ""` 请求把该场景组件提升为根，原生操作会替换 DefaultSceneRoot。
不要把直接删除 DefaultSceneRoot 当成根替换。

Blueprint graph 使用本次导出中的 `nodes` map。新 key 是图内实例 alias，候选与布局位于 row：

```hjson
nodes: {
  InterpolateDoor: {
    candidate: { name: "Interpolate" }
    pos: { x: 480, y: 80 }
  }
}
```

示例名称必须由目标图的真实 candidate 查询取得，不保证所有上下文都有该名称。
不填写 `id/guid/node_guid/operation`；服务器按隐藏基线与期望状态形成创建、修改或删除。
Blueprint 已知节点可沿用真实导出的 `node_type` 与目标字段声明新实例（例如
`call_function` 的 `function_owner_class/function_name`，本蓝图函数使用 `function_graph`）。
不同 map key 表示不同实例，允许多次调用同一函数或访问同一变量；未知原生 action 仍先查询 candidate。

## 图接口、默认值与连接

Blueprint 图头中的 `display_name`（与原生名不同时）及 `parent_graph` 是 UE 原生显示名称和子图归属的只读导航。
例如普通过渡图可显示 `Idle to Move (rule)` 并归于 `Locomotion`；按实际显示名定位，不按 `Transition2` 等后缀猜创建顺序。
共享规则使用 UE 原生共享名称，不把它误读为仅属于一对状态。图编辑仍使用导出的节点、属性与连接合同。
重名图还导出只读 `query_name`，作为 `blueprint.node_candidates` 的 graph 参数；未返回时使用 `name`。
该值是现有原生对象路径查询，不需要模型搬运 GUID；导入仍由服务器保留捕获身份。

Blueprint 的普通连接使用同图 alias 和实际 Pin 名，例如：

```hjson
connections: [
  { from: "InterpolateDoor.ReturnValue", to: "SetDoorRotation.NewRotation" }
]
```

两端节点必须在同一图中存在或明确声明创建；只写端点不会隐式创建事件/入口节点。
已知合法接口时可以同次提交新节点和连接。服务端按依赖建立节点、重捕获接口再解析连接；
缺少新接口信息时才按需先应用、读取成功刷新后的 workspace 再继续，不能机械要求每个新增节点两次 import。
Blueprint-family 的批量连接由服务端先落实可确定类型的连接，再处理依赖它们的通配符连接，
并完成原生图编辑的连接通知与输入替换；调用者不必按手工拖线顺序排列数组或手动指定提升后的函数。
`connections` 字段省略表示保持；显式提交完整期望数组时，要保留的既有连线也必须保留在数组中。

Pin 默认值在本次导出的节点 `items/<节点>/properties.hjson` 中编辑，使用
`categories.General.<PinAlias>` 的 `{type,value}`，保留该属性行导出的精确类型标签。
节点的 `properties_ref` 给出相对于当前图文档的属性文件路径；使用该导航定位属性，
不要按节点名称猜目录。它不属于节点业务属性，导入时不产生修改。
从只读 `interface_ref` companion 确认接口方向与同名消歧；它不能代替属性行的值编码。
不要在图节点声明内添加旧 `pin_defaults/default_value` 或 Pin GUID 包装。
Blueprint 的 wildcard/promotable Pin 可能在连线确定类型后才出现可编辑默认值属性行。
若该行尚未导出，先提交确定类型的连接，再从刷新目录读取属性行并填写默认值；
不要猜类型标签或使用内部 `pin_defaults` 补写。已经公开的接口与属性可在同次编辑中一起提交，
不因节点类别机械拆分请求。
可变 Pin 组与受影响默认值/连接由同一 prepared mutation 协调；不得保留指向最终已删除 Pin 的关系。

原生 action 可创建主节点、伴生节点与默认连接。模型只声明一次候选操作，不把伴生节点拆成第二套
手工创建流程；后续编辑使用真实读回。Material/其他领域的拉线候选上下文使用各自公开的可读引用，
不手工重建 `node_guid + pin_guid`。

Blueprint宏候选使用具体宏的可读名称，不使用通用`Macro Instance`代替。创建后的`macro_graph`
保留真实绑定：同一Blueprint内的宏使用本地名，外部宏库使用导出的宏图引用。后续局部编辑保留该字段，
不从宏名猜测所属库。旧短名只能从同一既有节点的真实外部绑定恢复，不是新建外部宏的完整身份。

## 领域差异与支持边界

- Material expression、ControlRig RigVM、Niagara Stage/module、UMG WidgetTree、AI 图和 Audio 图
  保留原生结构、归属、候选与编译差异；以当前导出和对应命令文档获取具体形状。
- 普通 workspace 不嵌入旧 typed `command_id/params`、内部 `semantic_id`、operation records
  或完整 provider_extension readback 来绕过公共模型。内部 mechanics 存在不证明该形状是模型入口。
- 可读 candidate 可被搜索不等于当前创建支持；按公开支持说明处理，不能猜测未支持 action。
- SoundCue/MetaSound 完整原生菜单与 UMG 完整 Palette 的受限范围不代表其他已支持编辑不可用；
  局部成功也不证明原生菜单全集等价。当前实现/验收范围由项目正式计划与真实结果限定。
- Playback、采样、渲染/捕获及外部格式转换是独立职责，不能通过修改媒体设置冒充播放操作。

## 失败处理

解析、类型、候选歧义、悬空引用、stale 基线和不支持操作都应返回可定位错误，不静默丢弃输入。
公共 HJSON 解析与导入使用 `uai.authoring.diagnostic/1`：保留首条 `code`，并通过 `diagnostics` 汇总多个
可靠错误；每项包含 `code/message` 及已知的 `source_file/line/column/json_pointer` 或 `target/property_path`。
尚未绑定源文件的输入错误使用 `semantic_pointer` 指出语义字段；它不是文件内的位置，不能据此猜造
`source_file` 或行列。候选解析、查询与领域交接保留已有精确位置；补充恢复事实时继续保持结构化诊断。
领域诊断声明具体字段时，workspace 根据原始解析节点继续细化已有容器位置，返回实际分片文件与字段行列。
缺失字段只定位到存在的父级并标为 `source_kind: related_input`；不把父级行号当作不存在字段的精确位置。
`message` 简述失败原因，具体请求值、期望类型或允许值分别放在 `actual_value/expected_value`，避免重复回显输入和内部上下文。
公共 keyed 集合的递归字段合并和文档预投影沿用原始语法节点，包括引用分片及含 `/`、`~` 的字段；
诊断定位到实际编辑文件。预投影提前结束时标出尚未检查的文档/属性与变更规划范围。
目录输入检查本次 inventory 与实际引用的内容，不把目录中未引用的用户文件当作导入内容。
独立分片、语义单元、图节点/布局、初始化变更和公共领域预检查继续收集；同一文件仅在明确分隔符/容器边界处恢复，不修补或应用
错误输入。无法可靠继续的范围在各级 `unchecked` 中以 `stage/reason` 和已知来源标明；也应查看嵌套
`cause` 与 `diagnostics` 的未检查范围。不再返回由空列表推导的 `complete` 布尔值：没有列出未检查项
不能证明诊断已充分或所有检查均已完成，也不表示资产可应用或运行验证通过。无效依赖不会衍生成大量“引用不存在”的假错误。
领域 provider 内部仍可能在首个语义错误处结束该单元检查，此时明确报告 `provider_stopped_at_error`，
其余无依赖单元继续预检查。公共 ObjectApply 的独立属性不存在、类型声明与值解码失败会汇总，并沿属性来源
返回文件/字段定位；图节点内联 `properties` 同样保留解析时的真实文件、指针和行列，不以内部投影的
`inline://` 替代输入来源。依赖这些值的检查标为未检查。创建、编译及运行时错误仍在实际阶段报告，静态诊断不承诺提前发现它们。
先检查是否到达预期对象与阶段，再决定修正输入或报告产品问题；不通过删除用户要求的内容使测试变绿。
已落盘后的失败需要同一事务恢复与独立读回，回调执行成功不等于恢复已验证；未能确认恢复时保留真实
失败/未知终态。新的普通编辑从有效的重新导出开始，不能靠修改隐藏身份或旧文件强行重放。
回滚自身失败时，`diagnostics` 同时保留原始失败和恢复失败各自的原因、阶段及已有来源；顶层仍以原始
失败为主因，并以 `rollback_succeeded=false`、`transaction_status=state_unknown`、`retry_safe=false` 表明恢复未得到确认。
