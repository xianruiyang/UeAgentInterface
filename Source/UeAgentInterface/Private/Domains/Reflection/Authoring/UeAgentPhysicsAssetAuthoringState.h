#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UPhysicsAsset;

namespace UeAgentReflectionAuthoring::PhysicsAssetState
{
TSharedPtr<FJsonObject> BuildValueDefinition();
bool Validate(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
bool Read(UPhysicsAsset& Asset, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError);
bool Apply(UPhysicsAsset& Asset, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
void Refresh(UPhysicsAsset& Asset);
}
