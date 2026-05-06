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
| `blueprint_layout_graph` | 自动排版指定 Blueprint 图节点 | `asset_path`、`graph_name`、`horizontal_spacing`、`vertical_spacing`、`node_padding`、`max_iterations`、`require_real_geometry`、`dry_run`、`insert_reroute_nodes`、`replace_existing_reroute_nodes`、`restore_editor_view`、`snap_restored_editor_view_to_pixel_grid`、`emit_pin_alignment_probes`、`compile_after_layout`、`save_after_layout` |

- 当前节点创建能力集中在：标准事件、自定义事件、函数调用、变量节点，以及按类创建的通用非结构节点。
- `graph_name` 现在既可以传图名，也可以直接传 `graph_path`；对子图、状态机子图、Transition Rule 图优先建议传 `graph_path`，避免重名歧义。
- 普通图节点、标准事件、自定义事件、变量节点和组件绑定事件都应通过 `graphs/*.json` 的结构化节点描述表达；旧原子图节点入口仅保留在废弃分册。

### `blueprint_layout_graph`

用途：把指定图里的节点移动到可读的分层布局中。默认只修改 `UEdGraphNode.NodePosX / NodePosY`；当显式设置 `insert_reroute_nodes=true` 时，会对仍然直连的长边/回线插入受控 `K2Node_Knot` 中途点以引导线缆，语义连接不变。

关键参数：

- `asset_path`：Blueprint 资产路径。
- `graph_name`：图名或 `graph_path`；默认 `EventGraph`。
- `horizontal_spacing`：列间距，默认 `300`。
- `vertical_spacing`：同列节点间距，默认 `140`。
- `node_padding`：计算重叠时额外留白，默认 `24`。
- `default_node_width` / `default_node_height`：仅在显式设置 `require_real_geometry=false` 且 UE Slate 几何不可用时作为降级估算尺寸，默认 `300 / 120`。
- `require_real_geometry`：默认 `true`。指令会打开/激活 Blueprint 图编辑器，从 `SGraphEditor/SGraphPanel` 读取节点真实 bounds，并对屏幕外节点做临时视图扫视和 redraw；接口锚点优先使用 pin / pin image 的 tick-space 绝对位置减去所属 `SGraphNode` 的 tick-space 绝对位置，再除以 `SGraphPanel` 当前 zoom 得到节点局部坐标，避免把窗口坐标、pan 或缩放误带进接口对齐公式。当 pin tick-space geometry 还不可用时，会对单个 `SGraphNode` 做离屏 widget render，促使 `SGraphPin::Tick()` 填充真实 offset，并用 `PanelCoordToGraphCoord` 路径兜底。Advanced pin 在节点处于 hidden 且未连接时不计入布局接口。连线端点的 Y 使用真实 pin 行坐标，X 固定锚到节点左/右边界，避免把 pin widget 内部 offset 误当接口边缘导致总线从输出 pin 反向后仰；若真实几何不完整会失败，不再静默按估算接口继续排版。
- `origin_x` / `origin_y`：排版起点，默认 `0 / 0`。
- `max_iterations`：交叉优化迭代次数，默认 `16`。
- `dry_run=true`：只返回建议位置、指标和 reroute 计划，不写入资产；当同时传 `replace_existing_reroute_nodes=true` 时，会按折叠后的语义边模拟“移除旧中途点后重建”，便于预览真实重排效果。
- `insert_reroute_nodes=true`：生成受控中途点计划；`dry_run=true` 时只返回计划，`dry_run=false` 时对符合条件且尚未经过 reroute 的长边写入 `K2Node_Knot`。当前自动插点统一为两类总线：局部可解决的连接使用 `local_bus`，仅当局部 trunk / corridor 不足以解决长距离穿越时才把 `external_bus` 作为高成本兜底候选。同一源 pin 的一对多连接只是 bus 拓扑：可共用 trunk，但 route kind 仍按距离和障碍归入 `local_bus` 或 `external_bus`。旧的单边 `*_vertical_lane`、`*_obstacle_side_lane`、`*_hit_*_corridor` 局部引导候选不再生成。同一条 bus 上相距很近的 trunk 中途点会合并到居中点；branch 的 Y 区间重合时会在 X 方向错开；单条 `local_bus` 若直连足够简单或中段过短，会退化成 direct，不写入中途点。重复运行不会对已经经过 reroute 的边重复插入。
- `replace_existing_reroute_nodes=true`：写入时先折叠并移除图内既有 `K2Node_Knot`，把语义源 pin 重新连到最终目标 pin，再按当前算法重新生成必要中途点；用于替换旧的自动排版中途点或彻底重排线缆。默认 `false`，避免意外删除用户手工布线。
- `restore_editor_view`：默认 `true`。排版需要打开 GraphEditor 读取真实几何，但命令结束时会把进入命令时的图面 pan/zoom 还原，避免排版改变用户看到的缩放状态。非 dry-run 且 `save_after_layout=true` 时，还会同步 `UBlueprint::LastEditedDocuments`，防止下次重新打开图时 UE 又按旧 `SavedZoomAmount` 恢复出模糊视图。
- `snap_restored_editor_view_to_pixel_grid`：默认 `true`。恢复 GraphEditor pan 时按 `round(view * zoom) / zoom` 对齐屏幕像素，而不是只把图坐标取整；这样在 `0.875` 等非 1 缩放下也能避免 Slate 半像素采样导致文字和线缆发糊、变粗。
- `emit_pin_alignment_probes`：默认 `false`。只读诊断字段，用于验证指定连接的接口 Y 坐标和对齐公式；可配合 `pin_alignment_probe_limit`、`pin_alignment_probe_source_title`、`pin_alignment_probe_source_pin`、`pin_alignment_probe_target_title`、`pin_alignment_probe_target_pin` 过滤输出。
- `compile_after_layout` / `save_after_layout`：排版后是否编译/保存，默认均为 `false`。

