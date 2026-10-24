# Validation

本页区分导入前的输入诊断、导入后的非阻断一致性观察，以及开发阶段的独立验证。输入错误由导入返回；UE 合法的归一化和差量存储不构成输入错误。

## 基础完成证据

普通结构化 HJSON 编辑使用正式 import/apply，消费该次结果和刷新后的 workspace；需要确认具体行为时再读回相关状态。JSON/HJSON 解析与合同校验由同一个服务端 owner 在 import 内完成，不要求调用方先运行 lint、dedicated validate、dry-run 或额外全量比较。破坏性编辑按所属公开命令的合同执行。

## Import 合同错误

预检失败必须有非空错误码，并保留最内层的结构化原因、独立诊断和未检查范围。公共事务将解析时的来源表用于定位；分片来源已有精确位置时保留原位置，普通内联文档也返回对应 JSON Pointer 和可用的行列。缺失字段定位到应填写的路径，行列可落在其所属对象。领域不得用字符串拆分截断结构化诊断，也不得只返回外层通用失败而丢弃原因。

错误产生处构造 `FDiagnostic`，明确设置公开原因和可公开的对象、候选、字段或值事实；内部上下文单独保存。后续 owner 整体传递诊断，只补缺失来源、阶段、未检查项、独立失败或执行/回滚事实。确有独立外层分类时保留完整 `cause`，不从 `Code/InternalContext` 重建。来源和 `unchecked` 必须在返回该诊断之前补齐。
公开原因是必需的构造输入，不从错误码生成句子。`unchecked` 保留实际未检查范围和来源，包括嵌套原因；公开诊断不再用派生的 `complete` 布尔值声称覆盖完整。

内存中的 workspace、query、operation 和异步结果使用同一结构化诊断，不经过 `code:detail`、内部 JSON 字符串或重复的 ErrorCode/ErrorDetail 接口。序列化只位于公开 wire、持久化和最终 report/log 边界；SDK/CLI 直接消费 `error.diagnostic`，不从私有文本恢复原因。公开 `message` 与内部上下文分别投影，取消、重试、写入、回滚和终态事实不因包装错误而重置。

服务端 import 在写入前直接返回解析或合同错误，包括 readback/debug/diagnostics/legacy 字段混入 `llm_authoring`、无效 enum、`value` 形态与 `kind` 不匹配、未实现 operation、空间值不符合当前领域 canonical shape，或 legacy alias 混入 authoring 主面。调用方直接处理该次错误。
- 默认字段项继续输出 `name/authored_name/cpp_type/enum_options` 等重复或 legacy 字段。
- 派生 count、file path、coverage report、compile report 被当成可写字段。
- raw config、export timestamp、coverage report 等 report/debug 字段进入默认 authoring。
- path-backed seed 使用旧 `full_name`、`asset_path`、`class_path`、`object_path`、`component_class_path`、`script_asset_path` 或 `module_script_path` 作为主输入。
- 仅凭显示 `name` 写入本应有路径的自定义资产、类或对象。
- 领域 candidate 的最小可读 `name`、消除歧义所需的短语义 `select` 或候选外层 context 被放到与 owning domain/profile 不一致的位置；机器 identity 与 binding revision 不进入模型文档。
- 忽略 `write_support/support_reason`，把候选用于不支持的写入场景。

## Adapter 必须拦截

- selector 找不到或匹配多个。
- source/operation 不支持。
- dynamic input 缺 `path`/`name` seed，或 path 无法解析到真实 script asset。
- linked input 缺目标参数。
- enum selected 不在候选项中。
- 空间值无法按当前领域合同解析或读回到同一 frame。
- 会丢用户意图的降级转换。
- Niagara module seed 指向的 module 不在当前导出 stage 中，却被当作新增 stack module 成功。

## Dedicated Validate / Diff Coverage

当前 folder validate/diff 覆盖不是完整矩阵。

