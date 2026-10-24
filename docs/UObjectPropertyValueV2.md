# UObject 属性值协议 v2

`uai.uobject_properties.v2` 定义 UAI 反射属性的 canonical value 和回写合同。完整状态捕获使用本文件的 canonical value；普通 semantic workspace 由公共模型层投影为可读简写，完整身份仍由原生状态和内部 canonical 持有。Apply 在已由反射确定真实字段类型时接受类型定向简写，不另建值协议。v1、`value_text` 写入、`property_name` 别名和 `ImportText` fallback 不属于 v2 输入。

## 字段契约

每个可写属性条目至少包含：

```hjson
{
  property_path: Intensity
  kind: float
  value: { literal: 5000 }
}
```

- `property_path` 是 owner 内唯一写入身份，不接受 `property_name` 或 `selector` 替代。
- `kind` 必须与 UE 反射得到的实际 `FProperty` 类型完全一致。
- `value` 是唯一写入来源。只要 `value` 存在但不合法，整次 owner apply 失败，不得回退到 `value_text`。
- `category_path` 不进入可写 descriptor、Details group field 或 `details/index.hjson`。公共反射 owner 仍捕获
  Category；semantic workspace 在同一对象的 `properties.hjson` 中把它投影为一次
  `categories.<完整 Category path>.<属性名>` 导航层，无有效 Category 时使用 `General`。类别 key 不参与
  apply 身份，精确属性仍由隐藏 manifest 恢复。
- `value_text` 只用于 debug/readback，或无法生成可靠 canonical value 的只读诊断；不能用于 authoring apply。
- 共享状态快照不随 Export profile 改变；profile 过滤在公共 projection 层处理 authoring envelope。`llm_authoring` 会在存在可靠同级 `value` 时移除 `value_text`，readback/debug 保留它，canonical `value` 内部的同名业务字段不受影响。

## 基本类型

bool、数值、字符串、name 和 text 使用 `literal`：

```hjson
{ kind: bool, value: { literal: true } }
```

```hjson
{ kind: int32, value: { literal: -12 } }
```

```hjson
{ kind: double, value: { literal: 0.125 } }
```

```hjson
{ kind: string, value: { literal: "text with {}[],:#" } }
```

Apply 可在 `value` 中直接提交对应的裸 bool、number 或 string；字段真实类型来自 UE 反射，不通过 JSON 值猜测类型。enum 的裸 string 按枚举项名称处理。完整状态保留 canonical value；普通 workspace 可以投影为裸值：

```hjson
{ property_path: bEnabled, value: true }
```

```hjson
{ property_path: Intensity, value: 5000 }
```

```hjson
{ property_path: DisplayName, value: "雷击区域" }
```

```hjson
{ property_path: Mobility, value: Movable }
```

### Text source 与本地化身份

FText 的 canonical value 为 `{literal: "..."}`，或带完整身份的
`{localized: {source: "...", namespace: "...", key: "..."}}`。`FText::FromString` 在 Editor 中并不保证
culture-invariant；UE 持久保存可以为尚无身份的文本生成本地化 key，因此不能把 literal 解释为
“强制不可本地化”，也不能仅比较显示文字而忽略显式 namespace/key。

普通 compact workspace 的 text 值显示 source 字符串。字符串修改由公共 prepared apply 按原生
基值表达 source 编辑，保留 UE 对本地化身份复用与冲突的规则；模型无需生成或维护保存产生的身份。
显式 `localized` 输入仍表示精确身份编辑，source 不变而 namespace/key 改变也属于实际修改。
完整 canonical 捕获、严格内部重放和恢复不使用模型面的简写。

文本身份在可回滚的应用阶段、expected capture 之前确定，proposed 与 actual 共用已解析结果。
prepare 不创建 namespace metadata；直接 prepared apply 由原 package-before 记录恢复本次文本
编辑修改的 metadata，再恢复 dirty 状态。独立 typed 输入的默认严格模式不因 compact source 编辑
而整体放宽。

