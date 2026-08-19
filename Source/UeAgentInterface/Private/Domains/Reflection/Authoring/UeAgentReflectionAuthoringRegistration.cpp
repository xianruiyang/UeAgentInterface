// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProjection.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDomainRegistry.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UeAgentReflectionAuthoring
{
	bool RegisterAuthoring(FString& OutError)
	{
		if (!RegisterDescriptors(OutError)
			|| !RegisterStateAccessor(OutError)
			|| !RegisterNativeDefaultProvider(OutError)
			|| !RegisterApplyContracts(OutError)
			|| !RegisterExportAuthoring(OutError))
		{
			return false;
		}
		static const TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> Provider = MakeApplyProvider();
		UeAgentAuthoringOperationApply::ERegistrationResult Result;
		if (!UeAgentAuthoringOperationApply::FProviderRegistry::Get().RegisterProvider(Provider, Result, OutError))
		{
			return false;
		}
		return UeAgentAuthoringDomainRegistry::RegisterGeneratedDomain(DomainId, OutError);
	}
}
