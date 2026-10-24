#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::QueryCalibration
{
	UEAGENTINTERFACE_API bool TryCreateQueryCalibrationContribution(
		TSharedPtr<
			Active::IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateProductionQueryCalibrationContribution(
		TSharedPtr<
			Active::IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateQueryCalibrationRegistration(
		TSharedPtr<
			const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>&
			OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError);
}
