// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

namespace UeAgentReflectionAuthoring
{
	struct FProfile;
	UEAGENTINTERFACE_API bool RegisterExportAuthoring(FString& OutError);
	UEAGENTINTERFACE_API bool GetExportContract(const FProfile& Profile, UeAgentExportContract::FContract& OutContract, FString& OutError);
}

