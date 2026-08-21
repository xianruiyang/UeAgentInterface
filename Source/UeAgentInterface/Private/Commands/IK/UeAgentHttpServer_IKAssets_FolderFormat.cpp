// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_IKAssets.h"

#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringApplyProvider.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringExport.h"
#include "Domains/Rigging/UeAgentIKDomainBackend.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "Utilities/UeAgentAuthoringFileIO.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

namespace UeAgentIKFolderOps
{
	static FString GetFolderRootAbsolute(const FString& Profile)
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), Profile));
	}

	static bool ResolveFolderForAsset(
		const FString& InAssetPath,
		const FString& Profile,
		FString& OutFolderPath,
		FString& OutError)
	{
		return UeAgentAuthoringWorkspacePaths::ResolveAssetWorkspaceFolder(
			GetFolderRootAbsolute(Profile),
			UeAgentRiggingAuthoring::NormalizePackagePath(InAssetPath),
			OutFolderPath,
			OutError);
	}

	static FUeAgentRequestContext MakeSubContext(
		const FString& RequestId,
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params)
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = RequestId;
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		return SubCtx;
	}

	static void CopyFieldIfPresent(
		const TSharedPtr<FJsonObject>& Source,
		const FString& SourceKey,
		const TSharedPtr<FJsonObject>& Dest,
		const FString& DestKey)
	{
		if (!Source.IsValid() || !Dest.IsValid()) return;
		if (const TSharedPtr<FJsonValue>* Value = Source->Values.Find(SourceKey))
			Dest->SetField(DestKey, *Value);
	}

	static TSharedRef<FJsonObject> MutationStatsJson(
		const UeAgentRiggingAuthoring::FMutationStats& Stats)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("goals"), Stats.Goals);
		Result->SetNumberField(TEXT("solvers"), Stats.Solvers);
		Result->SetNumberField(TEXT("chains"), Stats.Chains);
		Result->SetNumberField(TEXT("retarget_ops"), Stats.RetargetOps);
		Result->SetNumberField(TEXT("mappings"), Stats.Mappings);
		Result->SetNumberField(TEXT("poses"), Stats.Poses);
		return Result;
	}

	static TSharedRef<FJsonObject> ApplyResultJson(
		const UeAgentRiggingAuthoring::FAssetApplyResult& Result)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("status"), Result.Status);
		Json->SetStringField(TEXT("phase"), Result.Phase);
		Json->SetStringField(TEXT("resolved_operation"), Result.ResolvedOperation);
		Json->SetStringField(TEXT("asset_path"), Result.AssetPath);
		Json->SetStringField(TEXT("object_path"), Result.ObjectPath);
		Json->SetNumberField(TEXT("units_checked"), Result.UnitsChecked);
		Json->SetNumberField(TEXT("mutations_applied"), Result.MutationsApplied);
		Json->SetBoolField(TEXT("rollback_attempted"), Result.bRollbackAttempted);
		Json->SetBoolField(TEXT("rollback_succeeded"), Result.bRollbackSucceeded);
		Json->SetObjectField(TEXT("applied"), MutationStatsJson(Result.Stats));
		if (!Result.ErrorCode.IsEmpty()) Json->SetStringField(TEXT("error_code"), Result.ErrorCode);
		if (!Result.ErrorDetail.IsEmpty()) Json->SetStringField(TEXT("error_detail"), Result.ErrorDetail);
		return Json;
	}

	static TSharedRef<FJsonObject> InspectResultJson(
		const UeAgentRiggingAuthoring::FWorkspaceInspectResult& Result)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("status"), Result.Status);
		Json->SetStringField(TEXT("asset_path"), Result.AssetPath);
		Json->SetStringField(TEXT("requested_operation"), Result.RequestedOperation);
		Json->SetNumberField(TEXT("units_checked"), Result.UnitsChecked);
		Json->SetBoolField(TEXT("asset_exists"), Result.bAssetExists);
		Json->SetBoolField(TEXT("would_create"), Result.bWouldCreate);
		Json->SetBoolField(TEXT("no_op"), Result.bNoOp);
		Json->SetObjectField(TEXT("planned"), MutationStatsJson(Result.PlannedStats));
		if (!Result.ErrorCode.IsEmpty()) Json->SetStringField(TEXT("error_code"), Result.ErrorCode);
		if (!Result.ErrorDetail.IsEmpty()) Json->SetStringField(TEXT("error_detail"), Result.ErrorDetail);
		return Json;
	}

	static bool ResolveFolder(
		const FUeAgentRequestContext& Ctx,
		const FString& AssetPath,
		const FString& DefaultProfile,
		FString& OutFolderPath,
		FString& OutError)
	{
		if (Ctx.Params.IsValid()) Ctx.Params->TryGetStringField(TEXT("folder_path"), OutFolderPath);
		if (OutFolderPath.TrimStartAndEnd().IsEmpty()
			&& !ResolveFolderForAsset(AssetPath, DefaultProfile, OutFolderPath, OutError)) return false;
		OutFolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutFolderPath);
		return true;
	}
}

