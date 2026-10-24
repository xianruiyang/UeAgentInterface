// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
class USoundCue;

namespace UeAgentAudioAuthoring
{
TSharedPtr<FJsonObject> BuildSoundCueInfo(USoundCue* Cue, bool bIncludeNodes,
                                           bool bIncludeProperties,
                                           bool bIncludeValidation);
bool SaveSoundCuePackages(const TArray<UObject*>& Objects, UE::UAI::Contract::FDiagnostic& OutError);
} // namespace UeAgentAudioAuthoring
