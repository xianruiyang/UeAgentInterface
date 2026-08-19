#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/ReferenceCrud/UeAgentReferenceCrudApplication.h"

namespace UE::UAI::DomainAdapters::Reference
{
	UEAGENTINTERFACE_API const FString& GetCommandId();
	UEAGENTINTERFACE_API bool TryCreateApplication(
		TSharedPtr<Composition::ReferenceCrud::FUeAgentReferenceCrudApplication>&
			OutApplication,
		FString& OutError);
	UEAGENTINTERFACE_API bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError);
}
