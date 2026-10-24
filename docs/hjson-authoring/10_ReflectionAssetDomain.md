# Reflection Asset Authoring Domain

普通模型编辑使用 `content_graph.export_workspace` 导出的 semantic workspace，经
`content_graph.import_workspace`（SDK `workspace.import_changes()`）导入；属性位于普通文件的
`categories`，每项使用导出的 `type/value`。不要按下文历史内部 source 示例另造 `index.hjson`、
`operation` 或 `provider_extension`。第 2～5 节记录底层 Reflection source/apply 的历史契约，
不作为当前普通 workspace 的文件或公开输入规则；第 6～8 节说明网格和 PhysicsAsset 的普通入口。

## 1. 适用范围

Reflection Asset Domain 负责以下单一 UObject 资产 profile 的通用结构化编辑：

- AI 与图资产：`behavior_tree`、`state_tree`、`eqs`、`deformer_graph`、`sequence`
- Physics 与 mesh：`physics_asset`、`static_mesh`、`skeletal_mesh`
- Media 与 Paper2D：`media_player`、`media_source`、`media_texture`、`paper_flipbook`、`paper_sprite`、`paper_sprite_atlas`、`paper_tilemap`、`paper_tileset`
- Texture：`render_target`、`runtime_virtual_texture`、`sparse_volume_texture`、`subuv_animation`、`texture_array`、`texture_collection`、`texture_cube`、`texture_cube_array`、`texture_graph`、`volume_texture`
- Material：`material_instance`、`material_parameter_collection`、`material_function_instance`、`material_layer_instance`、`material_layer_blend_instance`
- PCG data：`pcg_data_asset`
- Deformer辅助资产：`deformer_source`、`mesh_deformer_collection`

`paper_tilemap` 是已登记的内部 profile，但其完整 HJSON 接入按用户决定搁置，不属于本轮完成范围。

函数实例三类的 Parent 使用公共属性，参数使用共享材质参数编解码与各自原生更新；
参数集合的 scalars/vectors 使用公共 keyed desired-state 和 Details 属性。
MaterialInstance 的参数与 layer_stack 由材质语义 owner 接入同一 workspace。

`ai_behavior_stack` 是多个资产组成的只读复合视图，不是单一 UObject authoring profile，不注册 Apply/Export contract。

## 2. 统一结构

目录、单文件和 inline source 最终都路由成一个逻辑单元。目录的唯一合法文件是 `index.hjson`：

```hjson
{
  operation: update
  target: {
    asset_path: "/Game/Example/MyAsset"
    asset_kind: render_target
    class_path: "/Script/Engine.TextureRenderTarget2D"
  }
  properties: [
    { property_path: "SizeX", kind: "int32", value: { literal: 512 } }
  ]
}
```

- `operation` 支持 contract 为该 profile 声明的 `create`、`update`、`upsert`、`clone`、`remove`。
- `target.asset_path` 是稳定 identity；`asset_kind` 是 profile 约束。
- `target.class_path` 是可选具体类约束。创建时只能选择 profile 基类范围内的非抽象类；更新和克隆时必须与真实类一致。
- `properties` 使用共享递归 UObject/UStruct/container property descriptor，不由各 profile 重复实现 parser 或 codec。
- `provider_extension.save` 可写入 Apply source；命令参数 `save` 可覆盖该值。

## 3. Apply

Apply 支持目录、单 HJSON 和 inline source。统一流程是 whole-workspace load、preflight、依赖检查、事务 mutation、可选保存、shared snapshot verification 和失败回滚。

缺失字段按 operation 处理：

- `update` 保留当前实例值。
- `create` 从 UE Factory、constructor 和 CDO 原生结果开始，再应用显式字段。
- owned UObject/UStruct/container 由共享递归解析器处理；允许的 owned object 使用真实 outer 创建。
- class、property、container 或 reference 约束冲突时在 mutation 前失败。

同一 Reflection source 中的显式 `properties` 先作为完整 proposed state 原子应用，再对发布后的真实资产触发一次整体 `PostEditChange`，最后执行 shared snapshot verification。这样 `Parent`、`BasePropertyOverrides` 等存在 UE 内部耦合的字段不会因逐字段通知而在中间状态互相覆盖；如果整体通知改变了请求语义，通知后的读回校验会使事务失败并回滚。

