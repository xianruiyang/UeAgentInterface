# Operations And Selectors

本页定义普通 authoring HJSON 的定位和变更意图。当前导出的 keyed desired-state 由公共层推导操作；
只有所属公开合同确实显示 `operation` 字段时，才直接填写该字段。

## Selector 是定位，不是编辑

selector 必须足够稳定，让 adapter 找到当前 UE HJSON 中的同一个对象。

常见 selector：

| 域 | 典型 selector |
| --- | --- |
| Niagara | stage：readable script usage/emitter name；module：readable module name；input：input name |
| Material | node name、可选短语义 select、pin name |
| Blueprint/UMG | component/widget name、graph name、node name、pin name |
| Sequence | 导出的可读 binding/track/section key、control name、frame；隐藏身份不手填 |
| ControlRig | hierarchy key、control name、graph name、pin path |
| Skeleton/IK | socket name、virtual bone name、goal name、chain name |
| ProjectSettings | settings container/category/section/page、field path |

selector 不应包含导出路径、行号、派生 count、runtime report 或旧 legacy path 字段。

创建候选保留原始 `name`；确有重名时，公共选择器为每个候选独立返回能唯一重放的一项或两项语义字段。
各项都使用同一个字段时，`choose` 压缩为该字段的值数组；否则返回对象数组，每个对象可以使用不同字段，
选中对象原样填入 `select` 后重试。类所属模块、资产目录、函数库和原生配置差异由所属候选 owner 提供；
专用成员或配置字段补充已有的节点形态、函数和资产/类绑定，不能重置基础语义集合。同一接口函数的普通
调用和消息调用保留节点类区别；普通函数保留函数名与owner，promotable operator 使用 `operation_name`，
不能将注册代表函数当作具体运算形式的选择值；具体函数由Pin类型提升决定。PCG保留原生操作类型。
类候选保留可读类来源，资产候选保留可读资产来源；中间适配器不得再压成短名而丢失 namespace。
同 basename 的不同来源保留完整可读 namespace，公共投影使用 breadcrumb，不要求调用方提供 UE 路径。
短字段已能唯一选择时不返回其他字段；完整来源仅作为必要消歧选项。返回选项按当前同名组验证可重放，
不以机器 ID、hash、临时菜单序号或任取第一项替代。PCG 配置值来自原生预配置值，不能使用列表下标。
MetaSound 创建候选遵循原生 Builder 的 class + major 规则，使用该主版本下当前最高次版本；旧次版本
不作为可独立创建的选择。已有节点的历史版本名称恢复使用独立的精确查询，不受创建目录归并影响。

## Operation 何时需要

需要 `operation` 的情况（仅限 owning domain detail 或当前导出明确公开该字段的形状）：

- 新增结构：widget、module、socket、track、section、row，以及仍由领域合同显式声明 operation 的 node/component。
- 删除结构：remove node、remove link、remove socket、remove virtual bone。
- 替换结构：replace graph nodes、replace binding、replace full list。
- 清空状态：clear override、clear binding、clear key。
- 重命名：rename graph/member/module。
- 一次性 action：retarget batch、packaging plan、dry-run config patch。

这不是所有文件的统一输入字段。ordinary keyed desired-state 会从“当前 map 与期望 map”的差异
内部推导 create/update/remove；当前 Blueprint component-tree 和 Blueprint graph 的模型可见
desired-state 都属于这一类，新 component/node row 不写 `operation`。graph row 的 map key 是
本地可读 alias，服务端从 alias 派生本地 node id，并根据当前与期望 map 的差异派生
create/update/remove。只有当前导出或 owning domain detail 明确展示 `operation` 时才由模型填写。
普通 keyed 条目显式填写 `operation` 会在首次写入前被拒绝；把条目包入 `values` 也不会取得内部记录权限。
服务端仅在实际完成公共集合解析后，以不进入 HJSON 的来源记录传递内部 operation records。

Level Sequence 的现有 `bindings` 同样使用 keyed desired-state：删除导出的可读 binding key 会解除该
possessable 及其所属 tracks，并保留实际 Actor 和其他 binding。新增普通对象绑定使用
`{binding_kind: "possessable", object_ref: "<公开对象路径>", tracks: {}}`，导入后重新导出取得属性轨道上下文。
删除父 binding 按 Sequencer 的原生语义同时删除其子 binding 和所属 tracks，按子先父后执行。
文档中未修改的子条目跟随这次删除；同次显式修改待删除的子条目会报告意图冲突。
删除同时清除当前资产本地 binding 的 tag 和 folder 成员关联，保留其他
SequenceID 的同 GUID tag 项、其他成员和 folder 结构。
既有 binding 条目的 `tags` 是本地标签名称集合，例如 `tags: ["Gameplay", "Focus"]`。
省略 `tags` 保持捕获的成员关系，`tags: []` 清除该 binding 的全部本地成员关系；名称按原生 FName
语义处理。它不编辑其他 SequenceID 成员，也不管理空标签组。
幸存 section 的旧引用按 UE 原生结果保留，不因此禁止删除 binding，也不擅自删除 section。
内部持久读回核对附属 metadata，普通 HJSON 不输出其机器序列化数据。
带 custom binding 引用的删除使用原生播放器注册表定位该 Sequence 的root或子序列实例，在解除引用前
按binding index调用原生spawn register清理。没有匹配播放态时只解除资产引用，不创建播放实例。
真实spawnable custom resolver已通过正常HJSON创建、配置和持久读回；root播放实例删除后原始Actor保留、
生成实例消失，无播放器同Editor重复创建/删除亦通过。子序列/停止态复用同一原生定位机制，未逐组合运行。

