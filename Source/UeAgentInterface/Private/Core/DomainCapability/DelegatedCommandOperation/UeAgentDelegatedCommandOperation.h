#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"

namespace UE::UAI::DomainCapability::DelegatedCommandOperation
{
	using FMechanicsResolver = bool (*)(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError);

	/**
	 * Adapts one existing exact mechanics handler into the common request-scoped
	 * operation lifecycle. The delegated handler remains the sole domain owner;
	 * this adapter only interprets its terminal result and never reimplements UE
	 * mutation, persistence, settle, or readback behavior.
	 */
	UEAGENTINTERFACE_API TSharedRef<const IDomainOperationInvocationFactory>
	CreateInvocationFactory(
		FString OperationId,
		FMechanicsResolver Resolver,
		bool bPersists,
		bool bIndependentReadback);
}
