// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_UMG.h"

#include "UeAgentHttpServer_UMGOps.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringApplyProvider.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringExport.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "WidgetBlueprint.h"

namespace
{
	FString GetUmgWorkspaceRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("WidgetBlueprint")));
	}

	bool ResolveUmgWorkspace(
		const FString& AssetPath,
		FString& OutFolderPath,
		FString& OutError)
	{
		return UeAgentAuthoringWorkspacePaths::ResolveAssetWorkspaceFolder(
			GetUmgWorkspaceRoot(),
			UeAgentUmgOps::NormalizeAssetPath(AssetPath),
			OutFolderPath,
			OutError);
	}

	void SetMutationCounters(
		const UeAgentUmgAuthoring::FMutationCounters& Counters,
		const TSharedPtr<FJsonObject>& OutData)
	{
		OutData->SetNumberField(TEXT("widgets_added"), Counters.WidgetsAdded);
		OutData->SetNumberField(TEXT("widgets_updated"), Counters.WidgetsUpdated);
		OutData->SetNumberField(TEXT("widgets_removed"), Counters.WidgetsRemoved);
		OutData->SetNumberField(
			TEXT("widget_properties_applied"), Counters.WidgetPropertiesApplied);
		OutData->SetNumberField(TEXT("slot_properties_applied"), Counters.SlotPropertiesApplied);
		OutData->SetNumberField(TEXT("bindings_added"), Counters.BindingsAdded);
		OutData->SetNumberField(TEXT("bindings_updated"), Counters.BindingsUpdated);
		OutData->SetNumberField(TEXT("bindings_removed"), Counters.BindingsRemoved);
		OutData->SetNumberField(TEXT("animations_added"), Counters.AnimationsAdded);
		OutData->SetNumberField(TEXT("animations_updated"), Counters.AnimationsUpdated);
		OutData->SetNumberField(TEXT("animations_removed"), Counters.AnimationsRemoved);
		OutData->SetNumberField(
			TEXT("animation_keys_applied"), Counters.AnimationKeysApplied);
		OutData->SetNumberField(
			TEXT("variables_added"), Counters.Blueprint.VariablesAdded);
		OutData->SetNumberField(
			TEXT("variables_updated"), Counters.Blueprint.VariablesUpdated);
		OutData->SetNumberField(
			TEXT("variables_removed"), Counters.Blueprint.VariablesRemoved);
		OutData->SetNumberField(TEXT("graphs_added"), Counters.Blueprint.GraphsAdded);
		OutData->SetNumberField(TEXT("graphs_updated"), Counters.Blueprint.GraphsUpdated);
		OutData->SetNumberField(TEXT("graphs_removed"), Counters.Blueprint.GraphsRemoved);
	}
}

bool FUeAgentLegacyMechanics::CmdUmgExportFolder(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput)
		|| AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPathInput);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	const FString AssetPath = UeAgentUmgOps::NormalizeAssetPath(AssetPathInput);
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.IsEmpty())
	{
		if (!ResolveUmgWorkspace(AssetPath, FolderPath, OutError)) return false;
	}
	FString ProfileId = TEXT("llm_authoring");
	JsonTryGetString(Ctx.Params, TEXT("profile"), ProfileId);

	UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
	if (!UeAgentUmgAuthoring::ExportAssetWorkspace(
		*WidgetBlueprint, FolderPath, ProfileId, Result, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("root_path"), GetUmgWorkspaceRoot());
	OutData->SetStringField(TEXT("profile"), Result.ProfileId);
	OutData->SetNumberField(TEXT("file_count"), Result.WrittenFiles.Num());
	OutData->SetNumberField(TEXT("character_count"), Result.Metrics.CharacterCount);
	OutData->SetNumberField(TEXT("estimated_tokens"), Result.Metrics.TokenProxy);
	return true;
}

bool FUeAgentLegacyMechanics::CmdUmgApplyFolder(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput)
		|| AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString AssetPath = UeAgentUmgOps::NormalizeAssetPath(AssetPathInput);
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.IsEmpty())
	{
		if (!ResolveUmgWorkspace(AssetPath, FolderPath, OutError)) return false;
	}

	UeAgentUmgAuthoring::FAssetApplyResult Result;
	if (!UeAgentUmgAuthoring::ApplyAssetWorkspace(FolderPath, AssetPath, Result, Ctx.RequestId))
	{
		OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
			Result.ErrorCode,
			Result.ErrorDetail,
			TEXT("umg_unified_apply_failed"));
		return false;
	}

	OutData->SetStringField(TEXT("status"), Result.Status);
	OutData->SetStringField(TEXT("phase"), Result.Phase);
	OutData->SetStringField(TEXT("resolved_operation"), Result.ResolvedOperation);
	OutData->SetStringField(TEXT("asset_path"), Result.AssetPath);
	OutData->SetStringField(TEXT("object_path"), Result.ObjectPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("units_checked"), Result.UnitsChecked);
	OutData->SetNumberField(TEXT("mutations_applied"), Result.MutationsApplied);
	OutData->SetBoolField(TEXT("rollback_attempted"), Result.bRollbackAttempted);
	OutData->SetBoolField(TEXT("rollback_succeeded"), Result.bRollbackSucceeded);
	SetMutationCounters(Result.Domain, OutData);
	return true;
}
