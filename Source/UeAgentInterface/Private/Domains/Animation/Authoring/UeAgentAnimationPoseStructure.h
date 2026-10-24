#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class UPoseAsset;
class FJsonObject;

namespace UeAgentAnimationAuthoring {
TSharedPtr<FJsonObject> ReadPoseStructure(UPoseAsset& Asset, UE::UAI::Contract::FDiagnostic& Error);
bool ApplyPoseStructure(UPoseAsset& Asset, const TSharedPtr<const FJsonObject>& Document, UE::UAI::Contract::FDiagnostic& Error,
    const FString& CanonicalAssetPath = FString());
}
