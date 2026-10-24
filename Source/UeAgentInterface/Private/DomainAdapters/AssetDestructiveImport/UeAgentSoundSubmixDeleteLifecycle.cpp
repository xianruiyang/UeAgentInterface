#include "DomainAdapters/AssetDestructiveImport/UeAgentSoundSubmixDeleteLifecycle.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "AudioMixerSubmix.h"
#include "AudioThread.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Sound/SoundSubmix.h"

namespace UE::UAI::DomainAdapters::AssetDestructiveImport
{
	namespace
	{
		UE::UAI::Contract::FDiagnostic SubmixFact(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Stage, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, Stage);
			Diagnostic.TargetPath = Target;
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = Path;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		void AudioFence()
		{
			FAudioCommandFence Fence; Fence.BeginFence(); Fence.Wait();
		}
		struct FObservation
		{
			FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
			bool bRegistered = false;
			bool bRequired = false;
			bool bSources = false;
			~FObservation() { FPlatformProcess::ReturnSynchEventToPool(Done); }
		};
		bool Observe(Audio::FMixerDevice& Mixer, const USoundSubmixBase& Asset, bool bInspectUses,
			TSharedRef<FObservation, ESPMode::ThreadSafe>& Result)
		{
			FAudioThread::RunCommandOnAudioThread([&Mixer, AssetPtr = &Asset, Result, bInspectUses]()
			{
				const auto Instance = Mixer.GetSubmixInstance(AssetPtr).Pin();
				Result->bRegistered = Instance.IsValid();
				Result->bRequired = Instance.IsValid() && (Instance == Mixer.GetMainSubmix().Pin()
					|| Instance == Mixer.GetBaseDefaultSubmix().Pin() || Instance == Mixer.GetReverbSubmix().Pin()
					|| Instance == Mixer.GetEQSubmix().Pin());
				if (bInspectUses && Instance.IsValid())
				{
					Mixer.AudioRenderThreadCommand([Instance, Result]()
					{
						Result->bSources = Instance->GetNumSourceVoices() != 0;
						Result->Done->Trigger();
					});
				}
				else Result->Done->Trigger();
			});
			AudioFence(); // Dispatch the game-thread audio command batch before waiting.
			return Result->Done->Wait(5000);
		}
	}

