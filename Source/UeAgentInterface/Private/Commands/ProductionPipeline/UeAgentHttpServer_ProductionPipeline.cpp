// Copyright Epic Games, Inc. All Rights Reserved.

#include "LegacyMechanics/UeAgentLegacyMechanics.h"

#include "Utilities/UeAgentAuthoringFileIO.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace UeAgentProductionPipelineOps
{
	static constexpr const TCHAR* LocalizationSchema = TEXT("uai.localization.v1");
	static constexpr const TCHAR* StringTableSchema = TEXT("uai.string_table.v1");
	static constexpr const TCHAR* PackagingSchema = TEXT("uai.packaging.v1");
	static constexpr const TCHAR* PlatformConfigSchema = TEXT("uai.platform_config.v1");
	static constexpr const TCHAR* DeviceProfileSchema = TEXT("uai.device_profile.v1");
	static constexpr const TCHAR* ScalabilitySchema = TEXT("uai.scalability.v1");
	static TSharedPtr<FJsonObject> MakeIssue(const FString& Severity, const FString& Code, const FString& Path, const FString& Message)
	{
		return UeAgentJsonDiagnostics::MakeIssue(Severity, Code, Path, Message);
	}

	static void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Severity, const FString& Code, const FString& Path, const FString& Message)
	{
		UeAgentJsonDiagnostics::AddIssue(Issues, Severity, Code, Path, Message);
	}

	static void ApplySectionAuthoringMetadata(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& SectionName,
		const FString& DisplayName,
		const FString& CategoryPath,
		std::initializer_list<FString> Operations)
	{
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Obj, TEXT("section"), SectionName, DisplayName, CategoryPath, TEXT("struct"));
	}

	static int32 CountErrors(const TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		return UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error"));
	}

	static FString NormalizeSlash(FString Path)
	{
		Path.TrimStartAndEndInline();
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Path;
	}

	static FString ResolveProjectFilePath(const FString& Path)
	{
		return UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(Path);
	}

	static FString NormalizeAbsolutePathForBoundary(const FString& Path)
	{
		FString Normalized = UeAgentJsonDiagnostics::NormalizeResolvedFilePath(FPaths::ConvertRelativePathToFull(Path));
		FPaths::CollapseRelativeDirectories(Normalized);
		FPaths::NormalizeFilename(Normalized);
		while (Normalized.Len() > 3 && Normalized.EndsWith(TEXT("/")))
		{
			Normalized.LeftChopInline(1, EAllowShrinking::No);
		}
		return Normalized;
	}

	static bool IsPathWithinDirectory(const FString& Path, const FString& RootDirectory)
	{
		const FString NormalizedPath = NormalizeAbsolutePathForBoundary(Path);
		const FString NormalizedRoot = NormalizeAbsolutePathForBoundary(RootDirectory);
		return !NormalizedPath.IsEmpty()
			&& !NormalizedRoot.IsEmpty()
			&& (NormalizedPath.Equals(NormalizedRoot, ESearchCase::IgnoreCase)
				|| NormalizedPath.StartsWith(NormalizedRoot + TEXT("/"), ESearchCase::IgnoreCase));
	}

	static FString UaiOwnedSavedRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterface")));
	}

	static FString UaiOwnedPackagesRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(UaiOwnedSavedRoot(), TEXT("Packages")));
	}

	static bool IsUaiOwnedSavedPath(const FString& Path)
	{
		return IsPathWithinDirectory(Path, UaiOwnedSavedRoot());
	}

	static bool IsUaiOwnedPackagesPath(const FString& Path)
	{
		return IsPathWithinDirectory(Path, UaiOwnedPackagesRoot());
	}

	static FString GetProjectFile()
	{
		TArray<FString> ProjectFiles;
		IFileManager::Get().FindFiles(ProjectFiles, *FPaths::Combine(FPaths::ProjectDir(), TEXT("*.uproject")), true, false);
		if (ProjectFiles.Num() > 0)
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), ProjectFiles[0]));
		}
		return FPaths::ConvertRelativePathToFull(FPaths::SetExtension(FPaths::Combine(FPaths::ProjectDir(), FApp::GetProjectName()), TEXT("uproject")));
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Item : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(Item));
		}
		return Values;
	}

	static TArray<FString> JsonArrayToStrings(const TArray<TSharedPtr<FJsonValue>>* Values)
	{
		TArray<FString> Strings;
		if (!Values)
		{
			return Strings;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (Value.IsValid() && Value->TryGetString(Text))
			{
				Text.TrimStartAndEndInline();
				if (!Text.IsEmpty())
				{
					Strings.Add(Text);
				}
			}
		}
		return Strings;
	}

	static bool TryGetStringArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, TArray<FString>& OutValues)
	{
		OutValues.Reset();
		if (!Obj.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Obj->TryGetArrayField(FieldName, Array) && Array)
		{
			OutValues = JsonArrayToStrings(Array);
			return true;
		}
		FString SingleValue;
		if (Obj->TryGetStringField(FieldName, SingleValue) && !SingleValue.TrimStartAndEnd().IsEmpty())
		{
			OutValues.Add(SingleValue);
			return true;
		}
		return false;
	}

	static bool SaveAuthoringFile(const FString& OutputFileInput, const TSharedPtr<FJsonObject>& Root, FString& OutResolvedPath, FString& OutError)
	{
		FString OutputFile = OutputFileInput;
		if (FPaths::GetExtension(OutputFile, false).Equals(TEXT("json"), ESearchCase::IgnoreCase))
		{
			OutputFile = FPaths::ChangeExtension(OutputFile, TEXT("hjson"));
		}
		UeAgentAuthoringFileIO::FSaveOptions Options;
		Options.SourceCommand = TEXT("production_pipeline_json_export");
		Options.bExternalizeInlineDetails = false;
		return UeAgentAuthoringFileIO::SaveObject(OutputFile, Root, Options, OutResolvedPath, OutError);
	}

	static bool LoadJsonFromParams(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutRoot, FString& OutJsonFile, FString& OutError)
	{
		OutRoot.Reset();
		OutJsonFile.Reset();
		if (!Params.IsValid())
		{
			OutError = TEXT("missing_params");
			return false;
		}
		const TSharedPtr<FJsonObject>* JsonObject = nullptr;
		if (Params->TryGetObjectField(TEXT("json"), JsonObject) && JsonObject && JsonObject->IsValid())
		{
			OutRoot = *JsonObject;
			return true;
		}
		if (Params->TryGetStringField(TEXT("json_file"), OutJsonFile) && !OutJsonFile.TrimStartAndEnd().IsEmpty())
		{
			UeAgentAuthoringFileIO::FLoadOptions Options;
			FString ResolvedPath;
			if (!UeAgentAuthoringFileIO::LoadObject(OutJsonFile, OutRoot, Options, ResolvedPath, OutError))
			{
				return false;
			}
			OutJsonFile = ResolvedPath;
			return true;
		}
		OutError = TEXT("missing_json_or_json_file");
		return false;
	}

	static bool HasJsonInput(const TSharedPtr<FJsonObject>& Params)
	{
		return Params.IsValid() && (Params->HasField(TEXT("json")) || Params->HasField(TEXT("json_file")));
	}

	static TSharedPtr<FJsonObject> MakeStringMapObject(const TMap<FString, FString>& Values)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Values)
		{
			Obj->SetStringField(Pair.Key, Pair.Value);
		}
		return Obj;
	}

	static FString JsonObjectToCompactString(const TSharedPtr<FJsonObject>& Obj)
	{
		FString JsonText;
		if (Obj.IsValid())
		{
			UeAgentJsonDiagnostics::SerializeJsonObjectCompact(Obj, JsonText);
		}
		return JsonText;
	}

	static bool ReadAuthoringObjectFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj)
	{
		UeAgentAuthoringFileIO::FLoadOptions Options;
		FString Error;
		return UeAgentAuthoringFileIO::LoadObject(FilePath, OutObj, Options, Error);
	}

	static bool CopyFileWithBackup(const FString& SourcePath, const FString& TargetPath, bool bBackup, TArray<TSharedPtr<FJsonValue>>& BackupPaths, FString& OutError)
	{
		const FString ResolvedSource = ResolveProjectFilePath(SourcePath);
		const FString ResolvedTarget = ResolveProjectFilePath(TargetPath);
		if (!FPaths::FileExists(ResolvedSource))
		{
			OutError = TEXT("source_file_not_found");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ResolvedTarget), true);
		if (bBackup && FPaths::FileExists(ResolvedTarget))
		{
			const FString BackupPath = ResolvedTarget + TEXT(".bak_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			if (IFileManager::Get().Copy(*BackupPath, *ResolvedTarget) == COPY_OK)
			{
				BackupPaths.Add(MakeShared<FJsonValueString>(BackupPath));
			}
		}
		if (IFileManager::Get().Copy(*ResolvedTarget, *ResolvedSource, true, true) != COPY_OK)
		{
			OutError = TEXT("copy_file_failed");
			return false;
		}
		return true;
	}

	static bool BackupTextFile(const FString& FilePath, TArray<TSharedPtr<FJsonValue>>& BackupPaths)
	{
		if (!FPaths::FileExists(FilePath))
		{
			return true;
		}
		const FString BackupPath = FilePath + TEXT(".bak_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		if (IFileManager::Get().Copy(*BackupPath, *FilePath) == COPY_OK)
		{
			BackupPaths.Add(MakeShared<FJsonValueString>(BackupPath));
			return true;
		}
		return false;
	}

	static bool SaveTextFileUtf8(const FString& FilePath, const FString& Text, FString& OutError)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		if (!FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("save_text_file_failed");
			return false;
		}
		return true;
	}

	static bool IsSimpleIdentifier(const FString& Value)
	{
		if (Value.IsEmpty())
		{
			return false;
		}
		for (TCHAR Ch : Value)
		{
			if (!FChar::IsAlnum(Ch) && Ch != TEXT('_') && Ch != TEXT('-') && Ch != TEXT('.') && Ch != TEXT('@'))
			{
				return false;
			}
		}
		return true;
	}

	static bool ContainsUnsafeShellMeta(const FString& Value)
	{
		static const FString UnsafeCharacters = TEXT("&|<>^%!\"()");
		for (const TCHAR Ch : Value)
		{
			int32 UnsafeIndex = INDEX_NONE;
			if (Ch < TEXT(' ') || UnsafeCharacters.FindChar(Ch, UnsafeIndex))
			{
				return true;
			}
		}
		return false;
	}

	static bool ContainsUnsafeIniToken(const FString& Value)
	{
		return Value.Contains(TEXT("\r")) || Value.Contains(TEXT("\n")) || Value.Contains(TEXT("="));
	}

	static bool IsSafeIniSectionName(const FString& Value)
	{
		return !Value.TrimStartAndEnd().IsEmpty()
			&& !ContainsUnsafeIniToken(Value)
			&& !Value.Contains(TEXT("["))
			&& !Value.Contains(TEXT("]"));
	}

	static bool IsSafeIniKey(const FString& Value)
	{
		return !Value.TrimStartAndEnd().IsEmpty() && !ContainsUnsafeIniToken(Value);
	}

	static bool IsSafeIniValue(const FString& Value)
	{
		return !ContainsUnsafeIniToken(Value);
	}

	static bool IsTaskIdValid(const FString& TaskId)
	{
		int32 LastSeparator = INDEX_NONE;
		if (!TaskId.FindLastChar(TEXT('_'), LastSeparator))
		{
			return false;
		}
		const FString HexSuffix = TaskId.Mid(LastSeparator + 1);
		const FString PrefixAndTimestamp = TaskId.Left(LastSeparator);
		int32 TimestampSeparator = INDEX_NONE;
		if (!PrefixAndTimestamp.FindLastChar(TEXT('_'), TimestampSeparator))
		{
			return false;
		}
		const FString Prefix = PrefixAndTimestamp.Left(TimestampSeparator);
		const FString Timestamp = PrefixAndTimestamp.Mid(TimestampSeparator + 1);
		if (Prefix != TEXT("packaging") && Prefix != TEXT("localization_gather") && Prefix != TEXT("localization_compile"))
		{
			return false;
		}
		if (Timestamp.Len() != 16 || Timestamp[8] != TEXT('T') || Timestamp[15] != TEXT('Z') || HexSuffix.Len() != 8)
		{
			return false;
		}
		for (int32 Index = 0; Index < Timestamp.Len(); ++Index)
		{
			if (Index != 8 && Index != 15 && !FChar::IsDigit(Timestamp[Index]))
			{
				return false;
			}
		}
		if (!FDateTime::Validate(
			FCString::Atoi(*Timestamp.Mid(0, 4)),
			FCString::Atoi(*Timestamp.Mid(4, 2)),
			FCString::Atoi(*Timestamp.Mid(6, 2)),
			FCString::Atoi(*Timestamp.Mid(9, 2)),
			FCString::Atoi(*Timestamp.Mid(11, 2)),
			FCString::Atoi(*Timestamp.Mid(13, 2)),
			0))
		{
			return false;
		}
		for (const TCHAR Ch : HexSuffix)
		{
			if (!FChar::IsHexDigit(Ch))
			{
				return false;
			}
		}
		return true;
	}

	static bool IsCultureNameAllowed(const FString& Culture)
	{
		if (!IsSimpleIdentifier(Culture) || Culture.Contains(TEXT("@")))
		{
			return false;
		}
		TArray<FString> CultureNames;
		FInternationalization::Get().GetCultureNames(CultureNames);
		return CultureNames.Contains(Culture) || Culture.Equals(TEXT("en"), ESearchCase::IgnoreCase) || Culture.StartsWith(TEXT("zh-"));
	}

	static FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = NormalizeSlash(InPath);
		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		const FString AssetPath = NormalizeAssetPath(InPath);
		if (AssetPath.Contains(TEXT(".")))
		{
			return AssetPath;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		return AssetName.IsEmpty() ? FString() : FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static UStringTable* LoadStringTable(const FString& AssetPath)
	{
		const FString ObjectPath = ToObjectPath(AssetPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UStringTable* Existing = FindObject<UStringTable>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UStringTable>(nullptr, *ObjectPath);
	}

	static bool SaveAssetPackage(UObject* Asset, FString& OutError)
	{
		if (!Asset || !Asset->GetOutermost())
		{
			OutError = TEXT("asset_not_found");
			return false;
		}
		TArray<UPackage*> Packages;
		Packages.Add(Asset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static UStringTable* CreateStringTableAsset(const FString& AssetPath, FString& OutError)
	{
		const FString PackageName = NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return nullptr;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		UStringTable* StringTable = NewObject<UStringTable>(Package, UStringTable::StaticClass(), *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!StringTable)
		{
			OutError = TEXT("create_string_table_failed");
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(StringTable);
		StringTable->MarkPackageDirty();
		Package->MarkPackageDirty();
		return StringTable;
	}

	static TSharedPtr<FJsonObject> BuildStringTableJson(UStringTable* StringTable)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), StringTableSchema);
		Root->SetStringField(TEXT("asset_path"), StringTable && StringTable->GetOutermost() ? StringTable->GetOutermost()->GetName() : FString());
		Root->SetStringField(TEXT("object_path"), StringTable ? StringTable->GetPathName() : FString());
		Root->SetStringField(TEXT("namespace"), StringTable ? StringTable->GetStringTable()->GetNamespace() : FString());
		TArray<TSharedPtr<FJsonValue>> Entries;
		if (StringTable)
		{
			StringTable->GetStringTable()->EnumerateSourceStrings(
				[&Entries, StringTable](const FString& Key, const FString& Source) -> bool
				{
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("key"), Key);
					Entry->SetStringField(TEXT("source"), Source);
					TMap<FString, FString> Metadata;
					StringTable->GetStringTable()->EnumerateMetaData(
						Key,
						[&Metadata](FName MetadataKey, const FString& MetadataValue) -> bool
						{
							Metadata.Add(MetadataKey.ToString(), MetadataValue);
							return true;
						});
					Entry->SetObjectField(TEXT("metadata"), MakeStringMapObject(Metadata));
					Entries.Add(MakeShared<FJsonValueObject>(Entry));
					return true;
				});
		}
		Root->SetArrayField(TEXT("entries"), Entries);
		Root->SetNumberField(TEXT("entry_count"), Entries.Num());
		return Root;
	}

	static bool ValidateStringTableRoot(const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Root.IsValid())
		{
			AddIssue(Issues, TEXT("error"), TEXT("json_root_invalid"), TEXT("$"), TEXT("String Table JSON root must be an object."));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Root->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
		{
			AddIssue(Issues, TEXT("error"), TEXT("missing_entries"), TEXT("$.entries"), TEXT("String Table JSON requires entries[]."));
			return false;
		}
		TSet<FString> Keys;
		for (int32 Index = 0; Index < Entries->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Entry = (*Entries)[Index].IsValid() ? (*Entries)[Index]->AsObject() : nullptr;
			const FString EntryPath = FString::Printf(TEXT("$.entries[%d]"), Index);
			if (!Entry.IsValid())
			{
				AddIssue(Issues, TEXT("error"), TEXT("entry_not_object"), EntryPath, TEXT("Entry must be an object."));
				continue;
			}
			FString Key;
			if (!Entry->TryGetStringField(TEXT("key"), Key) || Key.TrimStartAndEnd().IsEmpty())
			{
				AddIssue(Issues, TEXT("error"), TEXT("string_table_key_missing"), EntryPath + TEXT(".key"), TEXT("Entry key is required."));
			}
			else if (Keys.Contains(Key))
			{
				AddIssue(Issues, TEXT("error"), TEXT("string_table_key_duplicate"), EntryPath + TEXT(".key"), FString::Printf(TEXT("Duplicate key '%s'."), *Key));
			}
			else
			{
				Keys.Add(Key);
			}
			FString Source;
			if (!Entry->TryGetStringField(TEXT("source"), Source))
			{
				AddIssue(Issues, TEXT("error"), TEXT("string_table_source_missing"), EntryPath + TEXT(".source"), TEXT("Entry source string is required."));
			}
			const TSharedPtr<FJsonObject>* Metadata = nullptr;
			if (Entry->HasField(TEXT("metadata")) && (!Entry->TryGetObjectField(TEXT("metadata"), Metadata) || !Metadata || !Metadata->IsValid()))
			{
				AddIssue(Issues, TEXT("error"), TEXT("metadata_not_object"), EntryPath + TEXT(".metadata"), TEXT("metadata must be an object."));
			}
		}
		return CountErrors(Issues) == 0;
	}

	static bool ApplyStringTableRoot(
		const TSharedPtr<FJsonObject>& Root,
		const FString& ExplicitAssetPath,
		bool bDryRun,
		bool bBackup,
		bool bSaveAfterApply,
		TSharedPtr<FJsonObject>& OutReadback,
		TArray<TSharedPtr<FJsonValue>>& OutResults,
		TArray<TSharedPtr<FJsonValue>>& OutBackups,
		FString& OutError)
	{
		OutReadback.Reset();
		TArray<TSharedPtr<FJsonValue>> Issues;
		if (!ValidateStringTableRoot(Root, Issues))
		{
			OutError = TEXT("string_table_validate_failed");
			OutResults = Issues;
			return false;
		}
		FString AssetPath = ExplicitAssetPath;
		if (AssetPath.TrimStartAndEnd().IsEmpty())
		{
			Root->TryGetStringField(TEXT("asset_path"), AssetPath);
		}
		if (AssetPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_asset_path");
			return false;
		}
		UStringTable* StringTable = LoadStringTable(AssetPath);
		const bool bCreated = !StringTable;
		if (!StringTable && !bDryRun)
		{
			StringTable = CreateStringTableAsset(AssetPath, OutError);
			if (!StringTable)
			{
				return false;
			}
		}
		if (bBackup && StringTable && !bDryRun)
		{
			const FString BackupPath = FString::Printf(TEXT("/Game/__UeAgentInterfaceBackups/%s_Backup_%s"), *StringTable->GetName(), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			if (UPackage* BackupPackage = CreatePackage(*BackupPath))
			{
				UObject* BackupObject = StaticDuplicateObject(StringTable, BackupPackage, *FPackageName::GetLongPackageAssetName(BackupPath), RF_Public | RF_Standalone | RF_Transactional);
				if (BackupObject)
				{
					FAssetRegistryModule::AssetCreated(BackupObject);
					BackupObject->MarkPackageDirty();
					BackupPackage->MarkPackageDirty();
					OutBackups.Add(MakeShared<FJsonValueString>(BackupPath));
				}
			}
		}
		FString Namespace;
		Root->TryGetStringField(TEXT("namespace"), Namespace);
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		Root->TryGetArrayField(TEXT("entries"), Entries);
		int32 ChangedCount = 0;
		if (!bDryRun && StringTable)
		{
			StringTable->Modify();
			if (!Namespace.IsEmpty())
			{
				StringTable->GetMutableStringTable()->SetNamespace(Namespace);
			}
			StringTable->GetMutableStringTable()->ClearSourceStrings();
			StringTable->GetMutableStringTable()->ClearMetaData();
		}
		if (Entries)
		{
			for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
			{
				const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
				if (!Entry.IsValid())
				{
					continue;
				}
				FString Key;
				FString Source;
				Entry->TryGetStringField(TEXT("key"), Key);
				Entry->TryGetStringField(TEXT("source"), Source);
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("key"), Key);
				Result->SetStringField(TEXT("status"), bDryRun ? TEXT("planned") : TEXT("applied"));
				if (!bDryRun && StringTable)
				{
					StringTable->GetMutableStringTable()->SetSourceString(Key, Source);
					const TSharedPtr<FJsonObject>* Metadata = nullptr;
					if (Entry->TryGetObjectField(TEXT("metadata"), Metadata) && Metadata && Metadata->IsValid())
					{
						for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Metadata)->Values)
						{
							FString MetadataValue;
							if (Pair.Value.IsValid() && Pair.Value->TryGetString(MetadataValue))
							{
								StringTable->GetMutableStringTable()->SetMetaData(Key, FName(*Pair.Key), MetadataValue);
							}
						}
					}
				}
				OutResults.Add(MakeShared<FJsonValueObject>(Result));
				++ChangedCount;
			}
		}
		if (!bDryRun && StringTable)
		{
			StringTable->MarkPackageDirty();
			if (bSaveAfterApply && !SaveAssetPackage(StringTable, OutError))
			{
				return false;
			}
			OutReadback = BuildStringTableJson(StringTable);
		}
		else
		{
			OutReadback = MakeShared<FJsonObject>();
			OutReadback->SetStringField(TEXT("schema"), StringTableSchema);
			OutReadback->SetStringField(TEXT("asset_path"), AssetPath);
			OutReadback->SetNumberField(TEXT("entry_count"), ChangedCount);
		}
		OutReadback->SetBoolField(TEXT("created"), bCreated && !bDryRun);
		OutReadback->SetBoolField(TEXT("dry_run"), bDryRun);
		OutReadback->SetNumberField(TEXT("changed_count"), ChangedCount);
		return true;
	}
}

