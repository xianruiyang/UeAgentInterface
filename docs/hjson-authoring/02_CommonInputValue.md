# Common Input Value Contract

本页只定义 `uai.uobject_properties.v2` 的通用 UObject/FProperty descriptor 与 value codec。领域结构由各自的动态 authoring schema 定义字段形态；只有领域 schema 明确引用本合同的值，才复用这里的 canonical value，不能把本页的 Transform 容器形态套用到所有领域。

普通 `llm_authoring` workspace 使用下述压缩投影，不照搬本页后续 canonical 的 `literal/selected/items` 包装。新增时以本次 `examples/` 和实际导出为准；例如普通 enum 是名称，普通 bitmask 是名称数组，携带 `unmapped_bits` 时才保留对象包装。

## UObject Descriptor

```hjson
{
  property_path: Intensity
  kind: float
  value: { literal: 5000 }
}
```

- `property_path`、`kind`、`value` 是可写 descriptor 的核心字段。
- `property_path` 是唯一属性身份，不接受 `property_name`、`name`、`selector` 替代。
- `value` 是唯一写入来源；authoring 不接受 `value_text` 或 `import_text`。
- `display_name`、`tooltip`、`availability`、`constraints`、`candidates` 只在确有额外编辑语义时保留。
- 普通属性赋值不使用 `operation/operations`。
- `control_type` 可由 `kind` 推导，不输出。

## Semantic Workspace Category Grouping

上述 descriptor 是公共反射/apply 合同；`llm_authoring` semantic workspace 会把同一 UObject 的属性进一步
压缩到该对象现有的一份 `properties.hjson`，并按 UE Property Category 在文件内分组：

```hjson
{
  categories: {
    Rendering: {
      bVisible: { type: "bool", value: true }
      Mobility: { type: "/Script/Engine.EComponentMobility", value: "Movable" }
    }
    "Transform|Location": {
      RelativeLocation: { type: "vector3", value: [0, 0, 0] }
    }
    General: {
      ComponentTags: { type: "name[]", value: [] }
    }
  }
}
```

- category key 是清理后的完整 UE `Category` path，以 `|` 保留层级；无有效 Category 的属性进入 `General`。
- Category 在每组只出现一次，组内属性以属性名为 key；`type` 表达真实值类型，`value` 是直接值。
- 导出条目的可选 `definition` 对应所属 `definitions.hjson` 内 `definitions` 对象的 key；同文件的 `enums[definition]` 提供该定义的枚举候选。嵌套字段/元素定义若是 `collection:...` 短引用，则匹配定义条目的 `ref` 字段，不从名称或编号推算。不能用 `type` 作为定义 key。上例省略只读引用；编辑值时保留实际导出的 `type` 和 `definition`。
- `float`、`bool`、整数等基础类型不因属性约束或候选变化而编号；enum/struct 使用完整原生路径，bitmask 和 PCG selector 分别表达为 `bitmask<完整枚举路径>`、`pcg_selector<完整结构路径>`，容器递归保留元素类型。`instanced_struct` 的实际载荷类型仍由值中的 `struct_path` 表达。完整属性定义按类型、可编辑性、约束和候选等共享，定义别名仅在所属目录有效，不是跨导出的类型身份。
- 定义去重保留候选顺序、作用域和非数值约束；数值上下限按精确数值归一，JSON 对象字段顺序不影响共享。不同候选集合不能仅因 enum 类型相同而合并。
- 定义别名也使用公共 `X`、`X#2` 规则：例如不同约束的条目可分别引用 `definition: float` 和 `definition: "float#2"`，但两者的 `type` 都是 `float`。同一完整合同只导出一个定义，属性各自的值不会因共享定义而合并。类、枚举和结构引用按真实类型身份复用，不按引用次数编号。
- Category 只用于导航，不是属性 identity 或 selector；精确 property unit、captured type/value 与 pointer 由隐藏
  `.uai/` manifest 恢复，模型不复制 `category_path`、`property_path`、GUID 或 hash。
- 普通文件只有 `categories` 一个可编辑入口，不同时保留平铺 `properties`。旧 flat workspace 必须重新
  export，不通过双读或兼容表继续编辑。
- Category 不自动决定物理分片；大对象只有命中通用 workspace 分片合同时才拆文件，不按每个类别建文件。