	bool FSoundSubmixDeleteLifecycle::Prepare(UObject& Object, UE::UAI::Contract::FDiagnostic& Error)
	{
		auto* Submix = Cast<USoundSubmix>(&Object);
		if (!Submix) return true;
		FEntry Entry; Entry.Path = Object.GetPathName(); Entry.bRooted = Object.IsRooted();
		if (auto* Manager = FAudioDeviceManager::Get())
		{
			bool bAllowed = true;
			Manager->IterateOverAllDevices([&](Audio::FDeviceId Id, FAudioDevice* Device)
			{
				if (!bAllowed || !Device || !Device->bAudioMixerModuleLoaded) return;
				auto Observation = MakeShared<FObservation, ESPMode::ThreadSafe>();
				if (!Observe(*static_cast<Audio::FMixerDevice*>(Device), *Submix, true, Observation))
				{
					Error = SubmixFact(TEXT("sound_submix_delete_device_observation_failed"), TEXT("The audio device did not complete submix observation within five seconds."),
						Entry.Path, TEXT("delete_preflight"), TEXT("/audio_device/observation_completed"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Error.ActualEvidence = MakeShared<FJsonValueNumber>(static_cast<double>(Id));
					bAllowed = false; return;
				}
				if (Observation->bRequired || Observation->bSources)
				{
					Error = SubmixFact(Observation->bRequired ? TEXT("sound_submix_delete_required_device_use") : TEXT("sound_submix_delete_active_sources"),
						Observation->bRequired ? TEXT("The audio device requires this sound submix.") : TEXT("The sound submix has active source voices."),
						Entry.Path, TEXT("delete_preflight"), Observation->bRequired ? TEXT("/audio_device/required_submix") : TEXT("/audio_device/has_source_voices"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					Error.ActualEvidence = MakeShared<FJsonValueNumber>(static_cast<double>(Id));
					bAllowed = false; return;
				}
				if (Observation->bRegistered) Entry.Devices.Add(Manager->GetAudioDevice(Id));
			});
			if (!bAllowed) return false;
		}
		// An unexplained root is not ours to release.
		if (Entry.bRooted && Entry.Devices.IsEmpty())
		{
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("rooted"), true);
			Actual->SetNumberField(TEXT("registered_device_count"), 0);
			Error = SubmixFact(TEXT("sound_submix_delete_unowned_root"), TEXT("The submix is rooted without an owning registered audio device; its root cannot be released by this operation."),
				Entry.Path, TEXT("delete_preflight"), TEXT("/submix/root_ownership"), MakeShared<FJsonValueObject>(Actual),
				MakeShared<FJsonValueString>(TEXT("unrooted submix or captured audio-device ownership")));
			return false;
		}
		Entries.Add(MoveTemp(Entry));
		return true;
	}

	bool FSoundSubmixDeleteLifecycle::Release(const TArray<FString>& SelectedPaths, UE::UAI::Contract::FDiagnostic& Error)
	{
		for (FEntry& Entry : Entries)
		{
			if (!SelectedPaths.Contains(Entry.Path)) continue;
			Entry.bSelected = true;
			auto* Asset = FindObject<USoundSubmix>(nullptr, *Entry.Path);
			if (!Asset) continue; // Native deletion already collected an unregistered object.
			for (const auto& Device : Entry.Devices) Device.GetAudioDevice()->UnregisterSoundSubmix(Asset, true);
			AudioFence();
			for (const auto& Device : Entry.Devices)
			{
				auto* Mixer = static_cast<Audio::FMixerDevice*>(Device.GetAudioDevice());
				Mixer->FlushAudioRenderingCommands();
				auto Observation = MakeShared<FObservation, ESPMode::ThreadSafe>();
				const bool bObserved = Observe(*Mixer, *Asset, false, Observation);
				if (!bObserved || Observation->bRegistered)
				{
					Error = SubmixFact(TEXT("sound_submix_delete_unregister_failed"),
						bObserved ? TEXT("The audio device still registers the submix after unregistering it.") : TEXT("The audio device did not confirm submix unregistration within five seconds."),
						Entry.Path, TEXT("delete_release"), bObserved ? TEXT("/audio_device/submix_registered") : TEXT("/audio_device/observation_completed"),
						MakeShared<FJsonValueBoolean>(bObserved), MakeShared<FJsonValueBoolean>(!bObserved));
					Error.ActualEvidence = MakeShared<FJsonValueNumber>(static_cast<double>(Device.GetDeviceID()));
					return false;
				}
			}
			if (Entry.bRooted && !Entry.Devices.IsEmpty()) Asset->RemoveFromRoot();
		}
		return true;
	}

	bool FSoundSubmixDeleteLifecycle::Restore(UE::UAI::Contract::FDiagnostic& Error)
	{
		bool bRestored = true;
		for (const FEntry& Entry : Entries)
		{
			if (!Entry.bSelected) continue;
			auto* Asset = LoadObject<USoundSubmix>(nullptr, *Entry.Path);
			if (!Asset)
			{
				Error.Append(SubmixFact(TEXT("sound_submix_delete_restore_target_missing"), TEXT("The sound submix could not be loaded for restoration."),
					Entry.Path, TEXT("delete_restore"), TEXT("/submix/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				bRestored = false; continue;
			}
			// PostLoad can register on every device. Restore the captured device set,
			// including the initially unregistered case, before checking the result.
			if (auto* Manager = FAudioDeviceManager::Get())
			{
				Manager->IterateOverAllDevices([&](Audio::FDeviceId Id, FAudioDevice* Device)
				{
					if (Device && Device->bAudioMixerModuleLoaded && !Entry.Devices.ContainsByPredicate(
						[Id](const FAudioDeviceHandle& Handle) { return Handle.GetDeviceID() == Id; }))
						Device->UnregisterSoundSubmix(Asset, true);
				});
			}
			for (const auto& Device : Entry.Devices) Device.GetAudioDevice()->RegisterSoundSubmix(Asset, true);
			AudioFence();
			for (const auto& Device : Entry.Devices)
			{
				auto* Mixer = static_cast<Audio::FMixerDevice*>(Device.GetAudioDevice()); Mixer->FlushAudioRenderingCommands();
				auto Observation = MakeShared<FObservation, ESPMode::ThreadSafe>();
				const bool bObserved = Observe(*Mixer, *Asset, false, Observation);
				if (!bObserved || !Observation->bRegistered)
				{
					auto Diagnostic = SubmixFact(TEXT("sound_submix_delete_restore_registration_failed"),
						bObserved ? TEXT("The audio device does not register the submix after restoration.") : TEXT("The audio device did not confirm submix registration within five seconds."),
						Entry.Path, TEXT("delete_restore"), bObserved ? TEXT("/audio_device/submix_registered") : TEXT("/audio_device/observation_completed"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Diagnostic.ActualEvidence = MakeShared<FJsonValueNumber>(static_cast<double>(Device.GetDeviceID()));
					Error.Append(Diagnostic);
					bRestored = false;
				}
			}
			if (Entry.bRooted && !Asset->IsRooted())
			{
				Error.Append(SubmixFact(TEXT("sound_submix_delete_restore_root_failed"), TEXT("The restored sound submix has not regained its original root state."),
					Entry.Path, TEXT("delete_restore"), TEXT("/submix/rooted"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				bRestored = false;
			}
			if (!Entry.bRooted && Entry.Devices.IsEmpty()) Asset->RemoveFromRoot();
		}
		return bRestored;
	}
}
