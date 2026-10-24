#include "QueryOwners/BlueprintComponent/UeAgentBlueprintComponentQueryOwner.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/Blueprint/Component/UeAgentBlueprintComponentMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

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
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(Request.CanonicalParamsJson, Params, JsonError))
		{
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("blueprint_component_params_invalid"), JsonError, TEXT("candidate_query"));
			OutError.Schema = TEXT("uai.runtime.diagnostic/1");
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.CommandId;
			OutError.SourceKind = TEXT("command_input");
			return false;
		}
		FUeAgentBlueprintComponentCandidateQuery Query;
		if (!Params->TryGetStringField(TEXT("target"), Query.Target))
		{
			const auto Value = Params->TryGetField(TEXT("target"));
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_component_candidate_identity_required"),
				Value.IsValid() ? TEXT("Candidate query target must be a string.") : TEXT("Candidate query target is required."), TEXT("candidate_query"));
			OutError.Schema = TEXT("uai.runtime.diagnostic/1");
			OutError.SubjectId = Request.CommandId;
			OutError.SourceKind = TEXT("command_input");
			OutError.PropertyPath = TEXT("/target");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("string"));
			OutError.ActualValuePresent = Value.IsValid();
			if (Value.IsValid()) OutError.ActualValue = MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Value));
			return false;
		}
		Params->TryGetStringField(TEXT("parent"), Query.Parent);
		Params->TryGetStringField(TEXT("query"), Query.Query);
		FUeAgentBlueprintComponentMechanicsError MechanicsError;
		if (!TryQueryBlueprintComponentCandidates(
				Query,
				OutResponse.PayloadJson,
				MechanicsError))
		{
			OutError = MoveTemp(MechanicsError);
			if (OutError.IsEmpty())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_component_candidate_query_failed"),
					TEXT("The candidate query failed without a diagnostic."), TEXT("candidate_query"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("query_succeeded");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			}
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Query.Target;
			return false;
		}
		OutResponse.bReadOnly = true;
		return true;
	}
}
