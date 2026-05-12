# 指令详解：Blueprint

> 废弃写入命令已迁移到 `deprecatedCommand/02_Blueprint.md`；本分册只保留主流程、读取、导出/应用、编译、诊断，以及尚未被 JSON / 结构化 JSON 覆盖的命令。

## 资产与编译

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `blueprint_create` | 创建 Blueprint 资产 | `asset_path`、`parent_class`、`compile_after_create`、`open_editor`、`save_after_create` |
| `blueprint_compile` | 编译 Blueprint | `asset_path`、`include_messages`、`severity_filter`、`max_messages`、`save_after_compile` |
| `blueprint_get_compile_log` | 触发编译并读取编译日志 | `asset_path`、`severity_filter`、`max_messages`、`save_after_compile` |
| `blueprint_get_info` | 读取 Blueprint 基础信息 | `asset_path` |
| `blueprint_list_graphs` | 列出 UberGraph / 函数图 / 宏图 / 委托图 | `asset_path` |
| `blueprint_export_folder` | 导出 Actor Blueprint 到固定文件夹式 JSON 结构 | `asset_path`、可选 `clean_output_dir`、`include_validation` |
| `blueprint_apply_folder` | 从固定文件夹式 JSON 结构回写 Actor Blueprint | `asset_path`、可选 `create_if_missing`、`compile_after_apply`、`save_after_apply` |

- `blueprint_compile` 只有在 `include_messages=true` 时才会回传过滤后的编译消息数组。
- `blueprint_list_graphs` 返回 `graphs[]` 和 `graph_count`，适合做“新增函数图/宏图/委托图后”的结构校验。
- `blueprint_export_folder` / `blueprint_apply_folder` 当前只覆盖 `Actor Blueprint`。
- 固定导出根目录为：`Saved/UeAssetFolders/ActorBlueprint`
- 单个资产的文件夹路径按 `asset_path` 自动展开，例如：
  - `/Game/Blueprints/BP_Door`
  - `Saved/UeAssetFolders/ActorBlueprint/Game/Blueprints/BP_Door`
- `blueprint_apply_folder` 当前对支持的图采用“保留内建入口节点，重建其余节点”的方式应用。
- `function_graph / macro_graph` 的边定义可引用保留节点 ID：
  - `__entry__`
  - `__result__`

## 组件（SCS）

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `blueprint_inspect_components` | 查看 SCS 组件树 | `asset_path` |

## 图与节点

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `blueprint_inspect_nodes` | 列出图节点与引脚 | `asset_path`、`graph_name`、`limit_per_graph`、`include_pins` |
| `blueprint_layout_graph_self_test` | 运行 Blueprint 自动排版核心子算法自测 | 无 |
| `blueprint_layout_graph` | 自动排版指定 Blueprint 图节点 | `asset_path`、`graph_name`、`horizontal_spacing`、`vertical_spacing`、`node_padding`、`output_access_offset_x`、`input_access_offset_x`、`pin_direction_bias_x`、`use_real_geometry`、`require_real_geometry`、`allow_geometry_view_capture`、`allow_focused_geometry_capture`、`allow_remove_all_reroute_nodes`、`layout_report_file`、`dry_run`、`insert_reroute_nodes`、`replace_existing_reroute_nodes`、`compile_after_layout`、`save_after_layout` |

- 当前节点创建能力集中在：标准事件、自定义事件、函数调用、变量节点，以及按类创建的通用非结构节点。
- `graph_name` 现在既可以传图名，也可以直接传 `graph_path`；对子图、状态机子图、Transition Rule 图优先建议传 `graph_path`，避免重名歧义。
- 普通图节点、标准事件、自定义事件、变量节点和组件绑定事件都应通过 `graphs/*.json` 的结构化节点描述表达；旧原子图节点入口仅保留在废弃分册。

### `blueprint_layout_graph_self_test`

用途：运行 `blueprint_layout_graph` 当前主算法的核心子算法自测。该命令不读取或写入资产，只在内存中构造合成图并直接调用排版内部算法，用于防止“返回字段声称完整，但关键子步骤没有真实生效”的回归。

当前覆盖：

- `ConnectionAlignmentShortenX(OutputPoint, InputPoint, OutputOffset, InputOffset)` 使用真实 pin / 真实可见中途点坐标计算输入输出完全对齐时应缩短的 X 距离；可接受的前向接入必须满足返回值 `>= 0`，完全对齐时为 `0`。这条规则同时适用于 `source pin -> first knot`、`prev knot -> next knot`、`last knot -> target pin` 每一段；两个相邻中途点不能直接放在同一 `X` 上伪造竖线。中途点计划使用可见控制点坐标；写回 `UK2Node_Knot.NodePosX/Y` 时统一用 UE `SGraphNodeKnot` 的 `42 x 24` spacer 转成节点左上角，不再使用 projected access anchor 或旧偏置点反投影。
- SCC 回边检测与 `RankIgnoredFeedbackEdge` 标记；回边只从 rank / 列排序 / 输入链抽离中移除，仍保留弱连通成岛，不再把 island 切开。
- 排除 feedback 后的真正 fanout 出边转 `IslandCutFanoutEdge`，只有这类边会把显式图拆成多个 node island 并进入 island graph。
- 岛内显式边列分配与双列锚定 Y 放置，保证 rank-ignored 边不再回流到岛内 rank；普通核心列先按既定顺序密排，取紧凑段中点对应的元素作为 `BaseItem`，把该元素放到自己的 `PreferredTop`，再保持紧凑段形状向上/向下贴紧放置。输入链组保留更严格的 leading pin 精确对齐分支。
- 岛内列放置采用严格单次传播：基准列密排后，向右传播时当前列只看左侧最近相邻已知列，向左传播时当前列只看右侧最近相邻已知列。命令不再执行 `all-column` / `known-neighbor` 二次槽内修正，也不会把已放置非相邻列作为低权重锚点反向影响当前列。
- 同列节点连接到同一个相邻节点时，按该相邻节点的真实输入/输出 pin Y 顺序打破同邻居排序平局，覆盖共同下游输入口和共同上游输出口两个方向。
- 同一目标节点不同端口形成的上下顺序会作为 `DownstreamPortOrderDemand` 沿显式边向上游传播，用作多列排序的次级约束；只有 demand 属于同一个目标 group 时才参与比较。
- 同一源节点不同输出端口形成的上下顺序会作为 `SourcePortOrderDemand` 沿显式边向下游传播；当两个节点属于同一个源分支 group 时，source demand 优先于相邻列 barycenter，用于保持 Branch True/False 或其它上下输出端口跨多列不反转。
- island 输入候选、内部总线、边界入口、端口候选、局部端口匹配、确定式 island X/Y 求解和 corridor/lane 诊断契约。
- top/bottom 连续输入候选区间会按源端真实 X 和连接对齐缩短量选择，而不是固定吸附到区间末端；入口候选必须同时校验源输出到入口、入口到真实目标输入这两段的 `ConnectionAlignmentShortenX >= 0`。
- corridor 诊断在 lane coloring 完成后按最终中途点重算，`route_through_island_count` 必须反映最终路线；跨 bus branch lane 自测会验证不同 `SourceParamId` 的竖向 branch Y 投影冲突必须在生成期候选选择中转成不同 branch X，同一 `SourceParamId` 不参与 cross-bus lane，且输出点越靠近总线束越保留右侧接入；不同 `SourceParamId` 即使接到同一目标节点的不同输入 pin，也必须作为不同 bus 在同一候选冲突组内规划 trunk Y 与 branch X lane，不能共用同一分支 lane；横向 trunk lane 自测会验证不同 bus 的 X 投影冲突在 reroute 写回前已经由生成期候选选择分配不同 Y lane；同源 fanout 自测会验证 segment gate 改写单条分支后仍按 `AttachmentMergeGroup` 归一化源侧共享 trunk Y 和共享 trunk 前缀点，不能把同一个输出接口拆成两条平行总线；hard occupancy 自测会验证 bus 候选生成阶段优先选择不穿 sibling hard rect 的完整 polyline，不能靠后处理修补。

