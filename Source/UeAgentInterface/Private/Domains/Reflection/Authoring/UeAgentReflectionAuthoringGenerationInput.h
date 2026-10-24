#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class UObject;
class FJsonObject;
class FJsonValue;

namespace UeAgentReflectionAuthoring
{
struct FProfile;
namespace GenerationInput
{
struct FPreparedInput;
bool ReadTemplate(const FProfile& Profile, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError);
bool ReadGeneratedState(const FProfile& Profile, UObject& Target, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError);
bool Prepare(const FProfile& Profile, UObject& Target, const TSharedPtr<const FJsonObject>& Extension,
    const FString& SemanticAssetPath, TSharedPtr<FPreparedInput>& OutInput, UE::UAI::Contract::FDiagnostic& OutError);
bool Apply(UObject& Target, const TSharedPtr<FPreparedInput>& Input, UE::UAI::Contract::FDiagnostic& OutError);
bool Verify(UObject* Target, const TSharedPtr<FPreparedInput>& Input, UE::UAI::Contract::FDiagnostic& OutError);
}
}
