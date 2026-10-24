// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FText;

namespace UeAgentPaletteSearch
{
	FString StripUiOnlyMarkers(const FString& InName);
	/** Deterministic readable name derived from a stable reflected identifier. */
	FString ReadableSemanticName(FString Identifier);
	FString MenuNameFromText(const FText& Text, const FString& Fallback = FString());
	FString ObjectPathOrPackagePath(const FString& ObjectPath, const FString& PackagePath);
}
