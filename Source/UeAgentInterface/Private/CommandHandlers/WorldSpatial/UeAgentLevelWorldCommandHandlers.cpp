#include "CommandHandlers/WorldSpatial/UeAgentLevelWorldCommandHandlers.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/WorldSpatial/UeAgentLevelWorldDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "QueryOwners/WorldSpatial/UeAgentLevelWorldQueryOwner.h"

namespace UE::UAI::CommandHandlers::WorldSpatial
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace Contract;
	using namespace HjsonWorkspace;

	namespace
	{
		const FString ActorCandidateCommandId = TEXT("world.actor.candidates");
		const FString ActorCandidateOwner = TEXT("level-world-domain-owner");

		FUeAgentCommandResult CandidateFailure(const FUeAgentCommandRequest& Request,
            UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = SubjectId.IsEmpty() ? Request.GetCommandId() : SubjectId;
            if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("command_submit");
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
        }

		class FActorCandidateHandler final : public IUeAgentExactCommandHandler
		{
		public:
			virtual const FString& GetCommandId() const override
			{
				return ActorCandidateCommandId;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return ActorCandidateOwner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:query")))
				{
					return CandidateFailure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_actor_candidate_query_authority_required"), TEXT("the uai:query authority is required"), TEXT("command_submit")), Request.GetCommandId());
				}
				if (!IsInGameThread())
				{
					return CandidateFailure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_actor_candidate_game_thread_required"), TEXT("Place Actors discovery runs on the UE game thread"), TEXT("command_submit")), Request.GetCommandId());
				}
				if (Cancellation->IsCancelled())
				{
					return CandidateFailure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_actor_candidate_cancelled"), TEXT("the request was cancelled before enumeration"), TEXT("command_submit")), Request.GetCommandId());
				}

				FString PayloadJson;
				FWorkspaceError Error;
				if (!QueryOwners::WorldSpatial::TryExecuteActorCandidateQuery(
						Request.GetParamsJson(), PayloadJson, Error))
				{
					auto Diagnostic = Error.GetDiagnostic();
					if (Error.GetCode().IsEmpty())
					{
						Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_actor_candidate_query_failed"), TEXT("The actor candidate query failed without a diagnostic."), TEXT("command_submit"));
						Diagnostic.CandidateName = Request.GetCommandId();
					}
					return CandidateFailure(Request, Diagnostic, Error.GetSourceUnit());
				}
				TSharedPtr<FJsonObject> Payload;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryParseObject(PayloadJson, Payload, JsonError)
					|| !Payload.IsValid())
				{
					return CandidateFailure(Request, JsonError, Request.GetCommandId());
				}
				return FUeAgentCommandResult::Success(
					Request, MoveTemp(PayloadJson));
			}
		};
	}

	bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError)
	{
		FString Payload;
		bool bReadOnly = false;
		if (!DomainAdapters::WorldSpatial::TryExecuteContentGraph(
				Request.GetCommandId(), Request.GetTargetIdentity(),
				Request.GetCanonicalParamsJson(), Payload, bReadOnly, OutError,
				Request.GetRequestId()))
		{
			return false;
		}
		if (QueryOwners::WorldSpatial::IsReadOnlyContentGraphCommand(
				Request.GetCommandId()) != bReadOnly)
		{
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_world_domain_read_only_contract_mismatch"),
				TEXT("The domain result disagrees with the command's declared read-only behavior."), TEXT("domain_execute"));
			Detail.CandidateName = Request.GetCommandId();
			Detail.TargetPath = Request.GetTargetIdentity();
			Detail.ActualValue = MakeShared<FJsonValueBoolean>(bReadOnly);
			Detail.ExpectedValue = MakeShared<FJsonValueBoolean>(QueryOwners::WorldSpatial::IsReadOnlyContentGraphCommand(Request.GetCommandId()));
			Detail.SourceKind = TEXT("native_result");
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(
			MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}

	bool TryCreateWorldSpatialCommandHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(MakeShared<FActorCandidateHandler>());
		return true;
	}
}
