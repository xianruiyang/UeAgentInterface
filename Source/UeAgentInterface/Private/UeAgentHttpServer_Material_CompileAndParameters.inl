bool FUeAgentHttpServer::CmdMaterialCompile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentMaterialOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("material_asset_not_found");
		return false;
	}
	UMaterialInterface* MaterialInterface = Cast<UMaterialInterface>(Asset);
	if (!MaterialInterface)
	{
		OutError = TEXT("unsupported_material_asset_type");
		return false;
	}

	bool bCompiled = false;
	if (UMaterial* Material = Cast<UMaterial>(Asset))
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
		bCompiled = true;
	}
	else if (UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(Asset))
	{
		UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
		bCompiled = true;
	}
	else
	{
		OutError = TEXT("unsupported_material_asset_type");
		return false;
	}

	FString SeverityFilterText = TEXT("all");
	JsonTryGetString(Ctx.Params, TEXT("severity_filter"), SeverityFilterText);
	UeAgentMaterialOps::ECompileMessageSeverityFilter SeverityFilter = UeAgentMaterialOps::ECompileMessageSeverityFilter::All;
	if (!UeAgentMaterialOps::ParseCompileSeverityFilter(SeverityFilterText, SeverityFilter))
	{
		OutError = TEXT("invalid_severity_filter");
		return false;
	}

	int32 MaxMessages = 200;
	double MaxMessagesNumber = static_cast<double>(MaxMessages);
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_messages"), MaxMessagesNumber))
	{
		MaxMessages = FMath::Clamp(FMath::RoundToInt(MaxMessagesNumber), 1, 2000);
	}

	bool bIncludeMessages = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_messages"), bIncludeMessages);

	TArray<TSharedPtr<FJsonValue>> Messages;
	int32 ErrorCount = 0;
	UeAgentMaterialOps::CollectMaterialCompileMessages(MaterialInterface, SeverityFilter, MaxMessages, Messages, ErrorCount);

	bool bSaveAfterCompile = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_compile"), bSaveAfterCompile);
	if (bSaveAfterCompile && !UeAgentMaterialOps::SaveAssetPackage(Asset, OutError))
	{
		return false;
	}

	OutData->SetBoolField(TEXT("compiled"), bCompiled);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCompile);
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetNumberField(TEXT("error_count"), ErrorCount);
	OutData->SetNumberField(TEXT("warning_count"), 0);
	OutData->SetNumberField(TEXT("message_total_count"), ErrorCount);
	OutData->SetBoolField(TEXT("has_error"), ErrorCount > 0);

	if (bIncludeMessages)
	{
		OutData->SetStringField(TEXT("severity_filter"), SeverityFilterText);
		OutData->SetNumberField(TEXT("max_messages"), MaxMessages);
		OutData->SetArrayField(TEXT("messages"), Messages);
		OutData->SetNumberField(TEXT("filtered_message_count"), Messages.Num());
	}

	return true;
}

