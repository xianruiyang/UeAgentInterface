#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/Discovery/Command/UeAgentCommandDiscovery.h"
#include "DomainAdapters/QueryCalibration/UeAgentQueryCalibrationSource.h"

namespace UE::UAI::CommandHandlers::QueryCalibration
{
	UEAGENTINTERFACE_API bool TryCreateQueryCalibrationHandlers(
		TSharedRef<const Discovery::Command::FCommandDiscoveryService>
			Discovery,
		TSharedRef<
			DomainAdapters::QueryCalibration::IUeAgentQueryCalibrationSource>
			Source,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
			OutHandlers,
		FString& OutError);
}
