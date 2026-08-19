#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "DomainAdapters/QueryCalibration/UeAgentQueryCalibrationSource.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::QueryCalibration
{
	UEAGENTINTERFACE_API bool TryCreateQueryCalibrationContribution(
		TSharedRef<
			DomainAdapters::QueryCalibration::IUeAgentQueryCalibrationSource>
			Source,
		TSharedPtr<
			Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateProductionQueryCalibrationContribution(
		TSharedPtr<
			Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCreateQueryCalibrationRegistration(
		TSharedRef<
			DomainAdapters::QueryCalibration::IUeAgentQueryCalibrationSource>
			Source,
		TSharedPtr<
			const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>&
			OutDispatch,
		FString& OutError);
}
