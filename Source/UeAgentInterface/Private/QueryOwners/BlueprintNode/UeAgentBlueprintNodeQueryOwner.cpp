#include "QueryOwners/BlueprintNode/UeAgentBlueprintNodeQueryOwner.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::QueryOwners::BlueprintNode
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::DomainAdapters::Blueprint::Node;
	using namespace UE::UAI::OperationOwners::BlueprintNode;

	bool TryExecuteCandidateQuery(
		const FUeAgentBlueprintNodeCommandRequest& Request,
		FUeAgentBlueprintNodeCommandResponse& OutResponse,
		FUeAgentBlueprintNodeCommandError& OutError)
	{
		OutResponse = {};
		OutError = {};
		TSharedPtr<FJsonObject> Params;
		FString JsonError;
		if (!Json::TryParseObject(Request.CanonicalParamsJson, Params, JsonError))
		{
			OutError.Code = TEXT("blueprint_node_params_invalid");
			OutError.SubjectId = Request.CommandId;
			OutError.Detail = MoveTemp(JsonError);
			return false;
		}
		FUeAgentBlueprintNodeCandidateQuery Query;
		if (!Params->TryGetStringField(TEXT("target"), Query.Target)
			|| !Params->TryGetStringField(TEXT("graph_id"), Query.GraphId))
		{
			OutError.Code = TEXT("blueprint_node_candidate_identity_required");
			OutError.SubjectId = Request.CommandId;
			return false;
		}
		Params->TryGetStringField(TEXT("query"), Query.Query);
		Params->TryGetStringField(TEXT("id"), Query.CandidateId);
		Params->TryGetStringField(TEXT("node_guid"), Query.NodeGuid);
		Params->TryGetStringField(TEXT("pin_guid"), Query.PinGuid);
		Params->TryGetStringField(TEXT("pin_name"), Query.PinName);
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
		FUeAgentBlueprintNodeMechanicsError MechanicsError;
		if (!TryQueryBlueprintNodeCandidates(
				Query,
				OutResponse.PayloadJson,
				MechanicsError))
		{
			OutError.Code = MechanicsError.Code.IsEmpty()
				? FString(TEXT("blueprint_node_candidate_query_failed"))
				: MechanicsError.Code;
			OutError.SubjectId = MoveTemp(MechanicsError.SubjectId);
			OutError.Detail = MoveTemp(MechanicsError.Detail);
			return false;
		}
		OutResponse.bReadOnly = true;
		return true;
	}
}
