# Niagara Apply Domain

## 1. Scope

Niagara authoring registers one mutation owner, `niagara.apply`, for the following semantic targets:

- Niagara System、standalone Niagara Emitter 和 standalone Niagara Script 资产生命周期。
- External script modules and Assignment Modules in System, embedded Emitter, standalone Emitter, and standalone Module Script stages.
- Dynamic inputs attached to module inputs.
- Niagara module and dynamic-input script references.
- System emitter handle completion from a standalone Emitter source, followed by recursive renderer, Event Handler and stage/module Apply in the same transaction.

The provider uses the shared Workspace Apply coordinator for preflight, rollback capture, mutation, optional compile, shared state readback and semantic verification, optional save, then commit. Verification always precedes save so a failed semantic readback cannot persist an unverified mutation. Export remains independent and does not consume Apply input contracts or provider extensions.

## 2. Registered contracts

| Target | Apply input contract | Workspace contract | Supported operations |
| --- | --- | --- | --- |
| System / Emitter / Script asset | `uai.apply_input.niagara.asset` | `uai.apply_workspace.niagara.asset` | `create`, `update`, `upsert`, `remove`, `clone` |
| Stack module | `uai.apply_input.niagara.module` | `uai.apply_workspace.niagara.module` | `create`, `update`, `upsert`, `remove`, `clone` |
| Dynamic input | `uai.apply_input.niagara.dynamic_input` | `uai.apply_workspace.niagara.dynamic_input` | `create`, `update`, `upsert`, `remove` |

Dynamic input does not advertise `clone`. A correct clone must preserve the complete nested input graph and Data Interface state; replacing a link with the same script is an update, not a clone.

Module and dynamic-input contracts are separate so unsupported operations and selector fields are not advertised through a union contract. Both contracts still dispatch to the single `niagara.apply` provider.

## 3. Identity and state

The Niagara domain owns canonical identity models for the shared read-only asset projection, System, Emitter, script, stack entry, module, and dynamic input. All of them use `niagara.state` for readback.

Asset identity is based on canonical asset path and asset kind. Stack identity includes the owning asset, readable stage usage, optional emitter selector, and entry selector. Internal module and dynamic-input identities remain server/readback state; they are never candidate search selectors or model-facing discovery fields.

Mutation verification reads the real UE object or Niagara graph after execution. A successful command result is not sufficient by itself.

## 4. Native defaults

The domain registers distinct default providers rather than one provider with changing provenance:

- `niagara.factory`: System、Emitter 和 Script creation through UE factories；Script 复制 `UNiagaraEditorSettings` 配置的对应 UE 原生 Default Script，保留 native Source/Graph，缺失时明确失败而不伪造默认图。
- `niagara.schema`: deferred stack defaults created by Niagara stack APIs.
- `niagara.domain_native`: script, module, Assignment target, and dynamic-input defaults supplied by Niagara domain APIs and `FNiagaraConstants`.

Apply completion may accept more information than Export displays. Missing generated content is obtained from UE factory, schema, or domain-native behavior; Export fields remain controlled only by Export profiles.

## 5. Script resolution

`ResolveScript` remains the resolver for existing module assertions, dynamic
inputs, and non-candidate internal references. New external Stack modules use
the contextual candidate chain instead: `niagara.module.candidates` resolves
the exact System/readable-usage/emitter Stack group and enumerates that group's
native Add Module actions.

Aliases that describe the same script must resolve to the same value. Conflicting aliases fail in preflight with `niagara_script_reference_conflict:*`. Unknown scripts fail with `niagara_script_not_found`. Missing dynamic-input references fail with `niagara_script_reference_missing`. These failures never become warning-and-skip mutations.