返回字段：

- `layout_self_test_version`：自测版本。
- `case_count` / `passed_count` / `failed_count` / `all_passed`：自测汇总。
- `cases[]`：每个用例的 `name`、`status` 和关键诊断细节。任何用例失败时命令返回失败，调用方必须停止依赖当前排版结果。

### `blueprint_layout_graph`

用途：把指定 Blueprint 图转换为新的干净自动排版结果。当前命令入口只做三件事：从 UE 图中折叠已有 `K2Node_Knot` 并收集语义节点/边，调用独立的 `Commands/Blueprint/Layout/UeAgentBlueprintAutoLayoutAlgorithm.*` 算法模块，最后把节点坐标和可选的新 `K2Node_Knot` 写回图。旧 `GraphLayout.inl` 中的 exec skeleton、data docking、consumer slot packing、final single consumer alignment 等级联后处理已经不再作为命令主路径存在。

当前算法主流程：

1. 普通排版必须读取真实 Slate 几何：命令会打开或复用目标 GraphEditor 来构造 `SGraphNode` / `SGraphPin` widget，并在固定 `geometry_measurement_zoom=1.0` 的临时采样视图下读取真实节点尺寸和真实 pin anchor，采集后立即恢复图面 view。节点尺寸优先使用 `SGraphNode::GetDesiredSize()` 的 Slate 缓存结果，`GraphEditor->GetBoundsForNode()` 只作为尺寸读回兜底。已连接 pin anchor 不完整时命令直接失败，`dry_run` 也不再回退到估算 pin 行。
2. 收集非 reroute 节点、可见输入/输出 pin、语义边，并把旧 reroute 链折叠成源 pin 到目标 pin 的语义边。
3. 计算 SCC，选择 SCC 内稳定的反馈边标记为 `RankIgnoredFeedbackEdge`。这类边只用于打断 rank / 列分配意义上的环，不参与岛内 rank、列排序、输入链抽离和 crossing minimization，但仍参与弱连通成岛。
4. 在排除 `RankIgnoredFeedbackEdge` 后，按同一 source pin 的剩余出边寻找真正 fanout，并把这些边标记为 `IslandCutFanoutEdge`。只有 `IslandCutFanoutEdge` 会从弱连通成岛中移除并提升为 island graph 边；一个节点如果只是“一个正常输出 + 一个回边”，不会被误判成 fanout。
5. 用只移除 `IslandCutFanoutEdge` 的图做弱连通切岛；岛内 rank、列分配、列排序和双列锚定只看显式边，避免已经切掉的回边重新影响岛内框架。列排序的普通主 key 是相邻列节点顺序，真实 pin Y 顺序只在多个节点共享同一个相邻节点时作为 tie-break，避免同一目标输入口或同一源输出口上下顺序被排反。对于连接到同一个外部目标节点不同端口的分支，命令会先计算 downstream port-order demand set，并沿显式边向上游传播，使该端口顺序能影响多列排序；对于来自同一个外部源节点不同输出端口的分支，命令会计算 source port-order demand set，并沿显式边向下游传播。demand set 按 group 合并并保留多级上下文；排序时只比较两个节点共同拥有且 value 不同的 group，优先选择传播距离更近的共同 group。source demand 属于同一个源 group 时优先于相邻列 barycenter，即使一个节点有相邻列邻居、另一个节点只有跨列连接，也必须先保持源输出端口上下顺序，以避免已经反转的相邻列顺序继续污染后续列；不同源/目标 group 的 demand 不互相比大小。
6. 每个岛内部按最宽列和双列锚定放置节点；列间距会预先纳入内部总线 lane 需求，但按 `min(output_access_offset_x + input_access_offset_x, branch_x_avoidance_distance * (1 + internal_bus_lane_count))` 计算紧凑局部 corridor，不再随总线数线性撑宽列。`branch_x_avoidance_distance` 来自真实相邻 pin 行距/可见 fallback，并带有半个输入输出访问距离的安全下限，避免列被压到直连穿节点但又放不下合法局部总线。附属输入链会先为目标列左侧 corridor 预留足够 X 宽度，再把链节点放在上一核心列和目标列之间；同 corridor 多条输入链只做局部 Y 平衡避让，不再把链节点整体向下推离目标行。
7. 生成 `InternalBusGroup` / `BoundaryEntry` / `BoundaryBus` / `IslandPortCandidate`；单内部总线的 left 入口仍按左侧虚拟列处理，非最左列的 top/bottom 入口会同时生成正上/正下固定优先点和 top/bottom 连续区间候选，固定点优先但允许像多内部组外部总线一样按源端真实 X 与连接对齐缩短量选择更合适的边缘点；多内部总线继续使用 top/bottom 连续候选区间并生成 `BoundaryBus`。候选会同时保留两个点：`target_local` 是岛边界/总线接入候选点，只用于岛位置约束和入口选择；`actual_target_local` 是目标输入 pin 的真实局部坐标。任何 top/bottom 入口不能直接把 `X` 吸附到真实输入 pin，而要先由 `ComputeOutputXAlignedToInput()` 得到目标侧最大输出 X，并由 `ConnectionAlignmentShortenX >= 0` 验证。
8. 岛间连接使用 `StableFrontierIslandRouter`：先建立 placement DAG 和 route-only 边，再按 ready frontier 选择同源 fanout batch，枚举有限 endpoint candidate，按字典序一次提交目标 island 的整体位置、选中的输入候选、route reservation 和 bus contract。已提交 island 只允许整体坐标固定，不允许后续阶段修改岛内节点、输入链、内部总线或岛内 reroute ownership。同一个源 island 的多个输出接口连接到不同目标 island 时，endpoint 会记录 `SourcePortOrderKey=SourceLocal.Y`，Y placement 会构造源接口顺序约束；上方源接口对应的目标 island 必须排在下方源接口对应目标 island 上方。该约束在 constructive placement 顺序和下方目标的 `MinY` 下界中生效，不是 route 后处理；`source_port_order_violation_count` 用于诊断这条约束是否被破坏。
9. 根据已提交的 endpoint candidate 和总线 contract 生成 `direct`、`local_bus`、`external_bus` 的 reroute 计划；每条路线在生成时就使用节点级 hard occupancy、输入链分段碰撞箱、route reservation 和连接对齐缩短量判断是否可行，不能靠后处理再修形状。所有岛间 direct 路线，包括 exec 白线和 data 彩线，只要会穿过非源/目标 hard occupancy，就会升级为局部或外部总线；过长且明显倾斜的 direct 也会升级，避免跨越其它 island 的大斜线；达到超长阈值 `max(HorizontalSpacing * 4.5, 1350)` 的 direct 即使近水平也会升级，避免长水平线横穿其它节点区。岛间 corridor 之外，所有尚未生成 reroute plan 的同岛 direct 连接也会在同一轮生成期做最终准入检查：若 direct 可见 spline 穿 hard occupancy，或直连跨度过长且明显倾斜，或达到上述超长阈值，则生成 `local_bus`，避免同一 island 内部的长执行线继续横穿其它节点。近水平超长线的初始 trunk Y 取 `min(Start.Y, End.Y) - VerticalSpacing`，再由 hard-occupancy 候选选择最终无穿透 lane。非输入链连接若原始端点 AABB 穿过输入链分段碰撞箱，会生成 `input_chain_collision_guard` 局部守卫中途点，守卫点位于被撞输入链分段箱左边，Y 对齐目标真实接口。已有 direct/local/external route plan 不在该步骤二次增强；但所有已生成的 `reroute_plan` 在写回前必须再通过 segment admissibility gate：用真实源 pin、可见中途点、真实目标 pin 组成最终控制点链，若任意相邻段仍穿过非源/目标 hard occupancy，就用同一套 `BuildOrthogonalRoutePolyline` 重新生成完整多点 `local_bus` 候选，只接受逐段 `ConnectionAlignmentShortenX >= 0` 且 segment hard-hit 为 0 的路线。segment gate 若改写同源 fanout 的某条分支，必须在同一生成期内按 `AttachmentMergeGroup` 把组内源侧共享 trunk Y 和共享 trunk 前缀点重新归一化；归一化只接受所有 sibling route 都 hard-hit 为 0 且缩短量非负的共享 Y / 共享前缀，不允许把同一 `SourceParamId` 写成多条平行岛间主干。最终路线重新检查后，`route_through_island_count` 必须为 0 才算设计完成。同一 `SourceParamId` 的岛间 fanout 若有多条边升级为 bus，必须先合并为一个 bus contract 并共享同一条 trunk lane，再为各目标选择合法 branch X；不能按 edge 拆出多条平行岛间总线。trunk Y lane 和 branch X lane 都不是后处理：每个 `BusContract` 会在生成期枚举 trunk Y、branch X 和以真实目标 pin 结束的完整可见 wire 控制点候选，X 投影冲突或 branch Y 投影冲突的 bus 作为冲突组一次性选择候选组合，优先满足 `hard_hit_count=0`、`BusTrunkLaneSpacing` 和 branch X lane spacing。后续生成的 direct-obstacle / input-chain guard 局部 bus 也必须把当前已确定的不同 `SourceParamId` 总线水平 trunk 和分支段转成 lane reservation，再在候选期优先换 Y/X lane；lane reservation 是规划期软约束，不是 hard occupancy，若完全避让 reservation 会导致无合法路线，则接受 hard-hit 为 0 且逐段缩短量非负的路线并输出 `direct_obstacle_route_soft_lane_conflict_accepted`，不能退回穿节点原曲线。`target_local/entry_abs` 只用于岛边界入口和 island placement 约束，不能作为最终硬占用检测终点；`entry_abs -> actual_target_abs` 这一段也必须作为 UE spline 可见线进入候选评分和穿岛检测。hard hit 计算按相邻控制点之间的 UE spline 采样，而不是只检测控制点直线段。初始单边 local bus / dogleg 候选和后续 BusContract 候选都必须是有限集合：只从当前连接走廊内采集 hard rect 边界，并按与目标 trunk Y、源/目标接口 Y、目标侧 branch 上限和 corridor 中点的距离裁剪到固定上限；不能把 dogleg 组合做成无界四重枚举。源侧第一个中途点必须满足 `FirstKnot.X >= ComputeInputXAlignedToOutput(SourceRealX)`，目标侧最后一个中途点必须满足 `LastKnot.X <= ComputeOutputXAlignedToInput(ActualTargetRealX)`，任意两个相邻中途点也必须按前者输出、后者输入逐段满足 `ConnectionAlignmentShortenX >= 0`；目标侧完全对齐时返回值为 `0`，不再用经验偏移、直接 X 吸附或同 X 中途点制造 S 形回绕。branch point 优先使用满足非负缩短量、完整可见 polyline hard-hit 为 0 的最右合法 X，通常是 `ComputeOutputXAlignedToInput(TargetX)`；只有该点不可行时才在候选集合内向左搜索目标侧接入点、branch lane 或 hard-rect 避让点；从 trunk 接回目标接口需要改变 Y 时必须使用目标侧短 dogleg，不能从远离目标的分支点直接拉大斜线。跨 bus 竖向 branch 若 Y 投影重叠且当前/理想 branch X 过近，会在候选选择中分配不同 branch X lane：同一 `SourceParamId` 不参与 cross-bus lane；输出点到总线束的 Y 距离越小，branch point 越靠右；其它冲突 branch 只能选择候选集合中更靠左且保持非负 shorten 的 branch X。合法 X 区间重叠本身不构成 X lane 冲突；只有已选/理想 branch X 过近才进入同一 X 避让组，避免远端分支被近端分支牵连。所有 corridor route 的起点和终点都是真实图坐标；`reroute_plan[].points` 与 `reroute_plan[].knot_points` 都表示可见中途点控制点坐标，命令写回时再转换为 `K2Node_Knot` 左上角。
10. 输出 metrics、设计覆盖、真实几何统计、岛诊断、总线诊断、端口匹配诊断、corridor/lane 诊断和 reroute 计划，用于判断新算法是否真的走通。

