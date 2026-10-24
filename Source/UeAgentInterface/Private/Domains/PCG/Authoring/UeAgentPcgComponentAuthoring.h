#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class UActorComponent;
class AActor;
class FJsonObject;

namespace UeAgentPcgComponentAuthoring
{
TSharedPtr<FJsonObject> Read(const UActorComponent& Component);
bool Apply(UActorComponent& Component, const TSharedPtr<const FJsonObject>& Value,
    UE::UAI::Contract::FDiagnostic& Error, const FString& LogicalTarget = FString());
// Finish native owned-PCG refresh before a synchronous Level transaction saves.
bool SettleOwnedInputs(AActor& Actor, UE::UAI::Contract::FDiagnostic& Error);
}
