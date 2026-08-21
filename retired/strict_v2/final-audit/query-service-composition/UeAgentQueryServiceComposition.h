#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "NetworkTransport/UeAgentNetworkTransport.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

class AWorldSettings;

namespace UE::UAI::NetworkTransport
{
	class IUeAgentHttpRouteHost;
}

namespace UE::UAI::Composition
{
	struct UEAGENTINTERFACE_API FUeAgentQueryServiceConfig
	{
		FString OwnerId;
		FString InstanceId;
		FString ServerSessionId;
		int64 RegistryEpoch = 0;
		FString AuthToken;
		FString TicketIntegrityKey;
		double QueryTimeoutSeconds = 0.0;
		double TicketTtlSeconds = 0.0;
		int64 MaxFrameBytes = 0;
	};

	struct UEAGENTINTERFACE_API FUeAgentQueryServiceError
	{
		FString Code;
		FString Detail;
	};

	UEAGENTINTERFACE_API const FString& GetReferenceQueryRegistryHash();
	UEAGENTINTERFACE_API
	Contract::FUeAgentDescriptorIdentity
	GetReferenceQueryDescriptorIdentity();
	UEAGENTINTERFACE_API
	Contract::FUeAgentDescriptorIdentity
	GetReferenceOperationDescriptorIdentity();
	UEAGENTINTERFACE_API bool TryCreateReferenceOperationDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError);

class UEAGENTINTERFACE_API FUeAgentQueryServiceComposition final
	{
	public:
		~FUeAgentQueryServiceComposition();

		static bool TryCreateReference(
			const FUeAgentQueryServiceConfig& Config,
			AWorldSettings& WorldSettings,
			TSharedRef<
				NetworkTransport::IUeAgentHttpRouteHost,
				ESPMode::ThreadSafe> RouteHost,
			TUniquePtr<FUeAgentQueryServiceComposition>& OutComposition,
			FUeAgentQueryServiceError& OutError);

		bool Start(
			NetworkTransport::FUeAgentTransportDiagnostic& OutDiagnostic);
		bool Stop(
			NetworkTransport::FUeAgentTransportDiagnostic& OutDiagnostic);
		bool IsRunning() const;

	private:
		struct FImpl;

		explicit FUeAgentQueryServiceComposition(
			TUniquePtr<FImpl> InImpl);

		TUniquePtr<FImpl> Impl;
	};
}
