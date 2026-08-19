#include "UeAgentHttpServer_ControlRig.h"

#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringApplyProvider.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringExport.h"
#include "Domains/Rigging/UeAgentControlRigDomainBackend.h"
#include "Dom/JsonObject.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

namespace
{
	const UeAgentRiggingAuthoring::FProfile& ControlRigProfile()
	{
		return *UeAgentRiggingAuthoring::FindProfile(UeAgentRiggingAuthoring::EAssetKind::ControlRig);
	}

	TSharedRef<FJsonObject> MutationStatsJson(const UeAgentRiggingAuthoring::FMutationStats& Stats)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("hierarchy_elements"), Stats.HierarchyElements);
		Result->SetNumberField(TEXT("graph_nodes"), Stats.GraphNodes);
		Result->SetNumberField(TEXT("graph_links"), Stats.GraphLinks);
		return Result;
	}

	TSharedRef<FJsonObject> ApplyResultJson(const UeAgentRiggingAuthoring::FAssetApplyResult& Result)
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

	TSharedRef<FJsonObject> InspectResultJson(const UeAgentRiggingAuthoring::FWorkspaceInspectResult& Result)
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
}

bool FUeAgentHttpServer::CmdControlRigExportFolder(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("asset_path_required");
		return false;
	}
	UObject* Asset = UeAgentRiggingAuthoring::LoadAsset(
		UeAgentRiggingAuthoring::EAssetKind::ControlRig, AssetPath);
	if (!Asset)
	{
		OutError = TEXT("control_rig_not_found");
		return false;
	}
	FString FolderPath;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath);
	if (FolderPath.TrimStartAndEnd().IsEmpty())
		FolderPath = UeAgentControlRigOps::DefaultFolderForAsset(AssetPath);
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
	FString ExportProfile = TEXT("llm_authoring");
	JsonTryGetString(Ctx.Params, TEXT("profile"), ExportProfile);
	UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
	if (!UeAgentRiggingAuthoring::ExportAssetWorkspace(
		ControlRigProfile(), *Asset, FolderPath, ExportProfile, Result, OutError))
	{
		OutData = Result.ToJsonObject();
		return false;
	}
	OutData = Result.ToJsonObject();
	OutData->SetStringField(TEXT("asset_path"), UeAgentRiggingAuthoring::NormalizePackagePath(AssetPath));
	return true;
}

bool FUeAgentHttpServer::CmdControlRigValidateFolder(
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
	UeAgentRiggingAuthoring::FWorkspaceInspectResult Result;
	const bool bValid = UeAgentRiggingAuthoring::InspectAssetWorkspace(
		ControlRigProfile(), FolderPath, AssetPath, Result);
	OutData = InspectResultJson(Result);
	if (!bValid)
	{
		OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
			Result.ErrorCode, Result.ErrorDetail, TEXT("control_rig_folder_validation_failed"));
	}
	return bValid;
}

bool FUeAgentHttpServer::CmdControlRigApplyFolder(
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
	UeAgentRiggingAuthoring::FAssetApplyResult Result;
	const bool bApplied = UeAgentRiggingAuthoring::ApplyAssetWorkspace(
		ControlRigProfile(), FolderPath, AssetPath, Result, Ctx.RequestId);
	OutData = ApplyResultJson(Result);
	if (!bApplied)
	{
		OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
			Result.ErrorCode, Result.ErrorDetail, TEXT("control_rig_folder_apply_failed"));
	}
	return bApplied;
}
