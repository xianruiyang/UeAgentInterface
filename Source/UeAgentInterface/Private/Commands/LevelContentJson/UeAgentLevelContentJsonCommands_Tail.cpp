// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Level/UeAgentHttpServer_Level.h"
#include "Commands/Level/UeAgentCommandSafety.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringApplyProvider.h"
#include "UeAgentLevelContentJsonOps.h"
#include "Utilities/UeAgentLevelFolderApply.h"
#include "Utilities/UeAgentLevelFolderDiff.h"
#include "Utilities/UeAgentLevelFolderExport.h"
#include "Utilities/UeAgentLevelFolderValidate.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

bool FUeAgentHttpServer::CmdLevelContentMergeJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Base;
	TSharedPtr<FJsonObject> Current;
	TSharedPtr<FJsonObject> Target;
	if (!Ctx.Params.IsValid() ||
		!UeAgentLevelContentJson::TryGetObject(Ctx.Params, TEXT("base"), Base) ||
		!UeAgentLevelContentJson::TryGetObject(Ctx.Params, TEXT("current"), Current) ||
		!UeAgentLevelContentJson::TryGetObject(Ctx.Params, TEXT("target"), Target) ||
		!Base.IsValid() || !Current.IsValid() || !Target.IsValid())
	{
		OutError = TEXT("missing_base_current_target");
		return false;
	}
	OutData = UeAgentLevelContentJson::MergeThreeWay(Base, Current, Target);
	return true;
}

bool FUeAgentHttpServer::CmdLevelContentRepairJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	OutData = UeAgentLevelContentJson::BuildRepairDiagnosis(World, Ctx.Params);
	bool bApplySafe = false;
	UeAgentLevelContentJson::TryGetBoolAny(Ctx.Params, { TEXT("apply_safe"), TEXT("execute") }, bApplySafe);
	OutData->SetBoolField(TEXT("apply_safe"), bApplySafe);
	if (bApplySafe)
	{
		TSharedPtr<FJsonObject> CleanupData = MakeShared<FJsonObject>();
		FString CleanupError;
		FUeAgentRequestContext CleanupCtx = Ctx;
		CleanupCtx.Params = MakeShared<FJsonObject>();
		FString Prefix;
		UeAgentLevelContentJson::TryGetStringAny(Ctx.Params, { TEXT("folder_path_prefix"), TEXT("folder_root") }, Prefix);
		CleanupCtx.Params->SetStringField(TEXT("folder_path_prefix"), Prefix);
		CleanupCtx.Params->SetBoolField(TEXT("dry_run"), false);
		const bool bCleanupOk = CmdLevelCleanupEmptyFolders(CleanupCtx, CleanupData, CleanupError);
		OutData->SetBoolField(TEXT("cleanup_empty_folders_ok"), bCleanupOk);
		OutData->SetObjectField(TEXT("cleanup_empty_folders"), CleanupData);
		if (!bCleanupOk)
		{
			OutData->SetStringField(TEXT("cleanup_empty_folders_error"), CleanupError);
		}
	}
	return true;
}

bool FUeAgentHttpServer::CmdLevelContentQueryJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return ExecuteLevelContentQueryJsonInternal(Ctx, OutData, OutError, false);
}

bool FUeAgentHttpServer::CmdLevelContentValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return ExecuteLevelContentValidateJsonInternal(Ctx, OutData, OutError, false);
}

bool FUeAgentHttpServer::CmdLevelContentApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return ExecuteLevelContentApplyJsonInternal(Ctx, OutData, OutError, false);
}

bool FUeAgentHttpServer::CmdLevelContentDiffJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return ExecuteLevelContentDiffJsonInternal(Ctx, OutData, OutError, false);
}

bool FUeAgentHttpServer::CmdLevelContentSnapshotJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return ExecuteLevelContentSnapshotJsonInternal(Ctx, OutData, OutError, false);
}

