#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"

namespace UE::UAI::OperationOwners::AssetSimple
{
	UEAGENTINTERFACE_API const TArray<Composition::SimpleAuthoring::
		FUeAgentSimpleAuthoringCommandRoute>&
	GetAssetSimpleOperationRoutes();

	UEAGENTINTERFACE_API TSharedPtr<const DomainCapability::
		IDomainOperationInvocationFactory> FindOperationInvocationFactory(
			const FString& OperationId);

	/**
	 * Submits one exact AssetSimple command through the process-owned
	 * SimpleAuthoring mechanics and the common Operation lifecycle.
	 * Operation application. The lazy host owns one runtime for the process;
	 * transport and CommandService reconstruction never create a second copy.
	 */
	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult
	SubmitProductionAssetSimpleOperation(
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& CallerContext,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}
