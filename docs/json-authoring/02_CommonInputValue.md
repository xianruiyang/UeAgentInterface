# Common Input Value Contract

本页是所有结构化 JSON authoring 的通用 input/value 教程。只要字段表示“某个可写值”，都应优先使用这里的形态；模块分册只补定位方式和特殊验证。

## 基本原则

1. 可写值统一放在 `value` 对象、域内结构化值或 `value_text` 中。
2. `value_kind` 是紧凑编辑语义类型，例如 `bool`、`number`、`enum`、`vector3`、`asset_path`、`struct`。
3. `control_type` 表达 UE 编辑器控件语义，例如 checkbox、number、enum、object picker、collection。
4. 普通改值只改 `value`、域内业务值或 `value_text`，不改 metadata。
5. `candidates[]` 是 enum/choice/object picker 的可写候选；LLM 只能写候选里的值。
6. `display_name`、`tooltip`、候选展示名、默认值、raw enum options、override 状态属于只读辅助，不表达写入意图。
7. `import_text` 只作为兼容 fallback；生成新 authoring 时不要优先使用它。
8. JSON 输出使用统一 compact writer：默认尽量压缩为单行；对象和数组在长度阈值内递归内联，超大结构才按统一规则折行。不要在单命令里绕开该 writer 自行 pretty print。

## 通用类型写法

| `value_kind` 类别 | 写法 | 示例 |
| --- | --- | --- |
| `bool` | `value.literal` | `{ "value": { "literal": true } }` |
| `int` / `float` | `value.literal` | `{ "value": { "literal": 12.5 } }` |
| `string` / `name` / `text` | `value.literal` | `{ "value": { "literal": "Player" } }` |
| `enum` / choice | `value.selected` 或域内 `value_text` + `candidates[]` | `{ "value": { "selected": "NewEnumerator2" } }` |
| `vector2` / `vector3` / `color` / `rotator` / `quat` | `value.components` | `{ "value": { "components": { "X": 1, "Y": 2, "Z": 3 } } }` |
| `transform` | `value.translation`、`value.rotation`、`value.scale` 或域内 transform object | 需同时带 `space_contract` |
| object/class/asset reference | 公开 `path` seed 或域内明确引用对象 | `{ "value": { "path": "/Game/Foo.Bar" } }` |
| folder/package path | 明确语义 path 字段 | `{ "value": { "path": "/Game/Generated" } }` |
| curve | `value.curve` | `{ "value": { "curve": { "schema": "ue_agent_interface.curve.v1", "keys": [] } } }` |
| array/object | `value.items` 或域内明确结构 | 保留 JSON 原生结构，不转成字符串 |

旧 `asset_path`、`class_path`、`object_path`、`script_asset_path` 等仅用于 legacy/debug/readback 兼容；新 authoring 主路径优先使用语义明确的 `path`。

## Enum 规则

可编辑 enum 必须有候选项：

```json
{
  "input_name": "Randomness Mode",
  "value_kind": "enum",
  "candidates": [
    { "name": "NewEnumerator0", "value": 0, "display_name": "Simulation Defaults" },
    { "name": "NewEnumerator2", "value": 2, "display_name": "Non-Deterministic" }
  ],
  "value": {
    "selected": "NewEnumerator2"
  }
}
```

规则：

- LLM 写 `value.selected`。
- 写入值必须来自 `candidates[]` 中的候选身份。
- `value.display_name` 不写。
- `candidates[].display_name` 只读，用于显示候选语义。
- enum 的数字值、raw `enum_options`、UE 内部 display name 留在 readback 或 debug。
- 缺少候选项的 editable enum 是 lint error。

## Source 层

有些 input 不只是“改本地值”，还可以切换来源，例如 default、linked input、dynamic input。统一表达为 `value.source`，但只有对应域声明支持时才可用。

当前已支持的典型来源是 Niagara module input：

```json
{ "value": { "source": "local_value", "literal": 12.5 } }
{ "value": { "source": "default_value" } }
{ "value": { "source": "linked_input", "linked_parameter": "Particles.Velocity" } }
{
  "value": {
    "source": "dynamic_input",
    "dynamic_input": {
      "path": "/Niagara/Functions/Random/RandomRangeVector.RandomRangeVector"
    }
  }
}
```

通用规则：

- `source` 表示值从哪里来，不替代 `value_kind`。
- `local_value` 后面仍按普通 value kind 写 `literal/selected/components/curve`。
- `default_value` 表示清除本地 override/link，回到默认。
- `linked_input` 必须写明确目标参数。
- `dynamic_input` / `make` 优先写 search 返回的 `path` seed；无路径候选才写 `name`。
- 也可以把 `node_catalog_search.items_tsv` 解析出的 `path` / `name` seed 放在 input 的 `value` 位置，由 adapter 解包。
- 旧 `script_asset_path` / `node_name` 只作为显式迁移、debug/readback 识别字段，新 authoring 主路径不使用它们。
- 未支持的 source 必须明确失败，不得静默忽略。

## Operation 边界

普通值编辑不用 `operation`：

```json
{
  "input_name": "Spawn Rate",
  "value_kind": "number",
  "value": { "literal": 120.0 }
}
```

`operation` 只用于非普通值意图，例如：

- `add`
- `remove`
- `replace`
- `clear`
- `rename`
- `set_binding`
- `link_existing`
- `create_if_missing`

如果只是把 float 从 10 改到 20，使用 `operation` 反而会让语义变复杂。

## 只读字段

以下字段不应进入可写 authoring 主面：

- `has_override`
- `has_links`
- `generated_class`
- `engine_version`
- `*_count`
- `file`
- `complete_inventory_path`
- `validation`
- `compile_report`
- `runtime_probe`
- `roundtrip_status`
- raw `enum_options`
- raw `input_type` 显示名

需要这些信息时放到 readback 或 debug。

## Details 属性面

UE Details / UObject 属性面板字段默认不在入口 JSON 内展开 `properties[]`。有稳定输出文件边界时，公共层会把这类数组外置为 `details` 或 `*_details` 引用，并在相邻 `details/<owner_or_field>/index.json` 与 group JSON 中保存可编辑字段。LLM 编辑 group `fields[]` 的值，不编辑入口里的旧 `properties[]`。

旧 inline `properties[]`、`settings_properties[]` 等输入只允许由显式迁移工具、reader 诊断或负例测试识别；新导出和新 authoring 不把它们作为 authoring 面。`raw_properties`、`readonly_properties`、`missing_properties`、`unsupported_properties` 不属于默认可写面，也不作为 apply 输入。
