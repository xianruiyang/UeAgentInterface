#include "CommandHandlers/UmgWidget/UeAgentUmgWidgetCommandHandlers.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/UmgWidget/UeAgentUmgWidgetMechanics.h"
#include "Dom/JsonObject.h"
#include "QueryOwners/UmgWidget/UeAgentUmgWidgetQueryOwner.h"

namespace UE::UAI::CommandHandlers::UmgWidget
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::UmgWidget;

	namespace
	{
		const FString CommandId = TEXT("umg.authoring.apply");
		const FString Owner = TEXT("umg-widget-domain-owner");
		const FString CandidateCommandId = TEXT("umg.widget.candidates");

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
				ErrorJson = TEXT("{\"code\":\"umg_widget_error_serialization_failed\"}");
			}
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(ErrorJson));
		}

		class FRetiredUmgWidgetHandler final : public IUeAgentExactCommandHandler
		{
		public:
			virtual const FString& GetCommandId() const override { return CommandId; }
			virtual const FString& GetPublicOwner() const override { return Owner; }

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				(void)Caller;
				(void)Cancellation;
				return Failure(
					Request,
					TEXT("umg_legacy_authoring_retired"),
					Request.GetCommandId(),
					TEXT("use content_graph.import_workspace"));
			}
		};

		class FUmgWidgetCandidateHandler final
			: public IUeAgentExactCommandHandler
		{
		public:
			explicit FUmgWidgetCandidateHandler(
				TSharedRef<IUeAgentUmgWidgetCandidateCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				return CandidateCommandId;
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
						TEXT("umg_widget_candidate_query_authority_required"),
						Request.GetCommandId(),
						TEXT("the uai:query authority is required"));
				}
				if (!IsInGameThread())
				{
					return Failure(
						Request,
						TEXT("umg_widget_candidate_game_thread_required"),
						Request.GetCommandId(),
						TEXT("UMG candidate discovery runs on the UE game thread"));
				}

				FUmgWidgetCandidateCommandRequest ServiceRequest;
				ServiceRequest.CommandId = Request.GetCommandId();
				ServiceRequest.RequestId = Request.GetRequestId();
				ServiceRequest.CanonicalParamsJson = Request.GetParamsJson();
				ServiceRequest.CallerId = Caller.GetCallerId();
				ServiceRequest.AuthorityScopes = Caller.GetAuthorityScopes();
				ServiceRequest.CorrelationId = Caller.GetCorrelationId();
				ServiceRequest.bCancellationRequested = Cancellation->IsCancelled();
				FUmgWidgetCandidateCommandResponse Response;
				FUmgWidgetCandidateCommandError Error;
				if (!Service->Execute(ServiceRequest, Response, Error))
				{
					return Failure(
						Request,
						Error.Code.IsEmpty()
							? FString(TEXT("umg_widget_candidate_query_failed"))
							: Error.Code,
						Error.SubjectId,
						Error.Detail);
				}
				if (!Response.bReadOnly)
				{
					return Failure(
						Request,
						TEXT("umg_widget_candidate_query_not_read_only"),
						Request.GetCommandId(),
						TEXT("the candidate query service violated its contract"));
				}
				TSharedPtr<FJsonObject> Payload;
				FString JsonError;
				if (!Json::TryParseObject(
						Response.PayloadJson, Payload, JsonError)
					|| !Payload.IsValid())
				{
					return Failure(
						Request,
						TEXT("umg_widget_candidate_result_invalid"),
						Request.GetCommandId(),
						JsonError);
				}
				FString CanonicalPayload;
				if (!Json::TryCanonicalize(Payload, CanonicalPayload, JsonError))
				{
					return Failure(
						Request,
						TEXT("umg_widget_candidate_result_invalid"),
						Request.GetCommandId(),
						JsonError);
				}
				return FUeAgentCommandResult::Success(
					Request, MoveTemp(CanonicalPayload));
			}

		private:
			TSharedRef<IUeAgentUmgWidgetCandidateCommandService> Service;
		};
	}

	bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError)
	{
		FString Payload;
		bool bReadOnly = false;
		if (!DomainAdapters::UmgWidget::TryExecuteContentGraph(
			Request.GetCommandId(), Request.GetTargetIdentity(),
			Request.GetCanonicalParamsJson(), Payload, bReadOnly, OutError))
		{
			return false;
		}
		if (QueryOwners::UmgWidget::IsReadOnlyContentGraphCommand(
				Request.GetCommandId()) != bReadOnly)
		{
			OutError.Set(TEXT("umg_widget_read_only_contract_mismatch"),
				Request.GetCommandId());
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(
			MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}

	bool TryCreateUmgWidgetCommandHandlers(
		TSharedRef<IUeAgentUmgWidgetCommandService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		(void)Service;
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(MakeShared<FRetiredUmgWidgetHandler>());
		return true;
	}

	bool TryCreateUmgWidgetCandidateCommandHandlers(
		TSharedRef<IUeAgentUmgWidgetCandidateCommandService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(
			MakeShared<FUmgWidgetCandidateHandler>(MoveTemp(Service)));
		return true;
	}
}
