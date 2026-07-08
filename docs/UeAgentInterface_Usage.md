# UeAgentInterface 使用入口

本文是 UeAgentInterface 的调用入口说明。完整命令参数、返回字段和边界以 `docs/commands/` 下的正式分册为准。

## 推荐入口

日常自动化优先使用已定位 `UeAgentInterfaceCMD` 仓库中的 Python `uai_core`，不直接手写 HTTP：

```text
<UaiCmdRoot>/cli/uai_core
```

`UaiCmdRoot` 通常是 `<ProjectRoot>/UeAgentInterfaceCMD`，但不保证在项目根目录下。找不到默认候选时，应先向用户确认是否已安装 UAICMD 以及安装路径；只有确认没有可用 UAICMD 或用户明确要求回退时，才使用 CLI。skill 场景的 CLI 回退入口固定为：

```text
<SkillDir>/tools/uai-cli.exe
```

推荐流程：

1. 定位 `UaiCmdRoot`，将 `<UaiCmdRoot>/cli` 加入 `sys.path`，导入 `UaiCore` 和 `uai_core.commands.*` wrapper。
2. 用 `ue.doctor()` 检查 UE Editor 服务连通性。
3. 单条命令优先用对应 wrapper；多步骤任务用 `ue.batch(..., stop_on_error=True)`。
4. 读取 `CommandResult.data` 获取业务返回，读取 `CommandResult.debug` 获取诊断信息，必要时读取 `CommandResult.envelope` 查看服务端原始 envelope；批处理通过 `BatchResult.results`、`failed_index`、`failed_command` 和 `failed_error` 定位失败步骤。
5. 默认传入 `response_options={"view":"llm_compact"}` 获取 LLM 友好的紧凑返回；需要完整诊断时显式请求 debug/raw 视图，不要让完整读回或大段调试字段默认进入 `data`。
6. 如果回退 CLI，则读取 `<UserWorkDir>/runtimeLogs/` 下的 report JSON。
7. 写入资产或配置后做 export/readback、compile、probe、screenshot、smoke 或 coverage 验证。

不要在命令行里内联长 JSON；复杂参数写入 JSON 文件。CLI 仓库开发、发布验证和 CLI 自身行为仍以 `UeAgentInterfaceCMD/docs/USAGE.md` 为准。

## 编辑器启动参数

UAI 自动化需要“有正常 UE 窗口、任务栏可恢复，但启动首态最小化”时，推荐在启动 UnrealEditor 时同时传：

```text
-UeAgentInterfaceAutoStart -UeAgentInterfaceStartMinimized
```

`-UeAgentInterfaceStartMinimized` 会在插件启动阶段启用 MainFrame 延迟显示：主框架创建完成后先对 `SWindow` 写入首次显示最小化状态，再放行 UE 自己的 `ShowMainFrameWindow`。该参数只影响启动首帧，不会在后续持续压制用户手动恢复窗口；如果 UE 已经创建完主窗口后才加载插件，则只会退化为一次普通最小化。

## 文档阅读顺序

1. `docs/commands/README.md`：正式命令总索引、任务路由、工作流优先级。
2. `docs/UeAgentInterface_Status.md`：当前能力与边界快照。
3. `docs/Workflow_ExecBatch_Practice.md`：批处理和高效调用。
4. `UeAgentInterfaceCMD/docs/USAGE.md`：CLI 具体参数和 report/log 行为。
5. `docs/commands/<编号>_*.md`：具体分册。
6. `docs/generated/command_coverage_matrix.md`：当前实现、文档、smoke 覆盖矩阵。
7. `docs/commands/deprecatedCommand/README.md`：废弃写入命令归档。

## 分册路由

