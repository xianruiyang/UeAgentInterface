// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringRegistration.h"

#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringApplyProvider.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDescriptors.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringProjection.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDomainRegistry.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UeAgentAnimationAuthoring {
bool RegisterAuthoring(FString &OutError) {
  if (!RegisterDescriptors(OutError) || !RegisterStateAccessor(OutError) ||
      !RegisterNativeDefaultProvider(OutError) ||
      !RegisterApplyContracts(OutError) || !RegisterExportAuthoring(OutError)) {
    return false;
  }
  static const TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider>
      Provider = MakeApplyProvider();
  UeAgentAuthoringOperationApply::ERegistrationResult Result;
  if (!UeAgentAuthoringOperationApply::FProviderRegistry::Get()
           .RegisterProvider(Provider, Result, OutError)) {
    return false;
  }
  return UeAgentAuthoringDomainRegistry::RegisterGeneratedDomain(DomainId,
                                                                  OutError);
}
} // namespace UeAgentAnimationAuthoring
