// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_IKAssets.h"

#include "Domains/Rigging/UeAgentIKDomainBackend.h"

#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentFolderDetailsExport.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "UeAgentInterfaceSettings.h"

#include "Animation/Skeleton.h"
#include "Camera/CameraTypes.h"
#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "ControlRig.h"
#include "ControlRigObjectBinding.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Developer/AssetTools/Public/IAssetTools.h"
#include "EditorAnimUtils.h"
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/PlatformProcess.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "Materials/Material.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "GameFramework/Actor.h"
#include "Engine/PointLight.h"
#include "MovieScene.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieSceneTrack.h"
#include "Rigs/RigHierarchy.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargetFactory.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargetDeprecated.h"
#include "Retargeter/IKRetargetSettings.h"
#include "Retargeter/RetargetOps/StrideWarpingOp.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/IKRigProcessor.h"
#include "Rig/Solvers/IKRigBodyMoverSolver.h"
#include "Rig/Solvers/IKRigFullBodyIK.h"
#include "RigEditor/IKRigController.h"
#include "RigEditor/IKRigDefinitionFactory.h"

bool FUeAgentLegacyMechanics::ExecuteIKRigCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("ik_rig_create")) return CmdIKRigCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_get_info")) return CmdIKRigGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_export_folder")) return CmdIKRigExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_apply_folder")) return CmdIKRigApplyFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_validate_folder")) return CmdIKRigValidateFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_set_solver")) return CmdIKRigSetSolver(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_set_preview_mesh")) return CmdIKRigSetPreviewMesh(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_set_goal")) return CmdIKRigSetGoal(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_set_retarget_root")) return CmdIKRigSetRetargetRoot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_set_retarget_chain")) return CmdIKRigSetRetargetChain(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_apply_auto_retarget_definition")) return CmdIKRigApplyAutoRetargetDefinition(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_preview_solve")) return CmdIKRigPreviewSolve(Ctx, OutData, OutError);

	OutError = TEXT("unknown_ik_rig_command");
	return false;
}

bool FUeAgentLegacyMechanics::ExecuteIKRetargeterCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("ik_retargeter_create")) return CmdIKRetargeterCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_get_info")) return CmdIKRetargeterGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_export_folder")) return CmdIKRetargeterExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_apply_folder")) return CmdIKRetargeterApplyFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_validate_folder")) return CmdIKRetargeterValidateFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_set_ik_rig")) return CmdIKRetargeterSetIKRig(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_set_settings")) return CmdIKRetargeterSetSettings(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_set_pose")) return CmdIKRetargeterSetPose(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_set_preview_mesh")) return CmdIKRetargeterSetPreviewMesh(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_auto_map_chains")) return CmdIKRetargeterAutoMapChains(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_auto_align_pose")) return CmdIKRetargeterAutoAlignPose(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_duplicate_and_retarget")) return CmdIKRetargeterDuplicateAndRetarget(Ctx, OutData, OutError);

	OutError = TEXT("unknown_ik_retargeter_command");
	return false;
}

bool FUeAgentLegacyMechanics::ExecuteRetargetBatchCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("retarget_batch_export_json")) return CmdRetargetBatchExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("retarget_batch_apply_json")) return CmdRetargetBatchApplyJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("retarget_batch_validate_json")) return CmdRetargetBatchValidateJson(Ctx, OutData, OutError);

	OutError = TEXT("unknown_retarget_batch_command");
	return false;
}