关键参数：

- `asset_path`：Blueprint 资产路径。
- `graph_name`：图名或 `graph_path`；默认 `EventGraph`。
- `horizontal_spacing`：节点列或岛列之间的基础 X 间距，默认 `300`。
- `vertical_spacing`：岛列之间的基础 Y 间距，默认 `140`。
- `node_padding`：节点避让和重叠检测额外留白，默认 `24`。
- `output_access_offset_x` / `input_access_offset_x`：`ConnectionAlignmentShortenX` 使用的输出端向右切线距离和输入端向左切线距离。算法不再生成独立偏置点；这些值只作为真实输出点和真实输入点之间的对齐距离参与计算。默认从 UE `UGraphEditorSettings::ComputeSplineTangent()` 相关设置推导；显式传入时覆盖默认值。
- `pin_direction_bias_x`：兼容旧参数。若传入，会同时设置 `output_access_offset_x` 与 `input_access_offset_x`；新调用优先使用上面两个角色化参数。
- `use_real_geometry`：兼容参数。普通排版路径强制视为 `true`，因为算法不再支持估算 pin；`layout_report_file` 恢复路径不重新运行算法，因此不需要采集几何。普通真实几何采集会临时切到固定 `geometry_measurement_zoom=1.0`，避免节点尺寸和 pin anchor 随当前 GraphEditor zoom 漂移；只要 `allow_geometry_view_capture=false`，命令不会准备窗口、调整 overview、截图或强制窗口 redraw。pin anchor 优先通过 Slate `ArrangeChildren` 递归读取 `SGraphPin` arranged geometry，并按 UE `FConnectionDrawingPolicy::DrawSplineWithArrow(StartGeom, EndGeom)` 的实际连接端点计算：输出端取 `SGraphPin` 几何右侧中点并应用 `StartFudgeX=4`，输入端取左侧中点并应用 `ArrowRadius.X - EndFudgeX`。不再使用 pin image 中心作为排版 anchor；即使当前窗口最小化或 GraphPanel 没有可用 tick/cached geometry，也会使用合成 GraphPanel root geometry 完成布局计算。
- `require_real_geometry`：兼容参数。普通排版路径强制视为 `true`；如果节点尺寸或已连接 pin anchor 不完整，命令失败而不是退回估算。
- `allow_geometry_view_capture`：是否允许真实几何采集准备 Blueprint 编辑器窗口、调整图面 view、执行窗口 redraw，默认 `false`。该参数是高风险开关，只用于专门诊断或离线验收；它不是读取 pin arranged geometry 的必要条件。
- `allow_focused_geometry_capture`：是否允许逐节点 focused 几何补采集，默认 `false`，并会隐式启用 `allow_geometry_view_capture=true`。该路径会大量移动 GraphEditor view 和刷新 Slate，是已知可能造成蓝图编辑器卡顿/临时模糊的高风险路径；普通 `use_real_geometry` / `require_real_geometry` 不会进入该路径。
- `layout_report_file`：项目相对或绝对 report JSON 路径。传入后命令不重新运行自动排版算法，而是从该 report 中读取 `nodes[].guid/new_pos_x/new_pos_y` 和 `reroute_plan[]`，按当前图语义边重建节点位置和 `K2Node_Knot`。恢复 report 时 `knot_points` 优先作为可见中途点控制点坐标，旧 report 的 `point_roles=target_input/source_output` 只作兼容输入并会被归一化为 `Auto`，不会触发旧偏置点反投影；写回 `K2Node_Knot` 时仍统一套用 UE spacer 转换。该参数只用于恢复已验证布局或事故回滚；它会绕过真实几何采集，但仍保留 `dry_run`、`insert_reroute_nodes`、`replace_existing_reroute_nodes`、`compile_after_layout`、`save_after_layout` 的语义。
- `origin_x` / `origin_y`：最终图布局起点，默认 `0 / 0`。
- `dry_run=true`：只返回位置、metrics 和 reroute 计划，不写入资产。
- `insert_reroute_nodes=true`：按当前 reroute 计划写入新的 `K2Node_Knot`。
- `replace_existing_reroute_nodes=true`：写入前删除图中已有 `K2Node_Knot`，再按当前计划重建。若 `insert_reroute_nodes=true`，当前实现会强制进入等价的替换模式，避免旧中途点和新中途点叠加成重复连线。
- `allow_remove_all_reroute_nodes=false`：默认保护项。自动排版写回时，如果图中已有 reroute 节点、请求替换旧 reroute，但当前算法计划生成的 reroute knot 数为 0，命令会失败并返回 `reroute_delete_all_guard_blocked`，避免一次算法退化把已验证的输入链/总线中途点全部删除。只有明确要把整图所有 reroute 展平时才传 `true`。`layout_report_file` 恢复不受此保护阻断。
- `compile_after_layout` / `save_after_layout`：排版后是否编译/保存，默认均为 `false`。实际写回且 `compile_after_layout=true` 时，命令会在采集布局几何前先编译一次 Blueprint，避免 dirty / 未编译图的 Slate 尺寸与保存后尺寸不一致。