For native module creation in an exported ordinary workspace, the new module
row contains top-level `name`, alongside ordering and inputs. Keep the returned
base name and add top-level `select` only when the service supplies semantic
choices for an ambiguity. The shared keyed-item projection preserves both fields
in the internal candidate envelope. For an explicit inline provider-extension
document, HJSON contains `candidate: {name: ..., select: {...}}` (omit `select` when unnecessary)
inside the target stage module entry. Stage identity,
ordering, inputs, and operation remain outside `candidate`; the caller does not
copy a script path or binding revision from search. The server re-enumerates the
same native Stack action set in preflight and immediately before mutation. A
System target executes the selected
`INiagaraStackItemGroupAddUtilities::ExecuteAddAction` on its real Stack. A
standalone Emitter uses an exact transient placeholder copy to execute that same
native action and obtain its dependency-resolved Module index, then applies the
same Script, exposed version, and final index to the unchanged original Stage
through the shared exported Stage helper. The placeholder is neither persisted
nor copied back as an object graph. The exact stage context and returned scoped
name select the native action together. Current creation support is explicitly limited to `asset_module`;
scratch-pad, assignment/set-specific, and wizard actions remain visible in
search but fail before mutation. Dynamic-input search and creation are a
separate surface.

Module selectors for existing entries use dedicated fields such as
`module_selector_script_asset_path`; they are not merged into dynamic-input
script references or candidate identity.

## 6. Mutation and rollback

Asset create and clone use UE factory or asset duplication paths. Asset update mutates the existing object and never copies an entire unrelated UObject over it. Whole-object property copying is unsafe for Niagara because internal scripts and script sources have ownership relationships that must remain intact.

For a System update, an emitter entry may provide Apply-only `source_asset_path`. If `target.emitter_name` does not resolve to an existing handle, Apply asks Niagara to create the handle from that standalone Emitter and its exposed version, captures the generated handle ID, then resolves all nested renderer, Event Handler and stage/module targets against the new handle. If the handle already exists and its UE parent matches `source_asset_path`, `source_sync_mode` selects the UE-native baseline operation before explicit nested patches: `merge` is the default and calls `UNiagaraEmitter::MergeChangesFromParent()` while preserving local overrides; `replace` creates a fresh parent-backed instance from the current source while preserving the existing handle ID, name and enabled state. Use `replace` only when the source is intentionally authoritative and stale local overrides must not survive. A conflicting parent path or version fails preflight instead of silently reusing unrelated content. An explicit handle ID is rejected for a missing target because UE owns generated handle identity. Optional `emitter_version` overrides the exposed source version.

Renderer overrides follow UE's renderer-object granularity: an authored change to one renderer property can preserve that renderer's other values during source merge. Attribute-binding derivatives are refreshed for the source, previous-source snapshot and instance before comparison, so binding-cache initialization is not mistaken for an authored override. Explicit renderer patches still apply after source synchronization.

Target and source paths must remain valid long package names under registered UE mount points. Resolution checks an already-loaded object inside its package, then a complete object path, then disk loading. This supports unsaved in-memory UObjects without admitting unregistered paths such as `/Temp`.

The same input is idempotent. If the named handle already exists, Apply reuses the handle, performs the requested matching-parent source synchronization, and repairs System integration instead of adding a duplicate. `replace` changes only the handle's parent-backed emitter instance; explicit owner properties, renderer patches, Event Handlers and stage patches in the same request are applied afterward and therefore have higher priority. Integration rebuilds System spawn/update emitter nodes, synchronizes the overview graph, recomputes emitter execution order, invalidates cached data and broadcasts the structural post-edit change before compilation. This step is required: `enabled=true` on the asset is insufficient when a runtime instance still reports the emitter as `Disabled` and never ticks.

Native asset-module creation preserves the exact contextual Add Module action
identity and its dependency-resolved placement for both System and standalone
Emitter targets.
Existing module move/remove, Assignment creation/replacement, and dynamic-input
replacement continue through the shared Niagara stage/module helper. Assignment
creation uses Niagara's native `AddParameterModuleToStack`; target defaults come
from `FNiagaraConstants::GetAttributeDefaultValue`, not values invented by the
authoring provider. Replacing a dynamic input removes its owned nested value
nodes before changing the link, preventing orphaned graph nodes. Clearing a
dynamic input follows Niagara's native Reset semantics: it removes the
module-input override pin after deleting owned value nodes, so the module
script's UE-native default becomes active again instead of leaving an empty
synthetic literal override.

