// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDescriptors.h"

class FJsonObject;

namespace UeAgentAudioFolderCommandBridge {
bool CreateMetaSound(const TSharedPtr<FJsonObject> &Params,
                     TSharedPtr<FJsonObject> &OutData, FString &OutError,
                     const FString &RequestId = FString());

bool CreateSoundCue(const TSharedPtr<FJsonObject> &Params,
                    TSharedPtr<FJsonObject> &OutData, FString &OutError,
                    const FString &RequestId = FString());

bool CreateSoundSubmix(const TSharedPtr<FJsonObject> &Params,
                       TSharedPtr<FJsonObject> &OutData, FString &OutError,
                       const FString &RequestId = FString());

bool Export(UeAgentAudioAuthoring::EAssetKind Kind,
            const TSharedPtr<FJsonObject> &Params,
            TSharedPtr<FJsonObject> &OutData, FString &OutError);

bool Apply(UeAgentAudioAuthoring::EAssetKind Kind,
           const TSharedPtr<FJsonObject> &Params,
           TSharedPtr<FJsonObject> &OutData, FString &OutError,
           const FString &RequestId = FString());

bool Validate(UeAgentAudioAuthoring::EAssetKind Kind,
              const TSharedPtr<FJsonObject> &Params,
              TSharedPtr<FJsonObject> &OutData, FString &OutError);
} // namespace UeAgentAudioFolderCommandBridge
