#include "DomainAdapters/AiCommands/UeAgentAiCommandMechanicsBoundary.h"

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

	bool IsQuery(const FString& CommandId)
	{
		return false;
	}

	bool TryGetActivationProfiles(
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>& OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const auto& Canonical =
			Composition::GraphCompound::GetGraphCompoundActivationProfiles();
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
				OutError = TEXT("ai_command_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num();
	}
}
