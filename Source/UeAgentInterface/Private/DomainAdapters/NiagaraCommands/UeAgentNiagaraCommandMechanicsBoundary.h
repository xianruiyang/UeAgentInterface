#pragma once

#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "CoreMinimal.h"

namespace UE::UAI::DomainAdapters::NiagaraCommands
{
	UEAGENTINTERFACE_API const TArray<FString>& GetCommandIds();
	UEAGENTINTERFACE_API bool IsQuery(const FString& CommandId);
	UEAGENTINTERFACE_API bool RequiresHjsonImportAuthority(
		const FString& CommandId);
	UEAGENTINTERFACE_API bool TryGetActivationProfiles(
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>&
			OutProfiles,
		FString& OutError);
}
