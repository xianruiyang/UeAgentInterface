#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "NetworkTransport/UeAgentNetworkTransport.h"

namespace UE::UAI::ServiceComposition::Query
{
	UEAGENTINTERFACE_API bool TryCreateQueryCommandIngress(
		TSharedRef<const CommandEdge::FUeAgentExactCommandDispatch>
			Dispatch,
		FString PrincipalSubject,
		TArray<FString> AuthorityScopes,
		TSharedPtr<
			NetworkTransport::IUeAgentTransportRequestHandler,
			ESPMode::ThreadSafe>& OutIngress,
		FString& OutError);
}
