// Copyright Epic Games, Inc. All Rights Reserved.

#include "LegacyMechanics/UeAgentLegacyMechanics.h"
#include "Commands/Level/UeAgentCommandSafety.h"
#include "UeAgentLevelContentJsonApply.h"
#include "UeAgentLevelContentJsonCore.h"
#include "UeAgentLevelContentJsonDiff.h"
#include "UeAgentLevelContentJsonQuery.h"
#include "UeAgentLevelContentJsonValidatePlan.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorActorFolders.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"

namespace
{
	bool LevelContentRequestsDelete(
		const TSharedPtr<FJsonObject>& Content,
		const FString& Mode,
		const FString& DeletePolicy)
	{
		if (Mode == TEXT("delete_only") || Mode == TEXT("replace_scope") ||
			DeletePolicy == TEXT("only_managed_missing") || DeletePolicy == TEXT("replace_scope_managed"))
		{
			return true;
		}

		TArray<TSharedPtr<FJsonObject>> Resources;
		UeAgentLevelContentJson::CollectResourceObjects(Content, Resources);
		for (const TSharedPtr<FJsonObject>& Resource : Resources)
		{
			if (UeAgentLevelContentJson::GetResourceOperation(Resource, TEXT("upsert")) == TEXT("delete"))
			{
				return true;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		if (Content.IsValid() && Content->TryGetArrayField(TEXT("operations"), Operations) && Operations)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Operations)
			{
				const TSharedPtr<FJsonObject> Operation = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Type;
				if (Operation.IsValid() && UeAgentLevelContentJson::TryGetStringAny(Operation, { TEXT("type"), TEXT("operation") }, Type) &&
					Type.TrimStartAndEnd().Equals(TEXT("delete"), ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
		}
		return false;
	}

	bool LevelContentValidationResultsPassed(const TArray<TSharedPtr<FJsonValue>>& ValidationResults)
	{
		for (const TSharedPtr<FJsonValue>& Value : ValidationResults)
		{
			const TSharedPtr<FJsonObject> Result = Value.IsValid() ? Value->AsObject() : nullptr;
			bool bPassed = false;
			if (!Result.IsValid() || !Result->TryGetBoolField(TEXT("passed"), bPassed) || !bPassed)
			{
				return false;
			}
		}
		return true;
	}
}

bool FUeAgentLegacyMechanics::ExecuteLevelContentQueryJsonInternal(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError, bool bActorOnly) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Query;
	if (!UeAgentLevelContentJson::QueryLevelContent(World, Ctx.Params, Query, bActorOnly, false))
	{
		OutError = TEXT("level_content_query_failed");
		return false;
	}
	OutData = Query;
	return UeAgentLevelContentJson::SaveJsonIfRequested(Query, Ctx.Params, OutData, OutError);
}

bool FUeAgentLegacyMechanics::ExecuteLevelContentSnapshotJsonInternal(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError, bool bActorOnly) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Snapshot;
	if (!UeAgentLevelContentJson::QueryLevelContent(World, Ctx.Params, Snapshot, bActorOnly, true))
	{
		OutError = TEXT("level_content_snapshot_failed");
		return false;
	}
	OutData = Snapshot;
	return UeAgentLevelContentJson::SaveJsonIfRequested(Snapshot, Ctx.Params, OutData, OutError);
}

bool FUeAgentLegacyMechanics::ExecuteLevelContentValidateJsonInternal(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError, bool bActorOnly) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Content;
	FString Source;
	if (!UeAgentLevelContentJson::ResolveContentObject(Ctx.Params, Content, Source, OutError))
	{
		return false;
	}
	UeAgentLevelContentJson::ValidateLevelContent(World, Content, Ctx.Params, bActorOnly, OutData);
	OutData->SetStringField(TEXT("source"), Source);
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelContentPlanJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Content;
	FString Source;
	if (!UeAgentLevelContentJson::ResolveContentObject(Ctx.Params, Content, Source, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Validation;
	UeAgentLevelContentJson::ValidateLevelContent(World, Content, Ctx.Params, false, Validation);
	OutData = UeAgentLevelContentJson::BuildPlan(World, Content, Ctx.Params, false);
	OutData->SetStringField(TEXT("source"), Source);
	OutData->SetObjectField(TEXT("validation"), Validation);
	OutData->SetBoolField(TEXT("can_apply"), Validation.IsValid() && Validation->GetBoolField(TEXT("valid")));
	return true;
}

bool FUeAgentLegacyMechanics::ExecuteLevelContentApplyJsonInternal(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError, bool bActorOnly) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Content;
	FString Source;
	if (!UeAgentLevelContentJson::ResolveContentObject(Ctx.Params, Content, Source, OutError))
	{
		return false;
	}

	bool bDryRun = true;
	bool bPreflight = true;
	bool bTransaction = true;
	bool bValidateAfterApply = true;
	bool bAllowDestructive = false;
	if (!UeAgentCommandSafety::ReadStrictBool(Ctx.Params, TEXT("dry_run"), true, bDryRun, OutError) ||
		!UeAgentCommandSafety::ReadStrictBool(Ctx.Params, TEXT("preflight"), true, bPreflight, OutError) ||
		!UeAgentCommandSafety::ReadStrictBool(Ctx.Params, TEXT("transaction"), true, bTransaction, OutError) ||
		!UeAgentCommandSafety::ReadStrictBool(Ctx.Params, TEXT("validate_after_apply"), true, bValidateAfterApply, OutError) ||
		!UeAgentCommandSafety::ReadStrictBool(Ctx.Params, TEXT("allow_destructive"), false, bAllowDestructive, OutError))
	{
		return false;
	}
	FString Mode = TEXT("upsert");
	UeAgentLevelContentJson::TryGetStringAny(Ctx.Params, { TEXT("mode") }, Mode);
	Mode = Mode.TrimStartAndEnd().ToLower();
	if (Mode.IsEmpty())
	{
		Mode = TEXT("upsert");
	}
	const TSet<FString> SupportedModes = { TEXT("upsert"), TEXT("patch"), TEXT("create_only"), TEXT("delete_only"), TEXT("replace_scope") };
	if (!SupportedModes.Contains(Mode))
	{
		OutError = TEXT("invalid_mode");
		return false;
	}

	FString DeletePolicy;
	UeAgentLevelContentJson::TryGetStringAny(Ctx.Params, { TEXT("delete_policy") }, DeletePolicy);
	DeletePolicy = DeletePolicy.TrimStartAndEnd().ToLower();
	const TSet<FString> SupportedDeletePolicies = { TEXT(""), TEXT("none"), TEXT("only_managed_missing"), TEXT("replace_scope_managed") };
	if (!SupportedDeletePolicies.Contains(DeletePolicy))
	{
		OutError = TEXT("invalid_delete_policy");
		return false;
	}
	const bool bDestructive = LevelContentRequestsDelete(Content, Mode, DeletePolicy);
	if (!bDryRun && bDestructive && !bAllowDestructive)
	{
		OutData->SetStringField(TEXT("status"), TEXT("blocked"));
		OutData->SetBoolField(TEXT("dry_run"), false);
		OutData->SetBoolField(TEXT("destructive"), true);
		OutData->SetBoolField(TEXT("allow_destructive"), false);
		OutError = TEXT("allow_destructive_required");
		return false;
	}

	TSharedPtr<FJsonObject> Validation;
	UeAgentLevelContentJson::ValidateLevelContent(World, Content, Ctx.Params, bActorOnly, Validation);
	if ((bPreflight || !bDryRun) && Validation.IsValid() && !Validation->GetBoolField(TEXT("valid")))
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), UeAgentLevelContentJson::ApplySchema);
		OutData->SetStringField(TEXT("source"), Source);
		OutData->SetStringField(TEXT("status"), TEXT("preflight_failed"));
		OutData->SetObjectField(TEXT("validation"), Validation);
		OutError = bPreflight ? TEXT("level_content_preflight_failed") : TEXT("level_content_validation_failed");
		return false;
	}

	if (bDryRun)
	{
		OutData = UeAgentLevelContentJson::BuildPlan(World, Content, Ctx.Params, bActorOnly);
		OutData->SetStringField(TEXT("source"), Source);
		OutData->SetBoolField(TEXT("dry_run"), true);
		OutData->SetBoolField(TEXT("destructive"), bDestructive);
		OutData->SetBoolField(TEXT("allow_destructive"), bAllowDestructive);
		OutData->SetObjectField(TEXT("validation"), Validation);
		return true;
	}

	TArray<TSharedPtr<FJsonObject>> Resources;
	UeAgentLevelContentJson::CollectResourceObjects(Content, Resources);
	TArray<TSharedPtr<FJsonValue>> Results;
	TArray<TSharedPtr<FJsonValue>> AttachmentResults;
	TArray<TSharedPtr<FJsonValue>> OperationResults;
	TArray<TSharedPtr<FJsonValue>> TaskResults;
	TArray<TSharedPtr<FJsonValue>> ValidationResults;
	TSharedPtr<FJsonObject> EvidenceResults;
	TMap<FString, AActor*> ActorById;
	TSet<FString> TargetIds;
	const TArray<FString> ProtectionTags = UeAgentLevelContentJson::ReadProtectionTags(Content);
	bool bMayNeedRollback = true;

	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UeAgentInterface", "LevelContentApplyJsonTx", "UeAgentInterface Level Content Apply JSON"));
	}

	bool bOk = true;
	FString LocalError;
	for (const TSharedPtr<FJsonObject>& Resource : Resources)
	{
		const FString Id = UeAgentLevelContentJson::GetResourceId(Resource);
		if (!Id.IsEmpty())
		{
			TargetIds.Add(Id);
		}
		TSharedPtr<FJsonObject> Result;
		if (!UeAgentLevelContentJson::ApplyResource(World, Content, Ctx.Params, Resource, Mode, ProtectionTags, ActorById, Result, LocalError))
		{
			Results.Add(MakeShared<FJsonValueObject>(Result));
			bOk = false;
			break;
		}
		bMayNeedRollback = true;
		Results.Add(MakeShared<FJsonValueObject>(Result));
	}

	if (bOk)
	{
		bOk = UeAgentLevelContentJson::ApplyAttachments(World, Resources, ActorById, AttachmentResults, LocalError);
	}

	if (bOk && (Mode == TEXT("replace_scope") || DeletePolicy == TEXT("only_managed_missing") || DeletePolicy == TEXT("replace_scope_managed")))
	{
		bOk = UeAgentLevelContentJson::DeleteManagedMissing(World, Content, Ctx.Params, TargetIds, ProtectionTags, false, Results, LocalError);
	}

	if (bOk)
	{
		bOk = UeAgentLevelContentJson::ExecuteOperations(World, Content, Ctx.Params, false, ProtectionTags, OperationResults, LocalError);
	}
	if (bOk)
	{
		bOk = UeAgentLevelContentJson::ExecuteTasks(World, Content, false, TaskResults, LocalError);
	}
	if (bOk && bValidateAfterApply)
	{
		UeAgentLevelContentJson::ExecuteValidations(World, Content, ValidationResults);
		if (!LevelContentValidationResultsPassed(ValidationResults))
		{
			LocalError = TEXT("level_content_validation_failed");
			bOk = false;
		}
	}
	if (bOk)
	{
		FString EvidenceError;
		EvidenceResults = UeAgentLevelContentJson::BuildEvidenceReport(World, Content, Ctx.Params, TEXT("post_apply"), false, true, EvidenceError);
		if (!EvidenceError.IsEmpty())
		{
			LocalError = EvidenceError;
			bOk = false;
		}
	}

	bool bRollbackAttempted = false;
	bool bRollbackSucceeded = false;
	if (!bOk && Transaction && bMayNeedRollback)
	{
		Transaction.Reset();
		bRollbackAttempted = true;
		bRollbackSucceeded = GEditor && GEditor->UndoTransaction();
	}
	else if (!bOk && Transaction)
	{
		Transaction->Cancel();
		Transaction.Reset();
	}
	else
	{
		Transaction.Reset();
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), UeAgentLevelContentJson::ApplySchema);
	OutData->SetStringField(TEXT("source"), Source);
	OutData->SetStringField(TEXT("status"), bOk ? TEXT("applied") : TEXT("failed"));
	OutData->SetBoolField(TEXT("success"), bOk);
	OutData->SetBoolField(TEXT("dry_run"), false);
	OutData->SetBoolField(TEXT("destructive"), bDestructive);
	OutData->SetBoolField(TEXT("allow_destructive"), bAllowDestructive);
	OutData->SetBoolField(TEXT("validate_after_apply"), bValidateAfterApply);
	OutData->SetBoolField(TEXT("transaction"), bTransaction);
	OutData->SetBoolField(TEXT("rollback_attempted"), bRollbackAttempted);
	OutData->SetBoolField(TEXT("rollback_succeeded"), bRollbackSucceeded);
	OutData->SetObjectField(TEXT("validation"), Validation);
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetArrayField(TEXT("resource_results"), Results);
	OutData->SetArrayField(TEXT("attachment_results"), AttachmentResults);
	OutData->SetArrayField(TEXT("operation_results"), OperationResults);
	OutData->SetArrayField(TEXT("task_results"), TaskResults);
	OutData->SetArrayField(TEXT("validation_results"), ValidationResults);
	OutData->SetBoolField(TEXT("saved"), false);
	if (EvidenceResults.IsValid())
	{
		OutData->SetObjectField(TEXT("evidence_results"), EvidenceResults);
	}
	OutData->SetObjectField(TEXT("feature_gates"), UeAgentLevelContentJson::BuildFeatureGateReport(Content));
	if (!bOk)
	{
		OutData->SetStringField(TEXT("error"), LocalError);
		OutError = LocalError.IsEmpty() ? TEXT("level_content_apply_failed") : LocalError;
		return false;
	}

	bool bSaveLevel = false;
	UeAgentLevelContentJson::TryGetBoolAny(Ctx.Params, { TEXT("save_level") }, bSaveLevel);
	bool bAllowPythonGcRisk = false;
	UeAgentLevelContentJson::TryGetBoolAny(Ctx.Params, { TEXT("allow_python_gc_risk") }, bAllowPythonGcRisk);
	TSharedPtr<FJsonObject> SavePolicy;
	if (UeAgentLevelContentJson::TryGetObject(Content, TEXT("save_policy"), SavePolicy))
	{
		UeAgentLevelContentJson::TryGetBoolAny(SavePolicy, { TEXT("save_level") }, bSaveLevel);
		UeAgentLevelContentJson::TryGetBoolAny(SavePolicy, { TEXT("allow_python_gc_risk") }, bAllowPythonGcRisk);
	}
	if (bSaveLevel)
	{
		FString SaveError;
		TSharedPtr<FJsonObject> SaveData = MakeShared<FJsonObject>();
		FUeAgentRequestContext SaveCtx = Ctx;
		SaveCtx.Params = MakeShared<FJsonObject>();
		SaveCtx.Params->SetBoolField(TEXT("allow_python_gc_risk"), bAllowPythonGcRisk);
		const bool bSaved = CmdSaveCurrentLevel(SaveCtx, SaveData, SaveError);
		OutData->SetBoolField(TEXT("saved"), bSaved);
		OutData->SetBoolField(TEXT("save_level_ok"), bSaved);
		OutData->SetObjectField(TEXT("save_level_data"), SaveData);
		if (!bSaved)
		{
			OutData->SetStringField(TEXT("save_level_error"), SaveError);
		}
	}
	return true;
}

