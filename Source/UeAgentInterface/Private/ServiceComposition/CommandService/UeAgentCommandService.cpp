#include "ServiceComposition/CommandService/UeAgentCommandService.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentCommandProtocol.h"

namespace UE::UAI::ServiceComposition::CommandService
{
	using namespace CommandEdge;
	using namespace NetworkTransport;

	namespace
	{
		class FUnavailableCommandIngress final
			: public IUeAgentTransportRequestHandler
		{
		public:
			virtual FUeAgentTransportReply Handle(
				const FUeAgentTransportIngress& Ingress) override
			{
				FUeAgentCommandProtocolCodec Codec;
				TOptional<FUeAgentCommandRequest> Request;
				FUeAgentCommandProtocolDiagnostic Diagnostic;
				FUeAgentCommandResult Result =
					FUeAgentCommandResult::ProtocolError(Ingress.Frame.GetCorrelationId(), FString(), FString(), FString(), UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_service_unavailable"), TEXT("command contracts have not been activated"), TEXT("command_protocol")));
				if (Codec.DecodeRequest(
						Ingress.Frame.GetPayload(),
						Request,
						Diagnostic)
					&& Request.IsSet())
				{
					Result = FUeAgentCommandResult::ProtocolError(Request->GetCorrelationId(), Request->GetRequestId(), Request->GetCommandId(), Request->GetCommandBindingHash(), UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_service_unavailable"), TEXT("command contracts have not been activated"), TEXT("command_protocol")));
				}

				TArray<uint8> Payload;
				if (!Codec.EncodeResult(Result, Payload, Diagnostic))
				{
					const FString Fallback =
						TEXT("{\"message_kind\":\"protocol_error\",\"code\":")
						TEXT("\"command_service_unavailable\"}");
					FTCHARToUTF8 Converted(*Fallback);
					Payload.Append(
						reinterpret_cast<const uint8*>(Converted.Get()),
						Converted.Length());
				}

				return FUeAgentTransportReply(FUeAgentTransportFrame(
					Ingress.Frame.GetChannel(),
					MoveTemp(Payload),
					Ingress.Frame.GetCorrelationId(),
					Ingress.Peer));
			}
		};
	}

	bool FUeAgentCommandService::TryCreate(
		TSharedRef<
			IUeAgentTransportRequestHandler,
			ESPMode::ThreadSafe> IngressHandler,
		TSharedRef<
			IUeAgentRequestContextInvalidator,
			ESPMode::ThreadSafe> ContextInvalidator,
		TUniquePtr<FUeAgentCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		OutService = TUniquePtr<FUeAgentCommandService>(
			new FUeAgentCommandService(
				MoveTemp(IngressHandler),
				MoveTemp(ContextInvalidator)));
		return true;
	}

	TSharedRef<
		IUeAgentTransportRequestHandler,
		ESPMode::ThreadSafe>
	FUeAgentCommandService::GetIngressHandler() const
	{
		return IngressHandler;
	}

	void FUeAgentCommandService::NotifyWorldChanged(
		const FString& WorldIdentity)
	{
		++ContextGeneration;
		ContextInvalidator->InvalidateWorld(WorldIdentity);
	}

	uint64 FUeAgentCommandService::GetContextGeneration() const
	{
		return ContextGeneration;
	}

	FUeAgentCommandService::FUeAgentCommandService(
		TSharedRef<
			IUeAgentTransportRequestHandler,
			ESPMode::ThreadSafe> InIngressHandler,
		TSharedRef<
			IUeAgentRequestContextInvalidator,
			ESPMode::ThreadSafe> InContextInvalidator)
		: IngressHandler(MoveTemp(InIngressHandler))
		, ContextInvalidator(MoveTemp(InContextInvalidator))
	{
	}

	TSharedRef<IUeAgentTransportRequestHandler, ESPMode::ThreadSafe>
	MakeUnavailableCommandIngress()
	{
		return MakeShared<FUnavailableCommandIngress, ESPMode::ThreadSafe>();
	}
}