当前实现边界：

- 普通排版会打开或复用目标 GraphEditor 以构造 Slate widget，并会临时切到固定 `geometry_measurement_zoom=1.0` 做几何采样，结束前必须恢复 document view 或进入采集前的 snapped view。只有显式开启 `allow_geometry_view_capture` 或 `allow_focused_geometry_capture` 的诊断路径才允许准备窗口、全图 overview、focused 单节点补采集或强制 redraw；该高风险路径不得写入半像素 pan 或非预期缩放。
- 真实几何采集即使失败，也必须恢复到 Blueprint document view 或进入采集前的 snapped view；document view 恢复失败时必须 fallback 到保存的 view/zoom，不能把 overview/focused capture 的临时低缩放 live view 留给用户。
- 默认真实几何采集不依赖截图或窗口 pixel readback；它使用 GraphEditor widget 树、`SGraphNode::GetDesiredSize()` 和 `SGraphPin` arranged geometry，并在固定采样 zoom 下转换为 UE 实际连线端点。只有显式开启 `allow_geometry_view_capture` 时才允许窗口准备和全图诊断采集；只有显式开启 `allow_focused_geometry_capture` 时才允许有预算的单节点补采集。超过补采集预算时返回 `focused_geometry_budget_exhausted=true`；已连接 pin 缺 anchor 会直接失败，未连接可见 pin 的缺失只进入 `missing_visible_pin_anchor_count` 统计。
- 实际写回路径会在几何采集前执行一次可选预编译，并返回 `precompiled_before_layout`；这样可把节点真实尺寸、pin anchor、后续写回和幂等 dry-run 锁定在同一份编译状态上。
- 当前自测覆盖设计契约和核心子算法，不代表视觉效果已经达到所有手排细节；视觉质量仍应结合 `metrics_after_reroute_plan`、截图和用户手排目标继续迭代新模块。

