#include "Domains/Reflection/Authoring/UeAgentEqPresetRuntimeState.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "AudioMixerSubmix.h"
#include "AudioThread.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "Sound/SoundSubmix.h"
#include "SubmixEffects/AudioMixerSubmixEffectEQ.h"
#include "UObject/UObjectIterator.h"

namespace UeAgentReflectionAuthoring
{
namespace
{
using FDiagnostic = UE::UAI::Contract::FDiagnostic;

FDiagnostic NativeFailure(const FString& Code, const FString& Reason, const FString& Target,
    const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
{
    auto Error = FDiagnostic::Failure(Code, Reason, TEXT("eq_preset_runtime_state"));
    Error.SourceKind = TEXT("native_result");
    Error.TargetPath = Target;
    Error.PropertyPath = Field;
    Error.ActualValue = Actual;
    Error.ExpectedValue = Expected;
    Error.ActualValuePresent = Actual.IsValid();
    Error.ExpectedValuePresent = Expected.IsValid();
    return Error;
}

bool RequireNative(bool bCondition, FDiagnostic& Error, const FString& Code,
    const FString& Reason, const FString& Target, const FString& Field)
{
    if (bCondition) return true;
    Error = NativeFailure(Code, Reason, Target, Field,
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
}

struct FRenderCall
{
    FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
    FCriticalSection Gate;
    bool bCancelled = false;
    bool bResult = false;
    FDiagnostic Error;
    ~FRenderCall() { FPlatformProcess::ReturnSynchEventToPool(Done); }
};
bool OnRender(Audio::FMixerDevice& Mixer, const FString& Target,
    TFunction<bool(FDiagnostic&)> Work, FDiagnostic& Error)
{
    auto Call = MakeShared<FRenderCall, ESPMode::ThreadSafe>();
    FAudioThread::RunCommandOnAudioThread([&Mixer, Call, Work = MoveTemp(Work)]() mutable
    {
        Mixer.AudioRenderThreadCommand([Call, Work = MoveTemp(Work)]()
        {
            FScopeLock Lock(&Call->Gate);
            if (!Call->bCancelled) Call->bResult = Work(Call->Error);
            Call->Done->Trigger();
        });
    });
    FAudioCommandFence Fence; Fence.BeginFence(); Fence.Wait();
    const bool bCompleted = Call->Done->Wait(5000);
    // A timed-out queued call cannot mutate a later transaction or dereference
    // caller-owned state after returning. If it began, wait for its short body.
    FScopeLock Lock(&Call->Gate);
    Call->bCancelled = true;
    if (!bCompleted)
    {
        Error = NativeFailure(TEXT("eq_preset_render_command_timeout"),
            TEXT("The audio render command did not complete within the 5000 ms wait."),
            Target, TEXT("render_command_completed"),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        if (!Call->Error.IsEmpty()) Error.Diagnostics.Add(MoveTemp(Call->Error));
        return false;
    }
    if (!Call->bResult) Error = MoveTemp(Call->Error);
    return Call->bResult;
}
}

struct FEqPresetRuntimeState::FImpl
{
    struct FChain
    {
        FAudioDeviceHandle Device;
        TWeakObjectPtr<USoundSubmix> Submix;
        FString SubmixPath;
        Audio::FMixerSubmixPtr Instance;
        TArray<FSoundEffectSubmixPtr> Effects;
        TArray<int32> TargetSlots;
        TArray<bool> Enabled;
    };
    TArray<FChain> Chains;
    bool Visit(USubmixEffectSubmixEQPreset* Preset, UE::UAI::Contract::FDiagnostic& Error)
    {
        for (FChain& Chain : Chains)
        {
            auto* Mixer = static_cast<Audio::FMixerDevice*>(Chain.Device.GetAudioDevice());
            Audio::FMixerSubmixPtr Registered;
            if (Mixer && Chain.Submix.IsValid()) {
                FAudioThread::RunCommandOnAudioThread([&]() { Registered = Mixer->GetSubmixInstance(Chain.Submix.Get()).Pin(); });
                FAudioCommandFence Fence; Fence.BeginFence(); Fence.Wait();
            }
            if (Registered != Chain.Instance)
            {
                const FString Field = !Mixer ? TEXT("audio_device_available")
                    : !Chain.Submix.IsValid() ? TEXT("submix_available") : TEXT("registered_instance_matches_captured");
                const FString Reason = !Mixer ? TEXT("The audio device for the captured submix is no longer available.")
                    : !Chain.Submix.IsValid() ? TEXT("The captured sound submix is no longer available.")
                    : TEXT("The registered runtime submix instance changed after capture.");
                RequireNative(false, Error, TEXT("eq_preset_registration_changed"), Reason, Chain.SubmixPath, Field);
                return false;
            }
            if (!RequireNative(Mixer != nullptr, Error, TEXT("eq_preset_audio_device_unavailable"),
                TEXT("The audio device for the captured submix is no longer available."),
                Chain.SubmixPath, TEXT("audio_device_available"))) return false;
            if (!OnRender(*Mixer, Chain.SubmixPath, [&Chain, Preset, Mixer](FDiagnostic& RenderError)
            {
                if (!RequireNative(Chain.Instance.IsValid(), RenderError, TEXT("eq_preset_chain_unavailable"),
                    TEXT("The captured runtime submix chain is no longer available."),
                    Chain.SubmixPath, TEXT("chain_available"))) return false;
                const int32 CurrentCount = Chain.Instance->GetNumChainEffects();
                if (CurrentCount != Chain.Effects.Num())
                {
                    RenderError = NativeFailure(TEXT("eq_preset_chain_count_changed"),
                        TEXT("The number of runtime effects changed after capture."), Chain.SubmixPath,
                        TEXT("effects/count"), MakeShared<FJsonValueNumber>(CurrentCount),
                        MakeShared<FJsonValueNumber>(Chain.Effects.Num()));
                    return false;
                }
                for (int32 Index = 0; Index < Chain.Effects.Num(); ++Index)
                    if (!RequireNative(Chain.Instance->GetSubmixEffect(Index) == Chain.Effects[Index], RenderError,
                        TEXT("eq_preset_chain_effect_changed"), TEXT("The runtime effect at this slot changed after capture."),
                        Chain.SubmixPath, FString::Printf(TEXT("effects/%d/instance_matches_captured"), Index))) return false;
                for (int32 Index = 0; Index < Chain.TargetSlots.Num(); ++Index)
                {
                    const bool bActive = Chain.Effects[Chain.TargetSlots[Index]]->IsActive();
                    if (bActive != Chain.Enabled[Index])
                    {
                        RenderError = NativeFailure(TEXT("eq_preset_effect_enabled_changed"),
                            TEXT("The runtime EQ effect's enabled state changed after capture."), Chain.SubmixPath,
                            FString::Printf(TEXT("effects/%d/enabled"), Chain.TargetSlots[Index]),
                            MakeShared<FJsonValueBoolean>(bActive), MakeShared<FJsonValueBoolean>(Chain.Enabled[Index]));
                        return false;
                    }
                }
                if (!Preset) return true;
                for (int32 Index = 0; Index < Chain.TargetSlots.Num(); ++Index)
                {
                    const int32 Slot = Chain.TargetSlots[Index];
                    FSoundEffectSubmixInitData InitData;
                    InitData.DeviceID = Chain.Device.GetDeviceID();
                    InitData.SampleRate = Mixer->GetSampleRate();
                    InitData.PresetSettings = nullptr;
                    InitData.ParentPresetUniqueId = Preset->GetUniqueID();
                    auto Effect = USoundEffectPreset::CreateInstance<FSoundEffectSubmixInitData, FSoundEffectSubmix>(InitData, *Preset);
                    if (!RequireNative(Effect.IsValid(), RenderError, TEXT("eq_preset_effect_create_failed"),
                        TEXT("Unreal could not create the replacement runtime EQ effect."), Chain.SubmixPath,
                        FString::Printf(TEXT("effects/%d/replacement_created"), Slot))) return false;
                    Effect->SetEnabled(Chain.Enabled[Index]);
                    Chain.Instance->ReplaceSoundEffectSubmix(Slot, Effect);
                    Chain.Effects[Slot] = Effect;
                    if (!RequireNative(Chain.Instance->GetSubmixEffect(Slot) == Effect, RenderError,
                        TEXT("eq_preset_effect_replace_failed"), TEXT("The submix slot does not contain the replacement EQ effect."),
                        Chain.SubmixPath, FString::Printf(TEXT("effects/%d/instance_matches_replacement"), Slot))) return false;
                    if (!RequireNative(Effect->GetPreset() == Preset, RenderError,
                        TEXT("eq_preset_effect_preset_mismatch"), TEXT("The replacement EQ effect is bound to a different preset."),
                        Chain.SubmixPath, FString::Printf(TEXT("effects/%d/preset_matches_requested"), Slot))) return false;
                    if (!RequireNative(Effect->GetParentPresetId() == Preset->GetUniqueID(), RenderError,
                        TEXT("eq_preset_effect_parent_mismatch"), TEXT("The replacement EQ effect retains a different parent preset identity."),
                        Chain.SubmixPath, FString::Printf(TEXT("effects/%d/parent_preset_matches_requested"), Slot))) return false;
                }
                return true;
            }, Error)) return false;
        }
        return true;
    }
};

FEqPresetRuntimeState::FEqPresetRuntimeState() : Impl(MakeUnique<FImpl>()) {}
FEqPresetRuntimeState::~FEqPresetRuntimeState() = default;

bool FEqPresetRuntimeState::Capture(USubmixEffectSubmixEQPreset& Preset, UE::UAI::Contract::FDiagnostic& Error)
{
    auto* Manager = FAudioDeviceManager::Get();
    if (!Manager) return true;
    TArray<USoundSubmix*> Submixes;
    for (TObjectIterator<USoundSubmix> It; It; ++It)
        if (!It->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) && !It->IsTemplate()) Submixes.Add(*It);
    bool bValid = true;
    Manager->IterateOverAllDevices([&](Audio::FDeviceId Id, FAudioDevice* Device)
    {
        if (!bValid || !Device || !Device->bAudioMixerModuleLoaded) return;
        auto* Mixer = static_cast<Audio::FMixerDevice*>(Device);
        for (USoundSubmix* Submix : Submixes)
        {
            Audio::FMixerSubmixPtr Instance;
            FAudioThread::RunCommandOnAudioThread([Mixer, Submix, &Instance]() { Instance = Mixer->GetSubmixInstance(Submix).Pin(); });
            FAudioCommandFence Fence; Fence.BeginFence(); Fence.Wait();
            if (!Instance.IsValid()) continue;
            FImpl::FChain Chain; Chain.Device = Manager->GetAudioDevice(Id); Chain.Instance = Instance; Chain.Submix = Submix;
            Chain.SubmixPath = Submix->GetPathName();
            if (!OnRender(*Mixer, Chain.SubmixPath, [&](FDiagnostic& RenderError)
            {
                bool bTarget = Submix->SubmixEffectChain.Contains(&Preset);
                const int32 Count = Instance->GetNumChainEffects();
                for (int32 Index = 0; Index < Count; ++Index)
                {
                    auto Effect = Instance->GetSubmixEffect(Index);
                    Chain.Effects.Add(Effect);
                    if (Effect.IsValid() && Effect->GetPreset() == &Preset)
                    { bTarget = true; Chain.TargetSlots.Add(Index); Chain.Enabled.Add(Effect->IsActive()); }
                }
                if (!bTarget) return true;
                // Only a current chain matching the native asset is eligible;
                // a dynamic override cannot be interpreted using asset indices.
                if (Count != Submix->SubmixEffectChain.Num())
                {
                    RenderError = NativeFailure(TEXT("eq_preset_incompatible_current_chain"),
                        TEXT("The runtime effect count differs from the sound submix asset's effect chain."),
                        Chain.SubmixPath, TEXT("effects/count"), MakeShared<FJsonValueNumber>(Count),
                        MakeShared<FJsonValueNumber>(Submix->SubmixEffectChain.Num()));
                    return false;
                }
                for (int32 Index = 0; Index < Count; ++Index)
                {
                    if (!RequireNative(Chain.Effects[Index].IsValid(), RenderError,
                        TEXT("eq_preset_incompatible_current_chain"), TEXT("The runtime submix chain has no effect at this slot."),
                        Chain.SubmixPath, FString::Printf(TEXT("effects/%d/available"), Index))) return false;
                    if (!RequireNative(Chain.Effects[Index]->GetPreset() == Submix->SubmixEffectChain[Index], RenderError,
                        TEXT("eq_preset_incompatible_current_chain"), TEXT("The runtime effect's preset differs from the sound submix asset at this slot."),
                        Chain.SubmixPath, FString::Printf(TEXT("effects/%d/preset_matches_asset"), Index))) return false;
                }
                return true;
            }, Error)) { bValid = false; return; }
            if (!Chain.TargetSlots.IsEmpty()) Impl->Chains.Add(MoveTemp(Chain));
        }
    });
    return bValid;
}
bool FEqPresetRuntimeState::Validate(UE::UAI::Contract::FDiagnostic& Error) { return Impl->Visit(nullptr, Error); }
bool FEqPresetRuntimeState::Rebind(USubmixEffectSubmixEQPreset& Preset, UE::UAI::Contract::FDiagnostic& Error) { return Impl->Visit(&Preset, Error); }
}
