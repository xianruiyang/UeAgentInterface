// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class AActor;
class UActorComponent;
class UClass;
class UWorld;

namespace UeAgentLevelAuthoring
{
	struct FAuthorabilityResult
	{
		bool bAuthorable = false;
		UE::UAI::Contract::FDiagnostic Diagnostic;
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
	bool TryMapAuthorabilityFailure(
		const UE::UAI::Contract::FDiagnostic& LookupError,
		EAuthorabilityPublicTarget PublicTarget,
		FAuthorabilityResult& OutFailure);
}
