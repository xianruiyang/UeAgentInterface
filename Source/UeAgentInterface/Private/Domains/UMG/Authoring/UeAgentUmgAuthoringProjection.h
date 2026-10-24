// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

namespace UeAgentUmgAuthoring
{
	UEAGENTINTERFACE_API bool RegisterExportAuthoring(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool GetExportContract(
		UeAgentExportContract::FContract& OutContract,
		UE::UAI::Contract::FDiagnostic& OutError);
}
