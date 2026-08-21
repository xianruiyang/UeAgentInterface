// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Niagara.h"
#include "UeAgentHttpServer_NiagaraOps.h"

#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringApplyProvider.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringDescriptors.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringExport.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"

namespace
{
	FString WorkspaceKindRoot(const FString& AssetKind)
	{
		const FString FolderName = AssetKind == TEXT("system")
			? TEXT("NiagaraSystem")
			: AssetKind == TEXT("emitter")
				? TEXT("NiagaraEmitter")
				: TEXT("NiagaraScript");
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), FolderName));
	}

	bool ResolveWorkspaceFolder(
		const FString& AssetKind,
		const FString& AssetPath,
		FString& OutFolderPath,
		FString& OutError)
	{
		return UeAgentAuthoringWorkspacePaths::ResolveAssetWorkspaceFolder(
			WorkspaceKindRoot(AssetKind),
			AssetPath,
			OutFolderPath,
			OutError);
	}

	bool ReadAssetPath(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* PreferredField,
		FString& OutAssetPath)
	{
		OutAssetPath.Reset();
		if (Params.IsValid()) Params->TryGetStringField(PreferredField, OutAssetPath);
		if (OutAssetPath.IsEmpty() && Params.IsValid()) Params->TryGetStringField(TEXT("asset_path"), OutAssetPath);
		OutAssetPath.TrimStartAndEndInline();
		return !OutAssetPath.IsEmpty();
	}

	bool ExportWorkspaceCommand(
		const FUeAgentRequestContext& Context,
		const TCHAR* PreferredAssetPathField,
		const FString& ExpectedAssetKind,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		FString AssetPath;
		if (!ReadAssetPath(Context.Params, PreferredAssetPathField, AssetPath))
		{
			OutError = FString(TEXT("missing_")) + PreferredAssetPathField;
			return false;
		}
		UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPath);
		if (!Asset)
		{
			OutError = TEXT("niagara_asset_not_found");
			return false;
		}
		const FString AssetKind = UeAgentNiagaraAuthoring::GetAssetKind(Asset);
		if (AssetKind != ExpectedAssetKind)
		{
			OutError = TEXT("niagara_asset_kind_conflict:") + AssetKind + TEXT(" != ") + ExpectedAssetKind;
			return false;
		}

		FString FolderPath;
		if (Context.Params.IsValid()) Context.Params->TryGetStringField(TEXT("folder_path"), FolderPath);
		FolderPath.TrimStartAndEndInline();
		if (FolderPath.IsEmpty()
			&& !ResolveWorkspaceFolder(
				ExpectedAssetKind,
				Asset->GetOutermost()->GetName(),
				FolderPath,
				OutError))
		{
			return false;
		}
		FolderPath = FPaths::ConvertRelativePathToFull(FolderPath);
		FString ProfileId = TEXT("llm_authoring");
		if (Context.Params.IsValid()) Context.Params->TryGetStringField(TEXT("profile"), ProfileId);

		UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
		if (!UeAgentNiagaraAuthoring::ExportAssetWorkspace(
			*Asset, FolderPath, ProfileId, ExportResult, OutError))
		{
			return false;
		}
		OutData = ExportResult.ToJsonObject();
		OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("asset_kind"), AssetKind);
		OutData->SetStringField(TEXT("folder_path"), FolderPath);
		OutData->SetNumberField(TEXT("file_count"), ExportResult.WrittenFiles.Num());
		return true;
	}

	bool ApplyWorkspaceCommand(
		const FUeAgentRequestContext& Context,
		const TCHAR* PreferredAssetPathField,
		const FString& ExpectedAssetKind,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		FString ExpectedAssetPath;
		ReadAssetPath(Context.Params, PreferredAssetPathField, ExpectedAssetPath);
		FString FolderPath;
		if (Context.Params.IsValid()) Context.Params->TryGetStringField(TEXT("folder_path"), FolderPath);
		FolderPath.TrimStartAndEndInline();
		if (FolderPath.IsEmpty())
		{
			if (ExpectedAssetPath.IsEmpty())
			{
				OutError = TEXT("missing_folder_path_or_asset_path");
				return false;
			}
			if (!ResolveWorkspaceFolder(
				ExpectedAssetKind,
				ExpectedAssetPath,
				FolderPath,
				OutError))
			{
				return false;
			}
		}
		FolderPath = FPaths::ConvertRelativePathToFull(FolderPath);

		UeAgentNiagaraAuthoring::FAssetApplyResult ApplyResult;
		if (!UeAgentNiagaraAuthoring::ApplyAssetWorkspace(
			FolderPath,
			ExpectedAssetKind,
			ExpectedAssetPath,
			ApplyResult,
			Context.RequestId))
		{
			OutData->SetStringField(TEXT("status"), ApplyResult.Status);
			OutData->SetStringField(TEXT("phase"), ApplyResult.Phase);
			OutData->SetStringField(TEXT("error_code"), ApplyResult.ErrorCode);
			OutData->SetStringField(TEXT("error_detail"), ApplyResult.ErrorDetail);
			OutData->SetBoolField(TEXT("rollback_attempted"), ApplyResult.bRollbackAttempted);
			OutData->SetBoolField(TEXT("rollback_succeeded"), ApplyResult.bRollbackSucceeded);
			// Keep the stable fields in OutData, but preserve the provider's actionable
			// failure detail for debug/raw responses and failure diagnostics.
			OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
				ApplyResult.ErrorCode,
				ApplyResult.ErrorDetail,
				TEXT("niagara_workspace_apply_failed"));
			return false;
		}

		OutData->SetStringField(TEXT("status"), ApplyResult.Status);
		OutData->SetStringField(TEXT("phase"), ApplyResult.Phase);
		OutData->SetStringField(TEXT("asset_path"), ApplyResult.AssetPath);
		OutData->SetStringField(TEXT("asset_kind"), ApplyResult.AssetKind);
		OutData->SetStringField(TEXT("object_path"), ApplyResult.ObjectPath);
		OutData->SetStringField(TEXT("resolved_operation"), ApplyResult.ResolvedOperation);
		OutData->SetStringField(TEXT("compile_status"), ApplyResult.CompileStatus);
		OutData->SetBoolField(TEXT("compilation_complete"), ApplyResult.bCompilationComplete);
		OutData->SetNumberField(TEXT("compile_error_count"), ApplyResult.CompileErrorCount);
		OutData->SetNumberField(TEXT("compile_warning_count"), ApplyResult.CompileWarningCount);
		OutData->SetNumberField(TEXT("stack_issue_count"), ApplyResult.StackIssueCount);
		OutData->SetNumberField(TEXT("stack_warning_count"), ApplyResult.StackWarningCount);
		TArray<TSharedPtr<FJsonValue>> CompiledTargets;
		CompiledTargets.Reserve(ApplyResult.CompiledTargets.Num());
		for (const FString& Target : ApplyResult.CompiledTargets)
		{
			CompiledTargets.Add(MakeShared<FJsonValueString>(Target));
		}
		OutData->SetArrayField(TEXT("compiled_targets"), CompiledTargets);
		OutData->SetBoolField(TEXT("saved"), ApplyResult.bSaved);
		OutData->SetStringField(TEXT("folder_path"), FolderPath);
		OutData->SetNumberField(TEXT("units_checked"), ApplyResult.UnitsChecked);
		OutData->SetNumberField(TEXT("mutations_applied"), ApplyResult.MutationsApplied);
		OutData->SetNumberField(TEXT("repaired_emitter_version_count"), ApplyResult.RepairedEmitterVersionCount);
		OutData->SetBoolField(TEXT("editor_isolation_performed"), ApplyResult.bEditorIsolationPerformed);
		OutData->SetNumberField(TEXT("closed_editor_count"), ApplyResult.ClosedEditorCount);
		OutData->SetBoolField(TEXT("rollback_attempted"), ApplyResult.bRollbackAttempted);
		OutData->SetBoolField(TEXT("rollback_succeeded"), ApplyResult.bRollbackSucceeded);
		return true;
	}
}

bool FUeAgentLegacyMechanics::CmdNiagaraExportFolder(
	const FUeAgentRequestContext& Context,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	return ExportWorkspaceCommand(Context, TEXT("asset_path"), TEXT("system"), OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdNiagaraApplyFolder(
	const FUeAgentRequestContext& Context,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	return ApplyWorkspaceCommand(Context, TEXT("asset_path"), TEXT("system"), OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterExportFolder(
	const FUeAgentRequestContext& Context,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	return ExportWorkspaceCommand(Context, TEXT("emitter_asset_path"), TEXT("emitter"), OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdNiagaraEmitterApplyFolder(
	const FUeAgentRequestContext& Context,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	return ApplyWorkspaceCommand(Context, TEXT("emitter_asset_path"), TEXT("emitter"), OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdNiagaraScriptExportFolder(
	const FUeAgentRequestContext& Context,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	return ExportWorkspaceCommand(Context, TEXT("script_asset_path"), TEXT("script"), OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdNiagaraScriptApplyFolder(
	const FUeAgentRequestContext& Context,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	return ApplyWorkspaceCommand(Context, TEXT("script_asset_path"), TEXT("script"), OutData, OutError);
}
