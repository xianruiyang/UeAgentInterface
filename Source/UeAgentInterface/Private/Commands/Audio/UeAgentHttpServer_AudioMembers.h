// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

// NOTE: This header is included inside `FUeAgentLegacyMechanics`'s class declaration.
// It must only contain member declarations.
#if !defined(UEAGENT_HTTP_SERVER_MEMBER_DECL)
#error "UeAgentHttpServer_AudioMembers.h must be included from UeAgentHttpServer.h inside FUeAgentLegacyMechanics."
#endif

	bool ExecuteAudioCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;

	bool CmdSoundWaveImport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundWaveReimport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundWaveGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundWaveValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundWavePreview(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;

	bool CmdSoundCueCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundCueGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundCueExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundCueValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundCueApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundCueOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundCueScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundCuePreview(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;

	bool CmdMetaSoundCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdMetaSoundGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdMetaSoundExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdMetaSoundValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdMetaSoundApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdMetaSoundCompile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdMetaSoundOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdMetaSoundScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdMetaSoundPreview(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;

	bool CmdSoundAttenuationCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundAttenuationGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundAttenuationExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundAttenuationValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundAttenuationApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;

	bool CmdSoundConcurrencyCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundConcurrencyGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundConcurrencyExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundConcurrencyValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundConcurrencyApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;

	bool CmdSoundClassCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundClassGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundClassExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundClassValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundClassApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundMixCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundMixExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundMixApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;

	bool CmdSoundSubmixCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundSubmixGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundSubmixExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundSubmixValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdSoundSubmixApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdAudioEffectPresetCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdAudioEffectPresetExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdAudioEffectPresetApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;

	bool CmdAudioComponentValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdAudioComponentRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdAudioComponentRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdAudioVolumeValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdAmbientSoundValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;

	bool CmdAudioRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdAudioRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdAudioSubmixMeterProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdAudioSubmixRecordStart(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdAudioSubmixRecordStop(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdAudioSubmixRecordCancel(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdAudioStopAllPreviewSounds(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
