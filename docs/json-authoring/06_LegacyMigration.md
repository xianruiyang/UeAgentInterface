# Legacy Migration And Deprecation

本页定义旧结构、旧字段和 deprecated 命令在结构化 JSON authoring 中的迁移边界。它只记录长期稳定规则；具体命令字段和返回仍以单命令文档、adapter/profile、实际导出和 readback 为准。

## 新 Authoring 主路径

- 新 folder authoring 默认使用 `llm_authoring`。
- 新增候选优先来自 `node_catalog_search.items_tsv` 的 `path` / `name` seed。
- 公开主身份只推荐：
  - `path`：资产、类、对象、owner 或 script 有稳定路径时使用。
  - `name`：无稳定路径但当前上下文可解析的候选使用。
- `function_name`、`graph_name`、`variable_name`、`event_name`、`parameter_name`、`input_name` 等只作为槽位判别字段，不替代 `path/name` 主身份。

## 旧字段策略

这些字段只作为 legacy adapter 输入、debug 信息或 readback 证据，不作为新 authoring 推荐输入：

- `full_name`
- `asset_path`
- `class_path`
- `object_path`
- `package_path`
- `component_class_path`
- `script_asset_path`
- `module_script_path`
- `legacy_path`

迁移时按以下规则处理：

| 旧字段 | 新 authoring 目标 | 保留场景 |
| --- | --- | --- |
| `asset_path`, `class_path`, `object_path`, `package_path`, `component_class_path`, `script_asset_path`, `module_script_path`, `legacy_path` | 可解析为稳定实体时迁移为 `path`。 | legacy adapter、debug/readback、单文件旧 payload 兼容。 |
| `full_name` | 若能解析 owner/path，拆为 `path` 加必要槽位；否则只作为 display/readback。 | 搜索显示、差异证据、旧输入兼容。 |
| 旧 `class_path` Level folder actor seed | 迁移到 `actors/items/<actor_id>/actor.json#/path`。 | 仅限旧 single-json command、显式迁移或 readback/debug 识别语境。 |

新示例不要从旧字段反推 JSON。旧字段可被 adapter 接收，不代表它们是推荐 authoring 面。

## 旧结构迁移

| 旧结构 | 新结构 | 迁移规则 |
| --- | --- | --- |
| 旧平铺 `graphs/<name>.json` | `graphs/items/<graph_id>/graph.json` | 逻辑节点、pins、links 或等价内容进入 `graph.json`。 |
| graph 内混合 layout | `graphs/items/<graph_id>/layout.json` | 节点位置、注释框、折叠状态等视觉布局单独进入 `layout.json`。 |
| 旧 `widget_tree/tree.json` | `widget_tree.json` | UMG widget tree 默认合并为一个 authoring 文件。 |
| 旧 split `properties.json` 或 inline `properties[]` | `details/index.json` + `details/<group>.json`，或内嵌 owner 的 `*_details` 引用 | UE Details / UObject 属性面板字段进入公共 Details authoring 层；旧输入只允许显式迁移、负例、错误或 debug/readback 识别，不作为 authoring 输入。 |
| 旧 full-object `index.json` | summary-only `index.json` + `items/<id>/...` | `index.json` 只保留导航摘要，大型 item 按需打开。 |
| 无法识别或不可迁移字段 | `legacy/unmapped.json` + `validation/summary.json` | 不静默丢弃；保留摘要、原因和后续人工处理边界。 |

P3 legacy 验证证明旧输入可以按 contract/data-shape 迁移到新结构，但不表示每一种历史旧输入都有单独 UE legacy round-trip。真实 UE round-trip 证据仍以 export/apply/readback 验证和单命令文档为准。

## Deprecated 命令使用边界

`commands/deprecatedCommand/**` 中的命令不进入新 authoring 主流程，也不进入默认 command pack 示例。只有以下用途可以显式使用：

- compatibility
- migration
- diagnostic
- repair
- bootstrap
- schema_boundary
- smoke

调用 deprecated 命令时必须记录用途和迁移理由，并确认当前替代命令、folder workflow 或 JSON authoring 不能覆盖该操作。普通资产制作、节点新增、属性写入、UMG/Blueprint/Material/Niagara authoring 不应选择 deprecated 命令。

## 文档与 Pack 规则

- 新教程、single command docs 和 command pack 示例必须使用 `path/name` seed。
- `full_name/class_path/asset_path/object_path` 等旧字段只能出现在 legacy/debug/readback 说明、旧 payload 兼容说明或返回字段说明中。
- 如果命令缺 dedicated validate/diff，不得把 legacy migration 或 adapter 成功写成 full coverage。
- skill 同步文档不包含 `deprecatedCommand/**`；需要读 deprecated 文档时回到插件仓库归档，并说明兼容或迁移理由。
