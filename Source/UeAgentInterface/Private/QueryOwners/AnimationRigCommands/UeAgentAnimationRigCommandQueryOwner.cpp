#include "QueryOwners/AnimationRigCommands/UeAgentAnimationRigCommandQueryOwner.h"

namespace UE::UAI::QueryOwners::AnimationRigCommands
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("animation.control_rig.asset.create"),
			TEXT("animation.control_rig.hierarchy_graph.apply"),
			TEXT("animation.control_rig.shape_library.apply"),
			TEXT("animation.control_rig.shape_library.asset.create"),
			TEXT("animation.ik_retargeter.asset.create"),
			TEXT("animation.ik_retargeter.definition.apply"),
			TEXT("animation.ik_retargeter.settings.apply"),
			TEXT("animation.ik_rig.asset.create"),
			TEXT("animation.ik_rig.definition.apply"),
			TEXT("animation.ik_rig.definition.auto_generate"),
			TEXT("animation.skeleton.compatibility.apply"),
			TEXT("animation.skeleton.definition.apply"),
			TEXT("animation.skeleton.slots.read"),
			TEXT("animation.skeleton.virtual_bone.apply"),
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
		const auto& Canonical = Composition::GraphCompound::GetGraphCompoundActivationProfiles();
		for (const FString& CommandId : GetCommandIds())
		{
			if (CommandId == TEXT("animation.skeleton.slots.read")) continue;
			const auto* Match = Canonical.FindByPredicate(
				[&CommandId](const auto& Profile) { return Profile.GetCommandId() == CommandId; });
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = TEXT("animation_rig_graph_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num() - 1;
	}

	bool TryGetTimelineProfiles(
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>& OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const FString CommandId(TEXT("animation.skeleton.slots.read"));
		const auto& Canonical = Composition::TimelineSpatial::GetTimelineSpatialActivationProfiles();
		const auto* Match = Canonical.FindByPredicate(
			[&CommandId](const auto& Profile) { return Profile.CommandId == CommandId; });
		if (!Match || !Match->IsValid())
		{
			OutError = TEXT("animation_rig_timeline_profile_missing:") + CommandId;
			return false;
		}
		OutProfiles.Add(*Match);
		return true;
	}

	bool IsQuery(const FString& CommandId)
	{
		const auto* GraphMatch = Composition::GraphCompound::GetGraphCompoundActivationProfiles().FindByPredicate(
			[&CommandId](const auto& Profile) { return Profile.GetCommandId() == CommandId; });
		if (GraphMatch) return GraphMatch->GetOperationKind() == TEXT("query");
		const auto* TimelineMatch = Composition::TimelineSpatial::GetTimelineSpatialActivationProfiles().FindByPredicate(
			[&CommandId](const auto& Profile) { return Profile.CommandId == CommandId; });
		return TimelineMatch && TimelineMatch->OperationKind == TEXT("query");
	}

	const TArray<FString>& GetReadbackFlows()
	{
		static const TArray<FString> Values = {
			TEXT("animation.skeleton.slots.read"),
			TEXT("query.command.detail"),
		};
		return Values;
	}
}
