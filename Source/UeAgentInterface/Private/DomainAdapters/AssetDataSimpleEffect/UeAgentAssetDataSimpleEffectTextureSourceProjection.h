#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"

class UObject;

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	bool CanProjectTextureSourceState(
		const FString& NativeMechanicId);

	bool TryProjectTextureSourceCurrentState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FString>& SelectedFieldIds,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutCurrentFields,
		StateSemantic::FUeAgentCanonicalValuePtr& OutDefaultFields,
		StateSemantic::FUeAgentCanonicalValuePtr& OutProjectionContext,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryProjectTextureSourceExpectedState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		const StateSemantic::FUeAgentCanonicalValue& CurrentFields,
		const StateSemantic::FUeAgentCanonicalValue& DefaultFields,
		const StateSemantic::FUeAgentCanonicalValue& ProjectionContext,
		const StateSemantic::FUeAgentCanonicalValue& ProposedAfter,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutExpectedFields,
		ExecutionMutation::FExecutionMutationError& OutError);
}
