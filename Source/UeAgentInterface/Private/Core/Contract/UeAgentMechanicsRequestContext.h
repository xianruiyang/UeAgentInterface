// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

// Shared private inputs for domain-owned command mechanics. These types are not
// a wire protocol, transport contract, command registry, or public ingress.
struct FUeAgentResponseOptions
{
	FString View = TEXT("llm_compact");
};

struct FUeAgentRequestContext
{
	FString RequestId;
	FString Command;
	TSharedPtr<FJsonObject> Params;
	FUeAgentResponseOptions ResponseOptions;
};
