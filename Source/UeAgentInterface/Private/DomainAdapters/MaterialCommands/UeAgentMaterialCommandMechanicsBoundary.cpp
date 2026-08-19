#include "DomainAdapters/MaterialCommands/UeAgentMaterialCommandMechanicsBoundary.h"

namespace UE::UAI::DomainAdapters::MaterialCommands
{
	using Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile;

	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("material.asset.create"),
			TEXT("material.function.graph.apply"),
			TEXT("material.graph.apply"),
			TEXT("material_instance.asset.create"),
			TEXT("material_instance.parameters.apply"),
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
				OutError = TEXT("material_command_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num();
	}
}


