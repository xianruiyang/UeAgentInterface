#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"
#include "CoreMinimal.h"
class UMaterialFunctionInstance;
class FJsonValue;
namespace UeAgentMaterialAuthoring { struct FInstanceParameterEdit; }

namespace UeAgentMaterialAuthoring::FunctionInstance
{
bool Read(UMaterialFunctionInstance& Asset, TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error,
    TArray<TSharedPtr<FJsonValue>>* Definitions = nullptr);
bool Apply(UMaterialFunctionInstance& Asset, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error,
    TArray<FInstanceParameterEdit>* OutIntent = nullptr);
bool VerifyParameters(UMaterialFunctionInstance& Asset, const TArray<FInstanceParameterEdit>& Intent, UE::UAI::Contract::FDiagnostic& Error);
bool Update(UMaterialFunctionInstance& Asset, UE::UAI::Contract::FDiagnostic& Error);
// A detached proposal will take Destination's identity when published/updated.
bool ValidateParentChain(UMaterialFunctionInstance& Asset, UE::UAI::Contract::FDiagnostic& Error,
    const UMaterialFunctionInstance* Destination = nullptr);
}
