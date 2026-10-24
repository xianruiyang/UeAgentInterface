#include "CommandHandlers/UmgWidget/UeAgentUmgWidgetCommandHandlers.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

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
		const FString Owner = TEXT("umg-widget-domain-owner");
		const FString CandidateCommandId = TEXT("umg.widget.candidates");

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request,
            UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = SubjectId.IsEmpty() ? Request.GetCommandId() : SubjectId;
            if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("command_submit");
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
        }

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
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_widget_candidate_query_authority_required"), TEXT("the uai:query authority is required"), TEXT("command_submit")), Request.GetCommandId());
				}
				if (!IsInGameThread())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_widget_candidate_game_thread_required"), TEXT("UMG candidate discovery runs on the UE game thread"), TEXT("command_submit")), Request.GetCommandId());
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
					auto Diagnostic = Error;
					if (Error.Code.IsEmpty())
					{
						Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_widget_candidate_query_failed"), TEXT("The widget candidate service failed without a diagnostic."), TEXT("command_submit"));
						Diagnostic.CandidateName = Request.GetCommandId();
					}
					return Failure(Request, Diagnostic, Error.SubjectId);
				}
				if (!Response.bReadOnly)
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_widget_candidate_query_not_read_only"), TEXT("the candidate query service violated its contract"), TEXT("command_submit")), Request.GetCommandId());
				}
				return FUeAgentCommandResult::Success(Request, MoveTemp(Response.PayloadJson));
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
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_widget_read_only_contract_mismatch"),
				TEXT("The domain result disagrees with the command's declared read-only behavior."), TEXT("domain_execute"));
			Detail.CandidateName = Request.GetCommandId();
			Detail.TargetPath = Request.GetTargetIdentity();
			Detail.SourceKind = TEXT("native_result");
			Detail.ActualValue = MakeShared<FJsonValueBoolean>(bReadOnly);
			Detail.ExpectedValue = MakeShared<FJsonValueBoolean>(QueryOwners::UmgWidget::IsReadOnlyContentGraphCommand(Request.GetCommandId()));
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(
			MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}

	bool TryCreateUmgWidgetCandidateCommandHandlers(
		TSharedRef<IUeAgentUmgWidgetCandidateCommandService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(
			MakeShared<FUmgWidgetCandidateHandler>(MoveTemp(Service)));
		return true;
	}
}
