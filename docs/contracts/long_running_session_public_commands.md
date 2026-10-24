# Long-running / Session Public Commands

本文是 `job.compile_build`、`editor.session`、Capture 与 operation lifecycle 的正式
服务端行为说明。Python SDK 和 CLI 只转发这里列出的真实 command；它们不维护第二套
任务状态、不解释 UE 业务结果，也不自动重试写操作。

## 统一生命周期

七条 start command 是：

| command | Python 方法 | 必填参数 |
|---|---|---|
| `job.compile_build` | `client.job.compile_build(...)` | `family_id`、`subject_ref`、`policy_id`、`parameters` |
| `editor.session` | `client.editor.session(...)` | `family_id`、`subject_ref`、`policy_id`、`parameters` |
| `runtime.scenario.execute` | `client.runtime.scenario_execute(...)` | `subject_ref`、`policy_id`、`parameters` |
| `capture.image.execute` | `client.capture.image_execute(...)` | `subject_ref`、`policy_id`、`parameters` |
| `capture.media.execute` | `client.capture.media_execute(...)` | `subject_ref`、`policy_id`、`parameters` |
| `capture.render.execute` | `client.capture.render_execute(...)` | `subject_ref`、`policy_id`、`parameters` |
| `editor.safe_exit` | `client.editor.safe_exit(...)` | `dirty_resolution`、`external_witness` |

start 成功只表示 operation 已被接受，不表示 native work 已完成。公开结果提供
不透明的 `start_result.operation` handle；调用方不得读取或构造其内部字段：

该 handle 由 SDK 保持，调用方不能自行序列化、拼接或替换内部字段。
需要跨调用进程继续时，使用 `handle.checkpoint("readable-name")` 保存 SDK 托管的续接信息，
在新进程调用 `client.operation.recover("readable-name")`。SDK 通过现有 status 校验服务端
会话和所有权并刷新 generation，不重放原请求；此能力不跨 Editor 重启。
本地 checkpoint 不改变服务端生命周期；完成后正常 release，并用
`client.operation.forget_checkpoint("readable-name")` 删除本地续接文件。
PIE scenario 完成后，父 handle 与 scenario 共享的运行会话状态已更新；跨进程继续前应重新
checkpoint 父 handle。SDK 将该运行状态一并托管保存，recover 仍向服务端确认 operation。
同一运行会话的旧缓存不能覆盖更新后的快照，不同会话不合并，已结束状态不复活。
旧 checkpoint 缺少运行会话状态时保持原 operation 恢复能力，但不能还原从未保存的新快照；
外部并发修改仍受服务端精确快照校验约束，不自动重放或绕过 stale。

调用方用该 handle 执行以下六条 lifecycle 方法：

| command | 用途 |
|---|---|
| `operation.status` | 只读取当前 snapshot，不启动或推进 native work |
| `operation.resume` | 推进一次 |
| `operation.cancel` | 请求 exact-owned native resource 在安全边界停止 |
| `operation.receipt` | 读取已形成的终态 receipt |
| `operation.reconcile` | 只对账；不得启动、推进或重放 native work |
| `operation.release` | 释放当前 operation 的 C11 owner 与临时 native resource |

无效或过期的内部上下文必须 fail closed。release 后 reconcile 返回
`state_unknown` 时，`retry_safe=false`；调用方不能把它解释为可以重放。snapshot 的
`latest_signal` 是当前 generation 的最新 typed signal，整数 lexeme 不经过 IEEE-754
浮点转换。canonical value 的数组和对象转换使用迭代遍历，没有嵌套深度上限。

推进时 deadline 已到但 native 仍在运行，会请求停止；停止尚未确认时保留
`terminal=false` 和资源所有权，`error_code=native_stop_pending_deadline_exceeded`
表示需要查看 Editor/native 状态，不表示可以释放活跃资源或重放原操作。

## 正式 policy

policy 只按 exact `(family_id, policy_id)` 注册，不接受后缀匹配或前缀猜测。

### Compile / Build

- `asset_compile` + `policy:asset-compile:standard/v1`
- `asset_compile` + `policy:asset-compile:blueprint/v1`
- `world_build` + `policy:world-build:standard/v1`
- `external_process` + `policy:owned-tool:standard/v1`
- `world_partition_resave` + `policy:world-partition-resave:actors-plan/v1`
- `world_partition_resave` + `policy:world-partition-resave:actors-run/v1`
- `external_process` + `policy:owned-tool:localization-gather-text/v1`
- `external_process` + `policy:owned-tool:localization-compile-text/v1`
- `external_process` + `policy:owned-tool:localization-export-po/v1`

