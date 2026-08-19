// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDescriptors.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

namespace UeAgentAudioAuthoring {
UEAGENTINTERFACE_API bool RegisterExportAuthoring(FString &OutError);
UEAGENTINTERFACE_API bool
GetExportContract(const FProfile &Profile,
                  UeAgentExportContract::FContract &OutContract,
                  FString &OutError);
} // namespace UeAgentAudioAuthoring
