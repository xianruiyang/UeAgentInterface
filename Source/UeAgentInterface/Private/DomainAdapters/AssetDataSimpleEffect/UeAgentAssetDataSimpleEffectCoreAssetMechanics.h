#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "Templates/SharedPointer.h"

class UObject;

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	struct FCoreAssetPreparedData;

	struct FCoreAssetChangeView final
	{
		FString FieldId;
		FString Presence;
		const StateSemantic::FUeAgentCanonicalValue*
			CanonicalValue = nullptr;
	};

	struct FCoreAssetPreparedState final
	{
		TSharedPtr<
			FCoreAssetPreparedData,
			ESPMode::ThreadSafe> Data;
	};

	bool IsCoreAssetMechanicId(
		const FString& NativeMechanicId);

	bool TryPrepareCoreAssetUpdate(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FCoreAssetChangeView>& Changes,
		FCoreAssetPreparedState& OutState,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryApplyCoreAssetUpdate(
		UObject* Target,
		const FCoreAssetPreparedState& State,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryRollbackCoreAssetUpdate(
		UObject* Target,
		const FCoreAssetPreparedState& State,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool CanProjectCoreAssetFieldState(
		const FString& NativeMechanicId);

	bool TryProjectCoreAssetCurrentFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FString>& SelectedFieldIds,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutCurrentFields,
		StateSemantic::FUeAgentCanonicalValuePtr& OutDefaultFields,
		StateSemantic::FUeAgentCanonicalValuePtr& OutProjectionContext,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryProjectCoreAssetExpectedFieldState(
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