| 类型 | dedicated validate | dedicated diff |
| --- | --- | --- |
| `blueprint` | missing | missing |
| `umg` | missing | missing |
| `anim_blueprint` | missing | missing |
| `material` | missing | missing |
| `material_instance` | missing | missing |
| `material_function` | missing | missing |
| `niagara_system` | missing | missing |
| `niagara_emitter` | missing | missing |
| `niagara_script` | missing | missing |
| `sequence` | missing | missing |

AI 行为树相关既有 folder 文档存在，但 contract operation registration 仍为 `missing`；不得把这些文档写成完整闭环能力。

按本次改动实际需要使用以下直接证据，不把 dedicated validate、测试或全局检查设为固定前置：

- command response 和 report。
- export/readback 后目标 JSON 的语义比较。
- 受影响行为确实需要时的 domain compile、stack issue 或 probe。
- 必要时对两份 folder package 做结构化文件比较，但必须写明不是 dedicated diff coverage。

公开入口和 owner 一致性见 `07_ImplementationConsistency.md`。

## Apply 后读回

一致性观察以实际编辑意图为范围，复用正式类型 codec。判断用户改动时保留存储差异；比较原生差量表示时，从预期存储和实际存储分别在临时值上按 UE 原生规则恢复有效值。双方各自使用原生默认值，不能用预期补写实际一侧。组件模板的瞬态缓存可能等待加载修复，不能把缓存已重建设成资产导入的额外要求；实际运行效果属于其生命周期下的独立验证。也不能把所有空数组解释成默认值。

普通反射属性由公共 ObjectApply 消费创建候选、实例绑定和数组来源信息，冻结已解码的原始意图。
最终读回复用这份意图检查当前 root 及嵌套 owned 对象，并独立核对原始请求的 delta 仍包含在写入器收到的输入中。
内部 `from` 和 `owned_object` 创建语法不再作为原生值重新送入 codec；原生归一化仍须有其所属 owner 的独立证明。

例如 `FCollisionResponse.ResponseArray` 保存相对默认响应容器的差量，空数组不表示没有响应。公共 codec 对整个结构、嵌套结构和带原生父容器的成员核验使用同一响应语义；普通数组仍按自身元素比较。原生归一化的其他生命周期效果继续使用已有来源证明。一致性不符作为 warning 返回，不撤销已完成的原生操作、不覆盖 UE 状态，也不冒充输入错误；开发阶段用独立正反例检查转换与读回是否正确。

| 编辑类型 | 验证 |
| --- | --- |
| 普通值 | 读回值 exact match 或可解释的 UE normalized match。 |
| Enum | 读回 selected/write value，并用 display label 辅助确认语义。 |
| Graph | `graph.hjson` 中节点/连线/pin default 读回；若改 layout，再读 `layout.hjson`；必要时 compile。 |
| Niagara | module input 读回、active branch/mode、Stack issue、compile log；视觉相关再 runtime probe/screenshot。 |
| 曲线 | key 数量、时间、值、插值、tangent、外推模式读回。 |
| 空间字段 | 同一 subject、同一 frame、同一 state 读回。 |
| ProjectSettings | 正式 update 结果、受影响字段 readback，必要时记录重启影响。 |
| destructive edit | dry-run/plan、显式删除请求、删除后 readback。 |
| catalog seed 新增 | readback 中出现目标 component/widget/node/value，并补齐 class/template/slot/pin/properties 或 domain readback 字段。 |

## Warning 处理

Warning 不等于可以忽略。以下 warning 必须回到 authoring 修正：

- `hidden_or_inactive_branch`
- `unsupported_write_policy`
- `dynamic_input_source_missing_script_asset_path`
- `linked_input_source_missing_parameter`
- `module_input_link_apply_verification_failed`
- `property_import_status=import_failed`
- `value_text_changed_after_import` 且无法解释为 UE 正常规范化

## Editor 和进程

- 普通验证复用当前 UE Editor。
- 不因普通验证完成而关闭用户已有 UE。
- 需要编译插件/UE C++ 时，先处理 dirty resource，再关闭 UE 构建。
- 如果验证脚本自己启动了临时 Editor 或 `UnrealEditor-Cmd`，脚本结束必须清理。
- 每次结束确认无 `CrashReportClient` 残留。
