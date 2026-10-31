# UeAgentInterface 当前能力与边界

本文记录统一框架的当前正式状态。它不以旧 CommandSpec、旧 handler 数量或 generated
目录数量推导可用性；命令全集以 `Resources/UaiCommandManifest.json` 为准，单条命令
的 production-ready 状态以对应真实纵向证据为准。

## 当前公开边界

- 正式 active command 集合与数量在读取时从 `Resources/UaiCommandManifest.json` 动态取得；
  command ID 必须唯一，长期文档不固化易过期数量。
- 业务请求只经 `POST /api/uai/command` 进入正式 CommandService。
- HTTP 只是当前 Transport Adapter；Protocol、Client、CommandService 和 C++ owner
  不依赖 HTTP 业务类型，可在保持上层调用不变的情况下增加其他 transport。
- Python SDK/CLI 只转发一条真实 command，不拥有 UE、HJSON 或领域业务。
- dotted canonical command 不进入旧 underscore prefix fallback。
- 已迁移 legacy command ID 在正式 registry 中 fail closed；旧业务 ingress 不再绑定。
- ping/status/listener 只承担连接和服务状态，不是业务第二入口。

manifest 或 generated binding 只证明其读取时公开结构存在，不代表其中全部 command 已经获得
同等级的真实 UE 纵向证明。对外声明能力时只引用当前正式入口的实际结果与必要独立读回。

## 统一模块职责

| 模块 | 唯一职责 | 当前验证重点 |
| --- | --- | --- |
| NetworkTransport | 连接、认证、wire request/response；当前实现 HTTP Adapter | 认证、协议、失败映射，不含领域业务 |
| CommandService / Registry | canonical ID、binding、正式 dispatch、legacy fail-closed | public ingress 与唯一 owner |
| Read / Export | 无副作用查询、投影、分页、artifact export | complete/partial 语义与来源可追踪 |
| Reference CRUD | 通用目标解析和 typed CRUD | create/update/delete、持久化、独立读回 |
| SimpleAuthoring | typed 属性与项目配置 | 字段合同、原生默认、保存与读回 |
| Timeline / Spatial | 时间、空间、序列与世界状态 | frame/subject 不混用、HJSON source parity |
| GraphCompound | 图结构、关系、节点属性与复合资产 | 公共 composition、compile/settle、持久读回 |
| Destructive / Import | 删除、世界破坏性操作、外部导入 | 完整 preflight、terminal state、rollback/state_unknown |
| LongRunning / Session | operation lifecycle、cancel/recovery、safe exit | 外部 terminal witness 与 cleanup |

跨模块公共能力必须由 composition、事务、持久化、settle 和 readback owner 提供；单条
route 不得复制 parser、直接保存、伪造 readback 或建立第二套业务实现。

## HJSON 架构

HJSON 以 LLM 易用和最小必要字段为目标：

- token 控制通过 profile、字段分层、投影、引用与文件拆分完成。
- 统一对象/属性/关系模型组合类属性、节点属性、图结构、连接和领域扩展。
- folder export 的 `included_files`、`read_order` 和实际 ref 决定读取顺序。
- owned item/structure 使用公共可读别名生成独立目录；缺少显示名时仍按路径或类型回退并消歧，
  不将其属性合并到父目录。UE 子对象路径使用末端对象名，属性冲突保留公开文件与字段定位。
- 定义的公共引用优先使用捕获的类型名称并按需消歧；内部短摘要身份保留在映射中，不作为可读别名回退。
- 同一 owner 的 inline 属性与 grouped details ref 冲突时 fail closed。
- `update` 与 `replace` 语义由服务端合同决定；Python 不补默认值。
- keyed property 导入按捕获类型恢复简写值；已有 `path/literal/selected/items/components/value`
  包装按字段保留，不再次套入同名字段，`owned_object` 创建意图仍走原公共创建链。
