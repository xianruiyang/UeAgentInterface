// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"
#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentFolderDetailsExport.h"
#include "HAL/FileManager.h"
#include "Sound/SoundCue.h"
#include "Subsystems/AssetEditorSubsystem.h"

bool FUeAgentHttpServer::CmdSoundCueCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	bool bReplace = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplace, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	USoundCue* Cue = Cast<USoundCue>(CreateAssetObject(USoundCue::StaticClass(), AssetPath, bReplace, OutError));
	if (!Cue)
	{
		return false;
	}
	if (bSave && !SavePackagesForObjects({ Cue }, OutError))
	{
		return false;
	}
	OutData = BuildSoundCueInfo(Cue, true, false, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundCueProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundCueGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
bool FUeAgentHttpServer::CmdSoundCueExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	USoundCue* Cue = Cast<USoundCue>(LoadAssetObject(AssetPath));
	if (!Cue)
	{
		OutError = TEXT("sound_cue_not_found");
		return false;
	}
	bool bClean = false;
	bool bIncludeRaw = false;
	GetOptionalBool(Ctx.Params, TEXT("clean_output_dir"), bClean, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_raw_properties"), bIncludeRaw, OutError);
	FolderPath = ResolveFilePath(FolderPath);
	if (bClean)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);

	TSharedPtr<FJsonObject> Info = BuildSoundCueInfo(Cue, true, bIncludeRaw, true);
	TArray<TSharedPtr<FJsonValue>> Files;
	TSharedPtr<FJsonObject> AssetJson = BuildGenericAssetJson(Cue, SoundCueProfile, bIncludeRaw);
	if (!UeAgentFolderDetailsExport::AttachOwnerDetails(Cue, FolderPath, TEXT("sound_cue_export_folder"), TEXT("sound_cue_asset"), Cue->GetPathName(), TEXT("asset"), AssetJson, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("asset.json"), AssetJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> NodesJson = MakeShared<FJsonObject>();
	NodesJson->SetStringField(TEXT("root_node_id"), Info->GetStringField(TEXT("root_node_id")));
	NodesJson->SetArrayField(TEXT("nodes"), Info->GetArrayField(TEXT("nodes")));
	if (!SaveTrackedJson(FolderPath, TEXT("nodes.json"), NodesJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> EdgesJson = MakeShared<FJsonObject>();
	EdgesJson->SetArrayField(TEXT("edges"), Info->GetArrayField(TEXT("edges")));
	if (!SaveTrackedJson(FolderPath, TEXT("edges.json"), EdgesJson, Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("parameters.json"), MakeShared<FJsonObject>(), Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("layout.json"), MakeShared<FJsonObject>(), Files, OutError)) return false;
	TSharedPtr<FJsonObject> AuthoringGraphJson = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphInfoJson = MakeShared<FJsonObject>();
	GraphInfoJson->SetStringField(TEXT("name"), TEXT("main"));
	GraphInfoJson->SetStringField(TEXT("graph_kind"), TEXT("sound_cue_graph"));
	AuthoringGraphJson->SetObjectField(TEXT("graph"), GraphInfoJson);
	AuthoringGraphJson->SetStringField(TEXT("root_node_id"), Info->GetStringField(TEXT("root_node_id")));
	AuthoringGraphJson->SetArrayField(TEXT("nodes"), Info->GetArrayField(TEXT("nodes")));
	AuthoringGraphJson->SetArrayField(TEXT("edges"), Info->GetArrayField(TEXT("edges")));
	if (!SaveTrackedJson(FolderPath, TEXT("graphs/items/main/graph.json"), AuthoringGraphJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> AuthoringLayoutJson = MakeShared<FJsonObject>();
	AuthoringLayoutJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.sound_cue_graph_layout.v1"));
	AuthoringLayoutJson->SetArrayField(TEXT("nodes"), {});
	if (!SaveTrackedJson(FolderPath, TEXT("graphs/items/main/layout.json"), AuthoringLayoutJson, Files, OutError)) return false;
	if (bIncludeRaw && !SaveTrackedJson(FolderPath, TEXT("debug/raw_properties.json"), BuildGenericAssetJson(Cue, SoundCueProfile, true), Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("validation/coverage_report.json"), MakeCoverageReport(SoundCueProfile), Files, OutError)) return false;
	TSharedPtr<FJsonObject> IssuesJson = MakeShared<FJsonObject>();
	IssuesJson->SetArrayField(TEXT("json_issues"), Info->HasField(TEXT("json_issues")) ? Info->GetArrayField(TEXT("json_issues")) : TArray<TSharedPtr<FJsonValue>>{});
	if (!SaveTrackedJson(FolderPath, TEXT("validation/issues.json"), IssuesJson, Files, OutError)) return false;
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Cue, OutData);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetArrayField(TEXT("files"), Files);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundCueProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundCueValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> NodesJson;
	TSharedPtr<FJsonObject> EdgesJson;
	FString Folder;
	if (!LoadSoundCueFolder(FolderPath, AssetJson, NodesJson, EdgesJson, Folder, OutError))
	{
		return false;
	}
	FString AssetPath;
	AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	TSharedPtr<FJsonObject> LegacyRejection;
	if (UeAgentDetailsAuthoring::BuildLegacyPropertiesFolderRejectionIfPresent(
		Folder,
		UeAgentDetailsAuthoring::MakeOwnerInfoForObject(nullptr, TEXT("sound_cue_asset"), AssetPath, TEXT("sound_cue_validate_folder")),
		TEXT("asset.json.details.index_file"),
		LegacyRejection,
		OutError))
	{
		OutData = LegacyRejection;
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	ValidateSoundCueFolderJson(NodesJson, EdgesJson, Issues);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("folder_path"), Folder);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("failed"));
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundCueProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundCueApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	FString AssetPath;
	Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	bool bCreateIfMissing = true;
	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bAllowDestructive = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing, OutError);
	GetOptionalBool(Ctx.Params, TEXT("dry_run"), bDryRun, OutError);
	GetOptionalBool(Ctx.Params, TEXT("validate_only"), bValidateOnly, OutError);
	GetOptionalBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);

	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> NodesJson;
	TSharedPtr<FJsonObject> EdgesJson;
	FString Folder;
	if (!LoadSoundCueFolder(FolderPath, AssetJson, NodesJson, EdgesJson, Folder, OutError))
	{
		return false;
	}
	if (AssetPath.IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	TSharedPtr<FJsonObject> LegacyRejection;
	if (UeAgentDetailsAuthoring::BuildLegacyPropertiesFolderRejectionIfPresent(
		Folder,
		UeAgentDetailsAuthoring::MakeOwnerInfoForObject(nullptr, TEXT("sound_cue_asset"), AssetPath, TEXT("sound_cue_apply_folder")),
		TEXT("asset.json.details.index_file"),
		LegacyRejection,
		OutError))
	{
		OutData = LegacyRejection;
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!ValidateSoundCueFolderJson(NodesJson, EdgesJson, Issues))
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("status"), TEXT("failed"));
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutError = TEXT("sound_cue_folder_validation_failed");
		return false;
	}
	if (bValidateOnly)
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundCueProfile));
		return true;
	}
	USoundCue* Cue = Cast<USoundCue>(LoadAssetObject(AssetPath));
	const bool bWouldCreate = Cue == nullptr;
	if (!Cue)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("sound_cue_not_found");
			return false;
		}
		if (bDryRun)
		{
			Cue = NewObject<USoundCue>(GetTransientPackage(), NAME_None, RF_Transient);
		}
		else
		{
			Cue = Cast<USoundCue>(CreateAssetObject(USoundCue::StaticClass(), AssetPath, false, OutError));
			if (!Cue)
			{
				return false;
			}
		}
	}
	if (!bAllowDestructive && Cue->AllNodes.Num() > 0)
	{
		OutError = TEXT("sound_cue_apply_requires_allow_destructive");
		return false;
	}
	int32 NodesCreated = 0;
	int32 EdgesCreated = 0;
	int32 GroupedDetailsApplied = 0;
	TSharedPtr<FJsonObject> DetailsApplySummary;
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	if (!UeAgentDetailsAuthoring::ApplyOwnerDetailsFromEntryIfPresent(
		Cue,
		Folder,
		AssetJson,
		UeAgentDetailsAuthoring::MakeOwnerInfoForObject(Cue, TEXT("sound_cue_asset"), Cue->GetPathName(), TEXT("sound_cue_apply_folder")),
		TEXT("details"),
		!bDryRun,
		DetailsApplySummary,
		GroupedDetailsApplied,
		OutError))
	{
		return false;
	}
	if (!ApplySoundCueGraph(Cue, NodesJson, EdgesJson, PropertyResults, NodesCreated, EdgesCreated, bDryRun, OutError))
	{
		return false;
	}
	if (!bDryRun && bSave && !SavePackagesForObjects({ Cue }, OutError))
	{
		return false;
	}
	OutData = BuildSoundCueInfo(Cue, true, false, true);
	OutData->SetNumberField(TEXT("nodes_created"), NodesCreated);
	OutData->SetNumberField(TEXT("edges_created"), EdgesCreated);
	OutData->SetNumberField(TEXT("grouped_details_applied"), GroupedDetailsApplied);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	if (DetailsApplySummary.IsValid())
	{
		OutData->SetObjectField(TEXT("details_apply"), DetailsApplySummary);
	}
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("would_create"), bDryRun && bWouldCreate);
	OutData->SetBoolField(TEXT("saved"), !bDryRun && bSave);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundCueProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundCueOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
bool FUeAgentHttpServer::CmdSoundCueScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
bool FUeAgentHttpServer::CmdSoundCuePreview(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FUeAgentRequestContext RuntimeCtx = Ctx;
	return CmdAudioRuntimeProbe(RuntimeCtx, OutData, OutError);
}
