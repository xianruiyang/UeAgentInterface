#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UAnimMontage;
namespace UeAgentAuthoringState { struct FStateEntry; }

namespace UeAgentAnimationAuthoring {
TSharedPtr<FJsonObject> ReadMontageStructure(UAnimMontage& Asset, UE::UAI::Contract::FDiagnostic& Error);
bool ApplyMontageStructure(UAnimMontage& Asset, const TSharedPtr<const FJsonObject>& Document,
                           int32& Changes, UE::UAI::Contract::FDiagnostic& Error,
                           const FString& CanonicalAssetPath = FString());
bool ProjectMontageOriginalIntent(UeAgentAuthoringState::FStateEntry& NativeEntry, TSharedPtr<FJsonValue>& Before,
    TSharedPtr<FJsonValue>& Desired, TSharedPtr<FJsonValue>& Actual,
    TMap<FString, FString>& SourcePaths, UE::UAI::Contract::FDiagnostic& Error);
}
