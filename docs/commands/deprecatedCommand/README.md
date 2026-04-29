# deprecatedCommand

本目录保存已从主分册抽出的废弃写入命令，按原 `commands/*.md` 文件和章节结构归档。

这些命令仍保留兼容，但资产制作流程应优先使用 JSON / 结构化 JSON：先创建资产，再导出 JSON，写入基础 module 或结构，apply 后再次导出补全属性，继续修改并应用。废弃命令只用于 bootstrap、诊断、迁移、schema 边界和紧急局部修补，不再作为默认 authoring 入口。

## 索引

| 原分册 | 废弃命令归档 |
|---|---|
| `02_Blueprint.md` | [`02_Blueprint.md`](02_Blueprint.md) |
| `03_UMG.md` | [`03_UMG.md`](03_UMG.md) |
| `04_StaticMesh_EnhancedInput.md` | [`04_StaticMesh_EnhancedInput.md`](04_StaticMesh_EnhancedInput.md) |
| `05_Material.md` | [`05_Material.md`](05_Material.md) |
| `06_Sequence.md` | [`06_Sequence.md`](06_Sequence.md) |
| `07_Niagara_System.md` | [`07_Niagara_System.md`](07_Niagara_System.md) |
| `08_Niagara_Emitter.md` | [`08_Niagara_Emitter.md`](08_Niagara_Emitter.md) |
| `09_Niagara_StageGraph.md` | [`09_Niagara_StageGraph.md`](09_Niagara_StageGraph.md) |
| `11_AnimBlueprint.md` | [`11_AnimBlueprint.md`](11_AnimBlueprint.md) |
| `12_Montage.md` | [`12_Montage.md`](12_Montage.md) |
| `13_AnimationAssets_Skeleton.md` | [`13_AnimationAssets_Skeleton.md`](13_AnimationAssets_Skeleton.md) |
| `14_IKRig_IKRetargeter.md` | [`14_IKRig_IKRetargeter.md`](14_IKRig_IKRetargeter.md) |
