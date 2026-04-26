// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_IKAssets.h"

#include "UeAgentJsonDiagnostics.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Developer/AssetTools/Public/IAssetTools.h"
#include "EditorAnimUtils.h"
#include "Engine/SkeletalMesh.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargetFactory.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargetDeprecated.h"
#include "Retargeter/IKRetargetSettings.h"
#include "Retargeter/RetargetOps/StrideWarpingOp.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/Solvers/IKRigBodyMoverSolver.h"
#include "Rig/Solvers/IKRigFullBodyIK.h"
#include "RigEditor/IKRigController.h"
#include "RigEditor/IKRigDefinitionFactory.h"

namespace UeAgentIKAssetOps
{
	static FString NormalizeAssetPath(const FString& InPath)
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

	static FString ToObjectPath(const FString& InPath)
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

	static UObject* LoadAssetObject(const FString& InPath)
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

	static UIKRigDefinition* LoadIKRig(const FString& InPath)
	{
		return Cast<UIKRigDefinition>(LoadAssetObject(InPath));
	}

	static UIKRetargeter* LoadIKRetargeter(const FString& InPath)
	{
		return Cast<UIKRetargeter>(LoadAssetObject(InPath));
	}

	static USkeletalMesh* LoadSkeletalMesh(const FString& InPath)
	{
		return Cast<USkeletalMesh>(LoadAssetObject(InPath));
	}

	static UObject* CreateAssetWithFactory(const FString& AssetPath, UClass* AssetClass, UFactory* Factory, FString& OutError)
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

	static bool SaveAssetPackage(UObject* Asset, FString& OutError)
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

