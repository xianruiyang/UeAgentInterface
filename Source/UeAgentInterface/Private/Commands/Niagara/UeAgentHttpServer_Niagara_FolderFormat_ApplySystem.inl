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

	bool bCompileAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterApply)
	{
		bCompilationRequested = NiagaraSystem->RequestCompile(false);
		bCompilationComplete = bWaitForComplete ? NiagaraSystem->PollForCompilationComplete(true) : false;
	}

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (bSaveAfterApply)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	UeAgentNiagaraFolderOps::FPostApplyStackIssueReport StackIssueReport;
	if (!UeAgentNiagaraFolderOps::CollectPostApplyStackIssues(this, Ctx, NiagaraSystem->GetOutermost()->GetName(), bStrict, TEXT("system_apply_stack_issues"), StackIssueReport))
	{
		UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("stack_issue_collection_failed"), NiagaraSystem->GetOutermost()->GetName(), StackIssueReport.CollectionError);
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
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	UeAgentNiagaraFolderOps::AppendPostApplyStackIssueFields(OutData, StackIssueReport);

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
	return true;
}
