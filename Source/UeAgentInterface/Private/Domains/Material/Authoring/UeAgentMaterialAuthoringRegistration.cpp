// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialAuthoringRegistration.h"

#include "Domains/Material/Authoring/UeAgentMaterialAuthoringApplyProvider.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringDescriptors.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringProjection.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringState.h"
#include "Domains/Material/Authoring/UeAgentMaterialGraphMutationApplyProvider.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDomainRegistry.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UeAgentMaterialAuthoring
{
	bool RegisterAuthoring(FString& OutError)
	{
		if (!RegisterDescriptors(OutError)
			|| !RegisterStateAccessor(OutError)
			|| !RegisterNativeDefaultProvider(OutError)
			|| !RegisterApplyContracts(OutError)
			|| !RegisterGraphMutationApplyContracts(OutError)
			|| !RegisterExportAuthoring(OutError))
		{
			return false;
		}
		static const TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> AssetProvider = MakeApplyProvider();
		static const TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> GraphProvider = MakeGraphMutationApplyProvider();
		UeAgentAuthoringOperationApply::ERegistrationResult Result;
		if (!UeAgentAuthoringOperationApply::FProviderRegistry::Get().RegisterProvider(
			AssetProvider, Result, OutError)) return false;
		if (!UeAgentAuthoringOperationApply::FProviderRegistry::Get().RegisterProvider(
			GraphProvider, Result, OutError)) return false;
		return UeAgentAuthoringDomainRegistry::RegisterGeneratedDomain(DomainId, OutError);
	}
}
