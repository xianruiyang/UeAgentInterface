---
command: asset_apply_property_json
category: core_level
title: `asset_apply_property_json`
summary: 从 JSON 回写资产属性
aliases: []
tags: [core_level]
status: active
---

# `asset_apply_property_json`

## LLM Compact

- status: active.
- purpose: 从 JSON 回写资产属性
- key_inputs: `asset_path`, `json_file`, `properties[]`, `save_after_apply`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 这是与 `asset_export_property_json` 成对的主回写入口。
- note: 推荐流程是：导出 JSON -> 修改 grouped Details `fields[]` 的值 -> 回写，而不是长链路反复调用原子属性命令。
- note: 先创建或导入最小可用资产
- note: 再 `export_property_json`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 这是与 `asset_export_property_json` 成对的主回写入口。
- 推荐流程是：导出 JSON -> 修改 grouped Details `fields[]` 的值 -> 回写，而不是长链路反复调用原子属性命令。
- 先创建或导入最小可用资产
- 再 `export_property_json`
- 再在导出结果上补高价值字段
- 再 `apply_property_json`
- 返回包含 `property_results[]`。每个结果会记录 `requested_value_text`、`applied_value_text`、`property_import_status`、`property_import_verified`、`value_text_exact_match`、`value_text_changed_after_import`、`cpp_type`。
- 新 grouped Details 会先恢复为内部兼容结构；旧 inline `properties[]` 仍可作为 legacy 输入。如果属性条目有 JSON 结构问题，例如数组元素不是 object、字段拼写错误、`property_path/value_text` 缺失或类型不对，对应结果会带 `json_issues[]`。
- 曲线结构化 JSON 写入失败时，失败项会返回 `property_import_status=curve_json_apply_failed` 和 `json_issues[]`；未知字段、字段拼写相近、缺 key 值、重复 key 时间、非法插值/外推模式都会显式返回。
- 曲线属性写入会先在临时曲线上完整解析；失败时不会改写目标曲线，也不会把目标资产标脏。多通道曲线任一 channel 失败时，已解析成功的其它 channel 也不会被部分写入。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `json_file`：JSON authoring 文件路径；长 JSON 和可复用 payload 优先走文件。
- `properties[]`：legacy inline 属性列表；新 authoring 优先通过 `json_file` 中的 grouped Details 写入。
- `save_after_apply`：布尔/开关字段；默认值以本命令契约为准。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
