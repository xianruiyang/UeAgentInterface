#pragma once

#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundAdapter.h"

namespace UE::UAI::DomainAdapters::NiagaraCompound
{
	UEAGENTINTERFACE_API const FString& GetNiagaraCompoundCatalogKind();
	UEAGENTINTERFACE_API const FString&
	GetNiagaraCompoundCatalogSourceId();
	UEAGENTINTERFACE_API const FString& GetNiagaraCompoundReadPortId();
	UEAGENTINTERFACE_API
	Contract::FUeAgentDescriptorIdentity
	GetNiagaraCompoundOperationDescriptorIdentity();
	UEAGENTINTERFACE_API
	TSharedRef<const Contract::FUeAgentDescriptorRegistry>
	GetNiagaraCompoundOperationDescriptorRegistry();

	class UEAGENTINTERFACE_API FUeAgentNiagaraCompoundNativeHook final
		: public IUeAgentNiagaraCompoundUeHook
	{
	public:
		explicit FUeAgentNiagaraCompoundNativeHook(
			FString InGeneration);
		~FUeAgentNiagaraCompoundNativeHook() override;

		bool IsAvailable() const;

		bool TryCapture(
			const Discovery::FUeAgentStateReadRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			const ContextResource::FUeAgentOperationDeadline& Deadline,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const
				GenericRead::FUeAgentUeReadCapture>& OutCapture,
			GenericRead::FUeAgentGenericReadError& OutError) override;
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
		bool TryCompileAndSettle(
			const ExecutionMutation::FBarrierRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			FUeAgentNiagaraCompileSettleReceipt& OutReceipt,
			ExecutionMutation::FExecutionMutationError& OutError) override;
		void ReleaseVerifiedOperation(
			const FString& OperationId) override;

	private:
		class FState;
		FString Generation;
		TSharedRef<FState, ESPMode::ThreadSafe> State;
	};
}