Sequence 的 `folders` 是可读 keyed 集合。新增条目使用 `name`，`parent` 引用同集合的可读键（空字符串为根）；
`bindings` 和 `root_tracks` 是相应集合的可读键集合。也可引用同次新增的 folder。省略字段保留捕获值，
显式空集合清除成员；把成员放到新 folder 会解除其旧归属，同时显式指定多个归属会拒绝。
修改 `name` 或 `parent` 保留原生 folder 对象身份和其他属性；循环、无效引用及同级重名会在写入前拒绝。
从集合删除 folder key 按原生递归语义删除其捕获的子 folder、binding（含 possessable 子项）及 tracks，
保留实际绑定对象。未改动的后代条目可保留在文档中，由同一删除规划处理；同时修改待删除后代或成员会拒绝，
不能通过移动子项逃避该删除闭包。未编辑的成员引用允许随显式 binding/track 删除清除；新写入的悬空引用不允许。
custom binding 生命周期沿上述原生清理路径处理。folder 编辑沿同一资产事务保存、重载与恢复。

不需要 `operation` 的情况：

- 改 bool/number/string/enum。
- 改 vector/color/transform 的已有值。
- 改 object/class/asset `path`。
- 改 Niagara module input 的 `value.source`，除非域 recipe 明确要求。

## 新增结构的最小种子

LLM 可以用最小 seed 创建结构，让 adapter/UAI 补齐默认值。最小 seed 是“能唯一表达新增目标的最小 authoring”，不是完整 readback：只写身份、必要定位上下文、必要 `operation` 和用户要改的初始字段。

涉及节点、模块、组件、控件、Material expression 或 Niagara dynamic input 时，先调用
对应领域的 exact candidate command。统一创建合同使用最小可读 `name` candidate，必要时
附带消除歧义所需的短语义 `select` 字段；
尚未接入的领域继续服从其当前 detail。完整链路见 `00_DirectHjsonAuthoring.md`。

| 结构 | 最小 seed |
| --- | --- |
| Blueprint component | 在导出的 `components` stable map 中新增实例 key，row 顶层最低只写 `name: ...` 或 `id: ...`，可选写 `parent`。不得写 `provider_extension`、显式 `operation` 或嵌套 `candidate`；成功后重新 export，再编辑新实例的 property units。 |
| UMG widget | 在当前导出的 widget 集合位置提供候选可读名称、实例 key 和父/slot 上下文；不从旧 typed widget payload 复制字段。 |
| Material node | 在当前图集合位置提供候选可读名称和实例 alias；Function Call 的资产引用按其实际字段表达，不从候选名称猜对象身份。 |
| Blueprint graph node | 在本次导出的 graph `nodes` map 中新增本地可读 alias，row 写 `candidate: {name: ...}` 和可选短 `select`、`pos: {x, y}`。服务端派生隐藏身份与变更；事件/入口也必须显式声明 candidate，端点不隐式创建节点。已知合法接口时同次提交连接；缺少接口信息时才按需重新导出。 |
| Niagara dynamic input | path-backed script 写 `path`；无路径候选写 `name`；只放在某个 module input 的 `value` 位置。 |
| Niagara module | 在目标 Stage 的当前 modules keyed 集合声明新实例与可读候选，必要时使用查询返回的短 `select`；由公共 desired-state 推导操作。不补旧 `apply_mode/operation`，不把未编辑 inputs 强制写空。 |
| Socket | `socket_name` + parent bone/component |
| ControlRig control | `name` + control type/settings |
| ProjectSettings field | field path + typed value |

如果最小 seed 不足，adapter 应报错，而不是猜。apply 成功后必须重新 export/readback，由读回的新结构补齐后续可编辑字段。

## 普通期望状态的编辑

新增节点是在当前节点集合增加可读实例 key 与候选；删除是在该完整集合移除对应条目。
连接集合按所属合同表达最终关系，不另行附加旧 typed remove payload。
已有普通属性直接修改导出 row 的 `value` 并保留 `type`；例如 bool row 使用布尔值，不写字符串代替。

字段或集合整体省略表示保持；显式空集合表示清空该集合，完整期望集合缺少原条目表达删除。
删除父项是否包含后代由原生归属合同决定，不能把某个领域的递归删除规则推广到所有集合。

## Graph 文件与连接边界

- 支持 merge-first folder contract 的 Graph 用 `graphs/items/<graph_id>/graph.hjson` 保存逻辑、用同项 `layout.hjson` 保存视觉布局。
- 当前 Blueprint graph 服从本次 `content_graph` export 的 `graphs/<local-alias>.hjson`；直接编辑该 workspace 的导出文件并回灌，不要改目录形状，也不要复用旧 revision。
- Blueprint 显式提交的 `connections` 是完整期望集合，每行使用 `{from: <alias>.<output-pin>, to: <alias>.<input-pin>}`。两侧 alias 必须存在或在同次 nodes 中声明创建，Pin 名来自实际接口；数组中遗漏既有连接表示删除，省略整个字段表示保持。
- event/entry node 必须在 nodes 中声明，endpoint 不会隐式创建 node；合法的新建节点与连接可以同次提交，由服务端先建立接口再解析连接。可变 Pin 组同样不强制模型分两次 import。其它旧平铺 graph 不是兼容输入，应重新导出当前 workspace。

## 失败策略

以下情况必须失败或产生明确 warning：

- selector 找不到目标。
- selector 匹配多个目标。
- `operation` 未实现。
- 删除/替换会影响未声明对象。
- 最小 seed 不足。
- LLM 提供的是 readback/debug/legacy 字段而不是 authoring 字段。
- path-backed 实体用旧 `full_name`、`asset_path`、`class_path` 等字段作为主输入。
