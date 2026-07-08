---
command: modeling_create_sphere
category: modeling
title: 使用 bounds 创建 DynamicMesh sphere，并默认生成 actor sphere collision
summary: 使用 bounds 创建 DynamicMesh sphere，并默认生成 actor sphere collision
aliases: []
tags: [modeling]
status: active
---

# `modeling_create_sphere`

## LLM Compact

- status: active.
- purpose: 使用 bounds 创建 DynamicMesh sphere，并默认生成 actor sphere collision
- key_inputs: `bounds`, `collision`, `folder_path`, `label`, `rotation`
- key_returns: `collision`, `created_actor`, `derived_primitive`
- behavior: 写入命令；必须明确目标范围、默认值和写后读回验收。
- note: 高层 bounds 路径默认输出 DynamicMeshActor；`bounds + rotation` 决定尺寸、朝向和 actor 放置。
- note: `collision` 不填时走 primitive 默认碰撞；只有明确需求才覆盖。简单碰撞无法贴合时默认应使用 DynamicMesh complex，而不是整体 bounds box。
- validation: 检查 `created_actor`、`derived_primitive`、`collision.mode/source/count`；可玩结构用 `level_sweep_capsule_path` 或等价 sweep 验证。

## Authoring Notes

- 高层 bounds 路径默认输出 DynamicMeshActor；`bounds + rotation` 决定尺寸、朝向和 actor 放置。
- `collision` 不填时走 primitive 默认碰撞；只有明确需求才覆盖。简单碰撞无法贴合时默认应使用 DynamicMesh complex，而不是整体 bounds box。

## Parameters

- `bounds`：空间范围；通常支持 `center/extent` 或 `min/max`，坐标空间由命令或 `bounds.space` 决定。
- `collision`：碰撞生成/覆盖策略；未传时使用命令默认策略，显式传入才覆盖。
- `folder_path`：Outliner folder 或资产 folder；具体含义以命令所在领域为准。
- `label`：新建或目标 Actor 的 editor label。
- `rotation`：UE Rotator，单位 degree；与 `bounds` 一起决定生成物朝向。

## Returns

- `collision`：碰撞生成摘要；重点看 mode/source/count/coverage。
- `created_actor`：新建 Actor 摘要。
- `derived_primitive`：由 bounds 推导出的 primitive/tool 参数摘要。

## Validation

检查 `created_actor`、`derived_primitive`、`collision.mode/source/count`；可玩结构用 `level_sweep_capsule_path` 或等价 sweep 验证。
