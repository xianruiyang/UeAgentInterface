#include "UeAgentHttpServer_ControlRig.h"

#include "UeAgentHttpServer_ControlRigOps.h"

#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentFolderDetailsExport.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "ControlRigBlueprint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Paths.h"

bool FUeAgentHttpServer::CmdControlRigExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	FString FolderPath;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath);
	if (FolderPath.TrimStartAndEnd().IsEmpty())
	{
		FolderPath = UeAgentControlRigOps::DefaultFolderForAsset(AssetPath);
	}
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
	IFileManager::Get().MakeDirectory(*FolderPath, true);

	const TSharedPtr<FJsonObject> Info = UeAgentControlRigOps::BuildControlRigInfo(Blueprint);
	TSharedPtr<FJsonObject> AssetJson = MakeShared<FJsonObject>();
	AssetJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.asset.v1"));
	AssetJson->SetStringField(TEXT("asset_path"), Info->GetStringField(TEXT("asset_path")));
	AssetJson->SetStringField(TEXT("object_path"), Info->GetStringField(TEXT("object_path")));
	AssetJson->SetStringField(TEXT("asset_class"), Info->GetStringField(TEXT("asset_class")));
	AssetJson->SetStringField(TEXT("control_rig_type"), Info->GetStringField(TEXT("control_rig_type")));
	AssetJson->SetStringField(TEXT("control_rig_class"), Info->GetStringField(TEXT("control_rig_class")));
	AssetJson->SetStringField(TEXT("generated_class"), Info->GetStringField(TEXT("generated_class")));
	AssetJson->SetStringField(TEXT("preview_skeletal_mesh"), Info->GetStringField(TEXT("preview_skeletal_mesh")));
	AssetJson->SetNumberField(TEXT("blueprint_status"), Info->GetNumberField(TEXT("blueprint_status")));
	AssetJson->SetArrayField(TEXT("supported_events"), Info->GetArrayField(TEXT("supported_events")));
	AssetJson->SetArrayField(TEXT("required_animation_editing_events"), Info->GetArrayField(TEXT("required_animation_editing_events")));
	AssetJson->SetBoolField(TEXT("supports_forward_solve"), Info->GetBoolField(TEXT("supports_forward_solve")));
	AssetJson->SetBoolField(TEXT("supports_backward_solve"), Info->GetBoolField(TEXT("supports_backward_solve")));
	AssetJson->SetBoolField(TEXT("supports_inverse_event"), Info->GetBoolField(TEXT("supports_inverse_event")));
	AssetJson->SetBoolField(TEXT("animation_editing_ready"), Info->GetBoolField(TEXT("animation_editing_ready")));
	AssetJson->SetBoolField(TEXT("bake_to_control_rig_ready"), Info->GetBoolField(TEXT("bake_to_control_rig_ready")));
	if (!UeAgentFolderDetailsExport::AttachOwnerDetails(
		Blueprint,
		FolderPath,
		TEXT("control_rig_export_folder"),
		TEXT("control_rig_asset"),
		Blueprint->GetPathName(),
		TEXT("asset"),
		AssetJson,
		OutError))
	{
		return false;
	}
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, OutError);

	TSharedPtr<FJsonObject> ControlRigSettingsJson = MakeShared<FJsonObject>();
	ControlRigSettingsJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.settings.v1"));
	ControlRigSettingsJson->SetStringField(TEXT("control_rig_type"), Blueprint->ControlRigType == EControlRigType::RigModule ? TEXT("rig_module") : (Blueprint->ControlRigType == EControlRigType::ModularRig ? TEXT("modular_rig") : TEXT("control_rig")));
	ControlRigSettingsJson->SetArrayField(TEXT("event_queue"), UeAgentControlRigOps::MakeStringArray({ TEXT("Construction"), TEXT("Forwards Solve"), TEXT("Backwards Solve") }));
	ControlRigSettingsJson->SetArrayField(TEXT("animation_editing_required_events"), UeAgentControlRigOps::MakeStringArray({ TEXT("Forwards Solve"), TEXT("Backwards Solve") }));
	ControlRigSettingsJson->SetBoolField(TEXT("animation_editing_ready"), Info->GetBoolField(TEXT("animation_editing_ready")));
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("control_rig.json")), ControlRigSettingsJson, OutError);

	TSharedPtr<FJsonObject> PreviewJson = MakeShared<FJsonObject>();
	PreviewJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.preview.v1"));
	PreviewJson->SetStringField(TEXT("preview_skeletal_mesh"), Blueprint->GetPreviewMesh() ? Blueprint->GetPreviewMesh()->GetPathName() : FString());
	PreviewJson->SetBoolField(TEXT("import_hierarchy_from_preview"), false);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("preview.json")), PreviewJson, OutError);

	TSharedPtr<FJsonObject> ShapeRefsJson = MakeShared<FJsonObject>();
	ShapeRefsJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.shape_library_references.v1"));
	ShapeRefsJson->SetArrayField(TEXT("shape_libraries"), Info->GetArrayField(TEXT("shape_libraries")));
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("shape_libraries"), TEXT("references.json")), ShapeRefsJson, OutError);

	const TSharedPtr<FJsonObject>* HierarchyObj = nullptr;
	Info->TryGetObjectField(TEXT("hierarchy"), HierarchyObj);
	if (HierarchyObj && HierarchyObj->IsValid())
	{
		TSharedPtr<FJsonObject> BonesJson = MakeShared<FJsonObject>();
		BonesJson->SetArrayField(TEXT("bones"), (*HierarchyObj)->GetArrayField(TEXT("bones")));
		UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("bones.json")), BonesJson, OutError);

		TSharedPtr<FJsonObject> NullsJson = MakeShared<FJsonObject>();
		NullsJson->SetArrayField(TEXT("nulls"), (*HierarchyObj)->GetArrayField(TEXT("nulls")));
		UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("nulls.json")), NullsJson, OutError);

		TSharedPtr<FJsonObject> ControlsJson = MakeShared<FJsonObject>();
		ControlsJson->SetArrayField(TEXT("controls"), (*HierarchyObj)->GetArrayField(TEXT("controls")));
		UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("controls.json")), ControlsJson, OutError);

		TSharedPtr<FJsonObject> CurvesJson = MakeShared<FJsonObject>();
		CurvesJson->SetArrayField(TEXT("curves"), (*HierarchyObj)->GetArrayField(TEXT("curves")));
		UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("curves.json")), CurvesJson, OutError);
	}

	TSharedPtr<FJsonObject> GraphsJson = MakeShared<FJsonObject>();
	GraphsJson->SetArrayField(TEXT("graphs"), Info->GetArrayField(TEXT("graphs")));
	GraphsJson->SetBoolField(TEXT("replace_nodes"), false);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("graphs.json")), GraphsJson, OutError);

	TSharedPtr<FJsonObject> IkNodesJson = MakeShared<FJsonObject>();
	IkNodesJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.ik_nodes.v1"));
	IkNodesJson->SetArrayField(TEXT("nodes"), {});
	IkNodesJson->SetNumberField(TEXT("node_count"), 0);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("ik_nodes.json")), IkNodesJson, OutError);

	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("connectors.json")), UeAgentControlRigOps::MakeEmptyItemsJson(TEXT("ue_agent_interface.control_rig.hierarchy.connectors.v1")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("sockets.json")), UeAgentControlRigOps::MakeEmptyItemsJson(TEXT("ue_agent_interface.control_rig.hierarchy.sockets.v1")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("rigid_bodies.json")), UeAgentControlRigOps::MakeEmptyItemsJson(TEXT("ue_agent_interface.control_rig.hierarchy.rigid_bodies.v1")), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("metadata.json")), UeAgentControlRigOps::MakeEmptyItemsJson(TEXT("ue_agent_interface.control_rig.hierarchy.metadata.v1")), OutError);
	const TSharedPtr<FJsonObject>* VariablesInfoObj = nullptr;
	if (Info->TryGetObjectField(TEXT("variables"), VariablesInfoObj) && VariablesInfoObj && VariablesInfoObj->IsValid())
	{
		UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("variables"), TEXT("variables.json")), *VariablesInfoObj, OutError);
	}
	else
	{
		UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("variables"), TEXT("variables.json")), UeAgentControlRigOps::BuildVariablesJson(Blueprint), OutError);
	}

	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), UeAgentControlRigOps::MakeCoverageJson(), OutError);
	TSharedPtr<FJsonObject> DiagnosticsJson = MakeShared<FJsonObject>();
	DiagnosticsJson->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.diagnostics.v1"));
	DiagnosticsJson->SetArrayField(TEXT("issues"), {});
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("diagnostics.json")), DiagnosticsJson, OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("unsupported_profiles.json")), UeAgentControlRigOps::MakeUnsupportedProfilesJson(), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("compile_report.json")), UeAgentControlRigOps::MakeCompileReportJson(Blueprint), OutError);
	TSharedPtr<FJsonObject> ReadbackDiffJson = MakeShared<FJsonObject>();
	ReadbackDiffJson->SetBoolField(TEXT("has_diff"), false);
	ReadbackDiffJson->SetArrayField(TEXT("diffs"), {});
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("readback_diff.json")), ReadbackDiffJson, OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("runtime_probe_report.json")), UeAgentControlRigOps::MakeRuntimeProbeReportJson(), OutError);
	UeAgentControlRigOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("rigvm_unit_registry.json")), UeAgentControlRigOps::MakeRigVMUnitRegistryJson(), OutError);

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), UeAgentControlRigOps::NormalizeAssetPath(AssetPath));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.control_rig.folder.v1"));
	OutData->SetObjectField(TEXT("coverage"), UeAgentControlRigOps::MakeCoverageJson());
	return true;
}

