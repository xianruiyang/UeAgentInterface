#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::Landscape
{
	UEAGENTINTERFACE_API const FString&
	GetLandscapeHeightReadProviderBinding();

	UEAGENTINTERFACE_API bool
	TryCreateLandscapeHeightReadDescriptorRegistry(
		TSharedPtr<
			const UE::UAI::Contract::FUeAgentDescriptorRegistry>&
				OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateLandscapeHeightReadProviderRegistrations(
		TArray<
			UE::UAI::BindingGateway::Query::
				FUeAgentDomainQueryProviderRegistration>&
					OutRegistrations,
		UE::UAI::Contract::FDiagnostic& OutError);
}
