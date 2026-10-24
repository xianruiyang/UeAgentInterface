#include "DomainAdapters/ContentGraphRead/UeAgentExternalLocalizationContentGraphProvider.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "LocalizationSettings.h"
#include "LocalizationTargetTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::DomainAdapters::ContentGraphRead
{
	namespace
	{
		const TSet<FString> Capabilities = {
			TEXT("localization.diff"),
			TEXT("localization.target.read"),
			TEXT("localization.culture.read"),
			TEXT("localization.query_targets"),
			TEXT("localization.report"),
			TEXT("localization.validate"),
			TEXT("string_table.diff"),
			TEXT("string_table.validate"),
		};

		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
            return false;
        }


		FString LocalizationContentRoot()
		{
			return FPaths::ConvertRelativePathToFull(
				FPaths::ProjectContentDir() / TEXT("Localization"));
		}

		FString LocalizationConfigRoot()
		{
			return FPaths::ConvertRelativePathToFull(
				FPaths::ProjectConfigDir() / TEXT("Localization"));
		}

		FString ResolveProjectFile(const FString& Value)
		{
			return FPaths::ConvertRelativePathToFull(
				FPaths::IsRelative(Value) ? FPaths::ProjectDir() / Value : Value);
		}

		TArray<FString> ReadStringArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field)
		{
			TArray<FString> Result;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Object.IsValid() && Object->TryGetArrayField(Field, Values) && Values)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Values)
				{
					if (Value.IsValid() && Value->Type == EJson::String)
					{
						Result.Add(Value->AsString());
					}
				}
			}
			return Result;
		}

		struct FExternalDocumentOrigin
		{
			FString File;
			FString Parameter;
			bool bObjectParameter = false;
		};

		TSharedPtr<FJsonValue> ExternalInputShape(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value) return MakeShared<FJsonValueString>(TEXT("<missing>"));
			if (Value->Type != EJson::Array && Value->Type != EJson::Object) return Value;
			return MakeShared<FJsonValueString>(Value->Type == EJson::Array ? TEXT("array") : TEXT("object"));
		}

		void LocateExternalIssue(UE::UAI::Contract::FDiagnostic& Issue, const FExternalDocumentOrigin& Origin,
			const FString& Pointer)
		{
			if (!Origin.File.IsEmpty())
			{
				Issue.SourceKind = TEXT("authored_value");
				Issue.Source.SourceFile = Origin.File;
				Issue.Source.JsonPath = Pointer.IsEmpty() ? TEXT("/") : Pointer;
			}
			else
			{
				Issue.SourceKind = TEXT("command_input");
				Issue.Source.JsonPath = Origin.Parameter + (Origin.bObjectParameter ? Pointer : FString());
			}
		}

		void AddIssue(TArray<UE::UAI::Contract::FDiagnostic>& Issues, const FExternalDocumentOrigin& Origin,
			const FString& Code, const FString& ReportPath, const FString& Pointer, const FString& Message,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Issue = UE::UAI::Contract::FDiagnostic::Failure(Code, Message, TEXT("execution"));
			Issue.PropertyPath = ReportPath;
			Issue.ActualValue = MoveTemp(Actual);
			Issue.ExpectedValue = MoveTemp(Expected);
			LocateExternalIssue(Issue, Origin, Pointer);
			Issues.Add(MoveTemp(Issue));
		}

		TArray<TSharedPtr<FJsonValue>> ExternalIssueReport(const TArray<UE::UAI::Contract::FDiagnostic>& Issues)
		{
			TArray<TSharedPtr<FJsonValue>> Report;
			for (const auto& Issue : Issues)
			{
				auto Row = Issue.ToPublicJson();
				Row->SetStringField(TEXT("path"), Issue.PropertyPath);
				Report.Add(MakeShared<FJsonValueObject>(Row));
			}
			return Report;
		}

		UE::UAI::Contract::FDiagnostic ExternalValidationFailure(const FString& Code,
			TArray<UE::UAI::Contract::FDiagnostic> Issues)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code,
				TEXT("The supplied document failed validation; each invalid field is listed in diagnostics."), TEXT("execution"));
			Error.SourceKind = TEXT("native_result");
			Error.ActualValue = MakeShared<FJsonValueNumber>(Issues.Num());
			Error.SourceKind = TEXT("native_result");
			Error.ExpectedValue = MakeShared<FJsonValueNumber>(0);
			Error.Diagnostics = MoveTemp(Issues);
			return Error;
		}


		bool LoadDocument(const TSharedPtr<FJsonObject>& Parameters, TSharedPtr<FJsonObject>& OutDocument,
			UE::UAI::Contract::FDiagnostic& OutError, FExternalDocumentOrigin& Origin)
		{
			OutDocument.Reset();
			Origin = {};
			if (!Parameters.IsValid())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("localization_parameters_required"),
					TEXT("Localization input parameters must be an object."), TEXT("execution"));
				OutError.SourceKind = TEXT("command_input");
				OutError.Source.JsonPath = TEXT("/");
				OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("<missing>"));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
				return false;
			}
			const TSharedPtr<FJsonObject>* Object = nullptr;
			const bool bDocument = Parameters->TryGetObjectField(TEXT("document"), Object);
			if ((bDocument || Parameters->TryGetObjectField(TEXT("json"), Object)) && Object && Object->IsValid())
			{
				OutDocument = *Object;
				Origin.Parameter = bDocument ? TEXT("/document") : TEXT("/json");
				Origin.bObjectParameter = true;
				OutError.Reset();
				return true;
			}
			FString Text;
			Parameters->TryGetStringField(TEXT("json_text"), Text);
			Origin.Parameter = TEXT("/json_text");
			if (Text.IsEmpty())
			{
				FString JsonFile;
				if (!Parameters->TryGetStringField(TEXT("json_file"), JsonFile) || JsonFile.TrimStartAndEnd().IsEmpty())
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("localization_document_required"),
						TEXT("Supply a document/json object, non-empty json_text, or json_file path."), TEXT("execution"));
					OutError.SourceKind = TEXT("command_input");
					OutError.Source.JsonPath = TEXT("/");
					auto Actual = MakeShared<FJsonObject>();
					for (const TCHAR* Carrier : {TEXT("document"), TEXT("json"), TEXT("json_text"), TEXT("json_file")})
						Actual->SetField(Carrier, ExternalInputShape(Parameters->TryGetField(Carrier)));
					OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("document object, JSON text or JSON file"));
					return false;
				}
				Origin.File = ResolveProjectFile(JsonFile);
				Origin.Parameter = TEXT("/json_file");
				if (!FFileHelper::LoadFileToString(Text, *Origin.File))
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("localization_document_read_failed"),
						TEXT("The supplied JSON document file could not be read."), TEXT("execution"));
					LocateExternalIssue(OutError, Origin, TEXT("/"));
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return false;
				}
			}
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(Reader, OutDocument) || !OutDocument.IsValid())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("localization_document_invalid"),
					TEXT("The document must be valid object JSON. ") + Reader->GetErrorMessage(), TEXT("execution"));
				LocateExternalIssue(OutError, Origin, TEXT("/"));
				if (!Origin.File.IsEmpty())
				{
					OutError.Source.Line = Reader->GetLineNumber();
					OutError.Source.Column = Reader->GetCharacterNumber();
				}
				else OutError.PublicMessage += FString::Printf(TEXT(" (JSON text line %d, column %d)"), Reader->GetLineNumber(), Reader->GetCharacterNumber());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object JSON"));
				return false;
			}
			OutError.Reset();
			return true;
		}

		ULocalizationTarget* FindTarget(const FString& Name)
		{
			ULocalizationTargetSet* Set = ULocalizationSettings::GetGameTargetSet();
			if (!Set)
			{
				return nullptr;
			}
			const TObjectPtr<ULocalizationTarget>* Found = Set->TargetObjects.FindByPredicate(
				[&Name](const ULocalizationTarget* Candidate)
				{
					return Candidate && Candidate->Settings.Name.Equals(Name, ESearchCase::CaseSensitive);
				});
			return Found ? Found->Get() : nullptr;
		}

		TArray<FString> DiscoverTargetNames()
		{
			TSet<FString> Names;
			if (ULocalizationTargetSet* Set = ULocalizationSettings::GetGameTargetSet())
			{
				for (const ULocalizationTarget* Target : Set->TargetObjects)
				{
					if (Target && !Target->Settings.Name.IsEmpty())
					{
						Names.Add(Target->Settings.Name);
					}
				}
			}
			TArray<FString> Configs;
			IFileManager::Get().FindFiles(
				Configs,
				*(LocalizationConfigRoot() / TEXT("*.ini")),
				true,
				false);
			for (const FString& File : Configs)
			{
				Names.Add(FPaths::GetBaseFilename(File));
			}
			TArray<FString> Result = Names.Array();
			Result.Sort();
			return Result;
		}

		TSharedPtr<FJsonObject> TargetJson(
			const FString& Name,
			const bool bIncludeFiles,
			const bool bIncludeStats)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("name"), Name);
			const FString ContentDir = LocalizationContentRoot() / Name;
			const FString ConfigFile = LocalizationConfigRoot() / (Name + TEXT(".ini"));
			Result->SetStringField(TEXT("content_dir"), ContentDir);
			Result->SetStringField(TEXT("config_file"), ConfigFile);
			if (ULocalizationTarget* Target = FindTarget(Name))
			{
				TArray<TSharedPtr<FJsonValue>> Cultures;
				for (const FCultureStatistics& Culture : Target->Settings.SupportedCulturesStatistics)
				{
					Cultures.Add(MakeShared<FJsonValueString>(Culture.CultureName));
				}
				Result->SetArrayField(TEXT("cultures"), Cultures);
				if (Target->Settings.SupportedCulturesStatistics.IsValidIndex(
						Target->Settings.NativeCultureIndex))
				{
					Result->SetStringField(
						TEXT("native_culture"),
						Target->Settings.SupportedCulturesStatistics[
							Target->Settings.NativeCultureIndex].CultureName);
				}
			}
			if (bIncludeFiles || bIncludeStats)
			{
				TArray<FString> Files;
				IFileManager::Get().FindFilesRecursive(Files, *ContentDir, TEXT("*.*"), true, false);
				Files.Sort();
				if (bIncludeFiles)
				{
					TArray<TSharedPtr<FJsonValue>> Values;
					for (const FString& File : Files)
					{
						Values.Add(MakeShared<FJsonValueString>(File));
					}
					Result->SetArrayField(TEXT("files"), Values);
				}
				if (bIncludeStats)
				{
					Result->SetNumberField(TEXT("file_count"), Files.Num());
					int64 Bytes = 0;
					for (const FString& File : Files)
					{
						Bytes += FMath::Max<int64>(0, IFileManager::Get().FileSize(*File));
					}
					Result->SetNumberField(TEXT("byte_count"), static_cast<double>(Bytes));
				}
			}
			return Result;
		}

		TSharedPtr<FJsonObject> LocalizationSnapshot(
			const bool bIncludeFiles,
			const bool bIncludeStats,
			const FString& Filter = FString())
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("uai.localization.v1"));
			Result->SetStringField(TEXT("content_root"), LocalizationContentRoot());
			Result->SetStringField(TEXT("config_root"), LocalizationConfigRoot());
			TArray<TSharedPtr<FJsonValue>> Targets;
			for (const FString& Name : DiscoverTargetNames())
			{
				if (Filter.IsEmpty() || Name == Filter)
				{
					Targets.Add(MakeShared<FJsonValueObject>(
						TargetJson(Name, bIncludeFiles, bIncludeStats)));
				}
			}
			Result->SetNumberField(TEXT("target_count"), Targets.Num());
			Result->SetArrayField(TEXT("targets"), Targets);
			return Result;
		}

		UStringTable* LoadStringTable(const FString& Target)
		{
			FString ObjectPath = Target;
			if (FPackageName::IsValidLongPackageName(Target))
			{
				ObjectPath = Target + TEXT(".") + FPackageName::GetLongPackageAssetName(Target);
			}
			return LoadObject<UStringTable>(nullptr, *ObjectPath);
		}

		TMap<FString, FString> ReadStringTableEntries(UStringTable& Table)
		{
			TMap<FString, FString> Result;
			Table.GetStringTable()->EnumerateSourceStrings(
				[&Result](const FString& Key, const FString& Source)
				{
					Result.Add(Key, Source);
					return true;
				});
			return Result;
		}

		bool ValidateLocalization(
			const TSharedPtr<FJsonObject>& Root,
			TArray<UE::UAI::Contract::FDiagnostic>& Issues, const FExternalDocumentOrigin& Origin)
		{
			if (!Root.IsValid())
			{
				AddIssue(Issues, Origin, TEXT("json_root_invalid"), TEXT("$"), TEXT("/"),
					TEXT("Localization root must be an object."), MakeShared<FJsonValueString>(TEXT("<missing>")), MakeShared<FJsonValueString>(TEXT("object")));
				return false;
			}
			FString Schema;
			if (!Root->TryGetStringField(TEXT("schema"), Schema)
				|| Schema != TEXT("uai.localization.v1"))
			{
				AddIssue(Issues, Origin, TEXT("schema_invalid"), TEXT("$.schema"), TEXT("/schema"),
					TEXT("Expected uai.localization.v1."), ExternalInputShape(Root->TryGetField(TEXT("schema"))), MakeShared<FJsonValueString>(TEXT("uai.localization.v1")));
			}
			const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
			if (!Root->TryGetArrayField(TEXT("targets"), Targets) || !Targets)
			{
				AddIssue(Issues, Origin, TEXT("targets_required"), TEXT("$.targets"), TEXT("/targets"),
					TEXT("Targets must be an array."), ExternalInputShape(Root->TryGetField(TEXT("targets"))), MakeShared<FJsonValueString>(TEXT("array")));
			}
			else
			{
				TSet<FString> Names;
				for (int32 Index = 0; Index < Targets->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> Target = (*Targets)[Index].IsValid()
						? (*Targets)[Index]->AsObject() : nullptr;
					FString Name;
					if (!Target.IsValid() || !Target->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
					{
						AddIssue(Issues, Origin, TEXT("target_name_required"), FString::Printf(TEXT("$.targets[%d].name"), Index),
							TEXT("/targets/") + FString::FromInt(Index) + (Target ? TEXT("/name") : TEXT("")),
							Target ? TEXT("Target name must be a non-empty string.") : TEXT("Localization target must be an object."),
							ExternalInputShape(Target ? Target->TryGetField(TEXT("name")) : (*Targets)[Index]),
							MakeShared<FJsonValueString>(Target ? TEXT("non-empty name string") : TEXT("object")));
					}
					else if (Names.Contains(Name))
					{
						AddIssue(Issues, Origin, TEXT("target_name_duplicate"), FString::Printf(TEXT("$.targets[%d].name"), Index), TEXT("/targets/") + FString::FromInt(Index) + TEXT("/name"),
							TEXT("Target names must be unique."), MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("unique target name")));
					}
					else
					{
						Names.Add(Name);
					}
				}
			}
			return Issues.IsEmpty();
		}

		bool ValidateStringTable(
			const TSharedPtr<FJsonObject>& Root,
			TArray<UE::UAI::Contract::FDiagnostic>& Issues, const FExternalDocumentOrigin& Origin)
		{
			if (!Root.IsValid())
			{
				AddIssue(Issues, Origin, TEXT("json_root_invalid"), TEXT("$"), TEXT("/"),
					TEXT("StringTable root must be an object."), MakeShared<FJsonValueString>(TEXT("<missing>")), MakeShared<FJsonValueString>(TEXT("object")));
				return false;
			}
			FString Schema;
			if (!Root->TryGetStringField(TEXT("schema"), Schema)
				|| Schema != TEXT("uai.string_table.v1"))
			{
				AddIssue(Issues, Origin, TEXT("schema_invalid"), TEXT("$.schema"), TEXT("/schema"),
					TEXT("Expected uai.string_table.v1."), ExternalInputShape(Root->TryGetField(TEXT("schema"))), MakeShared<FJsonValueString>(TEXT("uai.string_table.v1")));
			}
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (!Root->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
			{
				AddIssue(Issues, Origin, TEXT("entries_required"), TEXT("$.entries"), TEXT("/entries"),
					TEXT("Entries must be an array."), ExternalInputShape(Root->TryGetField(TEXT("entries"))), MakeShared<FJsonValueString>(TEXT("array")));
			}
			else
			{
				TSet<FString> Keys;
				for (int32 Index = 0; Index < Entries->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> Entry = (*Entries)[Index].IsValid()
						? (*Entries)[Index]->AsObject() : nullptr;
					FString Key;
					FString Source;
					if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty()
						|| !Entry->TryGetStringField(TEXT("source"), Source))
					{
						AddIssue(Issues, Origin, TEXT("entry_invalid"), FString::Printf(TEXT("$.entries[%d]"), Index),
							TEXT("/entries/") + FString::FromInt(Index) + (!Entry ? TEXT("") : Key.IsEmpty() ? TEXT("/key") : TEXT("/source")),
							!Entry ? TEXT("StringTable entry must be an object.") : Key.IsEmpty()
								? TEXT("StringTable entry key must be a non-empty string.") : TEXT("StringTable entry source must be a string."),
							ExternalInputShape(Entry ? Entry->TryGetField(Key.IsEmpty() ? TEXT("key") : TEXT("source")) : (*Entries)[Index]),
							MakeShared<FJsonValueString>(!Entry ? TEXT("object") : Key.IsEmpty() ? TEXT("non-empty key string") : TEXT("source string")));
					}
					else if (Keys.Contains(Key))
					{
						AddIssue(Issues, Origin, TEXT("entry_key_duplicate"), FString::Printf(TEXT("$.entries[%d].key"), Index), TEXT("/entries/") + FString::FromInt(Index) + TEXT("/key"),
							TEXT("Entry keys must be unique."), MakeShared<FJsonValueString>(Key), MakeShared<FJsonValueString>(TEXT("unique entry key")));
					}
					else
					{
						Keys.Add(Key);
					}
				}
			}
			return Issues.IsEmpty();
		}

		int32 CountPoEntries(const FString& Path, int32& OutTranslated)
		{
			OutTranslated = 0;
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				return 0;
			}
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, false);
			int32 Entries = 0;
			for (const FString& Line : Lines)
			{
				if (Line.StartsWith(TEXT("msgid \"")) && Line != TEXT("msgid \"\""))
				{
					++Entries;
				}
				if (Line.StartsWith(TEXT("msgstr \"")) && Line != TEXT("msgstr \"\""))
				{
					++OutTranslated;
				}
			}
			return Entries;
		}

		bool ExecuteLocalizationValidate(
			const TSharedPtr<FJsonObject>& Parameters,
			TSharedPtr<FJsonObject>& Out,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<FJsonObject> Root;
			FExternalDocumentOrigin Origin;
			if (!LoadDocument(Parameters, Root, OutError, Origin)) return false;
			TArray<UE::UAI::Contract::FDiagnostic> Issues;
			const bool bValid = ValidateLocalization(Root, Issues, Origin);
			Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("valid"), bValid);
			Out->SetNumberField(TEXT("error_count"), Issues.Num());
			Out->SetArrayField(TEXT("issues"), ExternalIssueReport(Issues));
			return true;
		}

		bool ExecuteLocalizationDiff(
			const TSharedPtr<FJsonObject>& Parameters,
			TSharedPtr<FJsonObject>& Out,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<FJsonObject> Desired;
			FExternalDocumentOrigin Origin;
			if (!LoadDocument(Parameters, Desired, OutError, Origin)) return false;
			TArray<UE::UAI::Contract::FDiagnostic> Issues;
			if (!ValidateLocalization(Desired, Issues, Origin))
			{
				return Fail(OutError, ExternalValidationFailure(TEXT("localization_document_validation_failed"), MoveTemp(Issues)));
			}
			TMap<FString, TSharedPtr<FJsonObject>> Existing;
			const TSharedPtr<FJsonObject> Current = LocalizationSnapshot(false, false);
			const TArray<TSharedPtr<FJsonValue>>* ExistingTargets = nullptr;
			Current->TryGetArrayField(TEXT("targets"), ExistingTargets);
			if (ExistingTargets)
			{
				for (const TSharedPtr<FJsonValue>& Value : *ExistingTargets)
				{
					const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
					FString Name;
					if (Item.IsValid() && Item->TryGetStringField(TEXT("name"), Name)) Existing.Add(Name, Item);
				}
			}
			TArray<TSharedPtr<FJsonValue>> Diffs;
			TSet<FString> DesiredNames;
			const TArray<TSharedPtr<FJsonValue>>* DesiredTargets = nullptr;
			Desired->TryGetArrayField(TEXT("targets"), DesiredTargets);
			if (DesiredTargets)
			{
				for (const TSharedPtr<FJsonValue>& Value : *DesiredTargets)
				{
					const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
					FString Name;
					if (!Item.IsValid() || !Item->TryGetStringField(TEXT("name"), Name)) continue;
					DesiredNames.Add(Name);
					if (!Existing.Contains(Name))
					{
						const TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
						Diff->SetStringField(TEXT("type"), TEXT("target_added"));
						Diff->SetStringField(TEXT("target"), Name);
						Diffs.Add(MakeShared<FJsonValueObject>(Diff));
					}
				}
			}
			for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : Existing)
			{
				if (!DesiredNames.Contains(Pair.Key))
				{
					const TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
					Diff->SetStringField(TEXT("type"), TEXT("target_removed"));
					Diff->SetStringField(TEXT("target"), Pair.Key);
					Diffs.Add(MakeShared<FJsonValueObject>(Diff));
				}
			}
			Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("changed"), !Diffs.IsEmpty());
			Out->SetNumberField(TEXT("diff_count"), Diffs.Num());
			Out->SetArrayField(TEXT("diffs"), Diffs);
			return true;
		}

		bool ExecuteStringTableValidate(
			const TSharedPtr<FJsonObject>& Parameters,
			TSharedPtr<FJsonObject>& Out,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<FJsonObject> Root;
			FExternalDocumentOrigin Origin;
			if (!LoadDocument(Parameters, Root, OutError, Origin)) return false;
			TArray<UE::UAI::Contract::FDiagnostic> Issues;
			const bool bValid = ValidateStringTable(Root, Issues, Origin);
			Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("valid"), bValid);
			Out->SetNumberField(TEXT("error_count"), Issues.Num());
			Out->SetArrayField(TEXT("issues"), ExternalIssueReport(Issues));
			return true;
		}

		bool ExecuteStringTableDiff(
			const TSharedPtr<FJsonObject>& Parameters,
			TSharedPtr<FJsonObject>& Out,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<FJsonObject> Desired;
			FExternalDocumentOrigin Origin;
			if (!LoadDocument(Parameters, Desired, OutError, Origin)) return false;
			TArray<UE::UAI::Contract::FDiagnostic> Issues;
			if (!ValidateStringTable(Desired, Issues, Origin))
			{
				return Fail(OutError, ExternalValidationFailure(TEXT("string_table_document_validation_failed"), MoveTemp(Issues)));
			}
			FString Target;
			if (!Parameters->TryGetStringField(TEXT("target"), Target)
				&& !Parameters->TryGetStringField(TEXT("asset_path"), Target))
			{
				Desired->TryGetStringField(TEXT("asset_path"), Target);
			}
			UStringTable* Table = LoadStringTable(Target);
			const TMap<FString, FString> Existing = Table
				? ReadStringTableEntries(*Table) : TMap<FString, FString>();
			TMap<FString, FString> Wanted;
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			Desired->TryGetArrayField(TEXT("entries"), Entries);
			if (Entries)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Entries)
				{
					const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
					FString Key;
					FString Source;
					if (Entry.IsValid() && Entry->TryGetStringField(TEXT("key"), Key)
						&& Entry->TryGetStringField(TEXT("source"), Source)) Wanted.Add(Key, Source);
				}
			}
			TArray<TSharedPtr<FJsonValue>> Added;
			TArray<TSharedPtr<FJsonValue>> Removed;
			TArray<TSharedPtr<FJsonValue>> Changed;
			for (const TPair<FString, FString>& Pair : Wanted)
			{
				if (!Existing.Contains(Pair.Key)) Added.Add(MakeShared<FJsonValueString>(Pair.Key));
				else if (Existing[Pair.Key] != Pair.Value) Changed.Add(MakeShared<FJsonValueString>(Pair.Key));
			}
			for (const TPair<FString, FString>& Pair : Existing)
			{
				if (!Wanted.Contains(Pair.Key)) Removed.Add(MakeShared<FJsonValueString>(Pair.Key));
			}
			Out = MakeShared<FJsonObject>();
			Out->SetArrayField(TEXT("added"), Added);
			Out->SetArrayField(TEXT("removed"), Removed);
			Out->SetArrayField(TEXT("changed_entries"), Changed);
			Out->SetBoolField(TEXT("changed"), !Added.IsEmpty() || !Removed.IsEmpty() || !Changed.IsEmpty());
			return true;
		}
	}

	bool IsExternalLocalizationContentGraphRoute(
		const FString& ProviderBinding,
		const FString& Capability)
	{
		return ProviderBinding == TEXT("content_graph.read.compound")
			&& Capabilities.Contains(Capability);
	}

	bool TryExecuteExternalLocalizationContentGraph(
		const FString& Capability,
		const TSharedPtr<FJsonObject>& Parameters,
		TSharedPtr<FJsonObject>& OutObservation,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutObservation.Reset();
		OutError.Reset();
		if (!Capabilities.Contains(Capability))
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("localization_content_graph_capability_unknown"),
				TEXT("No localization content graph capability is registered with this name."), TEXT("execution"));
			Diagnostic.SourceKind = TEXT("command_input");
			Diagnostic.PropertyPath = TEXT("capability");
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Capability);
			TArray<FString> Names = Capabilities.Array(); Names.Sort();
			TArray<TSharedPtr<FJsonValue>> Supported;
			for (const auto& Name : Names) Supported.Add(MakeShared<FJsonValueString>(Name));
			Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(Supported);
			return Fail(OutError, MoveTemp(Diagnostic));
		}
		if (Capability == TEXT("localization.validate"))
		{
			return ExecuteLocalizationValidate(Parameters, OutObservation, OutError);
		}
		if (Capability == TEXT("localization.diff"))
		{
			return ExecuteLocalizationDiff(Parameters, OutObservation, OutError);
		}
		if (Capability == TEXT("string_table.validate"))
		{
			return ExecuteStringTableValidate(Parameters, OutObservation, OutError);
		}
		if (Capability == TEXT("string_table.diff"))
		{
			return ExecuteStringTableDiff(Parameters, OutObservation, OutError);
		}
		if (Capability == TEXT("localization.culture.read"))
		{
			OutObservation = MakeShared<FJsonObject>();
			OutObservation->SetStringField(
				TEXT("current_culture"),
				FInternationalization::Get().GetCurrentCulture()->GetName());
			return true;
		}
		if (Capability == TEXT("localization.target.read"))
		{
			FString Target;
			if (!Parameters.IsValid()
				|| !Parameters->TryGetStringField(TEXT("target"), Target)
				|| Target.IsEmpty())
			{
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("localization_target_required"),
					TEXT("Localization target read requires a non-empty target name."), TEXT("execution"));
				Diagnostic.SourceKind = TEXT("command_input");
				Diagnostic.Source.JsonPath = TEXT("/target");
				Diagnostic.PropertyPath = TEXT("target");
				Diagnostic.ActualValue = ExternalInputShape(Parameters.IsValid() ? Parameters->TryGetField(TEXT("target")) : nullptr);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty target name"));
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			OutObservation = TargetJson(Target, true, true);
			OutObservation->SetBoolField(TEXT("exists"), FindTarget(Target) != nullptr);
			return true;
		}
		if (Capability == TEXT("localization.query_targets"))
		{
			bool bFiles = false;
			bool bStats = false;
			if (Parameters.IsValid())
			{
				Parameters->TryGetBoolField(TEXT("include_files"), bFiles);
				Parameters->TryGetBoolField(TEXT("include_stats"), bStats);
			}
			OutObservation = LocalizationSnapshot(bFiles, bStats);
			return true;
		}
		if (Capability == TEXT("localization.report"))
		{
			FString TargetFilter;
			bool bIncludeAssets = false;
			TSet<FString> CultureFilter;
			if (Parameters.IsValid())
			{
				Parameters->TryGetStringField(TEXT("target"), TargetFilter);
				Parameters->TryGetBoolField(TEXT("include_assets"), bIncludeAssets);
				CultureFilter.Append(ReadStringArray(Parameters, TEXT("cultures")));
			}
			TArray<TSharedPtr<FJsonValue>> Coverage;
			for (const FString& Target : DiscoverTargetNames())
			{
				if (!TargetFilter.IsEmpty() && Target != TargetFilter) continue;
				TArray<FString> PoFiles;
				IFileManager::Get().FindFilesRecursive(
					PoFiles,
					*(LocalizationContentRoot() / Target),
					TEXT("*.po"),
					true,
					false);
				PoFiles.Sort();
				for (const FString& File : PoFiles)
				{
					const FString Culture = FPaths::GetCleanFilename(FPaths::GetPath(File));
					if (!CultureFilter.IsEmpty() && !CultureFilter.Contains(Culture)) continue;
					int32 Translated = 0;
					const int32 Total = CountPoEntries(File, Translated);
					const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("target"), Target);
					Item->SetStringField(TEXT("culture"), Culture);
					if (bIncludeAssets) Item->SetStringField(TEXT("po_file"), File);
					Item->SetNumberField(TEXT("entry_count"), Total);
					Item->SetNumberField(TEXT("translated_count"), Translated);
					Item->SetNumberField(TEXT("coverage"), Total > 0 ? static_cast<double>(Translated) / Total : 0.0);
					Coverage.Add(MakeShared<FJsonValueObject>(Item));
				}
			}
			OutObservation = MakeShared<FJsonObject>();
			OutObservation->SetArrayField(TEXT("coverage_by_culture"), Coverage);
		OutObservation->SetArrayField(
			TEXT("missing"), TArray<TSharedPtr<FJsonValue>>());
		OutObservation->SetArrayField(
			TEXT("stale"), TArray<TSharedPtr<FJsonValue>>());
			return true;
		}
		auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("localization_content_graph_capability_unhandled"),
			TEXT("The registered localization capability has no execution handler."), TEXT("execution"));
		Diagnostic.SourceKind = TEXT("native_result");
		Diagnostic.PropertyPath = TEXT("capability");
		Diagnostic.ActualValue = MakeShared<FJsonValueString>(Capability);
		Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("implemented capability"));
		return Fail(OutError, MoveTemp(Diagnostic));
	}
}
