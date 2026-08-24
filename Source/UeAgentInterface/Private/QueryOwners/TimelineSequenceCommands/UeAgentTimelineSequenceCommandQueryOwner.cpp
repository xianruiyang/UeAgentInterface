#include "QueryOwners/TimelineSequenceCommands/UeAgentTimelineSequenceCommandQueryOwner.h"

namespace UE::UAI::QueryOwners::TimelineSequenceCommands
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("timeline.sequence.create"),
			TEXT("timeline.sequence.read"),
			TEXT("timeline.sequence.settings.update"),
			TEXT("timeline.sequence.structure.replace"),
			TEXT("timeline.widget_animation.read"),
		};
		return Ids;
	}

	bool IsCommand(const FString& CommandId)
	{
		return GetCommandIds().Contains(CommandId);
	}

	bool TryGetProfiles(
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>& OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const auto& Canonical = Composition::TimelineSpatial::GetTimelineSpatialActivationProfiles();
		for (const FString& CommandId : GetCommandIds())
		{
			const auto* Match = Canonical.FindByPredicate(
				[&CommandId](const auto& Profile) { return Profile.CommandId == CommandId; });
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = TEXT("timeline_sequence_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num();
	}

	bool IsQuery(const FString& CommandId)
	{
		const auto* Match = Composition::TimelineSpatial::GetTimelineSpatialActivationProfiles().FindByPredicate(
			[&CommandId](const auto& Profile) { return Profile.CommandId == CommandId; });
		return Match && Match->OperationKind == TEXT("query");
	}

	const TArray<FString>& GetReadbackFlows()
	{
		static const TArray<FString> Values = {
			TEXT("timeline.sequence.read"),
			TEXT("timeline.widget_animation.read"),
		};
		return Values;
	}
}
