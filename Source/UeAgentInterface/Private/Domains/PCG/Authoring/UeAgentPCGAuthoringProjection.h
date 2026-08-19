// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

namespace UeAgentPCGAuthoring
{
	inline constexpr const TCHAR* ExportContractId = TEXT("uai.export.pcg.graph");
	inline constexpr const TCHAR* ProjectionId = TEXT("uai.export.pcg.graph.structure");

	UEAGENTINTERFACE_API bool RegisterExportAuthoring(FString& OutError);
	UEAGENTINTERFACE_API bool GetExportContract(
		UeAgentExportContract::FContract& OutContract,
		FString& OutError);
}
