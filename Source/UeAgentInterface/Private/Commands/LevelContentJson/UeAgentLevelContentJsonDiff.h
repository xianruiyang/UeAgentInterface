// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UeAgentLevelContentJsonOps.h"

class UWorld;

namespace UeAgentLevelContentJson
{
	TSharedPtr<FJsonObject> DiffAgainstWorld(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, bool bActorOnly);
}
