bool FUeAgentHttpServer::ExecuteMaterialCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("material_create")) return CmdMaterialCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_instance_create")) return CmdMaterialInstanceCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_open_editor")) return CmdMaterialOpenEditor(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_get_info")) return CmdMaterialGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_compile")) return CmdMaterialCompile(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_get_compile_log")) return CmdMaterialGetCompileLog(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_set_property")) return CmdMaterialSetProperty(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_set_instance_parent")) return CmdMaterialSetInstanceParent(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_set_parameter")) return CmdMaterialSetParameter(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_set_scalar_parameter")) return CmdMaterialSetScalarParameter(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_set_vector_parameter")) return CmdMaterialSetVectorParameter(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_set_texture_parameter")) return CmdMaterialSetTextureParameter(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_set_static_switch_parameter")) return CmdMaterialSetStaticSwitchParameter(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_list_expressions")) return CmdMaterialListExpressions(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_add_expression")) return CmdMaterialAddExpression(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_set_expression_property")) return CmdMaterialSetExpressionProperty(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_connect_expressions")) return CmdMaterialConnectExpressions(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_connect_expression_to_property")) return CmdMaterialConnectExpressionToProperty(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_delete_expression")) return CmdMaterialDeleteExpression(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_export_folder")) return CmdMaterialExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_apply_folder")) return CmdMaterialApplyFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_instance_export_folder")) return CmdMaterialInstanceExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_instance_apply_folder")) return CmdMaterialInstanceApplyFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_function_export_folder")) return CmdMaterialFunctionExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("material_function_apply_folder")) return CmdMaterialFunctionApplyFolder(Ctx, OutData, OutError);

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("command"), CommandLower);
	OutData->SetStringField(TEXT("category"), TEXT("material"));
	OutError = TEXT("unknown_material_command");
	return false;
}

bool FUeAgentHttpServer::CmdMaterialCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = UeAgentMaterialOps::NormalizeAssetPath(AssetPathInput);
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
	if (UeAgentMaterialOps::AssetExists(ObjectPath))
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

	UMaterial* Material = NewObject<UMaterial>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Material)
	{
		OutError = TEXT("create_material_failed");
		return false;
	}

	FAssetRegistryModule::AssetCreated(Material);
	Material->MarkPackageDirty();
	Material->PostEditChange();

	bool bCompileAfterCreate = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_create"), bCompileAfterCreate);
	if (bCompileAfterCreate)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}

	bool bSaveAfterCreate = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentMaterialOps::SaveAssetPackage(Material, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Material->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Material->GetPathName());
	OutData->SetStringField(TEXT("class"), Material->GetClass()->GetPathName());
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterCreate);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialInstanceCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = UeAgentMaterialOps::NormalizeAssetPath(AssetPathInput);
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
	if (UeAgentMaterialOps::AssetExists(ObjectPath))
	{
		OutError = TEXT("asset_already_exists");
		return false;
	}

	FString ParentPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("parent_material_path"), ParentPath) || ParentPath.IsEmpty())
	{
		OutError = TEXT("missing_parent_material_path");
		return false;
	}
	UMaterialInterface* ParentMaterial = UeAgentMaterialOps::LoadMaterialInterface(ParentPath);
	if (!ParentMaterial)
	{
		OutError = TEXT("parent_material_not_found");
		return false;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = TEXT("create_package_failed");
		return false;
	}

	UMaterialInstanceConstant* MaterialInstance = NewObject<UMaterialInstanceConstant>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!MaterialInstance)
	{
		OutError = TEXT("create_material_instance_failed");
		return false;
	}

	UMaterialEditingLibrary::SetMaterialInstanceParent(MaterialInstance, ParentMaterial);
	UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);

	FAssetRegistryModule::AssetCreated(MaterialInstance);
	MaterialInstance->MarkPackageDirty();
	MaterialInstance->PostEditChange();

	bool bSaveAfterCreate = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentMaterialOps::SaveAssetPackage(MaterialInstance, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), MaterialInstance->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), MaterialInstance->GetPathName());
	OutData->SetStringField(TEXT("class"), MaterialInstance->GetClass()->GetPathName());
	OutData->SetStringField(TEXT("parent_path"), ParentMaterial->GetPathName());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentMaterialOps::LoadAssetObject(AssetPathInput);
	if (!Asset || !Asset->IsA<UMaterialInterface>())
	{
		OutError = TEXT("material_asset_not_found");
		return false;
	}

	if (!GEditor)
	{
		OutError = TEXT("missing_editor");
		return false;
	}
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		OutError = TEXT("missing_asset_editor_subsystem");
		return false;
	}

	AssetEditorSubsystem->OpenEditorForAsset(Asset);
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
	return true;
}