bool FUeAgentHttpServer::CmdLevelExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	OutData = MakeShared<FJsonObject>();
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	UeAgentLevelFolderExport::FExportOptions Options;
	if (Ctx.Params.IsValid())
	{
		if (!Ctx.Params->TryGetStringField(TEXT("output_folder"), Options.OutputFolder))
		{
			Ctx.Params->TryGetStringField(TEXT("output_dir"), Options.OutputFolder);
		}
		Ctx.Params->TryGetStringField(TEXT("profile"), Options.Profile);
	}

	FString OwnershipRoot;
	if (!Options.OutputFolder.TrimStartAndEnd().IsEmpty())
	{
		FString ResolvedOutputFolder;
		if (!UeAgentCommandSafety::ResolveOwnedAuthoringArtifactFolder(Options.OutputFolder, ResolvedOutputFolder, OwnershipRoot, OutError))
		{
			OutData->SetStringField(TEXT("requested_output_folder"), Options.OutputFolder);
			OutData->SetStringField(TEXT("status"), TEXT("blocked"));
			OutData->SetBoolField(TEXT("output_reset_allowed"), false);
			return false;
		}
		Options.OutputFolder = ResolvedOutputFolder;
	}

	const bool bExported = UeAgentLevelFolderExport::ExportLevelFolder(World, Options, OutData, OutError);
	if (OutData.IsValid())
	{
		OutData->SetBoolField(TEXT("output_reset_allowed"), true);
		if (!OwnershipRoot.IsEmpty())
		{
			OutData->SetStringField(TEXT("ownership_root"), OwnershipRoot);
		}
	}
	return bExported;
}

bool FUeAgentHttpServer::CmdLevelApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	OutData = MakeShared<FJsonObject>();
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString InputFolder;
	bool bDryRun = true;
	if (Ctx.Params.IsValid())
	{
		if (!Ctx.Params->TryGetStringField(TEXT("input_folder"), InputFolder))
		{
			Ctx.Params->TryGetStringField(TEXT("folder_path"), InputFolder);
		}
		if (!UeAgentCommandSafety::ReadStrictBool(
			Ctx.Params, TEXT("dry_run"), true, bDryRun, OutError))
		{
			return false;
		}
	}
	UeAgentLevelAuthoring::FWorkspaceApplyOptions ApplyOptions;
	ApplyOptions.bDryRun = bDryRun;
	UeAgentLevelAuthoring::FWorkspaceApplyResult Result;
	const bool bApplied = UeAgentLevelAuthoring::ApplyWorkspaceDirectory(
		*World, InputFolder, ApplyOptions, Result, Ctx.RequestId);
	OutData = Result.ToJsonObject();
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	if (!bApplied)
	{
		OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
			Result.ErrorCode, Result.ErrorDetail, TEXT("level_content_folder_apply_failed"));
	}
	return bApplied;
}

bool FUeAgentHttpServer::CmdLevelValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentLevelFolderValidate::FValidateOptions Options;
	if (Ctx.Params.IsValid())
	{
		if (!Ctx.Params->TryGetStringField(TEXT("input_folder"), Options.InputFolder))
		{
			Ctx.Params->TryGetStringField(TEXT("folder_path"), Options.InputFolder);
		}
	}

	return UeAgentLevelFolderValidate::ValidateLevelFolder(Options, OutData, OutError);
}

bool FUeAgentHttpServer::CmdLevelDiffFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentLevelFolderDiff::FDiffOptions Options;
	if (Ctx.Params.IsValid())
	{
		if (!Ctx.Params->TryGetStringField(TEXT("base_folder"), Options.BaseFolder))
		{
			Ctx.Params->TryGetStringField(TEXT("left_folder"), Options.BaseFolder);
		}
		if (!Ctx.Params->TryGetStringField(TEXT("target_folder"), Options.TargetFolder))
		{
			Ctx.Params->TryGetStringField(TEXT("right_folder"), Options.TargetFolder);
		}
		double MaxChanges = 0.0;
		if (Ctx.Params->TryGetNumberField(TEXT("max_changes"), MaxChanges))
		{
			Options.MaxChanges = static_cast<int32>(MaxChanges);
		}
	}

	return UeAgentLevelFolderDiff::DiffLevelFolders(Options, OutData, OutError);
}

bool FUeAgentHttpServer::CmdLevelActorExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return ExecuteLevelContentQueryJsonInternal(Ctx, OutData, OutError, true);
}

bool FUeAgentHttpServer::CmdLevelActorValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return ExecuteLevelContentValidateJsonInternal(Ctx, OutData, OutError, true);
}

bool FUeAgentHttpServer::CmdLevelActorApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return ExecuteLevelContentApplyJsonInternal(Ctx, OutData, OutError, true);
}

bool FUeAgentHttpServer::CmdLevelActorDiffJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return ExecuteLevelContentDiffJsonInternal(Ctx, OutData, OutError, true);
}

bool FUeAgentHttpServer::CmdLevelActorSnapshotJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return ExecuteLevelContentSnapshotJsonInternal(Ctx, OutData, OutError, true);
}
