#include "CommandHandlers/ExternalImport/Localization/UeAgentLocalizationImportCommandHandlers.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::CommandHandlers::ExternalImport::Localization
{
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::ExternalImport::Localization;

	namespace
	{
		const FString Owner = TEXT("localization-import-owner");

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FUeAgentLocalizationImportCommandError& Error)
		{
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(
				TEXT("code"),
				Error.Code.IsEmpty()
					? FString(TEXT("localization_import_execution_failed"))
					: Error.Code);
			Object->SetStringField(TEXT("subject_id"), Error.SubjectId);
			Object->SetStringField(TEXT("detail"), Error.Detail);
			Object->SetStringField(TEXT("terminal_status"), Error.TerminalStatus);
			Object->SetBoolField(TEXT("mutation_started"), Error.bMutationStarted);
			Object->SetBoolField(TEXT("retry_safe"), Error.bRetrySafe);
			FString ErrorJson;
			FString JsonError;
			if (!CommandContract::Json::TryCanonicalize(
					Object,
					ErrorJson,
					JsonError))
			{
				ErrorJson = TEXT(
					"{\"code\":\"localization_import_error_serialization_failed\","
					"\"terminal_status\":\"state_unknown\"}");
			}
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				MoveTemp(ErrorJson));
		}

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			FHandler(
				FUeAgentLocalizationImportCommandRoute InRoute,
				TSharedRef<IUeAgentLocalizationImportCommandService> InService)
				: Route(MoveTemp(InRoute))
				, Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				return Route.CommandId;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					FUeAgentLocalizationImportCommandError Error;
					Error.Code = TEXT("localization_import_authority_required");
					Error.SubjectId = Request.GetCommandId();
					Error.Detail =
						TEXT("the public localization import route requires uai:operate");
					return Failure(Request, Error);
				}
				if (!IsInGameThread())
				{
					FUeAgentLocalizationImportCommandError Error;
					Error.Code = TEXT("localization_import_game_thread_required");
					Error.SubjectId = Request.GetCommandId();
					Error.Detail =
						TEXT("localization import mechanics must execute on the game thread");
					return Failure(Request, Error);
				}

				FUeAgentLocalizationImportCommandRequest ServiceRequest;
				ServiceRequest.CommandId = Request.GetCommandId();
				ServiceRequest.RequestId = Request.GetRequestId();
				ServiceRequest.CanonicalParamsJson = Request.GetParamsJson();
				ServiceRequest.CallerId = Caller.GetCallerId();
				ServiceRequest.AuthorityScopes = Caller.GetAuthorityScopes();
				ServiceRequest.CorrelationId = Caller.GetCorrelationId();
				ServiceRequest.bCancellationRequested = Cancellation->IsCancelled();

				FUeAgentLocalizationImportCommandResponse Response;
				FUeAgentLocalizationImportCommandError Error;
				if (!Service->Execute(ServiceRequest, Response, Error))
				{
					return Failure(Request, Error);
				}
				TSharedPtr<FJsonObject> Payload;
				FString JsonError;
				FString CanonicalPayload;
				if (!CommandContract::Json::TryParseObject(
						Response.PayloadJson,
						Payload,
						JsonError)
					|| !CommandContract::Json::TryCanonicalize(
						Payload,
						CanonicalPayload,
						JsonError))
				{
					Error.Code = TEXT("localization_import_result_invalid");
					Error.SubjectId = Request.GetCommandId();
					Error.Detail = MoveTemp(JsonError);
					Error.TerminalStatus = TEXT("state_unknown");
					Error.bMutationStarted = true;
					Error.bRetrySafe = false;
					return Failure(Request, Error);
				}
				return FUeAgentCommandResult::Success(
					Request,
					MoveTemp(CanonicalPayload));
			}

		private:
			FUeAgentLocalizationImportCommandRoute Route;
			TSharedRef<IUeAgentLocalizationImportCommandService> Service;
		};
	}

	bool TryCreateLocalizationImportCommandHandlers(
		TSharedRef<IUeAgentLocalizationImportCommandService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FUeAgentLocalizationImportCommandRoute& Route
			: GetLocalizationImportCommandRoutes())
		{
			if (!Route.IsValid())
			{
				OutHandlers.Reset();
				OutError = TEXT("localization_import_route_invalid:")
					+ Route.CommandId;
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Route, Service));
		}
		return true;
	}
}
