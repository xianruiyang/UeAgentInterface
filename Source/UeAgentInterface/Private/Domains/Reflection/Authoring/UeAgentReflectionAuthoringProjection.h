// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

namespace UeAgentReflectionAuthoring
{
	struct FProfile;
	UEAGENTINTERFACE_API bool RegisterExportAuthoring(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool GetExportContract(const FProfile& Profile, UeAgentExportContract::FContract& OutContract, UE::UAI::Contract::FDiagnostic& OutError);
}

