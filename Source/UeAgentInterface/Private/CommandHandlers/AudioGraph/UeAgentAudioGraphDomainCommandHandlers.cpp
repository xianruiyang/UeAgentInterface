#include "CommandHandlers/AudioGraph/UeAgentAudioGraphDomainCommandHandlers.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/AudioGraph/UeAgentAudioGraphDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "QueryOwners/AudioGraph/UeAgentAudioGraphDomainQueryOwner.h"

namespace UE::UAI::CommandHandlers::AudioGraph
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::AudioGraph;

	namespace
	{
		const FString CandidateCommandId = TEXT("audio.graph.candidates");
		const FString CandidateOwner = TEXT("audio-graph-domain-owner");


		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request, UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
		}

		

		class FAudioGraphCandidateHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FAudioGraphCandidateHandler(
				TSharedRef<IUeAgentAudioGraphCandidateCommandService> InService)
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
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("audio_graph_candidate_query_authority_required"), TEXT("the uai:query authority is required"), TEXT("command"), FString(), Request.GetCommandId()));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("audio_graph_candidate_game_thread_required"), TEXT("audio candidate discovery runs on the UE game thread"), TEXT("command"), FString(), Request.GetCommandId()));
				}
				FAudioGraphCandidateCommandRequest ServiceRequest;
				ServiceRequest.CommandId = Request.GetCommandId();
				ServiceRequest.RequestId = Request.GetRequestId();
				ServiceRequest.CanonicalParamsJson = Request.GetParamsJson();
				ServiceRequest.CallerId = Caller.GetCallerId();
				ServiceRequest.AuthorityScopes = Caller.GetAuthorityScopes();
				ServiceRequest.CorrelationId = Caller.GetCorrelationId();
				ServiceRequest.bCancellationRequested = Cancellation->IsCancelled();
				FAudioGraphCandidateCommandResponse Response;
				FAudioGraphCandidateCommandError Error;
				if (!Service->Execute(ServiceRequest, Response, Error))
				{
					return Failure(Request, !Error.IsEmpty() ? Error : UE::UAI::Contract::FDiagnostic::Failure(TEXT("audio_graph_candidate_query_failed"), TEXT("The candidate query failed without a diagnostic."), TEXT("query")));
				}
				if (!Response.bReadOnly)
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("audio_graph_candidate_query_not_read_only"), TEXT("the candidate query service violated its contract"), TEXT("command"), FString(), Request.GetCommandId()));
				}
			return FUeAgentCommandResult::Success(Request, MoveTemp(Response.PayloadJson));
			}

		private:
			TSharedRef<IUeAgentAudioGraphCandidateCommandService> Service;
		};
	}

	bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError)
	{
		FString Payload;
		bool bReadOnly = false;
		if (!DomainAdapters::AudioGraph::TryExecuteContentGraph(
			Request.GetCommandId(), Request.GetTargetIdentity(),
			Request.GetCanonicalParamsJson(), Payload, bReadOnly, OutError))
		{
			return false;
		}
		if (QueryOwners::AudioGraph::IsReadOnlyContentGraphCommand(Request.GetCommandId()) != bReadOnly)
		{
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("audio_graph_domain_read_only_contract_mismatch"),
				TEXT("The domain result disagrees with the command's declared read-only behavior."), TEXT("domain_execute"));
			Detail.CandidateName = Request.GetCommandId();
			Detail.TargetPath = Request.GetTargetIdentity();
			Detail.SourceKind = TEXT("native_result");
			Detail.ActualValue = MakeShared<FJsonValueBoolean>(bReadOnly);
			Detail.ExpectedValue = MakeShared<FJsonValueBoolean>(QueryOwners::AudioGraph::IsReadOnlyContentGraphCommand(Request.GetCommandId()));
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}

	bool TryCreateAudioGraphCandidateCommandHandlers(
		TSharedRef<IUeAgentAudioGraphCandidateCommandService> Service,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(MakeShared<FAudioGraphCandidateHandler>(MoveTemp(Service)));
		return true;
	}
}
