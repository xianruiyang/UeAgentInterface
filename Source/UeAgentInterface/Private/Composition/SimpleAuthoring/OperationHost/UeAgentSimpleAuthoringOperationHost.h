#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/Discovery/UeAgentDiscovery.h"

class UWorld;

namespace UE::UAI::Composition::SimpleAuthoring::OperationHost
{
	/**
	 * Resolves one exact SimpleAuthoring mechanics handler from the single
	 * process-owned runtime. Domain owners remain responsible for public
	 * authority, command inventory, and public failure vocabulary.
	 */
	UEAGENTINTERFACE_API bool TryResolveHandler(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		FString& OutError);

	/**
	 * Resolves one catalog kind from the same process-owned SimpleAuthoring
	 * runtime used by mutation planning. Query owners may project that catalog,
	 * but may not construct or cache a second catalog owner.
	 */
	UEAGENTINTERFACE_API bool TryResolveCatalogSource(
		const FString& CatalogKind,
		TSharedPtr<Discovery::IUeAgentCatalogSource>& OutSource,
		FString& OutError);

	/**
	 * Returns the stable request-scoped OperationService adapter for one exact
	 * SimpleAuthoring command. The adapter delegates the domain mutation once to
	 * the process-owned runtime and exposes its verified durable result through
	 * the common operation lifecycle without introducing a second mechanics path.
	 */
	UEAGENTINTERFACE_API TSharedRef<const DomainCapability::
		IDomainOperationInvocationFactory> GetOperationInvocationFactory(
			const FString& CommandId);

#if WITH_DEV_AUTOMATION_TESTS
	/** Rebinds only the owned automation process to a test-owned real UWorld. */
	UEAGENTINTERFACE_API void SetAutomationWorldOverride(UWorld* World);

	/**
	 * Replaces one delegated mechanics handler inside the owned automation
	 * process. Passing null removes the override. This seam verifies the common
	 * lifecycle adapter's terminal semantics without creating a second runtime.
	 */
	UEAGENTINTERFACE_API void SetAutomationHandlerOverride(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler> Handler);
#endif
}
