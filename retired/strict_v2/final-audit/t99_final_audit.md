# T99-FINAL-AUDIT 最终复核投影

## 职责与权威

本文是 UAI 统一架构产品侧的最终复核投影，固定最终验证需要聚合的产品入口、范围与失败条件。交付目标只来自项目工作区 `workflow.json` 登记的当前 `requirements.md` 与 `user-design.md`；任务合同、状态和结果只来自该工作区 `task-table.json` 登记的目录。本文、旧 `plan.json`、旧 `state.json`、planning-audit、旧 evidence envelope 和任一 taskctl 命令都不能单独裁决整体完成，也不得从旧实现、旧测试或旧文档反推需求。

- 工作区：`docs/plan/20260806_UAI_REQUIREMENT_DESIGN_REALIGNMENT/workflow.json`
- 任务入口：同目录 `task-table.json`
- 执行边界：本任务只读审计正式投影，只通过任务结果保存复核摘要；不得修改或同步被审对象，不承担新的业务迁移；`no publish`、不提交、不推送、不自动归档。

## 活动指令与分发投影

- `Plugins/UeAgentInterface/Resources/UaiCommandManifest.json` 是唯一 C++ command registry 的生成投影：schema 为 `uai.command_manifest.v1`，来源为 `cpp_immutable_command_registry_export`。
- 当前 command 数量、ID 唯一性和逐条任务分配必须在执行复核时从正式 manifest 与当前 inventory 动态取得；本文不固化易过期数量。
- `UeAgentInterfaceCMD/cli/uai/commands/generated.py` 与 `generated.pyi` 只承载由同一 registry 派生的 SDK/CLI binding，不拥有第二套 UE 业务、alias、prefix 或 fallback 权威；旧 `cli/uai/generated_commands/` 已离开活动路径。
- 活动正式文档位于 `Plugins/UeAgentInterface/docs/active`、`docs/contracts`、`docs/hjson-authoring` 与 `UeAgentInterfaceCMD/docs`；已退休的逐 command 页面不得重新成为活动入口。
- Pak 中 `ue-agent-interface` skill 是分发真源，installed skill 只允许由其真实 owner 通过 `scripts/skills/sync_codex_skills.py` 执行 `pak-to-installed` 同步；T99 只检查一致性，发现漂移时返回对应 owner，不在最终复核中执行同步。

## HJSON 公共递归与领域覆盖

公共链由 T09 登记并解析唯一领域 provider/初始化合同，由 T13 消费公共初始化图完成正常 UE 生命周期的递归创建、默认保留、引用绑定、持久化和独立读回。领域不得复制递归、默认、引用绑定或清理职责；支持创建的领域必须登记唯一初始化合同，不支持的创建必须在第一次写入前明确拒绝。

最终聚合覆盖以下 15 个领域生产范围：

1. Blueprint；
2. UObject/DataAsset；
3. UMG/Widget；
4. Animation Blueprint；
5. Material/Function/Instance；
6. Niagara；
7. Sequence；
8. Animation/Curve；
9. Rig/IK/Skeleton；
10. Level/World；
11. PCG；
12. AI Behavior；
13. Audio；
14. DataTable/CurveTable；
15. External Payload。

## 公开链聚合

最终验证聚合 10 条代表产品与生命周期公开链，以及 1 条最终 production-evidence 聚合链：

- 完整内容操作；
- SDK/CLI 真实服务等价；
- 节点与 Component 候选直接创建；
- 指令发现与内容身份直达；
- 主 Editor、普通构建与 owned worker 生命周期；
- 长任务重连、取消、状态未知与清理；
- 全部 command 唯一 disposition 与权威；
- HJSON export；
- HJSON import；
- HJSON 领域覆盖；
- production-ready 声明的公开真实与有界证据聚合。

T99 只聚合当前计划修订下各依赖任务的 fresh receipt；它不得把静态文件存在、编译、mock、旧测试或过期 receipt 扩张为真实 UE mechanics、持久化、独立读回、失败终态或清理结论。

## 旧权威退出与完成条件

- `legacy_asset_retirement_manifest.json` 固定 T90 的 23 项资产裁决和 50 个 task-helper 隔离映射；旧 command source、一次性生成器、旧 command-specs/command pages、旧 fixture/snapshot 与空文档入口已离开活动路径。
- 活动 mechanics 仅在仍有真实消费者时保留；保留不等于继续拥有第二权威。
- 最终 Automation oracle 当前标识为 `UAI.StrictV2.T99.FINAL.AUDIT`；`StrictV2` 只是既有测试身份命名，不是活动计划或状态入口。Python 语义回归、inventory/census 一致性和适用的 skill 同步检查必须同时通过。
- T99 只能提交最终复核结果摘要，不得修改 runtime、manifest、inventory、SDK binding、正式文档、交付链文档或任务合同；整体完成仍须逐项确认当前 `REQ/AC/UDES` 的直接证据、适用 `CON`、全部 `DCR`、旧权威退出和无适用验证失败。任务状态或 CLI 诊断不能替代该判断。

任何未知 command、重复 owner、未分配 disposition、缺少领域合同、领域私有递归、过期 receipt、旧权威重新进入活动路径、installed skill 漂移，或未授权发布行为，均使最终审计失败。
