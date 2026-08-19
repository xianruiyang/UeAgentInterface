// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Blueprint.h"

#include "Commands/Blueprint/UeAgentHttpServer_BlueprintOps.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringApplyProvider.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringExport.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Misc/Paths.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"

namespace
{
	FString GetBlueprintWorkspaceRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("Blueprint")));
	}

	bool ResolveBlueprintWorkspace(
		const FString& AssetPath,
		FString& OutFolderPath,
		FString& OutError)
	{
		return UeAgentAuthoringWorkspacePaths::ResolveAssetWorkspaceFolder(
			GetBlueprintWorkspaceRoot(),
			UeAgentBlueprintOps::NormalizeAssetPath(AssetPath),
			OutFolderPath,
			OutError);
	}

	void SetApplyFailure(
		const UeAgentBlueprintAuthoring::FAssetApplyResult& Result,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		if (!OutData.IsValid()) OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("status"), Result.Status);
		OutData->SetStringField(TEXT("phase"), Result.Phase);
		OutData->SetStringField(TEXT("error_code"), Result.ErrorCode);
		OutData->SetStringField(TEXT("error_detail"), Result.ErrorDetail);
		OutData->SetBoolField(TEXT("rollback_attempted"), Result.bRollbackAttempted);
		OutData->SetBoolField(TEXT("rollback_succeeded"), Result.bRollbackSucceeded);
		OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
			Result.ErrorCode,
			Result.ErrorDetail,
			TEXT("blueprint_unified_apply_failed"));
	}

	int32 Sum3(const int32 Added, const int32 Updated, const int32 Removed)
	{
		return Added + Updated + Removed;
	}
}

bool FUeAgentHttpServer::CmdBlueprintExportFolder(
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

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}

	const FString AssetPath = UeAgentBlueprintOps::NormalizeAssetPath(AssetPathInput);
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("output_folder"), FolderPath)
		|| FolderPath.TrimStartAndEnd().IsEmpty())
	{
		if (!ResolveBlueprintWorkspace(AssetPath, FolderPath, OutError)) return false;
	}

	FString Profile = TEXT("llm_authoring");
	JsonTryGetString(Ctx.Params, TEXT("profile"), Profile);
	UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
	if (!UeAgentBlueprintAuthoring::ExportAssetWorkspace(
		*Blueprint, FolderPath, Profile, Result, OutError))
	{
		return false;
	}

	OutData = Result.ToJsonObject();
	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("asset_kind"), UeAgentBlueprintAuthoring::GetAssetKind(Blueprint));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("file_count"), Result.WrittenFiles.Num());
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintApplyFolder(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	if (!OutData.IsValid()) OutData = MakeShared<FJsonObject>();

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput)
		|| AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	const FString AssetPath = UeAgentBlueprintOps::NormalizeAssetPath(AssetPathInput);

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath)
		|| FolderPath.IsEmpty())
	{
		if (!ResolveBlueprintWorkspace(AssetPath, FolderPath, OutError)) return false;
	}
	FolderPath = FPaths::ConvertRelativePathToFull(FolderPath);

	UeAgentBlueprintAuthoring::FAssetApplyResult Result;
	if (!UeAgentBlueprintAuthoring::ApplyAssetWorkspace(FolderPath, AssetPath, Result, Ctx.RequestId))
	{
		SetApplyFailure(Result, OutData, OutError);
		return false;
	}

	OutData->SetStringField(TEXT("status"), Result.Status);
	OutData->SetStringField(TEXT("phase"), Result.Phase);
	OutData->SetStringField(TEXT("asset_path"), Result.AssetPath);
	OutData->SetStringField(TEXT("asset_kind"), Result.AssetKind);
	OutData->SetStringField(TEXT("object_path"), Result.ObjectPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("resolved_operation"), Result.ResolvedOperation);
	OutData->SetNumberField(TEXT("variables_applied"), Sum3(
		Result.Domain.VariablesAdded,
		Result.Domain.VariablesUpdated,
		Result.Domain.VariablesRemoved));
	OutData->SetNumberField(TEXT("delegates_applied"), Sum3(
		Result.Domain.DelegatesAdded,
		Result.Domain.DelegatesUpdated,
		Result.Domain.DelegatesRemoved));
	OutData->SetNumberField(TEXT("components_applied"), Sum3(
		Result.Domain.ComponentsAdded,
		Result.Domain.ComponentsUpdated,
		Result.Domain.ComponentsRemoved));
	OutData->SetNumberField(TEXT("graphs_applied"), Sum3(
		Result.Domain.GraphsAdded,
		Result.Domain.GraphsUpdated,
		Result.Domain.GraphsRemoved));
	OutData->SetNumberField(TEXT("nodes_applied"), Sum3(
		Result.Domain.NodesAdded,
		Result.Domain.NodesUpdated,
		Result.Domain.NodesRemoved));
	OutData->SetNumberField(TEXT("edges_applied"),
		Result.Domain.EdgesAdded + Result.Domain.EdgesRemoved);
	OutData->SetNumberField(TEXT("units_checked"), Result.UnitsChecked);
	OutData->SetNumberField(TEXT("mutations_applied"), Result.MutationsApplied);
	OutData->SetBoolField(TEXT("rollback_attempted"), Result.bRollbackAttempted);
	OutData->SetBoolField(TEXT("rollback_succeeded"), Result.bRollbackSucceeded);
	return true;
}
