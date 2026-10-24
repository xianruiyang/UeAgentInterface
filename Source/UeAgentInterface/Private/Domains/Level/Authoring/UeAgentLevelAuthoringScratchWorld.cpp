// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringScratchWorld.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"

namespace UeAgentLevelAuthoring
{
	namespace
	{
		UE::UAI::Contract::FDiagnostic ScratchFailure(const TCHAR* Code,
			const FString& Reason, const TCHAR* Field, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected, const FString& Candidate = FString())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("scratch_prepare"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.CandidateName = Candidate;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}
	}
	FLevelAuthoringScratchWorld::~FLevelAuthoringScratchWorld()
	{
		Cleanup();
	}

	AActor* FLevelAuthoringScratchWorld::DuplicateActor(
		const AActor& Source,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		UWorld* World = GetOrCreateWorld(OutError);
		if (!World) return nullptr;
		if (!World->PersistentLevel)
		{
			OutError = ScratchFailure(TEXT("level_scratch_world_persistent_level_missing"),
				TEXT("The scratch world has no persistent level for actor duplication."), TEXT("persistent_level_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Source.GetActorLabel());
			return nullptr;
		}

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
		// SpawnActor otherwise clears the template label. A scratch duplicate must
		// preserve the source state without generating a label on class defaults.
		Parameters.InitialActorLabel = Source.GetActorLabel(false);
		Parameters.bTemporaryEditorActor = true;
		Parameters.bHideFromSceneOutliner = true;
		Parameters.bCreateActorPackage = false;
#endif
		AActor* Result = World->SpawnActor<AActor>(
			Source.GetClass(), Source.GetActorTransform(), Parameters);
		if (!Result)
		{
			OutError = ScratchFailure(TEXT("level_actor_scratch_spawn_failed"),
				TEXT("Unreal could not spawn a scratch copy of the actor."), TEXT("actor_created"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Source.GetActorLabel());
			OutError.ProposedValue = MakeShared<FJsonValueString>(Source.GetClass()->GetPathName());
		}
		return Result;
	}

	UActorComponent* FLevelAuthoringScratchWorld::DuplicateComponent(
		const UActorComponent& Source,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const AActor* Owner = Source.GetOwner();
		if (!Owner)
		{
			OutError = ScratchFailure(TEXT("level_component_scratch_owner_missing"),
				TEXT("The component has no owning actor to duplicate into the scratch world."), TEXT("owner_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Source.GetName());
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		TArray<UActorComponent*> Components;
		ScratchOwner.GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (!Component || Component->GetFName() != DesiredName) continue;
			if (!bReuseMatchingComponent)
			{
				OutError = ScratchFailure(TEXT("level_component_scratch_name_conflict"),
					TEXT("A scratch component already uses the requested name and reuse is disabled."), TEXT("component_name"),
					MakeShared<FJsonValueString>(DesiredName.ToString()), MakeShared<FJsonValueString>(TEXT("unused component name")), DesiredName.ToString());
				return nullptr;
			}
			if (Component->GetClass() != Template.GetClass())
			{
				OutError = ScratchFailure(TEXT("level_component_scratch_class_conflict"),
					TEXT("The existing scratch component has a different class from the requested template."), TEXT("component_class"),
					MakeShared<FJsonValueString>(Component->GetClass()->GetPathName()),
					MakeShared<FJsonValueString>(Template.GetClass()->GetPathName()), DesiredName.ToString());
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
			OutError = ScratchFailure(TEXT("level_component_scratch_create_failed"),
				TEXT("Unreal could not create the requested scratch component."), TEXT("component_created"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), DesiredName.ToString());
			OutError.ProposedValue = MakeShared<FJsonValueString>(Template.GetClass()->GetPathName());
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

	UWorld* FLevelAuthoringScratchWorld::GetOrCreateWorld(UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (ScratchWorld.IsValid()) return ScratchWorld.Get();
		if (bCleanupPerformed)
		{
			OutError = ScratchFailure(TEXT("level_scratch_world_already_cleaned"),
				TEXT("The scratch world has already been released and cannot be reused."), TEXT("scratch_world_released"),
				MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
			return nullptr;
		}

		const FString PackageName = FString::Printf(
			TEXT("/Temp/UeAgentInterface/LevelAuthoring/%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = ScratchFailure(TEXT("level_scratch_world_package_create_failed"),
				TEXT("Unreal could not create the temporary package for the scratch world."), TEXT("package_created"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.InternalContext = PackageName;
			return nullptr;
		}
		Package->SetFlags(RF_Transient);
		ScratchPackage.Reset(Package);

		UWorld* World = NewObject<UWorld>(Package, TEXT("UAI_LevelAuthoringScratchWorld"));
		if (!World)
		{
			OutError = ScratchFailure(TEXT("level_scratch_world_create_failed"),
				TEXT("Unreal could not allocate the scratch world."), TEXT("world_created"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.InternalContext = PackageName;
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
			OutError = ScratchFailure(TEXT("level_scratch_world_persistent_level_missing"),
				World->PersistentLevel ? TEXT("The scratch persistent level belongs to a different world.")
					: TEXT("Scratch world initialization did not create a persistent level."),
				World->PersistentLevel ? TEXT("persistent_level_owner_matches") : TEXT("persistent_level_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			Cleanup();
			return nullptr;
		}
		if (GEngine && GEngine->GetWorldContextFromWorld(World))
		{
			OutError = ScratchFailure(TEXT("level_scratch_world_context_forbidden"),
				TEXT("The scratch world was unexpectedly registered as an engine world context."), TEXT("engine_world_context_present"),
				MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
			Cleanup();
			return nullptr;
		}
		return World;
	}

	void FLevelAuthoringScratchWorld::ReleaseForPersistentReadback()
	{
		Cleanup();
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