整数 kind 精确区分 `int8/int16/int32/int64/uint8/uint16/uint32/uint64`。`int64` 和 `uint64` 的 `literal` 必须是十进制字符串，防止 JSON/HJSON number 丢失整数精度：

```hjson
{ kind: int64, value: { literal: "9007199254740993" } }
```

enum 使用枚举项名称；bitmask 使用枚举项名称列表：

```hjson
{ kind: enum, value: { selected: Movable } }
```

```hjson
{ kind: bitmask, value: { selected: [OptionA, OptionC] } }
```

bitmask 以目标属性的实际整数位宽解释名称。没有对应已选名称的原生置位以可选 `unmapped_bits` 十进制字符串保留；仅编辑 `selected` 时保留该字段，显式清除用 `"0"`。名称查找是否成功与 enum 底层值分开，合法的 `-1` 枚举值不是缺失名称。

`bitmask` 只在 `BitmaskEnum` 能通过 UE 反射可靠解析时成立；此时 descriptor 同时携带枚举约束，`selected` 必须使用真实枚举项名称。如果属性声明了 bitmask metadata，但当前反射生命周期无法解析对应枚举，Export 必须按属性的底层整数类型输出 `literal`，Apply 也按同一整数类型解码。不得输出缺少 enum 约束、无法验证或无法回写的伪 `bitmask`。这种回退保留完整位值；枚举重新可解析后才恢复名称列表形式。

## 固定分量类型

分量顺序由 `kind` 固定，不输出 `x/y/z/w/r/g/b/a` 字段名：

```hjson
{ kind: vector2, value: { components: [1, 2] } }
```

```hjson
{ kind: vector3, value: { components: [1, 2, 3] } }
```

```hjson
{ kind: vector4, value: { components: [1, 2, 3, 4] } }
```

```hjson
{ kind: rotator, value: { components: [10, 20, 30] } } // pitch, yaw, roll
```

```hjson
{ kind: quat, value: { components: [0, 0, 0, 1] } }
```

```hjson
{ kind: color8, value: { components: [255, 128, 0, 255] } }
```

```hjson
{ kind: linear_color, value: { components: [1, 0.5, 0, 1] } }
```

长度必须完全匹配。`color8` 分量必须是 `0..255` 整数，quat 写入后归一化。

Rotator 的 pitch/yaw/roll 数值单位为 degrees。只读 `type.value_semantics` 由公共类型 owner
提供，容器、嵌套和继承字段共享；显式 Rotator3d 保留普通 struct 的字段表示并提供 `field_units`。
Quat 的分量不是角度。字段声明的 `Units` / `ForceUnits` 分别保留在
`constraints.units` / `constraints.force_units`，不触发数值转换。

Transform 保留语义字段，但字段内部使用固定数组：

```hjson
{
  kind: transform
  value: {
    translation: [100, 0, 50]
    rotation: [0, 90, 0] // pitch, yaw, roll in degrees
    scale: [1, 1, 1]
  }
}
```

## Struct

通用 struct 使用 `fields`。每个成员按自身实际 `FProperty` 类型递归编码，不允许把 struct 成员降级为文本或统一裸值：

```hjson
{
  property_path: Bounds
  kind: struct
  struct_path: "/Script/CoreUObject.Box"
  value: {
    fields: {
      Min: { components: [-100, -100, 0] }
      Max: { components: [100, 100, 500] }
      IsValid: { literal: true }
    }
  }
}
```

authoring export 会递归只投影可编辑且安全的 struct 成员；无 `CPF_Edit`、`EditConst`、transient、deprecated 等内部成员不会进入 authoring value，但仍可进入 readback/debug 诊断。域级 provider 可以通过共享的三态 struct field override 明确授权由该域专用解析器管理的非普通 Edit 成员；未作决定时继续使用通用反射规则，显式拒绝时不得由通用规则重新放行。transient、deprecated 等危险成员不受域级 override 放宽。apply 接受部分 `fields`：显式成员按真实 `FProperty` 类型递归解析，缺失成员保留现值；创建新值时由 UE 初始化后的默认结构补全。未知成员、成员 shape 错误，或显式提交只读/危险成员仍会失败，不能借部分写入覆盖内部缓存。