返回字段：

  - `layout_strategy`：当前为 `rule_driven_port_constraint_layered_layout_v2`。指令会先折叠已有 reroute/knot 得到语义边，再用单一的端口约束分层布局引擎排布节点；旧的 `exec_dominant_*`、`dependency_dominant_*`、`existing_*` 候选评分和局部 pass 已不再参与最终布局。
  - `selected_candidate`：固定为 `port_constraint_layered_layout`。为兼容旧调用，返回中仍保留旧候选 metrics/score 字段，但这些字段只是最终结果的别名，不表示仍存在旧候选选择流程。
- `require_real_geometry` / `used_real_geometry` / `geometry_source` / `measured_node_count` / `pin_geometry_anchor_count` / `pin_offset_anchor_count` / `missing_pin_anchor_count`：真实几何读取状态。默认要求 `used_real_geometry=true` 且 `missing_pin_anchor_count=0`；`pin_geometry_anchor_count` 表示从 redraw 后的 tick-space pin / pin image 几何读取接口锚点，`pin_offset_anchor_count` 仅表示使用非零 `GetNodeOffset()` 兜底，并非估算。
- `restore_editor_view` / `editor_view_captured` / `editor_view_restored` / `snap_restored_editor_view_to_pixel_grid` / `persist_restored_editor_view_state` / `persisted_restored_editor_view_state` / `captured_editor_view_x` / `captured_editor_view_y` / `captured_editor_view_zoom`：GraphEditor 视图保护状态。若 `editor_view_restored=true`，说明命令结束时已恢复进入命令时的 pan/zoom；默认还会按当前 zoom 把 pan 吸附到屏幕像素网格，防止 Slate 子像素渲染导致图面发糊。`persisted_restored_editor_view_state=true` 表示同步了 Blueprint 文档恢复状态，避免重开图时回到旧缩放。
- `node_alignment_flow_enabled` / `pin_aligned_compaction_move_count` / `final_single_consumer_pin_alignment_candidate_count` / `final_single_consumer_pin_alignment_move_count` / `direct_leaf_exec_input_docking_candidate_count` / `direct_leaf_exec_input_docking_move_count` / `exec_input_expression_island_candidate_count` / `exec_input_expression_island_move_count` / `shared_exec_input_fanout_docking_candidate_count` / `shared_exec_input_fanout_docking_move_count`：节点接口对齐流程诊断。用于确认“按接口 Y 对齐后再自上而下避让”、“直接叶子数据节点停靠执行节点数据输入”、“执行节点数据输入表达式岛停靠”和“多执行消费者共享数据源按目标输入中位行停靠”确实进入布局，而不是只在文档或探针里存在。当前接口对齐只移动数据节点，执行节点作为控制流骨架保留；执行 lane 会在数据节点停靠和 overlap 处理之后再次恢复，并强制下游执行节点位于源执行输出右侧的最小水平距离之外，避免 UE Bezier 画出右侧回绕线。
- `component_count` / `rank_count`：连通分量数量和最终层级数量。
- `nodes[]`：每个节点的 `node_guid`、`title`、`component`、`rank`、`order`、`input_pin_count`、`output_pin_count`、`has_exec_pin`、`is_reroute_node`、真实 `width/height`、pin anchor 数、旧/新位置和 `moved`。`estimated_width/estimated_height` 为兼容旧调用保留，默认也填入真实尺寸。
- `raw_edge_count` / `edge_count`：`raw_edge_count` 是图内原始连线数量；`edge_count` 是折叠已有 reroute 后用于布局分析的语义边数量。
- `reroute_node_count` / `removed_existing_reroute_node_count` / `reroute_plan_count` / `reroute_plan_edge_source` / `reroute_plan_edge_count` / `reroute_inserted_edge_count` / `reroute_plan[]`：已有 reroute 数、显式替换模式移除的 reroute 数、当前布局下需要新中途点的长边/回线数量、计划索引对应 `raw_edges` 还是折叠后的 `collapsed_edges`、计划使用的边数量、实际插入的语义边数量和每条计划的源/目标 pin、兼容字段 `first_x/first_y/second_x/second_y`、`point_count` 以及 `points[]` 中的完整中途点序列。
- `route_kind_counts` / `local_bus_edge_count` / `external_bus_edge_count` / `shared_fanout_bus_edge_count` / `orphan_local_reroute_count`：按路线语义统计中途点计划。`local_bus` 表示局部区域内的共享 trunk 或局部 corridor；`external_bus` 表示局部路线不够时才使用的外部走廊。外部走廊的 Y 不再按整张图边界拉远，而是按当前连接附近的障碍包围盒生成；不同 external bus 会按同侧 X 投影冲突分配 `bus_lane_index`，X 投影重叠的外绕总线不会共用同一个 Y 车道。`shared_fanout_bus` 是旧兼容字段，新的 fanout 会归入 `local_bus` 或 `external_bus`，因此通常为 `0`；`orphan_local_reroute_count` 也应为 `0`。
- `bus_corridors[]`：按 `bus_id` 汇总 local / external bus，包含 `route_kind`、`reason`、`bus_lane_index`、`bus_lane_side`、trunk 起止位置、`edge_count` 和对应边索引。一对多 bus 的 `edge_count` 通常大于 1，`trunk_end_x` 会覆盖最远分支点；单条需要避让的 local / external bus 也会出现在该数组中，方便审查哪些连接被总线化。
- `backward_edges_after[]`：最终仍存在的回线诊断，包含源/目标标题、pin、是否执行边、源/目标是否带执行 pin、源/目标 X 和 `backward_dx`，用于判断剩余回线是排版可消除问题还是需要 reroute/重构。
- `pin_order_violations_after[]`：最终仍存在的同类线 pin 顺序诊断；共享目标时返回两端源节点 guid/title、目标 guid/title、源 pin、目标 pin、目标 pin index 和 `source_y_delta`，共享源时返回源 guid/title、两端目标 guid/title、源 pin index 和 `target_y_delta`。执行线和数据线不再互相计入该诊断，避免把不同视觉 lane 的线误判为同一类上下约束。
- `pin_alignment_probe_count` / `pin_alignment_probes[]`：当 `emit_pin_alignment_probes=true` 时返回。每条 probe 包含源/目标 pin 是否拿到真实锚点、`source_pin_offset_y` / `target_pin_offset_y`、排版前后 pin Y 残差、若移动源节点或目标节点时的理论 top、公式残差和 `layout_currently_aligned`。用于区分“接口坐标采集错误”“对齐公式错误”和“最终布局没有应用该约束”。
- `overlap_count_before / overlap_count_after`：按估算包围盒计算的节点重叠数；`after` 应为 `0`。
- `edge_crossing_count_before / edge_crossing_count_after`：按 pin 上下顺序估算出入点和折线路径计算的连线交叉数。
- `edge_node_intersection_count_before / edge_node_intersection_count_after`：按 pin 上下顺序估算出入点和折线路径计算的连线穿过非端点节点数。
- `total_wire_manhattan_length_before / total_wire_manhattan_length_after`：近似总线长，用于判断是否过度拉长。
- `total_non_exec_wire_manhattan_length_before / total_non_exec_wire_manhattan_length_after`：近似非执行数据线总线长，用于判断 getter / 常量 / component 引用是否离消费者过远。
- `metrics_*` 中包含 `backward_edge_count`、`backward_wire_manhattan_length`、`pin_order_violation_count`、`pin_order_violation_distance`、`layout_width`、`layout_height`、`layout_area`。回线指源 pin X 在目标 pin X 右侧的边；pin 顺序违规指共享源或共享目标的同类线缆上下顺序与 pin 索引相反，不把执行线和数据线互相比。
- `metrics_after_reroute_plan` / `layout_score_after_reroute_plan`：当生成 reroute 计划时，按计划中的完整中途点序列重算线缆交叉、穿节点和总线长；这是判断真实写入后视觉走线质量的主指标。`metrics_after` 仍保留为“不考虑新中途点、只看节点移动”的兼容指标。
- `metrics_exec_dominant_candidate` / `metrics_exec_dominant_centered_candidate` / `metrics_dependency_dominant_candidate` / `metrics_dependency_dominant_refined_candidate` / `metrics_dependency_dominant_centered_candidate` / `metrics_existing_refined_candidate` / `metrics_existing_semantic_port_constraint_candidate` / `metrics_semantic_port_constraint_candidate`：兼容旧调用保留的字段，当前均为最终 `port_constraint_layered_layout` 结果的别名。
- `score_exec_dominant_candidate` / `score_exec_dominant_centered_candidate` / `score_dependency_dominant_candidate` / `score_dependency_dominant_refined_candidate` / `score_dependency_dominant_centered_candidate` / `score_existing_refined_candidate` / `score_existing_semantic_port_constraint_candidate` / `score_semantic_port_constraint_candidate`：兼容旧调用保留的字段，当前均为最终布局评分的别名。

