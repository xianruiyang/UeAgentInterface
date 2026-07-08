// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class FJsonObject;
class UWorld;

namespace UeAgentLevelContentJson
{
	bool ValidateLevelContent(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, bool bActorOnly, TSharedPtr<FJsonObject>& OutData);
	TArray<FString> ReadProtectionTags(const TSharedPtr<FJsonObject>& Content);
	bool ActorIsProtected(AActor* Actor, const TArray<FString>& ProtectionTags);
	TSharedPtr<FJsonObject> BuildEvidenceReport(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, const FString& Phase, bool bDryRun, bool bAllowFileWrite, FString& OutError);
	TSharedPtr<FJsonObject> BuildPlan(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, bool bActorOnly);
}
