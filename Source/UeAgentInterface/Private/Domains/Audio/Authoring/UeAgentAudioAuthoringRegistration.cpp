// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Audio/Authoring/UeAgentAudioAuthoringRegistration.h"

#include "Domains/Audio/Authoring/UeAgentAudioAuthoringApplyProvider.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDescriptors.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringProjection.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDomainRegistry.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UeAgentAudioAuthoring {
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
} // namespace UeAgentAudioAuthoring
