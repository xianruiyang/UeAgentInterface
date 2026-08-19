#include "QueryOwners/AnimationCurveCommands/UeAgentAnimationCurveCommandQueryOwner.h"

namespace UE::UAI::QueryOwners::AnimationCurveCommands
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("animation.blend_space.asset.create"),
			TEXT("animation.blend_space.definition.apply"),
			TEXT("animation.blend_space.sample"),
			TEXT("animation.montage.create"),
			TEXT("animation.montage.read"),
			TEXT("animation.montage.structure.replace"),
			TEXT("animation.sequence.bone_tracks.replace"),
			TEXT("animation.sequence.curves.update"),
			TEXT("animation.sequence.events.update"),
			TEXT("animation.sequence.metadata.update"),
			TEXT("animation.sequence.pose.sample"),
			TEXT("animation.sequence.read"),
			TEXT("animation.sequence.root_motion.analyze"),
		};
		return Ids;
	}

	bool IsCommand(const FString& CommandId) { return GetCommandIds().Contains(CommandId); }

	bool TryGetGraphProfiles(
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>& OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		static const TArray<FString> GraphCommandIds = {
			TEXT("animation.blend_space.asset.create"),
			TEXT("animation.blend_space.definition.apply"),
		};
		const auto& Canonical = Composition::GraphCompound::GetGraphCompoundActivationProfiles();
		for (const FString& CommandId : GraphCommandIds)
		{
			const auto* Match = Canonical.FindByPredicate(
				[&CommandId](const auto& Profile) { return Profile.GetCommandId() == CommandId; });
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = TEXT("animation_curve_graph_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GraphCommandIds.Num();
	}

	bool TryGetTimelineProfiles(
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>& OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const auto& Canonical = Composition::TimelineSpatial::GetTimelineSpatialActivationProfiles();
		for (const FString& CommandId : GetCommandIds())
		{
			if (CommandId == TEXT("animation.blend_space.asset.create")
				|| CommandId == TEXT("animation.blend_space.definition.apply"))
			{
				continue;
			}
			const auto* Match = Canonical.FindByPredicate(
				[&CommandId](const auto& Profile) { return Profile.CommandId == CommandId; });
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = TEXT("animation_curve_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num() - 2;
	}

	bool IsQuery(const FString& CommandId)
	{
		const auto* GraphMatch = Composition::GraphCompound::GetGraphCompoundActivationProfiles().FindByPredicate(
			[&CommandId](const auto& Profile) { return Profile.GetCommandId() == CommandId; });
		if (GraphMatch)
		{
			return GraphMatch->GetOperationKind() == TEXT("query");
		}
		const auto* Match = Composition::TimelineSpatial::GetTimelineSpatialActivationProfiles().FindByPredicate(
			[&CommandId](const auto& Profile) { return Profile.CommandId == CommandId; });
		return Match && Match->OperationKind == TEXT("query");
	}

	const TArray<FString>& GetReadbackFlows()
	{
		static const TArray<FString> Values = {
			TEXT("animation.sequence.read"),
			TEXT("animation.montage.read"),
			TEXT("animation.blend_space.sample"),
		};
		return Values;
	}
}