Asset operations capture an asset-level rollback object. Stack operations capture the owned Niagara graph and restore that graph on mutation, compile, verification, or save failure. The shared operation coordinator also captures the incoming dirty state of every package reported by a prepared state and restores it after the editor transaction is cancelled; a failed command therefore preserves both semantic content and the caller's original dirty state. Renderer rollback preserves the original UObject names and ordering so repeated failed transactions remain addressable by stable renderer identity. Existing-handle source refresh participates in the same graph, emitter-property, renderer and Event Handler rollback capture. A `replace` refresh also retains the previous instance until commit and restores it before graph and structure rollback if a later nested mutation, compile, verification or save phase fails. Emitter completion additionally records every handle created by the transaction and removes those handles in reverse order during rollback.

Preflight performs no mutation. Emitter Owner and renderer property groups are projected through the shared ObjectApply descriptor contract during preflight, including schema, property path, type/value conversion and renderer binding payload validation. This validation uses the current owner, the authoritative source baseline for add/replace completion, or a transient renderer probe as appropriate. Invalid nested fields fail in workspace preflight before source synchronization, renderer replacement, graph mutation, compile, save, or rollback is entered.

## 7. Export 与 Apply 公共入口

Niagara System、Emitter 和 Script 的结构化 authoring 只有一组公共入口：

- Export 使用 `content_graph.export_workspace`，Apply 使用 `content_graph.import_workspace`；二者分别进入共享 Export/Workspace Apply 纵切，不再公开 Niagara 专用 folder command adapter。
- System target 使用 `asset_kind=system`，Emitter target 使用 `asset_kind=emitter`，Script target 使用 `asset_kind=script`。
- Directory、single-file、inline 输入共用 loader、preflight、atomic transaction 与 snapshot verification；Export 执行 `niagara.state snapshot -> shared projector -> shared HJSON writer`。

这次入口收敛不按命令数量删除仍有真实消费者的 typed operations。`docs/active/t51_niagara_command_migration.md` 登记的六个 T51 typed commands 继续承担 HJSON 尚未覆盖的资产、compile/settle、compound apply 与 stack issue 等合同；它们不是 Niagara folder authoring 的第二入口。

standalone Script 的 `provider_extension.script_usage` 支持 `Module`、`Function`、`DynamicInput`，省略时为 `Module`。当前结构化 Stage authoring 只用于 `Module` usage；它复用相同的 stage/module/Assignment/recursive dynamic-input provider，而不是维护第二套 Script 图实现。Function/DynamicInput 可完成原生生命周期创建，但向它们提交 `stages` 会返回明确的 usage unsupported 错误。

普通 `llm_authoring` workspace 按公共类型规则拆分集合、模块与输入文件；沿导出 `content_ref` 定位，不假定单一 index 或旧 Niagara 专用 folder 布局。集合键是可读实例身份，UE 的 GUID 留在机器捕获层，不要求模型维护。

Assignment 模块以原生节点展示标题生成可读集合键、分片路径与语义引用，三者复用同一公共命名政策，重名沿用公共别名消歧；原生函数名、节点名与隐藏 selector 保持不变。所有 stage 和 standalone Assignment 共用该规则，不要求调用者改名或提取名称中的 GUID。

模块执行顺序由 `module_index` 表达。`ordering.hjson` 是按该原生顺序派生的导航引用，不构成另一套模块排序输入；移动模块仍编辑模块行的 `module_index`。层级集合的 `parent` 与兄弟 `index` 也不应解释为一条全局执行序列。

新增普通模块行使用查询返回的顶层 `name`，例如：

```hjson
Collision: {
  name: Collision
  module_index: 5
}
```