不知道空 `value: []` 中的成员形状时，按本次导出的 `examples/index.hjson` 查对应业务文件及指针。
示例消费完整类型定义与适用的创建规则，不根据基础类型短名或旧资产实例猜测。
示例文件与业务文件分离，详见 [Generated Examples](09_GeneratedExamples.md)。

## Component Property Application

同一 mesh component 的属性由公共 ObjectApply 先写完本批字段，再发送原生属性通知；
HJSON 文件或 category 的排列不代表编辑顺序。StaticMesh、骨骼网格别名/SkinnedAsset 的通知先于
材质槽等依赖通知，避免新材质被尚未设置的网格槽数裁掉。Blueprint 模板和 Level 组件共用此实现；
需要重构组件的 Level 生命周期仍使用其整体通知和对象快照恢复。

属性是否变化、编辑通知和模板传播使用同一公共值比较。字符串与本地化文本源文的大小写修改会实际写入并
通知；模板传播仍沿原生继承链筛选，实例自身的大小写覆盖也会保留。

AnimationData 表达持久化动画设置，不是 PlayAnimation 操作。原生叶属性通知完成校验后，公共层
保留本批写入后的 AnimationMode，并用完整 AnimationData 初始化动画实例，包括循环、播放、位置和速率。
不回填原生校验清除的无效动画引用；最终读回仍检查请求值。失败时先恢复同批原值，再通知，避免恢复中间态
再次清理材质或改变动画模式。纹理/Paper2D 等需要阶段间生成派生结构的对象继续使用既有 NativeStages，
不把组件批通知代替这些领域的阶段与快照合同。

## Canonical Values

| 类型 | `kind` | canonical `value` |
| --- | --- | --- |
| bool | `bool` | `{literal:true}` |
| 精确整数 | `int8/int16/int32/uint8/uint16/uint32` | `{literal:12}` |
| 64 位整数 | `int64/uint64` | `{literal:"9007199254740993"}` |
| 浮点 | `float/double` | `{literal:12.5}` |
| 文本 | `string/name/text` | `{literal:"Player"}` |
| enum | `enum` | `{selected:"ValueName"}` |
| bitmask | `bitmask` | `{selected:["FlagA","FlagB"]}` |
| Vector2/3/4 | `vector2/vector3/vector4` | `{components:[...]}` |
| Rotator | `rotator` | `{components:[pitch,yaw,roll]}` |
| Quat | `quat` | `{components:[x,y,z,w]}` |
| Color | `color8/linear_color` | `{components:[r,g,b,a]}` |
| Transform（仅 `uai.uobject_properties.v2`） | `transform` | `{translation:[x,y,z],rotation:[pitch,yaw,roll],scale:[x,y,z]}` |
| DateTime/Timespan | `datetime/timespan` | `{ticks:"9007199254740993"}` |
| 引用 | `object_ref/soft_object_ref/class_ref/...` | `{path:"/Game/A.A"}` 或 `{path:null}` |

64 位整数使用十进制字符串，保留完整精度；导入按目标的有符号/无符号范围解析整个字符串，不截取数值前缀、
不按八进制或十六进制解释，也不把溢出值截到边界。浮点值按目标原生精度写入，捕获和普通值比较不额外舍入。
普通整数也从完整数字词法判断整数性及范围；`1.0`、`1e3` 等数学上等价的整数写法可归一化，
真正的小数不会先经过 double 舍入后被误收为整数。
解析、传递和领域投影保留原始数字词法，再按目标原生类型校验与归一；诊断中的预期值和实际值也保留数值精度。
类型描述、编解码与值核验共用同一公共规则。核验按目标真实存储类型解释输入，不对所有数字套统一容差；
写入省略、默认值与集合键身份判断保留实际存储差异，结果核验才接受该类型已有的原生归一化语义。
完整 Transform 及领域明确声明的原生变换平移/缩放分量，结果核验采用 UE Transform 的逐分量
`KINDA_SMALL_NUMBER`（约 `1e-4`）等价范围：平移按其原生长度单位，缩放为无量纲。这样可接受原生
setter 的减法/加回所产生的浮点尾数；不舍入输入或读回值，超出范围仍报告具体分量及两侧值。
普通 Vector、float/double、未声明类型的数字，以及输入差量识别、写入省略、默认值和集合键的存储比较仍保持精确。
领域的局部变换可只声明实际出现的分量，不要求补齐未编辑的平移、旋转或缩放。
`string` 与 `text` 的源文区分大小写；`name` 仍遵循 UE 的原生名称身份规则。
DateTime/Timespan 的 `ticks` 同样使用完整的有符号 64 位十进制字符串。
原生 float 的标量、Vector/Rotator/Color 分量及 Transform3f 的平移/缩放遵守相同的可表示范围。
Quat 的有限非零输入先按方向归一化，再写入原生分量；输入尺度不改变方向，极大或极小尺度也使用相同规则。
时间轴的规范时间表达仍由领域 schema 定义；转换到原生 rate、tick、channel 或动画曲线时遵守其原生存储精度，
不会把超出原生宽度的整数截断，也不会把有限 double 静默写成无限大的 float。
图的位置遵循对应原生领域已有的取整语义；其整数存储范围在取整后检查，不静默溢出或钳位。