bool FUeAgentLegacyMechanics::CmdIKRigExportFolder(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	const UeAgentRiggingAuthoring::FProfile* Profile =
		UeAgentRiggingAuthoring::FindProfile(UeAgentRiggingAuthoring::EAssetKind::IKRig);
	UObject* Asset = UeAgentRiggingAuthoring::LoadAsset(Profile->Kind, AssetPath);
	if (!Asset)
	{
		OutError = TEXT("ik_rig_not_found");
		return false;
	}
	FString FolderPath;
	if (!UeAgentIKFolderOps::ResolveFolder(Ctx, AssetPath, TEXT("IKRig"), FolderPath, OutError)) return false;
	FString ExportProfile = TEXT("llm_authoring");
	JsonTryGetString(Ctx.Params, TEXT("profile"), ExportProfile);
	UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
	if (!UeAgentRiggingAuthoring::ExportAssetWorkspace(
		*Profile, *Asset, FolderPath, ExportProfile, Result, OutError))
	{
		OutData = Result.ToJsonObject();
		return false;
	}
	OutData = Result.ToJsonObject();
	OutData->SetStringField(TEXT("asset_path"), UeAgentRiggingAuthoring::NormalizePackagePath(AssetPath));
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRigApplyFolder(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("folder_path_required");
		return false;
	}
	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
	const UeAgentRiggingAuthoring::FProfile& Profile = *UeAgentRiggingAuthoring::FindProfile(
		UeAgentRiggingAuthoring::EAssetKind::IKRig);
	UeAgentRiggingAuthoring::FAssetApplyResult Result;
	const bool bApplied = UeAgentRiggingAuthoring::ApplyAssetWorkspace(
		Profile, FolderPath, AssetPath, Result, Ctx.RequestId);
	OutData = UeAgentIKFolderOps::ApplyResultJson(Result);
	if (!bApplied)
	{
		OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
			Result.ErrorCode, Result.ErrorDetail, TEXT("ik_rig_folder_apply_failed"));
	}
	return bApplied;
}

bool FUeAgentLegacyMechanics::CmdIKRigValidateFolder(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("folder_path_required");
		return false;
	}
	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
	const UeAgentRiggingAuthoring::FProfile& Profile = *UeAgentRiggingAuthoring::FindProfile(
		UeAgentRiggingAuthoring::EAssetKind::IKRig);
	UeAgentRiggingAuthoring::FWorkspaceInspectResult Result;
	const bool bValid = UeAgentRiggingAuthoring::InspectAssetWorkspace(
		Profile, FolderPath, AssetPath, Result);
	OutData = UeAgentIKFolderOps::InspectResultJson(Result);
	if (!bValid)
	{
		OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
			Result.ErrorCode, Result.ErrorDetail, TEXT("ik_rig_folder_validation_failed"));
	}
	return bValid;
}

bool FUeAgentLegacyMechanics::CmdIKRetargeterExportFolder(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	const UeAgentRiggingAuthoring::FProfile* Profile =
		UeAgentRiggingAuthoring::FindProfile(UeAgentRiggingAuthoring::EAssetKind::IKRetargeter);
	UObject* Asset = UeAgentRiggingAuthoring::LoadAsset(Profile->Kind, AssetPath);
	if (!Asset)
	{
		OutError = TEXT("ik_retargeter_not_found");
		return false;
	}
	FString FolderPath;
	if (!UeAgentIKFolderOps::ResolveFolder(Ctx, AssetPath, TEXT("IKRetargeter"), FolderPath, OutError)) return false;
	FString ExportProfile = TEXT("llm_authoring");
	JsonTryGetString(Ctx.Params, TEXT("profile"), ExportProfile);
	UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
	if (!UeAgentRiggingAuthoring::ExportAssetWorkspace(
		*Profile, *Asset, FolderPath, ExportProfile, Result, OutError))
	{
		OutData = Result.ToJsonObject();
		return false;
	}
	OutData = Result.ToJsonObject();
	OutData->SetStringField(TEXT("asset_path"), UeAgentRiggingAuthoring::NormalizePackagePath(AssetPath));
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRetargeterApplyFolder(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("folder_path_required");
		return false;
	}
	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
	const UeAgentRiggingAuthoring::FProfile& Profile = *UeAgentRiggingAuthoring::FindProfile(
		UeAgentRiggingAuthoring::EAssetKind::IKRetargeter);
	UeAgentRiggingAuthoring::FAssetApplyResult Result;
	const bool bApplied = UeAgentRiggingAuthoring::ApplyAssetWorkspace(
		Profile, FolderPath, AssetPath, Result, Ctx.RequestId);
	OutData = UeAgentIKFolderOps::ApplyResultJson(Result);
	if (!bApplied)
	{
		OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
			Result.ErrorCode, Result.ErrorDetail, TEXT("ik_retargeter_folder_apply_failed"));
	}
	return bApplied;
}