namespace UeAgentProductionPipelineOps
{
	struct FIniSection
	{
		FString Name;
		FString SourceFile;
		TMap<FString, FString> Values;
		TMultiMap<FString, FString> ArrayValues;
	};

	static bool ParseIniFile(const FString& FilePath, TMap<FString, FIniSection>& InOutSections)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FilePath))
		{
			return false;
		}
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		FString CurrentSection;
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty() || Line.StartsWith(TEXT(";")) || Line.StartsWith(TEXT("#")))
			{
				continue;
			}
			if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
			{
				CurrentSection = Line.Mid(1, Line.Len() - 2);
				FIniSection& Section = InOutSections.FindOrAdd(CurrentSection);
				Section.Name = CurrentSection;
				Section.SourceFile = FilePath;
				continue;
			}
			if (CurrentSection.IsEmpty())
			{
				continue;
			}
			FString Key;
			FString Value;
			if (Line.Split(TEXT("="), &Key, &Value))
			{
				Key.TrimStartAndEndInline();
				Value.TrimStartAndEndInline();
				FIniSection& Section = InOutSections.FindOrAdd(CurrentSection);
				Section.Name = CurrentSection;
				Section.SourceFile = FilePath;
				if (Key.StartsWith(TEXT("+")) || Key.StartsWith(TEXT("-")) || Key.StartsWith(TEXT(".")))
				{
					Section.ArrayValues.Add(Key, Value);
				}
				else
				{
					Section.Values.Add(Key, Value);
				}
			}
		}
		return true;
	}

	static bool UpsertIniSections(const FString& FilePath, const TMap<FString, FString>& SectionTextByName, bool bBackup, TArray<TSharedPtr<FJsonValue>>& BackupPaths, FString& OutError)
	{
		FString ExistingText;
		FFileHelper::LoadFileToString(ExistingText, *FilePath);
		if (bBackup && FPaths::FileExists(FilePath) && !BackupTextFile(FilePath, BackupPaths))
		{
			OutError = TEXT("backup_config_failed");
			return false;
		}
		TSet<FString> TargetSections;
		for (const TPair<FString, FString>& Pair : SectionTextByName)
		{
			TargetSections.Add(Pair.Key);
		}
		TArray<FString> Lines;
		ExistingText.ParseIntoArrayLines(Lines, false);
		FString NewText;
		bool bSkipping = false;
		for (const FString& RawLine : Lines)
		{
			const FString Line = RawLine.TrimStartAndEnd();
			if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
			{
				const FString SectionName = Line.Mid(1, Line.Len() - 2);
				bSkipping = TargetSections.Contains(SectionName);
			}
			if (!bSkipping)
			{
				NewText += RawLine + LINE_TERMINATOR;
			}
		}
		if (!NewText.IsEmpty() && !NewText.EndsWith(LINE_TERMINATOR LINE_TERMINATOR))
		{
			NewText += LINE_TERMINATOR;
		}
		for (const TPair<FString, FString>& Pair : SectionTextByName)
		{
			NewText += FString::Printf(TEXT("[%s]%s%s%s"), *Pair.Key, LINE_TERMINATOR, *Pair.Value, LINE_TERMINATOR);
		}
		return SaveTextFileUtf8(FilePath, NewText, OutError);
	}

	static FString BuildSectionTextFromValues(const TMap<FString, FString>& Values, const TArray<FString>& RawLines = TArray<FString>())
	{
		FString Text;
		for (const TPair<FString, FString>& Pair : Values)
		{
			Text += FString::Printf(TEXT("%s=%s%s"), *Pair.Key, *Pair.Value, LINE_TERMINATOR);
		}
		for (const FString& RawLine : RawLines)
		{
			Text += RawLine + LINE_TERMINATOR;
		}
		return Text;
	}

	static FString LocalizationContentRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Localization")));
	}

	static FString LocalizationConfigRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("Localization")));
	}

	static TArray<FString> FindFilesRecursiveByExtension(const FString& RootDir, const FString& Extension)
	{
		TArray<FString> Files;
		if (IFileManager::Get().DirectoryExists(*RootDir))
		{
			IFileManager::Get().FindFilesRecursive(Files, *RootDir, *FString::Printf(TEXT("*.%s"), *Extension), true, false, false);
		}
		return Files;
	}

	static TSharedPtr<FJsonObject> BuildLocalizationTargetJson(const FString& TargetName, bool bIncludeStats, bool bIncludeFiles)
	{
		const FString ContentDir = FPaths::Combine(LocalizationContentRoot(), TargetName);
		const FString ConfigFile = FPaths::Combine(LocalizationConfigRoot(), TargetName + TEXT(".ini"));
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), TargetName);
		Obj->SetStringField(TEXT("content_dir"), ContentDir);
		Obj->SetStringField(TEXT("config_file"), ConfigFile);
		Obj->SetBoolField(TEXT("has_content_dir"), IFileManager::Get().DirectoryExists(*ContentDir));
		Obj->SetBoolField(TEXT("has_config"), FPaths::FileExists(ConfigFile));
		TArray<FString> CultureNames;
		TArray<FString> CultureDirs;
		IFileManager::Get().FindFiles(CultureDirs, *FPaths::Combine(ContentDir, TEXT("*")), false, true);
		for (const FString& CultureDirName : CultureDirs)
		{
			if (IsCultureNameAllowed(CultureDirName))
			{
				CultureNames.Add(CultureDirName);
			}
		}
		Obj->SetArrayField(TEXT("cultures"), StringArrayToJson(CultureNames));
		TArray<FString> MissingFiles;
		if (!FPaths::FileExists(ConfigFile))
		{
			MissingFiles.Add(ConfigFile);
		}
		Obj->SetArrayField(TEXT("missing_files"), StringArrayToJson(MissingFiles));
		TArray<FString> ManifestFiles = FindFilesRecursiveByExtension(ContentDir, TEXT("manifest"));
		TArray<FString> ArchiveFiles = FindFilesRecursiveByExtension(ContentDir, TEXT("archive"));
		TArray<FString> LocresFiles = FindFilesRecursiveByExtension(ContentDir, TEXT("locres"));
		TArray<FString> PoFiles = FindFilesRecursiveByExtension(ContentDir, TEXT("po"));
		Obj->SetBoolField(TEXT("has_manifest"), ManifestFiles.Num() > 0);
		Obj->SetBoolField(TEXT("has_archive"), ArchiveFiles.Num() > 0);
		Obj->SetBoolField(TEXT("has_locres"), LocresFiles.Num() > 0);
		if (bIncludeFiles)
		{
			Obj->SetArrayField(TEXT("manifest_files"), StringArrayToJson(ManifestFiles));
			Obj->SetArrayField(TEXT("archive_files"), StringArrayToJson(ArchiveFiles));
			Obj->SetArrayField(TEXT("locres_files"), StringArrayToJson(LocresFiles));
			Obj->SetArrayField(TEXT("po_files"), StringArrayToJson(PoFiles));
		}
		if (bIncludeStats)
		{
			Obj->SetNumberField(TEXT("manifest_count"), ManifestFiles.Num());
			Obj->SetNumberField(TEXT("archive_count"), ArchiveFiles.Num());
			Obj->SetNumberField(TEXT("locres_count"), LocresFiles.Num());
			Obj->SetNumberField(TEXT("po_count"), PoFiles.Num());
		}
		return Obj;
	}

	static TArray<FString> DiscoverLocalizationTargetNames()
	{
		TSet<FString> TargetNames;
		TArray<FString> ContentDirs;
		IFileManager::Get().FindFiles(ContentDirs, *FPaths::Combine(LocalizationContentRoot(), TEXT("*")), false, true);
		for (const FString& Dir : ContentDirs)
		{
			TargetNames.Add(Dir);
		}
		TArray<FString> ConfigFiles;
		IFileManager::Get().FindFiles(ConfigFiles, *FPaths::Combine(LocalizationConfigRoot(), TEXT("*.ini")), true, false);
		for (const FString& File : ConfigFiles)
		{
			TargetNames.Add(FPaths::GetBaseFilename(File));
		}
		TArray<FString> SortedNames = TargetNames.Array();
		SortedNames.Sort();
		return SortedNames;
	}

	static TSharedPtr<FJsonObject> BuildLocalizationExportJson(bool bIncludeStats, bool bIncludeFiles, const FString& TargetFilter)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), LocalizationSchema);
		Root->SetStringField(TEXT("content_root"), LocalizationContentRoot());
		Root->SetStringField(TEXT("config_root"), LocalizationConfigRoot());
		TArray<TSharedPtr<FJsonValue>> Targets;
		for (const FString& TargetName : DiscoverLocalizationTargetNames())
		{
			if (!TargetFilter.IsEmpty() && !TargetName.Equals(TargetFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
			Targets.Add(MakeShared<FJsonValueObject>(BuildLocalizationTargetJson(TargetName, bIncludeStats, bIncludeFiles)));
		}
		Root->SetArrayField(TEXT("targets"), Targets);
		Root->SetNumberField(TEXT("target_count"), Targets.Num());
		Root->SetArrayField(TEXT("string_tables"), TArray<TSharedPtr<FJsonValue>>());
		return Root;
	}

	static bool ValidateLocalizationRoot(const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Root.IsValid())
		{
			AddIssue(Issues, TEXT("error"), TEXT("json_root_invalid"), TEXT("$"), TEXT("Localization JSON root must be an object."));
			return false;
		}
		const bool bHasLocalizationAuthoringFields = Root->HasField(TEXT("targets")) || Root->HasField(TEXT("string_tables"));
		const bool bLooksLikeProjectSettingsOnly = Root->HasField(TEXT("settings"))
			|| Root->HasField(TEXT("project_settings"))
			|| Root->HasField(TEXT("pages"))
			|| Root->HasField(TEXT("categories"))
			|| Root->HasField(TEXT("sections"));
		if (!bHasLocalizationAuthoringFields && bLooksLikeProjectSettingsOnly)
		{
			AddIssue(Issues, TEXT("error"), TEXT("use_project_settings_required"), TEXT("$"), TEXT("Project Settings fields must be edited through editor_project.project_settings.update."));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
		if (Root->TryGetArrayField(TEXT("targets"), Targets) && Targets)
		{
			TSet<FString> SeenTargets;
			for (int32 Index = 0; Index < Targets->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Target = (*Targets)[Index].IsValid() ? (*Targets)[Index]->AsObject() : nullptr;
				const FString TargetPath = FString::Printf(TEXT("$.targets[%d]"), Index);
				if (!Target.IsValid())
				{
					AddIssue(Issues, TEXT("error"), TEXT("localization_target_config_invalid"), TargetPath, TEXT("Target entry must be an object."));
					continue;
				}
				FString Name;
				if (!Target->TryGetStringField(TEXT("name"), Name) || !IsSimpleIdentifier(Name))
				{
					AddIssue(Issues, TEXT("error"), TEXT("localization_target_config_invalid"), TargetPath + TEXT(".name"), TEXT("Target name is required and must be a simple identifier."));
				}
				else if (SeenTargets.Contains(Name))
				{
					AddIssue(Issues, TEXT("error"), TEXT("localization_target_duplicate"), TargetPath + TEXT(".name"), FString::Printf(TEXT("Duplicate localization target '%s'."), *Name));
				}
				else
				{
					SeenTargets.Add(Name);
				}
				TArray<FString> Cultures;
				TryGetStringArray(Target, TEXT("cultures"), Cultures);
				FString NativeCulture;
				if (Target->TryGetStringField(TEXT("native_culture"), NativeCulture))
				{
					Cultures.AddUnique(NativeCulture);
				}
				for (const FString& Culture : Cultures)
				{
					if (!IsCultureNameAllowed(Culture))
					{
						AddIssue(Issues, TEXT("error"), TEXT("culture_invalid"), TargetPath + TEXT(".cultures"), FString::Printf(TEXT("Culture '%s' is not valid or not available."), *Culture));
					}
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* StringTables = nullptr;
		if (Root->TryGetArrayField(TEXT("string_tables"), StringTables) && StringTables)
		{
			for (int32 Index = 0; Index < StringTables->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> StringTable = (*StringTables)[Index].IsValid() ? (*StringTables)[Index]->AsObject() : nullptr;
				if (!StringTable.IsValid())
				{
					AddIssue(Issues, TEXT("error"), TEXT("string_table_config_invalid"), FString::Printf(TEXT("$.string_tables[%d]"), Index), TEXT("String table item must be an object."));
					continue;
				}
				ValidateStringTableRoot(StringTable, Issues);
			}
		}
		return CountErrors(Issues) == 0;
	}

	static bool TryParsePoCoverage(const FString& FilePath, int32& OutEntryCount, int32& OutTranslated)
	{
		OutEntryCount = 0;
		OutTranslated = 0;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FilePath))
		{
			return false;
		}
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		int32 MsgIdCount = 0;
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("msgid ")))
			{
				++MsgIdCount;
			}
			else if (Line.StartsWith(TEXT("msgstr \"")) && Line.Len() > 9)
			{
				const FString Value = Line.Mid(8).TrimQuotes();
				if (!Value.IsEmpty())
				{
					++OutTranslated;
				}
			}
		}
		OutEntryCount = MsgIdCount;
		return MsgIdCount > 0;
	}

	static int32 ParsePoCoverage(const FString& FilePath, int32& OutTranslated)
	{
		int32 EntryCount = 0;
		TryParsePoCoverage(FilePath, EntryCount, OutTranslated);
		return EntryCount;
	}

	static FString DetectPoCultureSubdir(const FString& TargetDir, const FString& PoFile, const TArray<FString>& RequestedCultures)
	{
		FString NormalizedTargetDir = UeAgentJsonDiagnostics::NormalizeResolvedFilePath(FPaths::ConvertRelativePathToFull(TargetDir));
		FPaths::NormalizeDirectoryName(NormalizedTargetDir);
		if (!NormalizedTargetDir.EndsWith(TEXT("/")))
		{
			NormalizedTargetDir += TEXT("/");
		}
		const FString NormalizedPoFile = UeAgentJsonDiagnostics::NormalizeResolvedFilePath(FPaths::ConvertRelativePathToFull(PoFile));
		if (!NormalizedPoFile.StartsWith(NormalizedTargetDir, ESearchCase::IgnoreCase))
		{
			return FString();
		}
		const FString RelativePath = NormalizedPoFile.Mid(NormalizedTargetDir.Len());
		TArray<FString> Segments;
		RelativePath.ParseIntoArray(Segments, TEXT("/"), true);
		if (Segments.Num() < 2)
		{
			return FString();
		}
		const FString CandidateCulture = Segments[0];
		if (RequestedCultures.Num() > 0 && RequestedCultures.Contains(CandidateCulture))
		{
			return CandidateCulture;
		}
		return IsCultureNameAllowed(CandidateCulture) ? CandidateCulture : FString();
	}

	static FString EngineEditorCmdPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::EngineDir(), TEXT("Binaries/Win64/UnrealEditor-Cmd.exe")));
	}

	static FString RunUatPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::EngineDir(), TEXT("Build/BatchFiles/RunUAT.bat")));
	}

	static FString UnrealPakPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::EngineDir(), TEXT("Binaries/Win64/UnrealPak.exe")));
	}

	struct FPackagingProfile
	{
		FString Platform = TEXT("Win64");
		FString Configuration = TEXT("Development");
		bool bBuild = false;
		bool bCook = true;
		bool bStage = true;
		bool bPackage = true;
		bool bArchive = true;
		bool bPak = true;
		bool bIoStore = false;
		bool bClean = false;
		bool bCompressed = true;
		bool bPrereqs = false;
		TArray<FString> Maps;
		TArray<FString> Cultures;
		FString ArchiveDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterface/Packages/Win64"));
	};

	static bool IsKnownConfiguration(const FString& Configuration)
	{
		return Configuration.Equals(TEXT("Debug"), ESearchCase::IgnoreCase)
			|| Configuration.Equals(TEXT("DebugGame"), ESearchCase::IgnoreCase)
			|| Configuration.Equals(TEXT("Development"), ESearchCase::IgnoreCase)
			|| Configuration.Equals(TEXT("Test"), ESearchCase::IgnoreCase)
			|| Configuration.Equals(TEXT("Shipping"), ESearchCase::IgnoreCase);
	}

	static bool ParsePackagingProfile(const TSharedPtr<FJsonObject>& Root, const TSharedPtr<FJsonObject>& Params, FPackagingProfile& OutProfile)
	{
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("platform"), OutProfile.Platform);
			Params->TryGetStringField(TEXT("configuration"), OutProfile.Configuration);
			Params->TryGetBoolField(TEXT("cook"), OutProfile.bCook);
			Params->TryGetBoolField(TEXT("stage"), OutProfile.bStage);
			Params->TryGetBoolField(TEXT("package"), OutProfile.bPackage);
			Params->TryGetBoolField(TEXT("archive"), OutProfile.bArchive);
		}
		if (Root.IsValid())
		{
			const TSharedPtr<FJsonObject>* TargetObj = nullptr;
			if (Root->TryGetObjectField(TEXT("target"), TargetObj) && TargetObj && TargetObj->IsValid())
			{
				(*TargetObj)->TryGetStringField(TEXT("platform"), OutProfile.Platform);
				(*TargetObj)->TryGetStringField(TEXT("configuration"), OutProfile.Configuration);
			}
			const TSharedPtr<FJsonObject>* PipelineObj = nullptr;
			if (Root->TryGetObjectField(TEXT("pipeline"), PipelineObj) && PipelineObj && PipelineObj->IsValid())
			{
				(*PipelineObj)->TryGetBoolField(TEXT("build"), OutProfile.bBuild);
				(*PipelineObj)->TryGetBoolField(TEXT("cook"), OutProfile.bCook);
				(*PipelineObj)->TryGetBoolField(TEXT("stage"), OutProfile.bStage);
				(*PipelineObj)->TryGetBoolField(TEXT("package"), OutProfile.bPackage);
				(*PipelineObj)->TryGetBoolField(TEXT("archive"), OutProfile.bArchive);
				(*PipelineObj)->TryGetBoolField(TEXT("pak"), OutProfile.bPak);
				(*PipelineObj)->TryGetBoolField(TEXT("iostore"), OutProfile.bIoStore);
			}
			const TSharedPtr<FJsonObject>* ContentObj = nullptr;
			if (Root->TryGetObjectField(TEXT("content"), ContentObj) && ContentObj && ContentObj->IsValid())
			{
				TryGetStringArray(*ContentObj, TEXT("maps"), OutProfile.Maps);
				TryGetStringArray(*ContentObj, TEXT("cultures"), OutProfile.Cultures);
			}
			const TSharedPtr<FJsonObject>* OutputObj = nullptr;
			if (Root->TryGetObjectField(TEXT("output"), OutputObj) && OutputObj && OutputObj->IsValid())
			{
				(*OutputObj)->TryGetStringField(TEXT("archive_directory"), OutProfile.ArchiveDirectory);
			}
			const TSharedPtr<FJsonObject>* OptionsObj = nullptr;
			if (Root->TryGetObjectField(TEXT("options"), OptionsObj) && OptionsObj && OptionsObj->IsValid())
			{
				(*OptionsObj)->TryGetBoolField(TEXT("clean"), OutProfile.bClean);
				(*OptionsObj)->TryGetBoolField(TEXT("compressed"), OutProfile.bCompressed);
				(*OptionsObj)->TryGetBoolField(TEXT("prereqs"), OutProfile.bPrereqs);
			}
		}
		if (OutProfile.Platform.IsEmpty())
		{
			OutProfile.Platform = TEXT("Win64");
		}
		if (OutProfile.ArchiveDirectory.IsEmpty())
		{
			OutProfile.ArchiveDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterface/Packages"), OutProfile.Platform);
		}
		OutProfile.ArchiveDirectory = ResolveProjectFilePath(OutProfile.ArchiveDirectory);
		return true;
	}

	static TSharedPtr<FJsonObject> BuildPackagingProfileJson(const FPackagingProfile& Profile)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), PackagingSchema);
		TSharedPtr<FJsonObject> ProjectObj = MakeShared<FJsonObject>();
		ProjectObj->SetStringField(TEXT("uproject"), GetProjectFile());
		Root->SetObjectField(TEXT("project"), ProjectObj);
		TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
		TargetObj->SetStringField(TEXT("platform"), Profile.Platform);
		TargetObj->SetStringField(TEXT("configuration"), Profile.Configuration);
		TargetObj->SetBoolField(TEXT("client"), true);
		ApplySectionAuthoringMetadata(TargetObj, TEXT("target"), TEXT("Target"), TEXT("Packaging|Target"), { TEXT("set") });
		Root->SetObjectField(TEXT("target"), TargetObj);
		TSharedPtr<FJsonObject> PipelineObj = MakeShared<FJsonObject>();
		PipelineObj->SetBoolField(TEXT("build"), Profile.bBuild);
		PipelineObj->SetBoolField(TEXT("cook"), Profile.bCook);
		PipelineObj->SetBoolField(TEXT("stage"), Profile.bStage);
		PipelineObj->SetBoolField(TEXT("package"), Profile.bPackage);
		PipelineObj->SetBoolField(TEXT("archive"), Profile.bArchive);
		PipelineObj->SetBoolField(TEXT("pak"), Profile.bPak);
		PipelineObj->SetBoolField(TEXT("iostore"), Profile.bIoStore);
		ApplySectionAuthoringMetadata(PipelineObj, TEXT("pipeline"), TEXT("Pipeline"), TEXT("Packaging|Pipeline"), { TEXT("set") });
		Root->SetObjectField(TEXT("pipeline"), PipelineObj);
		TSharedPtr<FJsonObject> ContentObj = MakeShared<FJsonObject>();
		ContentObj->SetArrayField(TEXT("maps"), StringArrayToJson(Profile.Maps));
		ContentObj->SetArrayField(TEXT("cultures"), StringArrayToJson(Profile.Cultures));
		ApplySectionAuthoringMetadata(ContentObj, TEXT("content"), TEXT("Content"), TEXT("Packaging|Content"), { TEXT("set") });
		Root->SetObjectField(TEXT("content"), ContentObj);
		TSharedPtr<FJsonObject> OutputObj = MakeShared<FJsonObject>();
		OutputObj->SetStringField(TEXT("archive_directory"), Profile.ArchiveDirectory);
		ApplySectionAuthoringMetadata(OutputObj, TEXT("output"), TEXT("Output"), TEXT("Packaging|Output"), { TEXT("set") });
		Root->SetObjectField(TEXT("output"), OutputObj);
		TSharedPtr<FJsonObject> OptionsObj = MakeShared<FJsonObject>();
		OptionsObj->SetBoolField(TEXT("clean"), Profile.bClean);
		OptionsObj->SetBoolField(TEXT("compressed"), Profile.bCompressed);
		OptionsObj->SetBoolField(TEXT("prereqs"), Profile.bPrereqs);
		ApplySectionAuthoringMetadata(OptionsObj, TEXT("options"), TEXT("Options"), TEXT("Packaging|Options"), { TEXT("set") });
		Root->SetObjectField(TEXT("options"), OptionsObj);
		TArray<TSharedPtr<FJsonValue>> SettingsRefs;
		SettingsRefs.Add(MakeShared<FJsonValueString>(TEXT("project_settings_export_page:Project/Packaging")));
		Root->SetArrayField(TEXT("settings_refs"), SettingsRefs);
		return Root;
	}

	static void ValidatePackagingProfile(const FPackagingProfile& Profile, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!IsSimpleIdentifier(Profile.Platform) || Profile.Platform.Contains(TEXT("@")) || ContainsUnsafeShellMeta(Profile.Platform))
		{
			AddIssue(Issues, TEXT("error"), TEXT("platform_token_unsafe"), TEXT("$.target.platform"), TEXT("Platform must be a simple shell-safe identifier."));
		}
		if (!IsKnownConfiguration(Profile.Configuration))
		{
			AddIssue(Issues, TEXT("error"), TEXT("configuration_invalid"), TEXT("$.target.configuration"), TEXT("Unknown packaging configuration."));
		}
		if (!FPaths::FileExists(RunUatPath()))
		{
			AddIssue(Issues, TEXT("error"), TEXT("uat_not_found"), TEXT("$.environment.uat"), RunUatPath());
		}
		for (const FString& Map : Profile.Maps)
		{
			if (ContainsUnsafeShellMeta(Map) || !FPackageName::IsValidLongPackageName(Map))
			{
				AddIssue(Issues, TEXT("error"), TEXT("map_token_unsafe"), TEXT("$.content.maps"), FString::Printf(TEXT("Map token is not shell-safe: %s"), *Map));
				continue;
			}
			if (!FPackageName::DoesPackageExist(Map))
			{
				AddIssue(Issues, TEXT("error"), TEXT("map_not_found"), TEXT("$.content.maps"), FString::Printf(TEXT("Map does not exist: %s"), *Map));
			}
		}
		for (const FString& Culture : Profile.Cultures)
		{
			if (!IsCultureNameAllowed(Culture) || ContainsUnsafeShellMeta(Culture))
			{
				AddIssue(Issues, TEXT("error"), TEXT("culture_token_unsafe"), TEXT("$.content.cultures"), FString::Printf(TEXT("Culture token is invalid or not shell-safe: %s"), *Culture));
			}
		}
		if (!IsUaiOwnedPackagesPath(Profile.ArchiveDirectory))
		{
			AddIssue(Issues, TEXT("error"), TEXT("archive_dir_unsafe"), TEXT("$.output.archive_directory"), TEXT("Archive directory must be under Saved/UeAgentInterface/Packages."));
		}
		else if (ContainsUnsafeShellMeta(Profile.ArchiveDirectory))
		{
			AddIssue(Issues, TEXT("error"), TEXT("archive_dir_token_unsafe"), TEXT("$.output.archive_directory"), TEXT("Archive directory contains shell metacharacters."));
		}
	}

	static TArray<FString> BuildUatArgs(const FPackagingProfile& Profile)
	{
		TArray<FString> Args;
		Args.Add(TEXT("BuildCookRun"));
		Args.Add(FString::Printf(TEXT("-project=\"%s\""), *GetProjectFile()));
		Args.Add(TEXT("-noP4"));
		Args.Add(FString::Printf(TEXT("-platform=%s"), *Profile.Platform));
		Args.Add(FString::Printf(TEXT("-clientconfig=%s"), *Profile.Configuration));
		if (Profile.bBuild) Args.Add(TEXT("-build"));
		if (Profile.bCook) Args.Add(TEXT("-cook"));
		if (Profile.bStage) Args.Add(TEXT("-stage"));
		if (Profile.bPackage) Args.Add(TEXT("-package"));
		if (Profile.bArchive)
		{
			Args.Add(TEXT("-archive"));
			Args.Add(FString::Printf(TEXT("-archivedirectory=\"%s\""), *Profile.ArchiveDirectory));
		}
		if (Profile.bPak) Args.Add(TEXT("-pak"));
		if (Profile.bIoStore) Args.Add(TEXT("-iostore"));
		if (Profile.bClean) Args.Add(TEXT("-clean"));
		if (Profile.bCompressed) Args.Add(TEXT("-compressed"));
		if (Profile.bPrereqs) Args.Add(TEXT("-prereqs"));
		if (Profile.Maps.Num() > 0)
		{
			Args.Add(FString::Printf(TEXT("-map=%s"), *FString::Join(Profile.Maps, TEXT("+"))));
		}
		if (Profile.Cultures.Num() > 0)
		{
			Args.Add(FString::Printf(TEXT("-CookCultures=%s"), *FString::Join(Profile.Cultures, TEXT("+"))));
		}
		return Args;
	}

	static TSharedPtr<FJsonObject> BuildPlanJson(const FPackagingProfile& Profile)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		const TArray<FString> Args = BuildUatArgs(Profile);
		Obj->SetStringField(TEXT("schema"), PackagingSchema);
		Obj->SetStringField(TEXT("uat_path"), RunUatPath());
		Obj->SetArrayField(TEXT("uat_args"), StringArrayToJson(Args));
		Obj->SetStringField(TEXT("command_line"), FString::Printf(TEXT("\"%s\" %s"), *RunUatPath(), *FString::Join(Args, TEXT(" "))));
		TArray<TSharedPtr<FJsonValue>> ArtifactsExpected;
		TSharedPtr<FJsonObject> Archive = MakeShared<FJsonObject>();
		Archive->SetStringField(TEXT("type"), TEXT("archive_directory"));
		Archive->SetStringField(TEXT("path"), Profile.ArchiveDirectory);
		ArtifactsExpected.Add(MakeShared<FJsonValueObject>(Archive));
		Obj->SetArrayField(TEXT("artifacts_expected"), ArtifactsExpected);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildPackagingEnvironmentJson(bool bIncludeSdks, bool bIncludeTools)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("uai.packaging.environment.v1"));
		TArray<TSharedPtr<FJsonValue>> Tools;
		auto AddTool = [&Tools](const FString& Name, const FString& Path)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Name);
			Obj->SetStringField(TEXT("path"), Path);
			Obj->SetBoolField(TEXT("exists"), FPaths::FileExists(Path));
			Tools.Add(MakeShared<FJsonValueObject>(Obj));
		};
		if (bIncludeTools)
		{
			AddTool(TEXT("RunUAT"), RunUatPath());
			AddTool(TEXT("UnrealEditor-Cmd"), EngineEditorCmdPath());
			AddTool(TEXT("UnrealPak"), UnrealPakPath());
		}
		Root->SetArrayField(TEXT("tools"), Tools);
		TArray<TSharedPtr<FJsonValue>> Platforms;
		if (FModuleManager::Get().ModuleExists(TEXT("TargetPlatform")))
		{
			ITargetPlatformManagerModule& TargetPlatformManager = FModuleManager::LoadModuleChecked<ITargetPlatformManagerModule>(TEXT("TargetPlatform"));
			for (ITargetPlatform* Platform : TargetPlatformManager.GetTargetPlatforms())
			{
				if (!Platform)
				{
					continue;
				}
				TSharedPtr<FJsonObject> PlatformObj = MakeShared<FJsonObject>();
				PlatformObj->SetStringField(TEXT("name"), Platform->PlatformName());
				PlatformObj->SetStringField(TEXT("display_name"), Platform->DisplayName().ToString());
				PlatformObj->SetStringField(TEXT("ini_platform_name"), Platform->IniPlatformName());
				PlatformObj->SetBoolField(TEXT("supports_packaging"), Platform->SupportsFeature(ETargetPlatformFeatures::Packaging));
				if (bIncludeSdks)
				{
					FString DocumentationPath;
					PlatformObj->SetBoolField(TEXT("sdk_installed"), Platform->IsSdkInstalled(false, DocumentationPath));
					PlatformObj->SetStringField(TEXT("sdk_documentation_path"), DocumentationPath);
				}
				Platforms.Add(MakeShared<FJsonValueObject>(PlatformObj));
			}
		}
		else
		{
			TSharedPtr<FJsonObject> PlatformObj = MakeShared<FJsonObject>();
			PlatformObj->SetStringField(TEXT("name"), TEXT("Win64"));
			PlatformObj->SetBoolField(TEXT("supports_packaging"), true);
			PlatformObj->SetStringField(TEXT("sdk_status"), TEXT("target_platform_module_unavailable"));
			Platforms.Add(MakeShared<FJsonValueObject>(PlatformObj));
		}
		Root->SetArrayField(TEXT("target_platforms"), Platforms);
		return Root;
	}

	struct FDeviceProfileInfo
	{
		FString Name;
		FString DeviceType;
		FString BaseProfile;
		TMap<FString, FString> CVars;
		FString SourceFile;
	};

	static FString SectionToProfileName(const FString& SectionName)
	{
		if (SectionName.EndsWith(TEXT(" DeviceProfile")))
		{
			return SectionName.LeftChop(14);
		}
		return SectionName;
	}

	static TMap<FString, FDeviceProfileInfo> ReadDeviceProfiles()
	{
		TMap<FString, FDeviceProfileInfo> Profiles;
		const TArray<FString> Files = {
			FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::EngineDir(), TEXT("Config/BaseDeviceProfiles.ini"))),
			FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultDeviceProfiles.ini")))
		};
		for (const FString& File : Files)
		{
			TMap<FString, FIniSection> Sections;
			ParseIniFile(File, Sections);
			for (const TPair<FString, FIniSection>& Pair : Sections)
			{
				if (!Pair.Key.EndsWith(TEXT(" DeviceProfile")))
				{
					continue;
				}
				const FString ProfileName = SectionToProfileName(Pair.Key);
				FDeviceProfileInfo& Profile = Profiles.FindOrAdd(ProfileName);
				Profile.Name = ProfileName;
				Profile.SourceFile = File;
				if (const FString* DeviceType = Pair.Value.Values.Find(TEXT("DeviceType")))
				{
					Profile.DeviceType = *DeviceType;
				}
				if (const FString* BaseProfile = Pair.Value.Values.Find(TEXT("BaseProfileName")))
				{
					Profile.BaseProfile = *BaseProfile;
				}
				TArray<FString> CVarLines;
				Pair.Value.ArrayValues.MultiFind(TEXT("+CVars"), CVarLines);
				for (const FString& CVarLine : CVarLines)
				{
					FString Key;
					FString Value;
					if (CVarLine.Split(TEXT("="), &Key, &Value))
					{
						Key.TrimStartAndEndInline();
						Value.TrimStartAndEndInline();
						if (!Key.IsEmpty())
						{
							Profile.CVars.Add(Key, Value);
						}
					}
				}
			}
		}
		return Profiles;
	}

	static TSharedPtr<FJsonObject> DeviceProfileToJson(const FDeviceProfileInfo& Profile, bool bIncludeSource)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Profile.Name);
		Obj->SetStringField(TEXT("device_type"), Profile.DeviceType);
		Obj->SetStringField(TEXT("base_profile"), Profile.BaseProfile);
		Obj->SetObjectField(TEXT("cvars"), MakeStringMapObject(Profile.CVars));
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Obj, TEXT("name"), Profile.Name, Profile.Name, TEXT("Device Profiles"), TEXT("struct"));
		if (bIncludeSource)
		{
			Obj->SetStringField(TEXT("source_file"), Profile.SourceFile);
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildDeviceProfileExportJson(const FString& ProfileFilter, bool bIncludeInherited)
	{
		const TMap<FString, FDeviceProfileInfo> Profiles = ReadDeviceProfiles();
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), DeviceProfileSchema);
		TArray<TSharedPtr<FJsonValue>> ProfileValues;
		for (const TPair<FString, FDeviceProfileInfo>& Pair : Profiles)
		{
			if (!ProfileFilter.IsEmpty() && !Pair.Key.Equals(ProfileFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
			ProfileValues.Add(MakeShared<FJsonValueObject>(DeviceProfileToJson(Pair.Value, true)));
		}
		Root->SetArrayField(TEXT("profiles"), ProfileValues);
		Root->SetNumberField(TEXT("profile_count"), ProfileValues.Num());
		if (!ProfileFilter.IsEmpty() && bIncludeInherited)
		{
			TArray<TSharedPtr<FJsonValue>> Chain;
			FString Current = ProfileFilter;
			TSet<FString> Visited;
			while (!Current.IsEmpty() && Profiles.Contains(Current) && !Visited.Contains(Current))
			{
				Visited.Add(Current);
				Chain.Add(MakeShared<FJsonValueString>(Current));
				Current = Profiles[Current].BaseProfile;
			}
			Root->SetArrayField(TEXT("inheritance_chain"), Chain);
		}
		return Root;
	}

	static bool ValidateDeviceProfileRoot(const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Root.IsValid())
		{
			AddIssue(Issues, TEXT("error"), TEXT("json_root_invalid"), TEXT("$"), TEXT("DeviceProfile JSON root must be an object."));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
		if (!Root->TryGetArrayField(TEXT("profiles"), Profiles) || !Profiles)
		{
			AddIssue(Issues, TEXT("error"), TEXT("missing_profiles"), TEXT("$.profiles"), TEXT("DeviceProfile JSON requires profiles[]."));
			return false;
		}
		TSet<FString> Names;
		TMap<FString, FString> BaseByProfile;
		for (int32 Index = 0; Index < Profiles->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Profile = (*Profiles)[Index].IsValid() ? (*Profiles)[Index]->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("$.profiles[%d]"), Index);
			if (!Profile.IsValid())
			{
				AddIssue(Issues, TEXT("error"), TEXT("device_profile_invalid"), Path, TEXT("Profile entry must be an object."));
				continue;
			}
			FString Name;
			if (!Profile->TryGetStringField(TEXT("name"), Name) || !IsSimpleIdentifier(Name))
			{
				AddIssue(Issues, TEXT("error"), TEXT("device_profile_not_found"), Path + TEXT(".name"), TEXT("Profile name is required and must be a simple identifier."));
				continue;
			}
			if (Names.Contains(Name))
			{
				AddIssue(Issues, TEXT("error"), TEXT("device_profile_duplicate"), Path + TEXT(".name"), FString::Printf(TEXT("Duplicate profile '%s'."), *Name));
			}
			Names.Add(Name);
			FString BaseProfile;
			Profile->TryGetStringField(TEXT("base_profile"), BaseProfile);
			if (!BaseProfile.IsEmpty() && (!IsSimpleIdentifier(BaseProfile) || ContainsUnsafeIniToken(BaseProfile)))
			{
				AddIssue(Issues, TEXT("error"), TEXT("device_profile_base_invalid"), Path + TEXT(".base_profile"), TEXT("Base profile contains unsafe INI characters."));
			}
			FString DeviceType;
			if (Profile->TryGetStringField(TEXT("device_type"), DeviceType) && (!IsSimpleIdentifier(DeviceType) || ContainsUnsafeIniToken(DeviceType)))
			{
				AddIssue(Issues, TEXT("error"), TEXT("device_profile_device_type_invalid"), Path + TEXT(".device_type"), TEXT("Device type contains unsafe INI characters."));
			}
			BaseByProfile.Add(Name, BaseProfile);
			const TSharedPtr<FJsonObject>* CVars = nullptr;
			if (Profile->TryGetObjectField(TEXT("cvars"), CVars) && CVars && CVars->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*CVars)->Values)
				{
					FString CVarValue;
					if (!IsSafeIniKey(Pair.Key))
					{
						AddIssue(Issues, TEXT("error"), TEXT("device_profile_cvar_key_invalid"), Path + TEXT(".cvars.") + Pair.Key, TEXT("CVar key contains unsafe INI characters."));
					}
					if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(CVarValue) || !IsSafeIniValue(CVarValue))
					{
						AddIssue(Issues, TEXT("error"), TEXT("device_profile_cvar_value_invalid"), Path + TEXT(".cvars.") + Pair.Key, TEXT("CVar value must be a shell-free single-line INI value."));
					}
				}
			}
		}
		const TMap<FString, FDeviceProfileInfo> Existing = ReadDeviceProfiles();
		for (const TPair<FString, FString>& Pair : BaseByProfile)
		{
			if (!Pair.Value.IsEmpty() && !Names.Contains(Pair.Value) && !Existing.Contains(Pair.Value))
			{
				AddIssue(Issues, TEXT("error"), TEXT("device_profile_base_missing"), TEXT("$.profiles"), FString::Printf(TEXT("Base profile '%s' for '%s' is missing."), *Pair.Value, *Pair.Key));
			}
			TSet<FString> Visited;
			FString Current = Pair.Key;
			while (!Current.IsEmpty())
			{
				if (Visited.Contains(Current))
				{
					AddIssue(Issues, TEXT("error"), TEXT("device_profile_cycle"), TEXT("$.profiles"), FString::Printf(TEXT("Device profile inheritance cycle at '%s'."), *Current));
					break;
				}
				Visited.Add(Current);
				const FString* LocalBase = BaseByProfile.Find(Current);
				if (LocalBase)
				{
					Current = *LocalBase;
				}
				else if (const FDeviceProfileInfo* ExistingProfile = Existing.Find(Current))
				{
					Current = ExistingProfile->BaseProfile;
				}
				else
				{
					break;
				}
			}
		}
		return CountErrors(Issues) == 0;
	}

	static TMap<FString, FString> ResolveDeviceProfileCVars(const FString& ProfileName, TArray<FString>& OutChain, TArray<TSharedPtr<FJsonValue>>& OutConflicts)
	{
		const TMap<FString, FDeviceProfileInfo> Profiles = ReadDeviceProfiles();
		TMap<FString, FString> Resolved;
		TArray<const FDeviceProfileInfo*> Stack;
		FString Current = ProfileName;
		TSet<FString> Visited;
		while (!Current.IsEmpty() && Profiles.Contains(Current) && !Visited.Contains(Current))
		{
			Visited.Add(Current);
			Stack.Insert(&Profiles[Current], 0);
			Current = Profiles[Current].BaseProfile;
		}
		for (const FDeviceProfileInfo* Profile : Stack)
		{
			if (!Profile)
			{
				continue;
			}
			OutChain.Add(Profile->Name);
			for (const TPair<FString, FString>& Pair : Profile->CVars)
			{
				if (Resolved.Contains(Pair.Key) && Resolved[Pair.Key] != Pair.Value)
				{
					TSharedPtr<FJsonObject> Conflict = MakeShared<FJsonObject>();
					Conflict->SetStringField(TEXT("key"), Pair.Key);
					Conflict->SetStringField(TEXT("previous_value"), Resolved[Pair.Key]);
					Conflict->SetStringField(TEXT("new_value"), Pair.Value);
					Conflict->SetStringField(TEXT("profile"), Profile->Name);
					OutConflicts.Add(MakeShared<FJsonValueObject>(Conflict));
				}
				Resolved.Add(Pair.Key, Pair.Value);
			}
		}
		return Resolved;
	}

	static bool ApplyDeviceProfiles(const TSharedPtr<FJsonObject>& Root, bool bDryRun, bool bBackup, TArray<TSharedPtr<FJsonValue>>& OutChanges, TArray<TSharedPtr<FJsonValue>>& OutBackups, FString& OutError)
	{
		TArray<TSharedPtr<FJsonValue>> Issues;
		if (!ValidateDeviceProfileRoot(Root, Issues))
		{
			OutChanges = Issues;
			OutError = TEXT("device_profile_validate_failed");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
		Root->TryGetArrayField(TEXT("profiles"), Profiles);
		TMap<FString, FString> SectionTextByName;
		if (Profiles)
		{
			for (const TSharedPtr<FJsonValue>& ProfileValue : *Profiles)
			{
				const TSharedPtr<FJsonObject> Profile = ProfileValue.IsValid() ? ProfileValue->AsObject() : nullptr;
				if (!Profile.IsValid())
				{
					continue;
				}
				FString Name;
				FString BaseProfile;
				FString DeviceType;
				Profile->TryGetStringField(TEXT("name"), Name);
				Profile->TryGetStringField(TEXT("base_profile"), BaseProfile);
				Profile->TryGetStringField(TEXT("device_type"), DeviceType);
				TMap<FString, FString> Values;
				if (!DeviceType.IsEmpty())
				{
					Values.Add(TEXT("DeviceType"), DeviceType);
				}
				if (!BaseProfile.IsEmpty())
				{
					Values.Add(TEXT("BaseProfileName"), BaseProfile);
				}
				TArray<FString> CVarLines;
				const TSharedPtr<FJsonObject>* CVars = nullptr;
				if (Profile->TryGetObjectField(TEXT("cvars"), CVars) && CVars && CVars->IsValid())
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*CVars)->Values)
					{
						FString Value;
						Pair.Value->TryGetString(Value);
						CVarLines.Add(FString::Printf(TEXT("+CVars=%s=%s"), *Pair.Key, *Value));
					}
				}
				SectionTextByName.Add(Name + TEXT(" DeviceProfile"), BuildSectionTextFromValues(Values, CVarLines));
				TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
				Change->SetStringField(TEXT("profile"), Name);
				Change->SetStringField(TEXT("status"), bDryRun ? TEXT("planned") : TEXT("applied"));
				OutChanges.Add(MakeShared<FJsonValueObject>(Change));
			}
		}
		if (bDryRun)
		{
			return true;
		}
		const FString ProjectDeviceProfiles = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultDeviceProfiles.ini")));
		return UpsertIniSections(ProjectDeviceProfiles, SectionTextByName, bBackup, OutBackups, OutError);
	}

	static TSharedPtr<FJsonObject> BuildScalabilityJson()
	{
		TMap<FString, FIniSection> Sections;
		ParseIniFile(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::EngineDir(), TEXT("Config/BaseScalability.ini"))), Sections);
		ParseIniFile(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultScalability.ini"))), Sections);
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), ScalabilitySchema);
		TArray<TSharedPtr<FJsonValue>> Groups;
		TArray<FString> QualityLevels;
		for (const TPair<FString, FIniSection>& Pair : Sections)
		{
			if (!Pair.Key.Contains(TEXT("Quality@")))
			{
				continue;
			}
			FString Left;
			FString QualityLevel;
			if (Pair.Key.Split(TEXT("@"), &Left, &QualityLevel) && !QualityLevel.IsEmpty())
			{
				QualityLevels.AddUnique(QualityLevel);
			}
			TSharedPtr<FJsonObject> Group = MakeShared<FJsonObject>();
			Group->SetStringField(TEXT("section"), Pair.Key);
			Group->SetStringField(TEXT("source_file"), Pair.Value.SourceFile);
			Group->SetObjectField(TEXT("values"), MakeStringMapObject(Pair.Value.Values));
			ApplySectionAuthoringMetadata(Group, Pair.Key, Pair.Key, TEXT("Scalability"), { TEXT("set") });
			Groups.Add(MakeShared<FJsonValueObject>(Group));
		}
		QualityLevels.Sort();
		Root->SetArrayField(TEXT("groups"), Groups);
		Root->SetArrayField(TEXT("quality_levels"), StringArrayToJson(QualityLevels));
		Root->SetNumberField(TEXT("group_count"), Groups.Num());
		return Root;
	}

	static bool ValidateScalabilityRoot(const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Root.IsValid())
		{
			AddIssue(Issues, TEXT("error"), TEXT("json_root_invalid"), TEXT("$"), TEXT("Scalability JSON root must be an object."));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
		if (!Root->TryGetArrayField(TEXT("groups"), Groups) || !Groups)
		{
			AddIssue(Issues, TEXT("error"), TEXT("missing_groups"), TEXT("$.groups"), TEXT("Scalability JSON requires groups[]."));
			return false;
		}
		for (int32 Index = 0; Index < Groups->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Group = (*Groups)[Index].IsValid() ? (*Groups)[Index]->AsObject() : nullptr;
			const FString Path = FString::Printf(TEXT("$.groups[%d]"), Index);
			FString Section;
			if (!Group.IsValid() || !Group->TryGetStringField(TEXT("section"), Section) || !Section.Contains(TEXT("Quality@")) || !IsSafeIniSectionName(Section))
			{
				AddIssue(Issues, TEXT("error"), TEXT("scalability_section_invalid"), Path + TEXT(".section"), TEXT("Group section must be a safe single-line name like ViewDistanceQuality@0."));
			}
			const TSharedPtr<FJsonObject>* Values = nullptr;
			if (Group.IsValid() && (!Group->TryGetObjectField(TEXT("values"), Values) || !Values || !Values->IsValid()))
			{
				AddIssue(Issues, TEXT("error"), TEXT("scalability_values_invalid"), Path + TEXT(".values"), TEXT("Group values must be an object."));
			}
			else if (Values && Values->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
				{
					FString ValueText;
					if (!IsSafeIniKey(Pair.Key))
					{
						AddIssue(Issues, TEXT("error"), TEXT("scalability_key_invalid"), Path + TEXT(".values.") + Pair.Key, TEXT("Scalability key contains unsafe INI characters."));
					}
					if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(ValueText) || !IsSafeIniValue(ValueText))
					{
						AddIssue(Issues, TEXT("error"), TEXT("scalability_value_invalid"), Path + TEXT(".values.") + Pair.Key, TEXT("Scalability value must be a single-line INI value without '='."));
					}
				}
			}
		}
		return CountErrors(Issues) == 0;
	}

	static bool ApplyScalabilityRoot(const TSharedPtr<FJsonObject>& Root, bool bDryRun, bool bBackup, TArray<TSharedPtr<FJsonValue>>& OutChanges, TArray<TSharedPtr<FJsonValue>>& OutBackups, FString& OutError)
	{
		TArray<TSharedPtr<FJsonValue>> Issues;
		if (!ValidateScalabilityRoot(Root, Issues))
		{
			OutChanges = Issues;
			OutError = TEXT("scalability_validate_failed");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
		Root->TryGetArrayField(TEXT("groups"), Groups);
		TMap<FString, FString> SectionTextByName;
		if (Groups)
		{
			for (const TSharedPtr<FJsonValue>& GroupValue : *Groups)
			{
				const TSharedPtr<FJsonObject> Group = GroupValue.IsValid() ? GroupValue->AsObject() : nullptr;
				if (!Group.IsValid())
				{
					continue;
				}
				FString Section;
				Group->TryGetStringField(TEXT("section"), Section);
				TMap<FString, FString> ValuesMap;
				const TSharedPtr<FJsonObject>* Values = nullptr;
				if (Group->TryGetObjectField(TEXT("values"), Values) && Values && Values->IsValid())
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
					{
						FString ValueText;
						if (Pair.Value.IsValid() && Pair.Value->TryGetString(ValueText))
						{
							ValuesMap.Add(Pair.Key, ValueText);
						}
					}
				}
				SectionTextByName.Add(Section, BuildSectionTextFromValues(ValuesMap));
				TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
				Change->SetStringField(TEXT("section"), Section);
				Change->SetStringField(TEXT("status"), bDryRun ? TEXT("planned") : TEXT("applied"));
				OutChanges.Add(MakeShared<FJsonValueObject>(Change));
			}
		}
		if (bDryRun)
		{
			return true;
		}
		const FString FilePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultScalability.ini")));
		return UpsertIniSections(FilePath, SectionTextByName, bBackup, OutBackups, OutError);
	}

	static bool IsRootProjectConfigFileName(const FString& FileName)
	{
		return FileName.Equals(TEXT("DefaultEngine.ini"), ESearchCase::IgnoreCase)
			|| FileName.Equals(TEXT("DefaultGame.ini"), ESearchCase::IgnoreCase)
			|| FileName.Equals(TEXT("DefaultDeviceProfiles.ini"), ESearchCase::IgnoreCase)
			|| FileName.Equals(TEXT("DefaultScalability.ini"), ESearchCase::IgnoreCase);
	}

	static bool IsPlatformConfigFileNameForPlatform(const FString& Platform, const FString& FileName)
	{
		if (Platform.IsEmpty() || !IsSimpleIdentifier(Platform))
		{
			return false;
		}
		return FileName.Equals(Platform + TEXT("Engine.ini"), ESearchCase::IgnoreCase)
			|| FileName.Equals(Platform + TEXT("Game.ini"), ESearchCase::IgnoreCase)
			|| FileName.Equals(Platform + TEXT("DeviceProfiles.ini"), ESearchCase::IgnoreCase);
	}

	static bool IsPlatformConfigPathSafe(const FString& Normalized)
	{
		if (Normalized.IsEmpty() || Normalized.Contains(TEXT(":")) || Normalized.StartsWith(TEXT("/")) || Normalized.Contains(TEXT("//")))
		{
			return false;
		}
		TArray<FString> Segments;
		Normalized.ParseIntoArray(Segments, TEXT("/"), false);
		for (const FString& Segment : Segments)
		{
			if (Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT(".."))
			{
				return false;
			}
		}
		return true;
	}

	static bool IsPlatformConfigFileAllowed(const FString& Platform, const FString& File)
	{
		const FString Normalized = NormalizeSlash(File);
		if (!IsPlatformConfigPathSafe(Normalized))
		{
			return false;
		}
		TArray<FString> Segments;
		Normalized.ParseIntoArray(Segments, TEXT("/"), true);
		if (Segments.Num() == 1)
		{
			const FString FileName = Segments[0];
			return IsRootProjectConfigFileName(FileName) || IsPlatformConfigFileNameForPlatform(Platform, FileName);
		}
		if (Segments.Num() == 2)
		{
			return Segments[0].Equals(Platform, ESearchCase::IgnoreCase)
				&& IsPlatformConfigFileNameForPlatform(Platform, Segments[1]);
		}
		if (Segments.Num() == 3)
		{
			return Segments[0].Equals(TEXT("Config"), ESearchCase::IgnoreCase)
				&& Segments[1].Equals(Platform, ESearchCase::IgnoreCase)
				&& IsPlatformConfigFileNameForPlatform(Platform, Segments[2]);
		}
		return false;
	}

	static FString ResolvePlatformConfigFile(const FString& Platform, const FString& File)
	{
		FString Normalized = NormalizeSlash(File);
		if (!IsPlatformConfigFileAllowed(Platform, Normalized))
		{
			return FString();
		}
		if (Normalized.StartsWith(TEXT("Config/")))
		{
			return UeAgentJsonDiagnostics::NormalizeResolvedFilePath(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Normalized)));
		}
		if (Normalized.Contains(TEXT("/")))
		{
			return UeAgentJsonDiagnostics::NormalizeResolvedFilePath(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), Normalized)));
		}
		if (Normalized.StartsWith(TEXT("Default")))
		{
			return UeAgentJsonDiagnostics::NormalizeResolvedFilePath(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), Normalized)));
		}
		return UeAgentJsonDiagnostics::NormalizeResolvedFilePath(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), Platform, Normalized)));
	}

	static bool ValidatePlatformConfigRoot(const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Root.IsValid())
		{
			AddIssue(Issues, TEXT("error"), TEXT("json_root_invalid"), TEXT("$"), TEXT("Platform config JSON root must be an object."));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		if (!Root->TryGetArrayField(TEXT("files"), Files) || !Files)
		{
			AddIssue(Issues, TEXT("error"), TEXT("missing_files"), TEXT("$.files"), TEXT("Platform config JSON requires files[]."));
			return false;
		}
		FString Platform = TEXT("Windows");
		Root->TryGetStringField(TEXT("platform"), Platform);
		for (int32 FileIndex = 0; FileIndex < Files->Num(); ++FileIndex)
		{
			const TSharedPtr<FJsonObject> FileObj = (*Files)[FileIndex].IsValid() ? (*Files)[FileIndex]->AsObject() : nullptr;
			const FString FilePath = FString::Printf(TEXT("$.files[%d]"), FileIndex);
			FString FileName;
			if (!FileObj.IsValid() || !FileObj->TryGetStringField(TEXT("file"), FileName) || !IsPlatformConfigFileAllowed(Platform, FileName))
			{
				AddIssue(Issues, TEXT("error"), TEXT("platform_config_file_not_allowed"), FilePath + TEXT(".file"), TEXT("File is not in the platform config allow-list."));
			}
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			if (!FileObj.IsValid() || !FileObj->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
			{
				AddIssue(Issues, TEXT("error"), TEXT("platform_config_sections_missing"), FilePath + TEXT(".sections"), TEXT("sections[] is required."));
				continue;
			}
			for (int32 SectionIndex = 0; SectionIndex < Sections->Num(); ++SectionIndex)
			{
				const TSharedPtr<FJsonObject> Section = (*Sections)[SectionIndex].IsValid() ? (*Sections)[SectionIndex]->AsObject() : nullptr;
				const FString SectionPath = FString::Printf(TEXT("%s.sections[%d]"), *FilePath, SectionIndex);
				FString SectionName;
				if (!Section.IsValid() || !Section->TryGetStringField(TEXT("name"), SectionName) || !IsSafeIniSectionName(SectionName))
				{
					AddIssue(Issues, TEXT("error"), TEXT("platform_config_section_name_invalid"), SectionPath + TEXT(".name"), TEXT("Section name contains unsafe INI characters."));
					continue;
				}
				const TSharedPtr<FJsonObject>* Values = nullptr;
				if (!Section->TryGetObjectField(TEXT("values"), Values) || !Values || !Values->IsValid())
				{
					AddIssue(Issues, TEXT("error"), TEXT("platform_config_values_invalid"), SectionPath + TEXT(".values"), TEXT("Section values must be an object."));
					continue;
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
				{
					FString ValueText;
					if (!IsSafeIniKey(Pair.Key))
					{
						AddIssue(Issues, TEXT("error"), TEXT("platform_config_key_invalid"), SectionPath + TEXT(".values.") + Pair.Key, TEXT("Config key contains unsafe INI characters."));
					}
					if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(ValueText) || !IsSafeIniValue(ValueText))
					{
						AddIssue(Issues, TEXT("error"), TEXT("platform_config_value_invalid"), SectionPath + TEXT(".values.") + Pair.Key, TEXT("Config value must be a single-line INI value without '='."));
					}
				}
			}
		}
		return CountErrors(Issues) == 0;
	}

	static TSharedPtr<FJsonObject> BuildPlatformConfigExportJson(const FString& Platform, const TArray<FString>& SectionFilters, bool bIncludeSources)
	{
		const TArray<FString> CandidateFiles = {
			FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultEngine.ini"))),
			FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultGame.ini"))),
			FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultDeviceProfiles.ini"))),
			FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultScalability.ini"))),
			FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), Platform, Platform + TEXT("Engine.ini"))),
			FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), Platform, Platform + TEXT("Game.ini"))),
			FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), Platform, Platform + TEXT("DeviceProfiles.ini")))
		};
		TMap<FString, FIniSection> Sections;
		TArray<FString> SourceFiles;
		for (const FString& File : CandidateFiles)
		{
			if (ParseIniFile(File, Sections))
			{
				SourceFiles.Add(File);
			}
		}
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), PlatformConfigSchema);
		Root->SetStringField(TEXT("platform"), Platform);
		TSharedPtr<FJsonObject> MergedValues = MakeShared<FJsonObject>();
		for (const TPair<FString, FIniSection>& Pair : Sections)
		{
			if (SectionFilters.Num() > 0 && !SectionFilters.Contains(Pair.Key))
			{
				continue;
			}
			TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
			SectionObj->SetObjectField(TEXT("values"), MakeStringMapObject(Pair.Value.Values));
			if (bIncludeSources)
			{
				SectionObj->SetStringField(TEXT("source_file"), Pair.Value.SourceFile);
			}
			MergedValues->SetObjectField(Pair.Key, SectionObj);
		}
		Root->SetObjectField(TEXT("merged_values"), MergedValues);
		Root->SetArrayField(TEXT("source_files"), StringArrayToJson(SourceFiles));
		return Root;
	}

	static bool ApplyPlatformConfigRoot(const TSharedPtr<FJsonObject>& Root, bool bDryRun, bool bBackup, TArray<TSharedPtr<FJsonValue>>& OutChanges, TArray<TSharedPtr<FJsonValue>>& OutBackups, FString& OutError)
	{
		TArray<TSharedPtr<FJsonValue>> Issues;
		if (!ValidatePlatformConfigRoot(Root, Issues))
		{
			OutChanges = Issues;
			OutError = TEXT("platform_config_validate_failed");
			return false;
		}
		FString Platform = TEXT("Windows");
		Root->TryGetStringField(TEXT("platform"), Platform);
		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		Root->TryGetArrayField(TEXT("files"), Files);
		TMap<FString, TMap<FString, FString>> SectionTextByFile;
		if (Files)
		{
			for (const TSharedPtr<FJsonValue>& FileValue : *Files)
			{
				const TSharedPtr<FJsonObject> FileObj = FileValue.IsValid() ? FileValue->AsObject() : nullptr;
				if (!FileObj.IsValid())
				{
					continue;
				}
				FString FileName;
				FileObj->TryGetStringField(TEXT("file"), FileName);
				const FString FilePath = ResolvePlatformConfigFile(Platform, FileName);
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				FileObj->TryGetArrayField(TEXT("sections"), Sections);
				if (!Sections)
				{
					continue;
				}
				for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
				{
					const TSharedPtr<FJsonObject> SectionObj = SectionValue.IsValid() ? SectionValue->AsObject() : nullptr;
					if (!SectionObj.IsValid())
					{
						continue;
					}
					FString SectionName;
					SectionObj->TryGetStringField(TEXT("name"), SectionName);
					TMap<FString, FString> ValuesMap;
					const TSharedPtr<FJsonObject>* Values = nullptr;
					if (SectionObj->TryGetObjectField(TEXT("values"), Values) && Values && Values->IsValid())
					{
						for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
						{
							FString ValueText;
							if (Pair.Value.IsValid() && Pair.Value->TryGetString(ValueText))
							{
								ValuesMap.Add(Pair.Key, ValueText);
							}
						}
					}
					SectionTextByFile.FindOrAdd(FilePath).Add(SectionName, BuildSectionTextFromValues(ValuesMap));
					TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
					Change->SetStringField(TEXT("file"), FilePath);
					Change->SetStringField(TEXT("section"), SectionName);
					Change->SetStringField(TEXT("status"), bDryRun ? TEXT("planned") : TEXT("applied"));
					OutChanges.Add(MakeShared<FJsonValueObject>(Change));
				}
			}
		}
		if (bDryRun)
		{
			return true;
		}
		for (const TPair<FString, TMap<FString, FString>>& Pair : SectionTextByFile)
		{
			if (!UpsertIniSections(Pair.Key, Pair.Value, bBackup, OutBackups, OutError))
			{
				return false;
			}
		}
		return true;
	}

	static void AddLastLogLines(const FString& LogFile, TSharedPtr<FJsonObject>& OutData, int32 MaxLines = 30)
	{
		FString Text;
		TArray<TSharedPtr<FJsonValue>> LinesJson;
		if (FFileHelper::LoadFileToString(Text, *LogFile))
		{
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, false);
			const int32 StartIndex = FMath::Max(0, Lines.Num() - MaxLines);
			for (int32 Index = StartIndex; Index < Lines.Num(); ++Index)
			{
				LinesJson.Add(MakeShared<FJsonValueString>(Lines[Index]));
			}
		}
		OutData->SetArrayField(TEXT("last_log_lines"), LinesJson);
	}

	static void ParseLogText(const FString& Text, TArray<TSharedPtr<FJsonValue>>& Errors, TArray<TSharedPtr<FJsonValue>>& Warnings)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			const FString& Line = Lines[Index];
			const bool bError = Line.Contains(TEXT("Error:"), ESearchCase::IgnoreCase) || Line.Contains(TEXT("ERROR:"), ESearchCase::IgnoreCase) || Line.Contains(TEXT("AutomationTool exiting with ExitCode="));
			const bool bWarning = Line.Contains(TEXT("Warning:"), ESearchCase::IgnoreCase) || Line.Contains(TEXT("WARNING:"), ESearchCase::IgnoreCase);
			if (!bError && !bWarning)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("line"), Index + 1);
			Item->SetStringField(TEXT("text"), Line);
			if (bError)
			{
				Errors.Add(MakeShared<FJsonValueObject>(Item));
			}
			else
			{
				Warnings.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
	}
}

