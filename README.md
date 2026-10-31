# UeAgentInterface

`UeAgentInterface` 是 Unreal Engine Editor 插件，负责在 Editor 内执行 UAI command 并返回结构化结果。
外部模型和自动化通过配套 `UeAgentInterfaceCMD` 的公开 Python SDK 或薄 CLI 调用，不直接访问插件内部
HTTP 端点。

当前构建和实践基线是 Windows 上带配套原生补丁的 UE 5.6.1。插件直接使用 PropertyEditor、UMGEditor、
AudioEditor 和 MetasoundEditor 的补丁接口；原版引擎不能仅复制本插件后直接编译使用。安装前先按
[引擎补丁说明](EnginePatches/README.md) 应用补丁并重编对应模块，再构建插件。其他引擎版本尚未据此证明兼容。

本 README 提供仓库入口与安装边界，命令参数由正式 manifest 和合同维护。配套版本、升级与旧版恢复
从 [UeAgentInterfacePak](https://github.com/xianruiyang/UeAgentInterfacePak) 进入；局部编译或实践证据不代表全部能力已经验证。

## 正式调用入口

本地项目通过托管连接使用 Python SDK：

```python
import uai

with uai.connect_project() as ue:
    result = ue.project.summary()

if not result.ok:
    raise RuntimeError(result.error)
```

单次查询或命令也可直接使用已安装的 CLI：

```powershell
ueagentinterfacecmd exec project.summary
```

默认从当前目录定位项目，其他目录用 `uai.connect_project(project)` 或 CLI `--project`。
独立部署的显式 Connection TOML 用法见 CMD `docs/PYTHON_SDK_AND_CLI.md`。

一条 SDK method、`Client.execute()` 或 `ueagentinterfacecmd exec` 只转发一条同名真实 command。复杂资产优先通过
`content_graph.export_workspace` / `content_graph.import_workspace` 进行结构化往返；图布局使用
`graph.layout.apply`。具体命令、参数和版本每次从正式 manifest 与生成的 CMD binding 取得，不在
README 维护容易失效的命令清单。

## 权威入口

| 对象 | 职责 |
| --- | --- |
| `Resources/UaiCommandManifest.json` | canonical command、参数与 wire 版本的正式集合 |
| `docs/UeAgentInterface_Usage.md` | 当前公开调用链、HJSON、读回和 Editor 生命周期说明 |
| `docs/UeAgentInterface_Status.md` | 当前架构、证据等级和已证明范围的有界状态 |
| `docs/contracts/` | 公开模块与领域合同 |
| `docs/active/` | 当前实现和验证边界；不是根本需求、任务状态或完成裁判 |
| `Source/UeAgentInterface/` | Editor 插件与 C++ 业务 owner |
| `Source/UeAgentInterfaceTests/` | 活动 Automation test source；测试存在不等于产品完成 |
| `UeAgentInterfaceCMD` 配套仓库 | `import uai`、Connection、薄 CLI、generated Python binding 与客户端测试 |

Manifest、generated binding、handler forwarding、mock 或测试发现只证明各自结构层。对外声明 command
可用还要覆盖正式 public ingress、唯一业务 owner、真实 UE mechanics、适用 compile/settle、持久化、
独立读回、失败语义和 cleanup。

## 目录生命周期

- `Config/`、`Content/`、`Resources/`、`Source/` 和上述活动文档是正常实现与维护入口。
- `Content/Materials/M_UAI_MetricGrid.uasset` 是建模默认尺寸材质，挂载为 `/UeAgentInterface/Materials/M_UAI_MetricGrid`。安装或分发插件时必须包含 `Content/`；它不依赖示例项目或 `/Game/UAI`。颜色和刻度原点存于使用它的组件，见 [T38 建模材质](docs/active/t38_modeling_geometry_operation.md#bundled-metric-material)。
- 当前没有活动 `Tests/Fixtures/` root。原五个不完整且无 consumer 的 manifest 已等形移到
  `retired/strict_v2/audit-fixtures/unbound_contract_manifests/`；该位置只保存最小失败设计证据，不是
  fixture truth 或自动发现入口。
- `retired/strict_v2/` 只保留历史迁移与审计证据，不是活动需求、任务状态、生成入口、runtime owner 或
  发布真源。普通实现发现默认排除该目录；只有历史审计、迁移或 T99 工作才按精确路径读取。
- 现有 `UAI.StrictV2.T99.FINAL.AUDIT` source 仍检查历史隔离根存在，但其静态 oracle 已包含过期的固定
  command 数量和缺失文档文本；在独立任务替代该 oracle、保存等价证据并完成构建/Automation 前，
  不得删除整个 `retired/strict_v2/`，也不得把它当作当前完成门禁。

仓库文本由根 `.gitattributes` 统一为 LF；不要依赖单台机器的 `core.autocrlf`，也不要把无关全树
renormalize 混入业务提交。

## 开发与验证

嵌入 `GptProjectTest` 工作区时，按外层 `AGENTS.md` 和 `docs/agent/UE_UAI_PROJECT_POLICY.md` 选择正式
构建、测试和 Editor 生命周期入口。C++ 修改后使用非 Live Coding 的普通 Development 构建；需要关闭
用户 Editor 时先处理 dirty resource 风险。局部结构或文档检查不能替代受影响 UE 行为的真实验证。

## 许可证

本仓库使用 MIT License，见 `LICENSE`。
