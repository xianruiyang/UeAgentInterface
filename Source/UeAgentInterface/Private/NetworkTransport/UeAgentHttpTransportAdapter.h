#pragma once

#include "NetworkTransport/UeAgentNetworkTransport.h"

#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

#include <atomic>

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
		static FUeAgentHttpResponse Failure(int32 StatusCode, const FString& Code, const FString& Message);

		int32 StatusCode = 500;
		TMap<FString, FString> Headers;
		TArray<uint8> Body;
	};

	using FUeAgentHttpRequestHandler =
		TFunction<FUeAgentHttpResponse(const FUeAgentHttpRequest&)>;
	using FUeAgentHttpImmediateHandler = TFunction<TOptional<FUeAgentHttpResponse>(const FUeAgentHttpRequest&)>;

	enum class EUeAgentHttpRouteExecution : uint8
	{
		GameThread,
		NetworkThread,
	};

	class UEAGENTINTERFACE_API IUeAgentHttpRouteHost
	{
	public:
		virtual ~IUeAgentHttpRouteHost() = default;

		virtual bool Bind(
			const FString& InstanceId,
			const FString& Method,
			const FString& RoutePath,
			FUeAgentHttpRequestHandler Handler,
			FUeAgentTransportDiagnostic& OutDiagnostic,
			EUeAgentHttpRouteExecution Execution = EUeAgentHttpRouteExecution::GameThread) = 0;
		virtual bool BindWithImmediate(const FString& InstanceId, const FString& Method, const FString& RoutePath,
			FUeAgentHttpRequestHandler Handler, FUeAgentHttpImmediateHandler Immediate, FUeAgentTransportDiagnostic& Diagnostic)
		{
			return Bind(InstanceId, Method, RoutePath, MoveTemp(Handler), Diagnostic);
		}
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
		TOptional<FUeAgentHttpResponse> HandleRequestInternal(const FUeAgentHttpRequest& Request, bool bConcurrent) const;
		FString ProbeBindingId() const;
		FUeAgentHttpResponse HandleProbeRequest(
			const FUeAgentHttpRequest& Request) const;
		bool ValidateConfig(FUeAgentTransportDiagnostic& OutDiagnostic) const;
		FString FindHeader(
			const TMap<FString, FString>& Headers,
			const FString& Name) const;

		FUeAgentHttpAdapterConfig Config;
		TSharedRef<IUeAgentHttpRouteHost, ESPMode::ThreadSafe> RouteHost;
		TSharedRef<IUeAgentTransportRequestHandler, ESPMode::ThreadSafe> Handler;
		std::atomic<bool> bCommandRouteBound{false};
		std::atomic<bool> bProbeRouteBound{false};
		std::atomic<bool> bRunning{false};
	};

	class UEAGENTINTERFACE_API FUeAgentSocketHttpRouteHost final
		: public IUeAgentHttpRouteHost
	{
	public:
		explicit FUeAgentSocketHttpRouteHost(uint32 Port);
		virtual ~FUeAgentSocketHttpRouteHost() override;

		virtual bool Bind(
			const FString& InstanceId,
			const FString& Method,
			const FString& RoutePath,
			FUeAgentHttpRequestHandler Handler,
			FUeAgentTransportDiagnostic& OutDiagnostic,
			EUeAgentHttpRouteExecution Execution = EUeAgentHttpRouteExecution::GameThread) override;
		bool BindWithImmediate(const FString& InstanceId, const FString& Method, const FString& RoutePath,
			FUeAgentHttpRequestHandler Handler, FUeAgentHttpImmediateHandler Immediate, FUeAgentTransportDiagnostic& Diagnostic) override;
		virtual bool Unbind(
			const FString& InstanceId,
			FUeAgentTransportDiagnostic& OutDiagnostic) override;
		virtual bool IsBound(const FString& InstanceId) const override;

	private:
		struct FImpl;
		TSharedPtr<FImpl, ESPMode::ThreadSafe> Impl;
	};
}
