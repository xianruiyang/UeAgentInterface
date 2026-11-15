#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationSettingsConfig.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/EditorProject/UeAgentSettingsConfig.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"

#include "LocalizationSettings.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::LocalizationStringTable
{
	namespace
	{
		UE::UAI::Contract::FDiagnostic ConfigFailure(const FString& Code, const FString& Reason,
			const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("verification"));
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = GetDefault<ULocalizationSettings>()->GetPathName();
			Error.PropertyPath = Field;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			return Error;
		}

		FArrayProperty* GameProperty(UE::UAI::Contract::FDiagnostic& Error)
		{
			auto* Field = FindFProperty<FProperty>(ULocalizationSettings::StaticClass(), TEXT("GameTargetsSettings"));
			auto* Property = CastField<FArrayProperty>(Field);
			const auto* Inner = Property ? CastField<FStructProperty>(Property->Inner) : nullptr;
			if (!Property || !Property->HasAnyPropertyFlags(CPF_Config) || !Inner || Inner->Struct != FLocalizationTargetSettings::StaticStruct())
			{
				FString Reason, Actual, Expected;
				if (!Property)
				{
					Reason = TEXT("The localization backing property is missing or is not an array.");
					Actual = Field ? Field->GetCPPType() : TEXT("missing");
					Expected = TEXT("array property");
				}
				else if (!Property->HasAnyPropertyFlags(CPF_Config))
				{
					Reason = TEXT("The localization backing array is not marked as a config property.");
					Actual = TEXT("Config flag absent"); Expected = TEXT("Config flag present");
				}
				else
				{
					Reason = TEXT("The localization backing array has an unexpected element type.");
					Actual = Inner && Inner->Struct ? Inner->Struct->GetPathName()
						: Property->Inner ? Property->Inner->GetCPPType() : TEXT("missing");
					Expected = FLocalizationTargetSettings::StaticStruct()->GetPathName();
				}
				Error = ConfigFailure(TEXT("localization_config_property_invalid"), Reason, TEXT("GameTargetsSettings"),
					MakeShared<FJsonValueString>(Actual), MakeShared<FJsonValueString>(Expected));
				Error.Stage = TEXT("native_access");
				return nullptr;
			}
			return Property;
		}
	}

	bool GetLocalizationBackingSettings(const ULocalizationSettings& Object, TArray<FLocalizationTargetSettings>& Values, UE::UAI::Contract::FDiagnostic& Error)
	{
		const auto* Property = GameProperty(Error);
		if (!Property) return false;
		Values = *Property->ContainerPtrToValuePtr<TArray<FLocalizationTargetSettings>>(&Object);
		return true;
	}

	bool SetLocalizationBackingSettings(ULocalizationSettings& Object, const TArray<FLocalizationTargetSettings>& Values, UE::UAI::Contract::FDiagnostic& Error)
	{
		const auto* Property = GameProperty(Error);
		if (!Property) return false;
		*Property->ContainerPtrToValuePtr<TArray<FLocalizationTargetSettings>>(&Object) = Values;
		return true;
	}

	bool ReadLocalizationSettingsFile(const FString& Filename, TArray<FLocalizationTargetSettings>& Values, UE::UAI::Contract::FDiagnostic& Error)
	{
		Values.Reset();
		// Verify the published file with the same native inheritance semantics as
		// ordinary settings. Reading only this file misses inherited array entries.
		const auto* Owner = GetDefault<ULocalizationSettings>();
		TStrongObjectPtr<ULocalizationSettings> Shadow(NewObject<ULocalizationSettings>(GetTransientPackage()));
		return EditorProject::ReadSettingsConfig(*Shadow, *Owner, Filename, Error)
			&& GetLocalizationBackingSettings(*Shadow, Values, Error);
	}

	namespace
	{
		void LocateComparedTarget(UE::UAI::Contract::FDiagnostic& Error, const FString& Prefix)
		{
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = GetDefault<ULocalizationSettings>()->GetPathName();
			Error.PropertyPath = Prefix + (Error.PropertyPath.IsEmpty() ? FString() : TEXT("/") + Error.PropertyPath);
			for (auto& Child : Error.Diagnostics) LocateComparedTarget(Child, Prefix);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Error.PublicCause);
				LocateComparedTarget(*Cause, Prefix);
				Error.PublicCause = MoveTemp(Cause);
			}
		}

		UE::UAI::Contract::FDiagnostic TargetDifference(const FLocalizationTargetSettings& Actual,
			const FLocalizationTargetSettings& Expected, int32 Index, const FString& Code)
		{
			const FString Prefix = TEXT("/targets/") + FString::FromInt(Index);
			auto Difference = [&](const FString& Field, const FString& Reason,
				const TSharedPtr<FJsonValue>& ActualValue, const TSharedPtr<FJsonValue>& ExpectedValue)
			{
				return ConfigFailure(Code, Reason, Prefix + TEXT("/") + Field, ActualValue, ExpectedValue);
			};
			if (Actual.Name != Expected.Name)
				return Difference(TEXT("Name"), TEXT("The localization target name differs from the expected state."),
					MakeShared<FJsonValueString>(Actual.Name), MakeShared<FJsonValueString>(Expected.Name));
			if (Actual.Guid != Expected.Guid)
			{
				auto Error = Difference(TEXT("Guid"), TEXT("The native localization target identity differs from the expected state."),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				Error.ActualEvidence = MakeShared<FJsonValueString>(Actual.Guid.ToString());
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.Guid.ToString());
				return Error;
			}
			if (Actual.ConflictStatus != Expected.ConflictStatus)
				return Difference(TEXT("ConflictStatus"), TEXT("The localization target conflict status differs from the expected state."),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Actual.ConflictStatus)),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Expected.ConflictStatus)));
			if (Actual.TargetDependencies.Num() != Expected.TargetDependencies.Num())
				return Difference(TEXT("TargetDependencies/count"), TEXT("The number of localization target dependencies differs."),
					MakeShared<FJsonValueNumber>(Actual.TargetDependencies.Num()), MakeShared<FJsonValueNumber>(Expected.TargetDependencies.Num()));
			for (int32 DependencyIndex = 0; DependencyIndex < Actual.TargetDependencies.Num(); ++DependencyIndex)
				if (Actual.TargetDependencies[DependencyIndex] != Expected.TargetDependencies[DependencyIndex])
				{
					auto Error = Difference(TEXT("TargetDependencies/") + FString::FromInt(DependencyIndex),
						TEXT("The native localization target dependency identity differs from the expected state."),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Error.ActualEvidence = MakeShared<FJsonValueString>(Actual.TargetDependencies[DependencyIndex].ToString());
					Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.TargetDependencies[DependencyIndex].ToString());
					return Error;
				}
			if (Actual.NativeCultureIndex != Expected.NativeCultureIndex)
				return Difference(TEXT("NativeCultureIndex"), TEXT("The native localization culture index differs from the expected state."),
					MakeShared<FJsonValueNumber>(Actual.NativeCultureIndex), MakeShared<FJsonValueNumber>(Expected.NativeCultureIndex));
			if (Actual.SupportedCulturesStatistics.Num() != Expected.SupportedCulturesStatistics.Num())
				return Difference(TEXT("SupportedCulturesStatistics/count"), TEXT("The number of supported localization cultures differs."),
					MakeShared<FJsonValueNumber>(Actual.SupportedCulturesStatistics.Num()), MakeShared<FJsonValueNumber>(Expected.SupportedCulturesStatistics.Num()));
			for (int32 CultureIndex = 0; CultureIndex < Actual.SupportedCulturesStatistics.Num(); ++CultureIndex)
			{
				const auto& ActualCulture = Actual.SupportedCulturesStatistics[CultureIndex];
				const auto& ExpectedCulture = Expected.SupportedCulturesStatistics[CultureIndex];
				const FString CulturePath = TEXT("SupportedCulturesStatistics/") + FString::FromInt(CultureIndex);
				if (ActualCulture.CultureName != ExpectedCulture.CultureName)
					return Difference(CulturePath + TEXT("/CultureName"), TEXT("The supported localization culture differs from the expected state."),
						MakeShared<FJsonValueString>(ActualCulture.CultureName), MakeShared<FJsonValueString>(ExpectedCulture.CultureName));
				if (ActualCulture.WordCount != ExpectedCulture.WordCount)
					return Difference(CulturePath + TEXT("/WordCount"), TEXT("The localization culture word count differs from the expected state."),
						MakeShared<FJsonValueNumber>(ActualCulture.WordCount), MakeShared<FJsonValueNumber>(ExpectedCulture.WordCount));
			}

			// These native identity fields are diagnosed above; they never enter the public snapshot.
			UeAgentDetailsAuthoringService::FCapturePolicy Policy;
			Policy.ExcludedProperties = {TEXT("Guid"), TEXT("TargetDependencies")};
			UeAgentAuthoringState::FStateSnapshot ExpectedSnapshot, ActualSnapshot;
			UE::UAI::Contract::FDiagnostic CaptureError;
			const FString Owner = GetDefault<ULocalizationSettings>()->GetPathName();
			if (UeAgentDetailsAuthoringService::CaptureStructSnapshot(FLocalizationTargetSettings::StaticStruct(),
				&Expected, Owner, nullptr, Policy, ExpectedSnapshot, CaptureError)
				&& UeAgentDetailsAuthoringService::CaptureStructSnapshot(FLocalizationTargetSettings::StaticStruct(),
					&Actual, Owner, nullptr, Policy, ActualSnapshot, CaptureError))
			{
				UeAgentAuthoringSemanticCompare::FComparisonReport Report;
				const bool bCompared = UeAgentAuthoringSemanticCompare::CompareSnapshots(ExpectedSnapshot, ActualSnapshot, {}, Report);
				if ((!bCompared || !Report.bEquivalent) && Report.Diagnostic.IsValid())
				{
					const auto* FirstDifference = bCompared ? Report.Entries.FindByPredicate([](const auto& Entry) {
						return !Entry.bEquivalent && Entry.Diagnostic.IsValid();
					}) : nullptr;
					auto Error = UE::UAI::Contract::FDiagnostic::WrapFailure(Code,
						FirstDifference ? FirstDifference->Diagnostic : Report.Diagnostic);
					LocateComparedTarget(Error, Prefix);
					return Error;
				}
			}
			auto Error = Difference(TEXT("native_settings_match"),
				TEXT("The native localization settings differ outside the available public property comparison."),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			if (CaptureError.IsValid())
			{
				LocateComparedTarget(CaptureError, Prefix);
				Error.Append(CaptureError);
			}
			return Error;
		}

		bool CompareSettings(const TArray<FLocalizationTargetSettings>& Actual,
			const TArray<FLocalizationTargetSettings>& Expected, bool bSerialized,
			const FString& Code, UE::UAI::Contract::FDiagnostic& Error)
		{
			Error.Reset();
			if (Actual.Num() != Expected.Num())
			{
				Error = ConfigFailure(Code, TEXT("The number of localization targets differs from the expected state."),
					TEXT("/targets/count"), MakeShared<FJsonValueNumber>(Actual.Num()), MakeShared<FJsonValueNumber>(Expected.Num()));
				return false;
			}
			for (int32 Index = 0; Index < Actual.Num(); ++Index)
			{
				TOptional<FLocalizationTargetSettings> PersistentExpected;
				const FLocalizationTargetSettings* ComparedExpected = &Expected[Index];
				if (bSerialized)
				{
					// UE's config representation omits these two transient statistics
					// (LocalizationTargetTypes.h). Compare the persisted native fields
					// with the same native struct owner, without two ExportText copies.
					PersistentExpected.Emplace(Expected[Index]);
					PersistentExpected->ConflictStatus = Actual[Index].ConflictStatus;
					for (int32 Culture = 0; Culture < PersistentExpected->SupportedCulturesStatistics.Num()
						&& Culture < Actual[Index].SupportedCulturesStatistics.Num(); ++Culture)
						PersistentExpected->SupportedCulturesStatistics[Culture].WordCount = Actual[Index].SupportedCulturesStatistics[Culture].WordCount;
					ComparedExpected = &PersistentExpected.GetValue();
				}
				const bool bEqual = FLocalizationTargetSettings::StaticStruct()->CompareScriptStruct(&Actual[Index], ComparedExpected, PPF_None);
				if (!bEqual)
				{
					Error = TargetDifference(Actual[Index], *ComparedExpected, Index, Code);
					return false;
				}
			}
			return true;
		}
	}

	bool EqualLocalizationConfigValues(const TArray<FLocalizationTargetSettings>& Actual,
		const TArray<FLocalizationTargetSettings>& Expected, UE::UAI::Contract::FDiagnostic& Error)
	{
		return CompareSettings(Actual, Expected, true, TEXT("localization_config_stage_readback_mismatch"), Error);
	}

	bool EqualLocalizationNativeValues(const TArray<FLocalizationTargetSettings>& Actual,
		const TArray<FLocalizationTargetSettings>& Expected, UE::UAI::Contract::FDiagnostic& Error)
	{
		return CompareSettings(Actual, Expected, false, TEXT("localization_native_state_mismatch"), Error);
	}

	bool WriteLocalizationSettingsStage(const TArray<FLocalizationTargetSettings>& Values, const FString& StagingPath, UE::UAI::Contract::FDiagnostic& Error)
	{
		const auto* Property = GameProperty(Error);
		if (!Property) return false;
		const FString DefaultFilename = GetDefault<ULocalizationSettings>()->GetDefaultConfigFilename();
		TStrongObjectPtr<ULocalizationSettings> Shadow(NewObject<ULocalizationSettings>(GetTransientPackage()));
		if (!SetLocalizationBackingSettings(*Shadow, Values, Error)) return false;
		if (!EditorProject::StageSettingsConfig(*Shadow,
			[Property](const FProperty& Field) { return &Field == Property; },
			DefaultFilename, StagingPath, Error)) return false;
		TArray<FLocalizationTargetSettings> Readback;
		if (!ReadLocalizationSettingsFile(StagingPath, Readback, Error)) return false;
		if (Readback.Num() != Values.Num())
		{
			Error = ConfigFailure(TEXT("localization_config_stage_readback_count"),
				TEXT("The staged localization config has an unexpected number of targets."), TEXT("/targets/count"),
				MakeShared<FJsonValueNumber>(Readback.Num()), MakeShared<FJsonValueNumber>(Values.Num()));
			Error.Source.SourceFile = StagingPath;
			return false;
		}
		if (!EqualLocalizationConfigValues(Readback, Values, Error))
		{
			Error.Source.SourceFile = StagingPath;
			return false;
		}
		return true;
	}
}
