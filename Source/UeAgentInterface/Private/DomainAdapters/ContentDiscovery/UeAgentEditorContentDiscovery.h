#pragma once

#include "CoreMinimal.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::ContentDiscovery
{
	UEAGENTINTERFACE_API bool TryCreateEditorAssetContentDiscoveryProvider(
		TSharedPtr<UE::UAI::ContentDiscovery::
			IUeAgentContentDiscoveryProvider>& OutProvider,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCreateEditorWorldContentDiscoveryProvider(
		TSharedPtr<UE::UAI::ContentDiscovery::
			IUeAgentContentDiscoveryProvider>& OutProvider,
		FString& OutError);
}
