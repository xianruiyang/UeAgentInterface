#include "QueryOwners/SpatialContent/UeAgentSpatialContentQueryOwner.h"

namespace UE::UAI::QueryOwners::SpatialContent
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("mesh.element.select_by_ray"),
			TEXT("mesh.pivot.update"),
			TEXT("mesh.transform.bake"),
			TEXT("world.object_align"),
			TEXT("world.object_snap_to_surface"),
			TEXT("world.object_transform.read"),
			TEXT("world.transform_candidates.evaluate"),
		};
		return Ids;
	}

	bool IsCommand(const FString& CommandId)
	{
		return GetCommandIds().Contains(CommandId);
	}

	bool TryGetProfiles(
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>&
			OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const auto& Canonical = Composition::TimelineSpatial::
			GetTimelineSpatialActivationProfiles();
		for (const FString& CommandId : GetCommandIds())
		{
			const auto* Match = Canonical.FindByPredicate(
				[&CommandId](const auto& Profile)
				{
					return Profile.CommandId == CommandId;
				});
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = TEXT("spatial_content_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num();
	}

	bool IsQuery(const FString& CommandId)
	{
		const auto* Match = Composition::TimelineSpatial::
			GetTimelineSpatialActivationProfiles().FindByPredicate(
				[&CommandId](const auto& Profile)
				{
					return Profile.CommandId == CommandId;
				});
		return Match && Match->OperationKind == TEXT("query");
	}

	const TArray<FString>& GetReadbackFlows()
	{
		static const TArray<FString> Values = {
			TEXT("world.object_transform.read"),
			TEXT("world.transform_candidates.evaluate"),
		};
		return Values;
	}
}
