#pragma once

#include "UObject/Object.h"
#include "UeAgentLevelActorDeletionUndo.generated.h"

// A transient identity for the native compound change in Editor undo history.
// It has no asset data and is never saved to an asset package.
UCLASS(Transient)
class UUeAgentLevelActorDeletionUndo : public UObject
{
    GENERATED_BODY()
};