bool FUeAgentLegacyMechanics::CmdIKRetargeterValidateFolder(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("folder_path_required");
		return false;
	}
	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
	const UeAgentRiggingAuthoring::FProfile& Profile = *UeAgentRiggingAuthoring::FindProfile(
		UeAgentRiggingAuthoring::EAssetKind::IKRetargeter);
	UeAgentRiggingAuthoring::FWorkspaceInspectResult Result;
	const bool bValid = UeAgentRiggingAuthoring::InspectAssetWorkspace(
		Profile, FolderPath, AssetPath, Result);
	OutData = UeAgentIKFolderOps::InspectResultJson(Result);
	if (!bValid)
	{
		OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
			Result.ErrorCode, Result.ErrorDetail, TEXT("ik_retargeter_folder_validation_failed"));
	}
	return bValid;
}

bool FUeAgentLegacyMechanics::CmdRetargetBatchExportJson(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	if (OutputFile.TrimStartAndEnd().IsEmpty())
	{
		OutputFile = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("RetargetBatch"), TEXT("batch.hjson"));
	}

	TSharedPtr<FJsonObject> BatchJson = MakeShared<FJsonObject>();
	BatchJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.retarget_batch.v1"));
	FString RetargeterPath;
	JsonTryGetString(Ctx.Params, TEXT("retargeter"), RetargeterPath);
	BatchJson->SetStringField(TEXT("retargeter"), RetargeterPath);
	TArray<TSharedPtr<FJsonValue>> AssetPaths;
	const TArray<TSharedPtr<FJsonValue>>* InputAssets = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("asset_paths"), InputAssets) && InputAssets) AssetPaths = *InputAssets;
	BatchJson->SetArrayField(TEXT("source_assets"), AssetPaths);
	for (const FString& Key : {
		FString(TEXT("source_mesh")), FString(TEXT("target_mesh")), FString(TEXT("output_folder")),
		FString(TEXT("prefix")), FString(TEXT("suffix")), FString(TEXT("search")), FString(TEXT("replace")) })
	{
		if (const TSharedPtr<FJsonValue>* Value = Ctx.Params->Values.Find(Key)) BatchJson->SetField(Key, *Value);
	}

	UeAgentAuthoringFileIO::FSaveOptions SaveOptions;
	SaveOptions.SourceCommand = TEXT("retarget_batch_export_json");
	FString ResolvedOutputFile;
	FString SaveError;
	if (!UeAgentAuthoringFileIO::SaveObject(OutputFile, BatchJson, SaveOptions, ResolvedOutputFile, SaveError))
	{
		OutError = SaveError.IsEmpty() ? TEXT("save_json_failed") : SaveError;
		return false;
	}
	OutData = BatchJson;
	OutData->SetStringField(TEXT("output_file"), ResolvedOutputFile);
	return true;
}

bool FUeAgentLegacyMechanics::CmdRetargetBatchApplyJson(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	FString JsonFile;
	if (!JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile) || JsonFile.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_json_file");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> BatchJson;
	UeAgentAuthoringFileIO::FLoadOptions LoadOptions;
	FString LoadError;
	if (!UeAgentAuthoringFileIO::LoadObject(JsonFile, BatchJson, LoadOptions, LoadError))
	{
		OutError = LoadError.IsEmpty() ? TEXT("hjson_load_failed") : LoadError;
		UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("hjson_load_failed"), JsonFile, OutError);
		OutData = MakeShared<FJsonObject>();
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}
	UeAgentJsonDiagnostics::WarnUnknownFields(
		BatchJson,
		TEXT("batch.hjson"),
		{ TEXT("schema"), TEXT("retargeter"), TEXT("source_assets"), TEXT("asset_paths"),
			TEXT("source_mesh"), TEXT("target_mesh"), TEXT("source_mesh_path"), TEXT("target_mesh_path"),
			TEXT("output_folder"), TEXT("prefix"), TEXT("suffix"), TEXT("search"), TEXT("replace"),
			TEXT("include_referenced_assets"), TEXT("overwrite_policy"), TEXT("preserve") },
		Issues);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	FString RetargeterPath;
	if (!BatchJson->TryGetStringField(TEXT("retargeter"), RetargeterPath) || RetargeterPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_retargeter");
		UeAgentJsonDiagnostics::AddIssue(
			Issues, TEXT("error"), TEXT("json_missing_required_field"),
			TEXT("batch.hjson.retargeter"), TEXT("retargeter is required."));
		OutData = MakeShared<FJsonObject>();
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}
	Params->SetStringField(TEXT("asset_path"), RetargeterPath);
	UeAgentIKFolderOps::CopyFieldIfPresent(BatchJson, TEXT("source_assets"), Params, TEXT("asset_paths"));
	UeAgentIKFolderOps::CopyFieldIfPresent(BatchJson, TEXT("asset_paths"), Params, TEXT("asset_paths"));
	UeAgentIKFolderOps::CopyFieldIfPresent(BatchJson, TEXT("source_mesh"), Params, TEXT("source_mesh_path"));
	UeAgentIKFolderOps::CopyFieldIfPresent(BatchJson, TEXT("target_mesh"), Params, TEXT("target_mesh_path"));
	for (const FString& Key : {
		FString(TEXT("source_mesh_path")), FString(TEXT("target_mesh_path")), FString(TEXT("output_folder")),
		FString(TEXT("prefix")), FString(TEXT("suffix")), FString(TEXT("search")), FString(TEXT("replace")),
		FString(TEXT("include_referenced_assets")) })
	{
		UeAgentIKFolderOps::CopyFieldIfPresent(BatchJson, Key, Params, Key);
	}

	TSharedPtr<FJsonObject> SubData = MakeShared<FJsonObject>();
	if (!CmdIKRetargeterDuplicateAndRetarget(
		UeAgentIKFolderOps::MakeSubContext(TEXT("retarget_batch_apply"), TEXT("ik_retargeter_duplicate_and_retarget"), Params),
		SubData,
		OutError))
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}
	OutData = SubData;
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	OutData->SetStringField(TEXT("json_file"), UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(JsonFile));
	return true;
}

