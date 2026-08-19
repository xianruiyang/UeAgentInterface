#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"

namespace UE::UAI::OperationOwners::AssetSimple
{
	UEAGENTINTERFACE_API const TArray<Composition::SimpleAuthoring::
		FUeAgentSimpleAuthoringCommandRoute>&
	GetAssetSimpleOperationRoutes();

	/**
	 * Submits one exact T37 command through the process-owned SimpleAuthoring
	 * Operation application. The lazy host owns one runtime for the process;
	 * transport and CommandService reconstruction never create a second copy.
	 */
	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult
	SubmitProductionAssetSimpleOperation(
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& CallerContext,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}
