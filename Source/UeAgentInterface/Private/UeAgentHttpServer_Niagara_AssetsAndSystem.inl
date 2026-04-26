bool FUeAgentHttpServer::CmdNiagaraListAssets(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString RootPath = TEXT("/Game");
	JsonTryGetString(Ctx.Params, TEXT("root_path"), RootPath);
	RootPath.TrimStartAndEndInline();
	if (RootPath.IsEmpty())
	{
		RootPath = TEXT("/Game");
	}
	if (!RootPath.StartsWith(TEXT("/")))
	{
		RootPath = TEXT("/") + RootPath;
	}
	if (!FPackageName::IsValidPath(RootPath))
	{
		OutError = TEXT("invalid_root_path");
		return false;
	}

	int32 Limit = 200;
	double LimitNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("limit"), LimitNumber))
	{
		Limit = FMath::Clamp(FMath::RoundToInt(LimitNumber), 1, 5000);
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
	Filter.ClassPaths.Add(UNiagaraEmitter::StaticClass()->GetClassPathName());
	Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(*RootPath);
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(FMath::Min(AssetList.Num(), Limit));
	for (const FAssetData& AssetData : AssetList)
	{
		if (Items.Num() >= Limit)
		{
			break;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Item->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		Item->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		Item->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Item->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutData->SetStringField(TEXT("root_path"), RootPath);
	OutData->SetNumberField(TEXT("total_count"), AssetList.Num());
	OutData->SetNumberField(TEXT("returned_count"), Items.Num());
	OutData->SetArrayField(TEXT("items"), Items);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraListModuleLibrary(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString RootPath = TEXT("/Niagara/Modules");
	JsonTryGetString(Ctx.Params, TEXT("root_path"), RootPath);
	RootPath.TrimStartAndEndInline();
	if (RootPath.IsEmpty())
	{
		RootPath = TEXT("/Niagara/Modules");
	}
	if (!RootPath.StartsWith(TEXT("/")))
	{
		RootPath = TEXT("/") + RootPath;
	}
	if (!FPackageName::IsValidPath(RootPath))
	{
		OutError = TEXT("invalid_root_path");
		return false;
	}

	FString CategoryFilter;
	JsonTryGetString(Ctx.Params, TEXT("category"), CategoryFilter);
	CategoryFilter.TrimStartAndEndInline();

	FString NameContains;
	JsonTryGetString(Ctx.Params, TEXT("name_contains"), NameContains);
	if (NameContains.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("search"), NameContains);
	}
	NameContains.TrimStartAndEndInline();

	int32 Limit = 5000;
	double LimitNumber = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("limit"), LimitNumber))
	{
		JsonTryGetNumber(Ctx.Params, TEXT("max_results"), LimitNumber);
	}
	if (LimitNumber > 0.0)
	{
		Limit = FMath::Clamp(FMath::RoundToInt(LimitNumber), 1, 20000);
	}

	bool bIncludeNonModuleScripts = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_non_module_scripts"), bIncludeNonModuleScripts);

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(*RootPath);
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);
	AssetList.Sort([](const FAssetData& A, const FAssetData& B)
	{
		const int32 PackagePathCompare = A.PackagePath.ToString().Compare(B.PackagePath.ToString(), ESearchCase::IgnoreCase);
		if (PackagePathCompare != 0)
		{
			return PackagePathCompare < 0;
		}
		return A.AssetName.ToString().Compare(B.AssetName.ToString(), ESearchCase::IgnoreCase) < 0;
	});

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(FMath::Min(AssetList.Num(), Limit));
	int32 MatchedCount = 0;
	for (const FAssetData& AssetData : AssetList)
	{
		if (!NameContains.IsEmpty() && !AssetData.AssetName.ToString().Contains(NameContains, ESearchCase::IgnoreCase))
		{
			continue;
		}

		UNiagaraScript* NiagaraScript = Cast<UNiagaraScript>(AssetData.GetAsset());
		if (!NiagaraScript)
		{
			continue;
		}
		if (!bIncludeNonModuleScripts && !NiagaraScript->IsModuleScript())
		{
			continue;
		}

		const FString PackagePath = AssetData.PackagePath.ToString();
		const FString RelativePackagePath = UeAgentNiagaraOps::MakeRelativePackagePath(PackagePath, RootPath);
		const FString Category = UeAgentNiagaraOps::MakeModuleCategory(RelativePackagePath);
		if (!CategoryFilter.IsEmpty() && !Category.Equals(CategoryFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		++MatchedCount;
		if (Items.Num() >= Limit)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Item->SetStringField(TEXT("asset_path"), AssetData.PackageName.ToString());
		Item->SetStringField(TEXT("package_path"), PackagePath);
		Item->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Item->SetStringField(TEXT("category"), Category);
		Item->SetStringField(TEXT("relative_package_path"), RelativePackagePath);
		Item->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(NiagaraScript->GetUsage()));
		Item->SetBoolField(TEXT("is_module_script"), NiagaraScript->IsModuleScript());

		if (const FVersionedNiagaraScriptData* ScriptData = NiagaraScript->GetLatestScriptData())
		{
			Item->SetArrayField(TEXT("supported_usages"), UeAgentNiagaraOps::BuildScriptUsageJsonArray(ScriptData->GetSupportedUsageContexts()));
			Item->SetStringField(TEXT("library_visibility"), UeAgentNiagaraOps::ScriptLibraryVisibilityToString(ScriptData->LibraryVisibility));
			Item->SetNumberField(TEXT("module_usage_bitmask"), ScriptData->ModuleUsageBitmask);
		}
		else
		{
			Item->SetArrayField(TEXT("supported_usages"), TArray<TSharedPtr<FJsonValue>>());
		}

		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutData->SetStringField(TEXT("root_path"), RootPath);
	OutData->SetStringField(TEXT("category_filter"), CategoryFilter);
	OutData->SetStringField(TEXT("name_contains"), NameContains);
	OutData->SetBoolField(TEXT("include_non_module_scripts"), bIncludeNonModuleScripts);
	OutData->SetNumberField(TEXT("total_count"), AssetList.Num());
	OutData->SetNumberField(TEXT("matched_count"), MatchedCount);
	OutData->SetNumberField(TEXT("returned_count"), Items.Num());
	OutData->SetArrayField(TEXT("items"), Items);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraCreateSystem(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = UeAgentNiagaraOps::NormalizeAssetPath(AssetPathInput);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("invalid_asset_name");
		return false;
	}

	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	bool bFoundStaleObjectBeforeCreate = false;
	const bool bDetachedStaleObjectBeforeCreate = UeAgentNiagaraOps::DetachStaleObjectAtPath(ObjectPath, &bFoundStaleObjectBeforeCreate);
	if (UeAgentNiagaraOps::LoadAssetObject(ObjectPath))
	{
		OutError = TEXT("asset_already_exists");
		return false;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = TEXT("create_package_failed");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = NewObject<UNiagaraSystem>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!NiagaraSystem)
	{
		OutError = TEXT("create_niagara_system_failed");
		return false;
	}

	bool bCreateDefaultNodes = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_default_nodes"), bCreateDefaultNodes);
	UNiagaraSystemFactoryNew::InitializeSystem(NiagaraSystem, bCreateDefaultNodes);

	FAssetRegistryModule::AssetCreated(NiagaraSystem);
	NiagaraSystem->MarkPackageDirty();
	Package->MarkPackageDirty();

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		if (!UeAgentNiagaraOps::CanSafelyOpenNiagaraSystemEditor(*NiagaraSystem, OutError))
		{
			return false;
		}
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->OpenEditorForAsset(NiagaraSystem);
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), PackageName);
	OutData->SetStringField(TEXT("asset_name"), AssetName);
	OutData->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	OutData->SetBoolField(TEXT("created_default_nodes"), bCreateDefaultNodes);
	OutData->SetBoolField(TEXT("found_stale_object_before_create"), bFoundStaleObjectBeforeCreate);
	OutData->SetBoolField(TEXT("detached_stale_object_before_create"), bDetachedStaleObjectBeforeCreate);
	OutData->SetBoolField(TEXT("opened_editor"), bOpenEditor);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraCreateEmitter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = UeAgentNiagaraOps::NormalizeAssetPath(AssetPathInput);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("invalid_asset_name");
		return false;
	}

	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	bool bFoundStaleObjectBeforeCreate = false;
	const bool bDetachedStaleObjectBeforeCreate = UeAgentNiagaraOps::DetachStaleObjectAtPath(ObjectPath, &bFoundStaleObjectBeforeCreate);
	if (UeAgentNiagaraOps::LoadAssetObject(ObjectPath))
	{
		OutError = TEXT("asset_already_exists");
		return false;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = TEXT("create_package_failed");
		return false;
	}

	UNiagaraEmitter* NiagaraEmitter = NewObject<UNiagaraEmitter>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!NiagaraEmitter)
	{
		OutError = TEXT("create_niagara_emitter_failed");
		return false;
	}

	bool bAddDefaultModulesAndRenderers = true;
	JsonTryGetBool(Ctx.Params, TEXT("add_default_modules_and_renderers"), bAddDefaultModulesAndRenderers);
	UNiagaraEmitterFactoryNew::InitializeEmitter(NiagaraEmitter, bAddDefaultModulesAndRenderers);

	FAssetRegistryModule::AssetCreated(NiagaraEmitter);
	NiagaraEmitter->MarkPackageDirty();
	Package->MarkPackageDirty();

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->OpenEditorForAsset(NiagaraEmitter);
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraEmitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), PackageName);
	OutData->SetStringField(TEXT("asset_name"), AssetName);
	OutData->SetStringField(TEXT("object_path"), NiagaraEmitter->GetPathName());
	OutData->SetBoolField(TEXT("added_default_modules_and_renderers"), bAddDefaultModulesAndRenderers);
	OutData->SetBoolField(TEXT("found_stale_object_before_create"), bFoundStaleObjectBeforeCreate);
	OutData->SetBoolField(TEXT("detached_stale_object_before_create"), bDetachedStaleObjectBeforeCreate);
	OutData->SetBoolField(TEXT("opened_editor"), bOpenEditor);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraDeleteAsset(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	if (!Cast<UNiagaraSystem>(Asset) && !Cast<UNiagaraEmitter>(Asset) && !Cast<UNiagaraScript>(Asset))
	{
		OutError = TEXT("asset_is_not_niagara");
		return false;
	}

	bool bForceDelete = false;
	JsonTryGetBool(Ctx.Params, TEXT("force_delete"), bForceDelete);
	bool bUseUncheckedDelete = false;
	JsonTryGetBool(Ctx.Params, TEXT("use_unchecked_delete"), bUseUncheckedDelete);

	const FString DeletedAssetPath = Asset->GetOutermost()->GetName();
	const FString DeletedObjectPath = Asset->GetPathName();
	const FString AssetKind = UeAgentNiagaraOps::GetNiagaraAssetKind(Asset);
	const bool bPackageExistsOnDisk = FPackageName::DoesPackageExist(DeletedAssetPath);
	const bool bUseUncheckedDeletePath = bForceDelete && (bUseUncheckedDelete || !bPackageExistsOnDisk);

	FUeAgentInterfaceLogger::Log(
		TEXT("Niagara DeleteAsset start asset=%s kind=%s force=%s unchecked=%s package_exists=%s"),
		*DeletedObjectPath,
		*AssetKind,
		bForceDelete ? TEXT("true") : TEXT("false"),
		bUseUncheckedDeletePath ? TEXT("true") : TEXT("false"),
		bPackageExistsOnDisk ? TEXT("true") : TEXT("false"));

	bool bDeleted = false;
	bool bFoundResidualObjectAfterDelete = false;
	bool bDetachedResidualObjectAfterDelete = false;
	{
		TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
		if (bForceDelete)
		{
			TArray<UObject*> ObjectsToDelete;
			ObjectsToDelete.Add(Asset);
			bDeleted = bUseUncheckedDeletePath
				? ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete) > 0
				: ObjectTools::ForceDeleteObjects(ObjectsToDelete, false) > 0;
		}
		else
		{
			bDeleted = ObjectTools::DeleteSingleObject(Asset, true);
		}
	}

	if (bDeleted)
	{
		bDetachedResidualObjectAfterDelete = UeAgentNiagaraOps::DetachStaleObjectAtPath(DeletedObjectPath, &bFoundResidualObjectAfterDelete);
	}

	FUeAgentInterfaceLogger::Log(
		TEXT("Niagara DeleteAsset done asset=%s kind=%s deleted=%s strategy=%s residual_found=%s residual_detached=%s"),
		*DeletedObjectPath,
		*AssetKind,
		bDeleted ? TEXT("true") : TEXT("false"),
		bUseUncheckedDeletePath ? TEXT("delete_objects_unchecked") : (bForceDelete ? TEXT("force_delete_objects") : TEXT("delete_single_object")),
		bFoundResidualObjectAfterDelete ? TEXT("true") : TEXT("false"),
		bDetachedResidualObjectAfterDelete ? TEXT("true") : TEXT("false"));

	if (!bDeleted)
	{
		OutError = bForceDelete ? TEXT("force_delete_asset_failed") : TEXT("delete_asset_failed_or_blocked_by_references");
		return false;
	}

	OutData->SetStringField(TEXT("deleted_asset_path"), DeletedAssetPath);
	OutData->SetStringField(TEXT("deleted_object_path"), DeletedObjectPath);
	OutData->SetStringField(TEXT("asset_kind"), AssetKind);
	OutData->SetBoolField(TEXT("force_delete"), bForceDelete);
	OutData->SetBoolField(TEXT("package_exists_on_disk"), bPackageExistsOnDisk);
	OutData->SetBoolField(TEXT("use_unchecked_delete"), bUseUncheckedDeletePath);
	OutData->SetBoolField(TEXT("found_residual_object_after_delete"), bFoundResidualObjectAfterDelete);
	OutData->SetBoolField(TEXT("detached_residual_object_after_delete"), bDetachedResidualObjectAfterDelete);
	OutData->SetStringField(TEXT("delete_strategy"), bUseUncheckedDeletePath ? TEXT("delete_objects_unchecked") : (bForceDelete ? TEXT("force_delete_objects") : TEXT("delete_single_object")));
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraDuplicateAsset(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SourceAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_asset_path"), SourceAssetPathInput) || SourceAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_source_asset_path");
		return false;
	}

	FString TargetAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("target_asset_path"), TargetAssetPathInput) || TargetAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_target_asset_path");
		return false;
	}

	UObject* SourceAsset = UeAgentNiagaraOps::LoadAssetObject(SourceAssetPathInput);
	if (!SourceAsset)
	{
		OutError = TEXT("source_asset_not_found");
		return false;
	}
	if (!Cast<UNiagaraSystem>(SourceAsset) && !Cast<UNiagaraEmitter>(SourceAsset) && !Cast<UNiagaraScript>(SourceAsset))
	{
		OutError = TEXT("source_asset_is_not_niagara");
		return false;
	}

	const FString TargetPackageName = UeAgentNiagaraOps::NormalizeAssetPath(TargetAssetPathInput);
	if (!FPackageName::IsValidLongPackageName(TargetPackageName))
	{
		OutError = TEXT("invalid_target_asset_path");
		return false;
	}
	const FString TargetAssetName = FPackageName::GetLongPackageAssetName(TargetPackageName);
	if (TargetAssetName.IsEmpty())
	{
		OutError = TEXT("invalid_target_asset_name");
		return false;
	}

	const FString TargetObjectPath = FString::Printf(TEXT("%s.%s"), *TargetPackageName, *TargetAssetName);
	bool bFoundStaleTargetObjectBeforeDuplicate = false;
	const bool bDetachedStaleTargetObjectBeforeDuplicate = UeAgentNiagaraOps::DetachStaleObjectAtPath(TargetObjectPath, &bFoundStaleTargetObjectBeforeDuplicate);
	if (UeAgentNiagaraOps::LoadAssetObject(TargetObjectPath))
	{
		OutError = TEXT("target_asset_already_exists");
		return false;
	}

	UPackage* TargetPackage = CreatePackage(*TargetPackageName);
	if (!TargetPackage)
	{
		OutError = TEXT("create_target_package_failed");
		return false;
	}

	UObject* DuplicatedAsset = StaticDuplicateObject(SourceAsset, TargetPackage, *TargetAssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!DuplicatedAsset)
	{
		OutError = TEXT("duplicate_asset_failed");
		return false;
	}

	int32 RepairedEmitterVersionCount = 0;
	if (UNiagaraSystem* DuplicatedSystem = Cast<UNiagaraSystem>(DuplicatedAsset))
	{
		RepairedEmitterVersionCount = UeAgentNiagaraOps::RepairSystemInvalidEmitterVersions(*DuplicatedSystem);
		if (RepairedEmitterVersionCount > 0)
		{
			DuplicatedSystem->Modify();
			DuplicatedSystem->MarkPackageDirty();
		}
	}

	FAssetRegistryModule::AssetCreated(DuplicatedAsset);
	DuplicatedAsset->MarkPackageDirty();
	TargetPackage->MarkPackageDirty();

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->OpenEditorForAsset(DuplicatedAsset);
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(DuplicatedAsset, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("source_asset_path"), SourceAsset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("target_asset_path"), TargetPackageName);
	OutData->SetStringField(TEXT("target_object_path"), DuplicatedAsset->GetPathName());
	OutData->SetStringField(TEXT("asset_kind"), UeAgentNiagaraOps::GetNiagaraAssetKind(DuplicatedAsset));
	OutData->SetNumberField(TEXT("repaired_emitter_version_count"), RepairedEmitterVersionCount);
	OutData->SetBoolField(TEXT("found_stale_target_object_before_duplicate"), bFoundStaleTargetObjectBeforeDuplicate);
	OutData->SetBoolField(TEXT("detached_stale_target_object_before_duplicate"), bDetachedStaleTargetObjectBeforeDuplicate);
	OutData->SetBoolField(TEXT("opened_editor"), bOpenEditor);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	if (!Cast<UNiagaraSystem>(Asset) && !Cast<UNiagaraEmitter>(Asset) && !Cast<UNiagaraScript>(Asset))
	{
		OutError = TEXT("asset_is_not_niagara");
		return false;
	}

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!AssetEditorSubsystem)
	{
		OutError = TEXT("missing_asset_editor_subsystem");
		return false;
	}
	AssetEditorSubsystem->OpenEditorForAsset(Asset);

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("asset_kind"), UeAgentNiagaraOps::GetNiagaraAssetKind(Asset));
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	if (!Settings || !Settings->bEnableScreenshots)
	{
		OutError = TEXT("screenshots_disabled");
		return false;
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	if (!Cast<UNiagaraSystem>(Asset) && !Cast<UNiagaraEmitter>(Asset) && !Cast<UNiagaraScript>(Asset))
	{
		OutError = TEXT("asset_is_not_niagara");
		return false;
	}

	bool bSynchronizedOverviewGraph = false;
	int32 OverviewNodeCountBefore = INDEX_NONE;
	int32 OverviewNodeCountAfter = INDEX_NONE;
	if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset))
	{
		bSynchronizedOverviewGraph = UeAgentNiagaraOps::SynchronizeSystemEditorData(*NiagaraSystem, OverviewNodeCountBefore, OverviewNodeCountAfter);
	}

	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);

	IAssetEditorInstance* AssetEditor = UeAgentNiagaraOps::GetNiagaraAssetEditor(Asset, bOpenEditorIfNeeded, OutError);
	if (!AssetEditor)
	{
		return false;
	}

	FString Target = TEXT("window");
	JsonTryGetString(Ctx.Params, TEXT("target"), Target);
	Target = Target.TrimStartAndEnd().ToLower();

	FString Format = TEXT("png");
	int32 Quality = 95;
	int32 MaxSize = 2048;
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);

	double QualityNum = static_cast<double>(Quality);
	if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), QualityNum))
	{
		Quality = FMath::Clamp(static_cast<int32>(QualityNum), 1, 100);
	}

	double MaxSizeNum = static_cast<double>(MaxSize);
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MaxSizeNum))
	{
		MaxSize = FMath::Clamp(static_cast<int32>(MaxSizeNum), 64, 8192);
	}

	double PreviewAdvanceSeconds = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("preview_advance_seconds"), PreviewAdvanceSeconds);
	PreviewAdvanceSeconds = FMath::Clamp(PreviewAdvanceSeconds, 0.0, 60.0);

	double PreviewTickDeltaSeconds = 1.0 / 60.0;
	JsonTryGetNumber(Ctx.Params, TEXT("preview_tick_delta_seconds"), PreviewTickDeltaSeconds);
	PreviewTickDeltaSeconds = FMath::Clamp(PreviewTickDeltaSeconds, 1.0 / 240.0, 1.0 / 5.0);

	FString PreviewAdvanceMode = TEXT("desired_age");
	JsonTryGetString(Ctx.Params, TEXT("preview_advance_mode"), PreviewAdvanceMode);
	PreviewAdvanceMode.TrimStartAndEndInline();
	if (PreviewAdvanceMode.IsEmpty())
	{
		PreviewAdvanceMode = TEXT("desired_age");
	}

	bool bResetPreview = PreviewAdvanceSeconds > 0.0;
	JsonTryGetBool(Ctx.Params, TEXT("reset_preview"), bResetPreview);

	bool bPausePreviewAfterAdvance = true;
	JsonTryGetBool(Ctx.Params, TEXT("pause_preview_after_advance"), bPausePreviewAfterAdvance);

	const bool bPreviewPrepareRequested = PreviewAdvanceSeconds > 0.0 || bResetPreview;
	bool bUseOffscreenRenderer = false;
	const bool bUseOffscreenRendererSpecified =
		JsonTryGetBool(Ctx.Params, TEXT("offscreen"), bUseOffscreenRenderer) ||
		JsonTryGetBool(Ctx.Params, TEXT("use_offscreen_renderer"), bUseOffscreenRenderer);

	Format = Format.ToLower();
	if (Format == TEXT("jpeg"))
	{
		Format = TEXT("jpg");
	}
	if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
	{
		OutError = TEXT("unsupported_format");
		return false;
	}

	const TSharedPtr<SWindow> NiagaraWindow = UeAgentNiagaraOps::ResolveNiagaraEditorWindow(AssetEditor, OutError);
	if (!NiagaraWindow.IsValid())
	{
		return false;
	}

	bool bPreviewPrepared = false;
	FString PreviewPrepareError;
	bool bPrePreviewRedrawPerformed = false;
	FString PrePreviewRedrawSkipReason;

	TSharedPtr<SWidget> WidgetToShot;
	FString EffectiveTarget = Target;
	FString CaptureMode = TEXT("slate_widget");
	if (Target == TEXT("window"))
	{
		WidgetToShot = NiagaraWindow;
	}
	else if (Target == TEXT("viewport") || Target == TEXT("preview") || Target == TEXT("preview_viewport"))
	{
		const TSharedPtr<SWidget> ViewportWidget = UeAgentNiagaraOps::ResolveNiagaraViewportWidget(NiagaraWindow.ToSharedRef(), OutError);
		if (!ViewportWidget.IsValid())
		{
			return false;
		}

		const TSharedPtr<SWidget> SceneWidget = UeAgentNiagaraOps::ResolveNiagaraViewportSceneWidget(ViewportWidget.ToSharedRef());
		WidgetToShot = SceneWidget.IsValid() ? SceneWidget : ViewportWidget;
		EffectiveTarget = TEXT("viewport");
	}
	else
	{
		OutError = TEXT("invalid_target");
		return false;
	}

	if (!WidgetToShot.IsValid())
	{
		OutError = TEXT("target_widget_not_found");
		return false;
	}

	FString WindowBackbufferUnsafeReason;
	const bool bWindowBackbufferSafe = UeAgentNiagaraOps::IsSlateWindowSafeForBackbufferCapture(NiagaraWindow, WindowBackbufferUnsafeReason);
	if (!bUseOffscreenRendererSpecified && !bWindowBackbufferSafe)
	{
		bUseOffscreenRenderer = true;
	}
	if (!bWindowBackbufferSafe && !bUseOffscreenRenderer)
	{
		OutError = FString::Printf(TEXT("window_backbuffer_capture_unsafe:%s"), *WindowBackbufferUnsafeReason);
		return false;
	}

	if (bPreviewPrepareRequested)
	{
		if (bUseOffscreenRenderer)
		{
			PrePreviewRedrawSkipReason = TEXT("offscreen_renderer");
		}
		else
		{
			bPrePreviewRedrawPerformed = UeAgentNiagaraOps::PrepareSlateWindowForCapture(NiagaraWindow, PrePreviewRedrawSkipReason);
		}
		bPreviewPrepared = UeAgentNiagaraOps::PrepareNiagaraPreviewForCapture(Asset, bResetPreview, PreviewAdvanceSeconds, PreviewTickDeltaSeconds, PreviewAdvanceMode, bPausePreviewAfterAdvance, PreviewPrepareError);
	}

	bool bCaptureRedrawPerformed = false;
	FString CaptureRedrawSkipReason;
	if (!bUseOffscreenRenderer)
	{
		bCaptureRedrawPerformed = UeAgentNiagaraOps::PrepareSlateWindowForCapture(NiagaraWindow, CaptureRedrawSkipReason);
	}

	FString PreviewStatsError;
	TSharedPtr<FJsonObject> PreviewStats = UeAgentNiagaraOps::CollectNiagaraPreviewStats(Asset, PreviewStatsError);

	TArray<FColor> Pixels;
	FIntPoint ShotSize(0, 0);
	FVector2D OffscreenDrawSize(0.0, 0.0);
	if (bUseOffscreenRenderer)
	{
		CaptureMode = TEXT("slate_widget_offscreen");
		OffscreenDrawSize = UeAgentNiagaraOps::ResolveOffscreenDrawSize(WidgetToShot.ToSharedRef(), EffectiveTarget, Ctx.Params, MaxSize);
		if (!UeAgentNiagaraOps::ScreenshotSlateWidgetOffscreen(WidgetToShot.ToSharedRef(), OffscreenDrawSize, Pixels, ShotSize, OutError))
		{
			FUeAgentInterfaceLogger::Log(TEXT("CmdNiagaraScreenshot offscreen failed target=%s asset=%s error=%s"), *EffectiveTarget, *Asset->GetOutermost()->GetName(), *OutError);
			return false;
		}
	}
	else if (!UeAgentNiagaraOps::ScreenshotSlateWidget(WidgetToShot.ToSharedRef(), Pixels, ShotSize, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdNiagaraScreenshot failed target=%s asset=%s error=%s"), *EffectiveTarget, *Asset->GetOutermost()->GetName(), *OutError);
		return false;
	}

	TArray<FColor> ResizedPixels;
	FIntPoint ResizedSize(0, 0);
	if (!ResizePixelsMaxSize(Pixels, ShotSize, MaxSize, ResizedPixels, ResizedSize, OutError))
	{
		return false;
	}

	const FString OutPath = UeAgentNiagaraOps::ResolveOutputScreenshotPath(Ctx.Params, Format);
	if (!WriteCompressedImage(ResizedPixels, ResizedSize, Format, Quality, OutPath, OutError))
	{
		return false;
	}

	const int64 Bytes = IFileManager::Get().FileSize(*OutPath);
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_kind"), UeAgentNiagaraOps::GetNiagaraAssetKind(Asset));
	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetStringField(TEXT("target"), EffectiveTarget);
	OutData->SetStringField(TEXT("capture_mode"), CaptureMode);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetNumberField(TEXT("width"), ResizedSize.X);
	OutData->SetNumberField(TEXT("height"), ResizedSize.Y);
	OutData->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
	OutData->SetBoolField(TEXT("opened_editor_if_needed"), bOpenEditorIfNeeded);
	OutData->SetBoolField(TEXT("synchronized_overview_graph"), bSynchronizedOverviewGraph);
	OutData->SetNumberField(TEXT("overview_node_count_before"), OverviewNodeCountBefore);
	OutData->SetNumberField(TEXT("overview_node_count_after"), OverviewNodeCountAfter);
	OutData->SetBoolField(TEXT("offscreen_renderer"), bUseOffscreenRenderer);
	OutData->SetNumberField(TEXT("offscreen_draw_width"), OffscreenDrawSize.X);
	OutData->SetNumberField(TEXT("offscreen_draw_height"), OffscreenDrawSize.Y);
	OutData->SetBoolField(TEXT("window_backbuffer_safe"), bWindowBackbufferSafe);
	OutData->SetStringField(TEXT("window_backbuffer_unsafe_reason"), WindowBackbufferUnsafeReason);
	OutData->SetBoolField(TEXT("pre_preview_redraw_performed"), bPrePreviewRedrawPerformed);
	OutData->SetStringField(TEXT("pre_preview_redraw_skip_reason"), PrePreviewRedrawSkipReason);
	OutData->SetBoolField(TEXT("capture_redraw_performed"), bCaptureRedrawPerformed);
	OutData->SetStringField(TEXT("capture_redraw_skip_reason"), CaptureRedrawSkipReason);
	OutData->SetBoolField(TEXT("preview_prepare_requested"), bPreviewPrepareRequested);
	OutData->SetBoolField(TEXT("preview_prepared"), bPreviewPrepared);
	OutData->SetStringField(TEXT("preview_prepare_error"), PreviewPrepareError);
	OutData->SetBoolField(TEXT("preview_reset"), bResetPreview);
	OutData->SetNumberField(TEXT("preview_advance_seconds"), PreviewAdvanceSeconds);
	OutData->SetNumberField(TEXT("preview_tick_delta_seconds"), PreviewTickDeltaSeconds);
	OutData->SetStringField(TEXT("preview_advance_mode"), PreviewAdvanceMode);
	OutData->SetBoolField(TEXT("preview_paused_after_advance"), bPausePreviewAfterAdvance);
	OutData->SetStringField(TEXT("preview_stats_error"), PreviewStatsError);
	if (PreviewStats.IsValid())
	{
		OutData->SetObjectField(TEXT("preview_stats"), PreviewStats);
	}
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset);
	if (!NiagaraSystem)
	{
		OutError = TEXT("asset_is_not_niagara_system");
		return false;
	}

	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);

	if (NiagaraSystem->HasOutstandingCompilationRequests(true))
	{
		NiagaraSystem->WaitForCompilationComplete(true, false);
		NiagaraSystem->PollForCompilationComplete(true);
	}
	NiagaraSystem->CacheFromCompiledData();

	bool bUseTransientComponent = false;
	JsonTryGetBool(Ctx.Params, TEXT("use_transient_component"), bUseTransientComponent);
	if (!bUseTransientComponent)
	{
		JsonTryGetBool(Ctx.Params, TEXT("use_transient_preview_component"), bUseTransientComponent);
	}

	TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
	UNiagaraComponent* PreviewComponent = nullptr;
	if (bUseTransientComponent)
	{
		if (!GEditor)
		{
			OutError = TEXT("editor_unavailable");
			return false;
		}
		UWorld* PreviewWorld = GEditor->GetEditorWorldContext().World();
		if (!PreviewWorld)
		{
			OutError = TEXT("editor_world_unavailable");
			return false;
		}

		PreviewComponent = NewObject<UNiagaraComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		if (!PreviewComponent)
		{
			OutError = TEXT("transient_niagara_component_create_failed");
			return false;
		}
		PreviewComponent->AddToRoot();
		PreviewComponent->SetAutoActivate(false);
		PreviewComponent->SetAsset(NiagaraSystem, false);
		PreviewComponent->RegisterComponentWithWorld(PreviewWorld);
	}
	else
	{
		IAssetEditorInstance* AssetEditor = UeAgentNiagaraOps::GetNiagaraAssetEditor(Asset, bOpenEditorIfNeeded, OutError);
		if (!AssetEditor)
		{
			return false;
		}

		FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor"));
		if (!NiagaraEditorModule)
		{
			OutError = TEXT("niagara_editor_module_not_loaded");
			return false;
		}

		SystemViewModel = NiagaraEditorModule->GetExistingViewModelForSystem(NiagaraSystem);
		if (!SystemViewModel.IsValid())
		{
			OutError = TEXT("niagara_system_view_model_not_found");
			return false;
		}

		PreviewComponent = SystemViewModel->GetPreviewComponent();
		if (!PreviewComponent)
		{
			OutError = TEXT("niagara_preview_component_not_found");
			return false;
		}
	}

	bool bResetPreview = true;
	JsonTryGetBool(Ctx.Params, TEXT("reset_preview"), bResetPreview);

	bool bDeactivateBeforeReset = true;
	JsonTryGetBool(Ctx.Params, TEXT("deactivate_before_reset"), bDeactivateBeforeReset);

	bool bIncludeEachTick = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_each_tick"), bIncludeEachTick);

	bool bPauseAfterProbe = false;
	JsonTryGetBool(Ctx.Params, TEXT("pause_after_probe"), bPauseAfterProbe);

	FString AdvanceMode = TEXT("advance_simulation");
	JsonTryGetString(Ctx.Params, TEXT("advance_mode"), AdvanceMode);
	AdvanceMode.TrimStartAndEndInline();
	if (AdvanceMode.IsEmpty())
	{
		AdvanceMode = TEXT("advance_simulation");
	}
	const FString NormalizedAdvanceMode = AdvanceMode.ToLower();
	const bool bUseAdvanceSimulation =
		NormalizedAdvanceMode == TEXT("advance") ||
		NormalizedAdvanceMode == TEXT("advance_simulation") ||
		NormalizedAdvanceMode == TEXT("advance_simulation_by_time");
	const bool bUseTickComponent =
		NormalizedAdvanceMode == TEXT("tick") ||
		NormalizedAdvanceMode == TEXT("tick_component") ||
		NormalizedAdvanceMode == TEXT("tick_delta") ||
		NormalizedAdvanceMode == TEXT("tick_delta_time");

	double TickDeltaNumber = 1.0 / 60.0;
	JsonTryGetNumber(Ctx.Params, TEXT("tick_delta_seconds"), TickDeltaNumber);
	const float TickDeltaSeconds = FMath::Clamp(static_cast<float>(TickDeltaNumber), 1.0f / 240.0f, 1.0f / 5.0f);

	double TickCountNumber = 3.0;
	JsonTryGetNumber(Ctx.Params, TEXT("tick_count"), TickCountNumber);
	const int32 TickCount = FMath::Clamp(FMath::RoundToInt(TickCountNumber), 0, 240);

	auto ConfigurePreviewComponent = [TickDeltaSeconds, NiagaraSystem](UNiagaraComponent* Component)
	{
		if (!Component)
		{
			return;
		}
		Component->SetAsset(NiagaraSystem, false);
		Component->SetForceSolo(true);
		Component->SetAgeUpdateMode(ENiagaraAgeUpdateMode::TickDeltaTime);
		Component->SetSeekDelta(TickDeltaSeconds);
		Component->SetLockDesiredAgeDeltaTimeToSeekDelta(true);
		Component->SetCanRenderWhileSeeking(true);
		Component->SetAllowScalability(false);
		Component->SetRenderingEnabled(true);
	};

	TArray<TSharedPtr<FJsonValue>> Snapshots;
	auto AddSnapshot = [&Snapshots, NiagaraSystem, &PreviewComponent](const FString& Label)
	{
		FString StatsError;
		TSharedPtr<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		Snapshot->SetStringField(TEXT("label"), Label);
		TSharedPtr<FJsonObject> Stats = UeAgentNiagaraOps::CollectNiagaraComponentStats(NiagaraSystem, PreviewComponent, StatsError);
		Snapshot->SetStringField(TEXT("stats_error"), StatsError);
		if (Stats.IsValid())
		{
			Snapshot->SetObjectField(TEXT("stats"), Stats);
		}
		Snapshots.Add(MakeShared<FJsonValueObject>(Snapshot));
	};

	ConfigurePreviewComponent(PreviewComponent);
	AddSnapshot(TEXT("after_configure_before_reset"));

	if (bResetPreview)
	{
		if (bDeactivateBeforeReset)
		{
			PreviewComponent->DeactivateImmediate();
			UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
			AddSnapshot(TEXT("after_deactivate_immediate"));
		}

		PreviewComponent->SetDesiredAge(0.0f);
		PreviewComponent->ReinitializeSystem();
		if (!bUseTransientComponent)
		{
			PreviewComponent = SystemViewModel->GetPreviewComponent();
			if (!PreviewComponent)
			{
				OutError = TEXT("niagara_preview_component_missing_after_reset");
				return false;
			}
		}
		ConfigurePreviewComponent(PreviewComponent);
		UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
		AddSnapshot(TEXT("after_reinitialize"));
	}

	PreviewComponent->SetPaused(false);
	PreviewComponent->Activate(true);
	UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
	AddSnapshot(TEXT("after_activate_before_tick"));

	for (int32 TickIndex = 0; TickIndex < TickCount; ++TickIndex)
	{
		if (bUseAdvanceSimulation)
		{
			PreviewComponent->AdvanceSimulation(1, TickDeltaSeconds);
		}
		else
		{
			PreviewComponent->TickComponent(TickDeltaSeconds, ELevelTick::LEVELTICK_All, nullptr);
		}
		UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
		if (bIncludeEachTick || TickIndex == TickCount - 1)
		{
			AddSnapshot(FString::Printf(TEXT("after_%s_%d"), bUseAdvanceSimulation ? TEXT("advance") : TEXT("tick"), TickIndex + 1));
		}
	}

	if (bPauseAfterProbe)
	{
		PreviewComponent->SetPaused(true);
	}
	PreviewComponent->MarkRenderStateDirty();

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	OutData->SetStringField(TEXT("component_source"), bUseTransientComponent ? TEXT("transient_component") : TEXT("editor_preview_component"));
	OutData->SetNumberField(TEXT("tick_count"), TickCount);
	OutData->SetNumberField(TEXT("tick_delta_seconds"), TickDeltaSeconds);
	OutData->SetStringField(TEXT("advance_mode"), bUseAdvanceSimulation ? TEXT("advance_simulation") : (bUseTickComponent ? TEXT("tick_component") : NormalizedAdvanceMode));
	OutData->SetBoolField(TEXT("reset_preview"), bResetPreview);
	OutData->SetBoolField(TEXT("deactivate_before_reset"), bDeactivateBeforeReset);
	OutData->SetBoolField(TEXT("pause_after_probe"), bPauseAfterProbe);
	OutData->SetArrayField(TEXT("snapshots"), Snapshots);
	OutData->SetNumberField(TEXT("snapshot_count"), Snapshots.Num());
	if (bUseTransientComponent && PreviewComponent)
	{
		PreviewComponent->DeactivateImmediate();
		UeAgentNiagaraOps::FlushNiagaraPreviewWorld(PreviewComponent);
		PreviewComponent->UnregisterComponent();
		PreviewComponent->RemoveFromRoot();
		PreviewComponent->DestroyComponent();
	}
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraGetStackIssues(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	FString SeverityFilter = TEXT("warning_or_error");
	JsonTryGetString(Ctx.Params, TEXT("severity_filter"), SeverityFilter);

	bool bCompileBeforeRead = false;
	JsonTryGetBool(Ctx.Params, TEXT("compile_before_read"), bCompileBeforeRead);

	bool bPreferExistingViewModel = true;
	JsonTryGetBool(Ctx.Params, TEXT("prefer_existing_view_model"), bPreferExistingViewModel);

	bool bOpenEditorIfNeeded = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);

	bool bIncludeSystemScope = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_system_scope"), bIncludeSystemScope);

	bool bIncludeEmitters = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_emitters"), bIncludeEmitters);

	FString EmitterNameFilter;
	const bool bHasEmitterNameFilter = JsonTryGetString(Ctx.Params, TEXT("emitter_name"), EmitterNameFilter) && !EmitterNameFilter.IsEmpty();

	FGuid EmitterIdFilter;
	FString EmitterIdFilterText;
	const bool bHasEmitterIdFilter =
		JsonTryGetString(Ctx.Params, TEXT("emitter_id"), EmitterIdFilterText) &&
		!EmitterIdFilterText.IsEmpty() &&
		FGuid::Parse(EmitterIdFilterText, EmitterIdFilter);

	int32 EmitterIndexFilter = INDEX_NONE;
	double EmitterIndexNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("emitter_index"), EmitterIndexNumber))
	{
		EmitterIndexFilter = FMath::RoundToInt(EmitterIndexNumber);
	}

	TArray<TSharedPtr<FJsonValue>> ScopeJson;
	TArray<TSharedPtr<FJsonValue>> IssuesJson;
	UeAgentNiagaraOps::FNiagaraStackIssueCounts TotalIssueCounts;
	UeAgentNiagaraOps::FNiagaraStackIssueCounts ReturnedIssueCounts;
	FString AssetKind;
	FString ViewModelSource;

	if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset))
	{
		AssetKind = TEXT("system");
		if (bCompileBeforeRead)
		{
			NiagaraSystem->RequestCompile(false);
			NiagaraSystem->WaitForCompilationComplete(true, false);
			NiagaraSystem->PollForCompilationComplete(true);
			NiagaraSystem->CacheFromCompiledData();
		}

		TSharedPtr<FNiagaraSystemViewModel> TemporarySystemViewModel;
		TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
		if (bPreferExistingViewModel)
		{
			if (bOpenEditorIfNeeded)
			{
				if (!UeAgentNiagaraOps::GetNiagaraAssetEditor(Asset, true, OutError))
				{
					return false;
				}
			}

			if (FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor")))
			{
				SystemViewModel = NiagaraEditorModule->GetExistingViewModelForSystem(NiagaraSystem);
				if (SystemViewModel.IsValid())
				{
					ViewModelSource = TEXT("existing_view_model");
				}
			}
		}
		if (!SystemViewModel.IsValid())
		{
			TemporarySystemViewModel = UeAgentNiagaraOps::CreateDataProcessingSystemViewModel(*NiagaraSystem);
			SystemViewModel = TemporarySystemViewModel;
			ViewModelSource = TEXT("data_processing_view_model");
		}

		if (!SystemViewModel.IsValid())
		{
			OutError = TEXT("niagara_system_view_model_create_failed");
			return false;
		}

		if (bIncludeSystemScope)
		{
			TArray<FNiagaraMessageSourceAndStore> SystemMessageStores;
			SystemMessageStores.Add(FNiagaraMessageSourceAndStore(*NiagaraSystem, NiagaraSystem->GetMessageStore()));
			TSharedPtr<FJsonObject> ScopeObj = UeAgentNiagaraOps::CollectNiagaraStackIssuesFromViewModel(
				SystemViewModel->GetSystemStackViewModel(),
				TEXT("system"),
				SystemViewModel->GetDisplayName().ToString(),
				FGuid(),
				SeverityFilter,
				&SystemMessageStores,
				IssuesJson,
				TotalIssueCounts,
				ReturnedIssueCounts);
			UeAgentNiagaraOps::AppendNiagaraSystemCompileStatus(ScopeObj, *NiagaraSystem);
			ScopeJson.Add(MakeShared<FJsonValueObject>(ScopeObj));
		}

		if (bIncludeEmitters)
		{
			const TArray<TSharedRef<FNiagaraEmitterHandleViewModel>>& EmitterHandleViewModels = SystemViewModel->GetEmitterHandleViewModels();
			for (int32 EmitterIndex = 0; EmitterIndex < EmitterHandleViewModels.Num(); ++EmitterIndex)
			{
				const TSharedRef<FNiagaraEmitterHandleViewModel>& EmitterHandleViewModel = EmitterHandleViewModels[EmitterIndex];
				const FString EmitterName = EmitterHandleViewModel->GetNameText().ToString();
				const FGuid EmitterHandleId = EmitterHandleViewModel->GetId();

				if (EmitterIndexFilter != INDEX_NONE && EmitterIndex != EmitterIndexFilter)
				{
					continue;
				}
				if (bHasEmitterNameFilter && EmitterName != EmitterNameFilter)
				{
					continue;
				}
				if (bHasEmitterIdFilter && EmitterHandleId != EmitterIdFilter)
				{
					continue;
				}

				TArray<FNiagaraMessageSourceAndStore> EmitterMessageStores;
				UNiagaraStackViewModel::FTopLevelViewModel EmitterTopLevelViewModel(EmitterHandleViewModel);
				EmitterTopLevelViewModel.GetMessageStores(EmitterMessageStores);
				TSharedPtr<FJsonObject> ScopeObj = UeAgentNiagaraOps::CollectNiagaraStackIssuesFromViewModel(
					EmitterHandleViewModel->GetEmitterStackViewModel(),
					TEXT("emitter"),
					EmitterName,
					EmitterHandleId,
					SeverityFilter,
					&EmitterMessageStores,
					IssuesJson,
					TotalIssueCounts,
					ReturnedIssueCounts);
				UeAgentNiagaraOps::AppendNiagaraEmitterHandleStatus(ScopeObj, EmitterHandleViewModel, EmitterIndex);
				ScopeJson.Add(MakeShared<FJsonValueObject>(ScopeObj));
			}
		}
	}
	else if (UNiagaraEmitter* NiagaraEmitter = Cast<UNiagaraEmitter>(Asset))
	{
		AssetKind = TEXT("emitter");
		UNiagaraSystem* TransientSystem = nullptr;
		TSharedPtr<FNiagaraSystemViewModel> SystemViewModel = UeAgentNiagaraOps::CreateDataProcessingEmitterViewModel(*NiagaraEmitter, TransientSystem);
		ViewModelSource = TEXT("data_processing_transient_system");
		if (!SystemViewModel.IsValid())
		{
			OutError = TEXT("niagara_emitter_view_model_create_failed");
			return false;
		}

		const TArray<TSharedRef<FNiagaraEmitterHandleViewModel>>& EmitterHandleViewModels = SystemViewModel->GetEmitterHandleViewModels();
		if (EmitterHandleViewModels.Num() == 0)
		{
			OutError = TEXT("niagara_emitter_view_model_missing_emitter_handle");
			return false;
		}

		const TSharedRef<FNiagaraEmitterHandleViewModel>& EmitterHandleViewModel = EmitterHandleViewModels[0];
		TArray<FNiagaraMessageSourceAndStore> EmitterMessageStores;
		UNiagaraStackViewModel::FTopLevelViewModel EmitterTopLevelViewModel(EmitterHandleViewModel);
		EmitterTopLevelViewModel.GetMessageStores(EmitterMessageStores);
		TSharedPtr<FJsonObject> ScopeObj = UeAgentNiagaraOps::CollectNiagaraStackIssuesFromViewModel(
			EmitterHandleViewModel->GetEmitterStackViewModel(),
			TEXT("emitter"),
			EmitterHandleViewModel->GetNameText().ToString(),
			EmitterHandleViewModel->GetId(),
			SeverityFilter,
			&EmitterMessageStores,
			IssuesJson,
			TotalIssueCounts,
			ReturnedIssueCounts);
		UeAgentNiagaraOps::AppendNiagaraEmitterHandleStatus(ScopeObj, EmitterHandleViewModel, 0);
		ScopeJson.Add(MakeShared<FJsonValueObject>(ScopeObj));

		OutData->SetStringField(TEXT("transient_system_path"), TransientSystem ? TransientSystem->GetPathName() : FString());
	}
	else
	{
		OutError = TEXT("asset_is_not_niagara_system_or_emitter");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_kind"), AssetKind);
	OutData->SetStringField(TEXT("view_model_source"), ViewModelSource);
	OutData->SetStringField(TEXT("severity_filter"), SeverityFilter);
	OutData->SetBoolField(TEXT("compile_before_read"), bCompileBeforeRead);
	OutData->SetBoolField(TEXT("prefer_existing_view_model"), bPreferExistingViewModel);
	OutData->SetBoolField(TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);
	OutData->SetArrayField(TEXT("scopes"), ScopeJson);
	OutData->SetNumberField(TEXT("scope_count"), ScopeJson.Num());
	OutData->SetArrayField(TEXT("issues"), IssuesJson);
	OutData->SetNumberField(TEXT("returned_issue_count"), IssuesJson.Num());
	TotalIssueCounts.AppendToJson(OutData, TEXT("total_"));
	ReturnedIssueCounts.AppendToJson(OutData, TEXT("returned_"));
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraApplyStackIssueFix(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset);
	if (!NiagaraSystem)
	{
		OutError = TEXT("asset_is_not_niagara_system");
		return false;
	}

	FString IssueUniqueIdentifier;
	const bool bHasIssueUniqueIdentifier =
		JsonTryGetString(Ctx.Params, TEXT("issue_unique_identifier"), IssueUniqueIdentifier) &&
		!IssueUniqueIdentifier.IsEmpty();

	FString ModuleNodeGuidText;
	FGuid ModuleNodeGuid;
	const bool bHasModuleNodeGuid =
		JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) &&
		!ModuleNodeGuidText.IsEmpty() &&
		FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid);
	if (!ModuleNodeGuidText.IsEmpty() && !bHasModuleNodeGuid)
	{
		OutError = TEXT("invalid_module_node_guid");
		return false;
	}

	FString ShortDescriptionContains;
	const bool bHasShortDescriptionContains =
		JsonTryGetString(Ctx.Params, TEXT("short_description_contains"), ShortDescriptionContains) &&
		!ShortDescriptionContains.IsEmpty();

	FString LongDescriptionContains;
	const bool bHasLongDescriptionContains =
		JsonTryGetString(Ctx.Params, TEXT("long_description_contains"), LongDescriptionContains) &&
		!LongDescriptionContains.IsEmpty();

	FString EntryPathContains;
	const bool bHasEntryPathContains =
		JsonTryGetString(Ctx.Params, TEXT("entry_path_contains"), EntryPathContains) &&
		!EntryPathContains.IsEmpty();

	bool bApplyFirstMatch = false;
	JsonTryGetBool(Ctx.Params, TEXT("apply_first_match"), bApplyFirstMatch);
	if (!bHasIssueUniqueIdentifier && !bHasModuleNodeGuid && !bHasShortDescriptionContains && !bHasLongDescriptionContains && !bHasEntryPathContains && !bApplyFirstMatch)
	{
		OutError = TEXT("missing_stack_issue_selector");
		return false;
	}

	FString FixUniqueIdentifier;
	const bool bHasFixUniqueIdentifier =
		JsonTryGetString(Ctx.Params, TEXT("fix_unique_identifier"), FixUniqueIdentifier) &&
		!FixUniqueIdentifier.IsEmpty();

	FString FixDescriptionContains;
	const bool bHasFixDescriptionContains =
		JsonTryGetString(Ctx.Params, TEXT("fix_description_contains"), FixDescriptionContains) &&
		!FixDescriptionContains.IsEmpty();

	int32 RequestedFixIndex = 0;
	double FixIndexNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("fix_index"), FixIndexNumber))
	{
		RequestedFixIndex = FMath::RoundToInt(FixIndexNumber);
	}

	bool bAllowLinkFix = false;
	JsonTryGetBool(Ctx.Params, TEXT("allow_link_fix"), bAllowLinkFix);

	FString SeverityFilter = TEXT("all");
	JsonTryGetString(Ctx.Params, TEXT("severity_filter"), SeverityFilter);

	bool bPreferExistingViewModel = true;
	JsonTryGetBool(Ctx.Params, TEXT("prefer_existing_view_model"), bPreferExistingViewModel);

	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);

	bool bIncludeSystemScope = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_system_scope"), bIncludeSystemScope);

	bool bIncludeEmitters = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_emitters"), bIncludeEmitters);

	FString ScopeTypeFilter;
	const bool bHasScopeTypeFilter =
		JsonTryGetString(Ctx.Params, TEXT("scope_type"), ScopeTypeFilter) &&
		!ScopeTypeFilter.IsEmpty();

	FString EmitterNameFilter;
	const bool bHasEmitterNameFilter =
		JsonTryGetString(Ctx.Params, TEXT("emitter_name"), EmitterNameFilter) &&
		!EmitterNameFilter.IsEmpty();

	FGuid EmitterIdFilter;
	FString EmitterIdFilterText;
	const bool bHasEmitterIdFilter =
		JsonTryGetString(Ctx.Params, TEXT("emitter_id"), EmitterIdFilterText) &&
		!EmitterIdFilterText.IsEmpty() &&
		FGuid::Parse(EmitterIdFilterText, EmitterIdFilter);
	if (!EmitterIdFilterText.IsEmpty() && !bHasEmitterIdFilter)
	{
		OutError = TEXT("invalid_emitter_id");
		return false;
	}

	int32 EmitterIndexFilter = INDEX_NONE;
	double EmitterIndexNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("emitter_index"), EmitterIndexNumber))
	{
		EmitterIndexFilter = FMath::RoundToInt(EmitterIndexNumber);
	}

	TSharedPtr<FNiagaraSystemViewModel> TemporarySystemViewModel;
	TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
	FString ViewModelSource;
	if (bPreferExistingViewModel)
	{
		if (bOpenEditorIfNeeded)
		{
			if (!UeAgentNiagaraOps::GetNiagaraAssetEditor(Asset, true, OutError))
			{
				return false;
			}
		}

		if (FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor")))
		{
			SystemViewModel = NiagaraEditorModule->GetExistingViewModelForSystem(NiagaraSystem);
			if (SystemViewModel.IsValid())
			{
				ViewModelSource = TEXT("existing_view_model");
			}
		}
	}
	if (!SystemViewModel.IsValid())
	{
		TemporarySystemViewModel = UeAgentNiagaraOps::CreateDataProcessingSystemViewModel(*NiagaraSystem);
		SystemViewModel = TemporarySystemViewModel;
		ViewModelSource = TEXT("data_processing_view_model");
	}
	if (!SystemViewModel.IsValid())
	{
		OutError = TEXT("niagara_system_view_model_create_failed");
		return false;
	}

	struct FMatchedStackIssueFix
	{
		UNiagaraStackViewModel* StackViewModel = nullptr;
		UNiagaraStackEntry* Entry = nullptr;
		UNiagaraStackEntry::FStackIssue Issue;
		UNiagaraStackEntry::FStackIssueFix Fix;
		FString ScopeType;
		FString ScopeName;
		FGuid EmitterHandleId;
		FString EntryPath;
		int32 IssueIndex = INDEX_NONE;
		int32 FixIndex = INDEX_NONE;
	};

	TArray<FMatchedStackIssueFix> Matches;
	TArray<TSharedPtr<FJsonValue>> MatchSummaries;
	int32 VisitedScopeCount = 0;
	int32 VisitedIssueCount = 0;

	auto FindFixIndex = [&](const UNiagaraStackEntry::FStackIssue& Issue, int32& OutFixIndex) -> bool
	{
		OutFixIndex = INDEX_NONE;
		const TArray<UNiagaraStackEntry::FStackIssueFix>& Fixes = Issue.GetFixes();
		if (bHasFixUniqueIdentifier)
		{
			for (int32 FixIndex = 0; FixIndex < Fixes.Num(); ++FixIndex)
			{
				if (Fixes[FixIndex].GetUniqueIdentifier() == FixUniqueIdentifier)
				{
					OutFixIndex = FixIndex;
					return true;
				}
			}
			return false;
		}
		if (bHasFixDescriptionContains)
		{
			for (int32 FixIndex = 0; FixIndex < Fixes.Num(); ++FixIndex)
			{
				if (Fixes[FixIndex].GetDescription().ToString().Contains(FixDescriptionContains, ESearchCase::IgnoreCase))
				{
					OutFixIndex = FixIndex;
					return true;
				}
			}
			return false;
		}
		if (!Fixes.IsValidIndex(RequestedFixIndex))
		{
			return false;
		}
		OutFixIndex = RequestedFixIndex;
		return true;
	};

	auto CollectMatchesForScope = [&](UNiagaraStackViewModel* StackViewModel, const FString& ScopeType, const FString& ScopeName, const FGuid& EmitterHandleId)
	{
		if (!StackViewModel)
		{
			return;
		}
		if (bHasScopeTypeFilter && !ScopeType.Equals(ScopeTypeFilter, ESearchCase::IgnoreCase))
		{
			return;
		}

		UNiagaraStackEntry* RootEntry = StackViewModel->GetRootEntry();
		if (!RootEntry)
		{
			return;
		}

		++VisitedScopeCount;
		RootEntry->RefreshChildren();
		RootEntry->RefreshFilteredChildren();

		TArray<UNiagaraStackEntry*> EntriesToCheck;
		EntriesToCheck.Add(RootEntry);
		EntriesToCheck.Append(RootEntry->GetAllChildrenWithIssues());

		TSet<UNiagaraStackEntry*> SeenEntries;
		for (UNiagaraStackEntry* EntryToCheck : EntriesToCheck)
		{
			if (!EntryToCheck || SeenEntries.Contains(EntryToCheck))
			{
				continue;
			}
			SeenEntries.Add(EntryToCheck);

			const FString EntryPath = UeAgentNiagaraOps::BuildStackIssueEntryPath(StackViewModel, *EntryToCheck);
			const TArray<UNiagaraStackEntry::FStackIssue>& Issues = EntryToCheck->GetIssues();
			for (int32 IssueIndex = 0; IssueIndex < Issues.Num(); ++IssueIndex)
			{
				const UNiagaraStackEntry::FStackIssue& Issue = Issues[IssueIndex];
				++VisitedIssueCount;
				if (!UeAgentNiagaraOps::ShouldIncludeStackIssueSeverity(Issue.GetSeverity(), SeverityFilter))
				{
					continue;
				}
				if (bHasIssueUniqueIdentifier && Issue.GetUniqueIdentifier() != IssueUniqueIdentifier)
				{
					continue;
				}
				if (bHasShortDescriptionContains && !Issue.GetShortDescription().ToString().Contains(ShortDescriptionContains, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (bHasLongDescriptionContains && !Issue.GetLongDescription().ToString().Contains(LongDescriptionContains, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (bHasEntryPathContains && !EntryPath.Contains(EntryPathContains, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (bHasModuleNodeGuid)
				{
					UNiagaraStackModuleItem* ModuleItem = Cast<UNiagaraStackModuleItem>(EntryToCheck);
					if (!ModuleItem || ModuleItem->GetModuleNode().NodeGuid != ModuleNodeGuid)
					{
						continue;
					}
				}

				int32 FixIndex = INDEX_NONE;
				if (!FindFixIndex(Issue, FixIndex))
				{
					continue;
				}

				const TArray<UNiagaraStackEntry::FStackIssueFix>& Fixes = Issue.GetFixes();
				const UNiagaraStackEntry::FStackIssueFix& Fix = Fixes[FixIndex];
				if (!bAllowLinkFix && Fix.GetStyle() == UNiagaraStackEntry::EStackIssueFixStyle::Link)
				{
					continue;
				}

				FMatchedStackIssueFix Match;
				Match.StackViewModel = StackViewModel;
				Match.Entry = EntryToCheck;
				Match.Issue = Issue;
				Match.Fix = Fix;
				Match.ScopeType = ScopeType;
				Match.ScopeName = ScopeName;
				Match.EmitterHandleId = EmitterHandleId;
				Match.EntryPath = EntryPath;
				Match.IssueIndex = IssueIndex;
				Match.FixIndex = FixIndex;
				Matches.Add(Match);

				TSharedPtr<FJsonObject> SummaryObj = UeAgentNiagaraOps::BuildStackIssueJson(*EntryToCheck, Issue, ScopeType, ScopeName, EmitterHandleId, EntryPath);
				SummaryObj->SetNumberField(TEXT("selected_fix_index"), FixIndex);
				SummaryObj->SetStringField(TEXT("selected_fix_description"), Fix.GetDescription().ToString());
				SummaryObj->SetStringField(TEXT("selected_fix_unique_identifier"), Fix.GetUniqueIdentifier());
				MatchSummaries.Add(MakeShared<FJsonValueObject>(SummaryObj));
			}
		}
	};

	if (bIncludeSystemScope)
	{
		CollectMatchesForScope(SystemViewModel->GetSystemStackViewModel(), TEXT("system"), SystemViewModel->GetDisplayName().ToString(), FGuid());
	}

	if (bIncludeEmitters)
	{
		const TArray<TSharedRef<FNiagaraEmitterHandleViewModel>>& EmitterHandleViewModels = SystemViewModel->GetEmitterHandleViewModels();
		for (int32 EmitterIndex = 0; EmitterIndex < EmitterHandleViewModels.Num(); ++EmitterIndex)
		{
			const TSharedRef<FNiagaraEmitterHandleViewModel>& EmitterHandleViewModel = EmitterHandleViewModels[EmitterIndex];
			const FString EmitterName = EmitterHandleViewModel->GetNameText().ToString();
			const FGuid EmitterHandleId = EmitterHandleViewModel->GetId();

			if (EmitterIndexFilter != INDEX_NONE && EmitterIndex != EmitterIndexFilter)
			{
				continue;
			}
			if (bHasEmitterNameFilter && EmitterName != EmitterNameFilter)
			{
				continue;
			}
			if (bHasEmitterIdFilter && EmitterHandleId != EmitterIdFilter)
			{
				continue;
			}

			CollectMatchesForScope(EmitterHandleViewModel->GetEmitterStackViewModel(), TEXT("emitter"), EmitterName, EmitterHandleId);
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	OutData->SetStringField(TEXT("view_model_source"), ViewModelSource);
	OutData->SetNumberField(TEXT("visited_scope_count"), VisitedScopeCount);
	OutData->SetNumberField(TEXT("visited_issue_count"), VisitedIssueCount);
	OutData->SetNumberField(TEXT("matching_issue_count"), Matches.Num());
	OutData->SetArrayField(TEXT("matches"), MatchSummaries);

	if (Matches.Num() == 0)
	{
		OutError = TEXT("no_matching_stack_issue_fix");
		return false;
	}
	if (Matches.Num() > 1 && !bApplyFirstMatch)
	{
		OutError = TEXT("multiple_matching_stack_issue_fixes");
		return false;
	}

	FMatchedStackIssueFix Match = Matches[0];
	if (!Match.Fix.IsValid() || !Match.Fix.GetFixDelegate().IsBound())
	{
		OutError = TEXT("selected_stack_issue_fix_has_no_delegate");
		return false;
	}

	TSharedPtr<FJsonObject> AppliedObj = UeAgentNiagaraOps::BuildStackIssueJson(*Match.Entry, Match.Issue, Match.ScopeType, Match.ScopeName, Match.EmitterHandleId, Match.EntryPath);
	AppliedObj->SetNumberField(TEXT("selected_fix_index"), Match.FixIndex);
	AppliedObj->SetStringField(TEXT("selected_fix_description"), Match.Fix.GetDescription().ToString());
	AppliedObj->SetStringField(TEXT("selected_fix_unique_identifier"), Match.Fix.GetUniqueIdentifier());

	NiagaraSystem->Modify();
	UNiagaraStackEntry::FStackIssueFixDelegate FixDelegate = Match.Fix.GetFixDelegate();
	FixDelegate.Execute();
	const UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult =
		UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*NiagaraSystem, true, true);

	if (Match.StackViewModel)
	{
		if (UNiagaraStackEntry* RootEntry = Match.StackViewModel->GetRootEntry())
		{
			RootEntry->RefreshChildren();
			RootEntry->RefreshFilteredChildren();
		}
	}

	bool bForceCompile = false;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);

	bool bCompileAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);

	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);

	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
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

	OutData->SetObjectField(TEXT("applied_issue"), AppliedObj);
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	OutData->SetBoolField(TEXT("applied"), true);
	OutData->SetBoolField(TEXT("compile_after_apply"), bCompileAfterApply);
	OutData->SetBoolField(TEXT("force_compile"), bForceCompile);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("has_outstanding_compilation_requests"), NiagaraSystem->HasOutstandingCompilationRequests(true));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterClearParent(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	bool bClearMergeMessage = true;
	JsonTryGetBool(Ctx.Params, TEXT("clear_merge_message"), bClearMergeMessage);

	bool bForce = false;
	JsonTryGetBool(Ctx.Params, TEXT("force"), bForce);

	auto BuildVersionedEmitterJson = [](const FVersionedNiagaraEmitter& VersionedEmitter) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("has_emitter"), VersionedEmitter.Emitter != nullptr);
		Obj->SetStringField(TEXT("emitter_path"), VersionedEmitter.Emitter ? VersionedEmitter.Emitter->GetPathName() : FString());
		Obj->SetStringField(TEXT("version"), VersionedEmitter.Version.IsValid() ? VersionedEmitter.Version.ToString(EGuidFormats::DigitsWithHyphensLower) : FString());
		return Obj;
	};

	const FVersionedNiagaraEmitter ParentBefore = EditContext.EmitterData->GetParent();
	const FVersionedNiagaraEmitter ParentAtLastMergeBefore = EditContext.EmitterData->GetParentAtLastMerge();
	const bool bHadParent = ParentBefore.Emitter != nullptr || ParentAtLastMergeBefore.Emitter != nullptr;
	const FGuid EmitterMergeMessageId(0xDAF26E73, 0x4D1B416B, 0x815FA6C2, 0x6D5D0A75);
	const bool bHadMergeMessage = EditContext.Emitter->GetMessageStore().GetMessages().Contains(EmitterMergeMessageId);

	if (!bHadParent && !bHadMergeMessage && !bForce)
	{
		OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("object_path"), EditContext.Emitter->GetPathName());
		OutData->SetObjectField(TEXT("parent_before"), BuildVersionedEmitterJson(ParentBefore));
		OutData->SetObjectField(TEXT("parent_at_last_merge_before"), BuildVersionedEmitterJson(ParentAtLastMergeBefore));
		OutData->SetBoolField(TEXT("had_parent"), false);
		OutData->SetBoolField(TEXT("had_merge_message"), false);
		OutData->SetBoolField(TEXT("changed"), false);
		return true;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("Clear Niagara emitter parent")));
	EditContext.Emitter->Modify();
	if (UNiagaraSystem* OwningSystem = EditContext.Emitter->GetTypedOuter<UNiagaraSystem>())
	{
		OwningSystem->Modify();
	}

	EditContext.EmitterData->RemoveParent();
	if (bClearMergeMessage && bHadMergeMessage)
	{
		EditContext.Emitter->GetMessageStore().RemoveMessage(EmitterMergeMessageId);
	}
	EditContext.EmitterData->InvalidateCompileResults();
	const UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult =
		UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	UNiagaraSystem* OwningSystem = EditContext.Emitter->GetTypedOuter<UNiagaraSystem>();

	bool bForceCompile = false;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);

	bool bCompileAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);

	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);

	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterApply && OwningSystem)
	{
		bCompilationRequested = OwningSystem->RequestCompile(bForceCompile);
		if (bWaitForComplete)
		{
			OwningSystem->WaitForCompilationComplete(true, false);
			OwningSystem->PollForCompilationComplete(true);
			OwningSystem->CacheFromCompiledData();
			bCompilationComplete = !OwningSystem->HasOutstandingCompilationRequests(true);
		}
	}

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (bSaveAfterApply)
	{
		UObject* SaveTarget = OwningSystem ? static_cast<UObject*>(OwningSystem) : static_cast<UObject*>(EditContext.Emitter);
		if (!UeAgentNiagaraOps::SaveAssetPackage(SaveTarget, OutError))
		{
			return false;
		}
	}

	const FVersionedNiagaraEmitter ParentAfter = EditContext.EmitterData->GetParent();
	const FVersionedNiagaraEmitter ParentAtLastMergeAfter = EditContext.EmitterData->GetParentAtLastMerge();
	const bool bHasMergeMessageAfter = EditContext.Emitter->GetMessageStore().GetMessages().Contains(EmitterMergeMessageId);

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), EditContext.Emitter->GetPathName());
	OutData->SetStringField(TEXT("owning_system_path"), OwningSystem ? OwningSystem->GetPathName() : FString());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetObjectField(TEXT("parent_before"), BuildVersionedEmitterJson(ParentBefore));
	OutData->SetObjectField(TEXT("parent_at_last_merge_before"), BuildVersionedEmitterJson(ParentAtLastMergeBefore));
	OutData->SetObjectField(TEXT("parent_after"), BuildVersionedEmitterJson(ParentAfter));
	OutData->SetObjectField(TEXT("parent_at_last_merge_after"), BuildVersionedEmitterJson(ParentAtLastMergeAfter));
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	OutData->SetBoolField(TEXT("had_parent"), bHadParent);
	OutData->SetBoolField(TEXT("had_merge_message"), bHadMergeMessage);
	OutData->SetBoolField(TEXT("has_merge_message_after"), bHasMergeMessageAfter);
	OutData->SetBoolField(TEXT("cleared_merge_message"), bClearMergeMessage && bHadMergeMessage);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("compile_after_apply"), bCompileAfterApply);
	OutData->SetBoolField(TEXT("force_compile"), bForceCompile);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	if (!Cast<UNiagaraSystem>(Asset) && !Cast<UNiagaraEmitter>(Asset) && !Cast<UNiagaraScript>(Asset))
	{
		OutError = TEXT("asset_is_not_niagara");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("asset_kind"), UeAgentNiagaraOps::GetNiagaraAssetKind(Asset));

	if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset))
	{
		OutData->SetNumberField(TEXT("emitter_handle_count"), NiagaraSystem->GetEmitterHandles().Num());
		OutData->SetBoolField(TEXT("has_fixed_bounds"), NiagaraSystem->GetFixedBounds().IsValid != 0);
		if (UNiagaraSystemEditorData* SystemEditorData = Cast<UNiagaraSystemEditorData>(NiagaraSystem->GetEditorData()))
		{
			UEdGraph* OverviewGraph = SystemEditorData->GetSystemOverviewGraph();
			OutData->SetNumberField(TEXT("overview_node_count"), OverviewGraph ? OverviewGraph->Nodes.Num() : 0);
		}

		TArray<FNiagaraVariable> UserParameters;
		NiagaraSystem->GetExposedParameters().GetUserParameters(UserParameters);

		TArray<TSharedPtr<FJsonValue>> ParametersJson;
		ParametersJson.Reserve(UserParameters.Num());
		for (const FNiagaraVariable& Parameter : UserParameters)
		{
			TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
			ParamObj->SetStringField(TEXT("name"), Parameter.GetName().ToString());
			ParamObj->SetStringField(TEXT("type"), Parameter.GetType().GetNameText().ToString());
			ParametersJson.Add(MakeShared<FJsonValueObject>(ParamObj));
		}
		OutData->SetArrayField(TEXT("user_parameters"), ParametersJson);
		OutData->SetNumberField(TEXT("user_parameter_count"), ParametersJson.Num());
	}
	else if (UNiagaraEmitter* NiagaraEmitter = Cast<UNiagaraEmitter>(Asset))
	{
		const FNiagaraAssetVersion ExposedVersion = NiagaraEmitter->GetExposedVersion();
		OutData->SetStringField(TEXT("exposed_version"), ExposedVersion.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetNumberField(TEXT("major_version"), ExposedVersion.MajorVersion);
		OutData->SetNumberField(TEXT("minor_version"), ExposedVersion.MinorVersion);
	}
	else if (UNiagaraScript* NiagaraScript = Cast<UNiagaraScript>(Asset))
	{
		const FNiagaraAssetVersion ExposedVersion = NiagaraScript->GetExposedVersion();
		OutData->SetStringField(TEXT("exposed_version"), ExposedVersion.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetNumberField(TEXT("major_version"), ExposedVersion.MajorVersion);
		OutData->SetNumberField(TEXT("minor_version"), ExposedVersion.MinorVersion);
		if (const UEnum* ScriptUsageEnum = StaticEnum<ENiagaraScriptUsage>())
		{
			OutData->SetStringField(TEXT("usage"), ScriptUsageEnum->GetNameStringByValue((int64)NiagaraScript->GetUsage()));
		}
		else
		{
			OutData->SetStringField(TEXT("usage"), TEXT(""));
		}
	}

	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemGetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyName = UeAgentNiagaraOps::NormalizeEventHandlerPropertyName(PropertyName);

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	FString ValueText;
	if (!UeAgentNiagaraOps::GetObjectPropertyText(NiagaraSystem, PropertyName, ValueText))
	{
		OutError = TEXT("get_property_failed");
		return false;
	}

	FProperty* ResolvedProperty = nullptr;
	void* ValuePtr = nullptr;
	UeAgentNiagaraOps::ResolvePropertyPath(NiagaraSystem, PropertyName, ResolvedProperty, ValuePtr);

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("cpp_type"), ResolvedProperty ? ResolvedProperty->GetCPPType() : TEXT(""));
	OutData->SetBoolField(TEXT("property_found"), ResolvedProperty != nullptr);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}

	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	if (!Cast<UNiagaraSystem>(Asset) && !Cast<UNiagaraEmitter>(Asset) && !Cast<UNiagaraScript>(Asset))
	{
		OutError = TEXT("asset_is_not_niagara");
		return false;
	}

	FString AppliedValueText;
	FString CppType;
	FString ImportError;
	if (!UeAgentNiagaraOps::SetObjectPropertyText(Asset, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		ResultObj->SetStringField(TEXT("cpp_type"), CppType);
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		OutData->SetObjectField(TEXT("property_import_result"), ResultObj);
		OutError = ImportError.IsEmpty() ? FString::Printf(TEXT("set_property_failed:%s:%s"), *PropertyName, *ValueText) : ImportError;
		return false;
	}

	Asset->MarkPackageDirty();
	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(Asset, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("asset_kind"), UeAgentNiagaraOps::GetNiagaraAssetKind(Asset));
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraRefreshSystem(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("niagara_system_not_found");
		return false;
	}
	UPackage* NiagaraSystemPackage = NiagaraSystem->GetOutermost();
	const bool bWasPackageDirtyBeforeRefresh = NiagaraSystemPackage ? NiagaraSystemPackage->IsDirty() : false;

	bool bBroadcastPostEditChange = true;
	JsonTryGetBool(Ctx.Params, TEXT("broadcast_post_edit_change"), bBroadcastPostEditChange);
	bool bMarkDirty = true;
	JsonTryGetBool(Ctx.Params, TEXT("mark_dirty"), bMarkDirty);
	bool bCompileAfterRefresh = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_refresh"), bCompileAfterRefresh);
	bool bForceCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);

	const UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult =
		UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*NiagaraSystem, bBroadcastPostEditChange, bMarkDirty);

	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterRefresh)
	{
		bCompilationRequested = NiagaraSystem->RequestCompile(bForceCompile);
		if (bWaitForComplete)
		{
			NiagaraSystem->WaitForCompilationComplete(true, false);
			NiagaraSystem->PollForCompilationComplete(true);
			NiagaraSystem->CacheFromCompiledData();
			bCompilationComplete = !NiagaraSystem->HasOutstandingCompilationRequests(true);
		}
	}

	bool bSaveAfterRefresh = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_refresh"), bSaveAfterRefresh);
	bool bRestoredDirtyState = false;
	if (!bMarkDirty && !bSaveAfterRefresh && NiagaraSystemPackage && !bWasPackageDirtyBeforeRefresh && NiagaraSystemPackage->IsDirty())
	{
		NiagaraSystemPackage->SetDirtyFlag(false);
		bRestoredDirtyState = true;
	}
	if (bSaveAfterRefresh)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), NiagaraSystem->GetPathName());
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	OutData->SetBoolField(TEXT("broadcast_post_edit_change"), bBroadcastPostEditChange);
	OutData->SetBoolField(TEXT("mark_dirty"), bMarkDirty);
	OutData->SetBoolField(TEXT("compile_after_refresh"), bCompileAfterRefresh);
	OutData->SetBoolField(TEXT("force_compile"), bForceCompile);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("has_outstanding_compilation_requests"), NiagaraSystem->HasOutstandingCompilationRequests(true));
	OutData->SetBoolField(TEXT("is_ready_to_run"), NiagaraSystem->IsReadyToRun());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRefresh);
	OutData->SetBoolField(TEXT("restored_dirty_state"), bRestoredDirtyState);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraCompileSystem(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	UPackage* AssetPackage = Asset->GetOutermost();
	const bool bWasPackageDirtyBeforeCompile = AssetPackage ? AssetPackage->IsDirty() : false;

	bool bForceCompile = false;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bRefreshBeforeCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("refresh_before_compile"), bRefreshBeforeCompile);
	bool bSaveAfterCompile = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_compile"), bSaveAfterCompile);
	bool bMarkDirtyAfterCompile = bSaveAfterCompile;
	JsonTryGetBool(Ctx.Params, TEXT("mark_dirty_after_compile"), bMarkDirtyAfterCompile);

	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	FString CompileTarget;
	UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult;

	if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset))
	{
		if (bRefreshBeforeCompile)
		{
			RefreshResult = UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*NiagaraSystem, true, bMarkDirtyAfterCompile);
		}
		bCompilationRequested = NiagaraSystem->RequestCompile(bForceCompile);
		if (bWaitForComplete)
		{
			NiagaraSystem->WaitForCompilationComplete(true, false);
			NiagaraSystem->PollForCompilationComplete(true);
			NiagaraSystem->CacheFromCompiledData();
			bCompilationComplete = !NiagaraSystem->HasOutstandingCompilationRequests(true);
		}
		CompileTarget = TEXT("system");
	}
	else if (UNiagaraScript* NiagaraScript = Cast<UNiagaraScript>(Asset))
	{
		NiagaraScript->RequestCompile(NiagaraScript->GetExposedVersion().VersionGuid, bForceCompile);
		bCompilationRequested = true;
		bCompilationComplete = !bWaitForComplete;
		CompileTarget = TEXT("script");
	}
	else
	{
		OutError = TEXT("niagara_compile_requires_system_or_script_asset");
		return false;
	}

	if (bMarkDirtyAfterCompile)
	{
		Asset->MarkPackageDirty();
	}
	bool bRestoredDirtyState = false;
	if (!bMarkDirtyAfterCompile && AssetPackage && !bWasPackageDirtyBeforeCompile && AssetPackage->IsDirty())
	{
		AssetPackage->SetDirtyFlag(false);
		bRestoredDirtyState = true;
	}
	if (bSaveAfterCompile)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(Asset, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("compile_target"), CompileTarget);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("refresh_before_compile"), bRefreshBeforeCompile);
	OutData->SetBoolField(TEXT("mark_dirty_after_compile"), bMarkDirtyAfterCompile);
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset))
	{
		OutData->SetBoolField(TEXT("has_outstanding_compilation_requests"), NiagaraSystem->HasOutstandingCompilationRequests(true));
		OutData->SetBoolField(TEXT("is_ready_to_run"), NiagaraSystem->IsReadyToRun());
	}
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCompile);
	OutData->SetBoolField(TEXT("restored_dirty_state"), bRestoredDirtyState);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraGetCompileLog(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNiagaraOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}
	UPackage* AssetPackage = Asset->GetOutermost();
	const bool bWasPackageDirtyBeforeRead = AssetPackage ? AssetPackage->IsDirty() : false;

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset);
	UNiagaraEmitter* NiagaraEmitter = Cast<UNiagaraEmitter>(Asset);
	UNiagaraScript* NiagaraScript = Cast<UNiagaraScript>(Asset);
	if (!NiagaraSystem && !NiagaraEmitter && !NiagaraScript)
	{
		OutError = TEXT("asset_is_not_niagara");
		return false;
	}

	bool bCompileBeforeRead = false;
	JsonTryGetBool(Ctx.Params, TEXT("compile_before_read"), bCompileBeforeRead);

	bool bForceCompile = false;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);

	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bRefreshBeforeCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("refresh_before_compile"), bRefreshBeforeCompile);

	bool bIncludeEvents = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_events"), bIncludeEvents);

	bool bIncludeStackGuids = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_stack_guids"), bIncludeStackGuids);

	bool bIncludeDisabledEmitters = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_disabled_emitters"), bIncludeDisabledEmitters);

	int32 MaxEventsPerScript = 200;
	double MaxEventsPerScriptNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_events_per_script"), MaxEventsPerScriptNumber))
	{
		MaxEventsPerScript = FMath::Clamp(FMath::FloorToInt(MaxEventsPerScriptNumber), 0, 5000);
	}

	FString SeverityFilterText = TEXT("all");
	JsonTryGetString(Ctx.Params, TEXT("severity_filter"), SeverityFilterText);

	UeAgentNiagaraOps::ENiagaraCompileLogSeverityFilter SeverityFilter = UeAgentNiagaraOps::ENiagaraCompileLogSeverityFilter::All;
	if (!UeAgentNiagaraOps::ParseCompileLogSeverityFilter(SeverityFilterText, SeverityFilter))
	{
		OutError = TEXT("invalid_severity_filter");
		return false;
	}

	UeAgentNiagaraOps::FNiagaraEmitterEditContext EmitterEditContext;
	if (NiagaraEmitter)
	{
		if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EmitterEditContext, OutError, false))
		{
			return false;
		}
	}

	TArray<UeAgentNiagaraOps::FCollectedNiagaraScriptInfo> CollectedScripts;
	if (NiagaraSystem)
	{
		UeAgentNiagaraOps::CollectSystemScriptsForCompileLog(*NiagaraSystem, bIncludeDisabledEmitters, CollectedScripts);
	}
	else if (NiagaraEmitter)
	{
		TSet<const UNiagaraScript*> SeenScripts;
		TArray<UNiagaraScript*> EmitterScripts;
		EmitterEditContext.EmitterData->GetScripts(EmitterScripts, false, false);
		const FString EmitterVersion = EmitterEditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		for (UNiagaraScript* Script : EmitterScripts)
		{
			UeAgentNiagaraOps::AppendUniqueCollectedScript(
				CollectedScripts,
				SeenScripts,
				Script,
				TEXT("emitter_asset"),
				EmitterEditContext.Emitter->GetName(),
				EmitterEditContext.Emitter->GetPathName(),
				FString(),
				EmitterVersion,
				true);
		}
	}
	else if (NiagaraScript)
	{
		TSet<const UNiagaraScript*> SeenScripts;
		UeAgentNiagaraOps::AppendUniqueCollectedScript(
			CollectedScripts,
			SeenScripts,
			NiagaraScript,
			TEXT("script_asset"),
			NiagaraScript->GetName(),
			NiagaraScript->GetPathName(),
			FString(),
			FString(),
			true);
	}

	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	bool bWaitForCompleteSupported = false;
	UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult;

	if (bCompileBeforeRead)
	{
		if (NiagaraSystem)
		{
			bWaitForCompleteSupported = true;
			if (bRefreshBeforeCompile)
			{
				RefreshResult = UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*NiagaraSystem, true, false);
			}
			bCompilationRequested = NiagaraSystem->RequestCompile(bForceCompile);
			if (bWaitForComplete)
			{
				NiagaraSystem->WaitForCompilationComplete(true, false);
				NiagaraSystem->PollForCompilationComplete(true);
				NiagaraSystem->CacheFromCompiledData();
				bCompilationComplete = !NiagaraSystem->HasOutstandingCompilationRequests(true);
			}
		}
		else if (NiagaraScript)
		{
			NiagaraScript->RequestCompile(NiagaraScript->GetExposedVersion().VersionGuid, bForceCompile);
			bCompilationRequested = true;
			bCompilationComplete = !bWaitForComplete;
		}
		else if (NiagaraEmitter)
		{
			for (const UeAgentNiagaraOps::FCollectedNiagaraScriptInfo& ScriptInfo : CollectedScripts)
			{
				if (!ScriptInfo.Script)
				{
					continue;
				}

				ScriptInfo.Script->RequestCompile(ScriptInfo.Script->GetExposedVersion().VersionGuid, bForceCompile);
				bCompilationRequested = true;
			}
			bCompilationComplete = !bWaitForComplete;
		}
	}
	bool bRestoredDirtyState = false;
	if (AssetPackage && !bWasPackageDirtyBeforeRead && AssetPackage->IsDirty())
	{
		AssetPackage->SetDirtyFlag(false);
		bRestoredDirtyState = true;
	}

	TArray<TSharedPtr<FJsonValue>> ScriptsJson;
	ScriptsJson.Reserve(CollectedScripts.Num());

	int32 TotalCompileEventCount = 0;
	int32 TotalFilteredCompileEventCount = 0;
	int32 ErrorScriptCount = 0;
	int32 WarningScriptCount = 0;
	bool bHasError = false;
	bool bHasWarning = false;

	for (const UeAgentNiagaraOps::FCollectedNiagaraScriptInfo& ScriptInfo : CollectedScripts)
	{
		if (!ScriptInfo.Script)
		{
			continue;
		}

		UNiagaraScript& Script = *ScriptInfo.Script;
		const FNiagaraVMExecutableData& VMExecutableData = Script.GetVMExecutableData();
		const ENiagaraScriptCompileStatus CompileStatus = Script.GetLastCompileStatus();

		const int32 RawCompileEventCount = VMExecutableData.LastCompileEvents.Num();
		TotalCompileEventCount += RawCompileEventCount;

		int32 ErrorEventCount = 0;
		int32 WarningEventCount = 0;
		int32 InfoEventCount = 0;
		int32 FilteredEventCount = 0;

		TArray<TSharedPtr<FJsonValue>> EventsJson;
		if (bIncludeEvents)
		{
			EventsJson.Reserve(FMath::Min(RawCompileEventCount, MaxEventsPerScript));
		}

		for (const FNiagaraCompileEvent& CompileEvent : VMExecutableData.LastCompileEvents)
		{
			if (!UeAgentNiagaraOps::ShouldIncludeCompileEvent(CompileEvent, SeverityFilter))
			{
				continue;
			}

			++FilteredEventCount;
			if (CompileEvent.Severity == FNiagaraCompileEventSeverity::Error)
			{
				++ErrorEventCount;
			}
			else if (CompileEvent.Severity == FNiagaraCompileEventSeverity::Warning)
			{
				++WarningEventCount;
			}
			else
			{
				++InfoEventCount;
			}

			if (bIncludeEvents && EventsJson.Num() < MaxEventsPerScript)
			{
				EventsJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildCompileEventJson(CompileEvent, bIncludeStackGuids)));
			}
		}

		TotalFilteredCompileEventCount += FilteredEventCount;

		const bool bScriptHasError = CompileStatus == ENiagaraScriptCompileStatus::NCS_Error || !VMExecutableData.ErrorMsg.IsEmpty() || ErrorEventCount > 0;
		const bool bScriptHasWarning = UeAgentNiagaraOps::IsCompileStatusWarning(CompileStatus) || WarningEventCount > 0;
		if (bScriptHasError)
		{
			++ErrorScriptCount;
			bHasError = true;
		}
		if (bScriptHasWarning)
		{
			++WarningScriptCount;
			bHasWarning = true;
		}

		TSharedPtr<FJsonObject> ScriptObj = MakeShared<FJsonObject>();
		ScriptObj->SetStringField(TEXT("script_object_path"), Script.GetPathName());
		ScriptObj->SetStringField(TEXT("script_asset_path"), Script.GetOutermost()->GetName());
		ScriptObj->SetStringField(TEXT("script_name"), Script.GetName());
		ScriptObj->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Script.GetUsage()));
		ScriptObj->SetStringField(TEXT("script_usage_id"), Script.GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower));
		ScriptObj->SetStringField(TEXT("compile_status"), UeAgentNiagaraOps::ScriptCompileStatusToString(CompileStatus));
		ScriptObj->SetStringField(TEXT("error_msg"), VMExecutableData.ErrorMsg);
		ScriptObj->SetBoolField(TEXT("has_error"), bScriptHasError);
		ScriptObj->SetBoolField(TEXT("has_warning"), bScriptHasWarning);
		ScriptObj->SetNumberField(TEXT("compile_event_total_count"), RawCompileEventCount);
		ScriptObj->SetNumberField(TEXT("compile_event_filtered_count"), FilteredEventCount);
		ScriptObj->SetNumberField(TEXT("compile_event_emitted_count"), EventsJson.Num());
		ScriptObj->SetNumberField(TEXT("error_event_count"), ErrorEventCount);
		ScriptObj->SetNumberField(TEXT("warning_event_count"), WarningEventCount);
		ScriptObj->SetNumberField(TEXT("info_event_count"), InfoEventCount);
		ScriptObj->SetBoolField(TEXT("events_truncated"), bIncludeEvents && EventsJson.Num() < FilteredEventCount);
		ScriptObj->SetStringField(TEXT("owner_type"), ScriptInfo.OwnerType);
		ScriptObj->SetStringField(TEXT("owner_name"), ScriptInfo.OwnerName);
		ScriptObj->SetStringField(TEXT("owner_path"), ScriptInfo.OwnerPath);
		ScriptObj->SetStringField(TEXT("emitter_handle_id"), ScriptInfo.EmitterHandleId);
		ScriptObj->SetStringField(TEXT("emitter_version"), ScriptInfo.EmitterVersion);
		ScriptObj->SetBoolField(TEXT("emitter_enabled"), ScriptInfo.bEmitterEnabled);

		if (bIncludeEvents)
		{
			ScriptObj->SetArrayField(TEXT("events"), EventsJson);
		}

		ScriptsJson.Add(MakeShared<FJsonValueObject>(ScriptObj));
	}

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_kind"), UeAgentNiagaraOps::GetNiagaraAssetKind(Asset));
	OutData->SetBoolField(TEXT("compile_before_read"), bCompileBeforeRead);
	OutData->SetBoolField(TEXT("refresh_before_compile"), bRefreshBeforeCompile);
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("wait_for_complete"), bWaitForComplete);
	OutData->SetBoolField(TEXT("wait_for_complete_supported"), bWaitForCompleteSupported);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("restored_dirty_state"), bRestoredDirtyState);
	if (NiagaraSystem)
	{
		OutData->SetBoolField(TEXT("has_outstanding_compilation_requests"), NiagaraSystem->HasOutstandingCompilationRequests(true));
		OutData->SetBoolField(TEXT("is_ready_to_run"), NiagaraSystem->IsReadyToRun());
	}
	OutData->SetStringField(TEXT("severity_filter"), SeverityFilterText);
	OutData->SetBoolField(TEXT("include_events"), bIncludeEvents);
	OutData->SetBoolField(TEXT("include_stack_guids"), bIncludeStackGuids);
	OutData->SetNumberField(TEXT("max_events_per_script"), MaxEventsPerScript);
	OutData->SetNumberField(TEXT("script_count"), ScriptsJson.Num());
	OutData->SetNumberField(TEXT("total_compile_event_count"), TotalCompileEventCount);
	OutData->SetNumberField(TEXT("total_compile_event_filtered_count"), TotalFilteredCompileEventCount);
	OutData->SetNumberField(TEXT("error_script_count"), ErrorScriptCount);
	OutData->SetNumberField(TEXT("warning_script_count"), WarningScriptCount);
	OutData->SetBoolField(TEXT("has_error"), bHasError);
	OutData->SetBoolField(TEXT("has_warning"), bHasWarning);
	OutData->SetArrayField(TEXT("scripts"), ScriptsJson);

	if (NiagaraEmitter)
	{
		OutData->SetStringField(TEXT("emitter_version"), EmitterEditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	}

	return true;
}

