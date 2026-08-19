# Project Settings / Config

本分册只提供命令导航；当前参数与返回以对应单命令文档为准。

## Project Settings

Project Settings 属性面使用 `uai.uobject_properties.v2`：

```hjson
{
  property_schema: uai.uobject_properties.v2
  properties: [
    { property_path: ProjectName value_kind: string value: { literal: "My Project" } }
  ]
}
```

- `project_settings_export_index`：发现 section id 和 schema/value 摘要。
- `project_settings_export_page`：导出 `ue_agent.project_settings.page.v2` HJSON。
- `project_settings_validate_page`：严格只读 preflight。
- `project_settings_diff_page`：结构化差异。
- 写入先通过 `reference.catalog_search` 解析 `editor_project_target` 和
  `editor_project_settings_field`，再调用
  `editor_project.project_settings.update(changes, expected_revision, target_ref)`；
  持久化与独立读回由统一 authoring runtime 完成。

输入只接受 `properties[]` 或导出 HJSON 文件。属性 schema hash 不一致、未知属性、错误 `value_kind` 或非法 canonical value 都会阻断整次 apply。

## Raw Config

- `config_export_section`：按 ini section 导出 `entries[]/values{}`。这里的 `values{}` 是 config key/value 领域结构，不是 UObject 属性协议。
- 写入先解析 `editor_project_target` 和对应字段，再调用
  `editor_project.config_section.update(changes, expected_revision, target_ref)`；
  不再存在绕过统一 revision、authority、持久化和读回链的裸 apply 入口。

Project Settings UObject authoring 与 raw config 操作是不同边界，不互相兼容或自动转换。