bool FUeAgentLegacyMechanics::ExecuteLevelContentDiffJsonInternal(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError, bool bActorOnly) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Content;
	FString Source;
	if (!UeAgentLevelContentJson::ResolveContentObject(Ctx.Params, Content, Source, OutError))
	{
		return false;
	}
	OutData = UeAgentLevelContentJson::DiffAgainstWorld(World, Content, Ctx.Params, bActorOnly);
	OutData->SetStringField(TEXT("source"), Source);
	return true;
}

bool FUeAgentLegacyMechanics::CmdLevelContentDeleteScope(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	bool bDryRun = true;
	bool bAllowDestructive = false;
	if (!UeAgentCommandSafety::ReadStrictBool(Ctx.Params, TEXT("dry_run"), true, bDryRun, OutError) ||
		!UeAgentCommandSafety::ReadStrictBool(Ctx.Params, TEXT("allow_destructive"), false, bAllowDestructive, OutError))
	{
		return false;
	}
	if (!bDryRun && !bAllowDestructive)
	{
		OutData->SetStringField(TEXT("status"), TEXT("blocked"));
		OutData->SetBoolField(TEXT("dry_run"), false);
		OutData->SetBoolField(TEXT("allow_destructive"), false);
		OutData->SetNumberField(TEXT("deleted_count"), 0);
		OutError = TEXT("allow_destructive_required");
		return false;
	}

	bool bRecursive = true;
	UeAgentLevelContentJson::TryGetBoolAny(Ctx.Params, { TEXT("recursive"), TEXT("include_child_folders") }, bRecursive);

	bool bFailOnBlocked = true;
	UeAgentLevelContentJson::TryGetBoolAny(Ctx.Params, { TEXT("fail_on_blocked") }, bFailOnBlocked);

	bool bDeleteEmptyFolders = true;
	UeAgentLevelContentJson::TryGetBoolAny(Ctx.Params, { TEXT("delete_empty_folders"), TEXT("delete_folders") }, bDeleteEmptyFolders);

	bool bSaveLevel = false;
	UeAgentLevelContentJson::TryGetBoolAny(Ctx.Params, { TEXT("save_level"), TEXT("save_after_delete") }, bSaveLevel);

	TArray<FString> ProtectionTags = UeAgentLevelContentJson::ReadStringArray(Ctx.Params, TEXT("protected_tags"));
	TArray<FString> ManagedIds = UeAgentLevelContentJson::ReadStringArray(Ctx.Params, TEXT("managed_ids"));
	FString SingleManagedId;
	if (UeAgentLevelContentJson::TryGetStringAny(Ctx.Params, { TEXT("managed_id") }, SingleManagedId) && !SingleManagedId.TrimStartAndEnd().IsEmpty())
	{
		ManagedIds.Add(SingleManagedId.TrimStartAndEnd());
	}

	TSet<FString> ManagedIdSet;
	for (FString ManagedId : ManagedIds)
	{
		ManagedId.TrimStartAndEndInline();
		if (!ManagedId.IsEmpty())
		{
			ManagedIdSet.Add(ManagedId);
		}
	}

	TArray<FString> RequestedFolderPaths;
	auto AddFolderPath = [&RequestedFolderPaths](FString FolderPath)
	{
		FolderPath = UeAgentLevelContentJson::NormalizeFolder(FolderPath);
		if (!FolderPath.IsEmpty())
		{
			RequestedFolderPaths.Add(FolderPath);
		}
	};

	FString FolderRoot;
	if (UeAgentLevelContentJson::TryGetStringAny(Ctx.Params, { TEXT("folder_root"), TEXT("folder_path"), TEXT("folder") }, FolderRoot))
	{
		AddFolderPath(FolderRoot);
	}
	for (const FString& FolderPath : UeAgentLevelContentJson::ReadStringArray(Ctx.Params, TEXT("folder_paths")))
	{
		AddFolderPath(FolderPath);
	}
	for (const FString& FolderPath : UeAgentLevelContentJson::ReadStringArray(Ctx.Params, TEXT("folder_roots")))
	{
		AddFolderPath(FolderPath);
	}

	FString ScopedManagedTag;
	bool bHasScopedManagedTag = false;
	TSharedPtr<FJsonObject> Scope;
	if (UeAgentLevelContentJson::TryGetObject(Ctx.Params, TEXT("scope"), Scope))
	{
		FString ScopeFolderRoot;
		if (UeAgentLevelContentJson::TryGetStringAny(Scope, { TEXT("folder_root"), TEXT("folder_path"), TEXT("folder") }, ScopeFolderRoot))
		{
			AddFolderPath(ScopeFolderRoot);
		}
		if (UeAgentLevelContentJson::TryGetStringAny(Scope, { TEXT("managed_tag") }, ScopedManagedTag) && !ScopedManagedTag.TrimStartAndEnd().IsEmpty())
		{
			ScopedManagedTag = ScopedManagedTag.TrimStartAndEnd();
			bHasScopedManagedTag = true;
		}
	}

	TArray<FString> UniqueFolderPaths;
	TSet<FString> SeenFolderPaths;
	for (const FString& FolderPath : RequestedFolderPaths)
	{
		if (!SeenFolderPaths.Contains(FolderPath))
		{
			SeenFolderPaths.Add(FolderPath);
			UniqueFolderPaths.Add(FolderPath);
		}
	}

	if (UniqueFolderPaths.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}

	FString ManagedTag;
	bool bHasManagedTag = UeAgentLevelContentJson::TryGetStringAny(Ctx.Params, { TEXT("managed_tag") }, ManagedTag) && !ManagedTag.TrimStartAndEnd().IsEmpty();
	if (!bHasManagedTag && bHasScopedManagedTag)
	{
		ManagedTag = ScopedManagedTag;
		bHasManagedTag = true;
	}
	ManagedTag = bHasManagedTag ? ManagedTag.TrimStartAndEnd() : FString();

	bool bAllowUnmanagedDelete = false;
	if (!UeAgentCommandSafety::ReadStrictBool(Ctx.Params, TEXT("allow_unmanaged_delete"), false, bAllowUnmanagedDelete, OutError))
	{
		return false;
	}

	auto MakeReason = [](const FString& Reason, const FString& Detail = FString()) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("reason"), Reason);
		if (!Detail.IsEmpty())
		{
			Obj->SetStringField(TEXT("detail"), Detail);
		}
		return Obj;
	};

	auto AddActorParents = [](const FString& FolderPath, TSet<FString>& OutUsedFolders)
	{
		TArray<FString> Parts;
		FolderPath.ParseIntoArray(Parts, TEXT("/"), true);
		FString Accum;
		for (const FString& Part : Parts)
		{
			Accum = Accum.IsEmpty() ? Part : Accum + TEXT("/") + Part;
			OutUsedFolders.Add(UeAgentLevelContentJson::NormalizeFolder(Accum));
		}
	};

	struct FActorDeleteCandidate
	{
		TWeakObjectPtr<AActor> Actor;
		FString InputFolderPath;
		FString ActorName;
		FString ActorLabel;
		FString ObjectPath;
		FString FolderPath;
		FString ManagedId;
		bool bDeletable = false;
	};

	TArray<TSharedPtr<FJsonValue>> FolderExpansionJson;
	TArray<TSharedPtr<FJsonValue>> ActorsJson;
	TArray<TSharedPtr<FJsonValue>> BlockedJson;
	TArray<FActorDeleteCandidate> Candidates;
	TArray<AActor*> ActorsToDelete;
	TSet<FString> SeenObjectPaths;
	int32 DuplicateCount = 0;
	int32 DeletableCount = 0;
	int32 ManagedIdFilteredCount = 0;

	auto MakeActorReport = [&MakeReason, &ProtectionTags, &ManagedIdSet, &ManagedTag, bAllowUnmanagedDelete](AActor* Actor, const FString& InputFolderPath, bool& bOutDeletable, bool& bOutFilteredByManagedId) -> TSharedPtr<FJsonObject>
	{
		bOutDeletable = false;
		bOutFilteredByManagedId = false;

		TSharedPtr<FJsonObject> ActorReport = MakeShared<FJsonObject>();
		if (!Actor)
		{
			TArray<TSharedPtr<FJsonValue>> Reasons;
			Reasons.Add(MakeShared<FJsonValueObject>(MakeReason(TEXT("actor_not_found"))));
			ActorReport->SetStringField(TEXT("input_folder_path"), InputFolderPath);
			ActorReport->SetBoolField(TEXT("deletable"), false);
			ActorReport->SetArrayField(TEXT("blocked_reasons"), Reasons);
			return ActorReport;
		}

		const FString ManagedId = UeAgentLevelContentJson::GetManagedId(Actor);
		ActorReport->SetStringField(TEXT("input_folder_path"), InputFolderPath);
		ActorReport->SetStringField(TEXT("actor"), Actor->GetActorLabel());
		ActorReport->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		ActorReport->SetStringField(TEXT("actor_name"), Actor->GetName());
		ActorReport->SetStringField(TEXT("object_path"), Actor->GetPathName());
		ActorReport->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
		ActorReport->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
		ActorReport->SetStringField(TEXT("managed_id"), ManagedId);

		TArray<TSharedPtr<FJsonValue>> TagsJson;
		for (const FName& Tag : Actor->Tags)
		{
			TagsJson.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		ActorReport->SetArrayField(TEXT("tags"), TagsJson);

		TArray<TSharedPtr<FJsonValue>> Reasons;
		if (Actor->IsActorBeingDestroyed())
		{
			Reasons.Add(MakeShared<FJsonValueObject>(MakeReason(TEXT("actor_already_being_destroyed"))));
		}

		if (!ManagedIdSet.IsEmpty() && !ManagedIdSet.Contains(ManagedId))
		{
			bOutFilteredByManagedId = true;
			Reasons.Add(MakeShared<FJsonValueObject>(MakeReason(TEXT("managed_id_not_requested"))));
		}

		if (!bAllowUnmanagedDelete)
		{
			const bool bHasManagedTag = !ManagedTag.IsEmpty() && UeAgentLevelContentJson::ActorHasTagString(Actor, ManagedTag);
			const bool bHasManagedId = !ManagedId.IsEmpty();
			if (!bHasManagedTag && !bHasManagedId)
			{
				Reasons.Add(MakeShared<FJsonValueObject>(MakeReason(TEXT("unmanaged_actor"), TEXT("pass allow_unmanaged_delete=true to delete actors without managed tag/id"))));
			}
		}

		if (UeAgentLevelContentJson::ActorIsProtected(Actor, ProtectionTags))
		{
			Reasons.Add(MakeShared<FJsonValueObject>(MakeReason(TEXT("actor_protected"))));
		}

		bOutDeletable = Reasons.IsEmpty();
		ActorReport->SetBoolField(TEXT("deletable"), bOutDeletable);
		ActorReport->SetArrayField(TEXT("blocked_reasons"), Reasons);
		return ActorReport;
	};

	for (const FString& RequestedFolderPath : UniqueFolderPaths)
	{
		TSharedPtr<FJsonObject> FolderReport = MakeShared<FJsonObject>();
		FolderReport->SetStringField(TEXT("folder_path"), RequestedFolderPath);
		FolderReport->SetBoolField(TEXT("recursive"), bRecursive);

		TArray<TSharedPtr<FJsonValue>> FolderActorsJson;
		int32 FolderActorCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			if (!UeAgentLevelContentJson::FolderMatches(Actor->GetFolderPath().ToString(), RequestedFolderPath, bRecursive))
			{
				continue;
			}

			++FolderActorCount;
			TSharedPtr<FJsonObject> FolderActor = MakeShared<FJsonObject>();
			FolderActor->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
			FolderActor->SetStringField(TEXT("actor_name"), Actor->GetName());
			FolderActor->SetStringField(TEXT("object_path"), Actor->GetPathName());
			FolderActor->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
			FolderActorsJson.Add(MakeShared<FJsonValueObject>(FolderActor));

			const FString ObjectPath = Actor->GetPathName();
			if (SeenObjectPaths.Contains(ObjectPath))
			{
				++DuplicateCount;
				continue;
			}
			SeenObjectPaths.Add(ObjectPath);

			bool bDeletable = false;
			bool bFilteredByManagedId = false;
			TSharedPtr<FJsonObject> ActorReport = MakeActorReport(Actor, RequestedFolderPath, bDeletable, bFilteredByManagedId);
			if (bFilteredByManagedId)
			{
				++ManagedIdFilteredCount;
			}
			ActorsJson.Add(MakeShared<FJsonValueObject>(ActorReport));
			if (!bDeletable)
			{
				BlockedJson.Add(MakeShared<FJsonValueObject>(ActorReport));
				continue;
			}

			++DeletableCount;
			ActorsToDelete.Add(Actor);
			FActorDeleteCandidate Candidate;
			Candidate.Actor = Actor;
			Candidate.InputFolderPath = RequestedFolderPath;
			Candidate.ActorName = Actor->GetName();
			Candidate.ActorLabel = Actor->GetActorLabel();
			Candidate.ObjectPath = ObjectPath;
			Candidate.FolderPath = Actor->GetFolderPath().ToString();
			Candidate.ManagedId = UeAgentLevelContentJson::GetManagedId(Actor);
			Candidate.bDeletable = true;
			Candidates.Add(Candidate);
		}

		FolderReport->SetStringField(TEXT("status"), TEXT("expanded"));
		FolderReport->SetNumberField(TEXT("actor_count"), FolderActorCount);
		FolderReport->SetArrayField(TEXT("actors"), FolderActorsJson);
		FolderExpansionJson.Add(MakeShared<FJsonValueObject>(FolderReport));
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), UeAgentLevelContentJson::DeleteScopeSchema);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("recursive"), bRecursive);
	OutData->SetBoolField(TEXT("include_child_folders"), bRecursive);
	OutData->SetBoolField(TEXT("fail_on_blocked"), bFailOnBlocked);
	OutData->SetBoolField(TEXT("allow_destructive"), bAllowDestructive);
	OutData->SetBoolField(TEXT("allow_unmanaged_delete"), bAllowUnmanagedDelete);
	OutData->SetBoolField(TEXT("delete_empty_folders"), bDeleteEmptyFolders);
	OutData->SetBoolField(TEXT("save_level"), bSaveLevel);
	OutData->SetStringField(TEXT("folder_root"), UniqueFolderPaths.Num() > 0 ? UniqueFolderPaths[0] : FString());
	OutData->SetStringField(TEXT("managed_tag"), ManagedTag);
	TArray<TSharedPtr<FJsonValue>> FolderPathsJson;
	for (const FString& FolderPath : UniqueFolderPaths)
	{
		FolderPathsJson.Add(MakeShared<FJsonValueString>(FolderPath));
	}
	OutData->SetArrayField(TEXT("folder_paths"), FolderPathsJson);
	OutData->SetNumberField(TEXT("requested_folder_count"), UniqueFolderPaths.Num());
	OutData->SetArrayField(TEXT("folder_expansions"), FolderExpansionJson);
	OutData->SetNumberField(TEXT("requested_count"), ActorsJson.Num());
	OutData->SetNumberField(TEXT("matched_count"), ActorsJson.Num());
	OutData->SetNumberField(TEXT("unique_actor_count"), SeenObjectPaths.Num());
	OutData->SetNumberField(TEXT("duplicate_count"), DuplicateCount);
	OutData->SetNumberField(TEXT("managed_id_filtered_count"), ManagedIdFilteredCount);
	OutData->SetNumberField(TEXT("deletable_count"), DeletableCount);
	OutData->SetNumberField(TEXT("blocked_count"), BlockedJson.Num());
	OutData->SetNumberField(TEXT("would_delete_count"), DeletableCount);
	OutData->SetArrayField(TEXT("actors"), ActorsJson);
	OutData->SetArrayField(TEXT("blocked"), BlockedJson);
	OutData->SetArrayField(TEXT("results"), ActorsJson);

	if (!BlockedJson.IsEmpty() && bFailOnBlocked)
	{
		OutError = TEXT("level_content_delete_scope_blocked");
		return false;
	}

	bool bDestroyActorsOk = true;
	if (!bDryRun && !ActorsToDelete.IsEmpty())
	{
		UEditorActorSubsystem* ActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
		if (!ActorSubsystem)
		{
			OutError = TEXT("editor_actor_subsystem_unavailable");
			return false;
		}

		FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "LevelContentDeleteScopeTx", "UeAgentInterface Level Content Delete Scope"));
		for (AActor* Actor : ActorsToDelete)
		{
			if (Actor)
			{
				Actor->Modify();
			}
		}
		bDestroyActorsOk = ActorSubsystem->DestroyActors(ActorsToDelete);
	}

	TArray<TSharedPtr<FJsonValue>> DeletedJson;
	int32 DeletedCount = 0;
	int32 RemainingActorCount = 0;
	for (const FActorDeleteCandidate& Candidate : Candidates)
	{
		const bool bActorExistsAfter = Candidate.Actor.IsValid() && !Candidate.Actor->IsActorBeingDestroyed();
		if (!bDryRun && !bActorExistsAfter)
		{
			++DeletedCount;
		}
		if (!bDryRun && bActorExistsAfter)
		{
			++RemainingActorCount;
		}

		TSharedPtr<FJsonObject> Deleted = MakeShared<FJsonObject>();
		Deleted->SetStringField(TEXT("input_folder_path"), Candidate.InputFolderPath);
		Deleted->SetStringField(TEXT("actor"), Candidate.ActorLabel);
		Deleted->SetStringField(TEXT("actor_label"), Candidate.ActorLabel);
		Deleted->SetStringField(TEXT("actor_name"), Candidate.ActorName);
		Deleted->SetStringField(TEXT("object_path"), Candidate.ObjectPath);
		Deleted->SetStringField(TEXT("folder_path"), Candidate.FolderPath);
		Deleted->SetStringField(TEXT("managed_id"), Candidate.ManagedId);
		Deleted->SetBoolField(TEXT("would_delete"), true);
		Deleted->SetBoolField(TEXT("deleted"), !bDryRun && !bActorExistsAfter);
		Deleted->SetBoolField(TEXT("actor_exists_after"), !bDryRun && bActorExistsAfter);
		DeletedJson.Add(MakeShared<FJsonValueObject>(Deleted));
	}
	OutData->SetBoolField(TEXT("destroy_actors_ok"), bDestroyActorsOk);
	OutData->SetNumberField(TEXT("deleted_count"), bDryRun ? 0 : DeletedCount);
	OutData->SetArrayField(TEXT("deleted_actors"), DeletedJson);
	OutData->SetNumberField(TEXT("remaining_actor_count"), RemainingActorCount);

	FActorFolders& ActorFolders = FActorFolders::Get();
	auto CollectAllFolders = [&ActorFolders, World]()
	{
		TArray<FFolder> AllFolders;
		ActorFolders.ForEachFolder(*World, [&AllFolders](const FFolder& Folder)
		{
			AllFolders.Add(Folder);
			return true;
		});
		return AllFolders;
	};

	auto FolderExists = [&ActorFolders, World](const FString& FolderPath)
	{
		bool bExists = false;
		ActorFolders.ForEachFolder(*World, [&bExists, &FolderPath](const FFolder& Folder)
		{
			if (UeAgentLevelContentJson::NormalizeFolder(Folder.ToString()).Equals(FolderPath, ESearchCase::IgnoreCase))
			{
				bExists = true;
				return false;
			}
			return true;
		});
		return bExists;
	};

	TArray<TSharedPtr<FJsonValue>> FolderDeletionJson;
	int32 FolderDeleteCount = 0;
	int32 FolderWouldDeleteCount = 0;
	int32 FolderDeleteBlockedCount = 0;
	if (!UniqueFolderPaths.IsEmpty())
	{
		TSet<FString> ExpectedDeletedActorPaths;
		if (bDryRun)
		{
			for (const FActorDeleteCandidate& Candidate : Candidates)
			{
				ExpectedDeletedActorPaths.Add(Candidate.ObjectPath);
			}
		}

		TSet<FString> UsedFolders;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			if (bDryRun && ExpectedDeletedActorPaths.Contains(Actor->GetPathName()))
			{
				continue;
			}
			const FString ActorFolder = UeAgentLevelContentJson::NormalizeFolder(Actor->GetFolderPath().ToString());
			if (!ActorFolder.IsEmpty())
			{
				AddActorParents(ActorFolder, UsedFolders);
			}
		}

		TArray<FFolder> ExistingFolders = CollectAllFolders();
		TArray<FFolder> CandidateFolders;
		TSet<FString> CandidateFolderPaths;
		for (const FFolder& Folder : ExistingFolders)
		{
			const FString FolderPath = UeAgentLevelContentJson::NormalizeFolder(Folder.ToString());
			if (FolderPath.IsEmpty())
			{
				continue;
			}
			for (const FString& RequestedFolderPath : UniqueFolderPaths)
			{
				if (UeAgentLevelContentJson::FolderMatches(FolderPath, RequestedFolderPath, bRecursive))
				{
					CandidateFolders.Add(Folder);
					CandidateFolderPaths.Add(FolderPath);
					break;
				}
			}
		}
		CandidateFolders.Sort([](const FFolder& A, const FFolder& B)
		{
			return A.ToString().Len() > B.ToString().Len();
		});

		for (const FString& RequestedFolderPath : UniqueFolderPaths)
		{
			if (!CandidateFolderPaths.Contains(RequestedFolderPath))
			{
				TSharedPtr<FJsonObject> Missing = MakeShared<FJsonObject>();
				Missing->SetStringField(TEXT("folder_path"), RequestedFolderPath);
				Missing->SetBoolField(TEXT("recursive"), bRecursive);
				Missing->SetBoolField(TEXT("delete_empty_folders"), bDeleteEmptyFolders);
				Missing->SetStringField(TEXT("status"), TEXT("missing_noop"));
				Missing->SetBoolField(TEXT("would_delete"), false);
				Missing->SetBoolField(TEXT("deleted"), false);
				Missing->SetBoolField(TEXT("folder_exists_after"), false);
				Missing->SetArrayField(TEXT("blocked_reasons"), TArray<TSharedPtr<FJsonValue>>());
				FolderDeletionJson.Add(MakeShared<FJsonValueObject>(Missing));
			}
		}

		for (const FFolder& Folder : CandidateFolders)
		{
			const FString FolderPath = UeAgentLevelContentJson::NormalizeFolder(Folder.ToString());
			TSharedPtr<FJsonObject> FolderResult = MakeShared<FJsonObject>();
			FolderResult->SetStringField(TEXT("folder_path"), FolderPath);
			FolderResult->SetBoolField(TEXT("recursive"), bRecursive);
			FolderResult->SetBoolField(TEXT("delete_empty_folders"), bDeleteEmptyFolders);

			TArray<TSharedPtr<FJsonValue>> FolderReasons;
			if (!bDeleteEmptyFolders)
			{
				FolderResult->SetStringField(TEXT("status"), TEXT("disabled"));
				FolderResult->SetBoolField(TEXT("would_delete"), false);
				FolderResult->SetBoolField(TEXT("deleted"), false);
				FolderResult->SetBoolField(TEXT("folder_exists_after"), FolderExists(FolderPath));
				FolderResult->SetArrayField(TEXT("blocked_reasons"), FolderReasons);
				FolderDeletionJson.Add(MakeShared<FJsonValueObject>(FolderResult));
				continue;
			}

			if (UsedFolders.Contains(FolderPath))
			{
				FolderReasons.Add(MakeShared<FJsonValueObject>(MakeReason(TEXT("folder_not_empty"))));
			}

			const bool bFolderBlocked = !FolderReasons.IsEmpty();
			const bool bWouldDeleteFolder = !bFolderBlocked;
			bool bFolderDeleted = false;
			if (!bDryRun && bWouldDeleteFolder)
			{
				ActorFolders.DeleteFolder(*World, Folder);
				bFolderDeleted = !FolderExists(FolderPath);
				if (!bFolderDeleted)
				{
					FolderReasons.Add(MakeShared<FJsonValueObject>(MakeReason(TEXT("delete_folder_failed"))));
				}
			}
			const bool bFolderExistsAfter = bDryRun ? true : FolderExists(FolderPath);

			if (bDryRun && bWouldDeleteFolder)
			{
				++FolderWouldDeleteCount;
			}
			else if (!bDryRun && bFolderDeleted)
			{
				++FolderDeleteCount;
			}
			if (!FolderReasons.IsEmpty())
			{
				++FolderDeleteBlockedCount;
			}

			FolderResult->SetStringField(TEXT("status"), FolderReasons.IsEmpty() ? (bDryRun ? TEXT("would_delete") : TEXT("deleted")) : TEXT("blocked"));
			FolderResult->SetBoolField(TEXT("would_delete"), bWouldDeleteFolder);
			FolderResult->SetBoolField(TEXT("deleted"), bFolderDeleted);
			FolderResult->SetBoolField(TEXT("folder_exists_after"), bFolderExistsAfter);
			FolderResult->SetArrayField(TEXT("blocked_reasons"), FolderReasons);
			FolderDeletionJson.Add(MakeShared<FJsonValueObject>(FolderResult));
		}
	}
	OutData->SetArrayField(TEXT("folder_deletions"), FolderDeletionJson);
	OutData->SetNumberField(TEXT("folder_delete_count"), FolderDeleteCount);
	OutData->SetNumberField(TEXT("folder_would_delete_count"), FolderWouldDeleteCount);
	OutData->SetNumberField(TEXT("folder_delete_blocked_count"), FolderDeleteBlockedCount);

	OutData->SetBoolField(TEXT("saved"), false);
	if (!bDryRun && bSaveLevel)
	{
		bool bAllowPythonGcRisk = false;
		UeAgentLevelContentJson::TryGetBoolAny(Ctx.Params, { TEXT("allow_python_gc_risk") }, bAllowPythonGcRisk);
		FString SaveError;
		TSharedPtr<FJsonObject> SaveData = MakeShared<FJsonObject>();
		FUeAgentRequestContext SaveCtx = Ctx;
		SaveCtx.Params = MakeShared<FJsonObject>();
		SaveCtx.Params->SetBoolField(TEXT("allow_python_gc_risk"), bAllowPythonGcRisk);
		const bool bSaved = CmdSaveCurrentLevel(SaveCtx, SaveData, SaveError);
		OutData->SetBoolField(TEXT("saved"), bSaved);
		OutData->SetBoolField(TEXT("save_level_ok"), bSaved);
		OutData->SetObjectField(TEXT("save_level_data"), SaveData);
		if (!bSaved)
		{
			OutData->SetStringField(TEXT("save_level_error"), SaveError);
		}
	}

	if (!bDryRun && (!bDestroyActorsOk || RemainingActorCount > 0))
	{
		OutError = TEXT("level_content_delete_scope_incomplete");
		return false;
	}
	if (!bDryRun && bDeleteEmptyFolders && FolderDeleteBlockedCount > 0)
	{
		OutError = TEXT("level_content_delete_scope_folder_incomplete");
		return false;
	}
	return true;
}
