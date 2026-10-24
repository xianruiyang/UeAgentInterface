#include "NetworkTransport/UeAgentNetworkTransport.h"

namespace UE::UAI::NetworkTransport
{
	FUeAgentTransportFrame::FUeAgentTransportFrame(
		FString InChannel,
		TArray<uint8> InPayload,
		FString InCorrelationId,
		FUeAgentTransportPeer InPeer)
		: Channel(MoveTemp(InChannel))
		, Payload(MoveTemp(InPayload))
		, CorrelationId(MoveTemp(InCorrelationId))
		, Peer(MoveTemp(InPeer))
	{
		checkf(!Channel.IsEmpty(), TEXT("transport_channel_missing"));
		checkf(
			!CorrelationId.IsEmpty(),
			TEXT("transport_correlation_id_missing"));
		checkf(!Peer.PeerId.IsEmpty(), TEXT("transport_peer_id_missing"));
		checkf(
			!Peer.Principal.Subject.IsEmpty(),
			TEXT("transport_principal_subject_missing"));
		checkf(
			!Peer.Principal.AuthenticationMethod.IsEmpty(),
			TEXT("transport_authentication_method_missing"));
	}

	const FString& FUeAgentTransportFrame::GetChannel() const
	{
		return Channel;
	}

	const TArray<uint8>& FUeAgentTransportFrame::GetPayload() const
	{
		return Payload;
	}

	const FString& FUeAgentTransportFrame::GetCorrelationId() const
	{
		return CorrelationId;
	}

	const FUeAgentTransportPeer& FUeAgentTransportFrame::GetPeer() const
	{
		return Peer;
	}

	bool TryBindCapabilities(
		const FUeAgentTransportCapabilities& Capabilities,
		const FUeAgentDeliveryRequirements& Requirements,
		FUeAgentCapabilityBinding& OutBinding,
		FUeAgentTransportBindingError& OutError)
	{
		OutError = {};
		if (Requirements.bRequireOrdered && !Capabilities.bOrdered)
		{
			OutError.MissingCapabilities.Add(TEXT("ordered"));
		}
		if (Requirements.bRequireReliable && !Capabilities.bReliable)
		{
			OutError.MissingCapabilities.Add(TEXT("reliable"));
		}
		if (
			Requirements.bRequireMessageBoundaries
			&& !Capabilities.bPreservesMessageBoundaries)
		{
			OutError.MissingCapabilities.Add(TEXT("message_boundaries"));
		}
		if (
			Requirements.bRequireRequestReply
			&& !Capabilities.bRequestReply)
		{
			OutError.MissingCapabilities.Add(TEXT("request_reply"));
		}
		if (
			Requirements.bRequireDuplicateSuppression
			&& !Capabilities.bDuplicateSuppression)
		{
			OutError.MissingCapabilities.Add(TEXT("duplicate_suppression"));
		}
		if (!OutError.MissingCapabilities.IsEmpty())
		{
			OutError.Code = TEXT("transport_capability_unsupported");
			return false;
		}

		OutBinding.Capabilities = Capabilities;
		OutBinding.Requirements = Requirements;
		return true;
	}

	FUeAgentTransportReply::FUeAgentTransportReply(
		FUeAgentTransportFrame InFrame)
		: Frame(MoveTemp(InFrame))
	{
	}

	FUeAgentTransportIngress::FUeAgentTransportIngress(
		FUeAgentTransportFrame InFrame,
		FUeAgentTransportPeer InPeer)
		: Frame(MoveTemp(InFrame))
		, Peer(MoveTemp(InPeer))
	{
		checkf(
			Frame.GetPeer().PeerId == Peer.PeerId,
			TEXT("transport_ingress_peer_mismatch"));
	}
}
