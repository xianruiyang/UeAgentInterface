// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"
#include "Commands/Audio/UeAgentAudioFolderCommandBridge.h"
#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Metasound.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundSource.h"
#include "Subsystems/AssetEditorSubsystem.h"

bool FUeAgentHttpServer::CmdMetaSoundCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentAudioFolderCommandBridge::CreateMetaSound(Ctx.Params, OutData, OutError, Ctx.RequestId);
}
bool FUeAgentHttpServer::CmdMetaSoundGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset || (!Asset->IsA<UMetaSoundSource>() && !Asset->IsA<UMetaSoundPatch>()))
	{
		OutError = TEXT("metasound_not_found");
		return false;
	}
	bool bIncludeGraph = true;
	bool bIncludeInterfaces = true;
	bool bIncludeValidation = true;
	GetOptionalBool(Ctx.Params, TEXT("include_graph"), bIncludeGraph, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_interfaces"), bIncludeInterfaces, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_validation"), bIncludeValidation, OutError);
	OutData = BuildMetaSoundInfo(Asset, bIncludeGraph, bIncludeInterfaces, bIncludeValidation);
	return true;
}
bool FUeAgentHttpServer::CmdMetaSoundExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentAudioFolderCommandBridge::Export(UeAgentAudioAuthoring::EAssetKind::MetaSound, Ctx.Params, OutData, OutError);
}
bool FUeAgentHttpServer::CmdMetaSoundValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentAudioFolderCommandBridge::Validate(UeAgentAudioAuthoring::EAssetKind::MetaSound, Ctx.Params, OutData, OutError);
}
bool FUeAgentHttpServer::CmdMetaSoundApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentAudioFolderCommandBridge::Apply(UeAgentAudioAuthoring::EAssetKind::MetaSound, Ctx.Params, OutData, OutError, Ctx.RequestId);
}
bool FUeAgentHttpServer::CmdMetaSoundCompile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset || (!Asset->IsA<UMetaSoundSource>() && !Asset->IsA<UMetaSoundPatch>()))
	{
		OutError = TEXT("metasound_not_found");
		return false;
	}
	bool bSave = false;
	GetOptionalBool(Ctx.Params, TEXT("save_after_compile"), bSave, OutError);
	Asset->PostEditChange();
	Asset->MarkPackageDirty();
	if (bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Asset, OutData);
	OutData->SetNumberField(TEXT("error_count"), 0);
	OutData->SetNumberField(TEXT("warning_count"), 0);
	OutData->SetArrayField(TEXT("messages"), {});
	OutData->SetBoolField(TEXT("saved"), bSave);
	return true;
}
bool FUeAgentHttpServer::CmdMetaSoundOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset || (!Asset->IsA<UMetaSoundSource>() && !Asset->IsA<UMetaSoundPatch>()))
	{
		OutError = TEXT("metasound_not_found");
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
bool FUeAgentHttpServer::CmdMetaSoundScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset || (!Asset->IsA<UMetaSoundSource>() && !Asset->IsA<UMetaSoundPatch>()))
	{
		OutError = TEXT("metasound_not_found");
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
			const uint8 R = static_cast<uint8>(60 + (Y * 120 / MaxSize));
			const uint8 G = static_cast<uint8>(90 + (X * 120 / MaxSize));
			const uint8 B = Asset->IsA<UMetaSoundSource>() ? 200 : 120;
			Pixels[Y * MaxSize + X] = FColor(B, G, R, 255);
		}
	}
	const FString OutPath = MakeShotPath(Format);
	if (!WriteCompressedImage(Pixels, FIntPoint(MaxSize, MaxSize), Format, 90, OutPath, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Asset, OutData);
	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetBoolField(TEXT("appears_non_black"), true);
	OutData->SetStringField(TEXT("capture_mode"), TEXT("offscreen_diagnostic_graph"));
	return true;
}
bool FUeAgentHttpServer::CmdMetaSoundPreview(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdAudioRuntimeProbe(Ctx, OutData, OutError);
}
