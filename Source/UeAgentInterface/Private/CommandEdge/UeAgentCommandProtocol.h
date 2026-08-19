#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentContract.h"
#include "Dom/JsonObject.h"
#include "NetworkTransport/UeAgentNetworkTransport.h"

#include "Misc/Optional.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::CommandEdge
{
	class FUeAgentCommandDispatch;

	struct FUeAgentCommandProtocolDiagnostic
	{
		FString Code;
		FString Message;

		bool IsValid() const
		{
			return !Code.IsEmpty();
		}
	};

	class UEAGENTINTERFACE_API FUeAgentCommandRequest final
	{
	public:
		const FString& GetCorrelationId() const;
		const FString& GetRequestId() const;
		const FString& GetCommandId() const;
		const FString& GetCommandBindingHash() const;
		const TSharedPtr<FJsonObject>& GetParams() const;
		const FString& GetParamsJson() const;
		bool IsValid() const;

	private:
		friend class FUeAgentCommandProtocolCodec;

		FUeAgentCommandRequest(
			FString InCorrelationId,
			FString InRequestId,
			FString InCommandId,
			FString InCommandBindingHash,
			TSharedPtr<FJsonObject> InParams,
			FString InParamsJson);

		FString CorrelationId;
		FString RequestId;
		FString CommandId;
		FString CommandBindingHash;
		TSharedPtr<FJsonObject> Params;
		FString ParamsJson;
	};

	class UEAGENTINTERFACE_API FUeAgentCommandResult final
	{
	public:
		static FUeAgentCommandResult Success(
			const FUeAgentCommandRequest& Request,
			FString PayloadJson);

		static FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			FString PayloadJson,
			FString ErrorJson);

		static FUeAgentCommandResult ProtocolError(
			FString CorrelationId,
			FString RequestId,
			FString CommandId,
			FString CommandBindingHash,
			FString Code,
			FString Message);

		const FString& GetCorrelationId() const;
		const FString& GetRequestId() const;
		const FString& GetCommandId() const;
		const FString& GetCommandBindingHash() const;
		const FString& GetPayloadJson() const;
		const FString& GetErrorJson() const;
		bool IsOk() const;
		bool IsProtocolError() const;
		bool IsValid() const;
		bool MatchesRequest(const FUeAgentCommandRequest& Request) const;

	private:
		friend class FUeAgentCommandProtocolCodec;

		FUeAgentCommandResult(
			FString InCorrelationId,
			FString InRequestId,
			FString InCommandId,
			FString InCommandBindingHash,
			FString InPayloadJson,
			FString InErrorJson,
			bool bInOk,
			bool bInProtocolError);

		FString CorrelationId;
		FString RequestId;
		FString CommandId;
		FString CommandBindingHash;
		FString PayloadJson;
		FString ErrorJson;
		bool bOk = false;
		bool bProtocolError = false;
	};

	class UEAGENTINTERFACE_API FUeAgentCommandProtocolCodec final
	{
	public:
		bool EncodeRequest(
			const FUeAgentCommandRequest& Request,
			TArray<uint8>& OutPayload,
			FUeAgentCommandProtocolDiagnostic& OutDiagnostic) const;

		bool DecodeRequest(
			const TArray<uint8>& Payload,
			TOptional<FUeAgentCommandRequest>& OutRequest,
			FUeAgentCommandProtocolDiagnostic& OutDiagnostic) const;

		bool EncodeResult(
			const FUeAgentCommandResult& Result,
			TArray<uint8>& OutPayload,
			FUeAgentCommandProtocolDiagnostic& OutDiagnostic) const;

		bool DecodeResult(
			const TArray<uint8>& Payload,
			TOptional<FUeAgentCommandResult>& OutResult,
			FUeAgentCommandProtocolDiagnostic& OutDiagnostic) const;
	};

	using FUeAgentCommandCallerFactory = TFunction<
		TOptional<Contract::FUeAgentCallerContext>(
			const NetworkTransport::FUeAgentTransportPeer& Peer,
			const FString& CorrelationId,
			FString& OutError)>;

	using FUeAgentCommandCancellationFactory = TFunction<
		TSharedRef<Contract::IUeAgentCancellationSignal>(
			const FString& CorrelationId)>;

	class UEAGENTINTERFACE_API FUeAgentCommandIngress final
		: public NetworkTransport::IUeAgentTransportRequestHandler
	{
	public:
		FUeAgentCommandIngress(
			FUeAgentCommandProtocolCodec InCodec,
			TSharedRef<
				const CommandContract::FUeAgentCommandContractRegistry>
				InRegistry,
			TSharedRef<const FUeAgentCommandDispatch> InDispatch,
			FUeAgentCommandCallerFactory InCallerFactory,
			FUeAgentCommandCancellationFactory InCancellationFactory);

		virtual NetworkTransport::FUeAgentTransportReply Handle(
			const NetworkTransport::FUeAgentTransportIngress&
				Ingress) override;

	private:
		NetworkTransport::FUeAgentTransportReply MakeReply(
			const NetworkTransport::FUeAgentTransportIngress& Ingress,
			const FUeAgentCommandResult& Result) const;

		FUeAgentCommandProtocolCodec Codec;
		TSharedRef<
			const CommandContract::FUeAgentCommandContractRegistry>
			Registry;
		TSharedRef<const FUeAgentCommandDispatch> Dispatch;
		FUeAgentCommandCallerFactory CallerFactory;
		FUeAgentCommandCancellationFactory CancellationFactory;
	};
}
