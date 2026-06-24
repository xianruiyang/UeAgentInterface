// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_AnimationAssets.h"

#include "UeAgentHttpServer_AnimationAssetOps.h"

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

bool FUeAgentHttpServer::CmdSkeletonGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(AssetPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();

	TArray<TSharedPtr<FJsonValue>> SlotGroups;
	for (const FAnimSlotGroup& SlotGroup : Skeleton->GetSlotGroups())
	{
		TSharedPtr<FJsonObject> GroupObject = MakeShared<FJsonObject>();
		GroupObject->SetStringField(TEXT("group_name"), SlotGroup.GroupName.ToString());

		TArray<TSharedPtr<FJsonValue>> SlotNames;
		for (const FName SlotName : SlotGroup.SlotNames)
		{
			SlotNames.Add(MakeShared<FJsonValueString>(SlotName.ToString()));
		}
		GroupObject->SetArrayField(TEXT("slot_names"), SlotNames);
		SlotGroups.Add(MakeShared<FJsonValueObject>(GroupObject));
	}

	TArray<TSharedPtr<FJsonValue>> Sockets;
	for (USkeletalMeshSocket* Socket : Skeleton->Sockets)
	{
		if (Socket)
		{
			Sockets.Add(MakeShared<FJsonValueObject>(UeAgentAnimationAssetOps::BuildSocketJson(Socket)));
		}
	}

	TArray<TSharedPtr<FJsonValue>> VirtualBones;
	for (const FVirtualBone& VirtualBone : Skeleton->GetVirtualBones())
	{
		VirtualBones.Add(MakeShared<FJsonValueObject>(UeAgentAnimationAssetOps::BuildVirtualBoneJson(VirtualBone)));
	}

	TArray<TSharedPtr<FJsonValue>> CompatibleSkeletons;
	for (const TSoftObjectPtr<USkeleton>& CompatibleSkeleton : Skeleton->GetCompatibleSkeletons())
	{
		CompatibleSkeletons.Add(MakeShared<FJsonValueString>(CompatibleSkeleton.ToSoftObjectPath().ToString()));
	}

#if WITH_EDITOR
	TArray<FName> AnimationNotifies;
	Skeleton->CollectAnimationNotifies(AnimationNotifies);
	TArray<TSharedPtr<FJsonValue>> AnimationNotifyNames;
	for (const FName NotifyName : AnimationNotifies)
	{
		AnimationNotifyNames.Add(MakeShared<FJsonValueString>(NotifyName.ToString()));
	}
#endif

	OutData->SetStringField(TEXT("asset_path"), UeAgentAnimationAssetOps::NormalizeAssetPath(Skeleton->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Skeleton->GetPathName());
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), Skeleton->GetPreviewMesh() ? Skeleton->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("root_bone_name"), ReferenceSkeleton.GetNum() > 0 ? ReferenceSkeleton.GetBoneName(0).ToString() : TEXT(""));
	OutData->SetNumberField(TEXT("bone_count"), ReferenceSkeleton.GetNum());
	OutData->SetNumberField(TEXT("socket_count"), Sockets.Num());
	OutData->SetNumberField(TEXT("virtual_bone_count"), VirtualBones.Num());
	OutData->SetNumberField(TEXT("slot_group_count"), SlotGroups.Num());
	OutData->SetNumberField(TEXT("compatible_skeleton_count"), CompatibleSkeletons.Num());
	OutData->SetArrayField(TEXT("slot_groups"), SlotGroups);
	OutData->SetArrayField(TEXT("sockets"), Sockets);
	OutData->SetArrayField(TEXT("virtual_bones"), VirtualBones);
	OutData->SetArrayField(TEXT("compatible_skeletons"), CompatibleSkeletons);
	OutData->SetBoolField(TEXT("use_retarget_modes_from_compatible_skeleton"), Skeleton->GetUseRetargetModesFromCompatibleSkeleton());
