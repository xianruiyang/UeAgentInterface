#include "DomainAdapters/AudioCommands/UeAgentAudioCommandMechanicsBoundary.h"

namespace UE::UAI::DomainAdapters::AudioCommands
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("audio_media.asset.create"),
			TEXT("metasound.graph.compile_settle"),
			TEXT("metasound.graph.default.mutate"),
			TEXT("metasound.graph.interface.mutate"),
			TEXT("metasound.graph.link.mutate"),
			TEXT("metasound.graph.node.mutate"),
			TEXT("sound_cue.graph.compile_settle"),
			TEXT("sound_cue.graph.node.mutate"),
			TEXT("sound_cue.graph.relation.mutate"),
			TEXT("sound_cue.graph.root.mutate"),
		};
		return Ids;
	}
	bool IsQuery(const FString& CommandId) { return false; }
	bool TryGetActivationProfiles(
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>& OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const auto& Canonical = Composition::GraphCompound::GetGraphCompoundActivationProfiles();
		for (const FString& CommandId : GetCommandIds())
		{
			const auto* Match = Canonical.FindByPredicate(
				[&CommandId](const auto& Profile) { return Profile.GetCommandId() == CommandId; });
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = TEXT("audio_command_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num();
	}
}
