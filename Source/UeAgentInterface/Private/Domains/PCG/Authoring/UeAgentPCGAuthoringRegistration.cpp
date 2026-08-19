// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/PCG/Authoring/UeAgentPCGAuthoringRegistration.h"

#include "Domains/PCG/Authoring/UeAgentPCGAuthoringApplyProvider.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringDescriptors.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringProjection.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDomainRegistry.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UeAgentPCGAuthoring
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
		if (!UeAgentAuthoringOperationApply::FProviderRegistry::Get().RegisterProvider(
			Provider, Result, OutError))
		{
			return false;
		}
		return UeAgentAuthoringDomainRegistry::RegisterGeneratedDomain(DomainId, OutError);
	}
}
