#include "QueryOwners/RigSpatialCommands/UeAgentRigSpatialCommandQueryOwner.h"

namespace UE::UAI::QueryOwners::RigSpatialCommands
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("animation.ik_probe_candidate.apply"),
			TEXT("animation.subject.morph_weights.update"),
			TEXT("animation.subject.pose.update"),
			TEXT("rig.control.bake.animation"),
			TEXT("rig.control.bake.controls"),
			TEXT("rig.ik.definition.update"),
			TEXT("rig.retarget.assets.generate"),
			TEXT("rig.retarget.definition.update"),
			TEXT("rig.retarget.pose.auto_align"),
			TEXT("rig.retarget.pose.update"),
			TEXT("rig.skeleton.preview_mesh.bind"),
			TEXT("rig.skeleton.socket.update"),
		};
		return Ids;
	}

	bool IsCommand(const FString& CommandId) { return GetCommandIds().Contains(CommandId); }

	bool TryGetTimelineProfiles(
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
				OutError = TEXT("rig_spatial_profile_missing:") + CommandId;
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
			TEXT("query.command.detail"),
			TEXT("animation.skeleton.slots.read"),
		};
		return Values;
	}
}
