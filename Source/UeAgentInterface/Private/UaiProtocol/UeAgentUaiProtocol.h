#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "NetworkTransport/UeAgentNetworkTransport.h"
#include "Misc/Optional.h"
#include "Templates/Function.h"

namespace UE::UAI::Protocol
{
	UEAGENTINTERFACE_API const FString& GetUaiWireSchema();
	UEAGENTINTERFACE_API int32 GetUaiWireVersion();

	enum class EUeAgentWireMessageKind : uint8
	{
		QueryRequest,
		QueryResult,
		OperationRequest,
		OperationResult,
		ProtocolError,
	};

	enum class EUeAgentProtocolErrorLayer : uint8
	{
		Wire,
		Transport,
	};

	class UEAGENTINTERFACE_API FUeAgentProtocolDiagnostic final
	{
	public:
		FUeAgentProtocolDiagnostic();
		FUeAgentProtocolDiagnostic(
			EUeAgentProtocolErrorLayer InLayer,
			FString InCode,
			FString InMessage,
			TOptional<NetworkTransport::EUeAgentTransportDeliveryState>
				InTransportState = {});

		EUeAgentProtocolErrorLayer GetLayer() const;
		const FString& GetCode() const;
		const FString& GetMessage() const;
		const TOptional<NetworkTransport::EUeAgentTransportDeliveryState>&
			GetTransportState() const;
		bool IsValid() const;

	private:
		EUeAgentProtocolErrorLayer Layer =
			EUeAgentProtocolErrorLayer::Wire;
		FString Code;
		FString Message;
		TOptional<NetworkTransport::EUeAgentTransportDeliveryState>
			TransportState;
	};

	class UEAGENTINTERFACE_API FUeAgentUaiWireRequest final
	{
	public:
		static FUeAgentUaiWireRequest MakeQuery(
			FString CorrelationId,
			Contract::FUeAgentQueryRequest Request,
			Contract::FUeAgentCallerContext CallerContext);
		static FUeAgentUaiWireRequest MakeOperation(
			FString CorrelationId,
			Contract::FUeAgentOperationRequest Request,
			Contract::FUeAgentCallerContext CallerContext);

		EUeAgentWireMessageKind GetMessageKind() const;
		const FString& GetCorrelationId() const;
		const Contract::FUeAgentCallerContext& GetCallerContext() const;
		bool IsQuery() const;
		bool IsOperation() const;
		const Contract::FUeAgentQueryRequest& GetQueryRequest() const;
		const Contract::FUeAgentOperationRequest&
			GetOperationRequest() const;
		bool IsValid() const;

	private:
		FUeAgentUaiWireRequest(
			EUeAgentWireMessageKind InMessageKind,
			FString InCorrelationId,
			Contract::FUeAgentCallerContext InCallerContext);

		EUeAgentWireMessageKind MessageKind =
			EUeAgentWireMessageKind::QueryRequest;
		FString CorrelationId;
		Contract::FUeAgentCallerContext CallerContext;
		TOptional<Contract::FUeAgentQueryRequest> QueryRequest;
		TOptional<Contract::FUeAgentOperationRequest> OperationRequest;
	};

	class UEAGENTINTERFACE_API FUeAgentUaiWireResult final
	{
	public:
		static FUeAgentUaiWireResult MakeQuery(
			FString CorrelationId,
			Contract::FUeAgentQueryResult Result);
		static FUeAgentUaiWireResult MakeOperation(
			FString CorrelationId,
			Contract::FUeAgentOperationResult Result);
		static FUeAgentUaiWireResult MakeError(
			FString CorrelationId,
			FUeAgentProtocolDiagnostic Diagnostic);

		EUeAgentWireMessageKind GetMessageKind() const;
		const FString& GetCorrelationId() const;
		bool HasQueryResult() const;
		bool HasOperationResult() const;
		bool HasDiagnostic() const;
		const Contract::FUeAgentQueryResult& GetQueryResult() const;
		const Contract::FUeAgentOperationResult&
			GetOperationResult() const;
		const FUeAgentProtocolDiagnostic& GetDiagnostic() const;
		bool IsValid() const;

