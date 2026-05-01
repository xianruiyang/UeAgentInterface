bool FUeAgentHttpServer::CmdMaterialListExpressions(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UMaterial* Material = UeAgentMaterialOps::LoadMaterialAsset(AssetPathInput);
	if (!Material)
	{
		OutError = TEXT("material_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> ExpressionArray;
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		if (!Expression)
		{
			continue;
		}
		ExpressionArray.Add(MakeShared<FJsonValueObject>(UeAgentMaterialOps::ExpressionToJson(Expression)));
	}

	OutData->SetStringField(TEXT("asset_path"), Material->GetOutermost()->GetName());
	OutData->SetArrayField(TEXT("expressions"), ExpressionArray);
	OutData->SetNumberField(TEXT("count"), ExpressionArray.Num());
	return true;
}

bool FUeAgentHttpServer::CmdMaterialAddExpression(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString ExpressionClassPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("expression_class"), ExpressionClassPath) || ExpressionClassPath.IsEmpty())
	{
		OutError = TEXT("missing_expression_class");
		return false;
	}

	UMaterial* Material = UeAgentMaterialOps::LoadMaterialAsset(AssetPathInput);
	if (!Material)
	{
		OutError = TEXT("material_not_found");
		return false;
	}

	UClass* ExpressionClass = UeAgentMaterialOps::ResolveClassByPath(ExpressionClassPath);
	if (!ExpressionClass || !ExpressionClass->IsChildOf(UMaterialExpression::StaticClass()))
	{
		OutError = TEXT("invalid_expression_class");
		return false;
	}

	double NodePosX = 0.0;
	double NodePosY = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("node_pos_x"), NodePosX);
	JsonTryGetNumber(Ctx.Params, TEXT("node_pos_y"), NodePosY);

	UMaterialExpression* NewExpression = UMaterialEditingLibrary::CreateMaterialExpression(Material, ExpressionClass, (int32)NodePosX, (int32)NodePosY);
	if (!NewExpression)
	{
		OutError = TEXT("create_expression_failed");
		return false;
	}

	FString ExpressionName;
	if (JsonTryGetString(Ctx.Params, TEXT("expression_name"), ExpressionName) && !ExpressionName.IsEmpty())
	{
		NewExpression->Rename(*ExpressionName, Material);
	}

	Material->MarkPackageDirty();
	Material->PostEditChange();

	bool bCompileAfterAdd = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);
	if (bCompileAfterAdd)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}

	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (bSaveAfterAdd && !UeAgentMaterialOps::SaveAssetPackage(Material, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Material->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("expression"), UeAgentMaterialOps::ExpressionToJson(NewExpression));
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterAdd);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialSetExpressionProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	UMaterial* Material = UeAgentMaterialOps::LoadMaterialAsset(AssetPathInput);
	if (!Material)
	{
		OutError = TEXT("material_not_found");
		return false;
	}

	UMaterialExpression* Expression = UeAgentMaterialOps::FindExpressionByParams(Material, Ctx.Params);
	if (!Expression)
	{
		OutError = TEXT("material_expression_not_found");
		return false;
	}

	FString AppliedValueText;
	FString CppType;
	FString ImportError;
	FString PropertyImportStatus = TEXT("imported_and_read_back");
	if (PropertyName.Equals(TEXT("MaterialFunction"), ESearchCase::IgnoreCase))
	{
		UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
		if (!FunctionCall)
		{
			OutError = TEXT("material_expression_not_function_call");
			return false;
		}

		UMaterialFunctionInterface* MaterialFunction = Cast<UMaterialFunctionInterface>(UeAgentMaterialOps::LoadAssetObject(ValueText));
		if (!MaterialFunction)
		{
			OutError = TEXT("material_function_not_found");
			return false;
		}

		if (!FunctionCall->SetMaterialFunction(MaterialFunction))
		{
			OutError = TEXT("set_material_function_failed");
			return false;
		}

		FunctionCall->UpdateFromFunctionResource();
		AppliedValueText = FunctionCall->MaterialFunction ? FunctionCall->MaterialFunction->GetPathName() : TEXT("");
		CppType = TEXT("UMaterialFunctionInterface*");
		PropertyImportStatus = TEXT("assigned_object_reference");
	}
	else if (!UeAgentMaterialOps::SetObjectPropertyText(Expression, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		ResultObj->SetStringField(TEXT("cpp_type"), CppType);
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		OutData->SetObjectField(TEXT("property_import_result"), ResultObj);
		OutError = ImportError.IsEmpty() ? FString::Printf(TEXT("set_expression_property_failed:%s:%s"), *PropertyName, *ValueText) : ImportError;
		return false;
	}

	Material->MarkPackageDirty();
	Material->PostEditChange();

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	if (bCompileAfterSet)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentMaterialOps::SaveAssetPackage(Material, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Material->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("expression_guid"), Expression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("expression_name"), Expression->GetName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, PropertyImportStatus);
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialConnectExpressions(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UMaterial* Material = UeAgentMaterialOps::LoadMaterialAsset(AssetPathInput);
	if (!Material)
	{
		OutError = TEXT("material_not_found");
		return false;
	}

	UMaterialExpression* FromExpression = UeAgentMaterialOps::FindExpressionBySelector(Material, Ctx.Params, TEXT("from_expression"));
	UMaterialExpression* ToExpression = UeAgentMaterialOps::FindExpressionBySelector(Material, Ctx.Params, TEXT("to_expression"));
	if (!FromExpression || !ToExpression)
	{
		OutError = TEXT("material_expression_not_found");
		return false;
	}

	FString FromOutputName;
	FString ToInputName;
	JsonTryGetString(Ctx.Params, TEXT("from_output_name"), FromOutputName);
	JsonTryGetString(Ctx.Params, TEXT("to_input_name"), ToInputName);

	const bool bConnected = UMaterialEditingLibrary::ConnectMaterialExpressions(FromExpression, FromOutputName, ToExpression, ToInputName);
	if (!bConnected && !UeAgentMaterialOps::ManualConnectExpressions(FromExpression, FromOutputName, ToExpression, ToInputName))
	{
		OutError = TEXT("connect_expressions_failed");
		return false;
	}

	Material->MarkPackageDirty();
	Material->PostEditChange();

	bool bCompileAfterConnect = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_connect"), bCompileAfterConnect);
	if (bCompileAfterConnect)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}

	bool bSaveAfterConnect = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_connect"), bSaveAfterConnect);
	if (bSaveAfterConnect && !UeAgentMaterialOps::SaveAssetPackage(Material, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Material->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("from_expression_guid"), FromExpression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("to_expression_guid"), ToExpression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("from_output_name"), FromOutputName);
	OutData->SetStringField(TEXT("to_input_name"), ToInputName);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterConnect);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterConnect);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialConnectExpressionToProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString MaterialPropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("material_property"), MaterialPropertyName) || MaterialPropertyName.IsEmpty())
	{
		OutError = TEXT("missing_material_property");
		return false;
	}

	UMaterial* Material = UeAgentMaterialOps::LoadMaterialAsset(AssetPathInput);
	if (!Material)
	{
		OutError = TEXT("material_not_found");
		return false;
	}

	UMaterialExpression* Expression = UeAgentMaterialOps::FindExpressionByParams(Material, Ctx.Params);
	if (!Expression)
	{
		OutError = TEXT("material_expression_not_found");
		return false;
	}

	EMaterialProperty MaterialProperty = MP_BaseColor;
	if (!UeAgentMaterialOps::ParseMaterialProperty(MaterialPropertyName, MaterialProperty))
	{
		OutError = TEXT("invalid_material_property");
		return false;
	}

	FString OutputName;
	JsonTryGetString(Ctx.Params, TEXT("output_name"), OutputName);
	const bool bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(Expression, OutputName, MaterialProperty);
	if (!bConnected)
	{
		OutError = TEXT("connect_expression_failed");
		return false;
	}

	Material->MarkPackageDirty();
	Material->PostEditChange();

	bool bCompileAfterConnect = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_connect"), bCompileAfterConnect);
	if (bCompileAfterConnect)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}

	bool bSaveAfterConnect = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_connect"), bSaveAfterConnect);
	if (bSaveAfterConnect && !UeAgentMaterialOps::SaveAssetPackage(Material, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Material->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("material_property"), MaterialPropertyName);
	OutData->SetStringField(TEXT("expression_guid"), Expression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("expression_name"), Expression->GetName());
	OutData->SetStringField(TEXT("output_name"), OutputName);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterConnect);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterConnect);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialDeleteExpression(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UMaterial* Material = UeAgentMaterialOps::LoadMaterialAsset(AssetPathInput);
	if (!Material)
	{
		OutError = TEXT("material_not_found");
		return false;
	}

	UMaterialExpression* Expression = UeAgentMaterialOps::FindExpressionByParams(Material, Ctx.Params);
	if (!Expression)
	{
		OutError = TEXT("material_expression_not_found");
		return false;
	}

	const FString DeletedGuid = Expression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString DeletedName = Expression->GetName();
	UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);

	Material->MarkPackageDirty();
	Material->PostEditChange();

	bool bCompileAfterDelete = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_delete"), bCompileAfterDelete);
	if (bCompileAfterDelete)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}

	bool bSaveAfterDelete = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_delete"), bSaveAfterDelete);
	if (bSaveAfterDelete && !UeAgentMaterialOps::SaveAssetPackage(Material, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Material->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("deleted_expression_guid"), DeletedGuid);
	OutData->SetStringField(TEXT("deleted_expression_name"), DeletedName);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterDelete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterDelete);
	return true;
}
