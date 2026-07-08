#include "Utilities/UeAgentLevelFolderValidate.h"
#include "Utilities/UeAgentDetailsAuthoring.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace UeAgentLevelFolderValidate
{
	FValidateOptions::FValidateOptions()
		: bCheckReferences(true)
	{
	}

	namespace
	{
		constexpr const TCHAR* ValidateSchema = TEXT("uai.level_folder_validate.v1");
		constexpr const TCHAR* LevelFolderSchema = TEXT("uai.level_folder.v1");
		constexpr const TCHAR* PropertiesSchema = TEXT("uai.uobject_properties.v1");

		bool LoadJsonObjectFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *FilePath))
			{
				OutError = FString::Printf(TEXT("json_read_failed:%s"), *FilePath);
				return false;
			}

			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
			{
				OutError = FString::Printf(TEXT("json_parse_failed:%s"), *FilePath);
				return false;
			}
			return true;
		}

		TSharedPtr<FJsonObject> MakeIssue(const FString& Severity, const FString& Code, const FString& Path, const FString& Message)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), Severity);
			Issue->SetStringField(TEXT("code"), Code);
			Issue->SetStringField(TEXT("path"), Path);
			Issue->SetStringField(TEXT("message"), Message);
			return Issue;
		}

		void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Severity, const FString& Code, const FString& Path, const FString& Message)
		{
			Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(Severity, Code, Path, Message)));
		}

		FString RelPath(const FString& Root, const FString& Path)
		{
			FString Relative = Path;
			FPaths::MakePathRelativeTo(Relative, *Root);
			return Relative;
		}

		FString GetStringFieldAny(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames)
		{
			if (!Object.IsValid())
			{
				return FString();
			}
			for (const FString& FieldName : FieldNames)
			{
				FString Value;
				if (Object->TryGetStringField(FieldName, Value))
				{
					return Value;
				}
			}
			return FString();
		}

		void AddLegacyPropertiesIssue(
			const FString& Path,
			const FString& Code,
			const FString& Surface,
			TArray<TSharedPtr<FJsonValue>>& Issues)
		{
			AddIssue(
				Issues,
				TEXT("error"),
				Code,
				Path,
				FString::Printf(TEXT("%s is a legacy properties authoring surface. Use grouped Details and edit details/index.json fields."), *Surface));
		}

		void AddLegacyPropertiesFileIssues(
			const FString& Root,
			const FString& SearchRoot,
			TArray<TSharedPtr<FJsonValue>>& Issues)
		{
			struct FLegacyFileRule
			{
				const TCHAR* FileName;
				const TCHAR* Surface;
				const TCHAR* Code;
			};

			static const FLegacyFileRule Rules[] = {
				{ TEXT("properties.json"), TEXT("properties.json"), TEXT("legacy_properties_file_not_supported") },
				{ TEXT("raw_properties.json"), TEXT("raw_properties.json"), TEXT("legacy_raw_properties_not_supported") },
				{ TEXT("readonly_properties.json"), TEXT("readonly_properties.json"), TEXT("readonly_properties_not_authoring") }
			};

			for (const FLegacyFileRule& Rule : Rules)
			{
				TArray<FString> Matches;
				IFileManager::Get().FindFilesRecursive(Matches, *SearchRoot, Rule.FileName, true, false, false);
				Matches.Sort();
				for (const FString& Match : Matches)
				{
					AddLegacyPropertiesIssue(RelPath(Root, Match), Rule.Code, Rule.Surface, Issues);
				}
			}
		}

		FString GetPropertyPathFromDescriptor(const TSharedPtr<FJsonObject>& Object)
		{
			FString PropertyPath = GetStringFieldAny(Object, { TEXT("property_path"), TEXT("property_name"), TEXT("name") });
			if (!PropertyPath.IsEmpty())
			{
				return PropertyPath;
			}

			const TSharedPtr<FJsonObject>* Selector = nullptr;
			if (Object.IsValid() && Object->TryGetObjectField(TEXT("selector"), Selector) && Selector && Selector->IsValid())
			{
				return GetStringFieldAny(*Selector, { TEXT("property_path"), TEXT("property_name"), TEXT("name") });
			}
			return FString();
		}

		UClass* ResolveClassPath(const FString& ClassPath)
		{
			if (ClassPath.IsEmpty())
			{
				return nullptr;
			}
			UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
			if (!Class)
			{
				Class = LoadObject<UClass>(nullptr, *ClassPath);
			}
			return Class;
		}

		FString RootPropertyNameFromPath(FString PropertyPath)
		{
			int32 DotIndex = INDEX_NONE;
			if (PropertyPath.FindChar(TEXT('.'), DotIndex))
			{
				PropertyPath.LeftInline(DotIndex);
			}
			int32 BracketIndex = INDEX_NONE;
			if (PropertyPath.FindChar(TEXT('['), BracketIndex))
			{
				PropertyPath.LeftInline(BracketIndex);
			}
			return PropertyPath;
		}

		bool IsPatchValuePresent(const TSharedPtr<FJsonObject>& PropertyObject)
		{
			return PropertyObject.IsValid() &&
				(PropertyObject->HasField(TEXT("value_text")) || PropertyObject->HasField(TEXT("value")));
		}

		void ValidatePropertiesObject(
			const FString& Root,
			const FString& Location,
			const TSharedPtr<FJsonObject>& PropertiesJson,
			UClass* OwnerClass,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			int32& SeenProperties)
		{
			FString Schema;
			if (PropertiesJson.IsValid() && PropertiesJson->TryGetStringField(TEXT("schema"), Schema) && Schema != PropertiesSchema && Schema != LevelFolderSchema)
			{
				AddIssue(Issues, TEXT("warning"), TEXT("unexpected_schema"), Location, Schema);
			}

			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if (!PropertiesJson.IsValid() || !PropertiesJson->TryGetArrayField(TEXT("properties"), Properties))
			{
				AddIssue(Issues, TEXT("error"), TEXT("missing_properties_array"), Location, TEXT("properties container must contain properties[]"));
				return;
			}

			SeenProperties += Properties ? Properties->Num() : 0;
			if (!OwnerClass)
			{
				AddIssue(Issues, TEXT("error"), TEXT("missing_owner_class"), Location, TEXT("cannot validate properties without a valid owner class"));
				return;
			}

			for (int32 Index = 0; Properties && Index < Properties->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> PropertyObject = (*Properties)[Index].IsValid() ? (*Properties)[Index]->AsObject() : nullptr;
				const FString PropertyLocation = FString::Printf(TEXT("%s:properties[%d]"), *Location, Index);
				if (!PropertyObject.IsValid())
				{
					AddIssue(Issues, TEXT("error"), TEXT("invalid_property_entry"), PropertyLocation, TEXT("property entry must be an object"));
					continue;
				}

				const FString PropertyPath = GetPropertyPathFromDescriptor(PropertyObject);
				if (PropertyPath.IsEmpty())
				{
					AddIssue(Issues, TEXT("error"), TEXT("missing_property_path"), PropertyLocation, TEXT("property_path or selector property alias is required"));
					continue;
				}
				if (!IsPatchValuePresent(PropertyObject))
				{
					AddIssue(Issues, TEXT("error"), TEXT("missing_patch_value"), PropertyLocation, TEXT("value_text or value is required"));
				}

				const FString RootPropertyName = RootPropertyNameFromPath(PropertyPath);
				FProperty* Property = FindFProperty<FProperty>(OwnerClass, *RootPropertyName);
				if (!Property)
				{
					AddIssue(Issues, TEXT("error"), TEXT("property_not_found"), PropertyLocation, PropertyPath);
					continue;
				}
				if (!Property->HasAnyPropertyFlags(CPF_Edit))
				{
					AddIssue(Issues, TEXT("warning"), TEXT("property_not_editable"), PropertyLocation, PropertyPath);
				}
				if (Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance | CPF_EditConst))
				{
					AddIssue(Issues, TEXT("warning"), TEXT("property_read_only"), PropertyLocation, PropertyPath);
				}
			}
		}

		void ValidateGroupedDetailsJson(
			const FString& Root,
			const FString& DetailsRoot,
			UClass* OwnerClass,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			int32& SeenProperties)
		{
			if (!UeAgentDetailsAuthoring::HasGroupedDetails(DetailsRoot))
			{
				return;
			}

			TSharedPtr<FJsonObject> PropertiesJson;
			FString Error;
			if (!UeAgentDetailsAuthoring::LoadGroupedDetails(DetailsRoot, PropertiesJson, Error))
			{
				AddIssue(Issues, TEXT("error"), TEXT("invalid_grouped_details"), RelPath(Root, DetailsRoot / TEXT("index.json")), Error);
				return;
			}

			ValidatePropertiesObject(Root, RelPath(Root, DetailsRoot / TEXT("index.json")), PropertiesJson, OwnerClass, Issues, SeenProperties);
		}

		void ValidateActorJson(
			const FString& Root,
			const FString& ActorDirectoryName,
			const FString& ActorRoot,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			int32& ActorCount,
			int32& PropertyCount)
		{
			const FString ActorJsonPath = ActorRoot / TEXT("actor.json");
			TSharedPtr<FJsonObject> ActorJson;
			FString Error;
			if (!LoadJsonObjectFile(ActorJsonPath, ActorJson, Error))
			{
				AddIssue(Issues, TEXT("error"), TEXT("missing_or_invalid_actor_json"), RelPath(Root, ActorJsonPath), Error);
				return;
			}

			++ActorCount;
			FString Schema;
			if (ActorJson->TryGetStringField(TEXT("schema"), Schema) && Schema != LevelFolderSchema)
			{
				AddIssue(Issues, TEXT("warning"), TEXT("unexpected_schema"), RelPath(Root, ActorJsonPath), Schema);
			}

			const FString ActorId = GetStringFieldAny(ActorJson, { TEXT("actor_id") });
			if (ActorId.IsEmpty())
			{
				AddIssue(Issues, TEXT("warning"), TEXT("missing_actor_id"), RelPath(Root, ActorJsonPath), ActorDirectoryName);
			}

			const FString ClassPath = GetStringFieldAny(ActorJson, { TEXT("path"), TEXT("class_path") });
			UClass* ActorClass = ResolveClassPath(ClassPath);
			if (ClassPath.IsEmpty())
			{
				AddIssue(Issues, TEXT("error"), TEXT("missing_path"), RelPath(Root, ActorJsonPath), TEXT("path is required"));
			}
			else if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()))
			{
				AddIssue(Issues, TEXT("error"), TEXT("invalid_actor_class"), RelPath(Root, ActorJsonPath), ClassPath);
				ActorClass = nullptr;
			}

			const TArray<TSharedPtr<FJsonValue>>* InlineActorProperties = nullptr;
			if (ActorJson->TryGetArrayField(TEXT("properties"), InlineActorProperties))
			{
				AddLegacyPropertiesIssue(
					RelPath(Root, ActorJsonPath) + TEXT(":properties"),
					TEXT("legacy_inline_properties_not_supported"),
					TEXT("properties[]"),
					Issues);
			}
			ValidateGroupedDetailsJson(Root, ActorRoot / TEXT("details"), ActorClass, Issues, PropertyCount);

			const FString ComponentsRoot = ActorRoot / TEXT("components");
			if (!IFileManager::Get().DirectoryExists(*ComponentsRoot))
			{
				return;
			}

			TArray<FString> ComponentDirectories;
			IFileManager::Get().FindFiles(ComponentDirectories, *(ComponentsRoot / TEXT("*")), false, true);
			for (const FString& ComponentDirectoryName : ComponentDirectories)
			{
				const FString ComponentRoot = ComponentsRoot / ComponentDirectoryName;
				const FString ComponentJsonPath = ComponentRoot / TEXT("component.json");
				TSharedPtr<FJsonObject> ComponentJson;
				FString ComponentError;
				if (!LoadJsonObjectFile(ComponentJsonPath, ComponentJson, ComponentError))
				{
					AddIssue(Issues, TEXT("error"), TEXT("missing_or_invalid_component_json"), RelPath(Root, ComponentJsonPath), ComponentError);
					continue;
				}

				const FString ComponentClassPath = GetStringFieldAny(ComponentJson, { TEXT("path"), TEXT("class_path"), TEXT("component_class_path") });
				UClass* ComponentClass = ResolveClassPath(ComponentClassPath);
				if (ComponentClassPath.IsEmpty())
				{
					AddIssue(Issues, TEXT("error"), TEXT("missing_component_path"), RelPath(Root, ComponentJsonPath), TEXT("path is required"));
				}
				else if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
				{
					AddIssue(Issues, TEXT("error"), TEXT("invalid_component_class"), RelPath(Root, ComponentJsonPath), ComponentClassPath);
					ComponentClass = nullptr;
				}
				const TArray<TSharedPtr<FJsonValue>>* InlineComponentProperties = nullptr;
				if (ComponentJson->TryGetArrayField(TEXT("properties"), InlineComponentProperties))
				{
					AddLegacyPropertiesIssue(
						RelPath(Root, ComponentJsonPath) + TEXT(":properties"),
						TEXT("legacy_inline_properties_not_supported"),
						TEXT("properties[]"),
						Issues);
				}
				ValidateGroupedDetailsJson(Root, ComponentRoot / TEXT("details"), ComponentClass, Issues, PropertyCount);
			}
		}

		void ValidateWorldSettingsJson(
			const FString& Root,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			int32& PropertyCount)
		{
			const FString SummaryPath = Root / TEXT("world_settings") / TEXT("summary.json");
			if (!IFileManager::Get().FileExists(*SummaryPath))
			{
				AddIssue(Issues, TEXT("warning"), TEXT("missing_world_settings_summary"), RelPath(Root, SummaryPath), TEXT("world_settings/summary.json is recommended for authoring packages"));
				return;
			}

			TSharedPtr<FJsonObject> SummaryJson;
			FString Error;
			if (!LoadJsonObjectFile(SummaryPath, SummaryJson, Error))
			{
				AddIssue(Issues, TEXT("error"), TEXT("invalid_world_settings_summary"), RelPath(Root, SummaryPath), Error);
				return;
			}

			const FString ClassPath = GetStringFieldAny(SummaryJson, { TEXT("class_path"), TEXT("path") });
			UClass* WorldSettingsClass = ResolveClassPath(ClassPath);
			if (!ClassPath.IsEmpty() && (!WorldSettingsClass || !WorldSettingsClass->IsChildOf(AWorldSettings::StaticClass())))
			{
				AddIssue(Issues, TEXT("error"), TEXT("invalid_world_settings_class"), RelPath(Root, SummaryPath), ClassPath);
				WorldSettingsClass = nullptr;
			}
			ValidateGroupedDetailsJson(Root, Root / TEXT("world_settings") / TEXT("details"), WorldSettingsClass, Issues, PropertyCount);
		}

		bool DoesReferenceTargetExist(const FString& TargetPath, const FString& TargetPackagePath)
		{
			if (!TargetPath.IsEmpty())
			{
				if (FindObject<UObject>(nullptr, *TargetPath) || LoadObject<UObject>(nullptr, *TargetPath))
				{
					return true;
				}
			}
			if (!TargetPackagePath.IsEmpty())
			{
				const FSoftObjectPath SoftPath(TargetPackagePath);
				if (SoftPath.IsValid() && SoftPath.TryLoad())
				{
					return true;
				}
			}
			return false;
		}

		void ValidateReferences(const FString& Root, TArray<TSharedPtr<FJsonValue>>& Issues, int32& ReferenceCount)
		{
			const FString ReferencesPath = Root / TEXT("references") / TEXT("assets.json");
			if (!IFileManager::Get().FileExists(*ReferencesPath))
			{
				return;
			}

			TSharedPtr<FJsonObject> ReferencesJson;
			FString Error;
			if (!LoadJsonObjectFile(ReferencesPath, ReferencesJson, Error))
			{
				AddIssue(Issues, TEXT("error"), TEXT("invalid_references_json"), RelPath(Root, ReferencesPath), Error);
				return;
			}

			const TArray<TSharedPtr<FJsonValue>>* References = nullptr;
			if (!ReferencesJson->TryGetArrayField(TEXT("references"), References))
			{
				AddIssue(Issues, TEXT("error"), TEXT("missing_references_array"), RelPath(Root, ReferencesPath), TEXT("references/assets.json must contain references[]"));
				return;
			}

			ReferenceCount += References ? References->Num() : 0;
			for (int32 Index = 0; References && Index < References->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> ReferenceObject = (*References)[Index].IsValid() ? (*References)[Index]->AsObject() : nullptr;
				const FString ReferenceLocation = FString::Printf(TEXT("%s:references[%d]"), *RelPath(Root, ReferencesPath), Index);
				if (!ReferenceObject.IsValid())
				{
					AddIssue(Issues, TEXT("error"), TEXT("invalid_reference_entry"), ReferenceLocation, TEXT("reference entry must be an object"));
					continue;
				}
				const FString TargetPath = GetStringFieldAny(ReferenceObject, { TEXT("target_path") });
				const FString TargetPackagePath = GetStringFieldAny(ReferenceObject, { TEXT("target_package_path") });
				if (TargetPath.IsEmpty() && TargetPackagePath.IsEmpty())
				{
					AddIssue(Issues, TEXT("error"), TEXT("missing_reference_target"), ReferenceLocation, TEXT("target_path or target_package_path is required"));
					continue;
				}
				if (!DoesReferenceTargetExist(TargetPath, TargetPackagePath))
				{
					AddIssue(Issues, TEXT("warning"), TEXT("reference_target_not_loaded_or_missing"), ReferenceLocation, TargetPath.IsEmpty() ? TargetPackagePath : TargetPath);
				}
			}
		}
	}

	bool ValidateLevelFolder(
		const FValidateOptions& Options,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError)
	{
		const FString Root = FPaths::ConvertRelativePathToFull(Options.InputFolder);
		if (Root.IsEmpty() || !IFileManager::Get().DirectoryExists(*Root))
		{
			OutError = TEXT("input_folder_not_found");
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> Issues;
		int32 ActorCount = 0;
		int32 PropertyCount = 0;
		int32 ReferenceCount = 0;
		AddLegacyPropertiesFileIssues(Root, Root, Issues);

		const FString ManifestPath = Root / TEXT("manifest.json");
		TSharedPtr<FJsonObject> ManifestJson;
		FString ManifestError;
		if (!LoadJsonObjectFile(ManifestPath, ManifestJson, ManifestError))
		{
			AddIssue(Issues, TEXT("error"), TEXT("missing_or_invalid_manifest"), RelPath(Root, ManifestPath), ManifestError);
		}
		else
		{
			FString Schema;
			if (ManifestJson->TryGetStringField(TEXT("schema"), Schema) && Schema != LevelFolderSchema)
			{
				AddIssue(Issues, TEXT("warning"), TEXT("unexpected_schema"), RelPath(Root, ManifestPath), Schema);
			}
		}

		const FString ActorItemsRoot = Root / TEXT("actors") / TEXT("items");
		if (!IFileManager::Get().DirectoryExists(*ActorItemsRoot))
		{
			AddIssue(Issues, TEXT("error"), TEXT("actor_items_folder_not_found"), RelPath(Root, ActorItemsRoot), TEXT("actors/items is required"));
		}
		else
		{
			TArray<FString> ActorDirectories;
			IFileManager::Get().FindFiles(ActorDirectories, *(ActorItemsRoot / TEXT("*")), false, true);
			for (const FString& ActorDirectoryName : ActorDirectories)
			{
				ValidateActorJson(Root, ActorDirectoryName, ActorItemsRoot / ActorDirectoryName, Issues, ActorCount, PropertyCount);
			}
		}

		if (Options.bCheckReferences)
		{
			ValidateReferences(Root, Issues, ReferenceCount);
		}
		ValidateWorldSettingsJson(Root, Issues, PropertyCount);

		int32 ErrorCount = 0;
		int32 WarningCount = 0;
		for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
		{
			const TSharedPtr<FJsonObject> Issue = IssueValue.IsValid() ? IssueValue->AsObject() : nullptr;
			const FString Severity = Issue.IsValid() ? Issue->GetStringField(TEXT("severity")) : FString();
			if (Severity == TEXT("error"))
			{
				++ErrorCount;
			}
			else if (Severity == TEXT("warning"))
			{
				++WarningCount;
			}
		}

		OutSummary = MakeShared<FJsonObject>();
		OutSummary->SetStringField(TEXT("schema"), ValidateSchema);
		OutSummary->SetStringField(TEXT("status"), ErrorCount == 0 ? TEXT("valid") : TEXT("invalid"));
		OutSummary->SetStringField(TEXT("folder_path"), Root);
		OutSummary->SetBoolField(TEXT("ok"), ErrorCount == 0);
		OutSummary->SetNumberField(TEXT("actors_seen"), ActorCount);
		OutSummary->SetNumberField(TEXT("properties_seen"), PropertyCount);
		OutSummary->SetNumberField(TEXT("references_seen"), ReferenceCount);
		OutSummary->SetNumberField(TEXT("errors"), ErrorCount);
		OutSummary->SetNumberField(TEXT("warnings"), WarningCount);
		OutSummary->SetArrayField(TEXT("issues"), Issues);
		return true;
	}
}
