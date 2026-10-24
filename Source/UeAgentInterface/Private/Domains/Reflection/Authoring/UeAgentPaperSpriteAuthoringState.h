// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"
#include "CoreMinimal.h"
class UPaperSprite;
class FJsonValue;
namespace UeAgentReflectionAuthoring::PaperSpriteState {
bool ReadCollision(UPaperSprite& Sprite, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError);
bool ApplyCollision(UPaperSprite& Sprite, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
void RefreshComponents(UPaperSprite& Sprite);
void RefreshSourceTextureBasis(UPaperSprite& Sprite);
}
