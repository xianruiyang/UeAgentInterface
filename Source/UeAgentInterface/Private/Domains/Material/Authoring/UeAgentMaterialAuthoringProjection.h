// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

namespace UeAgentMaterialAuthoring
{
	inline constexpr const TCHAR* MaterialExportContractId = TEXT("uai.export.material.asset");
	inline constexpr const TCHAR* MaterialFunctionExportContractId = TEXT("uai.export.material.function");
	inline constexpr const TCHAR* MaterialProjectionId = TEXT("uai.export.material.graph");

	UEAGENTINTERFACE_API bool RegisterExportAuthoring(FString& OutError);
	UEAGENTINTERFACE_API bool GetExportContract(
		const FString& AssetKind,
		UeAgentExportContract::FContract& OutContract,
		FString& OutError);
}
