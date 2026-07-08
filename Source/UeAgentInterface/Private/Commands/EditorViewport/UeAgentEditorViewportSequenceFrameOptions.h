// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UeAgentEditorViewportSequenceFrameOptions
{
	enum class ESequenceFrameCommandKind : uint8
	{
		Niagara,
		LevelSequence,
		AnimSequence,
		ActorProbe,
		IKProbeCandidates,
		IKProbeReviewFrames
	};

	struct FUnifiedSequenceFrameOptionsReport
	{
		FUnifiedSequenceFrameOptionsReport();

		TSharedPtr<FJsonObject> VisualOptionsApplied;
		TSharedPtr<FJsonObject> PreviewStateApplied;
		TArray<FString> VisualOptionsIgnored;
		TArray<FString> PreviewStateIgnored;
		TArray<FString> Conflicts;
		bool bHadVisualOptions;
		bool bHadPreviewState;

		bool HasAnyInput() const;
	};

	void ApplyUnifiedSequenceFrameOptionsToParams(
		const TSharedPtr<FJsonObject>& Params,
		ESequenceFrameCommandKind Kind,
		FUnifiedSequenceFrameOptionsReport& Report);

	void AttachUnifiedSequenceFrameOptionsReport(
		const TSharedPtr<FJsonObject>& OutData,
		const FUnifiedSequenceFrameOptionsReport& Report);
}