bool FUeAgentHttpServer::CmdMaterialGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentMaterialOps::LoadAssetObject(AssetPathInput);
	UMaterialInterface* MaterialInterface = Cast<UMaterialInterface>(Asset);
	if (!MaterialInterface)
	{
		OutError = TEXT("material_asset_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
	OutData->SetBoolField(TEXT("dirty"), Asset->GetOutermost()->IsDirty());
	OutData->SetBoolField(TEXT("is_material_instance"), Cast<UMaterialInstanceConstant>(Asset) != nullptr);

	if (UMaterial* Material = Cast<UMaterial>(Asset))
	{
		OutData->SetNumberField(TEXT("blend_mode"), (int32)Material->GetBlendMode());
		OutData->SetNumberField(TEXT("shading_model"), (int32)Material->GetShadingModels().GetFirstShadingModel());
		OutData->SetNumberField(TEXT("expression_count"), Material->GetExpressions().Num());
	}
	if (UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(Asset))
	{
		OutData->SetStringField(TEXT("parent_path"), MaterialInstance->Parent ? MaterialInstance->Parent->GetPathName() : TEXT(""));
	}

	TArray<FName> ScalarNames;
	TArray<FName> VectorNames;
	TArray<FName> TextureNames;
	TArray<FName> StaticSwitchNames;
	TArray<FMaterialParameterInfo> StaticComponentMaskInfos;
	TArray<FGuid> StaticComponentMaskIds;
	UMaterialEditingLibrary::GetScalarParameterNames(MaterialInterface, ScalarNames);
	UMaterialEditingLibrary::GetVectorParameterNames(MaterialInterface, VectorNames);
	UMaterialEditingLibrary::GetTextureParameterNames(MaterialInterface, TextureNames);
	UMaterialEditingLibrary::GetStaticSwitchParameterNames(MaterialInterface, StaticSwitchNames);
	MaterialInterface->GetAllStaticComponentMaskParameterInfo(StaticComponentMaskInfos, StaticComponentMaskIds);

	TArray<TSharedPtr<FJsonValue>> Scalars;
	for (const FName ParamName : ScalarNames)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), ParamName.ToString());
		float Value = 0.0f;
		if (UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(Asset))
		{
			Value = UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(MaterialInstance, ParamName);
		}
		else if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
			Value = UMaterialEditingLibrary::GetMaterialDefaultScalarParameterValue(Material, ParamName);
		}
		ParamObj->SetNumberField(TEXT("value"), Value);
		Scalars.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	OutData->SetArrayField(TEXT("scalar_parameters"), Scalars);

	TArray<TSharedPtr<FJsonValue>> Vectors;
	for (const FName ParamName : VectorNames)
	{
		FLinearColor Value = FLinearColor::Black;
		if (UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(Asset))
		{
			Value = UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(MaterialInstance, ParamName);
		}
		else if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
			Value = UMaterialEditingLibrary::GetMaterialDefaultVectorParameterValue(Material, ParamName);
		}

		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), ParamName.ToString());
		TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
		ColorObj->SetNumberField(TEXT("r"), Value.R);
		ColorObj->SetNumberField(TEXT("g"), Value.G);
		ColorObj->SetNumberField(TEXT("b"), Value.B);
		ColorObj->SetNumberField(TEXT("a"), Value.A);
		ParamObj->SetObjectField(TEXT("value"), ColorObj);
		Vectors.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	OutData->SetArrayField(TEXT("vector_parameters"), Vectors);

	TArray<TSharedPtr<FJsonValue>> Textures;
	for (const FName ParamName : TextureNames)
	{
		UTexture* Value = nullptr;
		if (UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(Asset))
		{
			Value = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(MaterialInstance, ParamName);
		}
		else if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
			Value = UMaterialEditingLibrary::GetMaterialDefaultTextureParameterValue(Material, ParamName);
		}

		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), ParamName.ToString());
		ParamObj->SetStringField(TEXT("texture_path"), Value ? Value->GetPathName() : TEXT(""));
		Textures.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	OutData->SetArrayField(TEXT("texture_parameters"), Textures);

	TArray<TSharedPtr<FJsonValue>> StaticSwitches;
	for (const FName ParamName : StaticSwitchNames)
	{
		bool bValue = false;
		if (UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(Asset))
		{
			bValue = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, ParamName);
		}
		else if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
			bValue = UMaterialEditingLibrary::GetMaterialDefaultStaticSwitchParameterValue(Material, ParamName);
		}

		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), ParamName.ToString());
		ParamObj->SetBoolField(TEXT("value"), bValue);
		StaticSwitches.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	OutData->SetArrayField(TEXT("static_switch_parameters"), StaticSwitches);

	TArray<TSharedPtr<FJsonValue>> StaticComponentMasks;
	for (int32 Index = 0; Index < StaticComponentMaskInfos.Num(); ++Index)
	{
		const FMaterialParameterInfo& ParamInfo = StaticComponentMaskInfos[Index];
		bool bR = false;
		bool bG = false;
		bool bB = false;
		bool bA = false;
		FGuid ExpressionGuid = StaticComponentMaskIds.IsValidIndex(Index) ? StaticComponentMaskIds[Index] : FGuid();
		const bool bResolved = UeAgentMaterialOps::ResolveStaticComponentMaskValue(
			MaterialInterface,
			ParamInfo,
			ExpressionGuid,
			bR,
			bG,
			bB,
			bA,
			ExpressionGuid);

		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), ParamInfo.Name.ToString());
		ParamObj->SetBoolField(TEXT("r"), bR);
		ParamObj->SetBoolField(TEXT("g"), bG);
		ParamObj->SetBoolField(TEXT("b"), bB);
		ParamObj->SetBoolField(TEXT("a"), bA);
		ParamObj->SetStringField(TEXT("expression_guid"), ExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		ParamObj->SetBoolField(TEXT("resolved"), bResolved);
		StaticComponentMasks.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	OutData->SetArrayField(TEXT("static_component_mask_parameters"), StaticComponentMasks);

	return true;
}
