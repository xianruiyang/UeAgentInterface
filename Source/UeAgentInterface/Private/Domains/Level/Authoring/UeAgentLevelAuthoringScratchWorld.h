// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

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

		AActor* DuplicateActor(const AActor& Source, UE::UAI::Contract::FDiagnostic& OutError);
		UActorComponent* DuplicateComponent(
			const UActorComponent& Source,
			UE::UAI::Contract::FDiagnostic& OutError);
		UActorComponent* CreateComponent(
			const UActorComponent& Template,
			AActor& ScratchOwner,
			FName DesiredName,
			bool bReuseMatchingComponent,
			UE::UAI::Contract::FDiagnostic& OutError);
		void RegisterPreparedActor(const FString& CanonicalKey, AActor& Actor);
		AActor* FindPreparedActor(const FString& CanonicalKey) const;
		void ReleaseForPersistentReadback();

	private:
		UWorld* GetOrCreateWorld(UE::UAI::Contract::FDiagnostic& OutError);
		void Cleanup();

		TStrongObjectPtr<UPackage> ScratchPackage;
		TStrongObjectPtr<UWorld> ScratchWorld;
		TMap<FString, TWeakObjectPtr<AActor>> PreparedActors;
		bool bWorldInitialized = false;
		bool bCleanupPerformed = false;
	};
}
