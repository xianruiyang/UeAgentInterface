#include "DomainAdapters/UmgCommands/UeAgentUmgCommandMechanicsBoundary.h"

namespace UE::UAI::DomainAdapters::UmgCommands
{
	using Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile;

	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("umg.asset.create"),
			TEXT("umg.widget.events.discover"),
		};
		return Ids;
	}

	bool IsQuery(const FString& CommandId)
	{
		return CommandId == TEXT("umg.widget.events.discover");
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
				OutError = TEXT("umg_command_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num();
	}
}
