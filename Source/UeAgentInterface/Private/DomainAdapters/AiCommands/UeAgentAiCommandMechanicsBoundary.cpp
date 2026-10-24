#include "DomainAdapters/AiCommands/UeAgentAiCommandMechanicsBoundary.h"

#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::AiCommands
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("ai.behavior_tree.asset.create"),
			TEXT("ai.behavior_tree.graph.apply"),
			TEXT("ai.blackboard.asset.create"),
			TEXT("ai.blackboard.schema.apply"),
			TEXT("ai.eqs.asset.create"),
			TEXT("ai.eqs.graph.apply"),
			TEXT("ai.smart_object_definition.apply"),
			TEXT("ai.smart_object_definition.asset.create"),
			TEXT("ai.state_tree.asset.create"),
			TEXT("ai.state_tree.hierarchy.apply"),
		};
		return Ids;
	}

	const TArray<FString>& GetPublicCommandIds()
	{
		static const TArray<FString> Ids = []
		{
			TArray<FString> Result = GetCommandIds();
			Result.Remove(TEXT("ai.behavior_tree.graph.apply"));
			Result.Remove(TEXT("ai.blackboard.schema.apply"));
			Result.Remove(TEXT("ai.eqs.graph.apply"));
			Result.Remove(TEXT("ai.smart_object_definition.apply"));
			Result.Remove(TEXT("ai.state_tree.hierarchy.apply"));
			return Result;
		}();
		return Ids;
	}

	bool IsQuery(const FString& CommandId)
	{
		return false;
	}

	bool TryGetActivationProfiles(
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>& OutProfiles,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>* GraphProfiles = nullptr;
		if (!Composition::GraphCompound::TryGetGraphCompoundActivationProfiles(GraphProfiles, OutError)) return false;
		const auto& Canonical = *GraphProfiles;
		for (const FString& CommandId : GetCommandIds())
		{
			const auto* Match = Canonical.FindByPredicate(
				[&CommandId](const auto& Profile)
				{
					return Profile.GetCommandId() == CommandId;
				});
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				if (Match) {
					OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("ai_command_profile_missing"),
						Match->GetValidationDiagnostic(), TEXT("runtime_factory"),
						TEXT("A required AI authoring activation profile failed validation."));
				} else {
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("ai_command_profile_missing"),
						TEXT("No activation profile is registered for a required AI authoring command."), TEXT("runtime_factory"));
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.PropertyPath = TEXT("activation_profile_registered");
				}
				OutError.InternalContext = CommandId;
				OutError.SourceKind = TEXT("native_result");
				if (GetPublicCommandIds().Contains(CommandId))
					OutError.PublicMessage += TEXT(" Command: ") + CommandId + TEXT(".");
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num();
	}
}