`FInstancedPropertyBag`、`FInstancedStruct` 等动态结构沿用同一递归规则：动态 schema 必须先明确每个字段的真实类型，已知 Vector/Rotator/Color/Transform 成员仍按对应固定结构编码，不能因为外层是动态 struct 就降级为普通 object 或文本。apply 会按请求中的动态 `struct_path` 确认递归写类型一致。

普通 authoring 投影将“可编码”与“有可操作内容”分开：固定 Struct 在有效字段筛选后没有可操作
后代时省略，混合 Struct 仅省略无操作的子成员；固定数组按元素合同判断，长度本身不提供增删能力。
该筛选由公共 PropertyValueCodec 的 `ProjectAuthoringValue` 提供，Details、semantic workspace 和
UObject authoring export 共同消费；完整状态 capture、readback 与恢复不据此删除原生字段。
公共 native-normalization preview 把值回流为后续编辑输入时也使用同一投影；不会把原生归一化
产生的内部只读缓存变成显式写入。领域仍须单独保留完整预期/实际读回比较，例如碰撞profile派生状态。

PrimitiveComponent的完整`BodyInstance`行由公共property operation owner统一规范化：以真实实例或模板
为显式delta基线，在隔离预览中加载命名profile，再重放显式修改的ObjectType、CollisionEnabled和响应通道。
Level组件实例与Blueprint SCS组件模板共用该实现；没有编辑的同级值随profile派生，显式冲突遵循UE原生Custom语义。
此机制不处理StaticMesh的BodySetup碰撞几何，不改变完整快照与独立读回职责。
空动态 Array/Set/Map、Optional、可选择的 null owned 引用、动态 schema 和整体语义值仍保留各自
操作入口。当前筛选递归覆盖固定 Struct/固定数组及 owned 对象属性；owned 属性为空不隐藏引用
本身，内部完整状态仍保留。Map 的 mapped value 同样递归裁剪，key 及其定义保持；value 没有
可操作成员时保留空结构壳和条目增删能力。Array/Optional 同样递归投影内部字段，分别保留数组
顺序/数量和 Optional presence，空结构壳不表示删除元素。普通 Array 基值按位置补全，owned 数组
仍按实例绑定；Optional 分支切换不继承另一分支的 value，显式矛盾输入仍拒绝。InstancedStruct
内部字段也递归投影，保留 struct_path 和空 fields 壳；同类型保留省略成员，切换类型按新类型默认。
同静态类型的不同动态定义使用无歧义的可读序号引用，相同完整定义共享引用。PropertyBag 使用真实
字段类型递归投影内部值及定义，保留全部 schema 条目和必要字段壳；schema 仍是目标字段集合，
已有 schema/补全入口管理匹配字段的原生 ID、metadata 和基值。Set desired 元素也递归投影，
保留元素数量与空壳；完整 from 来源事实不裁剪，其身份合同保持。Map key 与 mapped value
共用递归投影，各自定义同步，entry 与必要 key/value 壳保留；key 的来源匹配和身份校验不变。
递归字段定义在 workspace 内集中引用，普通 bool、数值、enum 和 null 仍使用直接值，不因此重新
暴露 `literal`、`selected` 或 `path` 包装。

### 普通 object 内的 keyed 集合边界

普通 object 的省略字段默认保留捕获值。领域在普通属性内提供 keyed desired-state 集合时，
由服务端值定义的 `collection_semantics: keyed_desired_state` 标明完整成员边界；模型不改写该定义。
导入器在此边界把输入交给公共 keyed-item owner，不先补回被省略的成员。
省略整个集合保持原值，显式 `{}` 清空集合，集合中的成员省略表示删除；保留成员的子属性仍由
其正式属性 owner 按局部编辑合同解析。该声明不使任意 object 获得删除语义，也不免除原生校验。
StaticMesh `mesh.sockets` 是此边界的消费者，材料槽和碰撞属性仍沿各自原生属性合同。

