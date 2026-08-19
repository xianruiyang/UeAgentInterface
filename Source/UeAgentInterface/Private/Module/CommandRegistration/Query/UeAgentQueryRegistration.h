#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "DomainAdapters/Query/UeAgentQueryService.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::Query
{
	UEAGENTINTERFACE_API bool TryCreateQueryContribution(
		TSharedRef<DomainAdapters::Query::IUeAgentQueryService> Service,
		TSharedPtr<Active::IUeAgentActiveCommandContribution>&
			OutContribution,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionQueryContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>&
			OutContribution,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCreateQueryRegistration(
		TSharedRef<DomainAdapters::Query::IUeAgentQueryService> Service,
		TSharedPtr<
			const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>&
			OutDispatch,
		FString& OutError);
}
