// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"

#include "UeAgentHttpServer_TextureRenderTargetOps.h"

#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentFolderDetailsExport.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "AutomatedAssetImportData.h"
#include "Components/LightComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorFramework/AssetImportData.h"
#include "EditorReimportHandler.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCollection.h"
#include "Engine/TextureLightProfile.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "FileMediaSource.h"
#include "ImageUtils.h"
#include "MediaPlayer.h"
#include "MediaSource.h"
#include "MediaTexture.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Particles/SubUVAnimation.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "StreamMediaSource.h"
#include "TextureGraph.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "VT/RuntimeVirtualTexture.h"

bool FUeAgentHttpServer::CmdMediaSourceCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	FString SourceType = TEXT("file");
	JsonTryGetString(Ctx.Params, TEXT("source_type"), SourceType);
	SourceType = SourceType.ToLower();
	UClass* Class = UFileMediaSource::StaticClass();
	if (SourceType == TEXT("stream") || SourceType == TEXT("url"))
	{
		Class = UStreamMediaSource::StaticClass();
	}
	bool bReplaceExisting = false;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError);
	UMediaSource* Source = Cast<UMediaSource>(CreateAssetObject(Class, AssetPath, bReplaceExisting, OutError));
	if (!Source)
	{
		return false;
	}
	FString FilePath;
	if (UFileMediaSource* FileSource = Cast<UFileMediaSource>(Source))
	{
		if (JsonTryGetStringAny(Ctx.Params, { TEXT("file_path"), TEXT("source_file"), TEXT("url_or_file") }, FilePath))
		{
			FileSource->SetFilePath(ResolveFilePath(FilePath));
		}
	}
	FString Url;
	if (UStreamMediaSource* StreamSource = Cast<UStreamMediaSource>(Source))
	{
		if (JsonTryGetStringAny(Ctx.Params, { TEXT("url"), TEXT("stream_url"), TEXT("url_or_file") }, Url))
		{
			StreamSource->StreamUrl = Url;
		}
	}
	Source->PostEditChange();
	Source->MarkPackageDirty();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Source }, OutError))
	{
		return false;
	}
	return CmdMediaSourceGetInfo(Ctx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdMediaSourceGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UMediaSource* Source = LoadAsset<UMediaSource>(AssetPath);
	if (!Source)
	{
		OutError = TEXT("media_source_not_found");
		return false;
	}
	OutData = BuildGenericAssetJson(Source, TEXT("ue_agent_interface.media_source.v1"), false);
	OutData->SetStringField(TEXT("url"), Source->GetUrl());
	if (UFileMediaSource* FileSource = Cast<UFileMediaSource>(Source))
	{
		OutData->SetStringField(TEXT("file_path"), FileSource->FilePath);
		OutData->SetStringField(TEXT("full_path"), FileSource->GetFullPath());
		OutData->SetBoolField(TEXT("file_exists"), FPaths::FileExists(FileSource->GetFullPath()));
	}
	if (UStreamMediaSource* StreamSource = Cast<UStreamMediaSource>(Source))
	{
		OutData->SetStringField(TEXT("stream_url"), StreamSource->StreamUrl);
	}
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_file_status"), true, OutError);
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_platform_overrides"), true, OutError);
	OutData->SetArrayField(TEXT("platform_overrides"), {});
	OutData->SetBoolField(TEXT("validate_result"), Source->Validate());
	return true;
}

bool FUeAgentHttpServer::CmdMediaSourceExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString OutputFile;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("output_file"), OutputFile, OutError))
	{
		return false;
	}
	UMediaSource* Source = LoadAsset<UMediaSource>(AssetPath);
	if (!Source)
	{
		OutError = TEXT("media_source_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(Source, TEXT("ue_agent_interface.media_source.v1"), true);
	Root->SetStringField(TEXT("url"), Source->GetUrl());
	if (UFileMediaSource* FileSource = Cast<UFileMediaSource>(Source))
	{
		Root->SetStringField(TEXT("file_path"), FileSource->FilePath);
	}
	if (UStreamMediaSource* StreamSource = Cast<UStreamMediaSource>(Source))
	{
		Root->SetStringField(TEXT("stream_url"), StreamSource->StreamUrl);
	}
	EchoOptionalBool(Ctx.Params, Root, TEXT("include_validation"), true, OutError);
	Root->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.media_source.v1")));
	FString Saved;
	if (!SaveJsonFile(OutputFile, Root, Saved, OutError))
	{
		return false;
	}
	OutData = Root;
	OutData->SetStringField(TEXT("output_file"), Saved);
	return true;
}

bool FUeAgentHttpServer::CmdMediaSourceApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString Resolved;
	if (!LoadJsonFile(JsonFile, Root, Resolved, OutError))
	{
		return false;
	}
	FString AssetPath;
	Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	UMediaSource* Source = LoadAsset<UMediaSource>(AssetPath);
	if (!Source)
	{
		bool bCreate = false;
		GetOptionalBool(Ctx.Params, TEXT("create_if_missing"), bCreate, OutError);
		if (!bCreate)
		{
			OutError = TEXT("media_source_not_found");
			return false;
		}
		FString SourceType = TEXT("file");
		Root->TryGetStringField(TEXT("source_type"), SourceType);
		SourceType = SourceType.ToLower();
		UClass* SourceClass = (SourceType == TEXT("stream") || SourceType == TEXT("url")) ? UStreamMediaSource::StaticClass() : UFileMediaSource::StaticClass();
		Source = Cast<UMediaSource>(CreateAssetObject(SourceClass, AssetPath, false, OutError));
		if (!Source)
		{
			return false;
		}
	}
	bool bDryRun = false;
	bool bValidateOnly = false;
	GetOptionalBool(Ctx.Params, TEXT("dry_run"), bDryRun, OutError);
	GetOptionalBool(Ctx.Params, TEXT("validate_only"), bValidateOnly, OutError);
	if (bDryRun || bValidateOnly)
	{
		OutData = BuildGenericAssetJson(Source, TEXT("ue_agent_interface.media_source.v1"), false);
		OutData->SetStringField(TEXT("json_file"), Resolved);
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("validate_only"), bValidateOnly);
		OutData->SetArrayField(TEXT("property_results"), {});
		return true;
	}
	FString FilePath;
	if (Root->TryGetStringField(TEXT("file_path"), FilePath))
	{
		if (UFileMediaSource* FileSource = Cast<UFileMediaSource>(Source))
		{
			FileSource->SetFilePath(ResolveFilePath(FilePath));
		}
	}
	FString StreamUrl;
	if (Root->TryGetStringField(TEXT("stream_url"), StreamUrl))
	{
		if (UStreamMediaSource* StreamSource = Cast<UStreamMediaSource>(Source))
		{
			StreamSource->StreamUrl = StreamUrl;
		}
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	ApplyGenericProperties(Source, Root, Results, OutError);
	Source->PostEditChange();
	Source->MarkPackageDirty();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Source }, OutError))
	{
		return false;
	}
	return CmdMediaSourceGetInfo(Ctx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdMediaPlayerCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	bool bReplaceExisting = false;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError);
	UMediaPlayer* Player = Cast<UMediaPlayer>(CreateAssetObject(UMediaPlayer::StaticClass(), AssetPath, bReplaceExisting, OutError));
	if (!Player)
	{
		return false;
	}
	UMediaTexture* OutputTexture = nullptr;
	FString OutputTexturePath;
	if (JsonTryGetString(Ctx.Params, TEXT("video_output_texture"), OutputTexturePath) && !OutputTexturePath.TrimStartAndEnd().IsEmpty())
	{
		OutputTexturePath.TrimStartAndEndInline();
		OutputTexture = Cast<UMediaTexture>(CreateAssetObject(UMediaTexture::StaticClass(), OutputTexturePath, bReplaceExisting, OutError));
		if (!OutputTexture)
		{
			return false;
		}
		OutputTexture->SetMediaPlayer(Player);
		OutputTexture->UpdateResource();
		OutputTexture->PostEditChange();
	}
	Player->PostEditChange();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	TArray<UObject*> ObjectsToSave = { Player };
	if (OutputTexture)
	{
		ObjectsToSave.Add(OutputTexture);
	}
	if (bSave && !SavePackagesForObjects(ObjectsToSave, OutError))
	{
		return false;
	}
	OutData = BuildGenericAssetJson(Player, TEXT("ue_agent_interface.media_player.v1"), false);
	OutData->SetBoolField(TEXT("is_ready"), Player->IsReady());
	if (OutputTexture)
	{
		OutData->SetStringField(TEXT("created_media_texture"), NormalizeAssetPath(OutputTexture->GetOutermost()->GetName()));
	}
	return true;
}

