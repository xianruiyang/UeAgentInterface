#include "QueryOwners/WorldSpatial/UeAgentLevelWorldQueryOwner.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/WorldSpatial/UeAgentLevelWorldDomainMechanics.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::QueryOwners::WorldSpatial
{
	using namespace CommandContract;
	using namespace DomainAdapters::WorldSpatial;
	using namespace HjsonWorkspace;

	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}

	bool TryExecuteActorCandidateQuery(
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		FWorkspaceError& OutError)
	{
		OutPayloadJson.Reset();
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		FString JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			OutError.Set(TEXT("world_actor_candidate_params_invalid"), JsonError);
			return false;
		}

		FUeAgentWorldActorCandidateQuery Query;
		if (!Params->TryGetStringField(TEXT("target"), Query.Target)
			|| Query.Target.TrimStartAndEnd().IsEmpty())
		{
			OutError.Set(TEXT("world_actor_candidate_target_required"));
			return false;
		}
		Params->TryGetStringField(TEXT("category"), Query.Category);
		Params->TryGetStringField(TEXT("query"), Query.Query);
		Params->TryGetStringField(TEXT("id"), Query.CandidateId);
		double Limit = Query.Limit;
		double Cursor = Query.Cursor;
		if (Params->TryGetNumberField(TEXT("limit"), Limit))
		{
			Query.Limit = FMath::RoundToInt(Limit);
		}
		if (Params->TryGetNumberField(TEXT("cursor"), Cursor))
		{
			Query.Cursor = FMath::RoundToInt(Cursor);
		}
		return TryQueryWorldActorCandidates(
			Query, OutPayloadJson, OutError);
	}
}