| 范围 | 分册 |
| --- | --- |
| Core / Level / Assets / Landscape | `docs/commands/01_Core_Level_Assets_Landscape.md` |
| Blueprint | `docs/commands/02_Blueprint.md` |
| UMG / WidgetBlueprint | `docs/commands/03_UMG.md` |
| StaticMesh / EnhancedInput | `docs/commands/04_StaticMesh_EnhancedInput.md` |
| Material / Material Instance / Material Function | `docs/commands/05_Material.md` |
| Level Sequence / UMG Animation / Sequencer | `docs/commands/06_Sequence.md` |
| Niagara System | `docs/commands/07_Niagara_System.md` |
| Niagara Emitter / Renderer / Event / Parameter | `docs/commands/08_Niagara_Emitter.md` |
| Niagara Stage / Graph / Node / Module Input | `docs/commands/09_Niagara_StageGraph.md` |
| Modeling Mode | `docs/commands/10_Modeling.md` |
| AnimBlueprint | `docs/commands/11_AnimBlueprint.md` |
| Montage | `docs/commands/12_Montage.md` |
| Animation Assets / Skeleton | `docs/commands/13_AnimationAssets_Skeleton.md` |
| IK Rig / IK Retargeter | `docs/commands/14_IKRig_IKRetargeter.md` |
| Niagara folder format | `docs/commands/15_Niagara_FolderFormat.md` |
| Skeletal Mesh folder format | `docs/commands/16_SkeletalMesh_FolderFormat.md` |
| Control Rig / Shape Library | `docs/commands/17_ControlRig_FolderFormat.md` |
| Deformer / ML Deformer / Geometry Cache | `docs/commands/18_Deformer_MLDeformer_GeometryCache.md` |
| AI Behavior / Blackboard / StateTree / EQS / Navigation / Smart Object | `docs/commands/19_AI_Behavior_Blackboard_StateTree_EQS_Navigation_SmartObject.md` |
| Audio | `docs/commands/20_Audio.md` |
| Texture / RenderTarget / Media | `docs/commands/21_Texture_RenderTarget_Media.md` |
| Project Settings / Config | `docs/commands/22_ProjectSettings_Config.md` |
| Level Content JSON | `docs/commands/23_LevelContent_JSON.md` |
| DataAsset / PrimaryAsset / DataTable | `docs/commands/24_DataDriven_DataAsset_DataTable.md` |
| Level Topology / Streaming / World Partition / DataLayer / HLOD | `docs/commands/25_LevelTopology_Streaming_WorldPartition_DataLayer_HLOD.md` |
| Physics / Chaos 基础 | `docs/commands/26_Physics_Baseline.md` |
| Localization / Packaging / Platform Profiles | `docs/commands/27_Localization_Packaging_PlatformProfiles.md` |
| Node Graph 通用排布 | `docs/commands/28_NodeGraph.md` |
| PCG / Procedural Content Generation | `docs/commands/29_PCG.md` |
| PCG folder format | `docs/commands/30_PCG_FolderFormat.md` |
| Probe / Candidate Search | `docs/commands/31_Probe.md` |
| Editor Viewport / Unified Preview | `docs/commands/32_EditorViewport.md` |

## 返回读取与紧凑输出

Python `uai_core` wrapper 返回 `CommandResult`：

- `result.data`：业务字段，默认应是紧凑、可扫描的结果。
- `result.debug`：调试诊断，仅在需要定位问题时读取。
- `result.envelope`：服务端原始 envelope，用于兼容或排障，不作为日常业务读取入口。
- `result.artifacts` / `result.warnings`：文件产物和警告摘要。

批处理返回 `BatchResult`，重点读取 `success`、`failed_index`、`failed_command`、`failed_request_id`、`failed_error` 和 `results`。不要从旧兼容字段读取业务结果。

推荐默认使用：

```python
result = command_wrapper(
    ue,
    response_options={"view": "llm_compact"},
    **params,
)
```

只有在明确需要完整诊断、原始读回或高成本明细时，才请求 debug/raw 视图。默认 `llm_compact` 会把长列表、大对象、截图、报告、完整 JSON dump 等内容压缩为摘要和 artifact path。

## 调用格式

底层 HTTP 协议仍是 `POST /api/exec`：

```json
{
  "request_id": "task-001",
  "command": "command_name",
  "params": {}
}
```

CLI `batch` 文件通常写成：

```json
{
  "stop_on_error": true,
  "steps": [
    {
      "request_id": "step-001",
      "command": "get_world_state",
      "params": {}
    }
  ]
}
```

## 资产编辑原则

默认不要用一长串原子命令手搓完整资产。优先级：

1. 单文件 JSON：小型资产、DataAsset/DataTable、配置、曲线、RenderTarget 等。
2. 文件夹式 JSON：Blueprint、UMG、AnimBlueprint、Material、Sequence、Niagara、ControlRig、SkeletalMesh 等。
3. Level Content / Level Topology JSON：场景 Actor 内容和关卡拓扑。
4. 动作命令：compile、screenshot、runtime probe、build、import、packaging。
5. 原子写入命令：只用于 bootstrap、迁移、探针、局部补修和故障恢复。

标准节奏：

```text
bootstrap -> export -> refine -> validate -> apply -> export/readback -> verify
```

## 写入后必须检查

写入 JSON、folder JSON 或 `value_text` 后，检查命令返回中的诊断字段：

- `issues[]` / `warnings[]`
- `property_results[]`
- `requested_value_text`
- `applied_value_text`
- `property_value_read_back`
- `property_import_status`
- `property_import_verified`
- `property_import_error`
- `value_text_exact_match`
- `value_text_changed_after_import`
- `cpp_type`

`json_file` 读取失败、JSON 解析失败、属性不存在、ImportText 失败或 folder 可选文件存在但不可解析，都必须当作需要处理的失败，不要继续下一步写入。

## 长任务和编辑器生命周期

Packaging、Localization commandlet、部分 build/probe 属于长任务，必须使用 `timeout_seconds` 和显式许可参数，例如 `allow_uat=true`、`allow_commandlet=true`。

关闭编辑器前的安全顺序：

1. `editor_list_dirty_resources`
2. `editor_resolve_dirty_resources`
3. `editor_close`

出现 UE 崩溃或 UAI 服务异常退出时，先收集 crash context、UE log、CLI report 和本轮 runtime log，定位根因后再继续同类命令。

## 覆盖状态

截至最近一次 coverage：

- 命令总数：`953`
- 缺正式文档：`0`
- 缺实现：`0`
- 缺 smoke：`0`
- actionable smoke debt：`0`

覆盖状态以 `docs/generated/command_coverage_matrix.md` 为准。
