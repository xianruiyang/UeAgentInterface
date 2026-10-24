#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Templates/SharedPointer.h"

#define UAI_GRAPH_COMPOUND_RUNTIME_V1 1

namespace UE::UAI::Composition::GraphCompound
{
	struct UEAGENTINTERFACE_API FGraphCompoundRuntimeConfig
	{
	};

	using FGraphCompoundRuntimeError = Contract::FDiagnostic;

	/**
	 * One exact C3Q owner. The binding identity is the only dispatch key;
	 * command routes and domain selection remain outside this contract.
	 */
	class UEAGENTINTERFACE_API IGraphCompoundQueryBinding
	{
	public:
		virtual ~IGraphCompoundQueryBinding() = default;

		virtual const Contract::FUeAgentDescriptorIdentity&
			GetDescriptorIdentity() const = 0;
		virtual const FString& GetOwnerId() const = 0;
		virtual bool TryExecute(
			const Contract::FUeAgentQueryRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation,
			Contract::FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError) = 0;
	};

	/** Exact C3O owner; operation idempotency remains part of the request. */
	class UEAGENTINTERFACE_API IGraphCompoundOperationBinding
	{
	public:
		virtual ~IGraphCompoundOperationBinding() = default;

		virtual const Contract::FUeAgentDescriptorIdentity&
			GetDescriptorIdentity() const = 0;
		virtual const FString& GetOwnerId() const = 0;
		virtual bool TryExecute(
			const Contract::FUeAgentOperationRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation,
			Contract::FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError) = 0;
	};

	/**
	 * Common Graph runtime. Construction is all-or-nothing relative to the
	 * supplied immutable descriptor registry: every descriptor has one exact
	 * same-lane native owner before either application port becomes visible.
	 */
	class UEAGENTINTERFACE_API FGraphCompoundRuntime final
	{
	public:
		class FState;

		static bool TryCreate(
			const FGraphCompoundRuntimeConfig& Config,
			TSharedRef<const Contract::FUeAgentDescriptorRegistry>
				DescriptorRegistry,
			TArray<TSharedRef<IGraphCompoundQueryBinding>> QueryBindings,
			TArray<TSharedRef<IGraphCompoundOperationBinding>>
				OperationBindings,
			TSharedPtr<FGraphCompoundRuntime>& OutRuntime,
			FGraphCompoundRuntimeError& OutError);

		const TSharedRef<Contract::IUeAgentQueryApplicationPort>&
			GetQueryPort() const;
		const TSharedRef<Contract::IUeAgentOperationApplicationPort>&
			GetOperationPort() const;
		int32 GetQueryBindingCount() const;
		int32 GetOperationBindingCount() const;

	private:
		FGraphCompoundRuntime(
			TSharedRef<FState, ESPMode::ThreadSafe> InState,
			TSharedRef<Contract::IUeAgentQueryApplicationPort> InQueryPort,
			TSharedRef<Contract::IUeAgentOperationApplicationPort>
				InOperationPort);

		TSharedRef<FState, ESPMode::ThreadSafe> State;
		TSharedRef<Contract::IUeAgentQueryApplicationPort> QueryPort;
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort;
	};
}