表中列出 canonical 包装形状。普通带类型的属性输入可省略 `literal`、`components`、`items` 等公共单字段包装；
固定分量数组长度必须完全一致，不接受 `{x,y,z}` 或 `{r,g,b,a}`。Transform 使用
`translation/rotation/scale` 三个字段。World、Level 或其他领域的 Transform 必须先读取该领域的动态 schema 与
[Space Contract](04_SpaceContract.md)，不得从这一行推断其输入形态。

### 单位与只读值语义

公共类型的 `value_semantics` 描述模型实际写入的值表示，集中保存在 definition 的 `type` 中；
Details 查询直接返回同一说明。Rotator/Rotator3f 给出 `unit: degrees` 和
`component_order: [pitch, yaw, roll]`；公共 Transform 给出 `rotation` 子值的同一合同。
显式 Rotator3d 保持原有普通 struct 形状，以 `field_units` 标明 Pitch/Yaw/Roll 为 degrees。
派生 struct 的同一说明只覆盖仍由原生 Rotator 声明的字段，不给新增或遮蔽字段推断单位。
Quat 的四个分量不是角度；普通 float/vector 和名称中含 Rotation/YPR 的字段不会自动取得角度单位。
数组、集合、Map、Optional 的类型说明随各自 element/key/mapped/inner type 传播，嵌套与继承字段
沿同一反射定义传播。Blueprint pin 的真实 struct 类型复用此说明；容器主类型说明属于元素或 Map key，
Map value 使用其 `value_type`。

`constraints.units` 保留 UE `Units` 声明的内部值单位；`constraints.force_units` 保留 `ForceUnits`
声明。UE 数值控件使用有效的 ForceUnits 固定解释和显示单位，否则使用 Units 并可能选择显示单位；
UAI 不复制控件的单位字符串解析或数值转换。顶层及嵌套字段从同一反射 owner 采集这些声明。
类型固有值语义与字段声明分别保留，字段显示设置不改写 codec 表示。声明缺失表示没有该项证据，
不能推断为无量纲。只读说明不构成新增必填项、写入值或单位换算指令，旧工作文件无需补写这些字段。
导入时公共类型投影直接跳过 `value_semantics`，不把它复制到属性 assertion。
保留导出的说明后修改 value 可以正常导入；说明本身不改变原生类型校验或解码。

领域数值须使用对应当前 input contract；例如 Niagara 的 `description` 和 `display_unit` 见
`../active/t18_niagara_domain.md`。显示单位本身不证明脚本计算单位，也不能据普通 vector3 推断欧拉角。

硬 UObject 引用在导入时会先解析已加载的 `UObjectRedirector` 链，再对最终对象执行 allowed-class 校验。重定向链循环或最终对象类型不兼容会拒绝导入；不能把 redirector 自身的类型当作目标类型，也不能依赖重启 Editor 后重定向对象恰好未加载的状态。

精确类型、struct、容器、optional、引用约束和 apply 原子语义见 `../UObjectPropertyValueV2.md`。

## Struct And Containers

专用结构转换按完整原生类型身份选择；没有专用转换的反射结构使用 `value.fields`，成员、容器和引用继续按
真实类型递归编码。同型动态结构也遵循这一规则，局部编辑保留未修改与未导出的成员。这里依赖 UE 已提供的
反射字段或领域描述，不能从任意未反射 C++ 内存推测字段布局。
结构包含不可编辑、不可序列化的原生成员时，不因此禁掉其他可编辑成员；这些原生成员由选定基线保留，
不作为新的可编辑 JSON 字段。索引成员继承容器声明的编辑范围，嵌套路径也须遵循其父结构的原生权限。
正式 value_definition 声明的结构字段拼错时明确定位失败；动态字典成员和已声明的集合新增语义继续适用。

