#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

namespace UE::UAI::DomainAdapters::EditorProject
{
bool IsRegisteredSettingsObject(const UObject& Object);
bool BuildSettingsReadRequests(const UObject& Object,
    TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error);
bool ApplySettingsProperties(UObject& Object, const FString& RequestId,
    const TArray<TSharedPtr<FJsonValue>>& Properties,
    HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome& Outcome);
TSharedRef<HjsonWorkspace::IImportInitializationDomainProvider> CreateSettingsHjsonProvider();
}
