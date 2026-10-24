// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

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

class UIKRigController;
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

	// Native controllers can return success after substituting an unresolved name
	// with None. Keep the request independent of that native normalization.
	UEAGENTINTERFACE_API bool ApplyRetargetRoot(UIKRigController& Controller,
		FName RequestedRoot, UE::UAI::Contract::FDiagnostic& OutError,
		const UE::UAI::Contract::FSourceLocation& Source = UE::UAI::Contract::FSourceLocation(), const FString& TargetPath = FString());
	UEAGENTINTERFACE_API bool ApplyRetargetChainGoal(UIKRigController& Controller,
		FName ChainName, FName RequestedGoal, UE::UAI::Contract::FDiagnostic& OutError,
		const UE::UAI::Contract::FSourceLocation& Source = UE::UAI::Contract::FSourceLocation(), const FString& TargetPath = FString());
	UEAGENTINTERFACE_API bool ApplySolverBone(UIKRigController& Controller,
		int32 SolverIndex, FName RequestedBone, bool bEndBone, UE::UAI::Contract::FDiagnostic& OutError);
	struct FRetargetMappingDiagnosticSource
	{
		FString TargetPath;
		UE::UAI::Contract::FSourceLocation Mapping;
		UE::UAI::Contract::FSourceLocation SourceChain;
		UE::UAI::Contract::FSourceLocation TargetChain;
	};
	UEAGENTINTERFACE_API bool ApplyRetargetChainMapping(UIKRetargeterController& Controller,
		FName SourceChain, FName TargetChain, FName OpName, UE::UAI::Contract::FDiagnostic& OutError,
		const FRetargetMappingDiagnosticSource& Source = FRetargetMappingDiagnosticSource());
	UEAGENTINTERFACE_API bool VerifyRetargetChainMapping(UIKRetargeterController& Controller,
		FName SourceChain, FName TargetChain, FName OpName, UE::UAI::Contract::FDiagnostic& OutError,
		const FRetargetMappingDiagnosticSource& Source = FRetargetMappingDiagnosticSource());
	struct FRetargetPoseDiagnosticSource
	{
		FString TargetPath;
		UE::UAI::Contract::FSourceLocation Pose;
		UE::UAI::Contract::FSourceLocation RootTranslation;
		UE::UAI::Contract::FSourceLocation Rotations;
		TMap<FName, UE::UAI::Contract::FSourceLocation> BoneRotations;
	};
	UEAGENTINTERFACE_API bool ApplyRetargetPoseRotation(UIKRetargeterController& Controller,
		ERetargetSourceOrTarget Side, FName Bone, const FQuat& Rotation, UE::UAI::Contract::FDiagnostic& OutError,
		const FRetargetPoseDiagnosticSource& Source = FRetargetPoseDiagnosticSource());
	UEAGENTINTERFACE_API bool ApplyRetargetPoseRootTranslation(UIKRetargeterController& Controller,
		ERetargetSourceOrTarget Side, const FVector& Translation, UE::UAI::Contract::FDiagnostic& OutError,
		const FRetargetPoseDiagnosticSource& Source = FRetargetPoseDiagnosticSource());
	// Compare requested native values after CleanAsset, not a recapture of the applied result.
	UEAGENTINTERFACE_API bool VerifyRetargetPose(UIKRetargeterController& Controller,
		ERetargetSourceOrTarget Side, FName PoseName, const FVector& RootTranslation,
		const TMap<FName, FQuat>& BoneRotations, UE::UAI::Contract::FDiagnostic& OutError,
		const FRetargetPoseDiagnosticSource& Source = FRetargetPoseDiagnosticSource());
	bool SaveAssetPackage(UObject* Asset, UE::UAI::Contract::FDiagnostic& OutError);
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
	bool ApplyBodyMoverSolverSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FIKRigBodyMoverSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError);
	bool ApplyPBIKPrePullSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FRootPrePullSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError);
	bool ApplyFBIKSolverSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FIKRigFBIKSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError);
	TSharedPtr<FJsonObject> BuildRetargetRootSettingsJson(const FTargetRootSettings& Settings);
	TSharedPtr<FJsonObject> BuildRetargetGlobalSettingsJson(const FRetargetGlobalSettings& Settings);
	TSharedPtr<FJsonObject> BuildRetargetChainFKSettingsJson(const FTargetChainFKSettings& Settings);
	TSharedPtr<FJsonObject> BuildRetargetChainIKSettingsJson(const FTargetChainIKSettings& Settings);
	TSharedPtr<FJsonObject> BuildRetargetChainSpeedPlantSettingsJson(const FTargetChainSpeedPlantSettings& Settings);
	TSharedPtr<FJsonObject> BuildRetargetChainSettingsJson(const FTargetChainSettings& Settings);
	bool ApplyRetargetRootSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetRootSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError);
	bool ApplyRetargetGlobalSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FRetargetGlobalSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError);
	bool ApplyRetargetChainFKSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainFKSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError);
	bool ApplyRetargetChainIKSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainIKSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError);
	bool ApplyRetargetChainSpeedPlantSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainSpeedPlantSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError);
	bool ApplyRetargetChainSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError);
	TSharedPtr<FJsonObject> BuildChainSettingsEntryJson(const URetargetChainSettings* ChainSettings);
	bool ResolveChainMappingOpName(UIKRetargeterController* Controller, FName TargetChainName, FName& OutOpName);
	FAssetData GetAssetDataForObject(UObject* Object);
	void GatherAssetsUnderPath(const FString& PackagePath, TMap<FString, FAssetData>& OutAssetsByObjectPath);
}
