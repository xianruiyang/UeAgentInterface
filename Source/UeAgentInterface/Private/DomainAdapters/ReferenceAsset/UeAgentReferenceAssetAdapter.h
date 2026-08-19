#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "DomainAdapters/GenericRead/UeAgentGenericReadAdapter.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::ReferenceAsset
{
	UEAGENTINTERFACE_API const FString& GetReferenceAssetCatalogKind();
	UEAGENTINTERFACE_API const FString& GetReferenceAssetCatalogSourceId();
	UEAGENTINTERFACE_API const FString& GetReferenceAssetReadPortId();
	UEAGENTINTERFACE_API const FString& GetReferenceAssetAllowedRoot();

	UEAGENTINTERFACE_API bool TryNormalizeReferenceAssetPath(
		const FString& Input,
		FString& OutPackageName,
		FString& OutObjectPath,
		FString& OutErrorCode);

	class UEAGENTINTERFACE_API FUeAgentReferenceAssetCatalogSource final
		: public Discovery::IUeAgentCatalogSource
	{
	public:
		FUeAgentReferenceAssetCatalogSource(
			Contract::FUeAgentDescriptorIdentity InOperationDescriptor,
			FString InGeneration);

		const FString& GetSourceId() const override;
		const TArray<FString>& GetCatalogKinds() const override;
		bool TrySearch(
			const Discovery::FUeAgentDiscoveryQuery& Query,
			ContextResource::FUeAgentResourceTracker& Resources,
			const ContextResource::FUeAgentOperationDeadline& Deadline,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const Discovery::FUeAgentCatalogSnapshot>& OutCatalog,
			Discovery::FUeAgentDiscoveryError& OutError) override;
		bool TryResolveExact(
			const FString& CandidateId,
			const Discovery::FUeAgentCatalogRevision& ExpectedRevision,
			ContextResource::FUeAgentResourceTracker& Resources,
			const ContextResource::FUeAgentOperationDeadline& Deadline,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const Discovery::FUeAgentCandidateBinding>& OutBinding,
			Discovery::FUeAgentDiscoveryError& OutError) override;

	private:
		Contract::FUeAgentDescriptorIdentity OperationDescriptor;
		FString Generation;
		TArray<FString> Kinds;
		FCriticalSection CacheMutex;
		TMap<FString, FString> ObjectPathByCandidateId;
	};

	class UEAGENTINTERFACE_API FUeAgentReferenceAssetReadHook final
		: public DomainAdapters::GenericRead::IUeAgentUeReadHook
	{
	public:
		explicit FUeAgentReferenceAssetReadHook(FString InGeneration);

		bool TryCapture(
			const Discovery::FUeAgentStateReadRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			const ContextResource::FUeAgentOperationDeadline& Deadline,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const
				DomainAdapters::GenericRead::FUeAgentUeReadCapture>&
				OutCapture,
			DomainAdapters::GenericRead::FUeAgentGenericReadError&
				OutError) override;

	private:
		FString Generation;
	};

	class UEAGENTINTERFACE_API FUeAgentReferenceAssetEffectHook final
		: public DomainAdapters::GenericEffect::IUeAgentGenericEffectUeHook
	{
	public:
		FUeAgentReferenceAssetEffectHook();
		~FUeAgentReferenceAssetEffectHook() override;

		bool TryApply(
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const ExecutionMutation::FLockFenceLease& Lease,
			const TArray<
				DomainAdapters::GenericEffect::
					FUeAgentGenericEffectBinding>& Bindings,
			ContextResource::FUeAgentResourceTracker& Resources,
			DomainAdapters::GenericEffect::
				FUeAgentGenericEffectApplyResult& OutResult,
			ExecutionMutation::FExecutionMutationError& OutError)
			override;
		bool TryRollback(
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const DomainAdapters::GenericEffect::
				FUeAgentGenericEffectApplyResult& Applied,
			ContextResource::FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			ExecutionMutation::FExecutionMutationError& OutError)
			override;
		void ReleaseVerifiedOperation(
			const FString& OperationId) override;
		bool TryWaitForSettle(
			const ExecutionMutation::FBarrierRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			ExecutionMutation::FBarrierResult& OutResult,
			ExecutionMutation::FExecutionMutationError& OutError)
			override;

	private:
		class FState;
		TSharedRef<FState, ESPMode::ThreadSafe> State;
		TSharedRef<
			DomainAdapters::GenericEffect::
				FUeAgentReflectionGenericEffectUeHook,
			ESPMode::ThreadSafe> Reflection;
	};
}
