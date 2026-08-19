// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/EngineTypes.h"

class AActor;
class FEditorViewportClient;
class FLevelEditorViewportClient;
class FJsonObject;
class FProperty;
class UActorComponent;
class UObject;
class UStaticMesh;
class UStaticMeshComponent;
class UWorld;
struct FHitResult;

namespace UeAgentLevelOps
{
	enum class EViewportVisualizationFamily : uint8
	{
		None,
		Buffer,
		Nanite,
		Lumen,
		Substrate,
		Groom,
		VirtualShadowMap,
		VirtualTexture,
		ActorColoration,
		GPUSkinCache,
		RayTracingDebug,
	};

	struct FResolvedViewportViewMode
	{
		FResolvedViewportViewMode();

		FString Key;
		FString Category;
		FString Label;
		EViewModeIndex ViewMode;
		EViewportVisualizationFamily VisualizationFamily;
		FName VisualizationMode;
		bool bFinalLitComposition;
	};

	struct FViewportViewModeState
	{
		FViewportViewModeState();

		EViewModeIndex ViewMode;
		FName BufferMode;
		FName NaniteMode;
		FName LumenMode;
		FName SubstrateMode;
		FName GroomMode;
		FName VirtualShadowMapMode;
		FName VirtualTextureMode;
		FName ActorColorationMode;
		FName GPUSkinCacheMode;
		FName RayTracingDebugMode;
	};

	enum class EBoundsAxis : uint8
	{
		X,
		Y,
		Z
	};

	enum class EBoundsAnchor : uint8
	{
		Min,
		Center,
		Max
	};