已有模块保持导出形状，输入遵循其描述符。删除普通模块时，从完整模块集合索引删除对应条目；公共 owner 生成内部删除操作，不在普通行写 `remove:true`。公共 keyed-item owner 将新行转换为内部候选并保留已捕获身份。显式内联领域文档另用 `provider_extension.stages`、`candidate` 和 `remove:true`，不得将这些包装粘入普通模块行。两种输入、候选与数值合同的权威示例见 [T18](../active/t18_niagara_domain.md)。

System 的普通 workspace 另有 `emitters` 与 `user_parameters` 完整集合，沿当前导出 ref 定位其索引和条目文件。新增 Emitter 用可读实例 key 与 `source_asset_path`，可同时设置 `enabled`；已有条目只改 `enabled` 不会重新同步来源。显式 `source_sync_mode:merge` 或 `replace` 作用于该实例的原来源及版本，不能把不同来源当作原实例的更新。从完整索引删除条目表示删除该 System handle；不会删除来源 Emitter 资产。导入后刷新会生成新实例的 stacks、renderer 等真实文件，再按这些 ref 编辑；不预造不存在的子文件。

普通 User Parameter 新行用 `User.*` key、`type` 和结构化 `value`，例如 `User.Intensity:{type:"float",value:{literal:2.5}}`；已有值保持导出的类型结构。删除完整索引中的条目表示移除参数。`kind`、`value_text` 是派生摘要，不是普通编辑字段；显式 provider-extension 对历史字段的接受规则仍见 §7.3。普通文件不写 `provider_extension`、`operation` 或 `remove` 包装。

已有 EventHandler 和独立 DynamicInput 在各自导出单元中编辑，并由同一个 Niagara provider 合入所属 Emitter/stage/module。保持导出的名称、输入绑定和 event identity；其可编辑字段以实际导出为准。这不承诺任意 event 创建或任意 Script topology。预检诊断沿原 HJSON 来源返回；参数、Emitter、stage/module 各自集合内的独立错误集中报告，存在错误时不应用其余合法补丁。依赖于失败输入的后续阶段不据此声称已检查。

### 7.1 Standalone Module Script

standalone Module Script 使用同一 `provider_extension.stages` 结构。`script_usage` 只决定 UE 创建的 Script 类型；真正可编辑的 Parameter Map 链仍通过 stage/module/dynamic-input 公共结构表达：

```hjson
operation: create
target: { asset_path: "/Game/VFX/NM_Example", asset_kind: "script" }
provider_extension: {
  script_usage: Module
  compile: true
  stages: {
    module: {
      target: { usage: "Module" }
      modules: {
        SetDefaults: {
          module_kind: assignment
          module_index: 0
          assignment_targets: { "Particles.ExampleValue": "float" }
          inputs: { "Module.Particles.ExampleValue": { kind: "float", value: 1 } }
        }
      }
    }
  }
}
```

Export 会读回 stack-shaped Module Script 图。UE 图中的任意低层节点、自由连线和布局不属于此纵切，不得把缺失的低层节点误判成完整任意图往返能力。

### 7.2 Assignment Module

Assignment Module 复用普通 `provider_extension.stages.<stage>.modules` 容器，通过 `module_kind: assignment` 区分，不新增独立命令：

```hjson
"<module-guid-or-create-key>": {
  module_kind: assignment
  module_index: 1
  assignment_targets: {
    "Particles.Position": null
    "Particles.RibbonWidth": null
    "Particles.DynamicMaterialParameter": null
    "Particles.ProjectSpecificValue": float
  }
  inputs: {
    "Module.Particles.Position": { value: { source: "linked_input", linked_parameter: "Particles.Position" } }
    "Module.Particles.RibbonWidth": { kind: "float", value: 12 }
    "Module.Particles.DynamicMaterialParameter": { kind: "vector4", value: [1, 1, 1, 1] }
  }
}
```

