#include "DomainAdapters/EditorProject/UeAgentSettingsConfig.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/ConfigContext.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::EditorProject
{
namespace
{
UE::UAI::Contract::FDiagnostic SettingsConfigFailure(const FString& Code, const FString& Reason,
	const FString& Filename, const FString& Path, bool Actual = false, bool Expected = true)
{
	auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("settings_config"));
	Error.SourceKind = TEXT("native_result");
	Error.Source.SourceFile = Filename;
	Error.SubjectId = Filename;
	Error.PropertyPath = Path;
	Error.ActualValue = MakeShared<FJsonValueBoolean>(Actual);
	Error.ExpectedValue = MakeShared<FJsonValueBoolean>(Expected);
	Error.ActualValuePresent = true;
	Error.ExpectedValuePresent = true;
	return Error;
}

FString ConfigSection(UObject& Object, const FProperty& Property)
{
	if (Object.GetClass()->HasAnyClassFlags(CLASS_PerObjectConfig) && !Object.HasAnyFlags(RF_ClassDefaultObject))
	{
		FString Section = (Object.GetOutermost() == GetTransientPackage() ? Object.GetName()
			: Object.GetPathName(Object.GetOutermost())) + TEXT(" ") + Object.GetClass()->GetName();
		Object.OverridePerObjectConfigSection(Section);
		return Section;
	}
	FString Section = Property.HasAnyPropertyFlags(CPF_GlobalConfig) ? Property.GetOwnerClass()->GetPathName() : Object.GetClass()->GetPathName();
	Object.OverrideConfigSection(Section);
	return Section;
}
}

FString SettingsConfigFilename(const UObject& Settings)
{
	const UClass* Class = Settings.GetClass();
	if (Class->HasAnyClassFlags(CLASS_DefaultConfig)) return Settings.GetDefaultConfigFilename();
	if (Class->HasAnyClassFlags(CLASS_GlobalUserConfig)) return Settings.GetGlobalUserConfigFilename();
	if (Class->HasAnyClassFlags(CLASS_ProjectUserConfig)) return Settings.GetProjectUserConfigFilename();
	return Class->GetConfigName();
}

FString SettingsConfigKey(const FProperty& Property)
{
	const FString& CVar = Property.GetMetaData(TEXT("ConsoleVariable"));
	return CVar.IsEmpty() ? Property.GetName() : CVar;
}

