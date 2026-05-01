namespace UeAgentNiagaraFolderOps
{
	static constexpr int32 NiagaraFolderFormatVersion = 1;

	static int32 GetJsonIntField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
	{
		double Value = 0.0;
		return Obj.IsValid() && Obj->TryGetNumberField(FieldName, Value) ? FMath::RoundToInt(Value) : 0;
	}

	struct FPostApplyStackIssueReport
	{
		bool bCollectAfterApply = true;
		bool bFailOnStackErrors = false;
		bool bCollectionOk = false;
		FString CollectionError;
		FString SeverityFilter = TEXT("warning_or_error");
		bool bPreferExistingViewModel = true;
		bool bOpenEditorIfNeeded = true;
		bool bCompileBeforeRead = true;
		TSharedPtr<FJsonObject> Report;
		int32 TotalIssueCount = 0;
		int32 TotalErrorCount = 0;
		int32 TotalWarningCount = 0;
		int32 TotalInfoCount = 0;
		int32 ReturnedIssueCount = 0;
	};

	static bool CollectPostApplyStackIssues(
		const FUeAgentHttpServer* Server,
		const FUeAgentRequestContext& SourceCtx,
		const FString& AssetPath,
		const bool bDefaultFailOnStackErrors,
		const FString& RequestIdSuffix,
		FPostApplyStackIssueReport& OutReport)
	{
		if (!Server)
		{
			OutReport.CollectionError = TEXT("missing_server");
			return false;
		}

		OutReport.bFailOnStackErrors = bDefaultFailOnStackErrors;
		if (SourceCtx.Params.IsValid())
		{
			SourceCtx.Params->TryGetBoolField(TEXT("collect_stack_issues_after_apply"), OutReport.bCollectAfterApply);
			SourceCtx.Params->TryGetBoolField(TEXT("fail_on_stack_errors"), OutReport.bFailOnStackErrors);
			SourceCtx.Params->TryGetStringField(TEXT("stack_issue_severity_filter"), OutReport.SeverityFilter);
			SourceCtx.Params->TryGetBoolField(TEXT("prefer_existing_stack_view_model"), OutReport.bPreferExistingViewModel);
			SourceCtx.Params->TryGetBoolField(TEXT("open_editor_for_stack_issues"), OutReport.bOpenEditorIfNeeded);
			SourceCtx.Params->TryGetBoolField(TEXT("compile_before_stack_issue_read"), OutReport.bCompileBeforeRead);
		}
		OutReport.SeverityFilter.TrimStartAndEndInline();
		if (OutReport.SeverityFilter.IsEmpty())
		{
			OutReport.SeverityFilter = TEXT("warning_or_error");
		}

		if (!OutReport.bCollectAfterApply)
		{
			OutReport.bCollectionOk = false;
			return true;
		}

		FUeAgentRequestContext StackCtx;
		StackCtx.RequestId = SourceCtx.RequestId.IsEmpty()
			? FString::Printf(TEXT("post_apply_stack_issues_%s"), *RequestIdSuffix)
			: FString::Printf(TEXT("%s_%s"), *SourceCtx.RequestId, *RequestIdSuffix);
		StackCtx.Command = TEXT("niagara_get_stack_issues");
		StackCtx.Params = MakeShared<FJsonObject>();
		StackCtx.Params->SetStringField(TEXT("asset_path"), AssetPath);
		StackCtx.Params->SetStringField(TEXT("severity_filter"), OutReport.SeverityFilter);
		StackCtx.Params->SetBoolField(TEXT("prefer_existing_view_model"), OutReport.bPreferExistingViewModel);
		StackCtx.Params->SetBoolField(TEXT("open_editor_if_needed"), OutReport.bOpenEditorIfNeeded);
		StackCtx.Params->SetBoolField(TEXT("compile_before_read"), OutReport.bCompileBeforeRead);

		OutReport.Report = MakeShared<FJsonObject>();
		OutReport.bCollectionOk = Server->CmdNiagaraGetStackIssues(StackCtx, OutReport.Report, OutReport.CollectionError);
		if (!OutReport.bCollectionOk)
		{
			return false;
		}

		OutReport.TotalIssueCount = GetJsonIntField(OutReport.Report, TEXT("total_issue_count"));
		OutReport.TotalErrorCount = GetJsonIntField(OutReport.Report, TEXT("total_error_count"));
		OutReport.TotalWarningCount = GetJsonIntField(OutReport.Report, TEXT("total_warning_count"));
		OutReport.TotalInfoCount = GetJsonIntField(OutReport.Report, TEXT("total_info_count"));
		OutReport.ReturnedIssueCount = GetJsonIntField(OutReport.Report, TEXT("returned_issue_count"));
		return true;
	}

	static void AppendPostApplyStackIssueFields(TSharedPtr<FJsonObject> OutData, const FPostApplyStackIssueReport& Report)
	{
		if (!OutData.IsValid())
		{
			return;
		}

		OutData->SetBoolField(TEXT("collect_stack_issues_after_apply"), Report.bCollectAfterApply);
		OutData->SetBoolField(TEXT("fail_on_stack_errors"), Report.bFailOnStackErrors);
		OutData->SetBoolField(TEXT("stack_issue_collection_ok"), Report.bCollectionOk);
		OutData->SetStringField(TEXT("stack_issue_collection_error"), Report.CollectionError);
		OutData->SetStringField(TEXT("stack_issue_severity_filter"), Report.SeverityFilter);
		OutData->SetBoolField(TEXT("prefer_existing_stack_view_model"), Report.bPreferExistingViewModel);
		OutData->SetBoolField(TEXT("open_editor_for_stack_issues"), Report.bOpenEditorIfNeeded);
		OutData->SetBoolField(TEXT("compile_before_stack_issue_read"), Report.bCompileBeforeRead);
		OutData->SetNumberField(TEXT("stack_total_issue_count"), Report.TotalIssueCount);
		OutData->SetNumberField(TEXT("stack_error_count"), Report.TotalErrorCount);
		OutData->SetNumberField(TEXT("stack_warning_count"), Report.TotalWarningCount);
		OutData->SetNumberField(TEXT("stack_info_count"), Report.TotalInfoCount);
		OutData->SetNumberField(TEXT("stack_returned_issue_count"), Report.ReturnedIssueCount);
		OutData->SetBoolField(TEXT("has_stack_errors"), Report.TotalErrorCount > 0);
		OutData->SetBoolField(TEXT("has_stack_warnings"), Report.TotalWarningCount > 0);

		if (Report.Report.IsValid())
		{
			OutData->SetObjectField(TEXT("stack_issue_report"), Report.Report);

			const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
			if (Report.Report->TryGetArrayField(TEXT("issues"), Issues) && Issues)
			{
				OutData->SetArrayField(TEXT("stack_issues"), *Issues);
			}

			const TArray<TSharedPtr<FJsonValue>>* Scopes = nullptr;
			if (Report.Report->TryGetArrayField(TEXT("scopes"), Scopes) && Scopes)
			{
				OutData->SetArrayField(TEXT("stack_scopes"), *Scopes);
			}

			FString ViewModelSource;
			if (Report.Report->TryGetStringField(TEXT("view_model_source"), ViewModelSource))
			{
				OutData->SetStringField(TEXT("stack_issue_view_model_source"), ViewModelSource);
			}

			FString AssetKind;
			if (Report.Report->TryGetStringField(TEXT("asset_kind"), AssetKind))
			{
				OutData->SetStringField(TEXT("stack_issue_asset_kind"), AssetKind);
			}
		}
	}

	static FString GetSystemFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("NiagaraSystem")));
	}

	static FString GetEmitterFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("NiagaraEmitter")));
	}

	static FString GetScriptFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("NiagaraScript")));
	}

	static FString NormalizeAssetRelativeFolder(const FString& InAssetPath)
	{
		FString AssetPath = UeAgentNiagaraOps::NormalizeAssetPath(InAssetPath);
		AssetPath.TrimStartAndEndInline();
		while (AssetPath.StartsWith(TEXT("/")))
		{
			AssetPath.RightChopInline(1, EAllowShrinking::No);
		}
		return AssetPath;
	}

	static bool ResolveFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentNiagaraOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		OutFolderPath = FPaths::Combine(GetSystemFolderRootAbsolute(), NormalizeAssetRelativeFolder(AssetPath));
		return true;
	}

	static bool ResolveEmitterFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentNiagaraOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		OutFolderPath = FPaths::Combine(GetEmitterFolderRootAbsolute(), NormalizeAssetRelativeFolder(AssetPath));
		return true;
	}

	static bool ResolveScriptFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentNiagaraOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		OutFolderPath = FPaths::Combine(GetScriptFolderRootAbsolute(), NormalizeAssetRelativeFolder(AssetPath));
		return true;
	}

	static FString MakeSafeFileName(const FString& InName)
	{
		const FString SafeName = FPaths::MakeValidFileName(InName, TEXT('_'));
		return SafeName.IsEmpty() ? TEXT("Item") : SafeName;
	}

	static FString MakeEmitterFolderName(const FString& DisplayName, const FGuid& HandleId, const int32 Index)
	{
		FString Prefix = FString::Printf(TEXT("%02d_%s"), Index, *MakeSafeFileName(DisplayName));
		if (HandleId.IsValid())
		{
			Prefix += TEXT("_") + HandleId.ToString(EGuidFormats::Digits).Left(8);
		}
		return Prefix;
	}

	static FString MakeStageFileName(const int32 Index, const FString& UsageText, const FGuid& UsageId)
	{
		const FString IdText = UsageId.IsValid() ? UsageId.ToString(EGuidFormats::Digits).Left(8) : TEXT("default");
		return FString::Printf(TEXT("%02d_%s_%s.json"), Index, *MakeSafeFileName(UsageText), *IdText);
	}

	static FString MakeJsonString(const TSharedPtr<FJsonObject>& Obj)
	{
		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return JsonText;
	}

	static bool SaveJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Obj, TArray<FString>* OutWrittenFiles = nullptr)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		const bool bSaved = FFileHelper::SaveStringToFile(
			MakeJsonString(Obj),
			*FilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (bSaved && OutWrittenFiles)
		{
			OutWrittenFiles->Add(FilePath);
		}
		return bSaved;
	}

	static bool LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, FString& OutError)
	{
		return UeAgentJsonDiagnostics::LoadJsonObjectFile(FilePath, OutObj, nullptr, OutError, false);
	}

	static bool LoadJsonFileOptional(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, FString& OutError)
	{
		return UeAgentJsonDiagnostics::LoadJsonObjectFile(FilePath, OutObj, nullptr, OutError, true);
	}

	static void AddWarning(TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& Code, const FString& Path, const FString& Message)
	{
		UeAgentJsonDiagnostics::AddIssue(Warnings, TEXT("warning"), Code, Path, Message);
	}

	static void AddCoverageArea(
		TArray<TSharedPtr<FJsonValue>>& Areas,
		const FString& Area,
		const FString& ExportSupport,
		const FString& ApplySupport,
		const FString& Notes)
	{
		TSharedPtr<FJsonObject> AreaObj = MakeShared<FJsonObject>();
		AreaObj->SetStringField(TEXT("area"), Area);
		AreaObj->SetStringField(TEXT("export_support"), ExportSupport);
		AreaObj->SetStringField(TEXT("apply_support"), ApplySupport);
		AreaObj->SetStringField(TEXT("notes"), Notes);
		Areas.Add(MakeShared<FJsonValueObject>(AreaObj));
	}

	static TSharedPtr<FJsonObject> BuildCoverageReport(
		const FString& AssetPath,
		const FString& Mode,
		const TArray<TSharedPtr<FJsonValue>>& Warnings,
		const TMap<FString, int32>& Stats)
	{
		TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
		Coverage->SetNumberField(TEXT("format_version"), NiagaraFolderFormatVersion);
		Coverage->SetStringField(TEXT("asset_kind"), TEXT("niagara_system"));
		Coverage->SetStringField(TEXT("asset_path"), AssetPath);
		Coverage->SetStringField(TEXT("mode"), Mode);
		Coverage->SetStringField(TEXT("target_engine"), TEXT("UE_5.6"));
		Coverage->SetBoolField(TEXT("uses_system_emitter_handle_model"), true);
		Coverage->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.niagara.system.folder"));
		Coverage->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
		Coverage->SetBoolField(TEXT("is_complete_target_schema"), true);
		Coverage->SetBoolField(TEXT("is_lossless_roundtrip"), Warnings.Num() == 0);
		Coverage->SetStringField(TEXT("completion_note"), TEXT("NiagaraSystem folder export/apply covers system properties, user parameters, emitter handles, standalone emitters, script assets, event handlers, simulation stages, module stacks, module input defaults, module input dynamic inputs, renderers, event generators and raw UObject/UStruct extension fields."));

		TArray<TSharedPtr<FJsonValue>> ImplementedProfiles;
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_system")));
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_emitter")));
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_script")));
		Coverage->SetArrayField(TEXT("implemented_profiles"), ImplementedProfiles);

		TArray<TSharedPtr<FJsonValue>> PendingProfiles;
		Coverage->SetArrayField(TEXT("pending_profiles"), PendingProfiles);

		TArray<TSharedPtr<FJsonValue>> BlockingGaps;
		Coverage->SetArrayField(TEXT("blocking_gaps"), BlockingGaps);

		TSharedPtr<FJsonObject> StatsObj = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : Stats)
		{
			StatsObj->SetNumberField(Pair.Key, Pair.Value);
		}
		Coverage->SetObjectField(TEXT("stats"), StatsObj);

		TArray<TSharedPtr<FJsonValue>> Areas;
		AddCoverageArea(Areas, TEXT("UNiagaraSystem asset metadata"), TEXT("structured"), TEXT("structured"), TEXT("asset.json and settings/system.json"));
		AddCoverageArea(Areas, TEXT("UNiagaraSystem UObject properties"), TEXT("raw_properties"), TEXT("generic_property_import"), TEXT("New UE fields are retained through property_name/value_text when ImportText supports the field."));
		AddCoverageArea(Areas, TEXT("System Spawn / System Update"), TEXT("structured_raw"), TEXT("structured_raw"), TEXT("Script references, raw script metadata and standalone NiagaraScript folder profiles are supported."));
		AddCoverageArea(Areas, TEXT("User parameters"), TEXT("structured"), TEXT("structured"), TEXT("User parameter add/set is supported for Niagara value types accepted by existing atomic commands."));
		AddCoverageArea(Areas, TEXT("Emitter handles"), TEXT("structured"), TEXT("structured"), TEXT("System -> FNiagaraEmitterHandle -> FVersionedNiagaraEmitter is preserved; handle GUIDs are exported and stable logical matching is name/folder based on apply."));
		AddCoverageArea(Areas, TEXT("UNiagaraEmitter version data"), TEXT("raw_properties"), TEXT("generic_struct_import"), TEXT("FVersionedNiagaraEmitterData properties are exported with property_name/value_text."));
		AddCoverageArea(Areas, TEXT("Stages and modules"), TEXT("structured"), TEXT("structured"), TEXT("Stage/module stack order, module script references, module enabled state, module input defaults and module input dynamic inputs are exported and applied."));
		AddCoverageArea(Areas, TEXT("Renderers"), TEXT("structured_raw"), TEXT("structured_raw"), TEXT("Renderer class/index and UObject properties are exported; missing renderers can be added when class is resolvable."));
		AddCoverageArea(Areas, TEXT("Event handlers"), TEXT("structured_raw"), TEXT("structured_raw"), TEXT("Event handler scripts, output stages and raw FNiagaraEventScriptProperties fields are exported and applied."));
		AddCoverageArea(Areas, TEXT("Event generators"), TEXT("structured_raw"), TEXT("structured_raw"), TEXT("Spawn/update script event generator arrays are exported explicitly and retained by raw script property import."));
		AddCoverageArea(Areas, TEXT("Data interfaces"), TEXT("structured_raw"), TEXT("structured_raw"), TEXT("Data interface references are exported through graph nodes, module inputs, dynamic inputs and raw properties; UObject import preserves supported reflected fields and curve_json."));
		AddCoverageArea(Areas, TEXT("Custom Niagara Script / Scratch Pad / Custom HLSL"), TEXT("structured_raw"), TEXT("structured_raw"), TEXT("Standalone NiagaraScript assets are supported; CustomHlsl nodes are exported with raw node properties and HLSL text where present."));
		AddCoverageArea(Areas, TEXT("External assets"), TEXT("reference"), TEXT("reference"), TEXT("External assets remain references and are not copied into the Niagara folder."));
		Coverage->SetArrayField(TEXT("coverage_areas"), Areas);
		Coverage->SetArrayField(TEXT("warnings"), Warnings);
		Coverage->SetNumberField(TEXT("warning_count"), Warnings.Num());
		return Coverage;
	}

	static TSharedPtr<FJsonObject> BuildEmitterCoverageReport(
		const FString& AssetPath,
		const FString& Mode,
		const TArray<TSharedPtr<FJsonValue>>& Warnings,
		const TMap<FString, int32>& Stats)
	{
		TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
		Coverage->SetNumberField(TEXT("format_version"), NiagaraFolderFormatVersion);
		Coverage->SetStringField(TEXT("asset_kind"), TEXT("niagara_emitter"));
		Coverage->SetStringField(TEXT("asset_path"), AssetPath);
		Coverage->SetStringField(TEXT("mode"), Mode);
		Coverage->SetStringField(TEXT("target_engine"), TEXT("UE_5.6"));
		Coverage->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.niagara.emitter.folder"));
		Coverage->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
		Coverage->SetBoolField(TEXT("is_complete_target_schema"), true);
		Coverage->SetBoolField(TEXT("is_lossless_roundtrip"), Warnings.Num() == 0);
		Coverage->SetStringField(TEXT("completion_note"), TEXT("Standalone NiagaraEmitter folder export/apply covers emitter metadata, version data, parameters, renderers, event handlers, event generators, simulation stages, module stacks, module input defaults, module input dynamic inputs, script references and raw reflected fields."));

		TArray<TSharedPtr<FJsonValue>> ImplementedProfiles;
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_system")));
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_emitter")));
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_script")));
		Coverage->SetArrayField(TEXT("implemented_profiles"), ImplementedProfiles);

		TArray<TSharedPtr<FJsonValue>> PendingProfiles;
		Coverage->SetArrayField(TEXT("pending_profiles"), PendingProfiles);

		TArray<TSharedPtr<FJsonValue>> BlockingGaps;
		Coverage->SetArrayField(TEXT("blocking_gaps"), BlockingGaps);

		TSharedPtr<FJsonObject> StatsObj = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : Stats)
		{
			StatsObj->SetNumberField(Pair.Key, Pair.Value);
		}
		Coverage->SetObjectField(TEXT("stats"), StatsObj);

		TArray<TSharedPtr<FJsonValue>> Areas;
		AddCoverageArea(Areas, TEXT("UNiagaraEmitter asset metadata"), TEXT("structured"), TEXT("structured"), TEXT("asset.json and emitter.json"));
		AddCoverageArea(Areas, TEXT("UNiagaraEmitter version data"), TEXT("raw_properties"), TEXT("generic_struct_import"), TEXT("FVersionedNiagaraEmitterData properties are exported with property_name/value_text."));
		AddCoverageArea(Areas, TEXT("Emitter parameters"), TEXT("structured"), TEXT("structured"), TEXT("Graph parameters and scalar default values are exported and applied."));
		AddCoverageArea(Areas, TEXT("Stages and modules"), TEXT("structured"), TEXT("structured"), TEXT("Stage/module stack order, module script references, module enabled state, module input defaults and module input dynamic inputs are exported and applied."));
		AddCoverageArea(Areas, TEXT("Renderers"), TEXT("structured_raw"), TEXT("structured_raw"), TEXT("Renderer class/index and UObject properties are exported; missing renderers can be added when class is resolvable."));
		AddCoverageArea(Areas, TEXT("Event handlers"), TEXT("structured_raw"), TEXT("structured_raw"), TEXT("Event handler scripts, output stages and raw FNiagaraEventScriptProperties fields are exported and applied."));
		AddCoverageArea(Areas, TEXT("Event generators"), TEXT("structured_raw"), TEXT("structured_raw"), TEXT("Spawn/update script event generator arrays are exported explicitly and retained by raw script property import."));
		AddCoverageArea(Areas, TEXT("Data interfaces"), TEXT("structured_raw"), TEXT("structured_raw"), TEXT("Data interface references are exported through graph nodes, module inputs, dynamic inputs and raw properties; UObject import preserves supported reflected fields and curve_json."));
		AddCoverageArea(Areas, TEXT("Custom Niagara Script / Scratch Pad / Custom HLSL"), TEXT("structured_raw"), TEXT("structured_raw"), TEXT("Standalone NiagaraScript assets are supported; CustomHlsl nodes are exported with raw node properties and HLSL text where present."));
		Coverage->SetArrayField(TEXT("coverage_areas"), Areas);
		Coverage->SetArrayField(TEXT("warnings"), Warnings);
		Coverage->SetNumberField(TEXT("warning_count"), Warnings.Num());
		return Coverage;
	}

	static TSharedPtr<FJsonObject> BuildScriptCoverageReport(
		const FString& AssetPath,
		const FString& Mode,
		const TArray<TSharedPtr<FJsonValue>>& Warnings,
		const TMap<FString, int32>& Stats)
	{
		TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
		Coverage->SetNumberField(TEXT("format_version"), NiagaraFolderFormatVersion);
		Coverage->SetStringField(TEXT("asset_kind"), TEXT("niagara_script"));
		Coverage->SetStringField(TEXT("asset_path"), AssetPath);
		Coverage->SetStringField(TEXT("mode"), Mode);
		Coverage->SetStringField(TEXT("target_engine"), TEXT("UE_5.6"));
		Coverage->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.niagara.script.folder"));
		Coverage->SetStringField(TEXT("implementation_status"), TEXT("complete_folder_profile"));
		Coverage->SetBoolField(TEXT("is_complete_target_schema"), true);
		Coverage->SetBoolField(TEXT("is_lossless_roundtrip"), Warnings.Num() == 0);
		Coverage->SetStringField(TEXT("completion_note"), TEXT("Standalone NiagaraScript folder export/apply covers script metadata, raw reflected fields, graph nodes, graph links and custom HLSL node text."));

		TArray<TSharedPtr<FJsonValue>> ImplementedProfiles;
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_system")));
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_emitter")));
		ImplementedProfiles.Add(MakeShared<FJsonValueString>(TEXT("niagara_script")));
		Coverage->SetArrayField(TEXT("implemented_profiles"), ImplementedProfiles);
		Coverage->SetArrayField(TEXT("pending_profiles"), TArray<TSharedPtr<FJsonValue>>());
		Coverage->SetArrayField(TEXT("blocking_gaps"), TArray<TSharedPtr<FJsonValue>>());

		TSharedPtr<FJsonObject> StatsObj = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : Stats)
		{
			StatsObj->SetNumberField(Pair.Key, Pair.Value);
		}
		Coverage->SetObjectField(TEXT("stats"), StatsObj);

		TArray<TSharedPtr<FJsonValue>> Areas;
		AddCoverageArea(Areas, TEXT("UNiagaraScript asset metadata"), TEXT("structured"), TEXT("structured"), TEXT("asset.json and script.json"));
		AddCoverageArea(Areas, TEXT("UNiagaraScript UObject properties"), TEXT("raw_properties"), TEXT("generic_property_import"), TEXT("Reflected fields are exported with property_name/value_text and re-imported when UE ImportText supports them."));
		AddCoverageArea(Areas, TEXT("Script graph"), TEXT("structured_raw"), TEXT("structured_raw"), TEXT("Graph nodes, links, node positions and raw node properties are exported; apply initializes a NiagaraScript graph and imports supported reflected state."));
		AddCoverageArea(Areas, TEXT("Custom HLSL"), TEXT("structured"), TEXT("structured"), TEXT("UNiagaraNodeCustomHlsl nodes expose their HLSL text in custom_hlsl_nodes and raw node properties."));
		Coverage->SetArrayField(TEXT("coverage_areas"), Areas);
		Coverage->SetArrayField(TEXT("warnings"), Warnings);
		Coverage->SetNumberField(TEXT("warning_count"), Warnings.Num());
		return Coverage;
	}

	static bool ShouldApplyProperty(const FProperty* Property)
	{
		return Property &&
			Property->HasAnyPropertyFlags(CPF_Edit) &&
			!Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst);
	}

	static TSharedPtr<FJsonObject> BuildPropertyJson(FProperty* Property, void* ValuePtr, UObject* OwnerForExport)
	{
		TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
		if (!Property || !ValuePtr)
		{
			PropertyObj->SetBoolField(TEXT("valid"), false);
			return PropertyObj;
		}

		FString ValueText;
		Property->ExportTextItem_Direct(ValueText, ValuePtr, nullptr, OwnerForExport, PPF_None);
		PropertyObj->SetBoolField(TEXT("valid"), true);
		PropertyObj->SetStringField(TEXT("property_name"), Property->GetName());
		PropertyObj->SetStringField(TEXT("authored_name"), Property->GetAuthoredName());
		PropertyObj->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		PropertyObj->SetStringField(TEXT("value_text"), ValueText);
		TSharedPtr<FJsonObject> CurveJson;
		if (UeAgentCurveJson::TryBuildPropertyCurveJson(Property, ValuePtr, CurveJson) && CurveJson.IsValid())
		{
			PropertyObj->SetObjectField(TEXT("value_json"), CurveJson);
			PropertyObj->SetObjectField(TEXT("curve_json"), CurveJson);
			PropertyObj->SetStringField(TEXT("value_json_schema"), UeAgentCurveJson::SchemaName);
		}
		const bool bCanApplyGeneric = ShouldApplyProperty(Property) && !Property->GetName().Equals(TEXT("NodeGuid"), ESearchCase::CaseSensitive);
		PropertyObj->SetBoolField(TEXT("can_apply_generic"), bCanApplyGeneric);
		PropertyObj->SetStringField(TEXT("property_flags"), FString::Printf(TEXT("0x%016llx"), static_cast<unsigned long long>(Property->PropertyFlags)));
		return PropertyObj;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildObjectPropertiesJson(UObject* Object)
	{
		TArray<TSharedPtr<FJsonValue>> PropertiesJson;
		if (!Object)
		{
			return PropertiesJson;
		}

		for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
			PropertiesJson.Add(MakeShared<FJsonValueObject>(BuildPropertyJson(Property, ValuePtr, Object)));
		}
		return PropertiesJson;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildStructPropertiesJson(UStruct* StructType, void* StructData, UObject* OwnerForExport)
	{
		TArray<TSharedPtr<FJsonValue>> PropertiesJson;
		if (!StructType || !StructData)
		{
			return PropertiesJson;
		}

		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(StructData);
			PropertiesJson.Add(MakeShared<FJsonValueObject>(BuildPropertyJson(Property, ValuePtr, OwnerForExport)));
		}
		return PropertiesJson;
	}

	static bool TryGetArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const TArray<TSharedPtr<FJsonValue>>*& OutArray)
	{
		OutArray = nullptr;
		return Obj.IsValid() && Obj->TryGetArrayField(Key, OutArray) && OutArray != nullptr;
	}

	static FString GetStringFieldAny(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, const FString& DefaultValue = FString())
	{
		if (!Obj.IsValid())
		{
			return DefaultValue;
		}

		for (const FString& Key : Keys)
		{
			FString Value;
			if (Obj->TryGetStringField(Key, Value))
			{
				return Value;
			}
		}
		return DefaultValue;
	}

	static bool GetObjectField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedPtr<FJsonObject>* FoundObject = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetObjectField(Key, FoundObject) || !FoundObject || !FoundObject->IsValid())
		{
			return false;
		}
		OutObject = *FoundObject;
		return true;
	}

	static void AppendPropertyArray(TSharedPtr<FJsonObject> TargetObj, const FString& FieldName, const TArray<TSharedPtr<FJsonValue>>& Properties)
	{
		TargetObj->SetArrayField(FieldName, Properties);
		TargetObj->SetNumberField(FieldName + TEXT("_count"), Properties.Num());
	}

	static TSharedPtr<FJsonObject> BuildScriptReferenceJson(UNiagaraScript* Script, const FString& OwnerKind, const FString& OwnerPath)
	{
		TSharedPtr<FJsonObject> ScriptObj = MakeShared<FJsonObject>();
		ScriptObj->SetStringField(TEXT("owner_kind"), OwnerKind);
		ScriptObj->SetStringField(TEXT("owner_path"), OwnerPath);
		if (!Script)
		{
			ScriptObj->SetBoolField(TEXT("valid"), false);
			return ScriptObj;
		}

		ScriptObj->SetBoolField(TEXT("valid"), true);
		ScriptObj->SetStringField(TEXT("script_name"), Script->GetName());
		ScriptObj->SetStringField(TEXT("script_object_path"), Script->GetPathName());
		ScriptObj->SetStringField(TEXT("script_asset_path"), Script->GetOutermost()->GetName());
		ScriptObj->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Script->GetUsage()));
		ScriptObj->SetStringField(TEXT("script_usage_id"), Script->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower));
		ScriptObj->SetStringField(TEXT("compile_status"), UeAgentNiagaraOps::ScriptCompileStatusToString(Script->GetLastCompileStatus()));
		AppendPropertyArray(ScriptObj, TEXT("raw_properties"), BuildObjectPropertiesJson(Script));
		return ScriptObj;
	}

	static void AddUniqueScriptReference(TMap<FString, TSharedPtr<FJsonObject>>& ScriptRefs, UNiagaraScript* Script, const FString& OwnerKind, const FString& OwnerPath)
	{
		if (!Script)
		{
			return;
		}

		const FString Key = Script->GetPathName();
		if (!ScriptRefs.Contains(Key))
		{
			ScriptRefs.Add(Key, BuildScriptReferenceJson(Script, OwnerKind, OwnerPath));
		}
	}

	static void AddUniqueScriptReferenceByPath(TMap<FString, TSharedPtr<FJsonObject>>& ScriptRefs, const FString& ScriptPath, const FString& OwnerKind, const FString& OwnerPath)
	{
		if (ScriptPath.IsEmpty() || ScriptRefs.Contains(ScriptPath))
		{
			return;
		}

		TSharedPtr<FJsonObject> ScriptObj = MakeShared<FJsonObject>();
		ScriptObj->SetBoolField(TEXT("valid"), false);
		ScriptObj->SetStringField(TEXT("script_object_path"), ScriptPath);
		ScriptObj->SetStringField(TEXT("owner_kind"), OwnerKind);
		ScriptObj->SetStringField(TEXT("owner_path"), OwnerPath);
		ScriptObj->SetStringField(TEXT("reference_only_reason"), TEXT("asset_not_loaded_or_non_script_reference"));
		ScriptRefs.Add(ScriptPath, ScriptObj);
	}

	static void ExportGraphLinksForNodes(const TSet<UEdGraphNode*>& Nodes, TArray<TSharedPtr<FJsonValue>>& OutLinks)
	{
		for (UEdGraphNode* Node : Nodes)
		{
			if (!Node)
			{
				continue;
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output)
				{
					continue;
				}
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin || !LinkedPin->GetOwningNode() || !Nodes.Contains(LinkedPin->GetOwningNode()))
					{
						continue;
					}
					TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
					LinkObj->SetStringField(TEXT("from_node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
					LinkObj->SetStringField(TEXT("from_pin"), Pin->PinName.ToString());
					LinkObj->SetStringField(TEXT("to_node_guid"), LinkedPin->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
					LinkObj->SetStringField(TEXT("to_pin"), LinkedPin->PinName.ToString());
					OutLinks.Add(MakeShared<FJsonValueObject>(LinkObj));
				}
			}
		}
	}

	static TSharedPtr<FJsonObject> BuildStageFolderJson(
		UNiagaraNodeOutput& OutputNode,
		const int32 StageIndex,
		FVersionedNiagaraEmitterData* EmitterData,
		TMap<FString, TSharedPtr<FJsonObject>>& ScriptRefs)
	{
		TSharedPtr<FJsonObject> StageObj = MakeShared<FJsonObject>();
		const ENiagaraScriptUsage Usage = OutputNode.GetUsage();
		const FGuid UsageId = OutputNode.GetUsageId();
		const FString UsageText = UeAgentNiagaraOps::ScriptUsageToString(Usage);
		const FString StageKey = UeAgentNiagaraOps::MakeStageKey(Usage, UsageId);

		StageObj->SetNumberField(TEXT("stage_index"), StageIndex);
		StageObj->SetStringField(TEXT("stage_key"), StageKey);
		StageObj->SetStringField(TEXT("script_usage"), UsageText);
		StageObj->SetStringField(TEXT("script_usage_id"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		StageObj->SetStringField(TEXT("output_node_guid"), OutputNode.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

		FString StageType = TEXT("standard");
		if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			StageType = TEXT("simulation_stage");
			if (EmitterData)
			{
				if (UNiagaraSimulationStageBase* SimulationStage = EmitterData->GetSimulationStageById(UsageId))
				{
					StageObj->SetStringField(TEXT("simulation_stage_class"), SimulationStage->GetClass()->GetPathName());
					StageObj->SetStringField(TEXT("simulation_stage_name"), SimulationStage->GetFName().ToString());
					AppendPropertyArray(StageObj, TEXT("simulation_stage_raw_properties"), BuildObjectPropertiesJson(SimulationStage));
				}
			}
		}
		else if (Usage == ENiagaraScriptUsage::ParticleEventScript)
		{
			StageType = TEXT("event_stage");
		}
		StageObj->SetStringField(TEXT("stage_type"), StageType);

		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		UeAgentNiagaraOps::GetStageModuleNodes(OutputNode, ModuleNodes);
		TArray<TSharedPtr<FJsonValue>> ModulesJson;
		TSet<UEdGraphNode*> StageNodes;
		TArray<UEdGraphNode*> UpstreamNodes;
		UeAgentNiagaraOps::GatherUpstreamNodesRecursive(&OutputNode, StageNodes, UpstreamNodes);
		StageNodes.Add(&OutputNode);
		ModulesJson.Reserve(ModuleNodes.Num());
		for (int32 ModuleIndex = 0; ModuleIndex < ModuleNodes.Num(); ++ModuleIndex)
		{
			UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIndex];
			if (!ModuleNode)
			{
				continue;
			}

			StageNodes.Add(ModuleNode);
			ModulesJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildModuleNodeJson(*ModuleNode, ModuleIndex, true)));
			AddUniqueScriptReference(ScriptRefs, ModuleNode->FunctionScript, TEXT("stage_module"), StageKey);
			AddUniqueScriptReferenceByPath(ScriptRefs, UeAgentNiagaraOps::GetModuleScriptPath(*ModuleNode), TEXT("stage_module"), StageKey);
		}
		StageObj->SetArrayField(TEXT("modules"), ModulesJson);
		StageObj->SetNumberField(TEXT("module_count"), ModulesJson.Num());

		TArray<TSharedPtr<FJsonValue>> NodesJson;
		TArray<TSharedPtr<FJsonValue>> CustomHlslNodesJson;
		for (UEdGraphNode* StageNode : StageNodes)
		{
			if (!StageNode)
			{
				continue;
			}
			TSharedPtr<FJsonObject> NodeJson = UeAgentNiagaraOps::BuildStageNodeJson(*StageNode, NodesJson.Num(), true, true);
			if (UNiagaraNodeCustomHlsl* CustomHlslNode = Cast<UNiagaraNodeCustomHlsl>(StageNode))
			{
				FString CustomHlslText;
				if (UeAgentNiagaraOps::GetObjectPropertyText(CustomHlslNode, TEXT("CustomHlsl"), CustomHlslText))
				{
					NodeJson->SetStringField(TEXT("custom_hlsl"), CustomHlslText);
				}
			}
			NodesJson.Add(MakeShared<FJsonValueObject>(NodeJson));
			if (StageNode->GetClass() && StageNode->GetClass()->GetName().Contains(TEXT("CustomHlsl")))
			{
				CustomHlslNodesJson.Add(MakeShared<FJsonValueObject>(NodeJson));
			}
		}
		StageObj->SetArrayField(TEXT("graph_nodes"), NodesJson);
		StageObj->SetNumberField(TEXT("graph_node_count"), NodesJson.Num());
		StageObj->SetArrayField(TEXT("custom_hlsl_nodes"), CustomHlslNodesJson);
		StageObj->SetNumberField(TEXT("custom_hlsl_node_count"), CustomHlslNodesJson.Num());

		TArray<TSharedPtr<FJsonValue>> LinksJson;
		ExportGraphLinksForNodes(StageNodes, LinksJson);
		StageObj->SetArrayField(TEXT("graph_links"), LinksJson);
		StageObj->SetNumberField(TEXT("graph_link_count"), LinksJson.Num());
		return StageObj;
	}

	static bool ExportEmitterFolder(
		const FString& EmitterFolder,
		const FString& EmitterFolderRelative,
		const int32 EmitterIndex,
		const FNiagaraEmitterHandle& Handle,
		TArray<FString>& WrittenFiles,
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		TMap<FString, TSharedPtr<FJsonObject>>& ScriptRefs,
		TSharedPtr<FJsonObject>& OutEmitterIndexObj)
	{
		FVersionedNiagaraEmitter VersionedEmitter = Handle.GetInstance();
		UNiagaraEmitter* Emitter = VersionedEmitter.Emitter;
		const FVersionedNiagaraEmitterData* ConstEmitterData = VersionedEmitter.GetEmitterData();
		if (!ConstEmitterData && Emitter)
		{
			ConstEmitterData = Emitter->GetLatestEmitterData();
		}
		FVersionedNiagaraEmitterData* EmitterData = const_cast<FVersionedNiagaraEmitterData*>(ConstEmitterData);

		OutEmitterIndexObj = MakeShared<FJsonObject>();
		OutEmitterIndexObj->SetNumberField(TEXT("index"), EmitterIndex);
		OutEmitterIndexObj->SetStringField(TEXT("logical_id"), EmitterFolderRelative);
		OutEmitterIndexObj->SetStringField(TEXT("folder"), EmitterFolderRelative);
		OutEmitterIndexObj->SetStringField(TEXT("handle_id"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
		OutEmitterIndexObj->SetStringField(TEXT("emitter_name"), Handle.GetName().ToString());
		OutEmitterIndexObj->SetStringField(TEXT("unique_instance_name"), Handle.GetUniqueInstanceName());
		OutEmitterIndexObj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		OutEmitterIndexObj->SetStringField(TEXT("emitter_version"), VersionedEmitter.Version.ToString(EGuidFormats::DigitsWithHyphensLower));

		TSharedPtr<FJsonObject> SourceObj = MakeShared<FJsonObject>();
		if (Emitter)
		{
			const bool bSystemInstance = Emitter->GetTypedOuter<UNiagaraSystem>() != nullptr;
			SourceObj->SetStringField(TEXT("ownership"), bSystemInstance ? TEXT("system_instance") : TEXT("standalone_asset"));
			SourceObj->SetBoolField(TEXT("is_embedded_instance"), bSystemInstance);
			if (bSystemInstance)
			{
				SourceObj->SetStringField(TEXT("source_system_asset_path"), Emitter->GetOutermost()->GetName());
			}
			else
			{
				SourceObj->SetStringField(TEXT("emitter_asset_path"), Emitter->GetOutermost()->GetName());
			}
			SourceObj->SetStringField(TEXT("emitter_object_path"), Emitter->GetPathName());
			SourceObj->SetStringField(TEXT("emitter_class"), Emitter->GetClass()->GetPathName());
		}
		OutEmitterIndexObj->SetObjectField(TEXT("source"), SourceObj);

		TSharedPtr<FJsonObject> EmitterObj = MakeShared<FJsonObject>();
		EmitterObj->SetNumberField(TEXT("format_version"), NiagaraFolderFormatVersion);
		EmitterObj->SetStringField(TEXT("asset_kind"), TEXT("niagara_emitter_in_system"));
		EmitterObj->SetStringField(TEXT("logical_id"), EmitterFolderRelative);
		EmitterObj->SetStringField(TEXT("handle_id"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
		EmitterObj->SetStringField(TEXT("emitter_name"), Handle.GetName().ToString());
		EmitterObj->SetStringField(TEXT("unique_instance_name"), Handle.GetUniqueInstanceName());
		EmitterObj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		EmitterObj->SetObjectField(TEXT("source"), SourceObj);
		EmitterObj->SetStringField(TEXT("emitter_version"), VersionedEmitter.Version.ToString(EGuidFormats::DigitsWithHyphensLower));
		if (EmitterData)
		{
			EmitterObj->SetStringField(TEXT("version_guid"), EmitterData->Version.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			EmitterObj->SetNumberField(TEXT("major_version"), EmitterData->Version.MajorVersion);
			EmitterObj->SetNumberField(TEXT("minor_version"), EmitterData->Version.MinorVersion);
		}
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("emitter.json")), EmitterObj, &WrittenFiles))
		{
			return false;
		}

		TSharedPtr<FJsonObject> PropertiesObj = MakeShared<FJsonObject>();
		PropertiesObj->SetStringField(TEXT("logical_id"), EmitterFolderRelative);
		if (EmitterData)
		{
			AppendPropertyArray(PropertiesObj, TEXT("raw_properties"), BuildStructPropertiesJson(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, Emitter));
		}
		else
		{
			AddWarning(Warnings, TEXT("emitter_missing_version_data"), EmitterFolderRelative, TEXT("Emitter has no readable FVersionedNiagaraEmitterData."));
			PropertiesObj->SetArrayField(TEXT("raw_properties"), TArray<TSharedPtr<FJsonValue>>());
			PropertiesObj->SetNumberField(TEXT("raw_properties_count"), 0);
		}
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("properties.json")), PropertiesObj, &WrittenFiles))
		{
			return false;
		}

		TSharedPtr<FJsonObject> ParametersObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ParametersJson;
		if (EmitterData)
		{
			UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
			UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
			if (Graph)
			{
				const UNiagaraGraph::FScriptVariableMap& ScriptVariableMap = Graph->GetAllMetaData();
				for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : ScriptVariableMap)
				{
					ParametersJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildGraphParameterJson(Pair.Key, Pair.Value, true)));
				}
			}
		}
		ParametersObj->SetArrayField(TEXT("parameters"), ParametersJson);
		ParametersObj->SetNumberField(TEXT("parameter_count"), ParametersJson.Num());
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("parameters.json")), ParametersObj, &WrittenFiles))
		{
			return false;
		}

		TSharedPtr<FJsonObject> RapidIterationObj = MakeShared<FJsonObject>();
		RapidIterationObj->SetArrayField(TEXT("parameters"), TArray<TSharedPtr<FJsonValue>>());
		RapidIterationObj->SetStringField(TEXT("note"), TEXT("Rapid iteration parameters are compiler/runtime generated and are reported here as an explicit non-lossless area."));
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("rapid_iteration_parameters.json")), RapidIterationObj, &WrittenFiles))
		{
			return false;
		}

		TSharedPtr<FJsonObject> RenderersObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> RenderersJson;
		if (EmitterData)
		{
			const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
			RenderersJson.Reserve(Renderers.Num());
			for (int32 RendererIndex = 0; RendererIndex < Renderers.Num(); ++RendererIndex)
			{
				UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];
				if (!Renderer)
				{
					continue;
				}
				TSharedPtr<FJsonObject> RendererObj = UeAgentNiagaraOps::BuildRendererJson(*Renderer, RendererIndex);
				AppendPropertyArray(RendererObj, TEXT("raw_properties"), BuildObjectPropertiesJson(Renderer));
				RenderersJson.Add(MakeShared<FJsonValueObject>(RendererObj));
			}
		}
		RenderersObj->SetArrayField(TEXT("renderers"), RenderersJson);
		RenderersObj->SetNumberField(TEXT("renderer_count"), RenderersJson.Num());
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("renderers.json")), RenderersObj, &WrittenFiles))
		{
			return false;
		}

		TSharedPtr<FJsonObject> EventHandlersObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> EventHandlersJson;
		if (EmitterData)
		{
			TArray<FNiagaraEventScriptProperties>& EventHandlers = EmitterData->EventHandlerScriptProps;
			EventHandlersJson.Reserve(EventHandlers.Num());
			for (int32 EventIndex = 0; EventIndex < EventHandlers.Num(); ++EventIndex)
			{
				FNiagaraEventScriptProperties& EventHandler = EventHandlers[EventIndex];
				TSharedPtr<FJsonObject> EventObj = UeAgentNiagaraOps::BuildEventHandlerJson(EventHandler, EventIndex);
				AppendPropertyArray(EventObj, TEXT("raw_properties"), BuildStructPropertiesJson(FNiagaraEventScriptProperties::StaticStruct(), &EventHandler, Emitter));
				AddUniqueScriptReference(ScriptRefs, EventHandler.Script, TEXT("event_handler"), EmitterFolderRelative);
				EventHandlersJson.Add(MakeShared<FJsonValueObject>(EventObj));
			}
		}
		EventHandlersObj->SetArrayField(TEXT("event_handlers"), EventHandlersJson);
		EventHandlersObj->SetNumberField(TEXT("event_handler_count"), EventHandlersJson.Num());
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("event_handlers.json")), EventHandlersObj, &WrittenFiles))
		{
			return false;
		}

		TSharedPtr<FJsonObject> EventGeneratorsObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> EventGeneratorsJson;
		if (EmitterData)
		{
			auto ResolveOwnerUsage = [](const FString& OwnerScript, ENiagaraScriptUsage& OutUsage) -> bool
			{
				if (OwnerScript.Equals(TEXT("emitter_spawn"), ESearchCase::IgnoreCase))
				{
					OutUsage = ENiagaraScriptUsage::EmitterSpawnScript;
					return true;
				}
				if (OwnerScript.Equals(TEXT("emitter_update"), ESearchCase::IgnoreCase))
				{
					OutUsage = ENiagaraScriptUsage::EmitterUpdateScript;
					return true;
				}
				if (OwnerScript.Equals(TEXT("particle_spawn"), ESearchCase::IgnoreCase))
				{
					OutUsage = ENiagaraScriptUsage::ParticleSpawnScript;
					return true;
				}
				if (OwnerScript.Equals(TEXT("particle_update"), ESearchCase::IgnoreCase))
				{
					OutUsage = ENiagaraScriptUsage::ParticleUpdateScript;
					return true;
				}
				return false;
			};

			auto StageContainsModule = [EmitterData, &ResolveOwnerUsage](const FString& OwnerScript, const FString& RequiredModuleName) -> bool
			{
				if (!EmitterData || RequiredModuleName.IsEmpty())
				{
					return true;
				}

				ENiagaraScriptUsage RequiredUsage = ENiagaraScriptUsage::Function;
				if (!ResolveOwnerUsage(OwnerScript, RequiredUsage))
				{
					return true;
				}

				UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
				UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
				if (!Graph)
				{
					return true;
				}

				TArray<UNiagaraNodeOutput*> OutputNodes;
				UeAgentNiagaraOps::GetOutputNodesFromGraph(*Graph, OutputNodes);
				for (UNiagaraNodeOutput* OutputNode : OutputNodes)
				{
					if (!OutputNode || OutputNode->GetUsage() != RequiredUsage)
					{
						continue;
					}

					TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
					UeAgentNiagaraOps::GetStageModuleNodes(*OutputNode, ModuleNodes);
					for (UNiagaraNodeFunctionCall* ModuleNode : ModuleNodes)
					{
						if (!ModuleNode)
						{
							continue;
						}

						const FString ModuleName = ModuleNode->GetFunctionName();
						const FString ModuleScriptPath = UeAgentNiagaraOps::GetModuleScriptPath(*ModuleNode);
						if (ModuleName.Equals(RequiredModuleName, ESearchCase::IgnoreCase) || ModuleScriptPath.Contains(RequiredModuleName, ESearchCase::IgnoreCase))
						{
							return true;
						}
					}
					return false;
				}

				return true;
			};

			auto ShouldSkipOrphanBuiltInGenerator = [&StageContainsModule](const FString& OwnerScript, const FString& GeneratorId) -> bool
			{
				FString RequiredModuleName;
				if (GeneratorId.Equals(TEXT("CollisionEvent"), ESearchCase::IgnoreCase))
				{
					RequiredModuleName = TEXT("GenerateCollisionEvent");
				}
				else if (GeneratorId.Equals(TEXT("DeathEvent"), ESearchCase::IgnoreCase))
				{
					RequiredModuleName = TEXT("GenerateDeathEvent");
				}

				return !RequiredModuleName.IsEmpty() && !StageContainsModule(OwnerScript, RequiredModuleName);
			};

			auto AppendEventGenerators = [&EventGeneratorsJson, Emitter, &ShouldSkipOrphanBuiltInGenerator](const FString& OwnerScript, const TArray<FNiagaraEventGeneratorProperties>& EventGenerators)
			{
				for (int32 GeneratorIndex = 0; GeneratorIndex < EventGenerators.Num(); ++GeneratorIndex)
				{
					FNiagaraEventGeneratorProperties& Generator = const_cast<FNiagaraEventGeneratorProperties&>(EventGenerators[GeneratorIndex]);
					if (ShouldSkipOrphanBuiltInGenerator(OwnerScript, Generator.ID.ToString()))
					{
						continue;
					}

					TSharedPtr<FJsonObject> GeneratorObj = MakeShared<FJsonObject>();
					GeneratorObj->SetStringField(TEXT("owner_script"), OwnerScript);
					GeneratorObj->SetNumberField(TEXT("generator_index"), GeneratorIndex);
					GeneratorObj->SetStringField(TEXT("id"), Generator.ID.ToString());
					GeneratorObj->SetNumberField(TEXT("max_events_per_frame"), Generator.MaxEventsPerFrame);
					AppendPropertyArray(GeneratorObj, TEXT("raw_properties"), BuildStructPropertiesJson(FNiagaraEventGeneratorProperties::StaticStruct(), &Generator, Emitter));
					EventGeneratorsJson.Add(MakeShared<FJsonValueObject>(GeneratorObj));
				}
			};

			AppendEventGenerators(TEXT("emitter_spawn"), EmitterData->EmitterSpawnScriptProps.EventGenerators);
			AppendEventGenerators(TEXT("emitter_update"), EmitterData->EmitterUpdateScriptProps.EventGenerators);
			AppendEventGenerators(TEXT("particle_spawn"), EmitterData->SpawnScriptProps.EventGenerators);
			AppendEventGenerators(TEXT("particle_update"), EmitterData->UpdateScriptProps.EventGenerators);
		}
		EventGeneratorsObj->SetArrayField(TEXT("event_generators"), EventGeneratorsJson);
		EventGeneratorsObj->SetNumberField(TEXT("event_generator_count"), EventGeneratorsJson.Num());
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("event_generators.json")), EventGeneratorsObj, &WrittenFiles))
		{
			return false;
		}

		TSharedPtr<FJsonObject> DataInterfacesObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> DataInterfacesJson;
		if (EmitterData)
		{
			if (UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource))
			{
				if (UNiagaraGraph* Graph = ScriptSource->NodeGraph)
				{
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
						if (!InputNode)
						{
							continue;
						}
						FProperty* DataInterfaceProperty = nullptr;
						void* DataInterfaceValuePtr = nullptr;
						if (!UeAgentNiagaraOps::ResolvePropertyPath(InputNode, TEXT("DataInterface"), DataInterfaceProperty, DataInterfaceValuePtr))
						{
							continue;
						}
						FObjectPropertyBase* DataInterfaceObjectProperty = CastField<FObjectPropertyBase>(DataInterfaceProperty);
						UNiagaraDataInterface* DataInterface = DataInterfaceObjectProperty ? Cast<UNiagaraDataInterface>(DataInterfaceObjectProperty->GetObjectPropertyValue(DataInterfaceValuePtr)) : nullptr;
						if (!DataInterface)
						{
							continue;
						}

						TSharedPtr<FJsonObject> DataInterfaceObj = MakeShared<FJsonObject>();
						DataInterfaceObj->SetStringField(TEXT("node_guid"), InputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
						DataInterfaceObj->SetStringField(TEXT("input_name"), InputNode->Input.GetName().ToString());
						DataInterfaceObj->SetStringField(TEXT("input_type"), InputNode->Input.GetType().GetNameText().ToString());
						DataInterfaceObj->SetStringField(TEXT("data_interface_class"), DataInterface->GetClass()->GetPathName());
						DataInterfaceObj->SetStringField(TEXT("data_interface_object_path"), DataInterface->GetPathName());
						AppendPropertyArray(DataInterfaceObj, TEXT("raw_properties"), BuildObjectPropertiesJson(DataInterface));
						DataInterfacesJson.Add(MakeShared<FJsonValueObject>(DataInterfaceObj));
					}
				}
			}
		}
		DataInterfacesObj->SetArrayField(TEXT("data_interfaces"), DataInterfacesJson);
		DataInterfacesObj->SetNumberField(TEXT("data_interface_count"), DataInterfacesJson.Num());
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("data_interfaces.json")), DataInterfacesObj, &WrittenFiles))
		{
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> StagesIndexJson;
		if (EmitterData)
		{
			UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
			UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
			if (Graph)
			{
				TArray<UNiagaraNodeOutput*> OutputNodes;
				UeAgentNiagaraOps::GetOutputNodesFromGraph(*Graph, OutputNodes);
				for (int32 StageIndex = 0; StageIndex < OutputNodes.Num(); ++StageIndex)
				{
					UNiagaraNodeOutput* OutputNode = OutputNodes[StageIndex];
					if (!OutputNode)
					{
						continue;
					}

					const FString UsageText = UeAgentNiagaraOps::ScriptUsageToString(OutputNode->GetUsage());
					const FString FileName = MakeStageFileName(StageIndex, UsageText, OutputNode->GetUsageId());
					TSharedPtr<FJsonObject> StageObj = BuildStageFolderJson(*OutputNode, StageIndex, EmitterData, ScriptRefs);
					StageObj->SetStringField(TEXT("file"), FileName);
					if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("stages"), FileName), StageObj, &WrittenFiles))
					{
						return false;
					}

					TSharedPtr<FJsonObject> StageIndexObj = MakeShared<FJsonObject>();
					StageIndexObj->SetNumberField(TEXT("stage_index"), StageIndex);
					StageIndexObj->SetStringField(TEXT("stage_key"), StageObj->GetStringField(TEXT("stage_key")));
					StageIndexObj->SetStringField(TEXT("script_usage"), UsageText);
					StageIndexObj->SetStringField(TEXT("script_usage_id"), OutputNode->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower));
					StageIndexObj->SetStringField(TEXT("file"), FileName);
					StagesIndexJson.Add(MakeShared<FJsonValueObject>(StageIndexObj));
				}
			}
			else
			{
				AddWarning(Warnings, TEXT("emitter_missing_graph_source"), EmitterFolderRelative, TEXT("Emitter graph source is missing; stages cannot be exported structurally."));
			}
		}

		TSharedPtr<FJsonObject> StagesIndexObj = MakeShared<FJsonObject>();
		StagesIndexObj->SetArrayField(TEXT("stages"), StagesIndexJson);
		StagesIndexObj->SetNumberField(TEXT("stage_count"), StagesIndexJson.Num());
		if (!SaveJsonFile(FPaths::Combine(EmitterFolder, TEXT("stages"), TEXT("index.json")), StagesIndexObj, &WrittenFiles))
		{
			return false;
		}

		if (EmitterData)
		{
			TArray<UNiagaraScript*> EmitterScripts;
			EmitterData->GetScripts(EmitterScripts, false, false);
			for (UNiagaraScript* Script : EmitterScripts)
			{
				AddUniqueScriptReference(ScriptRefs, Script, TEXT("emitter"), EmitterFolderRelative);
			}
		}

		return true;
	}

	static TSharedPtr<FJsonObject> BuildSystemStageJson(
		UNiagaraScript* Script,
		const FString& StageName,
		TMap<FString, TSharedPtr<FJsonObject>>& ScriptRefs)
	{
		TSharedPtr<FJsonObject> StageObj = MakeShared<FJsonObject>();
		StageObj->SetStringField(TEXT("stage_name"), StageName);
		StageObj->SetStringField(TEXT("stage_type"), TEXT("system_stage"));
		if (Script)
		{
			StageObj->SetObjectField(TEXT("script"), BuildScriptReferenceJson(Script, TEXT("system_stage"), StageName));
			UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
			UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
			if (Graph)
			{
				TArray<UNiagaraNodeOutput*> OutputNodes;
				UeAgentNiagaraOps::GetOutputNodesFromGraph(*Graph, OutputNodes);
				for (int32 OutputIndex = 0; OutputIndex < OutputNodes.Num(); ++OutputIndex)
				{
					UNiagaraNodeOutput* OutputNode = OutputNodes[OutputIndex];
					if (OutputNode && OutputNode->GetUsage() == Script->GetUsage())
					{
						StageObj = BuildStageFolderJson(*OutputNode, OutputIndex, nullptr, ScriptRefs);
						StageObj->SetStringField(TEXT("stage_name"), StageName);
						StageObj->SetStringField(TEXT("stage_type"), TEXT("system_stage"));
						StageObj->SetObjectField(TEXT("script"), BuildScriptReferenceJson(Script, TEXT("system_stage"), StageName));
						break;
					}
				}
			}
		}
		else
		{
			StageObj->SetObjectField(TEXT("script"), MakeShared<FJsonObject>());
		}
		StageObj->SetStringField(TEXT("coverage_note"), TEXT("System stage script reference, modules and module inputs are exported; apply reconstructs the System Spawn / System Update stage from modules[]."));
		return StageObj;
	}

	static int32 ApplyObjectPropertyArray(UObject* Target, const TSharedPtr<FJsonObject>& SourceObj, const FString& FieldName, TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& ContextPath)
	{
		if (!Target || !SourceObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!TryGetArrayField(SourceObj, *FieldName, Properties))
		{
			return 0;
		}

		int32 Applied = 0;
		for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
		{
			const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
			if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& PropertyObj = *PropertyObjPtr;
			bool bCanApply = true;
			PropertyObj->TryGetBoolField(TEXT("can_apply_generic"), bCanApply);
			if (!bCanApply)
			{
				continue;
			}

			const FString PropertyName = GetStringFieldAny(PropertyObj, { TEXT("property_name"), TEXT("path") });
			const FString ValueText = GetStringFieldAny(PropertyObj, { TEXT("value_text") });
			if (PropertyName.IsEmpty())
			{
				AddWarning(Warnings, TEXT("object_property_missing_name"), ContextPath, TEXT("Skipped UObject property entry with no property_name/path."));
				continue;
			}
			if (Target->IsA<UEdGraphNode>() && PropertyName.Equals(TEXT("NodeGuid"), ESearchCase::CaseSensitive))
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* CurveJsonObjPtr = nullptr;
			FString CurveJsonFieldName;
			if (PropertyObj->TryGetObjectField(TEXT("curve_json"), CurveJsonObjPtr) && CurveJsonObjPtr && CurveJsonObjPtr->IsValid())
			{
				CurveJsonFieldName = TEXT("curve_json");
			}
			else if (PropertyObj->TryGetObjectField(TEXT("value_json"), CurveJsonObjPtr) && CurveJsonObjPtr && CurveJsonObjPtr->IsValid())
			{
				CurveJsonFieldName = TEXT("value_json");
			}
			if (!CurveJsonFieldName.IsEmpty())
			{
				FProperty* Property = nullptr;
				void* ValuePtr = nullptr;
				if (!UeAgentNiagaraOps::ResolvePropertyPath(Target, PropertyName, Property, ValuePtr))
				{
					UeAgentCurveJson::AddCurveIssue(
						Warnings,
						TEXT("error"),
						TEXT("object_property_curve_path_not_found"),
						ContextPath + TEXT(".") + PropertyName,
						TEXT("Skipped curve_json because the target UObject property could not be resolved."));
					continue;
				}

				if (UeAgentCurveJson::TryApplyPropertyCurveJson(Property, ValuePtr, *CurveJsonObjPtr, ContextPath + TEXT(".") + PropertyName + TEXT(".") + CurveJsonFieldName, Warnings))
				{
					++Applied;
				}
				continue;
			}

			if (!PropertyObj->HasField(TEXT("value_text")))
			{
				AddWarning(Warnings, TEXT("object_property_missing_value_text"), ContextPath + TEXT(".") + PropertyName, TEXT("Skipped UObject property entry with no value_text."));
				continue;
			}

			FString AppliedValueText;
			FString CppType;
			FString ImportError;
			if (UeAgentNiagaraOps::SetObjectPropertyText(Target, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
			{
				++Applied;
				if (!AppliedValueText.Equals(ValueText, ESearchCase::CaseSensitive))
				{
					AddWarning(
						Warnings,
						TEXT("object_property_value_changed_after_import"),
						ContextPath + TEXT(".") + PropertyName,
						FString::Printf(TEXT("UObject property import read back a different value. requested='%s' applied='%s' cpp_type='%s'."), *ValueText, *AppliedValueText, *CppType));
				}
			}
			else
			{
				AddWarning(
					Warnings,
					TEXT("object_property_apply_failed"),
					ContextPath + TEXT(".") + PropertyName,
					FString::Printf(TEXT("ImportText failed for UObject property. requested='%s' cpp_type='%s' error='%s'."), *ValueText, *CppType, *ImportError));
			}
		}
		return Applied;
	}

	static int32 ApplyStructPropertyArray(UStruct* StructType, void* StructData, UObject* OwnerForImport, const TSharedPtr<FJsonObject>& SourceObj, const FString& FieldName, TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& ContextPath)
	{
		if (!StructType || !StructData || !SourceObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!TryGetArrayField(SourceObj, *FieldName, Properties))
		{
			return 0;
		}

		int32 Applied = 0;
		for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
		{
			const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
			if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& PropertyObj = *PropertyObjPtr;
			bool bCanApply = true;
			PropertyObj->TryGetBoolField(TEXT("can_apply_generic"), bCanApply);
			if (!bCanApply)
			{
				continue;
			}

			const FString PropertyName = GetStringFieldAny(PropertyObj, { TEXT("property_name"), TEXT("path") });
			const FString ValueText = GetStringFieldAny(PropertyObj, { TEXT("value_text") });
			if (PropertyName.IsEmpty())
			{
				AddWarning(Warnings, TEXT("struct_property_missing_name"), ContextPath, TEXT("Skipped UStruct property entry with no property_name/path."));
				continue;
			}

			const TSharedPtr<FJsonObject>* CurveJsonObjPtr = nullptr;
			FString CurveJsonFieldName;
			if (PropertyObj->TryGetObjectField(TEXT("curve_json"), CurveJsonObjPtr) && CurveJsonObjPtr && CurveJsonObjPtr->IsValid())
			{
				CurveJsonFieldName = TEXT("curve_json");
			}
			else if (PropertyObj->TryGetObjectField(TEXT("value_json"), CurveJsonObjPtr) && CurveJsonObjPtr && CurveJsonObjPtr->IsValid())
			{
				CurveJsonFieldName = TEXT("value_json");
			}
			if (!CurveJsonFieldName.IsEmpty())
			{
				FProperty* Property = nullptr;
				void* ValuePtr = nullptr;
				if (!UeAgentNiagaraOps::ResolveStructPropertyPath(StructType, StructData, PropertyName, Property, ValuePtr))
				{
					UeAgentCurveJson::AddCurveIssue(
						Warnings,
						TEXT("error"),
						TEXT("struct_property_curve_path_not_found"),
						ContextPath + TEXT(".") + PropertyName,
						TEXT("Skipped curve_json because the target UStruct property could not be resolved."));
					continue;
				}

				if (UeAgentCurveJson::TryApplyPropertyCurveJson(Property, ValuePtr, *CurveJsonObjPtr, ContextPath + TEXT(".") + PropertyName + TEXT(".") + CurveJsonFieldName, Warnings))
				{
					++Applied;
				}
				continue;
			}

			if (!PropertyObj->HasField(TEXT("value_text")))
			{
				AddWarning(Warnings, TEXT("struct_property_missing_value_text"), ContextPath + TEXT(".") + PropertyName, TEXT("Skipped UStruct property entry with no value_text."));
				continue;
			}

			FString AppliedValueText;
			FString CppType;
			FString ImportError;
			if (UeAgentNiagaraOps::SetStructPropertyText(StructType, StructData, OwnerForImport, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
			{
				++Applied;
				if (!AppliedValueText.Equals(ValueText, ESearchCase::CaseSensitive))
				{
					AddWarning(
						Warnings,
						TEXT("struct_property_value_changed_after_import"),
						ContextPath + TEXT(".") + PropertyName,
						FString::Printf(TEXT("UStruct property import read back a different value. requested='%s' applied='%s' cpp_type='%s'."), *ValueText, *AppliedValueText, *CppType));
				}
			}
			else
			{
				AddWarning(
					Warnings,
					TEXT("struct_property_apply_failed"),
					ContextPath + TEXT(".") + PropertyName,
					FString::Printf(TEXT("ImportText failed for UStruct property. requested='%s' cpp_type='%s' error='%s'."), *ValueText, *CppType, *ImportError));
			}
		}
		return Applied;
	}

	static bool JsonPropertyArrayContainsProperty(const TSharedPtr<FJsonObject>& SourceObj, const FString& FieldName, const FString& PropertyName)
	{
		if (!SourceObj.IsValid() || PropertyName.IsEmpty())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!TryGetArrayField(SourceObj, *FieldName, Properties))
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
		{
			const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
			if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
			{
				continue;
			}

			bool bCanApply = true;
			(*PropertyObjPtr)->TryGetBoolField(TEXT("can_apply_generic"), bCanApply);
			if (!bCanApply)
			{
				continue;
			}

			const FString CandidateName = GetStringFieldAny(*PropertyObjPtr, { TEXT("property_name"), TEXT("path") });
			if (CandidateName.Equals(PropertyName, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return false;
	}

	static FString OwnerScriptToStageScriptUsage(const FString& OwnerScript)
	{
		if (OwnerScript.Equals(TEXT("emitter_spawn"), ESearchCase::IgnoreCase))
		{
			return TEXT("EmitterSpawnScript");
		}
		if (OwnerScript.Equals(TEXT("emitter_update"), ESearchCase::IgnoreCase))
		{
			return TEXT("EmitterUpdateScript");
		}
		if (OwnerScript.Equals(TEXT("particle_spawn"), ESearchCase::IgnoreCase))
		{
			return TEXT("ParticleSpawnScript");
		}
		if (OwnerScript.Equals(TEXT("particle_update"), ESearchCase::IgnoreCase))
		{
			return TEXT("ParticleUpdateScript");
		}
		return FString();
	}

	static bool FolderStageContainsModule(const FString& EmitterFolder, const FString& OwnerScript, const FString& ModuleName)
	{
		const FString StageUsage = OwnerScriptToStageScriptUsage(OwnerScript);
		if (StageUsage.IsEmpty() || ModuleName.IsEmpty())
		{
			return true;
		}

		TSharedPtr<FJsonObject> StagesIndexObj;
		FString LoadError;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("stages"), TEXT("index.json")), StagesIndexObj, LoadError) || !StagesIndexObj.IsValid())
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Stages = nullptr;
		if (!TryGetArrayField(StagesIndexObj, TEXT("stages"), Stages))
		{
			return true;
		}

		for (const TSharedPtr<FJsonValue>& StageValue : *Stages)
		{
			const TSharedPtr<FJsonObject>* StageObjPtr = nullptr;
			if (!StageValue.IsValid() || !StageValue->TryGetObject(StageObjPtr) || !StageObjPtr || !StageObjPtr->IsValid())
			{
				continue;
			}

			FString ScriptUsage;
			(*StageObjPtr)->TryGetStringField(TEXT("script_usage"), ScriptUsage);
			if (!ScriptUsage.Equals(StageUsage, ESearchCase::IgnoreCase))
			{
				continue;
			}

			FString StageFile;
			(*StageObjPtr)->TryGetStringField(TEXT("file"), StageFile);
			if (StageFile.IsEmpty())
			{
				return true;
			}

			TSharedPtr<FJsonObject> StageObj;
			if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("stages"), StageFile), StageObj, LoadError) || !StageObj.IsValid())
			{
				return true;
			}

			const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
			if (!TryGetArrayField(StageObj, TEXT("modules"), Modules))
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& ModuleValue : *Modules)
			{
				const TSharedPtr<FJsonObject>* ModuleObjPtr = nullptr;
				if (!ModuleValue.IsValid() || !ModuleValue->TryGetObject(ModuleObjPtr) || !ModuleObjPtr || !ModuleObjPtr->IsValid())
				{
					continue;
				}

				FString ModuleNameText;
				FString ModuleScriptPath;
				(*ModuleObjPtr)->TryGetStringField(TEXT("module_name"), ModuleNameText);
				(*ModuleObjPtr)->TryGetStringField(TEXT("module_script_path"), ModuleScriptPath);
				if (ModuleNameText.Equals(ModuleName, ESearchCase::IgnoreCase) || ModuleScriptPath.Contains(ModuleName, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}

			return false;
		}

		return true;
	}

	static bool ShouldSkipBuiltInEventGeneratorWithoutStageModule(const FString& EmitterFolder, const FString& OwnerScript, const FString& GeneratorId, TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		FString RequiredModuleName;
		if (GeneratorId.Equals(TEXT("CollisionEvent"), ESearchCase::IgnoreCase))
		{
			RequiredModuleName = TEXT("GenerateCollisionEvent");
		}
		else if (GeneratorId.Equals(TEXT("DeathEvent"), ESearchCase::IgnoreCase))
		{
			RequiredModuleName = TEXT("GenerateDeathEvent");
		}
		else
		{
			return false;
		}

		if (FolderStageContainsModule(EmitterFolder, OwnerScript, RequiredModuleName))
		{
			return false;
		}

		AddWarning(
			Warnings,
			TEXT("event_generator_skipped_missing_stage_module"),
			EmitterFolder + TEXT(":") + OwnerScript + TEXT(":") + GeneratorId,
			FString::Printf(TEXT("Skipped built-in event generator because the %s stage has no %s module."), *OwnerScript, *RequiredModuleName));
		return true;
	}

	static UNiagaraSystem* CreateNiagaraSystemAsset(const FString& AssetPath, FString& OutError)
	{
		const FString PackageName = UeAgentNiagaraOps::NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (AssetName.IsEmpty())
		{
			OutError = TEXT("invalid_asset_name");
			return nullptr;
		}

		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
		UeAgentNiagaraOps::DetachStaleObjectAtPath(ObjectPath);

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return nullptr;
		}

		UNiagaraSystem* NiagaraSystem = NewObject<UNiagaraSystem>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!NiagaraSystem)
		{
			OutError = TEXT("create_niagara_system_failed");
			return nullptr;
		}

		UNiagaraSystemFactoryNew::InitializeSystem(NiagaraSystem, false);
		FAssetRegistryModule::AssetCreated(NiagaraSystem);
		NiagaraSystem->MarkPackageDirty();
		Package->MarkPackageDirty();
		return NiagaraSystem;
	}

	static UNiagaraEmitter* CreateNiagaraEmitterAsset(const FString& AssetPath, const bool bAddDefaultModulesAndRenderers, FString& OutError)
	{
		const FString PackageName = UeAgentNiagaraOps::NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (AssetName.IsEmpty())
		{
			OutError = TEXT("invalid_asset_name");
			return nullptr;
		}

		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
		UeAgentNiagaraOps::DetachStaleObjectAtPath(ObjectPath);

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return nullptr;
		}

		UNiagaraEmitter* NiagaraEmitter = NewObject<UNiagaraEmitter>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!NiagaraEmitter)
		{
			OutError = TEXT("create_niagara_emitter_failed");
			return nullptr;
		}

		UNiagaraEmitterFactoryNew::InitializeEmitter(NiagaraEmitter, bAddDefaultModulesAndRenderers);
		FAssetRegistryModule::AssetCreated(NiagaraEmitter);
		NiagaraEmitter->MarkPackageDirty();
		Package->MarkPackageDirty();
		return NiagaraEmitter;
	}

	static UNiagaraScript* CreateNiagaraScriptAsset(const FString& AssetPath, const ENiagaraScriptUsage Usage, FString& OutError)
	{
		const FString PackageName = UeAgentNiagaraOps::NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (AssetName.IsEmpty())
		{
			OutError = TEXT("invalid_asset_name");
			return nullptr;
		}

		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
		UeAgentNiagaraOps::DetachStaleObjectAtPath(ObjectPath);

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return nullptr;
		}

		UNiagaraScript* NiagaraScript = NewObject<UNiagaraScript>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!NiagaraScript)
		{
			OutError = TEXT("create_niagara_script_failed");
			return nullptr;
		}

		NiagaraScript->SetUsage(Usage);

		UNiagaraScriptSource* ScriptSource = NewObject<UNiagaraScriptSource>(NiagaraScript, NAME_None, RF_Transactional);
		UNiagaraGraph* Graph = ScriptSource ? NewObject<UNiagaraGraph>(ScriptSource, NAME_None, RF_Transactional) : nullptr;
		if (!ScriptSource || !Graph)
		{
			OutError = TEXT("create_niagara_script_graph_failed");
			return nullptr;
		}
		ScriptSource->NodeGraph = Graph;

		UNiagaraNodeInput* InputNode = NewObject<UNiagaraNodeInput>(Graph, NAME_None, RF_Transactional);
		UNiagaraNodeOutput* OutputNode = NewObject<UNiagaraNodeOutput>(Graph, NAME_None, RF_Transactional);
		if (!InputNode || !OutputNode)
		{
			OutError = TEXT("create_niagara_script_bridge_failed");
			return nullptr;
		}
		InputNode->CreateNewGuid();
		OutputNode->CreateNewGuid();

		if (Usage == ENiagaraScriptUsage::Module)
		{
			InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("MapIn"));
			OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Output")));
		}
		else
		{
			InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Input"));
			OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Output")));
		}
		InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
		OutputNode->SetUsage(Usage);

		InputNode->AllocateDefaultPins();
		OutputNode->AllocateDefaultPins();
		Graph->AddNode(InputNode, true, false);
		Graph->AddNode(OutputNode, true, false);
		if (UEdGraphPin* InputOutputPin = UeAgentNiagaraOps::FindFirstOutputPin(*InputNode))
		{
			if (UEdGraphPin* OutputInputPin = UeAgentNiagaraOps::FindFirstInputPin(*OutputNode))
			{
				if (const UEdGraphSchema* Schema = Graph->GetSchema())
				{
					Schema->TryCreateConnection(InputOutputPin, OutputInputPin);
				}
				if (!InputOutputPin->LinkedTo.Contains(OutputInputPin))
				{
					InputOutputPin->MakeLinkTo(OutputInputPin);
				}
			}
		}
		NiagaraScript->SetLatestSource(ScriptSource);
		NiagaraScript->RequestCompile(FGuid());
		FAssetRegistryModule::AssetCreated(NiagaraScript);
		NiagaraScript->MarkPackageDirty();
		Package->MarkPackageDirty();
		return NiagaraScript;
	}

	static bool FindUserParameterByName(const FNiagaraUserRedirectionParameterStore& Store, const FString& ParameterName, FNiagaraVariable& OutParameter)
	{
		return UeAgentNiagaraOps::FindUserParameterByName(Store, ParameterName, OutParameter);
	}

	static int32 ApplyUserParameters(UNiagaraSystem& NiagaraSystem, const FString& FolderPath, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		TSharedPtr<FJsonObject> UserParamsObj;
		if (!LoadJsonFileOptional(FPaths::Combine(FolderPath, TEXT("parameters"), TEXT("user_parameters.json")), UserParamsObj, OutError))
		{
			return INDEX_NONE;
		}
		if (!UserParamsObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
		if (!TryGetArrayField(UserParamsObj, TEXT("parameters"), Parameters))
		{
			return 0;
		}

		int32 Applied = 0;
		FNiagaraUserRedirectionParameterStore& Store = NiagaraSystem.GetExposedParameters();
		for (const TSharedPtr<FJsonValue>& ParameterValue : *Parameters)
		{
			const TSharedPtr<FJsonObject>* ParameterObjPtr = nullptr;
			if (!ParameterValue.IsValid() || !ParameterValue->TryGetObject(ParameterObjPtr) || !ParameterObjPtr || !ParameterObjPtr->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& ParameterObj = *ParameterObjPtr;
			const FString ParameterName = GetStringFieldAny(ParameterObj, { TEXT("name"), TEXT("parameter_name") });
			TArray<FString> TypeCandidates;
			auto AddTypeCandidate = [&TypeCandidates](FString TypeCandidate)
			{
				TypeCandidate.TrimStartAndEndInline();
				if (!TypeCandidate.IsEmpty())
				{
					TypeCandidates.AddUnique(TypeCandidate);
				}
			};
			AddTypeCandidate(GetStringFieldAny(ParameterObj, { TEXT("parameter_type") }));
			AddTypeCandidate(GetStringFieldAny(ParameterObj, { TEXT("type_path") }));
			AddTypeCandidate(GetStringFieldAny(ParameterObj, { TEXT("type_internal") }));
			AddTypeCandidate(GetStringFieldAny(ParameterObj, { TEXT("type") }));
			if (ParameterName.IsEmpty() || TypeCandidates.IsEmpty())
			{
				AddWarning(Warnings, TEXT("user_parameter_missing_name_or_type"), TEXT("parameters/user_parameters.json"), TEXT("Parameter entry has no name or type."));
				continue;
			}

			FNiagaraVariable Parameter;
			if (!FindUserParameterByName(Store, ParameterName, Parameter))
			{
				FNiagaraTypeDefinition TypeDefinition;
				FString ResolvedFromTypeText;
				for (const FString& TypeCandidate : TypeCandidates)
				{
					if (UeAgentNiagaraOps::ResolveNiagaraTypeDefinition(TypeCandidate, TypeDefinition))
					{
						ResolvedFromTypeText = TypeCandidate;
						break;
					}
				}
				if (ResolvedFromTypeText.IsEmpty())
				{
					AddWarning(Warnings, TEXT("user_parameter_type_unresolved"), ParameterName, FString::Join(TypeCandidates, TEXT(" | ")));
					continue;
				}

				FNiagaraVariable NewParameter(TypeDefinition, *ParameterName);
				FNiagaraUserRedirectionParameterStore::MakeUserVariable(NewParameter);
				if (!Store.AddParameter(NewParameter, true, true))
				{
					AddWarning(Warnings, TEXT("user_parameter_add_failed"), ParameterName, TEXT("Store.AddParameter returned false."));
					continue;
				}
				Parameter = NewParameter;
			}

			bool bHasValue = ParameterObj->HasField(TEXT("value_text"));
			ParameterObj->TryGetBoolField(TEXT("has_value"), bHasValue);
			if (bHasValue && ParameterObj->HasField(TEXT("value_text")))
			{
				const FString ValueText = ParameterObj->GetStringField(TEXT("value_text"));
				if (!UeAgentNiagaraOps::SetUserParameterValueFromText(Store, Parameter, ValueText, OutError))
				{
					AddWarning(Warnings, TEXT("user_parameter_value_apply_failed"), ParameterName, OutError);
					OutError.Reset();
					continue;
				}
			}
			++Applied;
		}

		return Applied;
	}

	static UNiagaraEmitter* CreateTemplateEmitterForFolderApply(UNiagaraSystem& NiagaraSystem, const FString& EmitterName)
	{
		const FName TemplateName = MakeUniqueObjectName(&NiagaraSystem, UNiagaraEmitter::StaticClass(), *FString::Printf(TEXT("%s_Template"), *MakeSafeFileName(EmitterName)));
		UNiagaraEmitter* TemplateEmitter = NewObject<UNiagaraEmitter>(&NiagaraSystem, TemplateName, RF_Transactional);
		if (TemplateEmitter)
		{
			UNiagaraEmitterFactoryNew::InitializeEmitter(TemplateEmitter, true);
		}
		return TemplateEmitter;
	}

	static int32 ApplyEmitterGraphParameters(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, const FString& EmitterFolder, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData.GraphSource);
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (!Graph)
		{
			AddWarning(Warnings, TEXT("emitter_parameters_missing_graph"), EmitterFolder, TEXT("Emitter graph source is missing; parameters cannot be applied."));
			return 0;
		}

		TSharedPtr<FJsonObject> ParametersObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("parameters.json")), ParametersObj, OutError))
		{
			return INDEX_NONE;
		}
		if (!ParametersObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
		if (!TryGetArrayField(ParametersObj, TEXT("parameters"), Parameters))
		{
			return 0;
		}

		int32 Applied = 0;
		for (const TSharedPtr<FJsonValue>& ParameterValue : *Parameters)
		{
			const TSharedPtr<FJsonObject>* ParameterObjPtr = nullptr;
			if (!ParameterValue.IsValid() || !ParameterValue->TryGetObject(ParameterObjPtr) || !ParameterObjPtr || !ParameterObjPtr->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& ParameterObj = *ParameterObjPtr;
			const FString ParameterName = GetStringFieldAny(ParameterObj, { TEXT("name"), TEXT("parameter_name") });
			TArray<FString> TypeCandidates;
			auto AddTypeCandidate = [&TypeCandidates](const FString& TypeCandidate)
			{
				const FString Trimmed = TypeCandidate.TrimStartAndEnd();
				if (!Trimmed.IsEmpty())
				{
					TypeCandidates.AddUnique(Trimmed);
				}
			};
			AddTypeCandidate(GetStringFieldAny(ParameterObj, { TEXT("type_path") }));
			AddTypeCandidate(GetStringFieldAny(ParameterObj, { TEXT("type_internal") }));
			AddTypeCandidate(GetStringFieldAny(ParameterObj, { TEXT("type") }));
			if (ParameterName.IsEmpty() || TypeCandidates.IsEmpty())
			{
				continue;
			}

			FNiagaraVariable ExistingParameter;
			UNiagaraScriptVariable* ScriptVariable = nullptr;
			if (!UeAgentNiagaraOps::FindGraphParameterByName(*Graph, ParameterName, ExistingParameter, ScriptVariable))
			{
				FNiagaraTypeDefinition TypeDefinition;
				for (const FString& TypeCandidate : TypeCandidates)
				{
					if (UeAgentNiagaraOps::ResolveNiagaraTypeDefinition(TypeCandidate, TypeDefinition))
					{
						break;
					}
				}
				if (!TypeDefinition.IsValid())
				{
					AddWarning(Warnings, TEXT("emitter_parameter_type_unresolved"), ParameterName, FString::Join(TypeCandidates, TEXT(" | ")));
					continue;
				}

				bool bIsStaticSwitch = false;
				ParameterObj->TryGetBoolField(TEXT("is_static_switch"), bIsStaticSwitch);
				ExistingParameter = FNiagaraVariable(TypeDefinition, *ParameterName);
				ScriptVariable = UeAgentNiagaraOps::AddGraphParameterMetadata(*Graph, ExistingParameter, bIsStaticSwitch);
			}

			if (!ScriptVariable)
			{
				AddWarning(Warnings, TEXT("emitter_parameter_apply_failed"), ParameterName, TEXT("Could not resolve or create UNiagaraScriptVariable metadata."));
				continue;
			}

			if (ParameterObj->HasField(TEXT("default_value_text")))
			{
				const FString DefaultValueText = ParameterObj->GetStringField(TEXT("default_value_text"));
				if (!UeAgentNiagaraOps::SetScriptVariableDefaultValueFromText(*ScriptVariable, DefaultValueText, OutError))
				{
					AddWarning(Warnings, TEXT("emitter_parameter_default_apply_failed"), ParameterName, OutError);
					OutError.Reset();
					continue;
				}
			}
			++Applied;
		}

		return Applied;
	}

	static bool EnsureEventHandlerForFolder(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, UNiagaraGraph& Graph, const TSharedPtr<FJsonObject>& EventObj, FNiagaraEventScriptProperties*& OutEventHandler, int32& OutEventIndex, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		OutEventHandler = nullptr;
		OutEventIndex = INDEX_NONE;

		FString UsageIdText;
		EventObj->TryGetStringField(TEXT("script_usage_id"), UsageIdText);
		FGuid UsageId;
		FGuid::Parse(UsageIdText, UsageId);
		if (!UsageId.IsValid())
		{
			UsageId = FGuid::NewGuid();
		}

		for (int32 Index = 0; Index < EmitterData.EventHandlerScriptProps.Num(); ++Index)
		{
			FNiagaraEventScriptProperties& Existing = EmitterData.EventHandlerScriptProps[Index];
			if (Existing.Script && Existing.Script->GetUsageId() == UsageId)
			{
				OutEventHandler = &Existing;
				OutEventIndex = Index;
				return true;
			}
		}

		FNiagaraEventScriptProperties NewEventHandler;
		FString SourceEventName = TEXT("Event");
		EventObj->TryGetStringField(TEXT("source_event_name"), SourceEventName);
		SourceEventName.TrimStartAndEndInline();
		if (SourceEventName.IsEmpty())
		{
			SourceEventName = TEXT("Event");
		}
		NewEventHandler.SourceEventName = FName(*SourceEventName);

		FString SourceEmitterIdText;
		if (EventObj->TryGetStringField(TEXT("source_emitter_id"), SourceEmitterIdText))
		{
			FGuid::Parse(SourceEmitterIdText, NewEventHandler.SourceEmitterID);
		}

		FString ExecutionModeText;
		if (EventObj->TryGetStringField(TEXT("execution_mode"), ExecutionModeText) && !ExecutionModeText.IsEmpty())
		{
			EScriptExecutionMode ParsedExecutionMode = EScriptExecutionMode::EveryParticle;
			if (UeAgentNiagaraOps::ParseExecutionMode(ExecutionModeText, ParsedExecutionMode))
			{
				NewEventHandler.ExecutionMode = ParsedExecutionMode;
			}
		}

		double NumberValue = 0.0;
		if (EventObj->TryGetNumberField(TEXT("spawn_number"), NumberValue))
		{
			NewEventHandler.SpawnNumber = FMath::Max<uint32>(0, static_cast<uint32>(FMath::RoundToInt(NumberValue)));
		}
		if (EventObj->TryGetNumberField(TEXT("max_events_per_frame"), NumberValue))
		{
			NewEventHandler.MaxEventsPerFrame = FMath::Max<uint32>(0, static_cast<uint32>(FMath::RoundToInt(NumberValue)));
		}
		if (EventObj->TryGetNumberField(TEXT("min_spawn_number"), NumberValue))
		{
			NewEventHandler.MinSpawnNumber = FMath::Max<uint32>(0, static_cast<uint32>(FMath::RoundToInt(NumberValue)));
		}
		EventObj->TryGetBoolField(TEXT("random_spawn_number"), NewEventHandler.bRandomSpawnNumber);
		EventObj->TryGetBoolField(TEXT("update_attribute_initial_values"), NewEventHandler.UpdateAttributeInitialValues);

		NewEventHandler.Script = NewObject<UNiagaraScript>(&Emitter, MakeUniqueObjectName(&Emitter, UNiagaraScript::StaticClass(), TEXT("EventHandler")), RF_Transactional);
		if (!NewEventHandler.Script)
		{
			OutError = TEXT("create_event_handler_script_failed");
			return false;
		}
		NewEventHandler.Script->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
		NewEventHandler.Script->SetUsageId(UsageId);
		if (UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData.GraphSource))
		{
			NewEventHandler.Script->SetLatestSource(ScriptSource);
		}

		Emitter.AddEventHandler(NewEventHandler, EmitterData.Version.VersionGuid);
		UNiagaraNodeOutput* OutputNode = UeAgentNiagaraOps::EnsureStageInputOutputBridge(Graph, ENiagaraScriptUsage::ParticleEventScript, UsageId);
		if (!OutputNode)
		{
			AddWarning(Warnings, TEXT("event_handler_output_bridge_failed"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower), TEXT("Event handler was created but output bridge could not be initialized."));
		}

		for (int32 Index = 0; Index < EmitterData.EventHandlerScriptProps.Num(); ++Index)
		{
			FNiagaraEventScriptProperties& Existing = EmitterData.EventHandlerScriptProps[Index];
			if (Existing.Script && Existing.Script->GetUsageId() == UsageId)
			{
				OutEventHandler = &Existing;
				OutEventIndex = Index;
				return true;
			}
		}

		OutError = TEXT("event_handler_added_but_not_resolved");
		return false;
	}

	static bool LoadDynamicStageUsageIdsFromFolder(const FString& EmitterFolder, bool& bOutHasStageIndex, TSet<FGuid>& OutEventUsageIds, TSet<FGuid>& OutSimulationStageUsageIds, FString& OutError)
	{
		bOutHasStageIndex = false;
		OutEventUsageIds.Reset();
		OutSimulationStageUsageIds.Reset();

		TSharedPtr<FJsonObject> StagesIndexObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("stages"), TEXT("index.json")), StagesIndexObj, OutError))
		{
			return false;
		}
		if (!StagesIndexObj.IsValid())
		{
			return true;
		}

		bOutHasStageIndex = true;
		const TArray<TSharedPtr<FJsonValue>>* Stages = nullptr;
		if (!TryGetArrayField(StagesIndexObj, TEXT("stages"), Stages))
		{
			return true;
		}

		for (const TSharedPtr<FJsonValue>& StageValue : *Stages)
		{
			const TSharedPtr<FJsonObject>* StageObjPtr = nullptr;
			if (!StageValue.IsValid() || !StageValue->TryGetObject(StageObjPtr) || !StageObjPtr || !StageObjPtr->IsValid())
			{
				continue;
			}

			ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
			FGuid UsageId;
			FString StageKey;
			if ((*StageObjPtr)->TryGetStringField(TEXT("stage_key"), StageKey) && !StageKey.IsEmpty())
			{
				UeAgentNiagaraOps::TryParseStageKey(StageKey, Usage, UsageId);
			}
			if (Usage == ENiagaraScriptUsage::Function)
			{
				FString UsageText;
				(*StageObjPtr)->TryGetStringField(TEXT("script_usage"), UsageText);
				UeAgentNiagaraOps::TryParseScriptUsage(UsageText, Usage);
				FString UsageIdText;
				(*StageObjPtr)->TryGetStringField(TEXT("script_usage_id"), UsageIdText);
				FGuid::Parse(UsageIdText, UsageId);
			}

			if (!UsageId.IsValid())
			{
				continue;
			}
			if (Usage == ENiagaraScriptUsage::ParticleEventScript)
			{
				OutEventUsageIds.Add(UsageId);
			}
			else if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
			{
				OutSimulationStageUsageIds.Add(UsageId);
			}
		}

		return true;
	}

	static int32 PruneDynamicStagesMissingFromFolder(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, const FString& EmitterFolder, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		bool bHasStageIndex = false;
		TSet<FGuid> ExpectedEventUsageIds;
		TSet<FGuid> ExpectedSimulationStageUsageIds;
		if (!LoadDynamicStageUsageIdsFromFolder(EmitterFolder, bHasStageIndex, ExpectedEventUsageIds, ExpectedSimulationStageUsageIds, OutError))
		{
			return INDEX_NONE;
		}
		if (!bHasStageIndex)
		{
			return 0;
		}

		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData.GraphSource);
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (!Graph)
		{
			AddWarning(Warnings, TEXT("dynamic_stage_prune_missing_graph"), EmitterFolder, TEXT("Emitter graph source is missing; obsolete dynamic stages cannot be pruned."));
			return 0;
		}

		int32 Pruned = 0;
		TArray<int32> EventHandlerIndicesWithoutUsageId;
		TArray<FGuid> EventUsageIdsToRemove;
		for (int32 Index = 0; Index < EmitterData.EventHandlerScriptProps.Num(); ++Index)
		{
			const FNiagaraEventScriptProperties& EventHandler = EmitterData.EventHandlerScriptProps[Index];
			const FGuid UsageId = EventHandler.Script ? EventHandler.Script->GetUsageId() : FGuid();
			if (!UsageId.IsValid())
			{
				EventHandlerIndicesWithoutUsageId.Add(Index);
			}
			else if (!ExpectedEventUsageIds.Contains(UsageId))
			{
				EventUsageIdsToRemove.Add(UsageId);
			}
		}

		for (int32 RemoveIndex = EventHandlerIndicesWithoutUsageId.Num() - 1; RemoveIndex >= 0; --RemoveIndex)
		{
			const int32 EventHandlerIndex = EventHandlerIndicesWithoutUsageId[RemoveIndex];
			if (EmitterData.EventHandlerScriptProps.IsValidIndex(EventHandlerIndex))
			{
				EmitterData.EventHandlerScriptProps.RemoveAt(EventHandlerIndex);
				++Pruned;
			}
		}
		for (const FGuid& UsageId : EventUsageIdsToRemove)
		{
			Emitter.RemoveEventHandlerByUsageId(UsageId, EmitterData.Version.VersionGuid);
			UeAgentNiagaraOps::DestroyGraphNodesForStage(*Graph, ENiagaraScriptUsage::ParticleEventScript, UsageId);
			++Pruned;
		}

		TArray<UNiagaraSimulationStageBase*> SimulationStagesToRemove;
		for (UNiagaraSimulationStageBase* SimulationStage : EmitterData.GetSimulationStages())
		{
			if (!SimulationStage)
			{
				continue;
			}
			const FGuid UsageId = (SimulationStage && SimulationStage->Script) ? SimulationStage->Script->GetUsageId() : FGuid();
			if (!UsageId.IsValid() || !ExpectedSimulationStageUsageIds.Contains(UsageId))
			{
				SimulationStagesToRemove.Add(SimulationStage);
			}
		}
		for (UNiagaraSimulationStageBase* SimulationStage : SimulationStagesToRemove)
		{
			if (!SimulationStage)
			{
				continue;
			}
			const FGuid UsageId = (SimulationStage && SimulationStage->Script) ? SimulationStage->Script->GetUsageId() : FGuid();
			Emitter.RemoveSimulationStage(SimulationStage, EmitterData.Version.VersionGuid);
			if (UsageId.IsValid())
			{
				UeAgentNiagaraOps::DestroyGraphNodesForStage(*Graph, ENiagaraScriptUsage::ParticleSimulationStageScript, UsageId);
			}
			++Pruned;
		}

		if (Pruned > 0)
		{
			Graph->NotifyGraphChanged();
			Emitter.MarkPackageDirty();
		}
		return Pruned;
	}

	static TArray<FNiagaraEventGeneratorProperties>* ResolveEventGeneratorArray(FVersionedNiagaraEmitterData& EmitterData, const FString& OwnerScript)
	{
		if (OwnerScript.Equals(TEXT("emitter_spawn"), ESearchCase::IgnoreCase))
		{
			return &EmitterData.EmitterSpawnScriptProps.EventGenerators;
		}
		if (OwnerScript.Equals(TEXT("emitter_update"), ESearchCase::IgnoreCase))
		{
			return &EmitterData.EmitterUpdateScriptProps.EventGenerators;
		}
		if (OwnerScript.Equals(TEXT("particle_spawn"), ESearchCase::IgnoreCase))
		{
			return &EmitterData.SpawnScriptProps.EventGenerators;
		}
		if (OwnerScript.Equals(TEXT("particle_update"), ESearchCase::IgnoreCase))
		{
			return &EmitterData.UpdateScriptProps.EventGenerators;
		}
		return nullptr;
	}

	static int32 ApplyEventGeneratorsFromFolder(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, const FString& EmitterFolder, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		TSharedPtr<FJsonObject> EventGeneratorsObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("event_generators.json")), EventGeneratorsObj, OutError))
		{
			return INDEX_NONE;
		}
		if (!EventGeneratorsObj.IsValid())
		{
			return 0;
		}

		EmitterData.EmitterSpawnScriptProps.EventGenerators.Reset();
		EmitterData.EmitterUpdateScriptProps.EventGenerators.Reset();
		EmitterData.SpawnScriptProps.EventGenerators.Reset();
		EmitterData.UpdateScriptProps.EventGenerators.Reset();

		const TArray<TSharedPtr<FJsonValue>>* EventGenerators = nullptr;
		if (!TryGetArrayField(EventGeneratorsObj, TEXT("event_generators"), EventGenerators))
		{
			return 0;
		}

		int32 Applied = 0;
		for (const TSharedPtr<FJsonValue>& GeneratorValue : *EventGenerators)
		{
			const TSharedPtr<FJsonObject>* GeneratorObjPtr = nullptr;
			if (!GeneratorValue.IsValid() || !GeneratorValue->TryGetObject(GeneratorObjPtr) || !GeneratorObjPtr || !GeneratorObjPtr->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& GeneratorObj = *GeneratorObjPtr;
			FString OwnerScript;
			GeneratorObj->TryGetStringField(TEXT("owner_script"), OwnerScript);
			TArray<FNiagaraEventGeneratorProperties>* TargetGenerators = ResolveEventGeneratorArray(EmitterData, OwnerScript);
			if (!TargetGenerators)
			{
				AddWarning(Warnings, TEXT("event_generator_owner_unresolved"), OwnerScript, TEXT("Event generator owner_script must be emitter_spawn, emitter_update, particle_spawn or particle_update."));
				continue;
			}

			FNiagaraEventGeneratorProperties NewGenerator;
			FString GeneratorId;
			if (GeneratorObj->TryGetStringField(TEXT("id"), GeneratorId) && !GeneratorId.IsEmpty())
			{
				NewGenerator.ID = FName(*GeneratorId);
			}
			if (ShouldSkipBuiltInEventGeneratorWithoutStageModule(EmitterFolder, OwnerScript, GeneratorId, Warnings))
			{
				continue;
			}

			double MaxEventsPerFrame = 0.0;
			if (GeneratorObj->TryGetNumberField(TEXT("max_events_per_frame"), MaxEventsPerFrame))
			{
				NewGenerator.MaxEventsPerFrame = FMath::Max(0, FMath::RoundToInt(MaxEventsPerFrame));
			}

			const int32 NewIndex = TargetGenerators->Add(NewGenerator);
			FNiagaraEventGeneratorProperties& AddedGenerator = (*TargetGenerators)[NewIndex];
			Applied += ApplyStructPropertyArray(FNiagaraEventGeneratorProperties::StaticStruct(), &AddedGenerator, &Emitter, GeneratorObj, TEXT("raw_properties"), Warnings, Emitter.GetPathName());
			++Applied;
		}

		return Applied;
	}

	static int32 ApplyEventHandlersFromFolder(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, const FString& EmitterFolder, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData.GraphSource);
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (!Graph)
		{
			AddWarning(Warnings, TEXT("event_handlers_missing_graph"), EmitterFolder, TEXT("Emitter graph source is missing; event handler output stages cannot be created."));
			return 0;
		}

		TSharedPtr<FJsonObject> EventHandlersObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("event_handlers.json")), EventHandlersObj, OutError))
		{
			return INDEX_NONE;
		}
		if (!EventHandlersObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* EventHandlers = nullptr;
		if (!TryGetArrayField(EventHandlersObj, TEXT("event_handlers"), EventHandlers))
		{
			return 0;
		}

		int32 Applied = 0;
		for (const TSharedPtr<FJsonValue>& EventValue : *EventHandlers)
		{
			const TSharedPtr<FJsonObject>* EventObjPtr = nullptr;
			if (!EventValue.IsValid() || !EventValue->TryGetObject(EventObjPtr) || !EventObjPtr || !EventObjPtr->IsValid())
			{
				continue;
			}

			FNiagaraEventScriptProperties* EventHandler = nullptr;
			int32 EventIndex = INDEX_NONE;
			if (!EnsureEventHandlerForFolder(Emitter, EmitterData, *Graph, *EventObjPtr, EventHandler, EventIndex, Warnings, OutError))
			{
				return INDEX_NONE;
			}
			if (!EventHandler)
			{
				continue;
			}

			Applied += ApplyStructPropertyArray(FNiagaraEventScriptProperties::StaticStruct(), EventHandler, &Emitter, *EventObjPtr, TEXT("raw_properties"), Warnings, Emitter.GetPathName());
			++Applied;
		}
		return Applied;
	}

	static int32 ApplyDataInterfacesFromFolder(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, const FString& EmitterFolder, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData.GraphSource);
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (!Graph)
		{
			AddWarning(Warnings, TEXT("data_interfaces_missing_graph"), EmitterFolder, TEXT("Emitter graph source is missing; data interfaces cannot be applied."));
			return 0;
		}

		TSharedPtr<FJsonObject> DataInterfacesObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("data_interfaces.json")), DataInterfacesObj, OutError))
		{
			return INDEX_NONE;
		}
		if (!DataInterfacesObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* DataInterfaces = nullptr;
		if (!TryGetArrayField(DataInterfacesObj, TEXT("data_interfaces"), DataInterfaces))
		{
			return 0;
		}

		int32 Applied = 0;
		for (int32 DataInterfaceIndex = 0; DataInterfaceIndex < DataInterfaces->Num(); ++DataInterfaceIndex)
		{
			const FString DataInterfacePath = FString::Printf(TEXT("%s/data_interfaces.json.data_interfaces[%d]"), *EmitterFolder, DataInterfaceIndex);
			TSharedPtr<FJsonObject> DataInterfaceObj;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*DataInterfaces)[DataInterfaceIndex], DataInterfacePath, DataInterfaceObj, Warnings))
			{
				continue;
			}

			UeAgentJsonDiagnostics::WarnUnknownFields(
				DataInterfaceObj,
				DataInterfacePath,
				{
					TEXT("node_guid"),
					TEXT("input_name"),
					TEXT("input_type"),
					TEXT("data_interface_class"),
					TEXT("data_interface_object_path"),
					TEXT("raw_properties"),
					TEXT("raw_properties_count")
				},
				Warnings);

			FString NodeGuidText;
			FString InputNameText;
			FString DataInterfaceClassText;
			DataInterfaceObj->TryGetStringField(TEXT("node_guid"), NodeGuidText);
			DataInterfaceObj->TryGetStringField(TEXT("input_name"), InputNameText);
			DataInterfaceObj->TryGetStringField(TEXT("data_interface_class"), DataInterfaceClassText);

			FGuid NodeGuid;
			FGuid::Parse(NodeGuidText, NodeGuid);
			UNiagaraNodeInput* TargetInputNode = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
				if (!InputNode)
				{
					continue;
				}
				if ((NodeGuid.IsValid() && InputNode->NodeGuid == NodeGuid)
					|| (!InputNameText.IsEmpty() && InputNode->Input.GetName().ToString().Equals(InputNameText, ESearchCase::IgnoreCase)))
				{
					TargetInputNode = InputNode;
					break;
				}
			}

			if (!TargetInputNode && !DataInterfaceClassText.IsEmpty())
			{
				TArray<UNiagaraNodeInput*> ClassMatchedInputNodes;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
					if (!InputNode)
					{
						continue;
					}

					FProperty* CandidateDataInterfaceProperty = nullptr;
					void* CandidateDataInterfaceValuePtr = nullptr;
					if (!UeAgentNiagaraOps::ResolvePropertyPath(InputNode, TEXT("DataInterface"), CandidateDataInterfaceProperty, CandidateDataInterfaceValuePtr))
					{
						continue;
					}

					FObjectPropertyBase* CandidateObjectProperty = CastField<FObjectPropertyBase>(CandidateDataInterfaceProperty);
					UNiagaraDataInterface* CandidateDataInterface = CandidateObjectProperty
						? Cast<UNiagaraDataInterface>(CandidateObjectProperty->GetObjectPropertyValue(CandidateDataInterfaceValuePtr))
						: nullptr;
					if (CandidateDataInterface
						&& CandidateDataInterface->GetClass()
						&& CandidateDataInterface->GetClass()->GetPathName().Equals(DataInterfaceClassText, ESearchCase::IgnoreCase))
					{
						ClassMatchedInputNodes.Add(InputNode);
					}
				}

				if (ClassMatchedInputNodes.Num() == 1)
				{
					TargetInputNode = ClassMatchedInputNodes[0];
				}
				else if (ClassMatchedInputNodes.Num() > 1)
				{
					AddWarning(
						Warnings,
						TEXT("data_interface_class_match_ambiguous"),
						DataInterfacePath,
						FString::Printf(TEXT("Found %d data interface input nodes for class '%s'; node_guid/input_name did not disambiguate."), ClassMatchedInputNodes.Num(), *DataInterfaceClassText));
				}
			}

			if (!TargetInputNode)
			{
				AddWarning(Warnings, TEXT("data_interface_input_node_not_found"), DataInterfacePath, FString::Printf(TEXT("No UNiagaraNodeInput matched node_guid='%s' input_name='%s'."), *NodeGuidText, *InputNameText));
				continue;
			}

			FProperty* DataInterfaceProperty = nullptr;
			void* DataInterfaceValuePtr = nullptr;
			if (!UeAgentNiagaraOps::ResolvePropertyPath(TargetInputNode, TEXT("DataInterface"), DataInterfaceProperty, DataInterfaceValuePtr))
			{
				AddWarning(Warnings, TEXT("data_interface_property_not_found"), DataInterfacePath, TEXT("UNiagaraNodeInput.DataInterface property could not be resolved."));
				continue;
			}

			FObjectPropertyBase* DataInterfaceObjectProperty = CastField<FObjectPropertyBase>(DataInterfaceProperty);
			if (!DataInterfaceObjectProperty)
			{
				AddWarning(Warnings, TEXT("data_interface_property_not_object"), DataInterfacePath, TEXT("UNiagaraNodeInput.DataInterface is not an object property."));
				continue;
			}

			UNiagaraDataInterface* DataInterface = Cast<UNiagaraDataInterface>(DataInterfaceObjectProperty->GetObjectPropertyValue(DataInterfaceValuePtr));
			if (!DataInterface && !DataInterfaceClassText.IsEmpty())
			{
				UClass* DataInterfaceClass = LoadObject<UClass>(nullptr, *DataInterfaceClassText);
				if (DataInterfaceClass && DataInterfaceClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
				{
					TargetInputNode->Modify();
					DataInterface = NewObject<UNiagaraDataInterface>(TargetInputNode, DataInterfaceClass, NAME_None, RF_Transactional);
					DataInterfaceObjectProperty->SetObjectPropertyValue(DataInterfaceValuePtr, DataInterface);
				}
			}

			if (!DataInterface)
			{
				AddWarning(Warnings, TEXT("data_interface_instance_missing"), DataInterfacePath, FString::Printf(TEXT("Data interface instance could not be resolved or created for class '%s'."), *DataInterfaceClassText));
				continue;
			}

			DataInterface->Modify();
			Applied += ApplyObjectPropertyArray(DataInterface, DataInterfaceObj, TEXT("raw_properties"), Warnings, DataInterface->GetPathName());
			++Applied;
		}

		return Applied;
	}

	static UNiagaraSimulationStageBase* EnsureSimulationStageForFolder(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, UNiagaraGraph& Graph, const TSharedPtr<FJsonObject>& StageObj, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		FString UsageIdText;
		StageObj->TryGetStringField(TEXT("script_usage_id"), UsageIdText);
		FGuid UsageId;
		FGuid::Parse(UsageIdText, UsageId);

		if (UsageId.IsValid())
		{
			if (UNiagaraSimulationStageBase* ExistingStage = EmitterData.GetSimulationStageById(UsageId))
			{
				return ExistingStage;
			}
		}
		else
		{
			UsageId = FGuid::NewGuid();
		}

		FString StageClassPath = TEXT("/Script/Niagara.NiagaraSimulationStageGeneric");
		StageObj->TryGetStringField(TEXT("simulation_stage_class"), StageClassPath);
		UClass* StageClass = UeAgentNiagaraOps::ResolveClassByPath(StageClassPath);
		if (!StageClass || !StageClass->IsChildOf(UNiagaraSimulationStageBase::StaticClass()) || StageClass->HasAnyClassFlags(CLASS_Abstract))
		{
			StageClass = UeAgentNiagaraOps::ResolveClassByPath(TEXT("/Script/Niagara.NiagaraSimulationStageGeneric"));
		}
		if (!StageClass)
		{
			OutError = TEXT("invalid_simulation_stage_class");
			return nullptr;
		}

		UNiagaraSimulationStageBase* AddedStage = NewObject<UNiagaraSimulationStageBase>(&Emitter, StageClass, NAME_None, RF_Transactional);
		if (!AddedStage)
		{
			OutError = TEXT("create_simulation_stage_failed");
			return nullptr;
		}

		AddedStage->Script = NewObject<UNiagaraScript>(AddedStage, MakeUniqueObjectName(AddedStage, UNiagaraScript::StaticClass(), TEXT("SimulationStage")), RF_Transactional);
		if (!AddedStage->Script)
		{
			OutError = TEXT("create_simulation_stage_script_failed");
			return nullptr;
		}

		FString StageName = TEXT("SimulationStage");
		StageObj->TryGetStringField(TEXT("simulation_stage_name"), StageName);
		StageName.TrimStartAndEndInline();
		if (StageName.IsEmpty())
		{
			StageName = TEXT("SimulationStage");
		}
		AddedStage->SimulationStageName = FName(*StageName);
		AddedStage->Script->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
		AddedStage->Script->SetUsageId(UsageId);
		if (UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData.GraphSource))
		{
			AddedStage->Script->SetLatestSource(ScriptSource);
		}

		Emitter.AddSimulationStage(AddedStage, EmitterData.Version.VersionGuid);
		if (!UeAgentNiagaraOps::EnsureStageInputOutputBridge(Graph, ENiagaraScriptUsage::ParticleSimulationStageScript, UsageId))
		{
			AddWarning(Warnings, TEXT("simulation_stage_output_bridge_failed"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower), TEXT("Simulation stage was created but output bridge could not be initialized."));
		}
		return AddedStage;
	}

	static bool IsAssignmentModuleFolderObject(const TSharedPtr<FJsonObject>& ModuleObj)
	{
		const FString ModuleKind = GetStringFieldAny(ModuleObj, { TEXT("module_kind") });
		if (ModuleKind.Equals(TEXT("assignment"), ESearchCase::IgnoreCase))
		{
			return true;
		}

		const FString ModuleName = GetStringFieldAny(ModuleObj, { TEXT("module_name") });
		if (ModuleName.StartsWith(TEXT("SetVariables"), ESearchCase::IgnoreCase))
		{
			return true;
		}

		const FString ModuleScriptPath = GetStringFieldAny(ModuleObj, { TEXT("module_script_path"), TEXT("module_script_asset_path") });
		return ModuleScriptPath.Contains(TEXT("NiagaraNodeAssignment"));
	}

	static bool IsPrivateInlineNiagaraScriptPath(const FString& ModuleScriptPath)
	{
		return ModuleScriptPath.Contains(TEXT(":"));
	}

	static FString GetNiagaraInputDefaultValue(const TSharedPtr<FJsonObject>& InputObj)
	{
		FString Value;
		if (InputObj.IsValid() && InputObj->TryGetStringField(TEXT("override_default_value"), Value) && !Value.IsEmpty())
		{
			return Value;
		}
		if (InputObj.IsValid() && InputObj->TryGetStringField(TEXT("autogenerated_default_value"), Value))
		{
			return Value;
		}
		return FString();
	}

	static FString GetZeroDefaultValueForNiagaraType(const FNiagaraTypeDefinition& TypeDefinition)
	{
		if (TypeDefinition == FNiagaraTypeDefinition::GetBoolDef())
		{
			return TEXT("false");
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetIntDef())
		{
			return TEXT("0");
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetFloatDef() || TypeDefinition == FNiagaraTypeHelper::GetDoubleDef())
		{
			return TEXT("0.0");
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec2Def())
		{
			return TEXT("X=0.0 Y=0.0");
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec3Def() || TypeDefinition == FNiagaraTypeDefinition::GetPositionDef())
		{
			return TEXT("X=0.0 Y=0.0 Z=0.0");
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec4Def() || TypeDefinition == FNiagaraTypeDefinition::GetQuatDef())
		{
			return TEXT("X=0.0 Y=0.0 Z=0.0 W=0.0");
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetColorDef())
		{
			return TEXT("R=0.0 G=0.0 B=0.0 A=1.0");
		}
		return FString();
	}

	static UNiagaraNodeAssignment* AddAssignmentModuleFromFolder(
		UNiagaraNodeOutput& OutputNode,
		const int32 TargetIndex,
		const TSharedPtr<FJsonObject>& ModuleObj,
		TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		if (!TryGetArrayField(ModuleObj, TEXT("inputs"), Inputs))
		{
			AddWarning(Warnings, TEXT("assignment_module_inputs_missing"), GetStringFieldAny(ModuleObj, { TEXT("module_name") }), TEXT("Assignment module could not be reconstructed because it has no exported inputs."));
			return nullptr;
		}

		TArray<FNiagaraVariable> AssignmentVariables;
		TArray<FString> AssignmentDefaults;
		for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
		{
			const TSharedPtr<FJsonObject>* InputObjPtr = nullptr;
			if (!InputValue.IsValid() || !InputValue->TryGetObject(InputObjPtr) || !InputObjPtr || !InputObjPtr->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& InputObj = *InputObjPtr;
			FString InputName = GetStringFieldAny(InputObj, { TEXT("input_name") });
			InputName.TrimStartAndEndInline();
			if (!InputName.StartsWith(TEXT("Module."), ESearchCase::CaseSensitive))
			{
				continue;
			}

			FString TargetName = InputName.Mid(7);
			TargetName.TrimStartAndEndInline();
			if (TargetName.IsEmpty())
			{
				continue;
			}

			const FString TypeText = GetStringFieldAny(InputObj, { TEXT("input_type"), TEXT("parameter_type"), TEXT("type_internal") });
			FNiagaraTypeDefinition TypeDefinition;
			if (!UeAgentNiagaraOps::ResolveNiagaraTypeDefinition(TypeText, TypeDefinition))
			{
				AddWarning(Warnings, TEXT("assignment_input_type_unresolved"), TargetName, TypeText);
				continue;
			}

			FString DefaultValue = GetNiagaraInputDefaultValue(InputObj);
			if (DefaultValue.IsEmpty())
			{
				DefaultValue = GetZeroDefaultValueForNiagaraType(TypeDefinition);
			}
			AssignmentVariables.Add(FNiagaraVariable(TypeDefinition, *TargetName));
			AssignmentDefaults.Add(DefaultValue);
		}

		if (AssignmentVariables.Num() == 0)
		{
			AddWarning(Warnings, TEXT("assignment_module_targets_missing"), GetStringFieldAny(ModuleObj, { TEXT("module_name") }), TEXT("Assignment module had no resolvable assignment targets."));
			return nullptr;
		}

		return FNiagaraStackGraphUtilities::AddParameterModuleToStack(AssignmentVariables, OutputNode, TargetIndex, AssignmentDefaults);
	}

	static bool ApplyModuleInputDefault(UNiagaraGraph& Graph, UNiagaraNodeFunctionCall& ModuleNode, const TSharedPtr<FJsonObject>& InputObj, const FString& ContextPath, TArray<TSharedPtr<FJsonValue>>& Warnings);

	static UNiagaraNodeInput* FindDynamicInputDataInterfaceNode(
		UNiagaraGraph& Graph,
		UNiagaraNodeFunctionCall& DynamicInputNode,
		const TSharedPtr<FJsonObject>& DataInterfaceObj,
		const FString& ContextPath,
		TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		FString NodeGuidText;
		FString InputNameText;
		FString DataInterfaceClassText;
		DataInterfaceObj->TryGetStringField(TEXT("node_guid"), NodeGuidText);
		DataInterfaceObj->TryGetStringField(TEXT("input_name"), InputNameText);
		DataInterfaceObj->TryGetStringField(TEXT("data_interface_class"), DataInterfaceClassText);

		FGuid NodeGuid;
		FGuid::Parse(NodeGuidText, NodeGuid);
		if (NodeGuid.IsValid())
		{
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
				if (InputNode && InputNode->NodeGuid == NodeGuid)
				{
					return InputNode;
				}
			}
		}

		if (!InputNameText.IsEmpty())
		{
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
				if (InputNode && InputNode->Input.GetName().ToString().Equals(InputNameText, ESearchCase::IgnoreCase))
				{
					return InputNode;
				}
			}
		}

		TArray<UNiagaraNodeInput*> LinkedDataInterfaceInputs;
		UeAgentNiagaraOps::CollectLinkedDataInterfaceInputNodesForDynamicInput(DynamicInputNode, LinkedDataInterfaceInputs);
		if (!DataInterfaceClassText.IsEmpty() || !InputNameText.IsEmpty())
		{
			TArray<UNiagaraNodeInput*> Candidates;
			const FString InputNameLower = UeAgentNiagaraOps::MakeLowerTrimmed(InputNameText);
			for (UNiagaraNodeInput* InputNode : LinkedDataInterfaceInputs)
			{
				if (!InputNode)
				{
					continue;
				}

				bool bMatchesInputName = InputNameLower.IsEmpty();
				const FString CandidateInputNameLower = UeAgentNiagaraOps::MakeLowerTrimmed(InputNode->Input.GetName().ToString());
				if (!InputNameLower.IsEmpty())
				{
					bMatchesInputName = CandidateInputNameLower == InputNameLower ||
						CandidateInputNameLower.EndsWith(TEXT(".") + InputNameLower) ||
						InputNameLower.EndsWith(TEXT(".") + CandidateInputNameLower);
				}

				bool bMatchesClass = DataInterfaceClassText.IsEmpty();
				if (!bMatchesClass)
				{
					FProperty* CandidateDataInterfaceProperty = nullptr;
					void* CandidateDataInterfaceValuePtr = nullptr;
					if (UeAgentNiagaraOps::ResolvePropertyPath(InputNode, TEXT("DataInterface"), CandidateDataInterfaceProperty, CandidateDataInterfaceValuePtr))
					{
						FObjectPropertyBase* CandidateObjectProperty = CastField<FObjectPropertyBase>(CandidateDataInterfaceProperty);
						UNiagaraDataInterface* CandidateDataInterface = CandidateObjectProperty
							? Cast<UNiagaraDataInterface>(CandidateObjectProperty->GetObjectPropertyValue(CandidateDataInterfaceValuePtr))
							: nullptr;
						bMatchesClass = CandidateDataInterface
							&& CandidateDataInterface->GetClass()
							&& CandidateDataInterface->GetClass()->GetPathName().Equals(DataInterfaceClassText, ESearchCase::IgnoreCase);
					}
				}

				if (bMatchesInputName && bMatchesClass)
				{
					Candidates.Add(InputNode);
				}
			}

			if (Candidates.Num() == 1)
			{
				return Candidates[0];
			}
			if (Candidates.Num() > 1)
			{
				AddWarning(
					Warnings,
					TEXT("dynamic_input_data_interface_match_ambiguous"),
					ContextPath,
					FString::Printf(TEXT("Found %d linked data interface input nodes for input_name='%s' class='%s'."), Candidates.Num(), *InputNameText, *DataInterfaceClassText));
				return nullptr;
			}
		}

		AddWarning(
			Warnings,
			TEXT("dynamic_input_data_interface_input_node_not_found"),
			ContextPath,
			FString::Printf(TEXT("No linked data interface input node matched node_guid='%s' input_name='%s'."), *NodeGuidText, *InputNameText));
		return nullptr;
	}

	static int32 ApplyDynamicInputDataInterfacesFromJson(
		UNiagaraGraph& Graph,
		UNiagaraNodeFunctionCall& DynamicInputNode,
		const TSharedPtr<FJsonObject>& DynamicInputObj,
		const FString& ContextPath,
		TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		const TArray<TSharedPtr<FJsonValue>>* DataInterfaces = nullptr;
		if (!UeAgentJsonDiagnostics::ReadArrayField(DynamicInputObj, TEXT("data_interfaces"), ContextPath + TEXT(".data_interfaces"), DataInterfaces, Warnings, false))
		{
			return 0;
		}

		int32 Applied = 0;
		for (int32 DataInterfaceIndex = 0; DataInterfaceIndex < DataInterfaces->Num(); ++DataInterfaceIndex)
		{
			const FString DataInterfacePath = FString::Printf(TEXT("%s.data_interfaces[%d]"), *ContextPath, DataInterfaceIndex);
			TSharedPtr<FJsonObject> DataInterfaceObj;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*DataInterfaces)[DataInterfaceIndex], DataInterfacePath, DataInterfaceObj, Warnings))
			{
				continue;
			}

			UeAgentJsonDiagnostics::WarnUnknownFields(
				DataInterfaceObj,
				DataInterfacePath,
				{
					TEXT("node_guid"),
					TEXT("input_name"),
					TEXT("input_type"),
					TEXT("data_interface_class"),
					TEXT("data_interface_object_path"),
					TEXT("raw_properties"),
					TEXT("raw_properties_count")
				},
				Warnings);

			UNiagaraNodeInput* TargetInputNode = FindDynamicInputDataInterfaceNode(Graph, DynamicInputNode, DataInterfaceObj, DataInterfacePath, Warnings);
			if (!TargetInputNode)
			{
				continue;
			}

			FString DataInterfaceClassText;
			DataInterfaceObj->TryGetStringField(TEXT("data_interface_class"), DataInterfaceClassText);

			FProperty* DataInterfaceProperty = nullptr;
			void* DataInterfaceValuePtr = nullptr;
			if (!UeAgentNiagaraOps::ResolvePropertyPath(TargetInputNode, TEXT("DataInterface"), DataInterfaceProperty, DataInterfaceValuePtr))
			{
				AddWarning(Warnings, TEXT("dynamic_input_data_interface_property_not_found"), DataInterfacePath, TEXT("UNiagaraNodeInput.DataInterface property could not be resolved."));
				continue;
			}

			FObjectPropertyBase* DataInterfaceObjectProperty = CastField<FObjectPropertyBase>(DataInterfaceProperty);
			if (!DataInterfaceObjectProperty)
			{
				AddWarning(Warnings, TEXT("dynamic_input_data_interface_property_not_object"), DataInterfacePath, TEXT("UNiagaraNodeInput.DataInterface is not an object property."));
				continue;
			}

			UNiagaraDataInterface* DataInterface = Cast<UNiagaraDataInterface>(DataInterfaceObjectProperty->GetObjectPropertyValue(DataInterfaceValuePtr));
			if (!DataInterface && !DataInterfaceClassText.IsEmpty())
			{
				UClass* DataInterfaceClass = LoadObject<UClass>(nullptr, *DataInterfaceClassText);
				if (DataInterfaceClass && DataInterfaceClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
				{
					TargetInputNode->Modify();
					DataInterface = NewObject<UNiagaraDataInterface>(TargetInputNode, DataInterfaceClass, NAME_None, RF_Transactional);
					DataInterfaceObjectProperty->SetObjectPropertyValue(DataInterfaceValuePtr, DataInterface);
				}
			}

			if (!DataInterface)
			{
				AddWarning(Warnings, TEXT("dynamic_input_data_interface_instance_missing"), DataInterfacePath, FString::Printf(TEXT("Data interface instance could not be resolved or created for class '%s'."), *DataInterfaceClassText));
				continue;
			}

			DataInterface->Modify();
			Applied += ApplyObjectPropertyArray(DataInterface, DataInterfaceObj, TEXT("raw_properties"), Warnings, DataInterface->GetPathName());
			++Applied;
		}
		return Applied;
	}

	static bool ApplyModuleInputDynamicInput(
		UNiagaraGraph& Graph,
		UNiagaraNodeFunctionCall& ModuleNode,
		UEdGraphPin& OverridePin,
		const TSharedPtr<FJsonObject>& DynamicInputObj,
		const FString& ContextPath,
		TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(
			DynamicInputObj,
			ContextPath,
			{
				TEXT("node_guid"),
				TEXT("node_name"),
				TEXT("dynamic_input_name"),
				TEXT("script_asset_path"),
				TEXT("dynamic_input_script_asset_path"),
				TEXT("module_script_path"),
				TEXT("inputs"),
				TEXT("input_count"),
				TEXT("data_interfaces"),
				TEXT("data_interface_count")
			},
			Warnings);

		const FString DynamicInputScriptPath = GetStringFieldAny(DynamicInputObj, {
			TEXT("script_asset_path"),
			TEXT("dynamic_input_script_asset_path"),
			TEXT("module_script_path")
		});
		if (DynamicInputScriptPath.IsEmpty())
		{
			AddWarning(Warnings, TEXT("dynamic_input_missing_script_asset_path"), ContextPath, TEXT("dynamic_input has no script_asset_path; the dynamic input was skipped."));
			return true;
		}

		UNiagaraScript* DynamicInputScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(DynamicInputScriptPath));
		if (!DynamicInputScript)
		{
			AddWarning(Warnings, TEXT("dynamic_input_script_not_found"), ContextPath + TEXT(".script_asset_path"), DynamicInputScriptPath);
			return true;
		}

		if (OverridePin.LinkedTo.Num() > 0)
		{
			OverridePin.BreakAllPinLinks();
		}

		FString DynamicInputSuggestedName = GetStringFieldAny(DynamicInputObj, { TEXT("node_name"), TEXT("dynamic_input_name") });
		DynamicInputSuggestedName.TrimStartAndEndInline();

		UNiagaraNodeFunctionCall* DynamicInputNode = nullptr;
		FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(
			OverridePin,
			DynamicInputScript,
			DynamicInputNode,
			FGuid(),
			DynamicInputSuggestedName,
			DynamicInputScript->GetExposedVersion().VersionGuid);
		if (!DynamicInputNode)
		{
			AddWarning(Warnings, TEXT("dynamic_input_create_failed"), ContextPath, DynamicInputScriptPath);
			return true;
		}

		FString DynamicInputInitializationSource;
		const bool bDynamicInputInputsInitialized = UeAgentNiagaraOps::InitializeDynamicInputStackInputs(
			nullptr,
			FGuid(),
			&Graph,
			ModuleNode,
			*DynamicInputNode,
			DynamicInputInitializationSource);
		if (!bDynamicInputInputsInitialized)
		{
			AddWarning(
				Warnings,
				TEXT("dynamic_input_inputs_initialization_incomplete"),
				ContextPath,
				DynamicInputInitializationSource);
		}

		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		if (UeAgentJsonDiagnostics::ReadArrayField(DynamicInputObj, TEXT("inputs"), ContextPath + TEXT(".inputs"), Inputs, Warnings, false))
		{
			for (int32 InputIndex = 0; InputIndex < Inputs->Num(); ++InputIndex)
			{
				TSharedPtr<FJsonObject> NestedInputObj;
				const FString NestedInputPath = FString::Printf(TEXT("%s.inputs[%d]"), *ContextPath, InputIndex);
				if (UeAgentJsonDiagnostics::ReadObjectFromValue((*Inputs)[InputIndex], NestedInputPath, NestedInputObj, Warnings))
				{
					ApplyModuleInputDefault(Graph, *DynamicInputNode, NestedInputObj, NestedInputPath, Warnings);
				}
			}
		}

		ApplyDynamicInputDataInterfacesFromJson(Graph, *DynamicInputNode, DynamicInputObj, ContextPath, Warnings);
		Graph.NotifyGraphChanged();
		return true;
	}

	static bool ApplyModuleInputDefault(UNiagaraGraph& Graph, UNiagaraNodeFunctionCall& ModuleNode, const TSharedPtr<FJsonObject>& InputObj, const FString& ContextPath, TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(
			InputObj,
			ContextPath,
			{
				TEXT("input_name"),
				TEXT("input_short_name"),
				TEXT("input_type"),
				TEXT("has_override"),
				TEXT("has_links"),
				TEXT("override_default_value"),
				TEXT("autogenerated_default_value"),
				TEXT("has_visible_pin"),
				TEXT("enum_type"),
				TEXT("enum_value_name"),
				TEXT("enum_value_display_name"),
				TEXT("enum_value_int"),
				TEXT("override_enum_value_name"),
				TEXT("override_enum_value_display_name"),
				TEXT("override_enum_value_int"),
				TEXT("autogenerated_enum_value_name"),
				TEXT("autogenerated_enum_value_display_name"),
				TEXT("autogenerated_enum_value_int"),
				TEXT("enum_options"),
				TEXT("link_kind"),
				TEXT("dynamic_input")
			},
			Warnings);

		TSharedPtr<FJsonObject> DynamicInputObj;
		const bool bHasDynamicInput = GetObjectField(InputObj, TEXT("dynamic_input"), DynamicInputObj);

		bool bHasOverride = false;
		const bool bReadHasOverride = UeAgentJsonDiagnostics::ReadBoolField(
			InputObj,
			TEXT("has_override"),
			ContextPath + TEXT(".has_override"),
			bHasOverride,
			Warnings,
			false);

		FString ValueText;
		const bool bReadOverrideDefaultValue = UeAgentJsonDiagnostics::ReadStringField(
			InputObj,
			TEXT("override_default_value"),
			ContextPath + TEXT(".override_default_value"),
			ValueText,
			Warnings,
			false);

		if (!bReadHasOverride && (bHasDynamicInput || (bReadOverrideDefaultValue && !ValueText.TrimStartAndEnd().IsEmpty())))
		{
			bHasOverride = true;
			if (!bHasDynamicInput)
			{
				AddWarning(
					Warnings,
					TEXT("module_input_has_override_missing_assumed_from_value"),
					ContextPath,
					TEXT("has_override is missing, but override_default_value is present and non-empty; applying it as an override."));
			}
		}

		if (!bHasOverride)
		{
			return true;
		}

		if (!bReadOverrideDefaultValue && !bHasDynamicInput)
		{
			AddWarning(
				Warnings,
				TEXT("module_input_missing_override_default_value"),
				ContextPath + TEXT(".override_default_value"),
				TEXT("has_override is true, but override_default_value is missing or not a string; the module input override was skipped."));
			return true;
		}

		bool bHasLinks = false;
		UeAgentJsonDiagnostics::ReadBoolField(
			InputObj,
			TEXT("has_links"),
			ContextPath + TEXT(".has_links"),
			bHasLinks,
			Warnings,
			false);
		if (bHasLinks && !bHasDynamicInput)
		{
			return true;
		}

		FString InputName;
		if (!UeAgentJsonDiagnostics::ReadStringField(InputObj, TEXT("input_name"), ContextPath + TEXT(".input_name"), InputName, Warnings, false))
		{
			UeAgentJsonDiagnostics::ReadStringField(InputObj, TEXT("input_short_name"), ContextPath + TEXT(".input_short_name"), InputName, Warnings, false);
		}
		if (InputName.IsEmpty())
		{
			AddWarning(
				Warnings,
				TEXT("module_input_missing_input_name"),
				ContextPath,
				TEXT("Module input has override data but no input_name/input_short_name; the override was skipped."));
			return true;
		}

		TArray<UeAgentNiagaraOps::FResolvedNiagaraModuleInput> ModuleInputs;
		UeAgentNiagaraOps::BuildResolvedModuleInputs(ModuleNode, ModuleInputs);
		FString ResolvedInputName;
		const UeAgentNiagaraOps::FResolvedNiagaraModuleInput* ResolvedInput =
			UeAgentNiagaraOps::ResolveModuleInputByName(ModuleInputs, InputName, ResolvedInputName);
		if (!ResolvedInput || !ResolvedInput->InputVariable.IsValid())
		{
			AddWarning(Warnings, TEXT("module_input_not_found"), ContextPath + TEXT(".input_name"), FString::Printf(TEXT("Module input '%s' listed in folder could not be resolved on the target module."), *InputName));
			return true;
		}

		bool bFolderSaidVisible = false;
		const bool bReadHasVisiblePin = UeAgentJsonDiagnostics::ReadBoolField(
			InputObj,
			TEXT("has_visible_pin"),
			ContextPath + TEXT(".has_visible_pin"),
			bFolderSaidVisible,
			Warnings,
			false);
		if (!ResolvedInput->VisiblePin)
		{
			AddWarning(
				Warnings,
				TEXT("module_input_hidden_or_inactive_branch"),
				ContextPath,
				FString::Printf(
					TEXT("Module input '%s' is not visible in the active Niagara stack branch while applying. The override may be stored on a hidden/static-switch branch and ignored by the UI/effective module state; set the controlling mode input first, re-export, then apply the branch value."),
					*ResolvedInputName));
		}
		else if (bReadHasVisiblePin && !bFolderSaidVisible)
		{
			AddWarning(
				Warnings,
				TEXT("module_input_visibility_changed_since_export"),
				ContextPath,
				FString::Printf(TEXT("Module input '%s' was exported as hidden but is currently visible; verify the controlling enum/static-switch value before trusting the imported default."), *ResolvedInputName));
		}

		const FNiagaraParameterHandle ModuleInputHandle(ResolvedInput->InputVariable.GetName());
		const FNiagaraParameterHandle AliasedInputHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleInputHandle, &ModuleNode);
		UEdGraphPin* OverridePin = ResolvedInput->VisiblePin;
		if (OverridePin)
		{
			if (UEdGraphNode* OverrideOwnerNode = OverridePin->GetOwningNode())
			{
				OverrideOwnerNode->Modify();
			}
		}
		else
		{
			FString OverridePinError;
			if (!UeAgentNiagaraOps::TryGetOrCreateStackFunctionInputOverridePin(
				ModuleNode,
				AliasedInputHandle,
				ResolvedInput->InputVariable.GetType(),
				ResolvedInput->InputScriptVariableGuid,
				OverridePin,
				OverridePinError) || !OverridePin)
			{
				AddWarning(Warnings, TEXT("module_input_override_unavailable"), ContextPath, FString::Printf(TEXT("%s: %s"), *ResolvedInputName, *OverridePinError));
				return true;
			}
		}

		if (bHasDynamicInput)
		{
			return ApplyModuleInputDynamicInput(Graph, ModuleNode, *OverridePin, DynamicInputObj, ContextPath + TEXT(".dynamic_input"), Warnings);
		}

		if (OverridePin->LinkedTo.Num() > 0)
		{
			OverridePin->BreakAllPinLinks();
		}

		FString ValueTextForApply = ValueText;
		FString NormalizeError;
		if (!UeAgentNiagaraOps::NormalizeNiagaraValueTextForPinDefault(ResolvedInput->InputVariable.GetType(), ValueText, ValueTextForApply, NormalizeError))
		{
			AddWarning(
				Warnings,
				TEXT("module_input_invalid_value_text"),
				ContextPath + TEXT(".override_default_value"),
				FString::Printf(TEXT("%s: %s; requested=%s"), *ResolvedInputName, *NormalizeError, *ValueText));
			return true;
		}
		if (const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>())
		{
			NiagaraSchema->TrySetDefaultValue(*OverridePin, ValueTextForApply, true);
		}
		else
		{
			OverridePin->DefaultValue = ValueTextForApply;
		}

		FString ApplyVerificationStatus;
		FString ApplyVerificationError;
		if (!UeAgentNiagaraOps::ValidateNiagaraPinDefaultValueAfterSet(ResolvedInput->InputVariable.GetType(), ValueTextForApply, OverridePin->DefaultValue, ApplyVerificationStatus, ApplyVerificationError))
		{
			AddWarning(
				Warnings,
				TEXT("module_input_default_apply_verification_failed"),
				ContextPath + TEXT(".override_default_value"),
				FString::Printf(
					TEXT("%s: %s; requested=%s; applied=%s; stored=%s"),
					*ResolvedInputName,
					*ApplyVerificationError,
					*ValueText,
					*ValueTextForApply,
					*OverridePin->DefaultValue));
			return true;
		}

		TArray<UeAgentNiagaraOps::FResolvedNiagaraModuleInput> ModuleInputsAfter;
		UeAgentNiagaraOps::BuildResolvedModuleInputs(ModuleNode, ModuleInputsAfter);
		FString ResolvedInputNameAfter;
		const UeAgentNiagaraOps::FResolvedNiagaraModuleInput* ResolvedInputAfter =
			UeAgentNiagaraOps::ResolveModuleInputByName(ModuleInputsAfter, ResolvedInputName, ResolvedInputNameAfter);
		if (!ResolvedInputAfter || !ResolvedInputAfter->InputVariable.IsValid())
		{
			AddWarning(
				Warnings,
				TEXT("module_input_readback_missing_after_apply"),
				ContextPath,
				FString::Printf(TEXT("Module input '%s' could not be resolved after applying its override."), *ResolvedInputName));
		}
		return true;
	}

	static int32 ApplyStagesFromFolder(UNiagaraEmitter& Emitter, FVersionedNiagaraEmitterData& EmitterData, const FString& EmitterFolder, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData.GraphSource);
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (!Graph)
		{
			AddWarning(Warnings, TEXT("stages_missing_graph"), EmitterFolder, TEXT("Emitter graph source is missing; stages cannot be applied."));
			return 0;
		}

		TSharedPtr<FJsonObject> StagesIndexObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("stages"), TEXT("index.json")), StagesIndexObj, OutError))
		{
			return INDEX_NONE;
		}
		if (!StagesIndexObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Stages = nullptr;
		if (!UeAgentJsonDiagnostics::ReadArrayField(StagesIndexObj, TEXT("stages"), TEXT("stages/index.json.stages"), Stages, Warnings, false))
		{
			return 0;
		}

		int32 Applied = 0;
		int32 StageIndex = 0;
		for (const TSharedPtr<FJsonValue>& StageIndexValue : *Stages)
		{
			TSharedPtr<FJsonObject> StageIndexObj;
			const FString StageIndexPath = FString::Printf(TEXT("stages/index.json.stages[%d]"), StageIndex);
			++StageIndex;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue(StageIndexValue, StageIndexPath, StageIndexObj, Warnings))
			{
				continue;
			}

			FString StageFile;
			if (!UeAgentJsonDiagnostics::ReadStringField(StageIndexObj, TEXT("file"), StageIndexPath + TEXT(".file"), StageFile, Warnings, true) || StageFile.IsEmpty())
			{
				AddWarning(Warnings, TEXT("stage_index_missing_file"), StageIndexPath, TEXT("Stage index entry has no file; it was skipped."));
				continue;
			}

			TSharedPtr<FJsonObject> StageObj;
			if (!LoadJsonFile(FPaths::Combine(EmitterFolder, TEXT("stages"), StageFile), StageObj, OutError))
			{
				return INDEX_NONE;
			}
			const FString StagePath = FString::Printf(TEXT("stages/%s"), *StageFile);
			UeAgentJsonDiagnostics::WarnUnknownFields(
				StageObj,
				StagePath,
				{
					TEXT("file"),
					TEXT("stage_index"),
					TEXT("stage_node_index"),
					TEXT("stage_key"),
					TEXT("stage_type"),
					TEXT("script_usage"),
					TEXT("script_usage_id"),
					TEXT("output_node_guid"),
					TEXT("node_guid"),
					TEXT("node_class"),
					TEXT("node_title"),
					TEXT("node_pos_x"),
					TEXT("node_pos_y"),
					TEXT("is_output_node"),
					TEXT("is_module_node"),
					TEXT("module_name"),
					TEXT("module_script_path"),
					TEXT("module_kind"),
					TEXT("module_has_enabled_state"),
					TEXT("module_enabled"),
					TEXT("input_count"),
					TEXT("inputs"),
					TEXT("modules"),
					TEXT("module_count"),
					TEXT("graph_nodes"),
					TEXT("graph_node_count"),
					TEXT("graph_links"),
					TEXT("graph_link_count"),
					TEXT("custom_hlsl_nodes"),
					TEXT("custom_hlsl_node_count"),
					TEXT("simulation_stage_raw_properties"),
					TEXT("event_handler_raw_properties")
				},
				Warnings);

			ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
			FGuid UsageId;
			FString StageKey;
			if (StageObj->TryGetStringField(TEXT("stage_key"), StageKey) && !StageKey.IsEmpty())
			{
				UeAgentNiagaraOps::TryParseStageKey(StageKey, Usage, UsageId);
			}
			if (Usage == ENiagaraScriptUsage::Function)
			{
				FString UsageText;
				StageObj->TryGetStringField(TEXT("script_usage"), UsageText);
				UeAgentNiagaraOps::TryParseScriptUsage(UsageText, Usage);
				FString UsageIdText;
				StageObj->TryGetStringField(TEXT("script_usage_id"), UsageIdText);
				FGuid::Parse(UsageIdText, UsageId);
			}
			if (Usage == ENiagaraScriptUsage::Function)
			{
				AddWarning(Warnings, TEXT("stage_usage_unresolved"), StageFile, TEXT("Stage script usage could not be resolved."));
				continue;
			}

			const FString StageType = GetStringFieldAny(StageObj, { TEXT("stage_type") });
			if (StageType.Equals(TEXT("simulation_stage"), ESearchCase::IgnoreCase))
			{
				UNiagaraSimulationStageBase* SimulationStage = EnsureSimulationStageForFolder(Emitter, EmitterData, *Graph, StageObj, Warnings, OutError);
				if (!SimulationStage)
				{
					return INDEX_NONE;
				}
				ApplyObjectPropertyArray(SimulationStage, StageObj, TEXT("simulation_stage_raw_properties"), Warnings, SimulationStage->GetPathName());
				if (SimulationStage->Script)
				{
					UsageId = SimulationStage->Script->GetUsageId();
				}
			}
			else if (StageType.Equals(TEXT("event_stage"), ESearchCase::IgnoreCase))
			{
				if (!UsageId.IsValid())
				{
					UsageId = FGuid::NewGuid();
				}
				bool bHasEventHandler = false;
				for (const FNiagaraEventScriptProperties& EventHandler : EmitterData.EventHandlerScriptProps)
				{
					if (EventHandler.Script && EventHandler.Script->GetUsageId() == UsageId)
					{
						bHasEventHandler = true;
						break;
					}
				}
				if (!bHasEventHandler)
				{
					TSharedPtr<FJsonObject> MinimalEventObj = MakeShared<FJsonObject>();
					MinimalEventObj->SetStringField(TEXT("script_usage_id"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
					MinimalEventObj->SetStringField(TEXT("source_event_name"), TEXT("Event"));
					FNiagaraEventScriptProperties* EventHandler = nullptr;
					int32 EventIndex = INDEX_NONE;
					if (!EnsureEventHandlerForFolder(Emitter, EmitterData, *Graph, MinimalEventObj, EventHandler, EventIndex, Warnings, OutError))
					{
						return INDEX_NONE;
					}
				}
			}

			UeAgentNiagaraOps::DestroyGraphNodesForStage(*Graph, Usage, UsageId);
			UNiagaraNodeOutput* OutputNode = UeAgentNiagaraOps::EnsureStageInputOutputBridge(*Graph, Usage, UsageId);
			if (!OutputNode)
			{
				AddWarning(Warnings, TEXT("stage_output_bridge_failed"), StageFile, TEXT("Stage output bridge could not be initialized."));
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
			if (UeAgentJsonDiagnostics::ReadArrayField(StageObj, TEXT("modules"), StagePath + TEXT(".modules"), Modules, Warnings, false))
			{
				TArray<TSharedPtr<FJsonValue>> SortedModules = *Modules;
				SortedModules.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
				{
					const TSharedPtr<FJsonObject>* AObj = nullptr;
					const TSharedPtr<FJsonObject>* BObj = nullptr;
					double AIndex = 0.0;
					double BIndex = 0.0;
					if (A.IsValid() && A->TryGetObject(AObj) && AObj && AObj->IsValid())
					{
						(*AObj)->TryGetNumberField(TEXT("module_index"), AIndex);
					}
					if (B.IsValid() && B->TryGetObject(BObj) && BObj && BObj->IsValid())
					{
						(*BObj)->TryGetNumberField(TEXT("module_index"), BIndex);
					}
					return AIndex < BIndex;
				});

				int32 TargetModuleIndex = 0;
				int32 SourceModuleIndex = 0;
				for (const TSharedPtr<FJsonValue>& ModuleValue : SortedModules)
				{
					TSharedPtr<FJsonObject> ModuleObj;
					const FString ModulePath = FString::Printf(TEXT("stages/%s.modules[%d]"), *StageFile, SourceModuleIndex);
					++SourceModuleIndex;
					if (!UeAgentJsonDiagnostics::ReadObjectFromValue(ModuleValue, ModulePath, ModuleObj, Warnings))
					{
						continue;
					}

					UeAgentJsonDiagnostics::WarnUnknownFields(
						ModuleObj,
						ModulePath,
						{
							TEXT("module_index"),
							TEXT("module_node_guid"),
							TEXT("module_name"),
							TEXT("module_script_path"),
							TEXT("module_script_asset_path"),
							TEXT("module_kind"),
							TEXT("module_has_enabled_state"),
							TEXT("module_enabled"),
							TEXT("inputs"),
							TEXT("input_count")
						},
						Warnings);

					const FString ModuleScriptPath = GetStringFieldAny(ModuleObj, { TEXT("module_script_path"), TEXT("module_script_asset_path") });
					UNiagaraNodeFunctionCall* AddedModule = nullptr;
					if (IsAssignmentModuleFolderObject(ModuleObj))
					{
						AddedModule = AddAssignmentModuleFromFolder(*OutputNode, TargetModuleIndex, ModuleObj, Warnings);
					}
					else
					{
						if (ModuleScriptPath.IsEmpty())
						{
							AddWarning(Warnings, TEXT("module_missing_script_path"), ModulePath, TEXT("Script module entry has no module_script_path/module_script_asset_path; it was skipped."));
							continue;
						}
						if (IsPrivateInlineNiagaraScriptPath(ModuleScriptPath))
						{
							AddWarning(Warnings, TEXT("private_inline_module_script_skipped"), ModuleScriptPath, TEXT("Private inline module scripts cannot be reused by a standalone emitter without creating illegal package references."));
							continue;
						}

						UNiagaraScript* ModuleScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(ModuleScriptPath));
						if (!ModuleScript)
						{
							AddWarning(Warnings, TEXT("module_script_not_found"), ModuleScriptPath, TEXT("Module could not be reconstructed because the referenced script asset was not found."));
							continue;
						}

						FString SuggestedName;
						ModuleObj->TryGetStringField(TEXT("module_name"), SuggestedName);
						AddedModule = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
							ModuleScript,
							*OutputNode,
							TargetModuleIndex,
							SuggestedName,
							ModuleScript->GetExposedVersion().VersionGuid);
					}
					if (!AddedModule)
					{
						AddWarning(Warnings, TEXT("module_recreate_failed"), ModuleScriptPath, TEXT("Module reconstruction returned null."));
						continue;
					}

					bool bModuleEnabled = true;
					if (ModuleObj->TryGetBoolField(TEXT("module_enabled"), bModuleEnabled))
					{
						FNiagaraStackGraphUtilities::SetModuleIsEnabled(*AddedModule, bModuleEnabled);
					}

					const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
					if (UeAgentJsonDiagnostics::ReadArrayField(ModuleObj, TEXT("inputs"), ModulePath + TEXT(".inputs"), Inputs, Warnings, false))
					{
						int32 InputIndex = 0;
						for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
						{
							TSharedPtr<FJsonObject> InputObj;
							const FString InputPath = FString::Printf(TEXT("%s.inputs[%d]"), *ModulePath, InputIndex);
							++InputIndex;
							if (UeAgentJsonDiagnostics::ReadObjectFromValue(InputValue, InputPath, InputObj, Warnings))
							{
								ApplyModuleInputDefault(*Graph, *AddedModule, InputObj, InputPath, Warnings);
							}
						}
					}
					++Applied;
					++TargetModuleIndex;
				}
			}

			++Applied;
		}
		return Applied;
	}

	static int32 ApplyEmitterFolder(UNiagaraEmitter& Emitter, const FGuid& VersionGuid, const FString& EmitterFolder, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError, const bool bApplyDataInterfacesAfterLocalRefresh = true)
	{
		FVersionedNiagaraEmitterData* EmitterData = VersionGuid.IsValid() ? Emitter.GetEmitterData(VersionGuid) : Emitter.GetLatestEmitterData();
		if (!EmitterData)
		{
			AddWarning(Warnings, TEXT("emitter_apply_missing_version_data"), EmitterFolder, TEXT("Emitter version data is missing."));
			return 0;
		}

		int32 Applied = 0;
		TSharedPtr<FJsonObject> PropertiesObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("properties.json")), PropertiesObj, OutError))
		{
			return INDEX_NONE;
		}
		if (PropertiesObj.IsValid())
		{
			const bool bApplyInterpolatedSpawnSideEffects = JsonPropertyArrayContainsProperty(PropertiesObj, TEXT("raw_properties"), TEXT("InterpolatedSpawnMode"));
			Applied += ApplyStructPropertyArray(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, &Emitter, PropertiesObj, TEXT("raw_properties"), Warnings, Emitter.GetPathName());
			if (bApplyInterpolatedSpawnSideEffects)
			{
				const UeAgentNiagaraOps::FEmitterPropertySideEffectResult SideEffects = UeAgentNiagaraOps::ApplyEmitterPropertySideEffects(EmitterData, TEXT("InterpolatedSpawnMode"));
				if (SideEffects.bHandled)
				{
					++Applied;
				}
			}
		}

		const int32 ParametersApplied = ApplyEmitterGraphParameters(Emitter, *EmitterData, EmitterFolder, Warnings, OutError);
		if (ParametersApplied == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		Applied += ParametersApplied;

		TSharedPtr<FJsonObject> RenderersObj;
		if (!LoadJsonFileOptional(FPaths::Combine(EmitterFolder, TEXT("renderers.json")), RenderersObj, OutError))
		{
			return INDEX_NONE;
		}
		if (RenderersObj.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Renderers = nullptr;
			if (TryGetArrayField(RenderersObj, TEXT("renderers"), Renderers))
			{
				for (const TSharedPtr<FJsonValue>& RendererValue : *Renderers)
				{
					const TSharedPtr<FJsonObject>* RendererObjPtr = nullptr;
					if (!RendererValue.IsValid() || !RendererValue->TryGetObject(RendererObjPtr) || !RendererObjPtr || !RendererObjPtr->IsValid())
					{
						continue;
					}

					const TSharedPtr<FJsonObject>& RendererObj = *RendererObjPtr;
					int32 RendererIndex = INDEX_NONE;
					double RendererIndexNumber = 0.0;
					if (RendererObj->TryGetNumberField(TEXT("renderer_index"), RendererIndexNumber))
					{
						RendererIndex = FMath::RoundToInt(RendererIndexNumber);
					}

					const TArray<UNiagaraRendererProperties*>& RenderersArray = EmitterData->GetRenderers();
					UNiagaraRendererProperties* Renderer = RenderersArray.IsValidIndex(RendererIndex) ? RenderersArray[RendererIndex] : nullptr;
					if (!Renderer)
					{
						FString RendererClassText;
						if (RendererObj->TryGetStringField(TEXT("renderer_class"), RendererClassText))
						{
							UClass* RendererClass = UeAgentNiagaraOps::ResolveRendererClass(RendererClassText);
							if (RendererClass)
							{
								Renderer = NewObject<UNiagaraRendererProperties>(&Emitter, RendererClass, NAME_None, RF_Transactional);
								if (Renderer)
								{
									Emitter.AddRenderer(Renderer, EmitterData->Version.VersionGuid);
									RendererIndex = EmitterData->GetRenderers().Find(Renderer);
								}
							}
						}
					}

					if (!Renderer)
					{
						AddWarning(Warnings, TEXT("renderer_apply_missing_target"), FString::Printf(TEXT("%s[%d]"), *EmitterFolder, RendererIndex), TEXT("Renderer was not found and could not be created."));
						continue;
					}

					Applied += ApplyObjectPropertyArray(Renderer, RendererObj, TEXT("raw_properties"), Warnings, Renderer->GetPathName());
				}
			}
		}

		const int32 EventGeneratorsApplied = ApplyEventGeneratorsFromFolder(Emitter, *EmitterData, EmitterFolder, Warnings, OutError);
		if (EventGeneratorsApplied == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		Applied += EventGeneratorsApplied;

		const int32 PrunedDynamicStages = PruneDynamicStagesMissingFromFolder(Emitter, *EmitterData, EmitterFolder, Warnings, OutError);
		if (PrunedDynamicStages == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		Applied += PrunedDynamicStages;

		const int32 EventHandlersApplied = ApplyEventHandlersFromFolder(Emitter, *EmitterData, EmitterFolder, Warnings, OutError);
		if (EventHandlersApplied == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		Applied += EventHandlersApplied;

		const int32 StagesApplied = ApplyStagesFromFolder(Emitter, *EmitterData, EmitterFolder, Warnings, OutError);
		if (StagesApplied == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		Applied += StagesApplied;

		if (EmitterData->GraphSource)
		{
			EmitterData->GraphSource->MarkNotSynchronized(TEXT("Niagara folder apply"));
		}
		Emitter.UpdateEmitterAfterLoad();

		if (bApplyDataInterfacesAfterLocalRefresh)
		{
			const int32 DataInterfacesApplied = ApplyDataInterfacesFromFolder(Emitter, *EmitterData, EmitterFolder, Warnings, OutError);
			if (DataInterfacesApplied == INDEX_NONE)
			{
				return INDEX_NONE;
			}
			Applied += DataInterfacesApplied;
		}
		Emitter.MarkPackageDirty();
		return Applied;
	}

	static int32 ApplySystemDataInterfacesFromFolder(UNiagaraSystem& NiagaraSystem, const FString& FolderPath, const TSharedPtr<FJsonObject>& EmittersIndexObj, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		if (!EmittersIndexObj.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
		if (!TryGetArrayField(EmittersIndexObj, TEXT("emitters"), Emitters))
		{
			return 0;
		}

		int32 Applied = 0;
		for (int32 EmitterIndex = 0; EmitterIndex < Emitters->Num(); ++EmitterIndex)
		{
			const FString EmitterPath = FString::Printf(TEXT("%s/emitters/index.json.emitters[%d]"), *FolderPath, EmitterIndex);
			TSharedPtr<FJsonObject> EmitterObj;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Emitters)[EmitterIndex], EmitterPath, EmitterObj, Warnings))
			{
				continue;
			}

			const FString EmitterName = GetStringFieldAny(EmitterObj, { TEXT("emitter_name"), TEXT("name") }, FString::Printf(TEXT("Emitter_%d"), EmitterIndex));
			FString EmitterFolderRelative;
			EmitterObj->TryGetStringField(TEXT("folder"), EmitterFolderRelative);
			if (EmitterName.IsEmpty() || EmitterFolderRelative.IsEmpty())
			{
				continue;
			}

			FNiagaraEmitterHandle* ExistingHandle = nullptr;
			for (FNiagaraEmitterHandle& Handle : NiagaraSystem.GetEmitterHandles())
			{
				if (Handle.GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
				{
					ExistingHandle = &Handle;
					break;
				}
			}
			if (!ExistingHandle)
			{
				AddWarning(Warnings, TEXT("data_interface_emitter_handle_not_found"), EmitterPath, FString::Printf(TEXT("Emitter handle '%s' was not found while applying post-refresh data interfaces."), *EmitterName));
				continue;
			}

			FVersionedNiagaraEmitter VersionedEmitter = ExistingHandle->GetInstance();
			if (!VersionedEmitter.Emitter)
			{
				AddWarning(Warnings, TEXT("data_interface_emitter_instance_missing"), EmitterPath, FString::Printf(TEXT("Emitter handle '%s' has no emitter instance while applying post-refresh data interfaces."), *EmitterName));
				continue;
			}

			FVersionedNiagaraEmitterData* EmitterData = VersionedEmitter.Version.IsValid()
				? VersionedEmitter.Emitter->GetEmitterData(VersionedEmitter.Version)
				: VersionedEmitter.Emitter->GetLatestEmitterData();
			if (!EmitterData)
			{
				AddWarning(Warnings, TEXT("data_interface_emitter_data_missing"), EmitterPath, FString::Printf(TEXT("Emitter handle '%s' has no version data while applying post-refresh data interfaces."), *EmitterName));
				continue;
			}

			const FString EmitterFolder = FPaths::Combine(FolderPath, TEXT("emitters"), EmitterFolderRelative);
			const int32 DataInterfacesApplied = ApplyDataInterfacesFromFolder(*VersionedEmitter.Emitter, *EmitterData, EmitterFolder, Warnings, OutError);
			if (DataInterfacesApplied == INDEX_NONE)
			{
				return INDEX_NONE;
			}
			Applied += DataInterfacesApplied;
			if (DataInterfacesApplied > 0)
			{
				VersionedEmitter.Emitter->MarkPackageDirty();
			}
		}

		return Applied;
	}

	static bool ResolveSystemStageUsageFromJson(const TSharedPtr<FJsonObject>& StageObj, const FString& StageName, ENiagaraScriptUsage& OutUsage, FGuid& OutUsageId)
	{
		OutUsage = ENiagaraScriptUsage::Function;
		OutUsageId.Invalidate();

		if (StageObj.IsValid())
		{
			FString StageKey;
			if (StageObj->TryGetStringField(TEXT("stage_key"), StageKey) && !StageKey.IsEmpty())
			{
				UeAgentNiagaraOps::TryParseStageKey(StageKey, OutUsage, OutUsageId);
			}
			if (!UeAgentNiagaraOps::IsSystemStageUsage(OutUsage))
			{
				FString UsageText;
				if (StageObj->TryGetStringField(TEXT("script_usage"), UsageText))
				{
					UeAgentNiagaraOps::TryParseScriptUsage(UsageText, OutUsage);
				}
				FString UsageIdText;
				if (StageObj->TryGetStringField(TEXT("script_usage_id"), UsageIdText))
				{
					FGuid::Parse(UsageIdText, OutUsageId);
				}
			}
		}

		if (!UeAgentNiagaraOps::IsSystemStageUsage(OutUsage))
		{
			if (StageName.Equals(TEXT("SystemSpawn"), ESearchCase::IgnoreCase))
			{
				OutUsage = ENiagaraScriptUsage::SystemSpawnScript;
			}
			else if (StageName.Equals(TEXT("SystemUpdate"), ESearchCase::IgnoreCase))
			{
				OutUsage = ENiagaraScriptUsage::SystemUpdateScript;
			}
		}

		if (!OutUsageId.IsValid())
		{
			OutUsageId = FGuid();
		}
		return UeAgentNiagaraOps::IsSystemStageUsage(OutUsage);
	}

	static int32 ApplySystemStageJsonToGraph(UNiagaraGraph& Graph, const TSharedPtr<FJsonObject>& StageObj, const FString& StageName, const FString& ContextPath, TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		if (!StageObj.IsValid())
		{
			return 0;
		}

		UeAgentJsonDiagnostics::WarnUnknownFields(
			StageObj,
			ContextPath,
			{
				TEXT("stage_name"),
				TEXT("stage_index"),
				TEXT("stage_key"),
				TEXT("stage_type"),
				TEXT("script_usage"),
				TEXT("script_usage_id"),
				TEXT("output_node_guid"),
				TEXT("script"),
				TEXT("node_guid"),
				TEXT("node_class"),
				TEXT("node_title"),
				TEXT("node_pos_x"),
				TEXT("node_pos_y"),
				TEXT("is_output_node"),
				TEXT("is_module_node"),
				TEXT("module_name"),
				TEXT("module_script_path"),
				TEXT("module_kind"),
				TEXT("module_has_enabled_state"),
				TEXT("module_enabled"),
				TEXT("input_count"),
				TEXT("inputs"),
				TEXT("modules"),
				TEXT("module_count"),
				TEXT("graph_nodes"),
				TEXT("graph_node_count"),
				TEXT("graph_links"),
				TEXT("graph_link_count"),
				TEXT("custom_hlsl_nodes"),
				TEXT("custom_hlsl_node_count"),
				TEXT("coverage_note")
			},
			Warnings);

		const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
		if (!UeAgentJsonDiagnostics::ReadArrayField(StageObj, TEXT("modules"), ContextPath + TEXT(".modules"), Modules, Warnings, false))
		{
			return 0;
		}

		ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
		FGuid UsageId;
		if (!ResolveSystemStageUsageFromJson(StageObj, StageName, Usage, UsageId))
		{
			AddWarning(Warnings, TEXT("system_stage_usage_unresolved"), ContextPath, TEXT("System stage usage could not be resolved; stage was skipped."));
			return 0;
		}

		UeAgentNiagaraOps::DestroyGraphNodesForStage(Graph, Usage, UsageId);
		UNiagaraNodeOutput* OutputNode = UeAgentNiagaraOps::EnsureStageInputOutputBridge(Graph, Usage, UsageId);
		if (!OutputNode)
		{
			AddWarning(Warnings, TEXT("system_stage_output_bridge_failed"), ContextPath, TEXT("System stage output bridge could not be initialized."));
			return 0;
		}

		TArray<TSharedPtr<FJsonValue>> SortedModules = *Modules;
		SortedModules.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const TSharedPtr<FJsonObject>* AObj = nullptr;
			const TSharedPtr<FJsonObject>* BObj = nullptr;
			double AIndex = 0.0;
			double BIndex = 0.0;
			if (A.IsValid() && A->TryGetObject(AObj) && AObj && AObj->IsValid())
			{
				(*AObj)->TryGetNumberField(TEXT("module_index"), AIndex);
			}
			if (B.IsValid() && B->TryGetObject(BObj) && BObj && BObj->IsValid())
			{
				(*BObj)->TryGetNumberField(TEXT("module_index"), BIndex);
			}
			return AIndex < BIndex;
		});

		int32 Applied = 0;
		int32 TargetModuleIndex = 0;
		int32 SourceModuleIndex = 0;
		for (const TSharedPtr<FJsonValue>& ModuleValue : SortedModules)
		{
			TSharedPtr<FJsonObject> ModuleObj;
			const FString ModulePath = FString::Printf(TEXT("%s.modules[%d]"), *ContextPath, SourceModuleIndex);
			++SourceModuleIndex;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue(ModuleValue, ModulePath, ModuleObj, Warnings))
			{
				continue;
			}

			UeAgentJsonDiagnostics::WarnUnknownFields(
				ModuleObj,
				ModulePath,
				{
					TEXT("module_index"),
					TEXT("module_node_guid"),
					TEXT("module_name"),
					TEXT("module_script_path"),
					TEXT("module_script_asset_path"),
					TEXT("module_kind"),
					TEXT("module_has_enabled_state"),
					TEXT("module_enabled"),
					TEXT("inputs"),
					TEXT("input_count")
				},
				Warnings);

			const FString ModuleScriptPath = GetStringFieldAny(ModuleObj, { TEXT("module_script_path"), TEXT("module_script_asset_path") });
			UNiagaraNodeFunctionCall* AddedModule = nullptr;
			if (IsAssignmentModuleFolderObject(ModuleObj))
			{
				AddedModule = AddAssignmentModuleFromFolder(*OutputNode, TargetModuleIndex, ModuleObj, Warnings);
			}
			else
			{
				if (ModuleScriptPath.IsEmpty())
				{
					AddWarning(Warnings, TEXT("system_stage_module_missing_script_path"), ModulePath, TEXT("Script module entry has no module_script_path/module_script_asset_path; it was skipped."));
					continue;
				}
				if (IsPrivateInlineNiagaraScriptPath(ModuleScriptPath))
				{
					AddWarning(Warnings, TEXT("system_stage_private_inline_module_script_skipped"), ModuleScriptPath, TEXT("Private inline module scripts cannot be reused safely in a system stage folder apply."));
					continue;
				}

				UNiagaraScript* ModuleScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(ModuleScriptPath));
				if (!ModuleScript)
				{
					AddWarning(Warnings, TEXT("system_stage_module_script_not_found"), ModuleScriptPath, TEXT("System stage module could not be reconstructed because the referenced script asset was not found."));
					continue;
				}

				FString SuggestedName;
				ModuleObj->TryGetStringField(TEXT("module_name"), SuggestedName);
				AddedModule = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
					ModuleScript,
					*OutputNode,
					TargetModuleIndex,
					SuggestedName,
					ModuleScript->GetExposedVersion().VersionGuid);
			}

			if (!AddedModule)
			{
				AddWarning(Warnings, TEXT("system_stage_module_recreate_failed"), ModuleScriptPath, TEXT("System stage module reconstruction returned null."));
				continue;
			}

			bool bModuleEnabled = true;
			if (ModuleObj->TryGetBoolField(TEXT("module_enabled"), bModuleEnabled))
			{
				FNiagaraStackGraphUtilities::SetModuleIsEnabled(*AddedModule, bModuleEnabled);
			}

			const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
			if (UeAgentJsonDiagnostics::ReadArrayField(ModuleObj, TEXT("inputs"), ModulePath + TEXT(".inputs"), Inputs, Warnings, false))
			{
				int32 InputIndex = 0;
				for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
				{
					TSharedPtr<FJsonObject> InputObj;
					const FString InputPath = FString::Printf(TEXT("%s.inputs[%d]"), *ModulePath, InputIndex);
					++InputIndex;
					if (UeAgentJsonDiagnostics::ReadObjectFromValue(InputValue, InputPath, InputObj, Warnings))
					{
						ApplyModuleInputDefault(Graph, *AddedModule, InputObj, InputPath, Warnings);
					}
				}
			}

			++Applied;
			++TargetModuleIndex;
		}

		Graph.NotifyGraphChanged();
		return Applied;
	}

	static int32 ApplySystemStagesFromFolder(UNiagaraSystem& NiagaraSystem, const FString& FolderPath, TArray<TSharedPtr<FJsonValue>>& Warnings, FString& OutError)
	{
		UNiagaraScript* SystemSpawnScript = NiagaraSystem.GetSystemSpawnScript();
		UNiagaraScriptSource* ScriptSource = SystemSpawnScript ? Cast<UNiagaraScriptSource>(SystemSpawnScript->GetLatestSource()) : nullptr;
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (!Graph)
		{
			AddWarning(Warnings, TEXT("system_stages_missing_graph"), FolderPath, TEXT("System graph source is missing; system stages cannot be applied."));
			return 0;
		}

		int32 Applied = 0;
		const TArray<TPair<FString, FString>> StageFiles = {
			TPair<FString, FString>(TEXT("SystemSpawn"), TEXT("SystemSpawn.json")),
			TPair<FString, FString>(TEXT("SystemUpdate"), TEXT("SystemUpdate.json"))
		};
		for (const TPair<FString, FString>& StageFile : StageFiles)
		{
			TSharedPtr<FJsonObject> StageObj;
			const FString StageFilePath = FPaths::Combine(FolderPath, TEXT("system_stages"), StageFile.Value);
			if (!LoadJsonFileOptional(StageFilePath, StageObj, OutError))
			{
				return INDEX_NONE;
			}
			if (!StageObj.IsValid())
			{
				continue;
			}
			Applied += ApplySystemStageJsonToGraph(*Graph, StageObj, StageFile.Key, FString::Printf(TEXT("system_stages/%s"), *StageFile.Value), Warnings);
		}

		if (Applied > 0)
		{
			NiagaraSystem.MarkPackageDirty();
		}
		return Applied;
	}
}
