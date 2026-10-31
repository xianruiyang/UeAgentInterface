#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class UWorld;
class FJsonObject;
namespace UE::UAI::Composition::HjsonImport { class FPreparedAuthoringImport; }

namespace UE::UAI::SceneAnalysis
{
    bool IsAnalysisCommand(const FString& Command);
    void Shutdown();
    // Capture/project on the game thread; RunPending only touches owned plain data.
    bool Query(UWorld& World, const FString& Command, const FString& RequestId,
        const TSharedPtr<FJsonObject>& Input, TSharedPtr<FJsonObject>& Output,
        Contract::FDiagnostic& Error);
    bool HasPending(const FString& RequestId);
    void RunPending(const FString& RequestId, const TFunction<bool()>& IsCancelled);
    void DeferPending(const FString& RequestId, TFunction<bool()> IsCancelled, TFunction<void()> Complete);
    bool ValidateForApply(UWorld& World, const FString& WorkspaceRoot,
        const FString& AnalysisRef, const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
        const TSharedRef<FJsonObject>& PreparedBundle, Contract::FDiagnostic& Error);
    // Read-only post-apply comparison. A difference is reported, never rolled back or
    // silently presented as having been covered by the pre-apply geometry result.
    TSharedPtr<FJsonObject> ReadApplied(UWorld& World, const FString& AnalysisRef);
}
