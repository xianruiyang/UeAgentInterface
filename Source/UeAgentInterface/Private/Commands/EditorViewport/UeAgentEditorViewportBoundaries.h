#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UeAgentEditorViewport
{
	struct FSpecialPreviewBoundary
	{
		FSpecialPreviewBoundary();

		FString Kind;
		FString PreviewModel;
		FString ErrorCode;
		FString RecommendedCommandFamily;
		FString Status;
		TArray<FString> PlannedCapabilities;
		bool bEditorViewport3DCamera;
		bool bEditorViewport3DFocus;
	};

	bool TryGetSpecialPreviewBoundaryForAssetType(const FString& InAssetType, FSpecialPreviewBoundary& OutBoundary);
	bool TryGetExcludedEditorWindowBoundaryForAssetType(const FString& InAssetType, FSpecialPreviewBoundary& OutBoundary);
	bool TryGetExcludedEditorWindowBoundaryForViewportRole(const FString& InViewportRole, const FString& InAssetType, FSpecialPreviewBoundary& OutBoundary);
	bool TryGetEditorViewportBoundaryForAssetType(const FString& InAssetType, FSpecialPreviewBoundary& OutBoundary);
	TSharedPtr<FJsonObject> MakeEditorViewportBoundaryJson(const FSpecialPreviewBoundary& Boundary);
	void ApplyEditorViewportBoundaryToCandidate(TSharedPtr<FJsonObject> Candidate, const FSpecialPreviewBoundary& Boundary);
}
