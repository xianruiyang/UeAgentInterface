// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

namespace UeAgentUmgAuthoring
{
	UEAGENTINTERFACE_API bool RegisterExportAuthoring(FString& OutError);
	UEAGENTINTERFACE_API bool GetExportContract(
		UeAgentExportContract::FContract& OutContract,
		FString& OutError);
}
