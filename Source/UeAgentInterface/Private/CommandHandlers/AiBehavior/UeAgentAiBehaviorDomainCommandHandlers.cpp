#include "CommandHandlers/AiBehavior/UeAgentAiBehaviorDomainCommandHandlers.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/AiBehavior/UeAgentAiBehaviorDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorDomainQueryOwner.h"

namespace UE::UAI::CommandHandlers::AiBehavior
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::AiBehavior;

	namespace
	{
		const FString CandidateCommandId = TEXT("ai.behavior.candidates");
		const FString CandidateOwner = TEXT("ai-behavior-domain-owner");

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
				ErrorJson = TEXT("{\"code\":\"ai_behavior_candidate_error_serialization_failed\"}");
			}
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(ErrorJson));
		}

		class FAiBehaviorCandidateHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FAiBehaviorCandidateHandler(
				TSharedRef<IUeAgentAiBehaviorCandidateCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetCommandId() const override { return CandidateCommandId; }
			virtual const FString& GetPublicOwner() const override { return CandidateOwner; }

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!Caller.IsValid() || !Caller.GetAuthorityScopes().Contains(TEXT("uai:query")))
				{
					return Failure(Request, TEXT("ai_behavior_candidate_query_authority_required"), Request.GetCommandId(), TEXT("the uai:query authority is required"));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, TEXT("ai_behavior_candidate_game_thread_required"), Request.GetCommandId(), TEXT("AI candidate discovery runs on the UE game thread"));
				}
				FAiBehaviorCandidateCommandRequest ServiceRequest;
				ServiceRequest.CommandId = Request.GetCommandId();
				ServiceRequest.RequestId = Request.GetRequestId();
				ServiceRequest.CanonicalParamsJson = Request.GetParamsJson();
				ServiceRequest.CallerId = Caller.GetCallerId();
				ServiceRequest.AuthorityScopes = Caller.GetAuthorityScopes();
				ServiceRequest.CorrelationId = Caller.GetCorrelationId();
				ServiceRequest.bCancellationRequested = Cancellation->IsCancelled();
				FAiBehaviorCandidateCommandResponse Response;
				FAiBehaviorCandidateCommandError Error;
				if (!Service->Execute(ServiceRequest, Response, Error))
				{
					return Failure(
						Request,
						Error.Code.IsEmpty() ? FString(TEXT("ai_behavior_candidate_query_failed")) : Error.Code,
						Error.SubjectId,
						Error.Detail);
				}
				if (!Response.bReadOnly)
				{
					return Failure(Request, TEXT("ai_behavior_candidate_query_not_read_only"), Request.GetCommandId(), TEXT("the candidate query service violated its contract"));
				}
				TSharedPtr<FJsonObject> Payload;
				FString JsonError;
				if (!Json::TryParseObject(Response.PayloadJson, Payload, JsonError)
					|| !Payload.IsValid())
				{
					return Failure(Request, TEXT("ai_behavior_candidate_result_invalid"), Request.GetCommandId(), JsonError);
				}
				FString CanonicalPayload;
				if (!Json::TryCanonicalize(Payload, CanonicalPayload, JsonError))
				{
					return Failure(Request, TEXT("ai_behavior_candidate_result_invalid"), Request.GetCommandId(), JsonError);
				}
				return FUeAgentCommandResult::Success(Request, MoveTemp(CanonicalPayload));
			}

		private:
			TSharedRef<IUeAgentAiBehaviorCandidateCommandService> Service;
		};
	}

	bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError)
	{
		FString Payload;
		bool bReadOnly = false;
		if (!DomainAdapters::AiBehavior::TryExecuteContentGraph(
			Request.GetCommandId(), Request.GetTargetIdentity(),
			Request.GetRootTypeIdentity(),
			Request.GetCanonicalParamsJson(), Payload, bReadOnly, OutError))
		{
			return false;
		}
		if (QueryOwners::AiBehavior::IsReadOnlyContentGraphCommand(Request.GetCommandId()) != bReadOnly)
		{
			OutError.Set(TEXT("ai_behavior_domain_read_only_contract_mismatch"), Request.GetCommandId());
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}

	bool TryCreateAiBehaviorCandidateCommandHandlers(
		TSharedRef<IUeAgentAiBehaviorCandidateCommandService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(MakeShared<FAiBehaviorCandidateHandler>(MoveTemp(Service)));
		return true;
	}
}
