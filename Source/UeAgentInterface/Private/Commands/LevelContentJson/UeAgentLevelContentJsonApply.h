// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class FJsonObject;
class FJsonValue;
class UWorld;

namespace UeAgentLevelContentJson
{
	bool ApplyResource(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Resource, const FString& Mode, const TArray<FString>& ProtectionTags, TMap<FString, AActor*>& ActorById, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	bool ApplyAttachments(UWorld* World, const TArray<TSharedPtr<FJsonObject>>& Resources, const TMap<FString, AActor*>& ActorById, TArray<TSharedPtr<FJsonValue>>& Results, FString& OutError);
	bool DeleteManagedMissing(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, const TSet<FString>& TargetIds, const TArray<FString>& ProtectionTags, bool bDryRun, TArray<TSharedPtr<FJsonValue>>& Results, FString& OutError);
	bool ExecuteOperations(UWorld* World, const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params, bool bDryRun, const TArray<FString>& ProtectionTags, TArray<TSharedPtr<FJsonValue>>& Results, FString& OutError);
	bool ExecuteTasks(UWorld* World, const TSharedPtr<FJsonObject>& Content, bool bDryRun, TArray<TSharedPtr<FJsonValue>>& Results, FString& OutError);
	bool ExecuteValidations(UWorld* World, const TSharedPtr<FJsonObject>& Content, TArray<TSharedPtr<FJsonValue>>& Results);
}