创建、导入、重导入、编译、bake、preview probe、运行时 setup validation 等命令仍是领域生命周期 hook，不属于通用 HJSON parser。

## 4. Export

Export 只从 Shared State Snapshot 投影，不读取 Apply request。`llm_authoring` 默认只输出：

- `operation: update`
- canonical `target`
- 可编辑 `properties`

默认不输出 `operations`、`control_type`、`category_path` 和可由 structured `value` 可靠表达的 `value_text`。`value_text` 仅允许出现在 `readback` 或 `debug` profile，或 structured value 无法可靠表达时。

ExportContract 与 ApplyInputContract 相互独立；导出的 authoring workspace 只是其中一条受支持的 Apply 输入，不限制 Apply 接收更多或更少的合法信息。

## 5. 文件和错误边界

- 目录 source 只允许 `index.hjson`，额外文件直接拒绝。
- filesystem reference 必须留在 workspace root 内。
- 同一 workspace 的重复 target、未解析依赖和 cycle 直接拒绝。
- Apply 成功必须经过写后 shared snapshot verification；不能只以命令返回成功作为资产状态证据。
- `validate_setup`、`validate_usage` 和 runtime probe 不替代 authoring source preflight。

## 6. StaticMesh 普通 workspace

`static_mesh` 对现有资产提供同一次导入中的 root 属性和 `mesh` 结构编辑。root 只暴露正式可写字段，
材料槽 `StaticMaterials` 使用公共结构数组、属性与引用 codec。它不提供 FBX 重导入或几何重建输入。

`mesh` 包含：

- `collision`：现有 BodySetup 的 `AggGeom` 与 `CollisionTraceFlag`，以普通分类属性表达。
- `build_settings`：各现有 LOD 的 BuildSettings 分类属性；必须保持原 LOD 数量。
- `sockets`：按 SocketName 组织的目标成员集合，每项的 `settings` 使用相同分类属性。
  删除成员键表示移除；`{}` 清空集合；省略整个 `sockets` 保持原集合。新增成员无需候选类型，
  已有成员保留原生对象及未编辑数据。集合边界定义见 [属性值协议](../UObjectPropertyValueV2.md)。

BodySetup 不在 root 再次导出，以免与 `mesh.collision` 重复决定同一内容。
子属性准备和校验复用公共 Details；集合解析复用 keyed desired-state；原生 mesh build 完成后，
保存、独立重载和失败恢复由同一 Reflection 公共事务负责。不得通过另写 typed mesh source 绕过该路径。

## 7. SkeletalMesh 普通 workspace

`skeletal_mesh` 对现有资产提供 root 与 `mesh` 同次编辑。`mesh.materials` 是有序材料槽数组，
每项由公共 Details 分类属性表达；保留槽位中未编辑的导入元数据。`mesh.bounds` 使用原生
ImportedBounds 的 `Origin`、`BoxExtent` 和 `SphereRadius` 分类属性，尺寸和半径不得为负。
`mesh.lod_settings_ref` 与 `mesh.skeleton_ref` 使用公共资产引用值，允许清空。

root 的 `LODInfo` 保持原生 `EditFixedSize` 数量约束；不能通过此入口新增或删除 LOD。
改变 LODSettings 引用时执行原生设置并应用其 LOD 默认值；引用不变时保留显式的 root LOD 编辑。
Materials、LODSettings、Skeleton 不在 root 重复导出。子属性准备和引用校验使用公共 Details 与
引用 codec，实际设置由唯一原生 mechanics 承担。完成 skinned build 后更新资产局部扩展 bounds，
保存、独立重载和失败恢复仍由 Reflection 公共事务负责。

该范围不包括 FBX 重导入、LOD 生成、骨骼拓扑编辑、蒙皮或姿态/渲染验证。

## 8. PhysicsAsset 普通 workspace

`physics_asset` 的 root 设置和 `physics` 结构在同次普通导入中应用。`physics` 包含：

- `bodies`：以原生 BoneName 为键的成员集合。每项 `settings` 使用 BodySetup 的分类属性，
  包括碰撞形状、物理类型和 BodyInstance 设置；`profiles` 保存各命名物理动画 profile 的分类参数。
- `constraints`：以 JointName 为键的成员集合。每项 `settings` 使用默认 ConstraintInstance 的分类属性，
  `ConstraintBone1/2` 必须引用最终 body 集合，位置和轴沿原生两端 body 局部约束 frame 表达。
  该项的 `profiles` 独立保存命名约束 profile 参数。
