#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"

namespace UE::UAI::OperationOwners::Landscape
{
	UEAGENTINTERFACE_API const TArray<Composition::SimpleAuthoring::
		FUeAgentSimpleAuthoringCommandRoute>& GetRoutes();

	UEAGENTINTERFACE_API TSharedRef<const DomainCapability::
		IDomainOperationInvocationFactory> FindOperationInvocationFactory(
			const FString& OperationId);

	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}
