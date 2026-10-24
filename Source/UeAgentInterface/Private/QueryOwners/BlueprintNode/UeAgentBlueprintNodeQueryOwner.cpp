#include "QueryOwners/BlueprintNode/UeAgentBlueprintNodeQueryOwner.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

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
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(Request.CanonicalParamsJson, Params, JsonError))
		{
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("blueprint_node_params_invalid"), JsonError, TEXT("candidate_query"));
			OutError.Schema = TEXT("uai.runtime.diagnostic/1");
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.CommandId;
			OutError.SourceKind = TEXT("command_input");
			return false;
		}
		FUeAgentBlueprintNodeCandidateQuery Query;
		for (const auto& Field : {TPair<FString, FString*>(TEXT("target"), &Query.Target),
			TPair<FString, FString*>(TEXT("graph_name"), &Query.GraphName)})
		{
			if (!Params->TryGetStringField(Field.Key, *Field.Value))
			{
				const auto Value = Params->TryGetField(Field.Key);
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_node_candidate_identity_required"),
					FString::Printf(TEXT("Candidate query field '%s' %s."), *Field.Key,
						Value.IsValid() ? TEXT("must be a string") : TEXT("is required")), TEXT("candidate_query"));
				OutError.Schema = TEXT("uai.runtime.diagnostic/1");
				OutError.SubjectId = Request.CommandId;
				OutError.SourceKind = TEXT("command_input");
				OutError.PropertyPath = TEXT("/") + Field.Key;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("string"));
				OutError.ActualValuePresent = Value.IsValid();
				if (Value.IsValid()) OutError.ActualValue = MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Value));
				return false;
			}
		}
		Params->TryGetStringField(TEXT("query"), Query.Query);
		UE::UAI::ContentDiscovery::FContentDiscoveryError TargetError;
		if (!UE::UAI::DomainAdapters::ContentDiscovery::TryResolveEditorContentReference(Query.Target, TEXT("asset"), TargetError))
		{
			OutError = MoveTemp(TargetError);
			return false;
		}
		Params->TryGetStringField(TEXT("node_name"), Query.NodeName);
		Params->TryGetStringField(TEXT("pin_name"), Query.PinName);
		double Occurrence = 0.0;
		if (Params->TryGetNumberField(TEXT("node_occurrence"), Occurrence))
		{
			Query.NodeOccurrence = FMath::RoundToInt(Occurrence);
		}
		if (Params->TryGetNumberField(TEXT("pin_occurrence"), Occurrence))
		{
			Query.PinOccurrence = FMath::RoundToInt(Occurrence);
		}
		FUeAgentBlueprintNodeMechanicsError MechanicsError;
		if (!TryQueryBlueprintNodeCandidates(
				Query,
				OutResponse.PayloadJson,
				MechanicsError))
		{
			OutError = MoveTemp(MechanicsError);
			if (OutError.IsEmpty())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_node_candidate_query_failed"),
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
