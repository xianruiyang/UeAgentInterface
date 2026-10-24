#include "CommandHandlers/ExternalImport/Factory/UeAgentFactoryExternalImportCommandHandlers.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::CommandHandlers::ExternalImport::Factory
{
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::ExternalImport::Factory;

	namespace
	{
		const FString Owner = TEXT("factory-external-import-owner");

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request,
            const FUeAgentFactoryExternalImportCommandError& Error)
        { return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Error); }

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			FHandler(
				FUeAgentFactoryExternalImportCommandRoute InRoute,
				TSharedRef<IUeAgentFactoryExternalImportCommandService> InService)
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
					FUeAgentFactoryExternalImportCommandError Error;
					Error.Code = TEXT("factory_import_authority_required");
					Error.SubjectId = Request.GetCommandId();
					Error.InternalContext =
						TEXT("the public factory import route requires uai:operate");
					return Failure(Request, Error);
				}
				if (!IsInGameThread())
				{
					FUeAgentFactoryExternalImportCommandError Error;
					Error.Code = TEXT("factory_import_game_thread_required");
					Error.SubjectId = Request.GetCommandId();
					Error.InternalContext =
						TEXT("the public ingress must dispatch UE import mechanics on the game thread");
					return Failure(Request, Error);
				}

				FUeAgentFactoryExternalImportCommandRequest ServiceRequest;
				ServiceRequest.CommandId = Request.GetCommandId();
				ServiceRequest.RequestId = Request.GetRequestId();
				ServiceRequest.CanonicalParamsJson = Request.GetParamsJson();
				ServiceRequest.CallerId = Caller.GetCallerId();
				ServiceRequest.AuthorityScopes = Caller.GetAuthorityScopes();
				ServiceRequest.CorrelationId = Caller.GetCorrelationId();
				ServiceRequest.bCancellationRequested =
					Cancellation->IsCancelled();

				FUeAgentFactoryExternalImportCommandResponse Response;
				FUeAgentFactoryExternalImportCommandError Error;
				if (!Service->Execute(ServiceRequest, Response, Error))
				{
					return Failure(Request, Error);
				}
				TSharedPtr<FJsonObject> Payload;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!CommandContract::Json::TryParseObject(
						Response.PayloadJson,
						Payload,
						JsonError))
				{
					Error = MoveTemp(JsonError);
					Error.SubjectId = Request.GetCommandId();
					Error.TerminalStatus = TEXT("state_unknown");
					Error.MutationStarted = true;
					Error.bRetrySafe = false;
					return Failure(Request, Error);
				}
				FString CanonicalPayload;
				if (!CommandContract::Json::TryCanonicalize(
						Payload,
						CanonicalPayload,
						JsonError))
				{
					Error = MoveTemp(JsonError);
					Error.SubjectId = Request.GetCommandId();
					Error.TerminalStatus = TEXT("state_unknown");
					Error.MutationStarted = true;
					Error.bRetrySafe = false;
					return Failure(Request, Error);
				}
				return FUeAgentCommandResult::Success(
					Request,
					MoveTemp(CanonicalPayload));
			}

		private:
			FUeAgentFactoryExternalImportCommandRoute Route;
			TSharedRef<IUeAgentFactoryExternalImportCommandService> Service;
		};
	}

	bool TryCreateFactoryExternalImportCommandHandlers(
		TSharedRef<IUeAgentFactoryExternalImportCommandService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FUeAgentFactoryExternalImportCommandRoute& Route
			: GetFactoryExternalImportCommandRoutes())
		{
			if (!Route.TryValidate(OutError))
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("factory_import_route_invalid"), OutError, TEXT("handler_factory"));
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Route, Service));
		}
		return true;
	}
}
