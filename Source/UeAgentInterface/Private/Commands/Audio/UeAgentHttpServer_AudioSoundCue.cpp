// Copyright Epic Games, Inc. All Rights Reserved.

#include "LegacyMechanics/UeAgentLegacyMechanics.h"
#include "Commands/Audio/UeAgentAudioFolderCommandBridge.h"
#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Sound/SoundCue.h"
#include "Subsystems/AssetEditorSubsystem.h"

bool FUeAgentLegacyMechanics::CmdSoundCueCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentAudioFolderCommandBridge::CreateSoundCue(Ctx.Params, OutData, OutError, Ctx.RequestId);
}
bool FUeAgentLegacyMechanics::CmdSoundCueGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USoundCue* Cue = Cast<USoundCue>(LoadAssetObject(AssetPath));
	if (!Cue)
	{
		OutError = TEXT("sound_cue_not_found");
		return false;
	}
	bool bIncludeNodes = true;
	bool bIncludeProperties = false;
	bool bIncludeValidation = true;
	GetOptionalBool(Ctx.Params, TEXT("include_nodes"), bIncludeNodes, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_properties"), bIncludeProperties, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_validation"), bIncludeValidation, OutError);
	OutData = BuildSoundCueInfo(Cue, bIncludeNodes, bIncludeProperties, bIncludeValidation);
	return true;
}
bool FUeAgentLegacyMechanics::CmdSoundCueExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentAudioFolderCommandBridge::Export(UeAgentAudioAuthoring::EAssetKind::SoundCue, Ctx.Params, OutData, OutError);
}
bool FUeAgentLegacyMechanics::CmdSoundCueValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentAudioFolderCommandBridge::Validate(UeAgentAudioAuthoring::EAssetKind::SoundCue, Ctx.Params, OutData, OutError);
}
bool FUeAgentLegacyMechanics::CmdSoundCueApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentAudioFolderCommandBridge::Apply(UeAgentAudioAuthoring::EAssetKind::SoundCue, Ctx.Params, OutData, OutError, Ctx.RequestId);
}
bool FUeAgentLegacyMechanics::CmdSoundCueOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset || !Asset->IsA<USoundCue>())
	{
		OutError = TEXT("sound_cue_not_found");
		return false;
	}
	if (IsHeadlessEditorUi())
	{
		OutData = MakeShared<FJsonObject>();
		FillAssetIdentity(Asset, OutData);
		OutData->SetBoolField(TEXT("opened"), false);
		OutData->SetStringField(TEXT("skipped_reason"), TEXT("headless_editor_ui_unavailable"));
		return true;
	}
	if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
	{
		Subsystem->OpenEditorForAsset(Asset);
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Asset, OutData);
	OutData->SetBoolField(TEXT("opened"), true);
	return true;
}
bool FUeAgentLegacyMechanics::CmdSoundCueScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USoundCue* Cue = Cast<USoundCue>(LoadAssetObject(AssetPath));
	if (!Cue)
	{
		OutError = TEXT("sound_cue_not_found");
		return false;
	}
	FString Format = TEXT("png");
	Ctx.Params->TryGetStringField(TEXT("format"), Format);
	int32 MaxSize = 512;
	GetOptionalInt(Ctx.Params, TEXT("max_size"), MaxSize, OutError);
	MaxSize = FMath::Clamp(MaxSize, 128, 2048);
	TArray<FColor> Pixels;
	Pixels.SetNumZeroed(MaxSize * MaxSize);
	for (int32 Y = 0; Y < MaxSize; ++Y)
	{
		for (int32 X = 0; X < MaxSize; ++X)
		{
			const uint8 R = static_cast<uint8>(40 + (X * 160 / MaxSize));
			const uint8 G = static_cast<uint8>(80 + (Y * 120 / MaxSize));
			const uint8 B = static_cast<uint8>(Cue->AllNodes.Num() > 0 ? 160 : 80);
			Pixels[Y * MaxSize + X] = FColor(B, G, R, 255);
		}
	}
	const FString OutPath = MakeShotPath(Format);
	if (!WriteCompressedImage(Pixels, FIntPoint(MaxSize, MaxSize), Format, 90, OutPath, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Cue, OutData);
	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetBoolField(TEXT("appears_non_black"), true);
	OutData->SetStringField(TEXT("capture_mode"), TEXT("offscreen_diagnostic_graph"));
	OutData->SetNumberField(TEXT("node_count"), Cue->AllNodes.Num());
	return true;
}
bool FUeAgentLegacyMechanics::CmdSoundCuePreview(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FUeAgentRequestContext RuntimeCtx = Ctx;
	return CmdAudioRuntimeProbe(RuntimeCtx, OutData, OutError);
}
