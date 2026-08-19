// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/UMG/Authoring/UeAgentUmgAuthoringRegistration.h"

#include "Domains/UMG/Authoring/UeAgentUmgAuthoringApplyProvider.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringDescriptors.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringProjection.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDomainRegistry.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UeAgentUmgAuthoring
{
	bool RegisterAuthoring(FString& OutError)
	{
		// Blueprint is registered earlier by its own root; UMG only declares UMG-owned registrations here.
		if (!RegisterDescriptors(OutError)
			|| !RegisterStateAccessor(OutError)
			|| !RegisterNativeDefaultProvider(OutError)
			|| !RegisterApplyContracts(OutError)
			|| !RegisterExportAuthoring(OutError))
		{
			return false;
		}

		static const TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> Provider =
			MakeApplyProvider();
		UeAgentAuthoringOperationApply::ERegistrationResult Result;
		if (!UeAgentAuthoringOperationApply::FProviderRegistry::Get().RegisterProvider(
			Provider, Result, OutError))
		{
			return false;
		}
		return UeAgentAuthoringDomainRegistry::RegisterGeneratedDomain(DomainId, OutError);
	}
}
