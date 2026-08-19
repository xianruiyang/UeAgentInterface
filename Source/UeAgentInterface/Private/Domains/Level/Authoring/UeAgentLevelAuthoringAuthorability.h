// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UActorComponent;
class UClass;
class UWorld;

namespace UeAgentLevelAuthoring
{
	struct FAuthorabilityResult
	{
		bool bAuthorable = false;
		FString ErrorCode;
		FString ErrorDetail;
	};

	enum class EAuthorabilityPublicTarget : uint8
	{
		Actor,
		Component,
	};

	FAuthorabilityResult EvaluateActorClassAuthorability(const UClass& ActorClass);
	FAuthorabilityResult EvaluateComponentClassAuthorability(const UClass& ComponentClass);
	FAuthorabilityResult EvaluateActorAuthorability(
		const UWorld& BoundWorld,
		const AActor& Actor);
	FAuthorabilityResult EvaluateComponentAuthorability(
		const UWorld& BoundWorld,
		const UActorComponent& Component);
	bool TryNormalizeAuthorabilityFailure(
		const FString& LookupError,
		EAuthorabilityPublicTarget PublicTarget,
		FAuthorabilityResult& OutFailure);
}
