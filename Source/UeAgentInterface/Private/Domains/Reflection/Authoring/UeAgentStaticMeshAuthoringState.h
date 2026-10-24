// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonValue;
class FJsonObject;
class UStaticMesh;

namespace UeAgentReflectionAuthoring::StaticMeshState
{
TSharedPtr<FJsonObject> BuildValueDefinition();
bool Validate(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
bool Read(UStaticMesh& Mesh, const FString& SemanticAssetPath,
    TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError);
bool Apply(UStaticMesh& Mesh, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
}
