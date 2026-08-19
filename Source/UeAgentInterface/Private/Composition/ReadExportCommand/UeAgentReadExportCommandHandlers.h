#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Composition::ReadExportCommand
{
	struct UEAGENTINTERFACE_API
		FUeAgentReadExportCommandRoute final
	{
		FString CommandId;
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity;
		bool bRequiresTarget = false;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API const TArray<
		FUeAgentReadExportCommandRoute>&
	GetReadExportQueryCommandRoutes();

	UEAGENTINTERFACE_API bool
	TryCreateReadExportDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>&
			OutRegistry,
		FString& OutError);

	struct UEAGENTINTERFACE_API
		FUeAgentReadExportCommandHandlerConfig final
	{
		FString ServerSessionId;
		int64 RegistryEpoch = 0;
		double TimeoutSeconds = 0.0;
		int64 MaxFrameBytes = 0;
		TSharedRef<
			const Contract::FUeAgentDescriptorRegistry>
				DescriptorRegistry;
		TSharedRef<const BindingGateway::Query::
			FUeAgentQueryBinding> QueryBinding;
		TSharedRef<BindingGateway::Query::
			IUeAgentQuerySessionSource> SessionSource;
		TSharedRef<const BindingGateway::Query::
			FUeAgentQueryTicketService> TicketService;

		FUeAgentReadExportCommandHandlerConfig(
			FString InServerSessionId,
			int64 InRegistryEpoch,
			double InTimeoutSeconds,
			int64 InMaxFrameBytes,
			TSharedRef<
				const Contract::FUeAgentDescriptorRegistry>
					InDescriptorRegistry,
			TSharedRef<const BindingGateway::Query::
				FUeAgentQueryBinding> InQueryBinding,
			TSharedRef<BindingGateway::Query::
				IUeAgentQuerySessionSource> InSessionSource,
			TSharedRef<const BindingGateway::Query::
				FUeAgentQueryTicketService> InTicketService);

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API bool TryCreateReadExportCommandHandlers(
		const FUeAgentReadExportCommandHandlerConfig& Config,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>&
			OutHandlers,
		FString& OutError);
}
