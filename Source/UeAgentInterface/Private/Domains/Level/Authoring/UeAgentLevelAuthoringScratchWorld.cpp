// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringScratchWorld.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"

namespace UeAgentLevelAuthoring
{
	FLevelAuthoringScratchWorld::~FLevelAuthoringScratchWorld()
	{
		Cleanup();
	}

	AActor* FLevelAuthoringScratchWorld::DuplicateActor(
		const AActor& Source,
		FString& OutError)
	{
		OutError.Reset();
		UWorld* World = GetOrCreateWorld(OutError);
		if (!World || !World->PersistentLevel) return nullptr;

		FActorSpawnParameters Parameters;
		Parameters.Template = const_cast<AActor*>(&Source);
		Parameters.OverrideLevel = World->PersistentLevel;
		Parameters.Name = MakeUniqueObjectName(
			World->PersistentLevel,
			Source.GetClass(),
			*FString::Printf(TEXT("UAI_%s"), *Source.GetName()));
		Parameters.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
		Parameters.ObjectFlags = RF_Transient;
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
#if WITH_EDITOR
		Parameters.bTemporaryEditorActor = true;
		Parameters.bHideFromSceneOutliner = true;
		Parameters.bCreateActorPackage = false;
#endif
		AActor* Result = World->SpawnActor<AActor>(
			Source.GetClass(), Source.GetActorTransform(), Parameters);
		if (!Result)
		{
			OutError = TEXT("level_actor_scratch_spawn_failed:") + Source.GetClass()->GetPathName();
		}
		return Result;
	}

	UActorComponent* FLevelAuthoringScratchWorld::DuplicateComponent(
		const UActorComponent& Source,
		FString& OutError)
	{
		const AActor* Owner = Source.GetOwner();
		if (!Owner)
		{
			OutError = TEXT("level_component_scratch_owner_missing");
			return nullptr;
		}
		AActor* ScratchOwner = DuplicateActor(*Owner, OutError);
		if (!ScratchOwner) return nullptr;
		return CreateComponent(Source, *ScratchOwner, Source.GetFName(), true, OutError);
	}

	UActorComponent* FLevelAuthoringScratchWorld::CreateComponent(
		const UActorComponent& Template,
		AActor& ScratchOwner,
		const FName DesiredName,
		const bool bReuseMatchingComponent,
		FString& OutError)
	{
		OutError.Reset();
		TArray<UActorComponent*> Components;
		ScratchOwner.GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (!Component || Component->GetFName() != DesiredName) continue;
			if (!bReuseMatchingComponent)
			{
				OutError = TEXT("level_component_scratch_name_conflict:") + DesiredName.ToString();
				return nullptr;
			}
			if (Component->GetClass() != Template.GetClass())
			{
				OutError = TEXT("level_component_scratch_class_conflict:")
					+ Component->GetClass()->GetPathName();
				return nullptr;
			}
			if (Component->IsRegistered()) Component->UnregisterComponent();
			return Component;
		}

		UActorComponent* Result = NewObject<UActorComponent>(
			&ScratchOwner,
			Template.GetClass(),
			DesiredName,
			RF_Transient,
			const_cast<UActorComponent*>(&Template));
		if (!Result)
		{
			OutError = TEXT("level_component_scratch_create_failed:")
				+ Template.GetClass()->GetPathName();
			return nullptr;
		}
		ScratchOwner.AddOwnedComponent(Result);
		ScratchOwner.AddInstanceComponent(Result);
		Result->OnComponentCreated();
		if (USceneComponent* Scene = Cast<USceneComponent>(Result))
		{
			if (!ScratchOwner.GetRootComponent()) ScratchOwner.SetRootComponent(Scene);
		}
		return Result;
	}

	void FLevelAuthoringScratchWorld::RegisterPreparedActor(
		const FString& CanonicalKey,
		AActor& Actor)
	{
		PreparedActors.Add(CanonicalKey, &Actor);
	}

	AActor* FLevelAuthoringScratchWorld::FindPreparedActor(const FString& CanonicalKey) const
	{
		const TWeakObjectPtr<AActor>* Found = PreparedActors.Find(CanonicalKey);
		return Found ? Found->Get() : nullptr;
	}

	UWorld* FLevelAuthoringScratchWorld::GetOrCreateWorld(FString& OutError)
	{
		OutError.Reset();
		if (ScratchWorld.IsValid()) return ScratchWorld.Get();
		if (bCleanupPerformed)
		{
			OutError = TEXT("level_scratch_world_already_cleaned");
			return nullptr;
		}

		const FString PackageName = FString::Printf(
			TEXT("/Temp/UeAgentInterface/LevelAuthoring/%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = TEXT("level_scratch_world_package_create_failed");
			return nullptr;
		}
		Package->SetFlags(RF_Transient);
		ScratchPackage.Reset(Package);

		UWorld* World = NewObject<UWorld>(Package, TEXT("UAI_LevelAuthoringScratchWorld"));
		if (!World)
		{
			OutError = TEXT("level_scratch_world_create_failed");
			return nullptr;
		}
		ScratchWorld.Reset(World);
		World->SetFlags(RF_Transient);
		World->WorldType = EWorldType::Inactive;
		World->InitializeNewWorld(UWorld::InitializationValues()
			.InitializeScenes(false)
			.AllowAudioPlayback(false)
			.RequiresHitProxies(false)
			.CreatePhysicsScene(false)
			.CreateNavigation(false)
			.CreateAISystem(false)
			.ShouldSimulatePhysics(false)
			.EnableTraceCollision(false)
			.SetTransactional(false)
			.CreateFXSystem(false));
		bWorldInitialized = true;
		if (!World->PersistentLevel || World->PersistentLevel->OwningWorld != World)
		{
			OutError = TEXT("level_scratch_world_persistent_level_missing");
			Cleanup();
			return nullptr;
		}
		if (GEngine && GEngine->GetWorldContextFromWorld(World))
		{
			OutError = TEXT("level_scratch_world_context_forbidden");
			Cleanup();
			return nullptr;
		}
		return World;
	}

	void FLevelAuthoringScratchWorld::Cleanup()
	{
		if (bCleanupPerformed) return;
		bCleanupPerformed = true;
		PreparedActors.Reset();
		if (ScratchWorld.IsValid() && bWorldInitialized)
		{
			ScratchWorld->CleanupWorld();
		}
		bWorldInitialized = false;
		ScratchWorld.Reset();
		ScratchPackage.Reset();
	}
}
