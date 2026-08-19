#include "ServiceComposition/Query/UeAgentQueryCommandIngress.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentContract.h"

namespace UE::UAI::ServiceComposition::Query
{
	using namespace CommandEdge;
	using namespace Contract;
	using namespace NetworkTransport;

	namespace
	{
		class FNeverCancelled final : public IUeAgentCancellationSignal
		{
		public:
			virtual bool IsCancelled() const override
			{
				return false;
			}
		};

		class FQueryCommandIngress final
			: public IUeAgentTransportRequestHandler
		{
		public:
			FQueryCommandIngress(
				TSharedRef<const FUeAgentExactCommandDispatch> InDispatch,
				FString InPrincipalSubject,
				TArray<FString> InAuthorityScopes)
				: Dispatch(MoveTemp(InDispatch))
				, PrincipalSubject(MoveTemp(InPrincipalSubject))
				, AuthorityScopes(MoveTemp(InAuthorityScopes))
			{
			}

			virtual FUeAgentTransportReply Handle(
				const FUeAgentTransportIngress& Ingress) override
			{
				TOptional<FUeAgentCommandRequest> Request;
				FUeAgentCommandProtocolDiagnostic Diagnostic;
				if (!Codec.DecodeRequest(Ingress.Frame.GetPayload(), Request, Diagnostic)
					|| !Request.IsSet())
				{
					return Reply(Ingress, FUeAgentCommandResult::ProtocolError(
						Ingress.Frame.GetCorrelationId(),
						FString(),
						FString(),
						FString(),
						Diagnostic.Code.IsEmpty() ? TEXT("command_request_invalid") : Diagnostic.Code,
						Diagnostic.Message.IsEmpty() ? TEXT("command request could not be decoded") : Diagnostic.Message));
				}
				if (Ingress.Peer.Principal.Subject != PrincipalSubject)
				{
					return Reply(Ingress, FUeAgentCommandResult::ProtocolError(
						Request->GetCorrelationId(),
						Request->GetRequestId(),
						Request->GetCommandId(),
						Request->GetCommandBindingHash(),
						TEXT("command_principal_mismatch"),
						TEXT("authenticated transport principal does not match the command service")));
				}
				const FUeAgentCallerContext Caller(
					Ingress.Peer.Principal.Subject,
					AuthorityScopes,
					Request->GetCorrelationId());
				return Reply(Ingress, Dispatch->Submit(
					Request.GetValue(), Caller, MakeShared<FNeverCancelled>()));
			}

		private:
			FUeAgentTransportReply Reply(
				const FUeAgentTransportIngress& Ingress,
				const FUeAgentCommandResult& Result) const
			{
				TArray<uint8> Payload;
				FUeAgentCommandProtocolDiagnostic Diagnostic;
				if (!Codec.EncodeResult(Result, Payload, Diagnostic))
				{
					const FString Fallback = TEXT("{\"message_kind\":\"protocol_error\",\"code\":\"command_result_encode_failed\"}");
					FTCHARToUTF8 Encoded(*Fallback);
					Payload.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
				}
				return FUeAgentTransportReply(FUeAgentTransportFrame(
					Ingress.Frame.GetChannel(),
					MoveTemp(Payload),
					Ingress.Frame.GetCorrelationId(),
					Ingress.Peer));
			}

			FUeAgentCommandProtocolCodec Codec;
			TSharedRef<const FUeAgentExactCommandDispatch> Dispatch;
			FString PrincipalSubject;
			TArray<FString> AuthorityScopes;
		};
	}

	bool TryCreateQueryCommandIngress(
		TSharedRef<const FUeAgentExactCommandDispatch> Dispatch,
		FString PrincipalSubject,
		TArray<FString> AuthorityScopes,
		TSharedPtr<IUeAgentTransportRequestHandler, ESPMode::ThreadSafe>& OutIngress,
		FString& OutError)
	{
		OutIngress.Reset();
		OutError.Reset();
		PrincipalSubject.TrimStartAndEndInline();
		AuthorityScopes.RemoveAll([](FString& Scope)
		{
			Scope.TrimStartAndEndInline();
			return Scope.IsEmpty();
		});
		if (PrincipalSubject.IsEmpty() || AuthorityScopes.IsEmpty())
		{
			OutError = TEXT("query_command_ingress_config_invalid");
			return false;
		}
		OutIngress = MakeShared<FQueryCommandIngress, ESPMode::ThreadSafe>(
			MoveTemp(Dispatch), MoveTemp(PrincipalSubject), MoveTemp(AuthorityScopes));
		return true;
	}
}