bool FUeAgentLegacyMechanics::ExecuteProductionPipelineCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("localization_query_targets")) return CmdLocalizationQueryTargets(Ctx, OutData, OutError);
	if (CommandLower == TEXT("localization_export_json")) return CmdLocalizationExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("localization_validate_json")) return CmdLocalizationValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("localization_diff_json")) return CmdLocalizationDiffJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("localization_export_po")) return CmdLocalizationExportPo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("localization_import_po")) return CmdLocalizationImportPo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("localization_report")) return CmdLocalizationReport(Ctx, OutData, OutError);
	if (CommandLower == TEXT("localization_preview_culture")) return CmdLocalizationPreviewCulture(Ctx, OutData, OutError);
	if (CommandLower == TEXT("string_table_export_json")) return CmdStringTableExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("string_table_validate_json")) return CmdStringTableValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("string_table_diff_json")) return CmdStringTableDiffJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("packaging_query_environment")) return CmdPackagingQueryEnvironment(Ctx, OutData, OutError);
	if (CommandLower == TEXT("packaging_export_profile_json")) return CmdPackagingExportProfileJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("packaging_validate_profile_json")) return CmdPackagingValidateProfileJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("packaging_diff_profile_json")) return CmdPackagingDiffProfileJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("packaging_plan_build")) return CmdPackagingPlanBuild(Ctx, OutData, OutError);
	if (CommandLower == TEXT("packaging_parse_log")) return CmdPackagingParseLog(Ctx, OutData, OutError);
	if (CommandLower == TEXT("packaging_report_artifacts")) return CmdPackagingReportArtifacts(Ctx, OutData, OutError);
	if (CommandLower == TEXT("packaging_clean_artifacts")) return CmdPackagingCleanArtifacts(Ctx, OutData, OutError);
	if (CommandLower == TEXT("target_platform_query")) return CmdTargetPlatformQuery(Ctx, OutData, OutError);
	if (CommandLower == TEXT("platform_config_export_json")) return CmdPlatformConfigExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("platform_config_validate_json")) return CmdPlatformConfigValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("device_profile_query")) return CmdDeviceProfileQuery(Ctx, OutData, OutError);
	if (CommandLower == TEXT("device_profile_export_json")) return CmdDeviceProfileExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("device_profile_validate_json")) return CmdDeviceProfileValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("device_profile_diff_json")) return CmdDeviceProfileDiffJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("device_profile_resolve_cvars")) return CmdDeviceProfileResolveCVars(Ctx, OutData, OutError);
	if (CommandLower == TEXT("scalability_export_json")) return CmdScalabilityExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("scalability_validate_json")) return CmdScalabilityValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("platform_profile_validate_packaging")) return CmdPlatformProfileValidatePackaging(Ctx, OutData, OutError);
	OutError = TEXT("unknown_production_pipeline_command");
	return false;
}

