#include "Domains/Reflection/Authoring/UeAgentMeshMaterialRelations.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"

namespace UeAgentReflectionAuthoring::MeshMaterialRelations
{
namespace
{
bool ChangesOrder(int32 Count, TConstArrayView<int32> Sources)
{
    if (Sources.Num() != Count) return true;
    for (int32 Index = 0; Index < Sources.Num(); ++Index)
        if (Sources[Index] != Index) return true;
    return false;
}

TArray<int32> BuildRemap(int32 Count, TConstArrayView<int32> Sources)
{
    TArray<int32> Result;
    Result.Init(INDEX_NONE, Count);
    for (int32 Index = 0; Index < Sources.Num(); ++Index)
        if (Result.IsValidIndex(Sources[Index])) Result[Sources[Index]] = Index;
    // Native slot deletion shifts following indices and leaves a removed slot's
    // own index for the subsequent native build to resolve. Retained rows use
    // their explicit source identity, including a simultaneous reorder.
    int32 RemovedBefore = 0;
    for (int32 Index = 0; Index < Count; ++Index)
        if (Result[Index] == INDEX_NONE) Result[Index] = Index - RemovedBefore++;
    return Result;
}

int32 RemapIndex(int32 Index, const TArray<int32>& Remap)
{
    return Remap.IsValidIndex(Index) ? Remap[Index] : Index;
}
}

struct FSnapshot::FImpl
{
    FMeshSectionInfoMap StaticSections;
    TArray<TArray<int32>> SkeletalSections;
    TArray<TArray<int32>> LodMaterialMaps;
};

FSnapshot::FSnapshot(UObject& Object) : Impl(MakeUnique<FImpl>())
{
    if (auto* Mesh = Cast<UStaticMesh>(&Object)) Impl->StaticSections = Mesh->GetSectionInfoMap();
    if (auto* Mesh = Cast<USkeletalMesh>(&Object))
        if (const auto* Model = Mesh->GetImportedModel())
            for (int32 Lod = 0; Lod < Model->LODModels.Num(); ++Lod)
            {
                auto& Indices = Impl->SkeletalSections.AddDefaulted_GetRef();
                for (const auto& Section : Model->LODModels[Lod].Sections) Indices.Add(Section.MaterialIndex);
                Impl->LodMaterialMaps.Add(Mesh->GetLODInfo(Lod) ? Mesh->GetLODInfo(Lod)->LODMaterialMap : TArray<int32>());
            }
}
FSnapshot::~FSnapshot() = default;

void FSnapshot::Restore(UObject& Object) const
{
    if (auto* Mesh = Cast<UStaticMesh>(&Object)) Mesh->GetSectionInfoMap() = Impl->StaticSections;
    if (auto* Mesh = Cast<USkeletalMesh>(&Object))
    {
        Mesh->InvalidateDeriveDataCacheGUID();
        if (auto* Model = Mesh->GetImportedModel())
            for (int32 Lod = 0; Lod < Model->LODModels.Num() && Lod < Impl->SkeletalSections.Num(); ++Lod)
            {
                auto& Sections = Model->LODModels[Lod].Sections;
                const auto& Indices = Impl->SkeletalSections[Lod];
                for (int32 Index = 0; Index < Sections.Num() && Index < Indices.Num(); ++Index)
                    Sections[Index].MaterialIndex = Indices[Index];
                if (auto* Info = Mesh->GetLODInfo(Lod)) Info->LODMaterialMap = Impl->LodMaterialMaps[Lod];
            }
    }
}

struct FPropertyEdits::FImpl
{
    TMap<UStaticMesh*, FMeshSectionInfoMap> Before;
};

FPropertyEdits::FPropertyEdits() : Impl(MakeUnique<FImpl>()) {}
FPropertyEdits::~FPropertyEdits() = default;

void FPropertyEdits::Configure(UeAgentAuthoringObjectApply::FOptions& Options)
{
    Options.NeedsNativePropertyTransition = [](const UObject& Owner, const FProperty& Property,
        const UeAgentAuthoringObjectApply::FResolvedNode& Node)
    {
        return Owner.IsA<UStaticMesh>() && Property.GetFName() == TEXT("StaticMaterials")
            && Node.CapturedArrayCount != INDEX_NONE
            && ChangesOrder(Node.CapturedArrayCount, Node.CapturedArrayIndices);
    };
    Options.NativePropertyTransition = [this](UObject& Owner, FProperty& Property,
        const UeAgentAuthoringObjectApply::FResolvedNode& Node, bool bRestoring)
    {
        auto* Mesh = Cast<UStaticMesh>(&Owner);
        if (!Mesh || Property.GetFName() != TEXT("StaticMaterials") || Node.CapturedArrayCount == INDEX_NONE) return;
        if (bRestoring)
        {
            if (const auto* Saved = Impl->Before.Find(Mesh)) Mesh->GetSectionInfoMap() = *Saved;
            return;
        }
        if (!ChangesOrder(Node.CapturedArrayCount, Node.CapturedArrayIndices)) return;
        Impl->Before.FindOrAdd(Mesh, Mesh->GetSectionInfoMap());
        ReindexStaticSlots(*Mesh, Node.CapturedArrayCount, Node.CapturedArrayIndices);
    };
}

void ReindexStaticSlots(UStaticMesh& Mesh, int32 CapturedCount, TConstArrayView<int32> CapturedIndices)
{
    if (!ChangesOrder(CapturedCount, CapturedIndices)) return;
    const auto Remap = BuildRemap(CapturedCount, CapturedIndices);
    // Match the native StaticMeshEditor slot deletion before PostEditChange.
    for (auto& Pair : Mesh.GetSectionInfoMap().Map)
        Pair.Value.MaterialIndex = RemapIndex(Pair.Value.MaterialIndex, Remap);
}

void ReindexSkeletalSlots(USkeletalMesh& Mesh, int32 CapturedCount,
    TConstArrayView<int32> CapturedIndices)
{
    if (!ChangesOrder(CapturedCount, CapturedIndices)) return;
    const auto Remap = BuildRemap(CapturedCount, CapturedIndices);
    Mesh.InvalidateDeriveDataCacheGUID();
    if (auto* Model = Mesh.GetImportedModel())
        for (int32 Lod = 0; Lod < Model->LODModels.Num(); ++Lod)
        {
            auto* Info = Mesh.GetLODInfo(Lod);
            if (!Info) continue;
            auto& Sections = Model->LODModels[Lod].Sections;
            for (int32 Section = 0; Section < Sections.Num(); ++Section)
            {
                const int32 OldBase = Sections[Section].MaterialIndex;
                const int32 OldEffective = Info->LODMaterialMap.IsValidIndex(Section)
                    && Info->LODMaterialMap[Section] != INDEX_NONE ? Info->LODMaterialMap[Section] : OldBase;
                const int32 NewEffective = RemapIndex(OldEffective, Remap);
                if (NewEffective != OldEffective)
                {
                    while (!Info->LODMaterialMap.IsValidIndex(Section)) Info->LODMaterialMap.Add(INDEX_NONE);
                    Info->LODMaterialMap[Section] = NewEffective;
                }
                Sections[Section].MaterialIndex = RemapIndex(OldBase, Remap);
            }
        }
}
}
