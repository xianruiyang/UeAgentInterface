// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringAuthorability.h"

#include "ActorEditorUtils.h"
#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"

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
			Result.ErrorCode = ErrorCode;
			Result.ErrorDetail = Result.ErrorCode + TEXT(":") + Reason;
			return Result;
		}

		const TCHAR* GetObjectRejectionReason(const UObject& Object)
		{
			if (Object.IsTemplate()) return TEXT("object_template");
			if (Object.HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			{
				return TEXT("object_destroying");
			}
			if (Object.HasAnyInternalFlags(
				EInternalObjectFlags::Garbage | EInternalObjectFlags::Unreachable))
			{
				return TEXT("object_garbage");
			}
			if (Object.HasAnyFlags(RF_Transient)) return TEXT("object_transient");
			if (Object.GetClass()->HasAnyClassFlags(CLASS_Transient))
			{
				return TEXT("class_transient");
			}
			return nullptr;
		}

		bool HasErrorCode(const FString& Error, const TCHAR* ErrorCode)
		{
			return Error.Equals(ErrorCode, ESearchCase::CaseSensitive)
				|| Error.StartsWith(FString(ErrorCode) + TEXT(":"), ESearchCase::CaseSensitive);
		}
	}

	FAuthorabilityResult EvaluateActorClassAuthorability(const UClass& ActorClass)
	{
		if (ActorClass.HasAnyClassFlags(CLASS_Transient))
		{
			return Rejected(ActorErrorCode, TEXT("class_transient"));
		}
		if (ActorClass.IsChildOf(AWorldSettings::StaticClass()))
		{
			return Rejected(ActorErrorCode, TEXT("world_settings"));
		}
		return Authorable();
	}

	FAuthorabilityResult EvaluateComponentClassAuthorability(const UClass& ComponentClass)
	{
		if (ComponentClass.HasAnyClassFlags(CLASS_Transient))
		{
			return Rejected(ComponentErrorCode, TEXT("class_transient"));
		}
		return Authorable();
	}

	FAuthorabilityResult EvaluateActorAuthorability(
		const UWorld& BoundWorld,
		const AActor& Actor)
	{
		if (Actor.IsA<AWorldSettings>()) return Rejected(ActorErrorCode, TEXT("world_settings"));
		if (FActorEditorUtils::IsABuilderBrush(&Actor))
		{
			return Rejected(ActorErrorCode, TEXT("builder_brush"));
		}
		if (Actor.IsActorBeingDestroyed()) return Rejected(ActorErrorCode, TEXT("actor_destroying"));
		if (const TCHAR* Reason = GetObjectRejectionReason(Actor))
		{
			return Rejected(ActorErrorCode, Reason);
		}
		if (Actor.GetWorld() != &BoundWorld)
		{
			return Rejected(ActorErrorCode, TEXT("world_mismatch"));
		}
		if (!BoundWorld.PersistentLevel || Actor.GetLevel() != BoundWorld.PersistentLevel)
		{
			return Rejected(ActorErrorCode, TEXT("unsupported_level"));
		}
		return Authorable();
	}

	FAuthorabilityResult EvaluateComponentAuthorability(
		const UWorld& BoundWorld,
		const UActorComponent& Component)
	{
		const AActor* Owner = Component.GetOwner();
		if (!Owner || !EvaluateActorAuthorability(BoundWorld, *Owner).bAuthorable)
		{
			return Rejected(ComponentErrorCode, TEXT("owner_not_authorable"));
		}
		if (Component.IsBeingDestroyed())
		{
			return Rejected(ComponentErrorCode, TEXT("component_destroying"));
		}
		if (const TCHAR* Reason = GetObjectRejectionReason(Component))
		{
			return Rejected(ComponentErrorCode, Reason);
		}
		return Authorable();
	}

	bool TryNormalizeAuthorabilityFailure(
		const FString& LookupError,
		const EAuthorabilityPublicTarget PublicTarget,
		FAuthorabilityResult& OutFailure)
	{
		OutFailure = FAuthorabilityResult();
		if (HasErrorCode(LookupError, ComponentErrorCode))
		{
			if (PublicTarget != EAuthorabilityPublicTarget::Component) return false;
			OutFailure = Rejected(ComponentErrorCode, TEXT("object_not_authorable"));
			OutFailure.ErrorDetail = LookupError;
			return true;
		}
		if (!HasErrorCode(LookupError, ActorErrorCode)) return false;
		if (PublicTarget == EAuthorabilityPublicTarget::Actor)
		{
			OutFailure = Rejected(ActorErrorCode, TEXT("object_not_authorable"));
			OutFailure.ErrorDetail = LookupError;
		}
		else
		{
			OutFailure = Rejected(ComponentErrorCode, TEXT("owner_not_authorable"));
		}
		return true;
	}
}
