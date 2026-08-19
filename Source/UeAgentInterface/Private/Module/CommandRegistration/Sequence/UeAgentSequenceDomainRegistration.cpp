#include "Module/CommandRegistration/Sequence/UeAgentSequenceDomainRegistration.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/Sequence/UeAgentSequenceDomainService.h"

namespace UE::UAI::Module::CommandRegistration::Sequence
{
	namespace
	{
		const FString ProviderRegistrationId = TEXT("sequence-domain-provider");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration
			Registration(
				TEXT("timeline-sequence-command-migration"),
				TEXT("sequence-domain-provider"),
				&OperationOwners::Sequence::TryCreateSequenceDomainProvider);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}
}
