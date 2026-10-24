// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentPaletteSearch
{
	struct FCandidate;

	UEAGENTINTERFACE_API bool BuildAuthoringProjection(
		const FCandidate& Candidate,
		TSharedPtr<FJsonObject>& OutSeed,
		TSharedPtr<FJsonObject>& OutPlacement,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ResolveCatalogNameToCandidate(
		const FString& CatalogType,
		const FString& Name,
		FCandidate& OutCandidate,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FString& AssetPath = FString(),
		const FString& GraphName = FString(),
		const TMap<FString, FString>& Select = {});
}
