#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class USubmixEffectSubmixEQPreset;

namespace UeAgentReflectionAuthoring
{
// Native current-chain state associated with an EQ asset replacement. This is
// deliberately not a snapshot of DSP history or dynamic/fading effect chains.
class FEqPresetRuntimeState
{
public:
    FEqPresetRuntimeState();
    ~FEqPresetRuntimeState();
    bool Capture(USubmixEffectSubmixEQPreset& Preset, UE::UAI::Contract::FDiagnostic& Error);
    bool Validate(UE::UAI::Contract::FDiagnostic& Error);
    bool Rebind(USubmixEffectSubmixEQPreset& Preset, UE::UAI::Contract::FDiagnostic& Error);
private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};
}
