// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDescriptors.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

namespace UeAgentAnimationAuthoring {
UEAGENTINTERFACE_API bool RegisterExportAuthoring(FString &OutError);
UEAGENTINTERFACE_API bool
GetExportContract(const FProfile &Profile,
                  UeAgentExportContract::FContract &OutContract,
                  FString &OutError);
} // namespace UeAgentAnimationAuthoring