- `physical_animation_profiles`、`constraint_profiles`：两类 profile 的名称声明数组。
  新增或显式修改的成员 profile 必须有对应声明。移除声明时，未修改的旧成员 profile
  按原生规则清除，失效的当前 profile 选择也会重置；不需要手工同步删除这些旧上下文。

body、constraint 和嵌套 profile 的成员键均沿公共 keyed desired-state：删除键移除成员，`{}` 清空，
省略整个集合保留原集合。新增 body/constraint 不需要选择 UObject 类；领域使用固定原生 owned 类型，
新增项从原生默认值开始，保留项维持原生对象与顺序。删除 body 时由原生 PhAT 机制处理关联约束；
未修改的旧关系随原生结果更新。同批显式关系编辑仍必须指向最终存在的 body。
普通 root 不再重复导出这些集合；ConstraintTemplate 也不是独立资产 root。

碰撞 `SphereElems`、`BoxElems`、`SphylElems` 支持新增和删除形状，数量变更由与 StaticMesh 共用的
原生碰撞授权处理。其他带 `EditFixedSize` 的数组继续保留原生数量，不由此放宽属性可写性。

约束默认设置按原生持久化 DefaultProfile 读取，与 PhAT 当前选中的命名 profile 分开。
shape、约束和 profile 的值准备使用公共 Details，集合使用公共 keyed owner；领域只处理原生身份、
关联约束、profile 同步，以及 body 索引变化时碰撞禁用表和编辑器标记的保全。
原生 physics 缓存与组件刷新由共享 native owner 执行，保存、重载及失败恢复由公共 Reflection 事务负责。
这一路径编辑资产定义，不执行 ragdoll 模拟、动作播放或物理效果验收。

## 9. PCGDataAsset 普通 workspace

`pcg_data_asset` 的普通 root 属性与 `data` 在同次导入中处理。`data.source` 使用公共软对象路径值；
`data.sets` 为有序目标集合，各项以导出的 `instance` 绑定原 TaggedData。删除行移除集合成员，
`[]` 清空；新增项省略 `instance`，`data_type` 使用 `point` 或 `point_array`。

两类点存储共用 `points` 有序集合，每点的 `settings` 使用公共 Details 分类属性与 `type/value`。
保留点的 `instance` 即保留原点未编辑字段和 MetadataEntry 关联；新增点从原生默认值开始。
存量集合的类型保持原生类型；要替换类型，移除旧项并新增目标类型项。
非点数据保留原类型和内容，不提供伪造的点编辑入口。

领域负责 native 点数据复制、点范围写入和缓存更新；绑定、值、属性准备及保存重载由公共能力承担。
Tags、Pin 与原生 metadata 随数据保全，内部保全快照不进入普通 HJSON。
`data_asset_create_from_level` 继续负责从关卡转换数据，不与普通内容编辑合并。

## 10. MeshDeformerCollection 普通 workspace

`mesh_deformer_collection` 直接使用公共 Reflection root 属性：`Description`、
`MeshDeformers` 软对象引用数组与 `MeshDeformerCollections` 对象引用数组。
引用值、数组顺序、增删/清空、属性准备和保存重载均由公共能力承担；不提供第二份 entries 镜像。
原生递归聚合通过 visited collection 集合终止循环并去重，普通编辑不额外施加无环门禁。
此入口编辑集合定义，不执行 Deformer 图或验证变形效果。

## 11. Deformer Source Library 普通 workspace

`deformer_source`对应UE原生`UOptimusSource`资产。先通过`asset.factory_candidates`查询
`Optimus Source`并将返回名称交给`asset.object_create`，再使用普通workspace编辑
`SourceText`和继承的`AdditionalSources`引用数组。字符串、引用、属性通知和保存读回复用公共Reflection能力。
该资产保存可被Deformer使用的HLSL源代码；单独编辑源资产不代表其消费图已编译或变形效果已验证。
Deformer内部的component bindings仍属于`UOptimusDeformer`，不是该源码资产的绑定表。

## 12. ML Deformer 模型配置

