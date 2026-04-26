# UeAgentInterface

`UeAgentInterface` 是一个 Unreal Engine Editor 插件，用于在编辑器内启动本地自动化服务，让外部工具可以通过结构化命令驱动 UE 资产制作、检查和调试。

本仓库是 `GptProjectTest` 项目中的独立插件仓库，外层项目通过 submodule/gitlink 引用它。

## 代码来源说明

本仓库的初步代码是使用 GPT 编写，并在实际 Unreal Engine 项目中持续调试、修复和扩展。后续维护应继续以真实 UE 编译、命令回读、Stack/日志验证和文档同步为准，不能只依赖生成结果。

## 主要能力

- Editor 内 HTTP/UAI 服务入口。
- 资产编辑命令：Blueprint、UMG、Material、Sequence、Montage、AnimBlueprint、Niagara、StaticMesh、EnhancedInput、IK、Modeling、Level 等。
- JSON / 文件夹式结构化工作流：优先通过导出、编辑、应用、回读的方式维护复杂资产。
- Niagara 工作流：System / Emitter / Script folder profile、Stack issue 读取、Stack quick fix、compile log、runtime probe 和刷新路径。
- Smoke test 与命令级验证辅助。

## 推荐使用方式

不要直接访问插件内部 HTTP 端点。外部自动化应通过配套 CLI 仓库 `UeAgentInterfaceCMD` 的 `uai-cli.exe` 执行：

```powershell
.\UeAgentInterfaceCMD\dist\uai-cli.exe doctor --json-output
.\UeAgentInterfaceCMD\dist\uai-cli.exe batch --file .\some_batch.json --json-output
```

复杂资产制作应优先使用 JSON 或 folder JSON 工作流：

- `blueprint_export_folder / blueprint_apply_folder`
- `umg_export_folder / umg_apply_folder`
- `material_export_folder / material_apply_folder`
- `sequence_export_folder / sequence_apply_folder`
- `anim_blueprint_export_folder / anim_blueprint_apply_folder`
- `niagara_export_folder / niagara_apply_folder`
- `niagara_emitter_export_folder / niagara_emitter_apply_folder`
- `niagara_script_export_folder / niagara_script_apply_folder`

原子命令主要用于 bootstrap、诊断、迁移和小范围修复。

## 目录结构

- `Source/UeAgentInterface/`：插件源码。
- `Config/`：插件配置。
- `docs/UeAgentInterface_Usage.md`：总使用说明。
- `docs/UeAgentInterface_Status.md`：能力状态记录。
- `docs/commands/`：命令分册。
- `docs/commands/deprecatedCommand/`：已被 JSON / folder workflow 覆盖的兼容写入命令归档。

## 构建与验证

常用构建命令在外层项目执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\ue\BuildEditor.ps1 -UERoot "D:\Epic Games\UE_5.6" -ProjectPath .\GptProjectTest.uproject -Target GptProjectTestEditor -Configuration Development -NoHotReloadFromIDE
```

如果 UE Editor 正在运行且占用插件 DLL，应先通过 UAI 的安全关闭流程处理 dirty 资源，再编译。

## 许可证

本仓库使用 MIT License，见 `LICENSE`。