- 公共 ObjectApply/DirectApply、领域 authoring、workspace、生命周期和 query/operation 使用同一
  typed diagnostic：源头声明公开原因、字段和值，内部上下文独立保存；后继整体传递并补来源与状态。
  内部 FString 错误载体及序列化后再恢复的路径已退出；JSON 仅留在真实 wire/report/持久化边界。
  下层解码、解析或 native 调用失败与重复/类型校验分开处理，避免通用错误覆盖具体原因。
  已有 `retry_safe=false` 不被预检默认值改成 true；SDK、CLI 和 runner 保留结构化诊断。
  R11 修复复验已证明显式 Cube 引用成功、非法引用通过公开 SDK 返回属性/文件/行列，且失败后
  原 Cube 引用不变；证据位于项目 `tmp/door_plugin_fix/r11-common-fix/`。不等于 R11 整门验收通过。
- import 由唯一 C++ owner 完成 parse、Planning、execution、compile/settle、save 与独立
  readback；失败必须给出明确 terminal state。
- 内容目录的资产、Actor、Component及PlaceActor候选在已有短scope之后补充真实所属路径段，
  公共投影仍取最短唯一可读名称；PlaceActor查询与创建解析共用同一builder。
- Level Actor 准备阶段读取模板标签时不生成缓存标签，避免修改类默认对象或 archetype。
  正常构建中已验证同进程创建后的 PlayerStart 标签往返修改与 PointLight 类名标签持久化；
  不依赖重启绕过，也不对具体 Actor 类作特判。
- 公共 ObjectApply 的模板属性修改向仍继承旧值的实例传播，保留实例覆盖；嵌套字段按叶路径判定。
  受影响实例有本次操作内的恢复快照，跨 Blueprint 重实例化/持久读回按路径重新取得对象，
  不自动保存其他关卡。定向属性传播、覆盖保留与固定数组回滚测试通过；独立 Blueprint/Level
  probe已验证位置覆盖与缩放继承可同时保留，保存重开读回一致。
- Workspace诊断区分HJSON文件定位与资产业务路径，保留target/property_path；AnimBP与AI
  语义投影包装保留WorkspaceError。公开候选失败已能定位具体节点，SDK格式校验不变。
- ContractSchema输入失败通过uai.input.diagnostic/1保留字段路径、schema路径、keyword及消息，
  接入Query/OperationBinding、GraphCompound、SimpleAuthoring和LongRunning公开输入边界。
  C3执行后失败通过uai.operation.failure/1保留收据与阶段事实；delegated消费明确事实，
  不把验证失败误投影为未写入且可重试。SDK只投影这些版本化carrier的受控字段。
- WorldSpatial共享空间输入接受公开compact值，subject由实际target/context绑定；用户坐标
  与带观察证据的frame明确区分，不要求模型编造机器身份或时间。viewport update/restore、
  graph live view往返和ray deproject已通过公开SDK复验；相机与对象变换共用FTransform语义
  比较，q/-q不再误判。旧完整输入兼容及descriptor定向检查通过；未冒充全部空间命令实测。
- 公共 Query 投影按实际 producer 补齐复合字段的对象子项和数组形状，空间值复用统一描述；
  viewport、asset、config/origin、world、partition/data layer及landscape的已定位字段不再被
  当作标量丢弃。DataTable行字段支持标量键值映射，仍不透传任意原始嵌套对象。
  Development构建及既有Query.PublicContract通过；公开SDK已验证视口状态、警告、模式、
  Cube边界和Blueprint关系图。其他补齐字段的运行覆盖不由这些样本替代。
- Blueprint 原生菜单在单次规范化或单个 graph Body 阶段内按相同无引脚上下文复用。
  标准函数、事件和变量先按权威语义名称排除无关候选，再按需展开模板及反射属性；其他
  spawner 保持完整解析。名称投影按查询与语义筛选复用，公共名称索引保留完整相关同名组、
  字面括号和作用域消歧规则。预演与正式资产分别持有目录，编译或 action database 刷新时
  失效；引脚上下文仍即时解析，不跨 UObject 生命周期共享创建对象。
