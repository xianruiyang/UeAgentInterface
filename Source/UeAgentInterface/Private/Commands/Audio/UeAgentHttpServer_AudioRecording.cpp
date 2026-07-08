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
#include "HAL/FileManager.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundWave.h"

bool FUeAgentHttpServer::CmdAudioSubmixRecordStart(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	FString SessionId;
	if (!JsonTryGetStringAny(Ctx.Params, { TEXT("session_id"), TEXT("recording_id") }, SessionId))
	{
		SessionId.Empty();
	}
	if (SessionId.IsEmpty())
	{
		SessionId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}
	FString SubmixPath;
	Ctx.Params->TryGetStringField(TEXT("submix"), SubmixPath);
	USoundSubmix* Submix = SubmixPath.IsEmpty() ? nullptr : Cast<USoundSubmix>(LoadAssetObject(SubmixPath));
	float ExpectedDuration = 5.0f;
	GetOptionalFloat(Ctx.Params, TEXT("expected_duration"), ExpectedDuration, OutError);
	UAudioMixerBlueprintLibrary::StartRecordingOutput(World, FMath::Max(0.01f, ExpectedDuration), Submix);
	FAudioRecordSession Session;
	Session.Submix = Submix;
	Session.StartTimeSeconds = FPlatformTime::Seconds();
	Ctx.Params->TryGetStringField(TEXT("name"), Session.Name);
	Ctx.Params->TryGetStringField(TEXT("path"), Session.Path);
	GRecordSessions.Add(SessionId, Session);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("session_id"), SessionId);
	OutData->SetStringField(TEXT("recording_id"), SessionId);
	OutData->SetStringField(TEXT("submix"), ObjectPathOrEmpty(Submix));
	OutData->SetBoolField(TEXT("started"), true);
	return true;
}
bool FUeAgentHttpServer::CmdAudioSubmixRecordStop(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	FString SessionId;
	if (!JsonTryGetStringAny(Ctx.Params, { TEXT("session_id"), TEXT("recording_id") }, SessionId) || SessionId.IsEmpty())
	{
		OutError = TEXT("missing_session_id");
		return false;
	}
	FAudioRecordSession Session;
	if (!GRecordSessions.RemoveAndCopyValue(SessionId, Session))
	{
		OutError = TEXT("audio_record_session_not_found");
		return false;
	}
	FString ExportTypeText = TEXT("wav_file");
	Ctx.Params->TryGetStringField(TEXT("export_type"), ExportTypeText);
	const bool bSoundWave = ExportTypeText.Equals(TEXT("sound_wave"), ESearchCase::IgnoreCase);
	FString Name = Session.Name.IsEmpty() ? FString::Printf(TEXT("UAI_SubmixRecord_%s"), *SessionId.Left(8)) : Session.Name;
	Ctx.Params->TryGetStringField(TEXT("name"), Name);
	FString Path = ResolveFilePath(Session.Path);
	FString RequestedPath;
	if (Ctx.Params->TryGetStringField(TEXT("path"), RequestedPath) && !RequestedPath.IsEmpty())
	{
		Path = ResolveFilePath(RequestedPath);
	}
	FString OutputPath;
	if (!bSoundWave && Ctx.Params->TryGetStringField(TEXT("output_path"), OutputPath) && !OutputPath.IsEmpty())
	{
		const FString ResolvedOutputPath = ResolveFilePath(OutputPath);
		if (FPaths::GetExtension(ResolvedOutputPath).Equals(TEXT("wav"), ESearchCase::IgnoreCase))
		{
			Path = FPaths::GetPath(ResolvedOutputPath);
			Name = FPaths::GetBaseFilename(ResolvedOutputPath);
		}
		else
		{
			Path = ResolvedOutputPath;
		}
	}
	FString AssetPath;
	if (bSoundWave && Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty())
	{
		const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
		Path = FPackageName::GetLongPackagePath(NormalizedAssetPath);
		Name = FPackageName::GetLongPackageAssetName(NormalizedAssetPath);
	}
	if (Path.IsEmpty())
	{
		Path = FPaths::ProjectSavedDir();
	}
	if (!bSoundWave)
	{
		IFileManager::Get().MakeDirectory(*Path, true);
	}
	USoundWave* RecordedWave = UAudioMixerBlueprintLibrary::StopRecordingOutput(World, bSoundWave ? EAudioRecordingExportType::SoundWave : EAudioRecordingExportType::WavFile, Name, Path, Session.Submix.Get());
	const double Elapsed = FPlatformTime::Seconds() - Session.StartTimeSeconds;
	const FString ExportedFile = bSoundWave ? FString() : FPaths::Combine(Path, Name + TEXT(".wav"));
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("session_id"), SessionId);
	OutData->SetStringField(TEXT("recording_id"), SessionId);
	OutData->SetStringField(TEXT("name"), Name);
	OutData->SetStringField(TEXT("path"), Path);
	OutData->SetStringField(TEXT("export_type"), bSoundWave ? TEXT("sound_wave") : TEXT("wav_file"));
	OutData->SetNumberField(TEXT("elapsed"), Elapsed);
	OutData->SetNumberField(TEXT("duration"), Elapsed);
	OutData->SetStringField(TEXT("exported_file"), ExportedFile);
	OutData->SetBoolField(TEXT("exported_file_exists"), !ExportedFile.IsEmpty() && FPaths::FileExists(ExportedFile));
	OutData->SetStringField(TEXT("asset_path"), ObjectPathOrEmpty(RecordedWave));
	OutData->SetStringField(TEXT("sound_wave"), ObjectPathOrEmpty(RecordedWave));
	OutData->SetBoolField(TEXT("stopped"), true);
	return true;
}
bool FUeAgentHttpServer::CmdAudioSubmixRecordCancel(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	FString SessionId;
	if (!JsonTryGetStringAny(Ctx.Params, { TEXT("session_id"), TEXT("recording_id") }, SessionId) || SessionId.IsEmpty())
	{
		OutError = TEXT("missing_session_id");
		return false;
	}
	FAudioRecordSession Session;
	if (!GRecordSessions.RemoveAndCopyValue(SessionId, Session))
	{
		OutError = TEXT("audio_record_session_not_found");
		return false;
	}

	const FString Name = FString::Printf(TEXT("UAI_SubmixRecordCancel_%s"), *SessionId.Left(8));
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UeAgentInterface"), TEXT("AudioRecordCancel")));
	IFileManager::Get().MakeDirectory(*Path, true);

	TArray<FString> BeforeFiles;
	const FString WavPattern = FPaths::Combine(Path, Name + TEXT("*.wav"));
	IFileManager::Get().FindFiles(BeforeFiles, *WavPattern, true, false);
	TSet<FString> BeforeSet(BeforeFiles);

	UAudioMixerBlueprintLibrary::StopRecordingOutput(World, EAudioRecordingExportType::WavFile, Name, Path, Session.Submix.Get());

	TArray<FString> AfterFiles;
	IFileManager::Get().FindFiles(AfterFiles, *WavPattern, true, false);
	int32 DeletedFileCount = 0;
	for (const FString& FileName : AfterFiles)
	{
		if (BeforeSet.Contains(FileName))
		{
			continue;
		}
		const FString FullPath = FPaths::Combine(Path, FileName);
		if (IFileManager::Get().Delete(*FullPath, false, true, true))
		{
			++DeletedFileCount;
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("session_id"), SessionId);
	OutData->SetStringField(TEXT("recording_id"), SessionId);
	OutData->SetStringField(TEXT("path"), Path);
	OutData->SetNumberField(TEXT("elapsed"), FPlatformTime::Seconds() - Session.StartTimeSeconds);
	OutData->SetNumberField(TEXT("deleted_file_count"), DeletedFileCount);
	OutData->SetBoolField(TEXT("stopped"), true);
	OutData->SetBoolField(TEXT("canceled"), true);
	return true;
}
bool FUeAgentHttpServer::CmdAudioStopAllPreviewSounds(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	int32 Stopped = 0;
	for (TWeakObjectPtr<UAudioComponent>& WeakComponent : GPreviewComponents)
	{
		if (UAudioComponent* Component = WeakComponent.Get())
		{
			if (Component->IsPlaying())
			{
				Component->Stop();
				++Stopped;
			}
			Component->DestroyComponent();
		}
	}
	GPreviewComponents.Reset();
	OutData = MakeShared<FJsonObject>();
	OutData->SetNumberField(TEXT("stopped_count"), Stopped);
	OutData->SetBoolField(TEXT("cleanup_complete"), true);
	return true;
}
