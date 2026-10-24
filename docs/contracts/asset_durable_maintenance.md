# AssetDurable Journal Maintenance Contract

本文定义 SimpleAuthoring 资产持久化 journal 的生产恢复职责和公开维护入口。它只处理
durability 证据与 journal 终态，不拥有或重放创建、编辑、保存、删除等原业务操作。

## 唯一职责链

| 职责 | owner |
|---|---|
| journal schema、完整性、pre/expected artifact state、phase、归档与碰撞保护 | Core `AssetDurable` |
| package/config artifact root 与 package registry/loaded/dirty 独立确认 | SimpleAuthoring durable service |
| dormant exact authority、字段合同与 machine envelope | `asset-durable-maintenance-owner` |
| manifest 与 Python 方法 | immutable registry 导出与 binding generator |
| read→exact binding→reconcile 的公开调用面 | Python SDK `DurableJournalHandle` |

`operation.reconcile` 只属于 long-running operation lifecycle，不接受 durable journal；
AssetSimple mutation owner 也不拥有 recovery。公开 handler 不解析 journal、不选择
backend、不比较 artifact state，也不实现回滚或保存。

## Journal identity 与 evidence

新 journal 使用 `uai.c8d.asset_durable.journal.v3`。每个 artifact 在变更前记录
`presence/content_hash`；成功 publish 后记录期望持久状态。durable owner 在同一项目实例中
稳定，fencing token 由 integrity-protected `fence-state.json` 单调分配，不再借用 operation
session 的易失 owner/fence。

旧 v1/v2 journal 只在其已保存的 inventory、backup marker/data、owner、fence、plan 和
integrity 足以形成同等证据时迁移读取。缺失证据不得用当前状态倒推历史预期。corrupt、
foreign 或 stale journal 按单项隔离，不能清空或遮蔽其他有效结果。

同一 journal id 的 completed 基础收据只有在 integrity、owner、fence、plan、inventory 与
terminal phase 全部精确匹配时才构成终结 tombstone。tombstone 存在后，任何不同 phase 的
陈旧写入都以 `durable_journal_already_terminal` 拒绝，不能重新创建 active journal。无效文件
或不同 binding 仍只作为归档碰撞证据保留，不得伪装成终结裁决。

## SDK-internal commands

- `asset_durable.journal.read`：只读取一个 exact active journal，验证 schema、
  integrity、路径和 inventory，并返回稳定 owner/fence、plan/inventory、phase、pre/expected
  state 及一次新鲜 artifact readback。它不改变 journal phase。
- `asset_durable.journal.reconcile`：由 `DurableJournalHandle` 原样转发上一步的 journal、owner、
  fence，并在 SDK 内生成 verification identity；Core 再次读取并绑定证据，SimpleAuthoring
  oracle 再确认真实 package/config 状态，然后才能形成 terminal receipt。

两条 exact command 均为 dormant machine service，不进入模型的 search、usage 或 named SDK
方法。模型只能从原业务 `Result.durable_journals` 取得类型化句柄，调用 `inspect()` 或
`reconcile()`；journal、owner、fence、verification、plan fingerprint 与 inventory hash
只保留在 SDK machine envelope。

具体字段、错误、调用模板和 contract hash 以运行中 Editor 的
`query.command.search/usage` 与按生成 SDK、正式 schema 与领域文档 为准，不在本文复制生成签名。

## 结果语义

| 观察 | 结果 | journal |
|---|---|---|
| pre-publish phase 且当前精确等于 pre-state | `rolled_back` + `aborted_before_mutation` | collision-safe archive |
| published/recovery/state_unknown 且只匹配 pre-state | `rolled_back` + `restored_original` | collision-safe archive |
| published/recovery/state_unknown 且只匹配 expected persisted state | `verified` + `verify_persisted` | collision-safe archive |
| 历史 active fork 与 completed terminal 的 immutable binding 精确一致，且独立 readback 再确认当前状态 | 沿用该 terminal 的 `rolled_back` 或 `verified` 及其既有恢复语义 | 保留基础收据，active fork 以碰撞安全后缀归档 |
| 同时匹配或两者都不匹配、legacy 证据不足 | `state_unknown` + typed failure | 保持 active |

reconciliation 的 `replayed` 必须始终为 `false`。现有同 ID completed receipt 不得覆盖；
若新 terminal 内容不同，使用 phase/hash 后缀保存新收据并保留旧证据。
历史 fork 路径以 completed terminal 证明既有终态，以新鲜 readback 证明当前可观察状态；它不把
当前 absent/present 反推成旧 legacy pre-state，也不重放 restore、publish 或原业务命令。

## 类型化维护入口

SDK 句柄依次调用 dormant read 和 reconcile，模型公开结果只保留 phase、status、
recovery_action、replayed 与短错误码。`verified`、`rolled_back` 为终态；`state_unknown`
禁止自动重试原业务命令。已删除要求模型传递 journal id、verification id、contract hash
和 receipt 绝对路径的旧项目 runner；需要持久审计时由机器侧消费者保存完整 envelope，
不得把原始 receipt 放入模型上下文。

## 验证边界

Development 构建、Core state-bound reconciliation、prepared restart、terminal tombstone
拒绝陈旧写入、历史 active fork 闭合、坏 journal 隔离、public command authority/forwarding、
manifest equality、动态教程完整性与 runner 单元测试分别证明其直接范围。文件存在或
`ok=true` 单独不证明业务资产正确；terminal 仍要求 Core 绑定证据和 SimpleAuthoring 独立
readback 同时成立。
