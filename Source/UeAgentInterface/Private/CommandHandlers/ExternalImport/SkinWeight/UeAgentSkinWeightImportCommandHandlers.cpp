#include "CommandHandlers/ExternalImport/SkinWeight/UeAgentSkinWeightImportCommandHandlers.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::CommandHandlers::ExternalImport::SkinWeight
{
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::ExternalImport::SkinWeight;

	namespace
	{
		const FString Owner = TEXT("skin-weight-import-owner");

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FUeAgentSkinWeightImportCommandError& Error)
		{
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(
				TEXT("code"),
				Error.Code.IsEmpty()
					? FString(TEXT("skin_weight_import_execution_failed"))
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
					"{\"code\":\"skin_weight_import_error_serialization_failed\","
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
				FUeAgentSkinWeightImportCommandRoute InRoute,
				TSharedRef<IUeAgentSkinWeightImportCommandService> InService)
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
					FUeAgentSkinWeightImportCommandError Error;
					Error.Code = TEXT("skin_weight_import_authority_required");
					Error.SubjectId = Request.GetCommandId();
					Error.Detail =
						TEXT("the public skin-weight import route requires uai:operate");
					return Failure(Request, Error);
				}
				if (!IsInGameThread())
				{
					FUeAgentSkinWeightImportCommandError Error;
					Error.Code = TEXT("skin_weight_import_game_thread_required");
					Error.SubjectId = Request.GetCommandId();
					Error.Detail =
						TEXT("skin-weight import mechanics must execute on the game thread");
					return Failure(Request, Error);
				}

				FUeAgentSkinWeightImportCommandRequest ServiceRequest;
				ServiceRequest.CommandId = Request.GetCommandId();
				ServiceRequest.RequestId = Request.GetRequestId();
				ServiceRequest.CanonicalParamsJson = Request.GetParamsJson();
				ServiceRequest.CallerId = Caller.GetCallerId();
				ServiceRequest.AuthorityScopes = Caller.GetAuthorityScopes();
				ServiceRequest.CorrelationId = Caller.GetCorrelationId();
				ServiceRequest.bCancellationRequested = Cancellation->IsCancelled();

				FUeAgentSkinWeightImportCommandResponse Response;
				FUeAgentSkinWeightImportCommandError Error;
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
					Error.Code = TEXT("skin_weight_import_result_invalid");
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
			FUeAgentSkinWeightImportCommandRoute Route;
			TSharedRef<IUeAgentSkinWeightImportCommandService> Service;
		};
	}

	bool TryCreateSkinWeightImportCommandHandlers(
		TSharedRef<IUeAgentSkinWeightImportCommandService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FUeAgentSkinWeightImportCommandRoute& Route
			: GetSkinWeightImportCommandRoutes())
		{
			if (!Route.IsValid())
			{
				OutHandlers.Reset();
				OutError = TEXT("skin_weight_import_route_invalid:")
					+ Route.CommandId;
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Route, Service));
		}
		return true;
	}
}
