#pragma once

#include "CoreMinimal.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

class FProperty;

namespace UE::UAI::DomainAdapters::GenericEffect
{
	UEAGENTINTERFACE_API FString GetUePropertySourceTypeId(
		const FProperty& Property);
	UEAGENTINTERFACE_API const FString&
		GetUeStringPropertySourceTypeId();

	class UEAGENTINTERFACE_API FUeAgentGenericEffectBinding final
	{
	public:
		FUeAgentGenericEffectBinding(
			Discovery::FUeAgentCandidateBinding InCandidateBinding,
			FString InSourceTypeId,
			StateSemantic::FUeAgentCanonicalType InCanonicalType,
			FString InTypeBindingEvidence);

		const Discovery::FUeAgentCandidateBinding&
		GetCandidateBinding() const;
		const FString& GetSourceTypeId() const;
		const StateSemantic::FUeAgentCanonicalType&
		GetCanonicalType() const;
		const FString& GetTypeBindingEvidence() const;
		bool IsValid() const;
		const ExecutionMutation::FExecutionMutationError& GetValidationDiagnostic() const;

	private:
		Discovery::FUeAgentCandidateBinding CandidateBinding;
		FString SourceTypeId;
		StateSemantic::FUeAgentCanonicalType CanonicalType;
		FString TypeBindingEvidence;
		bool bValid = false;
		TSharedPtr<const ExecutionMutation::FExecutionMutationError> ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentGenericEffectApplyResult final
	{
	public:
		FUeAgentGenericEffectApplyResult();
		// Use only while no target writes have been attempted; clear before Apply.
		static FUeAgentGenericEffectApplyResult NoMutation();
		bool IsNoMutationConfirmed() const;
		FUeAgentGenericEffectApplyResult(
			int64 InSubjectGeneration,
			TArray<FString> InMutatedIdentityKeys,
			TArray<FString> InRequiredBarrierSignals,
			bool bInRollbackSupported,
			FString InRollbackToken,
			bool bInMutationStarted,
			TArray<AssetDurable::FAssetDurableTarget>
				InDurableTargets = {});

		int64 GetSubjectGeneration() const;
		const TArray<FString>& GetMutatedIdentityKeys() const;
		const TArray<FString>& GetRequiredBarrierSignals() const;
		bool IsRollbackSupported() const;
		const FString& GetRollbackToken() const;
		const TArray<AssetDurable::FAssetDurableTarget>&
			GetDurableTargets() const;
		bool DidMutationStart() const;
		bool IsValid() const;
		const ExecutionMutation::FExecutionMutationError& GetValidationDiagnostic() const;

	private:
		int64 SubjectGeneration = -1;
		TArray<FString> MutatedIdentityKeys;
		TArray<FString> RequiredBarrierSignals;
		bool bRollbackSupported = false;
		FString RollbackToken;
		TArray<AssetDurable::FAssetDurableTarget> DurableTargets;
		bool bMutationStarted = false;
		bool bNoMutationConfirmed = false;
		bool bValid = false;
		TSharedPtr<const ExecutionMutation::FExecutionMutationError> ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API IUeAgentGenericEffectUeHook
	{
	public:
		virtual ~IUeAgentGenericEffectUeHook() = default;

		virtual bool TryApply(
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const ExecutionMutation::FLockFenceLease& Lease,
			const TArray<FUeAgentGenericEffectBinding>& Bindings,
			ContextResource::FUeAgentResourceTracker& Resources,
			FUeAgentGenericEffectApplyResult& OutResult,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual bool TryRollback(
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const FUeAgentGenericEffectApplyResult& Applied,
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

	class UEAGENTINTERFACE_API FUeAgentGenericEffectMutationHook final
		: public ExecutionMutation::IMutationHook
	{
	public:
		FUeAgentGenericEffectMutationHook(
			TArray<FUeAgentGenericEffectBinding> InBindings,
			TSharedRef<
				IUeAgentGenericEffectUeHook,
				ESPMode::ThreadSafe> InUeHook);

		bool Execute(
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const ExecutionMutation::FLockFenceLease& Lease,
			ContextResource::FUeAgentResourceTracker& Resources,
			ExecutionMutation::FMutationFacts& OutFacts,
			ExecutionMutation::FExecutionMutationError& OutError) override;
		bool Rollback(
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const ExecutionMutation::FMutationFacts& Facts,
			ContextResource::FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			ExecutionMutation::FExecutionMutationError& OutError) override;

	private:
		TArray<FUeAgentGenericEffectBinding> Bindings;
		TSharedRef<
			IUeAgentGenericEffectUeHook,
			ESPMode::ThreadSafe> UeHook;
		FCriticalSection StateMutex;
		bool bExecutionStarted = false;
		bool bRollbackAttempted = false;
		FString AppliedOperationId;
		TOptional<FUeAgentGenericEffectApplyResult> AppliedResult;
	};

	class UEAGENTINTERFACE_API FUeAgentGenericEffectBarrierProvider final
		: public ExecutionMutation::IAsyncBarrierProvider
	{
	public:
		explicit FUeAgentGenericEffectBarrierProvider(
			TSharedRef<
				IUeAgentGenericEffectUeHook,
				ESPMode::ThreadSafe> InUeHook);

		bool Wait(
			const ExecutionMutation::FBarrierRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			ExecutionMutation::FBarrierResult& OutResult,
			ExecutionMutation::FExecutionMutationError& OutError) override;

	private:
		TSharedRef<
			IUeAgentGenericEffectUeHook,
			ESPMode::ThreadSafe> UeHook;
	};

	class UEAGENTINTERFACE_API FUeAgentReflectionGenericEffectUeHook final
		: public IUeAgentGenericEffectUeHook
	{
	public:
		FUeAgentReflectionGenericEffectUeHook();
		~FUeAgentReflectionGenericEffectUeHook() override;

		bool TryApply(
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const ExecutionMutation::FLockFenceLease& Lease,
			const TArray<FUeAgentGenericEffectBinding>& Bindings,
			ContextResource::FUeAgentResourceTracker& Resources,
			FUeAgentGenericEffectApplyResult& OutResult,
			ExecutionMutation::FExecutionMutationError& OutError) override;
		bool TryRollback(
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const FUeAgentGenericEffectApplyResult& Applied,
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
		class FState;
		TSharedRef<FState, ESPMode::ThreadSafe> State;
	};
}
