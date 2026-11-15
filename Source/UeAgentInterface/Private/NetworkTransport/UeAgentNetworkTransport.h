#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"

namespace UE::UAI::NetworkTransport
{
	struct FUeAgentTransportPrincipal
	{
		FString Subject;
		FString AuthenticationMethod;
	};

	struct FUeAgentTransportPeer
	{
		FString PeerId;
		FUeAgentTransportPrincipal Principal;
	};

	class UEAGENTINTERFACE_API FUeAgentTransportFrame final
	{
	public:
		FUeAgentTransportFrame(
			FString InChannel,
			TArray<uint8> InPayload,
			FString InCorrelationId,
			FUeAgentTransportPeer InPeer);

		const FString& GetChannel() const;
		const TArray<uint8>& GetPayload() const;
		const FString& GetCorrelationId() const;
		const FUeAgentTransportPeer& GetPeer() const;

	private:
		FString Channel;
		TArray<uint8> Payload;
		FString CorrelationId;
		FUeAgentTransportPeer Peer;
	};

	struct FUeAgentTransportCapabilities
	{
		bool bOrdered = false;
		bool bReliable = false;
		bool bPreservesMessageBoundaries = false;
		bool bRequestReply = false;
		bool bDuplicateSuppression = false;
	};

	struct FUeAgentDeliveryRequirements
	{
		bool bRequireOrdered = false;
		bool bRequireReliable = false;
		bool bRequireMessageBoundaries = false;
		bool bRequireRequestReply = false;
		bool bRequireDuplicateSuppression = false;
	};

	struct FUeAgentCapabilityBinding
	{
		FUeAgentTransportCapabilities Capabilities;
		FUeAgentDeliveryRequirements Requirements;
	};

	struct FUeAgentTransportBindingError
	{
		FString Code;
		TArray<FString> MissingCapabilities;
	};

	UEAGENTINTERFACE_API bool TryBindCapabilities(
		const FUeAgentTransportCapabilities& Capabilities,
		const FUeAgentDeliveryRequirements& Requirements,
		FUeAgentCapabilityBinding& OutBinding,
		FUeAgentTransportBindingError& OutError);

	enum class EUeAgentTransportDeliveryState : uint8
	{
		Delivered,
		Backpressured,
		TimedOut,
		Disconnected,
		Failed,
		Unsupported,
	};

	struct FUeAgentTransportDiagnostic
	{
		FString Code;
		FString Message;
	};

	struct UEAGENTINTERFACE_API FUeAgentTransportReply
	{
		explicit FUeAgentTransportReply(FUeAgentTransportFrame InFrame);

		FUeAgentTransportFrame Frame;
	};

	struct FUeAgentTransportDelivery
	{
		FString CorrelationId;
		EUeAgentTransportDeliveryState State =
			EUeAgentTransportDeliveryState::Failed;
		TOptional<FUeAgentTransportReply> Reply;
		TOptional<FUeAgentTransportDiagnostic> Diagnostic;
		int32 Attempts = 1;
	};

	struct UEAGENTINTERFACE_API FUeAgentTransportIngress
	{
		FUeAgentTransportIngress(
			FUeAgentTransportFrame InFrame,
			FUeAgentTransportPeer InPeer);

		FUeAgentTransportFrame Frame;
		FUeAgentTransportPeer Peer;
	};

	struct FUeAgentTransportServerIdentity
	{
		FString OwnerId;
		FString InstanceId;
	};

	struct FUeAgentTransportServerHealth
	{
		bool bRunning = false;
		TOptional<FUeAgentTransportDiagnostic> Diagnostic;
	};

	class UEAGENTINTERFACE_API IUeAgentTransportClient
	{
	public:
		virtual ~IUeAgentTransportClient() = default;

		virtual FUeAgentTransportDelivery Send(
			const FUeAgentTransportFrame& Frame,
			const FUeAgentDeliveryRequirements& Requirements) = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentTransportRequestHandler
	{
	public:
		virtual ~IUeAgentTransportRequestHandler() = default;

		virtual FUeAgentTransportReply Handle(
			const FUeAgentTransportIngress& Ingress) = 0;
		virtual TOptional<FUeAgentTransportReply> TryHandleConcurrent(const FUeAgentTransportIngress&) { return {}; }
	};

	class UEAGENTINTERFACE_API IUeAgentNetworkServerControl
	{
	public:
		virtual ~IUeAgentNetworkServerControl() = default;

		virtual bool Start(
			FUeAgentTransportServerIdentity& OutIdentity,
			FUeAgentTransportDiagnostic& OutDiagnostic) = 0;
		virtual bool Stop(
			const FString& OwnerId,
			FUeAgentTransportDiagnostic& OutDiagnostic) = 0;
		virtual FUeAgentTransportServerHealth Health() const = 0;
		virtual FUeAgentTransportServerIdentity Identity() const = 0;
	};
}
