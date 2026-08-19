#include "DomainAdapters/LocalizationStringTable/UeAgentLocalizationStringTableHjsonProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/ArtifactPublisher/UeAgentVerifiedExport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "DomainAdapters/ContentGraphRead/UeAgentExternalLocalizationContentGraphProvider.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "LocalizationSettings.h"
#include "LocalizationTargetTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::LocalizationStringTable
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		const FString ProviderId = TEXT("localization-string-table-hjson-provider");
		const FString LocalizationSchema = TEXT("uai.localization.edit/1");
		const FString StringTableSchema = TEXT("uai.string_table.edit/1");
#if WITH_DEV_AUTOMATION_TESTS
		bool bFailStringTableSaveForTests = false;
#endif

		bool Fail(
			FWorkspaceError& OutError,
			FString Code,
			FString Source = FString(),
			FString Pointer = TEXT("/"))
		{
			OutError.Set(MoveTemp(Code), MoveTemp(Source), MoveTemp(Pointer));
			return false;
		}

		FString Sha256Bytes(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			SHA256(Bytes.GetData(), Bytes.Num(), Digest);
			return TEXT("sha256:")
				+ BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
		}

		FString Sha256Text(const FString& Text)
		{
			FTCHARToUTF8 Utf8(*Text);
			TArray<uint8> Bytes;
			Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			return Sha256Bytes(Bytes);
		}

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 16384},
				{EUeAgentResourceDimension::Items, 65536},
				{EUeAgentResourceDimension::Files, 4096},
				{EUeAgentResourceDimension::ReferenceEdges, 65536},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 256 * 1024 * 1024},
			}));
		}

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& Out,
			FWorkspaceError& OutError)
		{
			FString Error;
			if (!Object.IsValid() || !Json::TryCanonicalize(Object, Out, Error))
			{
				return Fail(OutError, TEXT("localization_hjson_json_invalid"), Error);
			}
			return true;
		}

		TSharedPtr<FJsonValue> JsonValueFromHjson(
			const FHjsonNode& Node,
			FWorkspaceError& OutError)
		{
			switch (Node.GetKind())
			{
			case EHjsonNodeKind::Null:
				return MakeShared<FJsonValueNull>();
			case EHjsonNodeKind::Boolean:
				return MakeShared<FJsonValueBoolean>(Node.GetBoolean());
			case EHjsonNodeKind::String:
				return MakeShared<FJsonValueString>(Node.GetText());
			case EHjsonNodeKind::Integer:
			case EHjsonNodeKind::Decimal:
				return MakeShared<FJsonValueNumber>(FCString::Atod(*Node.GetLexeme()));
			case EHjsonNodeKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const FHjsonNodeRef& Item : Node.GetItems())
				{
					TSharedPtr<FJsonValue> Converted = JsonValueFromHjson(*Item, OutError);
					if (!Converted.IsValid()) return nullptr;
					Items.Add(MoveTemp(Converted));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
			case EHjsonNodeKind::Object:
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				for (const TPair<FString, FHjsonNodeRef>& Entry : Node.GetEntries())
				{
					TSharedPtr<FJsonValue> Converted = JsonValueFromHjson(*Entry.Value, OutError);
					if (!Converted.IsValid()) return nullptr;
					Object->SetField(Entry.Key, MoveTemp(Converted));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			default:
				Fail(OutError, TEXT("localization_hjson_node_invalid"));
				return nullptr;
			}
		}

		bool ParseParams(
			const FString& CanonicalParamsJson,
			TSharedPtr<FJsonObject>& OutParams,
			FWorkspaceError& OutError)
		{
			FString Error;
			if (!Json::TryParseObject(CanonicalParamsJson, OutParams, Error)
				|| !OutParams.IsValid())
			{
				return Fail(OutError, TEXT("localization_hjson_params_invalid"), Error);
			}
			return true;
		}

		bool ParseDocument(
			const TSharedPtr<FJsonObject>& Params,
			const FString& ExpectedSchema,
			TSharedPtr<FJsonObject>& OutRoot,
			FWorkspaceError& OutError)
		{
			OutRoot.Reset();
			const TSharedPtr<FJsonObject>* Source = nullptr;
			FString Target;
			if (!Params.IsValid()
				|| Params->HasField(TEXT("carrier"))
				|| Params->HasField(TEXT("hjson_document"))
				|| !Params->TryGetStringField(TEXT("target"), Target)
				|| !Params->TryGetObjectField(TEXT("source"), Source)
				|| !Source || !Source->IsValid())
			{
				return Fail(OutError, TEXT("localization_hjson_resolved_source_required"));
			}
			FString Kind;
			FString TargetIdentity;
			FString EntryFile;
			const TArray<TSharedPtr<FJsonValue>>* Documents = nullptr;
			if (!(*Source)->TryGetStringField(TEXT("kind"), Kind)
				|| Kind != TEXT("resolved")
				|| !(*Source)->TryGetStringField(TEXT("target_identity"), TargetIdentity)
				|| TargetIdentity != Target
				|| !(*Source)->TryGetStringField(TEXT("entry_file"), EntryFile)
				|| !(*Source)->TryGetArrayField(TEXT("documents"), Documents)
				|| !Documents)
			{
				return Fail(OutError, TEXT("localization_hjson_resolved_source_invalid"));
			}
			FString DocumentText;
			int32 Matches = 0;
			for (const TSharedPtr<FJsonValue>& Value : *Documents)
			{
				const TSharedPtr<FJsonObject> Document = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Path;
				FString Text;
				if (Document.IsValid()
					&& Document->TryGetStringField(TEXT("path"), Path)
					&& Document->TryGetStringField(TEXT("document"), Text)
					&& Path == EntryFile)
				{
					DocumentText = MoveTemp(Text);
					++Matches;
				}
			}
			if (Matches != 1 || DocumentText.IsEmpty())
			{
				return Fail(OutError, TEXT("localization_hjson_entry_invalid"), EntryFile);
			}
			FUeAgentResourceTracker Resources = MakeResources();
			FHjsonDocumentPtr Document;
			if (!TryParseHjsonDocument(
					DocumentText,
					TEXT("inline:localization-string-table"),
					Resources,
					Document,
					OutError)
				|| !Document.IsValid())
			{
				return false;
			}
			const TSharedPtr<FJsonValue> Value = JsonValueFromHjson(*Document->GetRoot(), OutError);
			const TSharedPtr<FJsonObject> Parsed = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Schema;
			const TSharedPtr<FJsonObject>* Root = nullptr;
			if (!Parsed.IsValid()
				|| !Parsed->TryGetStringField(TEXT("schema"), Schema)
				|| Schema != ExpectedSchema
				|| !Parsed->TryGetObjectField(TEXT("root"), Root)
				|| !Root || !Root->IsValid())
			{
				return Fail(OutError, TEXT("localization_hjson_document_invalid"), Schema);
			}
			OutRoot = *Root;
			return true;
		}

		FString NormalizeObjectPath(const FString& Target)
		{
			return FPackageName::IsValidLongPackageName(Target)
				? Target + TEXT(".") + FPackageName::GetLongPackageAssetName(Target)
				: Target;
		}

		UStringTable* LoadStringTable(const FString& Target)
		{
			const FString ObjectPath = NormalizeObjectPath(Target);
			if (UStringTable* const Loaded = FindObject<UStringTable>(
					nullptr,
					*ObjectPath))
			{
				return Loaded;
			}
			const FString PackageName =
				FPackageName::ObjectPathToPackageName(ObjectPath);
			const FString Filename = FPackageName::LongPackageNameToFilename(
				PackageName,
				FPackageName::GetAssetPackageExtension());
			return FPaths::FileExists(Filename)
				? LoadObject<UStringTable>(nullptr, *ObjectPath)
				: nullptr;
		}

		FString PackageRevision(const FString& Target)
		{
			const FString PackageName = Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target) : Target;
			const FString Filename = FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension());
			TArray<uint8> Bytes;
			return FFileHelper::LoadFileToArray(Bytes, *Filename)
				? Sha256Bytes(Bytes) : FString(TEXT("absent"));
		}

		TSharedPtr<FJsonObject> BuildStringTableState(UStringTable& Table)
		{
			const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("type"), UStringTable::StaticClass()->GetPathName());
			State->SetStringField(TEXT("namespace"), Table.GetStringTable()->GetNamespace());
			TArray<TSharedPtr<FJsonValue>> Entries;
			Table.GetStringTable()->EnumerateSourceStrings(
				[&Entries](const FString& Key, const FString& Source)
				{
					const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("key"), Key);
					Entry->SetStringField(TEXT("source"), Source);
					Entries.Add(MakeShared<FJsonValueObject>(Entry));
					return true;
				});
			Entries.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
			{
				return A->AsObject()->GetStringField(TEXT("key"))
					< B->AsObject()->GetStringField(TEXT("key"));
			});
			State->SetArrayField(TEXT("entries"), Entries);
			State->SetNumberField(TEXT("entry_count"), Entries.Num());
			return State;
		}

		bool SaveStringTable(UStringTable& Table, FWorkspaceError& OutError)
		{
#if WITH_DEV_AUTOMATION_TESTS
			if (bFailStringTableSaveForTests)
			{
				return Fail(
					OutError,
					TEXT("string_table_save_failed"),
					TEXT("forced_by_automation_test"));
			}
#endif
			UPackage* Package = Table.GetOutermost();
			if (!Package)
			{
				return Fail(OutError, TEXT("string_table_package_missing"));
			}
			const FString Filename = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			if (!UPackage::SavePackage(Package, &Table, *Filename, Args))
			{
				return Fail(OutError, TEXT("string_table_save_failed"), Filename);
			}
			return true;
		}

		UStringTable* CreateStringTable(const FString& Target, FWorkspaceError& OutError)
		{
			FString PackageName = Target;
			if (Target.Contains(TEXT("."))) PackageName = FPackageName::ObjectPathToPackageName(Target);
			if (!FPackageName::IsValidLongPackageName(PackageName))
			{
				Fail(OutError, TEXT("string_table_target_invalid"), Target);
				return nullptr;
			}
			UPackage* Package = CreatePackage(*PackageName);
			const FString Name = FPackageName::GetLongPackageAssetName(PackageName);
			UStringTable* Table = NewObject<UStringTable>(
				Package,
				UStringTable::StaticClass(),
				*Name,
				RF_Public | RF_Standalone | RF_Transactional);
			if (!Table)
			{
				Fail(OutError, TEXT("string_table_create_failed"), Target);
				return nullptr;
			}
			return Table;
		}

		bool ImportStringTable(
			const FString& Target,
			const FString& ExpectedRevision,
			const TSharedPtr<FJsonObject>& Root,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FString Type;
			if (!Root.IsValid()
				|| !Root->TryGetStringField(TEXT("type"), Type)
				|| Type != UStringTable::StaticClass()->GetPathName())
			{
				return Fail(OutError, TEXT("string_table_document_type_invalid"));
			}
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (!Root->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
			{
				return Fail(OutError, TEXT("string_table_entries_required"));
			}
			TSet<FString> Keys;
			for (const TSharedPtr<FJsonValue>& Value : *Entries)
			{
				const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Key;
				FString Source;
				if (!Entry.IsValid()
					|| !Entry->TryGetStringField(TEXT("key"), Key)
					|| Key.IsEmpty()
					|| Keys.Contains(Key)
					|| !Entry->TryGetStringField(TEXT("source"), Source))
				{
					return Fail(OutError, TEXT("string_table_entry_invalid"), Key);
				}
				Keys.Add(Key);
			}
			const FString CurrentRevision = PackageRevision(Target);
			if (ExpectedRevision != CurrentRevision
				&& !(ExpectedRevision == TEXT("absent") && CurrentRevision == TEXT("absent")))
			{
				return Fail(OutError, TEXT("string_table_stale_revision"), CurrentRevision);
			}
			UStringTable* Table = LoadStringTable(Target);
			const bool bCreated = !Table;
			if (!Table) Table = CreateStringTable(Target, OutError);
			if (!Table) return false;
			struct FStringTableEntrySnapshot
			{
				FString Key;
				FString Source;
				TArray<TPair<FName, FString>> MetaData;
			};
			FString PreviousNamespace;
			TArray<FStringTableEntrySnapshot> PreviousEntries;
			if (!bCreated)
			{
				PreviousNamespace = Table->GetStringTable()->GetNamespace();
				Table->GetStringTable()->EnumerateSourceStrings(
					[&Table, &PreviousEntries](
						const FString& Key,
						const FString& Source)
					{
						FStringTableEntrySnapshot Snapshot;
						Snapshot.Key = Key;
						Snapshot.Source = Source;
						Table->GetStringTable()->EnumerateMetaData(
							Key,
							[&Snapshot](
								const FName MetaDataId,
								const FString& MetaDataValue)
							{
								Snapshot.MetaData.Emplace(
									MetaDataId,
									MetaDataValue);
								return true;
							});
						PreviousEntries.Add(MoveTemp(Snapshot));
						return true;
					});
			}
			UPackage* const Package = Table->GetOutermost();
			const bool bPackageWasDirty = Package && Package->IsDirty();
			const FString PackageFilename = Package
				? FPackageName::LongPackageNameToFilename(
					Package->GetName(),
					FPackageName::GetAssetPackageExtension())
				: FString();
			const FString BackupFilename = PackageFilename
				+ TEXT(".uai-t30-backup-")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const bool bHadPackageFile =
				!PackageFilename.IsEmpty() && FPaths::FileExists(PackageFilename);
			if (bHadPackageFile
				&& !IFileManager::Get().Move(
					*BackupFilename,
					*PackageFilename,
					true,
					true,
					false,
					true))
			{
				if (bCreated)
				{
					Table->ClearFlags(RF_Public | RF_Standalone);
					Table->MarkAsGarbage();
				}
				return Fail(
					OutError,
					TEXT("string_table_backup_failed"),
					PackageFilename);
			}
			Table->Modify();
			FString Namespace;
			Root->TryGetStringField(TEXT("namespace"), Namespace);
			FStringTable& Mutable = *Table->GetMutableStringTable();
			Mutable.SetNamespace(Namespace);
			Mutable.ClearSourceStrings();
			Mutable.ClearMetaData();
			for (const TSharedPtr<FJsonValue>& Value : *Entries)
			{
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				Mutable.SetSourceString(
					Entry->GetStringField(TEXT("key")),
					Entry->GetStringField(TEXT("source")));
			}
			Table->MarkPackageDirty();
			if (!SaveStringTable(*Table, OutError))
			{
				const FString SaveFailureCode = OutError.GetCode();
				bool bRolledBack = true;
				if (!PackageFilename.IsEmpty()
					&& FPaths::FileExists(PackageFilename))
				{
					bRolledBack = IFileManager::Get().Delete(
						*PackageFilename, false, true, true);
				}
				if (bHadPackageFile)
				{
					bRolledBack = IFileManager::Get().Move(
						*PackageFilename,
						*BackupFilename,
						true,
						true,
						false,
						true) && bRolledBack;
				}
				if (bCreated)
				{
					const FName TransientName = MakeUniqueObjectName(
						GetTransientPackage(),
						UStringTable::StaticClass(),
						TEXT("UAI_T30_FailedStringTable"));
					bRolledBack = Table->Rename(
						*TransientName.ToString(),
						GetTransientPackage(),
						REN_DontCreateRedirectors
							| REN_NonTransactional
							| REN_ForceNoResetLoaders) && bRolledBack;
					FStringTableRegistry::Get().UnregisterStringTable(
						Table->GetStringTableId());
					Table->ClearFlags(RF_Public | RF_Standalone);
					Table->MarkAsGarbage();
				}
				else
				{
					FStringTable& Restored = *Table->GetMutableStringTable();
					Restored.SetNamespace(PreviousNamespace);
					Restored.ClearSourceStrings();
					Restored.ClearMetaData();
					for (const FStringTableEntrySnapshot& Snapshot
						: PreviousEntries)
					{
						Restored.SetSourceString(Snapshot.Key, Snapshot.Source);
						for (const TPair<FName, FString>& MetaData
							: Snapshot.MetaData)
						{
							Restored.SetMetaData(
								Snapshot.Key,
								MetaData.Key,
								MetaData.Value);
						}
					}
				}
				if (Package)
				{
					Package->SetDirtyFlag(bPackageWasDirty);
				}
				return Fail(
					OutError,
					bRolledBack
						? SaveFailureCode
						: FString(TEXT("string_table_rollback_failed_state_unknown")),
					PackageFilename);
			}
			if (bHadPackageFile
				&& !IFileManager::Get().Delete(
					*BackupFilename, false, true, true))
			{
				return Fail(
					OutError,
					TEXT("string_table_commit_cleanup_failed_state_unknown"),
					BackupFilename);
			}
			if (bCreated)
			{
				FAssetRegistryModule::AssetCreated(Table);
			}
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Response->SetStringField(TEXT("provider_id"), ProviderId);
			Response->SetStringField(TEXT("target"), Target);
			Response->SetBoolField(TEXT("created"), bCreated);
			Response->SetBoolField(TEXT("saved"), true);
			Response->SetStringField(TEXT("revision"), PackageRevision(Target));
			Response->SetObjectField(TEXT("independent_readback"), BuildStringTableState(*Table));
			return CanonicalObject(Response, OutPayload, OutError);
		}

		FString LocalizationConfigRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("Localization"));
		}

		TSharedPtr<FJsonObject> BuildLocalizationState()
		{
			const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("type"), ULocalizationSettings::StaticClass()->GetPathName());
			TArray<TSharedPtr<FJsonValue>> Targets;
			if (ULocalizationTargetSet* Set = ULocalizationSettings::GetGameTargetSet())
			{
				for (const ULocalizationTarget* Target : Set->TargetObjects)
				{
					if (!Target) continue;
					const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("operation"), TEXT("update"));
					Item->SetStringField(TEXT("name"), Target->Settings.Name);
					TArray<TSharedPtr<FJsonValue>> Cultures;
					for (const FCultureStatistics& Culture : Target->Settings.SupportedCulturesStatistics)
					{
						Cultures.Add(MakeShared<FJsonValueString>(Culture.CultureName));
					}
					Item->SetArrayField(TEXT("cultures"), Cultures);
					if (Target->Settings.SupportedCulturesStatistics.IsValidIndex(Target->Settings.NativeCultureIndex))
					{
						Item->SetStringField(
							TEXT("native_culture"),
							Target->Settings.SupportedCulturesStatistics[
								Target->Settings.NativeCultureIndex].CultureName);
					}
					Targets.Add(MakeShared<FJsonValueObject>(Item));
				}
			}
			Targets.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
			{
				return A->AsObject()->GetStringField(TEXT("name"))
					< B->AsObject()->GetStringField(TEXT("name"));
			});
			State->SetArrayField(TEXT("targets"), Targets);
			return State;
		}

		FString LocalizationRevision()
		{
			FWorkspaceError Error;
			FString Json;
			return CanonicalObject(BuildLocalizationState(), Json, Error)
				? Sha256Text(Json) : FString(TEXT("unavailable"));
		}

		bool ImportLocalization(
			const FString& ExpectedRevision,
			const TSharedPtr<FJsonObject>& Root,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			struct FTargetMutation
			{
				FString Operation;
				FString Name;
				FString NativeCulture;
				TArray<FString> Cultures;
				TArray<FString> EffectiveCultures;
				FString EffectiveNativeCulture;
				FString ConfigText;
				bool bHasNativeCulture = false;
				bool bHasCultures = false;
				ULocalizationTarget* ExistingTarget = nullptr;
				ULocalizationTarget* TargetObject = nullptr;
			};

			FString Type;
			const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
			if (!Root.IsValid()
				|| !Root->TryGetStringField(TEXT("type"), Type)
				|| Type != ULocalizationSettings::StaticClass()->GetPathName()
				|| !Root->TryGetArrayField(TEXT("targets"), Targets)
				|| !Targets)
			{
				return Fail(OutError, TEXT("localization_document_invalid"));
			}
			const FString CurrentRevision = LocalizationRevision();
			if (ExpectedRevision != CurrentRevision)
			{
				return Fail(OutError, TEXT("localization_stale_revision"), CurrentRevision);
			}
			ULocalizationTargetSet* Set = ULocalizationSettings::GetGameTargetSet();
			if (!Set) return Fail(OutError, TEXT("localization_target_set_unavailable"));

			TSet<FString> Names;
			TArray<FTargetMutation> Mutations;
			for (const TSharedPtr<FJsonValue>& Value : *Targets)
			{
				const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Operation;
				FString Name;
				if (!Item.IsValid()
					|| !Item->TryGetStringField(TEXT("operation"), Operation)
					|| Operation.IsEmpty()
					|| !Item->TryGetStringField(TEXT("name"), Name)
					|| Name.IsEmpty()
					|| Names.Contains(Name)
					|| Name.Contains(TEXT("/"))
					|| Name.Contains(TEXT("\\"))
					|| Name.Contains(TEXT(".."))
					|| Name.Contains(TEXT(":")))
				{
					return Fail(
						OutError,
						Operation.IsEmpty()
							? TEXT("localization_target_operation_required")
							: TEXT("localization_target_invalid"),
						Name);
				}
				if (Operation != TEXT("create")
					&& Operation != TEXT("update")
					&& Operation != TEXT("upsert")
					&& Operation != TEXT("remove"))
				{
					return Fail(
						OutError,
						TEXT("localization_target_operation_invalid"),
						Operation);
				}
				Names.Add(Name);

				FTargetMutation Mutation;
				Mutation.Operation = Operation;
				Mutation.Name = Name;
				const TObjectPtr<ULocalizationTarget>* Found =
					Set->TargetObjects.FindByPredicate(
						[&Name](const ULocalizationTarget* Candidate)
						{
							return Candidate && Candidate->Settings.Name == Name;
						});
				Mutation.ExistingTarget = Found ? Found->Get() : nullptr;
				if (Operation == TEXT("create") && Mutation.ExistingTarget)
				{
					return Fail(
						OutError,
						TEXT("localization_target_already_exists"),
						Name);
				}
				if ((Operation == TEXT("update") || Operation == TEXT("remove"))
					&& !Mutation.ExistingTarget)
				{
					return Fail(
						OutError,
						TEXT("localization_target_not_found"),
						Name);
				}
				if (Operation != TEXT("remove"))
				{
					Mutation.bHasNativeCulture = Item->TryGetStringField(
						TEXT("native_culture"), Mutation.NativeCulture);
					const TArray<TSharedPtr<FJsonValue>>* Cultures = nullptr;
					Mutation.bHasCultures = Item->TryGetArrayField(
						TEXT("cultures"), Cultures) && Cultures;
					const bool bCreatesTarget = !Mutation.ExistingTarget;
					if ((bCreatesTarget
							&& (!Mutation.bHasNativeCulture
								|| !Mutation.bHasCultures))
						|| (Mutation.bHasNativeCulture
							&& Mutation.NativeCulture.IsEmpty()))
					{
						return Fail(
							OutError,
							TEXT("localization_target_state_incomplete"),
							Name);
					}
					if (Mutation.bHasCultures)
					{
						TSet<FString> UniqueCultures;
						for (const TSharedPtr<FJsonValue>& CultureValue : *Cultures)
						{
							FString Culture;
							if (!CultureValue.IsValid()
								|| !CultureValue->TryGetString(Culture)
								|| Culture.IsEmpty()
								|| UniqueCultures.Contains(Culture))
							{
								return Fail(
									OutError,
									TEXT("localization_target_cultures_invalid"),
									Name);
							}
							UniqueCultures.Add(Culture);
							Mutation.Cultures.Add(MoveTemp(Culture));
						}
					}
					if (Mutation.bHasCultures)
					{
						Mutation.EffectiveCultures = Mutation.Cultures;
					}
					else if (Mutation.ExistingTarget)
					{
						for (const FCultureStatistics& Culture
							: Mutation.ExistingTarget->Settings.SupportedCulturesStatistics)
						{
							Mutation.EffectiveCultures.Add(Culture.CultureName);
						}
					}
					Mutation.EffectiveNativeCulture = Mutation.NativeCulture;
					if (!Mutation.bHasNativeCulture && Mutation.ExistingTarget
						&& Mutation.ExistingTarget->Settings.SupportedCulturesStatistics.IsValidIndex(
							Mutation.ExistingTarget->Settings.NativeCultureIndex))
					{
						Mutation.EffectiveNativeCulture =
							Mutation.ExistingTarget->Settings.SupportedCulturesStatistics[
								Mutation.ExistingTarget->Settings.NativeCultureIndex].CultureName;
					}
					if (Mutation.EffectiveNativeCulture.IsEmpty()
						|| !Mutation.EffectiveCultures.Contains(
							Mutation.EffectiveNativeCulture))
					{
						return Fail(
							OutError,
							TEXT("localization_target_native_culture_missing"),
							Mutation.EffectiveNativeCulture);
					}
					Mutation.ConfigText += TEXT("[CommonSettings]") LINE_TERMINATOR;
					Mutation.ConfigText += FString::Printf(
						TEXT("SourcePath=Content/Localization/%s%s"),
						*Mutation.Name,
						LINE_TERMINATOR);
					Mutation.ConfigText += FString::Printf(
						TEXT("DestinationPath=Content/Localization/%s%s"),
						*Mutation.Name,
						LINE_TERMINATOR);
					Mutation.ConfigText += FString::Printf(
						TEXT("NativeCulture=%s%s"),
						*Mutation.EffectiveNativeCulture,
						LINE_TERMINATOR);
					for (const FString& Culture : Mutation.EffectiveCultures)
					{
						Mutation.ConfigText += FString::Printf(
							TEXT("+CulturesToGenerate=%s%s"),
							*Culture,
							LINE_TERMINATOR);
					}
				}
				Mutations.Add(MoveTemp(Mutation));
			}
			for (FTargetMutation& Mutation : Mutations)
			{
				Mutation.TargetObject = Mutation.ExistingTarget;
				if (Mutation.Operation != TEXT("remove")
					&& !Mutation.TargetObject)
				{
					Mutation.TargetObject = NewObject<ULocalizationTarget>(Set);
					if (!Mutation.TargetObject)
					{
						for (FTargetMutation& Pending : Mutations)
						{
							if (Pending.TargetObject
								&& Pending.TargetObject != Pending.ExistingTarget)
							{
								Pending.TargetObject->MarkAsGarbage();
							}
						}
						return Fail(
							OutError,
							TEXT("localization_target_create_failed"),
							Mutation.Name);
					}
				}
			}

			struct FFileChange
			{
				FString FinalConfig;
				FString StagedConfig;
				FString BackupConfig;
				FString ContentPath;
				FString BackupContent;
				bool bConfigBackedUp = false;
				bool bContentBackedUp = false;
				bool bConfigPublished = false;
			};
			const FString TransactionId =
				FGuid::NewGuid().ToString(EGuidFormats::Digits);
			TArray<FFileChange> FileChanges;
			FileChanges.SetNum(Mutations.Num());
			IFileManager& FileManager = IFileManager::Get();
			IPlatformFile& PlatformFile =
				FPlatformFileManager::Get().GetPlatformFile();
			FileManager.MakeDirectory(*LocalizationConfigRoot(), true);

			auto DiscardPendingTargets = [&Mutations]()
			{
				for (FTargetMutation& Mutation : Mutations)
				{
					if (Mutation.TargetObject
						&& Mutation.TargetObject != Mutation.ExistingTarget)
					{
						Mutation.TargetObject->MarkAsGarbage();
						Mutation.TargetObject = nullptr;
					}
				}
			};
			auto RemoveStagedFiles = [&FileChanges, &FileManager]()
			{
				for (const FFileChange& Change : FileChanges)
				{
					if (!Change.StagedConfig.IsEmpty())
					{
						FileManager.Delete(
							*Change.StagedConfig, false, true, true);
					}
				}
			};
			for (int32 Index = 0; Index < Mutations.Num(); ++Index)
			{
				const FTargetMutation& Mutation = Mutations[Index];
				FFileChange& Change = FileChanges[Index];
				Change.FinalConfig = LocalizationConfigRoot()
					/ (Mutation.Name + TEXT(".ini"));
				Change.BackupConfig = Change.FinalConfig
					+ TEXT(".uai-t30-backup-") + TransactionId;
				Change.ContentPath = FPaths::ProjectContentDir()
					/ TEXT("Localization") / Mutation.Name;
				Change.BackupContent = Change.ContentPath
					+ TEXT(".uai-t30-backup-") + TransactionId;
				if (Mutation.Operation != TEXT("remove"))
				{
					Change.StagedConfig = Change.FinalConfig
						+ TEXT(".uai-t30-staged-") + TransactionId;
					if (!FFileHelper::SaveStringToFile(
							Mutation.ConfigText,
							*Change.StagedConfig,
							FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
					{
						RemoveStagedFiles();
						DiscardPendingTargets();
						return Fail(
							OutError,
							TEXT("localization_config_stage_failed"),
							Change.StagedConfig);
					}
				}
			}

			auto RollbackFiles = [&FileChanges, &FileManager, &PlatformFile]()
			{
				bool bRestored = true;
				for (int32 Index = FileChanges.Num() - 1; Index >= 0; --Index)
				{
					FFileChange& Change = FileChanges[Index];
					if (Change.bConfigPublished)
					{
						bRestored = FileManager.Delete(
							*Change.FinalConfig, false, true, true) && bRestored;
					}
					if (Change.bConfigBackedUp)
					{
						bRestored = FileManager.Move(
							*Change.FinalConfig,
							*Change.BackupConfig,
							true,
							true,
							false,
							true) && bRestored;
					}
					if (Change.bContentBackedUp)
					{
						bRestored = PlatformFile.CopyDirectoryTree(
							*Change.ContentPath,
							*Change.BackupContent,
							true) && bRestored;
						FileManager.DeleteDirectory(
							*Change.BackupContent, false, true);
					}
					if (!Change.StagedConfig.IsEmpty())
					{
						FileManager.Delete(
							*Change.StagedConfig, false, true, true);
					}
				}
				return bRestored;
			};
			auto FailCommit = [&OutError, &RollbackFiles, &DiscardPendingTargets](
				const FString& Code,
				const FString& Source)
			{
				const bool bRolledBack = RollbackFiles();
				DiscardPendingTargets();
				return Fail(
					OutError,
					bRolledBack
						? Code
						: FString(TEXT("localization_rollback_failed_state_unknown")),
					Source);
			};

			for (int32 Index = 0; Index < Mutations.Num(); ++Index)
			{
				const FTargetMutation& Mutation = Mutations[Index];
				FFileChange& Change = FileChanges[Index];
				if (FPaths::FileExists(Change.FinalConfig))
				{
					if (!FileManager.Move(
							*Change.BackupConfig,
							*Change.FinalConfig,
							true,
							true,
							false,
							true))
					{
						return FailCommit(
							TEXT("localization_config_backup_failed"),
							Change.FinalConfig);
					}
					Change.bConfigBackedUp = true;
				}
				if (Mutation.Operation == TEXT("remove"))
				{
					if (FileManager.DirectoryExists(*Change.ContentPath))
					{
						if (!PlatformFile.CopyDirectoryTree(
								*Change.BackupContent,
								*Change.ContentPath,
								true))
						{
							FileManager.DeleteDirectory(
								*Change.BackupContent, false, true);
							return FailCommit(
								TEXT("localization_content_backup_failed"),
								Change.ContentPath);
						}
						Change.bContentBackedUp = true;
						if (!FileManager.DeleteDirectory(
								*Change.ContentPath, false, true))
						{
							return FailCommit(
								TEXT("localization_content_delete_failed"),
								Change.ContentPath);
						}
					}
				}
				else
				{
					if (!FileManager.Move(
							*Change.FinalConfig,
							*Change.StagedConfig,
							true,
							true,
							false,
							true))
					{
						return FailCommit(
							TEXT("localization_config_publish_failed"),
							Change.FinalConfig);
					}
					Change.bConfigPublished = true;
				}
			}

			Set->Modify();
			for (FTargetMutation& Mutation : Mutations)
			{
				if (Mutation.Operation == TEXT("remove"))
				{
					Set->TargetObjects.Remove(Mutation.ExistingTarget);
					continue;
				}
				ULocalizationTarget* const Target = Mutation.TargetObject;
				if (!Mutation.ExistingTarget)
				{
					Set->TargetObjects.Add(Target);
				}
				Target->Modify();
				Target->Settings.Name = Mutation.Name;
				Target->Settings.SupportedCulturesStatistics.Reset();
				for (const FString& Culture : Mutation.EffectiveCultures)
				{
					Target->Settings.SupportedCulturesStatistics.Add(
						FCultureStatistics(Culture));
				}
				Target->Settings.NativeCultureIndex =
					Target->Settings.SupportedCulturesStatistics.IndexOfByPredicate(
						[&Mutation](const FCultureStatistics& Culture)
						{
							return Culture.CultureName
								== Mutation.EffectiveNativeCulture;
						});
			}
			GetMutableDefault<ULocalizationSettings>()->SaveConfig();
			bool bCleanupComplete = true;
			for (const FFileChange& Change : FileChanges)
			{
				if (Change.bConfigBackedUp)
				{
					bCleanupComplete = FileManager.Delete(
						*Change.BackupConfig, false, true, true)
						&& bCleanupComplete;
				}
				if (Change.bContentBackedUp)
				{
					bCleanupComplete = FileManager.DeleteDirectory(
						*Change.BackupContent, false, true)
						&& bCleanupComplete;
				}
			}
			if (!bCleanupComplete)
			{
				return Fail(
					OutError,
					TEXT("localization_commit_cleanup_failed_state_unknown"));
			}
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Response->SetStringField(TEXT("provider_id"), ProviderId);
			Response->SetBoolField(TEXT("saved"), true);
			Response->SetStringField(TEXT("revision"), LocalizationRevision());
			Response->SetObjectField(TEXT("independent_readback"), BuildLocalizationState());
			return CanonicalObject(Response, OutPayload, OutError);
		}

		FExportSemanticSignature Signature(
			const FString& Target,
			const FString& RootType,
			const FString& FileHash,
			const FString& Schema)
		{
			return FExportSemanticSignature(
				Sha256Text(TEXT("content.hjson")),
				Sha256Text(Target + TEXT("|") + RootType),
				FileHash,
				Sha256Text(Schema),
				Sha256Text(RootType + TEXT("|") + FileHash));
		}

		class FVerifiedSource final : public IVerifiedExportWorkspaceSource
		{
		public:
			FVerifiedSource(
				FString InTarget,
				FString InRootType,
				FString InSchema,
				FString InDocument,
				TFunction<FString()> InRevision)
				: Target(MoveTemp(InTarget))
				, RootType(MoveTemp(InRootType))
				, Schema(MoveTemp(InSchema))
				, Document(MoveTemp(InDocument))
				, Revision(MoveTemp(InRevision))
			{
			}

			bool TryFreezeTarget(FUeAgentStateRevision& OutRevision, FArtifactPublishError& OutError) override
			{
				const FString Value = Revision();
				if (Value.IsEmpty() || Value == TEXT("absent"))
				{
					OutError.Set(TEXT("localization_export_target_missing"), Target);
					return false;
				}
				OutRevision = FUeAgentStateRevision(TEXT("localization-hjson-export"), Value, 1);
				return true;
			}

			bool TryCaptureWorkspace(
				const FUeAgentStateRevision& FrozenRevision,
				FExportWorkspacePlanPtr& OutPlan,
				FExportSemanticSignature& OutSemantic,
				FArtifactPublishError& OutError) override
			{
				FTCHARToUTF8 Utf8(*Document);
				TArray<uint8> Bytes;
				Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
				const FString Hash = Sha256Bytes(Bytes);
				OutSemantic = Signature(Target, RootType, Hash, Schema);
				OutPlan = MakeShared<FExportWorkspacePlan>(
					EWorkspaceKind::Directory,
					TEXT("content.hjson"),
					Target,
					FrozenRevision,
					Sha256Text(Target + TEXT("|") + Hash),
					TArray<FExportFilePlan>{FExportFilePlan(TEXT("content.hjson"), Bytes, Hash)},
					Sha256Text(Hash + TEXT("|") + FrozenRevision.GetGeneration()),
					FMath::Max<int64>(1, Bytes.Num() / 4));
				return true;
			}

			bool TryRebuildStagedWorkspace(
				const FString& StagedWorkspacePath,
				EWorkspaceKind WorkspaceKind,
				const FString& EntryFile,
				FUeAgentResourceTracker& Resources,
				FExportSemanticSignature& OutSemantic,
				FArtifactPublishError& OutError) override
			{
				TArray<uint8> Bytes;
				if (!FFileHelper::LoadFileToArray(Bytes, *(StagedWorkspacePath / EntryFile)))
				{
					OutError.Set(TEXT("localization_export_rebuild_failed"), StagedWorkspacePath / EntryFile);
					return false;
				}
				OutSemantic = Signature(Target, RootType, Sha256Bytes(Bytes), Schema);
				return true;
			}

			bool TryReadCurrentTargetRevision(FUeAgentStateRevision& OutRevision, FArtifactPublishError& OutError) override
			{
				return TryFreezeTarget(OutRevision, OutError);
			}

		private:
			FString Target;
			FString RootType;
			FString Schema;
			FString Document;
			TFunction<FString()> Revision;
		};

		bool ExportWorkspace(
			const TSharedPtr<FJsonObject>& Params,
			const FString& Target,
			const FString& RootType,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FString WorkspacePath;
			if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
				|| WorkspacePath.IsEmpty())
			{
				return Fail(OutError, TEXT("localization_export_path_required"));
			}
			TSharedPtr<FJsonObject> State;
			FString Schema;
			TFunction<FString()> Revision;
			if (RootType == UStringTable::StaticClass()->GetPathName())
			{
				UStringTable* Table = LoadStringTable(Target);
				if (!Table) return Fail(OutError, TEXT("string_table_target_missing"), Target);
				State = BuildStringTableState(*Table);
				Schema = StringTableSchema;
				Revision = [Target]() { return PackageRevision(Target); };
			}
			else if (RootType == ULocalizationSettings::StaticClass()->GetPathName())
			{
				State = BuildLocalizationState();
				Schema = LocalizationSchema;
				Revision = []() { return LocalizationRevision(); };
			}
			else
			{
				return Fail(OutError, TEXT("localization_export_type_unsupported"), RootType);
			}
			const TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
			Document->SetStringField(TEXT("schema"), Schema);
			Document->SetStringField(TEXT("target"), Target);
			Document->SetObjectField(TEXT("root"), State);
			FString Canonical;
			if (!CanonicalObject(Document, Canonical, OutError)) return false;
			FUeAgentResourceTracker Resources = MakeResources();
			FArtifactPublishReceipt Receipt;
			FArtifactPublishError PublishError;
			if (!TryExportVerifiedWorkspace(
					FVerifiedExportRequest(
						MakeShared<FVerifiedSource>(Target, RootType, Schema, Canonical, MoveTemp(Revision)),
						WorkspacePath,
						ProviderId),
					Resources,
					Receipt,
					PublishError))
			{
				return Fail(OutError, PublishError.GetCode(), PublishError.GetPath(), PublishError.GetDetail());
			}
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("status"), TEXT("published"));
			Response->SetStringField(TEXT("provider_id"), ProviderId);
			Response->SetStringField(TEXT("workspace_path"), Receipt.GetFinalPath());
			Response->SetStringField(TEXT("entry_file"), Receipt.GetEntryFile());
			Response->SetStringField(TEXT("plan_fingerprint"), Receipt.GetPlanFingerprint());
			Response->SetStringField(TEXT("inventory_hash"), Receipt.GetInventoryHash());
			const TSharedPtr<FJsonObject> Semantic = MakeShared<FJsonObject>();
			for (const TPair<FString, FString>& Field : Receipt.GetSemanticVerificationFields())
			{
				Semantic->SetStringField(Field.Key, Field.Value);
			}
			Response->SetObjectField(TEXT("semantic_verification"), Semantic);
			return CanonicalObject(Response, OutPayload, OutError);
		}
	}

	bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& RootTypeIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		if (!ParseParams(CanonicalParamsJson, Params, OutError)) return false;
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target != TargetIdentity)
		{
			return Fail(OutError, TEXT("localization_hjson_target_mismatch"), TargetIdentity);
		}
		const bool bStringTable = RootTypeIdentity == UStringTable::StaticClass()->GetPathName();
		const bool bLocalization = RootTypeIdentity == ULocalizationSettings::StaticClass()->GetPathName();
		if (!bStringTable && !bLocalization)
		{
			return Fail(OutError, TEXT("localization_hjson_type_unsupported"), RootTypeIdentity);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			FString ExpectedRevision;
			if (!Params->TryGetStringField(TEXT("expected_revision"), ExpectedRevision))
			{
				return Fail(OutError, TEXT("localization_hjson_expected_revision_required"));
			}
			TSharedPtr<FJsonObject> Root;
			if (!ParseDocument(
					Params,
					bStringTable ? StringTableSchema : LocalizationSchema,
					Root,
					OutError))
			{
				return false;
			}
			return bStringTable
				? ImportStringTable(Target, ExpectedRevision, Root, OutPayloadJson, OutError)
				: ImportLocalization(ExpectedRevision, Root, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			bOutReadOnly = true;
			return ExportWorkspace(Params, Target, RootTypeIdentity, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.compound"))
		{
			const TSharedPtr<FJsonObject>* Input = nullptr;
			FString Capability;
			if (!Params->TryGetObjectField(TEXT("input"), Input)
				|| !Input || !Input->IsValid()
				|| !(*Input)->TryGetStringField(
					TEXT("localization_capability"), Capability)
				|| !DomainAdapters::ContentGraphRead::
					IsExternalLocalizationContentGraphRoute(
						TEXT("content_graph.read.compound"), Capability))
			{
				return Fail(
					OutError,
					TEXT("localization_content_graph_compound_input_invalid"));
			}
			TSharedPtr<FJsonObject> CapabilityParameters = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>* Supplied = nullptr;
			if ((*Input)->TryGetObjectField(TEXT("parameters"), Supplied)
				&& Supplied && Supplied->IsValid())
			{
				CapabilityParameters = MakeShared<FJsonObject>(**Supplied);
			}
			if (!CapabilityParameters->HasField(TEXT("target")))
			{
				CapabilityParameters->SetStringField(TEXT("target"), Target);
			}
			TSharedPtr<FJsonObject> Observation;
			FString NativeError;
			if (!DomainAdapters::ContentGraphRead::
					TryExecuteExternalLocalizationContentGraph(
						Capability,
						CapabilityParameters,
						Observation,
						NativeError)
				|| !Observation.IsValid())
			{
				return Fail(
					OutError,
					TEXT("localization_content_graph_compound_failed"),
					NativeError);
			}
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("command_id"), CommandId);
			Response->SetStringField(TEXT("provider_id"), ProviderId);
			Response->SetStringField(TEXT("capability"), Capability);
			Response->SetBoolField(TEXT("read_only"), true);
			Response->SetObjectField(TEXT("query_result"), Observation);
			bOutReadOnly = true;
			return CanonicalObject(Response, OutPayloadJson, OutError);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("target"), Target);
			Response->SetStringField(TEXT("root_type"), RootTypeIdentity);
			Response->SetBoolField(TEXT("complete"), true);
			Response->SetBoolField(TEXT("has_more"), false);
			if (bStringTable)
			{
				UStringTable* Table = LoadStringTable(Target);
				if (!Table) return Fail(OutError, TEXT("string_table_target_missing"), Target);
				Response->SetStringField(TEXT("revision"), PackageRevision(Target));
				Response->SetObjectField(TEXT("query_result"), BuildStringTableState(*Table));
			}
			else
			{
				Response->SetStringField(TEXT("revision"), LocalizationRevision());
				Response->SetObjectField(TEXT("query_result"), BuildLocalizationState());
			}
			bOutReadOnly = true;
			return CanonicalObject(Response, OutPayloadJson, OutError);
		}
		return Fail(OutError, TEXT("localization_hjson_command_unsupported"), CommandId);
	}

#if WITH_DEV_AUTOMATION_TESTS
	void SetStringTableSaveFailureForTests(const bool bFail)
	{
		bFailStringTableSaveForTests = bFail;
	}
#endif
}