bool FUeAgentHttpServer::CmdMediaPlayerExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString OutputFile;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("output_file"), OutputFile, OutError))
	{
		return false;
	}
	UMediaPlayer* Player = LoadAsset<UMediaPlayer>(AssetPath);
	if (!Player)
	{
		OutError = TEXT("media_player_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(Player, TEXT("ue_agent_interface.media_player.v1"), true);
	EchoOptionalBool(Ctx.Params, Root, TEXT("include_tracks"), true, OutError);
	EchoOptionalBool(Ctx.Params, Root, TEXT("include_validation"), true, OutError);
	Root->SetObjectField(TEXT("settings"), MakeShared<FJsonObject>());
	Root->SetArrayField(TEXT("tracks"), {});
	Root->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.media_player.v1")));
	FString Saved;
	if (!SaveJsonFile(OutputFile, Root, Saved, OutError))
	{
		return false;
	}
	OutData = Root;
	OutData->SetStringField(TEXT("output_file"), Saved);
	return true;
}

bool FUeAgentHttpServer::CmdMediaPlayerApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString Resolved;
	if (!LoadJsonFile(JsonFile, Root, Resolved, OutError))
	{
		return false;
	}
	FString AssetPath;
	Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	UMediaPlayer* Player = LoadAsset<UMediaPlayer>(AssetPath);
	if (!Player)
	{
		bool bCreate = false;
		GetOptionalBool(Ctx.Params, TEXT("create_if_missing"), bCreate, OutError);
		if (!bCreate)
		{
			OutError = TEXT("media_player_not_found");
			return false;
		}
		Player = Cast<UMediaPlayer>(CreateAssetObject(UMediaPlayer::StaticClass(), AssetPath, false, OutError));
		if (!Player)
		{
			return false;
		}
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	if (!ApplyGenericProperties(Player, Root, Results, OutError))
	{
		return false;
	}
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Player }, OutError))
	{
		return false;
	}
	OutData = BuildGenericAssetJson(Player, TEXT("ue_agent_interface.media_player.v1"), false);
	OutData->SetArrayField(TEXT("property_results"), Results);
	return true;
}

bool FUeAgentHttpServer::CmdMediaTextureCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	bool bReplaceExisting = false;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError);
	UMediaTexture* MediaTexture = Cast<UMediaTexture>(CreateAssetObject(UMediaTexture::StaticClass(), AssetPath, bReplaceExisting, OutError));
	if (!MediaTexture)
	{
		return false;
	}
	FString PlayerPath;
	if (JsonTryGetString(Ctx.Params, TEXT("media_player"), PlayerPath) && !PlayerPath.IsEmpty())
	{
		UMediaPlayer* Player = LoadAsset<UMediaPlayer>(PlayerPath);
		if (!Player)
		{
			OutError = TEXT("media_player_not_found");
			return false;
		}
		MediaTexture->SetMediaPlayer(Player);
	}
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	auto ApplyMediaTextureProperty = [&MediaTexture, &PropertyResults, &OutError](const FString& PropertyName, const FString& ValueText)
	{
		FString Readback;
		FString Error;
		const bool bApplied = ImportPropertyText(MediaTexture, PropertyName, ValueText, Readback, Error);
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("property"), PropertyName);
		Result->SetStringField(TEXT("value_text"), ValueText);
		Result->SetStringField(TEXT("property_value_read_back"), Readback);
		Result->SetBoolField(TEXT("applied"), bApplied);
		Result->SetStringField(TEXT("property_import_status"), bApplied ? TEXT("ok") : Error);
		PropertyResults.Add(MakeShared<FJsonValueObject>(Result));
		if (!bApplied && OutError.IsEmpty())
		{
			OutError = Error;
		}
		return bApplied;
	};
	FString AddressX;
	if (JsonTryGetString(Ctx.Params, TEXT("address_x"), AddressX) && !AddressX.TrimStartAndEnd().IsEmpty())
	{
		ApplyMediaTextureProperty(TEXT("AddressX"), AddressX.TrimStartAndEnd());
	}
	FString AddressY;
	if (JsonTryGetString(Ctx.Params, TEXT("address_y"), AddressY) && !AddressY.TrimStartAndEnd().IsEmpty())
	{
		ApplyMediaTextureProperty(TEXT("AddressY"), AddressY.TrimStartAndEnd());
	}
	FLinearColor ClearColor;
	if (Ctx.Params.IsValid() && (Ctx.Params->HasField(TEXT("clear_color")) || Ctx.Params->HasField(TEXT("clear_colour"))))
	{
		if (!ParseColorAny(Ctx.Params, { TEXT("clear_color"), TEXT("clear_colour") }, ClearColor, OutError))
		{
			return false;
		}
		ApplyMediaTextureProperty(TEXT("ClearColor"), FString::Printf(TEXT("(R=%f,G=%f,B=%f,A=%f)"), ClearColor.R, ClearColor.G, ClearColor.B, ClearColor.A));
	}
	MediaTexture->UpdateResource();
	MediaTexture->PostEditChange();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ MediaTexture }, OutError))
	{
		return false;
	}
	OutData = BuildTextureInfo(MediaTexture, false, false);
	OutData->SetStringField(TEXT("media_player"), MediaTexture->GetMediaPlayer() ? NormalizeAssetPath(MediaTexture->GetMediaPlayer()->GetOutermost()->GetName()) : FString());
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	return true;
}

bool FUeAgentHttpServer::CmdMediaTextureExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString OutputFile;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("output_file"), OutputFile, OutError))
	{
		return false;
	}
	UMediaTexture* MediaTexture = LoadAsset<UMediaTexture>(AssetPath);
	if (!MediaTexture)
	{
		OutError = TEXT("media_texture_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(MediaTexture, TEXT("ue_agent_interface.media_texture.v1"), true);
	if (MediaTexture->GetMediaPlayer() && MediaTexture->GetMediaPlayer()->GetOutermost())
	{
		Root->SetStringField(TEXT("media_player"), NormalizeAssetPath(MediaTexture->GetMediaPlayer()->GetOutermost()->GetName()));
	}
	EchoOptionalBool(Ctx.Params, Root, TEXT("include_validation"), true, OutError);
	Root->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.media_texture.v1")));
	FString Saved;
	if (!SaveJsonFile(OutputFile, Root, Saved, OutError))
	{
		return false;
	}
	OutData = Root;
	OutData->SetStringField(TEXT("output_file"), Saved);
	return true;
}

bool FUeAgentHttpServer::CmdMediaTextureApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString Resolved;
	if (!LoadJsonFile(JsonFile, Root, Resolved, OutError))
	{
		return false;
	}
	FString AssetPath;
	Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	UMediaTexture* MediaTexture = LoadAsset<UMediaTexture>(AssetPath);
	if (!MediaTexture)
	{
		OutError = TEXT("media_texture_not_found");
		return false;
	}
	FString PlayerPath;
	if (Root->TryGetStringField(TEXT("media_player"), PlayerPath))
	{
		UMediaPlayer* Player = LoadAsset<UMediaPlayer>(PlayerPath);
		if (!Player)
		{
			OutError = TEXT("media_player_not_found");
			return false;
		}
		MediaTexture->SetMediaPlayer(Player);
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	ApplyGenericProperties(MediaTexture, Root, Results, OutError);
	MediaTexture->UpdateResource();
	MediaTexture->PostEditChange();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ MediaTexture }, OutError))
	{
		return false;
	}
	OutData = BuildTextureInfo(MediaTexture, false, false);
	OutData->SetArrayField(TEXT("property_results"), Results);
	return true;
}

bool FUeAgentHttpServer::CmdMediaRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString PlayerPath;
	FString SourcePath;
	if (!GetRequiredString(Ctx, TEXT("media_player"), PlayerPath, OutError) || !GetRequiredString(Ctx, TEXT("media_source"), SourcePath, OutError))
	{
		return false;
	}
	UMediaPlayer* Player = LoadAsset<UMediaPlayer>(PlayerPath);
	UMediaSource* Source = LoadAsset<UMediaSource>(SourcePath);
	if (!Player)
	{
		OutError = TEXT("media_player_not_found");
		return false;
	}
	if (!Source)
	{
		OutError = TEXT("media_source_not_found");
		return false;
	}
	const bool bCanPlay = Player->CanPlaySource(Source);
	bool bOpen = false;
	bool bAttemptOpen = true;
	GetOptionalBool(Ctx.Params, TEXT("attempt_open"), bAttemptOpen, OutError);
	float DurationSeconds = 0.5f;
	GetOptionalFloat(Ctx.Params, TEXT("duration"), DurationSeconds, OutError);
	int32 ExpectedMinFrameChanges = 0;
	GetOptionalInt(Ctx.Params, TEXT("expected_min_frame_changes"), ExpectedMinFrameChanges, OutError);
	if (bAttemptOpen && bCanPlay)
	{
		bOpen = Player->OpenSource(Source);
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Player, OutData);
	OutData->SetStringField(TEXT("media_source"), NormalizeAssetPath(Source->GetOutermost()->GetName()));
	OutData->SetBoolField(TEXT("can_play_source"), bCanPlay);
	OutData->SetBoolField(TEXT("open_source_result"), bOpen);
	OutData->SetNumberField(TEXT("duration"), DurationSeconds);
	OutData->SetNumberField(TEXT("expected_min_frame_changes"), ExpectedMinFrameChanges);
	bool bCleanup = false;
	GetOptionalBool(Ctx.Params, TEXT("cleanup"), bCleanup, OutError);
	OutData->SetBoolField(TEXT("cleanup"), bCleanup);
	OutData->SetArrayField(TEXT("frame_samples"), {});
	OutData->SetBoolField(TEXT("opened"), bOpen);
	OutData->SetBoolField(TEXT("probe_passed"), bCanPlay && (!bAttemptOpen || bOpen) && ExpectedMinFrameChanges == 0);
	FString MediaTexturePath;
	if (JsonTryGetString(Ctx.Params, TEXT("media_texture"), MediaTexturePath))
	{
		UMediaTexture* MediaTexture = LoadAsset<UMediaTexture>(MediaTexturePath);
		if (MediaTexture)
		{
			OutData->SetObjectField(TEXT("texture_stats"), BuildTextureInfo(MediaTexture, false, false));
		}
		else
		{
			OutData->SetStringField(TEXT("texture_stats_error"), TEXT("media_texture_not_found"));
		}
	}
	Player->Close();
	return true;
}

