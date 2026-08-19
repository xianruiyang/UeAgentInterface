#include "QueryOwners/ViewportGraph/UeAgentViewportGraphQueryOwner.h"

namespace UE::UAI::QueryOwners::ViewportGraph
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("graph.view.read"),
			TEXT("graph.view.update"),
			TEXT("viewport.actor.pick"),
			TEXT("viewport.actor.select"),
			TEXT("viewport.camera.frame"),
			TEXT("viewport.instance.list"),
			TEXT("viewport.pixel_scale.read"),
			TEXT("viewport.ray.deproject"),
			TEXT("viewport.ray.trace"),
			TEXT("viewport.state.read"),
			TEXT("viewport.state.restore"),
			TEXT("viewport.state.update"),
			TEXT("viewport.view_mode.list"),
			TEXT("viewport.warning.list"),
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
				OutError = TEXT("viewport_graph_profile_missing:") + CommandId;
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
			TEXT("viewport.state.read"),
			TEXT("graph.view.read"),
			TEXT("viewport.warning.list"),
		};
		return Values;
	}
}