返回字段：

- `layout_strategy`：当前为 `stable_frontier_island_router_v1`。
- `selected_candidate`：固定为 `clean_rewrite`，表示没有旧候选评分/回退流程。
- `layout_framework`：当前为 `clean_semantic_island_multiport_layout`。
- `layout_design_complete`：当设计覆盖无 missing/partial、节点 overlap 为 0、普通后向边为 0、非法岛边为 0，且 metrics/diagnostics 契约中的 `route_through_island_count=0` 时为 `true`；`RankIgnoredFeedbackEdge` 不再按普通后向边判失败。
- `geometry_source` / `used_real_geometry`：真实几何来源和是否完整使用真实几何。完整时 `geometry_source=slate_graph_editor_bounds_and_arranged_pin_geometry`。
- `geometry_source=incomplete_slate_real_pin_geometry` 表示真实节点尺寸或已连接 pin anchor 不完整；普通排版会失败，不再继续生成估算 pin 布局。
- `precompiled_before_layout`：实际写回且 `compile_after_layout=true` 时是否已在几何采集前预编译，用于追踪真实尺寸是否来自稳定编译状态。
- `measured_node_count`、`pin_widget_count`、`pin_geometry_anchor_count`、`missing_pin_anchor_count`、`missing_visible_pin_anchor_count`、`focused_geometry_node_count`、`focused_geometry_budget_exhausted`、`geometry_measurement_zoom`：真实几何采集统计。`pin_geometry_anchor_count` 统计通过 arranged `SGraphPin` geometry 取得并转换为 UE 实际连线端点的 anchor，包含合成 GraphPanel root geometry 路径；`geometry_measurement_zoom` 当前固定为 `1.0`，用于确认采样不再依赖用户当前蓝图缩放；`geometry_window_prepared=false` 仍可与 `used_real_geometry=true` 同时成立。`missing_pin_anchor_count` 指已连接 pin 的缺失数，是普通排版的硬失败依据。
- `node_count` / `edge_count` / `moved_node_count` / `applied_move_count`：节点、语义边和移动统计。
- `removed_reroute_node_count` / `created_reroute_node_count` / `reroute_plan_count`：旧中途点移除、新中途点创建和计划数量。
- `reroute_knot_node_spacer_x/y` / `reroute_knot_control_point_to_node_offset_x/y`：写回 `K2Node_Knot` 时使用的 UE reroute spacer 及半宽/半高偏移；用于确认 report 中的可见中途点坐标会按 UE 规则转换为节点左上角。
- `local_bus_edge_count` / `external_bus_edge_count`：按路线类型统计的计划数量。
- `data_island_count` / `island_edge_count` / `rank_count`：切岛和岛级 rank 统计。字段名保留 `data_island_count` 只是兼容旧调用，实际含义是 semantic node island count。
- `scc_feedback_edge_count` / `rank_ignored_edge_count`：SCC 回边破环统计。`rank_ignored_edge_count` 包含 feedback 和 true fanout 中不参与岛内 rank 的边。
- `fanout_implicit_edge_count` / `island_cut_edge_count` / `implicit_edge_count`：fanout 切岛统计。`implicit_edge_count` 是兼容旧调用的字段，当前语义等价于 `island_cut_edge_count`；普通 feedback edge 不计入 island cut。
- `comment_island_count` / `comment_island_node_count` / `comment_island_boundary_edge_count`：注释框特殊岛统计。被 `UEdGraphNode_Comment` 完整包围的一组节点会作为一个冻结 island 参与全图排布，内部不重排；跨出注释框边界的连接计入 boundary edge。
- `input_chain_count` / `input_chain_node_count`：被识别为目标输入接口附属单链的数量和节点数。输入链不参与岛内核心 rank，而是回插到目标列左侧 corridor，并按目标 pin 的 Y 做局部避让。
- `input_chain_forbidden_zone_count` / `input_chain_forbidden_avoidance_count`：输入链目标节点上，非输入链数据输入接口形成的 Y 禁区数量，以及输入链复合元素为了避开这些禁区发生的移动次数。输出 pin 不参与输入链禁区生成；输出连线和执行线冲突交给后续 direct / local bus / external bus 路由处理，不能推开输入链对目标输入 pin 的对齐。若非输入链连接的端点 AABB 穿过输入链分段碰撞箱，会生成 `input_chain_collision_guard` 中途点；守卫点 X 以输入链分段左边界为理想值，但会按 `ConnectionAlignmentShortenX >= 0` 向右推到合法输入对齐位置。
- `input_chain_collision_reroute_count` / `input_chain_collision_reroute_rejected_count`：非输入链连接端点 AABB 穿过输入链分段碰撞箱时生成的本地守卫中途点数量，以及因为 `ConnectionAlignmentShortenX < 0` 被拒绝的数量。
- 列内密排、双列锚定和输入链 slot packing 使用 pixel-safe 高度与整数 top 约束：真实 Slate 高度含小数时会先按上取整后的 slot 参与避让，再写回整数 `NodePosX/Y`。这避免先用小数密排、最后四舍五入导致下一次 dry-run 继续出现 1px 位移或 padding overlap。
- `island_local_frozen_node_count` / `island_local_membership_mutation_count` / `island_local_mutation_count` / `island_local_max_mutation`：岛内排布冻结契约诊断。岛内排布完成后会冻结每个 island 的成员顺序和节点局部坐标；后续岛间端口匹配、X/Y 求解、corridor routing 和 reroute 计划只能移动整岛或读取坐标。若成员或局部坐标被后续阶段修改，mutation count 会大于 0，`layout_design_complete=false`，非 `dry_run` 写回会以 `island_local_layout_mutation_blocked` 失败。
- `layout_design_feature_coverage[]` 中的 `scc_feedback_rank_ignored_edges` 表示回边只被 rank 忽略、不切岛；`comment_box_special_island` 表示注释框包围组已作为冻结特殊岛进入拆岛；`fanout_implicit_edges` 表示真正 fanout 已转为 island-cut；`weak_connected_node_islands` 表示只移除 island-cut 边后做弱连通成岛；`right_anchored_island_rank` 表示岛内核心节点已按右侧输出端从右向左计算 rank，使相连节点的 rank 差尽可能小；`input_chain_attachment` 表示单输入链抽离、附属和回插逻辑已启用。
- `metrics_before` / `metrics_after` / `metrics_after_reroute_plan`：排版前、节点移动后、考虑 reroute 计划后的 metrics。
- `nodes[]`：每个节点的 guid、标题、类名、rank、order、旧/新位置、真实宽高、是否移动、是否注释框节点和 `comment_depth`。
- `reroute_plan[]`：每条计划的边索引、route kind、bus id、lane、合并组和可见中途点控制点坐标。
- `layout_design_feature_coverage[]`：设计文档各子算法的真实覆盖状态，不再用固定常量伪造完成度。
- `island_diagnostics[]` / `input_chains[]` / `island_edge_diagnostics[]` / `column_diagnostics[]` / `internal_bus_groups[]` / `boundary_entries[]` / `boundary_buses[]` / `island_local_mutations[]` / `island_port_candidates[]` / `ready_batches[]` / `selected_endpoint_candidates[]` / `bus_contracts[]` / `cross_bus_lanes[]` / `route_reservations[]` / `corridor_routes[]` / `lane_assignments[]` / `rejection_diagnostics[]`：新算法内部诊断。`input_chains[]` 会列出每条附属输入链的目标节点、目标 pin、链节点数、占用 corridor、禁区避让计数、分段碰撞箱和避让后的放置摘要。`island_local_mutations[]` 仅当冻结后的 island-local 成员或坐标被后续阶段改变时出现，用于定位 stage/island/node。`island_port_candidates[]` 中的 `target_local` 是岛边界虚拟入口，`actual_target_local` 是真实目标输入接口；`selected_endpoint_candidates[]` 记录最终被 Stable Frontier 选中的入口；`bus_contracts[]` 会记录 `branch_y_min/max`、`output_to_bus_bundle_y_distance`、`branch_lane_index` 和 `merged_branch_x`；`cross_bus_lanes[]` 会记录 Y 投影冲突 lane 的理想/最终 branch X、合法左右界、`branch_x_planned_at_generation` 和 `closer_output_prefers_right_branch_x`；`lane_assignments[]` 会记录标准 `lane_spacing`、实际用于选择的 `effective_lane_spacing`、`minimum_visible_lane_spacing`、`compact_trunk_lane_spacing_used`、`hard_hit_relaxed_for_lane_spacing` 和 `lane_conflict_fallback_used`。`minimum_visible_lane_spacing` 来自本冲突组真实相邻 pin Y 行距，取不到时为 `max(NodePadding * 1.5, 36)`；`compact_trunk_lane_spacing_used=true` 时仍必须满足这个可见接口行距，不能退化为几像素或明显小于接口行距的视觉贴合。`lane_conflict_fallback_used` 应为 `false`；如果 hard-free 窄通道不足，生成期会先设置 `hard_hit_relaxed_for_lane_spacing=true` 并继续保持可见 lane 分离，最后才允许 lane conflict fallback。紧凑 trunk lane 只表示该 corridor 硬空窗口过窄，生成期使用可见紧凑 lane 仍严格避让，并不代表后处理补救。后续 direct-obstacle 局部 bus 若必须放弃软 lane reservation 才能保证 hard-hit 为 0，会在 `rejection_diagnostics[]` 输出 `direct_obstacle_route_soft_lane_conflict_accepted`，这不是 hard collision 失败。branch X 搜索区间是源端前方下限到目标端对齐上限，候选包含少量目标侧 lane 候选和 hard rect 左右边界避让点；诊断中的 `branch_x` 若明显早于目标端，表示生成期为了避免穿岛主动提前分支，不是后处理补线。检查上下连接是否应用输入输出对齐时，应看 `actual_target_local`、`corridor_routes[].points` 中接近目标的分支点，以及对应 `ConnectionAlignmentShortenX` 是否接近 0。`reroute_plan[].points` 与 `reroute_plan[].knot_points` 都是可见中途点控制点位置，`point_roles[]` 只保留兼容旧报告，当前默认 `Auto`。
- `corridor_routes[].post_refine_cleared=true` 表示该路线的初始 corridor 诊断曾穿 hard occupancy，但同一生成期的 segment refine 已经把最终 `reroute_plan` 修正为不穿节点；最终完成度以 `metrics_after_reroute_plan` 为准。
- `column_diagnostics[]` 中的 `column_gap_base_demands`、`column_gap_bus_lane_pitches`、`column_gap_bus_demands`、`input_chain_width_demands` 用于拆解岛内列间距来源：`column_gap_base_demands` 表示输入/输出访问偏置总和，`column_gap_bus_lane_pitches` 表示该 corridor 采用的分支点 X 向避让距离，且不会小于半个输入输出访问距离，`column_gap_bus_demands` 表示 `min(输入输出偏置总和, X避让距离 * (1 + 内部总线数量))` 后的紧凑局部总线需求；最终 `column_gaps` 等于该紧凑局部总线需求和输入链 corridor 宽度需求的较大值，不再以全局 `HorizontalSpacing` 作为岛内相邻列的硬下限。

