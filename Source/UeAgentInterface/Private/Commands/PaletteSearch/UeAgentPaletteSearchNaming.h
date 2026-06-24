// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FText;

namespace UeAgentPaletteSearch
{
	FString StripUiOnlyMarkers(const FString& InName);
	FString MenuNameFromText(const FText& Text, const FString& Fallback = FString());
	FString ObjectPathOrPackagePath(const FString& ObjectPath, const FString& PackagePath);
}