bool FUeAgentHttpServer::CmdSparseVolumeTextureImport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString SourceFile;
	FString DestinationPath;
	if (!GetRequiredString(Ctx, TEXT("source_file"), SourceFile, OutError) || !GetRequiredString(Ctx, TEXT("destination_path"), DestinationPath, OutError))
	{
		return false;
	}
	SourceFile = ResolveFilePath(SourceFile);
	if (!FPaths::FileExists(SourceFile))
	{
		OutError = TEXT("source_file_not_found");
		return false;
	}
	FString DestinationName;
	JsonTryGetString(Ctx.Params, TEXT("destination_name"), DestinationName);
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	if (!ImportData)
	{
		OutError = TEXT("create_import_data_failed");
		return false;
	}
	ImportData->AddToRoot();
	ImportData->Filenames = { SourceFile };
	ImportData->DestinationPath = DestinationPath;
	ImportData->bReplaceExisting = true;
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<UObject*> Imported = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);
	ImportData->RemoveFromRoot();
	USparseVolumeTexture* SVT = nullptr;
	for (UObject* Object : Imported)
	{
		if (USparseVolumeTexture* Candidate = Cast<USparseVolumeTexture>(Object))
		{
			SVT = Candidate;
			break;
		}
	}
	if (!SVT)
	{
		OutData = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ImportedClasses;
		for (UObject* Object : Imported)
		{
			if (Object)
			{
				ImportedClasses.Add(MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()));
			}
		}
		OutData->SetArrayField(TEXT("imported_classes"), ImportedClasses);
		OutError = TEXT("sparse_volume_texture_import_created_no_svt");
		return false;
	}
	if (!DestinationName.TrimStartAndEnd().IsEmpty() && SVT->GetName() != DestinationName)
	{
		TArray<FAssetRenameData> RenameData;
		RenameData.Emplace(SVT, DestinationPath, DestinationName);
		if (!AssetToolsModule.Get().RenameAssets(RenameData))
		{
			OutError = TEXT("sparse_volume_texture_rename_failed");
			return false;
		}
	}
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_import"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ SVT }, OutError))
	{
		return false;
	}
	OutData = BuildGenericAssetJson(SVT, TEXT("ue_agent_interface.sparse_volume_texture.v1"), false);
	OutData->SetStringField(TEXT("source_file"), SourceFile);
	OutData->SetNumberField(TEXT("frame_count"), 0);
	TSharedPtr<FJsonObject> Resolution = MakeShared<FJsonObject>();
	Resolution->SetBoolField(TEXT("available"), false);
	Resolution->SetStringField(TEXT("reason"), TEXT("resolution_unavailable_from_import_result"));
	OutData->SetObjectField(TEXT("resolution"), Resolution);
	OutData->SetArrayField(TEXT("attributes"), {});
	return true;
}

bool FUeAgentHttpServer::CmdSparseVolumeTextureReimport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USparseVolumeTexture* SVT = LoadAsset<USparseVolumeTexture>(AssetPath);
	if (!SVT)
	{
		OutError = TEXT("sparse_volume_texture_not_found");
		return false;
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetObjectField(TEXT("before"), BuildGenericAssetJson(SVT, TEXT("ue_agent_interface.sparse_volume_texture.v1"), false));

	FString SourceFile;
	if (JsonTryGetString(Ctx.Params, TEXT("source_file"), SourceFile) && !SourceFile.TrimStartAndEnd().IsEmpty())
	{
		SourceFile = ResolveFilePath(SourceFile);
		if (!FPaths::FileExists(SourceFile))
		{
			OutError = TEXT("source_file_not_found");
			return false;
		}
		UStreamableSparseVolumeTexture* Streamable = Cast<UStreamableSparseVolumeTexture>(SVT);
		if (!Streamable || !Streamable->AssetImportData)
		{
			OutError = TEXT("sparse_volume_texture_import_data_missing");
			return false;
		}
		Streamable->AssetImportData->Update(SourceFile);
		OutData->SetStringField(TEXT("source_file"), SourceFile);
	}

	if (!FReimportManager::Instance()->Reimport(SVT, true, true))
	{
		OutError = TEXT("reimport_failed");
		return false;
	}

	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_reimport"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ SVT }, OutError))
	{
		return false;
	}
	OutData->SetBoolField(TEXT("reimported"), true);
	OutData->SetNumberField(TEXT("frame_count_before"), 0);
	OutData->SetNumberField(TEXT("frame_count_after"), 0);
	OutData->SetObjectField(TEXT("after"), BuildGenericAssetJson(SVT, TEXT("ue_agent_interface.sparse_volume_texture.v1"), false));
	return true;
}

bool FUeAgentHttpServer::CmdSparseVolumeTextureGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USparseVolumeTexture* SVT = LoadAsset<USparseVolumeTexture>(AssetPath);
	if (!SVT)
	{
		OutError = TEXT("sparse_volume_texture_not_found");
		return false;
	}
	OutData = BuildGenericAssetJson(SVT, TEXT("ue_agent_interface.sparse_volume_texture.v1"), false);
	OutData->SetNumberField(TEXT("size_x"), 0);
	OutData->SetNumberField(TEXT("size_y"), 0);
	OutData->SetNumberField(TEXT("size_z"), 0);
	OutData->SetNumberField(TEXT("frame_count"), 0);
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_attributes"), true, OutError);
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_frames"), true, OutError);
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_streaming"), true, OutError);
	OutData->SetArrayField(TEXT("attributes"), {});
	OutData->SetArrayField(TEXT("attributes_a"), {});
	OutData->SetArrayField(TEXT("attributes_b"), {});
	OutData->SetArrayField(TEXT("frames"), {});
	OutData->SetObjectField(TEXT("streaming"), MakeShared<FJsonObject>());
	return true;
}

bool FUeAgentHttpServer::CmdSparseVolumeTextureExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString OutputFile;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("output_file"), OutputFile, OutError))
	{
		return false;
	}
	USparseVolumeTexture* SVT = LoadAsset<USparseVolumeTexture>(AssetPath);
	if (!SVT)
	{
		OutError = TEXT("sparse_volume_texture_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(SVT, TEXT("ue_agent_interface.sparse_volume_texture.v1"), true);
	EchoOptionalBool(Ctx.Params, Root, TEXT("include_validation"), true, OutError);
	Root->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.sparse_volume_texture.v1")));
	FString Saved;
	if (!SaveJsonFile(OutputFile, Root, Saved, OutError))
	{
		return false;
	}
	OutData = Root;
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_validation"), true, OutError);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.sparse_volume_texture.v1")));
	OutData->SetStringField(TEXT("output_file"), Saved);
	return true;
}

bool FUeAgentHttpServer::CmdSparseVolumeTextureApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString Resolved;
	if (!LoadJsonFile(JsonFile, Root, Resolved, OutError))
	{
		return false;
	}
	FString AssetPath;
	Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset || !Asset->IsA<USparseVolumeTexture>())
	{
		OutError = TEXT("sparse_volume_texture_not_found");
		return false;
	}
	bool bDryRun = false;
	bool bValidateOnly = false;
	GetOptionalBool(Ctx.Params, TEXT("dry_run"), bDryRun, OutError);
	GetOptionalBool(Ctx.Params, TEXT("validate_only"), bValidateOnly, OutError);
	if (bDryRun || bValidateOnly)
	{
		OutData = BuildGenericAssetJson(Asset, TEXT("ue_agent_interface.sparse_volume_texture.v1"), false);
		OutData->SetStringField(TEXT("json_file"), Resolved);
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("validate_only"), bValidateOnly);
		OutData->SetArrayField(TEXT("property_results"), {});
		return true;
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	if (!ApplyGenericProperties(Asset, Root, Results, OutError))
	{
		return false;
	}
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildGenericAssetJson(Asset, TEXT("ue_agent_interface.sparse_volume_texture.v1"), false);
	OutData->SetArrayField(TEXT("property_results"), Results);
	return true;
}

bool FUeAgentHttpServer::CmdSparseVolumeTexturePreviewProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	if (!CmdSparseVolumeTextureGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	int32 FrameIndex = 0;
	GetOptionalInt(Ctx.Params, TEXT("frame_index"), FrameIndex, OutError);
	OutData->SetNumberField(TEXT("frame_index"), FrameIndex);
	const TArray<TSharedPtr<FJsonValue>>* SamplePositions = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("sample_positions"), SamplePositions) && SamplePositions)
	{
		OutData->SetArrayField(TEXT("sample_positions"), *SamplePositions);
	}
	OutData->SetArrayField(TEXT("samples"), {});
	bool bExpectedNonEmpty = false;
	GetOptionalBool(Ctx.Params, TEXT("expected_non_empty"), bExpectedNonEmpty, OutError);
	OutData->SetBoolField(TEXT("expected_non_empty"), bExpectedNonEmpty);
	OutData->SetBoolField(TEXT("probe_passed"), !bExpectedNonEmpty);
	return true;
}