约束：

- `GraphLayout.inl` 只允许保留命令适配、UE 图读写和 JSON 返回；布局决策必须放在 `Layout/UeAgentBlueprintAutoLayoutAlgorithm.*`。
- 新增规则必须优先写成算法模块内的泛用子算法和自测，不能在命令适配层对某个节点名、pin 名或截图个例做特例修补。
- 真实节点尺寸和真实 pin anchor 必须通过命令适配层的几何采集结构传入算法模块；采集过程只能使用固定测量 zoom，并必须用自测或 UAI 读回证明不会把临时采样缩放写成用户可见或持久化视图状态。

## 变量 / 函数 / 宏 / 委托

> 废弃写入命令已迁移到 `deprecatedCommand/02_Blueprint.md` 的对应章节。

- `members/variables.json` 中变量 `type` 的常用高级字段：
  - `pin_category`：支持 `bool/int/int64/byte/enum/float/double/name/string/text/object/class/softobject/softclass/interface/struct`。
  - 常见 UE 结构体可以直接写在 `pin_category`：`vector/vector2d/vector4/rotator/transform/linearcolor/color/quat/intpoint/intvector/int64vector2/intvector4/randomstream/guid/datetime/timespan/framenumber/frametime/framerate/softobjectpath/softclasspath/primaryassettype/primaryassetid/toplevelassetpath/box/box2d/boxspherebounds/twovectors`。
  - 中频引擎/输入/UI 结构体别名：`hitresult/timerhandle/datatablerowhandle/curvetablerowhandle/vectornetquantize/vectornetquantize10/vectornetquantize100/vectornetquantizenormal/key/inputchord/gameplaytag/gameplaytagcontainer/gameplaytagquery/margin/slatecolor/slatebrush/slatefontinfo/widgettransform/anchors/anchordata`。
  - 常用枚举别名可直接作为 `pin_category`，或写在 `pin_subcategory_object`：`collisionchannel/objecttypequery/tracetypequery/inputevent/controllerhand/touchindex/physicalsurface`。枚举最终按 UE 变量习惯落为 `byte + UEnum`。
  - `pin_subcategory`：主要用于 `PC_Real` 的 `float/double` 区分；结构体和枚举也可用这里写别名。
  - `pin_subcategory_object`：对象、类、枚举、结构体等需要附带类型对象时使用；接受完整 `/Script/...` 路径、资产路径，也接受常用别名。
  - 常用对象/类/软引用别名：`object/actor/pawn/character/controller/playercontroller/gamemodebase/gamestatebase/playerstate/hud/playercameramanager/gameinstance/savegame/world/level/worldsubsystem/gameinstancesubsystem/enginesubsystem/actorcomponent/scenecomponent/primitivecomponent/staticmeshcomponent/skeletalmeshcomponent/cameracomponent/scenecapturecomponent2d/springarmcomponent/audiocomponent/timelinecomponent/particlesystemcomponent/widgetcomponent/niagaracomponent/staticmesh/skeletalmesh/materialinterface/material/materialinstance/materialinstanceconstant/texture/texture2d/texturerendertarget2d/soundbase/soundwave/soundcue/particlesystem/animsequence/animmontage/blendspace/animblueprint/animinstance/skeleton/physicsasset/datatable/dataasset/primarydataasset/curvefloat/curvevector/curvelinearcolor/blueprint/font/slatebrushasset/userwidget/widget/levelsequence/niagarasystem/niagaraemitter/inputaction/inputmappingcontext`。
  - `container_type`：支持 `array/set/map`；`map` 必须提供 `value_type`，其格式与外层 `type` 一致但不能再嵌套容器。
  - `default_value`：使用 UE `ImportText` 字符串，例如 `Vector` 用 `(X=1.000000,Y=2.000000,Z=3.000000)`，`Rotator` 用 `(Pitch=0.000000,Yaw=90.000000,Roll=0.000000)`。
  - `instance_editable=true`：把变量改成实例可编辑。
