#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentDiagnostic.h"

namespace UE::UAI::CommandContract
{
	/**
	 * Executes one private mechanics handler through its canonical command
	 * contract. The binding, request envelope and request identity remain
	 * machine-side implementation details.
	 */
	UEAGENTINTERFACE_API bool TryExecuteInternalCommand(
		const FUeAgentCommandContract& Contract,
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Handler,
		const FString& ParamsJson,
		const Contract::FUeAgentCallerContext& Caller,
		const FString& ErrorPrefix,
		FString& OutPayloadJson,
		Contract::FDiagnostic& OutError);
}
