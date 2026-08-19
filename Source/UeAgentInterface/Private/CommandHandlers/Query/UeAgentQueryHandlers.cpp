#include "CommandHandlers/Query/UeAgentQueryHandlers.h"

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

		FString ErrorJson(
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("code"), Code);
			Object->SetStringField(TEXT("subject_id"), SubjectId);
			Object->SetStringField(TEXT("detail"), Detail);
			FString Result;
			FString Error;
			return Json::TryCanonicalize(Object, Result, Error)
				? Result
				: FString(TEXT("{\"code\":\"query_error_serialization_failed\"}"));
		}

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				ErrorJson(Code, SubjectId, Detail));
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
					return Failure(
						Request,
						TEXT("query_authority_required"),
						CommandId,
						TEXT("uai:query is required"));
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(
						Request,
						TEXT("query_cancelled"),
						CommandId,
						TEXT("request was cancelled before QueryService execution"));
				}

				FString ParamsJson;
				FString CanonicalError;
				if (!Json::TryCanonicalize(
						Request.GetParams(),
						ParamsJson,
						CanonicalError))
				{
					return Failure(
						Request,
						TEXT("query_params_invalid"),
						CommandId,
						CanonicalError);
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
					return Failure(
						Request,
						Error.Code.IsEmpty()
							? FString(TEXT("query_execution_failed"))
							: Error.Code,
						Error.SubjectId.IsEmpty()
							? CommandId
							: Error.SubjectId,
						Error.Detail);
				}
				if (!Response.bReadOnly)
				{
					return Failure(
						Request,
						TEXT("query_source_not_read_only"),
						CommandId,
						TEXT("QueryService did not attest a read-only result"));
				}

				TSharedPtr<FJsonObject> Payload;
				FString PayloadError;
				if (!Json::TryParseObject(
						Response.PayloadJson,
						Payload,
						PayloadError))
				{
					return Failure(
						Request,
						TEXT("query_result_invalid"),
						CommandId,
						PayloadError);
				}
				return FUeAgentCommandResult::Success(
					Request,
					Response.PayloadJson);
			}

		private:
			FString CommandId;
			TSharedRef<IUeAgentQueryService> Service;
		};
	}

	bool TryCreateQueryHandlers(
		TSharedRef<IUeAgentQueryService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FQueryRoute& Route : GetNonHjsonQueryRoutes())
		{
			if (!Route.IsValid())
			{
				OutHandlers.Reset();
				OutError = TEXT("query_route_invalid");
				return false;
			}
			OutHandlers.Add(
				MakeShared<FQueryHandler>(Route.CommandId, Service));
		}
		return OutHandlers.Num() == GetNonHjsonQueryRoutes().Num();
	}
}
