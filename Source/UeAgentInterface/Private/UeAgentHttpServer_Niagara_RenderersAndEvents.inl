bool FUeAgentHttpServer::CmdNiagaraEmitterListRenderers(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	bool bIncludeProperties = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_properties"), bIncludeProperties);

	const TArray<UNiagaraRendererProperties*>& Renderers = EditContext.EmitterData->GetRenderers();
	TArray<TSharedPtr<FJsonValue>> RenderersJson;
	RenderersJson.Reserve(Renderers.Num());
	for (int32 RendererIndex = 0; RendererIndex < Renderers.Num(); ++RendererIndex)
	{
		UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];
		if (!Renderer)
		{
			continue;
		}

		TSharedPtr<FJsonObject> RendererObj = UeAgentNiagaraOps::BuildRendererJson(*Renderer, RendererIndex);
		if (bIncludeProperties)
		{
			TArray<TSharedPtr<FJsonValue>> PropertiesJson;
			for (TFieldIterator<FProperty> It(Renderer->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property)
				{
					continue;
				}

				void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Renderer);
				PropertiesJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildPropertyValueJson(*Renderer, *Property, ValuePtr)));
			}
			RendererObj->SetArrayField(TEXT("properties"), PropertiesJson);
			RendererObj->SetNumberField(TEXT("property_count"), PropertiesJson.Num());
		}

		RenderersJson.Add(MakeShared<FJsonValueObject>(RendererObj));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("renderers"), RenderersJson);
	OutData->SetNumberField(TEXT("renderer_count"), RenderersJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterAddRenderer(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	FString RendererClassText;
	if (!JsonTryGetString(Ctx.Params, TEXT("renderer_class"), RendererClassText))
	{
		JsonTryGetString(Ctx.Params, TEXT("renderer_type"), RendererClassText);
	}
	if (RendererClassText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_renderer_class");
		return false;
	}

	UClass* RendererClass = UeAgentNiagaraOps::ResolveRendererClass(RendererClassText);
	if (!RendererClass)
	{
		OutError = TEXT("invalid_renderer_class");
		return false;
	}

	FString RendererName;
	JsonTryGetString(Ctx.Params, TEXT("renderer_name"), RendererName);
	RendererName.TrimStartAndEndInline();

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "AddNiagaraRenderer", "UeAgentInterface Add Niagara Renderer"));
	EditContext.Emitter->Modify();

	UNiagaraRendererProperties* NewRenderer = NewObject<UNiagaraRendererProperties>(
		EditContext.Emitter,
		RendererClass,
		RendererName.IsEmpty() ? NAME_None : FName(*RendererName),
		RF_Transactional);
	if (!NewRenderer)
	{
		OutError = TEXT("add_renderer_failed");
		return false;
	}

	EditContext.Emitter->AddRenderer(NewRenderer, EditContext.VersionGuid);

	FString PropertyName;
	FString ValueText;
	TSharedPtr<FJsonObject> InitialPropertyResult;
	if (JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) && JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		FString AppliedValueText;
		FString CppType;
		FString ImportError;
		if (!UeAgentNiagaraOps::SetObjectPropertyText(NewRenderer, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
		{
			const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
			EditContext.Emitter->RemoveRenderer(NewRenderer, EditContext.VersionGuid);
			SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
			OutData->SetStringField(TEXT("property_name"), PropertyName);
			OutData->SetStringField(TEXT("value_text"), ValueText);
			OutData->SetStringField(TEXT("cpp_type"), CppType);
			OutData->SetBoolField(TEXT("renderer_removed_after_property_failure"), true);
			OutError = ImportError.IsEmpty() ? TEXT("set_renderer_property_failed") : ImportError;
			return false;
		}

		InitialPropertyResult = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
		InitialPropertyResult->SetStringField(TEXT("cpp_type"), CppType);
	}

	EditContext.Emitter->MarkPackageDirty();

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet)
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

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	int32 AddedRendererIndex = INDEX_NONE;
	const TArray<UNiagaraRendererProperties*>& Renderers = EditContext.EmitterData->GetRenderers();
	for (int32 Index = 0; Index < Renderers.Num(); ++Index)
	{
		if (Renderers[Index] == NewRenderer)
		{
			AddedRendererIndex = Index;
			break;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetObjectField(TEXT("renderer"), UeAgentNiagaraOps::BuildRendererJson(*NewRenderer, AddedRendererIndex));
	if (InitialPropertyResult.IsValid())
	{
		OutData->SetObjectField(TEXT("initial_property_import_result"), InitialPropertyResult);
	}
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterRemoveRenderer(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	int32 RendererIndex = INDEX_NONE;
	UNiagaraRendererProperties* Renderer = nullptr;
	if (!UeAgentNiagaraOps::ResolveRendererFromParams(EditContext, Ctx.Params, RendererIndex, Renderer, OutError))
	{
		return false;
	}

	const FString RemovedRendererName = Renderer->GetFName().ToString();
	const FString RemovedRendererClass = Renderer->GetClass()->GetPathName();

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "RemoveNiagaraRenderer", "UeAgentInterface Remove Niagara Renderer"));
	EditContext.Emitter->Modify();
	EditContext.Emitter->RemoveRenderer(Renderer, EditContext.VersionGuid);
	EditContext.Emitter->MarkPackageDirty();

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet)
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
	OutData->SetStringField(TEXT("removed_renderer_name"), RemovedRendererName);
	OutData->SetStringField(TEXT("removed_renderer_class"), RemovedRendererClass);
	OutData->SetNumberField(TEXT("removed_renderer_index"), RendererIndex);
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterMoveRenderer(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	int32 RendererIndex = INDEX_NONE;
	UNiagaraRendererProperties* Renderer = nullptr;
	if (!UeAgentNiagaraOps::ResolveRendererFromParams(EditContext, Ctx.Params, RendererIndex, Renderer, OutError))
	{
		return false;
	}

	double TargetIndexNumber = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("target_index"), TargetIndexNumber))
	{
		OutError = TEXT("missing_target_index");
		return false;
	}

	const TArray<UNiagaraRendererProperties*>& ExistingRenderers = EditContext.EmitterData->GetRenderers();
	const int32 TargetIndex = FMath::RoundToInt(TargetIndexNumber);
	if (!ExistingRenderers.IsValidIndex(TargetIndex))
	{
		OutError = TEXT("invalid_target_index");
		return false;
	}

	if (RendererIndex == TargetIndex)
	{
		OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("renderer_name"), Renderer->GetFName().ToString());
		OutData->SetNumberField(TEXT("old_index"), RendererIndex);
		OutData->SetNumberField(TEXT("target_index"), TargetIndex);
		OutData->SetNumberField(TEXT("new_index"), RendererIndex);
		OutData->SetBoolField(TEXT("moved"), false);
		return true;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "MoveNiagaraRenderer", "UeAgentInterface Move Niagara Renderer"));
	EditContext.Emitter->Modify();
	Renderer->Modify();
	EditContext.Emitter->MoveRenderer(Renderer, TargetIndex, EditContext.VersionGuid);
	EditContext.Emitter->MarkPackageDirty();

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet)
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

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	int32 NewRendererIndex = INDEX_NONE;
	const TArray<UNiagaraRendererProperties*>& UpdatedRenderers = EditContext.EmitterData->GetRenderers();
	for (int32 Index = 0; Index < UpdatedRenderers.Num(); ++Index)
	{
		if (UpdatedRenderers[Index] == Renderer)
		{
			NewRendererIndex = Index;
			break;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("renderer_name"), Renderer->GetFName().ToString());
	OutData->SetNumberField(TEXT("old_index"), RendererIndex);
	OutData->SetNumberField(TEXT("target_index"), TargetIndex);
	OutData->SetNumberField(TEXT("new_index"), NewRendererIndex);
	OutData->SetObjectField(TEXT("renderer"), UeAgentNiagaraOps::BuildRendererJson(*Renderer, NewRendererIndex));
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	OutData->SetBoolField(TEXT("moved"), true);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterGetRendererProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	int32 RendererIndex = INDEX_NONE;
	UNiagaraRendererProperties* Renderer = nullptr;
	if (!UeAgentNiagaraOps::ResolveRendererFromParams(EditContext, Ctx.Params, RendererIndex, Renderer, OutError))
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
	if (!UeAgentNiagaraOps::GetObjectPropertyText(Renderer, PropertyName, ValueText))
	{
		OutError = TEXT("get_renderer_property_failed");
		return false;
	}

	FProperty* ResolvedProperty = nullptr;
	void* ValuePtr = nullptr;
	UeAgentNiagaraOps::ResolvePropertyPath(Renderer, PropertyName, ResolvedProperty, ValuePtr);

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetNumberField(TEXT("renderer_index"), RendererIndex);
	OutData->SetStringField(TEXT("renderer_name"), Renderer->GetFName().ToString());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("cpp_type"), ResolvedProperty ? ResolvedProperty->GetCPPType() : TEXT(""));
	OutData->SetBoolField(TEXT("property_found"), ResolvedProperty != nullptr);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterSetRendererProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	int32 RendererIndex = INDEX_NONE;
	UNiagaraRendererProperties* Renderer = nullptr;
	if (!UeAgentNiagaraOps::ResolveRendererFromParams(EditContext, Ctx.Params, RendererIndex, Renderer, OutError))
	{
		return false;
	}

	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.TrimStartAndEnd().IsEmpty())
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

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "SetNiagaraRendererProperty", "UeAgentInterface Set Niagara Renderer Property"));
	EditContext.Emitter->Modify();
	Renderer->Modify();
	FString AppliedValueText;
	FString CppType;
	FString ImportError;
	if (!UeAgentNiagaraOps::SetObjectPropertyText(Renderer, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		OutData->SetStringField(TEXT("renderer_name"), Renderer->GetFName().ToString());
		OutData->SetStringField(TEXT("property_name"), PropertyName.TrimStartAndEnd());
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		OutError = ImportError.IsEmpty() ? TEXT("set_renderer_property_failed") : ImportError;
		return false;
	}
	EditContext.Emitter->MarkPackageDirty();

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet)
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
	OutData->SetNumberField(TEXT("renderer_index"), RendererIndex);
	OutData->SetStringField(TEXT("renderer_name"), Renderer->GetFName().ToString());
	OutData->SetStringField(TEXT("property_name"), PropertyName.TrimStartAndEnd());
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterListEventHandlers(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	const TArray<FNiagaraEventScriptProperties>& EventHandlers = EditContext.EmitterData->GetEventHandlers();
	TArray<TSharedPtr<FJsonValue>> EventsJson;
	EventsJson.Reserve(EventHandlers.Num());
	for (int32 EventIndex = 0; EventIndex < EventHandlers.Num(); ++EventIndex)
	{
		FNiagaraEventScriptProperties& EventHandler = const_cast<FNiagaraEventScriptProperties&>(EventHandlers[EventIndex]);
		EventsJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildEventHandlerJson(EventHandler, EventIndex)));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("event_handlers"), EventsJson);
	OutData->SetNumberField(TEXT("event_handler_count"), EventsJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterAddEventHandler(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString SourceEventName = TEXT("Event");
	JsonTryGetString(Ctx.Params, TEXT("source_event_name"), SourceEventName);
	SourceEventName.TrimStartAndEndInline();
	if (SourceEventName.IsEmpty())
	{
		SourceEventName = TEXT("Event");
	}

	FGuid ScriptUsageId = FGuid::NewGuid();
	FString ScriptUsageIdText;
	if (JsonTryGetString(Ctx.Params, TEXT("script_usage_id"), ScriptUsageIdText) && !ScriptUsageIdText.IsEmpty())
	{
		if (!FGuid::Parse(ScriptUsageIdText, ScriptUsageId))
		{
			OutError = TEXT("invalid_script_usage_id");
			return false;
		}
	}

	FGuid SourceEmitterId;
	FString SourceEmitterIdText;
	if (JsonTryGetString(Ctx.Params, TEXT("source_emitter_id"), SourceEmitterIdText) && !SourceEmitterIdText.IsEmpty())
	{
		if (!FGuid::Parse(SourceEmitterIdText, SourceEmitterId))
		{
			OutError = TEXT("invalid_source_emitter_id");
			return false;
		}
	}

	FNiagaraEventScriptProperties NewEventHandler;
	NewEventHandler.SourceEventName = FName(*SourceEventName);
	NewEventHandler.SourceEmitterID = SourceEmitterId;

	FString ExecutionModeText;
	if (JsonTryGetString(Ctx.Params, TEXT("execution_mode"), ExecutionModeText) && !ExecutionModeText.IsEmpty())
	{
		EScriptExecutionMode ParsedExecutionMode = EScriptExecutionMode::EveryParticle;
		if (!UeAgentNiagaraOps::ParseExecutionMode(ExecutionModeText, ParsedExecutionMode))
		{
			OutError = TEXT("invalid_execution_mode");
			return false;
		}
		NewEventHandler.ExecutionMode = ParsedExecutionMode;
	}

	double NumberValue = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("spawn_number"), NumberValue))
	{
		NewEventHandler.SpawnNumber = FMath::Max<uint32>(0, static_cast<uint32>(FMath::RoundToInt(NumberValue)));
	}
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_events_per_frame"), NumberValue))
	{
		NewEventHandler.MaxEventsPerFrame = FMath::Max<uint32>(0, static_cast<uint32>(FMath::RoundToInt(NumberValue)));
	}
	if (JsonTryGetNumber(Ctx.Params, TEXT("min_spawn_number"), NumberValue))
	{
		NewEventHandler.MinSpawnNumber = FMath::Max<uint32>(0, static_cast<uint32>(FMath::RoundToInt(NumberValue)));
	}
	JsonTryGetBool(Ctx.Params, TEXT("random_spawn_number"), NewEventHandler.bRandomSpawnNumber);
	JsonTryGetBool(Ctx.Params, TEXT("update_attribute_initial_values"), NewEventHandler.UpdateAttributeInitialValues);

	NewEventHandler.Script = NewObject<UNiagaraScript>(
		EditContext.Emitter,
		MakeUniqueObjectName(EditContext.Emitter, UNiagaraScript::StaticClass(), TEXT("EventHandler")),
		RF_Transactional);
	if (!NewEventHandler.Script)
	{
		OutError = TEXT("create_event_handler_script_failed");
		return false;
	}
	NewEventHandler.Script->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
	NewEventHandler.Script->SetUsageId(ScriptUsageId);
	NewEventHandler.Script->SetLatestSource(EditContext.ScriptSource);

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "AddNiagaraEventHandler", "UeAgentInterface Add Niagara Event Handler"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	EditContext.Emitter->AddEventHandler(NewEventHandler, EditContext.VersionGuid);

	UNiagaraNodeOutput* EventOutputNode = UeAgentNiagaraOps::EnsureStageInputOutputBridge(
		*EditContext.Graph,
		ENiagaraScriptUsage::ParticleEventScript,
		ScriptUsageId);
	if (!EventOutputNode)
	{
		OutError = TEXT("create_event_handler_output_failed");
		return false;
	}

	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet)
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

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.Emitter, OutError))
		{
			return false;
		}
	}

	int32 AddedEventIndex = INDEX_NONE;
	FNiagaraEventScriptProperties* AddedEventHandler = nullptr;
	if (!UeAgentNiagaraOps::ResolveEventHandlerFromParams(EditContext, Ctx.Params, AddedEventIndex, AddedEventHandler, OutError))
	{
		OutError.Reset();
		for (int32 Index = 0; Index < EditContext.EmitterData->EventHandlerScriptProps.Num(); ++Index)
		{
			FNiagaraEventScriptProperties& EventHandler = EditContext.EmitterData->EventHandlerScriptProps[Index];
			if (EventHandler.Script && EventHandler.Script->GetUsageId() == ScriptUsageId)
			{
				AddedEventIndex = Index;
				AddedEventHandler = &EventHandler;
				break;
			}
		}
	}
	if (!AddedEventHandler)
	{
		OutError = TEXT("event_handler_added_but_not_resolved");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetObjectField(TEXT("event_handler"), UeAgentNiagaraOps::BuildEventHandlerJson(*AddedEventHandler, AddedEventIndex));
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterRemoveEventHandler(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	int32 EventIndex = INDEX_NONE;
	FNiagaraEventScriptProperties* EventHandler = nullptr;
	if (!UeAgentNiagaraOps::ResolveEventHandlerFromParams(EditContext, Ctx.Params, EventIndex, EventHandler, OutError))
	{
		return false;
	}

	const FString RemovedSourceEventName = EventHandler->SourceEventName.ToString();
	const FGuid RemovedUsageId = EventHandler->Script ? EventHandler->Script->GetUsageId() : FGuid();

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "RemoveNiagaraEventHandler", "UeAgentInterface Remove Niagara Event Handler"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	EditContext.Emitter->RemoveEventHandlerByUsageId(RemovedUsageId, EditContext.VersionGuid);
	if (RemovedUsageId.IsValid())
	{
		UeAgentNiagaraOps::DestroyGraphNodesForStage(*EditContext.Graph, ENiagaraScriptUsage::ParticleEventScript, RemovedUsageId);
	}
	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet)
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
	OutData->SetNumberField(TEXT("removed_event_index"), EventIndex);
	OutData->SetStringField(TEXT("removed_script_usage_id"), RemovedUsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("removed_source_event_name"), RemovedSourceEventName);
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterGetEventHandlerProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, false))
	{
		return false;
	}

	int32 EventIndex = INDEX_NONE;
	FNiagaraEventScriptProperties* EventHandler = nullptr;
	if (!UeAgentNiagaraOps::ResolveEventHandlerFromParams(EditContext, Ctx.Params, EventIndex, EventHandler, OutError))
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
	if (!UeAgentNiagaraOps::GetStructPropertyText(FNiagaraEventScriptProperties::StaticStruct(), EventHandler, EditContext.Emitter, PropertyName, ValueText, CppType))
	{
		OutError = TEXT("get_event_handler_property_failed");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetNumberField(TEXT("event_index"), EventIndex);
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("property_found"), true);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterSetEventHandlerProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	int32 EventIndex = INDEX_NONE;
	FNiagaraEventScriptProperties* EventHandler = nullptr;
	if (!UeAgentNiagaraOps::ResolveEventHandlerFromParams(EditContext, Ctx.Params, EventIndex, EventHandler, OutError))
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
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "SetNiagaraEventHandlerProperty", "UeAgentInterface Set Niagara Event Handler Property"));
	EditContext.Emitter->Modify();
	FString AppliedValueText;
	FString CppType;
	FString ImportError;
	if (!UeAgentNiagaraOps::SetStructPropertyText(FNiagaraEventScriptProperties::StaticStruct(), EventHandler, EditContext.Emitter, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		FUeAgentInterfaceLogger::Log(
			TEXT("Niagara SetEventHandlerProperty FAILED: emitter=%s event_index=%d property=%s value=%s"),
			*EditContext.Emitter->GetPathName(),
			EventIndex,
			*PropertyName,
			*ValueText);
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		OutData->SetNumberField(TEXT("event_index"), EventIndex);
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		OutError = ImportError.IsEmpty() ? TEXT("set_event_handler_property_failed") : ImportError;
		return false;
	}
	FUeAgentInterfaceLogger::Log(
		TEXT("Niagara SetEventHandlerProperty: emitter=%s event_index=%d property=%s value=%s"),
		*EditContext.Emitter->GetPathName(),
		EventIndex,
		*PropertyName,
		*ValueText);
	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bWaitForComplete = true;
	JsonTryGetBool(Ctx.Params, TEXT("wait_for_complete"), bWaitForComplete);
	bool bCompilationRequested = false;
	bool bCompilationComplete = false;
	if (bCompileAfterSet)
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
	OutData->SetNumberField(TEXT("event_index"), EventIndex);
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("compile_after_set"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("compilation_requested"), bCompilationRequested);
	OutData->SetBoolField(TEXT("compilation_complete"), bCompilationComplete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