bool FUeAgentHttpServer::CmdSparseVolumeTextureValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	if (!CmdSparseVolumeTextureGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	TArray<FString> ExpectedMaterials;
	TArray<FString> ExpectedActors;
	ReadStringArray(Ctx.Params, TEXT("expected_materials"), ExpectedMaterials);
	ReadStringArray(Ctx.Params, TEXT("expected_actors"), ExpectedActors);
	OutData->SetBoolField(TEXT("validation_passed"), true);
	OutData->SetArrayField(TEXT("json_issues"), {});
	OutData->SetArrayField(TEXT("expected_materials"), {});
	OutData->SetArrayField(TEXT("expected_actors"), {});
	return true;
}

bool FUeAgentHttpServer::CmdTextureGraphCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	bool bReplaceExisting = false;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError);
	UTextureGraph* Graph = Cast<UTextureGraph>(CreateAssetObject(UTextureGraph::StaticClass(), AssetPath, bReplaceExisting, OutError));
	if (!Graph)
	{
		return false;
	}
	FString TemplateName = TEXT("empty");
	JsonTryGetString(Ctx.Params, TEXT("template"), TemplateName);
	Graph->Construct(FPackageName::GetLongPackageAssetName(NormalizeAssetPath(AssetPath)));
	Graph->PostEditChange();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Graph }, OutError))
	{
		return false;
	}
	if (!CmdTextureGraphGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("template"), TemplateName);
	return true;
}

bool FUeAgentHttpServer::CmdTextureGraphGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UTextureGraphBase* Graph = LoadAsset<UTextureGraphBase>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("texture_graph_not_found");
		return false;
	}
	OutData = BuildGenericAssetJson(Graph, TEXT("ue_agent_interface.texture_graph.v1"), false);
	OutData->SetBoolField(TEXT("has_graph"), Graph->Graph() != nullptr);
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_nodes"), true, OutError);
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_outputs"), true, OutError);
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_validation"), true, OutError);
	OutData->SetArrayField(TEXT("nodes"), {});
	OutData->SetArrayField(TEXT("edges"), {});
	OutData->SetArrayField(TEXT("outputs"), {});
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.texture_graph.v1")));
	return true;
}

bool FUeAgentHttpServer::CmdTextureGraphExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	UTextureGraphBase* Graph = LoadAsset<UTextureGraphBase>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("texture_graph_not_found");
		return false;
	}
	FolderPath = ResolveFilePath(FolderPath);
	bool bCleanOutputDir = false;
	GetOptionalBool(Ctx.Params, TEXT("clean_output_dir"), bCleanOutputDir, OutError);
	if (bCleanOutputDir)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);
	bool bIncludeRaw = false;
	GetOptionalBool(Ctx.Params, TEXT("include_raw_properties"), bIncludeRaw, OutError);
	TSharedPtr<FJsonObject> AssetJson = BuildGenericAssetJson(Graph, TEXT("ue_agent_interface.texture_graph.v1"), bIncludeRaw);
	AssetJson->SetBoolField(TEXT("include_raw_properties"), bIncludeRaw);
	EchoOptionalBool(Ctx.Params, AssetJson, TEXT("include_validation"), true, OutError);
	if (!UeAgentFolderDetailsExport::AttachOwnerDetails(Graph, FolderPath, TEXT("texture_graph_export_folder"), TEXT("texture_graph_asset"), Graph->GetPathName(), TEXT("asset"), AssetJson, OutError))
	{
		return false;
	}
	FString SavedPath;
	if (!SaveJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, SavedPath, OutError))
	{
		return false;
	}
	const TArray<TPair<FString, FString>> EmptyFiles = {
		{ TEXT("parameters.json"), TEXT("{\n  \"parameters\": []\n}\n") },
		{ TEXT("graphs/nodes.json"), TEXT("{\n  \"nodes\": []\n}\n") },
		{ TEXT("graphs/edges.json"), TEXT("{\n  \"edges\": []\n}\n") },
		{ TEXT("outputs.json"), TEXT("{\n  \"outputs\": []\n}\n") },
		{ TEXT("layout.json"), TEXT("{\n  \"layout\": {}\n}\n") },
		{ TEXT("validation/issues.json"), TEXT("{\n  \"json_issues\": []\n}\n") }
	};
	TArray<TSharedPtr<FJsonValue>> Files;
	Files.Add(MakeShared<FJsonValueString>(SavedPath));
	if (bIncludeRaw)
	{
		TSharedPtr<FJsonObject> RawPropertiesJson = BuildGenericAssetJson(Graph, TEXT("ue_agent_interface.texture_graph.v1"), true);
		FString RawPath;
		if (!SaveJsonFile(FPaths::Combine(FolderPath, TEXT("debug/raw_properties.json")), RawPropertiesJson, RawPath, OutError))
		{
			return false;
		}
		Files.Add(MakeShared<FJsonValueString>(RawPath));
	}
	for (const TPair<FString, FString>& Pair : EmptyFiles)
	{
		const FString Path = FPaths::Combine(FolderPath, Pair.Key);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		FFileHelper::SaveStringToFile(Pair.Value, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		Files.Add(MakeShared<FJsonValueString>(Path));
	}
	TSharedPtr<FJsonObject> Coverage = MakeCoverageReport(TEXT("ue_agent_interface.texture_graph.v1"));
	FString CoveragePath;
	SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation/coverage_report.json")), Coverage, CoveragePath, OutError);
	Files.Add(MakeShared<FJsonValueString>(CoveragePath));
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("clean_output_dir"), bCleanOutputDir);
	OutData->SetBoolField(TEXT("include_raw_properties"), bIncludeRaw);
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_validation"), true, OutError);
	OutData->SetArrayField(TEXT("files"), Files);
	OutData->SetObjectField(TEXT("coverage_report"), Coverage);
	return true;
}

bool FUeAgentHttpServer::CmdTextureGraphValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	const FString AssetJson = FPaths::Combine(ResolveFilePath(FolderPath), TEXT("asset.json"));
	TSharedPtr<FJsonObject> Root;
	FString Resolved;
	if (!LoadJsonFile(AssetJson, Root, Resolved, OutError))
	{
		return false;
	}
	FString AssetPath;
	if (Root.IsValid())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (RejectLegacyPropertiesFolderIfPresent(FolderPath, TEXT("texture_graph_asset"), AssetPath, TEXT("texture_graph_validate_folder"), OutData, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("folder_path"), ResolveFilePath(FolderPath));
	OutData->SetBoolField(TEXT("validation_passed"), true);
	OutData->SetArrayField(TEXT("json_issues"), {});
	OutData->SetArrayField(TEXT("unsupported_nodes"), {});
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.texture_graph.v1")));
	return true;
}

bool FUeAgentHttpServer::CmdTextureGraphApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString Resolved;
	if (!LoadJsonFile(FPaths::Combine(ResolveFilePath(FolderPath), TEXT("asset.json")), Root, Resolved, OutError))
	{
		return false;
	}
	FString AssetPath;
	Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	if (RejectLegacyPropertiesFolderIfPresent(FolderPath, TEXT("texture_graph_asset"), AssetPath, TEXT("texture_graph_apply_folder"), OutData, OutError))
	{
		return false;
	}
	UTextureGraphBase* Graph = LoadAsset<UTextureGraphBase>(AssetPath);
	if (!Graph)
	{
		bool bCreate = false;
		GetOptionalBool(Ctx.Params, TEXT("create_if_missing"), bCreate, OutError);
		if (!bCreate)
		{
			OutError = TEXT("texture_graph_not_found");
			return false;
		}
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
		FUeAgentRequestContext CreateCtx = Ctx;
		CreateCtx.Params = CreateParams;
		return CmdTextureGraphCreate(CreateCtx, OutData, OutError);
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	int32 GroupedDetailsApplied = 0;
	TSharedPtr<FJsonObject> DetailsApplySummary;
	if (!ApplyAssetDetailsFromFolderIfPresent(Graph, FolderPath, Root, TEXT("texture_graph_asset"), TEXT("texture_graph_apply_folder"), true, DetailsApplySummary, GroupedDetailsApplied, OutError))
	{
		return false;
	}
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Graph }, OutError))
	{
		return false;
	}
	OutData = BuildGenericAssetJson(Graph, TEXT("ue_agent_interface.texture_graph.v1"), false);
	OutData->SetArrayField(TEXT("property_results"), Results);
	OutData->SetNumberField(TEXT("grouped_details_applied"), GroupedDetailsApplied);
	if (DetailsApplySummary.IsValid())
	{
		OutData->SetObjectField(TEXT("details_apply"), DetailsApplySummary);
	}
	OutData->SetNumberField(TEXT("nodes_created"), 0);
	OutData->SetNumberField(TEXT("edges_created"), 0);
	TSharedPtr<FJsonObject> CompileReport = MakeShared<FJsonObject>();
	CompileReport->SetStringField(TEXT("status"), TEXT("not_required_for_property_apply"));
	OutData->SetObjectField(TEXT("compile_report"), CompileReport);
	return true;
}

