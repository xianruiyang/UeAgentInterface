#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "LocalizationTargetTypes.h"

class ULocalizationSettings;

namespace UE::UAI::DomainAdapters::LocalizationStringTable
{
	UEAGENTINTERFACE_API bool GetLocalizationBackingSettings(const ULocalizationSettings& Object, TArray<FLocalizationTargetSettings>& Values, UE::UAI::Contract::FDiagnostic& Error);
	UEAGENTINTERFACE_API bool SetLocalizationBackingSettings(ULocalizationSettings& Object, const TArray<FLocalizationTargetSettings>& Values, UE::UAI::Contract::FDiagnostic& Error);
	UEAGENTINTERFACE_API bool ReadLocalizationSettingsFile(const FString& Filename, TArray<FLocalizationTargetSettings>& Values, UE::UAI::Contract::FDiagnostic& Error);
	UEAGENTINTERFACE_API bool WriteLocalizationSettingsStage(const TArray<FLocalizationTargetSettings>& Values, const FString& StagingPath, UE::UAI::Contract::FDiagnostic& Error);
	bool EqualLocalizationConfigValues(const TArray<FLocalizationTargetSettings>& Actual, const TArray<FLocalizationTargetSettings>& Expected,
		UE::UAI::Contract::FDiagnostic& Error);
	bool EqualLocalizationNativeValues(const TArray<FLocalizationTargetSettings>& Actual, const TArray<FLocalizationTargetSettings>& Expected,
		UE::UAI::Contract::FDiagnostic& Error);
}
