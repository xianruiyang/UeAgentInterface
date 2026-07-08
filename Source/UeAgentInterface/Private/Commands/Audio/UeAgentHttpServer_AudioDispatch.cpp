// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"

namespace
{
	using FAudioCommandHandler = bool (FUeAgentHttpServer::*)(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&) const;

	struct FAudioCommandEntry
	{
		const TCHAR* Name;
		FAudioCommandHandler Handler;
	};
}

bool FUeAgentHttpServer::ExecuteAudioCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	static const FAudioCommandEntry Entries[] = {
		{ TEXT("sound_wave_import"), &FUeAgentHttpServer::CmdSoundWaveImport },
		{ TEXT("sound_wave_reimport"), &FUeAgentHttpServer::CmdSoundWaveReimport },
		{ TEXT("sound_wave_get_info"), &FUeAgentHttpServer::CmdSoundWaveGetInfo },
		{ TEXT("sound_wave_validate_setup"), &FUeAgentHttpServer::CmdSoundWaveValidateSetup },
		{ TEXT("sound_wave_preview"), &FUeAgentHttpServer::CmdSoundWavePreview },
		{ TEXT("sound_cue_create"), &FUeAgentHttpServer::CmdSoundCueCreate },
		{ TEXT("sound_cue_get_info"), &FUeAgentHttpServer::CmdSoundCueGetInfo },
		{ TEXT("sound_cue_export_folder"), &FUeAgentHttpServer::CmdSoundCueExportFolder },
		{ TEXT("sound_cue_validate_folder"), &FUeAgentHttpServer::CmdSoundCueValidateFolder },
		{ TEXT("sound_cue_apply_folder"), &FUeAgentHttpServer::CmdSoundCueApplyFolder },
		{ TEXT("sound_cue_open_editor"), &FUeAgentHttpServer::CmdSoundCueOpenEditor },
		{ TEXT("sound_cue_screenshot"), &FUeAgentHttpServer::CmdSoundCueScreenshot },
		{ TEXT("sound_cue_preview"), &FUeAgentHttpServer::CmdSoundCuePreview },
		{ TEXT("metasound_create"), &FUeAgentHttpServer::CmdMetaSoundCreate },
		{ TEXT("metasound_get_info"), &FUeAgentHttpServer::CmdMetaSoundGetInfo },
		{ TEXT("metasound_export_folder"), &FUeAgentHttpServer::CmdMetaSoundExportFolder },
		{ TEXT("metasound_validate_folder"), &FUeAgentHttpServer::CmdMetaSoundValidateFolder },
		{ TEXT("metasound_apply_folder"), &FUeAgentHttpServer::CmdMetaSoundApplyFolder },
		{ TEXT("metasound_compile"), &FUeAgentHttpServer::CmdMetaSoundCompile },
		{ TEXT("metasound_open_editor"), &FUeAgentHttpServer::CmdMetaSoundOpenEditor },
		{ TEXT("metasound_screenshot"), &FUeAgentHttpServer::CmdMetaSoundScreenshot },
		{ TEXT("metasound_preview"), &FUeAgentHttpServer::CmdMetaSoundPreview },
		{ TEXT("sound_attenuation_create"), &FUeAgentHttpServer::CmdSoundAttenuationCreate },
		{ TEXT("sound_attenuation_get_info"), &FUeAgentHttpServer::CmdSoundAttenuationGetInfo },
		{ TEXT("sound_attenuation_export_json"), &FUeAgentHttpServer::CmdSoundAttenuationExportJson },
		{ TEXT("sound_attenuation_validate_json"), &FUeAgentHttpServer::CmdSoundAttenuationValidateJson },
		{ TEXT("sound_attenuation_apply_json"), &FUeAgentHttpServer::CmdSoundAttenuationApplyJson },
		{ TEXT("sound_concurrency_create"), &FUeAgentHttpServer::CmdSoundConcurrencyCreate },
		{ TEXT("sound_concurrency_get_info"), &FUeAgentHttpServer::CmdSoundConcurrencyGetInfo },
		{ TEXT("sound_concurrency_export_json"), &FUeAgentHttpServer::CmdSoundConcurrencyExportJson },
		{ TEXT("sound_concurrency_validate_json"), &FUeAgentHttpServer::CmdSoundConcurrencyValidateJson },
		{ TEXT("sound_concurrency_apply_json"), &FUeAgentHttpServer::CmdSoundConcurrencyApplyJson },
		{ TEXT("sound_class_create"), &FUeAgentHttpServer::CmdSoundClassCreate },
		{ TEXT("sound_class_get_info"), &FUeAgentHttpServer::CmdSoundClassGetInfo },
		{ TEXT("sound_class_export_json"), &FUeAgentHttpServer::CmdSoundClassExportJson },
		{ TEXT("sound_class_validate_json"), &FUeAgentHttpServer::CmdSoundClassValidateJson },
		{ TEXT("sound_class_apply_json"), &FUeAgentHttpServer::CmdSoundClassApplyJson },
		{ TEXT("sound_mix_create"), &FUeAgentHttpServer::CmdSoundMixCreate },
		{ TEXT("sound_mix_export_json"), &FUeAgentHttpServer::CmdSoundMixExportJson },
		{ TEXT("sound_mix_apply_json"), &FUeAgentHttpServer::CmdSoundMixApplyJson },
		{ TEXT("sound_submix_create"), &FUeAgentHttpServer::CmdSoundSubmixCreate },
		{ TEXT("sound_submix_get_info"), &FUeAgentHttpServer::CmdSoundSubmixGetInfo },
		{ TEXT("sound_submix_export_folder"), &FUeAgentHttpServer::CmdSoundSubmixExportFolder },
		{ TEXT("sound_submix_validate_folder"), &FUeAgentHttpServer::CmdSoundSubmixValidateFolder },
		{ TEXT("sound_submix_apply_folder"), &FUeAgentHttpServer::CmdSoundSubmixApplyFolder },
		{ TEXT("audio_effect_preset_create"), &FUeAgentHttpServer::CmdAudioEffectPresetCreate },
		{ TEXT("audio_effect_preset_export_json"), &FUeAgentHttpServer::CmdAudioEffectPresetExportJson },
		{ TEXT("audio_effect_preset_apply_json"), &FUeAgentHttpServer::CmdAudioEffectPresetApplyJson },
		{ TEXT("audio_component_validate_setup"), &FUeAgentHttpServer::CmdAudioComponentValidateSetup },
		{ TEXT("audio_component_runtime_snapshot"), &FUeAgentHttpServer::CmdAudioComponentRuntimeSnapshot },
		{ TEXT("audio_component_runtime_probe"), &FUeAgentHttpServer::CmdAudioComponentRuntimeProbe },
		{ TEXT("audio_volume_validate_setup"), &FUeAgentHttpServer::CmdAudioVolumeValidateSetup },
		{ TEXT("ambient_sound_validate_setup"), &FUeAgentHttpServer::CmdAmbientSoundValidateSetup },
		{ TEXT("audio_runtime_snapshot"), &FUeAgentHttpServer::CmdAudioRuntimeSnapshot },
		{ TEXT("audio_runtime_probe"), &FUeAgentHttpServer::CmdAudioRuntimeProbe },
		{ TEXT("audio_submix_meter_probe"), &FUeAgentHttpServer::CmdAudioSubmixMeterProbe },
		{ TEXT("audio_submix_record_start"), &FUeAgentHttpServer::CmdAudioSubmixRecordStart },
		{ TEXT("audio_submix_record_stop"), &FUeAgentHttpServer::CmdAudioSubmixRecordStop },
		{ TEXT("audio_submix_record_cancel"), &FUeAgentHttpServer::CmdAudioSubmixRecordCancel },
		{ TEXT("audio_stop_all_preview_sounds"), &FUeAgentHttpServer::CmdAudioStopAllPreviewSounds },
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
