#include "DomainAdapters/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandMechanicsBoundary.h"

namespace UE::UAI::DomainAdapters::AnimationBlueprintCommands
{
	using Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile;

	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("animation.anim_blueprint.asset.create"),
			TEXT("animation.anim_blueprint.graph.apply"),
			TEXT("animation.anim_blueprint.layer_interface.asset.create"),
		};
		return Ids;
	}

	bool IsQuery(const FString& CommandId)
	{
		return false;
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
			const FUeAgentGraphCompoundActivationProfile* Match =
				Canonical.FindByPredicate(
					[&CommandId](const FUeAgentGraphCompoundActivationProfile& Profile)
					{
						return Profile.GetCommandId() == CommandId;
					});
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = TEXT("animation_blueprint_command_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num();
	}
}