- `WidgetBlueprint` 本质上也是 `Blueprint`，因此 UI 变量和事件图逻辑应通过 UMG / Blueprint folder JSON workflow 表达；旧原子入口仅保留在废弃分册。

## 图视图控制（新增）

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `blueprint_graph_get_view` | 获取指定图的平移/缩放 | `asset_path`、`graph_name`、`open_editor_if_needed` |
| `blueprint_graph_set_view` | 设置指定图的平移/缩放 | `asset_path`、`graph_name`、`view_x`、`view_y`、`zoom`、`snap_to_pixel_grid`、`update_document_view_state`、`save_after_set`、`open_editor_if_needed` |

- `graph_name` 默认 `EventGraph`。
- `blueprint_graph_set_view` 至少需要传入 `view_x/view_y/zoom` 三者之一。
- `snap_to_pixel_grid` 默认 `true`，会按 `round(view * zoom) / zoom` 写回 GraphEditor，保证 pan 乘以当前 zoom 后落在屏幕像素上，避免非整数屏幕偏移造成该图文字和线缆在 Slate 中半像素采样、看起来变粗或模糊。确实需要保留原始 pan 时可显式传 `false`。
- `update_document_view_state` 默认 `true`，会同步 UE 下次打开图时使用的 `LastEditedDocuments` 视图状态；该持久化路径会统一写入像素对齐后的 pan，避免调用方把半像素 view 保存进 Blueprint。`save_after_set` 默认 `false`，需要把该视图状态写入资产包时显式传 `true`。返回 `updated_document_view_state`、`removed_document_view_state_entry_count`、`saved`。
- UE 蓝图图面同时存在两类视图状态：当前已打开 `SGraphEditor` tab 的 live view，以及 Blueprint 资产 `LastEditedDocuments` 中保存的 document view。`blueprint_graph_get_view` 会同时返回 live view 与 `persisted_view_found / persisted_view_x/y / persisted_zoom / live_view_differs_from_persisted`，用于判断当前看到的糊图是否来自已打开 tab 的临时低缩放状态。
- `blueprint_graph_set_view` 在 `update_document_view_state=false` 时只改 live view，不再把本次 live view 伪装成已持久化结果；返回的 `persisted_*` 字段始终来自实际 `LastEditedDocuments` 或本次真实写入的 document view。
- `blueprint_layout_graph` 采集真实几何时会临时切到固定 `geometry_measurement_zoom=1.0`；命令结束优先恢复到 Blueprint 资产中的 document view，而不是恢复进入命令前的 live view，避免已经污染的低缩放 tab 被反复保留。

## 蓝图预览视口控制（新增）

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `blueprint_viewport_get_camera` | 获取蓝图预览视口相机 | `asset_path`、`open_editor_if_needed` |
| `blueprint_viewport_set_camera` | 设置蓝图预览视口相机 | `asset_path`、`location`、`rotation`、`fov`、`open_editor_if_needed` |

- 这两个命令操作的是 Blueprint 编辑器的组件预览视口（SCS Viewport），不是关卡主视口。

## 蓝图窗口截图（新增）

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `blueprint_screenshot` | 对蓝图编辑器窗口/视口/图面板截图 | `asset_path`、`target`、`graph_name`、`format`、`quality`、`max_size`、`restore_graph_view_after_capture`、`open_editor_if_needed`、`allow_ui_mutation`、`allow_window_mutation` |

`target` 支持：
- `viewport`：组件预览视口
- `graph`：指定 `graph_name` 的图面板
- `event_graph`：EventGraph 图面板
- `window`：蓝图编辑器所在窗口

返回字段补充：
- `capture_mode`：截图来源模式。常见值：
  - `viewport_window_crop_offscreen`：用离屏 WidgetRenderer 绘制 Blueprint 窗口后裁切预览视口区域；
  - `viewport_readpixels_fallback`：窗口裁切失败后，退回到视口像素读取；
  - `window_offscreen_fallback`：视口相关截图失败后，退回到离屏窗口截图；
  - `graph_widget_offscreen_noninvasive`：graph/event_graph 默认路径，只离屏绘制 `SGraphEditor`，不恢复、不置前、不强制重绘真实窗口；
  - `graph_widget_offscreen_mutating`：显式允许 UI 变更时的 graph 截图路径；
  - `slate_widget_offscreen`：普通 Slate Widget 离屏截图。