## 容器和 Optional

容器条目必须声明递归内层类型，元数据由反射生成且不可改写：

动态数组带 `EditFixedSize` 时只能编辑原数量的元素，公共 prepared apply 在写入前拒绝增删，
错误码为 `edit_fixed_size_array_length_mismatch`。这与原生固定内嵌数组的 `ArrayDim` 长度检查分别处理。

```hjson
{
  property_path: Points
  kind: array
  element_type: { kind: vector3 }
  value: { items: [{ components: [0, 0, 0] }] }
}
```

```hjson
{
  property_path: Lookup
  kind: map
  key_type: { kind: name }
  mapped_type: { kind: vector3 }
  value: {
    entries: [{ key: { literal: Origin }, value: { components: [0, 0, 0] } }]
  }
}
```

- array/set/fixed array 使用 `element_type`。
- map 使用 `key_type` 和 `mapped_type`。
- optional 使用 `inner_type`，值为 `{is_set:false}` 或 `{is_set:true,value:<canonical>}`。
- fixed array 额外使用 `fixed_length`，值为 `{items:[...]}`，长度必须相等。
- 普通 workspace 的 array/set/fixed array 和 GameplayTagContainer 可直接提交 `[...]`；公共 codec
  根据真实反射类型复用 `{items:[...]}` 解码，同样适用于 property bag 的新增字段、空集合和嵌套值。
  canonical 导出保持原包装；元素类型、固定长度、重复项与身份规则不变。Map 仍使用 `entries`。
- 动态 Struct 数组的可编辑定义与当前个数无关。集中 `value_definition.element_definition`
  引用元素的公共类型/字段定义，`element_template` 给出原生初始化默认值；普通 workspace 中的模板
  已投影为可复制到数组的普通值。引用字段仍按对应定义的 `allowed_class` 选择真实资产。
  `items` 只描述捕获时已有项，模板不追加真实元素、不携带 `instance`，也不替代 owned 候选查询。
  非空数组仍保留同一新元素模板，不能拿首个已有项的修改值当默认。生成模板只初始化临时 Struct
  内存，不执行真实对象捕获回调或资源通知；递归元素类型在当前模板类型路径上止环。
- AuthoringState 对 Set/Map 生成稳定顺序时，值与逐项 definition 共同排序；定义仍描述原来的元素或
  key/value，不按排序前索引错配。普通 Struct、固定数组、Array、Optional 的递归捕获遵循同一对应关系。
  固定 Struct 数组的部分成员输入按每个元素自己的原生基值补全，省略成员不会从相邻元素复制。
- Map 的稀疏 value 按完成并解码后的原生 key 匹配旧值，保留未提交成员；新增 key 使用原生初始化
  默认。输入 entries 重排不改变该匹配，公共导入不按数组位置注入旧 value。普通导出原样提交不
  产生属性编辑，重复 key 仍在写入前拒绝。
- Set 使用完成并解码后的原生元素 equality 匹配已有基值，省略 payload 从匹配元素保留，新增元素
  使用原生默认；不按输入位置补值，不硬编码身份字段。身份相等只用于匹配和重复拒绝，公共语义
  比较另行检查元素内容，纯重排保持 no-op；只比较身份的自定义 equality 不能遮蔽 payload 修改。
  对递归含 authoring 过滤状态或开放动态类型的复合 hash 类型，集中类型定义声明 `hash_source`：
  既有 Set item 使用 `from`，Map entry 使用与 key/value 并列的 `from`，保存完整只读来源事实；
  该来源保持 codec 包装，仅用于原生身份匹配及完整事实校验，不能作为字段赋值输入。
  `from:null` 明确创建原生默认的新项；适用类型缺失来源、来源篡改或失配均拒绝，已有项改变
  native identity 也拒绝。需要替换身份时删除旧项并显式新建。desired payload 可继续省略并从
  匹配基值补全，只读赋值禁令不变。类型资格与当前空集合/unset无关，完整标量保持原表达。
  已验证只读字段参与 identity 的反射 Set/Map 及真实落盘 workspace 编辑、stale 拒绝。