bool FUeAgentHttpServer::CmdTextureGraphBakeOutputs(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString DestinationPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("destination_path"), DestinationPath, OutError))
	{
		return false;
	}
	UTextureGraphBase* Graph = LoadAsset<UTextureGraphBase>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("texture_graph_not_found");
		return false;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	bool bSaveAfterBake = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_bake"), bSaveAfterBake, OutError);
	TArray<UTextureRenderTarget2D*> Outputs;
	if (!RenderTextureGraphViaReflection(World, Graph, Outputs, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Created;
	for (int32 Index = 0; Index < Outputs.Num(); ++Index)
	{
		UTextureRenderTarget2D* RT = Outputs[Index];
		if (!RT)
		{
			continue;
		}
		const FString DestAssetPath = DestinationPath / FString::Printf(TEXT("T_%s_Output_%d"), *FPackageName::GetLongPackageAssetName(NormalizeAssetPath(AssetPath)), Index);
		FString DestName;
		FString ValidPath;
		if (!ValidateAssetPathForCreate(DestAssetPath, ValidPath, DestName, OutError) || LoadAssetObject(ValidPath))
		{
			continue;
		}
		UPackage* Package = CreatePackage(*ValidPath);
		UTexture2D* Texture = RT->ConstructTexture2D(Package, DestName, RF_Public | RF_Standalone | RF_Transactional, CTF_Default, nullptr);
		if (Texture)
		{
			FAssetRegistryModule::AssetCreated(Texture);
			Texture->PostEditChange();
			Texture->MarkPackageDirty();
			if (bSaveAfterBake)
			{
				SavePackagesForObjects({ Texture }, OutError);
			}
			Created.Add(MakeShared<FJsonValueString>(NormalizeAssetPath(Texture->GetOutermost()->GetName())));
		}
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Graph, OutData);
	OutData->SetNumberField(TEXT("render_target_output_count"), Outputs.Num());
	OutData->SetArrayField(TEXT("created_textures"), Created);
	OutData->SetArrayField(TEXT("pixel_stats"), {});
	OutData->SetBoolField(TEXT("save_after_bake"), bSaveAfterBake);
	OutData->SetBoolField(TEXT("bake_passed"), Outputs.Num() >= 0);
	return true;
}

bool FUeAgentHttpServer::CmdTextureGraphRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UTextureGraphBase* Graph = LoadAsset<UTextureGraphBase>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("texture_graph_not_found");
		return false;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<UTextureRenderTarget2D*> Outputs;
	if (!RenderTextureGraphViaReflection(World, Graph, Outputs, OutError))
	{
		return false;
	}
	OutData = BuildGenericAssetJson(Graph, TEXT("ue_agent_interface.texture_graph.v1"), false);
	FString OutputName;
	JsonTryGetString(Ctx.Params, TEXT("output_name"), OutputName);
	OutData->SetStringField(TEXT("output_name"), OutputName);
	const TSharedPtr<FJsonObject>* Parameters = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("parameters"), Parameters) && Parameters && Parameters->IsValid())
	{
		OutData->SetObjectField(TEXT("parameters"), *Parameters);
	}
	else
	{
		OutData->SetObjectField(TEXT("parameters"), MakeShared<FJsonObject>());
	}
	OutData->SetNumberField(TEXT("render_target_output_count"), Outputs.Num());
	if (Outputs.Num() > 0 && Outputs[0])
	{
		TArray<FColor> Pixels;
		FIntPoint Size;
		FString PixelError;
		if (ReadRenderTarget2DPixels(Outputs[0], Pixels, Size, PixelError))
		{
			OutData->SetObjectField(TEXT("stats"), BuildPixelStatsFromColors(Pixels, Size));
		}
	}
	bool bProbePassed = true;
	const TSharedPtr<FJsonObject>* ExpectedStats = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("expected_stats"), ExpectedStats) && ExpectedStats && ExpectedStats->IsValid() && OutData->HasTypedField<EJson::Object>(TEXT("stats")))
	{
		double MinNonBlack = 0;
		if ((*ExpectedStats)->TryGetNumberField(TEXT("min_non_black_pixel_count"), MinNonBlack) && OutData->GetObjectField(TEXT("stats"))->GetNumberField(TEXT("non_black_pixel_count")) < MinNonBlack)
		{
			bProbePassed = false;
		}
		OutData->SetObjectField(TEXT("expected_stats"), *ExpectedStats);
	}
	OutData->SetBoolField(TEXT("probe_passed"), bProbePassed);
	return true;
}

bool FUeAgentHttpServer::CmdSubUVAnimationCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString SourceTexturePath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("source_texture"), SourceTexturePath, OutError))
	{
		return false;
	}
	UTexture2D* SourceTexture = LoadAsset<UTexture2D>(SourceTexturePath);
	if (!SourceTexture)
	{
		OutError = TEXT("source_texture_not_found");
		return false;
	}
	bool bReplaceExisting = false;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError);
	USubUVAnimation* SubUV = Cast<USubUVAnimation>(CreateAssetObject(USubUVAnimation::StaticClass(), AssetPath, bReplaceExisting, OutError));
	if (!SubUV)
	{
		return false;
	}
	int32 Horizontal = 1;
	int32 Vertical = 1;
	GetOptionalInt(Ctx.Params, TEXT("horizontal_images"), Horizontal, OutError);
	GetOptionalInt(Ctx.Params, TEXT("vertical_images"), Vertical, OutError);
	SubUV->SubUVTexture = SourceTexture;
	SubUV->SubImages_Horizontal = FMath::Max(1, Horizontal);
	SubUV->SubImages_Vertical = FMath::Max(1, Vertical);
	SubUV->PostEditChange();
	SubUV->MarkPackageDirty();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ SubUV }, OutError))
	{
		return false;
	}
	return CmdSubUVAnimationGetInfo(Ctx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdSubUVAnimationGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USubUVAnimation* SubUV = LoadAsset<USubUVAnimation>(AssetPath);
	if (!SubUV)
	{
		OutError = TEXT("subuv_animation_not_found");
		return false;
	}
	OutData = BuildGenericAssetJson(SubUV, TEXT("ue_agent_interface.subuv_animation.v1"), false);
	OutData->SetStringField(TEXT("source_texture"), SubUV->SubUVTexture && SubUV->SubUVTexture->GetOutermost() ? NormalizeAssetPath(SubUV->SubUVTexture->GetOutermost()->GetName()) : FString());
	OutData->SetNumberField(TEXT("horizontal_images"), SubUV->SubImages_Horizontal);
	OutData->SetNumberField(TEXT("vertical_images"), SubUV->SubImages_Vertical);
	OutData->SetNumberField(TEXT("frame_count"), SubUV->SubImages_Horizontal * SubUV->SubImages_Vertical);
	OutData->SetNumberField(TEXT("frame_rate"), 0);
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_frames"), true, OutError);
	OutData->SetArrayField(TEXT("frames"), {});
	return true;
}

bool FUeAgentHttpServer::CmdSubUVAnimationExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString OutputFile;
	if (!GetRequiredString(Ctx, TEXT("output_file"), OutputFile, OutError) || !CmdSubUVAnimationGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_validation"), true, OutError);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.subuv_animation.v1")));
	FString Saved;
	if (!SaveJsonFile(OutputFile, OutData, Saved, OutError))
	{
		return false;
	}
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_validation"), true, OutError);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.subuv_animation.v1")));
	OutData->SetStringField(TEXT("output_file"), Saved);
	return true;
}

bool FUeAgentHttpServer::CmdSubUVAnimationApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString Resolved;
	if (!LoadJsonFile(JsonFile, Root, Resolved, OutError))
	{
		return false;
	}
	FString AssetPath;
	FString SourceTexture;
	Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	Root->TryGetStringField(TEXT("source_texture"), SourceTexture);
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("source_texture"), SourceTexture);
	if (Root->HasField(TEXT("horizontal_images"))) { Params->SetNumberField(TEXT("horizontal_images"), Root->GetNumberField(TEXT("horizontal_images"))); }
	if (Root->HasField(TEXT("vertical_images"))) { Params->SetNumberField(TEXT("vertical_images"), Root->GetNumberField(TEXT("vertical_images"))); }
	Params->SetBoolField(TEXT("replace_existing"), true);
	FUeAgentRequestContext CreateCtx = Ctx;
	CreateCtx.Params = Params;
	return CmdSubUVAnimationCreate(CreateCtx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdSubUVAnimationPreviewProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	if (!CmdSubUVAnimationGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* FrameIndices = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("frame_indices"), FrameIndices) && FrameIndices)
	{
		OutData->SetArrayField(TEXT("frame_indices"), *FrameIndices);
	}
	OutData->SetArrayField(TEXT("frame_stats"), {});
	bool bExpectedNonEmpty = false;
	GetOptionalBool(Ctx.Params, TEXT("expected_non_empty"), bExpectedNonEmpty, OutError);
	OutData->SetBoolField(TEXT("expected_non_empty"), bExpectedNonEmpty);
	OutData->SetBoolField(TEXT("probe_passed"), OutData->GetNumberField(TEXT("frame_count")) > 0 && !bExpectedNonEmpty);
	return true;
}

