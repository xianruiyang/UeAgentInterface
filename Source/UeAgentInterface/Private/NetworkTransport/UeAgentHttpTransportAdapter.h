#pragma once

#include "NetworkTransport/UeAgentNetworkTransport.h"

#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

namespace UE::UAI::NetworkTransport
{
	UEAGENTINTERFACE_API const FString& GetUaiHttpCommandRoutePath();
	UEAGENTINTERFACE_API const FString& GetUaiHttpProbeRoutePath();

	inline const FString& GetUaiCommandRoutePath()
	{
		return GetUaiHttpCommandRoutePath();
	}

	struct FUeAgentHttpRequest
	{
		FString Method;
		FString RoutePath;
		TMap<FString, FString> Headers;
		TArray<uint8> Body;
		FString PeerId;
	};

	struct FUeAgentHttpResponse
	{
		int32 StatusCode = 500;
		TMap<FString, FString> Headers;
		TArray<uint8> Body;
	};

	using FUeAgentHttpRequestHandler =
		TFunction<FUeAgentHttpResponse(const FUeAgentHttpRequest&)>;

	class UEAGENTINTERFACE_API IUeAgentHttpRouteHost
	{
	public:
		virtual ~IUeAgentHttpRouteHost() = default;

		virtual bool Bind(
			const FString& InstanceId,
			const FString& Method,
			const FString& RoutePath,
			FUeAgentHttpRequestHandler Handler,
			FUeAgentTransportDiagnostic& OutDiagnostic) = 0;
		virtual bool Unbind(
			const FString& InstanceId,
			FUeAgentTransportDiagnostic& OutDiagnostic) = 0;
		virtual bool IsBound(const FString& InstanceId) const = 0;
	};

	struct FUeAgentHttpAdapterConfig
	{
		FString OwnerId;
		FString InstanceId;
		FString Method;
		FString RoutePath;
		FString ProbeRoutePath;
		FString AuthHeader;
		FString AuthToken;
		FString Channel;
		FString PrincipalSubject;
		int64 MaxFrameBytes = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentHttpTransportAdapter final
		: public IUeAgentNetworkServerControl
	{
	public:
		FUeAgentHttpTransportAdapter(
			FUeAgentHttpAdapterConfig InConfig,
			TSharedRef<IUeAgentHttpRouteHost, ESPMode::ThreadSafe> InRouteHost,
			TSharedRef<IUeAgentTransportRequestHandler, ESPMode::ThreadSafe> InHandler);
		virtual ~FUeAgentHttpTransportAdapter() override;

		virtual bool Start(
			FUeAgentTransportServerIdentity& OutIdentity,
			FUeAgentTransportDiagnostic& OutDiagnostic) override;
		virtual bool Stop(
			const FString& OwnerId,
			FUeAgentTransportDiagnostic& OutDiagnostic) override;
		virtual FUeAgentTransportServerHealth Health() const override;
		virtual FUeAgentTransportServerIdentity Identity() const override;

		FUeAgentHttpResponse HandleRequest(
			const FUeAgentHttpRequest& Request) const;

	private:
		FString ProbeBindingId() const;
		FUeAgentHttpResponse HandleProbeRequest(
			const FUeAgentHttpRequest& Request) const;
		bool ValidateConfig(FUeAgentTransportDiagnostic& OutDiagnostic) const;
		FUeAgentHttpResponse MakeFailure(
			int32 StatusCode,
			const FString& Code,
			const FString& Message) const;
		FString FindHeader(
			const TMap<FString, FString>& Headers,
			const FString& Name) const;

		FUeAgentHttpAdapterConfig Config;
		TSharedRef<IUeAgentHttpRouteHost, ESPMode::ThreadSafe> RouteHost;
		TSharedRef<IUeAgentTransportRequestHandler, ESPMode::ThreadSafe> Handler;
		bool bCommandRouteBound = false;
		bool bProbeRouteBound = false;
		bool bRunning = false;
	};

	class UEAGENTINTERFACE_API FUeAgentEngineHttpRouteHost final
		: public IUeAgentHttpRouteHost
	{
	public:
		explicit FUeAgentEngineHttpRouteHost(uint32 Port);
		virtual ~FUeAgentEngineHttpRouteHost() override;

		virtual bool Bind(
			const FString& InstanceId,
			const FString& Method,
			const FString& RoutePath,
			FUeAgentHttpRequestHandler Handler,
			FUeAgentTransportDiagnostic& OutDiagnostic) override;
		virtual bool Unbind(
			const FString& InstanceId,
			FUeAgentTransportDiagnostic& OutDiagnostic) override;
		virtual bool IsBound(const FString& InstanceId) const override;

	private:
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};
}
