// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

namespace UeAgentBlueprintAuthoring
{
	inline constexpr const TCHAR* BlueprintExportContractId = TEXT("uai.export.blueprint.asset");
	inline constexpr const TCHAR* BlueprintProjectionId = TEXT("uai.export.blueprint.compound");

	UEAGENTINTERFACE_API bool RegisterExportAuthoring(FString& OutError);
	UEAGENTINTERFACE_API bool GetExportContract(
		UeAgentExportContract::FContract& OutContract,
		FString& OutError);
	UEAGENTINTERFACE_API bool GetReusableProjectionKeys(
		const FString& FieldId,
		const UeAgentAuthoringState::FStateEntry& Entry,
		TArray<FString>& OutKeys);
}