普通 workspace 更新按导出基线提取普通反射结构的已改成员，未改成员保留 UE 原生更新后的值，不作为旧值约束。
直接 descriptor 的 `fields` 则以实际提供的成员为显式意图，包括等于当前值的成员；省略成员不参与意图核验。
写入、原生通知后的核验和保存重载后的核验共用这一成员边界。创建输入、容器成员清单及专用原子值仍保持各自合同，
同一原生字段在多个编辑位置被明确写成不同值时不会静默接受。

```hjson
{
  property_path: Bounds
  kind: struct
  struct_path: "/Script/CoreUObject.Box"
  value: {
    fields: {
      Min: { components: [-100, -100, -100] }
      Max: { components: [100, 100, 100] }
    }
  }
}
```

容器必须携带递归类型：

- array/set/fixed array: `element_type`
- map: `key_type` 和 `mapped_type`
- optional: `inner_type`
- fixed array: `fixed_length`

列表值使用 `{items:[...]}`，map 使用 `{entries:[{key:...,value:...}]}`。重复 set 项、重复 map key、错误固定长度和截断值都会在 preflight 失败。

## Enum And Candidates

enum descriptor 应携带 `candidates` 或 `candidates_ref`。LLM 只修改 `value.selected`，不能写展示名或底层数字。名称查找与底层值分开，合法的负值枚举不是查找失败。bitmask 的 `selected` 是候选名称列表，候选和导入按实际属性位宽解析；无效位索引或不能容纳的掩码明确报错。

bitmask 原生值中若有无法由已选枚举名称表示的置位，值对象还会导出 `unmapped_bits` 十进制字符串。例如 `{selected:["FlagA"],unmapped_bits:"2147483648"}`。编辑 `selected` 时保留该字段即可保留这些未修改位；显式清除用 `"0"`。只有剩余位非零时才导出这个字段，普通名称列表的形状保持不变。原生枚举若声明直接 mask 值，组合 mask 和符合目标位宽的负 mask 保留原有 HJSON 语义，不把 UE 菜单的单 bit 筛选当作转换限制。

`bitmask` 需要 UE 反射能够解析 `BitmaskEnum`。若 metadata 存在但枚举当前不可解析，公共 codec 会按真实底层整数 kind 导出和回写，例如 `{kind:"uint8",value:{literal:3}}`；不得构造没有枚举约束的 `selected` 列表。该规则由公共 UObject/FProperty codec 统一处理，不由资产域自行转换。

## Domain Values

领域结构不需要伪装成 FProperty descriptor。例如 Niagara module input 可以保留 `source/default/link/dynamic_input`，Sequence key 可以保留 frame/interpolation。领域层负责业务结构和字段形态；基础值只有在该领域 schema 明确引用本合同的对应 value codec 时才使用上面的 canonical 形态。特别是领域 Transform 可以由自己的 adapter 定义 components object、别名与规范化规则，不能与本页的 UObject Transform 互换。

领域 source 示例：

```hjson
{ value: { source: "local_value", literal: 12.5 } }
```

```hjson
{ value: { source: "default_value" } }
```

```hjson
{ value: { source: "linked_input", linked_parameter: "Particles.Velocity" } }
```

```hjson
{ value: { source: "dynamic_input", dynamic_input: { path: "/Niagara/Functions/Random/RandomRangeVector.RandomRangeVector" } } }
```

只有领域文档明确声明支持时才可切换 source。

## Details And Inline Seed

已有 owner 的持续编辑通过相邻 `details/index.hjson` 和 group 文件进行。创建期 owner 可以使用 inline `properties[]` 提供初始值。inline 与 grouped 使用同一 v2 descriptor、preflight、codec 和原子 apply。

同一 owner 不能同时提供 inline properties 和 Details ref，否则返回 `inline_grouped_details_conflict`。`raw_properties`、`readonly_properties`、`missing_properties`、`unsupported_properties` 属于诊断或只读内容，不是通用写入入口。

## HJSON Safety

字符串引用、注释符、冒号、花括号、反斜杠和换行由公共 HJSON writer 处理。批量编辑后必须重新 parse；不要用正则或字符串替换拼装值。

同行规则由公共 writer 统一执行：struct/object 递归有效字段数 `<= 6` 且子值可同行时同行；不含 object 的 list 整体同行并在所属 object 中只计 1 个字段。
