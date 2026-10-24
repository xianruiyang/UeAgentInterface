#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class FJsonObject;
class UWorld;

namespace UE::UAI::PcgGeneration
{
    struct FObservation
    {
        bool bFinished = false;
        Contract::FDiagnostic Diagnostic;
        TSharedPtr<FJsonObject> Details;
    };

    // Native work only. The editor.session owner supplies scheduling, recovery and release.
    // Start, Observe and RequestCancel run on the game thread.
    class IGenerationWork
    {
    public:
        virtual ~IGenerationWork() = default;
        virtual FObservation Observe() = 0;
        virtual void RequestCancel() = 0;
    };

    bool TryStart(UWorld* EditorWorld, const FString& ComponentPath,
        TSharedPtr<IGenerationWork, ESPMode::ThreadSafe>& OutWork,
        Contract::FDiagnostic& OutError);
}
