#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/Discovery/Command/UeAgentCommandDiscovery.h"

namespace UE::UAI::CommandHandlers::QueryCalibration
{
	UEAGENTINTERFACE_API bool TryCreateQueryCalibrationHandlers(
		TSharedRef<const Discovery::Command::FCommandDiscoveryService>
			Discovery,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
			OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);
}
