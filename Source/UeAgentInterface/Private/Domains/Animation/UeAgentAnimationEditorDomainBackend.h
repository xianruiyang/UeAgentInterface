// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

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
}

namespace UeAgentAnimationViewportOps
{
	FLevelEditorViewportClient* GetPreferredLevelViewportClient();
}
