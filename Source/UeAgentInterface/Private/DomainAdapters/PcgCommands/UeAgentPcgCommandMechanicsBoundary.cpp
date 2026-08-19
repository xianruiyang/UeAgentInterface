#include "DomainAdapters/PcgCommands/UeAgentPcgCommandMechanicsBoundary.h"

namespace UE::UAI::DomainAdapters::PcgCommands
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("pcg.component.apply"),
			TEXT("pcg.component.world_binding.apply"),
			TEXT("pcg.data_asset.apply"),
			TEXT("pcg.data_asset.create_from_level"),
			TEXT("pcg.generated_spatial_data.read"),
			TEXT("pcg.graph.apply"),
			TEXT("pcg.graph_instance.apply"),
			TEXT("pcg.graph_parameters.apply"),
			TEXT("pcg.partition.spatial_state.read"),
		};
		return Ids;
	}

	bool IsCommand(const FString& CommandId)
	{
		return GetCommandIds().Contains(CommandId);
	}

	bool IsQuery(const FString& CommandId)
	{
		return CommandId == TEXT("pcg.generated_spatial_data.read")
			|| CommandId == TEXT("pcg.partition.spatial_state.read");
	}

	bool TryGetGraphProfiles(
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>&
			OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const auto& Canonical =
			Composition::GraphCompound::GetGraphCompoundActivationProfiles();
		for (const FString& CommandId : GetCommandIds())
		{
			if (IsQuery(CommandId))
			{
				continue;
			}
			const auto* Match = Canonical.FindByPredicate(
				[&CommandId](const auto& Profile)
				{
					return Profile.GetCommandId() == CommandId;
				});
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = TEXT("pcg_command_graph_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == 7;
	}

	bool TryGetTimelineProfiles(
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>&
			OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const auto& Canonical =
			Composition::TimelineSpatial::GetTimelineSpatialActivationProfiles();
		for (const FString& CommandId : GetCommandIds())
		{
			if (!IsQuery(CommandId))
			{
				continue;
			}
			const auto* Match = Canonical.FindByPredicate(
				[&CommandId](const auto& Profile)
				{
					return Profile.CommandId == CommandId;
				});
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = TEXT("pcg_command_timeline_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == 2;
	}
}
