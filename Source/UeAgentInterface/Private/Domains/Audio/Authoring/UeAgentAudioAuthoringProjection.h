// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDescriptors.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

namespace UeAgentAudioAuthoring {
UEAGENTINTERFACE_API bool RegisterExportAuthoring(UE::UAI::Contract::FDiagnostic& OutError);
UEAGENTINTERFACE_API bool
GetExportContract(const FProfile &Profile,
                  UeAgentExportContract::FContract &OutContract,
                  UE::UAI::Contract::FDiagnostic& OutError);
} // namespace UeAgentAudioAuthoring
