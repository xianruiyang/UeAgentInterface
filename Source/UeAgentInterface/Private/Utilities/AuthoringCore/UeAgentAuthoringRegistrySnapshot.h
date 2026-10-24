// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentAuthoringRegistrySnapshot
{
	UEAGENTINTERFACE_API bool BuildSnapshot(TSharedPtr<FJsonObject>& OutSnapshot, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool SerializeSnapshot(FString& OutHjson, UE::UAI::Contract::FDiagnostic& OutError);
}
