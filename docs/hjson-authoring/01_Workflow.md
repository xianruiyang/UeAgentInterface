# HJSON Authoring Workflow

本页定义结构化 HJSON authoring 的通用工作流。它适用于 Niagara、Material、Blueprint/UMG、Sequence、ControlRig、PCG、Project Settings、DataAsset/DataTable 和其它已接入 profile/adapter 的域。

## 目标

LLM 只编辑面向 authoring 的 folder HJSON，不直接改 UE 原始导出、readback、debug 或 legacy 证据。默认工作方式是直接打开并修改 `llm_authoring`；只有批量生成、参数化调用、adapter 调试或重复变量过多时才写 Python 辅助。

```text
export/readback folder
-> follow included_files/read_order/ref to the current authoring unit
-> edit llm_authoring files
-> exact domain candidate command for new content when needed
-> import/apply
-> export/readback
-> verify semantic result
```

## Profile 分层

| Profile | 用途 | LLM 是否编辑 |
| --- | --- | --- |
| `llm_authoring` | 默认可写主面；包含 public seed、container context、editable authoring、required readonly。 | 是 |
| `readback` | UE 当前状态、默认值、链接、对象路径、可见性、能力状态。 | 否 |
| `debug` / `diagnostics` | validation、coverage、compile、runtime、warning、diff、unsupported 证据。 | 否 |
| `legacy` | 显式迁移、负例、错误或 debug/readback 识别层。 | 否，除显式迁移或诊断使用 |

`include_raw_properties`、debug/raw/readback 输出是显式诊断选项，不是默认 authoring。

## 标准步骤

1. 用 UAI 导出目标资产，或使用固定 fixture。
2. 按当前 ExportProfile/folder contract 的 `included_files` 和教程 `read_order` 读取必需 identity/entry 导航文件；不假定 manifest/index/references 必然存在。
3. 只打开当前目标需要的 `llm_authoring` 文件；大型 `items/<可读语义名>/...` 按需读取。同一 owner 内同名项使用导出给出的连续编号；唯一名称不加编号。`.uai` 中的 local handle、hash、GUID、revision 及映射属于服务端机器层，模型不读取、不复制、不生成。
4. 只编辑业务 authoring 文件。空集合/新条目形状不明时按 `examples/index.hjson` 定向读取只读示例；不修改说明文件来表达业务变化。
5. 新增节点、模块、组件、控件或输入来源前，先按 `00_DirectHjsonAuthoring.md` 调用对应领域的 exact candidate command 查询真实候选。
6. 候选只保留最小可读 `name`；仅在同名确有歧义时，按当前领域合同附加消除歧义所需的可读 `select` 字段。具体落点以当前 Export 和领域合同为准：普通 keyed row 可以使用 row 顶层字段，candidate-bound 领域可以使用 `candidate` envelope。原生 graph/pin、stage、parent、level 与 placement context 留在 selector 外层。明确知道的更多字段只能作为 assertion；完整搜索行、内部 revision、label、tooltip 和分类不进入 HJSON。
7. 对普通 input 值，只改 `value` 内部字段，不用 `operation` 表达普通改值。
8. 普通 workspace 的集合直接增删成员或编辑值，保留需要保留的成员；内部 `operation` 由服务端派生，不写入普通行。
9. 调用正式 import/apply 入口。
10. apply 成功并刷新目录后，读取刷新结果或对应 Query，按同一主体、同一坐标系、同一状态比较实际结果；没有刷新承诺时才另行 export。
11. 记录 response/report、warning、dirty resource 和剩余边界。

## HJSON 直接编辑优先级

优先顺序：

1. 直接改 `llm_authoring` HJSON。
2. 用领域 candidate command 返回的可读 `name` 和必要的短语义 `select` 构造该领域允许的最小 HJSON 片段，并把创建 context 保留在候选外层。
3. 用 Python 生成或修改 HJSON 文件，但仍让 HJSON 明确表达最终业务意图。
4. 只有 bootstrap、探针、迁移、schema 边界和故障恢复才使用原子命令。

不要让 Python 隐式替代 HJSON authoring 语义；脚本可以批量写 HJSON，但最终需要能从 HJSON 中读懂本次改了什么、为什么可 apply、如何 readback。

## Large Item Rule

- `index.hjson` 只做导航摘要。
- 默认上下文只沿实际 `included_files`、教程 `read_order` 和 `ref` 加载当前目标所需的导航与 authoring 文件；不得补造不存在的 manifest/index/reference。
- Level Actor/Component 等大型 item 文件只在当前任务确实要编辑该 item 时读取。
- P3 验证中 Level 有 30 个 actor/component item 文件超过 16,000 bytes；Guide、示例和命令包不得暗示这些文件应默认全量加载。
- 如果 item 中仍有 raw/readback-only 内容，应优先放到 readback/debug 或后续拆分任务处理。

## Editor 生命周期

- 普通导出、apply、readback、smoke 默认复用当前 UE Editor 会话。
- 不因为一次普通 smoke 结束就关闭用户已经打开的 UE。
- 只有需要重新编译插件/UE C++ 且 DLL 被 Editor 占用时，才先处理 dirty resource，然后安全关闭 UE。
- 如果本轮脚本自己启动了临时 headless/editor 进程，脚本结束时必须清理该进程。
- 每轮结束仍要确认没有额外 `UnrealEditor-Cmd`、`CrashReportClient` 或测试残留进程。

## 证据优先级

结构化 HJSON schema 和字段语义必须来自：

1. 当前插件源码和 adapter/lint 实现。
2. 当前 UAI 导出的 HJSON workspace 和独立 readback。
3. 命令文档。
4. live smoke 或离线单测。

不要用 KB、网络或旧大分册推断本项目私有 HJSON 字段含义。