GameThread stage 和 owned process 都由 operation-scoped port 持有。`external_process` 的
`policy:owned-tool:standard/v1` 只接受 subject `project:owned-tool-probe` 与空 `parameters`；executable 和 arguments
由服务端固定，客户端不能注入任意命令行。任务完成后，报告经公共 file artifact publisher 发布，
terminal snapshot 与 terminal receipt 均携带同一 artifact descriptor；SDK 调用方使用
`result.artifacts` 和 `artifact.materialize("tmp/<run>/report.txt")` 读取真实进程输出，
该 standard probe 的内容必须包含 `UAI_OWNED_TOOL_PROBE_OK`。其他 policy 使用各自的
subject 和参数合同。

同一报告通道覆盖 Compile/Build 各 family：JSON 报告使用 `application/json`，
外部进程原始输出使用 `text/plain`。机器 `report_ref` 不作为模型读取入口。
`native_exited_success` 只证明原生操作已成功执行；Blueprint policy 的编译正确性须读取
`uai.blueprint_compile.report.v1` 中的 `has_error/error_count`，不能用 operation 成功替代。
SDK 取得的报告由公共发布层保留，不因 operation release 丢失。
进程退出并排空输出后，公共 owned-process port 先完成 sink 的报告，再发布终态 artifact；
release 负责资源清理。报告格式由实际报告文件确定，不按 family 把结构化报告误当日志。

三个 Localization policy 只接受 subject `localization:<Target>`。参数可包含
`execute`、`cultures[]` 和非负的 `timeout_seconds`；PO export 还接受
`output_dir`。客户端不能注入 executable 或 arguments。`execute=false` 仍经过
operation lifecycle 形成计划报告；真实执行由 operation-owned commandlet 完成。
terminal report 使用 `uai.localization.owned_tool_report.v1`：compile 提供
`locres_files[]/compiled_count`，export 提供 `po_files[]/entry_count`，gather
提供 commandlet plan、log 和 report 路径。release 与异常析构都必须清理临时配置和
进程资源。

### Editor Session

- `asset_editor` + `policy:asset-editor:open/v1`
- `runtime_preview` + `policy:runtime-session:pie/v1`
- `runtime_preview` + `policy:runtime-session:pie-debug/v2`
- `runtime_preview` + `policy:runtime-session:culture-preview/v1`
- `world_context` + `policy:world-context:set-current/v1`
- `world_context` + `policy:world-context:create-blank/v1`
- `world_context` + `policy:world-context:partition-region-loader/v1`
- `world_context` + `policy:world-context:partition-actor-loader/v1`
- `modeling_tool` + `policy:modeling-tool:invoke-action/v1`
- `pcg_generation` + `policy:pcg-generation:generate/v1`

`host_lifecycle` + `policy:host-lifecycle:prepare-safe-exit/v1` 由专用公开入口
`editor.safe_exit` 固定绑定，不能作为 `editor.session` 的 `family_id` 传入。

