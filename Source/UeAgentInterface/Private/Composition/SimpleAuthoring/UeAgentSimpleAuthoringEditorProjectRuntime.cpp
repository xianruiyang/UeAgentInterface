#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeResolvers.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeIdentity.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
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

		void SetRuntimeError(
			FUeAgentSimpleAuthoringRuntimeResolverError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		bool FailMutation(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
		}

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
			FConfigFile& OutConfig)
		{
			OutConfig.Empty();
			const FString FullPath = NormalizeFullPath(Path);
			if (!IFileManager::Get().FileExists(*FullPath))
			{
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
			const FString& Text)
		{
			void* Address =
				Property.ContainerPtrToValuePtr<void>(&SettingsObject);
			if (const FStrProperty* StringProperty =
					CastField<FStrProperty>(&Property))
			{
				StringProperty->SetPropertyValue(Address, Text);
				return true;
			}
			return Property.ImportText_Direct(
				*Text,
				Address,
				&SettingsObject,
				PPF_None) != nullptr;
		}

		bool TryReloadSettingsFromDisk(
			const FTargetRecord& Record,
			const TMap<FString, FString>& BaselineValues,
			UObject& Snapshot,
			FExecutionMutationError& OutError)
		{
			FConfigFile PersistedConfig;
			if (!TryLoadConfigFile(
					Record.DurableLocation,
					PersistedConfig))
			{
				return FailMutation(
					OutError,
					TEXT("editor_project_settings_reload_failed"),
					Record.DurableLocation);
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
				if (!BaselineText
					|| !TryApplyPersistedSettingsText(
						*Property,
						Snapshot,
						*BaselineText))
				{
					return FailMutation(
						OutError,
						TEXT("editor_project_settings_baseline_invalid"),
						Property->GetName());
				}
				FString PersistedText;
				if (PersistedConfig.GetString(
						*Section,
						*Property->GetName(),
						PersistedText)
					&& !TryApplyPersistedSettingsText(
						*Property,
						Snapshot,
						PersistedText))
				{
					return FailMutation(
						OutError,
						TEXT("editor_project_settings_value_reload_failed"),
						Property->GetName());
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
			FString& OutError)
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
				if (!Profile.IsValid() || OutProfiles.Contains(Profile.ProfileId))
				{
					OutError = TEXT("editor_project_runtime_profile_invalid");
					return false;
				}
				OutProfiles.Add(Profile.ProfileId, Profile);
			}
			if (OutProfiles.Num() != 7)
			{
				OutError = TEXT("editor_project_runtime_profile_count_invalid");
				return false;
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
				if (!TryLoadConfigFile(Path, Config))
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
			const TMap<FString,
				FUeAgentEditorProjectSimpleEffectProductionProfile>& Profiles,
			TArray<FTargetRecord>& OutRecords)
		{
			const FUeAgentEditorProjectSimpleEffectProductionProfile* Profile =
				Profiles.Find(TEXT("project_settings"));
			ISettingsModule* SettingsModule =
				FModuleManager::LoadModulePtr<ISettingsModule>(TEXT("Settings"));
			if (!Profile || !SettingsModule)
			{
				return;
			}
			TArray<FName> ContainerNames;
			SettingsModule->GetContainerNames(ContainerNames);
			for (const FName& ContainerName : ContainerNames)
			{
				if (ContainerName != FName(TEXT("Project")))
				{
					continue;
				}
				const TSharedPtr<ISettingsContainer> Container =
					SettingsModule->GetContainer(ContainerName);
				if (!Container.IsValid())
				{
					continue;
				}
				TArray<TSharedPtr<ISettingsCategory>> Categories;
				Container->GetCategories(Categories);
				for (const TSharedPtr<ISettingsCategory>& Category : Categories)
				{
					if (!Category.IsValid())
					{
						continue;
					}
					TArray<TSharedPtr<ISettingsSection>> Sections;
					Category->GetSections(Sections, true);
					for (const TSharedPtr<ISettingsSection>& Section : Sections)
					{
						UObject* SettingsObject = Section.IsValid()
							? Section->GetSettingsObject().Get()
							: nullptr;
						if (!IsValid(SettingsObject))
						{
							continue;
						}
						UClass* SettingsClass = SettingsObject->GetClass();
						if (!SettingsClass
							|| SettingsClass->HasAnyClassFlags(
								CLASS_Abstract | CLASS_Deprecated
								| CLASS_NewerVersionExists)
							|| (SettingsClass->ClassWithin
								&& !GetTransientPackage()->IsA(
									SettingsClass->ClassWithin)))
						{
							continue;
						}
						bool bHasSupportedField = false;
						for (TFieldIterator<FProperty> It(
							SettingsObject->GetClass(),
							EFieldIteratorFlags::IncludeSuper);
							It;
							++It)
						{
							FString TypeId;
							if (*It && TrySettingsType(**It, TypeId))
							{
								bHasSupportedField = true;
								break;
							}
						}
						const FString Durable = NormalizeFullPath(
							SettingsObject->GetDefaultConfigFilename());
						if (!bHasSupportedField
							|| !IsUnderRoot(Durable, FPaths::ProjectConfigDir()))
						{
							continue;
						}
						const FString ContainerText = ContainerName.ToString();
						const FString CategoryText = Category->GetName().ToString();
						const FString SectionText = Section->GetName().ToString();
						FTargetRecord Record;
						Record.Profile = *Profile;
						Record.Ref = TargetRef(
							TEXT("project-settings"),
							{ContainerText, CategoryText, SectionText});
						Record.DurableLocation = Durable;
						Record.DisplayName = ContainerText + TEXT(" / ")
							+ CategoryText + TEXT(" / ") + SectionText;
						Record.SearchText = Record.Ref + TEXT(" ")
							+ Record.DisplayName + TEXT(" ")
							+ SettingsObject->GetClass()->GetPathName();
						Record.SettingsObject = SettingsObject;
						Record.Revision = TargetRevision(
							Record,
							SettingsStateRevisionInput(*SettingsObject));
						if (Record.IsValid())
						{
							OutRecords.Add(MoveTemp(Record));
						}
					}
				}
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
			if (!Profile || !TryLoadConfigFile(Path, Config))
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
				if (!TryLoadConfigFile(Path, Config)
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
					if (!TryLoadConfigFile(Path, Config))
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
			if (!Profile || !TryLoadConfigFile(Path, Config))
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
			FString& OutError)
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
					OutError = TEXT("editor_project_runtime_target_duplicate");
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
			FString& OutError)
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
				OutError.Code = Cancelled.Code;
				OutError.SubjectId = Cancelled.Reason;
				OutError.Detail = Stage;
				return false;
			}
			FUeAgentOperationDeadlineExceeded Expired;
			if (!Deadline.TryCheckpoint(Stage, Expired))
			{
				OutError.Code = Expired.Code;
				OutError.SubjectId = Stage;
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
				FString BuildError;
				if (Query.GetCatalogKind() == TargetCatalogKind)
				{
					TArray<FTargetRecord> Records;
					if (!TryBuildTargetRecords(Records, BuildError))
					{
						OutError.Code =
							TEXT("editor_project_target_catalog_unavailable");
						OutError.Detail = BuildError;
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
					OutError.Code =
						TEXT("editor_project_field_catalog_unavailable");
					OutError.Detail = BuildError;
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
				FString BuildError;
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
						OutError.Code =
							TEXT("editor_project_field_catalog_unavailable");
						OutError.SubjectId = CandidateId;
						OutError.Detail = BuildError;
						return false;
					}
					if (ExpectedRevision != FieldCatalogRevision(Fields))
					{
						OutError.Code = TEXT("catalog_revision_stale");
						OutError.SubjectId = CandidateId;
						OutError.Detail = BuildError;
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
				FUeAgentResourceBudgetExceeded BudgetError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::Items,
						1,
						TEXT("editor_project.catalog_resolve"),
						BudgetError))
				{
					OutError.Code = BudgetError.Code;
					OutError.SubjectId = BudgetError.Path;
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
				FString BuildError;
				if (!TryBuildTargetRecords(Records, BuildError))
				{
					return FailMutation(
						OutError,
						TEXT("editor_project_target_catalog_unavailable"),
						BuildError);
				}
				const FTargetRecord* Record = Records.FindByPredicate(
					[&CanonicalRef](const FTargetRecord& Candidate)
					{
						return Candidate.Ref == CanonicalRef;
					});
				if (!Record)
				{
					return FailMutation(
						OutError,
						TEXT("editor_project_target_unresolved"),
						CanonicalRef);
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
					if (!StringTable
						|| StringTable->GetPathName()
							!= Record->StringTableObjectPath
						|| StringTable->GetOutermost()->IsDirty())
					{
						return FailMutation(
							OutError,
							TEXT("editor_project_string_table_unavailable"),
							CanonicalRef);
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
					|| FailMutation(
						OutError,
						TEXT("editor_project_target_invalid"),
						CanonicalRef);
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
				FString BuildError;
				if (!TryBuildTargetRecords(Targets, BuildError))
				{
					return FailMutation(
						OutError,
						TEXT("editor_project_field_catalog_unavailable"),
						BuildError);
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
					return FailMutation(
						OutError,
						TEXT("editor_project_settings_target_unresolved"),
						PublicFieldRef);
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
					return FailMutation(
						OutError,
						TEXT("editor_project_settings_field_unresolved"),
						PublicFieldRef);
				}
				OutCurrentRevision = Field->Revision;
				if (ExpectedRevision != OutCurrentRevision)
				{
					return FailMutation(
						OutError,
						TEXT("editor_project_settings_field_revision_stale"),
						PublicFieldRef);
				}
				OutProperty = FindFProperty<FProperty>(
					SettingsObject.GetClass(),
					FName(*Field->PropertyName));
				return OutProperty
					|| FailMutation(
						OutError,
						TEXT("editor_project_settings_field_unresolved"),
						PublicFieldRef);
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
				FString BuildError;
				if (!TryBuildTargetRecords(Targets, BuildError))
				{
					return FailMutation(
						OutError,
						TEXT("editor_project_field_catalog_unavailable"),
						BuildError);
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
					return FailMutation(
						OutError,
						TEXT("editor_project_settings_target_unresolved"),
						SettingsObject.GetPathName());
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
						return FailMutation(
							OutError,
							TEXT("editor_project_settings_field_unresolved"),
							Record.FieldRef);
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
				FString BuildError;
				if (!Target.IsValid()
					|| !TryBuildTargetRecords(Records, BuildError))
				{
					return FailMutation(
						OutError,
						TEXT("editor_project_durable_target_unresolved"),
						BuildError);
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
					return FailMutation(
						OutError,
						TEXT("editor_project_durable_target_unresolved"),
						Target.GetCanonicalRef());
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
					return FailMutation(
						OutError,
						TEXT("editor_project_durable_location_forbidden"),
						Record->DurableLocation);
				}
				OutBinding = FUeAgentEditorProjectSimpleAuthoringDurableBinding(
					Kind,
					Record->DurableLocation);
				return OutBinding.IsValid();
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
					if (!TryLoadConfigFile(
							Record.DurableLocation,
							*Loaded)
						|| !Loaded->FindSection(Record.ConfigSection))
					{
						FailMutation(
							OutError,
							TEXT("editor_project_config_target_unavailable"),
							Record.Ref);
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
					FailMutation(
						OutError,
						TEXT("editor_project_settings_target_unavailable"),
						Record.Ref);
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
						FailMutation(
							OutError,
							TEXT("editor_project_settings_reload_failed"),
							Record.Ref);
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
		FString BuildError;
		if (!TryBuildTargetRecords(Targets, BuildError))
		{
			SetRuntimeError(
				OutError,
				TEXT("editor_project_runtime_catalog_unavailable"),
				EditorProjectCatalogSourceId,
				BuildError);
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
		if (!OutResolvers.IsValid())
		{
			SetRuntimeError(
				OutError,
				TEXT("editor_project_runtime_resolver_create_failed"));
			OutResolvers =
				FUeAgentEditorProjectSimpleAuthoringRuntimeResolvers();
			return false;
		}
		return true;
	}
}
