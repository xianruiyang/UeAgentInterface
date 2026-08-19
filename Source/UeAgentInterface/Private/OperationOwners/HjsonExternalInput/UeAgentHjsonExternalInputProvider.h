#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::HjsonExternalInput
{
	class UEAGENTINTERFACE_API IHjsonExternalInputImporterGateway
	{
	public:
		virtual ~IHjsonExternalInputImporterGateway() = default;

		virtual bool Execute(
			const FString& CommandId,
			const FString& CanonicalParamsJson,
			FString& OutPayloadJson,
			HjsonWorkspace::FWorkspaceError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateHjsonExternalInputDomainProvider(
		TSharedRef<IHjsonExternalInputImporterGateway> Gateway,
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionHjsonExternalInputDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);
}
