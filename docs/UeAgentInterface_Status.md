# UeAgentInterface 当前能力与边界

本文档只描述**当前已经完成并可文档化的能力**。  
未完成项不会写成“已支持”。

## 1. 已完成模块

- Level / Actor / Component
- Viewport / Screen Trace / Screenshot
- Asset Editor / Save / Exit
  - 当前已新增轻量资产属性 JSON 工作流：
    - `asset_export_property_json`
    - `asset_apply_property_json`
  - 第一版默认覆盖 `AnimSequence / Texture / StaticMesh / SkeletalMesh` 的常用属性
- Landscape
- Blueprint
- AnimBlueprint
- Montage
- Montage
  - 当前已新增单文件 JSON 工作流：
    - `montage_export_json`
    - `montage_apply_json`
  - `AnimMontage` 当前明确采用“单资产 = 单 JSON”模式
- UMG
  - 当前已新增 `WidgetBlueprint` 文件夹式工作流：
    - `umg_export_folder`
    - `umg_apply_folder`
  - 第一版固定根目录：`Saved/UeAssetFolders/WidgetBlueprint`
  - 当前聚焦 `WidgetTree + Blueprint 变量 + 常见绑定 + 常见 UMG 动画轨`
- StaticMesh
- EnhancedInput
  - 当前已新增单文件 JSON 工作流：
    - `enhanced_input_export_action_json`
    - `enhanced_input_apply_action_json`
    - `enhanced_input_export_mapping_context_json`
    - `enhanced_input_apply_mapping_context_json`
  - `InputAction / InputMappingContext` 当前明确采用“单资产 = 单 JSON”模式，不再另做文件夹式真源
- Material
  - 当前除 `UMaterial` 表达式图与实例参数命令外，已新增 `Material Instance` 文件夹式工作流：
    - `material_instance_export_folder`
    - `material_instance_apply_folder`
  - 第一版固定根目录：`Saved/UeAssetFolders/MaterialInstance`
  - 当前聚焦 `UMaterialInstanceConstant` 的“父材质 + 参数 overrides”
  - 已新增 `Material Function` 文件夹式工作流：
    - `material_function_export_folder`
    - `material_function_apply_folder`
  - 第一版固定根目录：`Saved/UeAssetFolders/MaterialFunction`
  - 当前聚焦 `UMaterialFunction` 的“函数级属性 + 表达式图”
- Sequence
- Sequence
  - 当前已新增文件夹式工作流：
    - `sequence_export_folder`
    - `sequence_apply_folder`
  - 第一版固定根目录：`Saved/UeAssetFolders/LevelSequence`
  - 当前聚焦“已有 binding 的结构化回写”，不是 Sequencer 全覆盖
- Niagara
  - 当前已新增文件夹式工作流：
    - `niagara_export_folder`
    - `niagara_apply_folder`
    - `niagara_emitter_export_folder`
    - `niagara_emitter_apply_folder`
    - `niagara_script_export_folder`
    - `niagara_script_apply_folder`
    - 固定根目录：`Saved/UeAssetFolders/NiagaraSystem`
    - Emitter 独立根目录：`Saved/UeAssetFolders/NiagaraEmitter`
    - Script 独立根目录：`Saved/UeAssetFolders/NiagaraScript`
    - 当前按 `UNiagaraSystem -> FNiagaraEmitterHandle -> FVersionedNiagaraEmitter -> UNiagaraEmitter` 建模
    - 导出覆盖 System/Emitter/User 参数/Renderer/Event Handler/Stage/Module/Module Input/Graph Node/Graph Link/Script 引用与 `raw_properties`，并区分 standalone emitter asset 与 system 内嵌 emitter instance
    - 回写支持 System 通用属性、User 参数、Emitter Handle、引用/内嵌 Emitter 的版本数据、Renderer、Event Generator、Event Handler、Simulation Stage、Module Stack、Module Input default、Data Interface 引用与 `raw_properties`
    - Script profile 支持创建或更新 `UNiagaraScript`，回写脚本元数据、raw reflected fields、graph nodes 与 Custom HLSL 文本
    - 调试辅助：`niagara_screenshot` 可截取 Niagara 编辑器 `window` 或 `viewport`，用于确认编辑器界面是否真实呈现写入结果；`niagara_get_stack_issues` 可读取 Stack 面板红/黄/信息提示，包含依赖校验、模块路径、模块 GUID 和可用修复项；`niagara_refresh_system` 可在结构化/原子写入后刷新 overview、emitter execution order、cached traversal data 和 Niagara ViewModel，避免必须手动新增/删除 emitter 才让 System 运行图恢复
    - 完成度：`NiagaraSystem / NiagaraEmitter / NiagaraScript` 三个 folder profile 已完整落地；`coverage_report.json` 输出 `implementation_status=complete_folder_profile`、`is_complete_target_schema=true`、空 `pending_profiles`、空 `blocking_gaps`
