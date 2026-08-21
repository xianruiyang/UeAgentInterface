#pragma once

#include "CoreMinimal.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Module::CommandRegistration::UnifiedContentSearch
{
	UEAGENTINTERFACE_API bool TryCreateProductionUnifiedContentSearchDomain(
		TSharedPtr<const UE::UAI::DomainCapability::
			FDomainCapabilityContribution>& OutContribution,
		FString& OutError);
}
