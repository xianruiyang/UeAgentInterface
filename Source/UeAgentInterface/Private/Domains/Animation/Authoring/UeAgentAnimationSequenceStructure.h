// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
class UAnimSequence;

namespace UeAgentAnimationAuthoring {
TSharedPtr<FJsonObject> BuildSequenceCurveAuthoringExample(bool bTransformChannel, float Value);
TSharedPtr<FJsonObject> ReadSequenceStructure(UAnimSequence& Sequence, UE::UAI::Contract::FDiagnostic& OutError);
bool ApplySequenceStructure(UAnimSequence& Sequence,
                            const TSharedPtr<const FJsonObject>& Document,
                            int32& OutChanges, UE::UAI::Contract::FDiagnostic& OutError,
                            const FString& CanonicalAssetPath = FString());
}
