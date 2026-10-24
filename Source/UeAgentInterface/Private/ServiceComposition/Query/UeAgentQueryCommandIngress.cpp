#include "ServiceComposition/Query/UeAgentQueryCommandIngress.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentOperationRecoveryPolicy.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/AssetPreview/UeAgentAssetPreviewScheduling.h"
#include "OperationOwners/LongRunning/UeAgentLongRunningLifecycleOwner.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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
				if (!Codec.DecodeRequest(Ingress.Frame.GetPayload(), Request, Diagnostic))
				{
					return Reply(Ingress, FUeAgentCommandResult::ProtocolError(Ingress.Frame.GetCorrelationId(), {}, {}, {}, MoveTemp(Diagnostic)));
				}
				if (!Request.IsSet())
				{
					return Reply(Ingress, FUeAgentCommandResult::ProtocolError(Ingress.Frame.GetCorrelationId(), {}, {}, {},
						FDiagnostic::Failure(TEXT("command_request_invalid"), TEXT("The command decoder completed without producing a request."), TEXT("command_protocol"))));
				}
				if (Ingress.Peer.Principal.Subject != PrincipalSubject)
				{
					Diagnostic = FDiagnostic::Failure(TEXT("command_principal_mismatch"),
						TEXT("The authenticated transport principal does not match the command service."), TEXT("command_protocol"));
					Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(PrincipalSubject);
					Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Ingress.Peer.Principal.Subject);
					Diagnostic.Source.JsonPath = TEXT("/transport/principal");
					Diagnostic.CandidateName = Request->GetCommandId();
					return Reply(Ingress, FUeAgentCommandResult::ProtocolError(Request->GetCorrelationId(), Request->GetRequestId(), Request->GetCommandId(), Request->GetCommandBindingHash(), MoveTemp(Diagnostic)));
				}
				const FUeAgentCallerContext Caller(
					Ingress.Peer.Principal.Subject,
					AuthorityScopes,
					Request->GetCorrelationId());
				FString Target;
				if (CommandRegistry::UsesDeferredPreviewExecution(Request->GetCommandId()) && Request->GetParams()
					&& Request->GetParams()->TryGetStringField(TEXT("target"), Target) && !Target.IsEmpty())
				{
					TSharedPtr<IUeAgentOperationApplicationPort> LifecyclePort;
					TSharedPtr<Composition::LongRunningSession::FUeAgentLongRunningSessionRuntime> Runtime;
					UE::UAI::Contract::FDiagnostic RuntimeError;
					if (!OperationOwners::LongRunning::TryGetProductionLongRunningLifecyclePort(LifecyclePort, Runtime, RuntimeError))
						return Reply(Ingress, FUeAgentCommandResult::ProtocolError(Request->GetCorrelationId(), Request->GetRequestId(), Request->GetCommandId(), Request->GetCommandBindingHash(), RuntimeError));
					return Reply(Ingress, Runtime->AcceptDeferredCommand(Request.GetValue(), Caller,
						[Dispatch = Dispatch, Request = Request.GetValue(), Caller, Target]
						(TFunction<bool()> IsCancelled, TFunction<void(FUeAgentCommandResult)> Complete)
						{
						AssetPreview::DeferPreviewWork(Target, [Dispatch, Request, Caller, IsCancelled = MoveTemp(IsCancelled), Complete = MoveTemp(Complete)](bool bCancelled) mutable
						{
							const auto Result = bCancelled || IsCancelled()
								? FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(), Request.GetCommandId(), Request.GetCommandBindingHash(), UE::UAI::Contract::FDiagnostic::Failure(TEXT("preview_work_cancelled"), TEXT("Editor preview scheduling stopped before import began"), TEXT("command_protocol")))
								: Dispatch->Submit(Request, Caller, MakeShared<FNeverCancelled>());
							Complete(Result);
						});
						}));
				}
				return Reply(Ingress, Dispatch->Submit(
					Request.GetValue(), Caller, MakeShared<FNeverCancelled>()));
			}

		private:
			static FUeAgentTransportReply Reply(
				const FUeAgentTransportIngress& Ingress,
				const FUeAgentCommandResult& Result)
			{
				FUeAgentCommandProtocolCodec Codec;
				TArray<uint8> Payload;
				FUeAgentCommandProtocolDiagnostic Diagnostic;
				if (!Codec.EncodeResult(Result, Payload, Diagnostic))
				{
					const auto Failure = FUeAgentCommandResult::ProtocolError(Ingress.Frame.GetCorrelationId(),
						Result.GetRequestId(), Result.GetCommandId(), Result.GetCommandBindingHash(), MoveTemp(Diagnostic));
					FDiagnostic EncodingFailure;
					if (!Codec.EncodeResult(Failure, Payload, EncodingFailure))
					{
						// No protocol reply can be delivered if even the encoding diagnostic is invalid.
						// Keep both failures in the Editor log; do not fabricate a malformed protocol body.
						UE_LOG(LogTemp, Error, TEXT("UAI could not encode its failure reply: %s; original failure: %s"),
							*EncodingFailure.ToPublicDetail(), *Failure.GetDiagnostic().ToPublicDetail());
					}
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = FDiagnostic::Failure(TEXT("query_command_ingress_config_invalid"),
				PrincipalSubject.IsEmpty() ? TEXT("The command service requires a non-empty authenticated principal.")
					: TEXT("The command service requires at least one non-empty authority scope."), TEXT("command_service"));
			OutError.PropertyPath = PrincipalSubject.IsEmpty() ? TEXT("principal_subject") : TEXT("authority_scopes");
			return false;
		}
		OutIngress = MakeShared<FQueryCommandIngress, ESPMode::ThreadSafe>(
			MoveTemp(Dispatch), MoveTemp(PrincipalSubject), MoveTemp(AuthorityScopes));
		return true;
	}
}
