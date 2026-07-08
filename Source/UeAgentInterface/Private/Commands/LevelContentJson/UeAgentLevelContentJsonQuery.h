// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UActorComponent;
class FJsonObject;
class UWorld;

namespace UeAgentLevelContentJson
{
	bool QueryLevelContent(UWorld* World, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutJson, bool bActorOnly, bool bSnapshot);
	bool TryReadBoundsSpec(const TSharedPtr<FJsonObject>& Obj, FBox& OutBounds);
	bool ActorMatchesDataLayerFallback(AActor* Actor, const TArray<FString>& DataLayers);
	bool ActorMatchesQuery(AActor* Actor, const TSharedPtr<FJsonObject>& Selection);
	TSharedPtr<FJsonObject> MakeBoundsJson(const FBox& Box);
	TSharedPtr<FJsonObject> MakeComponentJson(UActorComponent* Component);
	TSharedPtr<FJsonObject> MakeActorResourceJson(AActor* Actor, const TSharedPtr<FJsonObject>& Projection, bool bSnapshot);
	TSharedPtr<FJsonObject> BuildWorldInfo(UWorld* World);
	TSharedPtr<FJsonObject> BuildFeatureGateReport(const TSharedPtr<FJsonObject>& Content);
}
