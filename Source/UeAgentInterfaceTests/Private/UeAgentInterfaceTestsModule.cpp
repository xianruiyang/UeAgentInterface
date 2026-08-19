// Copyright Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"

class FUeAgentInterfaceTestsModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		DomainOwnerAuthorization =
			UeAgentAuthoringObjectApply::RegisterDomainAuthorizedOwnerPackagePrefix(
				TEXT("/UaiDurableTest_"));

	}

	virtual void ShutdownModule() override
	{
		UeAgentAuthoringObjectApply::UnregisterDomainAuthorizedOwnerPackagePrefix(
			DomainOwnerAuthorization);
	}

private:
	UeAgentAuthoringObjectApply::FDomainOwnerAuthorizationHandle DomainOwnerAuthorization = 0;
};

IMPLEMENT_MODULE(FUeAgentInterfaceTestsModule, UeAgentInterfaceTests)
