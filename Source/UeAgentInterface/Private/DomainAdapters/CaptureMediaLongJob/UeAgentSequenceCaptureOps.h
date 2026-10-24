// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "MovieScene.h"

class ULevelSequence;
class FLevelEditorViewportClient;
class FJsonObject;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
struct FLevelSequenceFrameRequest
{
	int32 FrameIndex = 0;
	double TimeSeconds = 0.0;
	FFrameNumber EvaluationFrame;
};

ULevelSequence* LoadLevelSequence(const FString& InPath);
bool TryGetSequenceNumberAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, double& OutValue, FString* OutField = nullptr);
bool AddLevelSequenceFrameRequest(UMovieScene* MovieScene, double RelativeTimeSeconds, int32 FrameIndex, TArray<FLevelSequenceFrameRequest>& OutFrames, UE::UAI::Contract::FDiagnostic& OutError, const FString& InputPointer);
bool ResolveLevelSequenceFrameSequence(ULevelSequence* Sequence, const TSharedPtr<FJsonObject>& Params, TArray<FLevelSequenceFrameRequest>& OutFrames, UE::UAI::Contract::FDiagnostic& OutError);
FLevelEditorViewportClient* GetPreferredLevelViewportClient();
}
