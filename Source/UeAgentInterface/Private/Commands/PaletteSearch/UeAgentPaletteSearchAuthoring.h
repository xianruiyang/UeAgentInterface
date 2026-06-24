// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentPaletteSearch
{
	struct FCandidate;

	TSharedPtr<FJsonObject> BuildJsonAuthoringPatch(const FCandidate& Candidate);
}
