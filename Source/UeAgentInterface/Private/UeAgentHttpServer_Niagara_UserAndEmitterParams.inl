bool FUeAgentHttpServer::CmdNiagaraUserParameterList(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	bool bIncludeValues = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_values"), bIncludeValues);

	TArray<FNiagaraVariable> UserParameters;
	NiagaraSystem->GetExposedParameters().GetUserParameters(UserParameters);
	UserParameters.Sort([](const FNiagaraVariable& A, const FNiagaraVariable& B)
	{
		return A.GetName().LexicalLess(B.GetName());
	});

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(UserParameters.Num());
	for (const FNiagaraVariable& UserParameter : UserParameters)
	{
		Items.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildUserParameterJson(NiagaraSystem->GetExposedParameters(), UserParameter, bIncludeValues)));
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetArrayField(TEXT("parameters"), Items);
	OutData->SetNumberField(TEXT("parameter_count"), Items.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraUserParameterAdd(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	FString ParameterTypeText;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_type"), ParameterTypeText) || ParameterTypeText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_type");
		return false;
	}
	ParameterTypeText = ParameterTypeText.TrimStartAndEnd();

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	FNiagaraTypeDefinition TypeDefinition;
	if (!UeAgentNiagaraOps::ResolveNiagaraTypeDefinition(ParameterTypeText, TypeDefinition))
	{
		OutError = TEXT("invalid_parameter_type");
		return false;
	}

	FNiagaraVariable ExistingParameter;
	if (UeAgentNiagaraOps::FindUserParameterByName(NiagaraSystem->GetExposedParameters(), ParameterName, ExistingParameter))
	{
		OutError = TEXT("user_parameter_already_exists");
		return false;
	}

	FNiagaraVariable NewParameter(TypeDefinition, *ParameterName);
	FNiagaraUserRedirectionParameterStore::MakeUserVariable(NewParameter);

	const bool bHasDefaultValueText = Ctx.Params.IsValid() && (Ctx.Params->HasField(TEXT("default_value_text")) || Ctx.Params->HasField(TEXT("default_value")));
	FString DefaultValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("default_value_text"), DefaultValueText))
	{
		JsonTryGetString(Ctx.Params, TEXT("default_value"), DefaultValueText);
	}

	NiagaraSystem->Modify();
	FNiagaraUserRedirectionParameterStore& ExposedParameters = NiagaraSystem->GetExposedParameters();
	if (!ExposedParameters.AddParameter(NewParameter, true, true))
	{
		OutError = TEXT("add_user_parameter_failed");
		return false;
	}

	if (bHasDefaultValueText)
	{
		if (!UeAgentNiagaraOps::SetUserParameterValueFromText(ExposedParameters, NewParameter, DefaultValueText, OutError))
		{
			return false;
		}
	}

	NiagaraSystem->MarkPackageDirty();

	bool bCompileAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet)
	{
		bCompilationRequested = NiagaraSystem->RequestCompile(false);
		bCompilationComplete = bWaitForComplete ? NiagaraSystem->PollForCompilationComplete(true) : false;
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	FNiagaraVariable AddedParameter;
	if (!UeAgentNiagaraOps::FindUserParameterByName(ExposedParameters, NewParameter.GetName().ToString(), AddedParameter))
	{
		OutError = TEXT("added_user_parameter_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("parameter"), UeAgentNiagaraOps::BuildUserParameterJson(ExposedParameters, AddedParameter, true));
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraUserParameterRemove(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	FNiagaraVariable ParameterToRemove;
	if (!UeAgentNiagaraOps::FindUserParameterByName(NiagaraSystem->GetExposedParameters(), ParameterName, ParameterToRemove))
	{
		OutError = TEXT("user_parameter_not_found");
		return false;
	}

	const FString RemovedName = ParameterToRemove.GetName().ToString();
	const FString RemovedType = ParameterToRemove.GetType().GetNameText().ToString();

	NiagaraSystem->Modify();
	FNiagaraUserRedirectionParameterStore& ExposedParameters = NiagaraSystem->GetExposedParameters();
	if (!ExposedParameters.RemoveParameter(ParameterToRemove))
	{
		OutError = TEXT("remove_user_parameter_failed");
		return false;
	}
	NiagaraSystem->MarkPackageDirty();

	bool bCompileAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet)
	{
		bCompilationRequested = NiagaraSystem->RequestCompile(false);
		bCompilationComplete = bWaitForComplete ? NiagaraSystem->PollForCompilationComplete(true) : false;
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("removed_parameter_name"), RemovedName);
	OutData->SetStringField(TEXT("removed_parameter_type"), RemovedType);
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraUserParameterGet(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	FNiagaraVariable UserParameter;
	if (!UeAgentNiagaraOps::FindUserParameterByName(NiagaraSystem->GetExposedParameters(), ParameterName, UserParameter))
	{
		OutError = TEXT("user_parameter_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("parameter"), UeAgentNiagaraOps::BuildUserParameterJson(NiagaraSystem->GetExposedParameters(), UserParameter, true));
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraUserParameterSet(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		if (!JsonTryGetString(Ctx.Params, TEXT("default_value_text"), ValueText))
		{
			OutError = TEXT("missing_value_text");
			return false;
		}
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(AssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	FNiagaraVariable UserParameter;
	if (!UeAgentNiagaraOps::FindUserParameterByName(NiagaraSystem->GetExposedParameters(), ParameterName, UserParameter))
	{
		OutError = TEXT("user_parameter_not_found");
		return false;
	}

	NiagaraSystem->Modify();
	FNiagaraUserRedirectionParameterStore& ExposedParameters = NiagaraSystem->GetExposedParameters();
	if (!UeAgentNiagaraOps::SetUserParameterValueFromText(ExposedParameters, UserParameter, ValueText, OutError))
	{
		return false;
	}
	NiagaraSystem->MarkPackageDirty();

	bool bCompileAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet)
	{
		bCompilationRequested = NiagaraSystem->RequestCompile(false);
		bCompilationComplete = bWaitForComplete ? NiagaraSystem->PollForCompilationComplete(true) : false;
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("parameter"), UeAgentNiagaraOps::BuildUserParameterJson(ExposedParameters, UserParameter, true));
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterParameterList(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	bool bIncludeDefaultValues = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_default_values"), bIncludeDefaultValues);

	FString NamespaceFilter;
	JsonTryGetString(Ctx.Params, TEXT("namespace"), NamespaceFilter);
	NamespaceFilter = NamespaceFilter.TrimStartAndEnd();

	const UNiagaraGraph::FScriptVariableMap& ScriptVariableMap = EditContext.Graph->GetAllMetaData();
	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(ScriptVariableMap.Num());
	for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : ScriptVariableMap)
	{
		const FString FullName = Pair.Key.GetName().ToString();
		const FString Namespace = UeAgentNiagaraOps::GetVariableNamespace(FullName);
		if (!NamespaceFilter.IsEmpty() && !Namespace.Equals(NamespaceFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		Items.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildGraphParameterJson(Pair.Key, Pair.Value, bIncludeDefaultValues)));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("parameters"), Items);
	OutData->SetNumberField(TEXT("parameter_count"), Items.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterParameterAdd(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	FString ParameterTypeText;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_type"), ParameterTypeText) || ParameterTypeText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_type");
		return false;
	}
	ParameterTypeText = ParameterTypeText.TrimStartAndEnd();

	FNiagaraVariable ExistingParameter;
	UNiagaraScriptVariable* ExistingScriptVariable = nullptr;
	if (UeAgentNiagaraOps::FindGraphParameterByName(*EditContext.Graph, ParameterName, ExistingParameter, ExistingScriptVariable))
	{
		OutError = TEXT("emitter_parameter_already_exists");
		return false;
	}

	FNiagaraTypeDefinition TypeDefinition;
	if (!UeAgentNiagaraOps::ResolveNiagaraTypeDefinition(ParameterTypeText, TypeDefinition))
	{
		OutError = TEXT("invalid_parameter_type");
		return false;
	}

	bool bIsStaticSwitch = false;
	JsonTryGetBool(Ctx.Params, TEXT("is_static_switch"), bIsStaticSwitch);

	const bool bHasDefaultValueText = Ctx.Params.IsValid() && (Ctx.Params->HasField(TEXT("default_value_text")) || Ctx.Params->HasField(TEXT("default_value")));
	FString DefaultValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("default_value_text"), DefaultValueText))
	{
		JsonTryGetString(Ctx.Params, TEXT("default_value"), DefaultValueText);
	}

	FNiagaraVariable NewParameter(TypeDefinition, *ParameterName);
	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "AddNiagaraEmitterParameter", "UeAgentInterface Add Niagara Emitter Parameter"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	UNiagaraScriptVariable* AddedScriptVariable = UeAgentNiagaraOps::AddGraphParameterMetadata(*EditContext.Graph, NewParameter, bIsStaticSwitch);
	if (!AddedScriptVariable)
	{
		OutError = TEXT("add_emitter_parameter_failed");
		return false;
	}

	if (bHasDefaultValueText)
	{
		if (!UeAgentNiagaraOps::SetScriptVariableDefaultValueFromText(*AddedScriptVariable, DefaultValueText, OutError))
		{
			return false;
		}
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetObjectField(TEXT("parameter"), UeAgentNiagaraOps::BuildGraphParameterJson(NewParameter, AddedScriptVariable, true));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterParameterRemove(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	FNiagaraVariable ParameterToRemove;
	UNiagaraScriptVariable* ScriptVariable = nullptr;
	if (!UeAgentNiagaraOps::FindGraphParameterByName(*EditContext.Graph, ParameterName, ParameterToRemove, ScriptVariable))
	{
		OutError = TEXT("emitter_parameter_not_found");
		return false;
	}

	const FString RemovedParameterName = ParameterToRemove.GetName().ToString();
	const FString RemovedParameterType = ParameterToRemove.GetType().GetNameText().ToString();

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "RemoveNiagaraEmitterParameter", "UeAgentInterface Remove Niagara Emitter Parameter"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	if (!UeAgentNiagaraOps::RemoveGraphParameterMetadata(*EditContext.Graph, ParameterToRemove))
	{
		OutError = TEXT("remove_emitter_parameter_failed");
		return false;
	}
	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("removed_parameter_name"), RemovedParameterName);
	OutData->SetStringField(TEXT("removed_parameter_type"), RemovedParameterType);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterParameterGet(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	FNiagaraVariable Parameter;
	UNiagaraScriptVariable* ScriptVariable = nullptr;
	if (!UeAgentNiagaraOps::FindGraphParameterByName(*EditContext.Graph, ParameterName, Parameter, ScriptVariable))
	{
		OutError = TEXT("emitter_parameter_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetObjectField(TEXT("parameter"), UeAgentNiagaraOps::BuildGraphParameterJson(Parameter, ScriptVariable, true));
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterParameterSet(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("parameter_name"), ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_parameter_name");
		return false;
	}
	ParameterName = ParameterName.TrimStartAndEnd();

	const bool bHasValueText = Ctx.Params.IsValid() && (Ctx.Params->HasField(TEXT("value_text")) || Ctx.Params->HasField(TEXT("default_value_text")));
	const bool bHasNewName = Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("new_parameter_name"));
	const bool bHasType = Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("parameter_type"));
	if (!bHasValueText && !bHasNewName && !bHasType)
	{
		OutError = TEXT("missing_set_operation");
		return false;
	}

	FNiagaraVariable ExistingParameter;
	UNiagaraScriptVariable* ExistingScriptVariable = nullptr;
	if (!UeAgentNiagaraOps::FindGraphParameterByName(*EditContext.Graph, ParameterName, ExistingParameter, ExistingScriptVariable))
	{
		OutError = TEXT("emitter_parameter_not_found");
		return false;
	}

	FNiagaraVariable WorkingParameter = ExistingParameter;
	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "SetNiagaraEmitterParameter", "UeAgentInterface Set Niagara Emitter Parameter"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();

	if (bHasType)
	{
		FString NewTypeText;
		if (!JsonTryGetString(Ctx.Params, TEXT("parameter_type"), NewTypeText) || NewTypeText.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("invalid_parameter_type");
			return false;
		}

		FNiagaraTypeDefinition NewType;
		if (!UeAgentNiagaraOps::ResolveNiagaraTypeDefinition(NewTypeText, NewType))
		{
			OutError = TEXT("invalid_parameter_type");
			return false;
		}

		if (!(WorkingParameter.GetType() == NewType))
		{
			TArray<FNiagaraVariable> ParametersToChange;
			ParametersToChange.Add(WorkingParameter);
			EditContext.Graph->ChangeParameterType(ParametersToChange, NewType, false);
			WorkingParameter.SetType(NewType);
		}
	}

	if (bHasNewName)
	{
		FString NewParameterName;
		if (!JsonTryGetString(Ctx.Params, TEXT("new_parameter_name"), NewParameterName) || NewParameterName.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("invalid_new_parameter_name");
			return false;
		}
		NewParameterName = NewParameterName.TrimStartAndEnd();

		const bool bRenamed = EditContext.Graph->RenameParameter(WorkingParameter, *NewParameterName, ExistingScriptVariable ? ExistingScriptVariable->GetIsStaticSwitch() : false, nullptr, false);
		if (!bRenamed)
		{
			OutError = TEXT("rename_parameter_failed");
			return false;
		}
		WorkingParameter.SetName(*NewParameterName);
	}

	UNiagaraScriptVariable* TargetScriptVariable = EditContext.Graph->GetScriptVariable(WorkingParameter.GetName());
	if (!TargetScriptVariable)
	{
		OutError = TEXT("updated_parameter_not_found");
		return false;
	}

	if (bHasValueText)
	{
		FString ValueText;
		if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
		{
			JsonTryGetString(Ctx.Params, TEXT("default_value_text"), ValueText);
		}
		if (!UeAgentNiagaraOps::SetScriptVariableDefaultValueFromText(*TargetScriptVariable, ValueText, OutError))
		{
			return false;
		}
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetObjectField(TEXT("parameter"), UeAgentNiagaraOps::BuildGraphParameterJson(WorkingParameter, TargetScriptVariable, true));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemListEmitters(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	const TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem->GetEmitterHandles();
	const int32 EmitterCompiledDataCount = NiagaraSystem->GetEmitterCompiledData().Num();
	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(Handles.Num());
	for (int32 Index = 0; Index < Handles.Num(); ++Index)
	{
		const FNiagaraEmitterHandle& Handle = Handles[Index];
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), Index);
		Item->SetStringField(TEXT("id"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
		Item->SetStringField(TEXT("name"), Handle.GetName().ToString());
		Item->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		if (const UNiagaraEmitter* EmitterAsset = Handle.GetInstance().Emitter)
		{
			Item->SetStringField(TEXT("instance_object_path"), EmitterAsset->GetPathName());
		}
		Item->SetStringField(TEXT("instance_version"), Handle.GetInstance().Version.ToString(EGuidFormats::DigitsWithHyphensLower));

		FVersionedNiagaraEmitterData* EmitterData = Handle.GetInstance().GetEmitterData();
		Item->SetBoolField(TEXT("has_emitter_data"), EmitterData != nullptr);
		Item->SetBoolField(TEXT("compiled_data_index_valid"), Index < EmitterCompiledDataCount);
		Item->SetBoolField(TEXT("runtime_allowed_by_handle"), Handle.IsAllowedByScalability());
		if (EmitterData)
		{
			Item->SetBoolField(TEXT("emitter_data_valid"), EmitterData->IsValid());
			Item->SetBoolField(TEXT("emitter_ready_to_run"), EmitterData->IsReadyToRun());
			Item->SetBoolField(TEXT("emitter_allowed_by_scalability"), EmitterData->IsAllowedByScalability());
			Item->SetBoolField(TEXT("emitter_allowed_to_execute"), EmitterData->IsAllowedToExecute());
			Item->SetStringField(TEXT("sim_target"), StaticEnum<ENiagaraSimTarget>() ? StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(static_cast<int64>(EmitterData->SimTarget)) : TEXT(""));
		}
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetNumberField(TEXT("compiled_emitter_data_count"), EmitterCompiledDataCount);
	OutData->SetArrayField(TEXT("emitters"), Items);
	OutData->SetNumberField(TEXT("emitter_count"), Items.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemAddEmitter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SystemAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("system_asset_path"), SystemAssetPathInput) || SystemAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_system_asset_path");
		return false;
	}

	FString EmitterAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("emitter_asset_path"), EmitterAssetPathInput) || EmitterAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_emitter_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(SystemAssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	UNiagaraEmitter* EmitterAsset = Cast<UNiagaraEmitter>(UeAgentNiagaraOps::LoadAssetObject(EmitterAssetPathInput));
	if (!EmitterAsset)
	{
		OutError = TEXT("emitter_asset_not_found");
		return false;
	}

	FString EmitterNameString;
	JsonTryGetString(Ctx.Params, TEXT("emitter_name"), EmitterNameString);
	if (EmitterNameString.IsEmpty())
	{
		EmitterNameString = EmitterAsset->GetName();
	}

	FGuid EmitterVersionGuid = EmitterAsset->GetExposedVersion().VersionGuid;
	FString EmitterVersionString;
	if (JsonTryGetString(Ctx.Params, TEXT("emitter_version"), EmitterVersionString) && !EmitterVersionString.IsEmpty())
	{
		FGuid ParsedGuid;
		if (FGuid::Parse(EmitterVersionString, ParsedGuid))
		{
			EmitterVersionGuid = ParsedGuid;
		}
	}

	bool bCloseOpenEditorBeforeStructuralEdit = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	const int32 ClosedEditorCount = bCloseOpenEditorBeforeStructuralEdit
		? UeAgentNiagaraOps::CloseAssetEditorsForStructuralNiagaraEdit(*NiagaraSystem)
		: 0;

	NiagaraSystem->Modify();
	const FNiagaraEmitterHandle NewHandle = NiagaraSystem->AddEmitterHandle(*EmitterAsset, *EmitterNameString, EmitterVersionGuid);
	const UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult =
		UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*NiagaraSystem, true, true);

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bForceCompile = true;
	JsonTryGetBool(Ctx.Params, TEXT("force_compile"), bForceCompile);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet)
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

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	const TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem->GetEmitterHandles();
	int32 HandleIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Handles.Num(); ++Index)
	{
		if (Handles[Index].GetId() == NewHandle.GetId())
		{
			HandleIndex = Index;
			break;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("added_emitter_id"), NewHandle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("added_emitter_name"), NewHandle.GetName().ToString());
	OutData->SetNumberField(TEXT("added_emitter_index"), HandleIndex);
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("force_compile"), bForceCompile);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	OutData->SetBoolField(TEXT("synchronized_overview_graph"), RefreshResult.bSynchronizedOverviewGraph);
	OutData->SetNumberField(TEXT("overview_node_count_before"), RefreshResult.OverviewNodeCountBefore);
	OutData->SetNumberField(TEXT("overview_node_count_after"), RefreshResult.OverviewNodeCountAfter);
	OutData->SetBoolField(TEXT("closed_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	OutData->SetNumberField(TEXT("closed_editor_count"), ClosedEditorCount);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemRemoveEmitter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SystemAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("system_asset_path"), SystemAssetPathInput) || SystemAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_system_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(SystemAssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	const TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem->GetEmitterHandles();
	if (Handles.Num() == 0)
	{
		OutError = TEXT("system_has_no_emitters");
		return false;
	}

	const FNiagaraEmitterHandle* HandleToRemove = nullptr;
	int32 HandleIndex = INDEX_NONE;

	double HandleIndexNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("emitter_index"), HandleIndexNumber))
	{
		const int32 Index = FMath::RoundToInt(HandleIndexNumber);
		if (Handles.IsValidIndex(Index))
		{
			HandleToRemove = &Handles[Index];
			HandleIndex = Index;
		}
	}

	if (!HandleToRemove)
	{
		FString EmitterIdString;
		if (JsonTryGetString(Ctx.Params, TEXT("emitter_id"), EmitterIdString) && !EmitterIdString.IsEmpty())
		{
			FGuid EmitterGuid;
			if (FGuid::Parse(EmitterIdString, EmitterGuid))
			{
				for (int32 Index = 0; Index < Handles.Num(); ++Index)
				{
					if (Handles[Index].GetId() == EmitterGuid)
					{
						HandleToRemove = &Handles[Index];
						HandleIndex = Index;
						break;
					}
				}
			}
		}
	}

	if (!HandleToRemove)
	{
		FString EmitterName;
		if (JsonTryGetString(Ctx.Params, TEXT("emitter_name"), EmitterName) && !EmitterName.IsEmpty())
		{
			for (int32 Index = 0; Index < Handles.Num(); ++Index)
			{
				if (Handles[Index].GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
				{
					HandleToRemove = &Handles[Index];
					HandleIndex = Index;
					break;
				}
			}
		}
	}

	if (!HandleToRemove)
	{
		OutError = TEXT("emitter_not_found");
		return false;
	}

	const FString RemovedId = HandleToRemove->GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString RemovedName = HandleToRemove->GetName().ToString();

	bool bCloseOpenEditorBeforeStructuralEdit = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	const int32 ClosedEditorCount = bCloseOpenEditorBeforeStructuralEdit
		? UeAgentNiagaraOps::CloseAssetEditorsForStructuralNiagaraEdit(*NiagaraSystem)
		: 0;

	NiagaraSystem->Modify();
	NiagaraSystem->RemoveEmitterHandle(*HandleToRemove);
	int32 OverviewNodeCountBefore = INDEX_NONE;
	int32 OverviewNodeCountAfter = INDEX_NONE;
	const bool bSynchronizedOverviewGraph = UeAgentNiagaraOps::SynchronizeSystemEditorData(*NiagaraSystem, OverviewNodeCountBefore, OverviewNodeCountAfter);
	NiagaraSystem->MarkPackageDirty();

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet)
	{
		bCompilationRequested = NiagaraSystem->RequestCompile(false);
		bCompilationComplete = bWaitForComplete ? NiagaraSystem->PollForCompilationComplete(true) : false;
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("removed_emitter_id"), RemovedId);
	OutData->SetStringField(TEXT("removed_emitter_name"), RemovedName);
	OutData->SetNumberField(TEXT("removed_emitter_index"), HandleIndex);
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("synchronized_overview_graph"), bSynchronizedOverviewGraph);
	OutData->SetNumberField(TEXT("overview_node_count_before"), OverviewNodeCountBefore);
	OutData->SetNumberField(TEXT("overview_node_count_after"), OverviewNodeCountAfter);
	OutData->SetBoolField(TEXT("closed_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	OutData->SetNumberField(TEXT("closed_editor_count"), ClosedEditorCount);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemMoveEmitter(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SystemAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("system_asset_path"), SystemAssetPathInput) || SystemAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_system_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(SystemAssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	double TargetIndexNumber = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("target_index"), TargetIndexNumber))
	{
		OutError = TEXT("missing_target_index");
		return false;
	}

	int32 EmitterIndex = INDEX_NONE;
	FNiagaraEmitterHandle* Handle = nullptr;
	if (!UeAgentNiagaraOps::ResolveEmitterHandleFromParams(*NiagaraSystem, Ctx.Params, EmitterIndex, Handle, OutError))
	{
		return false;
	}

	TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem->GetEmitterHandles();
	const int32 TargetIndex = FMath::Clamp(FMath::RoundToInt(TargetIndexNumber), 0, Handles.Num() - 1);

	const FString EmitterId = Handle->GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString EmitterName = Handle->GetName().ToString();
	const int32 SourceIndex = EmitterIndex;
	const bool bMoved = SourceIndex != TargetIndex;
	bool bCloseOpenEditorBeforeStructuralEdit = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	const int32 ClosedEditorCount = (bMoved && bCloseOpenEditorBeforeStructuralEdit)
		? UeAgentNiagaraOps::CloseAssetEditorsForStructuralNiagaraEdit(*NiagaraSystem)
		: 0;

	if (bMoved)
	{
		NiagaraSystem->Modify();
		const FNiagaraEmitterHandle MovedHandle = Handles[SourceIndex];
		Handles.RemoveAt(SourceIndex);
		Handles.Insert(MovedHandle, TargetIndex);
		int32 OverviewNodeCountBefore = INDEX_NONE;
		int32 OverviewNodeCountAfter = INDEX_NONE;
		UeAgentNiagaraOps::SynchronizeSystemEditorData(*NiagaraSystem, OverviewNodeCountBefore, OverviewNodeCountAfter);
		NiagaraSystem->MarkPackageDirty();
	}

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet && bMoved)
	{
		bCompilationRequested = NiagaraSystem->RequestCompile(false);
		bCompilationComplete = bWaitForComplete ? NiagaraSystem->PollForCompilationComplete(true) : false;
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_id"), EmitterId);
	OutData->SetStringField(TEXT("emitter_name"), EmitterName);
	OutData->SetNumberField(TEXT("source_index"), SourceIndex);
	OutData->SetNumberField(TEXT("target_index"), TargetIndex);
	OutData->SetBoolField(TEXT("moved"), bMoved);
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("closed_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	OutData->SetNumberField(TEXT("closed_editor_count"), ClosedEditorCount);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemSetEmitterEnabled(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SystemAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("system_asset_path"), SystemAssetPathInput) || SystemAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_system_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(SystemAssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	bool bEnabled = false;
	if (!JsonTryGetBool(Ctx.Params, TEXT("enabled"), bEnabled))
	{
		OutError = TEXT("missing_enabled");
		return false;
	}

	int32 EmitterIndex = INDEX_NONE;
	FNiagaraEmitterHandle* Handle = nullptr;
	if (!UeAgentNiagaraOps::ResolveEmitterHandleFromParams(*NiagaraSystem, Ctx.Params, EmitterIndex, Handle, OutError))
	{
		return false;
	}

	const FString EmitterId = Handle->GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString EmitterName = Handle->GetName().ToString();
	const bool bOldEnabled = Handle->GetIsEnabled();
	bool bCloseOpenEditorBeforeStructuralEdit = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	const int32 ClosedEditorCount = bCloseOpenEditorBeforeStructuralEdit
		? UeAgentNiagaraOps::CloseAssetEditorsForStructuralNiagaraEdit(*NiagaraSystem)
		: 0;

	NiagaraSystem->Modify();
	const bool bChanged = Handle->SetIsEnabled(bEnabled, *NiagaraSystem, false);
	if (bChanged)
	{
		int32 OverviewNodeCountBefore = INDEX_NONE;
		int32 OverviewNodeCountAfter = INDEX_NONE;
		UeAgentNiagaraOps::SynchronizeSystemEditorData(*NiagaraSystem, OverviewNodeCountBefore, OverviewNodeCountAfter);
		NiagaraSystem->MarkPackageDirty();
	}

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet && bChanged)
	{
		bCompilationRequested = NiagaraSystem->RequestCompile(false);
		bCompilationComplete = bWaitForComplete ? NiagaraSystem->PollForCompilationComplete(true) : false;
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_id"), EmitterId);
	OutData->SetStringField(TEXT("emitter_name"), EmitterName);
	OutData->SetNumberField(TEXT("emitter_index"), EmitterIndex);
	OutData->SetBoolField(TEXT("enabled_before"), bOldEnabled);
	OutData->SetBoolField(TEXT("enabled_after"), Handle->GetIsEnabled());
	OutData->SetBoolField(TEXT("changed"), bChanged);
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("closed_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	OutData->SetNumberField(TEXT("closed_editor_count"), ClosedEditorCount);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemSetEmitterVersion(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SystemAssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("system_asset_path"), SystemAssetPathInput) || SystemAssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_system_asset_path");
		return false;
	}

	UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(UeAgentNiagaraOps::LoadAssetObject(SystemAssetPathInput));
	if (!NiagaraSystem)
	{
		OutError = TEXT("system_asset_not_found");
		return false;
	}

	FString TargetVersionText;
	if (!JsonTryGetString(Ctx.Params, TEXT("emitter_version"), TargetVersionText) || TargetVersionText.IsEmpty())
	{
		OutError = TEXT("missing_emitter_version");
		return false;
	}

	FGuid TargetVersionGuid;
	if (!FGuid::Parse(TargetVersionText, TargetVersionGuid))
	{
		OutError = TEXT("invalid_emitter_version");
		return false;
	}

	int32 EmitterIndex = INDEX_NONE;
	FNiagaraEmitterHandle* Handle = nullptr;
	if (!UeAgentNiagaraOps::ResolveEmitterHandleFromParams(*NiagaraSystem, Ctx.Params, EmitterIndex, Handle, OutError))
	{
		return false;
	}

	FVersionedNiagaraEmitter VersionedEmitter = Handle->GetInstance();
	if (!VersionedEmitter.Emitter)
	{
		OutError = TEXT("emitter_instance_missing");
		return false;
	}
	if (!VersionedEmitter.Emitter->GetEmitterData(TargetVersionGuid))
	{
		OutError = TEXT("target_emitter_version_not_found");
		return false;
	}

	const FString EmitterId = Handle->GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString EmitterName = Handle->GetName().ToString();
	const FString PreviousVersion = VersionedEmitter.Version.ToString(EGuidFormats::DigitsWithHyphensLower);
	const bool bVersionChanged = VersionedEmitter.Version != TargetVersionGuid;
	bool bCloseOpenEditorBeforeStructuralEdit = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	const int32 ClosedEditorCount = (bVersionChanged && bCloseOpenEditorBeforeStructuralEdit)
		? UeAgentNiagaraOps::CloseAssetEditorsForStructuralNiagaraEdit(*NiagaraSystem)
		: 0;

	bool bChanged = false;
	if (bVersionChanged)
	{
		NiagaraSystem->Modify();
		bChanged = NiagaraSystem->ChangeEmitterVersion(VersionedEmitter, TargetVersionGuid);
		if (!bChanged)
		{
			OutError = TEXT("change_emitter_version_failed");
			return false;
		}
		int32 OverviewNodeCountBefore = INDEX_NONE;
		int32 OverviewNodeCountAfter = INDEX_NONE;
		UeAgentNiagaraOps::SynchronizeSystemEditorData(*NiagaraSystem, OverviewNodeCountBefore, OverviewNodeCountAfter);
		NiagaraSystem->MarkPackageDirty();
	}

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet && bVersionChanged)
	{
		bCompilationRequested = NiagaraSystem->RequestCompile(false);
		bCompilationComplete = bWaitForComplete ? NiagaraSystem->PollForCompilationComplete(true) : false;
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(NiagaraSystem, OutError))
		{
			return false;
		}
	}

	FString CurrentVersion = PreviousVersion;
	if (NiagaraSystem->GetEmitterHandles().IsValidIndex(EmitterIndex))
	{
		CurrentVersion = NiagaraSystem->GetEmitterHandles()[EmitterIndex].GetInstance().Version.ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	OutData->SetStringField(TEXT("asset_path"), NiagaraSystem->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_id"), EmitterId);
	OutData->SetStringField(TEXT("emitter_name"), EmitterName);
	OutData->SetNumberField(TEXT("emitter_index"), EmitterIndex);
	OutData->SetStringField(TEXT("previous_version"), PreviousVersion);
	OutData->SetStringField(TEXT("current_version"), CurrentVersion);
	OutData->SetBoolField(TEXT("changed"), bVersionChanged && bChanged);
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("closed_open_editor_before_structural_edit"), bCloseOpenEditorBeforeStructuralEdit);
	OutData->SetNumberField(TEXT("closed_editor_count"), ClosedEditorCount);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterGetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	PropertyName = UeAgentNiagaraOps::NormalizeEventHandlerPropertyName(PropertyName);

	FString ValueText;
	FString CppType;
	if (!UeAgentNiagaraOps::GetStructPropertyText(FVersionedNiagaraEmitterData::StaticStruct(), EditContext.EmitterData, EditContext.Emitter, PropertyName, ValueText, CppType))
	{
		OutError = TEXT("get_emitter_property_failed");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), EditContext.Emitter->GetPathName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("property_found"), true);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterSetProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
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

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "SetNiagaraEmitterProperty", "UeAgentInterface Set Niagara Emitter Property"));
	EditContext.Emitter->Modify();
	if (EditContext.ScriptSource)
	{
		EditContext.ScriptSource->Modify();
	}
	if (EditContext.Graph)
	{
		EditContext.Graph->Modify();
	}

	FString AppliedValueText;
	FString CppType;
	FString ImportError;
	if (!UeAgentNiagaraOps::SetStructPropertyText(FVersionedNiagaraEmitterData::StaticStruct(), EditContext.EmitterData, EditContext.Emitter, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("object_path"), EditContext.Emitter->GetPathName());
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		OutError = ImportError.IsEmpty() ? TEXT("set_emitter_property_failed") : ImportError;
		return false;
	}
	const UeAgentNiagaraOps::FEmitterPropertySideEffectResult SideEffects = UeAgentNiagaraOps::ApplyEmitterPropertySideEffects(EditContext.EmitterData, PropertyName);

	EditContext.Emitter->MarkPackageDirty();
	if (EditContext.Graph)
	{
		EditContext.Graph->NotifyGraphChanged();
	}

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet)
	{
		if (UNiagaraSystem* OwnerSystem = Cast<UNiagaraSystem>(EditContext.Emitter->GetOuter()))
		{
			bCompilationRequested = OwnerSystem->RequestCompile(false);
			bCompilationComplete = bWaitForComplete ? OwnerSystem->PollForCompilationComplete(true) : false;
		}
		else
		{
			TArray<UNiagaraScript*> EmitterScripts;
			EditContext.EmitterData->GetScripts(EmitterScripts, false, false);
			for (UNiagaraScript* Script : EmitterScripts)
			{
				if (!Script)
				{
					continue;
				}
				Script->RequestCompile(Script->GetExposedVersion().VersionGuid, false);
				bCompilationRequested = true;
			}
			bCompilationComplete = !bWaitForComplete;
		}
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), EditContext.Emitter->GetPathName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("side_effect_handled"), SideEffects.bHandled);
	OutData->SetBoolField(TEXT("updated_spawn_script_usage"), SideEffects.bUpdatedSpawnScriptUsage);
	OutData->SetBoolField(TEXT("marked_graph_source_unsynchronized"), SideEffects.bMarkedGraphSourceUnsynchronized);
	if (SideEffects.bHandled)
	{
		OutData->SetStringField(TEXT("old_spawn_script_usage"), UeAgentNiagaraOps::ScriptUsageToString(SideEffects.OldSpawnScriptUsage));
		OutData->SetStringField(TEXT("new_spawn_script_usage"), UeAgentNiagaraOps::ScriptUsageToString(SideEffects.NewSpawnScriptUsage));
	}
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

