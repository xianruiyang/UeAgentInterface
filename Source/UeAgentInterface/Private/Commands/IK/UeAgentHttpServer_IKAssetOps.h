// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UeAgentHttpServer_IKAssets.h"

#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargetSettings.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/RetargetOps/StrideWarpingOp.h"
#include "Rig/IKRigDataTypes.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/Solvers/IKRigBodyMoverSolver.h"
#include "Rig/Solvers/IKRigFullBodyIK.h"

class UFactory;
class UIKRigEffectorGoal;
class USkeletalMesh;

namespace UeAgentIKAssetOps
{
	FString NormalizeAssetPath(const FString& InPath);
	FString ToObjectPath(const FString& InPath);
	UObject* LoadAssetObject(const FString& InPath);
	UIKRigDefinition* LoadIKRig(const FString& InPath);
	UIKRetargeter* LoadIKRetargeter(const FString& InPath);
	USkeletalMesh* LoadSkeletalMesh(const FString& InPath);
	UObject* CreateAssetWithFactory(const FString& AssetPath, UClass* AssetClass, UFactory* Factory, FString& OutError);
	bool SaveAssetPackage(UObject* Asset, FString& OutError);
	FString SanitizeArtifactSegment(const FString& Value);
	FString MakeProbeArtifactPath(const FString& Ext, const FString& RequestId, const FString& Suffix);
	TSharedPtr<FJsonObject> BuildLinearColorJson(const FLinearColor& Color);
	TSharedPtr<FJsonObject> BuildVectorJson(const FVector& Value);
	TSharedPtr<FJsonObject> BuildRotatorJson(const FRotator& Value);
	TSharedPtr<FJsonObject> BuildTransformJson(const FTransform& Value);
	TArray<TSharedPtr<FJsonValue>> TextArrayToJson(const TArray<FText>& Texts);
	TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Strings);
	bool TryGetVectorObject(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue);
	bool TryGetRotatorObject(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue);
	const TCHAR* SourceOrTargetToString(ERetargetSourceOrTarget Value);
	bool ParseSourceOrTarget(const FString& InValue, ERetargetSourceOrTarget& OutValue);
	bool ParseAutoMapType(const FString& InValue, EAutoMapChainType& OutValue);
	bool ParseAutoAlignMethod(const FString& InValue, ERetargetAutoAlignMethod& OutValue);
	bool ParseIKRigGoalSpace(const FString& InValue, EIKRigGoalSpace& OutValue);
	FString IKRigGoalSpaceToString(EIKRigGoalSpace Value);
	FString AutoAlignMethodToString(ERetargetAutoAlignMethod InValue);
	TSharedPtr<FJsonObject> BuildGoalJson(const UIKRigEffectorGoal* Goal);
	FString BasicAxisToString(EBasicAxis Axis);
	bool ParseBasicAxis(const FString& InValue, EBasicAxis& OutValue);
	FString WarpingDirectionSourceToString(EWarpingDirectionSource Source);
	bool ParseWarpingDirectionSource(const FString& InValue, EWarpingDirectionSource& OutValue);
	FString RetargetTranslationModeToString(ERetargetTranslationMode Value);
	bool ParseRetargetTranslationMode(const FString& InValue, ERetargetTranslationMode& OutValue);
	FString RetargetRotationModeToString(ERetargetRotationMode Value);
	bool ParseRetargetRotationMode(const FString& InValue, ERetargetRotationMode& OutValue);
	TSharedPtr<FJsonObject> BuildChainJson(const FBoneChain& Chain);
	TSharedPtr<FJsonObject> BuildChainPairJson(const FRetargetChainPair& Pair);
	TSharedPtr<FJsonObject> BuildRetargetPoseJson(const FIKRetargetPose& Pose);
	TSharedPtr<FJsonObject> BuildBodyMoverSolverSettingsJson(const FIKRigBodyMoverSettings& Settings);
	TSharedPtr<FJsonObject> BuildBodyMoverGoalSettingsJson(const FIKRigBodyMoverGoalSettings& Settings);
	FString PBIKRootBehaviorToString(EPBIKRootBehavior Value);
	bool ParsePBIKRootBehavior(const FString& InValue, EPBIKRootBehavior& OutValue);
	TSharedPtr<FJsonObject> BuildPBIKPrePullSettingsJson(const FRootPrePullSettings& Settings);
	TSharedPtr<FJsonObject> BuildFBIKSolverSettingsJson(const FIKRigFBIKSettings& Settings);
	TSharedPtr<FJsonObject> BuildFBIKGoalSettingsJson(const FIKRigFBIKGoalSettings& Settings);
	TSharedPtr<FJsonObject> BuildFBIKBoneSettingsJson(const FIKRigFBIKBoneSettings& Settings);
	bool ApplyBodyMoverSolverSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FIKRigBodyMoverSettings& Settings, FString& OutError);
	bool ApplyPBIKPrePullSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FRootPrePullSettings& Settings, FString& OutError);
	bool ApplyFBIKSolverSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FIKRigFBIKSettings& Settings, FString& OutError);
	TSharedPtr<FJsonObject> BuildRetargetRootSettingsJson(const FTargetRootSettings& Settings);
	TSharedPtr<FJsonObject> BuildRetargetGlobalSettingsJson(const FRetargetGlobalSettings& Settings);
	TSharedPtr<FJsonObject> BuildRetargetChainFKSettingsJson(const FTargetChainFKSettings& Settings);
	TSharedPtr<FJsonObject> BuildRetargetChainIKSettingsJson(const FTargetChainIKSettings& Settings);
	TSharedPtr<FJsonObject> BuildRetargetChainSpeedPlantSettingsJson(const FTargetChainSpeedPlantSettings& Settings);
	TSharedPtr<FJsonObject> BuildRetargetChainSettingsJson(const FTargetChainSettings& Settings);
	bool ApplyRetargetRootSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetRootSettings& Settings, FString& OutError);
	bool ApplyRetargetGlobalSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FRetargetGlobalSettings& Settings, FString& OutError);
	bool ApplyRetargetChainFKSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainFKSettings& Settings, FString& OutError);
	bool ApplyRetargetChainIKSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainIKSettings& Settings, FString& OutError);
	bool ApplyRetargetChainSpeedPlantSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainSpeedPlantSettings& Settings, FString& OutError);
	bool ApplyRetargetChainSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainSettings& Settings, FString& OutError);
	TSharedPtr<FJsonObject> BuildChainSettingsEntryJson(const URetargetChainSettings* ChainSettings);
	bool ResolveChainMappingOpName(UIKRetargeterController* Controller, FName TargetChainName, FName& OutOpName);
	FAssetData GetAssetDataForObject(UObject* Object);
	void GatherAssetsUnderPath(const FString& PackagePath, TMap<FString, FAssetData>& OutAssetsByObjectPath);
}