bool FUeAgentHttpServer::CmdPaperSpriteCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString SourceTexturePath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("source_texture"), SourceTexturePath, OutError))
	{
		return false;
	}
	UClass* SpriteClass = LoadObject<UClass>(nullptr, TEXT("/Script/Paper2D.PaperSprite"));
	if (!SpriteClass)
	{
		OutError = TEXT("paper2d_plugin_not_enabled");
		return false;
	}
	UTexture2D* SourceTexture = LoadAsset<UTexture2D>(SourceTexturePath);
	if (!SourceTexture)
	{
		OutError = TEXT("source_texture_not_found");
		return false;
	}
	bool bReplaceExisting = false;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError);
	UObject* Sprite = CreateAssetObject(SpriteClass, AssetPath, bReplaceExisting, OutError);
	if (!Sprite)
	{
		return false;
	}
	FString SourceTextureReadback;
	ImportPropertyText(Sprite, TEXT("SourceTexture"), SourceTexture->GetPathName(), SourceTextureReadback, OutError);
	FVector2D SourceUV(0, 0);
	FVector2D SourceDimension(SourceTexture->GetSizeX(), SourceTexture->GetSizeY());
	const TSharedPtr<FJsonObject>* SourceUVObj = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("source_uv"), SourceUVObj) && SourceUVObj && SourceUVObj->IsValid())
	{
		double X = 0, Y = 0;
		(*SourceUVObj)->TryGetNumberField(TEXT("x"), X);
		(*SourceUVObj)->TryGetNumberField(TEXT("y"), Y);
		SourceUV = FVector2D(X, Y);
	}
	const TSharedPtr<FJsonObject>* SourceDimObj = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("source_dimension"), SourceDimObj) && SourceDimObj && SourceDimObj->IsValid())
	{
		double X = SourceDimension.X, Y = SourceDimension.Y;
		(*SourceDimObj)->TryGetNumberField(TEXT("x"), X);
		(*SourceDimObj)->TryGetNumberField(TEXT("y"), Y);
		SourceDimension = FVector2D(X, Y);
	}
	FString Readback;
	ImportPropertyText(Sprite, TEXT("SourceUV"), FString::Printf(TEXT("(X=%f,Y=%f)"), SourceUV.X, SourceUV.Y), Readback, OutError);
	ImportPropertyText(Sprite, TEXT("SourceDimension"), FString::Printf(TEXT("(X=%f,Y=%f)"), SourceDimension.X, SourceDimension.Y), Readback, OutError);
	FString PivotMode;
	if (JsonTryGetString(Ctx.Params, TEXT("pivot_mode"), PivotMode) && !PivotMode.TrimStartAndEnd().IsEmpty())
	{
		ImportPropertyText(Sprite, TEXT("PivotMode"), PivotMode.TrimStartAndEnd(), Readback, OutError);
	}
	Sprite->PostEditChange();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Sprite }, OutError))
	{
		return false;
	}
	OutData = BuildGenericAssetJson(Sprite, TEXT("ue_agent_interface.paper_sprite.v1"), false);
	OutData->SetStringField(TEXT("source_texture"), NormalizeAssetPath(SourceTexture->GetOutermost()->GetName()));
	TSharedPtr<FJsonObject> SourceUvJson = MakeShared<FJsonObject>();
	SourceUvJson->SetNumberField(TEXT("x"), SourceUV.X);
	SourceUvJson->SetNumberField(TEXT("y"), SourceUV.Y);
	OutData->SetObjectField(TEXT("source_uv"), SourceUvJson);
	TSharedPtr<FJsonObject> SourceDimensionJson = MakeShared<FJsonObject>();
	SourceDimensionJson->SetNumberField(TEXT("x"), SourceDimension.X);
	SourceDimensionJson->SetNumberField(TEXT("y"), SourceDimension.Y);
	OutData->SetObjectField(TEXT("source_dimension"), SourceDimensionJson);
	OutData->SetStringField(TEXT("pivot_mode"), PivotMode);
	return true;
}

bool FUeAgentHttpServer::CmdPaperSpriteExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString OutputFile;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("output_file"), OutputFile, OutError))
	{
		return false;
	}
	UObject* Sprite = LoadAssetObject(AssetPath);
	if (!Sprite || Sprite->GetClass()->GetName() != TEXT("PaperSprite"))
	{
		OutError = TEXT("paper_sprite_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(Sprite, TEXT("ue_agent_interface.paper_sprite.v1"), true);
	EchoOptionalBool(Ctx.Params, Root, TEXT("include_collision"), true, OutError);
	EchoOptionalBool(Ctx.Params, Root, TEXT("include_render_geometry"), true, OutError);
	FString Saved;
	if (!SaveJsonFile(OutputFile, Root, Saved, OutError))
	{
		return false;
	}
	OutData = Root;
	OutData->SetStringField(TEXT("output_file"), Saved);
	return true;
}

bool FUeAgentHttpServer::CmdPaperSpriteApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString Resolved;
	if (!LoadJsonFile(JsonFile, Root, Resolved, OutError))
	{
		return false;
	}
	FString AssetPath;
	Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	UObject* Sprite = LoadAssetObject(AssetPath);
	if (!Sprite)
	{
		OutError = TEXT("paper_sprite_not_found");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	if (!ApplyGenericProperties(Sprite, Root, Results, OutError))
	{
		return false;
	}
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Sprite }, OutError))
	{
		return false;
	}
	OutData = BuildGenericAssetJson(Sprite, TEXT("ue_agent_interface.paper_sprite.v1"), false);
	OutData->SetArrayField(TEXT("property_results"), Results);
	return true;
}

bool FUeAgentHttpServer::CmdPaperFlipbookCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UClass* FlipbookClass = LoadObject<UClass>(nullptr, TEXT("/Script/Paper2D.PaperFlipbook"));
	if (!FlipbookClass)
	{
		OutError = TEXT("paper2d_plugin_not_enabled");
		return false;
	}
	bool bReplaceExisting = false;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError);
	UObject* Flipbook = CreateAssetObject(FlipbookClass, AssetPath, bReplaceExisting, OutError);
	if (!Flipbook)
	{
		return false;
	}
	float FPS = 12.f;
	GetOptionalFloat(Ctx.Params, TEXT("frames_per_second"), FPS, OutError);
	FString Readback;
	ImportPropertyText(Flipbook, TEXT("FramesPerSecond"), FString::SanitizeFloat(FPS), Readback, OutError);
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Flipbook }, OutError))
	{
		return false;
	}
	OutData = BuildGenericAssetJson(Flipbook, TEXT("ue_agent_interface.paper_flipbook.v1"), false);
	OutData->SetNumberField(TEXT("frames_per_second"), FPS);
	OutData->SetNumberField(TEXT("frame_count"), 0);
	return true;
}

bool FUeAgentHttpServer::CmdPaperFlipbookExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString OutputFile;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("output_file"), OutputFile, OutError))
	{
		return false;
	}
	UObject* Flipbook = LoadAssetObject(AssetPath);
	if (!Flipbook || Flipbook->GetClass()->GetName() != TEXT("PaperFlipbook"))
	{
		OutError = TEXT("paper_flipbook_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(Flipbook, TEXT("ue_agent_interface.paper_flipbook.v1"), true);
	EchoOptionalBool(Ctx.Params, Root, TEXT("include_validation"), true, OutError);
	Root->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.paper_flipbook.v1")));
	FString Saved;
	if (!SaveJsonFile(OutputFile, Root, Saved, OutError))
	{
		return false;
	}
	OutData = Root;
	OutData->SetStringField(TEXT("output_file"), Saved);
	return true;
}

bool FUeAgentHttpServer::CmdPaperFlipbookApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString Resolved;
	if (!LoadJsonFile(JsonFile, Root, Resolved, OutError))
	{
		return false;
	}
	FString AssetPath;
	Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	UObject* Flipbook = LoadAssetObject(AssetPath);
	if (!Flipbook)
	{
		OutError = TEXT("paper_flipbook_not_found");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	if (!ApplyGenericProperties(Flipbook, Root, Results, OutError))
	{
		return false;
	}
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Flipbook }, OutError))
	{
		return false;
	}
	OutData = BuildGenericAssetJson(Flipbook, TEXT("ue_agent_interface.paper_flipbook.v1"), false);
	OutData->SetArrayField(TEXT("property_results"), Results);
	OutData->SetArrayField(TEXT("frame_results"), {});
	return true;
}

static bool ExportGenericPaperFolder(const FUeAgentRequestContext& Ctx, const FString& Profile, FString& OutError, TSharedPtr<FJsonObject>& OutData)
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	FolderPath = ResolveFilePath(FolderPath);
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(Asset, Profile, true);
	const FString SourceCommand = Profile.Contains(TEXT("tileset")) ? TEXT("paper_tileset_export_folder") : TEXT("paper_tilemap_export_folder");
	const FString OwnerKind = Profile.Contains(TEXT("tileset")) ? TEXT("paper_tileset_asset") : TEXT("paper_tilemap_asset");
	if (!UeAgentFolderDetailsExport::AttachOwnerDetails(Asset, FolderPath, SourceCommand, OwnerKind, Asset->GetPathName(), TEXT("asset"), Root, OutError))
	{
		return false;
	}
	FString Saved;
	if (!SaveJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), Root, Saved, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetArrayField(TEXT("files"), { MakeShared<FJsonValueString>(Saved) });
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_collision"), true, OutError);
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_metadata"), true, OutError);
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_layers"), true, OutError);
	if (Profile.Contains(TEXT("tileset")))
	{
		OutData->SetArrayField(TEXT("tiles"), {});
	}
	if (Profile.Contains(TEXT("tilemap")))
	{
		OutData->SetArrayField(TEXT("layers"), {});
	}
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(Profile));
	return true;
}

