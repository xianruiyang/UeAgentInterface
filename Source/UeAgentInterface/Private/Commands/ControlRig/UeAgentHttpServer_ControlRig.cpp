// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_ControlRig.h"
#include "Domains/Rigging/UeAgentControlRigDomainBackend.h"

#include "Utilities/UeAgentAuthoringFileIO.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentFolderDetailsExport.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"
#include "Utilities/UeAgentUObjectProperties.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "ControlRig.h"
#include "ControlRigBlueprint.h"
#include "ControlRigBlueprintFactory.h"
#include "ControlRigGizmoLibrary.h"
#include "ControlRigSequencerEditorLibrary.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Exporters/AnimSeqExportOption.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "Materials/Material.h"
#include "MovieScene.h"
#include "MovieSceneBindingProxy.h"
#include "MovieSceneSequenceID.h"
#include "MovieSceneSequencePlayer.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "Rigs/FKControlRig.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "SequencerTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Units/Execution/RigUnit_InverseExecution.h"
#include "Units/Execution/RigUnit_PrepareForExecution.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

bool FUeAgentHttpServer::ExecuteControlRigShapeLibraryCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("control_rig_shape_library_create"))
	{
		return CmdControlRigShapeLibraryCreate(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_shape_library_get_info"))
	{
		return CmdControlRigShapeLibraryGetInfo(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_shape_library_export_json"))
	{
		return CmdControlRigShapeLibraryExportJson(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_shape_library_validate_json"))
	{
		return CmdControlRigShapeLibraryValidateJson(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_shape_library_apply_json"))
	{
		return CmdControlRigShapeLibraryApplyJson(Ctx, OutData, OutError);
	}
	OutError = TEXT("unknown_control_rig_shape_library_command");
	return false;
}

bool FUeAgentHttpServer::ExecuteControlRigCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("control_rig_create"))
	{
		return CmdControlRigCreate(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_get_info"))
	{
		return CmdControlRigGetInfo(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_export_folder"))
	{
		return CmdControlRigExportFolder(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_validate_folder"))
	{
		return CmdControlRigValidateFolder(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_apply_folder"))
	{
		return CmdControlRigApplyFolder(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_compile"))
	{
		return CmdControlRigCompile(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_get_compile_log"))
	{
		return CmdControlRigGetCompileLog(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_open_editor"))
	{
		return CmdControlRigOpenEditor(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_graph_get_view"))
	{
		return CmdControlRigGraphGetView(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_graph_set_view"))
	{
		return CmdControlRigGraphSetView(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_screenshot"))
	{
		return CmdControlRigScreenshot(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_runtime_probe"))
	{
		return CmdControlRigRuntimeProbe(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_bake_to_animation"))
	{
		return CmdControlRigBakeToAnimation(Ctx, OutData, OutError);
	}
	if (CommandLower == TEXT("control_rig_bake_to_control_rig"))
	{
		return CmdControlRigBakeToControlRig(Ctx, OutData, OutError);
	}
	OutError = TEXT("unknown_control_rig_command");
	return false;
}

bool FUeAgentHttpServer::CmdControlRigCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("asset_path_required");
		return false;
	}

	FString RigTypeText;
	JsonTryGetString(Ctx.Params, TEXT("control_rig_type"), RigTypeText);
	EControlRigType RigType = EControlRigType::IndependentRig;
	if (!RigTypeText.TrimStartAndEnd().IsEmpty() && !UeAgentControlRigOps::ParseControlRigTypeText(RigTypeText, RigType))
	{
		OutError = TEXT("invalid_control_rig_type");
		return false;
	}

	FString PreviewMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("preview_skeletal_mesh"), PreviewMeshPath);
	bool bImportHierarchy = true;
	JsonTryGetBool(Ctx.Params, TEXT("import_hierarchy_from_preview"), bImportHierarchy);

	UControlRigBlueprint* Blueprint = UeAgentControlRigOps::CreateControlRigAsset(AssetPath, RigType == EControlRigType::ModularRig, PreviewMeshPath, bImportHierarchy, OutError);
	if (!Blueprint)
	{
		return false;
	}
	if (RigType == EControlRigType::RigModule)
	{
		FString ConvertError;
		if (!Blueprint->TurnIntoControlRigModule(false, &ConvertError))
		{
			OutError = ConvertError.IsEmpty() ? TEXT("turn_into_control_rig_module_failed") : ConvertError;
			return false;
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentControlRigOps::SaveAssetPackage(Blueprint, OutError))
	{
		return false;
	}

	OutData = UeAgentControlRigOps::BuildControlRigInfo(Blueprint);
	OutData->SetBoolField(TEXT("created"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdControlRigGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	OutData = UeAgentControlRigOps::BuildControlRigInfo(Blueprint);
	bool bRequireAnimationEditingReady = false;
	JsonTryGetBool(Ctx.Params, TEXT("require_animation_editing_ready"), bRequireAnimationEditingReady);
	if (bRequireAnimationEditingReady && !OutData->GetBoolField(TEXT("animation_editing_ready")))
	{
		OutError = TEXT("control_rig_missing_bidirectional_animation_editing_events");
		return false;
	}
	return true;
}
