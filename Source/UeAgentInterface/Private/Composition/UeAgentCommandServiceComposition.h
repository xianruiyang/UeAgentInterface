#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "NetworkTransport/UeAgentNetworkTransport.h"

#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

namespace UE::UAI::Composition
{
	struct FUeAgentCommandServiceConfig
	{
		FString PrincipalSubject;
		TArray<FString> AuthorityScopes;

		bool IsValid() const;
	};

	class UEAGENTINTERFACE_API
		FUeAgentCommandServiceComposition final
	{
	public:
		~FUeAgentCommandServiceComposition();

		static bool TryCreate(
			const FUeAgentCommandServiceConfig& Config,
			TSharedRef<
				const CommandContract::FUeAgentCommandContractRegistry>
				Registry,
			TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>
				Handlers,
			TUniquePtr<FUeAgentCommandServiceComposition>&
				OutComposition,
			FString& OutError);

		TSharedRef<
			NetworkTransport::IUeAgentTransportRequestHandler,
			ESPMode::ThreadSafe> GetIngressHandler() const;

	private:
		struct FImpl;

		explicit FUeAgentCommandServiceComposition(
			TUniquePtr<FImpl> InImpl);

		TUniquePtr<FImpl> Impl;
	};
}
