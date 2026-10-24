#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class AActor;
class UWorld;

namespace UeAgentLevelAuthoring
{
// One native deletion, including its editor relationships. The record can be
// restored before persistent readback; package checkpoints own restoration
// after native objects have been released.
class UEAGENTINTERFACE_API FActorDeletion
{
public:
    FActorDeletion();
    ~FActorDeletion();
    bool Apply(UWorld& World, const TArray<AActor*>& Actors, UE::UAI::Contract::FDiagnostic& Error);
    bool Restore(UE::UAI::Contract::FDiagnostic& Error);
    void Release();
private:
    struct FRecord;
    TSharedPtr<FRecord> Record;
};
}
