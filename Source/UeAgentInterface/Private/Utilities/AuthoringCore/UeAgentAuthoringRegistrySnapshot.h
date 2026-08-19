// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentAuthoringRegistrySnapshot
{
	UEAGENTINTERFACE_API bool BuildSnapshot(TSharedPtr<FJsonObject>& OutSnapshot, FString& OutError);
	UEAGENTINTERFACE_API bool SerializeSnapshot(FString& OutHjson, FString& OutError);
}
