# Direct JSON Authoring And Catalog Seed Chain

本页是 LLM 执行结构化 JSON 编辑的主入口。默认策略是直接编辑 `llm_authoring` folder JSON，只在批量生成、参数化调用、adapter 调试或重复变量过多时写 Python 辅助。Python 不能替代 JSON 意图本身。

本页只描述 UAI 结构化 JSON 内部链路；字段语义以本地 UAI 源码、adapter/profile、正式命令文档和实测 readback 为准。

## 主链路

```text
确认资产和 profile
-> export/readback 当前 folder
-> 先读 manifest/index/references
-> 按需打开 authoring JSON
-> 如需新增节点/模块/组件/控件/输入来源，先 node_catalog_search
-> 从 items_tsv 读取 path/name seed 写入目标 authoring 位置
-> 编辑普通 value/connection/layout/properties
-> validate/lint/adapter/apply
-> export/readback/compile/probe 验证
```

执行规则：

1. 先导出真实资产结构，再在 `llm_authoring` 文件上改。
2. 新增候选优先来自 `node_catalog_search`，分类不明确时先 `node_catalog_categories`。
3. `node_catalog_search.items_tsv` 是新增候选的压缩主返回；优先读取 `path` 列作为最小输入，无路径候选读取 `name` 列。
4. seed 只表达“要新增或引用什么”；放置位置、父级、连线、layout 或初始属性只有在当前意图需要时才额外写入。
5. 不要手写完整 readback 对象；真实 id、template、slot、pin、默认属性、generated/readback/debug 字段由 apply 后的 UE/UAI 补齐。
6. apply 成功后必须重新 export/readback；后续编辑以读回的新结构为依据继续。

## 统一公开身份

新 authoring 主路径只推荐两类主身份：

| 主身份 | 适用场景 | 示例 |
| --- | --- | --- |
| `path` | 已知资产、类、对象、owner 或 script 路径；搜索返回的 path-backed 候选。 | `{ "path": "/Script/Engine.StaticMeshComponent" }` |
| `name` | 无路径实体的内置 action、菜单项或上下文可解析候选。 | `{ "name": "Multiply" }` |

`function_name`、`graph_name`、`variable_name`、`event_name`、`parameter_name`、`input_name` 等可以作为槽位判别字段和 `path` 或上下文一起出现，但不是主身份。

旧字段如 `full_name`、`asset_path`、`class_path`、`object_path`、`script_asset_path`、`component_class`、`widget_class`、`node_class`、`function_owner_class`、`module_script_path` 只作为 legacy/debug/readback 兼容；新 authoring 示例不使用它们。

旧字段、旧平铺 graph、旧 widget tree、旧 index/properties 布局和 deprecated 命令的集中迁移边界见 `06_LegacyMigration.md`。

## Catalog Seed 形态

`node_catalog_search.items_tsv` 的 `path` / `name` 列是当前 seed apply 的主输入，不是 legacy patch。TSV 首行是 header：

```text
name	full_name	category	tooltip	path
Multiply	Multiply	Math	Multiplies inputs	Multiply
```

从 TSV 复制 `name` seed 到目标位置：

```json
{
  "operation": "add",
  "name": "Multiply",
  "properties": {}
}
```

如果 TSV 候选有稳定路径，seed 应优先使用 `path`：

```json
{
  "operation": "add",
  "path": "/Game/Materials/MF_Custom.MF_Custom",
  "properties": {}
}
```

## 直接写入支持面