约束：

- 指令先折叠已有 `K2Node_Knot` reroute，再构建整张图的连通分量；旧位置只用于 `before` 指标和返回字段，不参与排版决策。
- 层级构建优先遵守执行边方向；纯数据节点会根据数据依赖和最近消费者调整层级，避免所有 getter/pure 节点都挤到同一列。
- 节点排序会使用输入/输出 pin 的视觉上下索引；分支节点的不同输出 pin 会参与目标节点上下排序，避免 True/False 等分支天然交叉。
- 指令使用端口约束分层图布局：父节点先按有向语义边放到子节点左侧；相邻层之间执行 median/barycenter 风格 sweep 与 adjacent transpose；排序 key 使用源/目标 pin 的视觉序号，尤其会把同一消费节点的多个输入父节点按目标输入 pin 上下顺序排列，而不是把 `Target/self` 这类 pin 人工提到上方。
- 对无执行 pin 的 getter / 常量 / component 引用 / pure function，指令会把它们视为消费节点输入表达式的一部分。数据节点 rank 会向唯一或最近下游消费节点压近，同时仍保持上游数据父节点在左、下游消费节点在右。
- 最终排布会在 pure consumer 位置稳定后再重贴简单的一对多数据生产者：只有无执行 pin、输入 pin 不复杂、且至少连接两个纯消费者的源节点会被拉回消费者左侧；复杂 pure function、执行节点和单消费者表达式不参与，避免把已经正确落位的计算链整体拉远。
- 节点紧凑排列采用“先确定上下顺序，再对齐接口，再自上而下避让”的收尾 pass：不会重新打乱 crossing minimization 得到的顺序，只按 incident pin 的中位对齐目标微调 Y，并从上到下推开重叠节点。带执行 pin 的节点不会被该数据对齐 pass 下拉或上推，避免 Branch / Set 等控制流节点被数据输入牵离执行主 lane。
- 在全部全局 overlap、执行链细化和数据 docking 之后，还会对单输出、单消费者、无执行 pin 的紧凑值节点和简单 pure 节点做最终接口对齐：若源节点移动到目标输入 pin 的同一 Y 不会和真实节点 bounds 重叠，则直接把源输出 pin 对齐到目标输入 pin，避免最后一轮避让再次打散单独参数节点。
- 直接叶子数据节点喂执行节点数据输入时，会按目标输入 pin 精确停靠：源节点必须无执行 pin、无上游数据输入、只有一个真实数据消费者，并且是 compact value / literal / getter 或无输入的小型叶子 pure 值节点。该规则覆盖 bool、float、int、object/component 引用等 pin 类型，不绑定到 Branch 或 Condition；Branch 的 Condition 只是其中一个实例。带输入的单消费者表达式会作为“执行节点数据输入表达式岛”整体停靠：根表达式节点按目标输入 pin Y 对齐，整组上游紧凑/小型 pure 输入节点随根节点一起移动，并用整图真实 bounds 重叠评分、执行线走廊避让评分在 pin 对齐、上侧和下侧候选间选择；不再因为 pin 名叫 Condition 就强行上抬，也不会为了对齐把表达式放到目标执行输入曲线会穿过的区域。多执行消费者共享同一纯数据源时，会把该源及其单出边上游小岛按目标输入 pin 的中位 Y 停靠，让 fanout local bus 主干回到消费者区域附近；这同样按执行消费者数量、真实 pin Y 和真实 bounds 评分处理，不绑定到具体函数、变量或 pin 名。混合连接到 pure consumer 和执行节点数据输入的中间节点，会降低远端执行输入权重，避免局部计算链被执行节点数据 pin 拖离手排式的紧凑区域。最终 overlap 收尾后会再次执行共享 fanout、表达式岛和直接叶子输入停靠，避免避让 pass 把已经对齐的接口重新打散。
- 同一层节点的上下顺序一旦由端口约束和 crossing minimization 确定，会在后续 Y 放置阶段保持，不再由旧的 datahub lane、sidecar pass、全局评分或旧位置覆盖。
- 单一路径的执行链会在最终 Y 放置后再按执行 pin 拉平成同一条视觉 lane；从 Branch / Sequence 等多输出执行节点分出的子链以父节点对应输出 pin 为锚点分别对齐，避免每经过一个节点就因为输入/输出 pin 垂直偏移而逐步下沉。最终收尾还会重新检查所有执行边，不只修正真正反向的 exec 边，也会修正水平距离过短的 exec 边，使目标执行节点左边界落到源节点右边界之后的最小间距外。
- 写回前会用 UE Slate 真实节点 bounds 消除节点重叠，并用真实 pin anchor 对齐接口；创建 `K2Node_Knot` 后还会刷新 GraphEditor，读取 knot 自身真实 pin offset，再反推 `NodePosX/Y`，使可见中途点圆点落在计划坐标上，而不是把计划坐标误当节点左上角。若启用 `insert_reroute_nodes=true`，线缆阶段会先识别同源多消费者扇出，再按距离和障碍统一归入 `local_bus` 或 `external_bus`。远端子集少于 2 条时不会退回整组，避免把 ABS、变量 getter 等近端消费者也拉到总线里。随后再识别单边 local / external bus；中途点用于局部总线和环绕总线，不再用于掩盖节点上下顺序错误。external bus 在生成时按局部障碍包围盒和 X 投影冲突规划 Y 车道，不依赖写回后的微调。
  - 对候选中途点会做端点距离检查：首个中途点离源 pin 太近、最后一个中途点离目标 pin 太近，或两个中途点之间没有足够中段距离时，候选会被丢弃。生成后同一 bus 上距离很近的 trunk 点会合并到居中点；branch 的 Y 区间重叠时会错开 X；单连接且过于简单的 `local_bus` 会退化为 direct。
