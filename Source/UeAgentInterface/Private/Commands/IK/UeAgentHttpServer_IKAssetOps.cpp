// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_IKAssetOps.h"
#include "UeAgentHttpServer_IKAssets.h"

#include "Utilities/UeAgentAuthoringJson.h"
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

namespace UeAgentIKAssetOps
{
	FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();

		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	FString ToObjectPath(const FString& InPath)
	{
		FString InObjectPath = InPath;
		InObjectPath.TrimStartAndEndInline();
		if (InObjectPath.IsEmpty())
		{
			return FString();
		}
		if (InObjectPath.Contains(TEXT(".")))
		{
			return InObjectPath;
		}

		const FString AssetPath = NormalizeAssetPath(InObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	UIKRigDefinition* LoadIKRig(const FString& InPath)
	{
		return Cast<UIKRigDefinition>(LoadAssetObject(InPath));
	}

	UIKRetargeter* LoadIKRetargeter(const FString& InPath)
	{
		return Cast<UIKRetargeter>(LoadAssetObject(InPath));
	}

	USkeletalMesh* LoadSkeletalMesh(const FString& InPath)
	{
		return Cast<USkeletalMesh>(LoadAssetObject(InPath));
	}

	UObject* CreateAssetWithFactory(const FString& AssetPath, UClass* AssetClass, UFactory* Factory, FString& OutError)
	{
		const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(NormalizedAssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(NormalizedAssetPath);
		const FString PackagePath = FPackageName::GetLongPackagePath(NormalizedAssetPath);
		if (AssetName.IsEmpty() || PackagePath.IsEmpty())
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}

		if (LoadAssetObject(NormalizedAssetPath))
		{
			OutError = TEXT("asset_already_exists");
			return nullptr;
		}

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		UObject* NewAsset = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, AssetClass, Factory);
		if (!NewAsset)
		{
			OutError = TEXT("create_asset_failed");
			return nullptr;
		}
		return NewAsset;
	}

	bool SaveAssetPackage(UObject* Asset, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("asset_not_found");
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Asset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	FString SanitizeArtifactSegment(const FString& Value)
	{
		FString Out;
		Out.Reserve(Value.Len());
		for (const TCHAR Ch : Value)
		{
			if (FChar::IsAlnum(Ch) || Ch == TCHAR('-') || Ch == TCHAR('_'))
			{
				Out.AppendChar(Ch);
			}
			else
			{
				Out.AppendChar(TCHAR('_'));
			}
		}
		Out.TrimStartAndEndInline();
		return Out.IsEmpty() ? TEXT("probe") : Out.Left(64);
	}

	FString MakeProbeArtifactPath(const FString& Ext, const FString& RequestId, const FString& Suffix)
	{
		const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
		const FString RootRel = Settings ? Settings->ArtifactsDirRelative : TEXT("Saved/RemoteArtifacts");
		const FString ShotsDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), RootRel / TEXT("Shots"));
		const FString Hostname = SanitizeArtifactSegment(FPlatformProcess::ComputerName());
		const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		const FString SafeRequestId = SanitizeArtifactSegment(RequestId);
		const FString SafeSuffix = SanitizeArtifactSegment(Suffix);
		const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		return FPaths::Combine(
			ShotsDir,
			FString::Printf(TEXT("probe_%s_%s_%s_%s_%s.%s"), *Hostname, *Timestamp, *SafeRequestId, *SafeSuffix, *Guid, *Ext));
	}

