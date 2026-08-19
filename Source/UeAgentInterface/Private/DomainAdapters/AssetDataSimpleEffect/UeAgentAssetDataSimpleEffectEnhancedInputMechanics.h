#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "Templates/SharedPointer.h"

class UObject;

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	struct FEnhancedInputActionPreparedData;
	struct FEnhancedInputMappingContextPreparedData;

	struct FEnhancedInputChangeView final
	{
		bool bSpecified = false;
		FString Presence;
		const StateSemantic::FUeAgentCanonicalValue*
			CanonicalValue = nullptr;
	};

	struct FEnhancedInputActionPreparedState final
	{
		TSharedPtr<
			FEnhancedInputActionPreparedData,
			ESPMode::ThreadSafe> Data;
	};

	struct FEnhancedInputMappingContextPreparedState final
	{
		TSharedPtr<
			FEnhancedInputMappingContextPreparedData,
			ESPMode::ThreadSafe> Data;
	};

	bool TryPrepareEnhancedInputActionUpdate(
		UObject* Target,
		const FEnhancedInputChangeView& ValueType,
		const FEnhancedInputChangeView& ConsumeInput,
		const FEnhancedInputChangeView& Triggers,
		const FEnhancedInputChangeView& Modifiers,
		FEnhancedInputActionPreparedState& OutState,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryApplyEnhancedInputActionUpdate(
		UObject* Target,
		const FEnhancedInputActionPreparedState& State,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryRollbackEnhancedInputActionUpdate(
		UObject* Target,
		const FEnhancedInputActionPreparedState& State);

	bool TryPrepareEnhancedInputMappingContextUpdate(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		UObject* Target,
		const FEnhancedInputChangeView& Mappings,
		FEnhancedInputMappingContextPreparedState& OutState,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryApplyEnhancedInputMappingContextUpdate(
		UObject* Target,
		const FEnhancedInputMappingContextPreparedState& State,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryRollbackEnhancedInputMappingContextUpdate(
		UObject* Target,
		const FEnhancedInputMappingContextPreparedState& State);

	bool CanProjectEnhancedInputFieldState(
		const FString& NativeMechanicId);

	bool TryProjectEnhancedInputCurrentFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FString>& SelectedFieldIds,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutCurrentFields,
		StateSemantic::FUeAgentCanonicalValuePtr& OutDefaultFields,
		StateSemantic::FUeAgentCanonicalValuePtr& OutProjectionContext,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryProjectEnhancedInputExpectedFieldState(
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
