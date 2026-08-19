#include "DomainAdapters/NiagaraCommands/UeAgentNiagaraCommandMechanicsBoundary.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialRoutes.h"

namespace UE::UAI::DomainAdapters::NiagaraCommands
{
	using Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile;

	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("niagara.asset.duplicate"),
			TEXT("niagara.compile_refresh_settle"),
			TEXT("niagara.compound.apply"),
			TEXT("niagara.emitter.asset.create"),
			TEXT("niagara.renderer_event_parameter.apply"),
			TEXT("niagara.stack.issue_fix"),
			TEXT("niagara.stack.reorder"),
			TEXT("niagara.system.asset.create"),
		};
		return Ids;
	}

	bool IsQuery(const FString& CommandId)
	{
		return false;
	}

	bool RequiresHjsonImportAuthority(const FString& CommandId)
	{
		(void)CommandId;
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
				OutError = TEXT("niagara_command_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num();
	}
}
