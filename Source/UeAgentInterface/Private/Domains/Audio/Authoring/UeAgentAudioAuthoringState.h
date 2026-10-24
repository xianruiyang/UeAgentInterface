// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UObject;

namespace UeAgentAudioAuthoring {
UEAGENTINTERFACE_API bool BuildAssetSnapshotReadRequests(
    const FProfile &Profile, UObject *Asset, const FString &CanonicalAssetPath,
    bool bExpectedAbsent,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError);
UEAGENTINTERFACE_API bool BuildAssetExportSnapshotReadRequests(
    const FProfile &Profile, UObject &Asset,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError);
UEAGENTINTERFACE_API bool RegisterStateAccessor(UE::UAI::Contract::FDiagnostic& OutError);
UEAGENTINTERFACE_API bool RegisterNativeDefaultProvider(UE::UAI::Contract::FDiagnostic& OutError);
} // namespace UeAgentAudioAuthoring