- 合法递归结构没有固定人工深度上限。共享 Object Apply 与 PropertyValueCodec 不使用 `MaxDepth` 拒绝 authoring 输入；循环依据 UObject identity 的当前 active path 判断，深结构与环不能互相替代。资源计数仅用于观测，不作为额外准入配额。

## 引用

hard/soft/weak/lazy/class/interface reference 的 canonical value 都使用 `{path: ...}`；空引用使用 `{path:null}`。各引用类型有独立 `kind`，并携带反射得到的 `allowed_class`。Apply 在字段类型已知时也接受裸对象路径 string 或 `null`，并在共享 codec 中补全为同一引用结构；Export 始终保留 `{path: ...}`。

soft reference 的 authoring 校验只验证规范对象路径和类型约束，不为校验而强制加载资产；hard/class/interface reference 在提交前解析真实对象并检查类型。

### Owned 对象与有序实例

普通 `owned_object.properties` 使用 `Category.属性名: {type, value}`，与外层 UObject 属性
共用类型名、类别和行解析规则；内部 canonical 仍使用 `property_path/value` descriptor 数组。
已有对象的稀疏行按捕获定义恢复；新 `name/select` 对象的可选属性在原生 picker 初始化后，按
实际对象的反射 schema 解析，省略成员保留原生默认。集中定义的引用在普通 `definitions.hjson`
中提供唯一公开 `ref`，调用者无需读取隐藏映射才能解释类型或枚举合同。

可编辑 owned UObject 使用 `owned_object` 表达类与递归属性，继续由公共 Object Apply 检查真实类、
归属和引用。动态 owned 数组的普通 HJSON 保留数组顺序，每个已有非空条目在 `owned_object` 中携带
可读 `instance` 键；该键由语义类名与当前集合中的连续实例编号生成，不使用原生对象的历史名字。
编辑时保留该键即可修改、移位或保留同一个实例；删除条目表达删除该数组成员，空引用保留为空位置。
重新导出可以按新的集合重新编号，不能把旧 workspace 的实例键当作跨导出的永久对象身份。

精确实例定位保存在隐藏的递归 `value_definition` 中，由公共 keyed-item 投影与导入恢复；模型不编写
owner 路径或内部 selector。已有实例按捕获身份配对原对象，不按新数组索引、类名或属性内容猜测。
无 `instance` 的 owned 输入与已有实例绑定分开处理。已支持原生创建上下文的动态 owned 数组可使用
`{owned_object:{name:"查询返回的名称"}}` 新建；只有真实同名歧义才补 `select`，语法由公共 DomainKit
统一校验。新建不要求复制 `class_path` 或完整旧 `properties`；省略的值由原生创建初始化。
已有实例的普通表达为 `instance` 与 `properties`，精确类从该实例的捕获值恢复，不重复导出
`class_path`，也不使用 `name/select` 重选已有对象的类型。普通动态 owned 数组拒绝 `class_path`
输入；删除 `instance` 后复制类路径不能作为第二新建入口。内部 typed owned 值仍保留其独立类信息，
真正的类引用属性继续遵守前述引用合同。
缺失整个字段保持原值；普通无改动输入不会因恢复隐藏绑定而变成一次编辑。嵌套公共值定义保留，
只有机器绑定和没有业务定义的空占位树从公开定义中剥离。

动态Struct数组若包含owned后代，已有外层条目也携带局部键，例如
`{instance: Entry1, fields: {...}}`。外层键绑定该条目的完整捕获值和定义，内层owned对象仍使用
自己的`owned_object.instance`。重排时连同键移动；删除时移除条目；新增条目省略外层`instance`。
未知或重复键写前拒绝。该键只在本次workspace捕获范围有效，不是业务字段或跨导出永久身份。
InputMappingContext的`Mappings`使用此表示；空owned后代不会取消该类型合同。
固定数组、Set、不含owned后代的Struct数组和标量数组保留自身合同，不因序列化为数组就获得此身份语义。

