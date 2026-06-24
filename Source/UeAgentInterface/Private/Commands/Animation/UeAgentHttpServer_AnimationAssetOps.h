// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimEnums.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "Animation/VariableFrameStrippingSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

class FProperty;
class UAnimMetaData;
class UAnimBoneCompressionSettings;
class UAnimCurveCompressionSettings;
class UAnimSequence;
class UAnimSequenceBase;
class UClass;
class UObject;
class USkeletalMesh;
class USkeletalMeshSocket;
class USkeleton;
class UVariableFrameStrippingSettings;
struct FAnimNotifyEvent;
struct FAnimSyncMarker;
struct FVirtualBone;

namespace UeAgentAnimationAssetOps
{
	FString NormalizeAssetPath(const FString& InPath);
	UObject* LoadAssetObject(const FString& InPath);
	UAnimSequence* LoadAnimSequence(const FString& InPath);
	UAnimSequence* CreateAnimSequenceAsset(const FString& AssetPathInput, USkeleton* Skeleton, USkeletalMesh* PreviewMesh, FString& OutError);
	USkeleton* LoadSkeleton(const FString& InPath);
	USkeletalMesh* LoadSkeletalMesh(const FString& InPath);
	UAnimBoneCompressionSettings* LoadBoneCompressionSettings(const FString& InPath);
	UAnimCurveCompressionSettings* LoadCurveCompressionSettings(const FString& InPath);
	UVariableFrameStrippingSettings* LoadVariableFrameStrippingSettings(const FString& InPath);
	UClass* LoadClassByPath(const FString& InPath);
	bool SaveAssetPackage(UObject* Asset, FString& OutError);
	TSharedPtr<FJsonObject> BuildVectorJson(const FVector& Value);
	TSharedPtr<FJsonObject> BuildRotatorJson(const FRotator& Value);
	bool TryParseLinearColorField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FLinearColor& OutColor);
	const TCHAR* InterpolationTypeToString(const EAnimInterpolationType Type);
	bool ParseInterpolationType(const FString& InValue, EAnimInterpolationType& OutType);
	const TCHAR* CurveTypeToString(const ERawCurveTrackTypes Type);
	bool ParseCurveType(const FString& InValue, ERawCurveTrackTypes& OutType);
	const TCHAR* AdditiveAnimationTypeToString(const EAdditiveAnimationType Type);
	bool ParseAdditiveAnimationType(const FString& InValue, EAdditiveAnimationType& OutType);
	const TCHAR* AdditiveBasePoseTypeToString(const EAdditiveBasePoseType Type);
	bool ParseAdditiveBasePoseType(const FString& InValue, EAdditiveBasePoseType& OutType);
	const TCHAR* RootMotionLockTypeToString(const ERootMotionRootLock::Type Type);
	bool ParseRootMotionLockType(const FString& InValue, ERootMotionRootLock::Type& OutType);
	bool ApplyNotifyCommonSettings(const TSharedPtr<FJsonObject>& Params, FAnimNotifyEvent& Notify, FString& OutError);
	TSharedPtr<FJsonObject> BuildNotifyJson(const UAnimSequenceBase* Sequence, const FAnimNotifyEvent& Notify, const int32 NotifyIndex);
	TSharedPtr<FJsonObject> BuildSyncMarkerJson(const UAnimSequence* Sequence, const FAnimSyncMarker& Marker, const int32 MarkerIndex);
	TSharedPtr<FJsonObject> BuildCurveJson(UAnimSequenceBase* Sequence, const FName CurveName, const ERawCurveTrackTypes CurveType, const bool bIncludeKeys);
	TSharedPtr<FJsonObject> BuildSocketJson(const USkeletalMeshSocket* Socket);
	TSharedPtr<FJsonObject> BuildVirtualBoneJson(const FVirtualBone& VirtualBone);
	TSharedPtr<FJsonObject> BuildMetadataJson(const UAnimMetaData* MetaDataObject);
	FProperty* FindEditablePropertyByFlexibleName(UClass* Class, const FString& InName);
	bool ApplySimpleJsonValueToProperty(UObject* TargetObject, FProperty* Property, const TSharedPtr<FJsonValue>& JsonValue, FString& OutError);
	bool EnsureNotifyTrack(UAnimSequenceBase* Sequence, const FName TrackName, int32& OutTrackIndex);
	USkeletalMeshSocket* FindSocketByName(USkeleton* Skeleton, const FName SocketName);
	const FVirtualBone* FindVirtualBoneByName(const USkeleton* Skeleton, const FName VirtualBoneName);
}
