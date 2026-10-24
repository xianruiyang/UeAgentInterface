// Copyright Epic Games, Inc. All Rights Reserved.

#include "DomainAdapters/EditorProject/UeAgentEditorProjectQueryMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"

#include "Utilities/UeAgentAuthoringFileIO.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

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
	


	static FString ProfilePointerPart(const FString& Value)
	{
		return Value.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
	}

	static TSharedPtr<FJsonValue> ProfileActual(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->Type == EJson::String) return Value;
		return MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Value));
	}

	static UE::UAI::Contract::FDiagnostic& AddProfileIssue(
		TArray<UE::UAI::Contract::FDiagnostic>& Issues, const FString& Code, const FString& Reason,
		const FJsonObject* Source, const FString& Field, const TSharedPtr<FJsonValue>& Actual,
		const TSharedPtr<FJsonValue>& Expected, const FString& RelativePointer = FString())
	{
		auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("validation"));
		Error.ActualValue = Actual;
		Error.ExpectedValue = Expected;
		Error.ActualValuePresent = Actual.IsValid();
		Error.ExpectedValuePresent = Expected.IsValid();
		Error.Source.JsonPath = RelativePointer;
		if (Source) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Source, Field);
		if (Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("command_input");
		if (Error.Source.JsonPath.IsEmpty() && !Field.IsEmpty()) Error.Source.JsonPath = TEXT("/") + ProfilePointerPart(Field);
		return Issues.Emplace_GetRef(MoveTemp(Error));
	}

	static UE::UAI::Contract::FDiagnostic ProfileValidationFailure(const FString& Code,
		const FString& Reason, const TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("validation"));
		Error.SourceKind = TEXT("native_result");
		Error.PropertyPath = TEXT("/error_count");
		Error.ActualValue = MakeShared<FJsonValueNumber>(UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error")));
		Error.ExpectedValue = MakeShared<FJsonValueNumber>(0);
		Error.ActualValuePresent = true;
		Error.ExpectedValuePresent = true;
		Error.Diagnostics = Issues;
		return Error;
	}

	static TUniquePtr<UeAgentAuthoringDiagnostics::FScopedSourceLocations> BindProfileSources(
		const FJsonObject* Root, const FUeAgentStructuredHjsonDocument* Document = nullptr)
	{
		if (!Root) return nullptr;
		TMap<FString, UE::UAI::Contract::FSourceLocation> Sources;
		if (Document)
		{
			for (const auto& Pair : Document->SourceLocations)
			{
				UE::UAI::Contract::FSourceLocation Location;
				Location.SourceFile = Pair.Value.FilePath.IsEmpty() ? Document->SourceFile : Pair.Value.FilePath;
				Location.JsonPath = Pair.Key;
				Location.Line = Pair.Value.Line;
				Location.Column = Pair.Value.Column;
				Sources.Add(Pair.Key, MoveTemp(Location));
			}
		}
		return MakeUnique<UeAgentAuthoringDiagnostics::FScopedSourceLocations>(Root,
			[Sources = MoveTemp(Sources), bFile = Document != nullptr](const FString& Pointer)
			{
				if (bFile) return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Sources, Pointer);
				UE::UAI::Contract::FSourceLocation Location;
				Location.JsonPath = Pointer;
				return Location;
			}, Document == nullptr);
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

	static int32 CountErrors(const TArray<UE::UAI::Contract::FDiagnostic>& Issues)
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

	static bool TryGetStringArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName,
		TArray<FString>& OutValues, TArray<FString>* OutItemPointers = nullptr)
	{
		OutValues.Reset();
		if (OutItemPointers) OutItemPointers->Reset();
		if (!Obj.IsValid())
		{
			return false;
		}
		const FString FieldPointer = TEXT("/") + ProfilePointerPart(FieldName);
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Obj->TryGetArrayField(FieldName, Array) && Array)
		{
			for (int32 Index = 0; Index < Array->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*Array)[Index];
				FString Text;
				if (Value.IsValid() && Value->TryGetString(Text))
				{
					Text.TrimStartAndEndInline();
					if (!Text.IsEmpty())
					{
						OutValues.Add(Text);
						if (OutItemPointers) OutItemPointers->Add(FieldPointer + TEXT("/") + FString::FromInt(Index));
					}
				}
			}
			return true;
		}
		FString SingleValue;
		if (Obj->TryGetStringField(FieldName, SingleValue) && !SingleValue.TrimStartAndEnd().IsEmpty())
		{
			OutValues.Add(SingleValue);
			if (OutItemPointers) OutItemPointers->Add(FieldPointer);
			return true;
		}
		return false;
	}

	static bool SaveAuthoringFile(const FString& OutputFileInput, const TSharedPtr<FJsonObject>& Root, FString& OutResolvedPath, UE::UAI::Contract::FDiagnostic& OutError)
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

	static bool LoadJsonFromParams(const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutRoot, FString& OutJsonFile,
		FUeAgentStructuredHjsonDocument& OutDocument, UE::UAI::Contract::FDiagnostic& OutError,
		bool bRequired = true)
	{
		OutRoot.Reset();
		OutJsonFile.Reset();
		OutDocument = FUeAgentStructuredHjsonDocument();
		OutError.Reset();
		if (!Params.IsValid())
		{
			if (!bRequired) return true;
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("missing_params"),
				TEXT("The profile operation requires an input parameter object."), TEXT("validation"));
			OutError.SourceKind = TEXT("command_input");
			OutError.ActualValuePresent = false;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
			OutError.ExpectedValuePresent = true;
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
			FString ResolvedPath;
			if (!UeAgentAuthoringFileIO::LoadDocument(OutJsonFile, OutDocument, ResolvedPath, OutError))
				return false;
			OutRoot = OutDocument.Root;
			OutJsonFile = ResolvedPath;
			return true;
		}
		if (!bRequired) return true;
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("missing_json_or_json_file"),
			TEXT("Supply a profile json object or a nonempty json_file path."), TEXT("validation"));
		OutError.SourceKind = TEXT("command_input");
		const auto Actual = MakeShared<FJsonObject>();
		Actual->SetStringField(TEXT("json"), UeAgentJsonDiagnostics::JsonValueTypeToString(Params->Values.FindRef(TEXT("json"))));
		Actual->SetStringField(TEXT("json_file"), UeAgentJsonDiagnostics::JsonValueTypeToString(Params->Values.FindRef(TEXT("json_file"))));
		OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
		OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("json object or HJSON json_file path"));
		OutError.ActualValuePresent = true;
		OutError.ExpectedValuePresent = true;
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


	static bool BackupTextFile(const FString& FilePath, TArray<TSharedPtr<FJsonValue>>& BackupPaths, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!FPaths::FileExists(FilePath))
		{
			return true;
		}
		const FString BackupPath = FilePath + TEXT(".bak_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const int32 CopyResult = IFileManager::Get().Copy(*BackupPath, *FilePath);
		if (CopyResult == COPY_OK)
		{
			BackupPaths.Add(MakeShared<FJsonValueString>(BackupPath));
			return true;
		}
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("backup_config_failed"),
			TEXT("The configuration file could not be copied to its backup."), TEXT("execution"),
			FilePath + TEXT(" -> ") + BackupPath);
		OutError.SourceKind = TEXT("native_result");
		OutError.TargetPath = FPaths::GetCleanFilename(FilePath);
		OutError.PropertyPath = TEXT("/copy_result");
		OutError.ActualValue = MakeShared<FJsonValueNumber>(CopyResult);
		OutError.ExpectedValue = MakeShared<FJsonValueNumber>(COPY_OK);
		return false;
	}

	static bool SaveTextFileUtf8(const FString& FilePath, const FString& Text, UE::UAI::Contract::FDiagnostic& OutError)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		if (!FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("save_text_file_failed"),
				TEXT("The configuration text could not be written as UTF-8."), TEXT("execution"), FilePath);
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = FPaths::GetCleanFilename(FilePath);
			OutError.PropertyPath = TEXT("/write_succeeded");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
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

	static bool SaveAssetPackage(UObject* Asset, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Asset || !Asset->GetOutermost())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_not_found"),
				Asset ? TEXT("The asset has no package to save.") : TEXT("There is no asset to save."), TEXT("execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = Asset ? Asset->GetPathName() : FString();
			OutError.PropertyPath = Asset ? TEXT("/package_exists") : TEXT("/asset_exists");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		TArray<UPackage*> Packages;
		Packages.Add(Asset->GetOutermost());
		if (!UE::UAI::EditorResources::SavePackages(Packages, false, OutError))
		{
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("save_asset_failed"), OutError,
				TEXT("execution"), TEXT("The asset package could not be saved."));
			OutError.TargetPath = Asset->GetPathName();
			return false;
		}
		return true;
	}

	static UStringTable* CreateStringTableAsset(const FString& AssetPath, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const FString PackageName = NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("invalid_asset_path"), TEXT("/asset_path"),
				MakeShared<FJsonValueString>(TEXT("valid long package name")), MakeShared<FJsonValueString>(AssetPath),
				TEXT("validation"), TEXT("The String Table asset path does not identify a valid package."));
			OutError.SourceKind = TEXT("command_input");
			return nullptr;
		}
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("create_package_failed"),
				TEXT("The String Table package could not be created."), TEXT("execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = PackageName;
			OutError.PropertyPath = TEXT("/package_exists");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return nullptr;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		UStringTable* StringTable = NewObject<UStringTable>(Package, UStringTable::StaticClass(), *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!StringTable)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("create_string_table_failed"),
				TEXT("The String Table object could not be created in the package."), TEXT("execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = PackageName;
			OutError.PropertyPath = TEXT("/asset_exists");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
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

	static bool ValidateStringTableRoot(const TSharedPtr<FJsonObject>& Root, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		if (!Root.IsValid())
		{
			AddProfileIssue(Issues, TEXT("json_root_invalid"), TEXT("String Table input must be an object."),
				nullptr, FString(), nullptr, MakeShared<FJsonValueString>(TEXT("object")));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Root->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
		{
			AddProfileIssue(Issues, TEXT("missing_entries"), TEXT("String Table input requires an entries array."),
				Root.Get(), TEXT("entries"), ProfileActual(Root->Values.FindRef(TEXT("entries"))),
				MakeShared<FJsonValueString>(TEXT("array")));
			return false;
		}
		TSet<FString> Keys;
		for (int32 Index = 0; Index < Entries->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Entry = (*Entries)[Index].IsValid() ? (*Entries)[Index]->AsObject() : nullptr;
			if (!Entry.IsValid())
			{
				AddProfileIssue(Issues, TEXT("entry_not_object"), TEXT("A String Table entry must be an object."),
					Root.Get(), FString(), ProfileActual((*Entries)[Index]), MakeShared<FJsonValueString>(TEXT("object")),
					TEXT("/entries/") + FString::FromInt(Index));
				continue;
			}
			FString Key;
			if (!Entry->TryGetStringField(TEXT("key"), Key) || Key.TrimStartAndEnd().IsEmpty())
			{
				AddProfileIssue(Issues, TEXT("string_table_key_missing"), TEXT("A String Table entry requires a nonempty key."),
					Entry.Get(), TEXT("key"), ProfileActual(Entry->Values.FindRef(TEXT("key"))),
					MakeShared<FJsonValueString>(TEXT("nonempty string")));
			}
			else if (Keys.Contains(Key))
			{
				AddProfileIssue(Issues, TEXT("string_table_key_duplicate"), TEXT("The input repeats a String Table key."),
					Entry.Get(), TEXT("key"), MakeShared<FJsonValueString>(Key),
					MakeShared<FJsonValueString>(TEXT("unique key")));
			}
			else
			{
				Keys.Add(Key);
			}
			FString Source;
			if (!Entry->TryGetStringField(TEXT("source"), Source))
			{
				AddProfileIssue(Issues, TEXT("string_table_source_missing"), TEXT("A String Table entry requires a source string."),
					Entry.Get(), TEXT("source"), ProfileActual(Entry->Values.FindRef(TEXT("source"))),
					MakeShared<FJsonValueString>(TEXT("string")));
			}
			const TSharedPtr<FJsonObject>* Metadata = nullptr;
			if (Entry->HasField(TEXT("metadata")) && (!Entry->TryGetObjectField(TEXT("metadata"), Metadata) || !Metadata || !Metadata->IsValid()))
			{
				AddProfileIssue(Issues, TEXT("metadata_not_object"), TEXT("String Table metadata must be an object."),
					Entry.Get(), TEXT("metadata"), ProfileActual(Entry->Values.FindRef(TEXT("metadata"))),
					MakeShared<FJsonValueString>(TEXT("object")));
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutReadback.Reset();
		TArray<UE::UAI::Contract::FDiagnostic> Issues;
		if (!ValidateStringTableRoot(Root, Issues))
		{
			OutError = ProfileValidationFailure(TEXT("string_table_validate_failed"),
				TEXT("String Table input contains invalid entries."), Issues);
			OutResults = UE::UAI::Contract::ToPublicDiagnostics(Issues);
			return false;
		}
		FString AssetPath = ExplicitAssetPath;
		if (AssetPath.TrimStartAndEnd().IsEmpty())
		{
			Root->TryGetStringField(TEXT("asset_path"), AssetPath);
		}
		if (AssetPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = AddProfileIssue(Issues, TEXT("missing_asset_path"),
				TEXT("String Table creation requires an asset path."), Root.Get(), TEXT("asset_path"),
				ProfileActual(Root->Values.FindRef(TEXT("asset_path"))), MakeShared<FJsonValueString>(TEXT("nonempty asset path")));
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

	static bool UpsertIniSections(const FString& FilePath, const TMap<FString, FString>& SectionTextByName, bool bBackup, TArray<TSharedPtr<FJsonValue>>& BackupPaths, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FString ExistingText;
		FFileHelper::LoadFileToString(ExistingText, *FilePath);
		if (bBackup && FPaths::FileExists(FilePath) && !BackupTextFile(FilePath, BackupPaths, OutError))
		{
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

	static bool ValidateLocalizationRoot(const TSharedPtr<FJsonObject>& Root, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		if (!Root.IsValid())
		{
			AddProfileIssue(Issues, TEXT("json_root_invalid"), TEXT("Localization input must be an object."),
				nullptr, FString(), nullptr, MakeShared<FJsonValueString>(TEXT("object")));
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
			const FString Field = Root->HasField(TEXT("settings")) ? TEXT("settings")
				: Root->HasField(TEXT("project_settings")) ? TEXT("project_settings")
				: Root->HasField(TEXT("pages")) ? TEXT("pages")
				: Root->HasField(TEXT("categories")) ? TEXT("categories") : TEXT("sections");
			AddProfileIssue(Issues, TEXT("use_project_settings_required"),
				TEXT("Project Settings fields must be edited through editor_project.project_settings.update."),
				Root.Get(), Field, ProfileActual(Root->Values.FindRef(Field)),
				MakeShared<FJsonValueString>(TEXT("localization targets or string_tables")));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
		if (Root->TryGetArrayField(TEXT("targets"), Targets) && Targets)
		{
			TSet<FString> SeenTargets;
			for (int32 Index = 0; Index < Targets->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Target = (*Targets)[Index].IsValid() ? (*Targets)[Index]->AsObject() : nullptr;
				if (!Target.IsValid())
				{
					AddProfileIssue(Issues, TEXT("localization_target_config_invalid"), TEXT("A localization target must be an object."),
						Root.Get(), FString(), ProfileActual((*Targets)[Index]), MakeShared<FJsonValueString>(TEXT("object")),
						TEXT("/targets/") + FString::FromInt(Index));
					continue;
				}
				FString Name;
				if (!Target->TryGetStringField(TEXT("name"), Name) || !IsSimpleIdentifier(Name))
				{
					AddProfileIssue(Issues, TEXT("localization_target_config_invalid"),
						TEXT("A localization target requires a name containing only simple identifier characters."),
						Target.Get(), TEXT("name"), ProfileActual(Target->Values.FindRef(TEXT("name"))),
						MakeShared<FJsonValueString>(TEXT("nonempty simple identifier")));
				}
				else if (SeenTargets.Contains(Name))
				{
					AddProfileIssue(Issues, TEXT("localization_target_duplicate"), TEXT("The input repeats a localization target name."),
						Target.Get(), TEXT("name"), MakeShared<FJsonValueString>(Name),
						MakeShared<FJsonValueString>(TEXT("unique target name")));
				}
				else
				{
					SeenTargets.Add(Name);
				}
				TArray<FString> Cultures;
				TArray<FString> CulturePointers;
				TryGetStringArray(Target, TEXT("cultures"), Cultures, &CulturePointers);
				FString NativeCulture;
				if (Target->TryGetStringField(TEXT("native_culture"), NativeCulture) && !Cultures.Contains(NativeCulture))
				{
					Cultures.Add(NativeCulture);
					CulturePointers.Add(TEXT("/native_culture"));
				}
				for (int32 CultureIndex = 0; CultureIndex < Cultures.Num(); ++CultureIndex)
				{
					const FString& Culture = Cultures[CultureIndex];
					if (!IsCultureNameAllowed(Culture))
					{
						AddProfileIssue(Issues, TEXT("culture_invalid"), TEXT("The culture is invalid or unavailable."),
							Target.Get(), FString(), MakeShared<FJsonValueString>(Culture),
							MakeShared<FJsonValueString>(TEXT("available culture name")), CulturePointers[CultureIndex]);
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
					AddProfileIssue(Issues, TEXT("string_table_config_invalid"), TEXT("A String Table configuration item must be an object."),
						Root.Get(), FString(), ProfileActual((*StringTables)[Index]), MakeShared<FJsonValueString>(TEXT("object")),
						TEXT("/string_tables/") + FString::FromInt(Index));
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

	struct FPackagingInputField
	{
		bool bFromInput = false;
		UE::UAI::Contract::FSourceLocation Location;
	};

	struct FPackagingInputSources
	{
		FPackagingInputField Platform;
		FPackagingInputField Configuration;
		FPackagingInputField ArchiveDirectory;
		TArray<FPackagingInputField> Maps;
		TArray<FPackagingInputField> Cultures;
	};

	static void RecordPackagingField(const FJsonObject& Object, const FString& Field,
		FPackagingInputField& Out, const FString& RelativePointer = FString())
	{
		Out.bFromInput = true;
		UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(Object, Field, Out.Location,
			RelativePointer.IsEmpty() ? FString() : TEXT("/") + ProfilePointerPart(Field) + RelativePointer);
	}

	static UE::UAI::Contract::FDiagnostic& AddPackagingIssue(
		TArray<UE::UAI::Contract::FDiagnostic>& Issues, const FString& Code, const FString& Reason,
		const FPackagingInputField& Input, const FString& Path,
		const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
	{
		auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("validation"));
		Error.PropertyPath = Path;
		if (Input.bFromInput) Error.Source = Input.Location;
		Error.SourceKind = Input.bFromInput
			? Input.Location.SourceFile.IsEmpty() ? TEXT("command_input") : TEXT("related_input")
			: TEXT("native_result");
		Error.ActualValue = Actual;
		Error.ExpectedValue = Expected;
		Error.ActualValuePresent = Actual.IsValid();
		Error.ExpectedValuePresent = Expected.IsValid();
		return Issues.Emplace_GetRef(MoveTemp(Error));
	}

	static bool ParsePackagingProfile(const TSharedPtr<FJsonObject>& Root, const TSharedPtr<FJsonObject>& Params, FPackagingProfile& OutProfile, FPackagingInputSources& Sources)
	{
		Sources = FPackagingInputSources();
		auto ReadTrackedString = [](const TSharedPtr<FJsonObject>& Object, const FString& Field,
			FString& Value, FPackagingInputField& Input)
		{
			if (Object->TryGetStringField(Field, Value)) RecordPackagingField(*Object, Field, Input);
		};
		auto RecordArray = [](const FJsonObject& Object, const FString& Field, int32 Count,
			TArray<FPackagingInputField>& Locations)
		{
			Locations.SetNum(Count);
			for (int32 Index = 0; Index < Count; ++Index)
				RecordPackagingField(Object, Field, Locations[Index], TEXT("/") + FString::FromInt(Index));
		};
		if (Params.IsValid())
		{
			ReadTrackedString(Params, TEXT("platform"), OutProfile.Platform, Sources.Platform);
			ReadTrackedString(Params, TEXT("configuration"), OutProfile.Configuration, Sources.Configuration);
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
				ReadTrackedString(*TargetObj, TEXT("platform"), OutProfile.Platform, Sources.Platform);
				ReadTrackedString(*TargetObj, TEXT("configuration"), OutProfile.Configuration, Sources.Configuration);
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
				RecordArray(**ContentObj, TEXT("maps"), OutProfile.Maps.Num(), Sources.Maps);
				TryGetStringArray(*ContentObj, TEXT("cultures"), OutProfile.Cultures);
				RecordArray(**ContentObj, TEXT("cultures"), OutProfile.Cultures.Num(), Sources.Cultures);
			}
			const TSharedPtr<FJsonObject>* OutputObj = nullptr;
			if (Root->TryGetObjectField(TEXT("output"), OutputObj) && OutputObj && OutputObj->IsValid())
			{
				ReadTrackedString(*OutputObj, TEXT("archive_directory"), OutProfile.ArchiveDirectory, Sources.ArchiveDirectory);
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
			Sources.Platform = FPackagingInputField();
		}
		if (OutProfile.ArchiveDirectory.IsEmpty())
		{
			OutProfile.ArchiveDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterface/Packages"), OutProfile.Platform);
			Sources.ArchiveDirectory = FPackagingInputField();
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

	static void ValidatePackagingProfile(const FPackagingProfile& Profile,
		const FPackagingInputSources& Sources, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		if (!IsSimpleIdentifier(Profile.Platform) || Profile.Platform.Contains(TEXT("@")) || ContainsUnsafeShellMeta(Profile.Platform))
			AddPackagingIssue(Issues, TEXT("platform_token_unsafe"),
				TEXT("The packaging platform must be a shell-safe simple identifier without '@'."),
				Sources.Platform, TEXT("/platform"), MakeShared<FJsonValueString>(Profile.Platform),
				MakeShared<FJsonValueString>(TEXT("shell-safe simple identifier without '@'")));
		if (!IsKnownConfiguration(Profile.Configuration))
		{
			TArray<TSharedPtr<FJsonValue>> Choices;
			for (const TCHAR* Name : {TEXT("Debug"), TEXT("DebugGame"), TEXT("Development"), TEXT("Test"), TEXT("Shipping")})
				Choices.Add(MakeShared<FJsonValueString>(Name));
			AddPackagingIssue(Issues, TEXT("configuration_invalid"), TEXT("The packaging build configuration is unsupported."),
				Sources.Configuration, TEXT("/configuration"), MakeShared<FJsonValueString>(Profile.Configuration),
				MakeShared<FJsonValueArray>(MoveTemp(Choices)));
		}
		if (!FPaths::FileExists(RunUatPath()))
		{
			auto& Issue = AddPackagingIssue(Issues, TEXT("uat_not_found"),
				TEXT("The Unreal Automation Tool entry script does not exist."), FPackagingInputField(),
				TEXT("/uat_file_exists"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			Issue.Source.SourceFile = RunUatPath();
			Issue.CandidateName = TEXT("RunUAT");
		}
		for (int32 Index = 0; Index < Profile.Maps.Num(); ++Index)
		{
			const FString& Map = Profile.Maps[Index];
			const auto Input = Sources.Maps.IsValidIndex(Index) ? Sources.Maps[Index] : FPackagingInputField();
			const FString Path = TEXT("/maps/") + FString::FromInt(Index);
			if (ContainsUnsafeShellMeta(Map) || !FPackageName::IsValidLongPackageName(Map))
			{
				AddPackagingIssue(Issues, TEXT("map_token_unsafe"),
					TEXT("The map must be a shell-safe long package name."), Input, Path,
					MakeShared<FJsonValueString>(Map), MakeShared<FJsonValueString>(TEXT("shell-safe long package name"))).CandidateName = Map;
				continue;
			}
			if (!FPackageName::DoesPackageExist(Map))
				AddPackagingIssue(Issues, TEXT("map_not_found"), TEXT("The requested map package does not exist."),
					Input, Path, MakeShared<FJsonValueString>(Map),
					MakeShared<FJsonValueString>(TEXT("an existing map package"))).CandidateName = Map;
		}
		for (int32 Index = 0; Index < Profile.Cultures.Num(); ++Index)
		{
			const FString& Culture = Profile.Cultures[Index];
			if (!IsCultureNameAllowed(Culture) || ContainsUnsafeShellMeta(Culture))
				AddPackagingIssue(Issues, TEXT("culture_token_unsafe"),
					TEXT("The culture name is invalid or contains shell metacharacters."),
					Sources.Cultures.IsValidIndex(Index) ? Sources.Cultures[Index] : FPackagingInputField(),
					TEXT("/cultures/") + FString::FromInt(Index), MakeShared<FJsonValueString>(Culture),
					MakeShared<FJsonValueString>(TEXT("an allowed shell-safe culture name"))).CandidateName = Culture;
		}
		if (!IsUaiOwnedPackagesPath(Profile.ArchiveDirectory))
			AddPackagingIssue(Issues, TEXT("archive_dir_unsafe"),
				TEXT("The archive directory lies outside the owned packages directory."),
				Sources.ArchiveDirectory, TEXT("/archive_directory"), MakeShared<FJsonValueString>(Profile.ArchiveDirectory),
				MakeShared<FJsonValueString>(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterface/Packages"))));
		else if (ContainsUnsafeShellMeta(Profile.ArchiveDirectory))
			AddPackagingIssue(Issues, TEXT("archive_dir_token_unsafe"),
				TEXT("The archive directory contains shell metacharacters."),
				Sources.ArchiveDirectory, TEXT("/archive_directory"), MakeShared<FJsonValueString>(Profile.ArchiveDirectory),
				MakeShared<FJsonValueString>(TEXT("a path without shell metacharacters")));
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

	static bool ValidateDeviceProfileRoot(const TSharedPtr<FJsonObject>& Root, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		if (!Root.IsValid())
		{
			AddProfileIssue(Issues, TEXT("json_root_invalid"), TEXT("Device profile input must be an object."),
				nullptr, FString(), nullptr, MakeShared<FJsonValueString>(TEXT("object")));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
		if (!Root->TryGetArrayField(TEXT("profiles"), Profiles) || !Profiles)
		{
			AddProfileIssue(Issues, TEXT("missing_profiles"), TEXT("Device profile input requires a profiles array."),
				Root.Get(), TEXT("profiles"), ProfileActual(Root->Values.FindRef(TEXT("profiles"))),
				MakeShared<FJsonValueString>(TEXT("array")));
			return false;
		}
		TSet<FString> Names;
		TMap<FString, FString> BaseByProfile;
		TMap<FString, TSharedPtr<FJsonObject>> ProfileSources;
		for (int32 Index = 0; Index < Profiles->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Profile = (*Profiles)[Index].IsValid() ? (*Profiles)[Index]->AsObject() : nullptr;
			if (!Profile.IsValid())
			{
				AddProfileIssue(Issues, TEXT("device_profile_invalid"), TEXT("A device profile entry must be an object."),
					Root.Get(), FString(), ProfileActual((*Profiles)[Index]), MakeShared<FJsonValueString>(TEXT("object")),
					TEXT("/profiles/") + FString::FromInt(Index));
				continue;
			}
			FString Name;
			if (!Profile->TryGetStringField(TEXT("name"), Name) || !IsSimpleIdentifier(Name))
			{
				AddProfileIssue(Issues, TEXT("device_profile_not_found"),
					TEXT("A device profile requires a name containing only simple identifier characters."),
					Profile.Get(), TEXT("name"), ProfileActual(Profile->Values.FindRef(TEXT("name"))),
					MakeShared<FJsonValueString>(TEXT("nonempty simple identifier")));
				continue;
			}
			if (Names.Contains(Name))
			{
				AddProfileIssue(Issues, TEXT("device_profile_duplicate"), TEXT("The input repeats a device profile name."),
					Profile.Get(), TEXT("name"), MakeShared<FJsonValueString>(Name),
					MakeShared<FJsonValueString>(TEXT("unique profile name"))).CandidateName = Name;
			}
			Names.Add(Name);
			ProfileSources.Add(Name, Profile);
			FString BaseProfile;
			Profile->TryGetStringField(TEXT("base_profile"), BaseProfile);
			if (!BaseProfile.IsEmpty() && (!IsSimpleIdentifier(BaseProfile) || ContainsUnsafeIniToken(BaseProfile)))
			{
				AddProfileIssue(Issues, TEXT("device_profile_base_invalid"), TEXT("The base profile name contains unsafe INI characters."),
					Profile.Get(), TEXT("base_profile"), MakeShared<FJsonValueString>(BaseProfile),
					MakeShared<FJsonValueString>(TEXT("simple identifier without INI control characters"))).CandidateName = Name;
			}
			FString DeviceType;
			if (Profile->TryGetStringField(TEXT("device_type"), DeviceType) && (!IsSimpleIdentifier(DeviceType) || ContainsUnsafeIniToken(DeviceType)))
			{
				AddProfileIssue(Issues, TEXT("device_profile_device_type_invalid"), TEXT("The device type contains unsafe INI characters."),
					Profile.Get(), TEXT("device_type"), MakeShared<FJsonValueString>(DeviceType),
					MakeShared<FJsonValueString>(TEXT("simple identifier without INI control characters"))).CandidateName = Name;
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
						AddProfileIssue(Issues, TEXT("device_profile_cvar_key_invalid"), TEXT("The CVar name contains unsafe INI characters."),
						CVars->Get(), Pair.Key, MakeShared<FJsonValueString>(Pair.Key),
						MakeShared<FJsonValueString>(TEXT("safe INI key"))).CandidateName = Pair.Key;
					}
					if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(CVarValue) || !IsSafeIniValue(CVarValue))
					{
						AddProfileIssue(Issues, TEXT("device_profile_cvar_value_invalid"),
						TEXT("The CVar value must be a shell-free single-line INI string."),
						CVars->Get(), Pair.Key, ProfileActual(Pair.Value),
						MakeShared<FJsonValueString>(TEXT("shell-free single-line INI string"))).CandidateName = Pair.Key;
					}
				}
			}
		}
		const TMap<FString, FDeviceProfileInfo> Existing = ReadDeviceProfiles();
		for (const TPair<FString, FString>& Pair : BaseByProfile)
		{
			if (!Pair.Value.IsEmpty() && !Names.Contains(Pair.Value) && !Existing.Contains(Pair.Value))
			{
				AddProfileIssue(Issues, TEXT("device_profile_base_missing"),
					TEXT("The base profile is absent from both this input and the current device profile registry."),
					ProfileSources.FindRef(Pair.Key).Get(), TEXT("base_profile"), MakeShared<FJsonValueString>(Pair.Value),
					MakeShared<FJsonValueString>(TEXT("an input or registered device profile"))).CandidateName = Pair.Key;
			}
			TSet<FString> Visited;
			TArray<FString> InheritancePath;
			FString Current = Pair.Key;
			while (!Current.IsEmpty())
			{
				if (Visited.Contains(Current))
				{
					TArray<TSharedPtr<FJsonValue>> Cycle;
					const int32 CycleStart = InheritancePath.IndexOfByKey(Current);
					for (int32 PathIndex = FMath::Max(0, CycleStart); PathIndex < InheritancePath.Num(); ++PathIndex)
						Cycle.Add(MakeShared<FJsonValueString>(InheritancePath[PathIndex]));
					Cycle.Add(MakeShared<FJsonValueString>(Current));
					const auto Source = ProfileSources.Contains(Current) ? ProfileSources.FindRef(Current) : ProfileSources.FindRef(Pair.Key);
					auto& Issue = AddProfileIssue(Issues, TEXT("device_profile_cycle"),
						TEXT("The device profile inheritance chain contains a cycle."),
						Source.Get(), TEXT("base_profile"), MakeShared<FJsonValueArray>(MoveTemp(Cycle)),
						MakeShared<FJsonValueString>(TEXT("acyclic base profile chain")));
					Issue.CandidateName = Pair.Key;
					break;
				}
				Visited.Add(Current);
				InheritancePath.Add(Current);
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

	static bool ApplyDeviceProfiles(const TSharedPtr<FJsonObject>& Root, bool bDryRun, bool bBackup, TArray<TSharedPtr<FJsonValue>>& OutChanges, TArray<TSharedPtr<FJsonValue>>& OutBackups, UE::UAI::Contract::FDiagnostic& OutError)
	{
		TArray<UE::UAI::Contract::FDiagnostic> Issues;
		if (!ValidateDeviceProfileRoot(Root, Issues))
		{
			OutChanges = UE::UAI::Contract::ToPublicDiagnostics(Issues);
			OutError = ProfileValidationFailure(TEXT("device_profile_validate_failed"),
				TEXT("Device profile input contains invalid entries."), Issues);
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

	static bool ValidateScalabilityRoot(const TSharedPtr<FJsonObject>& Root, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		if (!Root.IsValid())
		{
			AddProfileIssue(Issues, TEXT("json_root_invalid"), TEXT("Scalability input must be an object."),
				nullptr, FString(), nullptr, MakeShared<FJsonValueString>(TEXT("object")));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
		if (!Root->TryGetArrayField(TEXT("groups"), Groups) || !Groups)
		{
			AddProfileIssue(Issues, TEXT("missing_groups"), TEXT("Scalability input requires a groups array."),
				Root.Get(), TEXT("groups"), ProfileActual(Root->Values.FindRef(TEXT("groups"))),
				MakeShared<FJsonValueString>(TEXT("array")));
			return false;
		}
		for (int32 Index = 0; Index < Groups->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Group = (*Groups)[Index].IsValid() ? (*Groups)[Index]->AsObject() : nullptr;
			FString Section;
			if (!Group.IsValid() || !Group->TryGetStringField(TEXT("section"), Section) || !Section.Contains(TEXT("Quality@")) || !IsSafeIniSectionName(Section))
			{
				AddProfileIssue(Issues, TEXT("scalability_section_invalid"),
					TEXT("A scalability group requires a safe single-line section name containing Quality@."),
					Group.IsValid() ? Group.Get() : Root.Get(), Group.IsValid() ? TEXT("section") : FString(),
					ProfileActual(Group.IsValid() ? Group->Values.FindRef(TEXT("section")) : (*Groups)[Index]),
					MakeShared<FJsonValueString>(TEXT("safe single-line section containing Quality@")),
					Group.IsValid() ? FString() : TEXT("/groups/") + FString::FromInt(Index));
			}
			const TSharedPtr<FJsonObject>* Values = nullptr;
			if (Group.IsValid() && (!Group->TryGetObjectField(TEXT("values"), Values) || !Values || !Values->IsValid()))
			{
				AddProfileIssue(Issues, TEXT("scalability_values_invalid"), TEXT("Scalability group values must be an object."),
					Group.Get(), TEXT("values"), ProfileActual(Group->Values.FindRef(TEXT("values"))),
					MakeShared<FJsonValueString>(TEXT("object")));
			}
			else if (Values && Values->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
				{
					FString ValueText;
					if (!IsSafeIniKey(Pair.Key))
					{
						AddProfileIssue(Issues, TEXT("scalability_key_invalid"), TEXT("The scalability key contains unsafe INI characters."),
						Values->Get(), Pair.Key, MakeShared<FJsonValueString>(Pair.Key),
						MakeShared<FJsonValueString>(TEXT("safe INI key"))).CandidateName = Pair.Key;
					}
					if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(ValueText) || !IsSafeIniValue(ValueText))
					{
						AddProfileIssue(Issues, TEXT("scalability_value_invalid"),
						TEXT("The scalability value must be a single-line INI string without '='."),
						Values->Get(), Pair.Key, ProfileActual(Pair.Value),
						MakeShared<FJsonValueString>(TEXT("single-line INI string without '='"))).CandidateName = Pair.Key;
					}
				}
			}
		}
		return CountErrors(Issues) == 0;
	}

	static bool ApplyScalabilityRoot(const TSharedPtr<FJsonObject>& Root, bool bDryRun, bool bBackup, TArray<TSharedPtr<FJsonValue>>& OutChanges, TArray<TSharedPtr<FJsonValue>>& OutBackups, UE::UAI::Contract::FDiagnostic& OutError)
	{
		TArray<UE::UAI::Contract::FDiagnostic> Issues;
		if (!ValidateScalabilityRoot(Root, Issues))
		{
			OutChanges = UE::UAI::Contract::ToPublicDiagnostics(Issues);
			OutError = ProfileValidationFailure(TEXT("scalability_validate_failed"),
				TEXT("Scalability input contains invalid entries."), Issues);
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

	static const TArray<FString>& RootProjectConfigFileNames()
	{
		static const TArray<FString> Names{TEXT("DefaultEngine.ini"), TEXT("DefaultGame.ini"),
			TEXT("DefaultDeviceProfiles.ini"), TEXT("DefaultScalability.ini")};
		return Names;
	}

	static bool IsRootProjectConfigFileName(const FString& FileName)
	{
		return RootProjectConfigFileNames().ContainsByPredicate(
			[&FileName](const FString& Name) { return FileName.Equals(Name, ESearchCase::IgnoreCase); });
	}

	static TArray<FString> PlatformConfigFileNames(const FString& Platform)
	{
		if (Platform.IsEmpty() || !IsSimpleIdentifier(Platform)) return {};
		return {Platform + TEXT("Engine.ini"), Platform + TEXT("Game.ini"), Platform + TEXT("DeviceProfiles.ini")};
	}

	static bool IsPlatformConfigFileNameForPlatform(const FString& Platform, const FString& FileName)
	{
		return PlatformConfigFileNames(Platform).ContainsByPredicate(
			[&FileName](const FString& Name) { return FileName.Equals(Name, ESearchCase::IgnoreCase); });
	}

	static TSharedPtr<FJsonValue> PlatformConfigExpectedPaths(const FString& Platform)
	{
		TArray<TSharedPtr<FJsonValue>> Choices;
		for (const FString& Name : RootProjectConfigFileNames()) Choices.Add(MakeShared<FJsonValueString>(Name));
		for (const FString& Name : PlatformConfigFileNames(Platform))
		{
			Choices.Add(MakeShared<FJsonValueString>(Name));
			Choices.Add(MakeShared<FJsonValueString>(Platform + TEXT("/") + Name));
			Choices.Add(MakeShared<FJsonValueString>(TEXT("Config/") + Platform + TEXT("/") + Name));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Choices));
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

	static bool ValidatePlatformConfigRoot(const TSharedPtr<FJsonObject>& Root, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		if (!Root.IsValid())
		{
			AddProfileIssue(Issues, TEXT("json_root_invalid"), TEXT("Platform configuration input must be an object."),
				nullptr, FString(), nullptr, MakeShared<FJsonValueString>(TEXT("object")));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		if (!Root->TryGetArrayField(TEXT("files"), Files) || !Files)
		{
			AddProfileIssue(Issues, TEXT("missing_files"), TEXT("Platform configuration input requires a files array."),
				Root.Get(), TEXT("files"), ProfileActual(Root->Values.FindRef(TEXT("files"))),
				MakeShared<FJsonValueString>(TEXT("array")));
			return false;
		}
		FString Platform = TEXT("Windows");
		Root->TryGetStringField(TEXT("platform"), Platform);
		for (int32 FileIndex = 0; FileIndex < Files->Num(); ++FileIndex)
		{
			const TSharedPtr<FJsonObject> FileObj = (*Files)[FileIndex].IsValid() ? (*Files)[FileIndex]->AsObject() : nullptr;
			FString FileName;
			if (!FileObj.IsValid() || !FileObj->TryGetStringField(TEXT("file"), FileName) || !IsPlatformConfigFileAllowed(Platform, FileName))
			{
				AddProfileIssue(Issues, TEXT("platform_config_file_not_allowed"),
					TEXT("The file is not allowed for the selected platform configuration."),
					FileObj.IsValid() ? FileObj.Get() : Root.Get(), FileObj.IsValid() ? TEXT("file") : FString(),
					ProfileActual(FileObj.IsValid() ? FileObj->Values.FindRef(TEXT("file")) : (*Files)[FileIndex]),
					PlatformConfigExpectedPaths(Platform),
					FileObj.IsValid() ? FString() : TEXT("/files/") + FString::FromInt(FileIndex)).CandidateName = Platform;
			}
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			if (!FileObj.IsValid() || !FileObj->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
			{
				AddProfileIssue(Issues, TEXT("platform_config_sections_missing"), TEXT("Each platform configuration file requires a sections array."),
					FileObj.IsValid() ? FileObj.Get() : Root.Get(), FileObj.IsValid() ? TEXT("sections") : FString(),
					ProfileActual(FileObj.IsValid() ? FileObj->Values.FindRef(TEXT("sections")) : (*Files)[FileIndex]),
					MakeShared<FJsonValueString>(TEXT("array")),
					FileObj.IsValid() ? FString() : TEXT("/files/") + FString::FromInt(FileIndex));
				continue;
			}
			for (int32 SectionIndex = 0; SectionIndex < Sections->Num(); ++SectionIndex)
			{
				const TSharedPtr<FJsonObject> Section = (*Sections)[SectionIndex].IsValid() ? (*Sections)[SectionIndex]->AsObject() : nullptr;
				FString SectionName;
				if (!Section.IsValid() || !Section->TryGetStringField(TEXT("name"), SectionName) || !IsSafeIniSectionName(SectionName))
				{
					AddProfileIssue(Issues, TEXT("platform_config_section_name_invalid"), TEXT("The configuration section requires a safe INI name."),
						Section.IsValid() ? Section.Get() : FileObj.Get(), Section.IsValid() ? TEXT("name") : FString(),
						ProfileActual(Section.IsValid() ? Section->Values.FindRef(TEXT("name")) : (*Sections)[SectionIndex]),
						MakeShared<FJsonValueString>(TEXT("safe INI section name")),
						Section.IsValid() ? FString() : TEXT("/sections/") + FString::FromInt(SectionIndex));
					continue;
				}
				const TSharedPtr<FJsonObject>* Values = nullptr;
				if (!Section->TryGetObjectField(TEXT("values"), Values) || !Values || !Values->IsValid())
				{
					AddProfileIssue(Issues, TEXT("platform_config_values_invalid"), TEXT("Configuration section values must be an object."),
						Section.Get(), TEXT("values"), ProfileActual(Section->Values.FindRef(TEXT("values"))),
						MakeShared<FJsonValueString>(TEXT("object")));
					continue;
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
				{
					FString ValueText;
					if (!IsSafeIniKey(Pair.Key))
					{
						AddProfileIssue(Issues, TEXT("platform_config_key_invalid"), TEXT("The configuration key contains unsafe INI characters."),
							Values->Get(), Pair.Key, MakeShared<FJsonValueString>(Pair.Key),
							MakeShared<FJsonValueString>(TEXT("safe INI key"))).CandidateName = Pair.Key;
					}
					if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(ValueText) || !IsSafeIniValue(ValueText))
					{
						AddProfileIssue(Issues, TEXT("platform_config_value_invalid"),
							TEXT("The configuration value must be a single-line INI string without '='."),
							Values->Get(), Pair.Key, ProfileActual(Pair.Value),
							MakeShared<FJsonValueString>(TEXT("single-line INI string without '='"))).CandidateName = Pair.Key;
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

	static bool ApplyPlatformConfigRoot(const TSharedPtr<FJsonObject>& Root, bool bDryRun, bool bBackup, TArray<TSharedPtr<FJsonValue>>& OutChanges, TArray<TSharedPtr<FJsonValue>>& OutBackups, UE::UAI::Contract::FDiagnostic& OutError)
	{
		TArray<UE::UAI::Contract::FDiagnostic> Issues;
		if (!ValidatePlatformConfigRoot(Root, Issues))
		{
			OutChanges = UE::UAI::Contract::ToPublicDiagnostics(Issues);
			OutError = ProfileValidationFailure(TEXT("platform_config_validate_failed"),
				TEXT("Platform configuration input contains invalid entries."), Issues);
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












































bool UE::UAI::DomainAdapters::EditorProject::FEditorProjectQueryMechanics::TargetPlatformQuery(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentProductionPipelineOps;
	bool bIncludeSdk = true;
	bool bIncludeTextureFormats = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_sdk"), bIncludeSdk);
		Params->TryGetBoolField(TEXT("include_texture_formats"), bIncludeTextureFormats);
	}
	OutData = BuildPackagingEnvironmentJson(bIncludeSdk, false);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.target_platform.query.v1"));
	OutData->SetBoolField(TEXT("include_texture_formats"), bIncludeTextureFormats);
	OutData->SetArrayField(TEXT("sdk_status"), OutData->GetArrayField(TEXT("target_platforms")));
	return true;
}



bool UE::UAI::DomainAdapters::EditorProject::FEditorProjectQueryMechanics::PlatformConfigExport(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentProductionPipelineOps;
	FString Platform = TEXT("Windows");
	FString OutputFile;
	bool bIncludeSources = true;
	TArray<FString> Sections;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("platform"), Platform);
		Params->TryGetStringField(TEXT("output_file"), OutputFile);
		Params->TryGetBoolField(TEXT("include_sources"), bIncludeSources);
	}
	TryGetStringArray(Params, TEXT("sections"), Sections);
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



bool UE::UAI::DomainAdapters::EditorProject::FEditorProjectQueryMechanics::PlatformConfigValidate(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	FUeAgentStructuredHjsonDocument SourceDocument;
	auto ParamsSources = BindProfileSources(Params.Get());
	if (!LoadJsonFromParams(Params, Root, JsonFile, SourceDocument, OutError))
	{
		return false;
	}
	auto FileSources = BindProfileSources(SourceDocument.Root.Get(), &SourceDocument);
	TArray<UE::UAI::Contract::FDiagnostic> Issues;
	const bool bValid = ValidatePlatformConfigRoot(Root, Issues);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.platform_config.validate_result.v1"));
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetArrayField(TEXT("issues"), UE::UAI::Contract::ToPublicDiagnostics(Issues));
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	if (!bValid)
	{
		OutError = ProfileValidationFailure(TEXT("platform_config_validate_failed"),
			TEXT("The platform configuration input failed validation."), Issues);
		return false;
	}
	return true;
}





bool UE::UAI::DomainAdapters::EditorProject::FEditorProjectQueryMechanics::DeviceProfileQuery(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentProductionPipelineOps;
	FString Profile;
	bool bIncludeInherited = true;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("profile"), Profile);
		Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);
	}
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



bool UE::UAI::DomainAdapters::EditorProject::FEditorProjectQueryMechanics::DeviceProfileExport(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentProductionPipelineOps;
	FString Profile;
	FString OutputFile;
	bool bIncludeInherited = true;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("profile"), Profile);
		Params->TryGetStringField(TEXT("output_file"), OutputFile);
		Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);
	}
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



bool UE::UAI::DomainAdapters::EditorProject::FEditorProjectQueryMechanics::DeviceProfileValidate(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	FUeAgentStructuredHjsonDocument SourceDocument;
	auto ParamsSources = BindProfileSources(Params.Get());
	if (!LoadJsonFromParams(Params, Root, JsonFile, SourceDocument, OutError))
	{
		return false;
	}
	auto FileSources = BindProfileSources(SourceDocument.Root.Get(), &SourceDocument);
	TArray<UE::UAI::Contract::FDiagnostic> Issues;
	const bool bValid = ValidateDeviceProfileRoot(Root, Issues);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.device_profile.validate_result.v1"));
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetArrayField(TEXT("issues"), UE::UAI::Contract::ToPublicDiagnostics(Issues));
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	if (!bValid)
	{
		OutError = ProfileValidationFailure(TEXT("device_profile_validate_failed"),
			TEXT("The device profile input failed validation."), Issues);
		return false;
	}
	return true;
}



bool UE::UAI::DomainAdapters::EditorProject::FEditorProjectQueryMechanics::DeviceProfileDiff(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	FUeAgentStructuredHjsonDocument SourceDocument;
	auto ParamsSources = BindProfileSources(Params.Get());
	if (!LoadJsonFromParams(Params, Root, JsonFile, SourceDocument, OutError))
	{
		return false;
	}
	auto FileSources = BindProfileSources(SourceDocument.Root.Get(), &SourceDocument);
	TArray<UE::UAI::Contract::FDiagnostic> Issues;
	if (!ValidateDeviceProfileRoot(Root, Issues))
	{
		OutData->SetArrayField(TEXT("issues"), UE::UAI::Contract::ToPublicDiagnostics(Issues));
		OutError = ProfileValidationFailure(TEXT("device_profile_validate_failed"),
			TEXT("The device profile input failed validation."), Issues);
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





bool UE::UAI::DomainAdapters::EditorProject::FEditorProjectQueryMechanics::DeviceProfileResolveCVars(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentProductionPipelineOps;
	FString Profile;
	TArray<FString> Keys;
	bool bIncludeSources = true;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("profile"), Profile);
		Params->TryGetBoolField(TEXT("include_sources"), bIncludeSources);
	}
	TryGetStringArray(Params, TEXT("keys"), Keys);
	if (Profile.IsEmpty() || !ReadDeviceProfiles().Contains(Profile))
	{
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("device_profile_not_found"),
			Profile.IsEmpty() ? TEXT("Resolving CVars requires a nonempty device profile name.")
				: TEXT("The requested device profile is not registered."), TEXT("query"));
		OutError.SourceKind = TEXT("command_input");
		OutError.Source.JsonPath = TEXT("/profile");
		OutError.CandidateName = Profile;
		OutError.ActualValue = ProfileActual(Params.IsValid() ? Params->Values.FindRef(TEXT("profile")) : nullptr);
		OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty registered device profile name"));
		OutError.ActualValuePresent = OutError.ActualValue.IsValid();
		OutError.ExpectedValuePresent = true;
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



bool UE::UAI::DomainAdapters::EditorProject::FEditorProjectQueryMechanics::ScalabilityExport(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentProductionPipelineOps;
	FString Platform;
	FString OutputFile;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("platform"), Platform);
		Params->TryGetStringField(TEXT("output_file"), OutputFile);
	}
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



bool UE::UAI::DomainAdapters::EditorProject::FEditorProjectQueryMechanics::ScalabilityValidate(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	FUeAgentStructuredHjsonDocument SourceDocument;
	auto ParamsSources = BindProfileSources(Params.Get());
	if (!LoadJsonFromParams(Params, Root, JsonFile, SourceDocument, OutError))
	{
		return false;
	}
	auto FileSources = BindProfileSources(SourceDocument.Root.Get(), &SourceDocument);
	TArray<UE::UAI::Contract::FDiagnostic> Issues;
	const bool bValid = ValidateScalabilityRoot(Root, Issues);
	OutData->SetStringField(TEXT("schema"), TEXT("uai.scalability.validate_result.v1"));
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetArrayField(TEXT("issues"), UE::UAI::Contract::ToPublicDiagnostics(Issues));
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	if (!bValid)
	{
		OutError = ProfileValidationFailure(TEXT("scalability_validate_failed"),
			TEXT("The scalability input failed validation."), Issues);
		return false;
	}
	return true;
}





bool UE::UAI::DomainAdapters::EditorProject::FEditorProjectQueryMechanics::PlatformProfileValidatePackaging(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UeAgentProductionPipelineOps;
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	FUeAgentStructuredHjsonDocument SourceDocument;
	auto ParamsSources = BindProfileSources(Params.Get());
	FString Platform = TEXT("Win64");
	FString DeviceProfile;
	const TSharedPtr<FJsonObject>* PackagingJson = nullptr;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("platform"), Platform);
		Params->TryGetStringField(TEXT("device_profile"), DeviceProfile);
		Params->TryGetObjectField(TEXT("packaging_json"), PackagingJson);
	}
	if (PackagingJson && PackagingJson->IsValid())
	{
		Root = *PackagingJson;
	}
	else
	{
		if (!LoadJsonFromParams(Params, Root, JsonFile, SourceDocument, OutError, false)) return false;
	}
	auto FileSources = BindProfileSources(SourceDocument.Root.Get(), &SourceDocument);
	FPackagingProfile Profile;
	FPackagingInputSources InputSources;
	ParsePackagingProfile(Root, Params, Profile, InputSources);
	if (!Platform.IsEmpty())
	{
		Profile.Platform = Platform;
		InputSources.Platform = FPackagingInputField();
		const auto Actual = Params.IsValid() ? Params->Values.FindRef(TEXT("platform")) : nullptr;
		if (Actual.IsValid() && Actual->Type == EJson::String)
			RecordPackagingField(*Params, TEXT("platform"), InputSources.Platform);
	}
	TArray<UE::UAI::Contract::FDiagnostic> Issues;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	ValidatePackagingProfile(Profile, InputSources, Issues);
	if (!DeviceProfile.IsEmpty() && !ReadDeviceProfiles().Contains(DeviceProfile))
	{
		AddProfileIssue(Issues, TEXT("device_profile_not_found"),
			TEXT("The selected packaging device profile is not registered."), Params.Get(), TEXT("device_profile"),
			MakeShared<FJsonValueString>(DeviceProfile), MakeShared<FJsonValueString>(TEXT("registered device profile")));
	}
	const bool bValid = CountErrors(Issues) == 0;
	OutData->SetStringField(TEXT("schema"), TEXT("uai.platform_profile.validate_packaging.v1"));
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetArrayField(TEXT("issues"), UE::UAI::Contract::ToPublicDiagnostics(Issues));
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	if (!bValid)
	{
		OutError = ProfileValidationFailure(TEXT("platform_profile_validate_failed"),
			TEXT("The packaging profile failed validation."), Issues);
		return false;
	}
	return true;
}
