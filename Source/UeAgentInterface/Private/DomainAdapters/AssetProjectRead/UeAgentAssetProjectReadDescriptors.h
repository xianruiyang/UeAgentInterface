#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::AssetProjectRead
{
	UEAGENTINTERFACE_API bool
	TryCreateAssetProjectReadDescriptorRegistry(
		TSharedPtr<
			const UE::UAI::Contract::
				FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError);
}
