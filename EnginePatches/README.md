# UAI 所需的 UE 原生修复

本目录维护插件使用的原生引擎补丁。当前基线为 UE 5.6.1 的源码；其他版本需要核对对应原生实现，不能按文件名直接套用。
补丁必须编译进入实际启动的引擎。只修改源码或成功构建项目插件，不代表引擎修复生效。

当前插件对 `property-editor-edit-condition-controls.patch` 和 `editor-candidate-catalog-context.patch`
有编译/链接级依赖：分别提供 PropertyEditor 的编辑条件接口，以及 UMGEditor、AudioEditor、
MetasoundEditor 的候选查询接口。这些补丁与相关模块二进制必须先于插件部署；仅修改头文件不能提供
新增导出符号。其余补丁修复对应领域的原生行为，使用当前完整能力集时也应部署本目录的匹配补丁。

| 补丁 | 原生职责与插件消费者 |
| --- | --- |
| [PCG 样条网格切线空间](UE5.6/pcg-spline-mesh-world-tangents.patch) | Spawn Spline Mesh 将局部切线按整个样条变换转换到世界空间，与端点一致；覆盖普通与 Landscape spline 的旋转、缩放。原始切线 getter、metadata 和其他消费者的局部语义不变。 |
| [AssetTools 无人值守重定向修复](UE5.6/asset-tools-unattended-redirector-report.patch) | 无人值守脚本跳过最终模态报告，按调用方已选的 DeleteFixedUpRedirectors 执行删除；人工 Editor 操作保留原报告与选择。UAI 调用前完成 AssetRegistry 发现，原生引用修复和保存仍由 AssetTools 承担。 |
| [Niagara 同步编译副本释放](UE5.6/niagara-request-compile-release.patch) | `UNiagaraScript::RequestCompile` 在请求作用域结束时由 duplicate-data owner 释放编译副本，覆盖 DDC 命中及普通返回；三个 Script 编译调用者共享它，插件不再全局扫描 RootSet 代为清理。System 的异步 owner 不变。 |
| [RenderTarget 非模态属性通知](UE5.6/render-target-remove-modal.patch) | 删除 RT2D 固定 2048 确认及拒绝后的缩小分支；保留原生格式对齐、16384 上限、gamma、资源和引用组件通知。插件直接使用公共属性通知链，不再复制基类通知或绕过原型传播。 |
| [PaperSprite Atlas 生成通知](UE5.6/paper-sprite-atlas-notification.patch) | Atlas 生成器向 Sprite 发出的 Unspecified 通知不再递归生成 Atlas；重载 Redirected 身份替换不再触发 Sprite/Atlas 重新打包，避免消费替换到一半的引用图。加载和引用替换后恢复非持久的 PreviousAtlasGroup，保证随后换组/退出会通知旧图集。Sprite 烘焙数据、碰撞及渲染刷新仍执行。普通 ValueSet 编辑仍通知旧图集和当前图集。可选压缩重排对话框显式采用默认 No，局部无人值守调用保留现有布局，人工 Editor 操作仍显示原对话框。 |
| [编辑器候选目录上下文](UE5.6/editor-candidate-catalog-context.patch) | UMG 的 Palette 过滤新增 WidgetBlueprint 目标入口，原 editor 入口委托同一实现；SoundCue 暴露采用原生 ClassViewer 策略的 Palette 类目录；MetaSound 公开原生 registry 与 asset-tag 联合目录，补齐 AssetRegistry 元数据来源并按 class/version 去重，不再受启动注册目录限制；选中后仍由原生 query result 加载注册。UAI 查询无需创建或注册资产编辑器，过滤语义仍由原生 owner 决定。需要重编 UMGEditor、AudioEditor、MetasoundEditor。 |
| [Enhanced Input 重载通知接续](UE5.6/enhanced-input-reload-notifications.patch) | InputBlueprintNodes 在原生包引用修复阶段将 ValueType/Triggers 的待处理通知迁移至替换后的 InputAction，并在整批引用修复和 GC 完成后复用 Tick 的同一刷新实现。覆盖 EnhancedInputAction 事件节点与 GetInputActionValue 节点，避免持久读回后仍消费旧引脚类型；不为无待处理变更的资产补发通知。需要重编 InputBlueprintNodes。 |
| [原生编辑条件控件发现](UE5.6/property-editor-edit-condition-controls.patch) | PropertyEditor 公开只读的 `GetEditConditionToggleProperty`，原生 Details 与公共查询共用原有表达式解析、单 bool 识别及 toggle 规则。UAI 由此把隐藏 bool 控制字段纳入对象和 struct 的共同 authoring 资格；不会自动改变控制值，也不以条件 false 阻断持久写入。接口不创建窗口，不改变人工 Details 行为。需要重编 PropertyEditor，再编译 UAI。 |