	private:
		FUeAgentUaiWireResult(
			EUeAgentWireMessageKind InMessageKind,
			FString InCorrelationId);

		EUeAgentWireMessageKind MessageKind =
			EUeAgentWireMessageKind::ProtocolError;
		FString CorrelationId;
		TOptional<Contract::FUeAgentQueryResult> QueryResult;
		TOptional<Contract::FUeAgentOperationResult> OperationResult;
		TOptional<FUeAgentProtocolDiagnostic> Diagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentUaiProtocolCodec final
	{
	public:
		bool EncodeRequest(
			const FUeAgentUaiWireRequest& Request,
			TArray<uint8>& OutPayload,
			FUeAgentProtocolDiagnostic& OutDiagnostic) const;
		bool DecodeRequest(
			const TArray<uint8>& Payload,
			TOptional<FUeAgentUaiWireRequest>& OutRequest,
			FUeAgentProtocolDiagnostic& OutDiagnostic) const;
		bool EncodeResult(
			const FUeAgentUaiWireResult& Result,
			TArray<uint8>& OutPayload,
			FUeAgentProtocolDiagnostic& OutDiagnostic) const;
		bool DecodeResult(
			const TArray<uint8>& Payload,
			TOptional<FUeAgentUaiWireResult>& OutResult,
			FUeAgentProtocolDiagnostic& OutDiagnostic) const;
	};

	class UEAGENTINTERFACE_API IUeAgentUaiProtocolClient
	{
	public:
		virtual ~IUeAgentUaiProtocolClient() = default;

		virtual FUeAgentUaiWireResult Invoke(
			const FUeAgentUaiWireRequest& Request) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentUaiProtocolClient final
		: public IUeAgentUaiProtocolClient
	{
	public:
		FUeAgentUaiProtocolClient(
			NetworkTransport::IUeAgentTransportClient& InTransportClient,
			FUeAgentUaiProtocolCodec InCodec,
			NetworkTransport::FUeAgentTransportPeer InPeer,
			FString InChannel,
			NetworkTransport::FUeAgentDeliveryRequirements
				InDeliveryRequirements);

		virtual FUeAgentUaiWireResult Invoke(
			const FUeAgentUaiWireRequest& Request) override;

	private:
		NetworkTransport::IUeAgentTransportClient& TransportClient;
		FUeAgentUaiProtocolCodec Codec;
		NetworkTransport::FUeAgentTransportPeer Peer;
		FString Channel;
		NetworkTransport::FUeAgentDeliveryRequirements
			DeliveryRequirements;
	};

	using FUeAgentCancellationFactory = TFunction<
		TSharedRef<Contract::IUeAgentCancellationSignal>(
			const FString& CorrelationId)>;

	class UEAGENTINTERFACE_API FUeAgentUaiProtocolIngress final
		: public NetworkTransport::IUeAgentTransportRequestHandler
	{
	public:
		FUeAgentUaiProtocolIngress(
			FUeAgentUaiProtocolCodec InCodec,
			Contract::IUeAgentQueryApplicationPort& InQueryPort,
			Contract::IUeAgentOperationApplicationPort& InOperationPort,
			FUeAgentCancellationFactory InCancellationFactory);

		virtual NetworkTransport::FUeAgentTransportReply Handle(
			const NetworkTransport::FUeAgentTransportIngress&
				Ingress) override;

	private:
		NetworkTransport::FUeAgentTransportReply MakeReply(
			const NetworkTransport::FUeAgentTransportIngress& Ingress,
			const FUeAgentUaiWireResult& Result) const;

		FUeAgentUaiProtocolCodec Codec;
		Contract::IUeAgentQueryApplicationPort& QueryPort;
		Contract::IUeAgentOperationApplicationPort& OperationPort;
		FUeAgentCancellationFactory CancellationFactory;
	};
}

