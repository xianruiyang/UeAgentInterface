// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"
#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Sound/AmbientSound.h"
#include "Sound/AudioVolume.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundSubmix.h"
#include "UObject/UObjectIterator.h"

bool FUeAgentHttpServer::CmdAudioComponentValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	UAudioComponent* Component = FindAudioComponentInWorld(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	FString ExpectedSound;
	if (Ctx.Params->TryGetStringField(TEXT("expected_sound"), ExpectedSound) && !ExpectedSound.IsEmpty() && ObjectPathOrEmpty(Component->Sound) != ToObjectPath(ExpectedSound))
	{
		AddIssue(Issues, TEXT("sound_mismatch"), TEXT("error"), TEXT("AudioComponent sound does not match expected_sound."));
	}
	OutData = MakeShared<FJsonObject>();
	AddAudioComponentSnapshot(Component, OutData);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("failed"));
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioRuntimeProfile));
	return true;
}
bool FUeAgentHttpServer::CmdAudioComponentRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	UAudioComponent* Component = FindAudioComponentInWorld(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	AddAudioComponentSnapshot(Component, OutData);
	OutData->SetBoolField(TEXT("audio_device_available"), World && World->GetAudioDeviceRaw() != nullptr);
	return true;
}
bool FUeAgentHttpServer::CmdAudioComponentRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	UAudioComponent* Component = FindAudioComponentInWorld(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	if (!World || !World->GetAudioDeviceRaw())
	{
		OutError = TEXT("audio_device_unavailable");
		return false;
	}
	float Duration = 0.25f;
	GetOptionalFloat(Ctx.Params, TEXT("duration"), Duration, OutError);
	Duration = FMath::Clamp(Duration, 0.01f, 5.0f);
	Component->Play(0.0f);
	FPlatformProcess::Sleep(Duration);
	const bool bPlaying = Component->IsPlaying();
	int32 ActiveWaveCount = 0;
	const float Peak = ComputePeakForSound(Component->Sound, ActiveWaveCount);
	bool bStop = true;
	GetOptionalBool(Ctx.Params, TEXT("stop_after_preview"), bStop, OutError);
	GetOptionalBool(Ctx.Params, TEXT("cleanup"), bStop, OutError);
	if (bStop)
	{
		Component->Stop();
	}
	double ExpectedMinPeak = 0.0;
	const bool bHasExpectedPeak = Ctx.Params->TryGetNumberField(TEXT("expected_min_peak"), ExpectedMinPeak);
	TArray<TSharedPtr<FJsonValue>> CleanupResults;
	if (bStop)
	{
		TSharedPtr<FJsonObject> CleanupResult = MakeShared<FJsonObject>();
		CleanupResult->SetStringField(TEXT("action"), TEXT("stop_component"));
		CleanupResult->SetBoolField(TEXT("completed"), true);
		CleanupResults.Add(MakeShared<FJsonValueObject>(CleanupResult));
	}
	OutData = MakeShared<FJsonObject>();
	AddAudioComponentSnapshot(Component, OutData);
	OutData->SetBoolField(TEXT("started"), true);
	OutData->SetBoolField(TEXT("stopped"), bStop);
	OutData->SetBoolField(TEXT("observed_playing"), bPlaying);
	OutData->SetNumberField(TEXT("elapsed"), Duration);
	OutData->SetNumberField(TEXT("peak_envelope"), Peak);
	OutData->SetNumberField(TEXT("active_wave_count"), ActiveWaveCount);
	OutData->SetBoolField(TEXT("probe_passed"), !bHasExpectedPeak || Peak >= ExpectedMinPeak);
	OutData->SetArrayField(TEXT("cleanup_results"), CleanupResults);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioRuntimeProfile));
	if (bHasExpectedPeak && Peak < ExpectedMinPeak)
	{
		OutError = TEXT("audio_peak_below_expected_min");
		return false;
	}
	return true;
}
bool FUeAgentHttpServer::CmdAudioVolumeValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	FString ActorName;
	if (!JsonTryGetStringAny(Ctx.Params, { TEXT("actor"), TEXT("actor_name"), TEXT("actor_label") }, ActorName))
	{
		OutError = TEXT("missing_actor");
		return false;
	}
	AAudioVolume* Volume = Cast<AAudioVolume>(FindActorByNameOrLabelDirect(World, ActorName));
	if (!Volume)
	{
		OutError = TEXT("audio_volume_not_found");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("actor"), Volume->GetActorLabel());
	OutData->SetNumberField(TEXT("priority"), Volume->GetPriority());
	OutData->SetBoolField(TEXT("enabled"), Volume->GetEnabled());
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	OutData->SetArrayField(TEXT("json_issues"), {});
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioRuntimeProfile));
	return true;
}
bool FUeAgentHttpServer::CmdAmbientSoundValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	FString ActorName;
	if (!JsonTryGetStringAny(Ctx.Params, { TEXT("actor"), TEXT("actor_name"), TEXT("actor_label") }, ActorName))
	{
		OutError = TEXT("missing_actor");
		return false;
	}
	AAmbientSound* Ambient = Cast<AAmbientSound>(FindActorByNameOrLabelDirect(World, ActorName));
	if (!Ambient)
	{
		OutError = TEXT("ambient_sound_not_found");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("actor"), Ambient->GetActorLabel());
	AddAudioComponentSnapshot(Ambient->GetAudioComponent(), OutData);
	OutData->SetStringField(TEXT("status"), Ambient->GetAudioComponent() ? TEXT("ok") : TEXT("failed"));
	OutData->SetArrayField(TEXT("json_issues"), Ambient->GetAudioComponent() ? TArray<TSharedPtr<FJsonValue>>{} : TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueObject>(MakeIssue(TEXT("missing_audio_component"), TEXT("error"), TEXT("AmbientSound has no AudioComponent."))) });
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioRuntimeProfile));
	return true;
}
bool FUeAgentHttpServer::CmdAudioRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	if (!World)
	{
		OutError = TEXT("world_not_found");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Components;
	TArray<TSharedPtr<FJsonValue>> ActiveSounds;
	int32 ActiveCount = 0;
	for (TObjectIterator<UAudioComponent> It; It; ++It)
	{
		UAudioComponent* Component = *It;
		if (!Component || Component->GetWorld() != World)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		AddAudioComponentSnapshot(Component, Item);
		if (Component->IsPlaying())
		{
			++ActiveCount;
			ActiveSounds.Add(MakeShared<FJsonValueObject>(Item));
		}
		Components.Add(MakeShared<FJsonValueObject>(Item));
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetBoolField(TEXT("audio_device_available"), World->GetAudioDeviceRaw() != nullptr);
	OutData->SetNumberField(TEXT("active_component_count"), ActiveCount);
	OutData->SetNumberField(TEXT("active_sound_count"), ActiveCount);
	OutData->SetNumberField(TEXT("audio_component_count"), Components.Num());
	OutData->SetArrayField(TEXT("active_sounds"), ActiveSounds);
	OutData->SetArrayField(TEXT("components"), Components);
	OutData->SetArrayField(TEXT("submixes"), {});
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioRuntimeProfile));
	return true;
}
bool FUeAgentHttpServer::CmdAudioRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	if (!World)
	{
		OutError = TEXT("world_not_found");
		return false;
	}
	if (!World->GetAudioDeviceRaw())
	{
		OutError = TEXT("audio_device_unavailable");
		return false;
	}
	FString SoundPath;
	if (!JsonTryGetStringAny(Ctx.Params, { TEXT("sound"), TEXT("asset_path"), TEXT("sound_asset") }, SoundPath))
	{
		OutError = TEXT("missing_sound");
		return false;
	}
	USoundBase* Sound = Cast<USoundBase>(LoadAssetObject(SoundPath));
	if (!Sound)
	{
		OutError = TEXT("sound_not_found");
		return false;
	}
	float Duration = 0.25f;
	float Volume = 1.0f;
	bool bStop = true;
	GetOptionalFloat(Ctx.Params, TEXT("duration"), Duration, OutError);
	GetOptionalFloat(Ctx.Params, TEXT("volume"), Volume, OutError);
	GetOptionalBool(Ctx.Params, TEXT("stop_after_preview"), bStop, OutError);
	Duration = FMath::Clamp(Duration, 0.01f, 5.0f);
	UAudioComponent* Component = NewObject<UAudioComponent>(World);
	if (!Component)
	{
		OutError = TEXT("create_audio_component_failed");
		return false;
	}
	Component->bAutoDestroy = false;
	Component->bIsPreviewSound = true;
	Component->SetSound(Sound);
	Component->SetVolumeMultiplier(Volume);
	Component->RegisterComponentWithWorld(World);
	GPreviewComponents.Add(Component);
	Component->Play(0.0f);
	FPlatformProcess::Sleep(Duration);
	const bool bObservedPlaying = Component->IsPlaying();
	int32 ActiveWaveCount = 0;
	const float Peak = ComputePeakForSound(Sound, ActiveWaveCount);
	double ExpectedMinPeak = 0.0;
	bool bHasExpectedPeak = Ctx.Params->TryGetNumberField(TEXT("expected_min_peak"), ExpectedMinPeak);
	if (bStop)
	{
		Component->Stop();
		Component->DestroyComponent();
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Sound, OutData);
	OutData->SetBoolField(TEXT("started"), true);
	OutData->SetBoolField(TEXT("stopped"), bStop);
	OutData->SetBoolField(TEXT("observed_playing"), bObservedPlaying);
	OutData->SetNumberField(TEXT("elapsed"), Duration);
	OutData->SetNumberField(TEXT("peak_envelope"), Peak);
	OutData->SetNumberField(TEXT("active_wave_count"), ActiveWaveCount);
	OutData->SetBoolField(TEXT("probe_passed"), !bHasExpectedPeak || Peak >= ExpectedMinPeak);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioRuntimeProfile));
	if (bHasExpectedPeak && Peak < ExpectedMinPeak)
	{
		OutError = TEXT("audio_peak_below_expected_min");
		return false;
	}
	return true;
}
bool FUeAgentHttpServer::CmdAudioSubmixMeterProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	if (!World)
	{
		OutError = TEXT("world_not_found");
		return false;
	}
	if (!World->GetAudioDeviceRaw())
	{
		OutError = TEXT("audio_device_unavailable");
		return false;
	}
	FString SubmixPath;
	Ctx.Params->TryGetStringField(TEXT("submix"), SubmixPath);
	USoundSubmix* Submix = SubmixPath.IsEmpty() ? nullptr : Cast<USoundSubmix>(LoadAssetObject(SubmixPath));
	TArray<float> Frequencies;
	const TArray<TSharedPtr<FJsonValue>>* FrequencyValues = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("frequencies"), FrequencyValues) && FrequencyValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *FrequencyValues)
		{
			Frequencies.Add(Value.IsValid() ? static_cast<float>(Value->AsNumber()) : 0.0f);
		}
	}
	if (Frequencies.Num() == 0 && Ctx.Params->TryGetArrayField(TEXT("bands"), FrequencyValues) && FrequencyValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *FrequencyValues)
		{
			Frequencies.Add(Value.IsValid() ? static_cast<float>(Value->AsNumber()) : 0.0f);
		}
	}
	if (Frequencies.Num() == 0)
	{
		Frequencies = { 100.0f, 1000.0f, 4000.0f };
	}
	float Duration = 0.15f;
	GetOptionalFloat(Ctx.Params, TEXT("duration"), Duration, OutError);
	UAudioMixerBlueprintLibrary::StartAnalyzingOutput(World, Submix);
	FPlatformProcess::Sleep(FMath::Clamp(Duration, 0.01f, 2.0f));
	TArray<float> Magnitudes;
	UAudioMixerBlueprintLibrary::GetMagnitudeForFrequencies(World, Frequencies, Magnitudes, Submix);
	UAudioMixerBlueprintLibrary::StopAnalyzingOutput(World, Submix);
	TArray<TSharedPtr<FJsonValue>> Mags;
	TArray<TSharedPtr<FJsonValue>> Spectrum;
	float Peak = 0.0f;
	for (float Magnitude : Magnitudes)
	{
		Mags.Add(MakeShared<FJsonValueNumber>(Magnitude));
		Peak = FMath::Max(Peak, Magnitude);
	}
	TArray<TSharedPtr<FJsonValue>> Freqs;
	for (int32 Index = 0; Index < Frequencies.Num(); ++Index)
	{
		const float Frequency = Frequencies[Index];
		Freqs.Add(MakeShared<FJsonValueNumber>(Frequency));
		TSharedPtr<FJsonObject> SpectrumItem = MakeShared<FJsonObject>();
		SpectrumItem->SetNumberField(TEXT("frequency"), Frequency);
		SpectrumItem->SetNumberField(TEXT("magnitude"), Magnitudes.IsValidIndex(Index) ? Magnitudes[Index] : 0.0f);
		Spectrum.Add(MakeShared<FJsonValueObject>(SpectrumItem));
	}
	TArray<TSharedPtr<FJsonValue>> PeakByChannel;
	PeakByChannel.Add(MakeShared<FJsonValueNumber>(Peak));
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("submix"), ObjectPathOrEmpty(Submix));
	OutData->SetArrayField(TEXT("frequencies"), Freqs);
	OutData->SetArrayField(TEXT("magnitudes"), Mags);
	OutData->SetArrayField(TEXT("spectrum"), Spectrum);
	OutData->SetArrayField(TEXT("peak_by_channel"), PeakByChannel);
	OutData->SetNumberField(TEXT("peak_envelope"), Peak);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioRuntimeProfile));
	double ExpectedMinPeak = 0.0;
	if (Ctx.Params->TryGetNumberField(TEXT("expected_min_peak"), ExpectedMinPeak) && Peak < ExpectedMinPeak)
	{
		OutError = TEXT("audio_peak_below_expected_min");
		return false;
	}
	return true;
}
