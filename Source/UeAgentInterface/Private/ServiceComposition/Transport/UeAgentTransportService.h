#pragma once

#include "NetworkTransport/UeAgentNetworkTransport.h"

#include "Templates/UniquePtr.h"

namespace UE::UAI::ServiceComposition::Transport
{
	struct FUeAgentTransportServiceConfig
	{
		FString OwnerId;
		NetworkTransport::FUeAgentTransportCapabilities Capabilities;
		NetworkTransport::FUeAgentDeliveryRequirements Requirements;

		bool IsValid() const;
	};

	class UEAGENTINTERFACE_API FUeAgentTransportService final
	{
	public:
		~FUeAgentTransportService();

		static bool TryCreate(
			const FUeAgentTransportServiceConfig& Config,
			TUniquePtr<
				NetworkTransport::IUeAgentNetworkServerControl> Control,
			TUniquePtr<FUeAgentTransportService>& OutService,
			NetworkTransport::FUeAgentTransportDiagnostic& OutDiagnostic);

		bool Start(
			NetworkTransport::FUeAgentTransportDiagnostic& OutDiagnostic);
		bool Stop(
			NetworkTransport::FUeAgentTransportDiagnostic& OutDiagnostic);
		bool IsRunning() const;
		const NetworkTransport::FUeAgentTransportServerIdentity&
			GetIdentity() const;

	private:
		FUeAgentTransportService(
			FUeAgentTransportServiceConfig InConfig,
			TUniquePtr<
				NetworkTransport::IUeAgentNetworkServerControl> InControl,
			NetworkTransport::FUeAgentCapabilityBinding InBinding);

		FUeAgentTransportServiceConfig Config;
		TUniquePtr<NetworkTransport::IUeAgentNetworkServerControl> Control;
		NetworkTransport::FUeAgentCapabilityBinding Binding;
		NetworkTransport::FUeAgentTransportServerIdentity Identity;
		bool bStarted = false;
	};
}
