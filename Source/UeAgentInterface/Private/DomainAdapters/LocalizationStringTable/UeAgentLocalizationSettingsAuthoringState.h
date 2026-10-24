#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "LocalizationTargetTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class FJsonObject;
class FProperty;

namespace UE::UAI::DomainAdapters::LocalizationStringTable
{
	UEAGENTINTERFACE_API TArray<FLocalizationTargetSettings> CaptureLocalizationGameSettings();
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> CaptureLocalizationSettingsState(const TArray<FLocalizationTargetSettings>& Settings, UE::UAI::Contract::FDiagnostic* OutError = nullptr);
	bool IsLocalizationSettingProperty(const FProperty* Property);
	bool CaptureLocalizationTargetProperties(const FLocalizationTargetSettings& Settings,
		UeAgentAuthoringState::FStateSnapshot& Snapshot, TSharedPtr<FJsonObject>& Properties, UE::UAI::Contract::FDiagnostic& Error);
	UEAGENTINTERFACE_API bool BuildLocalizationSettingsReadRequests(const TArray<FLocalizationTargetSettings>& Settings,
		TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error);
	UEAGENTINTERFACE_API bool RegisterLocalizationSettingsAuthoringState(UE::UAI::Contract::FDiagnostic& Error);
}