- Blueprint 与 UMG authoring 的 Proposed/Rollback 临时包共用 Blueprint lifecycle owner，
  预检早返、状态退出及持久读回释放均退休自有临时对象，并在原生 PIE 编译发现前按需 GC。
  原生依赖回收只标记 Blueprint 根，不强制销毁生成类、函数或 CDO。两者的 scratch 预检
  共用可取消事务作用域，退出时取消临时 undo 记录；已有编辑事务时零写入返回
  `blueprint_preflight_transaction_active`，不取消用户事务、不影响 Remove。
- 单次公开 import 的各读取阶段共享不可变 HJSON 解析结果。每阶段仍检查路径并读取实际
  文件，只有绝对路径、source unit 与文本完全相同时复用；变化的文本重新解析，请求结束
  即释放，不依赖修改时间、不跨请求缓存。增删改依赖顺序、持久化读回、workspace 刷新、
  公开格式和指令保持原有职责。
- 公共 composition 直接构建语义行对象，序列化与目录投影复用同一 builder，不再为了获得
  JSON 对象而先输出 HJSON 再解析。不可变 authoring model 一次建立属性定义的可读名称索引；
  文档可见性与聚合条目关联也先建立索引，避免按每个条目反复扫描整个模型或集合。
  这些优化作用于共用该链的资产，保留原有文件布局、字段值、定义消歧与内容 hash 语义。
- PCG、Control Rig、Niagara、SoundCue/MetaSound 的候选规范化使用公共只读批次容器，
  在一次 normalizer 调用内按候选类型、精确 target 和领域上下文复用成功目录；Rig 与
  MetaSound 同时复用公共名称解析索引。Niagara 使用完整 resolved context，分别区分用途、
  发射器与阶段。批次只修改待应用的 JSON，不执行资产变更；返回即释放，失败不缓存。
  实际原生创建仍重新取得当前操作对象，不把规范化目录传入 mutation 或下一次请求。
  2026-09-27 当前构建已通过 PCG CompactWorkspace、Niagara NativeAssetModuleVertical、
  ControlRig NativeActionVertical、SoundCue WorkspaceGraph、MetaSound FrontendVertical
  和 LevelSequence StructureValue 六项既有验证。Material 公共属性导入与独立再导出保持
  同一业务 HJSON；同方向单次导入样本为优化前 4.34 秒、公共布局优化后 0.96 秒。
  该样本未消除运行状态差异，不代表其他资产或候选批处理的端到端加速比例。
- 时序性能改进继续沿现有公共 owner 实现：MovieScene 的前后 channel 集合按引用一次索引；
  单 key upsert 不再复制和序列化整条 channel，改为有序时间查找并复用原值解析和编辑队列。
  LevelSequence 与 UMG animation 共用这部分实现；同帧仍选最后一个 key，无关 key 不重写。
  AnimSequence 结构应用取消仅为比较 skeleton 而生成整份骨骼轨道、曲线、事件 JSON 的快照；
  AnimSequence/Montage 事件最终归属通过一次 track 名称索引完成。必要的结构差异快照仍保留。
- UMG 控件候选及 TextureGraph 表达式类目录接入同一公共只读批次；TextureGraph 的 public
  candidate 与 node_type 保持不同过滤规则，批次内 LoadObject 后立即失效，避免漏掉新加载类。
  prepared import 一次索引 change 的 canonical/selector 身份，Material 与 PCG 共用；Material
  表达式 GUID 别名与实例参数单位也改为一次索引，保留原 first-match 和歧义判断。
  AnimBP 在变更投影前一次索引读取请求、原生图与节点身份，避免逐变更单元重新枚举和扫描。
  以上新增改动已通过 2026-09-27 Development 构建；当前 DLL 的 UMG AnimationWorkspace 与
  LevelSequence NativeTrackMenu.FloatProperty 两项既有验证及 cleanup 均通过，Editor 已恢复。
  证据在项目 `tmp/hjson-common-performance/t208-t85-targeted-current/r2/`；这两项验证仅证明
  覆盖路径的行为保持，不代表 AnimBP、TextureGraph 等其他领域已取得独立运行或性能数据。