- Modeling
- Animation Assets / Skeleton
- IK Rig / IK Retargeter

## 2. 当前能力边界

### 2.1 Level

当前稳定可用：
- Actor 生成、删除、复制、附加、文件夹与 Tag 管理
- Actor / Component 实例属性读写
- Selection、Transform、Bounds 对齐、Vertex 对齐
- Viewport 相机、屏幕点 Trace、截图
- NavMesh 构建、投影、查路、连通性验收
- 世界碰撞查询与贴地辅助

当前未完成：
- Data Layer / Level Instance
- PIE 控制
- 更完整的批量筛选与统计

### 2.2 Blueprint / UMG / Material / Sequence / Animation

- Blueprint
  - `Actor Blueprint` 当前主编辑路径是 `blueprint_export_folder / blueprint_apply_folder`
  - 原子命令主要用于资产创建、变量/组件初始化、live 探针读取和尾部补修
  - `graphs/*.json` 当前稳定覆盖 `event / custom_event / call_function / variable_node / component_bound_event / node_by_class`，并额外支持部分 EnhancedInput 专用节点
  - 已补按类创建的通用节点原语，以及通用删除 / 连线 / 断线
  - 当前仍缺会附带子图的结构型节点原语，以及更高层的 graph diff apply
- UMG
  - `WidgetBlueprint` 当前主编辑路径是 `umg_export_folder / umg_apply_folder`
  - 原子 `umg_*` 命令主要用于初始化、探针验证和边界补修
  - 逻辑图仍主要复用 `blueprint_*`
  - 当前已支持文件夹式导出/回写；第一版采用“优先复用匹配根控件、重建其余子树”的 apply，不是节点级 diff
  - 当前动画轨稳定覆盖 `RenderOpacity / ColorAndOpacity / BackgroundColor / RenderTransform`，并已补通用 `float_property / color_property`
- Material
  - `UMaterial / MaterialInstance / MaterialFunction` 当前主编辑路径都已收口到各自的 `*_export_folder / *_apply_folder`
  - 原子 `material_*` 命令主要用于初始化、探针验证与局部补修
  - `MaterialInstance` 当前支持 `scalar / vector / texture / static_switch / static_component_mask`
  - 当前有统一入口 `material_set_parameter`