	static TSharedPtr<FJsonObject> BuildLinearColorJson(const FLinearColor& Color)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("r"), Color.R);
		Obj->SetNumberField(TEXT("g"), Color.G);
		Obj->SetNumberField(TEXT("b"), Color.B);
		Obj->SetNumberField(TEXT("a"), Color.A);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildRotatorJson(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

	static bool TryGetVectorObject(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* ValueObject = nullptr;
		if (!Obj->TryGetObjectField(Key, ValueObject) || !ValueObject || !ValueObject->IsValid())
		{
			return false;
		}

		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ValueObject, { TEXT("x"), TEXT("X") }, X)
			|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ValueObject, { TEXT("y"), TEXT("Y") }, Y)
			|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ValueObject, { TEXT("z"), TEXT("Z") }, Z))
		{
			return false;
		}

		OutValue = FVector(X, Y, Z);
		return true;
	}

	static bool TryGetRotatorObject(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* ValueObject = nullptr;
		if (!Obj->TryGetObjectField(Key, ValueObject) || !ValueObject || !ValueObject->IsValid())
		{
			return false;
		}

		double Pitch = 0.0;
		double Yaw = 0.0;
		double Roll = 0.0;
		if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ValueObject, { TEXT("pitch"), TEXT("Pitch") }, Pitch)
			|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ValueObject, { TEXT("yaw"), TEXT("Yaw") }, Yaw)
			|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ValueObject, { TEXT("roll"), TEXT("Roll") }, Roll))
		{
			return false;
		}

		OutValue = FRotator(Pitch, Yaw, Roll);
		return true;
	}

	static const TCHAR* SourceOrTargetToString(const ERetargetSourceOrTarget Value)
	{
		return Value == ERetargetSourceOrTarget::Source ? TEXT("source") : TEXT("target");
	}

	static bool ParseSourceOrTarget(const FString& InValue, ERetargetSourceOrTarget& OutValue)
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

	static bool ParseAutoMapType(const FString& InValue, EAutoMapChainType& OutValue)
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

	static bool ParseAutoAlignMethod(const FString& InValue, ERetargetAutoAlignMethod& OutValue)
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

	static FString AutoAlignMethodToString(const ERetargetAutoAlignMethod InValue)
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

	static TSharedPtr<FJsonObject> BuildGoalJson(const UIKRigEffectorGoal* Goal)
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
		return Item;
	}

	static FString BasicAxisToString(const EBasicAxis Axis)
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

	static bool ParseBasicAxis(const FString& InValue, EBasicAxis& OutValue)
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

	static FString WarpingDirectionSourceToString(const EWarpingDirectionSource Source)
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

	static bool ParseWarpingDirectionSource(const FString& InValue, EWarpingDirectionSource& OutValue)
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

	static FString RetargetTranslationModeToString(const ERetargetTranslationMode Value)
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

	static bool ParseRetargetTranslationMode(const FString& InValue, ERetargetTranslationMode& OutValue)
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

	static FString RetargetRotationModeToString(const ERetargetRotationMode Value)
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

	static bool ParseRetargetRotationMode(const FString& InValue, ERetargetRotationMode& OutValue)
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

	static TSharedPtr<FJsonObject> BuildChainJson(const FBoneChain& Chain)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("chain_name"), Chain.ChainName.ToString());
		Item->SetStringField(TEXT("start_bone_name"), Chain.StartBone.BoneName.ToString());
		Item->SetStringField(TEXT("end_bone_name"), Chain.EndBone.BoneName.ToString());
		Item->SetStringField(TEXT("goal_name"), Chain.IKGoalName.ToString());
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildChainPairJson(const FRetargetChainPair& Pair)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("target_chain_name"), Pair.TargetChainName.ToString());
		Item->SetStringField(TEXT("source_chain_name"), Pair.SourceChainName.ToString());
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildRetargetPoseJson(const FIKRetargetPose& Pose)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetObjectField(TEXT("root_offset"), BuildVectorJson(Pose.GetRootTranslationDelta()));

		TArray<TSharedPtr<FJsonValue>> BoneRotationOffsets;
		for (const TPair<FName, FQuat>& Pair : Pose.GetAllDeltaRotations())
		{
			TSharedPtr<FJsonObject> BoneObject = MakeShared<FJsonObject>();
			BoneObject->SetStringField(TEXT("bone_name"), Pair.Key.ToString());
			BoneObject->SetObjectField(TEXT("rotation_offset"), BuildRotatorJson(Pair.Value.Rotator()));
			BoneRotationOffsets.Add(MakeShared<FJsonValueObject>(BoneObject));
		}
		Item->SetNumberField(TEXT("bone_rotation_offset_count"), BoneRotationOffsets.Num());
		Item->SetArrayField(TEXT("bone_rotation_offsets"), BoneRotationOffsets);
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildBodyMoverSolverSettingsJson(const FIKRigBodyMoverSettings& Settings)
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

	static TSharedPtr<FJsonObject> BuildBodyMoverGoalSettingsJson(const FIKRigBodyMoverGoalSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("goal_name"), Settings.Goal.ToString());
		Item->SetStringField(TEXT("bone_name"), Settings.BoneName.ToString());
		Item->SetNumberField(TEXT("influence_multiplier"), Settings.InfluenceMultiplier);
		return Item;
	}

	static FString PBIKRootBehaviorToString(const EPBIKRootBehavior Value)
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

	static bool ParsePBIKRootBehavior(const FString& InValue, EPBIKRootBehavior& OutValue)
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

	static TSharedPtr<FJsonObject> BuildPBIKPrePullSettingsJson(const FRootPrePullSettings& Settings)
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

	static TSharedPtr<FJsonObject> BuildFBIKSolverSettingsJson(const FIKRigFBIKSettings& Settings)
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

	static TSharedPtr<FJsonObject> BuildFBIKGoalSettingsJson(const FIKRigFBIKGoalSettings& Settings)
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

	static TSharedPtr<FJsonObject> BuildFBIKBoneSettingsJson(const FIKRigFBIKBoneSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("bone_name"), Settings.Bone.ToString());
		Item->SetNumberField(TEXT("rotation_stiffness"), Settings.RotationStiffness);
		Item->SetNumberField(TEXT("position_stiffness"), Settings.PositionStiffness);
		Item->SetBoolField(TEXT("use_preferred_angles"), Settings.bUsePreferredAngles);
		Item->SetObjectField(TEXT("preferred_angles"), BuildVectorJson(Settings.PreferredAngles));
		return Item;
	}

	static bool ApplyBodyMoverSolverSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FIKRigBodyMoverSettings& Settings, FString& OutError)
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

	static bool ApplyPBIKPrePullSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FRootPrePullSettings& Settings, FString& OutError)
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

	static bool ApplyFBIKSolverSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FIKRigFBIKSettings& Settings, FString& OutError)
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

	static TSharedPtr<FJsonObject> BuildRetargetRootSettingsJson(const FTargetRootSettings& Settings)
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
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildRetargetGlobalSettingsJson(const FRetargetGlobalSettings& Settings)
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
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildRetargetChainFKSettingsJson(const FTargetChainFKSettings& Settings)
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
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildRetargetChainIKSettingsJson(const FTargetChainIKSettings& Settings)
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
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildRetargetChainSpeedPlantSettingsJson(const FTargetChainSpeedPlantSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetBoolField(TEXT("enable_speed_planting"), Settings.EnableSpeedPlanting);
		Item->SetStringField(TEXT("speed_curve_name"), Settings.SpeedCurveName.ToString());
		Item->SetNumberField(TEXT("speed_threshold"), Settings.SpeedThreshold);
		Item->SetNumberField(TEXT("unplant_stiffness"), Settings.UnplantStiffness);
		Item->SetNumberField(TEXT("unplant_critical_damping"), Settings.UnplantCriticalDamping);
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildRetargetChainSettingsJson(const FTargetChainSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetObjectField(TEXT("fk"), BuildRetargetChainFKSettingsJson(Settings.FK));
		Item->SetObjectField(TEXT("ik"), BuildRetargetChainIKSettingsJson(Settings.IK));
		Item->SetObjectField(TEXT("speed_planting"), BuildRetargetChainSpeedPlantSettingsJson(Settings.SpeedPlanting));
		return Item;
	}

	static bool ApplyRetargetRootSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetRootSettings& Settings, FString& OutError)
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

	static bool ApplyRetargetGlobalSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FRetargetGlobalSettings& Settings, FString& OutError)
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

	static bool ApplyRetargetChainFKSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainFKSettings& Settings, FString& OutError)
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

	static bool ApplyRetargetChainIKSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainIKSettings& Settings, FString& OutError)
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

	static bool ApplyRetargetChainSpeedPlantSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainSpeedPlantSettings& Settings, FString& OutError)
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

	static bool ApplyRetargetChainSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainSettings& Settings, FString& OutError)
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

	static TSharedPtr<FJsonObject> BuildChainSettingsEntryJson(const URetargetChainSettings* ChainSettings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("target_chain_name"), ChainSettings ? ChainSettings->TargetChain.ToString() : TEXT(""));
		Item->SetStringField(TEXT("source_chain_name"), ChainSettings ? ChainSettings->SourceChain.ToString() : TEXT(""));
		Item->SetObjectField(TEXT("settings"), BuildRetargetChainSettingsJson(ChainSettings ? ChainSettings->Settings : FTargetChainSettings()));
		return Item;
	}

	static bool ResolveChainMappingOpName(UIKRetargeterController* Controller, const FName TargetChainName, FName& OutOpName)
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

	static FAssetData GetAssetDataForObject(UObject* Object)
	{
		if (!Object)
		{
			return FAssetData();
		}
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(Object->GetPathName()));
	}

	static void GatherAssetsUnderPath(const FString& PackagePath, TMap<FString, FAssetData>& OutAssetsByObjectPath)
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