| catalog / kind | 可直接用于 JSON 补全 | 推荐 seed | 补全与限制 |
| --- | --- | --- | --- |
| `actor_component` | 是 | `path` 或 `name` | 已知类路径直接写 `path`；无路径内置候选可写 `name`。属性需后续编辑。 |
| `umg_widget` | 是 | `path` 或 `name` | 自定义 WidgetBlueprint 已知路径直接写 `path`；挂载关系写 `parent_widget_id`。 |
| `material_expression` | 是 | `name`，明确 class 时可 `path` | 创建 expression 节点；连线、root input、属性需另写。 |
| `material_function_call` | 是 | `path` | Material Function 是资产实体，已知路径时不需要搜索。 |
| `niagara_dynamic_input` | 是，但只作为 input value | `path` 或 `name` | path-backed script 使用 `path`；子 input/data interface 需要 readback 后继续编辑。 |
| `blueprint_call_function` | 条件支持 | `path + function_name` 或搜索 seed | 必须有正确图上下文，pin default、连线和对象上下文通常还需补。 |
| `blueprint_event` | 条件支持 | `name` 或 `path + event_name` | 事件唯一性、绑定上下文和图类型会影响 apply/compile。 |
| `blueprint_variable_node` | 条件支持 | `variable_name`，可加 owner `path` | 变量必须存在；通常需要 `access` 或明确 get/set 语义。 |
| `niagara_module` | 仅匹配已有导出 module | `path` 或 `name` | 当前 adapter 用 seed 匹配已有 stack module 并编辑 input；目标 stage 没有该 module 时返回 `selector_not_found`。 |
| `niagara_graph_function` | seed 存在，需验证 | `path` 或 `name` | 实际写入以 `niagara_script_export_folder/apply_folder` 的图 JSON 和 readback/compile 为准。 |

判断可写时，以当前命令文档、catalog 类型和 readback 验证为准。`items_tsv` 中的 `path` / `name` 只表示可 apply 的推荐最小 seed，不表示可以省略目标上下文、连线、layout、properties 或编译验证。

## 写入示例

### Actor Component

```json
{
  "operation": "upsert",
  "path": "/Script/Engine.StaticMeshComponent"
}
```

### UMG Widget

```json
{
  "operation": "upsert",
  "path": "/Script/UMG.Button",
  "parent_widget_id": "RootCanvas"
}
```

### Material Node

```json
{
  "operation": "add",
  "name": "Multiply",
  "properties": {}
}
```

### Blueprint Callable Function

```json
{
  "operation": "add",
  "path": "/Game/Blueprints/BP_FunctionOwner.BP_FunctionOwner_C",
  "function_name": "MyCallable"
}
```

### Niagara Dynamic Input

```json
{
  "input_name": "Module.Offset",
  "type_info": "vector3",
  "value": {
    "source": "dynamic_input",
    "dynamic_input": {
      "path": "/Niagara/Functions/Random/RandomRangeVector.RandomRangeVector",
      "inputs": [],
      "data_interfaces": []
    }
  }
}
```

## 来源解析

刚搜索完候选或在 JSON 中看到 `path`/`name` seed 后，如需理解含义，可调用 `node_origin_resolve`。已知路径实体优先传 `path`：

```json
{
  "target": {
    "source": "name_reference",
    "path": "/Script/Engine.KismetSystemLibrary",
    "function_name": "PrintString"
  }
}
```

从 JSON 文件反查：

```json
{
  "target": {
    "source": "json_reference",
    "json_file": "<ProjectRoot>/tmp/uai_params/authoring.json",
    "json_pointer": "/graphs/items/main/graph.json/nodes/3"
  }
}
```

解析失败说明身份不足或当前环境缺源码/资产，不代表 JSON 可以靠猜继续写。

## 失败边界

以下情况必须停止或修正 JSON：

- path-backed 实体仍使用旧 `full_name`、`asset_path`、`class_path` 等字段作为主输入。
- 仅凭显示名写入本应有路径的自定义资产、类或对象。
- 从 `items_tsv` 解析出的 `path` / `name` seed 与实际放置位置不匹配。
- `write_support` 或 `support_reason` 表示当前 item 不能按目标用途写入。
- Blueprint/Material 从 pin 拉线搜索时没有传真实 pin context。
- Niagara module 不存在却直接把 module seed 当作新增 stack module 成功。
- apply 成功但 export/readback 没有出现目标对象、目标连接或目标值。
