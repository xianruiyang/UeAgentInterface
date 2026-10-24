// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringRegistration.h"

#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringApplyProvider.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDescriptors.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringProjection.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDomainRegistry.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UeAgentRiggingAuthoring
{
	bool RegisterAuthoring(UE::UAI::Contract::FDiagnostic& OutError)
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
