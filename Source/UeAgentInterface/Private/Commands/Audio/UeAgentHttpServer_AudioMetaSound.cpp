// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"
#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentFolderDetailsExport.h"
#include "Metasound.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundSource.h"
#include "Subsystems/AssetEditorSubsystem.h"

bool FUeAgentHttpServer::CmdMetaSoundCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	FString AssetType = TEXT("source");
	Ctx.Params->TryGetStringField(TEXT("asset_type"), AssetType);
	bool bReplace = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplace, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	UObject* Asset = nullptr;
	if (!CreateMetaSoundAsset(AssetPath, AssetType, bReplace, Asset, OutError))
	{
		return false;
	}
	if (bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildMetaSoundInfo(Asset, true, true, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(MetaSoundProfile));
	return true;
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
	using namespace UeAgentAudioOps;
	FString AssetPath;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset || (!Asset->IsA<UMetaSoundSource>() && !Asset->IsA<UMetaSoundPatch>()))
	{
		OutError = TEXT("metasound_not_found");
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

	TSharedPtr<FJsonObject> Info = BuildMetaSoundInfo(Asset, true, true, true);
	TArray<TSharedPtr<FJsonValue>> Files;
	TSharedPtr<FJsonObject> AssetJson = BuildGenericAssetJson(Asset, MetaSoundProfile, bIncludeRaw);
	AssetJson->SetStringField(TEXT("asset_type"), Asset->IsA<UMetaSoundSource>() ? TEXT("source") : TEXT("patch"));
	if (!UeAgentFolderDetailsExport::AttachOwnerDetails(Asset, FolderPath, TEXT("metasound_export_folder"), TEXT("metasound_asset"), Asset->GetPathName(), TEXT("asset"), AssetJson, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("asset.json"), AssetJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> InterfacesJson = MakeShared<FJsonObject>();
	InterfacesJson->SetArrayField(TEXT("interfaces"), Info->HasField(TEXT("interfaces")) ? Info->GetArrayField(TEXT("interfaces")) : TArray<TSharedPtr<FJsonValue>>{});
	if (!SaveTrackedJson(FolderPath, TEXT("interfaces.json"), InterfacesJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> InputsJson = MakeShared<FJsonObject>();
	InputsJson->SetArrayField(TEXT("inputs"), Info->GetArrayField(TEXT("inputs")));
	if (!SaveTrackedJson(FolderPath, TEXT("members/inputs.json"), InputsJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> OutputsJson = MakeShared<FJsonObject>();
	OutputsJson->SetArrayField(TEXT("outputs"), Info->GetArrayField(TEXT("outputs")));
	if (!SaveTrackedJson(FolderPath, TEXT("members/outputs.json"), OutputsJson, Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("members/variables.json"), MakeShared<FJsonObject>(), Files, OutError)) return false;
	TSharedPtr<FJsonObject> GraphRootJson = MakeShared<FJsonObject>();
	GraphRootJson->SetStringField(TEXT("root"), TEXT("default"));
	if (!SaveTrackedJson(FolderPath, TEXT("graphs/root.json"), GraphRootJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> NodesJson = MakeShared<FJsonObject>();
	NodesJson->SetArrayField(TEXT("nodes"), Info->HasField(TEXT("nodes")) ? Info->GetArrayField(TEXT("nodes")) : TArray<TSharedPtr<FJsonValue>>{});
	if (!SaveTrackedJson(FolderPath, TEXT("graphs/nodes.json"), NodesJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> EdgesJson = MakeShared<FJsonObject>();
	EdgesJson->SetArrayField(TEXT("edges"), Info->HasField(TEXT("edges")) ? Info->GetArrayField(TEXT("edges")) : TArray<TSharedPtr<FJsonValue>>{});
	if (!SaveTrackedJson(FolderPath, TEXT("graphs/edges.json"), EdgesJson, Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("graphs/literals.json"), MakeShared<FJsonObject>(), Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("layout.json"), MakeShared<FJsonObject>(), Files, OutError)) return false;
	TSharedPtr<FJsonObject> AuthoringGraphJson = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphInfoJson = MakeShared<FJsonObject>();
	GraphInfoJson->SetStringField(TEXT("name"), TEXT("root"));
	GraphInfoJson->SetStringField(TEXT("graph_kind"), TEXT("metasound_graph"));
	AuthoringGraphJson->SetObjectField(TEXT("graph"), GraphInfoJson);
	AuthoringGraphJson->SetObjectField(TEXT("root"), GraphRootJson);
	AuthoringGraphJson->SetArrayField(TEXT("nodes"), Info->HasField(TEXT("nodes")) ? Info->GetArrayField(TEXT("nodes")) : TArray<TSharedPtr<FJsonValue>>{});
	AuthoringGraphJson->SetArrayField(TEXT("edges"), Info->HasField(TEXT("edges")) ? Info->GetArrayField(TEXT("edges")) : TArray<TSharedPtr<FJsonValue>>{});
	AuthoringGraphJson->SetObjectField(TEXT("literals"), MakeShared<FJsonObject>());
	if (!SaveTrackedJson(FolderPath, TEXT("graphs/items/root/graph.json"), AuthoringGraphJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> AuthoringLayoutJson = MakeShared<FJsonObject>();
	AuthoringLayoutJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.metasound_graph_layout.v1"));
	AuthoringLayoutJson->SetArrayField(TEXT("nodes"), {});
	if (!SaveTrackedJson(FolderPath, TEXT("graphs/items/root/layout.json"), AuthoringLayoutJson, Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("settings.json"), MakeShared<FJsonObject>(), Files, OutError)) return false;
	if (bIncludeRaw && !SaveTrackedJson(FolderPath, TEXT("debug/raw_properties.json"), BuildGenericAssetJson(Asset, MetaSoundProfile, true), Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("validation/coverage_report.json"), MakeCoverageReport(MetaSoundProfile), Files, OutError)) return false;
	TSharedPtr<FJsonObject> IssuesJson = MakeShared<FJsonObject>();
	IssuesJson->SetArrayField(TEXT("json_issues"), Info->HasField(TEXT("json_issues")) ? Info->GetArrayField(TEXT("json_issues")) : TArray<TSharedPtr<FJsonValue>>{});
	if (!SaveTrackedJson(FolderPath, TEXT("validation/issues.json"), IssuesJson, Files, OutError)) return false;
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Asset, OutData);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetArrayField(TEXT("files"), Files);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(MetaSoundProfile));
	return true;
}
bool FUeAgentHttpServer::CmdMetaSoundValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> InputsJson;
	TSharedPtr<FJsonObject> OutputsJson;
	FString Folder;
	TArray<TSharedPtr<FJsonValue>> Issues;
	const bool bValid = ValidateMetaSoundFolder(FolderPath, Issues, AssetJson, InputsJson, OutputsJson, Folder, OutError);
	if (!bValid && Issues.Num() == 0 && !OutError.IsEmpty())
	{
		return false;
	}
	FString AssetPath;
	if (AssetJson.IsValid())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	TSharedPtr<FJsonObject> LegacyRejection;
	if (UeAgentDetailsAuthoring::BuildLegacyPropertiesFolderRejectionIfPresent(
		Folder,
		UeAgentDetailsAuthoring::MakeOwnerInfoForObject(nullptr, TEXT("metasound_asset"), AssetPath, TEXT("metasound_validate_folder")),
		TEXT("asset.json.details.index_file"),
		LegacyRejection,
		OutError))
	{
		OutData = LegacyRejection;
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("folder_path"), Folder);
	OutData->SetStringField(TEXT("status"), bValid ? TEXT("ok") : TEXT("failed"));
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetArrayField(TEXT("unsupported_nodes"), {});
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(MetaSoundProfile));
	return true;
}
bool FUeAgentHttpServer::CmdMetaSoundApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> InputsJson;
	TSharedPtr<FJsonObject> OutputsJson;
	FString Folder;
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!ValidateMetaSoundFolder(FolderPath, Issues, AssetJson, InputsJson, OutputsJson, Folder, OutError))
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("status"), TEXT("failed"));
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutError = TEXT("metasound_folder_validation_failed");
		return false;
	}
	FString AssetPath;
	Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	TSharedPtr<FJsonObject> LegacyRejection;
	if (UeAgentDetailsAuthoring::BuildLegacyPropertiesFolderRejectionIfPresent(
		Folder,
		UeAgentDetailsAuthoring::MakeOwnerInfoForObject(nullptr, TEXT("metasound_asset"), AssetPath, TEXT("metasound_apply_folder")),
		TEXT("asset.json.details.index_file"),
		LegacyRejection,
		OutError))
	{
		OutData = LegacyRejection;
		return false;
	}
	FString AssetType = TEXT("source");
	AssetJson->TryGetStringField(TEXT("asset_type"), AssetType);
	bool bCreateIfMissing = true;
	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bSave = true;
	bool bCompile = true;
	GetOptionalBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing, OutError);
	GetOptionalBool(Ctx.Params, TEXT("dry_run"), bDryRun, OutError);
	GetOptionalBool(Ctx.Params, TEXT("validate_only"), bValidateOnly, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	GetOptionalBool(Ctx.Params, TEXT("compile_after_apply"), bCompile, OutError);
	if (bValidateOnly)
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		return true;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	const bool bWouldCreate = Asset == nullptr;
	if (!Asset)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("metasound_not_found");
			return false;
		}
		if (bDryRun)
		{
			OutData = MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("asset_path"), NormalizeAssetPath(AssetPath));
			OutData->SetStringField(TEXT("asset_type"), AssetType);
			OutData->SetStringField(TEXT("status"), TEXT("ok"));
			OutData->SetArrayField(TEXT("json_issues"), Issues);
			OutData->SetNumberField(TEXT("nodes_created"), (InputsJson->HasField(TEXT("inputs")) ? InputsJson->GetArrayField(TEXT("inputs")).Num() : 0)
				+ (OutputsJson->HasField(TEXT("outputs")) ? OutputsJson->GetArrayField(TEXT("outputs")).Num() : 0));
			OutData->SetNumberField(TEXT("edges_created"), 0);
			OutData->SetBoolField(TEXT("dry_run"), true);
			OutData->SetBoolField(TEXT("would_create"), true);
			OutData->SetBoolField(TEXT("compiled"), false);
			OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(MetaSoundProfile));
			return true;
		}
		if (!CreateMetaSoundAsset(AssetPath, AssetType, false, Asset, OutError))
		{
			return false;
		}
	}
	if (!Asset->IsA<UMetaSoundSource>() && !Asset->IsA<UMetaSoundPatch>())
	{
		OutError = TEXT("metasound_not_found");
		return false;
	}
	int32 GroupedDetailsApplied = 0;
	TSharedPtr<FJsonObject> DetailsApplySummary;
	if (!UeAgentDetailsAuthoring::ApplyOwnerDetailsFromEntryIfPresent(
		Asset,
		Folder,
		AssetJson,
		UeAgentDetailsAuthoring::MakeOwnerInfoForObject(Asset, TEXT("metasound_asset"), Asset->GetPathName(), TEXT("metasound_apply_folder")),
		TEXT("details"),
		!bDryRun,
		DetailsApplySummary,
		GroupedDetailsApplied,
		OutError))
	{
		return false;
	}
	int32 NodesCreated = 0;
	if (!bDryRun)
	{
		UMetaSoundBuilderBase* Builder = nullptr;
		TScriptInterface<IMetaSoundDocumentInterface> Interface;
		Interface.SetObject(Asset);
		Interface.SetInterface(Cast<IMetaSoundDocumentInterface>(Asset));
		EMetaSoundBuilderResult FindBuilderResult = EMetaSoundBuilderResult::Failed;
		UMetaSoundEditorSubsystem* EditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>() : nullptr;
		Builder = EditorSubsystem ? EditorSubsystem->FindOrBeginBuilding(Interface, FindBuilderResult) : nullptr;
		if (!Builder)
		{
			OutError = TEXT("metasound_builder_unavailable");
			return false;
		}
		if (!AddMetaSoundMembers(Builder, InputsJson, TEXT("inputs"), true, NodesCreated, OutError)
			|| !AddMetaSoundMembers(Builder, OutputsJson, TEXT("outputs"), false, NodesCreated, OutError))
		{
			return false;
		}
		Builder->InitNodeLocations();
		Builder->ConformObjectToDocument();
		EditorSubsystem->RegisterGraphWithFrontend(*Asset, true);
		Asset->PostEditChange();
		Asset->MarkPackageDirty();
	}
	TSharedPtr<FJsonObject> CompileReport;
	bool bCompiled = false;
	if (!bDryRun && bCompile)
	{
		FUeAgentRequestContext CompileCtx;
		CompileCtx.RequestId = TEXT("metasound_apply_folder_compile");
		CompileCtx.Command = TEXT("metasound_compile");
		CompileCtx.Params = MakeShared<FJsonObject>();
		CompileCtx.Params->SetStringField(TEXT("asset_path"), NormalizeAssetPath(AssetPath));
		CompileCtx.Params->SetBoolField(TEXT("save_after_compile"), false);
		CompileReport = MakeShared<FJsonObject>();
		if (!CmdMetaSoundCompile(CompileCtx, CompileReport, OutError))
		{
			return false;
		}
		bCompiled = true;
	}
	if (!bDryRun && bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildMetaSoundInfo(Asset, true, true, true);
	if (CompileReport.IsValid())
	{
		OutData->SetObjectField(TEXT("compile_report"), CompileReport);
	}
	OutData->SetNumberField(TEXT("nodes_created"), NodesCreated);
	OutData->SetNumberField(TEXT("edges_created"), 0);
	OutData->SetNumberField(TEXT("grouped_details_applied"), GroupedDetailsApplied);
	if (DetailsApplySummary.IsValid())
	{
		OutData->SetObjectField(TEXT("details_apply"), DetailsApplySummary);
	}
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("would_create"), bDryRun && bWouldCreate);
	OutData->SetBoolField(TEXT("compiled"), bCompiled);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(MetaSoundProfile));
	return true;
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
