#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "Templates/SharedPointer.h"

#define UAI_WORLD_SIMPLE_EFFECT_ADAPTER_V1 1

class UWorld;

namespace UE::UAI::DomainAdapters::WorldSimpleEffect
{
	class UEAGENTINTERFACE_API
		IUeAgentWorldSimpleEffectWorldResolver
	{
	public:
		virtual ~IUeAgentWorldSimpleEffectWorldResolver() = default;

		virtual bool TryResolveWorld(
			const ContextResource::FUeAgentExecutionContextRef& Context,
			UWorld*& OutWorld,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		IUeAgentWorldSimpleEffectNativeHook
	{
	public:
		virtual ~IUeAgentWorldSimpleEffectNativeHook() = default;

		virtual bool TryApply(
			UWorld& World,
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const ExecutionMutation::FLockFenceLease& Lease,
			const TArray<
				GenericEffect::FUeAgentGenericEffectBinding>& Bindings,
			ContextResource::FUeAgentResourceTracker& Resources,
			GenericEffect::FUeAgentGenericEffectApplyResult& OutResult,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual bool TryRollback(
			UWorld& World,
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const GenericEffect::FUeAgentGenericEffectApplyResult& Applied,
			ContextResource::FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual void ReleaseVerifiedOperation(
			const FString& OperationId) = 0;
		virtual bool TryWaitForSettle(
			const ExecutionMutation::FBarrierRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			ExecutionMutation::FBarrierResult& OutResult,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		FUeAgentWorldSimpleEffectUeHook final
		: public GenericEffect::IUeAgentGenericEffectUeHook
	{
	public:
		FUeAgentWorldSimpleEffectUeHook(
			TSharedRef<
				IUeAgentWorldSimpleEffectWorldResolver,
				ESPMode::ThreadSafe> InWorldResolver,
			TSharedRef<
				IUeAgentWorldSimpleEffectNativeHook,
				ESPMode::ThreadSafe> InNativeHook);

		bool TryApply(
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const ExecutionMutation::FLockFenceLease& Lease,
			const TArray<
				GenericEffect::FUeAgentGenericEffectBinding>& Bindings,
			ContextResource::FUeAgentResourceTracker& Resources,
			GenericEffect::FUeAgentGenericEffectApplyResult& OutResult,
			ExecutionMutation::FExecutionMutationError& OutError) override;
		bool TryRollback(
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const GenericEffect::FUeAgentGenericEffectApplyResult& Applied,
			ContextResource::FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			ExecutionMutation::FExecutionMutationError& OutError) override;
		void ReleaseVerifiedOperation(
			const FString& OperationId) override;
		bool TryWaitForSettle(
			const ExecutionMutation::FBarrierRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			ExecutionMutation::FBarrierResult& OutResult,
			ExecutionMutation::FExecutionMutationError& OutError) override;

	private:
		TSharedRef<
			IUeAgentWorldSimpleEffectWorldResolver,
			ESPMode::ThreadSafe> WorldResolver;
		TSharedRef<
			IUeAgentWorldSimpleEffectNativeHook,
			ESPMode::ThreadSafe> NativeHook;
	};
}