	TSharedPtr<FJsonObject> BuildLinearColorJson(const FLinearColor& Color)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("r"), Color.R);
		Obj->SetNumberField(TEXT("g"), Color.G);
		Obj->SetNumberField(TEXT("b"), Color.B);
		Obj->SetNumberField(TEXT("a"), Color.A);
		return Obj;
	}

	TSharedPtr<FJsonObject> BuildVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	TSharedPtr<FJsonObject> BuildRotatorJson(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

	TSharedPtr<FJsonObject> BuildTransformJson(const FTransform& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("location"), BuildVectorJson(Value.GetLocation()));
		Obj->SetObjectField(TEXT("rotation"), BuildRotatorJson(Value.Rotator()));
		Obj->SetObjectField(TEXT("scale"), BuildVectorJson(Value.GetScale3D()));
		return Obj;
	}

	TArray<TSharedPtr<FJsonValue>> TextArrayToJson(const TArray<FText>& Texts)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FText& Text : Texts)
		{
			Values.Add(MakeShared<FJsonValueString>(Text.ToString()));
		}
		return Values;
	}

	TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	bool TryGetVectorObject(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue)
	{
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
		if (!FieldValue)
		{
			return false;
		}

		return UeAgentJsonDiagnostics::TryReadVectorValue(*FieldValue, OutValue);
	}

	bool TryGetRotatorObject(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue)
	{
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
		if (!FieldValue)
		{
			return false;
		}

		return UeAgentJsonDiagnostics::TryReadRotatorValue(*FieldValue, OutValue);
	}

	const TCHAR* SourceOrTargetToString(const ERetargetSourceOrTarget Value)
	{
		return Value == ERetargetSourceOrTarget::Source ? TEXT("source") : TEXT("target");
	}

	bool ParseSourceOrTarget(const FString& InValue, ERetargetSourceOrTarget& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized == TEXT("source") || Normalized == TEXT("src"))
		{
			OutValue = ERetargetSourceOrTarget::Source;
			return true;
		}
		if (Normalized == TEXT("target") || Normalized == TEXT("dst"))
		{
			OutValue = ERetargetSourceOrTarget::Target;
			return true;
		}
		return false;
	}

	bool ParseAutoMapType(const FString& InValue, EAutoMapChainType& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("exact"))
		{
			OutValue = EAutoMapChainType::Exact;
			return true;
		}
		if (Normalized == TEXT("fuzzy"))
		{
			OutValue = EAutoMapChainType::Fuzzy;
			return true;
		}
		if (Normalized == TEXT("clear"))
		{
			OutValue = EAutoMapChainType::Clear;
			return true;
		}
		return false;
	}

	bool ParseAutoAlignMethod(const FString& InValue, ERetargetAutoAlignMethod& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("chaintochain") || Normalized == TEXT("chain"))
		{
			OutValue = ERetargetAutoAlignMethod::ChainToChain;
			return true;
		}
		if (Normalized == TEXT("meshtomesh") || Normalized == TEXT("mesh"))
		{
			OutValue = ERetargetAutoAlignMethod::MeshToMesh;
			return true;
		}
		if (Normalized == TEXT("localrotationaxes") || Normalized == TEXT("localaxes"))
		{
			OutValue = ERetargetAutoAlignMethod::LocalRotationAxes;
			return true;
		}
		if (Normalized == TEXT("globalrotationaxes") || Normalized == TEXT("globalaxes"))
		{
			OutValue = ERetargetAutoAlignMethod::GlobalRotationAxes;
			return true;
		}
		return false;
	}

	bool ParseIKRigGoalSpace(const FString& InValue, EIKRigGoalSpace& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("additive"))
		{
			OutValue = EIKRigGoalSpace::Additive;
			return true;
		}
		if (Normalized == TEXT("component") || Normalized == TEXT("componentspace"))
		{
			OutValue = EIKRigGoalSpace::Component;
			return true;
		}
		if (Normalized == TEXT("world") || Normalized == TEXT("worldspace"))
		{
			OutValue = EIKRigGoalSpace::World;
			return true;
		}
		return false;
	}

	FString IKRigGoalSpaceToString(const EIKRigGoalSpace Value)
	{
		switch (Value)
		{
		case EIKRigGoalSpace::Component:
			return TEXT("component");
		case EIKRigGoalSpace::World:
			return TEXT("world");
		case EIKRigGoalSpace::Additive:
		default:
			return TEXT("additive");
		}
	}

	FString AutoAlignMethodToString(const ERetargetAutoAlignMethod InValue)
	{
		switch (InValue)
		{
		case ERetargetAutoAlignMethod::MeshToMesh:
			return TEXT("mesh_to_mesh");
		case ERetargetAutoAlignMethod::LocalRotationAxes:
			return TEXT("local_rotation_axes");
		case ERetargetAutoAlignMethod::GlobalRotationAxes:
			return TEXT("global_rotation_axes");
		case ERetargetAutoAlignMethod::ChainToChain:
		default:
			return TEXT("chain_to_chain");
		}
	}

	TSharedPtr<FJsonObject> BuildGoalJson(const UIKRigEffectorGoal* Goal)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("goal_name"), Goal ? Goal->GoalName.ToString() : TEXT(""));
		Item->SetStringField(TEXT("bone_name"), Goal ? Goal->BoneName.ToString() : TEXT(""));
		Item->SetNumberField(TEXT("position_alpha"), Goal ? Goal->PositionAlpha : 0.0);
		Item->SetNumberField(TEXT("rotation_alpha"), Goal ? Goal->RotationAlpha : 0.0);
		const FTransform GoalTransform = Goal ? Goal->CurrentTransform : FTransform::Identity;
		Item->SetObjectField(TEXT("current_location"), BuildVectorJson(GoalTransform.GetLocation()));
		Item->SetObjectField(TEXT("current_rotation"), BuildRotatorJson(GoalTransform.Rotator()));
		Item->SetObjectField(TEXT("current_scale"), BuildVectorJson(GoalTransform.GetScale3D()));
		TSharedPtr<FJsonObject> TransformObject = MakeShared<FJsonObject>();
		TransformObject->SetObjectField(TEXT("location"), BuildVectorJson(GoalTransform.GetLocation()));
		TransformObject->SetObjectField(TEXT("rotation"), BuildRotatorJson(GoalTransform.Rotator()));
		TransformObject->SetObjectField(TEXT("scale"), BuildVectorJson(GoalTransform.GetScale3D()));
		Item->SetObjectField(TEXT("current_transform"), TransformObject);
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("goal_name"),
			Goal ? Goal->GoalName.ToString() : TEXT(""),
			Goal ? Goal->GoalName.ToString() : TEXT("IK Goal"),
			TEXT("IK Rig/Goals"),
			TEXT("ik_goal"),
			TEXT("goal"),
			{ TEXT("set"), TEXT("remove") });
		return Item;
	}

	FString BasicAxisToString(const EBasicAxis Axis)
	{
		switch (Axis)
		{
		case EBasicAxis::X:
			return TEXT("x");
		case EBasicAxis::Y:
			return TEXT("y");
		case EBasicAxis::Z:
			return TEXT("z");
		case EBasicAxis::NegX:
			return TEXT("-x");
		case EBasicAxis::NegY:
			return TEXT("-y");
		case EBasicAxis::NegZ:
			return TEXT("-z");
		default:
			return TEXT("y");
		}
	}

	bool ParseBasicAxis(const FString& InValue, EBasicAxis& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("y") || Normalized == TEXT("+y") || Normalized == TEXT("forward"))
		{
			OutValue = EBasicAxis::Y;
			return true;
		}
		if (Normalized == TEXT("x") || Normalized == TEXT("+x"))
		{
			OutValue = EBasicAxis::X;
			return true;
		}
		if (Normalized == TEXT("z") || Normalized == TEXT("+z"))
		{
			OutValue = EBasicAxis::Z;
			return true;
		}
		if (Normalized == TEXT("-x") || Normalized == TEXT("negx"))
		{
			OutValue = EBasicAxis::NegX;
			return true;
		}
		if (Normalized == TEXT("-y") || Normalized == TEXT("negy"))
		{
			OutValue = EBasicAxis::NegY;
			return true;
		}
		if (Normalized == TEXT("-z") || Normalized == TEXT("negz"))
		{
			OutValue = EBasicAxis::NegZ;
			return true;
		}
		return false;
	}

	FString WarpingDirectionSourceToString(const EWarpingDirectionSource Source)
	{
		switch (Source)
		{
		case EWarpingDirectionSource::Chain:
			return TEXT("chain");
		case EWarpingDirectionSource::RootBone:
			return TEXT("root_bone");
		case EWarpingDirectionSource::Goals:
		default:
			return TEXT("goals");
		}
	}

	bool ParseWarpingDirectionSource(const FString& InValue, EWarpingDirectionSource& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("goals") || Normalized == TEXT("goal"))
		{
			OutValue = EWarpingDirectionSource::Goals;
			return true;
		}
		if (Normalized == TEXT("chain"))
		{
			OutValue = EWarpingDirectionSource::Chain;
			return true;
		}
		if (Normalized == TEXT("rootbone") || Normalized == TEXT("root"))
		{
			OutValue = EWarpingDirectionSource::RootBone;
			return true;
		}
		return false;
	}

	FString RetargetTranslationModeToString(const ERetargetTranslationMode Value)
	{
		switch (Value)
		{
		case ERetargetTranslationMode::GloballyScaled:
			return TEXT("globally_scaled");
		case ERetargetTranslationMode::Absolute:
			return TEXT("absolute");
		case ERetargetTranslationMode::StretchBoneLengthUniformly:
			return TEXT("stretch_bone_length_uniformly");
		case ERetargetTranslationMode::StretchBoneLengthNonUniformly:
			return TEXT("stretch_bone_length_non_uniformly");
		case ERetargetTranslationMode::None:
		default:
			return TEXT("none");
		}
	}

	bool ParseRetargetTranslationMode(const FString& InValue, ERetargetTranslationMode& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("none"))
		{
			OutValue = ERetargetTranslationMode::None;
			return true;
		}
		if (Normalized == TEXT("globallyscaled"))
		{
			OutValue = ERetargetTranslationMode::GloballyScaled;
			return true;
		}
		if (Normalized == TEXT("absolute"))
		{
			OutValue = ERetargetTranslationMode::Absolute;
			return true;
		}
		if (Normalized == TEXT("stretchbonelengthuniformly"))
		{
			OutValue = ERetargetTranslationMode::StretchBoneLengthUniformly;
			return true;
		}
		if (Normalized == TEXT("stretchbonelengthnonuniformly"))
		{
			OutValue = ERetargetTranslationMode::StretchBoneLengthNonUniformly;
			return true;
		}
		return false;
	}

	FString RetargetRotationModeToString(const ERetargetRotationMode Value)
	{
		switch (Value)
		{
		case ERetargetRotationMode::OneToOne:
			return TEXT("one_to_one");
		case ERetargetRotationMode::OneToOneReversed:
			return TEXT("one_to_one_reversed");
		case ERetargetRotationMode::MatchChain:
			return TEXT("match_chain");
		case ERetargetRotationMode::MatchScaledChain:
			return TEXT("match_scaled_chain");
		case ERetargetRotationMode::None:
			return TEXT("none");
		case ERetargetRotationMode::Interpolated:
		default:
			return TEXT("interpolated");
		}
	}

	bool ParseRetargetRotationMode(const FString& InValue, ERetargetRotationMode& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("interpolated"))
		{
			OutValue = ERetargetRotationMode::Interpolated;
			return true;
		}
		if (Normalized == TEXT("onetoone"))
		{
			OutValue = ERetargetRotationMode::OneToOne;
			return true;
		}
		if (Normalized == TEXT("onetoonereversed"))
		{
			OutValue = ERetargetRotationMode::OneToOneReversed;
			return true;
		}
		if (Normalized == TEXT("matchchain"))
		{
			OutValue = ERetargetRotationMode::MatchChain;
			return true;
		}
		if (Normalized == TEXT("matchscaledchain"))
		{
			OutValue = ERetargetRotationMode::MatchScaledChain;
			return true;
		}
		if (Normalized == TEXT("none"))
		{
			OutValue = ERetargetRotationMode::None;
			return true;
		}
		return false;
	}

	TSharedPtr<FJsonObject> BuildChainJson(const FBoneChain& Chain)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("chain_name"), Chain.ChainName.ToString());
		Item->SetStringField(TEXT("start_bone_name"), Chain.StartBone.BoneName.ToString());
		Item->SetStringField(TEXT("end_bone_name"), Chain.EndBone.BoneName.ToString());
		Item->SetStringField(TEXT("goal_name"), Chain.IKGoalName.ToString());
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("chain_name"),
			Chain.ChainName.ToString(),
			Chain.ChainName.ToString(),
			TEXT("IK Rig/Retarget Chains"),
			TEXT("ik_retarget_chain"),
			TEXT("chain"),
			{ TEXT("set"), TEXT("remove") });
		return Item;
	}

	TSharedPtr<FJsonObject> BuildChainPairJson(const FRetargetChainPair& Pair)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("target_chain_name"), Pair.TargetChainName.ToString());
		Item->SetStringField(TEXT("source_chain_name"), Pair.SourceChainName.ToString());
		return Item;
	}

	TSharedPtr<FJsonObject> BuildRetargetPoseJson(const FIKRetargetPose& Pose)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetObjectField(TEXT("root_offset"), BuildVectorJson(Pose.GetRootTranslationDelta()));
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("pose"),
			TEXT("current_pose"),
			TEXT("Retarget Pose"),
			TEXT("IK Retargeter/Poses"),
			TEXT("retarget_pose"),
			TEXT("pose"),
			{ TEXT("set"), TEXT("rename"), TEXT("remove") });

		TArray<TSharedPtr<FJsonValue>> BoneRotationOffsets;
		for (const TPair<FName, FQuat>& Pair : Pose.GetAllDeltaRotations())
		{
			TSharedPtr<FJsonObject> BoneObject = MakeShared<FJsonObject>();
			BoneObject->SetStringField(TEXT("bone_name"), Pair.Key.ToString());
			BoneObject->SetObjectField(TEXT("rotation_offset"), BuildRotatorJson(Pair.Value.Rotator()));
			UeAgentAuthoringJson::ApplyAuthoringMetadata(
				BoneObject,
				TEXT("bone_name"),
				Pair.Key.ToString(),
				Pair.Key.ToString(),
				TEXT("IK Retargeter/Pose Bone Offsets"),
				TEXT("retarget_pose_bone_offset"),
				TEXT("rotator"),
				{ TEXT("set"), TEXT("remove") });
			BoneRotationOffsets.Add(MakeShared<FJsonValueObject>(BoneObject));
		}
		Item->SetNumberField(TEXT("bone_rotation_offset_count"), BoneRotationOffsets.Num());
		Item->SetArrayField(TEXT("bone_rotation_offsets"), BoneRotationOffsets);
		return Item;
	}

	TSharedPtr<FJsonObject> BuildBodyMoverSolverSettingsJson(const FIKRigBodyMoverSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("start_bone"), Settings.StartBone.ToString());
		Item->SetNumberField(TEXT("position_alpha"), Settings.PositionAlpha);
		Item->SetNumberField(TEXT("position_positive_x"), Settings.PositionPositiveX);
		Item->SetNumberField(TEXT("position_negative_x"), Settings.PositionNegativeX);
		Item->SetNumberField(TEXT("position_positive_y"), Settings.PositionPositiveY);
		Item->SetNumberField(TEXT("position_negative_y"), Settings.PositionNegativeY);
		Item->SetNumberField(TEXT("position_positive_z"), Settings.PositionPositiveZ);
		Item->SetNumberField(TEXT("position_negative_z"), Settings.PositionNegativeZ);
		Item->SetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		Item->SetNumberField(TEXT("rotate_x_alpha"), Settings.RotateXAlpha);
		Item->SetNumberField(TEXT("rotate_y_alpha"), Settings.RotateYAlpha);
		Item->SetNumberField(TEXT("rotate_z_alpha"), Settings.RotateZAlpha);
		return Item;
	}

	TSharedPtr<FJsonObject> BuildBodyMoverGoalSettingsJson(const FIKRigBodyMoverGoalSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("goal_name"), Settings.Goal.ToString());
		Item->SetStringField(TEXT("bone_name"), Settings.BoneName.ToString());
		Item->SetNumberField(TEXT("influence_multiplier"), Settings.InfluenceMultiplier);
		return Item;
	}

	FString PBIKRootBehaviorToString(const EPBIKRootBehavior Value)
	{
		switch (Value)
		{
		case EPBIKRootBehavior::PinToInput:
			return TEXT("pin_to_input");
		case EPBIKRootBehavior::Free:
			return TEXT("free");
		case EPBIKRootBehavior::PrePull:
		default:
			return TEXT("pre_pull");
		}
	}

	bool ParsePBIKRootBehavior(const FString& InValue, EPBIKRootBehavior& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("prepull"))
		{
			OutValue = EPBIKRootBehavior::PrePull;
			return true;
		}
		if (Normalized == TEXT("pintoinput") || Normalized == TEXT("pin"))
		{
			OutValue = EPBIKRootBehavior::PinToInput;
			return true;
		}
		if (Normalized == TEXT("free"))
		{
			OutValue = EPBIKRootBehavior::Free;
			return true;
		}
		return false;
	}

	TSharedPtr<FJsonObject> BuildPBIKPrePullSettingsJson(const FRootPrePullSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		Item->SetNumberField(TEXT("rotation_alpha_x"), Settings.RotationAlphaX);
		Item->SetNumberField(TEXT("rotation_alpha_y"), Settings.RotationAlphaY);
		Item->SetNumberField(TEXT("rotation_alpha_z"), Settings.RotationAlphaZ);
		Item->SetNumberField(TEXT("position_alpha"), Settings.PositionAlpha);
		Item->SetNumberField(TEXT("position_alpha_x"), Settings.PositionAlphaX);
		Item->SetNumberField(TEXT("position_alpha_y"), Settings.PositionAlphaY);
		Item->SetNumberField(TEXT("position_alpha_z"), Settings.PositionAlphaZ);
		return Item;
	}

	TSharedPtr<FJsonObject> BuildFBIKSolverSettingsJson(const FIKRigFBIKSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("root_bone"), Settings.RootBone.ToString());
		Item->SetNumberField(TEXT("iterations"), Settings.Iterations);
		Item->SetNumberField(TEXT("sub_iterations"), Settings.SubIterations);
		Item->SetNumberField(TEXT("mass_multiplier"), Settings.MassMultiplier);
		Item->SetBoolField(TEXT("allow_stretch"), Settings.bAllowStretch);
		Item->SetStringField(TEXT("root_behavior"), PBIKRootBehaviorToString(Settings.RootBehavior));
		Item->SetObjectField(TEXT("pre_pull_root_settings"), BuildPBIKPrePullSettingsJson(Settings.PrePullRootSettings));
		Item->SetNumberField(TEXT("global_pull_chain_alpha"), Settings.GlobalPullChainAlpha);
		Item->SetNumberField(TEXT("max_angle"), Settings.MaxAngle);
		Item->SetNumberField(TEXT("over_relaxation"), Settings.OverRelaxation);
		return Item;
	}

	TSharedPtr<FJsonObject> BuildFBIKGoalSettingsJson(const FIKRigFBIKGoalSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("goal_name"), Settings.Goal.ToString());
		Item->SetStringField(TEXT("bone_name"), Settings.BoneName.ToString());
		Item->SetNumberField(TEXT("chain_depth"), Settings.ChainDepth);
		Item->SetNumberField(TEXT("strength_alpha"), Settings.StrengthAlpha);
		Item->SetNumberField(TEXT("pull_chain_alpha"), Settings.PullChainAlpha);
		Item->SetNumberField(TEXT("pin_rotation"), Settings.PinRotation);
		return Item;
	}

	TSharedPtr<FJsonObject> BuildFBIKBoneSettingsJson(const FIKRigFBIKBoneSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("bone_name"), Settings.Bone.ToString());
		Item->SetNumberField(TEXT("rotation_stiffness"), Settings.RotationStiffness);
		Item->SetNumberField(TEXT("position_stiffness"), Settings.PositionStiffness);
		Item->SetBoolField(TEXT("use_preferred_angles"), Settings.bUsePreferredAngles);
		Item->SetObjectField(TEXT("preferred_angles"), BuildVectorJson(Settings.PreferredAngles));
		return Item;
	}

	bool ApplyBodyMoverSolverSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FIKRigBodyMoverSettings& Settings, FString& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		SettingsObject->TryGetNumberField(TEXT("position_alpha"), Settings.PositionAlpha);
		SettingsObject->TryGetNumberField(TEXT("position_positive_x"), Settings.PositionPositiveX);
		SettingsObject->TryGetNumberField(TEXT("position_negative_x"), Settings.PositionNegativeX);
		SettingsObject->TryGetNumberField(TEXT("position_positive_y"), Settings.PositionPositiveY);
		SettingsObject->TryGetNumberField(TEXT("position_negative_y"), Settings.PositionNegativeY);
		SettingsObject->TryGetNumberField(TEXT("position_positive_z"), Settings.PositionPositiveZ);
		SettingsObject->TryGetNumberField(TEXT("position_negative_z"), Settings.PositionNegativeZ);
		SettingsObject->TryGetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		SettingsObject->TryGetNumberField(TEXT("rotate_x_alpha"), Settings.RotateXAlpha);
		SettingsObject->TryGetNumberField(TEXT("rotate_y_alpha"), Settings.RotateYAlpha);
		SettingsObject->TryGetNumberField(TEXT("rotate_z_alpha"), Settings.RotateZAlpha);
		return true;
	}

	bool ApplyPBIKPrePullSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FRootPrePullSettings& Settings, FString& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		SettingsObject->TryGetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		SettingsObject->TryGetNumberField(TEXT("rotation_alpha_x"), Settings.RotationAlphaX);
		SettingsObject->TryGetNumberField(TEXT("rotation_alpha_y"), Settings.RotationAlphaY);
		SettingsObject->TryGetNumberField(TEXT("rotation_alpha_z"), Settings.RotationAlphaZ);
		SettingsObject->TryGetNumberField(TEXT("position_alpha"), Settings.PositionAlpha);
		SettingsObject->TryGetNumberField(TEXT("position_alpha_x"), Settings.PositionAlphaX);
		SettingsObject->TryGetNumberField(TEXT("position_alpha_y"), Settings.PositionAlphaY);
		SettingsObject->TryGetNumberField(TEXT("position_alpha_z"), Settings.PositionAlphaZ);
		return true;
	}

	bool ApplyFBIKSolverSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FIKRigFBIKSettings& Settings, FString& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		double DoubleValue = 0.0;
		if (SettingsObject->TryGetNumberField(TEXT("iterations"), DoubleValue))
		{
			Settings.Iterations = static_cast<int32>(DoubleValue);
		}
		if (SettingsObject->TryGetNumberField(TEXT("sub_iterations"), DoubleValue))
		{
			Settings.SubIterations = static_cast<int32>(DoubleValue);
		}
		SettingsObject->TryGetNumberField(TEXT("mass_multiplier"), Settings.MassMultiplier);
		SettingsObject->TryGetBoolField(TEXT("allow_stretch"), Settings.bAllowStretch);
		SettingsObject->TryGetNumberField(TEXT("global_pull_chain_alpha"), Settings.GlobalPullChainAlpha);
		SettingsObject->TryGetNumberField(TEXT("max_angle"), Settings.MaxAngle);
		SettingsObject->TryGetNumberField(TEXT("over_relaxation"), Settings.OverRelaxation);

		FString RootBehaviorText;
		if (SettingsObject->TryGetStringField(TEXT("root_behavior"), RootBehaviorText))
		{
			if (!ParsePBIKRootBehavior(RootBehaviorText, Settings.RootBehavior))
			{
				OutError = TEXT("invalid_pbik_root_behavior");
				return false;
			}
		}

		const TSharedPtr<FJsonObject>* PrePullSettingsObject = nullptr;
		if (SettingsObject->TryGetObjectField(TEXT("pre_pull_root_settings"), PrePullSettingsObject) && PrePullSettingsObject && PrePullSettingsObject->IsValid())
		{
			if (!ApplyPBIKPrePullSettingsPatch(*PrePullSettingsObject, Settings.PrePullRootSettings, OutError))
			{
				return false;
			}
		}

		return true;
	}

	TSharedPtr<FJsonObject> BuildRetargetRootSettingsJson(const FTargetRootSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		Item->SetNumberField(TEXT("translation_alpha"), Settings.TranslationAlpha);
		Item->SetNumberField(TEXT("blend_to_source"), Settings.BlendToSource);
		Item->SetObjectField(TEXT("blend_to_source_weights"), BuildVectorJson(Settings.BlendToSourceWeights));
		Item->SetNumberField(TEXT("scale_horizontal"), Settings.ScaleHorizontal);
		Item->SetNumberField(TEXT("scale_vertical"), Settings.ScaleVertical);
		Item->SetObjectField(TEXT("translation_offset"), BuildVectorJson(Settings.TranslationOffset));
		Item->SetObjectField(TEXT("rotation_offset"), BuildRotatorJson(Settings.RotationOffset));
		Item->SetNumberField(TEXT("affect_ik_horizontal"), Settings.AffectIKHorizontal);
		Item->SetNumberField(TEXT("affect_ik_vertical"), Settings.AffectIKVertical);
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("settings"),
			TEXT("root_settings"),
			TEXT("Root Settings"),
			TEXT("IK Retargeter/Root Settings"),
			TEXT("retarget_root_settings"),
			TEXT("settings"),
			{ TEXT("set") });
		return Item;
	}

	TSharedPtr<FJsonObject> BuildRetargetGlobalSettingsJson(const FRetargetGlobalSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetBoolField(TEXT("enable_root"), Settings.bEnableRoot);
		Item->SetBoolField(TEXT("enable_fk"), Settings.bEnableFK);
		Item->SetBoolField(TEXT("enable_ik"), Settings.bEnableIK);
		Item->SetBoolField(TEXT("enable_post"), Settings.bEnablePost);
		Item->SetBoolField(TEXT("copy_base_pose"), Settings.bCopyBasePose);
		Item->SetStringField(TEXT("copy_base_pose_root"), Settings.CopyBasePoseRoot.ToString());
		Item->SetNumberField(TEXT("source_scale_factor"), Settings.SourceScaleFactor);
		Item->SetBoolField(TEXT("warping"), Settings.bWarping);
		Item->SetStringField(TEXT("direction_source"), WarpingDirectionSourceToString(Settings.DirectionSource));
		Item->SetStringField(TEXT("forward_direction"), BasicAxisToString(Settings.ForwardDirection));
		Item->SetStringField(TEXT("direction_chain"), Settings.DirectionChain.ToString());
		Item->SetNumberField(TEXT("warp_forwards"), Settings.WarpForwards);
		Item->SetNumberField(TEXT("sideways_offset"), Settings.SidewaysOffset);
		Item->SetNumberField(TEXT("warp_splay"), Settings.WarpSplay);
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("settings"),
			TEXT("global_settings"),
			TEXT("Global Settings"),
			TEXT("IK Retargeter/Global Settings"),
			TEXT("retarget_global_settings"),
			TEXT("settings"),
			{ TEXT("set") });
		return Item;
	}

	TSharedPtr<FJsonObject> BuildRetargetChainFKSettingsJson(const FTargetChainFKSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetBoolField(TEXT("enable_fk"), Settings.EnableFK);
		Item->SetStringField(TEXT("rotation_mode"), RetargetRotationModeToString(Settings.RotationMode));
		Item->SetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		Item->SetStringField(TEXT("translation_mode"), RetargetTranslationModeToString(Settings.TranslationMode));
		Item->SetNumberField(TEXT("translation_alpha"), Settings.TranslationAlpha);
		Item->SetNumberField(TEXT("pole_vector_matching"), Settings.PoleVectorMatching);
		Item->SetBoolField(TEXT("pole_vector_maintain_offset"), Settings.PoleVectorMaintainOffset);
		Item->SetNumberField(TEXT("pole_vector_offset"), Settings.PoleVectorOffset);
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("settings"),
			TEXT("fk"),
			TEXT("FK Settings"),
			TEXT("IK Retargeter/Chain Settings"),
			TEXT("retarget_chain_fk_settings"),
			TEXT("settings"),
			{ TEXT("set") });
		return Item;
	}

	TSharedPtr<FJsonObject> BuildRetargetChainIKSettingsJson(const FTargetChainIKSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetBoolField(TEXT("enable_ik"), Settings.EnableIK);
		Item->SetNumberField(TEXT("blend_to_source"), Settings.BlendToSource);
		Item->SetNumberField(TEXT("blend_to_source_translation"), Settings.BlendToSourceTranslation);
		Item->SetNumberField(TEXT("blend_to_source_rotation"), Settings.BlendToSourceRotation);
		Item->SetObjectField(TEXT("blend_to_source_weights"), BuildVectorJson(Settings.BlendToSourceWeights));
		Item->SetObjectField(TEXT("static_offset"), BuildVectorJson(Settings.StaticOffset));
		Item->SetObjectField(TEXT("static_local_offset"), BuildVectorJson(Settings.StaticLocalOffset));
		Item->SetObjectField(TEXT("static_rotation_offset"), BuildRotatorJson(Settings.StaticRotationOffset));
		Item->SetNumberField(TEXT("scale_vertical"), Settings.ScaleVertical);
		Item->SetNumberField(TEXT("extension"), Settings.Extension);
		Item->SetBoolField(TEXT("affected_by_ik_warping"), Settings.bAffectedByIKWarping);
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("settings"),
			TEXT("ik"),
			TEXT("IK Settings"),
			TEXT("IK Retargeter/Chain Settings"),
			TEXT("retarget_chain_ik_settings"),
			TEXT("settings"),
			{ TEXT("set") });
		return Item;
	}

	TSharedPtr<FJsonObject> BuildRetargetChainSpeedPlantSettingsJson(const FTargetChainSpeedPlantSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetBoolField(TEXT("enable_speed_planting"), Settings.EnableSpeedPlanting);
		Item->SetStringField(TEXT("speed_curve_name"), Settings.SpeedCurveName.ToString());
		Item->SetNumberField(TEXT("speed_threshold"), Settings.SpeedThreshold);
		Item->SetNumberField(TEXT("unplant_stiffness"), Settings.UnplantStiffness);
		Item->SetNumberField(TEXT("unplant_critical_damping"), Settings.UnplantCriticalDamping);
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("settings"),
			TEXT("speed_planting"),
			TEXT("Speed Planting"),
			TEXT("IK Retargeter/Chain Settings"),
			TEXT("retarget_chain_speed_planting_settings"),
			TEXT("settings"),
			{ TEXT("set") });
		return Item;
	}

	TSharedPtr<FJsonObject> BuildRetargetChainSettingsJson(const FTargetChainSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetObjectField(TEXT("fk"), BuildRetargetChainFKSettingsJson(Settings.FK));
		Item->SetObjectField(TEXT("ik"), BuildRetargetChainIKSettingsJson(Settings.IK));
		Item->SetObjectField(TEXT("speed_planting"), BuildRetargetChainSpeedPlantSettingsJson(Settings.SpeedPlanting));
		return Item;
	}

	bool ApplyRetargetRootSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetRootSettings& Settings, FString& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		SettingsObject->TryGetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		SettingsObject->TryGetNumberField(TEXT("translation_alpha"), Settings.TranslationAlpha);
		SettingsObject->TryGetNumberField(TEXT("blend_to_source"), Settings.BlendToSource);
		TryGetVectorObject(SettingsObject, TEXT("blend_to_source_weights"), Settings.BlendToSourceWeights);
		SettingsObject->TryGetNumberField(TEXT("scale_horizontal"), Settings.ScaleHorizontal);
		SettingsObject->TryGetNumberField(TEXT("scale_vertical"), Settings.ScaleVertical);
		TryGetVectorObject(SettingsObject, TEXT("translation_offset"), Settings.TranslationOffset);
		TryGetRotatorObject(SettingsObject, TEXT("rotation_offset"), Settings.RotationOffset);
		SettingsObject->TryGetNumberField(TEXT("affect_ik_horizontal"), Settings.AffectIKHorizontal);
		SettingsObject->TryGetNumberField(TEXT("affect_ik_vertical"), Settings.AffectIKVertical);
		return true;
	}

	bool ApplyRetargetGlobalSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FRetargetGlobalSettings& Settings, FString& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		SettingsObject->TryGetBoolField(TEXT("enable_root"), Settings.bEnableRoot);
		SettingsObject->TryGetBoolField(TEXT("enable_fk"), Settings.bEnableFK);
		SettingsObject->TryGetBoolField(TEXT("enable_ik"), Settings.bEnableIK);
		SettingsObject->TryGetBoolField(TEXT("enable_post"), Settings.bEnablePost);
		SettingsObject->TryGetBoolField(TEXT("copy_base_pose"), Settings.bCopyBasePose);

		FString CopyBasePoseRootText;
		if (SettingsObject->TryGetStringField(TEXT("copy_base_pose_root"), CopyBasePoseRootText))
		{
			Settings.CopyBasePoseRoot = FName(*CopyBasePoseRootText.TrimStartAndEnd());
		}

		SettingsObject->TryGetNumberField(TEXT("source_scale_factor"), Settings.SourceScaleFactor);
		SettingsObject->TryGetBoolField(TEXT("warping"), Settings.bWarping);

		FString DirectionSourceText;
		if (SettingsObject->TryGetStringField(TEXT("direction_source"), DirectionSourceText))
		{
			if (!ParseWarpingDirectionSource(DirectionSourceText, Settings.DirectionSource))
			{
				OutError = TEXT("invalid_warping_direction_source");
				return false;
			}
		}

		FString ForwardDirectionText;
		if (SettingsObject->TryGetStringField(TEXT("forward_direction"), ForwardDirectionText))
		{
			if (!ParseBasicAxis(ForwardDirectionText, Settings.ForwardDirection))
			{
				OutError = TEXT("invalid_forward_direction");
				return false;
			}
		}

		FString DirectionChainText;
		if (SettingsObject->TryGetStringField(TEXT("direction_chain"), DirectionChainText))
		{
			Settings.DirectionChain = FName(*DirectionChainText.TrimStartAndEnd());
		}

		SettingsObject->TryGetNumberField(TEXT("warp_forwards"), Settings.WarpForwards);
		SettingsObject->TryGetNumberField(TEXT("sideways_offset"), Settings.SidewaysOffset);
		SettingsObject->TryGetNumberField(TEXT("warp_splay"), Settings.WarpSplay);
		return true;
	}

	bool ApplyRetargetChainFKSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainFKSettings& Settings, FString& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		SettingsObject->TryGetBoolField(TEXT("enable_fk"), Settings.EnableFK);

		FString RotationModeText;
		if (SettingsObject->TryGetStringField(TEXT("rotation_mode"), RotationModeText))
		{
			if (!ParseRetargetRotationMode(RotationModeText, Settings.RotationMode))
			{
				OutError = TEXT("invalid_retarget_rotation_mode");
				return false;
			}
		}

		FString TranslationModeText;
		if (SettingsObject->TryGetStringField(TEXT("translation_mode"), TranslationModeText))
		{
			if (!ParseRetargetTranslationMode(TranslationModeText, Settings.TranslationMode))
			{
				OutError = TEXT("invalid_retarget_translation_mode");
				return false;
			}
		}

		SettingsObject->TryGetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		SettingsObject->TryGetNumberField(TEXT("translation_alpha"), Settings.TranslationAlpha);
		SettingsObject->TryGetNumberField(TEXT("pole_vector_matching"), Settings.PoleVectorMatching);
		SettingsObject->TryGetBoolField(TEXT("pole_vector_maintain_offset"), Settings.PoleVectorMaintainOffset);
		SettingsObject->TryGetNumberField(TEXT("pole_vector_offset"), Settings.PoleVectorOffset);
		return true;
	}

	bool ApplyRetargetChainIKSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainIKSettings& Settings, FString& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		SettingsObject->TryGetBoolField(TEXT("enable_ik"), Settings.EnableIK);
		SettingsObject->TryGetNumberField(TEXT("blend_to_source"), Settings.BlendToSource);
		SettingsObject->TryGetNumberField(TEXT("blend_to_source_translation"), Settings.BlendToSourceTranslation);
		SettingsObject->TryGetNumberField(TEXT("blend_to_source_rotation"), Settings.BlendToSourceRotation);
		TryGetVectorObject(SettingsObject, TEXT("blend_to_source_weights"), Settings.BlendToSourceWeights);
		TryGetVectorObject(SettingsObject, TEXT("static_offset"), Settings.StaticOffset);
		TryGetVectorObject(SettingsObject, TEXT("static_local_offset"), Settings.StaticLocalOffset);
		TryGetRotatorObject(SettingsObject, TEXT("static_rotation_offset"), Settings.StaticRotationOffset);
		SettingsObject->TryGetNumberField(TEXT("scale_vertical"), Settings.ScaleVertical);
		SettingsObject->TryGetNumberField(TEXT("extension"), Settings.Extension);
		SettingsObject->TryGetBoolField(TEXT("affected_by_ik_warping"), Settings.bAffectedByIKWarping);
		return true;
	}

	bool ApplyRetargetChainSpeedPlantSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainSpeedPlantSettings& Settings, FString& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		SettingsObject->TryGetBoolField(TEXT("enable_speed_planting"), Settings.EnableSpeedPlanting);

		FString SpeedCurveNameText;
		if (SettingsObject->TryGetStringField(TEXT("speed_curve_name"), SpeedCurveNameText))
		{
			Settings.SpeedCurveName = FName(*SpeedCurveNameText.TrimStartAndEnd());
		}

		SettingsObject->TryGetNumberField(TEXT("speed_threshold"), Settings.SpeedThreshold);
		SettingsObject->TryGetNumberField(TEXT("unplant_stiffness"), Settings.UnplantStiffness);
		SettingsObject->TryGetNumberField(TEXT("unplant_critical_damping"), Settings.UnplantCriticalDamping);
		return true;
	}

	bool ApplyRetargetChainSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainSettings& Settings, FString& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* FKObject = nullptr;
		if (SettingsObject->TryGetObjectField(TEXT("fk"), FKObject) && FKObject && FKObject->IsValid())
		{
			if (!ApplyRetargetChainFKSettingsPatch(*FKObject, Settings.FK, OutError))
			{
				return false;
			}
		}

		const TSharedPtr<FJsonObject>* IKObject = nullptr;
		if (SettingsObject->TryGetObjectField(TEXT("ik"), IKObject) && IKObject && IKObject->IsValid())
		{
			if (!ApplyRetargetChainIKSettingsPatch(*IKObject, Settings.IK, OutError))
			{
				return false;
			}
		}

		const TSharedPtr<FJsonObject>* SpeedPlantingObject = nullptr;
		if (SettingsObject->TryGetObjectField(TEXT("speed_planting"), SpeedPlantingObject) && SpeedPlantingObject && SpeedPlantingObject->IsValid())
		{
			if (!ApplyRetargetChainSpeedPlantSettingsPatch(*SpeedPlantingObject, Settings.SpeedPlanting, OutError))
			{
				return false;
			}
		}

		return true;
	}

	TSharedPtr<FJsonObject> BuildChainSettingsEntryJson(const URetargetChainSettings* ChainSettings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("target_chain_name"), ChainSettings ? ChainSettings->TargetChain.ToString() : TEXT(""));
		Item->SetStringField(TEXT("source_chain_name"), ChainSettings ? ChainSettings->SourceChain.ToString() : TEXT(""));
		Item->SetObjectField(TEXT("settings"), BuildRetargetChainSettingsJson(ChainSettings ? ChainSettings->Settings : FTargetChainSettings()));
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("target_chain_name"),
			ChainSettings ? ChainSettings->TargetChain.ToString() : TEXT(""),
			ChainSettings ? ChainSettings->TargetChain.ToString() : TEXT("Chain Settings"),
			TEXT("IK Retargeter/Chain Settings"),
			TEXT("retarget_chain_settings"),
			TEXT("settings"),
			{ TEXT("set") });
		return Item;
	}

	bool ResolveChainMappingOpName(UIKRetargeterController* Controller, const FName TargetChainName, FName& OutOpName)
	{
		if (!Controller)
		{
			return false;
		}

		for (int32 OpIndex = 0; OpIndex < Controller->GetNumRetargetOps(); ++OpIndex)
		{
			const FName CandidateOpName = Controller->GetOpName(OpIndex);
			const FRetargetChainMapping* Mapping = Controller->GetChainMapping(CandidateOpName);
			if (Mapping && Mapping->HasChain(TargetChainName, ERetargetSourceOrTarget::Target))
			{
				OutOpName = CandidateOpName;
				return true;
			}
		}

		const FRetargetChainMapping* DefaultMapping = Controller->GetChainMapping(NAME_None);
		if (DefaultMapping && DefaultMapping->HasChain(TargetChainName, ERetargetSourceOrTarget::Target))
		{
			OutOpName = NAME_None;
			return true;
		}

		return false;
	}

	FAssetData GetAssetDataForObject(UObject* Object)
	{
		if (!Object)
		{
			return FAssetData();
		}
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(Object->GetPathName()));
	}

	void GatherAssetsUnderPath(const FString& PackagePath, TMap<FString, FAssetData>& OutAssetsByObjectPath)
	{
		OutAssetsByObjectPath.Reset();
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> Assets;
		AssetRegistryModule.Get().GetAssetsByPath(FName(*PackagePath), Assets, true);
		for (const FAssetData& Asset : Assets)
		{
			OutAssetsByObjectPath.Add(Asset.GetObjectPathString(), Asset);
		}
	}
}
