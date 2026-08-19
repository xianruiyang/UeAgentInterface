// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class AActor;
class UActorComponent;
class UWorld;
class FJsonObject;

namespace UeAgentLevelAuthoring
{
	UEAGENTINTERFACE_API FString GetWorldPath(const UWorld& World);
	UEAGENTINTERFACE_API FString GetManagedActorId(const AActor& Actor);
	UEAGENTINTERFACE_API FString MakeManagedActorTag(const FString& ManagedId);
	UEAGENTINTERFACE_API FString GetActorFolderPathText(const AActor& Actor);
	UEAGENTINTERFACE_API UWorld* FindEditorWorldByPath(const FString& WorldPath);
	UEAGENTINTERFACE_API AActor* FindActorExact(
		UWorld& World,
		const TSharedPtr<const FJsonObject>& Target,
		FString& OutError);
	UEAGENTINTERFACE_API UActorComponent* FindComponentExact(
		AActor& Owner,
		const FString& ComponentName,
		FString& OutError);

	UEAGENTINTERFACE_API bool BuildWorldIdentity(
		const UWorld& World,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildActorIdentity(
		const AActor& Actor,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildComponentIdentity(
		const UActorComponent& Component,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		FString& OutError);

	UEAGENTINTERFACE_API bool BuildWorldSnapshotReadRequests(
		UWorld* World,
		const FString& WorldPath,
		bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildActorSnapshotReadRequests(
		AActor* Actor,
		const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity,
		bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildComponentSnapshotReadRequests(
		UActorComponent* Component,
		const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity,
		bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildWorldExportReadRequests(
		UWorld& World,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildActorExportReadRequests(
		AActor& Actor,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildComponentExportReadRequests(
		UActorComponent& Component,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);

	UEAGENTINTERFACE_API bool RegisterStateAccessor(FString& OutError);
	UEAGENTINTERFACE_API bool RegisterNativeDefaultProvider(FString& OutError);
}