bool StageSettingsConfig(UObject& Settings, TFunctionRef<bool(const FProperty&)> HasChanged,
	const FString& FinalPath, const FString& StagingPath, UE::UAI::Contract::FDiagnostic& Error)
{
	// Let UE serialize every native type and choose its own config sections/keys.
	// Only fields actually changed by the edit or its native callbacks are merged.
	FConfigCacheIni Native(EConfigCacheType::Temporary);
	Native.DisableFileOperations();
	FConfigFile& Serialized = Native.Add(FinalPath, FConfigFile());
	Settings.SaveConfig(CPF_Config, *FinalPath, &Native, false);
	FConfigFile Staged;
	FString Existing;
	if (FPaths::FileExists(FinalPath))
	{
		if (!FFileHelper::LoadFileToString(Existing, *FinalPath))
	{ Error = SettingsConfigFailure(TEXT("settings_config_read_failed"),
			TEXT("The existing settings configuration file could not be read before staging."),
			FinalPath, TEXT("/read_succeeded")); return false; }
		Staged.Read(FinalPath);
	}
	TSet<FString> ChangedSections;
	for (TFieldIterator<FProperty> It(Settings.GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Config) || !HasChanged(*Property)) continue;
		const FString Section = ConfigSection(Settings, *Property);
		ChangedSections.Add(Section);
		const FConfigSection* Values = Serialized.FindSection(Section);
		const FString Key = SettingsConfigKey(*Property);
		for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
		{
			const FString ElementKey = Property->ArrayDim == 1 ? Key : FString::Printf(TEXT("%s[%d]"), *Property->GetName(), Index);
			for (const TCHAR* Prefix : {TEXT(""), TEXT("+"), TEXT("-"), TEXT("."), TEXT("!"), TEXT("^")})
				Staged.RemoveKeyFromSection(*Section, FName(*(FString(Prefix) + ElementKey)));
			const bool bCollection = Property->IsA<FArrayProperty>() || Property->IsA<FSetProperty>();
			if (!Values) continue;
			bool bHasNativeValue = false;
			for (const auto& Pair : *Values)
			{
				FString NativeKey = Pair.Key.ToString();
				if (NativeKey.StartsWith(TEXT("+")) || NativeKey.StartsWith(TEXT(".")) || NativeKey.StartsWith(TEXT("!"))) NativeKey.RightChopInline(1);
				if (NativeKey == ElementKey) { bHasNativeValue = true; break; }
			}
			// No native entry means inheritance, not an empty replacement collection.
			if (bCollection && bHasNativeValue) Staged.AddToSection(*Section, FName(*(TEXT("!") + ElementKey)), TEXT("ClearArray"));
			for (const auto& Pair : *Values)
			{
				FString NativeKey = Pair.Key.ToString();
				FString BareKey = NativeKey;
				if (BareKey.StartsWith(TEXT("+")) || BareKey.StartsWith(TEXT(".")) || BareKey.StartsWith(TEXT("!"))) BareKey.RightChopInline(1);
				if (BareKey != ElementKey || NativeKey.StartsWith(TEXT("!"))) continue;
				// Dot preserves repeated array elements; the preceding clear expresses replacement.
				if (bCollection) NativeKey = TEXT(".") + ElementKey;
				Staged.AddToSection(*Section, FName(*NativeKey), Pair.Value.GetSavedValue());
			}
		}
	}
	// Native UpdateSections preserves text outside the selected sections, including
	// comments and the preamble. Do not regenerate unrelated settings from a parsed map.
	TArray<FString> Sections;
	Staged.GetKeys(Sections);
	for (const FString& Section : Sections)
		if (!ChangedSections.Contains(Section)) Staged.Remove(Section);
	if (!FFileHelper::SaveStringToFile(Existing, *StagingPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{ Error = SettingsConfigFailure(TEXT("settings_config_stage_failed"),
		TEXT("The existing configuration text could not be written to the staging file."),
		StagingPath, TEXT("/write_succeeded")); return false; }
	if (!ChangedSections.IsEmpty())
	{
		Staged.Dirty = true;
		Staged.UpdateSections(*StagingPath);
		// UpdateSections is void; UE retains Dirty when its native write fails.
		if (Staged.Dirty || !FPaths::FileExists(StagingPath))
	{ Error = SettingsConfigFailure(TEXT("settings_config_stage_failed"),
			Staged.Dirty ? TEXT("The native configuration writer retained unsaved staged sections.")
				: TEXT("The native configuration writer did not create the staging file."),
			StagingPath, Staged.Dirty ? TEXT("/native_config_dirty") : TEXT("/staging_file_exists"),
			Staged.Dirty, !Staged.Dirty); return false; }
	}
	return true;
}

bool ReadSettingsConfig(UObject& Snapshot, const UObject& Owner, const FString& Filename, UE::UAI::Contract::FDiagnostic& Error)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Filename))
	{ Error = SettingsConfigFailure(TEXT("settings_config_read_failed"),
		TEXT("The saved settings configuration file could not be read for verification."),
		Filename, TEXT("/read_succeeded")); return false; }
	FConfigFile File;
	// Read only the lower native layers before applying this published file.
	// Later user/generated overrides are live effects, not this file's saved value.
	const FString StopAt = FConfigCacheIni::NormalizeConfigIniPath(SettingsConfigFilename(Owner));
	const FString Platform = Owner.GetConfigOverridePlatform() ? Owner.GetConfigOverridePlatform() : TEXT("");
	FConfigContext Context = FConfigContext::ReadUpToBeforeFile(File, Platform, StopAt);
	Context.bWriteDestIni = false;
	if (!Context.Load(*Owner.GetClass()->ClassConfigName.ToString()))
	{ Error = SettingsConfigFailure(TEXT("settings_config_hierarchy_read_failed"),
		TEXT("The native configuration hierarchy could not be loaded below the saved settings file."),
		Filename, TEXT("/hierarchy_loaded"));
		Error.CandidateName = Owner.GetClass()->ClassConfigName.ToString();
		return false; }
	File.CombineFromBuffer(Text, Filename);
	// Detached instances have a different per-object section name. Rebind only
	// that identity; values still come exclusively from the published file.
	TSet<FString> CopiedSections;
	for (TFieldIterator<FProperty> It(Owner.GetClass()); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Config)) continue;
		const FString Source = ConfigSection(const_cast<UObject&>(Owner), **It);
		const FString Destination = ConfigSection(Snapshot, **It);
		if (Source != Destination && !CopiedSections.Contains(Source))
		{
			CopiedSections.Add(Source);
			if (const FConfigSection* Section = File.FindSection(Source))
			{
				const FConfigSection Copy = *Section;
				for (const auto& Pair : Copy) File.AddToSection(*Destination, Pair.Key, Pair.Value.GetSavedValue());
			}
		}
	}
	const FString CacheKey = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterface"),
		TEXT("settings-read-") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".ini"));
	GConfig->SetFile(CacheKey, &File);
	Snapshot.LoadConfig(Snapshot.GetClass(), *CacheKey, UE::LCPF_ReadParentSections);
	GConfig->UnloadFile(CacheKey);
	return true;
}
}
