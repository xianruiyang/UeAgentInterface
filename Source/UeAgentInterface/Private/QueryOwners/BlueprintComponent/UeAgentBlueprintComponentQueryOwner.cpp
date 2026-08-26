#include "QueryOwners/BlueprintComponent/UeAgentBlueprintComponentQueryOwner.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/Blueprint/Component/UeAgentBlueprintComponentMechanics.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::QueryOwners::BlueprintComponent
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::DomainAdapters::Blueprint::Component;
	using namespace UE::UAI::OperationOwners::BlueprintComponent;

	bool TryExecuteCandidateQuery(
		const FUeAgentBlueprintComponentCommandRequest& Request,
		FUeAgentBlueprintComponentCommandResponse& OutResponse,
		FUeAgentBlueprintComponentCommandError& OutError)
	{
		OutResponse = {};
		OutError = {};
		TSharedPtr<FJsonObject> Params;
		FString JsonError;
		if (!Json::TryParseObject(Request.CanonicalParamsJson, Params, JsonError))
		{
			OutError.Code = TEXT("blueprint_component_params_invalid");
			OutError.SubjectId = Request.CommandId;
			OutError.Detail = MoveTemp(JsonError);
			return false;
		}
		FUeAgentBlueprintComponentCandidateQuery Query;
		if (!Params->TryGetStringField(TEXT("target"), Query.Target))
		{
			OutError.Code = TEXT("blueprint_component_candidate_identity_required");
			OutError.SubjectId = Request.CommandId;
			return false;
		}
		Params->TryGetStringField(TEXT("parent"), Query.Parent);
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
		FUeAgentBlueprintComponentMechanicsError MechanicsError;
		if (!TryQueryBlueprintComponentCandidates(
				Query,
				OutResponse.PayloadJson,
				MechanicsError))
		{
			OutError.Code = MechanicsError.Code.IsEmpty()
				? FString(TEXT("blueprint_component_candidate_query_failed"))
				: MechanicsError.Code;
			OutError.SubjectId = MoveTemp(MechanicsError.SubjectId);
			OutError.Detail = MoveTemp(MechanicsError.Detail);
			return false;
		}
		OutResponse.bReadOnly = true;
		return true;
	}
}
