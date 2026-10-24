#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonValue;
class USkeletalMesh;

namespace UeAgentReflectionAuthoring::SkeletalMeshState
{
bool Validate(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
bool Read(USkeletalMesh& Mesh, const FString& SemanticAssetPath,
    TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError);
bool Apply(USkeletalMesh& Mesh, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
void Settle(USkeletalMesh& Mesh);
}