- Sequence
  - `Level Sequence` 当前主编辑路径已补到 `sequence_export_folder / sequence_apply_folder`
  - 已收口到统一 `sequence_add_property_track / sequence_add_property_key`
  - 当前支持 `bool / byte / double / float / integer / color / rotator / vector / actor_reference / object / string`
  - 仍保留 `visibility / transform / skeletal animation / UMG animation` 这类高价值专用命令
  - 当前 `apply_folder` 已能按 `binding.json` 自动恢复常见 `Actor / ActorComponent / Actor spawnable` 绑定
  - 当前 `spawn` 轨已纳入结构化导出/回写
  - 统一属性轨现已并入文件夹式 workflow：`float / double / bool / integer / color / byte / string / vector / rotator / actor_reference / object`
  - 当前 `outliner/folders.json` 已稳定支持 folder 树、常见 binding 归属、当前稳定 binding 内 track 归属与稳定 master track 归属
  - `master_tracks/index.json` 当前已导出真实摘要，并已稳定回写 `camera_cut / sub_sequence / cinematic_shot`
  - 还不是通用 Sequencer 全覆盖
  - Niagara
    - `Niagara System` 当前主编辑路径已补到 `niagara_export_folder / niagara_apply_folder`
    - standalone `Niagara Emitter` 当前主编辑路径已补到 `niagara_emitter_export_folder / niagara_emitter_apply_folder`
    - standalone `Niagara Script` 当前主编辑路径已补到 `niagara_script_export_folder / niagara_script_apply_folder`
    - 文件夹导出包含 `asset.json`、`settings/`、`parameters/`、`system_stages/`、`emitters/`、`scripts/`、`validation/coverage_report.json`
    - `coverage_report.json` 明确列出 System/Emitter/Stage/Renderer/Event/Script/Data Interface 的 export/apply 支持状态，并包含 `implementation_status`、`is_complete_target_schema`、`is_lossless_roundtrip`、`pending_profiles`、`blocking_gaps`
    - 当前 apply 已覆盖结构化与通用属性回写，并已处理 system 内嵌 emitter instance 的无 standalone 源资产场景；Stage/Module stack、自定义 Script 资产、Custom HLSL 和 Data Interface 引用都纳入 folder profile
    - 当前诊断面已补 `niagara_get_stack_issues`：可读取 live Niagara Stack ViewModel 的 `FStackIssue`，用于获取 UI 红色感叹号内容；未打开编辑器时会退回 data-processing ViewModel，但可能漏掉 UI-only / live Stack 状态
    - 当前刷新面已补 `niagara_refresh_system`，且 `niagara_compile_system` 对 System 默认 `refresh_before_compile=true`；用于处理 UAI 写入后 System compiled data / execution graph / editor ViewModel 没有完全刷新的情况
    - 结论：Niagara 当前实现与完整目标 schema 已对齐；用 `strict=true` 可把实际资产 warning 转为 apply 失败
- AnimBlueprint
  - `AnimBlueprint` 当前主编辑路径是 `anim_blueprint_export_folder / anim_blueprint_apply_folder`
  - 原子 `anim_blueprint_*` 图命令主要用于局部初始化、探针验证和 schema 边界补修
  - 当前覆盖资产级、Layer Interface、Anim Layer、State Machine、预览与部分图施工
  - 当前已新增文件夹式导出/回写：
    - `anim_blueprint_export_folder`
    - `anim_blueprint_apply_folder`
  - 第一版固定根目录：`Saved/UeAssetFolders/AnimBlueprint`
  - 当前采用“成员/逻辑图复用 Blueprint proxy，AnimLayer/StateMachine 结构重建后再回填动画图”的 apply 策略
  - 当前已额外补齐一批 AnimGraph 资产引用节点的属性真源回写，第一版稳定覆盖 `Node.BlendSpace / Node.Sequence`，足以支撑 `BlendSpace Player / RotationOffsetBlendSpace / Sequence Player / Sequence Evaluator` 的文件夹式 round-trip
  - 已补通用 K2 / 普通 AnimGraph 节点按类创建，以及通用删除 / 连线 / 断线
  - 仍缺会附带子图的结构型 AnimGraph 节点原语，以及 `BlendSpace` 资产本体的独立 authoring 命令面
- Montage
  - 当前主编辑路径已补到 `montage_export_json / montage_apply_json`
  - 当前覆盖资产、slot/segment/section、notify/notify state、notify/track 颜色、常见触发/过滤设置、skeleton slot/group、marker sync
  - 更细的 notify payload / 曲线 / metadata 仍未完全覆盖
- Animation Assets / Skeleton
  - 当前覆盖 `AnimSequence` 的资产级设置、选帧截图、压缩 / 剥帧设置、float 曲线、骨骼轨删除、metadata 生命周期、preview mesh、普通 notify、notify track、sync marker 与摘要回读
  - 当前覆盖 `Skeleton` 的骨骼层级回读、compatible skeleton、preview mesh、socket、virtual bone 与槽组/通知名摘要
- IK Rig / IK Retargeter
  - 当前覆盖 IK Rig 资产创建、preview mesh、goal、retarget root、retarget chain、solver 结构操作、有限的 solver 细粒度设置，以及 auto retarget definition
  - 当前覆盖 IK Retargeter 资产创建、source/target rig、preview mesh、retarget pose 生命周期与偏移数据、`global/root settings`、auto map chains 与 batch duplicate-and-retarget
  - 仍未覆盖全 solver 细粒度参数、完整 op 级编辑，以及稳定的单链显式 mapping 编辑

### 2.3 Modeling

