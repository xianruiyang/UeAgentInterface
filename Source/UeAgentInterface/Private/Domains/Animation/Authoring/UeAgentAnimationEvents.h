#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAnimationSequenceChanges.h"

class FJsonObject;
class UAnimSequenceBase;
struct FAnimNotifyEvent;

namespace UeAgentAnimationAuthoring {
bool ReadAnimationEvents(UAnimSequenceBase& Asset, FJsonObject& Document, UE::UAI::Contract::FDiagnostic& Error);
bool ApplyAnimationEvents(UAnimSequenceBase& Asset, const FJsonObject& Document, int32& Changes, UE::UAI::Contract::FDiagnostic& Error,
    bool bTrackIndicesResolved = false, const TArray<FAnimNotifyEvent>* CapturedNotifies = nullptr,
    const FString& CanonicalAssetPath = FString());
bool ApplyAnimationEventChanges(UAnimSequence& Asset,
    const UE::UAI::Authoring::Timeline::FSequenceChanges& Changes,
    UE::UAI::Authoring::Timeline::FUeAgentTimelineAnimationExecutionError& Error, const FString& CanonicalObjectPath);
}