- `allow_ui_mutation`：是否允许截图命令打开/拉前图面、改变 GraphEditor 焦点或触发窗口准备流程。`graph/event_graph` 默认 `false`，`viewport/window` 默认 `true`。当 `allow_ui_mutation=false` 时，如同时传 `open_editor_if_needed=true` 会失败返回 `open_editor_if_needed_requires_allow_ui_mutation`。
- `allow_window_mutation`：是否允许截图命令对真实编辑器窗口执行 `Restore()`、`ShowWindow()`、`BringToFront()` 和 `ForceRedrawWindow()`。`allow_ui_mutation=true` 会隐式允许窗口变更；graph 默认不允许窗口变更。
- `legacy_backbuffer_capture=disabled`：不再调用 `FSlateApplication::TakeScreenshot`，offscreen widget 截图也不再对所有真实 Slate viewport 执行 `InvalidateAllViewports()`。窗口变更被禁用时，目标窗口不可渲染会返回显式错误，而不是自动恢复或拉前真实 UE 窗口。
- `restore_graph_view_after_capture` 默认 `true`。当截图目标是 `graph/event_graph` 且未允许 UI 变更时，截图完成后会把 live `SGraphEditor` 恢复到截图前的 view/zoom，并返回 `graph_view_restore_mode=pre_capture`、`graph_view_changed_after_capture`、前后 pixel residual 字段；显式允许 UI 变更时仍按 document view 恢复，返回 `graph_view_restore_mode=document`。
- `prepared_window_for_capture` / `window_mutated_for_capture`：用于确认本次截图是否实际走过窗口准备和窗口变更路径。排查蓝图截图后变卡、变糊时应优先检查这两个字段。

## 类级操作

> 废弃写入命令已迁移到 `deprecatedCommand/02_Blueprint.md` 的对应章节。

### 属性写入返回

`blueprint_apply_folder` 中的组件、默认值和节点属性回写会暴露写入观测信息。文件夹式回写会把缺少 `property_name` / `value_text`、`ImportText` 失败、写后读回不一致等情况写入 `warning_count / warnings`。

`blueprint_apply_folder` 的可选文件（例如 `members/variables.json`、`members/delegates.json`、`components/tree.json`、`members/defaults.json`）只有不存在时才会跳过；如果文件存在但读取失败或 JSON 语法解析失败，会直接失败返回并带文件路径。数组条目不是 object 或缺 `name/property_name/value_text` 等关键字段时会进入 `warning_count / warnings[]`。

`members/variables.json` 的变量类型推荐写在 `type` 内：

```json
{
  "variables": [
    {
      "name": "MoveOffset",
      "type": { "pin_category": "vector" },
      "default_value": "(X=0.000000,Y=0.000000,Z=100.000000)",
      "instance_editable": true
    },
    {
      "name": "Scores",
      "type": {
        "pin_category": "string",
        "container_type": "map",
        "value_type": { "pin_category": "int" }
      },
      "default_value": "",
      "instance_editable": false
    }
  ]
}
```

导出时会保留 `pin_category/pin_subcategory/pin_subcategory_object/container_type/value_type/default_value`，其中 `default_value` 来自 UE 属性系统读回，不依赖请求字符串。

## 常见流程

1. `blueprint_create`
2. `blueprint_export_folder`
3. 编辑结构化目录中的 `members/*.json`、`components/tree.json`、`graphs/*.json`
4. `blueprint_apply_folder`
5. `blueprint_compile`
6. `save_asset`

## 文件夹式编辑流程（新增）

1. `blueprint_export_folder`
2. 在固定导出目录中编辑：
   - `asset.json`
   - `members/*.json`
   - `components/tree.json`
   - `graphs/*.json`
3. `blueprint_apply_folder`
4. `blueprint_compile` / `blueprint_get_compile_log`

说明：

- 这套模式适合中型以上 Blueprint 的结构化编辑。
- 当前它就是 `Actor Blueprint` 的主 authoring 工作流；废弃分册中的原子写入命令只供旧脚本兼容、bootstrap、迁移、探针验证和局部补修。
- 当前推荐把它当成“上层 authoring 格式 + 底层命令施工”的工作流，而不是整包覆盖重建器。
- 当某类对象属性面很大时，推荐不要第一次就把 `properties[]` 写满。
  更推荐：
  1. 先在 `components/tree.json` 或 `graphs/*.json` 里只写最小骨架
  2. `blueprint_apply_folder`
  3. `blueprint_export_folder`
  4. 以导出的真实 JSON 为模板补全属性
  5. 再次 `blueprint_apply_folder`
- `graphs/*.json` 当前稳定支持的基础 `node_type` 至少包括：
  - `event`
  - `custom_event`
  - `call_function`
  - `variable_node`
  - `component_bound_event`
  - `node_by_class`

`custom_event` 的最小结构示例：

```json
{
  "id": "custom_start",
  "node_type": "custom_event",
  "event_name": "StartRound",
  "pos": { "x": 0, "y": 180 }
}
```

`component` 的推荐最小骨架示例：

```json
{
  "name": "CameraBoom",
  "class": "/Script/Engine.SpringArmComponent",
  "parent": "DefaultSceneRoot",
  "properties": []
}
```

对这类组件，推荐先 apply 让 UE 建好真实模板，再 export 看当前可用属性，再继续补 `properties[]`。

## 2026-04-22 更新

- `blueprint_export_folder / blueprint_apply_folder` 的 `graphs/*.json` 现已支持以下专用 `node_type`：
  - `custom_event`
  - `enhanced_input_action_event`
  - `dynamic_cast`
  - `enhanced_input_get_local_player_subsystem`
  - `enhanced_input_add_mapping_context`
- 对象子属性默认值应优先纳入 folder JSON 导出/回写面；如果某个继承组件或 CDO 子属性无法被结构化表达，应修复 folder workflow，而不是回到原子写入主流程。

## CDO 默认值与继承组件

`components/tree.json` 会导出 SCS 组件以及 Blueprint CDO 上继承/native 组件。继承组件会带 `source="cdo"`、`inherited=true`、`template_name`；回写时这类组件只允许修改模板属性，不会被新增、删除或重建。

`members/defaults.json` 会导出可编辑 CDO 默认值，例如 GameMode 的 `DefaultPawnClass`、角色 Blueprint 的输入/视觉默认引用。回写后仍需要 `blueprint_compile` 和读回确认，不要只看 `ok=true`。

## 废弃命令

本分册不再列出已废弃写入命令；这些命令仅保留在 `deprecatedCommand/02_Blueprint.md`，供旧脚本兼容、bootstrap、迁移和故障补修查阅。
