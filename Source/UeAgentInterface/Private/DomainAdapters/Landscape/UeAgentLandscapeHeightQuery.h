#pragma once

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
		FString& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateLandscapeHeightReadProviderRegistrations(
		TArray<
			UE::UAI::BindingGateway::Query::
				FUeAgentDomainQueryProviderRegistration>&
					OutRegistrations,
		FString& OutError);
}
