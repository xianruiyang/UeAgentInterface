#include "DomainAdapters/EditorProject/UeAgentEditorProjectQueryMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::EditorProject
{
	bool FEditorProjectQueryMechanics::Execute(
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutObservation,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (Action == TEXT("project_settings.index")) return ProjectSettingsIndex(Params, OutObservation, OutError);
		if (Action == TEXT("project_settings.page")) return ProjectSettingsPage(Params, OutObservation, OutError);
		if (Action == TEXT("device_profile.query")) return DeviceProfileQuery(Params, OutObservation, OutError);
		if (Action == TEXT("device_profile.export")) return DeviceProfileExport(Params, OutObservation, OutError);
		if (Action == TEXT("device_profile.validate")) return DeviceProfileValidate(Params, OutObservation, OutError);
		if (Action == TEXT("device_profile.diff")) return DeviceProfileDiff(Params, OutObservation, OutError);
		if (Action == TEXT("device_profile.resolve_cvars")) return DeviceProfileResolveCVars(Params, OutObservation, OutError);
		if (Action == TEXT("platform_config.export")) return PlatformConfigExport(Params, OutObservation, OutError);
		if (Action == TEXT("platform_config.validate")) return PlatformConfigValidate(Params, OutObservation, OutError);
		if (Action == TEXT("platform_profile.validate_packaging")) return PlatformProfileValidatePackaging(Params, OutObservation, OutError);
		if (Action == TEXT("project_settings.validate")) return ProjectSettingsValidate(Params, OutObservation, OutError);
		if (Action == TEXT("project_settings.diff")) return ProjectSettingsDiff(Params, OutObservation, OutError);
		if (Action == TEXT("scalability.export")) return ScalabilityExport(Params, OutObservation, OutError);
		if (Action == TEXT("scalability.validate")) return ScalabilityValidate(Params, OutObservation, OutError);
		if (Action == TEXT("target_platform.query")) return TargetPlatformQuery(Params, OutObservation, OutError);
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("editor_project_query_action_unknown"),
			TEXT("The editor project query action is not supported."), TEXT("query"));
		OutError.SourceKind = TEXT("command_input");
		OutError.PropertyPath = TEXT("/action");
		OutError.CandidateName = Action;
		OutError.ActualValue = MakeShared<FJsonValueString>(Action);
		TArray<TSharedPtr<FJsonValue>> Choices;
		for (const TCHAR* Known : {TEXT("project_settings.index"), TEXT("project_settings.page"),
			TEXT("device_profile.query"), TEXT("device_profile.export"), TEXT("device_profile.validate"),
			TEXT("device_profile.diff"), TEXT("device_profile.resolve_cvars"), TEXT("platform_config.export"),
			TEXT("platform_config.validate"), TEXT("platform_profile.validate_packaging"),
			TEXT("project_settings.validate"), TEXT("project_settings.diff"), TEXT("scalability.export"),
			TEXT("scalability.validate"), TEXT("target_platform.query")})
			Choices.Add(MakeShared<FJsonValueString>(Known));
		OutError.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Choices));
		OutError.ActualValuePresent = true;
		OutError.ExpectedValuePresent = true;
		return false;
	}
}
