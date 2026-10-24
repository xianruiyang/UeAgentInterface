#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;
class UStaticMesh;
namespace UeAgentAuthoringObjectApply { struct FOptions; }

namespace UeAgentReflectionAuthoring::MeshMaterialRelations
{
/** Captures native non-property relationships for a retained native operation's rollback. */
class FSnapshot
{
public:
    explicit FSnapshot(UObject& Mesh);
    ~FSnapshot();
    void Restore(UObject& Mesh) const;
private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};

/** Native mesh section/material relationships, not a material value converter. */
class FPropertyEdits
{
public:
    FPropertyEdits();
    ~FPropertyEdits();
    void Configure(UeAgentAuthoringObjectApply::FOptions& Options);
private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};

void ReindexSkeletalSlots(USkeletalMesh& Mesh, int32 CapturedCount,
    TConstArrayView<int32> CapturedIndices);
void ReindexStaticSlots(UStaticMesh& Mesh, int32 CapturedCount,
    TConstArrayView<int32> CapturedIndices);
}
