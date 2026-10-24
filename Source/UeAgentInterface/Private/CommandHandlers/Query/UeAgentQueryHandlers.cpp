#include "CommandHandlers/Query/UeAgentQueryHandlers.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::CommandHandlers::Query
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace Contract;
	using namespace DomainAdapters::Query;

	namespace
	{
		const FString Owner = TEXT("query-command-owner");

		

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = SubjectId;
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Diagnostic);
        }

		class FQueryHandler final : public IUeAgentExactCommandHandler
		{
		public:
			FQueryHandler(
				FString InCommandId,
				TSharedRef<IUeAgentQueryService> InService)
				: CommandId(MoveTemp(InCommandId))
				, Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				return CommandId;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:query")))
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_authority_required"), TEXT("uai:query is required"), TEXT("query")), CommandId);
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_cancelled"), TEXT("request was cancelled before QueryService execution"), TEXT("query")), CommandId);
				}

				FString ParamsJson;
				UE::UAI::Contract::FDiagnostic CanonicalError;
				if (!Json::TryCanonicalize(
						Request.GetParams(),
						ParamsJson,
						CanonicalError))
				{
					return Failure(Request, CanonicalError, CommandId);
				}

				FQueryServiceRequest ServiceRequest;
				ServiceRequest.CommandId = CommandId;
				ServiceRequest.RequestId = Request.GetRequestId();
				ServiceRequest.CanonicalParamsJson = MoveTemp(ParamsJson);
				ServiceRequest.CallerId = Caller.GetCallerId();
				ServiceRequest.AuthorityScopes = Caller.GetAuthorityScopes();
				ServiceRequest.AuthorityScopes.Sort();
				ServiceRequest.CorrelationId = Caller.GetCorrelationId();
				ServiceRequest.bCancellationRequested = Cancellation->IsCancelled();

				FQueryServiceResponse Response;
				FQueryServiceError Error;
				if (!Service->Execute(ServiceRequest, Response, Error))
				{
					return Failure(Request, (Error.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_execution_failed"), TEXT("The query service returned failure without a diagnostic."), TEXT("query")) : Error), Error.SubjectId.IsEmpty()
							? CommandId
							: Error.SubjectId);
				}
				if (!Response.bReadOnly)
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_source_not_read_only"),
						TEXT("QueryService did not attest a read-only result"), TEXT("query"));
					Diagnostic.Diagnostics.Append(Response.Diagnostics);
					return Failure(Request, MoveTemp(Diagnostic), CommandId);
				}

				TSharedPtr<FJsonObject> Payload;
				UE::UAI::Contract::FDiagnostic PayloadError;
				if (!Json::TryParseObject(
						Response.PayloadJson,
						Payload,
						PayloadError))
				{
					PayloadError.Diagnostics.Append(Response.Diagnostics);
					return Failure(Request, PayloadError, CommandId);
				}
				return FUeAgentCommandResult::Success(
					Request,
					Response.PayloadJson, MoveTemp(Response.Diagnostics));
			}

		private:
			FString CommandId;
			TSharedRef<IUeAgentQueryService> Service;
		};
	}

	bool TryCreateQueryHandlers(
		TSharedRef<IUeAgentQueryService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FQueryRoute& Route : GetNonHjsonQueryRoutes())
		{
			if (!Route.TryValidate(OutError))
			{
				OutHandlers.Reset();
				return false;
			}
			OutHandlers.Add(
				MakeShared<FQueryHandler>(Route.CommandId, Service));
		}
		return OutHandlers.Num() == GetNonHjsonQueryRoutes().Num();
	}
}
