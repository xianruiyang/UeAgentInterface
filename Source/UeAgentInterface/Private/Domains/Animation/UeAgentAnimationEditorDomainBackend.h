// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ReferenceSkeleton.h"
#include "CoreMinimal.h"
#include "Misc/FrameRate.h"

class FJsonObject;
class FJsonValue;
class FLevelEditorViewportClient;
class UAnimSequence;

namespace UeAgentAnimationEditorOps
{
	struct FPreviewFrameRequest
	{
		int32 FrameIndex = 0;
		float TimeSeconds = 0.0f;
	};

	struct FAnimSequenceBoneTrackInput
	{
		FName BoneName;
		TArray<FVector> Positions;
		TArray<FQuat> Rotations;
		TArray<FVector> Scales;
		int32 ProvidedKeyCount = 0;
		bool bUsedExplicitFrames = false;
	};

	bool JsonTryGetNumberAnyField(
		const TSharedPtr<FJsonObject>& Obj,
		const TArray<FString>& Keys,
		double& OutValue);
	bool JsonTryGetStringAnyField(
		const TSharedPtr<FJsonObject>& Obj,
		const TArray<FString>& Keys,
		FString& OutValue);
	bool JsonTryGetArrayAnyField(
		const TSharedPtr<FJsonObject>& Obj,
		const TArray<FString>& Keys,
		const TArray<TSharedPtr<FJsonValue>>*& OutArray);
	bool ResolvePreviewFrameSequence(
		UAnimSequence* Sequence,
		const TSharedPtr<FJsonObject>& Params,
		TArray<FPreviewFrameRequest>& OutFrames,
		FString& OutError);
	bool ResolveAnimFrameRate(
		const TSharedPtr<FJsonObject>& Params,
		const FFrameRate& Fallback,
		FFrameRate& OutFrameRate,
		FString& OutError);
	bool ParseAnimSequenceBoneTrackInput(
		const TSharedPtr<FJsonObject>& TrackObj,
		const FReferenceSkeleton& RefSkeleton,
		int32 RequiredKeyCount,
		const FFrameRate& FrameRate,
		FAnimSequenceBoneTrackInput& OutTrack,
		FString& OutError);
}

namespace UeAgentAnimationViewportOps
{
	FLevelEditorViewportClient* GetPreferredLevelViewportClient();
}
