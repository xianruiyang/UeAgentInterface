# Space Semantics

位置、旋转、缩放、布局和其他空间字段以目标资产当前正式 HJSON 导出为准。导出 HJSON
承载可编辑字段、enum 和可读引用；revision等机器身份由SDK与隐藏workspace托管。缺失结构项时使用正向关键词搜索与
`the generated SDK and formal schema`，再按需查询 Actor、Component、节点或其他上下文候选。

只写导出声明的 canonical 值，不把世界轴、局部轴、屏幕轴或 graph/slot layout 混用。分析和
验收时明确 subject、坐标系、单位和验证 frame，但不要把调试信息写入默认 HJSON。导出、apply
和 readback 必须比较同一 subject、同一 frame 和同一状态。

Level 结构化写入使用 `content_graph.import_workspace`。Actor transform、attachment 和
SceneComponent relative transform 由当前 Level export 声明；候选可读 `name/select`、实例 identity
和 placement context 是不同职责，不能复制内部 class/factory 信息。

Level的Actor `transform`使用UE世界空间，Component `attachment.transform`使用父级相对空间；
`translation.components`与`scale.components`按X/Y/Z，`rotation.components`按Pitch/Yaw/Roll，
直接表达原生UE transform值。普通Level HJSON不需要`space_contract`或分析用的转换链占位文本；
这些字段不会改变数值的坐标解释，跨空间转换应在写入前完成。

通用 UObject value codec 与领域 HJSON 可能表达相同语义，但 shape 不一定相同；不得从字段名
推断坐标系或单位。