bool FUeAgentHttpServer::CmdMaterialGetCompileLog(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentMaterialOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("material_asset_not_found");
		return false;
	}

	UMaterialInterface* MaterialInterface = Cast<UMaterialInterface>(Asset);
	if (!MaterialInterface)
	{
		OutError = TEXT("unsupported_material_asset_type");
		return false;
	}

	FString SeverityFilterText = TEXT("all");
	JsonTryGetString(Ctx.Params, TEXT("severity_filter"), SeverityFilterText);
	UeAgentMaterialOps::ECompileMessageSeverityFilter SeverityFilter = UeAgentMaterialOps::ECompileMessageSeverityFilter::All;
	if (!UeAgentMaterialOps::ParseCompileSeverityFilter(SeverityFilterText, SeverityFilter))
	{
		OutError = TEXT("invalid_severity_filter");
		return false;
	}

	int32 MaxMessages = 200;
	double MaxMessagesNumber = static_cast<double>(MaxMessages);
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_messages"), MaxMessagesNumber))
	{
		MaxMessages = FMath::Clamp(FMath::RoundToInt(MaxMessagesNumber), 1, 2000);
	}

	bool bCompileBeforeRead = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_before_read"), bCompileBeforeRead);

	bool bCompiled = false;
	if (bCompileBeforeRead)
	{
		if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
			UMaterialEditingLibrary::RecompileMaterial(Material);
			bCompiled = true;
		}
		else if (UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(Asset))
		{
			UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
			bCompiled = true;
		}
	}

	bool bSaveAfterCompile = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_compile"), bSaveAfterCompile);
	if (bSaveAfterCompile && !UeAgentMaterialOps::SaveAssetPackage(Asset, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Messages;
	int32 ErrorCount = 0;
	UeAgentMaterialOps::CollectMaterialCompileMessages(MaterialInterface, SeverityFilter, MaxMessages, Messages, ErrorCount);

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("asset_type"), Asset->GetClass()->GetPathName());
	OutData->SetStringField(TEXT("severity_filter"), SeverityFilterText);
	OutData->SetNumberField(TEXT("max_messages"), MaxMessages);
	OutData->SetBoolField(TEXT("compile_before_read"), bCompileBeforeRead);
	OutData->SetBoolField(TEXT("compile_requested"), bCompiled);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCompile);
	OutData->SetNumberField(TEXT("error_count"), ErrorCount);
	OutData->SetNumberField(TEXT("warning_count"), 0);
	OutData->SetNumberField(TEXT("message_total_count"), ErrorCount);
	OutData->SetNumberField(TEXT("filtered_message_count"), Messages.Num());
	OutData->SetBoolField(TEXT("has_error"), ErrorCount > 0);
	OutData->SetArrayField(TEXT("messages"), Messages);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialSetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	UObject* Asset = UeAgentMaterialOps::LoadAssetObject(AssetPathInput);
	if (!Asset || !Asset->IsA<UMaterialInterface>())
	{
		OutError = TEXT("material_asset_not_found");
		return false;
	}

	FString AppliedValueText;
	FString CppType;
	FString ImportError;
	if (!UeAgentMaterialOps::SetObjectPropertyText(Asset, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		ResultObj->SetStringField(TEXT("cpp_type"), CppType);
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		OutData->SetObjectField(TEXT("property_import_result"), ResultObj);
		OutError = ImportError.IsEmpty() ? FString::Printf(TEXT("set_material_property_failed:%s:%s"), *PropertyName, *ValueText) : ImportError;
		return false;
	}

	Asset->MarkPackageDirty();
	Asset->PostEditChange();

	bool bCompileAfterSet = Asset->IsA<UMaterial>();
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	if (bCompileAfterSet)
	{
		if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
			UMaterialEditingLibrary::RecompileMaterial(Material);
		}
		else if (UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(Asset))
		{
			UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
		}
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentMaterialOps::SaveAssetPackage(Asset, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialSetInstanceParent(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString ParentPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("parent_material_path"), ParentPath) || ParentPath.IsEmpty())
	{
		OutError = TEXT("missing_parent_material_path");
		return false;
	}

	UMaterialInstanceConstant* MaterialInstance = UeAgentMaterialOps::LoadMaterialInstance(AssetPathInput);
	if (!MaterialInstance)
	{
		OutError = TEXT("material_instance_not_found");
		return false;
	}
	UMaterialInterface* ParentMaterial = UeAgentMaterialOps::LoadMaterialInterface(ParentPath);
	if (!ParentMaterial)
	{
		OutError = TEXT("parent_material_not_found");
		return false;
	}

	MaterialInstance->Modify();
	UMaterialEditingLibrary::SetMaterialInstanceParent(MaterialInstance, ParentMaterial);
	UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
	MaterialInstance->MarkPackageDirty();
	MaterialInstance->PostEditChange();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentMaterialOps::SaveAssetPackage(MaterialInstance, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), MaterialInstance->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("parent_path"), ParentMaterial->GetPathName());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialSetParameter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString ParameterTypeText;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_type"), ParameterTypeText) || ParameterTypeText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_type");
		return false;
	}

	UeAgentMaterialOps::EMaterialInstanceParameterType ParameterType;
	if (!UeAgentMaterialOps::ParseMaterialInstanceParameterType(ParameterTypeText, ParameterType))
	{
		OutError = TEXT("invalid_parameter_type");
		return false;
	}

	if (ParameterType == UeAgentMaterialOps::EMaterialInstanceParameterType::Scalar)
	{
		return CmdMaterialSetScalarParameter(Ctx, OutData, OutError);
	}
	if (ParameterType == UeAgentMaterialOps::EMaterialInstanceParameterType::Vector)
	{
		return CmdMaterialSetVectorParameter(Ctx, OutData, OutError);
	}
	if (ParameterType == UeAgentMaterialOps::EMaterialInstanceParameterType::StaticSwitch)
	{
		return CmdMaterialSetStaticSwitchParameter(Ctx, OutData, OutError);
	}

	if (ParameterType == UeAgentMaterialOps::EMaterialInstanceParameterType::Texture)
	{
		TSharedPtr<FJsonObject> ForwardParams = MakeShared<FJsonObject>();
		ForwardParams->Values = Ctx.Params->Values;

		FString ValuePath;
		JsonTryGetString(Ctx.Params, TEXT("value_path"), ValuePath);
		ValuePath = ValuePath.TrimStartAndEnd();
		if (!ValuePath.IsEmpty() && !ForwardParams->HasField(TEXT("texture_path")))
		{
			ForwardParams->SetStringField(TEXT("texture_path"), ValuePath);
		}

		FUeAgentRequestContext ForwardCtx = Ctx;
		ForwardCtx.Params = ForwardParams;
		return CmdMaterialSetTextureParameter(ForwardCtx, OutData, OutError);
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}

	bool bR = false;
	bool bG = false;
	bool bB = false;
	bool bA = false;
	if (!JsonTryGetBool(Ctx.Params, TEXT("r"), bR) ||
		!JsonTryGetBool(Ctx.Params, TEXT("g"), bG) ||
		!JsonTryGetBool(Ctx.Params, TEXT("b"), bB) ||
		!JsonTryGetBool(Ctx.Params, TEXT("a"), bA))
	{
		const TSharedPtr<FJsonObject>* ValueObject = nullptr;
		if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetObjectField(TEXT("value"), ValueObject) || !ValueObject || !ValueObject->IsValid() ||
			!((*ValueObject)->TryGetBoolField(TEXT("r"), bR) || (*ValueObject)->TryGetBoolField(TEXT("red"), bR)) ||
			!((*ValueObject)->TryGetBoolField(TEXT("g"), bG) || (*ValueObject)->TryGetBoolField(TEXT("green"), bG)) ||
			!((*ValueObject)->TryGetBoolField(TEXT("b"), bB) || (*ValueObject)->TryGetBoolField(TEXT("blue"), bB)) ||
			!((*ValueObject)->TryGetBoolField(TEXT("a"), bA) || (*ValueObject)->TryGetBoolField(TEXT("alpha"), bA)))
		{
			OutError = TEXT("missing_or_invalid_value");
			return false;
		}
	}

	UMaterialInstanceConstant* MaterialInstance = UeAgentMaterialOps::LoadMaterialInstance(AssetPathInput);
	if (!MaterialInstance)
	{
		OutError = TEXT("material_instance_not_found");
		return false;
	}

	TArray<FMaterialParameterInfo> StaticComponentMaskInfos;
	TArray<FGuid> StaticComponentMaskIds;
	MaterialInstance->GetAllStaticComponentMaskParameterInfo(StaticComponentMaskInfos, StaticComponentMaskIds);

	const FName ParameterFName(*ParameterName);
	int32 ParameterIndex = INDEX_NONE;
	for (int32 Index = 0; Index < StaticComponentMaskInfos.Num(); ++Index)
	{
		if (StaticComponentMaskInfos[Index].Name == ParameterFName)
		{
			ParameterIndex = Index;
			break;
		}
	}
	if (ParameterIndex == INDEX_NONE)
	{
		OutError = TEXT("parameter_not_found");
		return false;
	}

	FMaterialParameterMetadata Meta(FMaterialParameterValue(FStaticComponentMaskValue(bR, bG, bB, bA)));
	Meta.ExpressionGuid = StaticComponentMaskIds.IsValidIndex(ParameterIndex) ? StaticComponentMaskIds[ParameterIndex] : FGuid();

	MaterialInstance->Modify();
	FStaticParameterSet StaticParameters = MaterialInstance->GetStaticParameters();
	StaticParameters.SetParameterValue(StaticComponentMaskInfos[ParameterIndex], Meta);
	MaterialInstance->UpdateStaticPermutation(StaticParameters);
	MaterialInstance->MarkPackageDirty();
	MaterialInstance->PostEditChange();

	bool bResolvedR = false;
	bool bResolvedG = false;
	bool bResolvedB = false;
	bool bResolvedA = false;
	FGuid ResolvedExpressionGuid = Meta.ExpressionGuid;
	const bool bResolved = UeAgentMaterialOps::ResolveStaticComponentMaskValue(
		MaterialInstance,
		StaticComponentMaskInfos[ParameterIndex],
		ResolvedExpressionGuid,
		bResolvedR,
		bResolvedG,
		bResolvedB,
		bResolvedA,
		ResolvedExpressionGuid);
	const bool bUpdated = bResolved && bResolvedR == bR && bResolvedG == bG && bResolvedB == bB && bResolvedA == bA;

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentMaterialOps::SaveAssetPackage(MaterialInstance, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
	ValueObj->SetBoolField(TEXT("r"), bR);
	ValueObj->SetBoolField(TEXT("g"), bG);
	ValueObj->SetBoolField(TEXT("b"), bB);
	ValueObj->SetBoolField(TEXT("a"), bA);

	TSharedPtr<FJsonObject> ResolvedValueObj = MakeShared<FJsonObject>();
	ResolvedValueObj->SetBoolField(TEXT("r"), bResolvedR);
	ResolvedValueObj->SetBoolField(TEXT("g"), bResolvedG);
	ResolvedValueObj->SetBoolField(TEXT("b"), bResolvedB);
	ResolvedValueObj->SetBoolField(TEXT("a"), bResolvedA);

	OutData->SetStringField(TEXT("asset_path"), MaterialInstance->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("parameter_type"), TEXT("static_component_mask"));
	OutData->SetStringField(TEXT("parameter_name"), ParameterName);
	OutData->SetObjectField(TEXT("value"), ValueObj);
	OutData->SetObjectField(TEXT("resolved_value"), ResolvedValueObj);
	OutData->SetBoolField(TEXT("updated"), bUpdated);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialSetScalarParameter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	double ValueNumber = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("value"), ValueNumber))
	{
		OutError = TEXT("missing_value");
		return false;
	}

	UMaterialInstanceConstant* MaterialInstance = UeAgentMaterialOps::LoadMaterialInstance(AssetPathInput);
	if (!MaterialInstance)
	{
		OutError = TEXT("material_instance_not_found");
		return false;
	}

	const bool bUpdated = UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MaterialInstance, FName(*ParameterName), (float)ValueNumber);
	UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
	MaterialInstance->MarkPackageDirty();
	MaterialInstance->PostEditChange();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentMaterialOps::SaveAssetPackage(MaterialInstance, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), MaterialInstance->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("parameter_name"), ParameterName);
	OutData->SetNumberField(TEXT("value"), ValueNumber);
	OutData->SetBoolField(TEXT("updated"), bUpdated);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialSetVectorParameter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}

	FLinearColor ColorValue = FLinearColor::White;
	if (!UeAgentMaterialOps::ParseLinearColor(Ctx.Params, TEXT("value"), ColorValue))
	{
		FString ValueText;
		if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText) || !ColorValue.InitFromString(ValueText))
		{
			OutError = TEXT("missing_or_invalid_value");
			return false;
		}
	}

	UMaterialInstanceConstant* MaterialInstance = UeAgentMaterialOps::LoadMaterialInstance(AssetPathInput);
	if (!MaterialInstance)
	{
		OutError = TEXT("material_instance_not_found");
		return false;
	}

	const bool bUpdated = UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MaterialInstance, FName(*ParameterName), ColorValue);
	UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
	MaterialInstance->MarkPackageDirty();
	MaterialInstance->PostEditChange();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentMaterialOps::SaveAssetPackage(MaterialInstance, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
	ColorObj->SetNumberField(TEXT("r"), ColorValue.R);
	ColorObj->SetNumberField(TEXT("g"), ColorValue.G);
	ColorObj->SetNumberField(TEXT("b"), ColorValue.B);
	ColorObj->SetNumberField(TEXT("a"), ColorValue.A);

	OutData->SetStringField(TEXT("asset_path"), MaterialInstance->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("parameter_name"), ParameterName);
	OutData->SetObjectField(TEXT("value"), ColorObj);
	OutData->SetBoolField(TEXT("updated"), bUpdated);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialSetTextureParameter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	FString TexturePathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("texture_path"), TexturePathInput) || TexturePathInput.IsEmpty())
	{
		OutError = TEXT("missing_texture_path");
		return false;
	}

	UMaterialInstanceConstant* MaterialInstance = UeAgentMaterialOps::LoadMaterialInstance(AssetPathInput);
	if (!MaterialInstance)
	{
		OutError = TEXT("material_instance_not_found");
		return false;
	}

	UTexture* Texture = Cast<UTexture>(UeAgentMaterialOps::LoadAssetObject(TexturePathInput));
	if (!Texture)
	{
		OutError = TEXT("texture_not_found");
		return false;
	}

	const bool bUpdated = UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, FName(*ParameterName), Texture);
	UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
	MaterialInstance->MarkPackageDirty();
	MaterialInstance->PostEditChange();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentMaterialOps::SaveAssetPackage(MaterialInstance, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), MaterialInstance->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("parameter_name"), ParameterName);
	OutData->SetStringField(TEXT("texture_path"), Texture->GetPathName());
	OutData->SetBoolField(TEXT("updated"), bUpdated);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdMaterialSetStaticSwitchParameter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}

	bool bValue = false;
	if (!JsonTryGetBool(Ctx.Params, TEXT("value"), bValue))
	{
		OutError = TEXT("missing_value");
		return false;
	}

	UMaterialInstanceConstant* MaterialInstance = UeAgentMaterialOps::LoadMaterialInstance(AssetPathInput);
	if (!MaterialInstance)
	{
		OutError = TEXT("material_instance_not_found");
		return false;
	}

	TArray<FName> StaticSwitchNames;
	UMaterialEditingLibrary::GetStaticSwitchParameterNames(MaterialInstance, StaticSwitchNames);
	if (!StaticSwitchNames.Contains(FName(*ParameterName)))
	{
		OutError = TEXT("parameter_not_found");
		return false;
	}

	UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, FName(*ParameterName), bValue);
	MaterialInstance->MarkPackageDirty();
	MaterialInstance->PostEditChange();
	const bool bResolvedValue = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, FName(*ParameterName));
	const bool bUpdated = (bResolvedValue == bValue);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentMaterialOps::SaveAssetPackage(MaterialInstance, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), MaterialInstance->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("parameter_name"), ParameterName);
	OutData->SetBoolField(TEXT("value"), bValue);
	OutData->SetBoolField(TEXT("resolved_value"), bResolvedValue);
	OutData->SetBoolField(TEXT("updated"), bUpdated);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}