#if WITH_EDITOR
	OutData->SetNumberField(TEXT("animation_notify_name_count"), AnimationNotifyNames.Num());
	OutData->SetArrayField(TEXT("animation_notify_names"), AnimationNotifyNames);
#endif
	return true;
}

bool FUeAgentHttpServer::CmdSkeletonListBones(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(AssetPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
	TArray<TSharedPtr<FJsonValue>> Bones;
	Bones.Reserve(ReferenceSkeleton.GetNum());

	for (int32 BoneIndex = 0; BoneIndex < ReferenceSkeleton.GetNum(); ++BoneIndex)
	{
		const int32 ParentIndex = ReferenceSkeleton.GetParentIndex(BoneIndex);
		TSharedPtr<FJsonObject> BoneObject = MakeShared<FJsonObject>();
		BoneObject->SetNumberField(TEXT("bone_index"), BoneIndex);
		BoneObject->SetStringField(TEXT("bone_name"), ReferenceSkeleton.GetBoneName(BoneIndex).ToString());
		BoneObject->SetNumberField(TEXT("parent_index"), ParentIndex);
		BoneObject->SetStringField(TEXT("parent_bone_name"), ParentIndex != INDEX_NONE ? ReferenceSkeleton.GetBoneName(ParentIndex).ToString() : TEXT(""));
		Bones.Add(MakeShared<FJsonValueObject>(BoneObject));
	}

	OutData->SetStringField(TEXT("asset_path"), Skeleton->GetPathName());
	OutData->SetNumberField(TEXT("bone_count"), Bones.Num());
	OutData->SetArrayField(TEXT("bones"), Bones);
	return true;
}

bool FUeAgentHttpServer::CmdSkeletonSetCompatibleSkeletons(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(AssetPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	auto ApplySkeletonList = [&](const TArray<TSharedPtr<FJsonValue>>* Paths, const bool bAdd) -> bool
	{
		if (!Paths)
		{
			return true;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Paths)
		{
			if (!Value.IsValid())
			{
				continue;
			}

			const FString CompatibleSkeletonPath = Value->AsString().TrimStartAndEnd();
			if (CompatibleSkeletonPath.IsEmpty())
			{
				continue;
			}

			USkeleton* CompatibleSkeleton = UeAgentAnimationAssetOps::LoadSkeleton(CompatibleSkeletonPath);
			if (!CompatibleSkeleton)
			{
				OutError = TEXT("compatible_skeleton_not_found");
				return false;
			}
			if (CompatibleSkeleton == Skeleton)
			{
				OutError = TEXT("compatible_skeleton_same_as_asset");
				return false;
			}

			if (bAdd)
			{
				Skeleton->AddCompatibleSkeleton(CompatibleSkeleton);
			}
			else
			{
				Skeleton->RemoveCompatibleSkeleton(CompatibleSkeleton);
			}
		}
		return true;
	};

	Skeleton->Modify();

	bool bClearAll = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_all"), bClearAll);
	if (bClearAll)
	{
		TArray<TSoftObjectPtr<USkeleton>> ExistingCompatibleSkeletons = Skeleton->GetCompatibleSkeletons();
		for (const TSoftObjectPtr<USkeleton>& CompatibleSkeleton : ExistingCompatibleSkeletons)
		{
			Skeleton->RemoveCompatibleSkeleton(CompatibleSkeleton);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* SetCompatibleSkeletonPaths = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("set_compatible_skeleton_paths"), SetCompatibleSkeletonPaths) && SetCompatibleSkeletonPaths)
	{
		TArray<TSoftObjectPtr<USkeleton>> ExistingCompatibleSkeletons = Skeleton->GetCompatibleSkeletons();
		for (const TSoftObjectPtr<USkeleton>& CompatibleSkeleton : ExistingCompatibleSkeletons)
		{
			Skeleton->RemoveCompatibleSkeleton(CompatibleSkeleton);
		}
		if (!ApplySkeletonList(SetCompatibleSkeletonPaths, true))
		{
			return false;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* AddCompatibleSkeletonPaths = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("add_compatible_skeleton_paths"), AddCompatibleSkeletonPaths) && AddCompatibleSkeletonPaths)
	{
		if (!ApplySkeletonList(AddCompatibleSkeletonPaths, true))
		{
			return false;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* RemoveCompatibleSkeletonPaths = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("remove_compatible_skeleton_paths"), RemoveCompatibleSkeletonPaths) && RemoveCompatibleSkeletonPaths)
	{
		if (!ApplySkeletonList(RemoveCompatibleSkeletonPaths, false))
		{
			return false;
		}
	}

	if (Ctx.Params->HasField(TEXT("use_retarget_modes_from_compatible_skeleton")))
	{
		bool bUseRetargetModesFromCompatibleSkeleton = false;
		JsonTryGetBool(Ctx.Params, TEXT("use_retarget_modes_from_compatible_skeleton"), bUseRetargetModesFromCompatibleSkeleton);
		Skeleton->SetUseRetargetModesFromCompatibleSkeleton(bUseRetargetModesFromCompatibleSkeleton);
	}

	Skeleton->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Skeleton, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> CompatibleSkeletons;
	for (const TSoftObjectPtr<USkeleton>& CompatibleSkeleton : Skeleton->GetCompatibleSkeletons())
	{
		CompatibleSkeletons.Add(MakeShared<FJsonValueString>(CompatibleSkeleton.ToSoftObjectPath().ToString()));
	}

	OutData->SetStringField(TEXT("asset_path"), Skeleton->GetPathName());
	OutData->SetNumberField(TEXT("compatible_skeleton_count"), CompatibleSkeletons.Num());
	OutData->SetArrayField(TEXT("compatible_skeletons"), CompatibleSkeletons);
	OutData->SetBoolField(TEXT("use_retarget_modes_from_compatible_skeleton"), Skeleton->GetUseRetargetModesFromCompatibleSkeleton());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSkeletonSetPreviewMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(AssetPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
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
	}

	Skeleton->Modify();
	Skeleton->SetPreviewMesh(PreviewMesh, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Skeleton, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Skeleton->GetPathName());
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), Skeleton->GetPreviewMesh() ? Skeleton->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSkeletonSetSocket(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SocketNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("socket_name"), SocketNameText) || SocketNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_socket_name");
		return false;
	}
	const FName SocketName(*SocketNameText.TrimStartAndEnd());

	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(AssetPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	bool bRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("remove"), bRemove);

	USkeletalMeshSocket* Socket = UeAgentAnimationAssetOps::FindSocketByName(Skeleton, SocketName);
	Skeleton->Modify();

	if (bRemove)
	{
		if (!Socket)
		{
			OutError = TEXT("socket_not_found");
			return false;
		}
		Skeleton->Sockets.Remove(Socket);
		Skeleton->MarkPackageDirty();
	}
	else
	{
		FString BoneNameText;
		if (!JsonTryGetString(Ctx.Params, TEXT("bone_name"), BoneNameText) && !Socket)
		{
			OutError = TEXT("missing_bone_name");
			return false;
		}

		if (!Socket)
		{
			Socket = NewObject<USkeletalMeshSocket>(Skeleton, NAME_None, RF_Transactional);
			if (!Socket)
			{
				OutError = TEXT("create_socket_failed");
				return false;
			}
			Skeleton->Sockets.Add(Socket);
		}

		if (!BoneNameText.TrimStartAndEnd().IsEmpty())
		{
			Socket->BoneName = FName(*BoneNameText.TrimStartAndEnd());
		}
		Socket->SocketName = SocketName;

		FVector RelativeLocation = Socket->RelativeLocation;
		FRotator RelativeRotation = Socket->RelativeRotation;
		FVector RelativeScale = Socket->RelativeScale;
		JsonTryGetVector(Ctx.Params, TEXT("relative_location"), RelativeLocation);
		JsonTryGetRotator(Ctx.Params, TEXT("relative_rotation"), RelativeRotation);
		JsonTryGetVector(Ctx.Params, TEXT("relative_scale"), RelativeScale);
		Socket->RelativeLocation = RelativeLocation;
		Socket->RelativeRotation = RelativeRotation;
		Socket->RelativeScale = RelativeScale;
		Skeleton->MarkPackageDirty();
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Skeleton, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Skeleton->GetPathName());
	OutData->SetStringField(TEXT("socket_name"), SocketName.ToString());
	OutData->SetBoolField(TEXT("removed"), bRemove);
	if (!bRemove && Socket)
	{
		OutData->SetStringField(TEXT("bone_name"), Socket->BoneName.ToString());
		OutData->SetObjectField(TEXT("relative_location"), UeAgentAnimationAssetOps::BuildVectorJson(Socket->RelativeLocation));
		OutData->SetObjectField(TEXT("relative_rotation"), UeAgentAnimationAssetOps::BuildRotatorJson(Socket->RelativeRotation));
		OutData->SetObjectField(TEXT("relative_scale"), UeAgentAnimationAssetOps::BuildVectorJson(Socket->RelativeScale));
	}
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSkeletonSetVirtualBone(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(AssetPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	bool bRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("remove"), bRemove);

	Skeleton->Modify();
	FName VirtualBoneName = NAME_None;

	if (bRemove)
	{
		FString VirtualBoneNameText;
		if (!JsonTryGetString(Ctx.Params, TEXT("virtual_bone_name"), VirtualBoneNameText) || VirtualBoneNameText.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_virtual_bone_name");
			return false;
		}

		VirtualBoneName = FName(*VirtualBoneNameText.TrimStartAndEnd());
		if (!UeAgentAnimationAssetOps::FindVirtualBoneByName(Skeleton, VirtualBoneName))
		{
			OutError = TEXT("virtual_bone_not_found");
			return false;
		}

		TArray<FName> BonesToRemove;
		BonesToRemove.Add(VirtualBoneName);
		Skeleton->RemoveVirtualBones(BonesToRemove);
		Skeleton->MarkPackageDirty();
	}
	else
	{
		FString SourceBoneNameText;
		FString TargetBoneNameText;
		JsonTryGetString(Ctx.Params, TEXT("source_bone_name"), SourceBoneNameText);
		JsonTryGetString(Ctx.Params, TEXT("target_bone_name"), TargetBoneNameText);
		SourceBoneNameText = SourceBoneNameText.TrimStartAndEnd();
		TargetBoneNameText = TargetBoneNameText.TrimStartAndEnd();
		if (SourceBoneNameText.IsEmpty() || TargetBoneNameText.IsEmpty())
		{
			OutError = TEXT("missing_source_or_target_bone_name");
			return false;
		}

		FString VirtualBoneNameText;
		JsonTryGetString(Ctx.Params, TEXT("virtual_bone_name"), VirtualBoneNameText);
		VirtualBoneNameText = VirtualBoneNameText.TrimStartAndEnd();

		if (VirtualBoneNameText.IsEmpty())
		{
			if (!Skeleton->AddNewVirtualBone(FName(*SourceBoneNameText), FName(*TargetBoneNameText), VirtualBoneName))
			{
				OutError = TEXT("add_virtual_bone_failed");
				return false;
			}
		}
		else
		{
			VirtualBoneName = FName(*VirtualBoneNameText);
			if (!Skeleton->AddNewNamedVirtualBone(FName(*SourceBoneNameText), FName(*TargetBoneNameText), VirtualBoneName))
			{
				OutError = TEXT("add_virtual_bone_failed");
				return false;
			}
		}
		Skeleton->MarkPackageDirty();
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Skeleton, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Skeleton->GetPathName());
	OutData->SetStringField(TEXT("virtual_bone_name"), VirtualBoneName.ToString());
	OutData->SetBoolField(TEXT("removed"), bRemove);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}


#include "UeAgentHttpServer_AnimationAssets_FolderFormat.inl"