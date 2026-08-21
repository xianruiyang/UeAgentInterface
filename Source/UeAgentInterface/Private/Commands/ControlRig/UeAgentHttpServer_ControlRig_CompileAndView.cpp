#include "UeAgentHttpServer_ControlRig.h"

#include "Domains/Rigging/UeAgentControlRigDomainBackend.h"

#include "Commands/EditorViewport/UeAgentEditorViewportLegacy.h"
#include "ControlRigBlueprint.h"
#include "Dom/JsonObject.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"

bool FUeAgentLegacyMechanics::CmdControlRigCompile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("asset_path_required");
		return false;
	}

	UControlRigBlueprint* Blueprint = UeAgentControlRigOps::LoadControlRigBlueprint(AssetPath);
	if (!Blueprint)
	{
		OutError = TEXT("control_rig_not_found");
		return false;
	}

	FCompilerResultsLog CompilerLog;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), UeAgentControlRigOps::NormalizeAssetPath(AssetPath));
	OutData->SetNumberField(TEXT("blueprint_status"), static_cast<int32>(Blueprint->Status));
	OutData->SetBoolField(TEXT("compiled"), Blueprint->Status != BS_Error);
	OutData->SetNumberField(TEXT("error_count"), CompilerLog.NumErrors);
	OutData->SetNumberField(TEXT("warning_count"), CompilerLog.NumWarnings);
	OutData->SetObjectField(TEXT("compile_report"), UeAgentControlRigOps::MakeCompileReportJson(Blueprint, &CompilerLog));
	OutData->SetArrayField(TEXT("issues"), {});
	return Blueprint->Status != BS_Error;
}

bool FUeAgentLegacyMechanics::CmdControlRigGetCompileLog(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdControlRigCompile(Ctx, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("note"), TEXT("Control Rig compile status and compiler messages are returned through compile_report."));
	return true;
}

bool FUeAgentLegacyMechanics::CmdControlRigOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("asset_path_required");
		return false;
	}
	if (!UeAgentControlRigOps::LoadControlRigBlueprint(AssetPath))
	{
		OutError = TEXT("control_rig_not_found");
		return false;
	}
	if (!CmdOpenAssetEditor(Ctx, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("asset_kind"), TEXT("control_rig"));
	return true;
}

static FUeAgentRequestContext MakeControlRigBlueprintProxyContext(const FUeAgentRequestContext& Ctx)
{
	FUeAgentRequestContext Proxy = Ctx;
	Proxy.Params = MakeShared<FJsonObject>();
	if (Ctx.Params.IsValid())
	{
		Proxy.Params->Values = Ctx.Params->Values;
	}

	FString GraphNameOrPath;
	if (Proxy.Params.IsValid()
		&& !Proxy.Params->HasField(TEXT("graph_name"))
		&& Proxy.Params->TryGetStringField(TEXT("graph_name_or_path"), GraphNameOrPath)
		&& !GraphNameOrPath.TrimStartAndEnd().IsEmpty())
	{
		Proxy.Params->SetStringField(TEXT("graph_name"), GraphNameOrPath);
	}
	return Proxy;
}

bool FUeAgentLegacyMechanics::CmdControlRigGraphGetView(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FUeAgentRequestContext Proxy = MakeControlRigBlueprintProxyContext(Ctx);
	if (!CmdBlueprintGraphGetView(Proxy, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("asset_kind"), TEXT("control_rig"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdControlRigGraphSetView(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FUeAgentRequestContext Proxy = MakeControlRigBlueprintProxyContext(Ctx);
	if (!CmdBlueprintGraphSetView(Proxy, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("asset_kind"), TEXT("control_rig"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdControlRigViewportGetCamera(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UControlRigBlueprint* Blueprint = UeAgentControlRigOps::LoadControlRigBlueprint(AssetPath);
	if (!Blueprint)
	{
		OutError = TEXT("control_rig_not_found");
		return false;
	}
	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);
	if (!UeAgentEditorViewportLegacy::ExecuteCameraGet(
		*this,
		Ctx,
		UeAgentEditorViewportLegacy::MakeAssetTarget(Blueprint->GetPathName(), TEXT("control_rig"), TEXT("primary"), bOpenEditorIfNeeded),
		OutData,
		OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("asset_kind"), TEXT("control_rig"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdControlRigViewportSetCamera(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UControlRigBlueprint* Blueprint = UeAgentControlRigOps::LoadControlRigBlueprint(AssetPath);
	if (!Blueprint)
	{
		OutError = TEXT("control_rig_not_found");
		return false;
	}
	bool bOpenEditorIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);
	if (!UeAgentEditorViewportLegacy::ExecuteCameraSet(
		*this,
		Ctx,
		UeAgentEditorViewportLegacy::MakeAssetTarget(Blueprint->GetPathName(), TEXT("control_rig"), TEXT("primary"), bOpenEditorIfNeeded),
		OutData,
		OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("asset_kind"), TEXT("control_rig"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdControlRigScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FUeAgentRequestContext Proxy = MakeControlRigBlueprintProxyContext(Ctx);
	if (!CmdBlueprintScreenshot(Proxy, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("asset_kind"), TEXT("control_rig"));
	return true;
}