`asset.owned_candidates` 接受真实资产 `target`，以及 `input.property_path` 和可选 `input.query`，
返回全部去重原生 `names`。使用返回值原样填写新增项的 `name`；需要其他名称时细化查询。
查询与创建消费同一公共原生 picker 上下文，领域只声明该上下文适用性。已验证的生产上下文包括
UE 5.6 独立 Niagara Script 的 `ValidationRules`，以及 Input Action 的 `Triggers`、`Modifiers`
和直接 `PlayerMappableKeySettings`；Input Action 的操作说明见
[T37](active/t37_asset_simple_operation.md)。其他 owner/property、固定数组与 Set 不据此视为支持。
InputMappingContext还支持已有mapping中的`Mappings[0].Triggers`与`Mappings[0].Modifiers`
这类路径：索引为当前真实数组的零基索引，查询不追加真实mapping、不使资产变dirty。
新增mapping可使用已查询返回的候选名称，但创建时仍校验实际目标的原生资格与归属。
未知上下文或不完整候选来源明确失败，不以局部目录冒充完整目录；候选发现不设置人工单次加载预算。
该Script数组已验证正常名称查询、最小创建、独立重载与精确再导出，以及未知名称拒绝。prepare之后
仅真实owner变只读时，原生真实属性句柄检查在首次Modify/槽位写入前拒绝，验证在rollback前完成。
新增已保存且从clean新包读回后拒绝的事务测试，分别恢复完整dirty内存、原引用和不同的原磁盘内容。
这些结果不外推整批跨属性变更或其他owner上下文。

## Apply 语义

1. 校验 schema、`property_path` 唯一性、可编辑性、`kind`、容器类型元数据和 canonical value。
2. 所有字段在临时属性内存中完整解码；任一字段失败时 owner 保持不变。
3. create/replace omitted 内容从 UE factory、constructor、CDO/archetype/template、struct initializer、schema 或真实 staged object 补全；provider 输入即使采用 codec shorthand，也必须先进入 typed storage，再 canonical encode/readback。多态 owned object 缺少 existing/explicit/可靠 class source 时返回 `owned_object_class_source_required`。
4. 预检全部通过后，在同一 transaction 中提交。
5. 写后使用类型语义比较。float/double 使用类型匹配容差，Rotator/Quat/Transform 按等价旋转比较；含 FText 的值不能由 Editor 的显示字符串 `Identical` 早退判等，必须保留 canonical 身份比较。其他类型可复用原生相同值判断及 canonical 比较。任一字段仍不一致时恢复整组快照。
6. 全部读回一致后才标记 package dirty 并返回 `applied`。

Folder grouped Details 可以保留 `availability.editable:false` 的诊断字段，但提交该字段仍会失败；通用 UObject apply 总会重新检查真实反射写资格，删除或篡改 availability 不能把 unavailable 字段变成可写。

delegate、multicast delegate、field path、Verse value 和无法递归编码的 opaque property 只进入 unavailable/debug 诊断，不进入 authoring apply。

`EditCondition` 的当前值描述 Details UI 可用性，不限制持久 authoring 写入。原生 Details 以复选框公开的 bool 控制字段即使没有 `CPF_Edit`，也属于可编辑声明：UObject 属性、递归 struct 类型及值投影、公共 apply 共用反射资格判断。控制字段由 PropertyEditor 的只读 `GetEditConditionToggleProperty` 解析；原生 UI 与该接口共用单 bool token、`HideEditConditionToggle`、`InlineEditConditionToggle` 和 legacy CVar 规则，插件不另建表达式解析器。相关源码补丁及部署要求见 [引擎补丁说明](../EnginePatches/README.md)。

导出中的控制字段使用真实 native 名称及 bool 值；明确设置它与受控值，才能表达启用或禁用意图。例如 Blackboard Vector 的 `DefaultValue` 与 `bUseDefaultValue` 是两个独立字段。只写默认向量不会自动启用开关，写入 disabled 值也不会产生条件门禁；普通只读、transient、领域范围和对象编辑上下文规则仍由各自 owner 判断。
