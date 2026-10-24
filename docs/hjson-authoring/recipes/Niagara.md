# Niagara HJSON Authoring Recipe

本页只描述当前统一 Niagara workspace。通用 operation、identity、structured value 和 completion 规则先读 `../00_DirectHjsonAuthoring.md` 到 `../08_NiagaraApplyDomain.md`。

## 1. 能力边界

当前 folder Export/Apply 纵切覆盖：

- Niagara System、standalone Emitter、standalone Script 的资产生命周期。
- System stage、embedded Emitter stage、standalone Emitter stage 的稳定 identity。
- Module script、module order、module add/update/remove。
- Module input 上的 Dynamic Input script 设置和清除。
- UE factory/schema/domain-native completion、原子事务、回滚和 snapshot readback。

当前 folder Export 覆盖受控 Renderer、Event Handler、System User Parameter、Module literal/linked input 和递归 Dynamic Input；Data Interface 的任意对象拓扑和任意 Script graph topology 仍不在本纵切。不要从旧多文件 Niagara 文档推断额外字段仍由 folder 命令覆盖；缺失能力应补齐统一领域语义，而不是恢复旧 reader/writer。

## 2. 发现与取证

1. 已知命令用法时直接调用。
2. 新增脚本模块时，用 `niagara.module.candidates` 在目标 System、可读 usage 和可选 emitter 名称上下文中搜索；Assignment 直接填写 `module_kind: assignment` 和 `assignment_targets`，无需候选。Dynamic Input 只使用当前 Niagara 合同明确提供的候选入口或真实 readback。
3. 保留查询返回的原始可读 `name`；遇到歧义时使用服务端返回的短语义 `select`。普通行写顶层 `name/select`，显式 provider-extension 写 `candidate.name/select`，无歧义时省略 `select`。usage/context 写在 stage 外层，不复制完整候选、脚本路径或机器 identity，也不要经 `query.content.search` 转换模块领域 identity。
4. 用 `content_graph.export_workspace` 导出目标 Niagara System、Emitter 或 Script 的当前 HJSON workspace。
5. 需要 live Stack 细节时，再调用 `niagara_emitter_list_*` 或 `niagara_system_list_*`。
6. 编辑 HJSON、Apply、重新 Export，对比稳定 identity 与语义结果。

新脚本模块通过当前领域候选创建；已有模块和 Dynamic Input 的路径 assertion 必须来自当前 readback。不要凭 UI 显示名、记忆或网络示例拼路径。

候选只选择“创建什么”。stage identity、模块实例名、顺序、operation 和 input context 必须来自当前 workspace/readback 或用户意图，不由候选查询补全。服务端在首写前重枚举同一原生 Stack action；当前只有 `asset_module` 可创建，其他 Add Module action 会明确拒绝。Niagara input value source 和 Script graph function 没有可脱离 owner 的 standalone placement。

## 3. Workspace

不要根据资产类型拼接固定磁盘目录。以 `content_graph.export_workspace` 返回的 workspace、当前
ExportProfile `included_files`、教程 `read_order` 和实际 `ref` 导航到 Niagara authoring unit；不得补造
不存在的 manifest、index 或 reference 文件。

当前 workspace 按公共类型规则拆分集合索引、模块和输入；沿 `content_ref` 编辑实际文件。
旧 Niagara 专用 folder 布局不是输入合同，不根据旧目录名称补造文件。

## 4. Module 与 Dynamic Input

普通导出 workspace 的新脚本模块行使用顶层 `name`，例如 `Jitter: { name: "Jitter Position", module_index: 0 }`；输入按当前导出的描述符编辑，公共层生成内部候选。详见 [T18](../../active/t18_niagara_domain.md)。

Assignment 按当前 workspace 的生成示例新增：`AssignLifetime: { module_kind: "assignment", assignment_targets: { "Particles.Lifetime": null } }`。已知原生属性的 `null` 由 UE 类型声明补全；自定义变量显式写类型。此路径不执行交互式 Set parameter 菜单动作。