bool FUeAgentHttpServer::ExecuteIKRigCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("ik_rig_create")) return CmdIKRigCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_get_info")) return CmdIKRigGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_set_solver")) return CmdIKRigSetSolver(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_set_preview_mesh")) return CmdIKRigSetPreviewMesh(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_set_goal")) return CmdIKRigSetGoal(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_set_retarget_root")) return CmdIKRigSetRetargetRoot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_set_retarget_chain")) return CmdIKRigSetRetargetChain(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_rig_apply_auto_retarget_definition")) return CmdIKRigApplyAutoRetargetDefinition(Ctx, OutData, OutError);

	OutError = TEXT("unknown_ik_rig_command");
	return false;
}

bool FUeAgentHttpServer::ExecuteIKRetargeterCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("ik_retargeter_create")) return CmdIKRetargeterCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_get_info")) return CmdIKRetargeterGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_set_ik_rig")) return CmdIKRetargeterSetIKRig(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_set_settings")) return CmdIKRetargeterSetSettings(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_set_pose")) return CmdIKRetargeterSetPose(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_set_preview_mesh")) return CmdIKRetargeterSetPreviewMesh(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_auto_map_chains")) return CmdIKRetargeterAutoMapChains(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ik_retargeter_duplicate_and_retarget")) return CmdIKRetargeterDuplicateAndRetarget(Ctx, OutData, OutError);

	OutError = TEXT("unknown_ik_retargeter_command");
	return false;
}

