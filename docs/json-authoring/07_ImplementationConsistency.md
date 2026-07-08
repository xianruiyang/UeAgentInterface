# Implementation Consistency

本页定义结构化 JSON authoring 的实现一致性检查入口。它用于确认 docs、source contract、Python wrapper、command pack、search/show 输出和实际导出 profile 的语义一致；不是宽泛搜索源码或文档目录后的人工判断。

## Entry Points

| 层级 | 入口 | 检查内容 | 使用时机 |
| --- | --- | --- | --- |
| Authoring consistency | `uai_core.authoring_consistency.run_authoring_consistency_checks(...)` | folder registry、field contract、export profile、command docs、Python wrapper、`show_command`、`pack_commands`、seed 规则 | 修改 contract、profile、field、wrapper、command docs、JSON authoring 文档后 |
| Command toolchain audit | `python UeAgentInterfaceCMD/tools/audit_command_toolchain.py --project-root <ProjectRoot> --fail-on-error --output <ProjectRoot>/runtimeLogs/uai_command_toolchain_audit.json` | 插件/UAICMD/Pak/installed 命令文档同步、active wrapper import、manifest 污染、旧 wrapper import、关键 search/pack 示例 | 怀疑命令查询包、wrapper 或文档同步不一致时 |
| Command docs tests | `python -m pytest UeAgentInterfaceCMD/tests/test_command_docs.py -q` | `search_commands`、`show_command`、`pack_commands(profile=authoring)`、`json_authoring_docs`、seed 和 sections contract | 改 command docs API、USAGE、命令文档路由后 |
| Authoring consistency tests | `python -m pytest UeAgentInterfaceCMD/tests/test_authoring_consistency.py -q` | checker 正向路径和失败 issue 字段 | 改 consistency checker 或矩阵相关文档后 |
| Skill sync check | `python scripts/skills/sync_codex_skills.py --direction pak-to-installed --skills ue-agent-interface --mode check` | Pak skill 与 installed skill 受管文档一致性 | 改 skill 路由或 skill 内正式文档后 |
| Release gate | `python UeAgentInterfaceCMD/run_uai_cli.py release ci --json-output --ci-output <ProjectRoot>/runtimeLogs/uai_release_ci.json` | coverage、sync-check、Pak layout、hygiene、Dev/Pak CLI tests、quality gate | P5 发布门禁 |

`run_authoring_consistency_checks` 返回的 report 包含 `schema`、`ok`、`matrix_loaded`、`matrix_check_count`、`checked_type_count`、`checked_command_count`、`issue_count`、`deferred_checks` 和 `issues`。每个 issue 必须包含 `check_id`、`object`、`evidence`、`expected`、`actual`、`rollback_target`；失败处理必须按这些字段回到对应 contract、profile、wrapper、docs 或验证任务修正。

## What Is Checked

| 分组 | 证据入口 | 失败归属 |
| --- | --- | --- |
| `folder` | `load_command_folder_contract_registry`、operation status、export/apply/validate/diff 注册 | `P2-001` 或对应 folder contract 任务 |
| `field` | `load_field_contract`、字段 role/source/apply/readback、duplicate group | `P2-002` |
| `type` | `build_export_profile_plan`、`llm_authoring` included/excluded files and fields | `P2-003..P2-007` |
| `legacy` | `json_authoring_seed` normalization、legacy migration docs、deprecated command boundary | `P1-004`、`P2-008`、`P4-005` |
| `command` | `build_command_index`、`show_command(..., output_format="json")`、`pack_commands(profile=authoring)` | `P2-009`、`P4-003` |
| `docs` | command doc index、`skill_doc_path`、`json_authoring_docs`、guide route | `P0-003`、`P4-001..P4-006` |
| `gate` | P3 fixture reports、P5 release gate report、skill sync check | `P3-009`、`P5-001..P5-003` |

当前 `show_command(..., output_format="json")` 的正文必须在 `sections`，不是 `content`。`pack_commands(profile=authoring)` 必须包含 `Authoring Contract`、`minimal_seed`、`python_import`、`skill_doc_path`、`json_authoring_docs` 和 seed/coverage/raw/debug 边界。

## Failure Handling

1. 如果 checker 或 audit 输出 `ok=false` / `success=false`，停止继续 authoring 或发布任务。
2. 先读取失败 report，不用宽泛 `rg` 扫全源码或全 docs。根据 `check_id`、`object`、`evidence`、`expected`、`actual` 定位唯一真源。
3. 按归属修正源头：插件正式 docs、contract registry、field contract、export profile、Python wrapper、command docs API、Pak skill 源或 installed skill 同步。不要只改生成副本、Pak 镜像或 installed skill 来掩盖源头不一致。
4. 修改后重新运行对应最小入口；若失败跨层，先修复最底层 contract/profile/wrapper，再修 docs/pack/skill。
5. 只有 P3/P5 已标记为 deferred 的项可以进入后续门禁；不得把 deferred 写成已验证通过。

## Search Boundary

日常 authoring 和文档修正必须使用明确入口：

- 已知命令：直接 `pack_commands([...], profile="authoring")` 或 `command pack`。
- 未知命令：先 `search_commands(...)` 或 `command search` 定位命令名，再 `show_command` / `pack_commands`。
- JSON authoring 规则：只读包内列出的 `json_authoring_docs` 和当前命令 `skill_doc_path`。
- 迁移规则：读 `json-authoring/06_LegacyMigration.md`，不要从 deprecated 命令推断新主路径。
- 同步状态：用 `audit_command_toolchain.py` 和 `sync_codex_skills.py --mode check`，不要手动比较全目录。

不得把宽泛搜索源码、旧模板、旧大文档、runtime log 或 generated 产物作为常规字段发现路径。只有在上述入口自身失败、证据不足或正在修复工具实现时，才可以限定到相关模块做局部源码定位，并在修复后回到 checker/audit 验证。

## Current Boundaries

- P2 consistency report 已覆盖 21 个 matrix check、24 个 type、52 个 command，当前 issue count 为 0。
- P3 阶段没有 blocking gap；剩余 gap 为文档或边界说明，不代表 dedicated validate/diff 已完整覆盖。
- `FC-002`、`DOC-001`、`CMD-004`、`LEGACY-*`、`TYPE-*` 等仍应在 P5 gate 中复查。
- 真实 UE fixture、Pak/installed sync 和 release gate 中的 deferred checks 必须在 P5 作为门禁处理。
