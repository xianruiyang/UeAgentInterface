#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundNativeHook.h"

namespace UE::UAI::Composition::NiagaraCompound
{
	struct UEAGENTINTERFACE_API FUeAgentNiagaraCompoundApplicationConfig
	{
		ContextResource::FUeAgentExecutionContextInput
			ExecutionContext;
		FString ServerSessionId;
		int64 RegistryEpoch = 0;
		FString JournalRoot;
	};

	using FUeAgentNiagaraCompoundApplicationError = Contract::FDiagnostic;

	class UEAGENTINTERFACE_API FUeAgentNiagaraCompoundApplication final
	{
	public:
		~FUeAgentNiagaraCompoundApplication();

		static bool TryCreate(
			const FUeAgentNiagaraCompoundApplicationConfig& Config,
			TSharedRef<const
				BindingGateway::Ticket::
					FUeAgentOperationTicketService> TicketService,
			TSharedRef<Discovery::IUeAgentStateReadPort> ReadPort,
			TSharedRef<
				DomainAdapters::NiagaraCompound::
					FUeAgentNiagaraCompoundNativeHook,
				ESPMode::ThreadSafe> NativeHook,
			TSharedPtr<FUeAgentNiagaraCompoundApplication>&
				OutApplication,
			FUeAgentNiagaraCompoundApplicationError& OutError);

		TSharedRef<const
			BindingGateway::Operation::FUeAgentOperationBinding>
		GetOperationBinding() const;

	private:
		class FState;

		explicit FUeAgentNiagaraCompoundApplication(
			TSharedRef<FState, ESPMode::ThreadSafe> InState);

		TSharedRef<FState, ESPMode::ThreadSafe> State;
	};
}