`pcg_generation` 是有限工作：subject 为 `pcg-component:<当前 Editor 世界中精确的组件路径>`，
parameters 为空。即使输入不变也请求一次原生生成，完成信号只证明本次任务的生成事件。
共享 Operation 承担恢复、取消和收尾；取消只影响该任务，不承诺回滚部分生成内容。
成功输出在 release 后保留，持久化另走已有 Editor 保存入口。详见
[PCG 生成合同](../active/t52_pcg_command_migration.md#explicit-editor-generation)。

`asset_editor` 的 `open/v1` 是持久会话，包括 World 资产的原生地图打开。
就绪读取 operation Result 的 `payload.latest_signal.payload.phase == "active"`，
外层 operation 仍为 running；打开成功不意味着 terminal。需要异常恢复时，在第一次
resume 前 checkpoint，交互期间保留 handle；结束时 cancel、推进到 terminal、读取 receipt 并 release。

Explicit asset-editor opening keeps UE's native activation behavior. Background
preview work reuses an existing toolkit without opening or activating it again;
it creates a native preview carrier only when the requested capability needs one.

The PIE runtime-preview variant requires subject
`runtime-subject:pie:<non-negative instance>` and an empty `parameters` object.
For the first in-process PIE instance, use `runtime-subject:pie:0`. Resume the
returned operation until the session is running; query only the explicit
`runtime_instance` / PIE context. To stop, cancel the operation, resume it until
terminal, read its receipt, and release it. A stop request alone is not a
terminal result.

`policy:runtime-session:pie-debug/v2` 是面向非实时 LLM 调试的 run-to-pause
variant。subject 同样使用 `runtime-subject:pie:<non-negative instance>`，参数只接受
`local_player_index`。它创建不显示、不激活、不抢焦点的自有 PIE 窗口；只有在目标 PIE
World、LocalPlayer 和玩家视角均唯一解析，游戏已经暂停，并成功覆盖发布当前 workspace 后，
session 才在 operation 结果的 `latest_signal.payload.phase` 报告 `active`；外层 operation 的
`phase` 仍为 `running`，不能用它判断 PIE 就绪。当前 workspace 固定为
`Saved/UeAgentInterface/RuntimeDebug/PIE_<instance>/Current`；同一 PIE session 每次刷新只覆盖
`current.hjson`、标准 `.uai/workspace.hjson` 与 owner 标记，不累积按 revision 命名的副本。普通
`current.hjson` 使用 `uai.pie.runtime/2`、短 alias、对象关系和 `{kind,value}` 摘要；完整 UObject path、
session GUID、surface context identity 和 hash 不进入模型面。alias 到精确运行时对象的映射只由当前
session registry 持有，`.uai` manifest 只保存 target、base revision、entry 和完整性信息，不成为第二
对象索引。Current HJSON 是暂停态快照，不是持久资产 apply 入口。

骨骼网格组件的 `animation` 摘要提供动画类、`root_motion_mode` 和当前 `montages`：
资产路径、section、position_seconds、play_rate、weight、playing、blending_out、
root_motion_disabled 与 drives_root_motion。包含仍在淡出的有效实例，不消费累计 Root Motion。
`root.player.mapping_contexts` 使用公共属性 codec 的 map 表达当前 Enhanced Input 的资产与
AppliedInputContextData（Priority、RegistrationCount）。这些是暂停时的实际运行读回，资产路径不是运行时对象身份。

### Bounded PIE Runtime Scenario

普通入口为 `client.pie.start()`、`session.run(steps)`、`client.pie.recover()` 和 `session.close()`。
`pie.start(..., viewport_width=1920, viewport_height=1080)` 设置当前调试会话的真实视口物理像素
尺寸；两字段为正整数，分别省略时默认 1280/720，上限由当前 RHI 最大二维纹理尺寸决定。
底层 `editor.session` 的 `policy:runtime-session:pie-debug/v2` 使用同名 parameters，公开 describe
可查询。窗口与隐藏视口初始化共用该尺寸，不按桌面 DPI 再乘算；普通 Play 设置的
NewWindowWidth/Height 不控制此调试窗口。退出仅恢复原生关闭过程写回的窗口偏好字段，保留
结束前用户最新偏好与其他配置。普通 `pie/v1` 仍沿原生窗口语义，不共享此调试窗口 owner。
SDK 托管推进、终态领取、释放和恢复记录；中断后 `session.resume()` 继续领取原流程，不重放动作。
底层 operation 的 `runtime_session.scenario_execute(parameters=...)` 仍是相同 UE 执行入口。handle 私下托管
`runtime-session:pie-debug:<instance>:<guid>`、pause epoch 和 workspace revision，
模型不得读取、复制或回传这些值。参数包含 `steps`；`limits`（`wall_seconds`、`game_seconds`、`frame_count`）
仅在调用方主动设置执行界限时提供，0 表示未设该界限。显式 `at_seconds` 是本段起点的绝对游戏秒。
省略时刻采用从 0 开始的顺序执行；`wait.duration_ms` 从实际派发时开始等待，完成后放行后续顺序操作。
显式时刻可落在等待区间内；同刻已就绪操作按声明顺序执行，卡顿不预先消耗后续等待。
收据 `execution.operation_times_ms` 记录 `operation/game_ms`，等待另有 `wait_ms/completed_ms`；
SDK 在发送前注入最新 fence，服务端仍在恢复 PIE 前拒绝过期状态。

当前可执行 step 只有：

- `wait`：非负 `duration_ms` 使用毫秒，可选 `at_seconds`；等待不独占调度。不要求调用方填写帧数。旧 `duration` 仍按秒兼容，不能与 `duration_ms` 同时出现；
- `input_key`：声明 UE `key`、`pressed|released|repeat|axis` 和可选 `amount`；鼠标按钮另支持 `double_click`；
- `input_pointer`：`position: [x,y]` 为 PIE 视口内像素，或 `delta: [dx,dy]` 为一次相对像素位移，两者互斥；
- `input_scroll`：`delta` 为一次纵向滚轮增量，正值向上；
- `input_text`：`text` 为送往 PIE 内已聚焦控件的 Unicode 字符串；不操作系统剪贴板或 IME；
- `input_action`：声明精确 `UInputAction` 对象路径 `action_ref`，以及与资产 ValueType 匹配的
  `bool`、一维数值、二维数值数组或三维数值数组 `value`；
- `actor_transform`：使用 workspace 中的 Actor `alias`，并显式声明
  `ue_world`、三维、厘米和 `pitch_yaw_roll_degrees`，location/rotation/scale 至少提供一项；
- `property_write`：使用对象 `alias` 与精确反射 `property` 名，`value` 必须原样使用同一属性读取返回的
  `{kind,value}` 公共属性形状（容器或结构体按需携带 `element_type`、`struct_path` 等递归类型信息）；类型与目标属性不符会在写入前拒绝；
- `property_get` / `property_set`：使用 `ref: {alias, property, path: []}`；set 的 `value` 为公共 codec 值，类型取自真实属性。
  `path` 共用结构化 `{field}`、`{index}`、`{map_key}`、`{set_value}`；索引解释为执行时位置，Map/Set 按实际键/元素相等语义选择。
  不自动追踪 UObject 引用链；选中 Set 元素的写入在临时副本上完成重复检查与 rehash 后提交。
  操作目标始终使用当前会话 alias；属性值中的运行时对象使用 `{alias}`，资产/类使用公共 `{path}`，
  软引用保留未加载路径，`null` 或 `{path: null}` 清空引用。递归容器沿用相同规则和公共类型约束。
- `capture`：`format` 只接受 `png`，`source` 明确选择 `pie_output_surface`、
  `pie_player_camera` 或 `scene_camera`，同段可有多项；
- `capture_start`：同样的截图参数，加集合 `name` 与正数 `interval`（游戏秒），立即采首帧；
- `capture_stop`：按 `name` 关闭集合，在该操作位置返回帧集合。段末按开启顺序自动关闭剩余集合，失败保留已发布帧。

两种 camera 捕获保留捕获期间的渲染状态，输出经过色调映射的最终 LDR 颜色，不将 HDR SceneColor 直接量化为 PNG；
它们是独立相机渲染，不承诺与包含 UI 的实际 `pie_output_surface` 逐像素相同。

暂停快照的 `root.widgets` 登记同一PIE World中已经构造的UserWidget，以及各自WidgetTree中的子控件。
每项提供可读alias、name和class；UserWidget还提供in_viewport。它们来自真实运行实例，既不创建设计态副本，
也不把其他World的Widget混入当前session。alias沿用会话弱引用与同World校验，结束会话时退出。

可选 `observation_fields` 以 `{alias,property}` 按需读取 Actor、Component或已登记Widget的反射属性；terminal
observation 返回公共 property codec 的 `{kind,value}`；正常中途读取使用 `property_get`，同属性多次采样保留多项。
运行时属性不以 editor-editable 或 transient 筛除；真实只读、deprecated 和 codec 不支持的属性不被伪称可写。对象、Interface、SoftObject及其嵌套容器
引用使用公共codec的scoped reference接缝：读回为`{alias: "<当前workspace alias>"}`或`null`，
写入使用相同形状；alias仍须属于当前session的精确PIE world，原生属性类、接口及class约束由公共codec检查。
未登记或无法解析的引用读回为`{unavailable: true}`，它不是可写引用。完整对象路径不作为PIE引用输入，
不加载其他World或持久资产来替代失效alias。递归解码先写入临时值，全部成功后才复制到目标属性。
这不新增整个scenario的属性事务，也不把成功的属性写入自动恢复为session初值。

键鼠事件以目标 LocalPlayer 的 InputDeviceId 和会话独立 Slate user 投递，UI 焦点、指针捕获由该虚拟用户持有。
键盘传播限定在 PIE widget 子树，避免调用编辑器全局快捷键；UI 消费与 viewport/gameplay 的传递遵循该树。
相对鼠标在游戏捕获模式进入 MouseX/MouseY，UI 模式进入虚拟指针命中；绝对位置始终是视口像素定位。
`input_action` 解析同一 LocalPlayer 的 Enhanced Input subsystem，并用会话
逐帧重复的 one-shot sample；它不占用或清除 action-keyed continuous injection，也不改 mapping
context。非零 sample 会保持到同一 action 的后续 sample；精确零值停止该 action。不同 action
可以在同一 `at_seconds` 形成同时组合，按时间排列可形成顺序组合；非零到零的时间差表达长按、
短按或有界摇杆/视角输入，多条中间 sample 表达轻推和模拟轴曲线。同一 action 在同一时刻的
重复 sample 按声明顺序应用；值类型与资产不匹配在恢复 PIE 前失败。

两种输入路径都不调用操作系统键鼠、不移动系统光标、不切换前台窗口，也不依赖用户当前焦点。
正常成功暂停保留会话持有的键、轴和 Action 样本；暂停期间不推进输入，下段运行继续投递。
运行失败、超时、取消和会话退出释放输入与本会话 Slate 捕获；运行中的异常 Action 释放在后续引擎帧确认已归零，
不要求该 Action 曾产生非零游戏响应（项目 Trigger 可以正常过滤它）。无法证明释放时明确报告状态不明和收尾诊断，
保留原执行结果；不得清空用户 mapping、刷新全局按键或重放流程。运行前静态参数拒绝不改变上一段保持状态；
运行前取消清除注入状态并刷新暂停快照，不为清理而额外推进游戏。

`pie_output_surface` 在对应事件点重解同一 PIE session、World、LocalPlayer、GameViewport 与最终
surface identity，使用公共Slate离屏渲染器绘制真实PIE SViewport的场景纹理与GameLayerManager/UMG子层，
按当前视口像素尺寸及DPI采集；不创建设计态Widget副本，也不依赖窗口可见或激活。当前输出面若无法提供
可合成的场景纹理则明确返回不可用，不退回缺少UI的场景截图；它没有移动语义。`pie_player_camera`
读取目标 LocalPlayer 的 `PlayerCameraManager` evaluated POV，不接受 pose/FOV override；若需改变玩家
视点，只能让同一 scenario 的正常 PIE 逻辑或 `input_action` 先改变它。

`scene_camera` 有两种互斥选择：提供 workspace 中 CameraActor 的 `camera` alias 时，可选
`view_transform` 的 location/rotation 任一项及 `fov_degrees`，它们只作用于离屏观察副本；不提供
`camera` 时必须给出完整 location、rotation 与 `1..179` 的 FOV，创建 operation-owned 临时视角。
`view_transform` 显式使用 `frame=ue_world`、`dimension=3`、`unit=centimeter` 和
`rotation_format=pitch_yaw_roll_degrees`。两种路径都不移动、切换或保存现有场景相机，也不修改玩家
view target。

`location` 是 `[x,y,z]` 厘米数组，`rotation` 是 `[pitch,yaw,roll]` 度数数组；两者都严格包含三个有限数字。
例如 `view_transform:{frame:ue_world,dimension:3,unit:centimeter,rotation_format:pitch_yaw_roll_degrees,
location:[0,0,200],rotation:[-10,0,0]}`，配套的 `fov_degrees` 在 operation 顶层。单图 `capture` 与
截图集 `capture_start` 共用此合同；预检保留原错误码，并在 detail 指出 `steps[index]` 的字段及期望格式。

三类采集都不捕获 Level Editor viewport，不激活窗口，也不依赖前台焦点。图像与
`capture.image.execute` 共用唯一 Capture image artifact owner，并在 PNG 发布后通过独立 readback
oracle 验证。各采集点独立发布图像并保存到有序 `flow.results`，SDK 用 `artifact_index` 对应 `Result.artifacts`。
每个单图/集合帧保留 `source`、实际采样 `game_seconds` 和像素尺寸。相机结果还返回实际使用的
`view_transform`、`fov_degrees`、`projection`（正交时附 `ortho_width`），存在请求相机 alias 时附 `camera`。
截图集包含 `frame_count`、`missed_frames`、`failed_frames`、`gaps`；帧的 `sample_index` 与
`scheduled_seconds` 来自开启时确定的间隔时间轴，不能用实际晚采时间重新起算来隐藏遗漏。
`gaps` 记录漏采区间/数量或采集失败及实际时刻，不复制旧图补频率，不统计集合结束后的时段。
发生漏采、采集失败或异常关闭时集合为 `partial`；漏采本身不把已完成的流程操作改称执行失败。
读图、发布或读回失败会使 scenario 失败，不能以
已经重新暂停和刷新 workspace 冒充截图成功。

scenario 从已验证暂停屏障开始，短暂恢复同一 PIE，执行到完成、失败、超时或取消后立即停止
dispatch、重新暂停并覆盖刷新同一 current workspace。已经发布的图像不因后续失败而删除。
`flow` 分为 `execution`、有序 `results`、`snapshot`、`diagnostics`；执行成功、暂停成功与快照更新分别表示。
terminal typed signal 返回新的 `pause_epoch`、
`workspace_revision` 与 `workspace_path` 到 SDK 私有 envelope；typed handle 自动更新自身，
模型只看到业务终态。普通 SDK 使用面自动领取并释放有限 operation，保留 PIE session 供下一段运行；
不让调用方手工维护 revision 或 checkpoint。底层 typed handle 供生命周期诊断使用。

World-context policies use a map package subject, for example `world:/Game/Maps/L_Main`
(without the UObject suffix `.L_Main`), and never infer a map from the current
selection. This is distinct from an asset-editor `asset:` object target.
`set-current/v1` activates one existing map and accepts only
optional `save_existing_map` (default `true`). If the target is already the current
Editor World, it reuses that World; only a different target is switched and loaded.
Successful activation alone is not evidence of a reload. `create-blank/v1` creates and
persists one map; `overwrite_policy` is `fail`, `reuse`, or `replace` (default
`fail`), `save_existing_map` defaults to `true`, and
`create_world_partition=true` enables External Actors and initializes World
Partition before the map is saved.

Both partition loader policies require that the subject is the exact current
World Partition map. The region loader takes `bounds.center[3]`,
`bounds.extent[3]`, an optional non-empty `label`, and optional `dry_run`; the
actor loader takes at least one `actor_guids[]` or currently resolvable
`actor_ids[]`, plus optional `dry_run`. A non-dry loader remains owned by its
operation after publishing a `world-partition-loader:*` session ref. The caller
must use `operation.cancel`, advance to the cancelled terminal state, and then
`operation.release`; switching maps or dropping the Python object is not a
cleanup contract.

`modeling_tool` 只接受 subject `modeling-session:operation-local`、参数 `action_id`
取值 `spawn_transient_box`，以及可选的正数 `size_x/size_y/size_z`；缺省尺寸为 100，单轴上限
为 100000。它创建 operation-owned transient `ADynamicMeshActor`，不写入旧 static session
registry；cancel/release 只销毁该 operation 的 actor。

VoxelGrid 不属于 `editor.session` family。公开入口是
`client.mesh.voxel_grid_begin(mode='create', ...)`；成功结果的 `voxel_grid` typed handle 提供
`read/add_bounds/remove_bounds/add_shape/remove_shape/apply/close/cancel`。调用方只提供显式坐标空间、bounds、shape、mesh、
collision、material 和 keep-session 等业务参数；session、grid identity、revision、operation identity、
durability receipt 由 SDK 和服务端托管，不进入模型上下文。公开 readback 保留精确 Actor 路径、几何表示、
体积/边界及网格、碰撞、变换的等价结果；这些是业务观测，不是续传身份。持久业务真源仍是 managed
Actor 上唯一的 editor-only serialized component；session memory 只保存当前 fence，不成为第二真源。
`apply` 继续在 prepare/enlist/persist/verify/acknowledge 合同内只保存预检为 clean 的目标 package，
`cancel` 只移除尚未提交的 create Actor，`close` 只释放 clean session，二者都不改变 viewport 或窗口。

`shape` 包含 `kind=box|square_pyramid|triangular_prism`、`space=grid_local|component_local|world`、
厘米 `center/size`（三个正尺寸）、可选 degree `rotation={pitch,yaw,roll}`。四棱锥沿局部 +Z 指向尖顶；
三角柱沿 Y 挤出直角 XZ 截面，从 -X 低端升至 +X 高端。add/remove 分别做精确体积并集/差集。
首次精确编辑把整格表示转换为凸实体片，后续 bounds 和 shape 共享该源；v1 组件仍可读取，v2 原位保存
实际表示。纯整格保留 `exact_box_components`，精确实体默认 `exact_convex`，`none` 显式关闭碰撞。
表面及碰撞由同一实体生成，取消和失败恢复保留既有持久化职责。
完整差集得到空实体仍允许成功 apply，保留 managed Actor 供后续 edit。精确实体的 volume_cm3、solid_piece_count
为零，提交后 mesh 的 vertex/triangle/exposed_face 和 collision 的 box/convex 计数均为零，无残留 owned collision。
空结果本身不产生错误或警告，不触发 Actor 删除；调用方仍须处理其他实际诊断。

#### Save specified Editor resources

`editor.save_resources(resource_refs)` saves any persistent mounted asset package or map
through UE's native package serializer and keeps the Editor open. Object paths normalize to
packages, existing unloaded packages resolve automatically, and new in-memory packages are
supported. Native-owned external packages and the map's persistent-level build data belong to
the save, including dirty companions of a clean parent; unrelated referenced assets do not.
Native cleanup of an empty package is confirmed by its file removal, rather than requiring
that a deleted external object leave a package file behind.
Empty lists are a no-op. Every requested resource reports `saved`, `unchanged` or `failed`
with its failure reason; other items continue and successful saves are not rolled back.
Configuration refs use UE's native hierarchy/delta output and disk confirmation before dirty
is cleared. Unconfirmed writes remain dirty. `remaining_dirty_resources` uses the same discovery
owner as `editor.dirty_resources` and safe-exit, across all persistent content roots and the
six supported config scopes. Successful HJSON imports already persist their changes and do not
require an additional save unless subsequent Editor actions made the resource dirty.

`Utilities/EditorResources` owns explicit package save/confirmation, config cache-key versus
physical-path handling, and dirty discovery. Asset/domain operations share its package primitive
while retaining their compile, reload, rollback and readback lifecycle. HJSON durable staging and
publication retain the transaction owner. Settings imports enlist the actual generated IniPath
and confirm any pending native cache write before staging; a logical key such as `Engine` is
never a durable file path. Config success requires native output on disk or a proven empty
native output/deletion, including when the target file did not exist before the request.

#### Safe Exit

普通调用方不得手工拼装 `external_witness`。正式入口是项目的
`scripts/ue/SafeExitLifecycle.py`：它绑定 `RunEditor.ps1 -PassThru` 返回的精确 Editor
进程身份，在项目 `Saved/UeAgentInterface/SafeExitCompletion/v1/<launch>/<attempt>/`
创建单次 armed witness 与 heartbeat，再通过公开 SDK 依次发送一条
`editor.safe_exit` 和一条 `operation.resume`。Connection TOML 只承载 transport 与认证，
不保存 launch、attempt 或 completion 状态。

```powershell
python -X utf8 scripts/ue/SafeExitLifecycle.py `
  --project ./GptProjectTest.uproject `
  --connection ./connection.toml `
  --editor-pid <exact-run-editor-pid> `
  --output ./tmp/<run-id>/safe-exit-terminal.json `
  --mode reject
```

Plugin 必须在 dirty save/discard 和创建 operation 前验证 witness 的 launch/attempt/nonce、
Editor 身份、独立 witness 身份、存活状态、有效期和新鲜 heartbeat。缺失、过期或身份不一致
返回 typed failure，不改变 dirty package，也不请求退出。`dirty_resolution.mode=reject` 在存在
适用持久内容挂载根的 dirty package 或配置时返回 `editor_safe_exit_dirty_resources_present`；`save` 或
`discard` 必须提供与当前 dirty package name 集合完全一致的 `save_resources` 或
`discard_resources`。

通过 dirty 决策后，operation-owned ticker 走 MainFrame 原生关闭入口；无主窗口时先广播原生
EditorClose 再请求退出。资产编辑器在预览世界销毁前关闭，打开资产的恢复状态由 UE 自身保存。
Plugin 在 engine pre-exit 阶段以单写者
不可变发布 `uai.safe_exit.pre_exit_committed.v1`，其中包含 dirty 决策、精确 operation ref 与
Editor 身份；仍存活的 runner 用启动时取得的精确进程 handle 等待自然退出，再不可变发布
`uai.safe_exit.process_exited.v1`。resolver 只有在两阶段 launch/attempt/nonce、Editor identity
一致，且 `process_exited=true`、`force_kill=false` 时才发布
`uai.safe_exit.terminal.v1`。成功 terminal 同时记录 operation ref、退出码、两阶段摘要和
`operation_lease_cleanup=process_terminated`；`transport_disconnected`、PID 不存在或 start
command accepted 单独都不是完成证据。只有进程退出码为 0 才报告成功；非零或无法取得退出码
报告 failed，同时保留实际退出与协调清理结果，不把自然退出等同于正常退出。

正常成功与 dirty reject 会在输出发布前停止 heartbeat 并清理 attempt root；等待超时则返回
`pending_external_timeout` 并保留精确 coordination root 供诊断，不以强杀升级为成功。输出路径
必须位于 coordination root 之外，且采用不可覆盖发布；重复解析或重试不得重放退出副作用。

`culture-preview/v1` 的 subject 是 `culture:<Culture>`，参数精确为
`culture`、`restore_after` 与 `scope="session"`。该 policy 使用 Editor world
context，不要求 `runtime-subject:pie:<instance>`。当 `restore_after=true` 时，owned
action 设置请求 culture 后立即恢复先前 culture；previous/requested/restored 是可读业务状态，
session identity 留在 SDK 私有 envelope。调用方还必须用
`content_graph.compound` 的 `localization.culture.read` 独立确认恢复结果。
未知 culture、subject/policy 不一致或恢复失败均为 typed terminal failure。

### Capture

- image：`capture.image.execute` 当前公开以下 16 个常规 exact policy；每个 policy 的 subject 与
  参数合同由 production policy descriptor 拥有。只有 descriptor 已提供完整 discovery
  contract 的 policy 才会由相应 command 的关键词发现覆盖；客户端在需要具体 policy 时把
  可读 `variant` 交给 `generated SDK 与正式 schema`，并按返回的完整 `request_schema` 调用，
  不得从旧 command identity、Editor 状态或其他 policy 推断字段：

  - `policy:image-capture:anim-blueprint/v1`
  - `policy:image-capture:anim-sequence/v1`
  - `policy:image-capture:behavior-tree/v1`
  - `policy:image-capture:blueprint/v1`
  - `policy:image-capture:control-rig/v1`
  - `policy:image-capture:metasound/v1`
  - `policy:image-capture:niagara/v1`
  - `policy:image-capture:pcg-viewport/v1`
  - `policy:image-capture:viewport/v1`
  - `policy:image-capture:viewport-buffer/v1`
  - `policy:image-capture:viewport-sequence/v1`
  - `policy:image-capture:level-sequence/v1`
  - `policy:image-capture:sound-cue/v1`
  - `policy:image-capture:state-tree/v1`
  - `policy:image-capture:texture/v1`
  - `policy:image-capture:umg/v1`

  当前首个纵向公开的 viewport variant 是 `policy:image-capture:viewport/v1`；
  `policy:image-capture:transform-candidates/v1` 另以 actor subject 公开候选序列帧能力。
  `policy:image-capture:viewport/v1`、`policy:image-capture:viewport-buffer/v1` 与
  `policy:image-capture:viewport-sequence/v1` 三条 active viewport policy 的 subject
  均为 `capture-subject:viewport:active`；transform-candidates 使用 actor subject；资产与
  Actor policy 使用 `capture-subject:asset:<object-path>` 或
  `capture-subject:actor:<actor-name-or-label>`；每个 policy 的精确字段继续以正式
  manifest 与带可读 `variant` 的 `generated SDK 与正式 schema` 为准。尚未形成完整 discovery
  contract 的 policy 仍可由既有已知消费者使用，但不属于关键词发现已覆盖范围，
  search 缺席时必须 fail closed，不能猜测。

  `policy:image-capture:viewport-sequence/v1` 在同一个 long-running operation 内按真实 Editor
  engine frame 采集 `2..16` 帧；`interval_frames` 控制相邻采样间隔，另可设置
  `warmup_frames`、`frame_sheet_padding`、`format`、`quality`、`max_size` 与 `view_mode`。
  它不接受人工 world tick 或 `tick_delta_seconds`。terminal result 的主 `artifact_ref` 是帧表，
  `additional_artifact_refs` 按顺序持有原始帧；`capture_receipt.frames[*].raw_artifact_index`
  将每个采样映射到对应原始帧。operation 结束或取消时会恢复临时 view mode 与 viewport realtime，
  调用方仍按统一流程读取 `operation.receipt` 并执行 `operation.release`。

  各 image policy 的终态 `capture_receipt` 保留该 policy 已产生的公开采样信息；字段可选且语义属于对应 policy。
  `level-sequence/v1` 是独立 FinalColorLDR 相机渲染，active camera 使用原生 camera view 和后处理；viewport
  fallback 只借用相机几何，不继承 Editor backbuffer、show flags 或曝光历史，不能作为等价的高分辨率视口截图。
  每个显式时间跳转按 camera cut 采样，保留原生 view state 以支持 eye adaptation。收据包含 render path、
  尺寸、帧时间、相机来源、projection、后处理混合权重及曝光历史范围。逐帧输出以 `uses_primary_artifact`
  或 `raw_artifact_index` 关联主 artifact / `additional_artifact_refs`，不返回内部文件路径；仅帧表输出不生成单帧 artifact。
- submix media：`policy:media-recording:submix-wav/v1`，subject
  `media-source:audio-submix:master` 或显式 `USoundSubmix` object path
- sound preview：`policy:media-recording:sound-preview-wav/v1`，subject
  `media-source:sound:<USoundWave object path>`
- render：`policy:render-operation:clear/v1`；`render-resource:operation-local` 通过
  `parameters.width`、`height` 和 clear color 建立 operation-owned transient target

image、media 与 render 共用 C8F file artifact 发布。artifact 位于 terminal snapshot 的
`latest_signal.payload.artifact_ref`；调用方必须独立读取 `locator`，校验 `byte_count`、
`content_hash` 和格式内容。RenderTarget 还必须用独立像素 readback 验证，不能把 native
session 自报的 success 当作 C9。

`sound-preview-wav` 从指定 `USoundWave` 的真实导入 PCM 确定性生成 WAV，因此不依赖窗口
焦点或音频硬件；`submix-wav` 则要求实际 mixer frame。viewport capture 要求存在尺寸有效、
已经渲染的 level viewport；最小化导致 viewport 为零尺寸时会明确返回
`capture_image_viewport_unavailable`，不会生成替代图片冒充 viewport readback。

全部 image policy 都要求 Editor 进程具备渲染能力。`FApp::CanEverRender()` 为 false
（包括 `-NullRHI`）时，operation 在参数投影、截图 mechanics 或打开资产编辑器之前以
`capture_image_rhi_unavailable:<policy_id>` 失败关闭，不生成 artifact，也不提供无渲染
fallback。图片产品验证使用具备 RHI 的 owned worker；NullRHI 只验证这一失败合同和不依赖
渲染的 capture mechanics。

## 旧入口终态

旧 `/api/exec` 的 editor close、submix record、packaging task、localization external task、
world-partition resave、viewport screenshot 与 voxel-grid session command 均不在当前
immutable registry/manifest 中。旧 compiled registry、prefix/exact dispatch 与
`FUeAgentHttpServer` handler 已从编译树物理删除，不再存在一套可执行 tombstone registry；
未知旧 identity 只会在当前 exact ingress 以未注册 identity 失败关闭。

仍被 EditorProject、ModelingGeometry 与 CaptureMedia 当前领域 owner 复用的 UE mechanics
位于私有 `LegacyMechanics` 边界。该边界不拥有 listener、route、authentication、command
registry 或 dispatch，模块启动也不能实例化它；每个当前 consumer 只能显式调用其所需
mechanics。旧 generated spec/doc、hidden resave consumer、active legacy smoke/coverage，
以及 EditorViewport adapter 内第二套 screenshot 方法、辅助函数和 capability 广告均已从
活动入口删除。不得恢复
`GRecordSessions`、`FShotJob`、`GExternalTasks`、`GVoxelGridSessions`、ownerless task map、
delayed-exit wrapper、第二 screenshot 流程或 Python business owner。
