// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringApplyProvider.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringProjection.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDomainRegistry.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UeAgentBlueprintAuthoring
{
	bool RegisterAuthoring(FString& OutError)
	{
		if (!UE::UAI::HjsonWorkspace::
				RegisterGraphTreeTargetCompositionContracts(OutError)
			|| !RegisterDescriptors(OutError)
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
