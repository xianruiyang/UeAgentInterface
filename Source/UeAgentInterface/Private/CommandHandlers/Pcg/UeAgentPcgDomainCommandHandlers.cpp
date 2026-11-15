#include "CommandHandlers/Pcg/UeAgentPcgDomainCommandHandlers.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/Pcg/UeAgentPcgDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "QueryOwners/Pcg/UeAgentPcgDomainQueryOwner.h"

namespace UE::UAI::CommandHandlers::Pcg
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::Pcg;

	namespace
	{
		const FString CandidateCommandId = TEXT("pcg.node.candidates");
		const FString CandidateOwner = TEXT("pcg-domain-owner");

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request, UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
		}

		

		class FPcgCandidateHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FPcgCandidateHandler(
				TSharedRef<IUeAgentPcgCandidateCommandService> InService,
				FString InCommandId = CandidateCommandId)
				: Service(MoveTemp(InService)), CommandId(MoveTemp(InCommandId))
			{
			}

			virtual const FString& GetCommandId() const override { return CommandId; }
			virtual const FString& GetPublicOwner() const override { return CandidateOwner; }

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!Caller.IsValid() || !Caller.GetAuthorityScopes().Contains(TEXT("uai:query")))
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_candidate_query_authority_required"), TEXT("the uai:query authority is required"), TEXT("command"), FString(), Request.GetCommandId()));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_candidate_game_thread_required"), TEXT("PCG settings discovery runs on the UE game thread"), TEXT("command"), FString(), Request.GetCommandId()));
				}
				FUeAgentPcgCandidateCommandRequest ServiceRequest;
				ServiceRequest.CommandId = Request.GetCommandId();
				ServiceRequest.RequestId = Request.GetRequestId();
				ServiceRequest.CanonicalParamsJson = Request.GetParamsJson();
				ServiceRequest.CallerId = Caller.GetCallerId();
				ServiceRequest.AuthorityScopes = Caller.GetAuthorityScopes();
				ServiceRequest.CorrelationId = Caller.GetCorrelationId();
				ServiceRequest.bCancellationRequested = Cancellation->IsCancelled();
				FUeAgentPcgCandidateCommandResponse Response;
				FUeAgentPcgCandidateCommandError Error;
				if (!Service->Execute(ServiceRequest, Response, Error))
				{
					return Failure(Request, !Error.IsEmpty() ? Error : UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_candidate_query_failed"), TEXT("The candidate query failed without a diagnostic."), TEXT("query")));
				}
				if (!Response.bReadOnly)
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_candidate_query_not_read_only"), TEXT("the candidate query service violated its contract"), TEXT("command"), FString(), Request.GetCommandId()));
				}
				return FUeAgentCommandResult::Success(Request, MoveTemp(Response.PayloadJson));
			}

		private:
			TSharedRef<IUeAgentPcgCandidateCommandService> Service;
			FString CommandId;
		};
	}

	bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError)
	{
		FString Payload;
		bool bReadOnly = false;
		if (!DomainAdapters::Pcg::TryExecuteContentGraph(
				Request.GetCommandId(), Request.GetTargetIdentity(),
				Request.GetRootTypeIdentity(),
				Request.GetCanonicalParamsJson(), Payload, bReadOnly, OutError,
				Request.GetWorkspaceExportPreparation().Get()))
		{
			return false;
		}
		if (QueryOwners::Pcg::IsReadOnlyContentGraphCommand(Request.GetCommandId()) != bReadOnly)
		{
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_domain_read_only_contract_mismatch"),
				TEXT("The domain result disagrees with the command's declared read-only behavior."), TEXT("domain_execute"));
			Detail.CandidateName = Request.GetCommandId();
			Detail.TargetPath = Request.GetTargetIdentity();
			Detail.SourceKind = TEXT("native_result");
			Detail.ActualValue = MakeShared<FJsonValueBoolean>(bReadOnly);
			Detail.ExpectedValue = MakeShared<FJsonValueBoolean>(QueryOwners::Pcg::IsReadOnlyContentGraphCommand(Request.GetCommandId()));
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}

	bool TryCreatePcgCandidateCommandHandlers(
		TSharedRef<IUeAgentPcgCandidateCommandService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(MakeShared<FPcgCandidateHandler>(Service));
		OutHandlers.Add(MakeShared<FPcgCandidateHandler>(Service, TEXT("pcg.generated_spatial_data.read")));
		OutHandlers.Add(MakeShared<FPcgCandidateHandler>(Service, TEXT("pcg.partition.spatial_state.read")));
		return true;
	}
}
