// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"

class FEditorViewportClient;
class FJsonObject;

namespace UeAgentViewportViewMode
{
	enum class EViewportVisualizationFamily : uint8
	{
		None, Buffer, Nanite, Lumen, Substrate, Groom, VirtualShadowMap, VirtualTexture,
		ActorColoration, GPUSkinCache, RayTracingDebug,
	};
	struct FResolvedViewportViewMode
	{
		FResolvedViewportViewMode();
		FString Key; FString Category; FString Label;
		EViewModeIndex ViewMode;
		EViewportVisualizationFamily VisualizationFamily;
		FName VisualizationMode;
		bool bFinalLitComposition;
	};
	struct FViewportViewModeState
	{
		FViewportViewModeState();
		EViewModeIndex ViewMode;
		FName BufferMode; FName NaniteMode; FName LumenMode; FName SubstrateMode; FName GroomMode;
		FName VirtualShadowMapMode; FName VirtualTextureMode; FName ActorColorationMode;
		FName GPUSkinCacheMode; FName RayTracingDebugMode;
	};
	FString ViewModeIndexToStableName(EViewModeIndex ViewMode);
	FString ViewModeIndexToDisplayName(EViewModeIndex ViewMode);
	bool IsFinalLitCompositionViewMode(EViewModeIndex ViewMode);
	bool TryResolveRequestedViewportViewMode(const TSharedPtr<FJsonObject>& Params, bool& bOutHasViewMode, FResolvedViewportViewMode& OutResolved, UE::UAI::Contract::FDiagnostic& OutError);
	void ApplyResolvedViewportViewMode(FEditorViewportClient* ViewportClient, const FResolvedViewportViewMode& Resolved);
	FViewportViewModeState CaptureViewportViewModeState(FEditorViewportClient* ViewportClient);
	FResolvedViewportViewMode DescribeViewportViewModeState(const FViewportViewModeState& State);
	bool DoesViewportViewModeMatch(const FResolvedViewportViewMode& Requested, const FViewportViewModeState& Actual);
	void RestoreViewportViewModeState(FEditorViewportClient* ViewportClient, const FViewportViewModeState& State);
	TSharedPtr<FJsonObject> MakeResolvedViewportViewModeJson(const FResolvedViewportViewMode& Resolved);
	TSharedPtr<FJsonObject> MakeCurrentViewportViewModeJson(FEditorViewportClient* ViewportClient);
	TSharedPtr<FJsonObject> MakeViewportViewModeCatalogJson();
}

