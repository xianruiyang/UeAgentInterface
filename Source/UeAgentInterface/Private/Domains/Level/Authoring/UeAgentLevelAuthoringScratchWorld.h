// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

class AActor;
class UActorComponent;

namespace UeAgentLevelAuthoring
{
	class FLevelAuthoringScratchWorld final
	{
	public:
		FLevelAuthoringScratchWorld() = default;
		~FLevelAuthoringScratchWorld();

		FLevelAuthoringScratchWorld(const FLevelAuthoringScratchWorld&) = delete;
		FLevelAuthoringScratchWorld& operator=(const FLevelAuthoringScratchWorld&) = delete;

		AActor* DuplicateActor(const AActor& Source, FString& OutError);
		UActorComponent* DuplicateComponent(
			const UActorComponent& Source,
			FString& OutError);
		UActorComponent* CreateComponent(
			const UActorComponent& Template,
			AActor& ScratchOwner,
			FName DesiredName,
			bool bReuseMatchingComponent,
			FString& OutError);
		void RegisterPreparedActor(const FString& CanonicalKey, AActor& Actor);
		AActor* FindPreparedActor(const FString& CanonicalKey) const;

	private:
		UWorld* GetOrCreateWorld(FString& OutError);
		void Cleanup();

		TStrongObjectPtr<UPackage> ScratchPackage;
		TStrongObjectPtr<UWorld> ScratchWorld;
		TMap<FString, TWeakObjectPtr<AActor>> PreparedActors;
		bool bWorldInitialized = false;
		bool bCleanupPerformed = false;
	};
}
