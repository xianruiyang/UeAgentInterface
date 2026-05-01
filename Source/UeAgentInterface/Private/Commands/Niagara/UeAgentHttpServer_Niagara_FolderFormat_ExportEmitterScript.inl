bool FUeAgentHttpServer::CmdNiagaraExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		if (!UeAgentNiagaraFolderOps::ResolveFolderForAsset(NiagaraSystem->GetOutermost()->GetName(), FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = FPaths::ConvertRelativePathToFull(FolderPath);

	bool bCleanOutputDir = true;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bCleanOutputDir);
	if (bCleanOutputDir)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);

	TArray<FString> WrittenFiles;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TMap<FString, int32> Stats;
	TMap<FString, TSharedPtr<FJsonObject>> ScriptRefs;

	const FString AssetPath = NiagaraSystem->GetOutermost()->GetName();
	const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);

	TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
	AssetObj->SetNumberField(TEXT("format_version"), UeAgentNiagaraFolderOps::NiagaraFolderFormatVersion);
	AssetObj->SetStringField(TEXT("asset_kind"), TEXT("niagara_system"));
	AssetObj->SetStringField(TEXT("asset_path"), AssetPath);
	AssetObj->SetStringField(TEXT("asset_name"), AssetName);
	AssetObj->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	AssetObj->SetStringField(TEXT("class"), NiagaraSystem->GetClass()->GetPathName());
	AssetObj->SetStringField(TEXT("engine_target"), TEXT("UE_5.6"));
	AssetObj->SetStringField(TEXT("root_folder_kind"), TEXT("NiagaraSystem"));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetObj, &WrittenFiles))
	{
		OutError = TEXT("save_asset_json_failed");
		return false;
	}

	TSharedPtr<FJsonObject> SystemSettingsObj = MakeShared<FJsonObject>();
	SystemSettingsObj->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	UeAgentNiagaraFolderOps::AppendPropertyArray(SystemSettingsObj, TEXT("raw_properties"), UeAgentNiagaraFolderOps::BuildObjectPropertiesJson(NiagaraSystem));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("system.json")), SystemSettingsObj, &WrittenFiles))
	{
		OutError = TEXT("save_system_settings_failed");
		return false;
	}

	const TArray<FString> SettingsFiles = {
		TEXT("asset_options.json"),
		TEXT("effect_type.json"),
		TEXT("scalability.json"),
		TEXT("fixed_bounds.json"),
		TEXT("performance.json"),
		TEXT("baker.json")
	};
	for (const FString& SettingsFile : SettingsFiles)
	{
		TSharedPtr<FJsonObject> PlaceholderObj = MakeShared<FJsonObject>();
		PlaceholderObj->SetNumberField(TEXT("format_version"), UeAgentNiagaraFolderOps::NiagaraFolderFormatVersion);
		PlaceholderObj->SetStringField(TEXT("asset_path"), AssetPath);
		PlaceholderObj->SetStringField(TEXT("coverage_note"), TEXT("See settings/system.json raw_properties for the authoritative UObject field mirror; this file is a stable structured extension point."));
		if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), SettingsFile), PlaceholderObj, &WrittenFiles))
		{
			OutError = TEXT("save_settings_extension_failed");
			return false;
		}
	}

	TArray<FNiagaraVariable> UserParameters;
	NiagaraSystem->GetExposedParameters().GetUserParameters(UserParameters);
	UserParameters.Sort([](const FNiagaraVariable& A, const FNiagaraVariable& B)
	{
		return A.GetName().LexicalLess(B.GetName());
	});
	TArray<TSharedPtr<FJsonValue>> UserParamsJson;
	for (const FNiagaraVariable& Parameter : UserParameters)
	{
		UserParamsJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildUserParameterJson(NiagaraSystem->GetExposedParameters(), Parameter, true)));
	}
	TSharedPtr<FJsonObject> UserParamsObj = MakeShared<FJsonObject>();
	UserParamsObj->SetArrayField(TEXT("parameters"), UserParamsJson);
	UserParamsObj->SetNumberField(TEXT("parameter_count"), UserParamsJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("parameters"), TEXT("user_parameters.json")), UserParamsObj, &WrittenFiles))
	{
		OutError = TEXT("save_user_parameters_failed");
		return false;
	}
	Stats.Add(TEXT("user_parameter_count"), UserParamsJson.Num());

	const TArray<FString> ParameterFiles = {
		TEXT("system_parameters.json"),
		TEXT("parameter_definitions.json"),
		TEXT("parameter_collections.json"),
		TEXT("parameter_collection_overrides.json")
	};
	for (const FString& ParameterFile : ParameterFiles)
	{
		TSharedPtr<FJsonObject> ParameterObj = MakeShared<FJsonObject>();
		ParameterObj->SetArrayField(TEXT("items"), TArray<TSharedPtr<FJsonValue>>());
		ParameterObj->SetStringField(TEXT("coverage_note"), TEXT("Reserved structured extension point; concrete references are preserved through raw properties and module inputs."));
		if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("parameters"), ParameterFile), ParameterObj, &WrittenFiles))
		{
			OutError = TEXT("save_parameter_extension_failed");
			return false;
		}
	}

	UeAgentNiagaraFolderOps::AddUniqueScriptReference(ScriptRefs, NiagaraSystem->GetSystemSpawnScript(), TEXT("system_stage"), TEXT("SystemSpawn"));
	UeAgentNiagaraFolderOps::AddUniqueScriptReference(ScriptRefs, NiagaraSystem->GetSystemUpdateScript(), TEXT("system_stage"), TEXT("SystemUpdate"));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("system_stages"), TEXT("SystemSpawn.json")), UeAgentNiagaraFolderOps::BuildSystemStageJson(NiagaraSystem->GetSystemSpawnScript(), TEXT("SystemSpawn"), ScriptRefs), &WrittenFiles))
	{
		OutError = TEXT("save_system_spawn_failed");
		return false;
	}
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("system_stages"), TEXT("SystemUpdate.json")), UeAgentNiagaraFolderOps::BuildSystemStageJson(NiagaraSystem->GetSystemUpdateScript(), TEXT("SystemUpdate"), ScriptRefs), &WrittenFiles))
	{
		OutError = TEXT("save_system_update_failed");
		return false;
	}

	const TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem->GetEmitterHandles();
	TArray<TSharedPtr<FJsonValue>> EmitterIndexJson;
	for (int32 EmitterIndex = 0; EmitterIndex < Handles.Num(); ++EmitterIndex)
	{
		const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];
		const FString EmitterFolderName = UeAgentNiagaraFolderOps::MakeEmitterFolderName(Handle.GetName().ToString(), Handle.GetId(), EmitterIndex);
		TSharedPtr<FJsonObject> EmitterEntry;
		if (!UeAgentNiagaraFolderOps::ExportEmitterFolder(
			FPaths::Combine(FolderPath, TEXT("emitters"), EmitterFolderName),
			EmitterFolderName,
			EmitterIndex,
			Handle,
			WrittenFiles,
			Warnings,
			ScriptRefs,
			EmitterEntry))
		{
			OutError = TEXT("save_emitter_folder_failed");
			return false;
		}
		EmitterIndexJson.Add(MakeShared<FJsonValueObject>(EmitterEntry));
	}
	TSharedPtr<FJsonObject> EmittersIndexObj = MakeShared<FJsonObject>();
	EmittersIndexObj->SetArrayField(TEXT("emitters"), EmitterIndexJson);
	EmittersIndexObj->SetNumberField(TEXT("emitter_count"), EmitterIndexJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("emitters"), TEXT("index.json")), EmittersIndexObj, &WrittenFiles))
	{
		OutError = TEXT("save_emitters_index_failed");
		return false;
	}
	Stats.Add(TEXT("emitter_count"), EmitterIndexJson.Num());

	TArray<TSharedPtr<FJsonValue>> ScriptsJson;
	for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : ScriptRefs)
	{
		if (Pair.Value.IsValid())
		{
			ScriptsJson.Add(MakeShared<FJsonValueObject>(Pair.Value));
		}
	}
	TSharedPtr<FJsonObject> ScriptsIndexObj = MakeShared<FJsonObject>();
	ScriptsIndexObj->SetArrayField(TEXT("scripts"), ScriptsJson);
	ScriptsIndexObj->SetNumberField(TEXT("script_count"), ScriptsJson.Num());
	ScriptsIndexObj->SetStringField(TEXT("coverage_note"), TEXT("Script references are indexed here; standalone NiagaraScript folder commands export/apply script asset graphs when a script asset path is used as the root profile."));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("scripts"), TEXT("index.json")), ScriptsIndexObj, &WrittenFiles))
	{
		OutError = TEXT("save_scripts_index_failed");
		return false;
	}
	Stats.Add(TEXT("script_reference_count"), ScriptsJson.Num());

	TSharedPtr<FJsonObject> ChecksObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ChecksJson;
	TSharedPtr<FJsonObject> CompileCheckObj = MakeShared<FJsonObject>();
	CompileCheckObj->SetStringField(TEXT("kind"), TEXT("compile_success"));
	CompileCheckObj->SetStringField(TEXT("command"), TEXT("niagara_compile_system"));
	ChecksJson.Add(MakeShared<FJsonValueObject>(CompileCheckObj));
	ChecksObj->SetArrayField(TEXT("checks"), ChecksJson);
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("checks.json")), ChecksObj, &WrittenFiles))
	{
		OutError = TEXT("save_validation_checks_failed");
		return false;
	}

	TSharedPtr<FJsonObject> CompileSummaryObj = MakeShared<FJsonObject>();
	CompileSummaryObj->SetStringField(TEXT("asset_path"), AssetPath);
	CompileSummaryObj->SetStringField(TEXT("coverage_note"), TEXT("Run niagara_get_compile_log with compile_before_read=true for live compile diagnostics."));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("compile_summary.json")), CompileSummaryObj, &WrittenFiles))
	{
		OutError = TEXT("save_compile_summary_failed");
		return false;
	}

	TSharedPtr<FJsonObject> DependencyReportObj = MakeShared<FJsonObject>();
	DependencyReportObj->SetArrayField(TEXT("script_references"), ScriptsJson);
	DependencyReportObj->SetNumberField(TEXT("script_reference_count"), ScriptsJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("dependency_report.json")), DependencyReportObj, &WrittenFiles))
	{
		OutError = TEXT("save_dependency_report_failed");
		return false;
	}

	TSharedPtr<FJsonObject> RuntimePreviewObj = MakeShared<FJsonObject>();
	RuntimePreviewObj->SetStringField(TEXT("asset_path"), AssetPath);
	RuntimePreviewObj->SetStringField(TEXT("coverage_note"), TEXT("Runtime component overrides, Sequencer tracks and SimCache output are outside the UNiagaraSystem asset folder scope."));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("runtime_preview.json")), RuntimePreviewObj, &WrittenFiles))
	{
		OutError = TEXT("save_runtime_preview_failed");
		return false;
	}

	Stats.Add(TEXT("file_count"), WrittenFiles.Num() + 1);
	TSharedPtr<FJsonObject> CoverageObj = UeAgentNiagaraFolderOps::BuildCoverageReport(AssetPath, TEXT("export"), Warnings, Stats);
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageObj, &WrittenFiles))
	{
		OutError = TEXT("save_coverage_report_failed");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("file_count"), WrittenFiles.Num());
	OutData->SetNumberField(TEXT("emitter_count"), EmitterIndexJson.Num());
	OutData->SetNumberField(TEXT("user_parameter_count"), UserParamsJson.Num());
	OutData->SetNumberField(TEXT("script_reference_count"), ScriptsJson.Num());
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("emitter_asset_path"), AssetPathInput);
	if (AssetPathInput.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	}
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_emitter_asset_path");
		return false;
	}

	UNiagaraEmitter* NiagaraEmitter = Cast<UNiagaraEmitter>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraEmitter)
	{
		OutError = TEXT("emitter_asset_not_found");
		return false;
	}

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		if (!UeAgentNiagaraFolderOps::ResolveEmitterFolderForAsset(NiagaraEmitter->GetOutermost()->GetName(), FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = FPaths::ConvertRelativePathToFull(FolderPath);

	bool bCleanOutputDir = true;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bCleanOutputDir);
	if (bCleanOutputDir)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);

	TArray<FString> WrittenFiles;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TMap<FString, int32> Stats;
	TMap<FString, TSharedPtr<FJsonObject>> ScriptRefs;

	const FString AssetPath = NiagaraEmitter->GetOutermost()->GetName();
	const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
	const FGuid ExportVersion = NiagaraEmitter->GetExposedVersion().VersionGuid;

	TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
	AssetObj->SetNumberField(TEXT("format_version"), UeAgentNiagaraFolderOps::NiagaraFolderFormatVersion);
	AssetObj->SetStringField(TEXT("asset_kind"), TEXT("niagara_emitter"));
	AssetObj->SetStringField(TEXT("asset_path"), AssetPath);
	AssetObj->SetStringField(TEXT("asset_name"), AssetName);
	AssetObj->SetStringField(TEXT("object_path"), NiagaraEmitter->GetPathName());
	AssetObj->SetStringField(TEXT("class"), NiagaraEmitter->GetClass()->GetPathName());
	AssetObj->SetStringField(TEXT("engine_target"), TEXT("UE_5.6"));
	AssetObj->SetStringField(TEXT("root_folder_kind"), TEXT("NiagaraEmitter"));
	AssetObj->SetStringField(TEXT("emitter_version"), ExportVersion.ToString(EGuidFormats::DigitsWithHyphensLower));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetObj, &WrittenFiles))
	{
		OutError = TEXT("save_asset_json_failed");
		return false;
	}

	FNiagaraEmitterHandle StandaloneHandle(*NiagaraEmitter, ExportVersion);
	TSharedPtr<FJsonObject> EmitterIndexObj;
	if (!UeAgentNiagaraFolderOps::ExportEmitterFolder(
		FolderPath,
		AssetName,
		0,
		StandaloneHandle,
		WrittenFiles,
		Warnings,
		ScriptRefs,
		EmitterIndexObj))
	{
		OutError = TEXT("save_emitter_folder_failed");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> ScriptsJson;
	for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : ScriptRefs)
	{
		if (Pair.Value.IsValid())
		{
			ScriptsJson.Add(MakeShared<FJsonValueObject>(Pair.Value));
		}
	}
	TSharedPtr<FJsonObject> ScriptsIndexObj = MakeShared<FJsonObject>();
	ScriptsIndexObj->SetArrayField(TEXT("scripts"), ScriptsJson);
	ScriptsIndexObj->SetNumberField(TEXT("script_count"), ScriptsJson.Num());
	ScriptsIndexObj->SetStringField(TEXT("coverage_note"), TEXT("Script references are indexed here; standalone NiagaraScript folder commands export/apply script asset graphs when a script asset path is used as the root profile."));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("scripts"), TEXT("index.json")), ScriptsIndexObj, &WrittenFiles))
	{
		OutError = TEXT("save_scripts_index_failed");
		return false;
	}
	Stats.Add(TEXT("script_reference_count"), ScriptsJson.Num());

	TSharedPtr<FJsonObject> ChecksObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ChecksJson;
	TSharedPtr<FJsonObject> CompileCheckObj = MakeShared<FJsonObject>();
	CompileCheckObj->SetStringField(TEXT("kind"), TEXT("compile_containing_system"));
	CompileCheckObj->SetStringField(TEXT("command"), TEXT("niagara_compile_system"));
	CompileCheckObj->SetStringField(TEXT("coverage_note"), TEXT("Standalone emitters compile in the context of containing systems."));
	ChecksJson.Add(MakeShared<FJsonValueObject>(CompileCheckObj));
	ChecksObj->SetArrayField(TEXT("checks"), ChecksJson);
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("checks.json")), ChecksObj, &WrittenFiles))
	{
		OutError = TEXT("save_validation_checks_failed");
		return false;
	}

	TSharedPtr<FJsonObject> DependencyReportObj = MakeShared<FJsonObject>();
	DependencyReportObj->SetArrayField(TEXT("script_references"), ScriptsJson);
	DependencyReportObj->SetNumberField(TEXT("script_reference_count"), ScriptsJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("dependency_report.json")), DependencyReportObj, &WrittenFiles))
	{
		OutError = TEXT("save_dependency_report_failed");
		return false;
	}

	Stats.Add(TEXT("file_count"), WrittenFiles.Num() + 1);
	TSharedPtr<FJsonObject> CoverageObj = UeAgentNiagaraFolderOps::BuildEmitterCoverageReport(AssetPath, TEXT("export"), Warnings, Stats);
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageObj, &WrittenFiles))
	{
		OutError = TEXT("save_coverage_report_failed");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("emitter_asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("file_count"), WrittenFiles.Num());
	OutData->SetNumberField(TEXT("script_reference_count"), ScriptsJson.Num());
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderPath;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath);
	FolderPath.TrimStartAndEndInline();

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("emitter_asset_path"), AssetPathInput);
	if (AssetPathInput.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	}
	AssetPathInput.TrimStartAndEndInline();

	if (FolderPath.IsEmpty())
	{
		if (AssetPathInput.IsEmpty())
		{
			OutError = TEXT("missing_folder_path_or_emitter_asset_path");
			return false;
		}
		if (!UeAgentNiagaraFolderOps::ResolveEmitterFolderForAsset(AssetPathInput, FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = FPaths::ConvertRelativePathToFull(FolderPath);

	TSharedPtr<FJsonObject> AssetObj;
	const FString AssetJsonPath = FPaths::Combine(FolderPath, TEXT("asset.json"));
	if (!UeAgentNiagaraFolderOps::LoadJsonFileOptional(AssetJsonPath, AssetObj, OutError))
	{
		return false;
	}

	FString AssetPathFromFile;
	if (AssetObj.IsValid())
	{
		AssetObj->TryGetStringField(TEXT("asset_path"), AssetPathFromFile);
	}
	if (AssetPathInput.IsEmpty())
	{
		AssetPathInput = AssetPathFromFile;
	}
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_emitter_asset_path_and_asset_json");
		return false;
	}

	const FString AssetPath = UeAgentNiagaraOps::NormalizeAssetPath(AssetPathInput);
	UNiagaraEmitter* NiagaraEmitter = Cast<UNiagaraEmitter>(UeAgentNiagaraOps::LoadAssetObject(AssetPath));
	bool bCreateIfMissing = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	bool bAddDefaultModulesAndRenderers = true;
	JsonTryGetBool(Ctx.Params, TEXT("add_default_modules_and_renderers"), bAddDefaultModulesAndRenderers);
	bool bCreated = false;
	if (!NiagaraEmitter && bCreateIfMissing)
	{
		NiagaraEmitter = UeAgentNiagaraFolderOps::CreateNiagaraEmitterAsset(AssetPath, bAddDefaultModulesAndRenderers, OutError);
		bCreated = NiagaraEmitter != nullptr;
	}
	if (!NiagaraEmitter)
	{
		OutError = TEXT("emitter_asset_not_found");
		return false;
	}

	bool bStrict = false;
	JsonTryGetBool(Ctx.Params, TEXT("strict"), bStrict);

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TMap<FString, int32> Stats;
	NiagaraEmitter->Modify();

	TSharedPtr<FJsonObject> EmitterObj;
	FGuid EmitterVersionGuid;
	if (UeAgentNiagaraFolderOps::LoadJsonFileOptional(FPaths::Combine(FolderPath, TEXT("emitter.json")), EmitterObj, OutError) && EmitterObj.IsValid())
	{
		FString VersionText;
		if (!EmitterObj->TryGetStringField(TEXT("version_guid"), VersionText))
		{
			EmitterObj->TryGetStringField(TEXT("emitter_version"), VersionText);
		}
		FGuid::Parse(VersionText, EmitterVersionGuid);
	}
	else if (!OutError.IsEmpty())
	{
		return false;
	}

	const bool bCanUseExportedVersion = EmitterVersionGuid.IsValid() && NiagaraEmitter->GetEmitterData(EmitterVersionGuid) != nullptr;
	const FGuid ApplyVersion = bCanUseExportedVersion ? EmitterVersionGuid : NiagaraEmitter->GetExposedVersion().VersionGuid;
	const int32 AppliedEmitterPropertyCount = UeAgentNiagaraFolderOps::ApplyEmitterFolder(*NiagaraEmitter, ApplyVersion, FolderPath, Warnings, OutError);
	if (AppliedEmitterPropertyCount == INDEX_NONE)
	{
		return false;
	}

	NiagaraEmitter->MarkPackageDirty();

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (bSaveAfterApply)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraEmitter, OutError))
		{
			return false;
		}
	}

	UeAgentNiagaraFolderOps::FPostApplyStackIssueReport StackIssueReport;
	if (!UeAgentNiagaraFolderOps::CollectPostApplyStackIssues(this, Ctx, NiagaraEmitter->GetOutermost()->GetName(), bStrict, TEXT("emitter_apply_stack_issues"), StackIssueReport))
	{
		UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("stack_issue_collection_failed"), NiagaraEmitter->GetOutermost()->GetName(), StackIssueReport.CollectionError);
	}

	Stats.Add(TEXT("emitter_properties_applied"), AppliedEmitterPropertyCount);
	Stats.Add(TEXT("stack_total_issue_count"), StackIssueReport.TotalIssueCount);
	Stats.Add(TEXT("stack_error_count"), StackIssueReport.TotalErrorCount);
	Stats.Add(TEXT("stack_warning_count"), StackIssueReport.TotalWarningCount);
	Stats.Add(TEXT("stack_info_count"), StackIssueReport.TotalInfoCount);
	TSharedPtr<FJsonObject> CoverageObj = UeAgentNiagaraFolderOps::BuildEmitterCoverageReport(AssetPath, TEXT("apply"), Warnings, Stats);
	UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageObj, nullptr);

	OutData->SetStringField(TEXT("asset_path"), NiagaraEmitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_asset_path"), NiagaraEmitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("asset_json_loaded"), AssetObj.IsValid());
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetNumberField(TEXT("emitter_properties_applied"), AppliedEmitterPropertyCount);
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

bool FUeAgentHttpServer::CmdNiagaraScriptExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("script_asset_path"), AssetPathInput);
	if (AssetPathInput.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	}
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_script_asset_path");
		return false;
	}

	UNiagaraScript* NiagaraScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraScript)
	{
		OutError = TEXT("script_asset_not_found");
		return false;
	}

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		if (!UeAgentNiagaraFolderOps::ResolveScriptFolderForAsset(NiagaraScript->GetOutermost()->GetName(), FolderPath, OutError))
		{
			return false;
		}
	}
	FolderPath = FPaths::ConvertRelativePathToFull(FolderPath);

	bool bCleanOutputDir = true;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bCleanOutputDir);
	if (bCleanOutputDir)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);

	TArray<FString> WrittenFiles;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TMap<FString, int32> Stats;

	const FString AssetPath = NiagaraScript->GetOutermost()->GetName();
	const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);

	TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
	AssetObj->SetNumberField(TEXT("format_version"), UeAgentNiagaraFolderOps::NiagaraFolderFormatVersion);
	AssetObj->SetStringField(TEXT("asset_kind"), TEXT("niagara_script"));
	AssetObj->SetStringField(TEXT("asset_path"), AssetPath);
	AssetObj->SetStringField(TEXT("asset_name"), AssetName);
	AssetObj->SetStringField(TEXT("object_path"), NiagaraScript->GetPathName());
	AssetObj->SetStringField(TEXT("class"), NiagaraScript->GetClass()->GetPathName());
	AssetObj->SetStringField(TEXT("engine_target"), TEXT("UE_5.6"));
	AssetObj->SetStringField(TEXT("root_folder_kind"), TEXT("NiagaraScript"));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetObj, &WrittenFiles))
	{
		OutError = TEXT("save_asset_json_failed");
		return false;
	}

	TSharedPtr<FJsonObject> ScriptObj = MakeShared<FJsonObject>();
	ScriptObj->SetNumberField(TEXT("format_version"), UeAgentNiagaraFolderOps::NiagaraFolderFormatVersion);
	ScriptObj->SetStringField(TEXT("asset_kind"), TEXT("niagara_script"));
	ScriptObj->SetStringField(TEXT("script_name"), NiagaraScript->GetName());
	ScriptObj->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(NiagaraScript->GetUsage()));
	ScriptObj->SetStringField(TEXT("script_usage_id"), NiagaraScript->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower));
	ScriptObj->SetStringField(TEXT("compile_status"), UeAgentNiagaraOps::ScriptCompileStatusToString(NiagaraScript->GetLastCompileStatus()));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("script.json")), ScriptObj, &WrittenFiles))
	{
		OutError = TEXT("save_script_json_failed");
		return false;
	}

	TSharedPtr<FJsonObject> PropertiesObj = MakeShared<FJsonObject>();
	UeAgentNiagaraFolderOps::AppendPropertyArray(PropertiesObj, TEXT("raw_properties"), UeAgentNiagaraFolderOps::BuildObjectPropertiesJson(NiagaraScript));
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("properties.json")), PropertiesObj, &WrittenFiles))
	{
		OutError = TEXT("save_properties_json_failed");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> NodesJson;
	TArray<TSharedPtr<FJsonValue>> LinksJson;
	TArray<TSharedPtr<FJsonValue>> CustomHlslNodesJson;
	if (UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(NiagaraScript->GetLatestSource()))
	{
		if (UNiagaraGraph* Graph = ScriptSource->NodeGraph)
		{
			TSet<UEdGraphNode*> AllNodes;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				AllNodes.Add(Node);
				TSharedPtr<FJsonObject> NodeObj = UeAgentNiagaraOps::BuildStageNodeJson(*Node, NodesJson.Num(), true, true);
				if (UNiagaraNodeCustomHlsl* CustomHlslNode = Cast<UNiagaraNodeCustomHlsl>(Node))
				{
					FString CustomHlslText;
					if (UeAgentNiagaraOps::GetObjectPropertyText(CustomHlslNode, TEXT("CustomHlsl"), CustomHlslText))
					{
						NodeObj->SetStringField(TEXT("custom_hlsl"), CustomHlslText);
					}
					CustomHlslNodesJson.Add(MakeShared<FJsonValueObject>(NodeObj));
				}
				NodesJson.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
			UeAgentNiagaraFolderOps::ExportGraphLinksForNodes(AllNodes, LinksJson);
		}
		else
		{
			UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("script_missing_graph"), AssetPath, TEXT("NiagaraScript source has no graph."));
		}
	}
	else
	{
		UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("script_missing_source"), AssetPath, TEXT("NiagaraScript has no UNiagaraScriptSource."));
	}

	TSharedPtr<FJsonObject> NodesObj = MakeShared<FJsonObject>();
	NodesObj->SetArrayField(TEXT("nodes"), NodesJson);
	NodesObj->SetNumberField(TEXT("node_count"), NodesJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graph"), TEXT("nodes.json")), NodesObj, &WrittenFiles))
	{
		OutError = TEXT("save_graph_nodes_failed");
		return false;
	}

	TSharedPtr<FJsonObject> LinksObj = MakeShared<FJsonObject>();
	LinksObj->SetArrayField(TEXT("links"), LinksJson);
	LinksObj->SetNumberField(TEXT("link_count"), LinksJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graph"), TEXT("links.json")), LinksObj, &WrittenFiles))
	{
		OutError = TEXT("save_graph_links_failed");
		return false;
	}

	TSharedPtr<FJsonObject> CustomHlslObj = MakeShared<FJsonObject>();
	CustomHlslObj->SetArrayField(TEXT("custom_hlsl_nodes"), CustomHlslNodesJson);
	CustomHlslObj->SetNumberField(TEXT("custom_hlsl_node_count"), CustomHlslNodesJson.Num());
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graph"), TEXT("custom_hlsl_nodes.json")), CustomHlslObj, &WrittenFiles))
	{
		OutError = TEXT("save_custom_hlsl_failed");
		return false;
	}

	Stats.Add(TEXT("node_count"), NodesJson.Num());
	Stats.Add(TEXT("link_count"), LinksJson.Num());
	Stats.Add(TEXT("custom_hlsl_node_count"), CustomHlslNodesJson.Num());
	Stats.Add(TEXT("file_count"), WrittenFiles.Num() + 1);
	TSharedPtr<FJsonObject> CoverageObj = UeAgentNiagaraFolderOps::BuildScriptCoverageReport(AssetPath, TEXT("export"), Warnings, Stats);
	if (!UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageObj, &WrittenFiles))
	{
		OutError = TEXT("save_coverage_report_failed");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("script_asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("file_count"), WrittenFiles.Num());
	OutData->SetNumberField(TEXT("node_count"), NodesJson.Num());
	OutData->SetNumberField(TEXT("link_count"), LinksJson.Num());
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraScriptApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderPath;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath);
	FolderPath.TrimStartAndEndInline();

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("script_asset_path"), AssetPathInput);
	if (AssetPathInput.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	}
	AssetPathInput.TrimStartAndEndInline();

	if (FolderPath.IsEmpty())
	{
		if (AssetPathInput.IsEmpty())
		{
			OutError = TEXT("missing_folder_path_or_script_asset_path");
			return false;
		}
		if (!UeAgentNiagaraFolderOps::ResolveScriptFolderForAsset(AssetPathInput, FolderPath, OutError))
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

	TSharedPtr<FJsonObject> ScriptObj;
	if (!UeAgentNiagaraFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("script.json")), ScriptObj, OutError))
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
		OutError = TEXT("missing_script_asset_path");
		return false;
	}

	ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Module;
	FString UsageText;
	if (ScriptObj->TryGetStringField(TEXT("script_usage"), UsageText) && !UsageText.IsEmpty())
	{
		UeAgentNiagaraOps::TryParseScriptUsage(UsageText, Usage);
	}

	const FString AssetPath = UeAgentNiagaraOps::NormalizeAssetPath(AssetPathInput);
	UNiagaraScript* NiagaraScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(AssetPath));
	bool bCreateIfMissing = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	bool bCreated = false;
	if (!NiagaraScript && bCreateIfMissing)
	{
		NiagaraScript = UeAgentNiagaraFolderOps::CreateNiagaraScriptAsset(AssetPath, Usage, OutError);
		bCreated = NiagaraScript != nullptr;
	}
	if (!NiagaraScript)
	{
		OutError = TEXT("script_asset_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TMap<FString, int32> Stats;
	NiagaraScript->Modify();
	NiagaraScript->SetUsage(Usage);
	FString UsageIdText;
	if (ScriptObj->TryGetStringField(TEXT("script_usage_id"), UsageIdText) && !UsageIdText.IsEmpty())
	{
		FGuid UsageId;
		if (FGuid::Parse(UsageIdText, UsageId))
		{
			NiagaraScript->SetUsageId(UsageId);
		}
	}

	TSharedPtr<FJsonObject> PropertiesObj;
	if (!UeAgentNiagaraFolderOps::LoadJsonFileOptional(FPaths::Combine(FolderPath, TEXT("properties.json")), PropertiesObj, OutError))
	{
		return false;
	}
	int32 AppliedPropertyCount = 0;
	if (PropertiesObj.IsValid())
	{
		AppliedPropertyCount += UeAgentNiagaraFolderOps::ApplyObjectPropertyArray(NiagaraScript, PropertiesObj, TEXT("raw_properties"), Warnings, NiagaraScript->GetPathName());
	}

	int32 AppliedNodePropertyCount = 0;
	if (UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(NiagaraScript->GetLatestSource()))
	{
		if (UNiagaraGraph* Graph = ScriptSource->NodeGraph)
		{
			TSharedPtr<FJsonObject> NodesObj;
			if (UeAgentNiagaraFolderOps::LoadJsonFileOptional(FPaths::Combine(FolderPath, TEXT("graph"), TEXT("nodes.json")), NodesObj, OutError) && NodesObj.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
				if (UeAgentNiagaraFolderOps::TryGetArrayField(NodesObj, TEXT("nodes"), Nodes))
				{
					for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
					{
						const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
						if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
						{
							continue;
						}
						const TSharedPtr<FJsonObject>& NodeObj = *NodeObjPtr;

						FGuid NodeGuid;
						FString NodeGuidText;
						NodeObj->TryGetStringField(TEXT("node_guid"), NodeGuidText);
						FGuid::Parse(NodeGuidText, NodeGuid);
						UEdGraphNode* TargetNode = NodeGuid.IsValid() ? UeAgentNiagaraOps::FindGraphNodeByGuid(*Graph, NodeGuid) : nullptr;
						if (!TargetNode)
						{
							FString NodeClassPath;
							NodeObj->TryGetStringField(TEXT("node_class"), NodeClassPath);
							UClass* NodeClass = UeAgentNiagaraOps::ResolveClassByPath(NodeClassPath);
							if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()) || NodeClass->HasAnyClassFlags(CLASS_Abstract))
							{
								continue;
							}

							TargetNode = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
							if (!TargetNode)
							{
								continue;
							}
							if (NodeGuid.IsValid())
							{
								TargetNode->NodeGuid = NodeGuid;
							}
							else
							{
								TargetNode->CreateNewGuid();
							}
							TargetNode->AllocateDefaultPins();
							Graph->AddNode(TargetNode, true, false);
						}

						if (TargetNode)
						{
							AppliedNodePropertyCount += UeAgentNiagaraFolderOps::ApplyObjectPropertyArray(TargetNode, NodeObj, TEXT("properties"), Warnings, TargetNode->GetPathName());
							if (UNiagaraNodeCustomHlsl* CustomHlslNode = Cast<UNiagaraNodeCustomHlsl>(TargetNode))
							{
								FString CustomHlsl;
								if (NodeObj->TryGetStringField(TEXT("custom_hlsl"), CustomHlsl))
								{
									FString AppliedCustomHlsl;
									FString CppType;
									FString ImportError;
									if (!UeAgentNiagaraOps::SetObjectPropertyText(CustomHlslNode, TEXT("CustomHlsl"), CustomHlsl, &AppliedCustomHlsl, &CppType, &ImportError))
									{
										UeAgentNiagaraFolderOps::AddWarning(
											Warnings,
											TEXT("custom_hlsl_apply_failed"),
											TargetNode->GetPathName(),
											FString::Printf(TEXT("Failed to apply CustomHlsl. requested_length=%d cpp_type='%s' error='%s'."), CustomHlsl.Len(), *CppType, *ImportError));
									}
									else if (!AppliedCustomHlsl.Equals(CustomHlsl, ESearchCase::CaseSensitive))
									{
										UeAgentNiagaraFolderOps::AddWarning(
											Warnings,
											TEXT("custom_hlsl_value_changed_after_import"),
											TargetNode->GetPathName(),
											FString::Printf(TEXT("CustomHlsl read back a different value. requested_length=%d applied_length=%d cpp_type='%s'."), CustomHlsl.Len(), AppliedCustomHlsl.Len(), *CppType));
									}
								}
							}
						}
					}
				}
			}
			else if (!OutError.IsEmpty())
			{
				return false;
			}
			UeAgentNiagaraOps::NormalizeGraphNodeGuids(Graph);
			Graph->NotifyGraphChanged();
		}
	}
	else
	{
		UeAgentNiagaraFolderOps::AddWarning(Warnings, TEXT("script_apply_missing_source"), AssetPath, TEXT("NiagaraScript has no graph source after initialization."));
	}

	NiagaraScript->MarkPackageDirty();

	bool bCompileAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);
	bool bCompilationRequested = false;
	if (bCompileAfterApply)
	{
		NiagaraScript->RequestCompile(NiagaraScript->GetExposedVersion().VersionGuid, false);
		bCompilationRequested = true;
	}

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (bSaveAfterApply)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraScript, OutError))
		{
			return false;
		}
	}

	Stats.Add(TEXT("script_properties_applied"), AppliedPropertyCount);
	Stats.Add(TEXT("node_properties_applied"), AppliedNodePropertyCount);
	TSharedPtr<FJsonObject> CoverageObj = UeAgentNiagaraFolderOps::BuildScriptCoverageReport(AssetPath, TEXT("apply"), Warnings, Stats);
	UeAgentNiagaraFolderOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageObj, nullptr);

	bool bStrict = false;
	JsonTryGetBool(Ctx.Params, TEXT("strict"), bStrict);
	if (bStrict && Warnings.Num() > 0)
	{
		OutError = TEXT("strict_apply_has_warnings");
		OutData->SetArrayField(TEXT("warnings"), Warnings);
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraScript->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("script_asset_path"), NiagaraScript->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetNumberField(TEXT("script_properties_applied"), AppliedPropertyCount);
	OutData->SetNumberField(TEXT("node_properties_applied"), AppliedNodePropertyCount);
	OutData->SetBoolField(TEXT("compile_after_apply"), bCompileAfterApply);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	return true;
}
