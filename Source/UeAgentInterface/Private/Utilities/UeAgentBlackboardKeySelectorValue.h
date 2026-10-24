// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FProperty;
class UObject;

// The native Details customization resolves these values through the nearest
// IBlackboardAssetProvider. Keep that type lifecycle independent of BT node class.
namespace UeAgentBlackboardKeySelectorValue
{
TSet<FName> RootProperties(const UObject& Owner);
void Resolve(UObject& Owner, TArray<TArray<FProperty*>>& OutNotifications,
    bool bInitializeContext = false);
}
