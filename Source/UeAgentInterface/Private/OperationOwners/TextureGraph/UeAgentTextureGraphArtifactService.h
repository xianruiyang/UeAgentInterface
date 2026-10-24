#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"

namespace UE::UAI::OperationOwners::TextureGraph
{
	UEAGENTINTERFACE_API TSharedRef<const DomainCapability::
		IDomainOperationInvocationFactory>
	GetTextureGraphBakeInvocationFactory();

	UEAGENTINTERFACE_API bool TryCreateArtifactHandlers(
		TSharedRef<const DomainCapability::IDomainOperationInvocationFactory>
			BakeFactory,
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>&
			OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateArtifactHandlers(
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>&
			OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);
}
