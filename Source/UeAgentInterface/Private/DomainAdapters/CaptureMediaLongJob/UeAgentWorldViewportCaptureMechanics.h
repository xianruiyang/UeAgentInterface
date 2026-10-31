#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

class AActor;
class FLevelEditorViewportClient;
class FJsonObject;
class FSceneViewport;
struct FHitResult;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics
{
	TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Value);
	TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Value);
	void GetBoxCorners(const FBox& Box, TArray<FVector>& OutCorners);
	TSharedPtr<FJsonObject> MakeViewportClientJson(FLevelEditorViewportClient* ViewportClient, const FString& SelectionReason);
	FLevelEditorViewportClient* GetPreferredLevelViewportClient();
	FLevelEditorViewportClient* GetPreferredLevelViewportClient(FString* OutSelectionReason);
	TSharedPtr<FSceneViewport> GetLevelSceneViewport(FLevelEditorViewportClient* ViewportClient);
	bool PrepareLevelViewportForCapture(FLevelEditorViewportClient* ViewportClient, UE::UAI::Contract::FDiagnostic& OutError);
	TSharedPtr<FJsonObject> MakeActorSummaryJson(AActor* Actor);
	TSharedPtr<FJsonObject> MakeActorTransformJson(const FTransform& Transform);
	FString MakeProbeArtifactPath(const FString& Ext, const FString& RequestId, const FString& Suffix);
	bool GetActorBoundsBox(AActor* Actor, FBox& OutBox, UE::UAI::Contract::FDiagnostic& OutError);
	TSharedPtr<FJsonObject> MakeBoundsJson(const FBox& Box);
	bool ComputeViewportLocationToFrameBounds(FLevelEditorViewportClient* ViewportClient, const FBox& Bounds, const FRotator& ViewRotation, double Padding, FVector& OutLocation, UE::UAI::Contract::FDiagnostic& OutError);
	bool ResolveTraceChannel(const TSharedPtr<FJsonObject>& Params, ECollisionChannel& OutChannel, UE::UAI::Contract::FDiagnostic& OutError);
	void FillHitResultJson(const FHitResult& Hit, TSharedPtr<FJsonObject>& OutData);
}
