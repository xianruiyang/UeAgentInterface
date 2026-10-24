#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeResolvers.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeIdentity.h"
#include "AssetRegistry/AssetData.h"
#include "DomainAdapters/EditorProject/UeAgentSettingsDiscovery.h"
#include "DomainAdapters/EditorProject/UeAgentSettingsConfig.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "ISettingsCategory.h"
#include "ISettingsContainer.h"
#include "ISettingsModule.h"
#include "ISettingsSection.h"
#include "Internationalization/StringTable.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::Composition::SimpleAuthoring
{
	namespace
	{
		using namespace AssetDurable;
		using namespace ContextResource;
		using namespace Contract;
		using namespace Discovery;
		using namespace DomainAdapters::EditorProjectSimpleEffect;
		using namespace EditorProject;
		using namespace ExecutionMutation;
		using namespace RuntimePrivate;
		using namespace StateSemantic;

		const FString TargetCatalogKind = TEXT("editor_project_target");
		const FString FieldCatalogKind =
			TEXT("editor_project_settings_field");
		const FString EditorProjectCatalogSourceId =
			TEXT("editor-project-catalog");
		const FString TargetReadPortId =
			TEXT("editor-project-target-read");
		const FString FieldReadPortId =
			TEXT("editor-project-settings-field-read");

		enum class EResolverRole : uint8
		{
			PreState,
			Mutation,
			Persisted,
		};

		struct FTargetRecord
		{
			FUeAgentEditorProjectSimpleEffectProductionProfile Profile;
			FString Ref;
			FString Revision;
			FString DurableLocation;
			FString ConfigSection;
			FString DisplayName;
			FString SearchText;
			TWeakObjectPtr<UObject> SettingsObject;
			FString StringTableObjectPath;

			bool IsValid() const
			{
				return Profile.IsValid()
					&& !Ref.IsEmpty()
					&& !Revision.IsEmpty()
					&& !DurableLocation.IsEmpty()
					&& !DisplayName.IsEmpty();
			}
		};

		struct FFieldRecord
		{
			FUeAgentEditorProjectSimpleEffectProductionProfile Profile;
			FString TargetRef;
			FString FieldRef;
			FString PropertyName;
			FString TypeId;
			FString Revision;
			FString Summary;

			bool IsValid() const
			{
				return Profile.IsValid()
					&& !TargetRef.IsEmpty()
					&& !FieldRef.IsEmpty()
					&& !PropertyName.IsEmpty()
					&& !TypeId.IsEmpty()
					&& !Revision.IsEmpty();
			}
		};

		void SetRuntimeError(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;  }


		bool FailMutation(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject; return false; }


		FString NormalizeFullPath(const FString& Path)
		{
			FString Result = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeFilename(Result);
			return Result;
		}

		bool IsUnderRoot(const FString& Path, const FString& Root)
		{
			const FString NormalizedPath = NormalizeFullPath(Path);
			FString NormalizedRoot = NormalizeFullPath(Root);
			if (!NormalizedRoot.EndsWith(TEXT("/")))
			{
				NormalizedRoot += TEXT("/");
			}
			return NormalizedPath.StartsWith(
				NormalizedRoot,
				ESearchCase::IgnoreCase);
		}

		FString ProjectOwnerId()
		{
			return TEXT("uai.project:")
				+ Sha256(NormalizeFullPath(FPaths::GetProjectFilePath())).Mid(7);
		}

		FString TargetRef(
			const FString& Kind,
			std::initializer_list<FString> Segments)
		{
			FString Result = TEXT("uai://editor-project/v1/") + Kind;
			for (const FString& Segment : Segments)
			{
				Result += TEXT("/") + EncodeUriSegment(Segment);
			}
			return Result;
		}

		FString MakeFieldRef(
			const FString& Target,
			const FString& PropertyName)
		{
			return Target
				+ TEXT("/field/")
				+ EncodeUriSegment(PropertyName);
		}

		bool TryLoadConfigFile(
			const FString& Path,
			FConfigFile& OutConfig,
			FDiagnostic& OutError,
			const FString& FailureCode = TEXT("editor_project_config_file_not_found"))
		{
			OutError.Reset();
			OutConfig.Empty();
			const FString FullPath = NormalizeFullPath(Path);
			if (!IFileManager::Get().FileExists(*FullPath))
			{
				OutError = FDiagnostic::Failure(FailureCode,
					TEXT("The required project configuration file does not exist."), TEXT("simple_authoring"), FullPath);
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = FPaths::GetCleanFilename(FullPath);
				OutError.PropertyPath = TEXT("/file_exists");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			OutConfig.Read(FullPath);
			return true;
		}

		FString ConfigSectionRevisionInput(
			const FConfigFile& Config,
			const FString& Section)
		{
			const FConfigSection* Found = Config.FindSection(Section);
			if (!Found)
			{
				return FString();
			}
			TArray<FString> Entries;
			for (const TPair<FName, FConfigValue>& Pair : *Found)
			{
				Entries.Add(
					Pair.Key.ToString()
					+ TEXT("\x1f")
					+ Pair.Value.GetValue());
			}
			Entries.Sort();
			return FString::Join(Entries, TEXT("\n"));
		}

		bool TrySettingsType(FProperty& Property, FString& OutTypeId)
		{
			OutTypeId.Reset();
			if (!Property.HasAnyPropertyFlags(CPF_Config)
				|| !Property.HasAnyPropertyFlags(CPF_Edit)
				|| Property.HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
			{
				return false;
			}
			if (CastField<FBoolProperty>(&Property))
			{
				OutTypeId = TEXT("uai.value.boolean.v1");
			}
			else if (CastField<FStrProperty>(&Property))
			{
				OutTypeId = TEXT("uai.value.string.v1");
			}
			else if (CastField<FIntProperty>(&Property)
				|| CastField<FInt64Property>(&Property))
			{
				OutTypeId = TEXT("uai.value.integer.v1");
			}
			return !OutTypeId.IsEmpty();
		}

		FString SettingsFieldRevision(
			const FString& Target,
			FProperty& Property,
			const FString& TypeId)
		{
			return Sha256(
				ProjectOwnerId()
				+ TEXT("\n") + Target
				+ TEXT("\n") + Property.GetOwnerStruct()->GetPathName()
				+ TEXT("\n") + Property.GetName()
				+ TEXT("\n") + Property.GetClass()->GetName()
				+ TEXT("\n") + TypeId
				+ TEXT("\n") + FString::Printf(
					TEXT("%llu:%d"),
					static_cast<uint64>(Property.GetPropertyFlags()),
					Property.ArrayDim));
		}

		FString SettingsStateRevisionInput(UObject& SettingsObject)
		{
			TArray<FString> Fields;
			for (TFieldIterator<FProperty> It(
				SettingsObject.GetClass(),
				EFieldIteratorFlags::IncludeSuper);
				It;
				++It)
			{
				FProperty* Property = *It;
				FString TypeId;
				if (!Property || !TrySettingsType(*Property, TypeId))
				{
					continue;
				}
				const void* Address =
					Property->ContainerPtrToValuePtr<void>(&SettingsObject);
				FString Value;
				Property->ExportText_Direct(
					Value,
					Address,
					Address,
					&SettingsObject,
					PPF_None);
				Fields.Add(
					Property->GetName()
					+ TEXT("\x1f") + TypeId
					+ TEXT("\x1f") + Value);
			}
			Fields.Sort();
			return FString::Join(Fields, TEXT("\n"));
		}

		bool TryApplyPersistedSettingsText(
			FProperty& Property,
			UObject& SettingsObject,
			const FString& Text,
			FDiagnostic& OutError,
			const FString& FailureCode)
		{
			OutError.Reset();
			void* Address =
				Property.ContainerPtrToValuePtr<void>(&SettingsObject);
			if (const FStrProperty* StringProperty =
					CastField<FStrProperty>(&Property))
			{
				StringProperty->SetPropertyValue(Address, Text);
				return true;
			}
			if (Property.ImportText_Direct(*Text, Address, &SettingsObject, PPF_None) != nullptr)
			{
				return true;
			}
			OutError = FDiagnostic::Failure(FailureCode,
				TEXT("The stored setting cannot be imported as the property's type."), TEXT("simple_authoring"),
				SettingsObject.GetPathName());
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Property.GetName();
			OutError.ActualValue = MakeShared<FJsonValueString>(Text);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Property.GetCPPType());
			return false;
		}

		bool TryReloadSettingsFromDisk(
			const FTargetRecord& Record,
			const TMap<FString, FString>& BaselineValues,
			UObject& Snapshot,
			FExecutionMutationError& OutError)
		{
			FConfigFile PersistedConfig;
			if (!TryLoadConfigFile(Record.DurableLocation, PersistedConfig, OutError,
				TEXT("editor_project_settings_reload_failed")))
			{
				OutError.TargetPath = Record.Ref;
				return false;
			}

			const FString Section = Snapshot.GetClass()->GetPathName();
			for (TFieldIterator<FProperty> It(
				Snapshot.GetClass(),
				EFieldIteratorFlags::IncludeSuper);
				It;
				++It)
			{
				FProperty* Property = *It;
				FString TypeId;
				if (!Property || !TrySettingsType(*Property, TypeId))
				{
					continue;
				}
				const FString* BaselineText =
					BaselineValues.Find(Property->GetName());
				if (!BaselineText)
				{
					OutError = FDiagnostic::Failure(TEXT("editor_project_settings_baseline_invalid"),
						TEXT("The baseline has no stored value for this setting."), TEXT("simple_authoring"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Record.Ref;
					OutError.PropertyPath = Property->GetName();
					OutError.ActualValuePresent = false;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Property->GetCPPType());
					return false;
				}
				if (!TryApplyPersistedSettingsText(*Property, Snapshot, *BaselineText, OutError,
					TEXT("editor_project_settings_baseline_invalid")))
				{
					OutError.TargetPath = Record.Ref;
					return false;
				}
				FString PersistedText;
				if (PersistedConfig.GetString(
						*Section,
						*DomainAdapters::EditorProject::SettingsConfigKey(*Property),
						PersistedText)
					&& !TryApplyPersistedSettingsText(*Property, Snapshot, PersistedText, OutError,
						TEXT("editor_project_settings_value_reload_failed")))
				{
					OutError.TargetPath = Record.Ref;
					OutError.InternalContext = Record.DurableLocation;
					return false;
				}
			}
			return true;
		}

		FString TargetRevision(
			const FTargetRecord& Record,
			const FString& StateInput)
		{
			return Sha256(
				ProjectOwnerId()
				+ TEXT("\n") + Record.Profile.ProfileId
				+ TEXT("\n") + Record.Ref
				+ TEXT("\n") + Record.DurableLocation
				+ TEXT("\n") + Record.Profile.DescriptorId
				+ TEXT("\n") + Record.Profile.DescriptorHash
				+ TEXT("\n") + StateInput);
		}

		bool TryGetProfiles(
			TMap<FString, FUeAgentEditorProjectSimpleEffectProductionProfile>&
				OutProfiles,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutProfiles.Reset();
			TArray<FUeAgentEditorProjectSimpleEffectProductionProfile> Profiles;
			if (!TryGetEditorProjectSimpleEffectProductionProfiles(
					Profiles,
					OutError))
			{
				return false;
			}
			for (const FUeAgentEditorProjectSimpleEffectProductionProfile& Profile
				: Profiles)
			{
				if (OutProfiles.Contains(Profile.ProfileId))
				{
					OutError = FDiagnostic::Failure(TEXT("editor_project_runtime_profile_invalid"),
						TEXT("More than one editor-project production profile uses this profile ID."), TEXT("catalog"));
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = TEXT("profile_id");
					OutError.ActualValue = MakeShared<FJsonValueString>(Profile.ProfileId);
					return false;
				}
				OutProfiles.Add(Profile.ProfileId, Profile);
			}
			return true;
		}

		void AddConfigRecord(
			const FUeAgentEditorProjectSimpleEffectProductionProfile& Profile,
			const FString& Ref,
			const FString& DurableLocation,
			const FString& Section,
			const FString& DisplayName,
			const FString& SearchText,
			const FConfigFile& Config,
			TArray<FTargetRecord>& OutRecords)
		{
			FTargetRecord Record;
			Record.Profile = Profile;
			Record.Ref = Ref;
			Record.DurableLocation = NormalizeFullPath(DurableLocation);
			Record.ConfigSection = Section;
			Record.DisplayName = DisplayName;
			Record.SearchText = SearchText;
			Record.Revision = TargetRevision(
				Record,
				ConfigSectionRevisionInput(Config, Section));
			if (Record.IsValid())
			{
				OutRecords.Add(MoveTemp(Record));
			}
		}

		void AddDefaultConfigSections(
			const TMap<FString,
				FUeAgentEditorProjectSimpleEffectProductionProfile>& Profiles,
			TArray<FTargetRecord>& OutRecords)
		{
			const FUeAgentEditorProjectSimpleEffectProductionProfile* Profile =
				Profiles.Find(TEXT("config_section"));
			if (!Profile)
			{
				return;
			}
			const TArray<TPair<FString, FString>> Sources = {
				{TEXT("engine"), TEXT("DefaultEngine.ini")},
				{TEXT("game"), TEXT("DefaultGame.ini")},
				{TEXT("editor"), TEXT("DefaultEditor.ini")},
				{TEXT("input"), TEXT("DefaultInput.ini")},
			};
			for (const TPair<FString, FString>& Source : Sources)
			{
				const FString Path = FPaths::Combine(
					FPaths::ProjectConfigDir(),
					Source.Value);
				FConfigFile Config;
				FDiagnostic MissingOptionalConfig;
				if (!TryLoadConfigFile(Path, Config, MissingOptionalConfig))
				{
					continue;
				}
				for (const TPair<FString, FConfigSection>& Section
					: AsConst(Config))
				{
					const FString Ref = TargetRef(
						TEXT("config-section"),
						{Source.Key, Section.Key});
					AddConfigRecord(
						*Profile,
						Ref,
						Path,
						Section.Key,
						Source.Key + TEXT(": ") + Section.Key,
						Ref + TEXT(" ") + Source.Key + TEXT(" ")
							+ Source.Value + TEXT(" ") + Section.Key,
						Config,
						OutRecords);
				}
			}
		}

		void AddProjectSettings(
			const TMap<FString, FUeAgentEditorProjectSimpleEffectProductionProfile>& Profiles,
			TArray<FTargetRecord>& OutRecords)
		{
			const auto* Profile = Profiles.Find(TEXT("project_settings"));
			TArray<DomainAdapters::EditorProject::FSettingsPage> Pages;
			UE::UAI::Contract::FDiagnostic Error;
			if (!Profile || !DomainAdapters::EditorProject::CollectSettingsPages(Pages, Error)) return;
			for (const auto& Page : Pages)
			{
				UObject* SettingsObject = Page.SettingsObject;
				if (Page.ContainerName != FName(TEXT("Project")) || !IsValid(SettingsObject)) continue;
				UClass* SettingsClass = SettingsObject->GetClass();
				if (SettingsClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
					|| (SettingsClass->ClassWithin && !GetTransientPackage()->IsA(SettingsClass->ClassWithin))) continue;
				bool bSupported = false;
				for (TFieldIterator<FProperty> It(SettingsClass); It; ++It)
				{
					FString TypeId;
					if (TrySettingsType(**It, TypeId)) { bSupported = true; break; }
				}
				const FString Durable = NormalizeFullPath(SettingsObject->GetDefaultConfigFilename());
				if (!bSupported || !IsUnderRoot(Durable, FPaths::ProjectConfigDir())) continue;
				FTargetRecord Record;
				Record.Profile = *Profile;
				Record.Ref = TargetRef(TEXT("project-settings"),
					{Page.ContainerName.ToString(), Page.CategoryName.ToString(), Page.SectionName.ToString()});
				Record.DurableLocation = Durable;
				Record.DisplayName = Page.SectionId;
				Record.SearchText = Record.Ref + TEXT(" ") + Record.DisplayName + TEXT(" ") + SettingsClass->GetPathName();
				Record.SettingsObject = SettingsObject;
				Record.Revision = TargetRevision(Record, SettingsStateRevisionInput(*SettingsObject));
				if (Record.IsValid()) OutRecords.Add(MoveTemp(Record));
			}
		}

		void AddDeviceProfiles(
			const TMap<FString,
				FUeAgentEditorProjectSimpleEffectProductionProfile>& Profiles,
			TArray<FTargetRecord>& OutRecords)
		{
			const FUeAgentEditorProjectSimpleEffectProductionProfile* Profile =
				Profiles.Find(TEXT("device_profile"));
			const FString Path = FPaths::Combine(
				FPaths::ProjectConfigDir(),
				TEXT("DefaultDeviceProfiles.ini"));
			FConfigFile Config;
			FDiagnostic MissingOptionalConfig;
			if (!Profile || !TryLoadConfigFile(Path, Config, MissingOptionalConfig))
			{
				return;
			}
			for (const TPair<FString, FConfigSection>& Section
				: AsConst(Config))
			{
				if (!Section.Key.EndsWith(TEXT(" DeviceProfile")))
				{
					continue;
				}
				const FString Name = Section.Key.LeftChop(14);
				const FString Ref = TargetRef(
					TEXT("device-profile"),
					{Name});
				AddConfigRecord(
					*Profile,
					Ref,
					Path,
					Section.Key,
					Name,
					Ref + TEXT(" ") + Name + TEXT(" ") + Section.Key,
					Config,
					OutRecords);
			}
		}

		void AddLocalizationTargets(
			const TMap<FString,
				FUeAgentEditorProjectSimpleEffectProductionProfile>& Profiles,
			TArray<FTargetRecord>& OutRecords)
		{
			const FUeAgentEditorProjectSimpleEffectProductionProfile* Profile =
				Profiles.Find(TEXT("localization_target"));
			if (!Profile)
			{
				return;
			}
			const FString Root = FPaths::Combine(
				FPaths::ProjectConfigDir(),
				TEXT("Localization"));
			TArray<FString> Files;
			IFileManager::Get().FindFiles(
				Files,
				*FPaths::Combine(Root, TEXT("*.ini")),
				true,
				false);
			for (const FString& File : Files)
			{
				const FString Path = FPaths::Combine(Root, File);
				FConfigFile Config;
				FDiagnostic MissingOptionalConfig;
				if (!TryLoadConfigFile(Path, Config, MissingOptionalConfig)
					|| !Config.FindSection(TEXT("CommonSettings")))
				{
					continue;
				}
				const FString Name = FPaths::GetBaseFilename(File);
				const FString Ref = TargetRef(
					TEXT("localization-target"),
					{Name});
				AddConfigRecord(
					*Profile,
					Ref,
					Path,
					TEXT("CommonSettings"),
					Name,
					Ref + TEXT(" ") + Name,
					Config,
					OutRecords);
			}
		}

		void AddPlatformConfigs(
			const TMap<FString,
				FUeAgentEditorProjectSimpleEffectProductionProfile>& Profiles,
			TArray<FTargetRecord>& OutRecords)
		{
			const FUeAgentEditorProjectSimpleEffectProductionProfile* Profile =
				Profiles.Find(TEXT("platform_config"));
			if (!Profile)
			{
				return;
			}
			TArray<FString> Platforms;
			IFileManager::Get().FindFiles(
				Platforms,
				*FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("*")),
				false,
				true);
			for (const FString& Platform : Platforms)
			{
				if (Platform.IsEmpty()
					|| Platform.Contains(TEXT("/"))
					|| Platform.Contains(TEXT("\\"))
					|| Platform.Contains(TEXT("..")))
				{
					continue;
				}
				for (const FString& Suffix
					: TArray<FString>{
						TEXT("Engine"),
						TEXT("Game"),
						TEXT("DeviceProfiles"),
					})
				{
					const FString ConfigName = Platform + Suffix;
					const FString Path = FPaths::Combine(
						FPaths::ProjectConfigDir(),
						Platform,
						ConfigName + TEXT(".ini"));
					FConfigFile Config;
					FDiagnostic MissingOptionalConfig;
					if (!TryLoadConfigFile(Path, Config, MissingOptionalConfig))
					{
						continue;
					}
					for (const TPair<FString, FConfigSection>& Section
						: AsConst(Config))
					{
						const FString Ref = TargetRef(
							TEXT("platform-config"),
							{Platform, ConfigName, Section.Key});
						AddConfigRecord(
							*Profile,
							Ref,
							Path,
							Section.Key,
							Platform + TEXT(" / ") + ConfigName
								+ TEXT(" / ") + Section.Key,
							Ref + TEXT(" ") + Platform + TEXT(" ")
								+ ConfigName + TEXT(" ") + Section.Key,
							Config,
							OutRecords);
					}
				}
			}
		}

		void AddScalabilityGroups(
			const TMap<FString,
				FUeAgentEditorProjectSimpleEffectProductionProfile>& Profiles,
			TArray<FTargetRecord>& OutRecords)
		{
			const FUeAgentEditorProjectSimpleEffectProductionProfile* Profile =
				Profiles.Find(TEXT("scalability_group"));
			const FString Path = FPaths::Combine(
				FPaths::ProjectConfigDir(),
				TEXT("DefaultScalability.ini"));
			FConfigFile Config;
			FDiagnostic MissingOptionalConfig;
			if (!Profile || !TryLoadConfigFile(Path, Config, MissingOptionalConfig))
			{
				return;
			}
			for (const TPair<FString, FConfigSection>& Section
				: AsConst(Config))
			{
				if (!Section.Key.Contains(TEXT("Quality@")))
				{
					continue;
				}
				const FString Ref = TargetRef(
					TEXT("scalability-group"),
					{Section.Key});
				AddConfigRecord(
					*Profile,
					Ref,
					Path,
					Section.Key,
					Section.Key,
					Ref + TEXT(" ") + Section.Key,
					Config,
					OutRecords);
			}
		}

		void AddStringTables(
			const TMap<FString,
				FUeAgentEditorProjectSimpleEffectProductionProfile>& Profiles,
			TArray<FTargetRecord>& OutRecords)
		{
			const FUeAgentEditorProjectSimpleEffectProductionProfile* Profile =
				Profiles.Find(TEXT("string_table"));
			if (!Profile)
			{
				return;
			}
			FAssetRegistryModule& Module =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
					TEXT("AssetRegistry"));
			FARFilter Filter;
			Filter.ClassPaths.Add(UStringTable::StaticClass()->GetClassPathName());
			Filter.PackagePaths.Add(FName(TEXT("/Game")));
			Filter.bRecursivePaths = true;
			TArray<FAssetData> Assets;
			Module.Get().GetAssets(Filter, Assets);
			for (const FAssetData& Asset : Assets)
			{
				const FString ObjectPath = Asset.GetSoftObjectPath().ToString();
				const FString PackageName = Asset.PackageName.ToString();
				const FString PackageFile = FPackageName::LongPackageNameToFilename(
					PackageName,
					FPackageName::GetAssetPackageExtension());
				const FFileStatData Stat =
					IFileManager::Get().GetStatData(*PackageFile);
				if (ObjectPath.IsEmpty() || !Stat.bIsValid || Stat.bIsDirectory)
				{
					continue;
				}
				FTargetRecord Record;
				Record.Profile = *Profile;
				Record.Ref = TargetRef(
					TEXT("string-table"),
					{ObjectPath});
				Record.DurableLocation = PackageName;
				Record.StringTableObjectPath = ObjectPath;
				Record.DisplayName = Asset.AssetName.ToString();
				Record.SearchText = Record.Ref + TEXT(" ") + ObjectPath;
				Record.Revision = TargetRevision(
					Record,
					FString::Printf(
						TEXT("%lld:%lld"),
						Stat.FileSize,
						Stat.ModificationTime.GetTicks()));
				if (Record.IsValid())
				{
					OutRecords.Add(MoveTemp(Record));
				}
			}
		}

		bool TryBuildTargetRecords(
			TArray<FTargetRecord>& OutRecords,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutRecords.Reset();
			TMap<FString, FUeAgentEditorProjectSimpleEffectProductionProfile>
				Profiles;
			if (!TryGetProfiles(Profiles, OutError))
			{
				return false;
			}
			AddDefaultConfigSections(Profiles, OutRecords);
			AddProjectSettings(Profiles, OutRecords);
			AddDeviceProfiles(Profiles, OutRecords);
			AddLocalizationTargets(Profiles, OutRecords);
			AddPlatformConfigs(Profiles, OutRecords);
			AddScalabilityGroups(Profiles, OutRecords);
			AddStringTables(Profiles, OutRecords);
			OutRecords.Sort(
				[](const FTargetRecord& Left, const FTargetRecord& Right)
				{
					return Left.Ref < Right.Ref;
				});
			for (int32 Index = 1; Index < OutRecords.Num(); ++Index)
			{
				if (OutRecords[Index - 1].Ref == OutRecords[Index].Ref)
				{
					OutError = FDiagnostic::Failure(TEXT("editor_project_runtime_target_duplicate"),
						TEXT("More than one editor-project catalog target uses this reference."), TEXT("catalog"));
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = FString::Printf(TEXT("targets/%d/ref"), Index);
					OutError.ActualValue = MakeShared<FJsonValueString>(OutRecords[Index].Ref);
					return false;
				}
			}
			return true;
		}

		void BuildFieldRecordsForTarget(
			const FTargetRecord& Target,
			UObject& SettingsObject,
			TArray<FFieldRecord>& OutFields)
		{
			for (TFieldIterator<FProperty> It(
				SettingsObject.GetClass(),
				EFieldIteratorFlags::IncludeSuper);
				It;
				++It)
			{
				FProperty* Property = *It;
				FString TypeId;
				if (!Property || !TrySettingsType(*Property, TypeId))
				{
					continue;
				}
				FFieldRecord Field;
				Field.Profile = Target.Profile;
				Field.TargetRef = Target.Ref;
				Field.PropertyName = Property->GetName();
				Field.FieldRef = MakeFieldRef(
					Target.Ref,
					Field.PropertyName);
				Field.TypeId = MoveTemp(TypeId);
				Field.Revision = SettingsFieldRevision(
					Target.Ref,
					*Property,
					Field.TypeId);
				Field.Summary = SettingsObject.GetClass()->GetPathName()
					+ TEXT(".") + Field.PropertyName;
				if (Field.IsValid())
				{
					OutFields.Add(MoveTemp(Field));
				}
			}
		}

		bool TryBuildFieldRecords(
			TArray<FFieldRecord>& OutFields,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutFields.Reset();
			TArray<FTargetRecord> Targets;
			if (!TryBuildTargetRecords(Targets, OutError))
			{
				return false;
			}
			for (const FTargetRecord& Target : Targets)
			{
				if (Target.Profile.ProfileId == TEXT("project_settings")
					&& Target.SettingsObject.IsValid())
				{
					BuildFieldRecordsForTarget(
						Target,
						*Target.SettingsObject.Get(),
						OutFields);
				}
			}
			OutFields.Sort(
				[](const FFieldRecord& Left, const FFieldRecord& Right)
				{
					return Left.FieldRef < Right.FieldRef;
				});
			return true;
		}

		FUeAgentDescriptorIdentity DescriptorIdentity(
			const FUeAgentEditorProjectSimpleEffectProductionProfile& Profile)
		{
			return FUeAgentDescriptorIdentity(
				Profile.DescriptorId,
				Profile.DescriptorVersion,
				Profile.DescriptorHash);
		}

		FUeAgentCandidateSummary TargetSummary(const FTargetRecord& Record)
		{
			return FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(
					TEXT("uai.editor_project.target:") + Record.Ref,
					TEXT("editor_project_target"),
					ProjectOwnerId(),
					Record.Ref,
					Record.Profile.ProfileId,
					Record.Revision),
				DescriptorIdentity(Record.Profile),
				TargetCatalogKind,
				Record.DisplayName,
				TEXT("Exact EditorProject authoring target"),
				{
					TEXT("EditorProject"),
					Record.Profile.ProfileId,
				});
		}

		FUeAgentCandidateSummary FieldSummary(const FFieldRecord& Field)
		{
			return FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(
					TEXT("uai.editor_project.settings_field:")
						+ Field.FieldRef,
					Field.TypeId,
					Field.TargetRef,
					Field.FieldRef,
					TEXT("settings_field"),
					Field.Revision),
				DescriptorIdentity(Field.Profile),
				FieldCatalogKind,
				Field.PropertyName,
				Field.Summary,
				{
					TEXT("EditorProject"),
					TEXT("ProjectSettings"),
				});
		}

		FUeAgentCatalogRevision TargetCatalogRevision(
			const TArray<FTargetRecord>& Records)
		{
			FString Input;
			for (const FTargetRecord& Record : Records)
			{
				Input += Record.Ref + TEXT("\n") + Record.Revision + TEXT("\n");
			}
			return FUeAgentCatalogRevision(
				EditorProjectCatalogSourceId,
				Sha256(Input),
				1);
		}

		FUeAgentCatalogRevision FieldCatalogRevision(
			const TArray<FFieldRecord>& Fields)
		{
			FString Input;
			for (const FFieldRecord& Field : Fields)
			{
				Input += Field.FieldRef + TEXT("\n")
					+ Field.Revision + TEXT("\n")
					+ Field.TypeId + TEXT("\n");
			}
			return FUeAgentCatalogRevision(
				EditorProjectCatalogSourceId,
				Sha256(Input),
				1);
		}

		FUeAgentCandidateBinding TargetBinding(
			const FTargetRecord& Record,
			const FUeAgentCatalogRevision& Revision)
		{
			return FUeAgentCandidateBinding(
				TEXT("editor-project-target:") + Sha256(Record.Ref).Mid(7),
				TargetSummary(Record),
				Revision,
				Record.Ref,
				TargetReadPortId,
				{EUeAgentCandidateOperation::Update},
				TEXT("editor-project-runtime:v1:")
					+ Record.Profile.ProfileId);
		}

		FUeAgentCandidateBinding FieldBinding(
			const FFieldRecord& Field,
			const FUeAgentCatalogRevision& Revision)
		{
			return FUeAgentCandidateBinding(
				TEXT("editor-project-field:")
					+ Sha256(Field.FieldRef).Mid(7),
				FieldSummary(Field),
				Revision,
				Field.FieldRef,
				FieldReadPortId,
				{EUeAgentCandidateOperation::Update},
				TEXT("editor-project-settings-field:v1:") + Field.TypeId);
		}

		bool Checkpoint(
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			const FString& Stage,
			FUeAgentDiscoveryError& OutError)
		{
			FUeAgentCancellationRequested Cancelled;
			if (!Cancellation.TryCheckpoint(Cancelled))
			{
				OutError = Cancelled;
				return false;
			}
			return true;
		}

		class FEditorProjectRuntimeOwner final
			: public IUeAgentCatalogSource
			, public IUeAgentEditorProjectSimpleEffectTargetResolver
			, public IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver
		{
		public:
			explicit FEditorProjectRuntimeOwner(
				const EResolverRole InRole,
				const TArray<FTargetRecord>& InitialTargets = {})
				: Role(InRole)
				, Kinds({FieldCatalogKind, TargetCatalogKind})
			{
				if (Role != EResolverRole::Persisted)
				{
					return;
				}
				for (const FTargetRecord& Record : InitialTargets)
				{
					UObject* Live = Record.SettingsObject.Get();
					if (Record.Profile.ProfileId
							!= TEXT("project_settings")
						|| !IsValid(Live))
					{
						continue;
					}
					TMap<FString, FString> BaselineValues;
					for (TFieldIterator<FProperty> It(
						Live->GetClass(),
						EFieldIteratorFlags::IncludeSuper);
						It;
						++It)
					{
						FProperty* Property = *It;
						FString TypeId;
						if (!Property
							|| !TrySettingsType(*Property, TypeId))
						{
							continue;
						}
						const void* Address =
							Property->ContainerPtrToValuePtr<void>(Live);
						FString Value;
						Property->ExportText_Direct(
							Value,
							Address,
							Address,
							Live,
							PPF_None);
						BaselineValues.Add(Property->GetName(), MoveTemp(Value));
					}
					InitialSettingsValues.Add(
						Record.Ref,
						MoveTemp(BaselineValues));
				}
			}

			const FString& GetSourceId() const override
			{
				return EditorProjectCatalogSourceId;
			}

			const TArray<FString>& GetCatalogKinds() const override
			{
				return Kinds;
			}

			bool TrySearch(
				const FUeAgentDiscoveryQuery& Query,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TSharedPtr<const FUeAgentCatalogSnapshot>& OutCatalog,
				FUeAgentDiscoveryError& OutError) override
			{
				OutCatalog.Reset();
				OutError = {};
				if (!Query.IsValid()
					|| !Kinds.Contains(Query.GetCatalogKind()))
				{
					OutError.Code = TEXT("editor_project_catalog_query_invalid");
					OutError.SubjectId = Query.GetCatalogKind();
					return false;
				}
				if (!Checkpoint(
						Deadline,
						Cancellation,
						TEXT("editor_project.catalog_search"),
						OutError))
				{
					return false;
				}
				const FString Search = Query.GetSearchText().TrimStartAndEnd();
				TArray<FUeAgentCandidateSummary> Candidates;
				UE::UAI::Contract::FDiagnostic BuildError;
				if (Query.GetCatalogKind() == TargetCatalogKind)
				{
					TArray<FTargetRecord> Records;
					if (!TryBuildTargetRecords(Records, BuildError))
					{
						OutError = BuildError;
						return false;
					}
					for (const FTargetRecord& Record : Records)
					{
						const FUeAgentCandidateSummary Summary =
							TargetSummary(Record);
						const bool bExact = Search == Record.Ref
							|| Search == Summary.GetIdentity().GetCanonicalKey();
						const bool bBroad = Search.IsEmpty()
							|| Record.SearchText.Contains(
								Search,
								ESearchCase::IgnoreCase);
						if ((Query.GetMatchMode()
								== EUeAgentDiscoveryMatchMode::Exact && bExact)
							|| (Query.GetMatchMode()
								== EUeAgentDiscoveryMatchMode::Broad && bBroad))
						{
							Candidates.Add(Summary);
						}
					}
					return FUeAgentCatalogSnapshot::TryCreate(
						TEXT("uai-editor-project-targets:")
							+ TargetCatalogRevision(Records).GetGeneration(),
						TargetCatalogKind,
						TargetCatalogRevision(Records),
						EUeAgentCatalogCompleteness::Complete,
						Candidates,
						{},
						Resources,
						OutCatalog,
						OutError);
				}

				TArray<FFieldRecord> Fields;
				if (!TryBuildFieldRecords(Fields, BuildError))
				{
					OutError = BuildError;
					return false;
				}
				for (const FFieldRecord& Field : Fields)
				{
					const FUeAgentCandidateSummary Summary = FieldSummary(Field);
					const bool bExact = Search == Field.FieldRef
						|| Search == Summary.GetIdentity().GetCanonicalKey();
					const bool bBroad = Search.IsEmpty()
						|| Field.FieldRef.Contains(Search, ESearchCase::IgnoreCase)
						|| Field.PropertyName.Contains(
							Search,
							ESearchCase::IgnoreCase)
						|| Field.Summary.Contains(Search, ESearchCase::IgnoreCase);
					if ((Query.GetMatchMode()
							== EUeAgentDiscoveryMatchMode::Exact && bExact)
						|| (Query.GetMatchMode()
							== EUeAgentDiscoveryMatchMode::Broad && bBroad))
					{
						Candidates.Add(Summary);
					}
				}
				const FUeAgentCatalogRevision Revision =
					FieldCatalogRevision(Fields);
				return FUeAgentCatalogSnapshot::TryCreate(
					TEXT("uai-editor-project-fields:")
						+ Revision.GetGeneration(),
					FieldCatalogKind,
					Revision,
					EUeAgentCatalogCompleteness::Complete,
					Candidates,
					{},
					Resources,
					OutCatalog,
					OutError);
			}

			bool TryResolveExact(
				const FString& CandidateId,
				const FUeAgentCatalogRevision& ExpectedRevision,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TSharedPtr<const FUeAgentCandidateBinding>& OutBinding,
				FUeAgentDiscoveryError& OutError) override
			{
				OutBinding.Reset();
				OutError = {};
				if (!Checkpoint(
						Deadline,
						Cancellation,
						TEXT("editor_project.catalog_resolve"),
						OutError))
				{
					return false;
				}
				UE::UAI::Contract::FDiagnostic BuildError;
				TOptional<FUeAgentCandidateBinding> Binding;
				TArray<FTargetRecord> Records;
				const bool bTargetsBuilt =
					TryBuildTargetRecords(Records, BuildError);
				const FUeAgentCatalogRevision TargetRevision =
					bTargetsBuilt
						? TargetCatalogRevision(Records)
						: FUeAgentCatalogRevision(
							TEXT("invalid"), TEXT("invalid"), -1);
				if (bTargetsBuilt && ExpectedRevision == TargetRevision)
				{
					for (const FTargetRecord& Record : Records)
					{
						if (TargetSummary(Record).GetIdentity().GetCanonicalKey()
							== CandidateId)
						{
							Binding.Emplace(
								TargetBinding(Record, ExpectedRevision));
							break;
						}
					}
				}
				else
				{
					TArray<FFieldRecord> Fields;
					if (!TryBuildFieldRecords(Fields, BuildError))
					{
						OutError = MoveTemp(BuildError);
						if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = CandidateId;
						return false;
					}
					if (ExpectedRevision != FieldCatalogRevision(Fields))
					{
						OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("catalog_revision_stale"), TEXT("The field catalog changed after the candidate was selected."), TEXT("binding"), FString(), CandidateId);
						return false;
					}
					for (const FFieldRecord& Field : Fields)
					{
						if (FieldSummary(Field).GetIdentity().GetCanonicalKey()
							== CandidateId)
						{
							Binding.Emplace(
								FieldBinding(Field, ExpectedRevision));
							break;
						}
					}
				}
				if (!Binding.IsSet() || !Binding->IsValid())
				{
					OutError.Code = TEXT("candidate_not_found");
					OutError.SubjectId = CandidateId;
					return false;
				}
				FUeAgentResourceChargeError ChargeError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::Items,
						1,
						TEXT("editor_project.catalog_resolve"),
						ChargeError))
				{
					OutError = ChargeError;
					return false;
				}
				OutBinding = MakeShared<FUeAgentCandidateBinding>(
					MoveTemp(Binding.GetValue()));
				return true;
			}

			bool TryResolveTarget(
				const FString& CanonicalRef,
				FUeAgentEditorProjectSimpleEffectResolvedTarget& OutTarget,
				FExecutionMutationError& OutError) override
			{
				OutTarget = FUeAgentEditorProjectSimpleEffectResolvedTarget();
				OutError = FExecutionMutationError();
				TArray<FTargetRecord> Records;
				UE::UAI::Contract::FDiagnostic BuildError;
				if (!TryBuildTargetRecords(Records, BuildError))
				{
					return FailMutation(OutError, BuildError);
				}
				const FTargetRecord* Record = Records.FindByPredicate(
					[&CanonicalRef](const FTargetRecord& Candidate)
					{
						return Candidate.Ref == CanonicalRef;
					});
				if (!Record)
				{
					OutError = FDiagnostic::Failure(TEXT("editor_project_target_unresolved"),
						TEXT("The requested target is not present in the current editor project catalog."), TEXT("simple_authoring"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = CanonicalRef;
					OutError.PropertyPath = TEXT("/target_exists");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return false;
				}

				FConfigFile* Config = nullptr;
				UObject* Settings = nullptr;
				UStringTable* StringTable = nullptr;
				TSet<FString>* LocalizationDirectories = nullptr;
				FString Revision = Record->Revision;
				if (Record->Profile.ProfileId == TEXT("project_settings"))
				{
					Settings = TryMaterializeSettings(*Record, OutError);
					if (!Settings)
					{
						return false;
					}
					Revision = TargetRevision(
						*Record,
						SettingsStateRevisionInput(*Settings));
				}
				else if (Record->Profile.ProfileId == TEXT("string_table"))
				{
					UObject* Object = StaticFindObject(
						UObject::StaticClass(),
						nullptr,
						*Record->StringTableObjectPath);
					if (!Object)
					{
						Object = StaticLoadObject(
							UObject::StaticClass(),
							nullptr,
							*Record->StringTableObjectPath,
							nullptr,
							LOAD_NoWarn);
					}
					StringTable = Cast<UStringTable>(Object);
					if (!StringTable)
					{
						OutError = FDiagnostic::Failure(TEXT("editor_project_string_table_unavailable"),
							TEXT("The target did not resolve to a String Table asset."), TEXT("simple_authoring"));
						OutError.SourceKind = TEXT("native_result");
						OutError.TargetPath = CanonicalRef;
						OutError.PropertyPath = TEXT("/asset_class");
						OutError.ActualValuePresent = Object != nullptr;
						if (Object) OutError.ActualValue = MakeShared<FJsonValueString>(Object->GetClass()->GetPathName());
						OutError.ExpectedValue = MakeShared<FJsonValueString>(UStringTable::StaticClass()->GetPathName());
						return false;
					}
					if (StringTable->GetPathName() != Record->StringTableObjectPath)
					{
						OutError = FDiagnostic::Failure(TEXT("editor_project_string_table_unavailable"),
							TEXT("The loaded String Table does not match the catalog's asset path."), TEXT("simple_authoring"));
						OutError.SourceKind = TEXT("native_result");
						OutError.TargetPath = CanonicalRef;
						OutError.PropertyPath = TEXT("/asset_path");
						OutError.ActualValue = MakeShared<FJsonValueString>(StringTable->GetPathName());
						OutError.ExpectedValue = MakeShared<FJsonValueString>(Record->StringTableObjectPath);
						return false;
					}
					if (StringTable->GetOutermost()->IsDirty())
					{
						OutError = FDiagnostic::Failure(TEXT("editor_project_string_table_unavailable"),
							TEXT("The String Table package has unsaved changes."), TEXT("simple_authoring"));
						OutError.SourceKind = TEXT("native_result");
						OutError.TargetPath = CanonicalRef;
						OutError.PropertyPath = TEXT("/package_dirty");
						OutError.ActualValue = MakeShared<FJsonValueBoolean>(true);
						OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
						return false;
					}
				}
				else
				{
					Config = TryMaterializeConfig(*Record, OutError);
					if (!Config)
					{
						return false;
					}
					Revision = TargetRevision(
						*Record,
						ConfigSectionRevisionInput(
							*Config,
							Record->ConfigSection));
					if (Record->Profile.ProfileId
						== TEXT("localization_target"))
					{
						FScopeLock Lock(&Mutex);
						TSet<FString>& Directories =
							LocalizationDirectoriesByRef.FindOrAdd(
								Record->Ref);
						Directories.Reset();
						if (const FConfigSection* Section =
							Config->FindSection(Record->ConfigSection))
						{
							TArray<FConfigValue> Values;
							Section->MultiFind(
								FName(TEXT("CulturesToGenerate")),
								Values,
								true);
							for (const FConfigValue& Value : Values)
							{
								Directories.Add(Value.GetValue());
							}
						}
						LocalizationDirectories = &Directories;
					}
				}
				OutTarget = FUeAgentEditorProjectSimpleEffectResolvedTarget(
					Record->Ref,
					Record->Profile.ProfileId,
					Revision,
					Config,
					Record->ConfigSection,
					Settings,
					StringTable,
					LocalizationDirectories);
				return OutTarget.IsValid()
					|| FailMutation(OutError, OutTarget.GetValidationDiagnostic());
			}

			bool TryResolveSettingsField(
				UObject& SettingsObject,
				const FString& PublicFieldRef,
				const FString& ExpectedRevision,
				FProperty*& OutProperty,
				FString& OutCurrentRevision,
				FExecutionMutationError& OutError) override
			{
				OutProperty = nullptr;
				OutCurrentRevision.Reset();
				OutError = FExecutionMutationError();
				FString SettingsTargetRef;
				{
					FScopeLock Lock(&Mutex);
					if (const FString* Found =
						SettingsTargetByObject.Find(&SettingsObject))
					{
						SettingsTargetRef = *Found;
					}
				}
				TArray<FTargetRecord> Targets;
				UE::UAI::Contract::FDiagnostic BuildError;
				if (!TryBuildTargetRecords(Targets, BuildError))
				{
					return FailMutation(OutError, BuildError);
				}
				const FTargetRecord* Target = nullptr;
				for (const FTargetRecord& Candidate : Targets)
				{
					if ((!SettingsTargetRef.IsEmpty()
							&& Candidate.Ref == SettingsTargetRef)
						|| (SettingsTargetRef.IsEmpty()
							&& Candidate.SettingsObject.Get() == &SettingsObject))
					{
						Target = &Candidate;
						break;
					}
				}
				if (!Target
					|| Target->Profile.ProfileId != TEXT("project_settings"))
				{
					OutError = FDiagnostic::Failure(TEXT("editor_project_settings_target_unresolved"),
						Target ? TEXT("The resolved target is not a project settings target.")
							: TEXT("The settings object is not registered as a project settings target."), TEXT("simple_authoring"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = SettingsTargetRef.IsEmpty() ? SettingsObject.GetClass()->GetPathName() : SettingsTargetRef;
					OutError.PropertyPath = TEXT("/profile_id");
					OutError.ActualValuePresent = Target != nullptr;
					if (Target) OutError.ActualValue = MakeShared<FJsonValueString>(Target->Profile.ProfileId);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("project_settings"));
					return false;
				}
				TArray<FFieldRecord> Fields;
				BuildFieldRecordsForTarget(*Target, SettingsObject, Fields);
				const FFieldRecord* Field = Fields.FindByPredicate(
					[&PublicFieldRef](const FFieldRecord& Candidate)
					{
						return Candidate.FieldRef == PublicFieldRef;
					});
				if (!Field)
				{
					OutError = FDiagnostic::Failure(TEXT("editor_project_settings_field_unresolved"),
						TEXT("The requested field is not registered for this settings target."), TEXT("simple_authoring"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Target->Ref;
					OutError.PropertyPath = PublicFieldRef;
					OutError.ActualValuePresent = false;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered settings field"));
					return false;
				}
				OutCurrentRevision = Field->Revision;
				if (ExpectedRevision != OutCurrentRevision)
				{
					OutError = FDiagnostic::Failure(TEXT("editor_project_settings_field_revision_stale"),
						TEXT("The settings field changed after the supplied revision was observed."), TEXT("simple_authoring"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = PublicFieldRef;
					OutError.PropertyPath = TEXT("/revision_matches");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedRevision);
					OutError.ActualEvidence = MakeShared<FJsonValueString>(OutCurrentRevision);
					return false;
				}
				OutProperty = FindFProperty<FProperty>(
					SettingsObject.GetClass(),
					FName(*Field->PropertyName));
				if (!OutProperty)
				{
					OutError = FDiagnostic::Failure(TEXT("editor_project_settings_field_unresolved"),
						TEXT("The registered settings field no longer has a native property."), TEXT("simple_authoring"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Target->Ref;
					OutError.PropertyPath = Field->PropertyName;
					OutError.ActualValuePresent = false;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Field->TypeId);
					return false;
				}
				return true;
			}

			bool TryReadSettingsFields(
				UObject& SettingsObject,
				TArray<
					FUeAgentEditorProjectSettingsFieldObservation>& OutFields,
				FExecutionMutationError& OutError) override
			{
				OutFields.Reset();
				OutError = FExecutionMutationError();
				TArray<FTargetRecord> Targets;
				UE::UAI::Contract::FDiagnostic BuildError;
				if (!TryBuildTargetRecords(Targets, BuildError))
				{
					return FailMutation(OutError, BuildError);
				}

				const FTargetRecord* Target = Targets.FindByPredicate(
					[&SettingsObject](const FTargetRecord& Candidate)
					{
						return Candidate.Profile.ProfileId
								== TEXT("project_settings")
							&& Candidate.SettingsObject.Get() == &SettingsObject;
					});
				if (!Target)
				{
					OutError = FDiagnostic::Failure(TEXT("editor_project_settings_target_unresolved"),
						TEXT("The settings object is not registered as a project settings target."), TEXT("simple_authoring"),
						SettingsObject.GetPathName());
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = SettingsObject.GetClass()->GetPathName();
					OutError.PropertyPath = TEXT("/target_exists");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return false;
				}

				TArray<FFieldRecord> Records;
				BuildFieldRecordsForTarget(*Target, SettingsObject, Records);
				for (const FFieldRecord& Record : Records)
				{
					FProperty* Property = FindFProperty<FProperty>(
						SettingsObject.GetClass(),
						FName(*Record.PropertyName));
					if (!Property)
					{
						OutError = FDiagnostic::Failure(TEXT("editor_project_settings_field_unresolved"),
							TEXT("The registered settings field no longer has a native property."), TEXT("simple_authoring"));
						OutError.SourceKind = TEXT("native_result");
						OutError.TargetPath = Target->Ref;
						OutError.PropertyPath = Record.PropertyName;
						OutError.ActualValuePresent = false;
						OutError.ExpectedValue = MakeShared<FJsonValueString>(Record.TypeId);
						return false;
					}
					const void* Address =
						Property->ContainerPtrToValuePtr<void>(&SettingsObject);
					FString CanonicalValue;
					Property->ExportText_Direct(
						CanonicalValue,
						Address,
						Address,
						&SettingsObject,
						PPF_None);

					FUeAgentEditorProjectSettingsFieldObservation Observation;
					Observation.FieldRef = Record.FieldRef;
					Observation.FieldRevision = Record.Revision;
					Observation.TypeId = Record.TypeId;
					Observation.CanonicalValue = MoveTemp(CanonicalValue);
					if (Observation.IsValid())
					{
						OutFields.Add(MoveTemp(Observation));
					}
				}
				return true;
			}

			bool TryResolveDurableBinding(
				const FUeAgentEditorProjectSimpleEffectResolvedTarget& Target,
				FUeAgentEditorProjectSimpleAuthoringDurableBinding& OutBinding,
				FExecutionMutationError& OutError) override
			{
				OutBinding = FUeAgentEditorProjectSimpleAuthoringDurableBinding();
				OutError = FExecutionMutationError();
				TArray<FTargetRecord> Records;
				UE::UAI::Contract::FDiagnostic BuildError;
				if (!Target.IsValid())
				{
					const FDiagnostic& Validation = Target.GetValidationDiagnostic();
					if (Validation.IsValid())
					{
						return FailMutation(OutError, Validation);
					}
					// A default, unresolved target has no constructor failure to propagate.
					OutError = FDiagnostic::Failure(TEXT("editor_project_target_invalid"),
						TEXT("Durable binding requires a resolved project target."), TEXT("simple_authoring"));
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = TEXT("/target_resolved");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return false;
				}
				if (!TryBuildTargetRecords(Records, BuildError))
				{
					return FailMutation(OutError, BuildError);
				}
				const FTargetRecord* Record = Records.FindByPredicate(
					[&Target](const FTargetRecord& Candidate)
					{
						return Candidate.Ref == Target.GetCanonicalRef()
							&& Candidate.Profile.ProfileId
								== Target.GetProfileId();
					});
				if (!Record)
				{
					OutError = FDiagnostic::Failure(TEXT("editor_project_durable_target_unresolved"),
						TEXT("The target and profile do not match a registered durable project target."), TEXT("simple_authoring"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Target.GetCanonicalRef();
					OutError.PropertyPath = TEXT("/profile_id");
					OutError.ActualValue = MakeShared<FJsonValueString>(Target.GetProfileId());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered target/profile pair"));
					return false;
				}
				const EDurableArtifactKind Kind =
					Record->Profile.ProfileId == TEXT("string_table")
						? EDurableArtifactKind::Package
						: EDurableArtifactKind::ProjectConfig;
				if (Kind == EDurableArtifactKind::ProjectConfig
					&& !IsUnderRoot(
						Record->DurableLocation,
						FPaths::ProjectConfigDir()))
				{
					OutError = FDiagnostic::Failure(TEXT("editor_project_durable_location_forbidden"),
						TEXT("The configuration target is outside the project's Config directory."), TEXT("simple_authoring"),
						Record->DurableLocation);
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Record->Ref;
					OutError.PropertyPath = TEXT("/inside_project_config");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return false;
				}
				OutBinding = FUeAgentEditorProjectSimpleAuthoringDurableBinding(
					Kind,
					Record->DurableLocation);
				return true;
			}

		private:
			FConfigFile* TryMaterializeConfig(
				const FTargetRecord& Record,
				FExecutionMutationError& OutError)
			{
				FScopeLock Lock(&Mutex);
				TUniquePtr<FConfigFile>& Stored =
					ConfigDocuments.FindOrAdd(Record.Ref);
				const bool bRetainMutation =
					Role == EResolverRole::Mutation
					&& Stored.IsValid()
					&& Stored->Dirty;
				if (!bRetainMutation)
				{
					TUniquePtr<FConfigFile> Loaded = MakeUnique<FConfigFile>();
					if (!TryLoadConfigFile(Record.DurableLocation, *Loaded, OutError,
						TEXT("editor_project_config_target_unavailable")))
					{
						OutError.TargetPath = Record.Ref;
						return nullptr;
					}
					if (!Loaded->FindSection(Record.ConfigSection))
					{
						OutError = FDiagnostic::Failure(TEXT("editor_project_config_target_unavailable"),
							TEXT("The configuration file no longer contains the requested section."),
							TEXT("simple_authoring"), Record.DurableLocation);
						OutError.SourceKind = TEXT("native_result");
						OutError.TargetPath = Record.Ref;
						OutError.PropertyPath = Record.ConfigSection;
						OutError.ActualValuePresent = false;
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("configuration section"));
						return nullptr;
					}
					Stored = MoveTemp(Loaded);
				}
				return Stored.Get();
			}

			UObject* TryMaterializeSettings(
				const FTargetRecord& Record,
				FExecutionMutationError& OutError)
			{
				UObject* Settings = Record.SettingsObject.Get();
				if (!IsValid(Settings))
				{
					OutError = FDiagnostic::Failure(TEXT("editor_project_settings_target_unavailable"),
						TEXT("The registered settings object is no longer valid."), TEXT("simple_authoring"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Record.Ref;
					OutError.PropertyPath = TEXT("/settings_object_valid");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return nullptr;
				}
				FScopeLock Lock(&Mutex);
				if (Role == EResolverRole::Persisted)
				{
					const TMap<FString, FString>* Baseline =
						InitialSettingsValues.Find(Record.Ref);
					UObject* Snapshot = NewObject<UObject>(
						GetTransientPackage(),
						Settings->GetClass(),
						NAME_None,
						RF_Transient);
					if (!Snapshot || !Baseline || Baseline->IsEmpty())
					{
						OutError = FDiagnostic::Failure(TEXT("editor_project_settings_reload_failed"),
							!Snapshot ? TEXT("A transient settings snapshot could not be created.")
								: !Baseline ? TEXT("The settings target has no captured baseline.")
								: TEXT("The captured settings baseline has no values."), TEXT("simple_authoring"));
						OutError.SourceKind = TEXT("native_result");
						OutError.TargetPath = Record.Ref;
						if (!Snapshot || !Baseline)
						{
							OutError.PropertyPath = !Snapshot ? TEXT("/snapshot_exists") : TEXT("/baseline_exists");
							OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
							OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
						}
						else
						{
							OutError.PropertyPath = TEXT("/baseline_value_count");
							OutError.ActualValue = MakeShared<FJsonValueNumber>(Baseline->Num());
							OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("greater than zero"));
						}
						return nullptr;
					}
					if (!TryReloadSettingsFromDisk(
							Record,
							*Baseline,
							*Snapshot,
							OutError))
					{
						return nullptr;
					}
					PersistedSettingsObjects.Add(
						Record.Ref,
						TStrongObjectPtr<UObject>(Snapshot));
					Settings = PersistedSettingsObjects.FindChecked(
						Record.Ref).Get();
				}
				SettingsTargetByObject.Add(Settings, Record.Ref);
				return Settings;
			}

			EResolverRole Role = EResolverRole::PreState;
			TArray<FString> Kinds;
			FCriticalSection Mutex;
			TMap<FString, TUniquePtr<FConfigFile>> ConfigDocuments;
			TMap<FString, TSet<FString>> LocalizationDirectoriesByRef;
			TMap<FString, TStrongObjectPtr<UObject>> PersistedSettingsObjects;
			TMap<FString, TMap<FString, FString>> InitialSettingsValues;
			TMap<UObject*, FString> SettingsTargetByObject;
		};
	}

	bool FUeAgentEditorProjectSimpleAuthoringRuntimeResolvers::IsValid() const
	{
		return CatalogSource.IsValid()
			&& MutationTargetResolver.IsValid()
			&& PreStateTargetResolver.IsValid()
			&& PersistedTargetResolver.IsValid()
			&& DurableBindingResolver.IsValid()
			&& MutationTargetResolver != PreStateTargetResolver
			&& MutationTargetResolver != PersistedTargetResolver
			&& PreStateTargetResolver != PersistedTargetResolver;
	}

	bool TryCreateEditorProjectSimpleAuthoringRuntimeResolvers(
		FUeAgentEditorProjectSimpleAuthoringRuntimeResolvers& OutResolvers,
		FUeAgentSimpleAuthoringRuntimeResolverError& OutError)
	{
		OutResolvers = FUeAgentEditorProjectSimpleAuthoringRuntimeResolvers();
		OutError = {};
		TArray<FTargetRecord> Targets;
		UE::UAI::Contract::FDiagnostic BuildError;
		if (!TryBuildTargetRecords(Targets, BuildError))
		{
			SetRuntimeError(OutError, BuildError, EditorProjectCatalogSourceId);
			return false;
		}
		const TSharedRef<FEditorProjectRuntimeOwner, ESPMode::ThreadSafe>
			PreOwner = MakeShared<
				FEditorProjectRuntimeOwner,
				ESPMode::ThreadSafe>(EResolverRole::PreState);
		const TSharedRef<FEditorProjectRuntimeOwner, ESPMode::ThreadSafe>
			MutationOwner = MakeShared<
				FEditorProjectRuntimeOwner,
				ESPMode::ThreadSafe>(EResolverRole::Mutation);
		const TSharedRef<FEditorProjectRuntimeOwner, ESPMode::ThreadSafe>
			PersistedOwner = MakeShared<
				FEditorProjectRuntimeOwner,
				ESPMode::ThreadSafe>(
					EResolverRole::Persisted,
					Targets);
		OutResolvers.CatalogSource =
			StaticCastSharedRef<IUeAgentCatalogSource>(PreOwner);
		OutResolvers.PreStateTargetResolver = StaticCastSharedRef<
			IUeAgentEditorProjectSimpleEffectTargetResolver>(PreOwner);
		OutResolvers.MutationTargetResolver = StaticCastSharedRef<
			IUeAgentEditorProjectSimpleEffectTargetResolver>(MutationOwner);
		OutResolvers.PersistedTargetResolver = StaticCastSharedRef<
			IUeAgentEditorProjectSimpleEffectTargetResolver>(PersistedOwner);
		OutResolvers.DurableBindingResolver = StaticCastSharedRef<
			IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver>(
				PreOwner);
		return true;
	}
}
