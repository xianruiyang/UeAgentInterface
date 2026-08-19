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

		class FUmgWidgetHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FUmgWidgetHandler(
				TSharedRef<IUeAgentUmgWidgetCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetCommandId() const override { return CommandId; }
			virtual const FString& GetPublicOwner() const override { return Owner; }

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return Failure(Request, TEXT("umg_widget_authority_required"),
						Request.GetCommandId(), TEXT("the uai:operate authority is required"));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, TEXT("umg_widget_game_thread_required"),
						Request.GetCommandId(), TEXT("UMG mechanics run on the UE game thread"));
				}

				FUmgWidgetCommandRequest ServiceRequest;
				ServiceRequest.CommandId = Request.GetCommandId();
				ServiceRequest.RequestId = Request.GetRequestId();
				ServiceRequest.CanonicalParamsJson = Request.GetParamsJson();
				ServiceRequest.CallerId = Caller.GetCallerId();
				ServiceRequest.AuthorityScopes = Caller.GetAuthorityScopes();
				ServiceRequest.CorrelationId = Caller.GetCorrelationId();
				ServiceRequest.bCancellationRequested = Cancellation->IsCancelled();
				FUmgWidgetCommandResponse Response;
				FUmgWidgetCommandError Error;
				if (!Service->Execute(ServiceRequest, Response, Error))
				{
					return Failure(Request,
						Error.Code.IsEmpty() ? FString(TEXT("umg_widget_execution_failed")) : Error.Code,
						Error.SubjectId, Error.Detail);
				}
				if (Response.bReadOnly)
				{
					return Failure(Request, TEXT("umg_widget_mutation_reported_read_only"),
						Request.GetCommandId(), TEXT("the UMG authoring service violated its mutation contract"));
				}
				TSharedPtr<FJsonObject> Payload;
				FString JsonError;
				if (!Json::TryParseObject(Response.PayloadJson, Payload, JsonError)
					|| !Payload.IsValid())
				{
					return Failure(Request, TEXT("umg_widget_result_invalid"),
						Request.GetCommandId(), JsonError);
				}
				FString CanonicalPayload;
				if (!Json::TryCanonicalize(Payload, CanonicalPayload, JsonError))
				{
					return Failure(Request, TEXT("umg_widget_result_invalid"),
						Request.GetCommandId(), JsonError);
				}
				return FUeAgentCommandResult::Success(Request, MoveTemp(CanonicalPayload));
			}

		private:
			TSharedRef<IUeAgentUmgWidgetCommandService> Service;
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
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(MakeShared<FUmgWidgetHandler>(MoveTemp(Service)));
		return true;
	}
}
