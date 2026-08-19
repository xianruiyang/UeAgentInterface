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
		FString Target;
		FString ComponentClass;
		FString RequestedName;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| !Params->TryGetStringField(
				TEXT("component_class"), ComponentClass)
			|| !Params->TryGetStringField(
				TEXT("requested_name"), RequestedName))
		{
			OutError.Code = TEXT("blueprint_component_candidate_identity_required");
			OutError.SubjectId = Request.CommandId;
			return false;
		}
		FUeAgentBlueprintComponentMechanicsError MechanicsError;
		if (!TryQueryBlueprintComponentCandidates(
				Target,
				ComponentClass,
				RequestedName,
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
