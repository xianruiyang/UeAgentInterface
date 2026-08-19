#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/ReferenceCrud/UeAgentReferenceCrudApplication.h"
#include "Core/Contract/UeAgentContract.h"

#include "Templates/SharedPointer.h"

namespace UE::UAI::Composition::ReferenceCommand
{
	struct FUeAgentReferenceCommandHandlerConfig
	{
		FString ServerSessionId;
		int64 RegistryEpoch = 0;
		FString RegistryHash;
		Contract::FUeAgentDescriptorIdentity QueryDescriptor;
		TSharedRef<Contract::IUeAgentQueryApplicationPort> QueryPort;
		TSharedRef<
			ReferenceCrud::FUeAgentReferenceCrudApplication>
			OperationApplication;

		FUeAgentReferenceCommandHandlerConfig(
			FString InServerSessionId,
			int64 InRegistryEpoch,
			FString InRegistryHash,
			Contract::FUeAgentDescriptorIdentity InQueryDescriptor,
			TSharedRef<Contract::IUeAgentQueryApplicationPort>
				InQueryPort,
			TSharedRef<
				ReferenceCrud::FUeAgentReferenceCrudApplication>
				InOperationApplication);

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API bool
	TryCreateReferenceCommandHandlers(
		const FUeAgentReferenceCommandHandlerConfig& Config,
		TArray<
			TSharedRef<CommandEdge::IUeAgentCommandHandler>>&
			OutHandlers,
		FString& OutError);
}
