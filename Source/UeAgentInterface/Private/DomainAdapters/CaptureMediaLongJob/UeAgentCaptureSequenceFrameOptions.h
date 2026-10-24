// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::Contract { struct FDiagnostic; }

namespace UeAgentCaptureSequenceFrameOptions
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
		// Canonical top-level pointer -> original command pointer, recorded only when a value is copied.
		TMap<FString, FString> InputOrigins;
		bool bHadVisualOptions;
		bool bHadPreviewState;

		bool HasAnyInput() const;
		void LocateDiagnosticSource(UE::UAI::Contract::FDiagnostic& Diagnostic) const;
	};

	void ApplyUnifiedSequenceFrameOptionsToParams(
		const TSharedPtr<FJsonObject>& Params,
		ESequenceFrameCommandKind Kind,
		FUnifiedSequenceFrameOptionsReport& Report);

	void AttachUnifiedSequenceFrameOptionsReport(
		const TSharedPtr<FJsonObject>& OutData,
		const FUnifiedSequenceFrameOptionsReport& Report);
}