这些修改作用于共用该引擎安装的所有项目。未带补丁的原版 UE 5.6 不具备上述修复：大尺寸 RT 编辑仍可能弹窗，Niagara 同步编译仍可能保留编译复制体，Atlas 新页生成可能递归重入，PCG 样条网格在样条旋转或缩放后可能扭曲。这里声明依赖，不新增运行门禁。
InputAction 的延期通知也可能在包重载替换对象后丢失，导致已加载蓝图保留旧的动作值引脚类型。

[BSP 放置项的原生 builder 身份](UE5.6/bsp-placeable-builder-identity.patch)使 BspMode 在注册所有 brush builder 时，
向同一 FPlaceableItem 发布 UClass asset data。人工拖放保持原行为；只读候选无需启动拖拽、改动默认 builder brush
或猜测本地化名称。UAI 以该身份绑定共同 Actor factory，并通过原生 builder/CSG 与 Level 持久化链创建真实刷。
这项补丁需要重编 BspMode；未部署时原生 BSP 放置项仍缺少非交互创建输入。

## 应用与构建

先保留目标源文件和将被覆盖的二进制，再从引擎根目录应用选中的补丁。例如：

```powershell
git -C <UERoot> apply --check <本目录>/UE5.6/niagara-request-compile-release.patch
git -C <UERoot> apply <本目录>/UE5.6/niagara-request-compile-release.patch
```

源码版引擎使用其正常 Editor 构建流程。修改构建期间关闭使用该安装的 Editor，不使用 Live Coding。

在 GptProjectTest 开发工作区中，构建入口为 `scripts/ue/BuildEditor.ps1`；该项目脚本不随插件分发。
其他项目使用自身的正常 Editor target/UBT 入口。Installed Build 默认跳过引擎及内置插件编译，
以下分别说明引擎模块和 foreign-plugin 模块的构建方式。

### Niagara 模块

向原项目 Editor Development 构建添加：

```text
-Plugin <UERoot>/Engine/Plugins/FX/Niagara/Niagara.uplugin -Module Niagara -NoUBTMakefiles
```

显式 foreign-plugin 使 Niagara 模块重编，其他引擎依赖继续使用预编译输出。普通 UBT Makefile 缓存没有比较 foreign-plugin 和选定模块字段；项目 BuildEditor 的 Plugin/Module 模式会自动加 NoUBTMakefiles，避免复用旧输出映射。

### Enhanced Input 编辑器节点模块

通过 foreign-plugin 编译：

```text
-Plugin <UERoot>/Engine/Plugins/EnhancedInput/EnhancedInput.uplugin -Module InputBlueprintNodes -NoUBTMakefiles
```

先备份 InputBlueprintNodesModule.cpp、InputBlueprintNodes DLL/PDB 和 UnrealEditor.modules。补丁只接续原生待处理通知与包重载，不复制节点重建算法或放宽 Blueprint authoring 的准备/写入一致性校验。

### BspMode 模块

使用 `-Plugin <UERoot>/Engine/Plugins/Editor/GeometryMode/GeometryMode.uplugin -Module BspMode -NoUBTMakefiles`。
先备份 BspModeModule.cpp、BspMode DLL/PDB 与该插件的 UnrealEditor.modules。补丁只发布注册时已有的
builder class；不在引擎中增加 UAI 依赖，不改变人工放置模式，也不通过实例化拖拽对象获取元数据。

