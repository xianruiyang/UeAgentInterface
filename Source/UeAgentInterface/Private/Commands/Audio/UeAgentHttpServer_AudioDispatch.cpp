// Copyright Epic Games, Inc. All Rights Reserved.

#include "LegacyMechanics/UeAgentLegacyMechanics.h"

namespace
{
	using FAudioCommandHandler = bool (FUeAgentLegacyMechanics::*)(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&) const;

	struct FAudioCommandEntry
	{
		const TCHAR* Name;
		FAudioCommandHandler Handler;
	};
}

bool FUeAgentLegacyMechanics::ExecuteAudioCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	static const FAudioCommandEntry Entries[] = {
		{ TEXT("sound_wave_import"), &FUeAgentLegacyMechanics::CmdSoundWaveImport },
		{ TEXT("sound_wave_reimport"), &FUeAgentLegacyMechanics::CmdSoundWaveReimport },
		{ TEXT("sound_wave_get_info"), &FUeAgentLegacyMechanics::CmdSoundWaveGetInfo },
		{ TEXT("sound_wave_validate_setup"), &FUeAgentLegacyMechanics::CmdSoundWaveValidateSetup },
		{ TEXT("sound_wave_preview"), &FUeAgentLegacyMechanics::CmdSoundWavePreview },
		{ TEXT("sound_cue_create"), &FUeAgentLegacyMechanics::CmdSoundCueCreate },
		{ TEXT("sound_cue_get_info"), &FUeAgentLegacyMechanics::CmdSoundCueGetInfo },
		{ TEXT("sound_cue_export_folder"), &FUeAgentLegacyMechanics::CmdSoundCueExportFolder },
		{ TEXT("sound_cue_validate_folder"), &FUeAgentLegacyMechanics::CmdSoundCueValidateFolder },
		{ TEXT("sound_cue_apply_folder"), &FUeAgentLegacyMechanics::CmdSoundCueApplyFolder },
		{ TEXT("sound_cue_open_editor"), &FUeAgentLegacyMechanics::CmdSoundCueOpenEditor },
		{ TEXT("sound_cue_screenshot"), &FUeAgentLegacyMechanics::CmdSoundCueScreenshot },
		{ TEXT("sound_cue_preview"), &FUeAgentLegacyMechanics::CmdSoundCuePreview },
		{ TEXT("metasound_create"), &FUeAgentLegacyMechanics::CmdMetaSoundCreate },
		{ TEXT("metasound_get_info"), &FUeAgentLegacyMechanics::CmdMetaSoundGetInfo },
		{ TEXT("metasound_export_folder"), &FUeAgentLegacyMechanics::CmdMetaSoundExportFolder },
		{ TEXT("metasound_validate_folder"), &FUeAgentLegacyMechanics::CmdMetaSoundValidateFolder },
		{ TEXT("metasound_apply_folder"), &FUeAgentLegacyMechanics::CmdMetaSoundApplyFolder },
		{ TEXT("metasound_compile"), &FUeAgentLegacyMechanics::CmdMetaSoundCompile },
		{ TEXT("metasound_open_editor"), &FUeAgentLegacyMechanics::CmdMetaSoundOpenEditor },
		{ TEXT("metasound_screenshot"), &FUeAgentLegacyMechanics::CmdMetaSoundScreenshot },
		{ TEXT("metasound_preview"), &FUeAgentLegacyMechanics::CmdMetaSoundPreview },
		{ TEXT("sound_attenuation_create"), &FUeAgentLegacyMechanics::CmdSoundAttenuationCreate },
		{ TEXT("sound_attenuation_get_info"), &FUeAgentLegacyMechanics::CmdSoundAttenuationGetInfo },
		{ TEXT("sound_attenuation_export_json"), &FUeAgentLegacyMechanics::CmdSoundAttenuationExportJson },
		{ TEXT("sound_attenuation_validate_json"), &FUeAgentLegacyMechanics::CmdSoundAttenuationValidateJson },
		{ TEXT("sound_attenuation_apply_json"), &FUeAgentLegacyMechanics::CmdSoundAttenuationApplyJson },
		{ TEXT("sound_concurrency_create"), &FUeAgentLegacyMechanics::CmdSoundConcurrencyCreate },
		{ TEXT("sound_concurrency_get_info"), &FUeAgentLegacyMechanics::CmdSoundConcurrencyGetInfo },
		{ TEXT("sound_concurrency_export_json"), &FUeAgentLegacyMechanics::CmdSoundConcurrencyExportJson },
		{ TEXT("sound_concurrency_validate_json"), &FUeAgentLegacyMechanics::CmdSoundConcurrencyValidateJson },
		{ TEXT("sound_concurrency_apply_json"), &FUeAgentLegacyMechanics::CmdSoundConcurrencyApplyJson },
		{ TEXT("sound_class_create"), &FUeAgentLegacyMechanics::CmdSoundClassCreate },
		{ TEXT("sound_class_get_info"), &FUeAgentLegacyMechanics::CmdSoundClassGetInfo },
		{ TEXT("sound_class_export_json"), &FUeAgentLegacyMechanics::CmdSoundClassExportJson },
		{ TEXT("sound_class_validate_json"), &FUeAgentLegacyMechanics::CmdSoundClassValidateJson },
		{ TEXT("sound_class_apply_json"), &FUeAgentLegacyMechanics::CmdSoundClassApplyJson },
		{ TEXT("sound_mix_create"), &FUeAgentLegacyMechanics::CmdSoundMixCreate },
		{ TEXT("sound_mix_export_json"), &FUeAgentLegacyMechanics::CmdSoundMixExportJson },
		{ TEXT("sound_mix_apply_json"), &FUeAgentLegacyMechanics::CmdSoundMixApplyJson },
		{ TEXT("sound_submix_create"), &FUeAgentLegacyMechanics::CmdSoundSubmixCreate },
		{ TEXT("sound_submix_get_info"), &FUeAgentLegacyMechanics::CmdSoundSubmixGetInfo },
		{ TEXT("sound_submix_export_folder"), &FUeAgentLegacyMechanics::CmdSoundSubmixExportFolder },
		{ TEXT("sound_submix_validate_folder"), &FUeAgentLegacyMechanics::CmdSoundSubmixValidateFolder },
		{ TEXT("sound_submix_apply_folder"), &FUeAgentLegacyMechanics::CmdSoundSubmixApplyFolder },
		{ TEXT("audio_effect_preset_create"), &FUeAgentLegacyMechanics::CmdAudioEffectPresetCreate },
		{ TEXT("audio_effect_preset_export_json"), &FUeAgentLegacyMechanics::CmdAudioEffectPresetExportJson },
		{ TEXT("audio_effect_preset_apply_json"), &FUeAgentLegacyMechanics::CmdAudioEffectPresetApplyJson },
		{ TEXT("audio_component_validate_setup"), &FUeAgentLegacyMechanics::CmdAudioComponentValidateSetup },
		{ TEXT("audio_component_runtime_snapshot"), &FUeAgentLegacyMechanics::CmdAudioComponentRuntimeSnapshot },
		{ TEXT("audio_component_runtime_probe"), &FUeAgentLegacyMechanics::CmdAudioComponentRuntimeProbe },
		{ TEXT("audio_volume_validate_setup"), &FUeAgentLegacyMechanics::CmdAudioVolumeValidateSetup },
		{ TEXT("ambient_sound_validate_setup"), &FUeAgentLegacyMechanics::CmdAmbientSoundValidateSetup },
		{ TEXT("audio_runtime_snapshot"), &FUeAgentLegacyMechanics::CmdAudioRuntimeSnapshot },
		{ TEXT("audio_runtime_probe"), &FUeAgentLegacyMechanics::CmdAudioRuntimeProbe },
		{ TEXT("audio_submix_meter_probe"), &FUeAgentLegacyMechanics::CmdAudioSubmixMeterProbe },
		{ TEXT("audio_stop_all_preview_sounds"), &FUeAgentLegacyMechanics::CmdAudioStopAllPreviewSounds },
	};

	for (const FAudioCommandEntry& Entry : Entries)
	{
		if (CommandLower == Entry.Name)
		{
			return (this->*Entry.Handler)(Ctx, OutData, OutError);
		}
	}
	OutError = TEXT("unknown_audio_command");
	return false;
}