bool FUeAgentHttpServer::CmdIKRigCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdIKRigGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdIKRigSetSolver(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	if (JsonTryGetString(Ctx.Params, TEXT("start_bone_name"), StartBoneNameText) && !StartBoneNameText.TrimStartAndEnd().IsEmpty())
	{
		if (!Controller->SetStartBone(FName(*StartBoneNameText.TrimStartAndEnd()), SolverIndex))
		{
			OutError = TEXT("set_solver_start_bone_failed");
			return false;
		}
	}

	FString EndBoneNameText;
	if (JsonTryGetString(Ctx.Params, TEXT("end_bone_name"), EndBoneNameText) && !EndBoneNameText.TrimStartAndEnd().IsEmpty())
	{
		if (!Controller->SetEndBone(FName(*EndBoneNameText.TrimStartAndEnd()), SolverIndex))
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

bool FUeAgentHttpServer::CmdIKRigSetPreviewMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdIKRigSetGoal(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
			if (!Controller->SetGoalBone(GoalName, BoneName))
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

bool FUeAgentHttpServer::CmdIKRigSetRetargetRoot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdIKRigSetRetargetChain(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

		if (!ExistingChain)
		{
			if (StartBoneNameText.TrimStartAndEnd().IsEmpty() || EndBoneNameText.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("missing_start_or_end_bone_name");
				return false;
			}
			ChainName = Controller->AddRetargetChain(
				ChainName,
				FName(*StartBoneNameText.TrimStartAndEnd()),
				FName(*EndBoneNameText.TrimStartAndEnd()),
				GoalNameText.TrimStartAndEnd().IsEmpty() ? NAME_None : FName(*GoalNameText.TrimStartAndEnd()));
			if (ChainName.IsNone())
			{
				OutError = TEXT("add_retarget_chain_failed");
				return false;
			}
		}

		FString NewChainNameText;
		if (JsonTryGetString(Ctx.Params, TEXT("new_chain_name"), NewChainNameText) && !NewChainNameText.TrimStartAndEnd().IsEmpty())
		{
			ChainName = Controller->RenameRetargetChain(ChainName, FName(*NewChainNameText.TrimStartAndEnd()));
			if (ChainName.IsNone())
			{
				OutError = TEXT("rename_retarget_chain_failed");
				return false;
			}
		}

		if (!StartBoneNameText.TrimStartAndEnd().IsEmpty() && !Controller->SetRetargetChainStartBone(ChainName, FName(*StartBoneNameText.TrimStartAndEnd())))
		{
			OutError = TEXT("set_retarget_chain_start_bone_failed");
			return false;
		}
		if (!EndBoneNameText.TrimStartAndEnd().IsEmpty() && !Controller->SetRetargetChainEndBone(ChainName, FName(*EndBoneNameText.TrimStartAndEnd())))
		{
			OutError = TEXT("set_retarget_chain_end_bone_failed");
			return false;
		}
		if (!GoalNameText.TrimStartAndEnd().IsEmpty() && !Controller->SetRetargetChainGoal(ChainName, FName(*GoalNameText.TrimStartAndEnd())))
		{
			OutError = TEXT("set_retarget_chain_goal_failed");
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

bool FUeAgentHttpServer::CmdIKRigApplyAutoRetargetDefinition(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdIKRetargeterCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdIKRetargeterGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdIKRetargeterSetIKRig(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdIKRetargeterSetSettings(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdIKRetargeterSetPose(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdIKRetargeterSetPreviewMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdIKRetargeterAutoMapChains(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

bool FUeAgentHttpServer::CmdIKRetargeterDuplicateAndRetarget(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
