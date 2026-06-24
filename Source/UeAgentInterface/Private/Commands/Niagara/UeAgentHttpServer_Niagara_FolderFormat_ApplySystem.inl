bool FUeAgentHttpServer::CmdNiagaraApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderPath;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath);
	FolderPath.TrimStartAndEndInline();

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	AssetPathInput.TrimStartAndEndInline();

	if (FolderPath.IsEmpty())
	{
		if (AssetPathInput.IsEmpty())
		{
			OutError = TEXT("missing_folder_path_or_asset_path");
			return false;
		}
		if (!UeAgentNiagaraFolderOps::ResolveFolderForAsset(AssetPathInput, FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = FPaths::ConvertRelativePathToFull(FolderPath);

	TSharedPtr<FJsonObject> AssetObj;
	if (!UeAgentNiagaraFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetObj, OutError))
	{
		return false;
	}

	FString AssetPathFromFile;
	AssetObj->TryGetStringField(TEXT("asset_path"), AssetPathFromFile);
	if (AssetPathInput.IsEmpty())
	{
		AssetPathInput = AssetPathFromFile;
	}
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString AssetPath = UeAgentNiagaraOps::NormalizeAssetPath(AssetPathInput);
	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPath));
	bool bCreateIfMissing = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	bool bCreated = false;
	if (!NiagaraSystem && bCreateIfMissing)
	{
		NiagaraSystem = UeAgentNiagaraFolderOps::CreateNiagaraSystemAsset(AssetPath, OutError);
		bCreated = NiagaraSystem != nullptr;
	}
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	bool bApplyReferencedEmitters = true;
	JsonTryGetBool(Ctx.Params, TEXT("apply_referenced_emitters"), bApplyReferencedEmitters);
	bool bStrict = false;
	JsonTryGetBool(Ctx.Params, TEXT("strict"), bStrict);
	bool bRestorePreviewAfterApply = true;
	UeAgentNiagaraOps::TryGetBoolAnyField(
		Ctx.Params,
		{
			TEXT("restore_preview_after_apply"),
			TEXT("resume_preview_after_apply"),
			TEXT("restore_interactive_preview"),
			TEXT("resume_interactive_preview")
		},
		bRestorePreviewAfterApply);

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TMap<FString, int32> Stats;
	int32 AppliedSystemPropertyCount = 0;
	int32 AppliedUserParameterCount = 0;
	int32 AppliedSystemStageCount = 0;
	int32 AppliedEmitterHandleCount = 0;
	int32 AppliedEmitterPropertyCount = 0;
	int32 AppliedPostRefreshDataInterfaceCount = 0;

	NiagaraSystem->Modify();

	TSharedPtr<FJsonObject> SystemSettingsObj;
	if (!UeAgentNiagaraFolderOps::LoadJsonFileOptional(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("system.json")), SystemSettingsObj, OutError))
	{
		return false;
	}
	if (SystemSettingsObj.IsValid())
	{
		AppliedSystemPropertyCount += UeAgentNiagaraFolderOps::ApplyObjectPropertyArray(NiagaraSystem, SystemSettingsObj, TEXT("properties"), Warnings, NiagaraSystem->GetPathName());
		AppliedSystemPropertyCount += UeAgentNiagaraFolderOps::ApplyObjectPropertyArray(NiagaraSystem, SystemSettingsObj, TEXT("raw_properties"), Warnings, NiagaraSystem->GetPathName());
	}

	const int32 UserParamsApplied = UeAgentNiagaraFolderOps::ApplyUserParameters(*NiagaraSystem, FolderPath, Warnings, OutError);
	if (UserParamsApplied == INDEX_NONE)
	{
		return false;
	}
	AppliedUserParameterCount = UserParamsApplied;

	TSharedPtr<FJsonObject> EmittersIndexObj;
	if (!UeAgentNiagaraFolderOps::LoadJsonFileOptional(FPaths::Combine(FolderPath, TEXT("emitters"), TEXT("index.json")), EmittersIndexObj, OutError))
	{
		return false;
	}
	if (EmittersIndexObj.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
		if (UeAgentNiagaraFolderOps::TryGetArrayField(EmittersIndexObj, TEXT("emitters"), Emitters))
		{
			for (int32 EmitterIndex = 0; EmitterIndex < Emitters->Num(); ++EmitterIndex)
			{
				const TSharedPtr<FJsonValue>& EmitterValue = (*Emitters)[EmitterIndex];
				const TSharedPtr<FJsonObject>* EmitterObjPtr = nullptr;
				if (!EmitterValue.IsValid() || !EmitterValue->TryGetObject(EmitterObjPtr) || !EmitterObjPtr || !EmitterObjPtr->IsValid())
				{
					continue;
				}

				const TSharedPtr<FJsonObject>& EmitterObj = *EmitterObjPtr;
				const FString EmitterName = UeAgentNiagaraFolderOps::GetStringFieldAny(EmitterObj, { TEXT("emitter_name"), TEXT("name") }, FString::Printf(TEXT("Emitter_%d"), EmitterIndex));
				FString EmitterFolderRelative;
				EmitterObj->TryGetStringField(TEXT("folder"), EmitterFolderRelative);

				TSharedPtr<FJsonObject> SourceObj;
				FString SourceEmitterAssetPath;
				if (UeAgentNiagaraFolderOps::GetObjectField(EmitterObj, TEXT("source"), SourceObj))
				{
					SourceObj->TryGetStringField(TEXT("emitter_asset_path"), SourceEmitterAssetPath);
				}
				if (SourceEmitterAssetPath.IsEmpty())
				{
					EmitterObj->TryGetStringField(TEXT("emitter_asset_path"), SourceEmitterAssetPath);
				}

				FString EmitterVersionText;
				EmitterObj->TryGetStringField(TEXT("emitter_version"), EmitterVersionText);
				FGuid EmitterVersionGuid;
				FGuid::Parse(EmitterVersionText, EmitterVersionGuid);

				TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem->GetEmitterHandles();
				FNiagaraEmitterHandle* ExistingHandle = nullptr;
				int32 ExistingHandleIndex = INDEX_NONE;
				for (int32 HandleIndex = 0; HandleIndex < Handles.Num(); ++HandleIndex)
				{
					if (Handles[HandleIndex].GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
					{
						ExistingHandle = &Handles[HandleIndex];
						ExistingHandleIndex = HandleIndex;
						break;
					}
				}

				if (!ExistingHandle)
				{
					UNiagaraEmitter* SourceEmitter = Cast<UNiagaraEmitter>(UeAgentNiagaraOps::LoadAssetObject(SourceEmitterAssetPath));
					if (!SourceEmitter)
					{
						SourceEmitter = UeAgentNiagaraFolderOps::CreateTemplateEmitterForFolderApply(*NiagaraSystem, EmitterName);
						if (!SourceEmitter)
						{
							UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("emitter_source_not_found"), EmitterName, SourceEmitterAssetPath);
							continue;
						}
						if (!SourceEmitterAssetPath.IsEmpty())
						{
							const FString SourceMessage = FString::Printf(TEXT("Emitter source asset '%s' could not be loaded; generated a template emitter and applied folder data."), *SourceEmitterAssetPath);
							UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("emitter_source_generated_from_folder"), EmitterName, SourceMessage);
						}
					}

					const bool bCanUseExportedVersion = EmitterVersionGuid.IsValid() && SourceEmitter->GetEmitterData(EmitterVersionGuid) != nullptr;
					const FGuid AddVersion = bCanUseExportedVersion ? EmitterVersionGuid : SourceEmitter->GetExposedVersion().VersionGuid;
					const FNiagaraEmitterHandle NewHandle = NiagaraSystem->AddEmitterHandle(*SourceEmitter, *EmitterName, AddVersion);
					TArray<FNiagaraEmitterHandle>& UpdatedHandles = NiagaraSystem->GetEmitterHandles();
					for (int32 HandleIndex = 0; HandleIndex < UpdatedHandles.Num(); ++HandleIndex)
					{
						if (UpdatedHandles[HandleIndex].GetId() == NewHandle.GetId())
						{
							ExistingHandle = &UpdatedHandles[HandleIndex];
							ExistingHandleIndex = HandleIndex;
							break;
						}
					}
				}

				if (!ExistingHandle)
				{
					continue;
				}

				bool bEnabled = ExistingHandle->GetIsEnabled();
				if (EmitterObj->TryGetBoolField(TEXT("enabled"), bEnabled))
				{
					ExistingHandle->SetIsEnabled(bEnabled, *NiagaraSystem, false);
				}

				FVersionedNiagaraEmitter VersionedEmitterForApply = ExistingHandle->GetInstance();
				TArray<FNiagaraEmitterHandle>& MutableHandles = NiagaraSystem->GetEmitterHandles();
				const int32 TargetIndex = FMath::Clamp(EmitterIndex, 0, FMath::Max(0, MutableHandles.Num() - 1));
				if (MutableHandles.IsValidIndex(ExistingHandleIndex) && ExistingHandleIndex != TargetIndex)
				{
					const FNiagaraEmitterHandle MovedHandle = MutableHandles[ExistingHandleIndex];
					MutableHandles.RemoveAt(ExistingHandleIndex);
					MutableHandles.Insert(MovedHandle, TargetIndex);
				}
				++AppliedEmitterHandleCount;

				if (bApplyReferencedEmitters && !EmitterFolderRelative.IsEmpty())
				{
					if (VersionedEmitterForApply.Emitter)
					{
						const FString EmitterFolder = FPaths::Combine(FolderPath, TEXT("emitters"), EmitterFolderRelative);
						const int32 EmitterApplied = UeAgentNiagaraFolderOps::ApplyEmitterFolder(*VersionedEmitterForApply.Emitter, VersionedEmitterForApply.Version, EmitterFolder, Warnings, OutError, false);
						if (EmitterApplied == INDEX_NONE)
						{
							return false;
						}
						AppliedEmitterPropertyCount += EmitterApplied;
					}
				}
			}
		}
	}

	const int32 SystemStagesApplied = UeAgentNiagaraFolderOps::ApplySystemStagesFromFolder(*NiagaraSystem, FolderPath, Warnings, OutError);
	if (SystemStagesApplied == INDEX_NONE)
	{
		return false;
	}
	AppliedSystemStageCount = SystemStagesApplied;

	UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult =
		UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*NiagaraSystem, true, true);
	if (EmittersIndexObj.IsValid())
	{
		const int32 PostRefreshDataInterfacesApplied = UeAgentNiagaraFolderOps::ApplySystemDataInterfacesFromFolder(*NiagaraSystem, FolderPath, EmittersIndexObj, Warnings, OutError);
		if (PostRefreshDataInterfacesApplied == INDEX_NONE)
		{
			return false;
		}
		AppliedPostRefreshDataInterfaceCount = PostRefreshDataInterfacesApplied;
		if (AppliedPostRefreshDataInterfaceCount > 0)
		{
			NiagaraSystem->MarkPackageDirty();
			RefreshResult = UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*NiagaraSystem, true, true);
		}
	}

	bool bCompileAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);
	bool bForceCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	bool bCompileHasError = false;
	bool bCompileLogReadOk = true;
	FString CompileLogReadError;
	TSharedPtr<FJsonObject> CompileLogData;
	if (bCompileAfterApply)
	{
		bCompilationRequested = NiagaraSystem->RequestCompile(bForceCompile);
		if (bWaitForComplete)
		{
			NiagaraSystem->WaitForCompilationComplete(true, false);
			NiagaraSystem->PollForCompilationComplete(true);
			NiagaraSystem->CacheFromCompiledData();
			bCompilationComplete = !NiagaraSystem->HasOutstandingCompilationRequests(true);
		}
		FUeAgentRequestContext CompileLogCtx;
		CompileLogCtx.RequestId = TEXT("niagara_apply_folder_compile_log");
		CompileLogCtx.Command = TEXT("niagara_get_compile_log");
		CompileLogCtx.Params = MakeShared<FJsonObject>();
		CompileLogCtx.Params->SetStringField(TEXT("asset_path"), AssetPath);
		CompileLogCtx.Params->SetBoolField(TEXT("compile_before_read"), false);
		CompileLogCtx.Params->SetBoolField(TEXT("force_compile"), bForceCompile);
		CompileLogCtx.Params->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
		CompileLogCtx.Params->SetBoolField(TEXT("include_events"), true);
		CompileLogCtx.Params->SetStringField(TEXT("severity_filter"), TEXT("warning_or_error"));
		CompileLogCtx.Params->SetNumberField(TEXT("max_events_per_script"), 64);
		CompileLogData = MakeShared<FJsonObject>();
		if (!CmdNiagaraGetCompileLog(CompileLogCtx, CompileLogData, CompileLogReadError))
		{
			bCompileLogReadOk = false;
			CompileLogData->SetBoolField(TEXT("ok"), false);
			CompileLogData->SetStringField(TEXT("error"), CompileLogReadError);
		}
		if (CompileLogData.IsValid())
		{
			CompileLogData->TryGetBoolField(TEXT("has_error"), bCompileHasError);
		}
	}

	UeAgentNiagaraFolderOps::FPostApplyStackIssueReport StackIssueReport;
	if (!UeAgentNiagaraFolderOps::CollectPostApplyStackIssues(this, Ctx, NiagaraSystem->GetOutermost()->GetName(), bStrict, TEXT("system_apply_stack_issues"), StackIssueReport))
	{
		UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("stack_issue_collection_failed"), NiagaraSystem->GetOutermost()->GetName(), StackIssueReport.CollectionError);
	}
	bool bOutstandingAfterStackIssueRead = false;
	bool bPostStackCompilationRequested = false;
	bool bPostStackCompilationComplete = false;
	bool bFinalOutstandingCompilationRequests = bCompileAfterApply && bWaitForComplete
		? NiagaraSystem->HasOutstandingCompilationRequests(true)
		: false;
	if (bCompileAfterApply && bWaitForComplete && bFinalOutstandingCompilationRequests)
	{
		bOutstandingAfterStackIssueRead = true;
		TSharedPtr<FJsonObject> FinalCompileParams = MakeShared<FJsonObject>();
		FinalCompileParams->SetBoolField(TEXT("compile_after_set"), true);
		FinalCompileParams->SetBoolField(TEXT("force_compile"), bForceCompile);
		FinalCompileParams->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
		const UeAgentNiagaraOps::FNiagaraCompileAfterSetResult FinalCompileResult =
			UeAgentNiagaraOps::CompileSystemAfterSet(NiagaraSystem, FinalCompileParams, true);
		bPostStackCompilationRequested = FinalCompileResult.bCompilationRequested;
		bPostStackCompilationComplete = FinalCompileResult.bCompilationComplete;
		bCompilationRequested = bCompilationRequested || FinalCompileResult.bCompilationRequested;
		bCompilationComplete = FinalCompileResult.bCompilationComplete;
		bFinalOutstandingCompilationRequests = FinalCompileResult.bHasOutstandingCompilationRequests;

		CompileLogReadError.Reset();
		bCompileLogReadOk = true;
		CompileLogData = MakeShared<FJsonObject>();
		FUeAgentRequestContext CompileLogCtx;
		CompileLogCtx.RequestId = TEXT("niagara_apply_folder_post_stack_compile_log");
		CompileLogCtx.Command = TEXT("niagara_get_compile_log");
		CompileLogCtx.Params = MakeShared<FJsonObject>();
		CompileLogCtx.Params->SetStringField(TEXT("asset_path"), AssetPath);
		CompileLogCtx.Params->SetBoolField(TEXT("compile_before_read"), false);
		CompileLogCtx.Params->SetBoolField(TEXT("force_compile"), bForceCompile);
		CompileLogCtx.Params->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
		CompileLogCtx.Params->SetBoolField(TEXT("include_events"), true);
		CompileLogCtx.Params->SetStringField(TEXT("severity_filter"), TEXT("warning_or_error"));
		CompileLogCtx.Params->SetNumberField(TEXT("max_events_per_script"), 64);
		if (!CmdNiagaraGetCompileLog(CompileLogCtx, CompileLogData, CompileLogReadError))
		{
			bCompileLogReadOk = false;
			CompileLogData->SetBoolField(TEXT("ok"), false);
			CompileLogData->SetStringField(TEXT("error"), CompileLogReadError);
		}
		bCompileHasError = false;
		if (CompileLogData.IsValid())
		{
			CompileLogData->TryGetBoolField(TEXT("has_error"), bCompileHasError);
		}
	}

	Stats.Add(TEXT("system_properties_applied"), AppliedSystemPropertyCount);
	Stats.Add(TEXT("user_parameters_applied"), AppliedUserParameterCount);
	Stats.Add(TEXT("system_stages_applied"), AppliedSystemStageCount);
	Stats.Add(TEXT("emitter_handles_applied"), AppliedEmitterHandleCount);
	Stats.Add(TEXT("emitter_properties_applied"), AppliedEmitterPropertyCount);
	Stats.Add(TEXT("post_refresh_data_interfaces_applied"), AppliedPostRefreshDataInterfaceCount);
	Stats.Add(TEXT("overview_node_count_before"), RefreshResult.OverviewNodeCountBefore);
	Stats.Add(TEXT("overview_node_count_after"), RefreshResult.OverviewNodeCountAfter);
	Stats.Add(TEXT("stack_total_issue_count"), StackIssueReport.TotalIssueCount);
	Stats.Add(TEXT("stack_error_count"), StackIssueReport.TotalErrorCount);
	Stats.Add(TEXT("stack_warning_count"), StackIssueReport.TotalWarningCount);
	Stats.Add(TEXT("stack_info_count"), StackIssueReport.TotalInfoCount);
	TSharedPtr<FJsonObject> CoverageObj = UeAgentNiagaraFolderOps::BuildCoverageReport(AssetPath, TEXT("apply"), Warnings, Stats);
	UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageObj, nullptr);

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetNumberField(TEXT("system_properties_applied"), AppliedSystemPropertyCount);
	OutData->SetNumberField(TEXT("user_parameters_applied"), AppliedUserParameterCount);
	OutData->SetNumberField(TEXT("system_stages_applied"), AppliedSystemStageCount);
	OutData->SetNumberField(TEXT("emitter_handles_applied"), AppliedEmitterHandleCount);
	OutData->SetNumberField(TEXT("emitter_properties_applied"), AppliedEmitterPropertyCount);
	OutData->SetNumberField(TEXT("post_refresh_data_interfaces_applied"), AppliedPostRefreshDataInterfaceCount);
	OutData->SetBoolField(TEXT("synchronized_overview_graph"), RefreshResult.bSynchronizedOverviewGraph);
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	OutData->SetNumberField(TEXT("overview_node_count_before"), RefreshResult.OverviewNodeCountBefore);
	OutData->SetNumberField(TEXT("overview_node_count_after"), RefreshResult.OverviewNodeCountAfter);
	OutData->SetBoolField(TEXT("compile_after_apply"), bCompileAfterApply);
	OutData->SetBoolField(TEXT("force_compile"), bForceCompile);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("has_outstanding_compilation_requests"), bFinalOutstandingCompilationRequests);
	OutData->SetBoolField(TEXT("outstanding_after_stack_issue_read"), bOutstandingAfterStackIssueRead);
	OutData->SetBoolField(TEXT("post_stack_compilation_requested"), bPostStackCompilationRequested);
	OutData->SetBoolField(TEXT("post_stack_compilation_complete"), bPostStackCompilationComplete);
	OutData->SetBoolField(TEXT("compile_log_read_ok"), bCompileLogReadOk);
	OutData->SetStringField(TEXT("compile_log_read_error"), CompileLogReadError);
	OutData->SetBoolField(TEXT("compile_has_error"), bCompileHasError);
	if (CompileLogData.IsValid())
	{
		OutData->SetObjectField(TEXT("compile_log"), CompileLogData);
	}
	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	bool bSaved = false;
	OutData->SetBoolField(TEXT("saved"), bSaved);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	UeAgentNiagaraFolderOps::AppendPostApplyStackIssueFields(OutData, StackIssueReport);
	OutData->SetBoolField(TEXT("restore_preview_after_apply"), bRestorePreviewAfterApply);
	if (bRestorePreviewAfterApply)
	{
		TSharedPtr<FJsonObject> PreviewRestoreReport = UeAgentNiagaraOps::RestoreNiagaraPreviewForInteractiveEditing(NiagaraSystem, TSharedPtr<FNiagaraSystemViewModel>());
		OutData->SetObjectField(TEXT("preview_restore"), PreviewRestoreReport);
	}

	if (bCompileAfterApply && bWaitForComplete && bFinalOutstandingCompilationRequests)
	{
		OutError = TEXT("compile_incomplete_after_apply");
		return false;
	}
	if (bCompileAfterApply && !bCompileLogReadOk)
	{
		OutError = TEXT("compile_log_read_failed_after_apply");
		return false;
	}
	if (bCompileAfterApply && bCompileHasError)
	{
		OutError = TEXT("compile_failed_after_apply");
		return false;
	}
	if (bStrict && StackIssueReport.bCollectAfterApply && !StackIssueReport.bCollectionOk)
	{
		OutError = TEXT("strict_apply_stack_issue_collection_failed");
		return false;
	}
	if (StackIssueReport.bFailOnStackErrors && StackIssueReport.TotalErrorCount > 0)
	{
		OutError = TEXT("strict_apply_has_stack_errors");
		return false;
	}
	if (bStrict && Warnings.Num() > 0)
	{
		OutError = TEXT("strict_apply_has_warnings");
		return false;
	}
	if (bSaveAfterApply)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
		bSaved = true;
		OutData->SetBoolField(TEXT("saved"), bSaved);
	}
	return true;
}