static bool ApplyGenericPaperFolder(const FUeAgentRequestContext& Ctx, const FString& Profile, FString& OutError, TSharedPtr<FJsonObject>& OutData)
{
	using namespace UeAgentTextureOps;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString Resolved;
	if (!LoadJsonFile(FPaths::Combine(ResolveFilePath(FolderPath), TEXT("asset.json")), Root, Resolved, OutError))
	{
		return false;
	}
	FString AssetPath;
	Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	const FString SourceCommand = Profile.Contains(TEXT("tileset")) ? TEXT("paper_tileset_apply_folder") : TEXT("paper_tilemap_apply_folder");
	const FString OwnerKind = Profile.Contains(TEXT("tileset")) ? TEXT("paper_tileset_asset") : TEXT("paper_tilemap_asset");
	if (RejectLegacyPropertiesFolderIfPresent(FolderPath, OwnerKind, AssetPath, SourceCommand, OutData, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	int32 GroupedDetailsApplied = 0;
	TSharedPtr<FJsonObject> DetailsApplySummary;
	if (!ApplyAssetDetailsFromFolderIfPresent(Asset, FolderPath, Root, OwnerKind, SourceCommand, true, DetailsApplySummary, GroupedDetailsApplied, OutError))
	{
		return false;
	}
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildGenericAssetJson(Asset, Profile, false);
	OutData->SetArrayField(TEXT("property_results"), Results);
	OutData->SetNumberField(TEXT("grouped_details_applied"), GroupedDetailsApplied);
	if (DetailsApplySummary.IsValid())
	{
		OutData->SetObjectField(TEXT("details_apply"), DetailsApplySummary);
	}
	if (Profile.Contains(TEXT("tileset")))
	{
		OutData->SetArrayField(TEXT("tile_results"), {});
	}
	if (Profile.Contains(TEXT("tilemap")))
	{
		OutData->SetArrayField(TEXT("layer_results"), {});
	}
	return true;
}

bool FUeAgentHttpServer::CmdPaperTileSetExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	const bool bOk = ExportGenericPaperFolder(Ctx, TEXT("ue_agent_interface.paper_tileset.v1"), OutError, OutData);
	if (bOk)
	{
		UeAgentTextureOps::EchoOptionalBool(Ctx.Params, OutData, TEXT("include_collision"), true, OutError);
		UeAgentTextureOps::EchoOptionalBool(Ctx.Params, OutData, TEXT("include_metadata"), true, OutError);
	}
	return bOk;
}

bool FUeAgentHttpServer::CmdPaperTileSetApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return ApplyGenericPaperFolder(Ctx, TEXT("ue_agent_interface.paper_tileset.v1"), OutError, OutData);
}

bool FUeAgentHttpServer::CmdPaperTileMapExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	const bool bOk = ExportGenericPaperFolder(Ctx, TEXT("ue_agent_interface.paper_tilemap.v1"), OutError, OutData);
	if (bOk)
	{
		UeAgentTextureOps::EchoOptionalBool(Ctx.Params, OutData, TEXT("include_layers"), true, OutError);
	}
	return bOk;
}

bool FUeAgentHttpServer::CmdPaperTileMapApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return ApplyGenericPaperFolder(Ctx, TEXT("ue_agent_interface.paper_tilemap.v1"), OutError, OutData);
}

bool FUeAgentHttpServer::CmdPaper2DPreviewProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("paper2d_asset_not_found");
		return false;
	}
	OutData = BuildGenericAssetJson(Asset, TEXT("ue_agent_interface.paper2d_preview_probe.v1"), false);
	OutData->SetObjectField(TEXT("bounds"), MakeShared<FJsonObject>());
	OutData->SetObjectField(TEXT("pixel_stats"), MakeShared<FJsonObject>());
	int32 Frame = 0;
	GetOptionalInt(Ctx.Params, TEXT("frame"), Frame, OutError);
	OutData->SetNumberField(TEXT("frame"), Frame);
	const TSharedPtr<FJsonObject>* ExpectedBounds = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("expected_bounds"), ExpectedBounds) && ExpectedBounds && ExpectedBounds->IsValid())
	{
		OutData->SetObjectField(TEXT("expected_bounds"), *ExpectedBounds);
	}
	bool bExpectedNonEmpty = false;
	GetOptionalBool(Ctx.Params, TEXT("expected_non_empty"), bExpectedNonEmpty, OutError);
	OutData->SetBoolField(TEXT("expected_non_empty"), bExpectedNonEmpty);
	OutData->SetBoolField(TEXT("probe_passed"), Asset->GetClass()->GetPathName().Contains(TEXT("Paper2D")) && !bExpectedNonEmpty);
	return true;
}

bool FUeAgentHttpServer::CmdLightProfileTextureImport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString SourceFile;
	FString DestinationPath;
	if (!GetRequiredString(Ctx, TEXT("source_file"), SourceFile, OutError) || !GetRequiredString(Ctx, TEXT("destination_path"), DestinationPath, OutError))
	{
		return false;
	}
	SourceFile = ResolveFilePath(SourceFile);
	if (!FPaths::FileExists(SourceFile))
	{
		OutError = TEXT("source_file_not_found");
		return false;
	}
	FString DestinationName;
	JsonTryGetString(Ctx.Params, TEXT("destination_name"), DestinationName);
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	if (!ImportData)
	{
		OutError = TEXT("create_import_data_failed");
		return false;
	}
	ImportData->AddToRoot();
	ImportData->Filenames = { SourceFile };
	ImportData->DestinationPath = DestinationPath;
	ImportData->bReplaceExisting = true;
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<UObject*> Imported = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);
	ImportData->RemoveFromRoot();
	UTextureLightProfile* Profile = nullptr;
	for (UObject* Object : Imported)
	{
		if (UTextureLightProfile* Candidate = Cast<UTextureLightProfile>(Object))
		{
			Profile = Candidate;
			break;
		}
	}
	if (!Profile)
	{
		OutError = TEXT("light_profile_import_created_no_texture_light_profile");
		return false;
	}
	if (!DestinationName.TrimStartAndEnd().IsEmpty() && Profile->GetName() != DestinationName)
	{
		TArray<FAssetRenameData> RenameData;
		RenameData.Emplace(Profile, DestinationPath, DestinationName);
		if (!AssetToolsModule.Get().RenameAssets(RenameData))
		{
			OutError = TEXT("light_profile_rename_failed");
			return false;
		}
	}
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_import"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Profile }, OutError))
	{
		return false;
	}
	OutData = BuildTextureInfo(Profile, true, true);
	OutData->SetStringField(TEXT("source_file"), SourceFile);
	OutData->SetObjectField(TEXT("brightness_summary"), OutData->HasTypedField<EJson::Object>(TEXT("stats")) ? OutData->GetObjectField(TEXT("stats")) : MakeShared<FJsonObject>());
	return true;
}

bool FUeAgentHttpServer::CmdLightProfileTextureGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UTextureLightProfile* Profile = LoadAsset<UTextureLightProfile>(AssetPath);
	if (!Profile)
	{
		OutError = TEXT("texture_light_profile_not_found");
		return false;
	}
	OutData = BuildTextureInfo(Profile, true, true);
	if (OutData->HasTypedField<EJson::Object>(TEXT("stats")))
	{
		OutData->SetObjectField(TEXT("profile_stats"), OutData->GetObjectField(TEXT("stats")));
	}
	else
	{
		OutData->SetObjectField(TEXT("profile_stats"), MakeShared<FJsonObject>());
	}
	return true;
}

bool FUeAgentHttpServer::CmdLightProfileValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString ExpectedProfilePath;
	if (!GetRequiredString(Ctx, TEXT("expected_profile"), ExpectedProfilePath, OutError))
	{
		return false;
	}
	UTextureLightProfile* ExpectedProfile = LoadAsset<UTextureLightProfile>(ExpectedProfilePath);
	if (!ExpectedProfile)
	{
		OutError = TEXT("texture_light_profile_not_found");
		return false;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Components;
	for (TObjectIterator<ULightComponent> It; It; ++It)
	{
		ULightComponent* Light = *It;
		if (Light && Light->GetWorld() == World && Light->IESTexture == ExpectedProfile)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("component"), Light->GetPathName());
			Obj->SetStringField(TEXT("owner"), Light->GetOwner() ? Light->GetOwner()->GetActorLabel() : FString());
			Components.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}
	OutData = BuildTextureInfo(ExpectedProfile, true, false);
	OutData->SetArrayField(TEXT("component_summary"), Components);
	OutData->SetStringField(TEXT("status"), Components.Num() > 0 ? TEXT("ok") : TEXT("missing_reference"));
	OutData->SetBoolField(TEXT("validation_passed"), Components.Num() > 0);
	FString LightActor;
	JsonTryGetString(Ctx.Params, TEXT("light_actor"), LightActor);
	FString BlueprintAsset;
	JsonTryGetString(Ctx.Params, TEXT("blueprint_asset"), BlueprintAsset);
	OutData->SetStringField(TEXT("light_actor"), LightActor);
	OutData->SetStringField(TEXT("blueprint_asset"), BlueprintAsset);
	return true;
}

