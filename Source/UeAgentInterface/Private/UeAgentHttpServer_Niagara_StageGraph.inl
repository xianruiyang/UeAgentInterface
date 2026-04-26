bool FUeAgentHttpServer::CmdNiagaraEmitterListStages(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	bool bIncludeModules = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_modules"), bIncludeModules);
	bool bIncludeModuleInputs = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_module_inputs"), bIncludeModuleInputs);

	TArray<UNiagaraNodeOutput*> OutputNodes;
	UeAgentNiagaraOps::GetOutputNodesFromGraph(*EditContext.Graph, OutputNodes);

	TArray<TSharedPtr<FJsonValue>> StagesJson;
	StagesJson.Reserve(OutputNodes.Num());
	for (UNiagaraNodeOutput* OutputNode : OutputNodes)
	{
		if (!OutputNode)
		{
			continue;
		}

		TSharedPtr<FJsonObject> StageObj = MakeShared<FJsonObject>();
		const ENiagaraScriptUsage Usage = OutputNode->GetUsage();
		const FGuid UsageId = OutputNode->GetUsageId();

		StageObj->SetStringField(TEXT("stage_key"), UeAgentNiagaraOps::MakeStageKey(Usage, UsageId));
		StageObj->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Usage));
		StageObj->SetStringField(TEXT("script_usage_id"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		StageObj->SetStringField(TEXT("output_node_guid"), OutputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

		FString StageType = TEXT("builtin");
		if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			StageType = TEXT("simulation_stage");
		}
		else if (Usage == ENiagaraScriptUsage::ParticleEventScript)
		{
			StageType = TEXT("event_stage");
		}
		StageObj->SetStringField(TEXT("stage_type"), StageType);

		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		UeAgentNiagaraOps::GetStageModuleNodes(*OutputNode, ModuleNodes);
		StageObj->SetNumberField(TEXT("module_count"), ModuleNodes.Num());

		if (bIncludeModules)
		{
			TArray<TSharedPtr<FJsonValue>> ModulesJson;
			ModulesJson.Reserve(ModuleNodes.Num());
			for (int32 ModuleIndex = 0; ModuleIndex < ModuleNodes.Num(); ++ModuleIndex)
			{
				UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIndex];
				if (!ModuleNode)
				{
					continue;
				}
				ModulesJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildModuleNodeJson(*ModuleNode, ModuleIndex, bIncludeModuleInputs)));
			}
			StageObj->SetArrayField(TEXT("modules"), ModulesJson);
		}

		StagesJson.Add(MakeShared<FJsonValueObject>(StageObj));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("emitter_version"), EditContext.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("stages"), StagesJson);
	OutData->SetNumberField(TEXT("stage_count"), StagesJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterAddSimulationStage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString StageClassPath = TEXT("/Script/Niagara.NiagaraSimulationStageGeneric");
	JsonTryGetString(Ctx.Params, TEXT("stage_class"), StageClassPath);
	UClass* StageClass = UeAgentNiagaraOps::ResolveClassByPath(StageClassPath);
	if (!StageClass || !StageClass->IsChildOf(UNiagaraSimulationStageBase::StaticClass()))
	{
		OutError = TEXT("invalid_stage_class");
		return false;
	}
	if (StageClass->HasAnyClassFlags(CLASS_Abstract))
	{
		OutError = TEXT("stage_class_is_abstract");
		return false;
	}

	FString StageName = TEXT("SimulationStage");
	JsonTryGetString(Ctx.Params, TEXT("stage_name"), StageName);
	StageName.TrimStartAndEndInline();
	if (StageName.IsEmpty())
	{
		StageName = TEXT("SimulationStage");
	}

	int32 TargetIndex = INDEX_NONE;
	double TargetIndexNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("target_index"), TargetIndexNumber))
	{
		TargetIndex = FMath::Max(0, FMath::RoundToInt(TargetIndexNumber));
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "AddNiagaraSimulationStage", "UeAgentInterface Add Niagara Simulation Stage"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();

	UNiagaraSimulationStageBase* AddedStage = NewObject<UNiagaraSimulationStageBase>(EditContext.Emitter, StageClass, NAME_None, RF_Transactional);
	if (!AddedStage)
	{
		OutError = TEXT("create_simulation_stage_failed");
		return false;
	}

	AddedStage->Script = NewObject<UNiagaraScript>(
		AddedStage,
		MakeUniqueObjectName(AddedStage, UNiagaraScript::StaticClass(), TEXT("SimulationStage")),
		RF_Transactional);
	if (!AddedStage->Script)
	{
		OutError = TEXT("create_simulation_stage_script_failed");
		return false;
	}

	AddedStage->SimulationStageName = FName(*StageName);
	AddedStage->Script->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
	AddedStage->Script->SetUsageId(FGuid::NewGuid());
	AddedStage->Script->SetLatestSource(EditContext.ScriptSource);
	EditContext.Emitter->AddSimulationStage(AddedStage, EditContext.VersionGuid);
	if (TargetIndex != INDEX_NONE)
	{
		EditContext.Emitter->MoveSimulationStageToIndex(AddedStage, TargetIndex, EditContext.VersionGuid);
	}

	TArray<UNiagaraNodeOutput*> ExistingOutputs;
	UeAgentNiagaraOps::GetOutputNodesFromGraph(*EditContext.Graph, ExistingOutputs);
	bool bFoundOutputNode = false;
	for (UNiagaraNodeOutput* ExistingOutput : ExistingOutputs)
	{
		if (ExistingOutput &&
			ExistingOutput->GetUsage() == ENiagaraScriptUsage::ParticleSimulationStageScript &&
			ExistingOutput->GetUsageId() == AddedStage->Script->GetUsageId())
		{
			bFoundOutputNode = true;
			break;
		}
	}

	if (!bFoundOutputNode)
	{
		UNiagaraNodeOutput* OutputNode = NewObject<UNiagaraNodeOutput>(EditContext.Graph, NAME_None, RF_Transactional);
		if (OutputNode)
		{
			OutputNode->CreateNewGuid();
			OutputNode->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
			OutputNode->SetUsageId(AddedStage->Script->GetUsageId());
			OutputNode->AllocateDefaultPins();
			EditContext.Graph->AddNode(OutputNode, true, false);
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
	OutData->SetStringField(TEXT("stage_key"), UeAgentNiagaraOps::MakeStageKey(ENiagaraScriptUsage::ParticleSimulationStageScript, AddedStage->Script->GetUsageId()));
	OutData->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(ENiagaraScriptUsage::ParticleSimulationStageScript));
	OutData->SetStringField(TEXT("script_usage_id"), AddedStage->Script->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("stage_class"), StageClass->GetPathName());
	OutData->SetStringField(TEXT("stage_name"), AddedStage->SimulationStageName.ToString());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterRemoveStage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	if (!Stage.UsageId.IsValid())
	{
		OutError = TEXT("remove_stage_requires_valid_script_usage_id");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "RemoveNiagaraStage", "UeAgentInterface Remove Niagara Stage"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();

	bool bRemoved = false;
	if (Stage.Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
	{
		if (UNiagaraSimulationStageBase* SimulationStage = EditContext.EmitterData->GetSimulationStageById(Stage.UsageId))
		{
			EditContext.Emitter->RemoveSimulationStage(SimulationStage, EditContext.VersionGuid);
			UeAgentNiagaraOps::DestroyGraphNodesForStage(*EditContext.Graph, Stage.Usage, Stage.UsageId);
			bRemoved = true;
		}
	}
	else if (Stage.Usage == ENiagaraScriptUsage::ParticleEventScript)
	{
		EditContext.Emitter->RemoveEventHandlerByUsageId(Stage.UsageId, EditContext.VersionGuid);
		UeAgentNiagaraOps::DestroyGraphNodesForStage(*EditContext.Graph, Stage.Usage, Stage.UsageId);
		bRemoved = true;
	}
	else
	{
		OutError = TEXT("built_in_stage_cannot_remove");
		return false;
	}

	if (!bRemoved)
	{
		OutError = TEXT("stage_not_found_or_not_removed");
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
	OutData->SetStringField(TEXT("removed_stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage));
	OutData->SetStringField(TEXT("script_usage_id"), Stage.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterSetStageProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
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

	UObject* TargetObject = Stage.OutputNode;
	FString TargetObjectType = TEXT("output_node");
	if (Stage.Usage == ENiagaraScriptUsage::ParticleSimulationStageScript && Stage.UsageId.IsValid())
	{
		if (UNiagaraSimulationStageBase* SimulationStage = EditContext.EmitterData->GetSimulationStageById(Stage.UsageId))
		{
			TargetObject = SimulationStage;
			TargetObjectType = TEXT("simulation_stage");
		}
	}

	if (!TargetObject)
	{
		OutError = TEXT("stage_property_target_not_found");
		return false;
	}

	FString AppliedValueText;
	FString CppType;
	FString ImportError;
	if (!UeAgentNiagaraOps::SetObjectPropertyText(TargetObject, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
		OutData->SetStringField(TEXT("target_object_type"), TargetObjectType);
		OutData->SetStringField(TEXT("target_object_path"), TargetObject->GetPathName());
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		OutError = ImportError.IsEmpty() ? TEXT("set_stage_property_failed") : ImportError;
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
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("target_object_type"), TargetObjectType);
	OutData->SetStringField(TEXT("target_object_path"), TargetObject->GetPathName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterListStageModules(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	bool bIncludeInputs = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_inputs"), bIncludeInputs);

	TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
	UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ModuleNodes);

	TArray<TSharedPtr<FJsonValue>> ModulesJson;
	ModulesJson.Reserve(ModuleNodes.Num());
	for (int32 ModuleIndex = 0; ModuleIndex < ModuleNodes.Num(); ++ModuleIndex)
	{
		UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIndex];
		if (!ModuleNode)
		{
			continue;
		}
		ModulesJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildModuleNodeJson(*ModuleNode, ModuleIndex, bIncludeInputs)));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage));
	OutData->SetStringField(TEXT("script_usage_id"), Stage.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("modules"), ModulesJson);
	OutData->SetNumberField(TEXT("module_count"), ModulesJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemListStageModules(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraSystemEditContext EditContext;
	if (!UeAgentNiagaraOps::GetSystemEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}
	if (!UeAgentNiagaraOps::IsSystemStageUsage(Stage.Usage))
	{
		OutError = TEXT("stage_is_not_system_stage");
		return false;
	}

	bool bIncludeInputs = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_inputs"), bIncludeInputs);

	TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
	UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ModuleNodes);

	TArray<TSharedPtr<FJsonValue>> ModulesJson;
	ModulesJson.Reserve(ModuleNodes.Num());
	for (int32 ModuleIndex = 0; ModuleIndex < ModuleNodes.Num(); ++ModuleIndex)
	{
		UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIndex];
		if (!ModuleNode)
		{
			continue;
		}
		ModulesJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildModuleNodeJson(*ModuleNode, ModuleIndex, bIncludeInputs)));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.System->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage));
	OutData->SetStringField(TEXT("script_usage_id"), Stage.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("modules"), ModulesJson);
	OutData->SetNumberField(TEXT("module_count"), ModulesJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterAddStageModule(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	FString ModuleScriptPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("module_script_asset_path"), ModuleScriptPath) || ModuleScriptPath.IsEmpty())
	{
		OutError = TEXT("missing_module_script_asset_path");
		return false;
	}

	UNiagaraScript* ModuleScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(ModuleScriptPath));
	if (!ModuleScript)
	{
		OutError = TEXT("module_script_not_found");
		return false;
	}

	int32 TargetIndex = INDEX_NONE;
	double TargetIndexNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("target_index"), TargetIndexNumber))
	{
		TargetIndex = FMath::RoundToInt(TargetIndexNumber);
	}

	FString SuggestedName;
	JsonTryGetString(Ctx.Params, TEXT("module_name"), SuggestedName);

	TArray<UNiagaraNodeFunctionCall*> ExistingModules;
	UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ExistingModules);
	TArray<UeAgentNiagaraOps::FLocalNiagaraStackNodeGroup> StackGroups;
	UeAgentNiagaraOps::GetLocalStackNodeGroups(*Stage.OutputNode, StackGroups);
	if (StackGroups.Num() < 2)
	{
		UNiagaraNodeOutput* ResetOutputNode = UeAgentNiagaraOps::EnsureStageInputOutputBridge(
			*EditContext.Graph,
			Stage.Usage,
			Stage.UsageId);
		if (!ResetOutputNode)
		{
			OutError = TEXT("stage_stack_invalid_add_module_repair_failed");
			return false;
		}
		Stage.OutputNode = ResetOutputNode;
		UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ExistingModules);
		FUeAgentInterfaceLogger::Log(TEXT("Niagara AddStageModule repaired invalid stack: emitter=%s stage_key=%s usage=%s module=%s"),
			*EditContext.Emitter->GetPathName(),
			*Stage.StageKey,
			*UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage),
			*ModuleScriptPath);
	}
	if (ExistingModules.Num() == 0)
	{
		UNiagaraNodeOutput* ResetOutputNode = UeAgentNiagaraOps::EnsureStageInputOutputBridge(
			*EditContext.Graph,
			Stage.Usage,
			Stage.UsageId);
		if (!ResetOutputNode)
		{
			OutError = TEXT("stage_stack_invalid_or_empty_add_module_blocked");
			return false;
		}
		Stage.OutputNode = ResetOutputNode;
		UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ExistingModules);
		FUeAgentInterfaceLogger::Log(TEXT("Niagara AddStageModule bootstrap: emitter=%s stage_key=%s usage=%s module=%s"),
			*EditContext.Emitter->GetPathName(),
			*Stage.StageKey,
			*UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage),
			*ModuleScriptPath);
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "AddNiagaraStageModule", "UeAgentInterface Add Niagara Stage Module"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	Stage.OutputNode->Modify();

	UNiagaraNodeFunctionCall* AddedModule = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
		ModuleScript,
		*Stage.OutputNode,
		TargetIndex,
		SuggestedName,
		ModuleScript->GetExposedVersion().VersionGuid);
	if (!AddedModule)
	{
		OutError = TEXT("add_stage_module_failed");
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
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetObjectField(TEXT("module"), UeAgentNiagaraOps::BuildModuleNodeJson(*AddedModule, TargetIndex, false));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterRemoveStageModule(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UNiagaraNodeFunctionCall* ModuleNode = nullptr;
	int32 ModuleIndex = INDEX_NONE;
	FString StageKey;

	FString ModuleNodeGuidText;
	if (JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) && !ModuleNodeGuidText.IsEmpty())
	{
		FGuid ModuleNodeGuid;
		if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
		{
			OutError = TEXT("invalid_module_node_guid");
			return false;
		}
		ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(EditContext.Graph, ModuleNodeGuid);
		if (!ModuleNode)
		{
			OutError = TEXT("module_node_not_found");
			return false;
		}
	}
	else
	{
		UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
		if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
		{
			return false;
		}
		StageKey = Stage.StageKey;

		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ModuleNodes);
		if (ModuleNodes.Num() == 0)
		{
			OutError = TEXT("stage_has_no_modules");
			return false;
		}

		double ModuleIndexNumber = 0.0;
		if (JsonTryGetNumber(Ctx.Params, TEXT("module_index"), ModuleIndexNumber))
		{
			ModuleIndex = FMath::RoundToInt(ModuleIndexNumber);
			if (!ModuleNodes.IsValidIndex(ModuleIndex))
			{
				OutError = TEXT("invalid_module_index");
				return false;
			}
			ModuleNode = ModuleNodes[ModuleIndex];
		}
		else
		{
			FString ModuleName;
			if (!JsonTryGetString(Ctx.Params, TEXT("module_name"), ModuleName) || ModuleName.IsEmpty())
			{
				OutError = TEXT("missing_module_identifier");
				return false;
			}
			for (int32 Index = 0; Index < ModuleNodes.Num(); ++Index)
			{
				if (ModuleNodes[Index] && ModuleNodes[Index]->GetFunctionName().Equals(ModuleName, ESearchCase::IgnoreCase))
				{
					ModuleNode = ModuleNodes[Index];
					ModuleIndex = Index;
					break;
				}
			}
			if (!ModuleNode)
			{
				OutError = TEXT("module_name_not_found");
				return false;
			}
		}
	}

	const FString RemovedModuleGuid = ModuleNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString RemovedModuleName = ModuleNode->GetFunctionName();

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "DisableNiagaraStageModule", "UeAgentInterface Disable Niagara Stage Module"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	ModuleNode->Modify();

	// NiagaraEditor 的 RemoveModuleFromStack 未对外导出（无法稳定链接），
	// 直接 DestroyNode 会破坏 Stack 图，后续 AddScriptModuleToStack 可能触发断言崩溃。
	// 这里改为“软删除”：禁用模块，保持图结构稳定。
	FNiagaraStackGraphUtilities::SetModuleIsEnabled(*ModuleNode, false);

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
	OutData->SetStringField(TEXT("stage_key"), StageKey);
	OutData->SetStringField(TEXT("removed_module_node_guid"), RemovedModuleGuid);
	OutData->SetStringField(TEXT("removed_module_name"), RemovedModuleName);
	OutData->SetNumberField(TEXT("removed_module_index"), ModuleIndex);
	OutData->SetBoolField(TEXT("soft_removed"), true);
	OutData->SetBoolField(TEXT("module_enabled"), false);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterMoveStageModule(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	double TargetIndexNumber = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("target_index"), TargetIndexNumber))
	{
		OutError = TEXT("missing_target_index");
		return false;
	}

	TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
	UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ModuleNodes);
	if (ModuleNodes.Num() == 0)
	{
		OutError = TEXT("stage_has_no_modules");
		return false;
	}

	const int32 TargetIndex = FMath::RoundToInt(TargetIndexNumber);
	if (!ModuleNodes.IsValidIndex(TargetIndex))
	{
		OutError = TEXT("invalid_target_index");
		return false;
	}

	UNiagaraNodeFunctionCall* ModuleNode = nullptr;
	int32 ModuleIndex = INDEX_NONE;

	FString ModuleNodeGuidText;
	if (JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) && !ModuleNodeGuidText.IsEmpty())
	{
		FGuid ModuleNodeGuid;
		if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
		{
			OutError = TEXT("invalid_module_node_guid");
			return false;
		}

		for (int32 Index = 0; Index < ModuleNodes.Num(); ++Index)
		{
			if (ModuleNodes[Index] && ModuleNodes[Index]->NodeGuid == ModuleNodeGuid)
			{
				ModuleNode = ModuleNodes[Index];
				ModuleIndex = Index;
				break;
			}
		}
		if (!ModuleNode)
		{
			OutError = TEXT("module_node_not_found_in_stage");
			return false;
		}
	}
	else
	{
		double ModuleIndexNumber = 0.0;
		if (JsonTryGetNumber(Ctx.Params, TEXT("module_index"), ModuleIndexNumber))
		{
			ModuleIndex = FMath::RoundToInt(ModuleIndexNumber);
			if (!ModuleNodes.IsValidIndex(ModuleIndex))
			{
				OutError = TEXT("invalid_module_index");
				return false;
			}
			ModuleNode = ModuleNodes[ModuleIndex];
		}
		else
		{
			FString ModuleName;
			if (!JsonTryGetString(Ctx.Params, TEXT("module_name"), ModuleName) || ModuleName.IsEmpty())
			{
				OutError = TEXT("missing_module_identifier");
				return false;
			}
			for (int32 Index = 0; Index < ModuleNodes.Num(); ++Index)
			{
				if (ModuleNodes[Index] && ModuleNodes[Index]->GetFunctionName().Equals(ModuleName, ESearchCase::IgnoreCase))
				{
					ModuleNode = ModuleNodes[Index];
					ModuleIndex = Index;
					break;
				}
			}
			if (!ModuleNode)
			{
				OutError = TEXT("module_name_not_found");
				return false;
			}
		}
	}

	if (ModuleIndex == TargetIndex)
	{
		OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
		OutData->SetStringField(TEXT("module_node_guid"), ModuleNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
		OutData->SetNumberField(TEXT("old_index"), ModuleIndex);
		OutData->SetNumberField(TEXT("target_index"), TargetIndex);
		OutData->SetNumberField(TEXT("new_index"), ModuleIndex);
		OutData->SetBoolField(TEXT("moved"), false);
		return true;
	}

	TArray<UeAgentNiagaraOps::FLocalNiagaraStackNodeGroup> StackGroups;
	UeAgentNiagaraOps::GetLocalStackNodeGroups(*Stage.OutputNode, StackGroups);
	const int32 SourceGroupIndex = StackGroups.IndexOfByPredicate([ModuleNode](const UeAgentNiagaraOps::FLocalNiagaraStackNodeGroup& StackNodeGroup)
	{
		return StackNodeGroup.EndNode == ModuleNode;
	});
	if (SourceGroupIndex == INDEX_NONE || SourceGroupIndex <= 0 || SourceGroupIndex >= StackGroups.Num() - 1)
	{
		OutError = TEXT("stage_module_group_not_found");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "MoveNiagaraStageModule", "UeAgentInterface Move Niagara Stage Module"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	ModuleNode->Modify();

	const UeAgentNiagaraOps::FLocalNiagaraStackNodeGroup ModuleGroup = StackGroups[SourceGroupIndex];
	UeAgentNiagaraOps::DisconnectLocalStackNodeGroup(ModuleGroup, StackGroups[SourceGroupIndex - 1], StackGroups[SourceGroupIndex + 1]);

	TArray<UeAgentNiagaraOps::FLocalNiagaraStackNodeGroup> RebuiltGroups;
	UeAgentNiagaraOps::GetLocalStackNodeGroups(*Stage.OutputNode, RebuiltGroups);
	if (RebuiltGroups.Num() < 2)
	{
		UeAgentNiagaraOps::ConnectLocalStackNodeGroup(ModuleGroup, StackGroups[SourceGroupIndex - 1], StackGroups[SourceGroupIndex + 1]);
		Transaction.Cancel();
		OutError = TEXT("stage_stack_invalid_after_disconnect");
		return false;
	}

	const int32 InsertIndex = FMath::Clamp(TargetIndex + 1, 1, RebuiltGroups.Num() - 1);
	UeAgentNiagaraOps::ConnectLocalStackNodeGroup(ModuleGroup, RebuiltGroups[InsertIndex - 1], RebuiltGroups[InsertIndex]);
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

	TArray<UNiagaraNodeFunctionCall*> UpdatedModuleNodes;
	UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, UpdatedModuleNodes);
	int32 NewIndex = INDEX_NONE;
	for (int32 Index = 0; Index < UpdatedModuleNodes.Num(); ++Index)
	{
		if (UpdatedModuleNodes[Index] == ModuleNode)
		{
			NewIndex = Index;
			break;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetNumberField(TEXT("old_index"), ModuleIndex);
	OutData->SetNumberField(TEXT("target_index"), TargetIndex);
	OutData->SetNumberField(TEXT("new_index"), NewIndex);
	OutData->SetObjectField(TEXT("module"), UeAgentNiagaraOps::BuildModuleNodeJson(*ModuleNode, NewIndex, false));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	OutData->SetBoolField(TEXT("moved"), true);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterListStageNodes(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	bool bIncludeProperties = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_properties"), bIncludeProperties);
	bool bIncludeModuleInputs = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_module_inputs"), bIncludeModuleInputs);

	TSet<UEdGraphNode*> VisitedNodes;
	TArray<UEdGraphNode*> StageNodes;
	UeAgentNiagaraOps::GatherUpstreamNodesRecursive(Stage.OutputNode, VisitedNodes, StageNodes);
	StageNodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
	{
		if (A.NodePosX != B.NodePosX)
		{
			return A.NodePosX < B.NodePosX;
		}
		return A.NodePosY < B.NodePosY;
	});

	TArray<TSharedPtr<FJsonValue>> NodesJson;
	NodesJson.Reserve(StageNodes.Num());
	for (int32 NodeIndex = 0; NodeIndex < StageNodes.Num(); ++NodeIndex)
	{
		UEdGraphNode* StageNode = StageNodes[NodeIndex];
		if (!StageNode)
		{
			continue;
		}
		NodesJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildStageNodeJson(*StageNode, NodeIndex, bIncludeProperties, bIncludeModuleInputs)));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("script_usage"), UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage));
	OutData->SetStringField(TEXT("script_usage_id"), Stage.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetArrayField(TEXT("nodes"), NodesJson);
	OutData->SetNumberField(TEXT("node_count"), NodesJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterGetStageNodeProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	UEdGraphNode* StageNode = nullptr;
	if (!UeAgentNiagaraOps::ResolveStageNodeFromParams(*EditContext.Graph, Ctx.Params, Stage, StageNode, OutError))
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
	if (!UeAgentNiagaraOps::GetObjectPropertyText(StageNode, PropertyName, ValueText))
	{
		OutError = TEXT("get_stage_node_property_failed");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("node_guid"), StageNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("node_class"), StageNode->GetClass()->GetPathName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterSetStageNodeProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	UEdGraphNode* StageNode = nullptr;
	if (!UeAgentNiagaraOps::ResolveStageNodeFromParams(*EditContext.Graph, Ctx.Params, Stage, StageNode, OutError))
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

	FString AppliedValueText;
	FString CppType;
	FString ImportError;
	if (!UeAgentNiagaraOps::SetObjectPropertyText(StageNode, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
	{
		const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
		OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
		OutData->SetStringField(TEXT("node_guid"), StageNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		OutData->SetStringField(TEXT("node_class"), StageNode->GetClass()->GetPathName());
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
		OutData->SetStringField(TEXT("cpp_type"), CppType);
		OutError = ImportError.IsEmpty() ? TEXT("set_stage_node_property_failed") : ImportError;
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
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("node_guid"), StageNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("node_class"), StageNode->GetClass()->GetPathName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	SetPropertyImportResultFields(OutData, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
	OutData->SetStringField(TEXT("cpp_type"), CppType);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterRemoveStageNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	UEdGraphNode* StageNode = nullptr;
	if (!UeAgentNiagaraOps::ResolveStageNodeFromParams(*EditContext.Graph, Ctx.Params, Stage, StageNode, OutError))
	{
		return false;
	}

	if (StageNode == Stage.OutputNode)
	{
		OutError = TEXT("cannot_remove_stage_output_node_use_remove_stage");
		return false;
	}

	const FString RemovedNodeGuid = StageNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString RemovedNodeClass = StageNode->GetClass()->GetPathName();

	if (UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(StageNode))
	{
		FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "DisableNiagaraStageNodeModule", "UeAgentInterface Disable Niagara Stage Node Module"));
		EditContext.Emitter->Modify();
		EditContext.Graph->Modify();
		ModuleNode->Modify();
		FNiagaraStackGraphUtilities::SetModuleIsEnabled(*ModuleNode, false);
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
		OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
		OutData->SetStringField(TEXT("removed_node_guid"), RemovedNodeGuid);
		OutData->SetStringField(TEXT("removed_node_class"), RemovedNodeClass);
		OutData->SetBoolField(TEXT("soft_removed"), true);
		OutData->SetBoolField(TEXT("module_enabled"), false);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "RemoveNiagaraStageNode", "UeAgentInterface Remove Niagara Stage Node"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	StageNode->Modify();
	for (UEdGraphPin* Pin : StageNode->Pins)
	{
		if (Pin && Pin->LinkedTo.Num() > 0)
		{
			Pin->BreakAllPinLinks(true);
		}
	}
	StageNode->DestroyNode();

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
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("removed_node_guid"), RemovedNodeGuid);
	OutData->SetStringField(TEXT("removed_node_class"), RemovedNodeClass);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterSetStageModuleEnabled(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ModuleNodeGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) || ModuleNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_module_node_guid");
		return false;
	}

	FGuid ModuleNodeGuid;
	if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
	{
		OutError = TEXT("invalid_module_node_guid");
		return false;
	}

	UNiagaraNodeFunctionCall* ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(EditContext.Graph, ModuleNodeGuid);
	if (!ModuleNode)
	{
		OutError = TEXT("module_node_not_found");
		return false;
	}

	bool bEnabled = true;
	if (!JsonTryGetBool(Ctx.Params, TEXT("enabled"), bEnabled))
	{
		OutError = TEXT("missing_enabled");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "SetNiagaraStageModuleEnabled", "UeAgentInterface Set Niagara Stage Module Enabled"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	ModuleNode->Modify();

	FNiagaraStackGraphUtilities::SetModuleIsEnabled(*ModuleNode, bEnabled);
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
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetBoolField(TEXT("enabled"), bEnabled);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterListModuleInputs(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ModuleNodeGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) || ModuleNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_module_node_guid");
		return false;
	}

	FGuid ModuleNodeGuid;
	if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
	{
		OutError = TEXT("invalid_module_node_guid");
		return false;
	}

	UNiagaraNodeFunctionCall* ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(EditContext.Graph, ModuleNodeGuid);
	if (!ModuleNode)
	{
		OutError = TEXT("module_node_not_found");
		return false;
	}

	TArray<UeAgentNiagaraOps::FResolvedNiagaraModuleInput> ModuleInputs;
	UeAgentNiagaraOps::BuildResolvedModuleInputs(*ModuleNode, ModuleInputs);
	TArray<TSharedPtr<FJsonValue>> InputsJson;
	InputsJson.Reserve(ModuleInputs.Num());
	for (const UeAgentNiagaraOps::FResolvedNiagaraModuleInput& InputData : ModuleInputs)
	{
		InputsJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildModuleInputJson(*ModuleNode, InputData)));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.Emitter->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetArrayField(TEXT("inputs"), InputsJson);
	OutData->SetNumberField(TEXT("input_count"), InputsJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemListModuleInputs(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraSystemEditContext EditContext;
	if (!UeAgentNiagaraOps::GetSystemEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ModuleNodeGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) || ModuleNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_module_node_guid");
		return false;
	}

	FGuid ModuleNodeGuid;
	if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
	{
		OutError = TEXT("invalid_module_node_guid");
		return false;
	}

	UNiagaraNodeFunctionCall* ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(EditContext.Graph, ModuleNodeGuid);
	if (!ModuleNode)
	{
		OutError = TEXT("module_node_not_found");
		return false;
	}

	TArray<UeAgentNiagaraOps::FResolvedNiagaraModuleInput> ModuleInputs;
	UeAgentNiagaraOps::BuildResolvedModuleInputs(*ModuleNode, ModuleInputs);
	TArray<TSharedPtr<FJsonValue>> InputsJson;
	InputsJson.Reserve(ModuleInputs.Num());
	for (const UeAgentNiagaraOps::FResolvedNiagaraModuleInput& InputData : ModuleInputs)
	{
		InputsJson.Add(MakeShared<FJsonValueObject>(UeAgentNiagaraOps::BuildModuleInputJson(*ModuleNode, InputData)));
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.System->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetArrayField(TEXT("inputs"), InputsJson);
	OutData->SetNumberField(TEXT("input_count"), InputsJson.Num());
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterSetModuleInput(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ModuleNodeGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) || ModuleNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_module_node_guid");
		return false;
	}

	FGuid ModuleNodeGuid;
	if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
	{
		OutError = TEXT("invalid_module_node_guid");
		return false;
	}

	FString InputName;
	if (!JsonTryGetString(Ctx.Params, TEXT("input_name"), InputName) || InputName.IsEmpty())
	{
		OutError = TEXT("missing_input_name");
		return false;
	}

	FString ValueText;
	const bool bHasValueText = JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText);

	UNiagaraNodeFunctionCall* ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(EditContext.Graph, ModuleNodeGuid);
	if (!ModuleNode)
	{
		OutError = TEXT("module_node_not_found");
		return false;
	}

	TArray<UeAgentNiagaraOps::FResolvedNiagaraModuleInput> ModuleInputs;
	UeAgentNiagaraOps::BuildResolvedModuleInputs(*ModuleNode, ModuleInputs);
	FString ResolvedInputName;
	const UeAgentNiagaraOps::FResolvedNiagaraModuleInput* ResolvedInput =
		UeAgentNiagaraOps::ResolveModuleInputByName(ModuleInputs, InputName, ResolvedInputName);
	if (!ResolvedInput || !ResolvedInput->InputVariable.IsValid())
	{
		OutError = TEXT("input_name_not_found");
		return false;
	}

	FString ValueTextForApply = ValueText;

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "SetNiagaraModuleInput", "UeAgentInterface Set Niagara Module Input"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	ModuleNode->Modify();
	auto CancelAndFail = [&Transaction, &OutError](const FString& ErrorText) -> bool
	{
		Transaction.Cancel();
		OutError = ErrorText;
		return false;
	};

	const FNiagaraParameterHandle ModuleInputHandle(ResolvedInput->InputVariable.GetName());
	const FNiagaraParameterHandle AliasedInputHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleInputHandle, ModuleNode);
	UEdGraphPin* ExistingOverridePin =
		UeAgentNiagaraOps::FindOverridePinByAliasedInputName(EditContext.Graph, AliasedInputHandle.GetParameterHandleString());
	bool bBreakInputLinks = true;
	JsonTryGetBool(Ctx.Params, TEXT("break_input_links"), bBreakInputLinks);
	UEdGraphPin* OverridePinPtr = ResolvedInput->VisiblePin;
	FString WriteTarget = OverridePinPtr ? TEXT("visible_pin") : TEXT("stack_override_pin");
	if (OverridePinPtr)
	{
		if (UEdGraphNode* OverrideOwnerNode = OverridePinPtr->GetOwningNode())
		{
			OverrideOwnerNode->Modify();
		}
		if (bBreakInputLinks && OverridePinPtr->LinkedTo.Num() > 0)
		{
			OverridePinPtr->BreakAllPinLinks();
		}

	}
	else
	{
		if (bBreakInputLinks && ExistingOverridePin && ExistingOverridePin->LinkedTo.Num() > 0)
		{
			ExistingOverridePin->BreakAllPinLinks();
		}

		FString OverridePinError;
		if (!UeAgentNiagaraOps::TryGetOrCreateStackFunctionInputOverridePin(
			*ModuleNode,
			AliasedInputHandle,
			ResolvedInput->InputVariable.GetType(),
			ResolvedInput->InputScriptVariableGuid,
			OverridePinPtr,
			OverridePinError) || !OverridePinPtr)
		{
			return CancelAndFail(FString::Printf(TEXT("module_input_override_unavailable:%s"), *OverridePinError));
		}
	}
	UEdGraphPin& OverridePin = *OverridePinPtr;

	FString LinkedParameterName;
	FString ApplyVerificationStatus = TEXT("not_run");
	FString ApplyVerificationError;
	if (!JsonTryGetString(Ctx.Params, TEXT("link_parameter_name"), LinkedParameterName))
	{
		JsonTryGetString(Ctx.Params, TEXT("link_parameter"), LinkedParameterName);
	}
	LinkedParameterName.TrimStartAndEndInline();
	if (LinkedParameterName.IsEmpty() && bHasValueText)
	{
		const FString ValueTextTrimmed = ValueTextForApply.TrimStartAndEnd();
		if (ValueTextTrimmed.StartsWith(TEXT("link:"), ESearchCase::IgnoreCase))
		{
			LinkedParameterName = ValueTextTrimmed.Mid(5).TrimStartAndEnd();
		}
	}

	if (!bHasValueText && LinkedParameterName.IsEmpty())
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	if (LinkedParameterName.IsEmpty())
	{
		const FNiagaraTypeDefinition InputType = ResolvedInput->InputVariable.GetType();
		FString NormalizeError;
		if (!UeAgentNiagaraOps::NormalizeNiagaraValueTextForPinDefault(InputType, ValueText, ValueTextForApply, NormalizeError))
		{
			return CancelAndFail(FString::Printf(TEXT("invalid_value_text_for_input:%s:%s"), *ResolvedInputName, *NormalizeError));
		}
	}

	const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>();
	if (!LinkedParameterName.IsEmpty())
	{
		if (Cast<UNiagaraNodeFunctionCall>(OverridePin.GetOwningNode()) != nullptr)
		{
			return CancelAndFail(TEXT("input_link_not_supported_for_static_switch"));
		}

		if (OverridePin.LinkedTo.Num() > 0)
		{
			OverridePin.BreakAllPinLinks();
		}
		if (NiagaraSchema)
		{
			NiagaraSchema->TrySetDefaultValue(OverridePin, FString(), true);
		}
		else
		{
			OverridePin.DefaultValue.Empty();
		}

		TSet<FNiagaraVariableBase> KnownParameters;
		const FNiagaraVariableBase LinkedParameter(ResolvedInput->InputVariable.GetType(), *LinkedParameterName);
		FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(OverridePin, LinkedParameter, KnownParameters);
		FUeAgentInterfaceLogger::Log(
			TEXT("Niagara SetModuleInput linked: emitter=%s module=%s input=%s link=%s"),
			*EditContext.Emitter->GetPathName(),
			*ModuleNode->GetFunctionName(),
			*ResolvedInputName,
			*LinkedParameterName);
	}
	else
	{
		if (OverridePin.LinkedTo.Num() > 0 && !bBreakInputLinks)
		{
			return CancelAndFail(TEXT("input_has_linked_override_set_break_input_links_true"));
		}
		if (NiagaraSchema)
		{
			NiagaraSchema->TrySetDefaultValue(OverridePin, ValueTextForApply, true);
		}
		else
		{
			OverridePin.DefaultValue = ValueTextForApply;
		}
		FUeAgentInterfaceLogger::Log(
			TEXT("Niagara SetModuleInput value: emitter=%s module=%s input=%s requested=%s applied=%s"),
			*EditContext.Emitter->GetPathName(),
			*ModuleNode->GetFunctionName(),
			*ResolvedInputName,
			*ValueText,
			*ValueTextForApply);

		const FNiagaraTypeDefinition InputType = ResolvedInput->InputVariable.GetType();
		if (InputType.IsEnum())
		{
			UEnum* EnumType = InputType.GetEnum();
			if (!EnumType)
			{
				return CancelAndFail(TEXT("input_enum_type_not_found"));
			}

			int64 AppliedEnumValue = 0;
			if (!UeAgentNiagaraOps::TryResolveNiagaraEnumValueFromText(*EnumType, OverridePin.DefaultValue, AppliedEnumValue))
			{
				FUeAgentInterfaceLogger::Log(
					TEXT("Niagara SetModuleInput REJECTED enum apply: emitter=%s module=%s input=%s requested=%s applied_default=%s"),
					*EditContext.Emitter->GetPathName(),
					*ModuleNode->GetFunctionName(),
					*ResolvedInputName,
					*ValueText,
					*OverridePin.DefaultValue);
				return CancelAndFail(FString::Printf(TEXT("invalid_applied_enum_value_for_input:%s"), *ResolvedInputName));
			}
		}

		if (!UeAgentNiagaraOps::ValidateNiagaraPinDefaultValueAfterSet(InputType, ValueTextForApply, OverridePin.DefaultValue, ApplyVerificationStatus, ApplyVerificationError))
		{
			FUeAgentInterfaceLogger::Log(
				TEXT("Niagara SetModuleInput REJECTED default apply: emitter=%s module=%s input=%s requested=%s applied=%s stored=%s status=%s error=%s"),
				*EditContext.Emitter->GetPathName(),
				*ModuleNode->GetFunctionName(),
				*ResolvedInputName,
				*ValueText,
				*ValueTextForApply,
				*OverridePin.DefaultValue,
				*ApplyVerificationStatus,
				*ApplyVerificationError);
			return CancelAndFail(FString::Printf(TEXT("pin_default_apply_verification_failed:%s:%s"), *ResolvedInputName, *ApplyVerificationError));
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
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetStringField(TEXT("input_name"), ResolvedInputName);
	OutData->SetStringField(TEXT("input_type"), ResolvedInput->InputVariable.GetType().GetNameText().ToString());
	OutData->SetStringField(TEXT("override_default_value"), OverridePin.DefaultValue);
	OutData->SetStringField(TEXT("requested_value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), ValueTextForApply);
	OutData->SetBoolField(TEXT("value_text_normalized"), !LinkedParameterName.IsEmpty() ? false : !ValueTextForApply.Equals(ValueText, ESearchCase::CaseSensitive));
	OutData->SetBoolField(TEXT("default_value_apply_verified"), !LinkedParameterName.IsEmpty() ? false : ApplyVerificationError.IsEmpty());
	OutData->SetStringField(TEXT("default_value_apply_status"), !LinkedParameterName.IsEmpty() ? TEXT("linked_parameter") : ApplyVerificationStatus);
	if (!ApplyVerificationError.IsEmpty())
	{
		OutData->SetStringField(TEXT("default_value_apply_error"), ApplyVerificationError);
	}
	OutData->SetStringField(TEXT("write_target"), WriteTarget);
	OutData->SetBoolField(TEXT("is_linked_value"), !LinkedParameterName.IsEmpty());
	if (!LinkedParameterName.IsEmpty())
	{
		OutData->SetStringField(TEXT("linked_parameter_name"), LinkedParameterName);
	}
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraSystemSetModuleInput(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraSystemEditContext EditContext;
	if (!UeAgentNiagaraOps::GetSystemEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ModuleNodeGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) || ModuleNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_module_node_guid");
		return false;
	}

	FGuid ModuleNodeGuid;
	if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
	{
		OutError = TEXT("invalid_module_node_guid");
		return false;
	}

	FString InputName;
	if (!JsonTryGetString(Ctx.Params, TEXT("input_name"), InputName) || InputName.IsEmpty())
	{
		OutError = TEXT("missing_input_name");
		return false;
	}

	FString ValueText;
	const bool bHasValueText = JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText);

	UNiagaraNodeFunctionCall* ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(EditContext.Graph, ModuleNodeGuid);
	if (!ModuleNode)
	{
		OutError = TEXT("module_node_not_found");
		return false;
	}

	TArray<UeAgentNiagaraOps::FResolvedNiagaraModuleInput> ModuleInputs;
	UeAgentNiagaraOps::BuildResolvedModuleInputs(*ModuleNode, ModuleInputs);
	FString ResolvedInputName;
	const UeAgentNiagaraOps::FResolvedNiagaraModuleInput* ResolvedInput =
		UeAgentNiagaraOps::ResolveModuleInputByName(ModuleInputs, InputName, ResolvedInputName);
	if (!ResolvedInput || !ResolvedInput->InputVariable.IsValid())
	{
		OutError = TEXT("input_name_not_found");
		return false;
	}

	FString ValueTextForApply = ValueText;
	FString LinkedParameterName;
	if (!JsonTryGetString(Ctx.Params, TEXT("link_parameter_name"), LinkedParameterName))
	{
		JsonTryGetString(Ctx.Params, TEXT("link_parameter"), LinkedParameterName);
	}
	LinkedParameterName.TrimStartAndEndInline();
	if (LinkedParameterName.IsEmpty() && bHasValueText)
	{
		const FString ValueTextTrimmed = ValueTextForApply.TrimStartAndEnd();
		if (ValueTextTrimmed.StartsWith(TEXT("link:"), ESearchCase::IgnoreCase))
		{
			LinkedParameterName = ValueTextTrimmed.Mid(5).TrimStartAndEnd();
		}
	}

	if (!bHasValueText && LinkedParameterName.IsEmpty())
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "SetNiagaraSystemModuleInput", "UeAgentInterface Set Niagara System Module Input"));
	EditContext.System->Modify();
	EditContext.Graph->Modify();
	ModuleNode->Modify();
	auto CancelAndFail = [&Transaction, &OutError](const FString& ErrorText) -> bool
	{
		Transaction.Cancel();
		OutError = ErrorText;
		return false;
	};

	const FNiagaraParameterHandle ModuleInputHandle(ResolvedInput->InputVariable.GetName());
	const FNiagaraParameterHandle AliasedInputHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleInputHandle, ModuleNode);
	UEdGraphPin* ExistingOverridePin =
		UeAgentNiagaraOps::FindOverridePinByAliasedInputName(EditContext.Graph, AliasedInputHandle.GetParameterHandleString());
	bool bBreakInputLinks = true;
	JsonTryGetBool(Ctx.Params, TEXT("break_input_links"), bBreakInputLinks);
	UEdGraphPin* OverridePinPtr = ResolvedInput->VisiblePin;
	FString WriteTarget = OverridePinPtr ? TEXT("visible_pin") : TEXT("stack_override_pin");
	if (OverridePinPtr)
	{
		if (UEdGraphNode* OverrideOwnerNode = OverridePinPtr->GetOwningNode())
		{
			OverrideOwnerNode->Modify();
		}
		if (bBreakInputLinks && OverridePinPtr->LinkedTo.Num() > 0)
		{
			OverridePinPtr->BreakAllPinLinks();
		}
	}
	else
	{
		if (bBreakInputLinks && ExistingOverridePin && ExistingOverridePin->LinkedTo.Num() > 0)
		{
			ExistingOverridePin->BreakAllPinLinks();
		}

		FString OverridePinError;
		if (!UeAgentNiagaraOps::TryGetOrCreateStackFunctionInputOverridePin(
			*ModuleNode,
			AliasedInputHandle,
			ResolvedInput->InputVariable.GetType(),
			ResolvedInput->InputScriptVariableGuid,
			OverridePinPtr,
			OverridePinError) || !OverridePinPtr)
		{
			return CancelAndFail(FString::Printf(TEXT("module_input_override_unavailable:%s"), *OverridePinError));
		}
	}
	UEdGraphPin& OverridePin = *OverridePinPtr;
	FString ApplyVerificationStatus = TEXT("not_run");
	FString ApplyVerificationError;

	if (LinkedParameterName.IsEmpty())
	{
		const FNiagaraTypeDefinition InputType = ResolvedInput->InputVariable.GetType();
		FString NormalizeError;
		if (!UeAgentNiagaraOps::NormalizeNiagaraValueTextForPinDefault(InputType, ValueText, ValueTextForApply, NormalizeError))
		{
			return CancelAndFail(FString::Printf(TEXT("invalid_value_text_for_input:%s:%s"), *ResolvedInputName, *NormalizeError));
		}
	}

	const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>();
	if (!LinkedParameterName.IsEmpty())
	{
		if (Cast<UNiagaraNodeFunctionCall>(OverridePin.GetOwningNode()) != nullptr)
		{
			return CancelAndFail(TEXT("input_link_not_supported_for_static_switch"));
		}

		if (OverridePin.LinkedTo.Num() > 0)
		{
			OverridePin.BreakAllPinLinks();
		}
		if (NiagaraSchema)
		{
			NiagaraSchema->TrySetDefaultValue(OverridePin, FString(), true);
		}
		else
		{
			OverridePin.DefaultValue.Empty();
		}

		TSet<FNiagaraVariableBase> KnownParameters;
		const FNiagaraVariableBase LinkedParameter(ResolvedInput->InputVariable.GetType(), *LinkedParameterName);
		FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(OverridePin, LinkedParameter, KnownParameters);
		FUeAgentInterfaceLogger::Log(
			TEXT("Niagara System SetModuleInput linked: system=%s module=%s input=%s link=%s"),
			*EditContext.System->GetPathName(),
			*ModuleNode->GetFunctionName(),
			*ResolvedInputName,
			*LinkedParameterName);
	}
	else
	{
		if (OverridePin.LinkedTo.Num() > 0 && !bBreakInputLinks)
		{
			return CancelAndFail(TEXT("input_has_linked_override_set_break_input_links_true"));
		}
		if (NiagaraSchema)
		{
			NiagaraSchema->TrySetDefaultValue(OverridePin, ValueTextForApply, true);
		}
		else
		{
			OverridePin.DefaultValue = ValueTextForApply;
		}
		FUeAgentInterfaceLogger::Log(
			TEXT("Niagara System SetModuleInput value: system=%s module=%s input=%s requested=%s applied=%s"),
			*EditContext.System->GetPathName(),
			*ModuleNode->GetFunctionName(),
			*ResolvedInputName,
			*ValueText,
			*ValueTextForApply);

		const FNiagaraTypeDefinition InputType = ResolvedInput->InputVariable.GetType();
		if (InputType.IsEnum())
		{
			UEnum* EnumType = InputType.GetEnum();
			if (!EnumType)
			{
				return CancelAndFail(TEXT("input_enum_type_not_found"));
			}

			int64 AppliedEnumValue = 0;
			if (!UeAgentNiagaraOps::TryResolveNiagaraEnumValueFromText(*EnumType, OverridePin.DefaultValue, AppliedEnumValue))
			{
				FUeAgentInterfaceLogger::Log(
					TEXT("Niagara System SetModuleInput REJECTED enum apply: system=%s module=%s input=%s requested=%s applied_default=%s"),
					*EditContext.System->GetPathName(),
					*ModuleNode->GetFunctionName(),
					*ResolvedInputName,
					*ValueText,
					*OverridePin.DefaultValue);
				return CancelAndFail(FString::Printf(TEXT("invalid_applied_enum_value_for_input:%s"), *ResolvedInputName));
			}
		}

		if (!UeAgentNiagaraOps::ValidateNiagaraPinDefaultValueAfterSet(InputType, ValueTextForApply, OverridePin.DefaultValue, ApplyVerificationStatus, ApplyVerificationError))
		{
			FUeAgentInterfaceLogger::Log(
				TEXT("Niagara System SetModuleInput REJECTED default apply: system=%s module=%s input=%s requested=%s applied=%s stored=%s status=%s error=%s"),
				*EditContext.System->GetPathName(),
				*ModuleNode->GetFunctionName(),
				*ResolvedInputName,
				*ValueText,
				*ValueTextForApply,
				*OverridePin.DefaultValue,
				*ApplyVerificationStatus,
				*ApplyVerificationError);
			return CancelAndFail(FString::Printf(TEXT("pin_default_apply_verification_failed:%s:%s"), *ResolvedInputName, *ApplyVerificationError));
		}
	}

	const UeAgentNiagaraOps::FNiagaraSystemRefreshResult RefreshResult = UeAgentNiagaraOps::MarkSystemGraphEdited(EditContext);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentNiagaraOps::SaveAssetPackage(EditContext.System, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), EditContext.System->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetStringField(TEXT("input_name"), ResolvedInputName);
	OutData->SetStringField(TEXT("input_type"), ResolvedInput->InputVariable.GetType().GetNameText().ToString());
	OutData->SetStringField(TEXT("override_default_value"), OverridePin.DefaultValue);
	OutData->SetStringField(TEXT("requested_value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), ValueTextForApply);
	OutData->SetBoolField(TEXT("value_text_normalized"), !LinkedParameterName.IsEmpty() ? false : !ValueTextForApply.Equals(ValueText, ESearchCase::CaseSensitive));
	OutData->SetBoolField(TEXT("default_value_apply_verified"), !LinkedParameterName.IsEmpty() ? false : ApplyVerificationError.IsEmpty());
	OutData->SetStringField(TEXT("default_value_apply_status"), !LinkedParameterName.IsEmpty() ? TEXT("linked_parameter") : ApplyVerificationStatus);
	if (!ApplyVerificationError.IsEmpty())
	{
		OutData->SetStringField(TEXT("default_value_apply_error"), ApplyVerificationError);
	}
	OutData->SetStringField(TEXT("write_target"), WriteTarget);
	OutData->SetBoolField(TEXT("is_linked_value"), !LinkedParameterName.IsEmpty());
	if (!LinkedParameterName.IsEmpty())
	{
		OutData->SetStringField(TEXT("linked_parameter_name"), LinkedParameterName);
	}
	OutData->SetObjectField(TEXT("system_refresh"), UeAgentNiagaraOps::BuildNiagaraSystemRefreshJson(RefreshResult));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterClearModuleInput(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	FString ModuleNodeGuidText;
	if (!JsonTryGetString(Ctx.Params, TEXT("module_node_guid"), ModuleNodeGuidText) || ModuleNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_module_node_guid");
		return false;
	}

	FGuid ModuleNodeGuid;
	if (!FGuid::Parse(ModuleNodeGuidText, ModuleNodeGuid))
	{
		OutError = TEXT("invalid_module_node_guid");
		return false;
	}

	FString InputName;
	if (!JsonTryGetString(Ctx.Params, TEXT("input_name"), InputName) || InputName.IsEmpty())
	{
		OutError = TEXT("missing_input_name");
		return false;
	}

	UNiagaraNodeFunctionCall* ModuleNode = UeAgentNiagaraOps::FindModuleNodeByGuid(EditContext.Graph, ModuleNodeGuid);
	if (!ModuleNode)
	{
		OutError = TEXT("module_node_not_found");
		return false;
	}

	TArray<UeAgentNiagaraOps::FResolvedNiagaraModuleInput> ModuleInputs;
	UeAgentNiagaraOps::BuildResolvedModuleInputs(*ModuleNode, ModuleInputs);
	FString ResolvedInputName;
	const UeAgentNiagaraOps::FResolvedNiagaraModuleInput* ResolvedInput =
		UeAgentNiagaraOps::ResolveModuleInputByName(ModuleInputs, InputName, ResolvedInputName);
	if (!ResolvedInput || !ResolvedInput->InputVariable.IsValid())
	{
		OutError = TEXT("input_name_not_found");
		return false;
	}

	const FNiagaraParameterHandle ModuleInputHandle(ResolvedInput->InputVariable.GetName());
	const FNiagaraParameterHandle AliasedInputHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleInputHandle, ModuleNode);
	UEdGraphPin* OverridePin =
		UeAgentNiagaraOps::FindOverridePinByAliasedInputName(EditContext.Graph, AliasedInputHandle.GetParameterHandleString());
	const bool bHasOverride = OverridePin != nullptr &&
		(OverridePin->LinkedTo.Num() > 0 || OverridePin->DefaultValue != OverridePin->AutogeneratedDefaultValue);
	if (!bHasOverride)
	{
		OutError = TEXT("module_input_has_no_override");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "ClearNiagaraModuleInput", "UeAgentInterface Clear Niagara Module Input"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	ModuleNode->Modify();

	OverridePin->BreakAllPinLinks();

	if (UEdGraphNode* OverrideOwnerNode = OverridePin->GetOwningNode())
	{
		const bool bIsStaticSwitchInput = Cast<UNiagaraNodeFunctionCall>(OverrideOwnerNode) != nullptr;
		if (bIsStaticSwitchInput)
		{
			const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>();
			if (NiagaraSchema)
			{
				NiagaraSchema->TrySetDefaultValue(*OverridePin, OverridePin->AutogeneratedDefaultValue, true);
			}
			else
			{
				OverridePin->DefaultValue = OverridePin->AutogeneratedDefaultValue;
			}
		}
		else
		{
			OverrideOwnerNode->Modify();
			OverrideOwnerNode->RemovePin(OverridePin);
		}
	}
	else
	{
		const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>();
		if (NiagaraSchema)
		{
			NiagaraSchema->TrySetDefaultValue(*OverridePin, OverridePin->AutogeneratedDefaultValue, true);
		}
		else
		{
			OverridePin->DefaultValue = OverridePin->AutogeneratedDefaultValue;
		}
	}
	UeAgentNiagaraOps::MarkEmitterGraphEdited(EditContext);
	FUeAgentInterfaceLogger::Log(
		TEXT("Niagara ClearModuleInput: emitter=%s module=%s input=%s"),
		*EditContext.Emitter->GetPathName(),
		*ModuleNode->GetFunctionName(),
		*ResolvedInputName);

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
	OutData->SetStringField(TEXT("module_node_guid"), ModuleNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
	OutData->SetStringField(TEXT("cleared_input_name"), ResolvedInputName);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterAddStageNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	double NodePosX = 0.0;
	double NodePosY = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("node_pos_x"), NodePosX);
	JsonTryGetNumber(Ctx.Params, TEXT("node_pos_y"), NodePosY);

	UEdGraphNode* AddedNode = nullptr;
	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "AddNiagaraStageNode", "UeAgentInterface Add Niagara Stage Node"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();

	FString ModuleScriptPath;
	const bool bHasModuleScriptPath = JsonTryGetString(Ctx.Params, TEXT("module_script_asset_path"), ModuleScriptPath) && !ModuleScriptPath.IsEmpty();
	if (bHasModuleScriptPath)
	{
		if (Stage.Usage == ENiagaraScriptUsage::ParticleEventScript)
		{
			FUeAgentInterfaceLogger::Log(TEXT("Niagara AddStageNode blocked: emitter=%s stage_key=%s usage=%s module=%s reason=particle_event_script_crash_guard"),
				*EditContext.Emitter->GetPathName(),
				*Stage.StageKey,
				*UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage),
				*ModuleScriptPath);
			OutError = TEXT("stage_add_stage_node_particle_event_script_blocked");
			return false;
		}

		TArray<UNiagaraNodeFunctionCall*> ExistingModules;
		UeAgentNiagaraOps::GetStageModuleNodes(*Stage.OutputNode, ExistingModules);
		if (ExistingModules.Num() == 0)
		{
			FUeAgentInterfaceLogger::Log(TEXT("Niagara AddStageNode blocked: emitter=%s stage_key=%s usage=%s module=%s reason=stage_stack_invalid_or_empty"),
				*EditContext.Emitter->GetPathName(),
				*Stage.StageKey,
				*UeAgentNiagaraOps::ScriptUsageToString(Stage.Usage),
				*ModuleScriptPath);
			OutError = TEXT("stage_stack_invalid_or_empty_add_node_blocked");
			return false;
		}
	}
	if (bHasModuleScriptPath)
	{
		UNiagaraScript* ModuleScript = Cast<UNiagaraScript>(UeAgentNiagaraOps::LoadAssetObject(ModuleScriptPath));
		if (!ModuleScript)
		{
			OutError = TEXT("module_script_not_found");
			return false;
		}

		int32 TargetIndex = INDEX_NONE;
		double TargetIndexNumber = 0.0;
		if (JsonTryGetNumber(Ctx.Params, TEXT("target_index"), TargetIndexNumber))
		{
			TargetIndex = FMath::RoundToInt(TargetIndexNumber);
		}

		FString SuggestedName;
		JsonTryGetString(Ctx.Params, TEXT("module_name"), SuggestedName);

		UNiagaraNodeFunctionCall* AddedModule = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
			ModuleScript,
			*Stage.OutputNode,
			TargetIndex,
			SuggestedName,
			ModuleScript->GetExposedVersion().VersionGuid);
		if (!AddedModule)
		{
			OutError = TEXT("add_stage_node_failed");
			return false;
		}

		AddedModule->NodePosX = FMath::RoundToInt(NodePosX);
		AddedModule->NodePosY = FMath::RoundToInt(NodePosY);
		AddedNode = AddedModule;
	}
	else
	{
		FString NodeClassPath;
		if (!JsonTryGetString(Ctx.Params, TEXT("node_class"), NodeClassPath) || NodeClassPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_node_class_or_module_script_asset_path");
			return false;
		}

		UClass* NodeClass = UeAgentNiagaraOps::ResolveClassByPath(NodeClassPath);
		if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()) || NodeClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = TEXT("invalid_node_class");
			return false;
		}

		AddedNode = NewObject<UEdGraphNode>(EditContext.Graph, NodeClass, NAME_None, RF_Transactional);
		if (!AddedNode)
		{
			OutError = TEXT("create_stage_node_failed");
			return false;
		}

		AddedNode->CreateNewGuid();
		AddedNode->NodePosX = FMath::RoundToInt(NodePosX);
		AddedNode->NodePosY = FMath::RoundToInt(NodePosY);
		AddedNode->AllocateDefaultPins();
		EditContext.Graph->AddNode(AddedNode, true, false);
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
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetObjectField(TEXT("node"), UeAgentNiagaraOps::BuildStageNodeJson(*AddedNode, INDEX_NONE, true, true));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterConnectStageNodes(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	FString FromNodeGuidText;
	FString ToNodeGuidText;
	FString FromPinName;
	FString ToPinName;
	if (!JsonTryGetString(Ctx.Params, TEXT("from_node_guid"), FromNodeGuidText) || FromNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_from_node_guid");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("to_node_guid"), ToNodeGuidText) || ToNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_to_node_guid");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("from_pin"), FromPinName) || FromPinName.IsEmpty())
	{
		OutError = TEXT("missing_from_pin");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("to_pin"), ToPinName) || ToPinName.IsEmpty())
	{
		OutError = TEXT("missing_to_pin");
		return false;
	}

	FGuid FromNodeGuid;
	FGuid ToNodeGuid;
	if (!FGuid::Parse(FromNodeGuidText, FromNodeGuid))
	{
		OutError = TEXT("invalid_from_node_guid");
		return false;
	}
	if (!FGuid::Parse(ToNodeGuidText, ToNodeGuid))
	{
		OutError = TEXT("invalid_to_node_guid");
		return false;
	}

	UEdGraphNode* FromNode = UeAgentNiagaraOps::FindGraphNodeByGuid(*EditContext.Graph, FromNodeGuid);
	UEdGraphNode* ToNode = UeAgentNiagaraOps::FindGraphNodeByGuid(*EditContext.Graph, ToNodeGuid);
	if (!FromNode || !ToNode)
	{
		OutError = TEXT("connect_nodes_not_found");
		return false;
	}

	UEdGraphPin* FromPin = UeAgentNiagaraOps::ResolvePinByName(*FromNode, FromPinName);
	UEdGraphPin* ToPin = UeAgentNiagaraOps::ResolvePinByName(*ToNode, ToPinName);
	if (!FromPin || !ToPin)
	{
		OutError = TEXT("connect_pin_not_found");
		return false;
	}

	bool bBreakInputLinks = false;
	JsonTryGetBool(Ctx.Params, TEXT("break_input_links"), bBreakInputLinks);

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "ConnectNiagaraStageNodes", "UeAgentInterface Connect Niagara Stage Nodes"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	FromNode->Modify();
	ToNode->Modify();

	if (bBreakInputLinks && ToPin->Direction == EGPD_Input && ToPin->LinkedTo.Num() > 0)
	{
		ToPin->BreakAllPinLinks(true);
	}

	bool bConnected = false;
	if (const UEdGraphSchema* Schema = EditContext.Graph->GetSchema())
	{
		bConnected = Schema->TryCreateConnection(FromPin, ToPin);
	}
	if (!bConnected)
	{
		FromPin->MakeLinkTo(ToPin);
		bConnected = FromPin->LinkedTo.Contains(ToPin) || ToPin->LinkedTo.Contains(FromPin);
	}
	if (!bConnected)
	{
		OutError = TEXT("connect_stage_nodes_failed");
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
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("from_node_guid"), FromNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("from_pin"), FromPin->PinName.ToString());
	OutData->SetStringField(TEXT("to_node_guid"), ToNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("to_pin"), ToPin->PinName.ToString());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdNiagaraEmitterDisconnectStageNodes(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentNiagaraOps::FNiagaraEmitterEditContext EditContext;
	if (!UeAgentNiagaraOps::GetEmitterEditContext(Ctx.Params, EditContext, OutError, true))
	{
		return false;
	}

	UeAgentNiagaraOps::FResolvedNiagaraStage Stage;
	if (!UeAgentNiagaraOps::ResolveStageFromParams(*EditContext.Graph, Ctx.Params, Stage, OutError))
	{
		return false;
	}

	FString FromNodeGuidText;
	FString FromPinName;
	if (!JsonTryGetString(Ctx.Params, TEXT("from_node_guid"), FromNodeGuidText) || FromNodeGuidText.IsEmpty())
	{
		OutError = TEXT("missing_from_node_guid");
		return false;
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("from_pin"), FromPinName) || FromPinName.IsEmpty())
	{
		OutError = TEXT("missing_from_pin");
		return false;
	}

	FGuid FromNodeGuid;
	if (!FGuid::Parse(FromNodeGuidText, FromNodeGuid))
	{
		OutError = TEXT("invalid_from_node_guid");
		return false;
	}

	UEdGraphNode* FromNode = UeAgentNiagaraOps::FindGraphNodeByGuid(*EditContext.Graph, FromNodeGuid);
	if (!FromNode)
	{
		OutError = TEXT("disconnect_from_node_not_found");
		return false;
	}

	UEdGraphPin* FromPin = UeAgentNiagaraOps::ResolvePinByName(*FromNode, FromPinName);
	if (!FromPin)
	{
		OutError = TEXT("disconnect_from_pin_not_found");
		return false;
	}

	int32 RemovedLinkCount = 0;
	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "DisconnectNiagaraStageNodes", "UeAgentInterface Disconnect Niagara Stage Nodes"));
	EditContext.Emitter->Modify();
	EditContext.Graph->Modify();
	FromNode->Modify();

	FString ToNodeGuidText;
	FString ToPinName;
	if (JsonTryGetString(Ctx.Params, TEXT("to_node_guid"), ToNodeGuidText) && !ToNodeGuidText.IsEmpty() &&
		JsonTryGetString(Ctx.Params, TEXT("to_pin"), ToPinName) && !ToPinName.IsEmpty())
	{
		FGuid ToNodeGuid;
		if (!FGuid::Parse(ToNodeGuidText, ToNodeGuid))
		{
			OutError = TEXT("invalid_to_node_guid");
			return false;
		}

		UEdGraphNode* ToNode = UeAgentNiagaraOps::FindGraphNodeByGuid(*EditContext.Graph, ToNodeGuid);
		if (!ToNode)
		{
			OutError = TEXT("disconnect_to_node_not_found");
			return false;
		}
		UEdGraphPin* ToPin = UeAgentNiagaraOps::ResolvePinByName(*ToNode, ToPinName);
		if (!ToPin)
		{
			OutError = TEXT("disconnect_to_pin_not_found");
			return false;
		}

		const int32 OldLinkCount = FromPin->LinkedTo.Num();
		FromPin->BreakLinkTo(ToPin);
		RemovedLinkCount = FMath::Max(0, OldLinkCount - FromPin->LinkedTo.Num());
	}
	else
	{
		RemovedLinkCount = FromPin->LinkedTo.Num();
		FromPin->BreakAllPinLinks(true);
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
	OutData->SetStringField(TEXT("stage_key"), Stage.StageKey);
	OutData->SetStringField(TEXT("from_node_guid"), FromNodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("from_pin"), FromPin->PinName.ToString());
	OutData->SetNumberField(TEXT("removed_link_count"), RemovedLinkCount);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}