bool FUeAgentLegacyMechanics::CmdLocalizationQueryTargets(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	bool bIncludeStats = false;
	bool bIncludeFiles = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_stats"), bIncludeStats);
	JsonTryGetBool(Ctx.Params, TEXT("include_files"), bIncludeFiles);
	OutData = BuildLocalizationExportJson(bIncludeStats, bIncludeFiles, FString());
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	OutError.Reset();
	return true;
}

bool FUeAgentLegacyMechanics::CmdLocalizationExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString Target;
	FString OutputFile;
	bool bIncludeEntries = false;
	bool bIncludeWordCount = false;
	JsonTryGetString(Ctx.Params, TEXT("target"), Target);
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	JsonTryGetBool(Ctx.Params, TEXT("include_entries"), bIncludeEntries);
	JsonTryGetBool(Ctx.Params, TEXT("include_word_count"), bIncludeWordCount);
	OutData = BuildLocalizationExportJson(bIncludeWordCount, bIncludeEntries, Target);
	if (!OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString SavedPath;
		if (!SaveAuthoringFile(OutputFile, OutData, SavedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedPath);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdLocalizationValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	bool bStrict = false;
	JsonTryGetBool(Ctx.Params, TEXT("strict"), bStrict);
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	const bool bValid = ValidateLocalizationRoot(Root, Issues);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.localization.validate_result.v1"));
	OutData->SetBoolField(TEXT("strict"), bStrict);
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	if (!JsonFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), ResolveProjectFilePath(JsonFile));
	}
	if (!bValid)
	{
		OutError = TEXT("localization_validate_failed");
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdLocalizationDiffJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Desired;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Desired, JsonFile, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!ValidateLocalizationRoot(Desired, Issues))
	{
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutError = TEXT("localization_validate_failed");
		return false;
	}
	const TArray<FString> ExistingTargets = DiscoverLocalizationTargetNames();
	TSet<FString> ExistingSet(ExistingTargets);
	TArray<TSharedPtr<FJsonValue>> Diffs;
	const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
	if (Desired->TryGetArrayField(TEXT("targets"), Targets) && Targets)
	{
		for (const TSharedPtr<FJsonValue>& TargetValue : *Targets)
		{
			const TSharedPtr<FJsonObject> Target = TargetValue.IsValid() ? TargetValue->AsObject() : nullptr;
			FString Name;
			if (Target.IsValid() && Target->TryGetStringField(TEXT("name"), Name) && !ExistingSet.Contains(Name))
			{
				TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
				Diff->SetStringField(TEXT("type"), TEXT("target_added"));
				Diff->SetStringField(TEXT("target"), Name);
				Diffs.Add(MakeShared<FJsonValueObject>(Diff));
			}
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.localization.diff_result.v1"));
	OutData->SetBoolField(TEXT("changed"), Diffs.Num() > 0);
	OutData->SetArrayField(TEXT("diffs"), Diffs);
	OutData->SetNumberField(TEXT("diff_count"), Diffs.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdLocalizationApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	bool bDryRun = true;
	bool bBackup = true;
	FString SavePolicy = TEXT("config_only");
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("backup"), bBackup);
	JsonTryGetString(Ctx.Params, TEXT("save_policy"), SavePolicy);
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!ValidateLocalizationRoot(Root, Issues))
	{
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutError = TEXT("localization_validate_failed");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	TArray<TSharedPtr<FJsonValue>> BackupPaths;
	const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
	if (Root->TryGetArrayField(TEXT("targets"), Targets) && Targets)
	{
		for (const TSharedPtr<FJsonValue>& TargetValue : *Targets)
		{
			const TSharedPtr<FJsonObject> Target = TargetValue.IsValid() ? TargetValue->AsObject() : nullptr;
			if (!Target.IsValid())
			{
				continue;
			}
			FString Name;
			FString NativeCulture = TEXT("en");
			TArray<FString> Cultures;
			Target->TryGetStringField(TEXT("name"), Name);
			Target->TryGetStringField(TEXT("native_culture"), NativeCulture);
			TryGetStringArray(Target, TEXT("cultures"), Cultures);
			if (Cultures.Num() == 0)
			{
				Cultures.Add(NativeCulture);
			}
			const FString TargetContentDir = FPaths::Combine(LocalizationContentRoot(), Name);
			const FString TargetConfigFile = FPaths::Combine(LocalizationConfigRoot(), Name + TEXT(".ini"));
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("target"), Name);
			Result->SetStringField(TEXT("config_file"), TargetConfigFile);
			Result->SetStringField(TEXT("content_dir"), TargetContentDir);
			Result->SetStringField(TEXT("status"), bDryRun ? TEXT("planned") : TEXT("applied"));
			if (!bDryRun)
			{
				IFileManager::Get().MakeDirectory(*TargetContentDir, true);
				for (const FString& Culture : Cultures)
				{
					IFileManager::Get().MakeDirectory(*FPaths::Combine(TargetContentDir, Culture), true);
				}
				if (bBackup && FPaths::FileExists(TargetConfigFile))
				{
					BackupTextFile(TargetConfigFile, BackupPaths);
				}
				FString ConfigText;
				ConfigText += TEXT("[CommonSettings]") LINE_TERMINATOR;
				ConfigText += FString::Printf(TEXT("SourcePath=Content/Localization/%s%s"), *Name, LINE_TERMINATOR);
				ConfigText += FString::Printf(TEXT("DestinationPath=Content/Localization/%s%s"), *Name, LINE_TERMINATOR);
				ConfigText += FString::Printf(TEXT("NativeCulture=%s%s"), *NativeCulture, LINE_TERMINATOR);
				for (const FString& Culture : Cultures)
				{
					ConfigText += FString::Printf(TEXT("+CulturesToGenerate=%s%s"), *Culture, LINE_TERMINATOR);
				}
				if (!SaveTextFileUtf8(TargetConfigFile, ConfigText, OutError))
				{
					return false;
				}
			}
			Results.Add(MakeShared<FJsonValueObject>(Result));
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* StringTables = nullptr;
	if (Root->TryGetArrayField(TEXT("string_tables"), StringTables) && StringTables)
	{
		for (const TSharedPtr<FJsonValue>& StringTableValue : *StringTables)
		{
			TSharedPtr<FJsonObject> StringTableRoot = StringTableValue.IsValid() ? StringTableValue->AsObject() : nullptr;
			TSharedPtr<FJsonObject> Readback;
			TArray<TSharedPtr<FJsonValue>> StringTableResults;
			if (StringTableRoot.IsValid() && !ApplyStringTableRoot(StringTableRoot, FString(), bDryRun, bBackup, false, Readback, StringTableResults, BackupPaths, OutError))
			{
				return false;
			}
			if (Readback.IsValid())
			{
				Results.Add(MakeShared<FJsonValueObject>(Readback));
			}
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.localization.apply_result.v1"));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetStringField(TEXT("save_policy"), SavePolicy);
	OutData->SetBoolField(TEXT("changed"), Results.Num() > 0);
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetArrayField(TEXT("backup_paths"), BackupPaths);
	return true;
}

bool FUeAgentLegacyMechanics::CmdLocalizationExportPo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString Target = TEXT("Game");
	FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterface/Localization/PO"));
	bool bExecute = false;
	TArray<FString> Cultures;
	JsonTryGetString(Ctx.Params, TEXT("target"), Target);
	JsonTryGetString(Ctx.Params, TEXT("output_dir"), OutputDir);
	JsonTryGetBool(Ctx.Params, TEXT("execute"), bExecute);
	TryGetStringArray(Ctx.Params, TEXT("cultures"), Cultures);
	const FString TargetDir = FPaths::Combine(LocalizationContentRoot(), Target);
	TArray<FString> PoFiles = FindFilesRecursiveByExtension(TargetDir, TEXT("po"));
	TArray<TSharedPtr<FJsonValue>> ResultFiles;
	int32 EntryCount = 0;
	for (const FString& PoFile : PoFiles)
	{
		if (Cultures.Num() > 0)
		{
			bool bCultureMatches = false;
			for (const FString& Culture : Cultures)
			{
				if (NormalizeSlash(PoFile).Contains(TEXT("/") + Culture + TEXT("/")))
				{
					bCultureMatches = true;
					break;
				}
			}
			if (!bCultureMatches)
			{
				continue;
			}
		}
		int32 Translated = 0;
		EntryCount += ParsePoCoverage(PoFile, Translated);
		FString OutputFile = FPaths::Combine(ResolveProjectFilePath(OutputDir), FPaths::GetCleanFilename(PoFile));
		const FString CultureSubdir = DetectPoCultureSubdir(TargetDir, PoFile, Cultures);
		if (!CultureSubdir.IsEmpty())
		{
			OutputFile = FPaths::Combine(ResolveProjectFilePath(OutputDir), CultureSubdir, FPaths::GetCleanFilename(PoFile));
		}
		if (bExecute)
		{
			TArray<TSharedPtr<FJsonValue>> IgnoredBackups;
			if (!CopyFileWithBackup(PoFile, OutputFile, false, IgnoredBackups, OutError))
			{
				return false;
			}
			ResultFiles.Add(MakeShared<FJsonValueString>(OutputFile));
		}
		else
		{
			ResultFiles.Add(MakeShared<FJsonValueString>(PoFile));
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.localization.po_export_result.v1"));
	OutData->SetBoolField(TEXT("execute"), bExecute);
	OutData->SetArrayField(TEXT("po_files"), ResultFiles);
	OutData->SetNumberField(TEXT("entry_count"), EntryCount);
	return true;
}

bool FUeAgentLegacyMechanics::CmdLocalizationImportPo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString Target = TEXT("Game");
	FString Culture;
	FString PoFile;
	bool bDryRun = true;
	bool bBackup = true;
	JsonTryGetString(Ctx.Params, TEXT("target"), Target);
	JsonTryGetString(Ctx.Params, TEXT("culture"), Culture);
	JsonTryGetString(Ctx.Params, TEXT("po_file"), PoFile);
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("backup"), bBackup);
	if (Culture.IsEmpty() || !IsCultureNameAllowed(Culture))
	{
		OutError = TEXT("culture_invalid");
		return false;
	}
	if (PoFile.TrimStartAndEnd().IsEmpty() || !FPaths::FileExists(ResolveProjectFilePath(PoFile)))
	{
		OutError = TEXT("po_file_not_found");
		return false;
	}
	int32 Translated = 0;
	const FString ResolvedPoFile = ResolveProjectFilePath(PoFile);
	int32 EntryCount = 0;
	if (!TryParsePoCoverage(ResolvedPoFile, EntryCount, Translated))
	{
		OutError = TEXT("po_parse_failed");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Backups;
	FString TargetFile = FPaths::Combine(LocalizationContentRoot(), Target, Culture, FPaths::GetCleanFilename(PoFile));
	if (!bDryRun && !CopyFileWithBackup(PoFile, TargetFile, bBackup, Backups, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.localization.po_import_result.v1"));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetStringField(TEXT("target_file"), TargetFile);
	OutData->SetNumberField(TEXT("imported_count"), EntryCount);
	OutData->SetNumberField(TEXT("translated_count"), Translated);
	OutData->SetArrayField(TEXT("backup_paths"), Backups);
	OutData->SetArrayField(TEXT("conflicts"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetArrayField(TEXT("missing_source"), TArray<TSharedPtr<FJsonValue>>());
	return true;
}

bool FUeAgentLegacyMechanics::CmdLocalizationReport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString Target;
	bool bIncludeAssets = false;
	TArray<FString> Cultures;
	JsonTryGetString(Ctx.Params, TEXT("target"), Target);
	JsonTryGetBool(Ctx.Params, TEXT("include_assets"), bIncludeAssets);
	TryGetStringArray(Ctx.Params, TEXT("cultures"), Cultures);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.localization.report.v1"));
	OutData->SetBoolField(TEXT("include_assets"), bIncludeAssets);
	TArray<TSharedPtr<FJsonValue>> Coverage;
	for (const FString& TargetName : DiscoverLocalizationTargetNames())
	{
		if (!Target.IsEmpty() && !TargetName.Equals(Target, ESearchCase::IgnoreCase))
		{
			continue;
		}
		TArray<FString> PoFiles = FindFilesRecursiveByExtension(FPaths::Combine(LocalizationContentRoot(), TargetName), TEXT("po"));
		for (const FString& PoFile : PoFiles)
		{
			int32 Translated = 0;
			const int32 Total = ParsePoCoverage(PoFile, Translated);
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("target"), TargetName);
			Item->SetStringField(TEXT("po_file"), PoFile);
			Item->SetNumberField(TEXT("entry_count"), Total);
			Item->SetNumberField(TEXT("translated_count"), Translated);
			Item->SetNumberField(TEXT("coverage"), Total > 0 ? static_cast<double>(Translated) / static_cast<double>(Total) : 0.0);
			Coverage.Add(MakeShared<FJsonValueObject>(Item));
		}
	}
	OutData->SetArrayField(TEXT("coverage_by_culture"), Coverage);
	OutData->SetArrayField(TEXT("missing"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetArrayField(TEXT("stale"), TArray<TSharedPtr<FJsonValue>>());
	return true;
}

bool FUeAgentLegacyMechanics::CmdLocalizationPreviewCulture(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString Culture;
	FString Scope = TEXT("session");
	bool bRestoreAfter = true;
	JsonTryGetString(Ctx.Params, TEXT("culture"), Culture);
	JsonTryGetString(Ctx.Params, TEXT("scope"), Scope);
	JsonTryGetBool(Ctx.Params, TEXT("restore_after"), bRestoreAfter);
	if (Culture.IsEmpty() || !IsCultureNameAllowed(Culture))
	{
		OutError = TEXT("culture_invalid");
		return false;
	}
	const FString PreviousCulture = FInternationalization::Get().GetCurrentCulture()->GetName();
	const bool bSet = FInternationalization::Get().SetCurrentCulture(Culture);
	if (bRestoreAfter)
	{
		FInternationalization::Get().SetCurrentCulture(PreviousCulture);
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.localization.preview_culture.v1"));
	OutData->SetStringField(TEXT("scope"), Scope);
	OutData->SetStringField(TEXT("previous_culture"), PreviousCulture);
	OutData->SetStringField(TEXT("current_culture"), bRestoreAfter ? PreviousCulture : Culture);
	OutData->SetBoolField(TEXT("set_succeeded"), bSet);
	OutData->SetBoolField(TEXT("restored"), bRestoreAfter);
	return bSet;
}

bool FUeAgentLegacyMechanics::CmdStringTableExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString AssetPath;
	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	UStringTable* StringTable = LoadStringTable(AssetPath);
	if (!StringTable)
	{
		OutError = TEXT("string_table_asset_type_mismatch");
		return false;
	}
	OutData = BuildStringTableJson(StringTable);
	if (!OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString SavedPath;
		if (!SaveAuthoringFile(OutputFile, OutData, SavedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedPath);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdStringTableValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	const bool bValid = ValidateStringTableRoot(Root, Issues);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.string_table.validate_result.v1"));
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	if (!bValid)
	{
		OutError = TEXT("string_table_validate_failed");
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdStringTableDiffJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!ValidateStringTableRoot(Root, Issues))
	{
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutError = TEXT("string_table_validate_failed");
		return false;
	}
	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	UStringTable* StringTable = LoadStringTable(AssetPath);
	TSet<FString> CurrentKeys;
	if (StringTable)
	{
		StringTable->GetStringTable()->EnumerateSourceStrings([&CurrentKeys](const FString& Key, const FString&) -> bool
		{
			CurrentKeys.Add(Key);
			return true;
		});
	}
	TArray<TSharedPtr<FJsonValue>> Added;
	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	Root->TryGetArrayField(TEXT("entries"), Entries);
	if (Entries)
	{
		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
			FString Key;
			if (Entry.IsValid() && Entry->TryGetStringField(TEXT("key"), Key) && !CurrentKeys.Contains(Key))
			{
				Added.Add(MakeShared<FJsonValueString>(Key));
			}
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.string_table.diff_result.v1"));
	OutData->SetBoolField(TEXT("changed"), Added.Num() > 0 || !StringTable);
	OutData->SetArrayField(TEXT("added"), Added);
	OutData->SetArrayField(TEXT("removed"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetArrayField(TEXT("changed_entries"), TArray<TSharedPtr<FJsonValue>>());
	return true;
}

bool FUeAgentLegacyMechanics::CmdStringTableApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	FString AssetPath;
	bool bDryRun = true;
	bool bBackup = true;
	bool bSaveAfterApply = false;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("backup"), bBackup);
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	TSharedPtr<FJsonObject> Readback;
	TArray<TSharedPtr<FJsonValue>> Results;
	TArray<TSharedPtr<FJsonValue>> Backups;
	if (!ApplyStringTableRoot(Root, AssetPath, bDryRun, bBackup, bSaveAfterApply, Readback, Results, Backups, OutError))
	{
		OutData->SetArrayField(TEXT("issues"), Results);
		return false;
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.string_table.apply_result.v1"));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetArrayField(TEXT("backup_paths"), Backups);
	OutData->SetObjectField(TEXT("readback"), Readback.IsValid() ? Readback : MakeShared<FJsonObject>());
	OutData->SetBoolField(TEXT("created"), Readback.IsValid() && Readback->GetBoolField(TEXT("created")));
	OutData->SetNumberField(TEXT("changed_count"), Readback.IsValid() ? Readback->GetIntegerField(TEXT("changed_count")) : 0);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPackagingQueryEnvironment(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	bool bIncludeSdks = true;
	bool bIncludeTools = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_sdks"), bIncludeSdks);
	JsonTryGetBool(Ctx.Params, TEXT("include_tools"), bIncludeTools);
	OutData = BuildPackagingEnvironmentJson(bIncludeSdks, bIncludeTools);
	OutError.Reset();
	return true;
}

bool FUeAgentLegacyMechanics::CmdPackagingExportProfileJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString OutputFile;
	bool bIncludeProjectSettings = false;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	JsonTryGetBool(Ctx.Params, TEXT("include_project_settings"), bIncludeProjectSettings);
	FPackagingProfile Profile;
	ParsePackagingProfile(nullptr, Ctx.Params, Profile);
	OutData = BuildPackagingProfileJson(Profile);
	OutData->SetBoolField(TEXT("include_project_settings"), bIncludeProjectSettings);
	if (!OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString SavedPath;
		if (!SaveAuthoringFile(OutputFile, OutData, SavedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedPath);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPackagingValidateProfileJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	FPackagingProfile Profile;
	ParsePackagingProfile(Root, Ctx.Params, Profile);
	TArray<TSharedPtr<FJsonValue>> Issues;
	ValidatePackagingProfile(Profile, Issues);
	const bool bValid = CountErrors(Issues) == 0;
	OutData->SetStringField(TEXT("schema"), TEXT("uai.packaging.validate_result.v1"));
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	OutData->SetArrayField(TEXT("estimated_actions"), StringArrayToJson(BuildUatArgs(Profile)));
	if (!bValid)
	{
		OutError = TEXT("packaging_validate_failed");
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPackagingDiffProfileJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	FPackagingProfile Desired;
	ParsePackagingProfile(Root, Ctx.Params, Desired);
	FPackagingProfile Current;
	ParsePackagingProfile(nullptr, Ctx.Params, Current);
	TArray<TSharedPtr<FJsonValue>> SettingDiffs;
	if (!Desired.Platform.Equals(Current.Platform, ESearchCase::IgnoreCase))
	{
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetStringField(TEXT("field"), TEXT("platform"));
		Diff->SetStringField(TEXT("current"), Current.Platform);
		Diff->SetStringField(TEXT("desired"), Desired.Platform);
		SettingDiffs.Add(MakeShared<FJsonValueObject>(Diff));
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.packaging.diff_result.v1"));
	OutData->SetBoolField(TEXT("changed"), SettingDiffs.Num() > 0);
	OutData->SetArrayField(TEXT("setting_diffs"), SettingDiffs);
	OutData->SetArrayField(TEXT("transient_overrides"), StringArrayToJson(BuildUatArgs(Desired)));
	return true;
}

bool FUeAgentLegacyMechanics::CmdPackagingPlanBuild(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		if (HasJsonInput(Ctx.Params))
		{
			return false;
		}
		Root = nullptr;
		OutError.Reset();
	}
	FPackagingProfile Profile;
	ParsePackagingProfile(Root, Ctx.Params, Profile);
	TArray<TSharedPtr<FJsonValue>> Issues;
	ValidatePackagingProfile(Profile, Issues);
	OutData = BuildPlanJson(Profile);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetBoolField(TEXT("valid"), CountErrors(Issues) == 0);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPackagingParseLog(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString LogFile;
	bool bClassifyErrors = true;
	JsonTryGetString(Ctx.Params, TEXT("log_file"), LogFile);
	JsonTryGetBool(Ctx.Params, TEXT("classify_errors"), bClassifyErrors);
	if (LogFile.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_log_file");
		return false;
	}
	FString Text;
	const FString ResolvedLogFile = ResolveProjectFilePath(LogFile);
	if (!IsUaiOwnedSavedPath(ResolvedLogFile))
	{
		OutError = TEXT("log_file_not_uai_owned");
		return false;
	}
	if (!FFileHelper::LoadFileToString(Text, *ResolvedLogFile))
	{
		OutError = TEXT("log_file_not_found");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	if (bClassifyErrors)
	{
		ParseLogText(Text, Errors, Warnings);
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.packaging.parse_log.v1"));
	OutData->SetStringField(TEXT("log_file"), ResolvedLogFile);
	OutData->SetArrayField(TEXT("errors"), Errors);
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	TSharedPtr<FJsonObject> PhaseSummary = MakeShared<FJsonObject>();
	PhaseSummary->SetNumberField(TEXT("error_count"), Errors.Num());
	PhaseSummary->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetObjectField(TEXT("phase_summary"), PhaseSummary);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPackagingReportArtifacts(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString ArchiveDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterface/Packages"));
	FString Platform;
	bool bIncludeHashes = false;
	JsonTryGetString(Ctx.Params, TEXT("archive_dir"), ArchiveDir);
	JsonTryGetString(Ctx.Params, TEXT("platform"), Platform);
	JsonTryGetBool(Ctx.Params, TEXT("include_hashes"), bIncludeHashes);
	const FString ResolvedArchiveDir = ResolveProjectFilePath(ArchiveDir);
	if (!IsUaiOwnedPackagesPath(ResolvedArchiveDir))
	{
		OutError = TEXT("archive_dir_not_uai_owned");
		return false;
	}
	TArray<FString> Files;
	if (IFileManager::Get().DirectoryExists(*ResolvedArchiveDir))
	{
		IFileManager::Get().FindFilesRecursive(Files, *ResolvedArchiveDir, TEXT("*.*"), true, false, false);
	}
	int64 TotalSize = 0;
	TArray<TSharedPtr<FJsonValue>> Artifacts;
	for (const FString& File : Files)
	{
		const int64 Size = IFileManager::Get().FileSize(*File);
		TotalSize += FMath::Max<int64>(0, Size);
		TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
		Artifact->SetStringField(TEXT("path"), File);
		Artifact->SetNumberField(TEXT("size"), static_cast<double>(Size));
		if (bIncludeHashes)
		{
			Artifact->SetStringField(TEXT("md5"), LexToString(FMD5Hash::HashFile(*File)));
		}
		Artifacts.Add(MakeShared<FJsonValueObject>(Artifact));
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.packaging.artifacts.v1"));
	OutData->SetStringField(TEXT("archive_dir"), ResolvedArchiveDir);
	OutData->SetStringField(TEXT("platform"), Platform);
	OutData->SetArrayField(TEXT("artifacts"), Artifacts);
	OutData->SetNumberField(TEXT("total_size"), static_cast<double>(TotalSize));
	OutData->SetArrayField(TEXT("missing_expected"), TArray<TSharedPtr<FJsonValue>>());
	return true;
}

bool FUeAgentLegacyMechanics::CmdPackagingCleanArtifacts(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString ArchiveDir;
	bool bDryRun = true;
	bool bAllowDelete = false;
	double MaxDeleteBytes = 1024.0 * 1024.0 * 1024.0;
	JsonTryGetString(Ctx.Params, TEXT("archive_dir"), ArchiveDir);
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("allow_delete"), bAllowDelete);
	JsonTryGetNumber(Ctx.Params, TEXT("max_delete_bytes"), MaxDeleteBytes);
	const FString ResolvedArchiveDir = ResolveProjectFilePath(ArchiveDir);
	if (ResolvedArchiveDir.IsEmpty() || !IsUaiOwnedPackagesPath(ResolvedArchiveDir))
	{
		OutError = TEXT("archive_dir_not_uai_owned");
		return false;
	}
	if (!FMath::IsFinite(MaxDeleteBytes) || MaxDeleteBytes <= 0.0)
	{
		OutError = TEXT("max_delete_bytes_invalid");
		return false;
	}
	if (!bDryRun && !bAllowDelete)
	{
		OutError = TEXT("requires_delete_permission");
		return false;
	}
	TArray<FString> Files;
	if (IFileManager::Get().DirectoryExists(*ResolvedArchiveDir))
	{
		IFileManager::Get().FindFilesRecursive(Files, *ResolvedArchiveDir, TEXT("*.*"), true, false, false);
	}
	int64 TotalSize = 0;
	TArray<TSharedPtr<FJsonValue>> Deleted;
	for (const FString& File : Files)
	{
		TotalSize += FMath::Max<int64>(0, IFileManager::Get().FileSize(*File));
		Deleted.Add(MakeShared<FJsonValueString>(File));
	}
	if (static_cast<double>(TotalSize) > MaxDeleteBytes)
	{
		OutError = TEXT("max_delete_bytes_exceeded");
		return false;
	}
	if (!bDryRun && IFileManager::Get().DirectoryExists(*ResolvedArchiveDir))
	{
		if (!IFileManager::Get().DeleteDirectory(*ResolvedArchiveDir, false, true)
			|| IFileManager::Get().DirectoryExists(*ResolvedArchiveDir))
		{
			OutError = TEXT("delete_directory_failed");
			return false;
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.packaging.clean_artifacts.v1"));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("allow_delete"), bAllowDelete);
	OutData->SetArrayField(TEXT("deleted"), Deleted);
	OutData->SetNumberField(TEXT("freed_bytes"), static_cast<double>(TotalSize));
	return true;
}

bool FUeAgentLegacyMechanics::CmdTargetPlatformQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	bool bIncludeSdk = true;
	bool bIncludeTextureFormats = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_sdk"), bIncludeSdk);
	JsonTryGetBool(Ctx.Params, TEXT("include_texture_formats"), bIncludeTextureFormats);
	OutData = BuildPackagingEnvironmentJson(bIncludeSdk, false);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.target_platform.query.v1"));
	OutData->SetBoolField(TEXT("include_texture_formats"), bIncludeTextureFormats);
	OutData->SetArrayField(TEXT("sdk_status"), OutData->GetArrayField(TEXT("target_platforms")));
	return true;
}

bool FUeAgentLegacyMechanics::CmdPlatformConfigExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString Platform = TEXT("Windows");
	FString OutputFile;
	bool bIncludeSources = true;
	TArray<FString> Sections;
	JsonTryGetString(Ctx.Params, TEXT("platform"), Platform);
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	JsonTryGetBool(Ctx.Params, TEXT("include_sources"), bIncludeSources);
	TryGetStringArray(Ctx.Params, TEXT("sections"), Sections);
	OutData = BuildPlatformConfigExportJson(Platform, Sections, bIncludeSources);
	if (!OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString SavedPath;
		if (!SaveAuthoringFile(OutputFile, OutData, SavedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedPath);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPlatformConfigValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	const bool bValid = ValidatePlatformConfigRoot(Root, Issues);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.platform_config.validate_result.v1"));
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	if (!bValid)
	{
		OutError = TEXT("platform_config_validate_failed");
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPlatformConfigApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	bool bDryRun = true;
	bool bBackup = true;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("backup"), bBackup);
	TArray<TSharedPtr<FJsonValue>> Changes;
	TArray<TSharedPtr<FJsonValue>> Backups;
	if (!ApplyPlatformConfigRoot(Root, bDryRun, bBackup, Changes, Backups, OutError))
	{
		OutData->SetArrayField(TEXT("issues"), Changes);
		return false;
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.platform_config.apply_result.v1"));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetArrayField(TEXT("changes"), Changes);
	OutData->SetArrayField(TEXT("backup_paths"), Backups);
	return true;
}

bool FUeAgentLegacyMechanics::CmdDeviceProfileQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString Profile;
	bool bIncludeInherited = true;
	JsonTryGetString(Ctx.Params, TEXT("profile"), Profile);
	JsonTryGetBool(Ctx.Params, TEXT("include_inherited"), bIncludeInherited);
	OutData = BuildDeviceProfileExportJson(Profile, bIncludeInherited);
	if (!Profile.IsEmpty())
	{
		TArray<FString> Chain;
		TArray<TSharedPtr<FJsonValue>> Conflicts;
		const TMap<FString, FString> CVars = ResolveDeviceProfileCVars(Profile, Chain, Conflicts);
		OutData->SetArrayField(TEXT("inheritance_chain"), StringArrayToJson(Chain));
		OutData->SetObjectField(TEXT("cvars"), MakeStringMapObject(CVars));
		OutData->SetArrayField(TEXT("conflicts"), Conflicts);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdDeviceProfileExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString Profile;
	FString OutputFile;
	bool bIncludeInherited = true;
	JsonTryGetString(Ctx.Params, TEXT("profile"), Profile);
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	JsonTryGetBool(Ctx.Params, TEXT("include_inherited"), bIncludeInherited);
	OutData = BuildDeviceProfileExportJson(Profile, bIncludeInherited);
	if (!OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString SavedPath;
		if (!SaveAuthoringFile(OutputFile, OutData, SavedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedPath);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdDeviceProfileValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	const bool bValid = ValidateDeviceProfileRoot(Root, Issues);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.device_profile.validate_result.v1"));
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	if (!bValid)
	{
		OutError = TEXT("device_profile_validate_failed");
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdDeviceProfileDiffJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!ValidateDeviceProfileRoot(Root, Issues))
	{
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutError = TEXT("device_profile_validate_failed");
		return false;
	}
	const TMap<FString, FDeviceProfileInfo> Existing = ReadDeviceProfiles();
	TArray<TSharedPtr<FJsonValue>> AddedCVars;
	TArray<TSharedPtr<FJsonValue>> ChangedCVars;
	const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
	Root->TryGetArrayField(TEXT("profiles"), Profiles);
	if (Profiles)
	{
		for (const TSharedPtr<FJsonValue>& ProfileValue : *Profiles)
		{
			const TSharedPtr<FJsonObject> Profile = ProfileValue.IsValid() ? ProfileValue->AsObject() : nullptr;
			FString Name;
			if (!Profile.IsValid() || !Profile->TryGetStringField(TEXT("name"), Name))
			{
				continue;
			}
			const FDeviceProfileInfo* ExistingProfile = Existing.Find(Name);
			const TSharedPtr<FJsonObject>* CVars = nullptr;
			if (Profile->TryGetObjectField(TEXT("cvars"), CVars) && CVars && CVars->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*CVars)->Values)
				{
					FString DesiredValue;
					Pair.Value->TryGetString(DesiredValue);
					if (!ExistingProfile || !ExistingProfile->CVars.Contains(Pair.Key))
					{
						AddedCVars.Add(MakeShared<FJsonValueString>(Name + TEXT(":") + Pair.Key));
					}
					else if (ExistingProfile->CVars[Pair.Key] != DesiredValue)
					{
						ChangedCVars.Add(MakeShared<FJsonValueString>(Name + TEXT(":") + Pair.Key));
					}
				}
			}
		}
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.device_profile.diff_result.v1"));
	OutData->SetBoolField(TEXT("changed"), AddedCVars.Num() > 0 || ChangedCVars.Num() > 0);
	OutData->SetArrayField(TEXT("added_cvars"), AddedCVars);
	OutData->SetArrayField(TEXT("removed_cvars"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetArrayField(TEXT("changed_cvars"), ChangedCVars);
	return true;
}

bool FUeAgentLegacyMechanics::CmdDeviceProfileApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	bool bDryRun = true;
	bool bBackup = true;
	bool bReadBack = true;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("backup"), bBackup);
	JsonTryGetBool(Ctx.Params, TEXT("read_back"), bReadBack);
	TArray<TSharedPtr<FJsonValue>> Changes;
	TArray<TSharedPtr<FJsonValue>> Backups;
	if (!ApplyDeviceProfiles(Root, bDryRun, bBackup, Changes, Backups, OutError))
	{
		OutData->SetArrayField(TEXT("issues"), Changes);
		return false;
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.device_profile.apply_result.v1"));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("changed"), Changes.Num() > 0);
	OutData->SetArrayField(TEXT("changes"), Changes);
	OutData->SetArrayField(TEXT("backup_paths"), Backups);
	if (bReadBack)
	{
		OutData->SetObjectField(TEXT("readback"), BuildDeviceProfileExportJson(FString(), true));
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdDeviceProfileResolveCVars(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString Profile;
	TArray<FString> Keys;
	bool bIncludeSources = true;
	JsonTryGetString(Ctx.Params, TEXT("profile"), Profile);
	JsonTryGetBool(Ctx.Params, TEXT("include_sources"), bIncludeSources);
	TryGetStringArray(Ctx.Params, TEXT("keys"), Keys);
	if (Profile.IsEmpty() || !ReadDeviceProfiles().Contains(Profile))
	{
		OutError = TEXT("device_profile_not_found");
		return false;
	}
	TArray<FString> Chain;
	TArray<TSharedPtr<FJsonValue>> Conflicts;
	TMap<FString, FString> Resolved = ResolveDeviceProfileCVars(Profile, Chain, Conflicts);
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const TPair<FString, FString>& Pair : Resolved)
	{
		if (Keys.Num() > 0 && !Keys.Contains(Pair.Key))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("key"), Pair.Key);
		Item->SetStringField(TEXT("value"), Pair.Value);
		if (bIncludeSources)
		{
			Item->SetArrayField(TEXT("inheritance_chain"), StringArrayToJson(Chain));
		}
		Values.Add(MakeShared<FJsonValueObject>(Item));
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.device_profile.resolved_cvars.v1"));
	OutData->SetStringField(TEXT("profile"), Profile);
	OutData->SetArrayField(TEXT("inheritance_chain"), StringArrayToJson(Chain));
	OutData->SetArrayField(TEXT("resolved_cvars"), Values);
	OutData->SetArrayField(TEXT("conflicts"), Conflicts);
	return true;
}

bool FUeAgentLegacyMechanics::CmdScalabilityExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	FString Platform;
	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("platform"), Platform);
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutData = BuildScalabilityJson();
	OutData->SetStringField(TEXT("platform"), Platform);
	if (!OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString SavedPath;
		if (!SaveAuthoringFile(OutputFile, OutData, SavedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedPath);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdScalabilityValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	const bool bValid = ValidateScalabilityRoot(Root, Issues);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.scalability.validate_result.v1"));
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	if (!bValid)
	{
		OutError = TEXT("scalability_validate_failed");
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdScalabilityApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	bool bDryRun = true;
	bool bBackup = true;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("backup"), bBackup);
	TArray<TSharedPtr<FJsonValue>> Changes;
	TArray<TSharedPtr<FJsonValue>> Backups;
	if (!ApplyScalabilityRoot(Root, bDryRun, bBackup, Changes, Backups, OutError))
	{
		OutData->SetArrayField(TEXT("issues"), Changes);
		return false;
	}
	OutData->SetStringField(TEXT("schema"), TEXT("uai.scalability.apply_result.v1"));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetArrayField(TEXT("changes"), Changes);
	OutData->SetArrayField(TEXT("backup_paths"), Backups);
	OutData->SetObjectField(TEXT("readback"), bDryRun ? MakeShared<FJsonObject>() : BuildScalabilityJson());
	return true;
}

bool FUeAgentLegacyMechanics::CmdPlatformProfileValidatePackaging(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	FString Platform = TEXT("Win64");
	FString DeviceProfile;
	JsonTryGetString(Ctx.Params, TEXT("platform"), Platform);
	JsonTryGetString(Ctx.Params, TEXT("device_profile"), DeviceProfile);
	if (!JsonTryGetObject(Ctx.Params, TEXT("packaging_json"), Root))
	{
		LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError);
		OutError.Reset();
	}
	FPackagingProfile Profile;
	ParsePackagingProfile(Root, Ctx.Params, Profile);
	if (!Platform.IsEmpty())
	{
		Profile.Platform = Platform;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	ValidatePackagingProfile(Profile, Issues);
	if (!DeviceProfile.IsEmpty() && !ReadDeviceProfiles().Contains(DeviceProfile))
	{
		AddIssue(Issues, TEXT("error"), TEXT("device_profile_not_found"), TEXT("$.device_profile"), DeviceProfile);
	}
	const bool bValid = CountErrors(Issues) == 0;
	OutData->SetStringField(TEXT("schema"), TEXT("uai.platform_profile.validate_packaging.v1"));
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	if (!bValid)
	{
		OutError = TEXT("platform_profile_validate_failed");
		return false;
	}
	return true;
}