bool FUeAgentLegacyMechanics::CmdIKRigCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UIKRigDefinitionFactory* Factory = NewObject<UIKRigDefinitionFactory>();
	UIKRigDefinition* IKRig = Cast<UIKRigDefinition>(UeAgentIKAssetOps::CreateAssetWithFactory(AssetPath, UIKRigDefinition::StaticClass(), Factory, OutError));
	if (!IKRig)
	{
		return false;
	}

	UIKRigController* Controller = UIKRigController::GetController(IKRig);
	if (!Controller)
	{
		OutError = TEXT("ik_rig_controller_not_available");
		return false;
	}

	FString PreviewMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("preview_skeletal_mesh"), PreviewMeshPath);
	if (!PreviewMeshPath.TrimStartAndEnd().IsEmpty())
	{
		USkeletalMesh* PreviewMesh = UeAgentIKAssetOps::LoadSkeletalMesh(PreviewMeshPath);
		if (!PreviewMesh)
		{
			OutError = TEXT("preview_mesh_not_found");
			return false;
		}
		if (!Controller->SetSkeletalMesh(PreviewMesh))
		{
			OutError = TEXT("set_preview_mesh_failed");
			return false;
		}
	}

	bool bApplyAutoRetargetDefinition = false;
	JsonTryGetBool(Ctx.Params, TEXT("apply_auto_retarget_definition"), bApplyAutoRetargetDefinition);
	if (bApplyAutoRetargetDefinition)
	{
		Controller->ApplyAutoGeneratedRetargetDefinition();
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentIKAssetOps::SaveAssetPackage(IKRig, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentIKAssetOps::NormalizeAssetPath(IKRig->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), IKRig->GetPathName());
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), IKRig->GetPreviewMesh() ? IKRig->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("retarget_root"), Controller->GetRetargetRoot().ToString());
	OutData->SetNumberField(TEXT("goal_count"), Controller->GetAllGoals().Num());
	OutData->SetNumberField(TEXT("retarget_chain_count"), Controller->GetRetargetChains().Num());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRigGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UIKRigDefinition* IKRig = UeAgentIKAssetOps::LoadIKRig(AssetPath);
	if (!IKRig)
	{
		OutError = TEXT("ik_rig_not_found");
		return false;
	}

	UIKRigController* Controller = UIKRigController::GetController(IKRig);
	if (!Controller)
	{
		OutError = TEXT("ik_rig_controller_not_available");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Goals;
	for (UIKRigEffectorGoal* Goal : Controller->GetAllGoals())
	{
		if (!Goal)
		{
			continue;
		}
		Goals.Add(MakeShared<FJsonValueObject>(UeAgentIKAssetOps::BuildGoalJson(Goal)));
	}

	TArray<TSharedPtr<FJsonValue>> Chains;
	for (const FBoneChain& Chain : Controller->GetRetargetChains())
	{
		Chains.Add(MakeShared<FJsonValueObject>(UeAgentIKAssetOps::BuildChainJson(Chain)));
	}

	TArray<TSharedPtr<FJsonValue>> Solvers;
	for (int32 SolverIndex = 0; SolverIndex < Controller->GetNumSolvers(); ++SolverIndex)
	{
		TSharedPtr<FJsonObject> SolverObject = MakeShared<FJsonObject>();
		SolverObject->SetNumberField(TEXT("solver_index"), SolverIndex);
		SolverObject->SetBoolField(TEXT("enabled"), Controller->GetSolverEnabled(SolverIndex));
		if (FInstancedStruct* SolverStruct = Controller->GetSolverStructAtIndex(SolverIndex))
		{
			SolverObject->SetStringField(TEXT("solver_struct"), SolverStruct->GetScriptStruct() ? SolverStruct->GetScriptStruct()->GetPathName() : TEXT(""));
			if (SolverStruct->GetScriptStruct() == FIKRigBodyMoverSolver::StaticStruct())
			{
				const FIKRigBodyMoverSolver& BodyMoverSolver = SolverStruct->Get<FIKRigBodyMoverSolver>();
				SolverObject->SetStringField(TEXT("solver_kind"), TEXT("body_mover"));
				SolverObject->SetObjectField(TEXT("settings"), UeAgentIKAssetOps::BuildBodyMoverSolverSettingsJson(BodyMoverSolver.Settings));

				TArray<TSharedPtr<FJsonValue>> GoalSettings;
				for (const FIKRigBodyMoverGoalSettings& GoalSetting : BodyMoverSolver.AllGoalSettings)
				{
					GoalSettings.Add(MakeShared<FJsonValueObject>(UeAgentIKAssetOps::BuildBodyMoverGoalSettingsJson(GoalSetting)));
				}
				SolverObject->SetNumberField(TEXT("goal_setting_count"), GoalSettings.Num());
				SolverObject->SetArrayField(TEXT("goal_settings"), GoalSettings);
			}
			else if (SolverStruct->GetScriptStruct() == FIKRigFullBodyIKSolver::StaticStruct())
			{
				const FIKRigFullBodyIKSolver& FBIKSolver = SolverStruct->Get<FIKRigFullBodyIKSolver>();
				SolverObject->SetStringField(TEXT("solver_kind"), TEXT("full_body_ik"));
				SolverObject->SetObjectField(TEXT("settings"), UeAgentIKAssetOps::BuildFBIKSolverSettingsJson(FBIKSolver.Settings));

				TArray<TSharedPtr<FJsonValue>> GoalSettings;
				for (const FIKRigFBIKGoalSettings& GoalSetting : FBIKSolver.AllGoalSettings)
				{
					GoalSettings.Add(MakeShared<FJsonValueObject>(UeAgentIKAssetOps::BuildFBIKGoalSettingsJson(GoalSetting)));
				}
				SolverObject->SetNumberField(TEXT("goal_setting_count"), GoalSettings.Num());
				SolverObject->SetArrayField(TEXT("goal_settings"), GoalSettings);

				TArray<TSharedPtr<FJsonValue>> BoneSettings;
				for (const FIKRigFBIKBoneSettings& BoneSetting : FBIKSolver.AllBoneSettings)
				{
					BoneSettings.Add(MakeShared<FJsonValueObject>(UeAgentIKAssetOps::BuildFBIKBoneSettingsJson(BoneSetting)));
				}
				SolverObject->SetNumberField(TEXT("bone_setting_count"), BoneSettings.Num());
				SolverObject->SetArrayField(TEXT("bone_settings"), BoneSettings);
			}
		}
		SolverObject->SetStringField(TEXT("start_bone"), Controller->GetStartBone(SolverIndex).ToString());
		SolverObject->SetStringField(TEXT("end_bone"), Controller->GetEndBone(SolverIndex).ToString());
		UeAgentAuthoringJson::ApplyAuthoringMetadata(SolverObject, TEXT("solver_index"), FString::FromInt(SolverIndex), SolverObject->HasField(TEXT("solver_kind")) ? SolverObject->GetStringField(TEXT("solver_kind")) : TEXT("Solver"), TEXT("IK Rig/Solvers"), TEXT("solver"));
		Solvers.Add(MakeShared<FJsonValueObject>(SolverObject));
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentIKAssetOps::NormalizeAssetPath(IKRig->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), IKRig->GetPathName());
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), IKRig->GetPreviewMesh() ? IKRig->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("retarget_root"), Controller->GetRetargetRoot().ToString());
	OutData->SetNumberField(TEXT("goal_count"), Goals.Num());
	OutData->SetArrayField(TEXT("goals"), Goals);
	OutData->SetNumberField(TEXT("retarget_chain_count"), Chains.Num());
	OutData->SetArrayField(TEXT("retarget_chains"), Chains);
	OutData->SetNumberField(TEXT("solver_count"), Solvers.Num());
	OutData->SetArrayField(TEXT("solvers"), Solvers);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRigSetSolver(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UIKRigDefinition* IKRig = UeAgentIKAssetOps::LoadIKRig(AssetPath);
	if (!IKRig)
	{
		OutError = TEXT("ik_rig_not_found");
		return false;
	}

	UIKRigController* Controller = UIKRigController::GetController(IKRig);
	if (!Controller)
	{
		OutError = TEXT("ik_rig_controller_not_available");
		return false;
	}

	bool bRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("remove"), bRemove);

	double SolverIndexValue = 0.0;
	const bool bHasSolverIndex = JsonTryGetNumber(Ctx.Params, TEXT("solver_index"), SolverIndexValue);
	int32 SolverIndex = bHasSolverIndex ? static_cast<int32>(SolverIndexValue) : INDEX_NONE;

	if (bRemove)
	{
		if (SolverIndex == INDEX_NONE)
		{
			OutError = TEXT("missing_solver_index");
			return false;
		}
		if (!Controller->RemoveSolver(SolverIndex))
		{
			OutError = TEXT("remove_solver_failed");
			return false;
		}

		bool bSaveAfterSet = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
		if (bSaveAfterSet && !UeAgentIKAssetOps::SaveAssetPackage(IKRig, OutError))
		{
			return false;
		}

		OutData->SetStringField(TEXT("asset_path"), IKRig->GetPathName());
		OutData->SetNumberField(TEXT("solver_index"), SolverIndex);
		OutData->SetBoolField(TEXT("removed"), true);
		OutData->SetNumberField(TEXT("solver_count"), Controller->GetNumSolvers());
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}

	FString SolverType;
	JsonTryGetString(Ctx.Params, TEXT("solver_type"), SolverType);
	SolverType = SolverType.TrimStartAndEnd();
	if (SolverIndex == INDEX_NONE)
	{
		if (SolverType.IsEmpty())
		{
			OutError = TEXT("missing_solver_index_or_solver_type");
			return false;
		}
		SolverIndex = Controller->AddSolver(SolverType);
		if (SolverIndex == INDEX_NONE)
		{
			OutError = TEXT("add_solver_failed");
			return false;
		}
	}

	bool bBoolValue = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("enabled"), bBoolValue) && !Controller->SetSolverEnabled(SolverIndex, bBoolValue))
	{
		OutError = TEXT("set_solver_enabled_failed");
		return false;
	}

	FString StartBoneNameText;
	if (JsonTryGetString(Ctx.Params, TEXT("start_bone_name"), StartBoneNameText))
	{
		StartBoneNameText = StartBoneNameText.TrimStartAndEnd();
		if (!StartBoneNameText.IsEmpty() && !StartBoneNameText.Equals(TEXT("None"), ESearchCase::IgnoreCase) && !Controller->SetStartBone(FName(*StartBoneNameText), SolverIndex))
		{
			OutError = TEXT("set_solver_start_bone_failed");
			return false;
		}
	}

	FString EndBoneNameText;
	if (JsonTryGetString(Ctx.Params, TEXT("end_bone_name"), EndBoneNameText))
	{
		EndBoneNameText = EndBoneNameText.TrimStartAndEnd();
		if (!EndBoneNameText.IsEmpty() && !EndBoneNameText.Equals(TEXT("None"), ESearchCase::IgnoreCase) && !Controller->SetEndBone(FName(*EndBoneNameText), SolverIndex))
		{
			OutError = TEXT("set_solver_end_bone_failed");
			return false;
		}
	}

	double MoveToIndexValue = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("move_to_index"), MoveToIndexValue))
	{
		if (!Controller->MoveSolverInStack(SolverIndex, static_cast<int32>(MoveToIndexValue)))
		{
			OutError = TEXT("move_solver_failed");
			return false;
		}
	}

	bool bTouchedSolverStruct = false;
	FInstancedStruct* SolverStruct = Controller->GetSolverStructAtIndex(SolverIndex);
	if (!SolverStruct)
	{
		OutError = TEXT("solver_struct_not_found");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ConnectGoalNames = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("connect_goal_names"), ConnectGoalNames) && ConnectGoalNames)
	{
		for (const TSharedPtr<FJsonValue>& GoalNameValue : *ConnectGoalNames)
		{
			if (!GoalNameValue.IsValid())
			{
				continue;
			}
			const FString GoalNameText = GoalNameValue->AsString().TrimStartAndEnd();
			if (GoalNameText.IsEmpty())
			{
				continue;
			}
			if (!Controller->ConnectGoalToSolver(FName(*GoalNameText), SolverIndex))
			{
				OutError = TEXT("connect_goal_to_solver_failed");
				return false;
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* DisconnectGoalNames = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("disconnect_goal_names"), DisconnectGoalNames) && DisconnectGoalNames)
	{
		for (const TSharedPtr<FJsonValue>& GoalNameValue : *DisconnectGoalNames)
		{
			if (!GoalNameValue.IsValid())
			{
				continue;
			}
			const FString GoalNameText = GoalNameValue->AsString().TrimStartAndEnd();
			if (GoalNameText.IsEmpty())
			{
				continue;
			}
			if (!Controller->DisconnectGoalFromSolver(FName(*GoalNameText), SolverIndex))
			{
				OutError = TEXT("disconnect_goal_from_solver_failed");
				return false;
			}
		}
	}

	if (const TSharedPtr<FJsonObject>* SettingsObject = nullptr; Ctx.Params->TryGetObjectField(TEXT("settings"), SettingsObject) && SettingsObject && SettingsObject->IsValid())
	{
		if (SolverStruct->GetScriptStruct() == FIKRigBodyMoverSolver::StaticStruct())
		{
			FIKRigBodyMoverSolver& BodyMoverSolver = SolverStruct->GetMutable<FIKRigBodyMoverSolver>();
			if (!UeAgentIKAssetOps::ApplyBodyMoverSolverSettingsPatch(*SettingsObject, BodyMoverSolver.Settings, OutError))
			{
				return false;
			}
			bTouchedSolverStruct = true;
		}
		else if (SolverStruct->GetScriptStruct() == FIKRigFullBodyIKSolver::StaticStruct())
		{
			FIKRigFullBodyIKSolver& FBIKSolver = SolverStruct->GetMutable<FIKRigFullBodyIKSolver>();
			if (!UeAgentIKAssetOps::ApplyFBIKSolverSettingsPatch(*SettingsObject, FBIKSolver.Settings, OutError))
			{
				return false;
			}
			bTouchedSolverStruct = true;
		}
		else
		{
			OutError = TEXT("solver_settings_not_supported");
			return false;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* GoalSettingsArray = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("goal_settings"), GoalSettingsArray) && GoalSettingsArray)
	{
		for (const TSharedPtr<FJsonValue>& GoalSettingValue : *GoalSettingsArray)
		{
			const TSharedPtr<FJsonObject>* GoalSettingObject = nullptr;
			if (!GoalSettingValue.IsValid() || !GoalSettingValue->TryGetObject(GoalSettingObject) || !GoalSettingObject || !GoalSettingObject->IsValid())
			{
				continue;
			}

			FString GoalNameText;
			if (!(*GoalSettingObject)->TryGetStringField(TEXT("goal_name"), GoalNameText) || GoalNameText.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("invalid_goal_settings_entry");
				return false;
			}

			const FName GoalName(*GoalNameText.TrimStartAndEnd());
			if (SolverStruct->GetScriptStruct() == FIKRigBodyMoverSolver::StaticStruct())
			{
				FIKRigBodyMoverSolver& BodyMoverSolver = SolverStruct->GetMutable<FIKRigBodyMoverSolver>();
				FIKRigGoalSettingsBase* GoalSettingsBase = BodyMoverSolver.GetGoalSettings(GoalName);
				if (!GoalSettingsBase)
				{
					OutError = TEXT("solver_goal_not_connected");
					return false;
				}

				FIKRigBodyMoverGoalSettings* GoalSettings = static_cast<FIKRigBodyMoverGoalSettings*>(GoalSettingsBase);
				(*GoalSettingObject)->TryGetNumberField(TEXT("influence_multiplier"), GoalSettings->InfluenceMultiplier);
				bTouchedSolverStruct = true;
			}
			else if (SolverStruct->GetScriptStruct() == FIKRigFullBodyIKSolver::StaticStruct())
			{
				FIKRigFullBodyIKSolver& FBIKSolver = SolverStruct->GetMutable<FIKRigFullBodyIKSolver>();
				FIKRigGoalSettingsBase* GoalSettingsBase = FBIKSolver.GetGoalSettings(GoalName);
				if (!GoalSettingsBase)
				{
					OutError = TEXT("solver_goal_not_connected");
					return false;
				}

				FIKRigFBIKGoalSettings* GoalSettings = static_cast<FIKRigFBIKGoalSettings*>(GoalSettingsBase);
				double DoubleValue = 0.0;
				if (JsonTryGetNumber(*GoalSettingObject, TEXT("chain_depth"), DoubleValue))
				{
					GoalSettings->ChainDepth = static_cast<int32>(DoubleValue);
				}
				(*GoalSettingObject)->TryGetNumberField(TEXT("strength_alpha"), GoalSettings->StrengthAlpha);
				(*GoalSettingObject)->TryGetNumberField(TEXT("pull_chain_alpha"), GoalSettings->PullChainAlpha);
				(*GoalSettingObject)->TryGetNumberField(TEXT("pin_rotation"), GoalSettings->PinRotation);
				bTouchedSolverStruct = true;
			}
			else
			{
				OutError = TEXT("solver_goal_settings_not_supported");
				return false;
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* BoneSettingsArray = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("bone_settings"), BoneSettingsArray) && BoneSettingsArray)
	{
		if (SolverStruct->GetScriptStruct() != FIKRigFullBodyIKSolver::StaticStruct())
		{
			OutError = TEXT("solver_bone_settings_not_supported");
			return false;
		}

		FIKRigFullBodyIKSolver& FBIKSolver = SolverStruct->GetMutable<FIKRigFullBodyIKSolver>();
		for (const TSharedPtr<FJsonValue>& BoneSettingValue : *BoneSettingsArray)
		{
			const TSharedPtr<FJsonObject>* BoneSettingObject = nullptr;
			if (!BoneSettingValue.IsValid() || !BoneSettingValue->TryGetObject(BoneSettingObject) || !BoneSettingObject || !BoneSettingObject->IsValid())
			{
				continue;
			}

			FString BoneNameText;
			if (!(*BoneSettingObject)->TryGetStringField(TEXT("bone_name"), BoneNameText) || BoneNameText.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("invalid_bone_settings_entry");
				return false;
			}

			const FName BoneName(*BoneNameText.TrimStartAndEnd());
			if (!FBIKSolver.HasSettingsOnBone(BoneName))
			{
				FBIKSolver.AddSettingsToBone(BoneName);
			}

			FIKRigBoneSettingsBase* BoneSettingsBase = FBIKSolver.GetBoneSettings(BoneName);
			if (!BoneSettingsBase)
			{
				OutError = TEXT("solver_bone_settings_not_found");
				return false;
			}

			FIKRigFBIKBoneSettings* BoneSettings = static_cast<FIKRigFBIKBoneSettings*>(BoneSettingsBase);
			(*BoneSettingObject)->TryGetNumberField(TEXT("rotation_stiffness"), BoneSettings->RotationStiffness);
			(*BoneSettingObject)->TryGetNumberField(TEXT("position_stiffness"), BoneSettings->PositionStiffness);
			(*BoneSettingObject)->TryGetBoolField(TEXT("use_preferred_angles"), BoneSettings->bUsePreferredAngles);
			UeAgentIKAssetOps::TryGetVectorObject(*BoneSettingObject, TEXT("preferred_angles"), BoneSettings->PreferredAngles);
			bTouchedSolverStruct = true;
		}
	}

	if (bTouchedSolverStruct)
	{
		IKRig->Modify();
		Controller->BroadcastNeedsReinitialized();
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentIKAssetOps::SaveAssetPackage(IKRig, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), IKRig->GetPathName());
	OutData->SetNumberField(TEXT("solver_index"), SolverIndex);
	OutData->SetStringField(TEXT("solver_struct"), SolverStruct && SolverStruct->GetScriptStruct() ? SolverStruct->GetScriptStruct()->GetPathName() : TEXT(""));
	if (SolverStruct && SolverStruct->GetScriptStruct() == FIKRigBodyMoverSolver::StaticStruct())
	{
		const FIKRigBodyMoverSolver& BodyMoverSolver = SolverStruct->Get<FIKRigBodyMoverSolver>();
		OutData->SetStringField(TEXT("solver_kind"), TEXT("body_mover"));
		OutData->SetObjectField(TEXT("settings"), UeAgentIKAssetOps::BuildBodyMoverSolverSettingsJson(BodyMoverSolver.Settings));

		TArray<TSharedPtr<FJsonValue>> GoalSettings;
		for (const FIKRigBodyMoverGoalSettings& GoalSetting : BodyMoverSolver.AllGoalSettings)
		{
			GoalSettings.Add(MakeShared<FJsonValueObject>(UeAgentIKAssetOps::BuildBodyMoverGoalSettingsJson(GoalSetting)));
		}
		OutData->SetNumberField(TEXT("goal_setting_count"), GoalSettings.Num());
		OutData->SetArrayField(TEXT("goal_settings"), GoalSettings);
	}
	else if (SolverStruct && SolverStruct->GetScriptStruct() == FIKRigFullBodyIKSolver::StaticStruct())
	{
		const FIKRigFullBodyIKSolver& FBIKSolver = SolverStruct->Get<FIKRigFullBodyIKSolver>();
		OutData->SetStringField(TEXT("solver_kind"), TEXT("full_body_ik"));
		OutData->SetObjectField(TEXT("settings"), UeAgentIKAssetOps::BuildFBIKSolverSettingsJson(FBIKSolver.Settings));

		TArray<TSharedPtr<FJsonValue>> GoalSettings;
		for (const FIKRigFBIKGoalSettings& GoalSetting : FBIKSolver.AllGoalSettings)
		{
			GoalSettings.Add(MakeShared<FJsonValueObject>(UeAgentIKAssetOps::BuildFBIKGoalSettingsJson(GoalSetting)));
		}
		OutData->SetNumberField(TEXT("goal_setting_count"), GoalSettings.Num());
		OutData->SetArrayField(TEXT("goal_settings"), GoalSettings);

		TArray<TSharedPtr<FJsonValue>> BoneSettings;
		for (const FIKRigFBIKBoneSettings& BoneSetting : FBIKSolver.AllBoneSettings)
		{
			BoneSettings.Add(MakeShared<FJsonValueObject>(UeAgentIKAssetOps::BuildFBIKBoneSettingsJson(BoneSetting)));
		}
		OutData->SetNumberField(TEXT("bone_setting_count"), BoneSettings.Num());
		OutData->SetArrayField(TEXT("bone_settings"), BoneSettings);
	}
	OutData->SetBoolField(TEXT("enabled"), Controller->GetSolverEnabled(SolverIndex));
	OutData->SetStringField(TEXT("start_bone"), Controller->GetStartBone(SolverIndex).ToString());
	OutData->SetStringField(TEXT("end_bone"), Controller->GetEndBone(SolverIndex).ToString());
	OutData->SetNumberField(TEXT("solver_count"), Controller->GetNumSolvers());
	OutData->SetBoolField(TEXT("removed"), false);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRigSetPreviewMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UIKRigDefinition* IKRig = UeAgentIKAssetOps::LoadIKRig(AssetPath);
	if (!IKRig)
	{
		OutError = TEXT("ik_rig_not_found");
		return false;
	}

	bool bClearPreviewMesh = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_preview_mesh"), bClearPreviewMesh);

	if (bClearPreviewMesh)
	{
		IKRig->Modify();
		IKRig->SetPreviewMesh(nullptr, true);
	}
	else
	{
		FString PreviewMeshPath;
		if (!JsonTryGetString(Ctx.Params, TEXT("skeletal_mesh_path"), PreviewMeshPath) || PreviewMeshPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_skeletal_mesh_path");
			return false;
		}

		USkeletalMesh* PreviewMesh = UeAgentIKAssetOps::LoadSkeletalMesh(PreviewMeshPath);
		if (!PreviewMesh)
		{
			OutError = TEXT("skeletal_mesh_not_found");
			return false;
		}

		UIKRigController* Controller = UIKRigController::GetController(IKRig);
		if (!Controller || !Controller->SetSkeletalMesh(PreviewMesh))
		{
			OutError = TEXT("set_preview_mesh_failed");
			return false;
		}
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentIKAssetOps::SaveAssetPackage(IKRig, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), IKRig->GetPathName());
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), IKRig->GetPreviewMesh() ? IKRig->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRigSetGoal(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString GoalNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("goal_name"), GoalNameText) || GoalNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_goal_name");
		return false;
	}
	FName GoalName(*GoalNameText.TrimStartAndEnd());

	UIKRigDefinition* IKRig = UeAgentIKAssetOps::LoadIKRig(AssetPath);
	if (!IKRig)
	{
		OutError = TEXT("ik_rig_not_found");
		return false;
	}

	UIKRigController* Controller = UIKRigController::GetController(IKRig);
	if (!Controller)
	{
		OutError = TEXT("ik_rig_controller_not_available");
		return false;
	}

	bool bRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("remove"), bRemove);

	const TArray<UIKRigEffectorGoal*>& Goals = Controller->GetAllGoals();
	UIKRigEffectorGoal* ExistingGoal = nullptr;
	for (UIKRigEffectorGoal* Goal : Goals)
	{
		if (Goal && Goal->GoalName == GoalName)
		{
			ExistingGoal = Goal;
			break;
		}
	}

	if (bRemove)
	{
		if (!Controller->RemoveGoal(GoalName))
		{
			OutError = TEXT("goal_not_found");
			return false;
		}
	}
	else
	{
		FString BoneNameText;
		if (!JsonTryGetString(Ctx.Params, TEXT("bone_name"), BoneNameText) || BoneNameText.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_bone_name");
			return false;
		}
		const FName BoneName(*BoneNameText.TrimStartAndEnd());

		if (ExistingGoal)
		{
			if (Controller->GetBoneForGoal(GoalName) != BoneName && !Controller->SetGoalBone(GoalName, BoneName))
			{
				OutError = TEXT("set_goal_bone_failed");
				return false;
			}
		}
		else
		{
			const FName CreatedGoalName = Controller->AddNewGoal(GoalName, BoneName);
			if (CreatedGoalName.IsNone())
			{
				OutError = TEXT("add_goal_failed");
				return false;
			}
			ExistingGoal = Controller->GetGoal(GoalName);
		}

		FString NewGoalNameText;
		if (JsonTryGetString(Ctx.Params, TEXT("new_goal_name"), NewGoalNameText) && !NewGoalNameText.TrimStartAndEnd().IsEmpty())
		{
			const FName RenamedGoal = Controller->RenameGoal(GoalName, FName(*NewGoalNameText.TrimStartAndEnd()));
			if (RenamedGoal.IsNone())
			{
				OutError = TEXT("rename_goal_failed");
				return false;
			}
			GoalName = RenamedGoal;
			ExistingGoal = Controller->GetGoal(GoalName);
		}

		if (!ExistingGoal)
		{
			ExistingGoal = Controller->GetGoal(GoalName);
		}
		if (!ExistingGoal)
		{
			OutError = TEXT("goal_not_found_after_create_or_update");
			return false;
		}

		double PositionAlphaValue = 0.0;
		if (JsonTryGetNumber(Ctx.Params, TEXT("position_alpha"), PositionAlphaValue))
		{
			ExistingGoal->Modify();
			ExistingGoal->PositionAlpha = FMath::Clamp(static_cast<float>(PositionAlphaValue), 0.0f, 1.0f);
		}

		double RotationAlphaValue = 0.0;
		if (JsonTryGetNumber(Ctx.Params, TEXT("rotation_alpha"), RotationAlphaValue))
		{
			ExistingGoal->Modify();
			ExistingGoal->RotationAlpha = FMath::Clamp(static_cast<float>(RotationAlphaValue), 0.0f, 1.0f);
		}

		FTransform GoalTransform = ExistingGoal->CurrentTransform;
		bool bUpdatedGoalTransform = false;
		FVector GoalLocation = GoalTransform.GetLocation();
		if (JsonTryGetVector(Ctx.Params, TEXT("position"), GoalLocation))
		{
			GoalTransform.SetLocation(GoalLocation);
			bUpdatedGoalTransform = true;
		}

		FRotator GoalRotation = GoalTransform.Rotator();
		if (JsonTryGetRotator(Ctx.Params, TEXT("rotation"), GoalRotation))
		{
			GoalTransform.SetRotation(GoalRotation.Quaternion());
			bUpdatedGoalTransform = true;
		}

		if (const TSharedPtr<FJsonObject>* TransformObject = nullptr; Ctx.Params->TryGetObjectField(TEXT("current_transform"), TransformObject) && TransformObject && TransformObject->IsValid())
		{
			FVector Location = GoalTransform.GetLocation();
			if (JsonTryGetVector(*TransformObject, TEXT("location"), Location))
			{
				GoalTransform.SetLocation(Location);
				bUpdatedGoalTransform = true;
			}

			FRotator Rotation = GoalTransform.Rotator();
			if (JsonTryGetRotator(*TransformObject, TEXT("rotation"), Rotation))
			{
				GoalTransform.SetRotation(Rotation.Quaternion());
				bUpdatedGoalTransform = true;
			}

			FVector Scale = GoalTransform.GetScale3D();
			if (JsonTryGetVector(*TransformObject, TEXT("scale"), Scale))
			{
				GoalTransform.SetScale3D(Scale);
				bUpdatedGoalTransform = true;
			}
		}

		if (bUpdatedGoalTransform)
		{
			Controller->SetGoalCurrentTransform(GoalName, GoalTransform);
			ExistingGoal = Controller->GetGoal(GoalName);
		}
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentIKAssetOps::SaveAssetPackage(IKRig, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), IKRig->GetPathName());
	OutData->SetStringField(TEXT("goal_name"), GoalName.ToString());
	OutData->SetBoolField(TEXT("removed"), bRemove);
	OutData->SetNumberField(TEXT("goal_count"), Controller->GetAllGoals().Num());
	if (!bRemove)
	{
		UIKRigEffectorGoal* UpdatedGoal = Controller->GetGoal(GoalName);
		if (UpdatedGoal)
		{
			OutData->SetStringField(TEXT("bone_name"), UpdatedGoal->BoneName.ToString());
			OutData->SetNumberField(TEXT("position_alpha"), UpdatedGoal->PositionAlpha);
			OutData->SetNumberField(TEXT("rotation_alpha"), UpdatedGoal->RotationAlpha);
			OutData->SetObjectField(TEXT("current_location"), UeAgentIKAssetOps::BuildVectorJson(UpdatedGoal->CurrentTransform.GetLocation()));
			OutData->SetObjectField(TEXT("current_rotation"), UeAgentIKAssetOps::BuildRotatorJson(UpdatedGoal->CurrentTransform.Rotator()));
		}
	}
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRigSetRetargetRoot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString RootBoneNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("root_bone_name"), RootBoneNameText) || RootBoneNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_root_bone_name");
		return false;
	}

	UIKRigDefinition* IKRig = UeAgentIKAssetOps::LoadIKRig(AssetPath);
	if (!IKRig)
	{
		OutError = TEXT("ik_rig_not_found");
		return false;
	}

	UIKRigController* Controller = UIKRigController::GetController(IKRig);
	if (!Controller || !Controller->SetRetargetRoot(FName(*RootBoneNameText.TrimStartAndEnd())))
	{
		OutError = TEXT("set_retarget_root_failed");
		return false;
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentIKAssetOps::SaveAssetPackage(IKRig, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), IKRig->GetPathName());
	OutData->SetStringField(TEXT("retarget_root"), Controller->GetRetargetRoot().ToString());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRigSetRetargetChain(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString ChainNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("chain_name"), ChainNameText) || ChainNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_chain_name");
		return false;
	}
	FName ChainName(*ChainNameText.TrimStartAndEnd());

	UIKRigDefinition* IKRig = UeAgentIKAssetOps::LoadIKRig(AssetPath);
	if (!IKRig)
	{
		OutError = TEXT("ik_rig_not_found");
		return false;
	}

	UIKRigController* Controller = UIKRigController::GetController(IKRig);
	if (!Controller)
	{
		OutError = TEXT("ik_rig_controller_not_available");
		return false;
	}

	bool bRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("remove"), bRemove);

	const FBoneChain* ExistingChain = Controller->GetRetargetChainByName(ChainName);
	if (bRemove)
	{
		if (!Controller->RemoveRetargetChain(ChainName))
		{
			OutError = TEXT("retarget_chain_not_found");
			return false;
		}
	}
	else
	{
		FString StartBoneNameText;
		FString EndBoneNameText;
		FString GoalNameText;
		JsonTryGetString(Ctx.Params, TEXT("start_bone_name"), StartBoneNameText);
		JsonTryGetString(Ctx.Params, TEXT("end_bone_name"), EndBoneNameText);
		JsonTryGetString(Ctx.Params, TEXT("goal_name"), GoalNameText);

		FString NewChainNameText;
		JsonTryGetString(Ctx.Params, TEXT("new_chain_name"), NewChainNameText);

		const FName ExistingChainName = ExistingChain ? ExistingChain->ChainName : NAME_None;
		const FName ExistingStartBone = ExistingChain ? ExistingChain->StartBone.BoneName : NAME_None;
		const FName ExistingEndBone = ExistingChain ? ExistingChain->EndBone.BoneName : NAME_None;
		const FName ExistingGoal = ExistingChain ? ExistingChain->IKGoalName : NAME_None;

		const FName DesiredChainName = NewChainNameText.TrimStartAndEnd().IsEmpty()
			? ChainName
			: FName(*NewChainNameText.TrimStartAndEnd());
		const FName DesiredStartBone = StartBoneNameText.TrimStartAndEnd().IsEmpty()
			? ExistingStartBone
			: FName(*StartBoneNameText.TrimStartAndEnd());
		const FName DesiredEndBone = EndBoneNameText.TrimStartAndEnd().IsEmpty()
			? ExistingEndBone
			: FName(*EndBoneNameText.TrimStartAndEnd());
		const FName DesiredGoal = GoalNameText.TrimStartAndEnd().IsEmpty()
			? ExistingGoal
			: FName(*GoalNameText.TrimStartAndEnd());

		if (DesiredStartBone.IsNone() || DesiredEndBone.IsNone())
		{
			OutError = TEXT("missing_start_or_end_bone_name");
			return false;
		}

		if (ExistingChain && !Controller->RemoveRetargetChain(ChainName))
		{
			OutError = TEXT("replace_retarget_chain_remove_failed");
			return false;
		}

		ChainName = Controller->AddRetargetChain(DesiredChainName, DesiredStartBone, DesiredEndBone, DesiredGoal);
		if (ChainName.IsNone())
		{
			if (ExistingChain)
			{
				Controller->AddRetargetChain(ExistingChainName, ExistingStartBone, ExistingEndBone, ExistingGoal);
			}
			OutError = TEXT("add_retarget_chain_failed");
			return false;
		}
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentIKAssetOps::SaveAssetPackage(IKRig, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), IKRig->GetPathName());
	OutData->SetStringField(TEXT("chain_name"), ChainName.ToString());
	OutData->SetBoolField(TEXT("removed"), bRemove);
	OutData->SetNumberField(TEXT("retarget_chain_count"), Controller->GetRetargetChains().Num());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRigApplyAutoRetargetDefinition(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UIKRigDefinition* IKRig = UeAgentIKAssetOps::LoadIKRig(AssetPath);
	if (!IKRig)
	{
		OutError = TEXT("ik_rig_not_found");
		return false;
	}

	UIKRigController* Controller = UIKRigController::GetController(IKRig);
	if (!Controller)
	{
		OutError = TEXT("ik_rig_controller_not_available");
		return false;
	}

	const bool bApplied = Controller->ApplyAutoGeneratedRetargetDefinition();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentIKAssetOps::SaveAssetPackage(IKRig, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), IKRig->GetPathName());
	OutData->SetBoolField(TEXT("applied"), bApplied);
	OutData->SetStringField(TEXT("retarget_root"), Controller->GetRetargetRoot().ToString());
	OutData->SetNumberField(TEXT("retarget_chain_count"), Controller->GetRetargetChains().Num());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRigPreviewSolve(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UIKRigDefinition* IKRig = UeAgentIKAssetOps::LoadIKRig(AssetPath);
	if (!IKRig)
	{
		OutError = TEXT("ik_rig_not_found");
		return false;
	}

	UIKRigController* Controller = UIKRigController::GetController(IKRig);
	if (!Controller)
	{
		OutError = TEXT("ik_rig_controller_not_available");
		return false;
	}

	FString SkeletalMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("skeletal_mesh_path"), SkeletalMeshPath);
	USkeletalMesh* SkeletalMesh = SkeletalMeshPath.TrimStartAndEnd().IsEmpty()
		? IKRig->GetPreviewMesh()
		: UeAgentIKAssetOps::LoadSkeletalMesh(SkeletalMeshPath);
	if (!SkeletalMesh)
	{
		OutError = SkeletalMeshPath.TrimStartAndEnd().IsEmpty() ? TEXT("missing_preview_skeletal_mesh") : TEXT("skeletal_mesh_not_found");
		return false;
	}

	FIKRigGoalContainer OptionalGoals;
	const TArray<TSharedPtr<FJsonValue>>* GoalOverrides = nullptr;
	const bool bHasGoalOverrides = Ctx.Params->TryGetArrayField(TEXT("goals"), GoalOverrides) && GoalOverrides;
	if (bHasGoalOverrides)
	{
		OptionalGoals.FillWithGoalArray(Controller->GetAllGoals());
		for (const TSharedPtr<FJsonValue>& Value : *GoalOverrides)
		{
			const TSharedPtr<FJsonObject>* GoalObject = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(GoalObject) || !GoalObject || !GoalObject->IsValid())
			{
				OutError = TEXT("invalid_goal_entry");
				return false;
			}

			FString GoalNameText;
			if (!(*GoalObject)->TryGetStringField(TEXT("goal_name"), GoalNameText))
			{
				(*GoalObject)->TryGetStringField(TEXT("name"), GoalNameText);
			}
			GoalNameText = GoalNameText.TrimStartAndEnd();
			if (GoalNameText.IsEmpty())
			{
				OutError = TEXT("missing_goal_name");
				return false;
			}

			const FName GoalName(*GoalNameText);
			const FIKRigGoal* ExistingGoal = OptionalGoals.FindGoalByName(GoalName);
			FIKRigGoal Goal = ExistingGoal ? *ExistingGoal : FIKRigGoal();
			Goal.Name = GoalName;

			FString BoneNameText;
			if ((*GoalObject)->TryGetStringField(TEXT("bone_name"), BoneNameText))
			{
				BoneNameText = BoneNameText.TrimStartAndEnd();
				if (!BoneNameText.IsEmpty())
				{
					Goal.BoneName = FName(*BoneNameText);
				}
			}
			if (Goal.BoneName.IsNone())
			{
				OutError = TEXT("missing_goal_bone_name");
				return false;
			}

			JsonTryGetVector(*GoalObject, TEXT("position"), Goal.Position);
			JsonTryGetRotator(*GoalObject, TEXT("rotation"), Goal.Rotation);

			double AlphaValue = Goal.PositionAlpha;
			if (UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*GoalObject, { TEXT("position_alpha"), TEXT("PositionAlpha") }, AlphaValue))
			{
				Goal.PositionAlpha = static_cast<float>(AlphaValue);
			}
			AlphaValue = Goal.RotationAlpha;
			if (UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*GoalObject, { TEXT("rotation_alpha"), TEXT("RotationAlpha") }, AlphaValue))
			{
				Goal.RotationAlpha = static_cast<float>(AlphaValue);
			}

			bool bEnabled = Goal.bEnabled;
			if ((*GoalObject)->TryGetBoolField(TEXT("enabled"), bEnabled))
			{
				Goal.bEnabled = bEnabled;
			}

			FString SpaceText;
			if ((*GoalObject)->TryGetStringField(TEXT("position_space"), SpaceText))
			{
				EIKRigGoalSpace Space = Goal.PositionSpace;
				if (!UeAgentIKAssetOps::ParseIKRigGoalSpace(SpaceText, Space))
				{
					OutError = TEXT("invalid_position_space");
					return false;
				}
				Goal.PositionSpace = Space;
			}
			if ((*GoalObject)->TryGetStringField(TEXT("rotation_space"), SpaceText))
			{
				EIKRigGoalSpace Space = Goal.RotationSpace;
				if (!UeAgentIKAssetOps::ParseIKRigGoalSpace(SpaceText, Space))
				{
					OutError = TEXT("invalid_rotation_space");
					return false;
				}
				Goal.RotationSpace = Space;
			}

			OptionalGoals.SetIKGoal(Goal);
		}
	}

	FIKRigProcessor Processor;
	Processor.Log.SetLogTarget(IKRig);
	Processor.Log.Clear();
	Processor.Initialize(IKRig, SkeletalMesh, OptionalGoals);

	OutData->SetStringField(TEXT("asset_path"), UeAgentIKAssetOps::NormalizeAssetPath(IKRig->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), IKRig->GetPathName());
	OutData->SetStringField(TEXT("skeletal_mesh"), SkeletalMesh->GetPathName());
	OutData->SetNumberField(TEXT("solver_count"), IKRig->GetSolverStructs().Num());
	OutData->SetBoolField(TEXT("used_goal_overrides"), bHasGoalOverrides);
	OutData->SetArrayField(TEXT("errors"), UeAgentIKAssetOps::TextArrayToJson(Processor.Log.GetErrors()));
	OutData->SetArrayField(TEXT("warnings"), UeAgentIKAssetOps::TextArrayToJson(Processor.Log.GetWarnings()));
	OutData->SetArrayField(TEXT("messages"), UeAgentIKAssetOps::TextArrayToJson(Processor.Log.GetMessages()));
	OutData->SetBoolField(TEXT("initialized"), Processor.IsInitialized());

	if (!Processor.IsInitialized())
	{
		OutError = TEXT("ik_rig_preview_solve_initialize_failed");
		return false;
	}

	Processor.SetInputPoseToRefPose();
	Processor.Solve();

	TArray<FTransform> OutputPoseGlobal;
	Processor.GetOutputPoseGlobal(OutputPoseGlobal);

	const FIKRigGoalContainer& SolvedGoals = Processor.GetGoalContainer();
	TArray<TSharedPtr<FJsonValue>> GoalResults;
	for (const FIKRigGoal& Goal : SolvedGoals.GetGoalArray())
	{
		TSharedPtr<FJsonObject> GoalObject = MakeShared<FJsonObject>();
		GoalObject->SetStringField(TEXT("goal_name"), Goal.Name.ToString());
		GoalObject->SetStringField(TEXT("bone_name"), Goal.BoneName.ToString());
		GoalObject->SetBoolField(TEXT("enabled"), Goal.bEnabled);
		GoalObject->SetObjectField(TEXT("position"), UeAgentIKAssetOps::BuildVectorJson(Goal.Position));
		GoalObject->SetObjectField(TEXT("rotation"), UeAgentIKAssetOps::BuildRotatorJson(Goal.Rotation));
		GoalObject->SetNumberField(TEXT("position_alpha"), Goal.PositionAlpha);
		GoalObject->SetNumberField(TEXT("rotation_alpha"), Goal.RotationAlpha);
		GoalObject->SetStringField(TEXT("position_space"), UeAgentIKAssetOps::IKRigGoalSpaceToString(Goal.PositionSpace));
		GoalObject->SetStringField(TEXT("rotation_space"), UeAgentIKAssetOps::IKRigGoalSpaceToString(Goal.RotationSpace));
		GoalObject->SetObjectField(TEXT("final_blended_position"), UeAgentIKAssetOps::BuildVectorJson(Goal.FinalBlendedPosition));
		GoalObject->SetObjectField(TEXT("final_blended_rotation"), UeAgentIKAssetOps::BuildRotatorJson(Goal.FinalBlendedRotation.Rotator()));
		GoalResults.Add(MakeShared<FJsonValueObject>(GoalObject));
	}

	bool bIncludeAllBones = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_all_bones"), bIncludeAllBones);

	double MaxOutputBoneValue = 64.0;
	JsonTryGetNumber(Ctx.Params, TEXT("max_output_bones"), MaxOutputBoneValue);
	const int32 MaxOutputBones = FMath::Clamp(static_cast<int32>(MaxOutputBoneValue), 1, 512);

	TArray<int32> SampleBoneIndices;
	TArray<FString> MissingSampleBones;
	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
	const TArray<TSharedPtr<FJsonValue>>* SampleBones = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("sample_bones"), SampleBones) && SampleBones && SampleBones->Num() > 0)
	{
		for (const TSharedPtr<FJsonValue>& Value : *SampleBones)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			const FString BoneNameText = Value->AsString().TrimStartAndEnd();
			if (BoneNameText.IsEmpty())
			{
				continue;
			}
			const int32 BoneIndex = RefSkeleton.FindBoneIndex(FName(*BoneNameText));
			if (BoneIndex == INDEX_NONE || !OutputPoseGlobal.IsValidIndex(BoneIndex))
			{
				MissingSampleBones.Add(BoneNameText);
				continue;
			}
			SampleBoneIndices.Add(BoneIndex);
		}
	}
	else
	{
		const int32 Limit = bIncludeAllBones ? OutputPoseGlobal.Num() : FMath::Min(OutputPoseGlobal.Num(), MaxOutputBones);
		for (int32 BoneIndex = 0; BoneIndex < Limit; ++BoneIndex)
		{
			SampleBoneIndices.Add(BoneIndex);
		}
	}

	if (bIncludeAllBones && SampleBoneIndices.Num() > MaxOutputBones)
	{
		SampleBoneIndices.SetNum(MaxOutputBones);
	}

	TArray<TSharedPtr<FJsonValue>> OutputPoseSample;
	for (const int32 BoneIndex : SampleBoneIndices)
	{
		if (!OutputPoseGlobal.IsValidIndex(BoneIndex))
		{
			continue;
		}

		TSharedPtr<FJsonObject> BoneObject = MakeShared<FJsonObject>();
		BoneObject->SetNumberField(TEXT("bone_index"), BoneIndex);
		BoneObject->SetStringField(TEXT("bone_name"), RefSkeleton.GetBoneName(BoneIndex).ToString());
		BoneObject->SetObjectField(TEXT("global_transform"), UeAgentIKAssetOps::BuildTransformJson(OutputPoseGlobal[BoneIndex]));
		OutputPoseSample.Add(MakeShared<FJsonValueObject>(BoneObject));
	}

	OutData->SetBoolField(TEXT("solved"), true);
	OutData->SetNumberField(TEXT("goal_count"), GoalResults.Num());
	OutData->SetArrayField(TEXT("goals"), GoalResults);
	OutData->SetNumberField(TEXT("output_bone_count"), OutputPoseGlobal.Num());
	OutData->SetNumberField(TEXT("output_pose_sample_count"), OutputPoseSample.Num());
	OutData->SetArrayField(TEXT("output_pose_sample"), OutputPoseSample);
	OutData->SetArrayField(TEXT("missing_sample_bones"), UeAgentIKAssetOps::StringArrayToJson(MissingSampleBones));
	OutData->SetArrayField(TEXT("errors_after_solve"), UeAgentIKAssetOps::TextArrayToJson(Processor.Log.GetErrors()));
	OutData->SetArrayField(TEXT("warnings_after_solve"), UeAgentIKAssetOps::TextArrayToJson(Processor.Log.GetWarnings()));
	OutData->SetArrayField(TEXT("messages_after_solve"), UeAgentIKAssetOps::TextArrayToJson(Processor.Log.GetMessages()));
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRetargeterCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UIKRetargetFactory* Factory = NewObject<UIKRetargetFactory>();
	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(UeAgentIKAssetOps::CreateAssetWithFactory(AssetPath, UIKRetargeter::StaticClass(), Factory, OutError));
	if (!Retargeter)
	{
		return false;
	}

	UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	if (!Controller)
	{
		OutError = TEXT("ik_retargeter_controller_not_available");
		return false;
	}

	FString SourceIKRigPath;
	FString TargetIKRigPath;
	JsonTryGetString(Ctx.Params, TEXT("source_ik_rig"), SourceIKRigPath);
	JsonTryGetString(Ctx.Params, TEXT("target_ik_rig"), TargetIKRigPath);
	if (!SourceIKRigPath.TrimStartAndEnd().IsEmpty())
	{
		UIKRigDefinition* SourceIKRig = UeAgentIKAssetOps::LoadIKRig(SourceIKRigPath);
		if (!SourceIKRig)
		{
			OutError = TEXT("source_ik_rig_not_found");
			return false;
		}
		Controller->SetIKRig(ERetargetSourceOrTarget::Source, SourceIKRig);
	}
	if (!TargetIKRigPath.TrimStartAndEnd().IsEmpty())
	{
		UIKRigDefinition* TargetIKRig = UeAgentIKAssetOps::LoadIKRig(TargetIKRigPath);
		if (!TargetIKRig)
		{
			OutError = TEXT("target_ik_rig_not_found");
			return false;
		}
		Controller->SetIKRig(ERetargetSourceOrTarget::Target, TargetIKRig);
	}

	FString SourcePreviewMeshPath;
	FString TargetPreviewMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("source_preview_mesh"), SourcePreviewMeshPath);
	JsonTryGetString(Ctx.Params, TEXT("target_preview_mesh"), TargetPreviewMeshPath);
	if (!SourcePreviewMeshPath.TrimStartAndEnd().IsEmpty())
	{
		USkeletalMesh* SourcePreviewMesh = UeAgentIKAssetOps::LoadSkeletalMesh(SourcePreviewMeshPath);
		if (!SourcePreviewMesh)
		{
			OutError = TEXT("source_preview_mesh_not_found");
			return false;
		}
		Controller->SetPreviewMesh(ERetargetSourceOrTarget::Source, SourcePreviewMesh);
	}
	if (!TargetPreviewMeshPath.TrimStartAndEnd().IsEmpty())
	{
		USkeletalMesh* TargetPreviewMesh = UeAgentIKAssetOps::LoadSkeletalMesh(TargetPreviewMeshPath);
		if (!TargetPreviewMesh)
		{
			OutError = TEXT("target_preview_mesh_not_found");
			return false;
		}
		Controller->SetPreviewMesh(ERetargetSourceOrTarget::Target, TargetPreviewMesh);
	}

	bool bAddDefaultOps = true;
	JsonTryGetBool(Ctx.Params, TEXT("add_default_ops"), bAddDefaultOps);
	if (bAddDefaultOps)
	{
		Controller->AddDefaultOps();
	}
	Controller->CleanAsset();

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentIKAssetOps::SaveAssetPackage(Retargeter, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentIKAssetOps::NormalizeAssetPath(Retargeter->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Retargeter->GetPathName());
	OutData->SetStringField(TEXT("source_ik_rig"), Retargeter->GetIKRig(ERetargetSourceOrTarget::Source) ? Retargeter->GetIKRig(ERetargetSourceOrTarget::Source)->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("target_ik_rig"), Retargeter->GetIKRig(ERetargetSourceOrTarget::Target) ? Retargeter->GetIKRig(ERetargetSourceOrTarget::Target)->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("source_preview_mesh"), Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Source) ? Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Source)->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("target_preview_mesh"), Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Target) ? Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Target)->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRetargeterGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UIKRetargeter* Retargeter = UeAgentIKAssetOps::LoadIKRetargeter(AssetPath);
	if (!Retargeter)
	{
		OutError = TEXT("ik_retargeter_not_found");
		return false;
	}

	UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	if (!Controller)
	{
		OutError = TEXT("ik_retargeter_controller_not_available");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> ChainMappings;
	TSet<FString> SeenPairKeys;
	for (int32 OpIndex = 0; OpIndex < Controller->GetNumRetargetOps(); ++OpIndex)
	{
		const FName OpName = Controller->GetOpName(OpIndex);
		const FRetargetChainMapping* Mapping = Controller->GetChainMapping(OpName);
		if (!Mapping)
		{
			continue;
		}

		for (const FRetargetChainPair& Pair : Mapping->GetChainPairs())
		{
			const FString PairKey = Pair.TargetChainName.ToString() + TEXT("->") + Pair.SourceChainName.ToString() + TEXT("@") + OpName.ToString();
			if (SeenPairKeys.Contains(PairKey))
			{
				continue;
			}
			SeenPairKeys.Add(PairKey);

			TSharedPtr<FJsonObject> PairObject = UeAgentIKAssetOps::BuildChainPairJson(Pair);
			PairObject->SetStringField(TEXT("op_name"), OpName.ToString());
			ChainMappings.Add(MakeShared<FJsonValueObject>(PairObject));
		}
	}

	TArray<TSharedPtr<FJsonValue>> SourcePoseNames;
	for (const TPair<FName, FIKRetargetPose>& Pair : Controller->GetRetargetPoses(ERetargetSourceOrTarget::Source))
	{
		SourcePoseNames.Add(MakeShared<FJsonValueString>(Pair.Key.ToString()));
	}

	TArray<TSharedPtr<FJsonValue>> TargetPoseNames;
	for (const TPair<FName, FIKRetargetPose>& Pair : Controller->GetRetargetPoses(ERetargetSourceOrTarget::Target))
	{
		TargetPoseNames.Add(MakeShared<FJsonValueString>(Pair.Key.ToString()));
	}

	const FIKRetargetPose& CurrentSourcePose = Controller->GetCurrentRetargetPose(ERetargetSourceOrTarget::Source);
	const FIKRetargetPose& CurrentTargetPose = Controller->GetCurrentRetargetPose(ERetargetSourceOrTarget::Target);

	FTargetRootSettings RootSettings;
	FRetargetGlobalSettings GlobalSettings;
	TArray<const URetargetChainSettings*> AllChainSettings;
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	RootSettings = Controller->GetRootSettings();
	GlobalSettings = Controller->GetGlobalSettings();
	for (const URetargetChainSettings* ChainSettings : Controller->GetAllChainSettings())
	{
		if (ChainSettings)
		{
			AllChainSettings.Add(ChainSettings);
		}
	}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

	TArray<TSharedPtr<FJsonValue>> ChainSettingsArray;
	for (const URetargetChainSettings* ChainSettings : AllChainSettings)
	{
		ChainSettingsArray.Add(MakeShared<FJsonValueObject>(UeAgentIKAssetOps::BuildChainSettingsEntryJson(ChainSettings)));
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentIKAssetOps::NormalizeAssetPath(Retargeter->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Retargeter->GetPathName());
	OutData->SetStringField(TEXT("source_ik_rig"), Retargeter->GetIKRig(ERetargetSourceOrTarget::Source) ? Retargeter->GetIKRig(ERetargetSourceOrTarget::Source)->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("target_ik_rig"), Retargeter->GetIKRig(ERetargetSourceOrTarget::Target) ? Retargeter->GetIKRig(ERetargetSourceOrTarget::Target)->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("source_preview_mesh"), Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Source) ? Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Source)->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("target_preview_mesh"), Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Target) ? Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Target)->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("source_retarget_pose"), Controller->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Source).ToString());
	OutData->SetStringField(TEXT("target_retarget_pose"), Controller->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target).ToString());
	OutData->SetArrayField(TEXT("source_pose_names"), SourcePoseNames);
	OutData->SetArrayField(TEXT("target_pose_names"), TargetPoseNames);
	OutData->SetObjectField(TEXT("current_source_pose"), UeAgentIKAssetOps::BuildRetargetPoseJson(CurrentSourcePose));
	OutData->SetObjectField(TEXT("current_target_pose"), UeAgentIKAssetOps::BuildRetargetPoseJson(CurrentTargetPose));
	OutData->SetObjectField(TEXT("root_settings"), UeAgentIKAssetOps::BuildRetargetRootSettingsJson(RootSettings));
	OutData->SetObjectField(TEXT("global_settings"), UeAgentIKAssetOps::BuildRetargetGlobalSettingsJson(GlobalSettings));
	OutData->SetNumberField(TEXT("retarget_op_count"), Controller->GetNumRetargetOps());
	OutData->SetBoolField(TEXT("chain_mapping_ready"), ChainMappings.Num() > 0);
	OutData->SetNumberField(TEXT("chain_mapping_count"), ChainMappings.Num());
	OutData->SetArrayField(TEXT("chain_mappings"), ChainMappings);
	OutData->SetNumberField(TEXT("chain_settings_count"), ChainSettingsArray.Num());
	OutData->SetArrayField(TEXT("chain_settings"), ChainSettingsArray);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRetargeterSetIKRig(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SideText;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_or_target"), SideText) || SideText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_source_or_target");
		return false;
	}

	ERetargetSourceOrTarget Side = ERetargetSourceOrTarget::Source;
	if (!UeAgentIKAssetOps::ParseSourceOrTarget(SideText, Side))
	{
		OutError = TEXT("invalid_source_or_target");
		return false;
	}

	UIKRetargeter* Retargeter = UeAgentIKAssetOps::LoadIKRetargeter(AssetPath);
	if (!Retargeter)
	{
		OutError = TEXT("ik_retargeter_not_found");
		return false;
	}

	UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	if (!Controller)
	{
		OutError = TEXT("ik_retargeter_controller_not_available");
		return false;
	}

	bool bClear = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_ik_rig"), bClear);

	UIKRigDefinition* IKRig = nullptr;
	if (!bClear)
	{
		FString IKRigPath;
		if (!JsonTryGetString(Ctx.Params, TEXT("ik_rig_path"), IKRigPath) || IKRigPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_ik_rig_path");
			return false;
		}

		IKRig = UeAgentIKAssetOps::LoadIKRig(IKRigPath);
		if (!IKRig)
		{
			OutError = TEXT("ik_rig_not_found");
			return false;
		}
	}

	Controller->SetIKRig(Side, IKRig);
	Controller->CleanAsset();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentIKAssetOps::SaveAssetPackage(Retargeter, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
	OutData->SetStringField(TEXT("source_or_target"), UeAgentIKAssetOps::SourceOrTargetToString(Side));
	OutData->SetStringField(TEXT("ik_rig"), Retargeter->GetIKRig(Side) ? Retargeter->GetIKRig(Side)->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRetargeterSetSettings(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UIKRetargeter* Retargeter = UeAgentIKAssetOps::LoadIKRetargeter(AssetPath);
	if (!Retargeter)
	{
		OutError = TEXT("ik_retargeter_not_found");
		return false;
	}

	UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	if (!Controller)
	{
		OutError = TEXT("ik_retargeter_controller_not_available");
		return false;
	}

	bool bTouched = false;

	const TSharedPtr<FJsonObject>* GlobalSettingsObject = nullptr;
	if (Ctx.Params->TryGetObjectField(TEXT("global_settings"), GlobalSettingsObject) && GlobalSettingsObject && GlobalSettingsObject->IsValid())
	{
		FRetargetGlobalSettings GlobalSettings;
PRAGMA_DISABLE_DEPRECATION_WARNINGS
		GlobalSettings = Controller->GetGlobalSettings();
PRAGMA_ENABLE_DEPRECATION_WARNINGS
		if (!UeAgentIKAssetOps::ApplyRetargetGlobalSettingsPatch(*GlobalSettingsObject, GlobalSettings, OutError))
		{
			return false;
		}
PRAGMA_DISABLE_DEPRECATION_WARNINGS
		Controller->SetGlobalSettings(GlobalSettings);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
		bTouched = true;
	}

	const TSharedPtr<FJsonObject>* RootSettingsObject = nullptr;
	if (Ctx.Params->TryGetObjectField(TEXT("root_settings"), RootSettingsObject) && RootSettingsObject && RootSettingsObject->IsValid())
	{
		FTargetRootSettings RootSettings;
PRAGMA_DISABLE_DEPRECATION_WARNINGS
		RootSettings = Controller->GetRootSettings();
PRAGMA_ENABLE_DEPRECATION_WARNINGS
		if (!UeAgentIKAssetOps::ApplyRetargetRootSettingsPatch(*RootSettingsObject, RootSettings, OutError))
		{
			return false;
		}
PRAGMA_DISABLE_DEPRECATION_WARNINGS
		Controller->SetRootSettings(RootSettings);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
		bTouched = true;
	}

	FString TargetChainNameText;
	JsonTryGetString(Ctx.Params, TEXT("target_chain_name"), TargetChainNameText);
	TargetChainNameText = TargetChainNameText.TrimStartAndEnd();
	const FName TargetChainName = TargetChainNameText.IsEmpty() ? NAME_None : FName(*TargetChainNameText);

	bool bResetChainToDefault = false;
	JsonTryGetBool(Ctx.Params, TEXT("reset_chain_to_default"), bResetChainToDefault);

	const TSharedPtr<FJsonObject>* ChainSettingsObject = nullptr;
	const bool bHasChainSettings = Ctx.Params->TryGetObjectField(TEXT("chain_settings"), ChainSettingsObject) && ChainSettingsObject && ChainSettingsObject->IsValid();
	if (bResetChainToDefault || bHasChainSettings)
	{
		if (TargetChainName.IsNone())
		{
			OutError = TEXT("missing_target_chain_name");
			return false;
		}

		const URetargetChainSettings* ExistingChainSettings = nullptr;
PRAGMA_DISABLE_DEPRECATION_WARNINGS
		ExistingChainSettings = Controller->GetChainSettings(TargetChainName);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
		if (!ExistingChainSettings)
		{
			OutError = TEXT("retarget_chain_settings_not_found");
			return false;
		}

		FName MappingOpName = NAME_None;
		UeAgentIKAssetOps::ResolveChainMappingOpName(Controller, TargetChainName, MappingOpName);

		if (bResetChainToDefault)
		{
			Controller->ResetChainSettingsToDefault(TargetChainName, MappingOpName);
			bTouched = true;
		}

		if (bHasChainSettings)
		{
			FTargetChainSettings ChainSettings;
PRAGMA_DISABLE_DEPRECATION_WARNINGS
			ChainSettings = Controller->GetRetargetChainSettings(TargetChainName);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
			if (!UeAgentIKAssetOps::ApplyRetargetChainSettingsPatch(*ChainSettingsObject, ChainSettings, OutError))
			{
				return false;
			}
PRAGMA_DISABLE_DEPRECATION_WARNINGS
			if (!Controller->SetRetargetChainSettings(TargetChainName, ChainSettings))
PRAGMA_ENABLE_DEPRECATION_WARNINGS
			{
				OutError = TEXT("set_retarget_chain_settings_failed");
				return false;
			}
			bTouched = true;
		}
	}

	if (!bTouched)
	{
		OutError = TEXT("no_settings_requested");
		return false;
	}

	Controller->CleanAsset();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentIKAssetOps::SaveAssetPackage(Retargeter, OutError))
	{
		return false;
	}

	return CmdIKRetargeterGetInfo(Ctx, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdIKRetargeterSetPose(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SideText;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_or_target"), SideText) || SideText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_source_or_target");
		return false;
	}

	ERetargetSourceOrTarget Side = ERetargetSourceOrTarget::Source;
	if (!UeAgentIKAssetOps::ParseSourceOrTarget(SideText, Side))
	{
		OutError = TEXT("invalid_source_or_target");
		return false;
	}

	UIKRetargeter* Retargeter = UeAgentIKAssetOps::LoadIKRetargeter(AssetPath);
	if (!Retargeter)
	{
		OutError = TEXT("ik_retargeter_not_found");
		return false;
	}

	UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	if (!Controller)
	{
		OutError = TEXT("ik_retargeter_controller_not_available");
		return false;
	}

	FString PoseNameText;
	JsonTryGetString(Ctx.Params, TEXT("pose_name"), PoseNameText);
	PoseNameText = PoseNameText.TrimStartAndEnd();
	FName PoseName = PoseNameText.IsEmpty() ? NAME_None : FName(*PoseNameText);

	FString DuplicateFromPoseText;
	JsonTryGetString(Ctx.Params, TEXT("duplicate_from_pose"), DuplicateFromPoseText);
	DuplicateFromPoseText = DuplicateFromPoseText.TrimStartAndEnd();

	FString RenameFromPoseText;
	JsonTryGetString(Ctx.Params, TEXT("rename_from_pose"), RenameFromPoseText);
	RenameFromPoseText = RenameFromPoseText.TrimStartAndEnd();

	bool bRemove = false;
	bool bCreateIfMissing = false;
	bool bSetCurrent = false;
	bool bResetAll = false;
	JsonTryGetBool(Ctx.Params, TEXT("remove"), bRemove);
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	JsonTryGetBool(Ctx.Params, TEXT("set_current"), bSetCurrent);
	JsonTryGetBool(Ctx.Params, TEXT("reset_all"), bResetAll);

	const bool bNeedsCurrentPoseEdits =
		Ctx.Params->HasField(TEXT("root_offset")) ||
		Ctx.Params->HasField(TEXT("bone_rotation_offsets")) ||
		Ctx.Params->HasField(TEXT("auto_align_all")) ||
		Ctx.Params->HasField(TEXT("auto_align_bones")) ||
		Ctx.Params->HasField(TEXT("snap_bone_to_ground")) ||
		Ctx.Params->HasField(TEXT("reset_bones")) ||
		bResetAll;

	if (bRemove)
	{
		if (PoseName.IsNone())
		{
			OutError = TEXT("missing_pose_name");
			return false;
		}
		if (!Controller->RemoveRetargetPose(PoseName, Side))
		{
			OutError = TEXT("remove_pose_failed");
			return false;
		}
	}
	else
	{
		if (!DuplicateFromPoseText.IsEmpty())
		{
			if (PoseName.IsNone())
			{
				OutError = TEXT("missing_pose_name");
				return false;
			}
			const FName CreatedPose = Controller->DuplicateRetargetPose(FName(*DuplicateFromPoseText), PoseName, Side);
			if (CreatedPose.IsNone())
			{
				OutError = TEXT("duplicate_pose_failed");
				return false;
			}
			PoseName = CreatedPose;
			PoseNameText = CreatedPose.ToString();
		}

		if (!RenameFromPoseText.IsEmpty())
		{
			if (PoseName.IsNone())
			{
				OutError = TEXT("missing_pose_name");
				return false;
			}
			if (!Controller->RenameRetargetPose(FName(*RenameFromPoseText), PoseName, Side))
			{
				OutError = TEXT("rename_pose_failed");
				return false;
			}
		}

		if (!PoseName.IsNone() && (bCreateIfMissing || bSetCurrent || bNeedsCurrentPoseEdits))
		{
			TMap<FName, FIKRetargetPose>& Poses = Controller->GetRetargetPoses(Side);
			if (!Poses.Contains(PoseName) && bCreateIfMissing)
			{
				const FName CreatedPose = Controller->CreateRetargetPose(PoseName, Side);
				if (CreatedPose.IsNone())
				{
					OutError = TEXT("create_pose_failed");
					return false;
				}
				PoseName = CreatedPose;
				PoseNameText = CreatedPose.ToString();
			}

			if (!Poses.Contains(PoseName))
			{
				OutError = TEXT("pose_not_found");
				return false;
			}

			if ((bSetCurrent || bNeedsCurrentPoseEdits) && !Controller->SetCurrentRetargetPose(PoseName, Side))
			{
				OutError = TEXT("set_current_pose_failed");
				return false;
			}
		}

		if (bResetAll)
		{
			const FName TargetPoseName = !PoseName.IsNone() ? PoseName : Controller->GetCurrentRetargetPoseName(Side);
			Controller->ResetRetargetPose(TargetPoseName, TArray<FName>(), Side);
		}

		const TArray<TSharedPtr<FJsonValue>>* ResetBones = nullptr;
		if (Ctx.Params->TryGetArrayField(TEXT("reset_bones"), ResetBones) && ResetBones)
		{
			TArray<FName> BonesToReset;
			for (const TSharedPtr<FJsonValue>& Value : *ResetBones)
			{
				if (!Value.IsValid())
				{
					continue;
				}
				const FString BoneNameText = Value->AsString().TrimStartAndEnd();
				if (!BoneNameText.IsEmpty())
				{
					BonesToReset.Add(FName(*BoneNameText));
				}
			}
			const FName TargetPoseName = !PoseName.IsNone() ? PoseName : Controller->GetCurrentRetargetPoseName(Side);
			if (BonesToReset.Num() > 0)
			{
				Controller->ResetRetargetPose(TargetPoseName, BonesToReset, Side);
			}
		}

		FVector RootOffset = FVector::ZeroVector;
		if (JsonTryGetVector(Ctx.Params, TEXT("root_offset"), RootOffset))
		{
			Controller->SetRootOffsetInRetargetPose(RootOffset, Side);
		}

		const TArray<TSharedPtr<FJsonValue>>* BoneRotationOffsets = nullptr;
		if (Ctx.Params->TryGetArrayField(TEXT("bone_rotation_offsets"), BoneRotationOffsets) && BoneRotationOffsets)
		{
			for (const TSharedPtr<FJsonValue>& Value : *BoneRotationOffsets)
			{
				const TSharedPtr<FJsonObject>* BoneObject = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(BoneObject) || !BoneObject || !BoneObject->IsValid())
				{
					continue;
				}

				FString BoneNameText;
				if (!(*BoneObject)->TryGetStringField(TEXT("bone_name"), BoneNameText) || BoneNameText.TrimStartAndEnd().IsEmpty())
				{
					OutError = TEXT("invalid_bone_rotation_offset_entry");
					return false;
				}

				FRotator RotationOffset = FRotator::ZeroRotator;
				bool bHasRotation = JsonTryGetRotator(*BoneObject, TEXT("rotation_offset"), RotationOffset);
				if (!bHasRotation)
				{
					RotationOffset = FRotator::ZeroRotator;
					const bool bHasPitch = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*BoneObject, { TEXT("pitch"), TEXT("Pitch") }, RotationOffset.Pitch);
					const bool bHasYaw = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*BoneObject, { TEXT("yaw"), TEXT("Yaw") }, RotationOffset.Yaw);
					const bool bHasRoll = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*BoneObject, { TEXT("roll"), TEXT("Roll") }, RotationOffset.Roll);
					bHasRotation = bHasPitch || bHasYaw || bHasRoll;
				}
				if (!bHasRotation)
				{
					OutError = TEXT("missing_rotation_offset");
					return false;
				}

				Controller->SetRotationOffsetForRetargetPoseBone(FName(*BoneNameText.TrimStartAndEnd()), RotationOffset.Quaternion(), Side);
			}
		}

		bool bAutoAlignAll = false;
		if (JsonTryGetBool(Ctx.Params, TEXT("auto_align_all"), bAutoAlignAll) && bAutoAlignAll)
		{
			FString AutoAlignMethodText;
			JsonTryGetString(Ctx.Params, TEXT("auto_align_method"), AutoAlignMethodText);
			ERetargetAutoAlignMethod AutoAlignMethod = ERetargetAutoAlignMethod::ChainToChain;
			if (!UeAgentIKAssetOps::ParseAutoAlignMethod(AutoAlignMethodText, AutoAlignMethod))
			{
				OutError = TEXT("invalid_auto_align_method");
				return false;
			}
			Controller->AutoAlignAllBones(Side, AutoAlignMethod);
		}

		const TArray<TSharedPtr<FJsonValue>>* AutoAlignBones = nullptr;
		if (Ctx.Params->TryGetArrayField(TEXT("auto_align_bones"), AutoAlignBones) && AutoAlignBones && AutoAlignBones->Num() > 0)
		{
			TArray<FName> BoneNames;
			for (const TSharedPtr<FJsonValue>& Value : *AutoAlignBones)
			{
				if (!Value.IsValid())
				{
					continue;
				}
				const FString BoneNameText = Value->AsString().TrimStartAndEnd();
				if (!BoneNameText.IsEmpty())
				{
					BoneNames.Add(FName(*BoneNameText));
				}
			}

			FString AutoAlignMethodText;
			JsonTryGetString(Ctx.Params, TEXT("auto_align_method"), AutoAlignMethodText);
			ERetargetAutoAlignMethod AutoAlignMethod = ERetargetAutoAlignMethod::ChainToChain;
			if (!UeAgentIKAssetOps::ParseAutoAlignMethod(AutoAlignMethodText, AutoAlignMethod))
			{
				OutError = TEXT("invalid_auto_align_method");
				return false;
			}
			Controller->AutoAlignBones(BoneNames, AutoAlignMethod, Side);
		}

		FString SnapBoneToGroundText;
		if (JsonTryGetString(Ctx.Params, TEXT("snap_bone_to_ground"), SnapBoneToGroundText) && !SnapBoneToGroundText.TrimStartAndEnd().IsEmpty())
		{
			Controller->SnapBoneToGround(FName(*SnapBoneToGroundText.TrimStartAndEnd()), Side);
		}
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentIKAssetOps::SaveAssetPackage(Retargeter, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> PoseNames;
	for (const TPair<FName, FIKRetargetPose>& Pair : Controller->GetRetargetPoses(Side))
	{
		PoseNames.Add(MakeShared<FJsonValueString>(Pair.Key.ToString()));
	}

	const FName CurrentPoseName = Controller->GetCurrentRetargetPoseName(Side);
	const FIKRetargetPose& CurrentPose = Controller->GetCurrentRetargetPose(Side);
	OutData->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
	OutData->SetStringField(TEXT("source_or_target"), UeAgentIKAssetOps::SourceOrTargetToString(Side));
	OutData->SetStringField(TEXT("current_pose"), CurrentPoseName.ToString());
	OutData->SetArrayField(TEXT("pose_names"), PoseNames);
	OutData->SetObjectField(TEXT("current_pose_data"), UeAgentIKAssetOps::BuildRetargetPoseJson(CurrentPose));
	OutData->SetBoolField(TEXT("removed"), bRemove);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRetargeterAutoAlignPose(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SideText;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_or_target"), SideText) || SideText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_source_or_target");
		return false;
	}

	FString AutoAlignMethodText;
	JsonTryGetString(Ctx.Params, TEXT("auto_align_method"), AutoAlignMethodText);
	ERetargetAutoAlignMethod AutoAlignMethod = ERetargetAutoAlignMethod::ChainToChain;
	if (!UeAgentIKAssetOps::ParseAutoAlignMethod(AutoAlignMethodText, AutoAlignMethod))
	{
		OutError = TEXT("invalid_auto_align_method");
		return false;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Ctx.Params->Values)
	{
		Params->SetField(Pair.Key, Pair.Value);
	}

	const TSharedPtr<FJsonValue>* BonesValue = Ctx.Params->Values.Find(TEXT("bones"));
	if (BonesValue && BonesValue->IsValid() && !Params->HasField(TEXT("auto_align_bones")))
	{
		Params->SetField(TEXT("auto_align_bones"), *BonesValue);
	}

	const bool bHasAutoAlignBones = Params->HasTypedField<EJson::Array>(TEXT("auto_align_bones"));
	const bool bHasExplicitAction =
		Params->HasField(TEXT("auto_align_all")) ||
		bHasAutoAlignBones ||
		Params->HasField(TEXT("snap_bone_to_ground")) ||
		Params->HasField(TEXT("reset_all")) ||
		Params->HasField(TEXT("reset_bones")) ||
		Params->HasField(TEXT("root_offset"));
	if (!bHasExplicitAction)
	{
		Params->SetBoolField(TEXT("auto_align_all"), true);
	}

	FUeAgentRequestContext SubCtx = Ctx;
	SubCtx.Command = TEXT("ik_retargeter_set_pose");
	SubCtx.Params = Params;
	const bool bOk = CmdIKRetargeterSetPose(SubCtx, OutData, OutError);
	if (!bOk)
	{
		return false;
	}

	int32 AutoAlignBoneCount = 0;
	const TArray<TSharedPtr<FJsonValue>>* AutoAlignBones = nullptr;
	if (Params->TryGetArrayField(TEXT("auto_align_bones"), AutoAlignBones) && AutoAlignBones)
	{
		AutoAlignBoneCount = AutoAlignBones->Num();
	}

	bool bAutoAlignAll = false;
	JsonTryGetBool(Params, TEXT("auto_align_all"), bAutoAlignAll);

	OutData->SetStringField(TEXT("action"), TEXT("auto_align_pose"));
	OutData->SetStringField(TEXT("auto_align_method"), UeAgentIKAssetOps::AutoAlignMethodToString(AutoAlignMethod));
	OutData->SetBoolField(TEXT("used_auto_align_all"), bAutoAlignAll);
	OutData->SetNumberField(TEXT("used_auto_align_bones_count"), AutoAlignBoneCount);
	OutData->SetBoolField(TEXT("used_bones_alias"), BonesValue != nullptr);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRetargeterSetPreviewMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SideText;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_or_target"), SideText) || SideText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_source_or_target");
		return false;
	}

	ERetargetSourceOrTarget Side = ERetargetSourceOrTarget::Source;
	if (!UeAgentIKAssetOps::ParseSourceOrTarget(SideText, Side))
	{
		OutError = TEXT("invalid_source_or_target");
		return false;
	}

	UIKRetargeter* Retargeter = UeAgentIKAssetOps::LoadIKRetargeter(AssetPath);
	if (!Retargeter)
	{
		OutError = TEXT("ik_retargeter_not_found");
		return false;
	}

	UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	if (!Controller)
	{
		OutError = TEXT("ik_retargeter_controller_not_available");
		return false;
	}

	bool bClear = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_preview_mesh"), bClear);

	USkeletalMesh* PreviewMesh = nullptr;
	if (!bClear)
	{
		FString PreviewMeshPath;
		if (!JsonTryGetString(Ctx.Params, TEXT("skeletal_mesh_path"), PreviewMeshPath) || PreviewMeshPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_skeletal_mesh_path");
			return false;
		}
		PreviewMesh = UeAgentIKAssetOps::LoadSkeletalMesh(PreviewMeshPath);
		if (!PreviewMesh)
		{
			OutError = TEXT("skeletal_mesh_not_found");
			return false;
		}
	}

	Controller->SetPreviewMesh(Side, PreviewMesh);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentIKAssetOps::SaveAssetPackage(Retargeter, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
	OutData->SetStringField(TEXT("source_or_target"), UeAgentIKAssetOps::SourceOrTargetToString(Side));
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), Retargeter->GetPreviewMesh(Side) ? Retargeter->GetPreviewMesh(Side)->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRetargeterAutoMapChains(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UIKRetargeter* Retargeter = UeAgentIKAssetOps::LoadIKRetargeter(AssetPath);
	if (!Retargeter)
	{
		OutError = TEXT("ik_retargeter_not_found");
		return false;
	}

	UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	if (!Controller)
	{
		OutError = TEXT("ik_retargeter_controller_not_available");
		return false;
	}
	Controller->CleanAsset();

	FString AutoMapTypeText = TEXT("exact");
	JsonTryGetString(Ctx.Params, TEXT("auto_map_type"), AutoMapTypeText);
	EAutoMapChainType AutoMapType = EAutoMapChainType::Exact;
	if (!UeAgentIKAssetOps::ParseAutoMapType(AutoMapTypeText, AutoMapType))
	{
		OutError = TEXT("invalid_auto_map_type");
		return false;
	}

	bool bForceRemap = false;
	JsonTryGetBool(Ctx.Params, TEXT("force_remap"), bForceRemap);

	FString OpNameText;
	JsonTryGetString(Ctx.Params, TEXT("op_name"), OpNameText);
	Controller->AutoMapChains(AutoMapType, bForceRemap, OpNameText.TrimStartAndEnd().IsEmpty() ? NAME_None : FName(*OpNameText.TrimStartAndEnd()));

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentIKAssetOps::SaveAssetPackage(Retargeter, OutError))
	{
		return false;
	}

	const FRetargetChainMapping* ChainMapping = Controller->GetChainMapping(NAME_None);
	OutData->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
	OutData->SetStringField(TEXT("auto_map_type"), AutoMapTypeText);
	OutData->SetNumberField(TEXT("chain_mapping_count"), ChainMapping ? ChainMapping->GetChainPairs().Num() : 0);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdIKRetargeterDuplicateAndRetarget(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UIKRetargeter* Retargeter = UeAgentIKAssetOps::LoadIKRetargeter(AssetPath);
	if (!Retargeter)
	{
		OutError = TEXT("ik_retargeter_not_found");
		return false;
	}

	TArray<TWeakObjectPtr<UObject>> AssetsToRetarget;
	const TArray<TSharedPtr<FJsonValue>>* AssetPaths = nullptr;
	if (!Ctx.Params->TryGetArrayField(TEXT("asset_paths"), AssetPaths) || !AssetPaths || AssetPaths->Num() <= 0)
	{
		OutError = TEXT("missing_asset_paths");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *AssetPaths)
	{
		if (!Value.IsValid())
		{
			continue;
		}
		const FString SourceAssetPath = Value->AsString().TrimStartAndEnd();
		if (SourceAssetPath.IsEmpty())
		{
			continue;
		}
		UObject* AssetObject = UeAgentIKAssetOps::LoadAssetObject(SourceAssetPath);
		if (!AssetObject)
		{
			OutError = TEXT("retarget_source_asset_not_found");
			return false;
		}
		AssetsToRetarget.Add(AssetObject);
	}
	if (AssetsToRetarget.Num() <= 0)
	{
		OutError = TEXT("missing_asset_paths");
		return false;
	}

	UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	if (!Controller)
	{
		OutError = TEXT("ik_retargeter_controller_not_available");
		return false;
	}

	USkeletalMesh* SourceMesh = nullptr;
	USkeletalMesh* TargetMesh = nullptr;
	FString SourceMeshPath;
	FString TargetMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("source_mesh_path"), SourceMeshPath);
	JsonTryGetString(Ctx.Params, TEXT("target_mesh_path"), TargetMeshPath);

	if (!SourceMeshPath.TrimStartAndEnd().IsEmpty())
	{
		SourceMesh = UeAgentIKAssetOps::LoadSkeletalMesh(SourceMeshPath);
	}
	else
	{
		SourceMesh = Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Source);
	}

	if (!TargetMeshPath.TrimStartAndEnd().IsEmpty())
	{
		TargetMesh = UeAgentIKAssetOps::LoadSkeletalMesh(TargetMeshPath);
	}
	else
	{
		TargetMesh = Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Target);
	}

	if (!SourceMesh || !TargetMesh)
	{
		OutError = TEXT("missing_source_or_target_mesh");
		return false;
	}
	if (SourceMesh == TargetMesh)
	{
		OutError = TEXT("source_and_target_mesh_must_differ");
		return false;
	}

	FString OutputFolder;
	if (!JsonTryGetString(Ctx.Params, TEXT("output_folder"), OutputFolder) || OutputFolder.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_output_folder");
		return false;
	}
	OutputFolder = OutputFolder.TrimStartAndEnd();
	if (!FPackageName::IsValidLongPackageName(OutputFolder))
	{
		OutError = TEXT("invalid_output_folder");
		return false;
	}

	FString Prefix;
	FString Suffix;
	FString Search;
	FString Replace;
	JsonTryGetString(Ctx.Params, TEXT("prefix"), Prefix);
	JsonTryGetString(Ctx.Params, TEXT("suffix"), Suffix);
	JsonTryGetString(Ctx.Params, TEXT("search"), Search);
	JsonTryGetString(Ctx.Params, TEXT("replace"), Replace);

	bool bIncludeReferencedAssets = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_referenced_assets"), bIncludeReferencedAssets);

	TMap<FString, FAssetData> AssetsBefore;
	UeAgentIKAssetOps::GatherAssetsUnderPath(OutputFolder, AssetsBefore);

	UIKRetargetBatchOperation* BatchOperation = NewObject<UIKRetargetBatchOperation>();
	FIKRetargetBatchOperationContext BatchContext;
	BatchContext.AssetsToRetarget = AssetsToRetarget;
	BatchContext.SourceMesh = SourceMesh;
	BatchContext.TargetMesh = TargetMesh;
	BatchContext.IKRetargetAsset = Retargeter;
	BatchContext.bIncludeReferencedAssets = bIncludeReferencedAssets;
	BatchContext.NameRule.FolderPath = OutputFolder;
	BatchContext.NameRule.Prefix = Prefix;
	BatchContext.NameRule.Suffix = Suffix;
	BatchContext.NameRule.ReplaceFrom = Search;
	BatchContext.NameRule.ReplaceTo = Replace;
	BatchOperation->RunRetarget(BatchContext);

	TMap<FString, FAssetData> AssetsAfter;
	UeAgentIKAssetOps::GatherAssetsUnderPath(OutputFolder, AssetsAfter);

	TArray<TSharedPtr<FJsonValue>> CreatedAssets;
	for (const TPair<FString, FAssetData>& Pair : AssetsAfter)
	{
		if (AssetsBefore.Contains(Pair.Key))
		{
			continue;
		}

		TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
		AssetObject->SetStringField(TEXT("package_name"), Pair.Value.PackageName.ToString());
		AssetObject->SetStringField(TEXT("object_path"), Pair.Value.GetObjectPathString());
		AssetObject->SetStringField(TEXT("asset_name"), Pair.Value.AssetName.ToString());
		AssetObject->SetStringField(TEXT("asset_class"), Pair.Value.AssetClassPath.ToString());
		CreatedAssets.Add(MakeShared<FJsonValueObject>(AssetObject));
	}

	OutData->SetStringField(TEXT("asset_path"), Retargeter->GetPathName());
	OutData->SetStringField(TEXT("output_folder"), OutputFolder);
	OutData->SetNumberField(TEXT("source_asset_count"), AssetsToRetarget.Num());
	OutData->SetNumberField(TEXT("created_asset_count"), CreatedAssets.Num());
	OutData->SetArrayField(TEXT("created_assets"), CreatedAssets);
	return true;
}
