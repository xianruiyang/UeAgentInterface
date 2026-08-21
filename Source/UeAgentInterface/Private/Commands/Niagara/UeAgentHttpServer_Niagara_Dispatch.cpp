#include "UeAgentHttpServer_Niagara.h"

#include "Dom/JsonObject.h"

namespace
{
	using FNiagaraCommandHandler = bool (FUeAgentLegacyMechanics::*)(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&) const;

	struct FNiagaraCommandEntry
	{
		const TCHAR* Name;
		FNiagaraCommandHandler Handler;
	};

	static const FNiagaraCommandEntry GNiagaraCommandEntries[] = {
		{ TEXT("niagara_list_assets"), &FUeAgentLegacyMechanics::CmdNiagaraListAssets },
		{ TEXT("niagara_list_module_library"), &FUeAgentLegacyMechanics::CmdNiagaraListModuleLibrary },
		{ TEXT("niagara_create_system"), &FUeAgentLegacyMechanics::CmdNiagaraCreateSystem },
		{ TEXT("niagara_create_emitter"), &FUeAgentLegacyMechanics::CmdNiagaraCreateEmitter },
		{ TEXT("niagara_duplicate_asset"), &FUeAgentLegacyMechanics::CmdNiagaraDuplicateAsset },
		{ TEXT("niagara_open_editor"), &FUeAgentLegacyMechanics::CmdNiagaraOpenEditor },
		{ TEXT("niagara_preview_advance"), &FUeAgentLegacyMechanics::CmdNiagaraPreviewAdvance },
		{ TEXT("niagara_system_runtime_probe"), &FUeAgentLegacyMechanics::CmdNiagaraSystemRuntimeProbe },
		{ TEXT("niagara_get_stack_issues"), &FUeAgentLegacyMechanics::CmdNiagaraGetStackIssues },
		{ TEXT("niagara_apply_stack_issue_fix"), &FUeAgentLegacyMechanics::CmdNiagaraApplyStackIssueFix },
		{ TEXT("niagara_get_info"), &FUeAgentLegacyMechanics::CmdNiagaraGetInfo },
		{ TEXT("niagara_export_folder"), &FUeAgentLegacyMechanics::CmdNiagaraExportFolder },
		{ TEXT("niagara_apply_folder"), &FUeAgentLegacyMechanics::CmdNiagaraApplyFolder },
		{ TEXT("niagara_emitter_export_folder"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterExportFolder },
		{ TEXT("niagara_emitter_apply_folder"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterApplyFolder },
		{ TEXT("niagara_script_export_folder"), &FUeAgentLegacyMechanics::CmdNiagaraScriptExportFolder },
		{ TEXT("niagara_script_apply_folder"), &FUeAgentLegacyMechanics::CmdNiagaraScriptApplyFolder },
		{ TEXT("niagara_system_get_property"), &FUeAgentLegacyMechanics::CmdNiagaraSystemGetProperty },
		{ TEXT("niagara_refresh_system"), &FUeAgentLegacyMechanics::CmdNiagaraRefreshSystem },
		{ TEXT("niagara_compile_system"), &FUeAgentLegacyMechanics::CmdNiagaraCompileSystem },
		{ TEXT("niagara_get_compile_log"), &FUeAgentLegacyMechanics::CmdNiagaraGetCompileLog },
		{ TEXT("niagara_user_parameter_list"), &FUeAgentLegacyMechanics::CmdNiagaraUserParameterList },
		{ TEXT("niagara_user_parameter_get"), &FUeAgentLegacyMechanics::CmdNiagaraUserParameterGet },
		{ TEXT("niagara_system_list_emitters"), &FUeAgentLegacyMechanics::CmdNiagaraSystemListEmitters },
		{ TEXT("niagara_system_list_stage_modules"), &FUeAgentLegacyMechanics::CmdNiagaraSystemListStageModules },
		{ TEXT("niagara_system_list_module_inputs"), &FUeAgentLegacyMechanics::CmdNiagaraSystemListModuleInputs },
		{ TEXT("niagara_emitter_get_property"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterGetProperty },
		{ TEXT("niagara_emitter_list_renderers"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterListRenderers },
		{ TEXT("niagara_emitter_move_renderer"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterMoveRenderer },
		{ TEXT("niagara_emitter_get_renderer_property"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterGetRendererProperty },
		{ TEXT("niagara_emitter_list_event_handlers"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterListEventHandlers },
		{ TEXT("niagara_emitter_get_event_handler_property"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterGetEventHandlerProperty },
		{ TEXT("niagara_emitter_parameter_list"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterParameterList },
		{ TEXT("niagara_emitter_parameter_get"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterParameterGet },
		{ TEXT("niagara_emitter_list_stages"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterListStages },
		{ TEXT("niagara_emitter_list_stage_modules"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterListStageModules },
		{ TEXT("niagara_emitter_move_stage_module"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterMoveStageModule },
		{ TEXT("niagara_emitter_list_stage_nodes"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterListStageNodes },
		{ TEXT("niagara_emitter_get_stage_node_property"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterGetStageNodeProperty },
		{ TEXT("niagara_emitter_list_module_inputs"), &FUeAgentLegacyMechanics::CmdNiagaraEmitterListModuleInputs },
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

bool FUeAgentLegacyMechanics::ExecuteNiagaraCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (const FNiagaraCommandHandler Handler = FindNiagaraCommandHandler(CommandLower))
	{
		return (this->*Handler)(Ctx, OutData, OutError);
	}

	OutError = TEXT("unknown_niagara_command");
	return false;
}
