#include "Core/AssetPreview/UeAgentAssetPreviewState.h"

#include "Components/AudioComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Sound/SoundBase.h"

namespace UE::UAI::AssetPreview
{
	namespace
	{
		UAudioComponent* CurrentAudio(UObject* Asset)
		{
			auto* Component = GEditor ? GEditor->GetPreviewAudioComponent() : nullptr;
			return Component && Component->Sound == Asset ? Component : nullptr;
		}
	}

	bool IsAudioPreview(const UObject* Asset)
	{
		return Asset && Asset->IsA<USoundBase>();
	}

	bool ReadAudioState(UObject* Asset, FUeAgentAssetPreviewAudio& Out)
	{
		if (auto* Component = CurrentAudio(Asset))
		{
			Out.Volume = Component->VolumeMultiplier;
			Out.Pitch = Component->PitchMultiplier;
			return true;
		}
		return false;
	}

	bool ApplyAudioState(UObject* Asset, const FUeAgentAssetPreviewAudio& Value,
		const TSet<FName>& Fields, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!IsAudioPreview(Asset)) { OutError = PreviewFailure(TEXT("audio_preview_asset_unavailable"), Asset ? TEXT("The preview asset is not a sound asset.") : TEXT("The sound preview asset is unavailable."), Asset); return false; }
		if (!FMath::IsFinite(Value.Volume) || Value.Volume < 0
			|| !FMath::IsFinite(Value.Pitch) || Value.Pitch <= 0)
		{
			const bool bVolumeInvalid = !FMath::IsFinite(Value.Volume) || Value.Volume < 0;
			OutError = PreviewValueFailure(TEXT("audio_preview_parameters_invalid"), bVolumeInvalid ? TEXT("Audio preview volume must be finite and non-negative.") : TEXT("Audio preview pitch must be finite and positive."),
				bVolumeInvalid ? TEXT("Volume") : TEXT("Pitch"), MakeShared<FJsonValueString>(bVolumeInvalid ? TEXT("finite number >= 0") : TEXT("finite number > 0")),
				PreviewNumber(bVolumeInvalid ? Value.Volume : Value.Pitch), Asset); return false;
		}
		// Without this asset occupying the native component, configuration remains
		// in its workspace preview state until an explicit play action.
		if (auto* Component = CurrentAudio(Asset))
		{
			if (Fields.Contains(TEXT("Volume"))) Component->SetVolumeMultiplier(Value.Volume);
			if (Fields.Contains(TEXT("Pitch"))) Component->SetPitchMultiplier(Value.Pitch);
		}
		return true;
	}

	bool ExecuteAudioAction(UObject* Asset, const FString& Action,
		TSharedPtr<FJsonObject>& OutDetails, UE::UAI::Contract::FDiagnostic& OutError)
	{
		auto* Sound = Cast<USoundBase>(Asset);
		if (!Sound || !GEditor) { OutError = PreviewFailure(TEXT("audio_preview_asset_unavailable"), !Sound ? TEXT("The preview target is not an available sound asset.") : TEXT("The editor audio preview service is unavailable."), Asset); return false; }
		if (Action != TEXT("status") && Action != TEXT("play") && Action != TEXT("pause") && Action != TEXT("stop"))
		{ OutError = PreviewValueFailure(TEXT("audio_preview_action_unsupported"), TEXT("The action is not supported by sound preview."), TEXT("action"), MakeShared<FJsonValueString>(TEXT("status, play, pause or stop")), MakeShared<FJsonValueString>(Action), Asset); OutError.SourceKind = TEXT("command_input"); return false; }
		auto* Component = CurrentAudio(Asset);
		if (Action == TEXT("play"))
		{
			auto* Occupant = GEditor->GetPreviewAudioComponent();
			if (Occupant && Occupant->Sound != Asset
				&& (Occupant->IsPlaying() || Occupant->GetPlayState() == EAudioComponentPlayState::Paused))
			{
				OutError = PreviewFailure(TEXT("audio_preview_occupied"), TEXT("The shared audio preview device is playing or paused on a different sound."), Asset);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(Sound->GetName()); OutError.ActualValue = MakeShared<FJsonValueString>(GetNameSafe(Occupant->Sound));
				OutError.InternalContext = GetPathNameSafe(Occupant->Sound); return false;
			}
			if (Component && Component->GetPlayState() == EAudioComponentPlayState::Paused)
				Component->SetPaused(false);
			else if (!Component || !Component->IsPlaying()) Component = GEditor->PlayPreviewSound(Sound);
			if (!Component) { OutError = PreviewFailure(TEXT("audio_preview_device_unavailable"), TEXT("The editor could not create or acquire an audio component to play this sound preview."), Asset); return false; }
			const auto State = FindLocalState(Asset->GetPathName());
			const FUeAgentAssetPreviewAudio Settings = State ? State->Audio : FUeAgentAssetPreviewAudio();
			if (!ApplyAudioState(Asset, Settings, {TEXT("Volume"), TEXT("Pitch")}, OutError)) return false;
		}
		else if (Component && Action == TEXT("pause")) Component->SetPaused(true);
		else if (Component && Action == TEXT("stop")) Component->Stop();

		OutDetails = MakeShared<FJsonObject>();
		OutDetails->SetStringField(TEXT("target"), TEXT("main"));
		OutDetails->SetStringField(TEXT("action"), Action);
		OutDetails->SetStringField(TEXT("scope"), TEXT("shared_audio_preview"));
		OutDetails->SetBoolField(TEXT("owns_preview"), Component != nullptr);
		OutDetails->SetBoolField(TEXT("playing"), Component && Component->GetPlayState() == EAudioComponentPlayState::Playing);
		OutDetails->SetBoolField(TEXT("paused"), Component && Component->GetPlayState() == EAudioComponentPlayState::Paused);
		OutDetails->SetBoolField(TEXT("position_available"), false);
		if (Component)
		{
			OutDetails->SetNumberField(TEXT("volume"), Component->VolumeMultiplier);
			OutDetails->SetNumberField(TEXT("pitch"), Component->PitchMultiplier);
		}
		return true;
	}
}