当前稳定可用：
- Mode 激活
- Selection 与 mesh element 选择
- 通用 `start_tool / set_property / invoke_action / accept / cancel`
- 一批常用 wrapper

当前边界：
- 依赖 viewport、tool context、selection state
- 强交互工具不等于完全 headless 自动化

## 3. 推荐工作流

推荐使用：
- `UeAgentInterfaceCMD/dist/uai-cli.exe`

推荐顺序：
1. `doctor`
2. `run --plan ... --vars ...`
3. 必要时 `batch --file ...`
4. 查看 `dist/reports/*.json`

资产编辑默认优先级：

1. 单文件 JSON
   - 适合小型浅层资产
   - 例如：
     - `asset_export_property_json / asset_apply_property_json`
     - `enhanced_input_export_*_json / enhanced_input_apply_*_json`
2. 文件夹式结构化 JSON
   - 适合图类、树类、复合结构资产
   - 例如：
     - `blueprint_export_folder / blueprint_apply_folder`
     - `umg_export_folder / umg_apply_folder`
     - `anim_blueprint_export_folder / anim_blueprint_apply_folder`
     - `material_*_export_folder / material_*_apply_folder`
3. 原子命令只做辅助
   - 用于初始化、探针验证、schema 暂未覆盖的尾部字段，或回写失败后的定点修补

通用推荐方法：

1. 先最小创建对象骨架
2. 先让 UE 生成真实对象
3. 再 export 当前真实 JSON / 结构化 JSON
4. 以导出结果为模板补全高价值属性
5. 再 apply

这条方法尤其适合：

- Blueprint component
- UMG widget / slot
- Material expression / instance override
- AnimBlueprint 图和结构对象
- 各类字段很多的小型资产 JSON 工作流

不推荐：
- 日常直接手写 HTTP
- 在命令行内联长 JSON

## 4. 使用原则

- 先确认服务可用，再执行写操作
- 多步骤任务优先 `run` 或 `batch`
- 默认遇错中断
- Niagara / Modeling 优先小步验证
- 关闭编辑器前优先：
  1. `editor_list_dirty_resources`
  2. `editor_resolve_dirty_resources`
  3. `editor_close`

## 2026-04-22 增量说明

- `AnimBlueprint`
  - 第一版文件夹式工作流已落地：`anim_blueprint_export_folder / anim_blueprint_apply_folder`
  - 当前目录分层：`settings / members / layer_interfaces / anim_layers / state_machines / graphs / validation`
  - 已验证命令路由在新编辑器会话中可见；完整构建已通过
- `Animation Assets / Skeleton`
  - `anim_sequence_set_metadata` 已从“生命周期管理”扩展到“生命周期 + 简单属性编辑”。
  - 当前稳定属性面：`FName/FString/FText/bool/数值`。
  - `anim_sequence_get_info.metadata[]` 已补 `properties` 回读。
- `IK Rig / IK Retargeter`
  - 新增统一入口 `ik_retargeter_set_settings`。
  - 当前稳定覆盖 `global_settings/root_settings` 写入与回读。
  - `ik_retargeter_get_info` 已补 `root_settings/global_settings/chain_settings[]` 摘要。
  - 基于 UE 5.6 当前最小 retargeter 资产流实测，`chain_settings[]` 经常为空，当前不把它计为稳定能力面。

## 2026-04-21 增量说明

- `2026-04-21` 增量：`IK Rig` 当前已稳定补齐 `BodyMover` solver settings、goal 连接与 `InfluenceMultiplier`；其他 solver 的细粒度 settings 仍未计入稳定能力面。
- `2026-04-21` 增量：`IK Rig` 当前已稳定补齐 `goal` 的 `PositionAlpha / RotationAlpha` 资产级写入与回读；完整 goal transform/source/space 仍未计入稳定能力面。
- `2026-04-21` 增量：`IK Rig` 当前已稳定补齐 `goal current transform` 的即时写入与回读，以及 `ik_rig_get_info.goals[]` 的摘要字段回读；这层按“立即回读 + 摘要字段存在”验收，不承诺后续所有 rig 重初始化后绝对值恒定不变。
- `2026-04-21` 增量：`IK Rig` 当前已稳定补齐 `FBIK` 的基础 solver settings、goal settings、bone settings；这层不含 limit enum / 角度约束的完整细粒度配置。