- Variable Get、Self、Literal、Enum Literal、Temporary Variable 等紧凑值节点，以及少量无执行 pin 的单行 pure function 节点，默认也使用 GraphEditor 返回的真实 bounds；布局 pin 计数和 pin 序号会跳过 hidden pin，并优先使用真实 pin widget anchor，避免把隐藏 self/world context pin 当成可见接口或把接口 Y 位置估错而导致中途点错位。
- 对仍无法通过节点排布消除的纯数据回线，指令不会再用旧候选回退掩盖；它会在 `backward_edges_after[]` 中暴露诊断，并在启用 `insert_reroute_nodes=true` 时生成右侧外绕中途点计划。
- Blueprint 线缆最终由 UE 图编辑器绘制；默认模式不会改连线拓扑。启用 `insert_reroute_nodes=true` 时只对仍然直连且满足 local / external bus 条件的线插入 `K2Node_Knot`，用于把线缆引导到明确路线；一对多 bus 会为同一源 pin 创建共用 trunk knot 并从 trunk 分支接到各目标。普通短线不会为了美观强行加点。如果已经存在 reroute，指令会先折叠它们参与布局分析，不会移动旧 reroute，也不会重复插入。若需要清掉旧中途点并按当前算法重建，显式传 `replace_existing_reroute_nodes=true`。

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
- `update_document_view_state` 默认 `true`，会同步 UE 下次打开图时使用的 `LastEditedDocuments` 视图状态；`save_after_set` 默认 `false`，需要把该视图状态写入资产包时显式传 `true`。返回 `updated_document_view_state`、`removed_document_view_state_entry_count`、`saved`。

