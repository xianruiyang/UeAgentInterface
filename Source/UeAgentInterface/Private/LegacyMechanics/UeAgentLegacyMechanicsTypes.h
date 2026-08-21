// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

// These types are private inputs to retained, domain-owned mechanics. They are
// not a wire protocol, transport contract, command registry, or public ingress.
struct FUeAgentResponseOptions
{
	FString View = TEXT("llm_compact");
	int32 MaxItems = 0;
};

struct FUeAgentRequestContext
{
	FString RequestId;
	FString Command;
	TSharedPtr<FJsonObject> Params;
	FUeAgentResponseOptions ResponseOptions;
};
