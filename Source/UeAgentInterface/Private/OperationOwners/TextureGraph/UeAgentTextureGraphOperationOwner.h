#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "CoreMinimal.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"

namespace UE::UAI::OperationOwners::TextureGraph
{
	UEAGENTINTERFACE_API TSharedPtr<const DomainCapability::
		IDomainOperationInvocationFactory>
	FindTextureGraphOperationInvocationFactory(const FString& OperationId);

	UEAGENTINTERFACE_API bool TryCreateMechanicsHandlers(
		TSharedRef<const DomainCapability::IDomainOperationInvocationFactory>
			BakeFactory,
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError);

	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}