以下是显式内联领域文档的 provider-extension 示例，不能直接粘入普通 workspace 模块集合。它在 Particle Update stage 中新增原生 module，并为 `Module.Jitter Amount` 设置 Dynamic Input。

~~~hjson
operation: update
target: { asset_path: "/Game/VFX/NE_Example", asset_kind: "emitter" }
provider_extension: {
  stages: {
    "ParticleUpdateScript": {
      target: { usage: "ParticleUpdateScript" }
      modules: {
        "native-jitter-module": {
          candidate: { name: "Jitter Position" }
          module_index: 0
          dynamic_inputs: {
            "Module.Jitter Amount": {
              script_asset_path: "/Niagara/DynamicInputs/Multiply/Multiply_Float.Multiply_Float"
              input_name: "Module.Jitter Amount"
            }
          }
        }
      }
    }
  }
}
~~~

上述显式 provider-extension 示例的规则：

- 已有 module 优先使用导出的可读模块实例名。
- 新 module 使用可读语义键；Apply 创建后由读回确认实际实例。
- `module_index` 是目标顺序；不写则保持/追加。
- Dynamic Input key 与 `input_name` 应一致；Apply 会通过 Niagara schema 解析真实输入。
- 删除 module：provider-extension 对象用 `remove: true`；普通 workspace 则从完整 `modules` 集合索引删除该条目，由公共层生成删除操作。不要把两种写法混用。
- 清除 Dynamic Input：将对应 Dynamic Input 值设为 `null`，或写 `remove: true`。
- 未知脚本、冲突 alias 或歧义 stage 必须在 preflight 失败，不得 warning-and-skip。

## 5. System 与 Embedded Emitter

普通 System workspace 分别导航 system stacks 与 emitter stacks；以当前导出的 ref 为准。显式 provider-extension 文档用 `stages` 汇集两者；embedded emitter stage 的 `target` 带 `emitter_name`，System stage 不带。

已有 System 新增 Emitter：在普通 `emitters` 完整集合中加入可读实例 key，行内写 `source_asset_path` 与可选 `enabled`；导入刷新后再沿新生成的 stacks/renderer refs 编辑。只改 `enabled` 保留实例内已有编辑；同步原来源时才显式写 `source_sync_mode:merge` 或 `replace`。删除索引条目只移除 System handle，来源资产保留。

System 参数在普通 `user_parameters` 集合编辑：新行如 `User.Intensity:{type:"float",value:{literal:2.5}}`，删除索引条目表示移除。`value_text`/`kind` 不承担普通写入。已有 EventHandler 与 DynamicInput 沿实际导出的独立单元编辑，保留其名称和绑定。完整字段及 inline 与普通形状的边界见 [Niagara Apply Domain §7](../08_NiagaraApplyDomain.md#7-export-与-apply-公共入口)。

不要删除或改写导出的 `usage`、`usage_id`、`emitter_name` 来“猜”目标。它们共同构成 stage identity。

## 6. Script 边界

standalone Module Script 的受控 Parameter Map 链沿同一 workspace 编辑，支持 Assignment、受控 module/input 和 recursive DynamicInput。Function/DynamicInput usage 当前可完成资产生命周期，不能由此推断任意 Script graph topology 已开放。详细 usage 与 Assignment 合同见 [Niagara Apply Domain §7.1](../08_NiagaraApplyDomain.md#71-standalone-module-script)；不恢复旧 folder 命令。

## 7. 验证

最小完成证据：

1. Apply 成功且 `phase=complete`。
2. 重新 Export 到新目录。
3. 比较 `target`、stage identity、模块可读实例名、script path、module order 和 Dynamic Input。
4. 需要保存时在 HJSON 的 `provider_extension.save` 明确请求；发生失败时检查 rollback 字段。

不要只检查 HTTP `ok=true` 或 `mutations_applied` 数字；最终结论来自 Export/readback 语义。