- `assignment_targets` 存在时是该 Assignment 的完整目标集合；增删或改类型会通过原生 Niagara Stack API 重建模块，并保留稳定 module GUID 和目标顺序位置。
- Standalone Module Script 的已有 Assignment 只替换目标节点；其他 Assignment 保留。改同名参数类型交给 UE Graph 参数 owner，同步该参数的原生引用和 metadata，不要求用户重交整张 stage。
- `modules` 的键优先接受 Export 返回的合法 GUID；手写稳定语义键时，Apply 会按资产与 stage 上下文生成确定性 module GUID。相同 workspace 重复 Apply 必须命中同一模块，不得追加重复节点；Export 始终读回实际 GUID。
- 对 `FNiagaraConstants` 中的已知原生属性，值可写 `null`，Apply 自动使用 UE 声明的真实类型；显式类型与原生类型冲突时以 `niagara_assignment_target_type_conflict:*` 失败。
- 对未知项目变量必须显式提供类型；缺失时以 `niagara_assignment_target_type_missing:*` 失败。支持公共 Niagara 基础类型 token，也接受可加载的 struct、enum 或 class object path。
- 类型也可写为 `{ type: "float" }`。推断、显式 token 和对象式声明共用原生类型解析；读回规范类型 token，不将合法补全误报为用户意图丢失。普通 workspace 的直接 Assignment 创建见 [T18](../active/t18_niagara_domain.md#candidate-to-authoring-chain)。
- Assignment 不接受 `script_asset_path`；普通 Script Module 不接受把 Assignment 当作无脚本外部模块静默保留。冲突在 preflight 阶段失败，不进入 mutation。
- Export 对 Assignment 输出结构化 `module_kind`、`assignment_targets` 和必要 input overrides，不暴露 Niagara 为 Assignment 生成的内部瞬态脚本路径。
- Apply 在 mutation 前递归比较现有 literal、`linked_input`、Data Interface 和 dynamic-input 子树；语义相同的 Export->Apply 不重写 pin、不标脏图，也不触发无意义的结构编译。比较只使用本批更新后仍适用的节点接口：模块脚本、Assignment 目标集合或 dynamic-input 脚本将被替换时，旧节点不用于预解析新输入名称、类型或值；输入树在原生创建后按新节点接口解析和校验。未替换节点保留原有等值比较。

当前 Export 可编辑投影覆盖 stage、外部 script module、Assignment Module、dynamic-input script、System User Parameter，以及公共 Details capture 支持的 Emitter Owner 和 renderer authoring 属性，包括当前仍为默认值的字段。模型从导出的 `property_path`、类型和当前值直接编辑，不需要猜测默认属性名称。Emitter Owner 的 `target` 与 `properties` 是同一 snapshot 的两个独立投影槽：身份只进入 `target`，属性组只进入同级 `properties`。Renderer 与 Emitter Owner 共用结构化 property-group builder，array/set/map 会携带 `element_type`、`key_type`、`value_type` 等容器类型信息。公共 workspace baseline 识别实际变化；未修改的导出字段不是新的显式写入，省略字段仍保留原状态。原生默认 provider 继续负责默认恢复，不以默认值相等过滤可发现字段。Renderer property group 还总是包含按 `property_path` 排序的 `{property_path:"bIsEnabled",kind:"bool",value:{literal:true}}` descriptor；该字段由 Niagara renderer API 拥有，Apply 通过 `SetIsEnabled` 写入，而不是把它误当作普通反射 FProperty。重复 descriptor 或非 bool literal 在 preflight 拒绝。Event Handler 仅覆盖当前受控结构，Data Interface 详细属性和任意 Script graph topology 不在本纵切的完整 Export 范围内；不得从旧多文件文档推断这些内容仍由 folder 命令覆盖。Assignment 是 UE 原生参数写入节点的受控 authoring 形态，不代表任意 Script graph topology 已开放。Apply 可以接受契约允许但 Export profile 未展示的输入，两者字段集合互不推导。

### 7.3 System User Parameter completion

- `provider_extension.user_parameters` 是以规范参数名为 key 的对象映射；Export 使用 `User.*` 全名，Apply 同时接受短名并规范化为 `User.*`。
- 参数对象支持 `type`、结构化 `value`、可选断言 `kind` 和 `remove`。`null` 与 `remove: true` 等价，删除不存在的参数是幂等 no-op。
- `llm_authoring` Export 不输出可由结构化 `value` 是否存在推导的 `has_value`；`readback` / `debug` 可保留 `value_text`。Apply 接受历史 Export 中的布尔 `has_value` 和 readback `value_text` 作为只读元数据，结构化 `value` 始终是唯一写入真源；`has_value=false` 与同时提供 `value` 会在 mutation 前失败。
- 新参数必须提供 `type`；缺少 `value` 时由 UE 参数存储创建原生默认值。更新已有参数可省略 `type`，缺少 `value` 时保留当前值；显式类型与现有类型冲突会在 mutation 前失败。
- `value` 使用统一类型结构：标量 `{ literal }`，Vector/Position/Quat/Color `{ components: [] }`，Enum `{ selected }`，UObject/Data Interface `{ path }`，其它 struct 使用递归 `{ fields }`。
- User Parameter 具有独立 semantic identity、统一 snapshot readback 和事务回滚；Apply 接受范围不会改变 Export 的字段筛选规则，`value_text` 只在 readback/debug profile 保留。

### 7.4 System emitter handle completion

```hjson
provider_extension: {
  emitters: {
    MainArc: {
      source_asset_path: "/Game/VFX/Emitters/NE_Arc"
      source_sync_mode: replace
      emitter_version: "<optional-guid>"
      target: { emitter_name: "MainArc" }
    }
  }
}
```

This is not a separate seed command. It is the Niagara specialization of shared recursive completion: use caller evidence to resolve an existing UObject first, ask UE to create missing native content second, hide generated identity from the caller, then continue ordinary Apply. Replaying the same entry is also a synchronization request. Omitted `source_sync_mode` or `merge` keeps UE local overrides; `replace` uses the current source as the complete baseline and then applies explicit request fields. A different parent, incompatible version, unknown mode, or a mode without `source_asset_path` is rejected in preflight. Export remains profile-driven and does not add either Apply-only field solely because Apply accepted it.

### 7.5 Renderer attribute binding

`FNiagaraVariableAttributeBinding` 使用原生变量绑定编辑语义。输入字段仍由公共递归 codec 校验；它的 `ParamMapVariable`、`DataSetName`、缓存名称和可访问性标记由 UE 根据绑定根变量、Emitter 版本和 renderer source mode 推导，不属于独立编辑字段。

Renderer binding 的 authoring 输入只需要结构化提供 `RootVariable.Name`：

```hjson
{
  property_path: PositionBinding
  kind: struct
  value: { fields: { RootVariable: { fields: { Name: { literal: "Module.Position" } } } } }
}
```

Apply 会识别所有类型为 `FNiagaraVariableAttributeBinding` 的 renderer 属性，并调用 UE 的 `FNiagaraVariableAttributeBinding::SetValue` 自动补全命名空间、数据集名称、类型和缓存字段。该规则同样适用于 `PositionBinding`、`SpriteSizeBinding`、`SpriteAlignmentBinding`、`ColorBinding` 及其他 renderer attribute binding；不得通过手写派生字段或逐字段绕过来模拟绑定。其他 renderer 属性仍走共享 UObject property Apply。

普通导出包含该绑定名。跟随 renderer source mode 的隐式绑定保留无前缀名称；显式跨命名空间绑定保留原生显示命名空间。输入可以清空绑定名。若同批修改 source mode，先完成 renderer 属性更新，再调用原生绑定 setter。后续编译和读回核对由原始输入经原生归一化得到的绑定意图，不把重新捕获的缓存当作原始预期。
