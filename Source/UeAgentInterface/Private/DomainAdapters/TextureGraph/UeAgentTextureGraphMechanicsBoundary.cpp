#include "DomainAdapters/TextureGraph/UeAgentTextureGraphMechanicsBoundary.h"

namespace UE::UAI::DomainAdapters::TextureGraph
{
	using Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile;

	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("texture_graph.artifact.capture"),
			TEXT("texture_graph.artifact.bake"),
			TEXT("texture_graph.graph.link.mutate"),
			TEXT("texture_graph.graph.node.mutate"),
			TEXT("texture_graph.graph.parameter.mutate"),
			TEXT("texture_graph.graph.validate_evaluate_settle"),
			TEXT("texture_graph.runtime.probe"),
		};
		return Ids;
	}

	bool TryGetActivationProfiles(
		TArray<FUeAgentGraphCompoundActivationProfile>& OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const TArray<FUeAgentGraphCompoundActivationProfile>& Canonical =
			Composition::GraphCompound::GetGraphCompoundActivationProfiles();
		for (const FString& CommandId : GetCommandIds())
		{
			if (CommandId == TEXT("texture_graph.artifact.bake")
				|| CommandId == TEXT("texture_graph.runtime.probe"))
			{
				continue;
			}
			const FUeAgentGraphCompoundActivationProfile* Match =
				Canonical.FindByPredicate(
					[&CommandId](const FUeAgentGraphCompoundActivationProfile& Profile)
					{
						return Profile.GetCommandId() == CommandId;
					});
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = TEXT("texture_graph_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() + 2 == GetCommandIds().Num();
	}
}
