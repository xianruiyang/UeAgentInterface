#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

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
		UE::UAI::Contract::FDiagnostic& OutError);
}
