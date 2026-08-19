// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDescriptors.h"

class FJsonObject;

namespace UeAgentAnimationFolderCommandBridge {
bool Export(UeAgentAnimationAuthoring::EAssetKind Kind,
            const TSharedPtr<FJsonObject> &Params,
            TSharedPtr<FJsonObject> &OutData, FString &OutError);

bool Apply(UeAgentAnimationAuthoring::EAssetKind Kind,
           const TSharedPtr<FJsonObject> &Params,
           TSharedPtr<FJsonObject> &OutData, FString &OutError,
           const FString &RequestId = FString());

bool Validate(UeAgentAnimationAuthoring::EAssetKind Kind,
              const TSharedPtr<FJsonObject> &Params,
              TSharedPtr<FJsonObject> &OutData, FString &OutError);
} // namespace UeAgentAnimationFolderCommandBridge
