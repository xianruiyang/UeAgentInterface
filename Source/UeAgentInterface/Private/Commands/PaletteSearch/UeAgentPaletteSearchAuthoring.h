// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentPaletteSearch
{
	struct FCandidate;

	TSharedPtr<FJsonObject> BuildJsonAuthoringPatch(const FCandidate& Candidate);
	bool ResolveCatalogNameToCandidate(
		const FString& CatalogType,
		const FString& Name,
		FCandidate& OutCandidate,
		FString& OutError,
		const FString& AssetPath = FString(),
		const FString& GraphName = FString());
}
