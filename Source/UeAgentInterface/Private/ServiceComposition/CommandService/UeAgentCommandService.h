#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "NetworkTransport/UeAgentNetworkTransport.h"

#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

namespace UE::UAI::ServiceComposition::CommandService
{
	class UEAGENTINTERFACE_API IUeAgentRequestContextInvalidator
	{
	public:
		virtual ~IUeAgentRequestContextInvalidator() = default;

		virtual void InvalidateWorld(
			const FString& WorldIdentity) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentCommandService final
	{
	public:
		static bool TryCreate(
			TSharedRef<
				NetworkTransport::IUeAgentTransportRequestHandler,
				ESPMode::ThreadSafe> IngressHandler,
			TSharedRef<
				IUeAgentRequestContextInvalidator,
				ESPMode::ThreadSafe> ContextInvalidator,
			TUniquePtr<FUeAgentCommandService>& OutService,
			UE::UAI::Contract::FDiagnostic& OutError);

		TSharedRef<
			NetworkTransport::IUeAgentTransportRequestHandler,
			ESPMode::ThreadSafe> GetIngressHandler() const;
		void NotifyWorldChanged(const FString& WorldIdentity);
		uint64 GetContextGeneration() const;

	private:
		FUeAgentCommandService(
			TSharedRef<
				NetworkTransport::IUeAgentTransportRequestHandler,
				ESPMode::ThreadSafe> InIngressHandler,
			TSharedRef<
				IUeAgentRequestContextInvalidator,
				ESPMode::ThreadSafe> InContextInvalidator);

		TSharedRef<
			NetworkTransport::IUeAgentTransportRequestHandler,
			ESPMode::ThreadSafe> IngressHandler;
		TSharedRef<
			IUeAgentRequestContextInvalidator,
			ESPMode::ThreadSafe> ContextInvalidator;
		uint64 ContextGeneration = 0;
	};

	UEAGENTINTERFACE_API TSharedRef<
		NetworkTransport::IUeAgentTransportRequestHandler,
		ESPMode::ThreadSafe> MakeUnavailableCommandIngress();
}
