// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentPaletteSearch
{
	struct FCandidate;

	UEAGENTINTERFACE_API bool BuildAuthoringProjection(
		const FCandidate& Candidate,
		TSharedPtr<FJsonObject>& OutSeed,
		TSharedPtr<FJsonObject>& OutPlacement,
		FString& OutError);
	UEAGENTINTERFACE_API bool ResolveCatalogNameToCandidate(
		const FString& CatalogType,
		const FString& Name,
		FCandidate& OutCandidate,
		FString& OutError,
		const FString& AssetPath = FString(),
		const FString& GraphName = FString());
}
