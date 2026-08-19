#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::WorldRead
{
	UEAGENTINTERFACE_API bool TryCreateWorldReadDescriptorRegistry(
		TSharedPtr<
			const UE::UAI::Contract::
				FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError);
}
