#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class FJsonObject;

namespace UeAgentLevelAuthoring
{
    // Resolves the same native Add Component choice for discovery and Level import.
    bool ResolveComponentCreationCandidate(const TSharedPtr<FJsonObject>& Selector,
        FString& OutClassPath, FString& OutAssetOverride, UE::UAI::Contract::FDiagnostic& OutError);
    bool QueryComponentCandidates(const TSharedPtr<FJsonObject>& Params,
        FString& OutPayload, UE::UAI::Contract::FDiagnostic& OutError);
}