## 蓝图预览视口控制（新增）

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `blueprint_viewport_get_camera` | 获取蓝图预览视口相机 | `asset_path`、`open_editor_if_needed` |
| `blueprint_viewport_set_camera` | 设置蓝图预览视口相机 | `asset_path`、`location`、`rotation`、`fov`、`open_editor_if_needed` |

- 这两个命令操作的是 Blueprint 编辑器的组件预览视口（SCS Viewport），不是关卡主视口。

## 蓝图窗口截图（新增）

| 指令 | 作用 | 关键参数 |
|---|---|---|
| `blueprint_screenshot` | 对蓝图编辑器窗口/视口/图面板截图 | `asset_path`、`target`、`graph_name`、`format`、`quality`、`max_size`、`open_editor_if_needed` |

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
  - `slate_widget_offscreen`：普通 Slate Widget 离屏截图（如 graph/window 目标）。
- `legacy_backbuffer_capture=disabled`：不再调用 `FSlateApplication::TakeScreenshot`，offscreen widget 截图也不再对所有真实 Slate viewport 执行 `InvalidateAllViewports()`。若目标蓝图窗口最小化，命令会先尝试 `Restore()`；恢复失败时返回 `slate_window_not_renderable`，避免 D3D12/Slate swapchain backbuffer 空指针崩溃。

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
