#include "CommandHandlers/BlueprintComponent/UeAgentBlueprintComponentCommandHandlers.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::CommandHandlers::BlueprintComponent
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::OperationOwners::BlueprintComponent;

	namespace
	{
		const FString CommandId = TEXT("blueprint.component.candidates");
		const FString Owner = TEXT("blueprint-component-authoring-owner");

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request, UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
		}

		UE::UAI::Contract::FDiagnostic ResultFailure(const FString& Code, const FString& SubjectId, const UE::UAI::Contract::FDiagnostic& Cause)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, TEXT("The query input or result could not be encoded or decoded."), TEXT("command"), FString(), SubjectId);
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (!Cause.IsEmpty()) Diagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Cause);
			return Diagnostic;
		}

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FHandler(
				TSharedRef<IUeAgentBlueprintComponentCommandService> InService)
				: Service(MoveTemp(InService))
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
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:query")))
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_component_query_authority_required"), TEXT("the uai:query authority is required"), TEXT("command"), FString(), Request.GetCommandId()));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_component_game_thread_required"), TEXT("Blueprint Component discovery runs on the UE game thread"), TEXT("command"), FString(), Request.GetCommandId()));
				}
				FUeAgentBlueprintComponentCommandRequest ServiceRequest;
				ServiceRequest.CommandId = Request.GetCommandId();
				ServiceRequest.RequestId = Request.GetRequestId();
				ServiceRequest.CanonicalParamsJson = Request.GetParamsJson();
				ServiceRequest.CallerId = Caller.GetCallerId();
				ServiceRequest.AuthorityScopes = Caller.GetAuthorityScopes();
				ServiceRequest.CorrelationId = Caller.GetCorrelationId();
				ServiceRequest.bCancellationRequested = Cancellation->IsCancelled();
				FUeAgentBlueprintComponentCommandResponse Response;
				FUeAgentBlueprintComponentCommandError Error;
				if (!Service->Execute(ServiceRequest, Response, Error))
				{
					return Failure(Request, !Error.IsEmpty() ? Error : UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_component_query_failed"), TEXT("The candidate query failed without a diagnostic."), TEXT("query")));
				}
				if (!Response.bReadOnly)
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_component_query_not_read_only"), TEXT("the candidate service violated its read-only contract"), TEXT("command"), FString(), Request.GetCommandId()));
				}
				TSharedPtr<FJsonObject> Payload;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryParseObject(Response.PayloadJson, Payload, JsonError))
				{
					return Failure(Request, ResultFailure(TEXT("blueprint_component_result_invalid"), Request.GetCommandId(), JsonError));
				}
				FString CanonicalPayload;
				if (!Json::TryCanonicalize(Payload, CanonicalPayload, JsonError))
				{
					return Failure(Request, ResultFailure(TEXT("blueprint_component_result_invalid"), Request.GetCommandId(), JsonError));
				}
				return FUeAgentCommandResult::Success(
					Request, MoveTemp(CanonicalPayload));
			}

		private:
			TSharedRef<IUeAgentBlueprintComponentCommandService> Service;
		};
	}

	bool TryCreateBlueprintComponentCommandHandlers(
		TSharedRef<IUeAgentBlueprintComponentCommandService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(MakeShared<FHandler>(MoveTemp(Service)));
		return true;
	}
}
