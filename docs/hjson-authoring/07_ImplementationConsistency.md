# Public Authoring Consistency

本页定义结构化 HJSON authoring 在当前公开架构中的一致性检查。它只检查仍由正式
owner 维护的链路，不恢复旧 `uai_core` checker、命令 pack、任务级 audit 或旧 release
入口。

## Authority Chain

公开能力沿以下单向链路成立：

1. C++ 冻结 command registry 定义公开 command identity 与参数合同；
2. `UeAgentInterface.ExportCommandManifest` 把当前 registry 显式导出到 `Resources/UaiCommandManifest.json`，`tools/uai_binding/generate_bindings.py` 再从该投影生成 Python binding；
3. named method、`Client.execute()` 与 CLI 共用同一个 Client、Protocol 和 Transport；
4. command 经正式 ingress 进入 CommandService，再进入唯一 C++ 业务 owner；
5. 写操作由对应 owner 完成 compile/settle、持久化和独立 readback。

HJSON 的 schema、workspace、selector、operation 和 readback 语义由服务端 authoring
owner 持有。Python/CLI、skill 文档和测试不得复制这些业务决定。

目录 workspace 的机器基线由公共 composition 与 SemanticWorkspaceResolver 共同维护。
`unit_inventory` 行保留原 inline `envelope` 读取，也可通过互斥的 `envelope_ref` 指向
`.uai/baselines/` 中的 HJSON；文件根以稳定 unit identity 为 key，值为完整 captured envelope。
目录 manifest 的 inventory 可保持 inline `unit_inventory`，或通过互斥的
`unit_inventory_files` 引用 `.uai/inventory/` 下的文件；每个文件仍包含同一 `unit_inventory`
行数组。公共 resolver 合并后统一校验完整身份、引用与基线，跨分片重复同样拒绝。
embedded workspace 保留自包含的 inline 形式，不从外部解析 inventory 文件。
导出按已有物理文档归属稳定分片；多个文档可共用机器文件，使局部属性变化只使对应分片
失效，并控制文件数量。公开可编辑文件不变。
引用必须是工作区内受管相对路径；文件内容、单位身份和完整基线继续参与解析、版本/源指纹、
原子发布及独立读回。成功的内容证明可复用，mtime 不构成证明；引用文件缺失、非法或变化时
不能沿用旧的解析或语义结果。该机器表示不由 Python/CLI 或 Agent 手工维护。

## Current Entry Points

`scripts/ue/UpdateUaiCommandSurface.ps1` 是显式刷新命令 manifest 与 Python binding
的开发动作：从当前 registry 导出 manifest，再由 generator 重建 binding。真实可用性由
当前改动直接相关的 public input、UE 状态和独立 readback 证据判断。

`editor.session` 与 `capture.image.execute` 的策略参数、subject、用法和结果说明由对应
C++ policy descriptor 持有，注册时完整投影到 variants；固定 `editor.safe_exit` 由自己的命令
拥有合同。修改 descriptor 后须使用新构建刷新命令面，不能只修改生成 binding 或静默隐藏缺失合同。
图像策略的副作用按所选策略说明判断，例如 PCG viewport 的 Actor framing 会保留相机变换。

## Command And Candidate Discovery

- 已知 command 且签名已明确时，直接使用生成的 named method。
- 不知道 command 时，先调用 `query.command.search` 取得名称，再用
  `query.command.usage` 批量取得 named 最小调用；复杂 schema、variant 或失败语义以生成 SDK、正式 schema 与领域文档为准。
- 动态调用使用 `Client.execute(command, params)`；机器版本校验由 SDK 和 wire 内部完成。
- factory、node、component、actor 或其他 candidate 查询返回的 identity 必须可原样
  进入对应 create/apply 输入；调用侧不得建立别名表或领域映射。
- HJSON 文件导航只依据正式 export profile、`included_files`、`read_order`、manifest、
  index 和 ref，不扫描 sibling 或历史文档猜测字段。

## Failure Handling

1. 先确认失败属于 manifest/binding、Client/Protocol/Transport、CommandService、领域
   owner、持久化/readback、正式文档或 Pak/installed 同步中的哪一层。
2. 在该层的唯一 owner 修复，不在 SDK、CLI、skill 或单条 route 内增加业务兜底。
3. command 返回成功但 UE 状态或独立 readback 不符合语义时，停止依赖该成功结果，
   保存输入、Result、readback 和运行日志，并修复服务端 owner。
4. 只在当前完成结论需要时取得直接覆盖受影响行为的证据；不把测试、全消费者复跑或
   Pak/installed 同步设为继续开发的前置。

## Current Boundaries

- 正式能力全集与参数只由当前 manifest 定义，不在本文硬编码命令数、profile 数或
  domain 数。
- `query.command.search/usage` 与按生成 SDK、正式 schema 与领域文档 只解决公开 command 发现；领域 candidate discovery
  仍由对应 C++ owner 返回可直接消费的 identity。
- 真实可用性只由当前正式入口的实际结果与所需 readback 支撑；生成、同步、测试或
  文档本身都不签发领域完成。