- 性能范围按实际入口区分：Blueprint/AnimBP、材质系列、Niagara、PCG、Rig、音频、UMG、
  MovieScene、动画、AI、DataTable/CurveTable、StringTable、World/Level 和反射资产均在
  HJSON 公共链审查范围内；公共链获益不等于每种资产已有独立提速数据。DataTable/CurveTable、
  AI、StringTable 与反射根当前已是每次导入一次快照/规范化，未因本轮性能工作改写其语义；World 的不同
  owner 快照不当作重复快照删除。Montage 集合重建不等于逐 key 重建；Blueprint Timeline
  不凭名称归入 MovieScene 验证。Paper2D 的 TileMap 仍按用户决定搁置。
- 崩溃残留事务的磁盘恢复成功、内存 before-image 尚未确认时，公共 durable owner 保留完整
  journal 与两份备份，停止反复重放该事务；执行该次恢复的 HJSON import 通过 `warnings` 中的
  `durable_disk_restored_memory_preserved` 提示此事实。后续操作仅扫描到已保留的历史材料时不重复提示。
  此提示描述旧事务恢复，不表示当前 import 未保存；新操作成功也不证明旧内存恢复成功，
  保留目录不得被重用请求覆盖，也不得作为普通成功事务清理。

## 已证明的纵向范围

本轮统一框架验收已经覆盖以下代表性纵向路径：

- public Read/Export 正式 route 与 production dispatch；
- Reference CRUD owned fixture 的真实 create/update/delete 和持久读回；
- SimpleAuthoring Editor/Project 配置的持久读回；
- Timeline/Spatial public HJSON source parity；
- GraphCompound public Rig/IK 纵向路径；
- DestructiveImport public asset delete 纵向路径；
- LongRunning safe-exit 的进程外 terminal witness；
- active framework 的未知 ingress、重复 owner、legacy fallback 与 cleanup 审计。

这些代表性结果不能自动替代未选择 command 的领域验收，也不构成后续开发的测试前置。

## 验证与保护边界

- 完成判断只取得当前改动所需的正式入口结果和独立 readback；额外验证由用户明确要求或当前合同决定。
- mock、recording port、synthetic success、manifest 数量和 generated 文件数量不能
  冒充 production-ready。
- Result envelope 有效与业务成功必须分开判断。
- SafeExit、路径 containment、身份鉴权、事务回滚、脏资源和资源耗尽保护只约束其直接风险。
- 旧测试和旧 fixture 不因曾经通过而成为权威；保留在 Git 混合现场中的测试源码
  不进入插件加载，也不签发产品完成或阻断普通开发。

## 分发边界

Pak 与 installed `ue-agent-interface` skill 只同步正式可用文档、公开 CLI、Connection
模板和稳定工作流。以下内容不得进入 skill：

- `commands/deprecatedCommand/`
- `docs/generated/`
- coverage 生成物
- 任务表、任务复盘、runtime log 与临时文件

分发只在用户明确授权时执行。manifest/binding 与 Pak/installed skill 都从各自 owner
显式刷新或同步。

## 当前限制

- Transport 抽象已与业务解耦，但当前只实现 HTTP Adapter。
- 命令存在于 manifest 不等于已获得完整 E3/E4 证据；缺少真实 readback 的 route 不得
  声称 production-ready。
- 需要可选 UE 插件或运行时环境的命令必须显式返回 availability 失败，不静默降级。
- 发布提交、推送和远端发布不属于普通验证流程。
