// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringAuthorability.h"

#include "ActorEditorUtils.h"
#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "Helpers/PCGHelpers.h"
#include "PCGWorldActor.h"

namespace UeAgentLevelAuthoring
{
	namespace
	{
		constexpr const TCHAR* ActorErrorCode = TEXT("level_actor_not_authorable");
		constexpr const TCHAR* ComponentErrorCode = TEXT("level_component_not_authorable");

		FAuthorabilityResult Authorable()
		{
			FAuthorabilityResult Result;
			Result.bAuthorable = true;
			return Result;
		}

		FAuthorabilityResult Rejected(const TCHAR* ErrorCode, const TCHAR* Reason)
		{
			FAuthorabilityResult Result;
			Result.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(ErrorCode, Reason, TEXT("level_authorability"));
			return Result;
		}

		const TCHAR* GetObjectRejectionReason(const UObject& Object)
		{
			if (Object.IsTemplate()) return TEXT("Object templates cannot be authored as level instances.");
			if (Object.HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			{
				return TEXT("The object is being destroyed.");
			}
			if (Object.HasAnyInternalFlags(
				EInternalObjectFlags::Garbage | EInternalObjectFlags::Unreachable))
			{
				return TEXT("The object is unreachable or marked as garbage.");
			}
			if (Object.HasAnyFlags(RF_Transient)) return TEXT("Transient objects cannot be authored as persistent level instances.");
			if (Object.GetClass()->HasAnyClassFlags(CLASS_Transient))
			{
				return TEXT("The object class is transient.");
			}
			return nullptr;
		}

		bool HasErrorCode(const UE::UAI::Contract::FDiagnostic& Error, const TCHAR* ErrorCode)
		{
			return Error.Code.Equals(ErrorCode, ESearchCase::CaseSensitive);
		}
	}

	FAuthorabilityResult EvaluateActorClassAuthorability(const UClass& ActorClass)
	{
		if (ActorClass.HasAnyClassFlags(CLASS_Transient))
		{
			return Rejected(ActorErrorCode, TEXT("The object class is transient."));
		}
		if (ActorClass.IsChildOf(AWorldSettings::StaticClass()))
		{
			return Rejected(ActorErrorCode, TEXT("World settings must be authored through their dedicated world interface."));
		}
		return Authorable();
	}

	FAuthorabilityResult EvaluateComponentClassAuthorability(const UClass& ComponentClass)
	{
		if (ComponentClass.HasAnyClassFlags(CLASS_Transient))
		{
			return Rejected(ComponentErrorCode, TEXT("The object class is transient."));
		}
		return Authorable();
	}

	FAuthorabilityResult EvaluateActorAuthorability(
		const UWorld& BoundWorld,
		const AActor& Actor)
	{
		if (Actor.IsA<AWorldSettings>()) return Rejected(ActorErrorCode, TEXT("World settings must be authored through their dedicated world interface."));
		if (Actor.IsA<APCGWorldActor>()) return Rejected(ActorErrorCode, TEXT("The PCG world service is managed by the engine."));
		if (FActorEditorUtils::IsABuilderBrush(&Actor))
		{
			return Rejected(ActorErrorCode, TEXT("The editor builder brush cannot be authored as a level actor."));
		}
		if (Actor.IsActorBeingDestroyed()) return Rejected(ActorErrorCode, TEXT("The actor is being destroyed."));
		if (const TCHAR* Reason = GetObjectRejectionReason(Actor))
		{
			return Rejected(ActorErrorCode, Reason);
		}
		if (Actor.GetWorld() != &BoundWorld)
		{
			return Rejected(ActorErrorCode, TEXT("The actor belongs to a different world."));
		}
		if (!Actor.GetLevel() || !BoundWorld.GetLevels().Contains(Actor.GetLevel()))
		{
			return Rejected(ActorErrorCode, TEXT("The actor's level is not loaded in the bound world."));
		}
		return Authorable();
	}

	FAuthorabilityResult EvaluateComponentAuthorability(
		const UWorld& BoundWorld,
		const UActorComponent& Component)
	{
		const AActor* Owner = Component.GetOwner();
		if (!Owner) return Rejected(ComponentErrorCode, TEXT("The component has no owner."));
        const auto OwnerResult = EvaluateActorAuthorability(BoundWorld, *Owner);
        if (!OwnerResult.bAuthorable)
        {
            auto Result = Rejected(ComponentErrorCode, TEXT("The component owner is not authorable."));
            Result.Diagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(OwnerResult.Diagnostic);
            return Result;
        }
		if (Component.IsBeingDestroyed())
		{
			return Rejected(ComponentErrorCode, TEXT("The component is being destroyed."));
		}
		// PCG owns these derived objects; edit their input/graph instead.
		if (Component.ComponentHasTag(PCGHelpers::DefaultPCGTag))
		{
			return Rejected(ComponentErrorCode, TEXT("PCG owns this generated component; edit its input or graph instead."));
		}
		if (const TCHAR* Reason = GetObjectRejectionReason(Component))
		{
			return Rejected(ComponentErrorCode, Reason);
		}
		return Authorable();
	}

	bool TryMapAuthorabilityFailure(
		const UE::UAI::Contract::FDiagnostic& LookupError,
		const EAuthorabilityPublicTarget PublicTarget,
		FAuthorabilityResult& OutFailure)
	{
		OutFailure = FAuthorabilityResult();
		if (HasErrorCode(LookupError, ComponentErrorCode))
		{
			if (PublicTarget != EAuthorabilityPublicTarget::Component) return false;
			OutFailure = Rejected(ComponentErrorCode, TEXT("The requested object is not authorable."));
			OutFailure.Diagnostic = LookupError;
			return true;
		}
		if (!HasErrorCode(LookupError, ActorErrorCode)) return false;
		if (PublicTarget == EAuthorabilityPublicTarget::Actor)
		{
			OutFailure = Rejected(ActorErrorCode, TEXT("The requested object is not authorable."));
			OutFailure.Diagnostic = LookupError;
		}
		else
		{
			OutFailure = Rejected(ComponentErrorCode, TEXT("The component owner is not authorable."));
            OutFailure.Diagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(LookupError);
		}
		return true;
	}
}
