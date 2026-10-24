#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
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

	using FUeAgentCommandProtocolDiagnostic = Contract::FDiagnostic;

	class UEAGENTINTERFACE_API FUeAgentCommandRequest final
	{
	public:
		static bool TryCreate(
			FString CorrelationId, FString RequestId, FString CommandId, FString CommandBindingHash,
			TSharedPtr<FJsonObject> Params, TOptional<FUeAgentCommandRequest>& OutRequest,
			FUeAgentCommandProtocolDiagnostic& OutDiagnostic);

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

	enum class EApplicationFailureKind : uint8 { None, Query, Operation };

	class UEAGENTINTERFACE_API FUeAgentCommandResult final
	{
	public:
		static FUeAgentCommandResult Success(
			const FUeAgentCommandRequest& Request,
			FString PayloadJson, TArray<Contract::FDiagnostic> Diagnostics = {});

		static FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			FString PayloadJson,
			Contract::FDiagnostic Diagnostic, FString EnvelopeCode = FString(),
			EApplicationFailureKind ApplicationFailureKind = EApplicationFailureKind::None,
			TArray<Contract::FDiagnostic> Diagnostics = {});

		static FUeAgentCommandResult ProtocolError(
			FString CorrelationId,
			FString RequestId,
			FString CommandId,
			FString CommandBindingHash,
			Contract::FDiagnostic Diagnostic);

		const FString& GetCorrelationId() const;
		const FString& GetRequestId() const;
		const FString& GetCommandId() const;
		const FString& GetCommandBindingHash() const;
		const FString& GetPayloadJson() const;
		const Contract::FDiagnostic& GetDiagnostic() const;
		const TArray<Contract::FDiagnostic>& GetDiagnostics() const;
		FUeAgentCommandResult WithPayload(FString InPayloadJson) const;
		const FString& GetErrorCode() const;
		TSharedRef<FJsonObject> ToPublicError() const;
		EApplicationFailureKind GetApplicationFailureKind() const { return ApplicationFailureKind; }
		bool IsOk() const;
		bool IsProtocolError() const;
		bool IsValid() const;
		bool MatchesRequest(const FUeAgentCommandRequest& Request) const;
		bool TryValidate(Contract::FDiagnostic& OutDiagnostic) const;
		bool TryValidateForRequest(const FUeAgentCommandRequest& Request, Contract::FDiagnostic& OutDiagnostic) const;

	private:
		friend class FUeAgentCommandProtocolCodec;

		FUeAgentCommandResult(
			FString InCorrelationId,
			FString InRequestId,
			FString InCommandId,
			FString InCommandBindingHash,
			FString InPayloadJson,
			Contract::FDiagnostic InDiagnostic,
			bool bInOk,
			bool bInProtocolError, FString InEnvelopeCode = FString(),
			EApplicationFailureKind InApplicationFailureKind = EApplicationFailureKind::None,
			TArray<Contract::FDiagnostic> InDiagnostics = {});

		FString CorrelationId;
		FString RequestId;
		FString CommandId;
		FString CommandBindingHash;
		FString PayloadJson;
		Contract::FDiagnostic Diagnostic;
		TArray<Contract::FDiagnostic> Diagnostics;
		// An explicit envelope classification may differ from the underlying cause.
		FString EnvelopeCode;
		EApplicationFailureKind ApplicationFailureKind = EApplicationFailureKind::None;
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

		// Shared one-way public projection for transport and deferred operation signals.
		bool TryProjectResult(
			const FUeAgentCommandResult& Result,
			TSharedPtr<FJsonObject>& OutObject,
			FUeAgentCommandProtocolDiagnostic& OutDiagnostic) const;

		bool EncodeResult(
			const FUeAgentCommandResult& Result,
			TArray<uint8>& OutPayload,
			FUeAgentCommandProtocolDiagnostic& OutDiagnostic) const;
	};

	using FUeAgentCommandCallerFactory = TFunction<
		TOptional<Contract::FUeAgentCallerContext>(
			const NetworkTransport::FUeAgentTransportPeer& Peer,
			const FString& CorrelationId,
			UE::UAI::Contract::FDiagnostic& OutError)>;

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
