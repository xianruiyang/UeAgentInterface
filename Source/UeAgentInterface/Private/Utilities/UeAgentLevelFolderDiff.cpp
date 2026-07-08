#include "Utilities/UeAgentLevelFolderDiff.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentStructuredJsonFormatter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UeAgentLevelFolderDiff
{
	FDiffOptions::FDiffOptions()
		: MaxChanges(512)
	{
	}

	namespace
	{
		constexpr const TCHAR* DiffSchema = TEXT("uai.level_folder_diff.v1");

		bool LoadJsonObjectFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *FilePath))
			{
				return false;
			}
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
		}

		FString CanonicalJsonValue(const TSharedPtr<FJsonValue>& Value)
		{
			return Value.IsValid()
				? FUeAgentStructuredJsonFormatter::WriteValue(Value.ToSharedRef())
				: FString(TEXT("null"));
		}

		FString CanonicalJsonObject(const TSharedPtr<FJsonObject>& Object)
		{
			return Object.IsValid()
				? FUeAgentStructuredJsonFormatter::WriteObject(Object.ToSharedRef())
				: FString(TEXT("{}"));
		}

		void CollectJsonFiles(const FString& Root, TMap<FString, FString>& OutFiles)
		{
			FString NormalizedRoot = Root;
			FPaths::NormalizeDirectoryName(NormalizedRoot);
			if (!NormalizedRoot.EndsWith(TEXT("/")))
			{
				NormalizedRoot += TEXT("/");
			}

			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.json"), true, false);
			for (const FString& FilePath : Files)
			{
				FString NormalizedFilePath = FilePath;
				FPaths::NormalizeFilename(NormalizedFilePath);

				FString Relative = NormalizedFilePath;
				if (NormalizedFilePath.StartsWith(NormalizedRoot))
				{
					Relative.RightChopInline(NormalizedRoot.Len(), EAllowShrinking::No);
				}
				else
				{
					FPaths::MakePathRelativeTo(Relative, *NormalizedRoot);
				}
				Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
				OutFiles.Add(Relative, FilePath);
			}
		}

		FString DiffGroupForPath(const FString& RelativePath)
		{
			if (RelativePath.Contains(TEXT("/details/")) || RelativePath.StartsWith(TEXT("world_settings/details/")))
			{
				return TEXT("Property");
			}
			if (RelativePath.EndsWith(TEXT("/properties.json"))
				|| RelativePath == TEXT("world_settings/properties.json")
				|| RelativePath.EndsWith(TEXT("/raw_properties.json"))
				|| RelativePath == TEXT("world_settings/raw_properties.json")
				|| RelativePath.EndsWith(TEXT("/readonly_properties.json"))
				|| RelativePath == TEXT("world_settings/readonly_properties.json"))
			{
				return TEXT("Property");
			}
			if (RelativePath.StartsWith(TEXT("references/")) || RelativePath == TEXT("external_assets.json"))
			{
				return TEXT("Reference");
			}
			if (RelativePath.Contains(TEXT("/components/")))
			{
				if (RelativePath.EndsWith(TEXT("/properties.json")))
				{
					return TEXT("Property");
				}
				return TEXT("Component");
			}
			if (RelativePath.StartsWith(TEXT("actors/items/")))
			{
				if (RelativePath.EndsWith(TEXT("/properties.json")))
				{
					return TEXT("Property");
				}
				return TEXT("Actor");
			}
			return TEXT("Level");
		}

		bool IsLegacyPropertiesFile(const FString& RelativePath, FString& OutErrorCode)
		{
			if (RelativePath.EndsWith(TEXT("/properties.json")) || RelativePath == TEXT("world_settings/properties.json"))
			{
				OutErrorCode = TEXT("legacy_properties_file_not_supported");
				return true;
			}
			if (RelativePath.EndsWith(TEXT("/raw_properties.json")) || RelativePath == TEXT("world_settings/raw_properties.json"))
			{
				OutErrorCode = TEXT("legacy_raw_properties_not_supported");
				return true;
			}
			if (RelativePath.EndsWith(TEXT("/readonly_properties.json")) || RelativePath == TEXT("world_settings/readonly_properties.json"))
			{
				OutErrorCode = TEXT("readonly_properties_not_authoring");
				return true;
			}
			return false;
		}

		TSharedPtr<FJsonObject> MakeChange(const FString& Group, const FString& ChangeType, const FString& Path, const FString& Detail = FString())
		{
			TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("group"), Group);
			Change->SetStringField(TEXT("change"), ChangeType);
			Change->SetStringField(TEXT("path"), Path);
			if (!Detail.IsEmpty())
			{
				Change->SetStringField(TEXT("detail"), Detail);
			}
			return Change;
		}

		void AddChange(TArray<TSharedPtr<FJsonValue>>& Changes, TMap<FString, int32>& CountsByGroup, const TSharedPtr<FJsonObject>& Change, int32 MaxChanges, bool& bTruncated)
		{
			if (!Change.IsValid())
			{
				return;
			}
			FString Group = Change->GetStringField(TEXT("group"));
			CountsByGroup.FindOrAdd(Group)++;
			if (Changes.Num() < MaxChanges)
			{
				Changes.Add(MakeShared<FJsonValueObject>(Change));
			}
			else
			{
				bTruncated = true;
			}
		}

		TMap<FString, FString> BuildPropertyMap(const TSharedPtr<FJsonObject>& Object)
		{
			TMap<FString, FString> Result;
			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
			{
				return Result;
			}
			for (int32 Index = 0; Index < Properties->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> PropertyObject = (*Properties)[Index].IsValid() ? (*Properties)[Index]->AsObject() : nullptr;
				if (!PropertyObject.IsValid())
				{
					Result.Add(FString::Printf(TEXT("$invalid[%d]"), Index), CanonicalJsonValue((*Properties)[Index]));
					continue;
				}
				FString PropertyPath;
				auto TryField = [&PropertyPath](const TSharedPtr<FJsonObject>& Source, const TCHAR* FieldName)
				{
					return Source.IsValid()
						&& Source->TryGetStringField(FieldName, PropertyPath)
						&& !PropertyPath.IsEmpty();
				};
				const TSharedPtr<FJsonObject>* Selector = nullptr;
				const bool bHasPropertyPath =
					TryField(PropertyObject, TEXT("property_path"))
					|| TryField(PropertyObject, TEXT("property_name"))
					|| TryField(PropertyObject, TEXT("name"))
					|| (PropertyObject->TryGetObjectField(TEXT("selector"), Selector) && Selector && Selector->IsValid()
						&& (TryField(*Selector, TEXT("property_path"))
							|| TryField(*Selector, TEXT("property_name"))
							|| TryField(*Selector, TEXT("name"))));
				if (!bHasPropertyPath)
				{
					PropertyPath = FString::Printf(TEXT("$index[%d]"), Index);
				}
				Result.Add(PropertyPath, CanonicalJsonObject(PropertyObject));
			}
			return Result;
		}

		void DiffGroupedDetails(
			const FString& RelativeDetailsRoot,
			const FString& BaseDetailsRoot,
			const FString& TargetDetailsRoot,
			TArray<TSharedPtr<FJsonValue>>& Changes,
			TMap<FString, int32>& CountsByGroup,
			int32 MaxChanges,
			bool& bTruncated)
		{
			TSharedPtr<FJsonObject> BaseObject;
			TSharedPtr<FJsonObject> TargetObject;
			FString Error;
			if (!UeAgentDetailsAuthoring::LoadGroupedDetails(BaseDetailsRoot, BaseObject, Error) ||
				!UeAgentDetailsAuthoring::LoadGroupedDetails(TargetDetailsRoot, TargetObject, Error))
			{
				AddChange(Changes, CountsByGroup, MakeChange(TEXT("Property"), TEXT("changed"), RelativeDetailsRoot / TEXT("index.json"), TEXT("details_parse_or_read_diff")), MaxChanges, bTruncated);
				return;
			}

			TMap<FString, FString> BaseProps = BuildPropertyMap(BaseObject);
			TMap<FString, FString> TargetProps = BuildPropertyMap(TargetObject);
			TSet<FString> Keys;
			for (const TPair<FString, FString>& Pair : BaseProps)
			{
				Keys.Add(Pair.Key);
			}
			for (const TPair<FString, FString>& Pair : TargetProps)
			{
				Keys.Add(Pair.Key);
			}
			TArray<FString> SortedKeys = Keys.Array();
			SortedKeys.Sort();
			for (const FString& Key : SortedKeys)
			{
				const FString* BaseValue = BaseProps.Find(Key);
				const FString* TargetValue = TargetProps.Find(Key);
				if (!BaseValue)
				{
					AddChange(Changes, CountsByGroup, MakeChange(TEXT("Property"), TEXT("added"), RelativeDetailsRoot / TEXT("index.json"), Key), MaxChanges, bTruncated);
				}
				else if (!TargetValue)
				{
					AddChange(Changes, CountsByGroup, MakeChange(TEXT("Property"), TEXT("removed"), RelativeDetailsRoot / TEXT("index.json"), Key), MaxChanges, bTruncated);
				}
				else if (*BaseValue != *TargetValue)
				{
					AddChange(Changes, CountsByGroup, MakeChange(TEXT("Property"), TEXT("changed"), RelativeDetailsRoot / TEXT("index.json"), Key), MaxChanges, bTruncated);
				}
			}
		}

		bool IsUnderHandledDetailsRoot(const FString& RelativePath, const TSet<FString>& HandledDetailsRoots)
		{
			for (const FString& DetailsRoot : HandledDetailsRoots)
			{
				if (RelativePath.StartsWith(DetailsRoot) && RelativePath != DetailsRoot + TEXT("index.json"))
				{
					return true;
				}
			}
			return false;
		}

		TSharedPtr<FJsonObject> MakeCountsJson(const TMap<FString, int32>& CountsByGroup)
		{
			TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
			for (const FString& Group : { FString(TEXT("Level")), FString(TEXT("Actor")), FString(TEXT("Component")), FString(TEXT("Property")), FString(TEXT("Reference")) })
			{
				const int32* Count = CountsByGroup.Find(Group);
				Counts->SetNumberField(Group, Count ? *Count : 0);
			}
			return Counts;
		}
	}

	bool DiffLevelFolders(
		const FDiffOptions& Options,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError)
	{
		const FString BaseRoot = FPaths::ConvertRelativePathToFull(Options.BaseFolder);
		const FString TargetRoot = FPaths::ConvertRelativePathToFull(Options.TargetFolder);
		if (BaseRoot.IsEmpty() || !IFileManager::Get().DirectoryExists(*BaseRoot))
		{
			OutError = TEXT("base_folder_not_found");
			return false;
		}
		if (TargetRoot.IsEmpty() || !IFileManager::Get().DirectoryExists(*TargetRoot))
		{
			OutError = TEXT("target_folder_not_found");
			return false;
		}

		TMap<FString, FString> BaseFiles;
		TMap<FString, FString> TargetFiles;
		CollectJsonFiles(BaseRoot, BaseFiles);
		CollectJsonFiles(TargetRoot, TargetFiles);

		TSet<FString> Paths;
		for (const TPair<FString, FString>& Pair : BaseFiles)
		{
			Paths.Add(Pair.Key);
		}
		for (const TPair<FString, FString>& Pair : TargetFiles)
		{
			Paths.Add(Pair.Key);
		}
		TArray<FString> SortedPaths = Paths.Array();
		SortedPaths.Sort();

		TArray<TSharedPtr<FJsonValue>> Changes;
		TMap<FString, int32> CountsByGroup;
		const int32 MaxChanges = FMath::Max(1, Options.MaxChanges);
		bool bTruncated = false;
		TSet<FString> HandledDetailsRoots;

		for (const FString& RelativePath : SortedPaths)
		{
			if (IsUnderHandledDetailsRoot(RelativePath, HandledDetailsRoots))
			{
				continue;
			}

			const FString* BaseFile = BaseFiles.Find(RelativePath);
			const FString* TargetFile = TargetFiles.Find(RelativePath);
			const FString Group = DiffGroupForPath(RelativePath);
			FString LegacyErrorCode;
			if (IsLegacyPropertiesFile(RelativePath, LegacyErrorCode))
			{
				OutError = FString::Printf(TEXT("%s:%s"), *LegacyErrorCode, *RelativePath);
				return false;
			}
			if (!BaseFile)
			{
				AddChange(Changes, CountsByGroup, MakeChange(Group, TEXT("added"), RelativePath), MaxChanges, bTruncated);
				continue;
			}
			if (!TargetFile)
			{
				AddChange(Changes, CountsByGroup, MakeChange(Group, TEXT("removed"), RelativePath), MaxChanges, bTruncated);
				continue;
			}

			if (RelativePath.EndsWith(TEXT("/details/index.json")) || RelativePath == TEXT("world_settings/details/index.json"))
			{
				FString RelativeDetailsRoot = RelativePath;
				RelativeDetailsRoot.LeftChopInline(FString(TEXT("/index.json")).Len(), EAllowShrinking::No);
				HandledDetailsRoots.Add(RelativeDetailsRoot + TEXT("/"));
				DiffGroupedDetails(RelativeDetailsRoot, BaseRoot / RelativeDetailsRoot, TargetRoot / RelativeDetailsRoot, Changes, CountsByGroup, MaxChanges, bTruncated);
				continue;
			}

			FString BaseText;
			FString TargetText;
			if (!FFileHelper::LoadFileToString(BaseText, **BaseFile) || !FFileHelper::LoadFileToString(TargetText, **TargetFile))
			{
				AddChange(Changes, CountsByGroup, MakeChange(Group, TEXT("changed"), RelativePath, TEXT("file_read_failed")), MaxChanges, bTruncated);
				continue;
			}
			if (BaseText != TargetText)
			{
				AddChange(Changes, CountsByGroup, MakeChange(Group, TEXT("changed"), RelativePath), MaxChanges, bTruncated);
			}
		}

		int32 TotalChanges = 0;
		for (const TPair<FString, int32>& Pair : CountsByGroup)
		{
			TotalChanges += Pair.Value;
		}

		OutSummary = MakeShared<FJsonObject>();
		OutSummary->SetStringField(TEXT("schema"), DiffSchema);
		OutSummary->SetStringField(TEXT("status"), TotalChanges == 0 ? TEXT("same") : TEXT("different"));
		OutSummary->SetBoolField(TEXT("same"), TotalChanges == 0);
		OutSummary->SetStringField(TEXT("base_folder"), BaseRoot);
		OutSummary->SetStringField(TEXT("target_folder"), TargetRoot);
		OutSummary->SetNumberField(TEXT("files_base"), BaseFiles.Num());
		OutSummary->SetNumberField(TEXT("files_target"), TargetFiles.Num());
		OutSummary->SetNumberField(TEXT("changes_total"), TotalChanges);
		OutSummary->SetBoolField(TEXT("truncated"), bTruncated);
		OutSummary->SetObjectField(TEXT("counts_by_group"), MakeCountsJson(CountsByGroup));
		OutSummary->SetArrayField(TEXT("changes"), Changes);
		return true;
	}
}
