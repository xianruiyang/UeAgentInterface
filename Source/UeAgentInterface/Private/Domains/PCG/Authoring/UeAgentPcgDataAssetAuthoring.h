#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UPCGDataAsset;

namespace UeAgentPcgDataAssetAuthoring
{
bool Read(UPCGDataAsset& Asset, TSharedPtr<FJsonValue>& Value,
    UE::UAI::Contract::FDiagnostic& Error, const FString& LogicalTarget = FString());
bool Apply(UPCGDataAsset& Asset, const TSharedPtr<FJsonValue>& Value,
    UE::UAI::Contract::FDiagnostic& Error, const FString& LogicalTarget = FString());
void AppendPreservationSnapshot(UPCGDataAsset& Asset, const TSharedPtr<FJsonValue>& Value);
TSharedPtr<FJsonObject> BuildValueDefinition();
}
