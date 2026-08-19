// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"
#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "EditorFramework/AssetImportData.h"
#include "EditorReimportHandler.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundWave.h"

bool FUeAgentHttpServer::CmdSoundWaveImport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString SourceFileInput;
	if (!GetRequiredStringAny(Ctx, { TEXT("source_file"), TEXT("source_filename") }, SourceFileInput, OutError))
	{
		return false;
	}
	const FString SourceFile = ResolveFilePath(SourceFileInput);
	if (!FPaths::FileExists(SourceFile))
	{
		OutError = TEXT("source_file_not_found");
		return false;
	}
	FString DestinationPath;
	if (!GetRequiredString(Ctx, TEXT("destination_path"), DestinationPath, OutError))
	{
		return false;
	}
	FString DestinationName;
	Ctx.Params->TryGetStringField(TEXT("destination_name"), DestinationName);
	bool bReplaceExisting = false;
	bool bSave = true;
	if (!GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError)
		|| !GetOptionalBool(Ctx.Params, TEXT("save_after_import"), bSave, OutError))
	{
		return false;
	}
	USoundWave* Wave = nullptr;
	if (!ImportSoundWaveFromFile(SourceFile, DestinationPath, DestinationName, bReplaceExisting, Wave, OutError))
	{
		return false;
	}
	if (bSave && !SavePackagesForObjects({ Wave }, OutError))
	{
		return false;
	}
	OutData = BuildSoundWaveInfo(Wave, false, true, true);
	OutData->SetStringField(TEXT("source_file"), SourceFile);
	OutData->SetStringField(TEXT("destination_path"), DestinationPath);
	OutData->SetStringField(TEXT("destination_name"), FPackageName::GetLongPackageAssetName(AssetPathOrEmpty(Wave)));
	OutData->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
	OutData->SetBoolField(TEXT("save_after_import"), bSave);
	OutData->SetArrayField(TEXT("warnings"), {});
	return true;
}
bool FUeAgentHttpServer::CmdSoundWaveReimport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	bool bAllowReimport = false;
	if (!GetOptionalBool(Ctx.Params, TEXT("allow_reimport"), bAllowReimport, OutError))
	{
		return false;
	}
	if (!bAllowReimport)
	{
		OutError = TEXT("allow_reimport_required");
		return false;
	}
	USoundWave* Wave = Cast<USoundWave>(LoadAssetObject(AssetPath));
	if (!Wave)
	{
		OutError = TEXT("sound_wave_not_found");
		return false;
	}
	const float DurationBefore = Wave->GetDuration();
	FString SourceFileInput;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetStringField(TEXT("source_file"), SourceFileInput) && !SourceFileInput.TrimStartAndEnd().IsEmpty())
	{
		const FString SourceFile = ResolveFilePath(SourceFileInput);
		if (!FPaths::FileExists(SourceFile))
		{
			OutError = TEXT("source_file_not_found");
			return false;
		}
		if (Wave->AssetImportData)
		{
			Wave->AssetImportData->Update(SourceFile);
		}
	}
	bool bSave = true;
	if (!GetOptionalBool(Ctx.Params, TEXT("save_after_reimport"), bSave, OutError))
	{
		return false;
	}
	if (!FReimportManager::Instance()->Reimport(Wave, true, true))
	{
		OutError = TEXT("sound_wave_reimport_failed");
		return false;
	}
	Wave->PostEditChange();
	Wave->MarkPackageDirty();
	if (bSave && !SavePackagesForObjects({ Wave }, OutError))
	{
		return false;
	}
	OutData = BuildSoundWaveInfo(Wave, false, true, true);
	OutData->SetBoolField(TEXT("reimported"), true);
	OutData->SetNumberField(TEXT("duration_before"), DurationBefore);
	OutData->SetNumberField(TEXT("duration_after"), Wave->GetDuration());
	OutData->SetArrayField(TEXT("warnings"), {});
	return true;
}
bool FUeAgentHttpServer::CmdSoundWaveGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USoundWave* Wave = Cast<USoundWave>(LoadAssetObject(AssetPath));
	if (!Wave)
	{
		OutError = TEXT("sound_wave_not_found");
		return false;
	}
	bool bIncludeProperties = false;
	bool bIncludeCuePoints = false;
	bool bIncludeAnalysis = false;
	if (!GetOptionalBool(Ctx.Params, TEXT("include_properties"), bIncludeProperties, OutError)
		|| !GetOptionalBool(Ctx.Params, TEXT("include_cue_points"), bIncludeCuePoints, OutError)
		|| !GetOptionalBool(Ctx.Params, TEXT("include_analysis"), bIncludeAnalysis, OutError))
	{
		return false;
	}
	OutData = BuildSoundWaveInfo(Wave, bIncludeProperties, bIncludeCuePoints, bIncludeAnalysis);
	return true;
}
bool FUeAgentHttpServer::CmdSoundWaveValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USoundWave* Wave = Cast<USoundWave>(LoadAssetObject(AssetPath));
	if (!Wave)
	{
		OutError = TEXT("sound_wave_not_found");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	FString Expected;
	if (Ctx.Params->TryGetStringField(TEXT("expected_sound_class"), Expected) && !Expected.IsEmpty() && ObjectPathOrEmpty(Wave->GetSoundClass()) != ToObjectPath(Expected))
	{
		AddIssue(Issues, TEXT("sound_class_mismatch"), TEXT("error"), TEXT("SoundWave sound class does not match expected_sound_class."));
	}
	if (Ctx.Params->TryGetStringField(TEXT("expected_attenuation"), Expected) && !Expected.IsEmpty() && ObjectPathOrEmpty(Wave->AttenuationSettings) != ToObjectPath(Expected))
	{
		AddIssue(Issues, TEXT("attenuation_mismatch"), TEXT("error"), TEXT("SoundWave attenuation does not match expected_attenuation."));
	}
	TArray<FString> ExpectedConcurrency;
	if (ReadStringArray(Ctx.Params, TEXT("expected_concurrency"), ExpectedConcurrency))
	{
		TSet<FString> Actual;
		for (USoundConcurrency* ConcurrencyAsset : Wave->ConcurrencySet)
		{
			Actual.Add(ToObjectPath(ObjectPathOrEmpty(ConcurrencyAsset)));
		}
		for (const FString& ExpectedPath : ExpectedConcurrency)
		{
			if (!Actual.Contains(ToObjectPath(ExpectedPath)))
			{
				AddIssue(Issues, TEXT("concurrency_missing"), TEXT("error"), FString::Printf(TEXT("Missing concurrency asset %s."), *ExpectedPath));
			}
		}
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Wave, OutData);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("failed"));
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetObjectField(TEXT("readonly_info"), BuildSoundWaveInfo(Wave, false, true, true));
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundWaveProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundWavePreview(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FUeAgentRequestContext RuntimeCtx = Ctx;
	return CmdAudioRuntimeProbe(RuntimeCtx, OutData, OutError);
}
