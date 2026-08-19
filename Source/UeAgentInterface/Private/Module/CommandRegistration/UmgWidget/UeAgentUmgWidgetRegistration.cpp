#include "Module/CommandRegistration/UmgWidget/UeAgentUmgWidgetRegistration.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/UmgWidget/UeAgentUmgWidgetDomainService.h"

namespace UE::UAI::Module::CommandRegistration::UmgWidget
{
	namespace
	{
		const FString ProviderRegistrationId = TEXT("umg-widget-domain-provider");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration
			ProviderRegistration(
				TEXT("umg-widget-domain-provider"),
				&OperationOwners::UmgWidget::TryCreateUmgWidgetDomainProvider);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}
}
