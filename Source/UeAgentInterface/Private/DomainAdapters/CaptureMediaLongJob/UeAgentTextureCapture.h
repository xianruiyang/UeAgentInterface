#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob {
bool TryCaptureTextureImage(const TSharedPtr<FJsonObject> &Parameters,
                            const FString &InternalCommandId,
                            TSharedPtr<FJsonObject> &OutData,
                            UE::UAI::Contract::FDiagnostic& OutError);
}