bool FUeAgentLegacyMechanics::CmdRetargetBatchValidateJson(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	FString JsonFile;
	if (!JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile) || JsonFile.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_json_file");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> BatchJson;
	UeAgentAuthoringFileIO::FLoadOptions LoadOptions;
	FString LoadError;
	if (!UeAgentAuthoringFileIO::LoadObject(JsonFile, BatchJson, LoadOptions, LoadError))
	{
		OutError = LoadError.IsEmpty() ? TEXT("hjson_load_failed") : LoadError;
		UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("hjson_load_failed"), JsonFile, OutError);
		OutData = MakeShared<FJsonObject>();
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}

	FString RetargeterPath;
	if (!BatchJson->TryGetStringField(TEXT("retargeter"), RetargeterPath)
		|| !UeAgentIKAssetOps::LoadIKRetargeter(RetargeterPath))
	{
		UeAgentJsonDiagnostics::AddIssue(
			Issues, TEXT("error"), TEXT("retargeter_not_found"),
			TEXT("batch.hjson.retargeter"), TEXT("retargeter must load as UIKRetargeter."));
	}
	const TArray<TSharedPtr<FJsonValue>>* SourceAssets = nullptr;
	if (!BatchJson->TryGetArrayField(TEXT("source_assets"), SourceAssets)
		&& !BatchJson->TryGetArrayField(TEXT("asset_paths"), SourceAssets))
	{
		UeAgentJsonDiagnostics::AddIssue(
			Issues, TEXT("error"), TEXT("json_missing_required_field"),
			TEXT("batch.hjson.source_assets"), TEXT("source_assets or asset_paths is required."));
	}
	else if (!SourceAssets || SourceAssets->IsEmpty())
	{
		UeAgentJsonDiagnostics::AddIssue(
			Issues, TEXT("error"), TEXT("json_empty_required_field"),
			TEXT("batch.hjson.source_assets"), TEXT("source_assets or asset_paths must contain at least one asset path."));
	}
	else
	{
		for (int32 Index = 0; Index < SourceAssets->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& AssetValue = (*SourceAssets)[Index];
			if (!AssetValue.IsValid() || AssetValue->Type != EJson::String
				|| AssetValue->AsString().TrimStartAndEnd().IsEmpty())
			{
				UeAgentJsonDiagnostics::AddIssue(
					Issues, TEXT("error"), TEXT("json_array_item_type_mismatch"),
					FString::Printf(TEXT("batch.hjson.source_assets[%d]"), Index),
					TEXT("source asset paths must be non-empty strings."));
			}
		}
	}
	FString OutputFolder;
	if (!BatchJson->TryGetStringField(TEXT("output_folder"), OutputFolder)
		|| !FPackageName::IsValidLongPackageName(OutputFolder))
	{
		UeAgentJsonDiagnostics::AddIssue(
			Issues, TEXT("error"), TEXT("invalid_output_folder"),
			TEXT("batch.hjson.output_folder"), TEXT("output_folder must be a valid long package path."));
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("json_file"), UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(JsonFile));
	OutData->SetBoolField(TEXT("valid"), Issues.IsEmpty());
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	return Issues.IsEmpty();
}
