#pragma once

#include "CoreMinimal.h"
#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Composition::ReferenceCrud
{
	struct UEAGENTINTERFACE_API
		FUeAgentReferenceCrudApplicationConfig
	{
		TOptional<Contract::FUeAgentDescriptorIdentity>
			OperationDescriptor;
		TSharedPtr<
			const Contract::FUeAgentDescriptorRegistry>
			OperationDescriptorRegistry;
		ContextResource::FUeAgentExecutionContextInput
			ExecutionContext;
		FString ServerSessionId;
		int64 RegistryEpoch = 0;
		double TimeoutSeconds = 0.0;
		int64 MaxFrameBytes = 0;
		FString JournalRoot;
	};

	struct UEAGENTINTERFACE_API
		FUeAgentReferenceCrudApplicationError
	{
		FString Code;
		FString Detail;
	};

	struct UEAGENTINTERFACE_API
		FUeAgentReferenceCrudCommand
	{
		FString RequestId;
		FString IdempotencyKey;
		FString ObjectPath;
		FString Intent;
		TOptional<FString> ProposedText;

		bool IsValid() const;
	};

	class UEAGENTINTERFACE_API
		FUeAgentReferenceCrudApplication final
		: public Contract::IUeAgentOperationApplicationPort
	{
	public:
		~FUeAgentReferenceCrudApplication() override;

		static bool TryCreate(
			const FUeAgentReferenceCrudApplicationConfig& Config,
			TSharedRef<const
				BindingGateway::Ticket::
					FUeAgentOperationTicketService> TicketService,
			TSharedRef<Discovery::IUeAgentStateReadPort> ReadPort,
			TSharedPtr<FUeAgentReferenceCrudApplication>&
				OutApplication,
			FUeAgentReferenceCrudApplicationError& OutError);

		Contract::FUeAgentOperationResult Submit(
			const Contract::FUeAgentOperationRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<
				Contract::IUeAgentCancellationSignal>&
				Cancellation) override;

		Contract::FUeAgentOperationResult SubmitCommand(
			const FUeAgentReferenceCrudCommand& Command,
			const Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<
				Contract::IUeAgentCancellationSignal>&
				Cancellation);

		TSharedRef<
			const BindingGateway::Operation::
				FUeAgentOperationBinding>
		GetOperationBinding() const;

	private:
		class FState;

		explicit FUeAgentReferenceCrudApplication(
			TSharedRef<FState, ESPMode::ThreadSafe> InState);

		Contract::FUeAgentOperationResult
		SubmitWithExecutionContext(
			const Contract::FUeAgentOperationRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<
				ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe>& Resources,
			const ContextResource::FUeAgentOperationDeadline&
				Deadline,
			const ContextResource::FUeAgentCancellationToken&
				Cancellation);

		TSharedRef<FState, ESPMode::ThreadSafe> State;
	};
}
