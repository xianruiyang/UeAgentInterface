#include "CommandHandlers/WorldSpatial/UeAgentLevelWorldCommandHandlers.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/WorldSpatial/UeAgentLevelWorldDomainMechanics.h"
#include "Dom/JsonObject.h"
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

		FUeAgentCommandResult CandidateFailure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			FString ErrorJson;
			FString JsonError;
			if (!Json::TryCanonicalize(Error, ErrorJson, JsonError))
			{
				ErrorJson = TEXT("{\"code\":\"world_actor_candidate_error_serialization_failed\"}");
			}
			return FUeAgentCommandResult::Failure(
				Request, TEXT("{}"), MoveTemp(ErrorJson));
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
					return CandidateFailure(
						Request,
						TEXT("world_actor_candidate_query_authority_required"),
						Request.GetCommandId(),
						TEXT("the uai:query authority is required"));
				}
				if (!IsInGameThread())
				{
					return CandidateFailure(
						Request,
						TEXT("world_actor_candidate_game_thread_required"),
						Request.GetCommandId(),
						TEXT("Place Actors discovery runs on the UE game thread"));
				}
				if (Cancellation->IsCancelled())
				{
					return CandidateFailure(
						Request,
						TEXT("world_actor_candidate_cancelled"),
						Request.GetCommandId(),
						TEXT("the request was cancelled before enumeration"));
				}

				FString PayloadJson;
				FWorkspaceError Error;
				if (!QueryOwners::WorldSpatial::TryExecuteActorCandidateQuery(
						Request.GetParamsJson(), PayloadJson, Error))
				{
					return CandidateFailure(
						Request,
						Error.GetCode().IsEmpty()
							? FString(TEXT("world_actor_candidate_query_failed"))
							: Error.GetCode(),
						Error.GetSourceUnit(),
						Error.GetJsonPointer());
				}
				TSharedPtr<FJsonObject> Payload;
				FString JsonError;
				if (!Json::TryParseObject(PayloadJson, Payload, JsonError)
					|| !Payload.IsValid())
				{
					return CandidateFailure(
						Request,
						TEXT("world_actor_candidate_result_invalid"),
						Request.GetCommandId(), JsonError);
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
				Request.GetCanonicalParamsJson(), Payload, bReadOnly, OutError))
		{
			return false;
		}
		if (QueryOwners::WorldSpatial::IsReadOnlyContentGraphCommand(
				Request.GetCommandId()) != bReadOnly)
		{
			OutError.Set(TEXT("level_world_domain_read_only_contract_mismatch"),
				Request.GetCommandId());
			return false;
		}
		OutResponse = HjsonWorkspace::FImportInitializationDomainResponse(
			MoveTemp(Payload), bReadOnly);
		return OutResponse.IsValid();
	}

	bool TryCreateWorldSpatialCommandHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(MakeShared<FActorCandidateHandler>());
		return true;
	}
}