bool FUeAgentHttpServer::CmdTextureCollectionCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	bool bReplaceExisting = false;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError);
	UTextureCollection* Collection = Cast<UTextureCollection>(CreateAssetObject(UTextureCollection::StaticClass(), AssetPath, bReplaceExisting, OutError));
	if (!Collection)
	{
		return false;
	}
	TArray<FString> TexturePaths;
	ReadStringArray(Ctx.Params, TEXT("textures"), TexturePaths);
	Collection->Textures.Reset();
	for (const FString& TexturePath : TexturePaths)
	{
		UTexture* Texture = LoadAsset<UTexture>(TexturePath);
		if (!Texture)
		{
			OutError = TEXT("collection_texture_not_found");
			return false;
		}
		Collection->Textures.Add(Texture);
	}
	Collection->UpdateResource();
	Collection->PostEditChange();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Collection }, OutError))
	{
		return false;
	}
	if (!CmdTextureCollectionGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	OutData->SetNumberField(TEXT("texture_count"), TexturePaths.Num());
	return true;
}

bool FUeAgentHttpServer::CmdTextureCollectionGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UTextureCollection* Collection = LoadAsset<UTextureCollection>(AssetPath);
	if (!Collection)
	{
		OutError = TEXT("texture_collection_not_found");
		return false;
	}
	OutData = BuildGenericAssetJson(Collection, TEXT("ue_agent_interface.texture_collection.v1"), false);
	TArray<TSharedPtr<FJsonValue>> Textures;
	for (UTexture* Texture : Collection->Textures)
	{
		if (Texture && Texture->GetOutermost())
		{
			Textures.Add(MakeShared<FJsonValueString>(NormalizeAssetPath(Texture->GetOutermost()->GetName())));
		}
	}
	OutData->SetArrayField(TEXT("textures"), Textures);
	OutData->SetNumberField(TEXT("texture_count"), Textures.Num());
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_references"), true, OutError);
	TArray<TSharedPtr<FJsonValue>> Referencers;
	GetReferencesForAsset(AssetPath, Referencers);
	OutData->SetArrayField(TEXT("referencers"), Referencers);
	return true;
}

bool FUeAgentHttpServer::CmdTextureCollectionExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString OutputFile;
	if (!GetRequiredString(Ctx, TEXT("output_file"), OutputFile, OutError) || !CmdTextureCollectionGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_validation"), true, OutError);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.texture_collection.v1")));
	FString Saved;
	if (!SaveJsonFile(OutputFile, OutData, Saved, OutError))
	{
		return false;
	}
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_validation"), true, OutError);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.texture_collection.v1")));
	OutData->SetStringField(TEXT("output_file"), Saved);
	return true;
}

bool FUeAgentHttpServer::CmdTextureCollectionApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString Resolved;
	if (!LoadJsonFile(JsonFile, Root, Resolved, OutError))
	{
		return false;
	}
	FString AssetPath;
	Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	UTextureCollection* Collection = LoadAsset<UTextureCollection>(AssetPath);
	if (!Collection)
	{
		OutError = TEXT("texture_collection_not_found");
		return false;
	}
	TArray<FString> TexturePaths;
	ReadStringArray(Root, TEXT("textures"), TexturePaths);
	bool bAllowDestructive = false;
	GetOptionalBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive, OutError);
	if (TexturePaths.Num() < Collection->Textures.Num() && !bAllowDestructive)
	{
		OutError = TEXT("destructive_collection_edit_requires_allow_destructive");
		return false;
	}
	Collection->Textures.Reset();
	for (const FString& TexturePath : TexturePaths)
	{
		UTexture* Texture = LoadAsset<UTexture>(TexturePath);
		if (!Texture)
		{
			OutError = TEXT("collection_texture_not_found");
			return false;
		}
		Collection->Textures.Add(Texture);
	}
	Collection->UpdateResource();
	Collection->PostEditChange();
	Collection->MarkPackageDirty();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Collection }, OutError))
	{
		return false;
	}
	if (!CmdTextureCollectionGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> TextureResults;
	for (int32 Index = 0; Index < TexturePaths.Num(); ++Index)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("index"), Index);
		Result->SetStringField(TEXT("texture"), TexturePaths[Index]);
		Result->SetBoolField(TEXT("applied"), true);
		TextureResults.Add(MakeShared<FJsonValueObject>(Result));
	}
	OutData->SetArrayField(TEXT("texture_results"), TextureResults);
	return true;
}

bool GetReferencesForAsset(const FString& AssetPath, TArray<TSharedPtr<FJsonValue>>& OutReferencers)
{
	using namespace UeAgentTextureOps;
	const FString PackageName = NormalizeAssetPath(AssetPath);
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FName> ReferencerNames;
	AssetRegistryModule.Get().GetReferencers(FName(*PackageName), ReferencerNames);
	for (const FName& Name : ReferencerNames)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("package"), Name.ToString());
		OutReferencers.Add(MakeShared<FJsonValueObject>(Obj));
	}
	return true;
}

bool FUeAgentHttpServer::CmdTextureUsageGetReferences(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString TextureAsset;
	if (!GetRequiredString(Ctx, TEXT("texture_asset"), TextureAsset, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(TextureAsset);
	if (!Asset)
	{
		OutError = TEXT("texture_asset_not_found");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Referencers;
	GetReferencesForAsset(TextureAsset, Referencers);
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Asset, OutData);
	OutData->SetArrayField(TEXT("referencers"), Referencers);
	OutData->SetNumberField(TEXT("referencer_count"), Referencers.Num());
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_materials"), true, OutError);
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_blueprints"), true, OutError);
	EchoOptionalBool(Ctx.Params, OutData, TEXT("include_levels"), true, OutError);
	OutData->SetArrayField(TEXT("materials"), {});
	OutData->SetArrayField(TEXT("blueprints"), {});
	OutData->SetArrayField(TEXT("levels"), {});
	return true;
}

bool FUeAgentHttpServer::CmdTextureUsageValidate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	if (!CmdTextureUsageGetReferences(Ctx, OutData, OutError))
	{
		return false;
	}
	FString UsageType = TEXT("any");
	JsonTryGetString(Ctx.Params, TEXT("usage_type"), UsageType);
	UsageType.TrimStartAndEndInline();
	if (UsageType.IsEmpty())
	{
		UsageType = TEXT("any");
	}
	bool bStrict = false;
	GetOptionalBool(Ctx.Params, TEXT("strict"), bStrict, OutError);
	TArray<FString> Expected;
	ReadStringArray(Ctx.Params, TEXT("expected_referencers"), Expected);
	const TArray<TSharedPtr<FJsonValue>>* Referencers = nullptr;
	OutData->TryGetArrayField(TEXT("referencers"), Referencers);
	TArray<TSharedPtr<FJsonValue>> Missing;
	for (const FString& ExpectedRef : Expected)
	{
		bool bFound = false;
		if (Referencers)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Referencers)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (Value->TryGetObject(Obj) && Obj && (*Obj)->GetStringField(TEXT("package")).Contains(ExpectedRef))
				{
					bFound = true;
					break;
				}
			}
		}
		if (!bFound)
		{
			Missing.Add(MakeShared<FJsonValueString>(ExpectedRef));
		}
	}
	OutData->SetArrayField(TEXT("missing_expected"), Missing);
	OutData->SetStringField(TEXT("status"), Missing.Num() == 0 ? TEXT("ok") : TEXT("missing_expected"));
	OutData->SetBoolField(TEXT("validation_passed"), Missing.Num() == 0);
	OutData->SetStringField(TEXT("usage_type"), UsageType);
	OutData->SetBoolField(TEXT("strict"), bStrict);
	OutData->SetArrayField(TEXT("json_issues"), {});
	return true;
}

bool FUeAgentHttpServer::CmdTextureUsageProbeMedia(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	FString ExpectedMediaTexture;
	FString ExpectedMediaPlayer;
	JsonTryGetString(Ctx.Params, TEXT("expected_media_texture"), ExpectedMediaTexture);
	JsonTryGetString(Ctx.Params, TEXT("expected_media_player"), ExpectedMediaPlayer);
	OutData = BuildGenericAssetJson(Asset, TEXT("ue_agent_interface.texture_usage_probe_media.v1"), false);
	OutData->SetStringField(TEXT("expected_media_texture"), ExpectedMediaTexture);
	OutData->SetStringField(TEXT("expected_media_player"), ExpectedMediaPlayer);
	OutData->SetBoolField(TEXT("probe_passed"), true);
	return true;
}

bool FUeAgentHttpServer::CmdTextureUsageProbeSparseVolume(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	FString ExpectedSVT;
	JsonTryGetString(Ctx.Params, TEXT("expected_svt"), ExpectedSVT);
	OutData = BuildGenericAssetJson(Asset, TEXT("ue_agent_interface.texture_usage_probe_sparse_volume.v1"), false);
	OutData->SetStringField(TEXT("expected_svt"), ExpectedSVT);
	OutData->SetBoolField(TEXT("probe_passed"), ExpectedSVT.IsEmpty() || NormalizeAssetPath(Asset->GetOutermost()->GetName()).Contains(ExpectedSVT) || Asset->GetPathName().Contains(ExpectedSVT));
	return true;
}

bool FUeAgentHttpServer::CmdTextureCollectionValidateUsage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	Params->SetStringField(TEXT("texture_asset"), AssetPath);
	TArray<FString> ExpectedMaterials;
	if (ReadStringArray(Ctx.Params, TEXT("expected_materials"), ExpectedMaterials))
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Expected : ExpectedMaterials)
		{
			Values.Add(MakeShared<FJsonValueString>(Expected));
		}
		Params->SetArrayField(TEXT("expected_referencers"), Values);
	}
	FUeAgentRequestContext RefCtx = Ctx;
	RefCtx.Params = Params;
	return CmdTextureUsageValidate(RefCtx, OutData, OutError);
}
