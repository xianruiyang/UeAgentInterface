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

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			const TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			FString ErrorJson;
			FString JsonError;
			if (!Json::TryCanonicalize(Error, ErrorJson, JsonError))
			{
				ErrorJson = TEXT(
					"{\"code\":\"blueprint_component_error_serialization_failed\"}");
			}
			return FUeAgentCommandResult::Failure(
				Request, TEXT("{}"), MoveTemp(ErrorJson));
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
					return Failure(
						Request,
						TEXT("blueprint_component_query_authority_required"),
						Request.GetCommandId(),
						TEXT("the uai:query authority is required"));
				}
				if (!IsInGameThread())
				{
					return Failure(
						Request,
						TEXT("blueprint_component_game_thread_required"),
						Request.GetCommandId(),
						TEXT("Blueprint Component discovery runs on the UE game thread"));
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
					return Failure(
						Request,
						Error.Code.IsEmpty()
							? FString(TEXT("blueprint_component_query_failed"))
							: Error.Code,
						Error.SubjectId,
						Error.Detail);
				}
				if (!Response.bReadOnly)
				{
					return Failure(
						Request,
						TEXT("blueprint_component_query_not_read_only"),
						Request.GetCommandId(),
						TEXT("the candidate service violated its read-only contract"));
				}
				TSharedPtr<FJsonObject> Payload;
				FString JsonError;
				if (!Json::TryParseObject(Response.PayloadJson, Payload, JsonError))
				{
					return Failure(
						Request,
						TEXT("blueprint_component_result_invalid"),
						Request.GetCommandId(),
						JsonError);
				}
				FString CanonicalPayload;
				if (!Json::TryCanonicalize(Payload, CanonicalPayload, JsonError))
				{
					return Failure(
						Request,
						TEXT("blueprint_component_result_invalid"),
						Request.GetCommandId(),
						JsonError);
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
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(MakeShared<FHandler>(MoveTemp(Service)));
		return true;
	}
}