`ml_deformer`对应`UMLDeformerAsset`。通过公开factory候选选择`MLDeformer Asset`创建；
原生factory按已启用模型registry建立Model，普通workspace不创建抽象Model或替换其类型。
`model_settings.value`按原生Details分类提供当前Model的可编辑属性；使用导出的字段和`type/value`，
例如`Base Mesh.SkeletalMesh`、`Training Settings.MaxTrainingFrames`及`BatchSize`。

公共Details负责采集/可读解码，ObjectApply负责原地属性写入，公共Reflection事务负责预览、持久化和读回。
领域仅定位现有Model并保持其class/Outer，写入后用原生`UpdateCachedNumVertices/InitVertexMap`
稳定mesh派生数据，避免UE在保存序列化中首次更改VertexMap。此入口不执行训练、推理或模型切换。

## LevelSequence 根时间设置

`display_rate`、`tick_resolution`、`playback_range` 以普通属性中的完整具名对象导出，
由公共 composition 的 `DocumentValueFields` 声明及既有文档地址解析往返；不再拆为匿名集合。
修改 `tick_resolution` 时，共享 MovieScene owner 在同请求的其他编辑之后调用 UE 原生
`TimeHelpers::MigrateFrameTimes`，只迁移目标资产，不递归子序列。同次时间点按导出时的 clock/rate
解释，保存、重载与最终读回仍归公共事务。此操作保留 UE 原生进度显示，不改全局 unattended 状态。

`timeline.track_candidates(target, query)` 按目标 MovieScene 允许的原生轨道类及当前属性上下文
返回可读名称，不在查询时执行各类的 section 工厂。查询同时匹配显示名称与原生类名；
新增 `root_tracks` 项使用返回的 `name`，仅在同名候选要求细化时填写 `select`。
先创建空轨道并重新导出，再根据导出的原生默认 channel 编辑 `sections` 和 `keys`。
轨道、片段、channel 和 key 共用 keyed desired-state；隐藏 clock 与新片段默认值由公共
MovieScene capture 提供给公共 keyed resolver，模型不填写机器身份。

WidgetBlueprint 目标同时传 `animation_name`，使用已导出的动画名定位所属 MovieScene；
候选枚举与新轨道解析仍归共享 MovieScene owner。WidgetAnimation 的普通 bindings 可创建
root_widget、widget 和 slot 绑定；后两者以 Designer 控件名作为 object_ref，slot 指该控件所属槽。
属性轨道候选同时传 binding_kind、object_ref 和 property_path，共享 owner 用原生属性查找解析叶属性，
初始化 Vector 维数、ObjectProperty 类或 Byte 枚举，写入使用同一上下文。
新动画可在普通animations集合中同时提交bindings/root_tracks；UMG先建立原生MovieScene，再交公共owner应用。
省略的scene settings来自原生默认值，playback范围的隐藏clock由服务端补齐，不要求模型维护。
创建绑定后导出，再创建轨道并导出，随后添加 section/key；身份与默认值由公共层管理。
WidgetAnimation 的上述绑定及 Float、枚举、对象引用、二维 Vector 轨道/section/key 已有保存重载
与独立读回证据。

LevelSequence 的 `bindings` 普通集合支持新增
`{binding_kind: "possessable", object_ref: "<公开对象路径>", tracks: {}}`。
对象引用复用公共 codec，原生 locator 负责保存后解析；创建身份由本次 prepared operation 持有，
预演与实际写入共用，不进入模型字段。导出后可用该 object_ref 和 property_path 查询轨道候选，
不传 animation_name；新轨道、section 和 key 继续使用相同公共集合/default owner。
已通过现有场景 Actor 的绑定、Float 属性轨道及 key 的保存重载与独立读回，并保留原 root tracks。
生成Actor绑定使用`{binding_kind: "spawnable", object_ref: "<源Actor公开路径>", tracks: {}}`，
由原生binding factory创建resolver、ActorTemplate及SpawnTrack；导入后重新导出取得其可编辑默认内容。
binding的`properties`与按导出resolver键组织的`template_properties`复用公共反射记录和集中definitions。
custom resolver位于原生binding reference中，不是与possessable互斥的结构kind；不手填GUID或locator内部数据。
本轮独立证据已覆盖resolver/模板/Bool编辑、与普通绑定共存、保存重载、真实root播放实例清理及同Editor重复创建删除。
共同prepared创建状态稳定轨道、resolver及模板对象身份；不将这些代表外推为所有resolver/轨道插件的逐类运行验证。