### Paper2D 模块

同样通过 foreign-plugin 编译：

```text
-Plugin <UERoot>/Engine/Plugins/2D/Paper2D/Paper2D.uplugin -Module Paper2D -NoUBTMakefiles
```

PaperAtlasGenerator.cpp 的对话框默认值和 Paper2DEditorModule.cpp 的 Redirected 通知修改属于 `Paper2DEditor`，同一命令改用 `-Module Paper2DEditor` 构建。
先备份对应源文件、模块 DLL/PDB 及 UnrealEditor.modules。2026-09-25 Paper2D 模块构建已通过；Paper2DEditor 后续修改及公开 Atlas 消费验证范围由任务结果单独记录。

### PCG 模块

同样通过 foreign-plugin 编译：

```text
-Plugin <UERoot>/Engine/Plugins/PCG/PCG.uplugin -Module PCG -NoUBTMakefiles
```

先备份 PCGSpawnSplineMesh.cpp、PCG DLL/PDB 和 UnrealEditor.modules，再使用项目 BuildEditor 入口构建。验证直接使用原生 Spawn Spline Mesh，不能保留图中预先将切线转到世界坐标的旧补偿，否则会重复转换。

### Engine、AssetTools 与 PropertyEditor 模块

Installed Build 不能仅靠 ForceRulesCompile 重编 Engine 规则。AssetTools 同样采用以下流程，将 Engine.Build.cs、模块名与模块二进制替换为 AssetTools 对应项。当前采用的可恢复步骤是：

1. 正常关闭 Editor；备份 `InstalledBuild.txt`、`Engine.Build.cs`、`Engine/Intermediate/Build/BuildRules/` 中 `UE5Rules` 与 `UE5ProgramRules` 的 DLL、PDB、`*Manifest.json`，以及 Engine DLL、PDB、导入库和 `UnrealEditor.modules`。
2. 在带 `finally` 恢复的脚本内临时改名移出安装标记，仅在 Engine 模块构造函数中设 `bUsePrecompiled=false`。
3. 用原项目 Editor Development target，添加 `-UsePrecompiled -ForceRulesCompile -NoUBTMakefiles -Module Engine`。UsePrecompiled 使其他模块继续复用二进制；Engine.Build.cs 的实际修改触发规则重编。
4. 构建进程结束后，无论成功失败都恢复安装标记、Build.cs、原规则程序集及文件属性。失败恢复二进制输出；成功保留新 Engine 二进制。不要让临时规则成为后续项目构建的默认配置。
5. 正常启动 Editor，确认新模块可加载，再通过公开 HJSON 完成受影响行为读回。日志中的实际编译及链接动作是部署证据的一部分。

PropertyEditor 使用同一恢复流程，目标为 `Source/Editor/PropertyEditor/PropertyEditor.Build.cs` 与 `-Module PropertyEditor`。只在该模块构造函数临时设置 `bUsePrecompiled=false`；保留并恢复原规则程序集、manifest、安装标记、Build.cs 和文件属性。成功后保留模块 DLL/PDB/导入库，随后运行普通项目 Development 构建。仅修改公共头文件无法提供新导出符号，不能跳过原生模块部署。

`editor-candidate-catalog-context.patch` 同时涉及引擎编辑器模块 `UMGEditor`、`AudioEditor` 和
MetaSound 插件模块 `MetasoundEditor`。前两者按上述引擎模块流程处理各自 Build.cs 和输出；后者从
`Engine/Plugins/Runtime/Metasound/Metasound.uplugin` 使用 foreign-plugin 方式选择 `-Module MetasoundEditor`。
三个模块都部署完成后再编译 UAI，不能用单独更新 Plugin DLL 代替这些引擎依赖。

本项目的构建收据与恢复备份按任务进入 `tmp/`，不作为补丁维护真源。当前实现与验证范围由正式计划记录；本说明不宣称其他引擎版本兼容，也不代表已经发布到 Pak。
