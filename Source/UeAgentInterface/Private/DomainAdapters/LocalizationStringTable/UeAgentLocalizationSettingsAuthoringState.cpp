#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationSettingsAuthoringState.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LocalizationSettings.h"
#include "UObject/Class.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::LocalizationStringTable
{
	namespace
	{
		const FString IdentityModel = TEXT("localization_settings.authoring.identity");
		const FString Kind = TEXT("localization_settings");
		const FString AccessorId = TEXT("localization_settings.authoring.state");
		const FString TargetSemantic = TEXT("localization_settings.export.target");
		const FString TargetsSemantic = TEXT("localization_settings.targets");
		UeAgentAuthoringState::EReadOutcome Read(const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& Value, UE::UAI::Contract::FDiagnostic& Error)
		{
			using namespace UeAgentAuthoringState;
			Value = FStateValue();
			Value.TargetIdentity = Request.TargetIdentity;
			Value.Type.Kind = TEXT("object");
			Value.Provenance = AccessorId;
			if (Request.SemanticId == TargetSemantic)
			{
				const auto Target = MakeShared<FJsonObject>();
				Target->SetStringField(TEXT("asset_path"), Request.RootOwnerPath);
				Target->SetStringField(TEXT("asset_kind"), Kind);
				Value.Value = MakeShared<FJsonValueObject>(Target);
			}
			else if (Request.SemanticId == TargetsSemantic && Request.RootData)
			{
				const auto State = CaptureLocalizationSettingsState(
					*static_cast<const TArray<FLocalizationTargetSettings>*>(Request.RootData), &Error);
				if (!State) return EReadOutcome::Failed;
				Value.Value = MakeShared<FJsonValueObject>(State);
				Value.bEditable = true;
			}
			else
			{
				const bool bMissingState = Request.SemanticId == TargetsSemantic;
				Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("localization_settings_capture_invalid"),
					bMissingState ? TEXT("The native localization target settings required for capture are unavailable.")
						: TEXT("The localization state accessor does not support the requested semantic."), TEXT("state_capture"));
				Error.SourceKind = TEXT("native_result");
				Error.TargetPath = Request.RootOwnerPath;
				Error.PropertyPath = bMissingState ? TEXT("/root_data_present") : TEXT("/semantic_id");
				Error.ActualValue = bMissingState ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Request.SemanticId));
				Error.ExpectedValue = bMissingState ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TargetSemantic + TEXT(" or ") + TargetsSemantic));
				Error.ActualValuePresent = true;
				Error.ExpectedValuePresent = true;
				return EReadOutcome::Failed;
			}
			return EReadOutcome::Resolved;
		}
	}

	TArray<FLocalizationTargetSettings> CaptureLocalizationGameSettings()
	{
		TArray<FLocalizationTargetSettings> Settings;
		if (const auto* Set = ULocalizationSettings::GetGameTargetSet())
			for (const auto& Target : Set->TargetObjects) if (Target) Settings.Add(Target->Settings);
		return Settings;
	}

	bool IsLocalizationSettingProperty(const FProperty* Property)
	{
		static const TSet<FName> SeparatelyOwned = {TEXT("Name"), TEXT("Guid"), TEXT("NativeCultureIndex"), TEXT("SupportedCulturesStatistics")};
		return Property && !SeparatelyOwned.Contains(Property->GetFName());
	}

	bool CaptureLocalizationTargetProperties(const FLocalizationTargetSettings& Settings,
		UeAgentAuthoringState::FStateSnapshot& Snapshot, TSharedPtr<FJsonObject>& Properties, UE::UAI::Contract::FDiagnostic& Error)
	{
		UeAgentDetailsAuthoringService::FCapturePolicy Policy;
		Policy.IncludeProperty = [](const FProperty* Property, const void*, const UObject*) { return IsLocalizationSettingProperty(Property); };
		return UeAgentDetailsAuthoringService::CaptureStructProperties(FLocalizationTargetSettings::StaticStruct(), &Settings,
			GetDefault<ULocalizationSettings>()->GetPathName(), nullptr, Policy, Snapshot, Properties, Error)
			&& UeAgentDetailsAuthoringService::ProjectReadableProperties(Snapshot, Properties, Error);
	}

	TSharedPtr<FJsonObject> CaptureLocalizationSettingsState(const TArray<FLocalizationTargetSettings>& Settings, UE::UAI::Contract::FDiagnostic* OutError)
	{
		const auto Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Targets;
		for (const auto& SettingsItem : Settings)
		{
			const auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), SettingsItem.Name);
			TArray<TSharedPtr<FJsonValue>> Cultures;
			for (const auto& Culture : SettingsItem.SupportedCulturesStatistics)
				Cultures.Add(MakeShared<FJsonValueString>(Culture.CultureName));
			Item->SetArrayField(TEXT("cultures"), Cultures);
			if (SettingsItem.SupportedCulturesStatistics.IsValidIndex(SettingsItem.NativeCultureIndex))
				Item->SetStringField(TEXT("native_culture"), SettingsItem.SupportedCulturesStatistics[SettingsItem.NativeCultureIndex].CultureName);
			FString Native;
			FLocalizationTargetSettings::StaticStruct()->ExportText(Native, &SettingsItem, nullptr, nullptr, PPF_None, nullptr);
			Item->SetStringField(TEXT("native_settings"), Native);
			UeAgentAuthoringState::FStateSnapshot Snapshot;
			TSharedPtr<FJsonObject> Properties;
			UE::UAI::Contract::FDiagnostic Error;
			if (!CaptureLocalizationTargetProperties(SettingsItem, Snapshot, Properties, Error))
			{
				if (OutError) *OutError = Error;
				return nullptr;
			}
			Item->SetObjectField(TEXT("settings"), Properties);
			Targets.Add(MakeShared<FJsonValueObject>(Item));
		}
		Targets.Sort([](const auto& A, const auto& B) {
			return A->AsObject()->GetStringField(TEXT("name")).Compare(B->AsObject()->GetStringField(TEXT("name")), ESearchCase::CaseSensitive) < 0; });
		Result->SetArrayField(TEXT("targets"), Targets);
		return Result;
	}

	bool RegisterLocalizationSettingsAuthoringState(UE::UAI::Contract::FDiagnostic& Error)
	{
		UeAgentAuthoringIdentity::FModelDescriptor Identity;
		Identity.ModelId = IdentityModel;
		Identity.TargetKind = Kind;
		Identity.Selectors = {{TEXT("asset_path"), 100, false}};
		Identity.KeyPatterns = {{TEXT("asset"), 0, {TEXT("asset_path")}}};
		UeAgentAuthoringIdentity::ERegistrationResult IdentityResult;
		if (!UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(Identity, IdentityResult, Error)) return false;
		using namespace UeAgentAuthoringSemanticRegistry;
		FDomainDescriptor Domain;
		Domain.DomainId = TEXT("localization_settings.authoring");
		Domain.TargetKinds = {Kind};
		Domain.MutationProviderId = TEXT("localization_settings.authoring.apply");
		Domain.CapabilityTags = {TEXT("config"), TEXT("editor_authoring")};
		ERegistrationResult Registration;
		if (!FRegistry::Get().RegisterDomain(Domain, Registration, Error)) return false;
		for (const auto& Semantic : {TargetSemantic, TargetsSemantic})
		{
			FSemanticDescriptor Descriptor;
			Descriptor.SemanticId = Semantic;
			Descriptor.DomainId = Domain.DomainId;
			Descriptor.TargetKind = Kind;
			Descriptor.ValueType = TEXT("object");
			Descriptor.OwnerModel = TEXT("localization_settings_owner");
			Descriptor.IdentityModel = IdentityModel;
			Descriptor.StateAccessor = AccessorId;
			Descriptor.WriteCapability = Semantic == TargetsSemantic ? EWriteCapability::DomainMutation : EWriteCapability::ReadOnly;
			Descriptor.ConstraintSource = TEXT("localization_settings.native");
			Descriptor.AvailabilitySource = AccessorId;
			Descriptor.Comparator = TEXT("uai.compare.canonical_value");
			Descriptor.CapabilityTags = Domain.CapabilityTags;
			if (!FRegistry::Get().RegisterSemantic(Descriptor, Registration, Error)) return false;
		}
		UeAgentAuthoringState::FStateAccessorDescriptor Accessor;
		Accessor.AccessorId = AccessorId;
		Accessor.TargetKinds = {Kind};
		Accessor.CapabilityTags = Domain.CapabilityTags;
		UeAgentAuthoringState::ERegistrationResult StateResult;
		return UeAgentAuthoringState::RegisterBuiltInStateAccessors(UeAgentAuthoringState::FStateAccessorRegistry::Get(), Error)
			&& UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(Accessor, &Read, StateResult, Error);
	}

	bool BuildLocalizationSettingsReadRequests(const TArray<FLocalizationTargetSettings>& Settings,
		TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error)
	{
		Requests.Reset();
		const FString Path = GetDefault<ULocalizationSettings>()->GetPathName();
		for (const auto& Semantic : {TargetSemantic, TargetsSemantic})
		{
			UeAgentAuthoringState::FReadRequest Request;
			Request.SemanticId = Semantic;
			Request.RootOwnerPath = Path;
			Request.RootData = &Settings;
			if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(IdentityModel, Kind,
				{{TEXT("asset_path"), Path}}, Request.TargetIdentity, Error)) return false;
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
			Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			Requests.Add(MoveTemp(Request));
		}
		return true;
	}
}
