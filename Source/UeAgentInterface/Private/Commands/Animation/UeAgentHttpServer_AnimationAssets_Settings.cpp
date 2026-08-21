// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_AnimationAssets.h"

#include "Domains/Animation/UeAgentAnimationAssetDomainBackend.h"

#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Animation/AnimationAsset.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimLinkableElement.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
#include "Animation/SkeletalMeshActor.h"
#include "Animation/AnimEnums.h"
#include "Animation/AnimTypes.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/Skeleton.h"
#include "Animation/VariableFrameStrippingSettings.h"
#include "AnimationBlueprintLibrary.h"
#include "AnimPose.h"
#include "AnimPreviewInstance.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/SkeletalMeshSocket.h"
#include "FileHelpers.h"
#include "Misc/FileHelper.h"
#include "Framework/Application/SlateApplication.h"
#include "IAnimationEditor.h"
#include "IPersonaPreviewScene.h"
#include "IPersonaToolkit.h"
#include "Misc/PackageName.h"
#include "LevelEditorViewport.h"
#include "RenderingThread.h"
#include "SEditorViewport.h"
#include "Slate/SceneViewport.h"
#include "Slate/WidgetRenderer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "UeAgentInterfaceSettings.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UnrealType.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"

bool FUeAgentLegacyMechanics::CmdAnimSequenceSetSettings(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimSequence* Sequence = UeAgentAnimationAssetOps::LoadAnimSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("anim_sequence_not_found");
		return false;
	}

	Sequence->Modify();
	int32 SequenceNumFrames = 0;
	UAnimationBlueprintLibrary::GetNumFrames(Sequence, SequenceNumFrames);

	double RateScaleValue = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("rate_scale"), RateScaleValue))
	{
		UAnimationBlueprintLibrary::SetRateScale(Sequence, static_cast<float>(RateScaleValue));
	}

	bool bBoolValue = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("enable_root_motion"), bBoolValue))
	{
		UAnimationBlueprintLibrary::SetRootMotionEnabled(Sequence, bBoolValue);
	}
	if (JsonTryGetBool(Ctx.Params, TEXT("force_root_lock"), bBoolValue))
	{
		UAnimationBlueprintLibrary::SetIsRootMotionLockForced(Sequence, bBoolValue);
	}

	FString AdditiveAnimationTypeText;
	if (JsonTryGetString(Ctx.Params, TEXT("additive_animation_type"), AdditiveAnimationTypeText) && !AdditiveAnimationTypeText.TrimStartAndEnd().IsEmpty())
	{
		EAdditiveAnimationType AdditiveAnimationType = AAT_None;
		if (!UeAgentAnimationAssetOps::ParseAdditiveAnimationType(AdditiveAnimationTypeText, AdditiveAnimationType))
		{
			OutError = TEXT("invalid_additive_animation_type");
			return false;
		}
		UAnimationBlueprintLibrary::SetAdditiveAnimationType(Sequence, AdditiveAnimationType);
	}

	FString AdditiveBasePoseTypeText;
	if (JsonTryGetString(Ctx.Params, TEXT("additive_base_pose_type"), AdditiveBasePoseTypeText) && !AdditiveBasePoseTypeText.TrimStartAndEnd().IsEmpty())
	{
		EAdditiveBasePoseType AdditiveBasePoseType = ABPT_None;
		if (!UeAgentAnimationAssetOps::ParseAdditiveBasePoseType(AdditiveBasePoseTypeText, AdditiveBasePoseType))
		{
			OutError = TEXT("invalid_additive_base_pose_type");
			return false;
		}
		UAnimationBlueprintLibrary::SetAdditiveBasePoseType(Sequence, AdditiveBasePoseType);
	}

	FString RootMotionLockTypeText;
	if (JsonTryGetString(Ctx.Params, TEXT("root_motion_lock_type"), RootMotionLockTypeText) && !RootMotionLockTypeText.TrimStartAndEnd().IsEmpty())
	{
		ERootMotionRootLock::Type RootMotionLockType = ERootMotionRootLock::RefPose;
		if (!UeAgentAnimationAssetOps::ParseRootMotionLockType(RootMotionLockTypeText, RootMotionLockType))
		{
			OutError = TEXT("invalid_root_motion_lock_type");
			return false;
		}
		UAnimationBlueprintLibrary::SetRootMotionLockType(Sequence, RootMotionLockType);
	}

	FString InterpolationTypeText;
	if (JsonTryGetString(Ctx.Params, TEXT("interpolation_type"), InterpolationTypeText) && !InterpolationTypeText.TrimStartAndEnd().IsEmpty())
	{
		EAnimInterpolationType InterpolationType = EAnimInterpolationType::Linear;
		if (!UeAgentAnimationAssetOps::ParseInterpolationType(InterpolationTypeText, InterpolationType))
		{
			OutError = TEXT("invalid_interpolation_type");
			return false;
		}
		UAnimationBlueprintLibrary::SetAnimationInterpolationType(Sequence, InterpolationType);
	}

	FString BasePoseSequencePath;
	if (JsonTryGetString(Ctx.Params, TEXT("base_pose_sequence_path"), BasePoseSequencePath) && !BasePoseSequencePath.TrimStartAndEnd().IsEmpty())
	{
		UAnimSequence* BasePoseSequence = UeAgentAnimationAssetOps::LoadAnimSequence(BasePoseSequencePath);
		if (!BasePoseSequence)
		{
			OutError = TEXT("base_pose_sequence_not_found");
			return false;
		}
		if (Sequence->GetSkeleton() && BasePoseSequence->GetSkeleton() && !Sequence->GetSkeleton()->IsCompatibleForEditor(BasePoseSequence->GetSkeleton()))
		{
			OutError = TEXT("base_pose_sequence_skeleton_incompatible");
			return false;
		}
		Sequence->RefPoseSeq = BasePoseSequence;
		Sequence->MarkPackageDirty();
	}

	bool bClearBasePoseSequence = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("clear_base_pose_sequence"), bClearBasePoseSequence) && bClearBasePoseSequence)
	{
		Sequence->RefPoseSeq = nullptr;
		Sequence->MarkPackageDirty();
	}

	double BasePoseFrameIndexValue = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("base_pose_frame_index"), BasePoseFrameIndexValue))
	{
		const int32 BasePoseFrameIndex = static_cast<int32>(BasePoseFrameIndexValue);
		if (BasePoseFrameIndex < 0 || BasePoseFrameIndex >= SequenceNumFrames)
		{
			OutError = TEXT("invalid_base_pose_frame_index");
			return false;
		}
		Sequence->RefFrameIndex = BasePoseFrameIndex;
		Sequence->MarkPackageDirty();
	}

	if (Ctx.Params->HasField(TEXT("retarget_source")))
	{
		FString RetargetSourceText;
		JsonTryGetString(Ctx.Params, TEXT("retarget_source"), RetargetSourceText);
		Sequence->RetargetSource = RetargetSourceText.TrimStartAndEnd().IsEmpty() ? NAME_None : FName(*RetargetSourceText.TrimStartAndEnd());
		Sequence->MarkPackageDirty();
	}

	bool bClearRetargetSourceAsset = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("clear_retarget_source_asset"), bClearRetargetSourceAsset) && bClearRetargetSourceAsset)
	{
		Sequence->ClearRetargetSourceAsset();
		Sequence->MarkPackageDirty();
	}

	FString RetargetSourceAssetPath;
	if (JsonTryGetString(Ctx.Params, TEXT("retarget_source_asset_path"), RetargetSourceAssetPath) && !RetargetSourceAssetPath.TrimStartAndEnd().IsEmpty())
	{
		USkeletalMesh* RetargetSourceAsset = UeAgentAnimationAssetOps::LoadSkeletalMesh(RetargetSourceAssetPath);
		if (!RetargetSourceAsset)
		{
			OutError = TEXT("retarget_source_asset_not_found");
			return false;
		}
		if (Sequence->GetSkeleton() && RetargetSourceAsset->GetSkeleton() && !Sequence->GetSkeleton()->IsCompatibleForEditor(RetargetSourceAsset->GetSkeleton()))
		{
			OutError = TEXT("retarget_source_asset_skeleton_incompatible");
			return false;
		}
		Sequence->SetRetargetSourceAsset(RetargetSourceAsset);
		Sequence->MarkPackageDirty();
	}

	FString BoneCompressionSettingsPath;
	if (JsonTryGetString(Ctx.Params, TEXT("bone_compression_settings_path"), BoneCompressionSettingsPath) && !BoneCompressionSettingsPath.TrimStartAndEnd().IsEmpty())
	{
		UAnimBoneCompressionSettings* BoneCompressionSettings = UeAgentAnimationAssetOps::LoadBoneCompressionSettings(BoneCompressionSettingsPath);
		if (!BoneCompressionSettings)
		{
			OutError = TEXT("bone_compression_settings_not_found");
			return false;
		}
		if (!BoneCompressionSettings->AreSettingsValid())
		{
			OutError = TEXT("invalid_bone_compression_settings");
			return false;
		}
		UAnimationBlueprintLibrary::SetBoneCompressionSettings(Sequence, BoneCompressionSettings);
		Sequence->MarkPackageDirty();
	}

	FString CurveCompressionSettingsPath;
	if (JsonTryGetString(Ctx.Params, TEXT("curve_compression_settings_path"), CurveCompressionSettingsPath) && !CurveCompressionSettingsPath.TrimStartAndEnd().IsEmpty())
	{
		UAnimCurveCompressionSettings* CurveCompressionSettings = UeAgentAnimationAssetOps::LoadCurveCompressionSettings(CurveCompressionSettingsPath);
		if (!CurveCompressionSettings)
		{
			OutError = TEXT("curve_compression_settings_not_found");
			return false;
		}
		if (!CurveCompressionSettings->AreSettingsValid())
		{
			OutError = TEXT("invalid_curve_compression_settings");
			return false;
		}
		UAnimationBlueprintLibrary::SetCurveCompressionSettings(Sequence, CurveCompressionSettings);
		Sequence->MarkPackageDirty();
	}

	FString VariableFrameStrippingSettingsPath;
	if (JsonTryGetString(Ctx.Params, TEXT("variable_frame_stripping_settings_path"), VariableFrameStrippingSettingsPath) && !VariableFrameStrippingSettingsPath.TrimStartAndEnd().IsEmpty())
	{
		UVariableFrameStrippingSettings* VariableFrameStrippingSettings = UeAgentAnimationAssetOps::LoadVariableFrameStrippingSettings(VariableFrameStrippingSettingsPath);
		if (!VariableFrameStrippingSettings)
		{
			OutError = TEXT("variable_frame_stripping_settings_not_found");
			return false;
		}
		UAnimationBlueprintLibrary::SetVariableFrameStrippingSettings(Sequence, VariableFrameStrippingSettings);
		Sequence->MarkPackageDirty();
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Sequence, OutError))
	{
		return false;
	}

	float SequenceLength = 0.0f;
	float RateScale = 0.0f;
	TEnumAsByte<EAdditiveAnimationType> AdditiveAnimationType = AAT_None;
	TEnumAsByte<EAdditiveBasePoseType> AdditiveBasePoseType = ABPT_None;
	TEnumAsByte<ERootMotionRootLock::Type> RootMotionLockType = ERootMotionRootLock::RefPose;
	EAnimInterpolationType InterpolationType = EAnimInterpolationType::Linear;
	UAnimBoneCompressionSettings* BoneCompressionSettings = nullptr;
	UAnimCurveCompressionSettings* CurveCompressionSettings = nullptr;
	UVariableFrameStrippingSettings* VariableFrameStrippingSettings = nullptr;
	UAnimationBlueprintLibrary::GetSequenceLength(Sequence, SequenceLength);
	UAnimationBlueprintLibrary::GetRateScale(Sequence, RateScale);
	UAnimationBlueprintLibrary::GetAdditiveAnimationType(Sequence, AdditiveAnimationType);
	UAnimationBlueprintLibrary::GetAdditiveBasePoseType(Sequence, AdditiveBasePoseType);
	UAnimationBlueprintLibrary::GetRootMotionLockType(Sequence, RootMotionLockType);
	UAnimationBlueprintLibrary::GetAnimationInterpolationType(Sequence, InterpolationType);
	UAnimationBlueprintLibrary::GetBoneCompressionSettings(Sequence, BoneCompressionSettings);
	UAnimationBlueprintLibrary::GetCurveCompressionSettings(Sequence, CurveCompressionSettings);
	UAnimationBlueprintLibrary::GetVariableFrameStrippingSettings(Sequence, VariableFrameStrippingSettings);

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetNumberField(TEXT("sequence_length"), SequenceLength);
	OutData->SetNumberField(TEXT("rate_scale"), RateScale);
	OutData->SetStringField(TEXT("interpolation_type"), UeAgentAnimationAssetOps::InterpolationTypeToString(InterpolationType));
	OutData->SetStringField(TEXT("additive_animation_type"), UeAgentAnimationAssetOps::AdditiveAnimationTypeToString(AdditiveAnimationType));
	OutData->SetStringField(TEXT("additive_base_pose_type"), UeAgentAnimationAssetOps::AdditiveBasePoseTypeToString(AdditiveBasePoseType));
	OutData->SetStringField(TEXT("additive_base_pose_sequence"), Sequence->RefPoseSeq ? Sequence->RefPoseSeq->GetPathName() : TEXT(""));
	OutData->SetNumberField(TEXT("additive_base_pose_frame_index"), Sequence->RefFrameIndex);
	OutData->SetBoolField(TEXT("root_motion_enabled"), UAnimationBlueprintLibrary::IsRootMotionEnabled(Sequence));
	OutData->SetBoolField(TEXT("force_root_lock"), UAnimationBlueprintLibrary::IsRootMotionLockForced(Sequence));
	OutData->SetStringField(TEXT("root_motion_lock_type"), UeAgentAnimationAssetOps::RootMotionLockTypeToString(RootMotionLockType));
	OutData->SetStringField(TEXT("retarget_source"), Sequence->RetargetSource.ToString());
	OutData->SetStringField(TEXT("retarget_source_asset"), Sequence->GetRetargetSourceAsset().ToSoftObjectPath().ToString());
	OutData->SetStringField(TEXT("bone_compression_settings"), BoneCompressionSettings ? BoneCompressionSettings->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("curve_compression_settings"), CurveCompressionSettings ? CurveCompressionSettings->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("variable_frame_stripping_settings"), VariableFrameStrippingSettings ? VariableFrameStrippingSettings->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdAnimSequenceSetPreviewMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimSequence* Sequence = UeAgentAnimationAssetOps::LoadAnimSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("anim_sequence_not_found");
		return false;
	}

	bool bClearPreviewMesh = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_preview_mesh"), bClearPreviewMesh);

	FString PreviewMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("skeletal_mesh_path"), PreviewMeshPath);

	USkeletalMesh* PreviewMesh = nullptr;
	if (!bClearPreviewMesh)
	{
		if (PreviewMeshPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_skeletal_mesh_path");
			return false;
		}

		PreviewMesh = UeAgentAnimationAssetOps::LoadSkeletalMesh(PreviewMeshPath);
		if (!PreviewMesh)
		{
			OutError = TEXT("skeletal_mesh_not_found");
			return false;
		}
		if (Sequence->GetSkeleton() && PreviewMesh->GetSkeleton() && !Sequence->GetSkeleton()->IsCompatibleForEditor(PreviewMesh->GetSkeleton()))
		{
			OutError = TEXT("preview_mesh_incompatible_with_sequence_skeleton");
			return false;
		}
	}

	Sequence->Modify();
	Sequence->SetPreviewMesh(PreviewMesh, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Sequence, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), Sequence->GetPreviewMesh() ? Sequence->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdAnimSequenceSetCurve(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString CurveNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("curve_name"), CurveNameText) || CurveNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_curve_name");
		return false;
	}
	const FName CurveName(*CurveNameText.TrimStartAndEnd());

	FString CurveTypeText = TEXT("float");
	JsonTryGetString(Ctx.Params, TEXT("curve_type"), CurveTypeText);
	ERawCurveTrackTypes CurveType = ERawCurveTrackTypes::RCT_Float;
	if (!UeAgentAnimationAssetOps::ParseCurveType(CurveTypeText, CurveType))
	{
		OutError = TEXT("invalid_curve_type");
		return false;
	}

	UAnimSequence* Sequence = UeAgentAnimationAssetOps::LoadAnimSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("anim_sequence_not_found");
		return false;
	}

	bool bRemove = false;
	bool bMetaDataCurve = false;
	bool bClearExistingKeys = false;
	bool bRemoveNameFromSkeleton = false;
	JsonTryGetBool(Ctx.Params, TEXT("remove"), bRemove);
	JsonTryGetBool(Ctx.Params, TEXT("meta_data_curve"), bMetaDataCurve);
	JsonTryGetBool(Ctx.Params, TEXT("clear_existing_keys"), bClearExistingKeys);
	JsonTryGetBool(Ctx.Params, TEXT("remove_name_from_skeleton"), bRemoveNameFromSkeleton);

	TSharedPtr<FJsonObject> TypedCurveJson;
	const TSharedPtr<FJsonObject>* CurveJsonPtr = nullptr;
	bool bHasTypedCurveJson = false;
	if (Ctx.Params->TryGetObjectField(TEXT("curve_json"), CurveJsonPtr) && CurveJsonPtr && CurveJsonPtr->IsValid())
	{
		TypedCurveJson = *CurveJsonPtr;
		bHasTypedCurveJson = true;
	}
	else if (Ctx.Params->TryGetObjectField(TEXT("curve"), CurveJsonPtr) && CurveJsonPtr && CurveJsonPtr->IsValid())
	{
		TypedCurveJson = *CurveJsonPtr;
		bHasTypedCurveJson = true;
	}

	FRichCurve TypedRichCurve;
	TArray<TSharedPtr<FJsonValue>> CurveJsonIssues;
	if (!bRemove && bHasTypedCurveJson)
	{
		UeAgentCurveJson::WarnUnknownCurveFields(TypedCurveJson, TEXT("curve_json"), CurveJsonIssues);
		if (CurveType != ERawCurveTrackTypes::RCT_Float)
		{
			UeAgentCurveJson::AddCurveIssue(CurveJsonIssues, TEXT("error"), TEXT("curve_unsupported_carrier"), TEXT("curve_type"), TEXT("AnimSequence typed curve JSON currently supports curve_type=float."));
		}
		if (!UeAgentCurveJson::ApplyChannelJsonToRichCurve(TypedCurveJson, TEXT("value"), TypedRichCurve, TEXT("curve_json"), CurveJsonIssues))
		{
			OutData->SetArrayField(TEXT("json_issues"), CurveJsonIssues);
			OutError = TEXT("curve_json_apply_failed");
			return false;
		}
		for (const FRichCurveKey& Key : TypedRichCurve.GetConstRefOfKeys())
		{
			if (Key.Time < 0.0f || Key.Time > Sequence->GetPlayLength() + KINDA_SMALL_NUMBER)
			{
				UeAgentCurveJson::AddCurveIssue(CurveJsonIssues, TEXT("error"), TEXT("curve_key_time_out_of_range"), TEXT("curve_json.channels.value.keys"), FString::Printf(TEXT("Curve key time %.6f is outside AnimSequence range 0..%.6f."), Key.Time, Sequence->GetPlayLength()));
			}
		}
		if (UeAgentCurveJson::HasError(CurveJsonIssues))
		{
			OutData->SetArrayField(TEXT("json_issues"), CurveJsonIssues);
			OutError = TEXT("curve_json_apply_failed");
			return false;
		}
	}

	Sequence->Modify();

	if (bRemove)
	{
		UAnimationBlueprintLibrary::RemoveCurve(Sequence, CurveName, bRemoveNameFromSkeleton);
	}
	else
	{
		if ((bClearExistingKeys || bHasTypedCurveJson) && UAnimationBlueprintLibrary::DoesCurveExist(Sequence, CurveName, CurveType))
		{
			UAnimationBlueprintLibrary::RemoveCurve(Sequence, CurveName, false);
		}

		if (!UAnimationBlueprintLibrary::DoesCurveExist(Sequence, CurveName, CurveType))
		{
			UAnimationBlueprintLibrary::AddCurve(Sequence, CurveName, CurveType, bMetaDataCurve);
		}

		auto AddSingleCurveKey = [&](const TSharedPtr<FJsonObject>& CurveObject) -> bool
		{
			if (!CurveObject.IsValid())
			{
				return true;
			}

			double TimeSeconds = 0.0;
			if (!CurveObject->TryGetNumberField(TEXT("time_seconds"), TimeSeconds) || TimeSeconds < 0.0 || TimeSeconds > Sequence->GetPlayLength() + KINDA_SMALL_NUMBER)
			{
				OutError = TEXT("invalid_curve_time_seconds");
				return false;
			}

			switch (CurveType)
			{
			case ERawCurveTrackTypes::RCT_Float:
			default:
			{
				double FloatValue = 0.0;
				if (!CurveObject->TryGetNumberField(TEXT("value"), FloatValue))
				{
					OutError = TEXT("missing_float_curve_value");
					return false;
				}
				UAnimationBlueprintLibrary::AddFloatCurveKey(Sequence, CurveName, static_cast<float>(TimeSeconds), static_cast<float>(FloatValue));
				return true;
			}
			}
		};

		if (bHasTypedCurveJson)
		{
			for (const FRichCurveKey& Key : TypedRichCurve.GetConstRefOfKeys())
			{
				UAnimationBlueprintLibrary::AddFloatCurveKey(Sequence, CurveName, Key.Time, Key.Value);
			}
		}
		else if (Ctx.Params->HasTypedField<EJson::Array>(TEXT("keys")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
			if (Ctx.Params->TryGetArrayField(TEXT("keys"), Keys) && Keys)
			{
				for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
				{
					const TSharedPtr<FJsonObject>* KeyObject = nullptr;
					if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObject) || !KeyObject || !KeyObject->IsValid())
					{
						continue;
					}
					if (!AddSingleCurveKey(*KeyObject))
					{
						return false;
					}
				}
			}
		}
		else if (Ctx.Params->HasField(TEXT("time_seconds")))
		{
			if (!AddSingleCurveKey(Ctx.Params))
			{
				return false;
			}
		}
	}

	Sequence->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Sequence, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("curve_name"), CurveName.ToString());
	OutData->SetStringField(TEXT("curve_type"), UeAgentAnimationAssetOps::CurveTypeToString(CurveType));
	OutData->SetBoolField(TEXT("removed"), bRemove);
	OutData->SetBoolField(TEXT("curve_exists"), !bRemove);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	if (CurveJsonIssues.Num() > 0)
	{
		OutData->SetArrayField(TEXT("json_issues"), CurveJsonIssues);
	}
	if (!bRemove)
	{
		OutData->SetObjectField(TEXT("curve"), UeAgentAnimationAssetOps::BuildCurveJson(Sequence, CurveName, CurveType, true));
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdAnimSequenceSetBones(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimSequence* Sequence = UeAgentAnimationAssetOps::LoadAnimSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("anim_sequence_not_found");
		return false;
	}

	bool bRemoveAllBoneAnimation = false;
	bool bIncludeChildren = false;
	bool bExcludeChildrenRecursively = false;
	bool bFinalizeAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("remove_all_bone_animation"), bRemoveAllBoneAnimation);
	JsonTryGetBool(Ctx.Params, TEXT("include_children"), bIncludeChildren);
	JsonTryGetBool(Ctx.Params, TEXT("exclude_children_recursively"), bExcludeChildrenRecursively);
	JsonTryGetBool(Ctx.Params, TEXT("finalize_after_set"), bFinalizeAfterSet);

	Sequence->Modify();

	int32 RemovedBoneRequestCount = 0;
	int32 RemovedVirtualBoneRequestCount = 0;

	if (bRemoveAllBoneAnimation)
	{
		UAnimationBlueprintLibrary::RemoveAllBoneAnimation(Sequence);
	}
	else
	{
		const TArray<TSharedPtr<FJsonValue>>* RemoveBoneNames = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ChildrenExcluded = nullptr;
		Ctx.Params->TryGetArrayField(TEXT("remove_bone_names"), RemoveBoneNames);
		Ctx.Params->TryGetArrayField(TEXT("children_excluded"), ChildrenExcluded);

		TArray<FName> ExcludedChildrenNames;
		if (ChildrenExcluded)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ChildrenExcluded)
			{
				if (!Value.IsValid())
				{
					continue;
				}
				const FString BoneNameText = Value->AsString().TrimStartAndEnd();
				if (!BoneNameText.IsEmpty())
				{
					ExcludedChildrenNames.Add(FName(*BoneNameText));
				}
			}
		}

		if (RemoveBoneNames)
		{
			TArray<FName> BoneNames;
			for (const TSharedPtr<FJsonValue>& Value : *RemoveBoneNames)
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

			RemovedBoneRequestCount = BoneNames.Num();
			for (int32 Index = 0; Index < BoneNames.Num(); ++Index)
			{
				const bool bFinalizeThisRemoval = bFinalizeAfterSet && (Index == BoneNames.Num() - 1);
				if (ExcludedChildrenNames.Num() > 0)
				{
					UAnimationBlueprintLibrary::RemoveBoneSelectiveAnimation(Sequence, BoneNames[Index], ExcludedChildrenNames, bIncludeChildren, bExcludeChildrenRecursively, bFinalizeThisRemoval);
				}
				else
				{
					UAnimationBlueprintLibrary::RemoveBoneAnimation(Sequence, BoneNames[Index], bIncludeChildren, bFinalizeThisRemoval);
				}
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* RemoveVirtualBoneNames = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("remove_virtual_bone_names"), RemoveVirtualBoneNames) && RemoveVirtualBoneNames)
	{
		TArray<FName> VirtualBoneNames;
		for (const TSharedPtr<FJsonValue>& Value : *RemoveVirtualBoneNames)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			const FString BoneNameText = Value->AsString().TrimStartAndEnd();
			if (!BoneNameText.IsEmpty())
			{
				VirtualBoneNames.Add(FName(*BoneNameText));
			}
		}

		if (VirtualBoneNames.Num() > 0)
		{
			UAnimationBlueprintLibrary::RemoveVirtualBones(Sequence, VirtualBoneNames);
			RemovedVirtualBoneRequestCount = VirtualBoneNames.Num();
		}
	}

	Sequence->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Sequence, OutError))
	{
		return false;
	}

	TArray<FName> TrackNames;
	UAnimationBlueprintLibrary::GetAnimationTrackNames(Sequence, TrackNames);
	TArray<TSharedPtr<FJsonValue>> TrackNamesJson;
	for (const FName TrackName : TrackNames)
	{
		TrackNamesJson.Add(MakeShared<FJsonValueString>(TrackName.ToString()));
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetBoolField(TEXT("removed_all_bone_animation"), bRemoveAllBoneAnimation);
	OutData->SetNumberField(TEXT("removed_bone_request_count"), RemovedBoneRequestCount);
	OutData->SetNumberField(TEXT("removed_virtual_bone_request_count"), RemovedVirtualBoneRequestCount);
	OutData->SetNumberField(TEXT("track_count"), TrackNamesJson.Num());
	OutData->SetArrayField(TEXT("track_names"), TrackNamesJson);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}