bool FUeAgentHttpServer::CmdControlRigValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FUeAgentRequestContext ValidateCtx = Ctx;
	ValidateCtx.Params->SetBoolField(TEXT("validate_only"), true);
	ValidateCtx.Params->SetBoolField(TEXT("dry_run"), true);
	return CmdControlRigApplyFolder(ValidateCtx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdControlRigApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("folder_path_required");
		return false;
	}
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);

	bool bValidateOnly = false;
	bool bDryRun = false;
	bool bCreateIfMissing = false;
	bool bSaveAfterApply = false;
	bool bCompileAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);
	bool bRequireAnimationEditingReady = false;
	JsonTryGetBool(Ctx.Params, TEXT("require_animation_editing_ready"), bRequireAnimationEditingReady);

	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> AssetJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, Issues, OutError, false);
	if (AssetJson.IsValid())
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(AssetJson, TEXT("asset.json"), { TEXT("schema"), TEXT("asset_path"), TEXT("object_path"), TEXT("asset_class"), TEXT("control_rig_type"), TEXT("control_rig_class"), TEXT("generated_class"), TEXT("preview_skeletal_mesh"), TEXT("shape_libraries"), TEXT("hierarchy"), TEXT("graphs"), TEXT("graph_count"), TEXT("blueprint_status"), TEXT("supported_events"), TEXT("required_animation_editing_events"), TEXT("supports_forward_solve"), TEXT("supports_backward_solve"), TEXT("supports_inverse_event"), TEXT("animation_editing_ready"), TEXT("bake_to_control_rig_ready"), TEXT("details") }, Issues);
	}

	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) && AssetJson.IsValid())
	{
		UeAgentJsonDiagnostics::ReadStringField(AssetJson, TEXT("asset_path"), TEXT("asset.json.asset_path"), AssetPath, Issues, true);
	}
	if (AssetPath.TrimStartAndEnd().IsEmpty())
	{
		UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("asset_path_required"), TEXT("asset.json.asset_path"), TEXT("Control Rig asset_path is required."));
	}

	TSharedPtr<FJsonObject> LegacyRejection;
	if (UeAgentDetailsAuthoring::BuildLegacyPropertiesFolderRejectionIfPresent(
		FolderPath,
		UeAgentDetailsAuthoring::MakeOwnerInfoForObject(nullptr, TEXT("control_rig_asset"), AssetPath, TEXT("control_rig_apply_folder")),
		TEXT("asset.json.details.index_file"),
		LegacyRejection,
		OutError))
	{
		OutData = LegacyRejection;
		return false;
	}

	UControlRigBlueprint* Blueprint = AssetPath.IsEmpty() ? nullptr : UeAgentControlRigOps::LoadControlRigBlueprint(AssetPath);
	if (!Blueprint && bCreateIfMissing && !bValidateOnly && !bDryRun && !AssetPath.IsEmpty())
	{
		FString RigTypeText;
		if (AssetJson.IsValid())
		{
			AssetJson->TryGetStringField(TEXT("control_rig_type"), RigTypeText);
		}
		EControlRigType RigType = EControlRigType::IndependentRig;
		UeAgentControlRigOps::ParseControlRigTypeText(RigTypeText, RigType);
		Blueprint = UeAgentControlRigOps::CreateControlRigAsset(AssetPath, RigType == EControlRigType::ModularRig, FString(), false, OutError);
	}
	if (!Blueprint && !bValidateOnly && !bDryRun)
	{
		UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_not_found"), TEXT("asset.json.asset_path"), TEXT("Control Rig asset was not found and create_if_missing is false."));
	}

	TSharedPtr<FJsonObject> PreviewJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("preview.json")), PreviewJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ControlRigSettingsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("control_rig.json")), ControlRigSettingsJson, Issues, OutError, true);
	if (ControlRigSettingsJson.IsValid())
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(ControlRigSettingsJson, TEXT("settings/control_rig.json"), { TEXT("schema"), TEXT("profile"), TEXT("apply_policy"), TEXT("control_rig_type"), TEXT("event_queue"), TEXT("animation_editing_required_events"), TEXT("animation_editing_ready") }, Issues);
	}
	TSharedPtr<FJsonObject> CompileSettingsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("vm_compile_settings.json")), CompileSettingsJson, Issues, OutError, true);
	if (CompileSettingsJson.IsValid())
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(CompileSettingsJson, TEXT("settings/vm_compile_settings.json"), { TEXT("schema"), TEXT("apply_policy") }, Issues);
	}
	TSharedPtr<FJsonObject> ShapeRefsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("shape_libraries"), TEXT("references.json")), ShapeRefsJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> BonesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("bones.json")), BonesJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> NullsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("nulls.json")), NullsJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ControlsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("controls.json")), ControlsJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> CurvesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("hierarchy"), TEXT("curves.json")), CurvesJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> GraphsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("graphs.json")), GraphsJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> IkNodesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("ik_nodes.json")), IkNodesJson, Issues, OutError, true);
	UeAgentControlRigOps::MergeStructuredIkNodesJson(GraphsJson, IkNodesJson, TEXT("graphs/ik_nodes.json"), Issues);
	TSharedPtr<FJsonObject> ConstructionEventJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("construction_event.json")), ConstructionEventJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ForwardSolveJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("forward_solve.json")), ForwardSolveJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> BackwardSolveJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("backward_solve.json")), BackwardSolveJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> VariablesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("variables"), TEXT("variables.json")), VariablesJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ExposedPropertiesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("variables"), TEXT("exposed_properties.json")), ExposedPropertiesJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> FunctionsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("functions"), TEXT("functions.json")), FunctionsJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ModulesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("modular"), TEXT("modules.json")), ModulesJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ModuleConnectionsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("modular"), TEXT("connections.json")), ModuleConnectionsJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ModuleConnectorsJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("modular"), TEXT("connectors.json")), ModuleConnectorsJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> FootGroundFbikPresetJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("presets"), TEXT("foot_ground_fbik.json")), FootGroundFbikPresetJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> RawPropertiesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("raw_properties.json")), RawPropertiesJson, Issues, OutError, true);
	TSharedPtr<FJsonObject> ReadonlyPropertiesJson;
	UeAgentControlRigOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("readonly_properties.json")), ReadonlyPropertiesJson, Issues, OutError, true);

	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(ConstructionEventJson, TEXT("graphs/construction_event.json"), TEXT("Control Rig construction event view"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(ForwardSolveJson, TEXT("graphs/forward_solve.json"), TEXT("Control Rig forward solve view"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(BackwardSolveJson, TEXT("graphs/backward_solve.json"), TEXT("Control Rig backward solve view"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(ExposedPropertiesJson, TEXT("variables/exposed_properties.json"), TEXT("Control Rig exposed properties"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(FunctionsJson, TEXT("functions/functions.json"), TEXT("Control Rig functions"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(ModulesJson, TEXT("modular/modules.json"), TEXT("Modular Rig modules"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(ModuleConnectionsJson, TEXT("modular/connections.json"), TEXT("Modular Rig connections"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(ModuleConnectorsJson, TEXT("modular/connectors.json"), TEXT("Modular Rig connectors"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(FootGroundFbikPresetJson, TEXT("presets/foot_ground_fbik.json"), TEXT("Control Rig foot ground FBIK preset"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(RawPropertiesJson, TEXT("raw_properties.json"), TEXT("Control Rig raw properties"), Issues);
	UeAgentControlRigOps::AddUnsupportedApplyIssueIfNeeded(ReadonlyPropertiesJson, TEXT("readonly_properties.json"), TEXT("Control Rig readonly properties"), Issues);

	TSharedPtr<FJsonObject> ApplyCompileReport;
	UeAgentControlRigOps::FControlRigVariableApplyStats VariableStats;
	int32 GroupedDetailsApplied = 0;
	TSharedPtr<FJsonObject> DetailsApplySummary;
	bool bSaved = false;
	if (!bValidateOnly && !bDryRun && Blueprint && !UeAgentControlRigOps::HasBlockingIssue(Issues))
	{
		if (!UeAgentDetailsAuthoring::ApplyOwnerDetailsFromEntryIfPresent(
			Blueprint,
			FolderPath,
			AssetJson,
			UeAgentDetailsAuthoring::MakeOwnerInfoForObject(Blueprint, TEXT("control_rig_asset"), Blueprint->GetPathName(), TEXT("control_rig_apply_folder")),
			TEXT("details"),
			true,
			DetailsApplySummary,
			GroupedDetailsApplied,
			OutError))
		{
			return false;
		}
		UeAgentControlRigOps::ApplyPreviewJson(Blueprint, PreviewJson, TEXT("settings/preview.json"), Issues);
		UeAgentControlRigOps::ApplyShapeLibrariesJson(Blueprint, ShapeRefsJson, TEXT("shape_libraries/references.json"), Issues);
		UeAgentControlRigOps::ApplyVariablesJson(Blueprint, VariablesJson, TEXT("variables/variables.json"), Issues, VariableStats);
		UeAgentControlRigOps::ApplyBonesJson(Blueprint, BonesJson, TEXT("hierarchy/bones.json"), Issues);
		UeAgentControlRigOps::ApplyNullsJson(Blueprint, NullsJson, TEXT("hierarchy/nulls.json"), Issues);
		UeAgentControlRigOps::ApplyControlsJson(Blueprint, ControlsJson, TEXT("hierarchy/controls.json"), Issues);
		UeAgentControlRigOps::ApplyCurvesJson(Blueprint, CurvesJson, TEXT("hierarchy/curves.json"), Issues);
		UeAgentControlRigOps::ApplyGraphsJson(Blueprint, GraphsJson, TEXT("graphs/graphs.json"), Issues);
		UeAgentControlRigOps::MarkControlRigModified(Blueprint);

		if (bCompileAfterApply && !UeAgentControlRigOps::HasBlockingIssue(Issues))
		{
			FCompilerResultsLog CompilerLog;
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
			ApplyCompileReport = UeAgentControlRigOps::MakeCompileReportJson(Blueprint, &CompilerLog);
			if (CompilerLog.NumErrors > 0 || Blueprint->Status == BS_Error)
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("control_rig_compile_failed"), TEXT("compile"), TEXT("Control Rig compile failed after folder apply. See compile_report in the response."));
			}
		}

		if (bSaveAfterApply && !UeAgentControlRigOps::HasBlockingIssue(Issues) && !UeAgentControlRigOps::SaveAssetPackage(Blueprint, OutError))
		{
			return false;
		}
		bSaved = bSaveAfterApply && !UeAgentControlRigOps::HasBlockingIssue(Issues);
	}

	if (Blueprint && bRequireAnimationEditingReady)
	{
		TSharedPtr<FJsonObject> ReadbackInfo = UeAgentControlRigOps::BuildControlRigInfo(Blueprint);
		if (!ReadbackInfo->GetBoolField(TEXT("animation_editing_ready")))
		{
			UeAgentJsonDiagnostics::AddIssue(
				Issues,
				TEXT("error"),
				TEXT("control_rig_missing_bidirectional_animation_editing_events"),
				TEXT("control_rig.events"),
				TEXT("Animation editing requires both Forwards Solve and Backwards Solve. Fix the Control Rig before using it as the authoring rig for skeletal animation."));
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("asset_path"), UeAgentControlRigOps::NormalizeAssetPath(AssetPath));
	OutData->SetBoolField(TEXT("validate_only"), bValidateOnly || bDryRun);
	OutData->SetBoolField(TEXT("valid"), !UeAgentControlRigOps::HasBlockingIssue(Issues));
	OutData->SetBoolField(TEXT("compiled"), Blueprint ? Blueprint->Status != BS_Error : false);
	OutData->SetBoolField(TEXT("compile_after_apply"), bCompileAfterApply);
	OutData->SetBoolField(TEXT("save_after_apply"), bSaveAfterApply);
	OutData->SetBoolField(TEXT("saved"), bSaved);
	OutData->SetNumberField(TEXT("grouped_details_applied"), GroupedDetailsApplied);
	if (DetailsApplySummary.IsValid())
	{
		OutData->SetObjectField(TEXT("details_apply"), DetailsApplySummary);
	}
	OutData->SetBoolField(TEXT("require_animation_editing_ready"), bRequireAnimationEditingReady);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	OutData->SetNumberField(TEXT("error_count"), UeAgentControlRigOps::CountIssuesBySeverity(Issues, TEXT("error")));
	OutData->SetNumberField(TEXT("warning_count"), UeAgentControlRigOps::CountIssuesBySeverity(Issues, TEXT("warning")));
	OutData->SetArrayField(TEXT("issues"), Issues);
	TSharedPtr<FJsonObject> AppliedObj = MakeShared<FJsonObject>();
	AppliedObj->SetNumberField(TEXT("variables_added"), VariableStats.Added);
	AppliedObj->SetNumberField(TEXT("variable_defaults_updated"), VariableStats.DefaultsUpdated);
	AppliedObj->SetNumberField(TEXT("variable_flags_updated"), VariableStats.FlagsUpdated);
	AppliedObj->SetNumberField(TEXT("variables_existing"), VariableStats.Existing);
	OutData->SetObjectField(TEXT("applied"), AppliedObj);
	OutData->SetObjectField(TEXT("coverage"), UeAgentControlRigOps::MakeCoverageJson());
	OutData->SetObjectField(TEXT("compile_report"), ApplyCompileReport.IsValid() ? ApplyCompileReport : UeAgentControlRigOps::MakeCompileReportJson(Blueprint));
	if (Blueprint)
	{
		OutData->SetObjectField(TEXT("readback"), UeAgentControlRigOps::BuildControlRigInfo(Blueprint));
	}
	return !UeAgentControlRigOps::HasBlockingIssue(Issues);
}
