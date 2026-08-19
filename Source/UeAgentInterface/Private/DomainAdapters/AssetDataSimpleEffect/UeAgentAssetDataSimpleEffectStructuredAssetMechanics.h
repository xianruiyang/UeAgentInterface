#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "Templates/SharedPointer.h"

class UObject;

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	struct FStructuredAssetPreparedData;

	struct FStructuredAssetChangeView final
	{
		FString FieldId;
		FString Presence;
		const StateSemantic::FUeAgentCanonicalValue*
			CanonicalValue = nullptr;
	};

	struct FStructuredAssetPreparedState final
	{
		TSharedPtr<
			FStructuredAssetPreparedData,
			ESPMode::ThreadSafe> Data;
	};

	bool IsStructuredAssetMechanicId(
		const FString& NativeMechanicId);

	bool TryProjectStructuredAssetCurrentFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FString>& SelectedFieldIds,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutCurrentFields,
		StateSemantic::FUeAgentCanonicalValuePtr& OutDefaultFields,
		StateSemantic::FUeAgentCanonicalValuePtr& OutProjectionContext,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryProjectStructuredAssetExpectedFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		const StateSemantic::FUeAgentCanonicalValue& CurrentFields,
		const StateSemantic::FUeAgentCanonicalValue& DefaultFields,
		const StateSemantic::FUeAgentCanonicalValue& ProjectionContext,
		const StateSemantic::FUeAgentCanonicalValue& ProposedAfter,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutExpectedFields,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryPrepareStructuredAssetUpdate(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FStructuredAssetChangeView>& Changes,
		FStructuredAssetPreparedState& OutState,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryApplyStructuredAssetUpdate(
		UObject* Target,
		const FStructuredAssetPreparedState& State,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryRollbackStructuredAssetUpdate(
		UObject* Target,
		const FStructuredAssetPreparedState& State);
}