	bool TryGetLevelPackagePath(const TSharedPtr<FJsonObject>& Params, FString& OutPackagePath);
	bool TryLevelPackageToFilename(const FString& PackagePath, FString& OutFilename, FString& OutError);
	FString ViewModeIndexToStableName(EViewModeIndex ViewMode);
	bool IsFinalLitCompositionViewMode(EViewModeIndex ViewMode);
	bool TryResolveRequestedViewportViewMode(const TSharedPtr<FJsonObject>& Params, bool& bOutHasViewMode, FResolvedViewportViewMode& OutResolved, FString& OutError);
	void ApplyResolvedViewportViewMode(FEditorViewportClient* ViewportClient, const FResolvedViewportViewMode& Resolved);
	FViewportViewModeState CaptureViewportViewModeState(FEditorViewportClient* ViewportClient);
	void RestoreViewportViewModeState(FEditorViewportClient* ViewportClient, const FViewportViewModeState& State);
	TSharedPtr<FJsonObject> MakeResolvedViewportViewModeJson(const FResolvedViewportViewMode& Resolved);
	TSharedPtr<FJsonObject> MakeCurrentViewportViewModeJson(FEditorViewportClient* ViewportClient);
	TSharedPtr<FJsonObject> MakeViewportViewModeCatalogJson();
	bool BuildViewportWarnings(UWorld* World, bool bIncludeSuppressed, TSharedPtr<FJsonObject>& OutData, FString& OutError);
	TSharedPtr<FJsonObject> MakeViewportClientJson(FLevelEditorViewportClient* ViewportClient, const FString& SelectionReason);
	void RestoreEditorInteractionState(FLevelEditorViewportClient* ViewportClient, bool bGameView, bool bModeWidgets, bool bSelection, bool bPivot, const FString& WidgetModePolicy);
	FLevelEditorViewportClient* GetPreferredLevelViewportClient();
	FLevelEditorViewportClient* GetPreferredLevelViewportClient(FString* OutSelectionReason);
	TSharedPtr<FJsonObject> MakeActorSummaryJson(AActor* Actor);
	TSharedPtr<FJsonObject> MakeActorInfoJson(AActor* Actor);
	void FillActorTransformFields(AActor* Actor, TSharedPtr<FJsonObject>& OutData);
	TSharedPtr<FJsonObject> MakeActorTransformJson(const FTransform& Transform);
	TSharedPtr<FJsonObject> MakeBoundsJson(const FBox& Box);
	FString NormalizeFolderPathForMatch(const FString& InFolderPath);
	bool DoesActorMatchFolderPath(AActor* Actor, const FString& NormalizedFolderPath, bool bIncludeChildFolders);
	FString MakeProbeArtifactPath(const FString& Ext, const FString& RequestId, const FString& Suffix);
	void FinishActorTransformEdit(AActor* Actor);
	bool GetActorBoundsBox(AActor* Actor, FBox& OutBox, FString& OutError);
	void AppendIgnoredActorsFromFilters(UWorld* World, const TSharedPtr<FJsonObject>& Params, TArray<AActor*>& InOutIgnoredActors);
	bool ResolveTraceChannel(const TSharedPtr<FJsonObject>& Params, ECollisionChannel& OutChannel);
	bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr);
	bool ExportResolvedProperty(UObject* Object, const FString& PropertyPath, FString& OutValueText, FString& OutPropertyClass, FString& OutError);
	UActorComponent* FindComponentByNameOrPath(AActor* Actor, const FString& ComponentId);
	void AugmentActorSummaryWithFolderTags(AActor* Actor, TSharedPtr<FJsonObject>& ActorSummary);
	TSharedPtr<FJsonObject> MakeHitResultSummaryJson(const FHitResult& Hit, bool bIncludeActorFolderTags, bool bIncludePenetrationDepth);
	void MaybeAttachStartPenetratingAdvice(const FString& Context, bool bStartPenetrating, bool bFindInitialOverlaps, bool bIncludeFloorClearanceSuggestion, double FloorClearanceCm, TSharedPtr<FJsonObject>& OutData);
	double GetAxisValue(const FVector& Vector, EBoundsAxis Axis);
	void SetAxisValue(FVector& Vector, EBoundsAxis Axis, double Value);
	double GetBoundsAnchorValue(const FBox& Box, EBoundsAxis Axis, EBoundsAnchor Anchor);
	bool ParseBoundsAxis(const FString& AxisText, EBoundsAxis& OutAxis);
	bool ParseBoundsAnchor(const FString& AnchorText, EBoundsAnchor& OutAnchor);
	bool GetFolderActorBounds(UWorld* World, const FString& FolderPath, bool bIncludeChildFolders, FBox& OutCombinedBounds, TArray<AActor*>& OutActors, FString& OutError);
	bool GetActorsInFolder(UWorld* World, const FString& FolderPath, bool bIncludeChildFolders, TArray<AActor*>& OutActors, FString& OutError);
	bool ComputeViewportLocationToFrameBounds(FLevelEditorViewportClient* ViewportClient, const FBox& Bounds, const FRotator& ViewRotation, double Padding, FVector& OutLocation, FString& OutError);
	bool ResolveViewportScreenPoint(FLevelEditorViewportClient* ViewportClient, const TSharedPtr<FJsonObject>& Params, FVector2D& OutScreenPoint, FString& OutError);
	void FillHitResultJson(const FHitResult& Hit, TSharedPtr<FJsonObject>& OutData);
	bool TraceActorFromScreenPoint(UWorld* World, FLevelEditorViewportClient* ViewportClient, const TSharedPtr<FJsonObject>& Params, FVector2D& OutScreenPoint, FVector& OutWorldOrigin, FVector& OutWorldDirection, double& OutTraceDistance, ECollisionChannel& OutTraceChannel, bool& OutTraceComplex, FHitResult& OutHit, FString& OutError);
	bool ComputeActorLocalObbBox(AActor* Actor, FBox& OutLocalBox, FString& OutError);
	TSharedPtr<FJsonObject> MakeActorObbJson(AActor* Actor, const FBox& LocalBox);
	bool GetStaticMeshComponentForVertexQuery(AActor* Actor, const FString& ComponentId, UStaticMeshComponent*& OutComponent, FString& OutError);
	bool GetStaticMeshVertexLocalPositions(UStaticMesh* StaticMesh, TArray<FVector>& OutPositions, FString& OutError);
	bool ResolveVertexWorldPosition(UStaticMeshComponent* Component, const TArray<FVector>& LocalPositions, int32 VertexIndex, FVector& OutWorldPosition, FString& OutError);
}
