#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::EditorProject
{
	/**
	 * Unique mechanics owner for Editor/project read actions. Callers enter
	 * through this owner rather than through legacy command mechanics.
	 */
	class FEditorProjectQueryMechanics final
	{
	public:
		static bool Execute(
			const FString& Action,
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutObservation,
			UE::UAI::Contract::FDiagnostic& OutError);

		static bool ProjectSettingsIndex(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool ProjectSettingsPage(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool TargetPlatformQuery(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool DeviceProfileQuery(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool DeviceProfileExport(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool DeviceProfileValidate(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool DeviceProfileDiff(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool DeviceProfileResolveCVars(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool PlatformConfigExport(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool PlatformConfigValidate(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool PlatformProfileValidatePackaging(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool ProjectSettingsValidate(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool ProjectSettingsDiff(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool ScalabilityExport(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool ScalabilityValidate(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
	};
}
