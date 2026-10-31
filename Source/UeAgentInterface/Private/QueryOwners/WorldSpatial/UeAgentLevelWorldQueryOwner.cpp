#include "QueryOwners/WorldSpatial/UeAgentLevelWorldQueryOwner.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/WorldSpatial/UeAgentLevelWorldDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "Domains/Level/Authoring/UeAgentLevelComponentCandidates.h"

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

	bool TryExecuteComponentCandidateQuery(const FString& CanonicalParamsJson,
		FString& OutPayloadJson, FWorkspaceError& OutError)
	{
		OutPayloadJson.Reset();
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		Contract::FDiagnostic Error;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, Error)
			|| !UeAgentLevelAuthoring::QueryComponentCandidates(Params, OutPayloadJson, Error))
		{
			OutError.SetFailure(MoveTemp(Error));
			return false;
		}
		return true;
	}

	bool TryExecuteActorCandidateQuery(
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		FWorkspaceError& OutError)
	{
		OutPayloadJson.Reset();
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			OutError.SetFailure(JsonError, FString());
			return false;
		}

		FUeAgentWorldActorCandidateQuery Query;
		if (!Params->TryGetStringField(TEXT("target"), Query.Target)
			|| Query.Target.TrimStartAndEnd().IsEmpty())
		{
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_actor_candidate_target_required"),
				TEXT("Actor candidate queries require a non-empty target string."), TEXT("candidate_query"));
			Detail.SourceKind = TEXT("command_input");
			Detail.Source.JsonPath = TEXT("/target");
			Detail.ActualValue = Params->TryGetField(TEXT("target"));
			Detail.ActualValuePresent = Detail.ActualValue.IsValid();
			Detail.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty target string"));
			OutError.SetFailure(MoveTemp(Detail));
			return false;
		}
		Params->TryGetStringField(TEXT("category"), Query.Category);
		Params->TryGetStringField(TEXT("query"), Query.Query);
		return TryQueryWorldActorCandidates(
			Query, OutPayloadJson, OutError);
	}
}
