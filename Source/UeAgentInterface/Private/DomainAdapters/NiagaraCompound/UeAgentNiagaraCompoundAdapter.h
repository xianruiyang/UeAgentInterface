#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "DomainAdapters/GenericRead/UeAgentGenericReadAdapter.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::NiagaraCompound
{
	UEAGENTINTERFACE_API bool TryValidateNiagaraCompoundInput(
		const StateSemantic::FUeAgentCanonicalValueRef& Value,
		ContextResource::FUeAgentResourceTracker& Resources,
		ExecutionMutation::FExecutionMutationError& OutError);

	class UEAGENTINTERFACE_API FUeAgentNiagaraCompileSettleReceipt final
	{
	public:
		FUeAgentNiagaraCompileSettleReceipt() = default;
		FUeAgentNiagaraCompileSettleReceipt(
			int64 InSubjectGeneration,
			bool bInCompilationComplete,
			int32 InCompileErrorCount,
			int32 InStackIssueCount,
			int32 InCompileWarningCount,
			int32 InStackWarningCount,
			TArray<FString> InCompiledTargets,
			TArray<FString> InObservedSignals,
			FString InPostBarrierRevision,
			FString InEvidenceId);

		int64 GetSubjectGeneration() const;
		bool IsCompilationComplete() const;
		int32 GetCompileErrorCount() const;
		int32 GetStackIssueCount() const;
		int32 GetCompileWarningCount() const;
		int32 GetStackWarningCount() const;
		const TArray<FString>& GetCompiledTargets() const;
		const TArray<FString>& GetObservedSignals() const;
		const FString& GetPostBarrierRevision() const;
		const FString& GetEvidenceId() const;
		bool IsValid() const;

	private:
		int64 SubjectGeneration = -1;
		bool bCompilationComplete = false;
		int32 CompileErrorCount = 0;
		int32 StackIssueCount = 0;
		int32 CompileWarningCount = 0;
		int32 StackWarningCount = 0;
		TArray<FString> CompiledTargets;
		TArray<FString> ObservedSignals;
		FString PostBarrierRevision;
		FString EvidenceId;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API IUeAgentNiagaraCompoundUeHook
	{
	public:
		virtual ~IUeAgentNiagaraCompoundUeHook() = default;

		virtual bool TryCapture(
			const Discovery::FUeAgentStateReadRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			const ContextResource::FUeAgentOperationDeadline& Deadline,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const
				GenericRead::FUeAgentUeReadCapture>& OutCapture,
			GenericRead::FUeAgentGenericReadError& OutError) = 0;
		virtual bool TryApply(
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const ExecutionMutation::FLockFenceLease& Lease,
			const TArray<
				GenericEffect::FUeAgentGenericEffectBinding>& Bindings,
			ContextResource::FUeAgentResourceTracker& Resources,
			GenericEffect::FUeAgentGenericEffectApplyResult& OutResult,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual bool TryRollback(
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const GenericEffect::FUeAgentGenericEffectApplyResult& Applied,
			ContextResource::FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual void ReleaseVerifiedOperation(
			const FString& OperationId) = 0;
		virtual bool TryCompileAndSettle(
			const ExecutionMutation::FBarrierRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			FUeAgentNiagaraCompileSettleReceipt& OutReceipt,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentNiagaraCompoundReadHook final
		: public GenericRead::IUeAgentUeReadHook
	{
	public:
		explicit FUeAgentNiagaraCompoundReadHook(
			TSharedRef<
				IUeAgentNiagaraCompoundUeHook,
				ESPMode::ThreadSafe> InUeHook);

		bool TryCapture(
			const Discovery::FUeAgentStateReadRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			const ContextResource::FUeAgentOperationDeadline& Deadline,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const
				GenericRead::FUeAgentUeReadCapture>& OutCapture,
			GenericRead::FUeAgentGenericReadError& OutError) override;

	private:
		TSharedRef<
			IUeAgentNiagaraCompoundUeHook,
			ESPMode::ThreadSafe> UeHook;
	};

	class UEAGENTINTERFACE_API FUeAgentNiagaraCompoundEffectHook final
		: public GenericEffect::IUeAgentGenericEffectUeHook
	{
	public:
		explicit FUeAgentNiagaraCompoundEffectHook(
			TSharedRef<
				IUeAgentNiagaraCompoundUeHook,
				ESPMode::ThreadSafe> InUeHook);

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
			IUeAgentNiagaraCompoundUeHook,
			ESPMode::ThreadSafe> UeHook;
	};
}
