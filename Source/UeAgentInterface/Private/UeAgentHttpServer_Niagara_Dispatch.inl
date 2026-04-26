namespace
{
	using FNiagaraCommandHandler = bool (FUeAgentHttpServer::*)(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&) const;

	struct FNiagaraCommandEntry
	{
		const TCHAR* Name;
		FNiagaraCommandHandler Handler;
	};

	static const FNiagaraCommandEntry GNiagaraCommandEntries[] = {
		{ TEXT("niagara_list_assets"), &FUeAgentHttpServer::CmdNiagaraListAssets },
		{ TEXT("niagara_list_module_library"), &FUeAgentHttpServer::CmdNiagaraListModuleLibrary },
		{ TEXT("niagara_create_system"), &FUeAgentHttpServer::CmdNiagaraCreateSystem },
		{ TEXT("niagara_create_emitter"), &FUeAgentHttpServer::CmdNiagaraCreateEmitter },
		{ TEXT("niagara_delete_asset"), &FUeAgentHttpServer::CmdNiagaraDeleteAsset },
		{ TEXT("niagara_duplicate_asset"), &FUeAgentHttpServer::CmdNiagaraDuplicateAsset },
		{ TEXT("niagara_open_editor"), &FUeAgentHttpServer::CmdNiagaraOpenEditor },
		{ TEXT("niagara_screenshot"), &FUeAgentHttpServer::CmdNiagaraScreenshot },
		{ TEXT("niagara_system_runtime_probe"), &FUeAgentHttpServer::CmdNiagaraSystemRuntimeProbe },
		{ TEXT("niagara_get_stack_issues"), &FUeAgentHttpServer::CmdNiagaraGetStackIssues },
		{ TEXT("niagara_apply_stack_issue_fix"), &FUeAgentHttpServer::CmdNiagaraApplyStackIssueFix },
		{ TEXT("niagara_get_info"), &FUeAgentHttpServer::CmdNiagaraGetInfo },
		{ TEXT("niagara_export_folder"), &FUeAgentHttpServer::CmdNiagaraExportFolder },
		{ TEXT("niagara_apply_folder"), &FUeAgentHttpServer::CmdNiagaraApplyFolder },
		{ TEXT("niagara_emitter_export_folder"), &FUeAgentHttpServer::CmdNiagaraEmitterExportFolder },
		{ TEXT("niagara_emitter_apply_folder"), &FUeAgentHttpServer::CmdNiagaraEmitterApplyFolder },
		{ TEXT("niagara_script_export_folder"), &FUeAgentHttpServer::CmdNiagaraScriptExportFolder },
		{ TEXT("niagara_script_apply_folder"), &FUeAgentHttpServer::CmdNiagaraScriptApplyFolder },
		{ TEXT("niagara_system_get_property"), &FUeAgentHttpServer::CmdNiagaraSystemGetProperty },
		{ TEXT("niagara_set_property"), &FUeAgentHttpServer::CmdNiagaraSetProperty },
		{ TEXT("niagara_refresh_system"), &FUeAgentHttpServer::CmdNiagaraRefreshSystem },
		{ TEXT("niagara_compile_system"), &FUeAgentHttpServer::CmdNiagaraCompileSystem },
		{ TEXT("niagara_get_compile_log"), &FUeAgentHttpServer::CmdNiagaraGetCompileLog },
		{ TEXT("niagara_user_parameter_list"), &FUeAgentHttpServer::CmdNiagaraUserParameterList },
		{ TEXT("niagara_user_parameter_add"), &FUeAgentHttpServer::CmdNiagaraUserParameterAdd },
		{ TEXT("niagara_user_parameter_remove"), &FUeAgentHttpServer::CmdNiagaraUserParameterRemove },
		{ TEXT("niagara_user_parameter_get"), &FUeAgentHttpServer::CmdNiagaraUserParameterGet },
		{ TEXT("niagara_user_parameter_set"), &FUeAgentHttpServer::CmdNiagaraUserParameterSet },
		{ TEXT("niagara_system_list_emitters"), &FUeAgentHttpServer::CmdNiagaraSystemListEmitters },
		{ TEXT("niagara_system_add_emitter"), &FUeAgentHttpServer::CmdNiagaraSystemAddEmitter },
		{ TEXT("niagara_system_remove_emitter"), &FUeAgentHttpServer::CmdNiagaraSystemRemoveEmitter },
		{ TEXT("niagara_system_move_emitter"), &FUeAgentHttpServer::CmdNiagaraSystemMoveEmitter },
		{ TEXT("niagara_system_set_emitter_enabled"), &FUeAgentHttpServer::CmdNiagaraSystemSetEmitterEnabled },
		{ TEXT("niagara_system_set_emitter_version"), &FUeAgentHttpServer::CmdNiagaraSystemSetEmitterVersion },
		{ TEXT("niagara_system_list_stage_modules"), &FUeAgentHttpServer::CmdNiagaraSystemListStageModules },
		{ TEXT("niagara_system_list_module_inputs"), &FUeAgentHttpServer::CmdNiagaraSystemListModuleInputs },
		{ TEXT("niagara_system_set_module_input"), &FUeAgentHttpServer::CmdNiagaraSystemSetModuleInput },
		{ TEXT("niagara_emitter_clear_parent"), &FUeAgentHttpServer::CmdNiagaraEmitterClearParent },
		{ TEXT("niagara_emitter_get_property"), &FUeAgentHttpServer::CmdNiagaraEmitterGetProperty },
		{ TEXT("niagara_emitter_set_property"), &FUeAgentHttpServer::CmdNiagaraEmitterSetProperty },
		{ TEXT("niagara_emitter_list_renderers"), &FUeAgentHttpServer::CmdNiagaraEmitterListRenderers },
		{ TEXT("niagara_emitter_add_renderer"), &FUeAgentHttpServer::CmdNiagaraEmitterAddRenderer },
		{ TEXT("niagara_emitter_remove_renderer"), &FUeAgentHttpServer::CmdNiagaraEmitterRemoveRenderer },
		{ TEXT("niagara_emitter_move_renderer"), &FUeAgentHttpServer::CmdNiagaraEmitterMoveRenderer },
		{ TEXT("niagara_emitter_get_renderer_property"), &FUeAgentHttpServer::CmdNiagaraEmitterGetRendererProperty },
		{ TEXT("niagara_emitter_set_renderer_property"), &FUeAgentHttpServer::CmdNiagaraEmitterSetRendererProperty },
		{ TEXT("niagara_emitter_list_event_handlers"), &FUeAgentHttpServer::CmdNiagaraEmitterListEventHandlers },
		{ TEXT("niagara_emitter_add_event_handler"), &FUeAgentHttpServer::CmdNiagaraEmitterAddEventHandler },
		{ TEXT("niagara_emitter_remove_event_handler"), &FUeAgentHttpServer::CmdNiagaraEmitterRemoveEventHandler },
		{ TEXT("niagara_emitter_get_event_handler_property"), &FUeAgentHttpServer::CmdNiagaraEmitterGetEventHandlerProperty },
		{ TEXT("niagara_emitter_set_event_handler_property"), &FUeAgentHttpServer::CmdNiagaraEmitterSetEventHandlerProperty },
		{ TEXT("niagara_emitter_parameter_list"), &FUeAgentHttpServer::CmdNiagaraEmitterParameterList },
		{ TEXT("niagara_emitter_parameter_add"), &FUeAgentHttpServer::CmdNiagaraEmitterParameterAdd },
		{ TEXT("niagara_emitter_parameter_remove"), &FUeAgentHttpServer::CmdNiagaraEmitterParameterRemove },
		{ TEXT("niagara_emitter_parameter_get"), &FUeAgentHttpServer::CmdNiagaraEmitterParameterGet },
		{ TEXT("niagara_emitter_parameter_set"), &FUeAgentHttpServer::CmdNiagaraEmitterParameterSet },
		{ TEXT("niagara_emitter_list_stages"), &FUeAgentHttpServer::CmdNiagaraEmitterListStages },
		{ TEXT("niagara_emitter_add_simulation_stage"), &FUeAgentHttpServer::CmdNiagaraEmitterAddSimulationStage },
		{ TEXT("niagara_emitter_remove_stage"), &FUeAgentHttpServer::CmdNiagaraEmitterRemoveStage },
		{ TEXT("niagara_emitter_set_stage_property"), &FUeAgentHttpServer::CmdNiagaraEmitterSetStageProperty },
		{ TEXT("niagara_emitter_list_stage_modules"), &FUeAgentHttpServer::CmdNiagaraEmitterListStageModules },
		{ TEXT("niagara_emitter_add_stage_module"), &FUeAgentHttpServer::CmdNiagaraEmitterAddStageModule },
		{ TEXT("niagara_emitter_remove_stage_module"), &FUeAgentHttpServer::CmdNiagaraEmitterRemoveStageModule },
		{ TEXT("niagara_emitter_move_stage_module"), &FUeAgentHttpServer::CmdNiagaraEmitterMoveStageModule },
		{ TEXT("niagara_emitter_list_stage_nodes"), &FUeAgentHttpServer::CmdNiagaraEmitterListStageNodes },
		{ TEXT("niagara_emitter_get_stage_node_property"), &FUeAgentHttpServer::CmdNiagaraEmitterGetStageNodeProperty },
		{ TEXT("niagara_emitter_set_stage_node_property"), &FUeAgentHttpServer::CmdNiagaraEmitterSetStageNodeProperty },
		{ TEXT("niagara_emitter_remove_stage_node"), &FUeAgentHttpServer::CmdNiagaraEmitterRemoveStageNode },
		{ TEXT("niagara_emitter_set_stage_module_enabled"), &FUeAgentHttpServer::CmdNiagaraEmitterSetStageModuleEnabled },
		{ TEXT("niagara_emitter_list_module_inputs"), &FUeAgentHttpServer::CmdNiagaraEmitterListModuleInputs },
		{ TEXT("niagara_emitter_set_module_input"), &FUeAgentHttpServer::CmdNiagaraEmitterSetModuleInput },
		{ TEXT("niagara_emitter_clear_module_input"), &FUeAgentHttpServer::CmdNiagaraEmitterClearModuleInput },
		{ TEXT("niagara_emitter_add_stage_node"), &FUeAgentHttpServer::CmdNiagaraEmitterAddStageNode },
		{ TEXT("niagara_emitter_connect_stage_nodes"), &FUeAgentHttpServer::CmdNiagaraEmitterConnectStageNodes },
		{ TEXT("niagara_emitter_disconnect_stage_nodes"), &FUeAgentHttpServer::CmdNiagaraEmitterDisconnectStageNodes },
	};

	static FNiagaraCommandHandler FindNiagaraCommandHandler(const FString& CommandLower)
	{
		for (const FNiagaraCommandEntry& Entry : GNiagaraCommandEntries)
		{
			if (CommandLower == Entry.Name)
			{
				return Entry.Handler;
			}
		}
		return nullptr;
	}
}

bool FUeAgentHttpServer::ExecuteNiagaraCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (const FNiagaraCommandHandler Handler = FindNiagaraCommandHandler(CommandLower))
	{
		return (this->*Handler)(Ctx, OutData, OutError);
	}

	OutError = TEXT("unknown_niagara_command");
	return false;
}
