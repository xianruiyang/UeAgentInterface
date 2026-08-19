#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"
#include "Utilities/UeAgentAuthoringFieldPolicy.h"
#include "Utilities/UeAgentStructuredJsonFormatter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"

#include <initializer_list>

namespace UeAgentDetailsAuthoring
{
	namespace
	{
		constexpr const TCHAR* DetailsRefSchema = TEXT("uai.details_ref.v1");
		constexpr const TCHAR* DetailsIndexSchema = TEXT("uai.details_index.v1");
		constexpr const TCHAR* DetailsGroupSchema = TEXT("uai.details_group.v1");
		constexpr const TCHAR* DetailsCandidatesSchema = TEXT("uai.details_candidates.v1");
		constexpr const TCHAR* PropertiesSchema = TEXT("uai.uobject_properties.v2");
		constexpr int32 MaxInlineCandidateCount = 64;
		constexpr int32 MaxInlineCandidateJsonLength = 6000;

		TSharedPtr<FJsonObject> BuildDirectApplySummary(
			const UeAgentAuthoringDirectApply::FResult& Result)
		{
			TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
			Summary->SetStringField(TEXT("status"), Result.Status.IsEmpty() ? TEXT("failed") : Result.Status);
			Summary->SetStringField(TEXT("phase"), Result.Phase);
			if (!Result.ErrorCode.IsEmpty()) Summary->SetStringField(TEXT("error_code"), Result.ErrorCode);
			if (!Result.ErrorDetail.IsEmpty()) Summary->SetStringField(TEXT("error_detail"), Result.ErrorDetail);
			if (!Result.PropertyPath.IsEmpty()) Summary->SetStringField(TEXT("property_path"), Result.PropertyPath);
			Summary->SetNumberField(TEXT("properties_seen"), Result.PropertiesSeen);
			Summary->SetNumberField(TEXT("properties_applied"), Result.IsApplied() ? Result.PropertiesSeen : 0);
			Summary->SetNumberField(TEXT("properties_skipped"), 0);
			Summary->SetNumberField(TEXT("properties_failed"), Result.IsApplied() ? 0 : Result.PropertiesSeen);
			Summary->SetBoolField(TEXT("rollback_attempted"), Result.bRollbackAttempted);
			Summary->SetBoolField(TEXT("rollback_succeeded"), Result.bRollbackSucceeded);
			return Summary;
		}

		struct FDetailsGroup
		{
			FString GroupKey;
			FString DisplayName;
			FString Slug;
			FString FileName;
			FString CategorySource;
			TArray<TSharedPtr<FJsonValue>> CategoryPathValues;
			TArray<TSharedPtr<FJsonValue>> Fields;
			int32 EditableFieldCount = 0;
		};

		struct FDetailsDiffFieldInfo
		{
			FString Identity;
			FString PropertyPath;
			FString SelectorKey;
			FString GroupKey;
			FString GroupFile;
			TSharedPtr<FJsonObject> Field;
			FString CanonicalJson;
		};

		bool IsDetailsPropertiesFieldName(const FString& FieldName);
		FString MakeDetailsFieldName(const FString& PropertiesFieldName);

		bool IsHjsonFilePath(const FString& FilePath)
		{
			return FPaths::GetExtension(FilePath, false).Equals(TEXT("hjson"), ESearchCase::IgnoreCase);
		}

		bool LoadDetailsObjectFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
		{
			if (!IsHjsonFilePath(FilePath))
			{
				OutError = FString::Printf(TEXT("hjson_file_required:%s"), *FilePath);
				return false;
			}
			return UeAgentStructuredAuthoringIO::LoadObject(FilePath, OutObject, OutError);
		}

		bool ResolveDetailsChildPath(
			const FString& DetailsRoot,
			const FString& RelativePath,
			FString& OutPath,
			FString& OutError,
			const bool bRequireExists = false)
		{
			UeAgentAuthoringWorkspacePaths::FResolveOptions Options;
			Options.bRequireHjson = true;
			Options.bRequireExists = bRequireExists;
			UeAgentAuthoringWorkspacePaths::FResolvedPath Resolved;
			if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
				DetailsRoot,
				RelativePath,
				Options,
				Resolved,
				OutError))
			{
				return false;
			}
			OutPath = Resolved.AbsolutePath;
			return true;
		}

		bool LoadLegacyJsonObjectFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *FilePath))
			{
				return false;
			}

			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
			{
				return false;
			}
			return true;
		}

		bool SaveDetailsObjectFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Object, FString& OutError)
		{
			if (!IsHjsonFilePath(FilePath))
			{
				OutError = FString::Printf(TEXT("hjson_file_required:%s"), *FilePath);
				return false;
			}
			return UeAgentStructuredAuthoringIO::SaveObject(FilePath, Object, &OutError);
		}

		TSharedPtr<FJsonObject> CloneJsonObject(const TSharedPtr<FJsonObject>& Object, const TSet<FString>* OmittedFields = nullptr)
		{
			TSharedPtr<FJsonObject> Filtered = MakeShared<FJsonObject>();
			if (!Object.IsValid())
			{
				return Filtered;
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				if (OmittedFields && OmittedFields->Contains(Pair.Key))
				{
					continue;
				}
				Filtered->SetField(Pair.Key, Pair.Value);
			}
			const TSharedPtr<FJsonValue> Cloned =
				UeAgentAuthoringState::CloneCanonicalValue(
					MakeShared<FJsonValueObject>(Filtered));
			return Cloned.IsValid() && Cloned->Type == EJson::Object
				? Cloned->AsObject()
				: MakeShared<FJsonObject>();
		}

		TSharedPtr<FJsonValue> CloneJsonValue(const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonValue> Cloned =
				UeAgentAuthoringState::CloneCanonicalValue(Value);
			if (!Cloned.IsValid())
			{
				return MakeShared<FJsonValueNull>();
			}
			return Cloned;
		}

		TArray<TSharedPtr<FJsonValue>> CloneJsonArray(const TArray<TSharedPtr<FJsonValue>>& Values)
		{
			const TSharedPtr<FJsonValue> Cloned =
				UeAgentAuthoringState::CloneCanonicalValue(
					MakeShared<FJsonValueArray>(Values));
			return Cloned.IsValid() && Cloned->Type == EJson::Array
				? Cloned->AsArray()
				: TArray<TSharedPtr<FJsonValue>>();
		}

		FString SafeFileName(FString Value)
		{
			Value.TrimStartAndEndInline();
			if (Value.IsEmpty())
			{
				Value = TEXT("General");
			}

			const FString InvalidChars = FPaths::GetInvalidFileSystemChars();
			for (const TCHAR InvalidChar : InvalidChars)
			{
				Value.ReplaceCharInline(InvalidChar, TCHAR('_'));
			}
			Value.ReplaceInline(TEXT(":"), TEXT("_"));
			Value.ReplaceInline(TEXT("/"), TEXT("_"));
			Value.ReplaceInline(TEXT("\\"), TEXT("_"));
			Value.ReplaceInline(TEXT(" "), TEXT("_"));
			Value.ReplaceInline(TEXT("\t"), TEXT("_"));
			Value.ReplaceInline(TEXT("\r"), TEXT("_"));
			Value.ReplaceInline(TEXT("\n"), TEXT("_"));
			while (Value.Contains(TEXT("__")))
			{
				Value.ReplaceInline(TEXT("__"), TEXT("_"));
			}
			while (Value.StartsWith(TEXT("_")))
			{
				Value.RightChopInline(1, EAllowShrinking::No);
			}
			while (Value.EndsWith(TEXT("_")))
			{
				Value.LeftChopInline(1, EAllowShrinking::No);
			}
			return Value.IsEmpty() ? FString(TEXT("General")) : Value;
		}

		FString GetPropertyPath(const TSharedPtr<FJsonObject>& Field)
		{
			if (!Field.IsValid())
			{
				return FString();
			}
			FString PropertyPath;
			if (Field->TryGetStringField(TEXT("property_path"), PropertyPath) && !PropertyPath.IsEmpty())
			{
				return PropertyPath;
			}
			return FString();
		}

		bool IsEditableField(const TSharedPtr<FJsonObject>& Field)
		{
			const TSharedPtr<FJsonObject>* Availability = nullptr;
			if (Field.IsValid() && Field->TryGetObjectField(TEXT("availability"), Availability) && Availability && Availability->IsValid())
			{
				bool bEditable = true;
				if ((*Availability)->TryGetBoolField(TEXT("editable"), bEditable))
				{
					return bEditable;
				}
			}
			return true;
		}

		TSharedPtr<FJsonObject> BuildEditablePropertiesForApply(
			const TSharedPtr<FJsonObject>& PropertiesJson,
			int32& OutSkippedUnavailable)
		{
			OutSkippedUnavailable = 0;
			if (!PropertiesJson.IsValid())
			{
				return PropertiesJson;
			}

			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if (!PropertiesJson->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
			{
				return PropertiesJson;
			}

			TArray<TSharedPtr<FJsonValue>> EditableProperties;
			EditableProperties.Reserve(Properties->Num());
			for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
			{
				const TSharedPtr<FJsonObject> Property = PropertyValue.IsValid() && PropertyValue->Type == EJson::Object
					? PropertyValue->AsObject()
					: nullptr;
				if (Property.IsValid() && !IsEditableField(Property))
				{
					++OutSkippedUnavailable;
					continue;
				}
				EditableProperties.Add(PropertyValue);
			}

			TSharedPtr<FJsonObject> Filtered = MakeShared<FJsonObject>();
			Filtered->Values = PropertiesJson->Values;
			Filtered->SetArrayField(TEXT("properties"), MoveTemp(EditableProperties));
			Filtered->SetNumberField(TEXT("property_count"), Filtered->GetArrayField(TEXT("properties")).Num());
			return Filtered;
		}

		void ResolveCategory(
			const TSharedPtr<FJsonObject>& Field,
			FString& OutGroupKey,
			FString& OutDisplayName,
			FString& OutCategorySource,
			TArray<TSharedPtr<FJsonValue>>& OutCategoryPathValues)
		{
			OutCategoryPathValues.Reset();
			const TArray<TSharedPtr<FJsonValue>>* CategoryPath = nullptr;
			if (Field.IsValid() && Field->TryGetArrayField(TEXT("category_path"), CategoryPath) && CategoryPath && CategoryPath->Num() > 0)
			{
				TArray<FString> Parts;
				for (const TSharedPtr<FJsonValue>& PartValue : *CategoryPath)
				{
					const FString Part = PartValue.IsValid() ? PartValue->AsString().TrimStartAndEnd() : FString();
					if (!Part.IsEmpty())
					{
						Parts.Add(Part);
						OutCategoryPathValues.Add(MakeShared<FJsonValueString>(Part));
					}
				}
				if (Parts.Num() > 0)
				{
					OutGroupKey = FString::Join(Parts, TEXT("/"));
					OutDisplayName = Parts.Last();
					OutCategorySource = TEXT("reflection_category");
					return;
				}
			}

			OutGroupKey = TEXT("General");
			OutDisplayName = TEXT("General");
			OutCategorySource = TEXT("fallback");
			OutCategoryPathValues.Add(MakeShared<FJsonValueString>(OutDisplayName));
		}

		bool ShouldExternalizeCandidates(const TSharedPtr<FJsonObject>& Field)
		{
			const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
			if (!Field.IsValid() || !Field->TryGetArrayField(TEXT("candidates"), Candidates) || !Candidates)
			{
				return false;
			}
			if (Candidates->Num() > MaxInlineCandidateCount)
			{
				return true;
			}
			const TSharedPtr<FJsonValueArray> CandidateValue = MakeShared<FJsonValueArray>(*Candidates);
			const TSharedRef<FJsonValue> CandidateValueRef = StaticCastSharedRef<FJsonValue>(CandidateValue.ToSharedRef());
			return FUeAgentStructuredJsonFormatter::WriteValue(CandidateValueRef).Len() > MaxInlineCandidateJsonLength;
		}

		bool ExternalizeCandidatesIfNeeded(
			const FString& DetailsRoot,
			const FOwnerInfo& OwnerInfo,
			const FString& GroupSlug,
			const FString& DetailsFileExtension,
			const TSharedPtr<FJsonObject>& Field,
			FString& OutError)
		{
			if (!ShouldExternalizeCandidates(Field))
			{
				return true;
			}

			const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
			if (!Field->TryGetArrayField(TEXT("candidates"), Candidates) || !Candidates)
			{
				return true;
			}
			const int32 CandidateCount = Candidates->Num();

			const FString PropertySlug = SafeFileName(GetPropertyPath(Field));
			const FString CandidateFileName = FString::Printf(TEXT("candidates/%s_%s.%s"), *SafeFileName(GroupSlug), *PropertySlug, *DetailsFileExtension);
			TSharedPtr<FJsonObject> CandidateJson = MakeShared<FJsonObject>();
			CandidateJson->SetStringField(TEXT("schema"), DetailsCandidatesSchema);
			CandidateJson->SetObjectField(TEXT("owner_ref"), BuildOwnerRef(OwnerInfo));
			CandidateJson->SetStringField(TEXT("property_path"), GetPropertyPath(Field));
			CandidateJson->SetNumberField(TEXT("candidate_count"), CandidateCount);
			CandidateJson->SetArrayField(TEXT("candidates"), CloneJsonArray(*Candidates));
			if (!SaveDetailsObjectFile(DetailsRoot / CandidateFileName, CandidateJson, OutError))
			{
				return false;
			}

			Field->RemoveField(TEXT("candidates"));
			Field->SetStringField(TEXT("candidates_ref"), CandidateFileName);
			Field->SetNumberField(TEXT("candidate_count"), CandidateCount);
			return true;
		}

		TSharedPtr<FJsonObject> MakeGroupIndexEntry(const FDetailsGroup& Group)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("group_key"), Group.GroupKey);
			Entry->SetStringField(TEXT("display_name"), Group.DisplayName);
			Entry->SetStringField(TEXT("category_source"), Group.CategorySource);
			Entry->SetStringField(TEXT("file"), Group.FileName);
			Entry->SetNumberField(TEXT("field_count"), Group.Fields.Num());
			Entry->SetNumberField(TEXT("editable_field_count"), Group.EditableFieldCount);
			return Entry;
		}

		TSharedPtr<FJsonObject> MakeGroupFile(const FOwnerInfo& OwnerInfo, const FDetailsGroup& Group)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("schema"), DetailsGroupSchema);
			Object->SetObjectField(TEXT("owner_ref"), BuildOwnerRef(OwnerInfo));
			Object->SetStringField(TEXT("group_key"), Group.GroupKey);
			Object->SetStringField(TEXT("display_name"), Group.DisplayName);
			Object->SetStringField(TEXT("category_source"), Group.CategorySource);
			Object->SetNumberField(TEXT("field_count"), Group.Fields.Num());
			Object->SetNumberField(TEXT("editable_field_count"), Group.EditableFieldCount);
			Object->SetArrayField(TEXT("fields"), CloneJsonArray(Group.Fields));
			return Object;
		}

		TSet<FString> RedundantFieldNames()
		{
			TSet<FString> Fields;
			Fields.Add(TEXT("category_path"));
			Fields.Add(TEXT("operations"));
			return Fields;
		}

		TSharedPtr<FJsonObject> MakeCompactField(const TSharedPtr<FJsonObject>& Field)
		{
			static const TSet<FString> OmittedFields = RedundantFieldNames();
			TSharedPtr<FJsonObject> CompactField = CloneJsonObject(Field, &OmittedFields);
			UeAgentAuthoringFieldPolicy::NormalizeAuthoringObjectTree(CompactField);
			return CompactField;
		}

		FString FirstStringField(const TSharedPtr<FJsonObject>& Object, std::initializer_list<const TCHAR*> FieldNames)
		{
			if (!Object.IsValid())
			{
				return FString();
			}

			for (const TCHAR* FieldName : FieldNames)
			{
				FString Value;
				if (Object->TryGetStringField(FieldName, Value) && !Value.IsEmpty())
				{
					return Value;
				}
			}
			return FString();
		}

		bool IsDetailsPropertyDescriptor(const TSharedPtr<FJsonObject>& Field)
		{
			if (!Field.IsValid() || GetPropertyPath(Field).IsEmpty())
			{
				return false;
			}

			FString ValueKind;
			return Field->TryGetStringField(TEXT("value_kind"), ValueKind) && !ValueKind.IsEmpty();
		}

		bool ValidateInlineDetailsPropertyArray(
			const TArray<TSharedPtr<FJsonValue>>& Properties,
			FString& OutErrorCode)
		{
			TSet<FString> PropertyPaths;
			for (const TSharedPtr<FJsonValue>& PropertyValue : Properties)
			{
				const TSharedPtr<FJsonObject> Property = PropertyValue.IsValid() && PropertyValue->Type == EJson::Object
					? PropertyValue->AsObject()
					: nullptr;
				if (!IsDetailsPropertyDescriptor(Property))
				{
					OutErrorCode = TEXT("inline_properties_invalid");
					return false;
				}

				const FString PropertyPath = GetPropertyPath(Property).ToLower();
				if (PropertyPaths.Contains(PropertyPath))
				{
					OutErrorCode = TEXT("inline_properties_duplicate_property");
					return false;
				}
				PropertyPaths.Add(PropertyPath);

				if (!Property->HasField(TEXT("value")))
				{
					OutErrorCode = TEXT("inline_properties_value_missing");
					return false;
				}
			}

			OutErrorCode.Reset();
			return true;
		}

		FString NormalizeJsonRelativePath(FString Path)
		{
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			while (Path.Contains(TEXT("//")))
			{
				Path.ReplaceInline(TEXT("//"), TEXT("/"));
			}
			while (Path.StartsWith(TEXT("/")))
			{
				Path.RightChopInline(1, EAllowShrinking::No);
			}
			return Path;
		}

		FString NormalizeDetailsIndexFile(const FString& IndexFile)
		{
			FString Normalized = IndexFile;
			Normalized.TrimStartAndEndInline();
			if (Normalized.IsEmpty())
			{
				Normalized = TEXT("details/index.hjson");
			}
			return NormalizeJsonRelativePath(Normalized);
		}

		FString GetDetailsIndexFileName(const FString& IndexFile)
		{
			const FString Normalized = NormalizeDetailsIndexFile(IndexFile);
			const FString FileName = FPaths::GetCleanFilename(Normalized);
			return FileName.IsEmpty() ? FString(TEXT("index.hjson")) : FileName;
		}

		FString GetDetailsFileExtensionForIndex(const FString& IndexFile)
		{
			return TEXT("hjson");
		}

		FString MakeDetailsIndexFileForEntryFile(const FString& EntryFilePath)
		{
			return TEXT("index.hjson");
		}

		FString NormalizeDetailsRefFieldName(const FString& RefFieldName)
		{
			FString Normalized = RefFieldName;
			Normalized.TrimStartAndEndInline();
			return Normalized.IsEmpty() ? FString(TEXT("details")) : Normalized;
		}

		FOwnerInfo ResolveOwnerInfoForObject(const FOwnerInfo& OwnerInfo, UObject* Object)
		{
			FOwnerInfo Resolved = OwnerInfo;
			if (!Object)
			{
				return Resolved;
			}

			if (Resolved.OwnerName.IsEmpty())
			{
				Resolved.OwnerName = Object->GetName();
			}
			if (Resolved.OwnerPath.IsEmpty())
			{
				Resolved.OwnerPath = Object->GetPathName();
			}
			if (Resolved.OwnerClassPath.IsEmpty() && Object->GetClass())
			{
				Resolved.OwnerClassPath = Object->GetClass()->GetPathName();
			}
			if (Resolved.OwnerId.IsEmpty())
			{
				Resolved.OwnerId = Resolved.OwnerPath.IsEmpty() ? Object->GetName() : Resolved.OwnerPath;
			}
			return Resolved;
		}

		bool JsonValueIsArray(const TSharedPtr<FJsonValue>& Value)
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			return Value.IsValid() && Value->TryGetArray(Array) && Array;
		}

		bool IsAuthoringMetadataObject(const TSharedPtr<FJsonObject>& Object)
		{
			if (!Object.IsValid())
			{
				return false;
			}

			const TSharedPtr<FJsonObject>* Selector = nullptr;
			const TSharedPtr<FJsonObject>* Availability = nullptr;
			return Object->TryGetObjectField(TEXT("selector"), Selector)
				&& Selector
				&& Selector->IsValid()
				&& Object->TryGetObjectField(TEXT("availability"), Availability)
				&& Availability
				&& Availability->IsValid();
		}

		FString MakePathRelativeToRoot(const FString& FilePath, const FString& FolderRoot);

		FString NormalizeDiagnosticsRelativePath(FString Path)
		{
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			while (Path.Contains(TEXT("//")))
			{
				Path.ReplaceInline(TEXT("//"), TEXT("/"));
			}
			while (Path.StartsWith(TEXT("/")))
			{
				Path.RightChopInline(1, EAllowShrinking::No);
			}
			return Path.ToLower();
		}

		bool IsDiagnosticsOnlyKeyLower(const FString& KeyLower)
		{
			return KeyLower == TEXT("include_raw_properties")
				|| KeyLower == TEXT("raw_properties")
				|| KeyLower == TEXT("raw_properties_count")
				|| KeyLower == TEXT("readonly_properties")
				|| KeyLower == TEXT("missing_properties")
				|| KeyLower == TEXT("unsupported_properties")
				|| KeyLower.EndsWith(TEXT("_raw_properties"), ESearchCase::CaseSensitive)
				|| KeyLower.EndsWith(TEXT("_raw_properties_count"), ESearchCase::CaseSensitive)
				|| KeyLower.EndsWith(TEXT("_readonly_properties"), ESearchCase::CaseSensitive);
		}

		FString DiagnosticsErrorCodeForSurface(const FString& Surface)
		{
			const FString SurfaceLower = Surface.ToLower();
			if (SurfaceLower == TEXT("include_raw_properties"))
			{
				return TEXT("include_raw_properties_command_param_only");
			}
			if (SurfaceLower.Contains(TEXT("readonly_properties")))
			{
				return TEXT("readonly_properties_not_authoring");
			}
			if (SurfaceLower.Contains(TEXT("raw_properties")))
			{
				return TEXT("legacy_raw_properties_not_supported");
			}
			return TEXT("diagnostic_properties_not_authoring");
		}

		void AddDiagnosticsSurfaceSummary(
			TArray<TSharedPtr<FJsonValue>>& Surfaces,
			const FString& Surface,
			const FString& JsonPath,
			const FString& AllowedLocation,
			const FString& ApplyBehavior,
			const FString& ValidateBehavior)
		{
			TSharedPtr<FJsonObject> SurfaceObj = MakeShared<FJsonObject>();
			SurfaceObj->SetStringField(TEXT("surface"), Surface);
			SurfaceObj->SetStringField(TEXT("json_path"), JsonPath);
			SurfaceObj->SetStringField(TEXT("allowed_location"), AllowedLocation);
			SurfaceObj->SetStringField(TEXT("apply_behavior"), ApplyBehavior);
			SurfaceObj->SetStringField(TEXT("validate_behavior"), ValidateBehavior);
			SurfaceObj->SetBoolField(TEXT("authoring_input_allowed"), false);
			Surfaces.Add(MakeShared<FJsonValueObject>(SurfaceObj));
		}

		void ScanDiagnosticsIsolationObject(
			const TSharedPtr<FJsonObject>& Object,
			const FString& JsonPath,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			TArray<TSharedPtr<FJsonValue>>& Surfaces)
		{
			if (!Object.IsValid())
			{
				return;
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				const FString KeyLower = Pair.Key.ToLower();
				const FString FieldPath = JsonPath.IsEmpty()
					? Pair.Key
					: FString::Printf(TEXT("%s.%s"), *JsonPath, *Pair.Key);
				if (IsDiagnosticsOnlyKeyLower(KeyLower))
				{
					const FString AllowedLocation = GetDiagnosticsOnlyPath(Pair.Key);
					const FString ErrorCode = DiagnosticsErrorCodeForSurface(Pair.Key);
					UeAgentJsonDiagnostics::AddIssue(
						Issues,
						TEXT("error"),
						ErrorCode,
						FieldPath,
						FString::Printf(TEXT("%s is diagnostics-only and is not an authoring input. Allowed location: %s"), *Pair.Key, *AllowedLocation));
					AddDiagnosticsSurfaceSummary(Surfaces, Pair.Key, FieldPath, AllowedLocation, TEXT("reject"), TEXT("error in authoring root"));
					if (UeAgentJsonDiagnostics::JsonValueHasExplicitWriteIntent(Pair.Value))
					{
						UeAgentJsonDiagnostics::AddIssue(
							Issues,
							TEXT("error"),
							TEXT("diagnostic_write_intent_not_authoring"),
							FieldPath,
							TEXT("Diagnostics-only surfaces must not carry apply/write intent"));
					}
				}

				const TSharedPtr<FJsonObject>* ChildObject = nullptr;
				if (Pair.Value.IsValid() && Pair.Value->TryGetObject(ChildObject) && ChildObject && ChildObject->IsValid())
				{
					ScanDiagnosticsIsolationObject(*ChildObject, FieldPath, Issues, Surfaces);
					continue;
				}

				const TArray<TSharedPtr<FJsonValue>>* ChildArray = nullptr;
				if (Pair.Value.IsValid() && Pair.Value->TryGetArray(ChildArray) && ChildArray)
				{
					for (int32 Index = 0; Index < ChildArray->Num(); ++Index)
					{
						const TSharedPtr<FJsonObject>* ArrayObject = nullptr;
						if ((*ChildArray)[Index].IsValid() && (*ChildArray)[Index]->TryGetObject(ArrayObject) && ArrayObject && ArrayObject->IsValid())
						{
							ScanDiagnosticsIsolationObject(*ArrayObject, FString::Printf(TEXT("%s.%d"), *FieldPath, Index), Issues, Surfaces);
						}
					}
				}
			}
		}

		void AddDiagnosticsFileIssueIfPresent(
			const FString& FolderRoot,
			const FString& FileName,
			const FString& ErrorCode,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			TArray<TSharedPtr<FJsonValue>>& Surfaces)
		{
			TArray<FString> Matches;
			IFileManager::Get().FindFilesRecursive(Matches, *FolderRoot, *FileName, true, false, false);
			if (Matches.Num() == 0)
			{
				return;
			}

			Matches.Sort();
			for (const FString& Match : Matches)
			{
				const FString RelativePath = MakePathRelativeToRoot(Match, FolderRoot);
				const FString AllowedLocation = GetDiagnosticsOnlyPath(FileName);
				UeAgentJsonDiagnostics::AddIssue(
					Issues,
					TEXT("error"),
					ErrorCode,
					RelativePath,
					FString::Printf(TEXT("%s is diagnostics-only and must not be placed in the authoring root. Allowed location: %s"), *FileName, *AllowedLocation));
				AddDiagnosticsSurfaceSummary(Surfaces, FileName, RelativePath, AllowedLocation, TEXT("reject"), TEXT("error in authoring root"));
			}
		}

		bool FindAuthoringPropertiesIssueRecursive(
			const TSharedPtr<FJsonObject>& Object,
			const FString& JsonPath,
			FString& OutSurface,
			FString& OutJsonPath,
			FString& OutErrorCode)
		{
			if (!Object.IsValid())
			{
				return false;
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				const FString KeyLower = Pair.Key.ToLower();
				const FString FieldPath = JsonPath.IsEmpty()
					? Pair.Key
					: FString::Printf(TEXT("%s.%s"), *JsonPath, *Pair.Key);
				if (IsDiagnosticsOnlyKeyLower(KeyLower))
				{
					OutSurface = Pair.Key;
					OutJsonPath = FieldPath;
					OutErrorCode = DiagnosticsErrorCodeForSurface(Pair.Key);
					return true;
				}
				if (IsDetailsPropertiesFieldName(KeyLower) && IsAuthoringMetadataObject(Object))
				{
					continue;
				}
				if (IsDetailsPropertiesFieldName(KeyLower))
				{
					if (!JsonValueIsArray(Pair.Value))
					{
						OutSurface = Pair.Key;
						OutJsonPath = FieldPath;
						OutErrorCode = TEXT("inline_properties_invalid");
						return true;
					}
					if (Object->HasField(MakeDetailsFieldName(Pair.Key)))
					{
						OutSurface = Pair.Key + TEXT("[]");
						OutJsonPath = FieldPath;
						OutErrorCode = TEXT("inline_grouped_details_conflict");
						return true;
					}

					FString InlineErrorCode;
					if (!ValidateInlineDetailsPropertyArray(Pair.Value->AsArray(), InlineErrorCode))
					{
						OutSurface = Pair.Key + TEXT("[]");
						OutJsonPath = FieldPath;
						OutErrorCode = InlineErrorCode.IsEmpty() ? FString(TEXT("inline_properties_invalid")) : InlineErrorCode;
						return true;
					}
					continue;
				}
				const TSharedPtr<FJsonObject>* ChildObject = nullptr;
				if (Pair.Value.IsValid() && Pair.Value->TryGetObject(ChildObject) && ChildObject && ChildObject->IsValid())
				{
					if (FindAuthoringPropertiesIssueRecursive(*ChildObject, FieldPath, OutSurface, OutJsonPath, OutErrorCode))
					{
						return true;
					}
					continue;
				}

				const TArray<TSharedPtr<FJsonValue>>* ChildArray = nullptr;
				if (Pair.Value.IsValid() && Pair.Value->TryGetArray(ChildArray) && ChildArray)
				{
					for (int32 Index = 0; Index < ChildArray->Num(); ++Index)
					{
						const TSharedPtr<FJsonObject>* ArrayObject = nullptr;
						if (!(*ChildArray)[Index].IsValid() || !(*ChildArray)[Index]->TryGetObject(ArrayObject) || !ArrayObject || !ArrayObject->IsValid())
						{
							continue;
						}
						const FString ArrayPath = FString::Printf(TEXT("%s.%d"), *FieldPath, Index);
						if (FindAuthoringPropertiesIssueRecursive(*ArrayObject, ArrayPath, OutSurface, OutJsonPath, OutErrorCode))
						{
							return true;
						}
					}
				}
			}
			return false;
		}

		FString ResolutionHintForAuthoringPropertiesError(const FString& ErrorCode)
		{
			if (ErrorCode == TEXT("inline_grouped_details_conflict"))
			{
				return TEXT("Choose one property source for the owner: inline *_properties[] for creation seeds, or the referenced grouped Details files");
			}
			if (ErrorCode == TEXT("inline_properties_invalid"))
			{
				return TEXT("Each inline property must use the uai.uobject_properties.v2 property_path, value_kind, and value fields");
			}
			if (ErrorCode == TEXT("inline_properties_duplicate_property"))
			{
				return TEXT("Keep one inline descriptor for each case-insensitive property path");
			}
			if (ErrorCode == TEXT("inline_properties_value_missing"))
			{
				return TEXT("Provide the canonical value field for every inline property");
			}
			if (ErrorCode == TEXT("diagnostic_properties_not_authoring"))
			{
				return TEXT("Do not copy diagnostic fields into authoring input");
			}
			if (ErrorCode == TEXT("legacy_raw_properties_not_supported"))
			{
				return TEXT("Use grouped Details, or run an explicit migration tool when available");
			}
			if (ErrorCode == TEXT("readonly_properties_not_authoring"))
			{
				return TEXT("Readonly snapshots cannot be edited; edit grouped Details fields only");
			}
			if (ErrorCode == TEXT("include_raw_properties_command_param_only"))
			{
				return TEXT("include_raw_properties is a command parameter only; do not place it in folder JSON");
			}
			return TEXT("Use valid inline *_properties[] for a creation seed, or move editable fields into the referenced grouped Details owner");
		}

		FString MakePathRelativeToRoot(const FString& FilePath, const FString& FolderRoot)
		{
			FString NormalizedPath = FPaths::ConvertRelativePathToFull(FilePath);
			FString NormalizedRoot = FPaths::ConvertRelativePathToFull(FolderRoot);
			FPaths::NormalizeFilename(NormalizedPath);
			FPaths::NormalizeDirectoryName(NormalizedRoot);

			FString RootPrefix = NormalizedRoot / TEXT("");
			FPaths::NormalizeFilename(RootPrefix);
			if (NormalizedPath.StartsWith(RootPrefix, ESearchCase::IgnoreCase))
			{
				return NormalizedPath.RightChop(RootPrefix.Len());
			}
			return NormalizedPath;
		}

		bool IsDiagnosticsOrMigrationRelativePath(const FString& RelativePath)
		{
			const FString PathLower = NormalizeDiagnosticsRelativePath(RelativePath);
			return PathLower.StartsWith(TEXT("debug/"))
				|| PathLower.StartsWith(TEXT("validation/"))
				|| PathLower.StartsWith(TEXT("legacy/"));
		}

		bool FindAuthoringPropertiesFileIssueRecursive(
			const FString& FolderRoot,
			FString& OutSurface,
			FString& OutJsonPath,
			FString& OutErrorCode)
		{
			FString Root = FolderRoot;
			Root.TrimStartAndEndInline();
			if (Root.IsEmpty())
			{
				return false;
			}

			struct FLegacyFileRule
			{
				const TCHAR* FileName;
				const TCHAR* Surface;
				const TCHAR* ErrorCode;
			};

			static const FLegacyFileRule Rules[] = {
				{ TEXT("properties.hjson"), TEXT("properties.hjson"), TEXT("legacy_properties_file_not_supported") },
				{ TEXT("raw_properties.hjson"), TEXT("raw_properties.hjson"), TEXT("legacy_raw_properties_not_supported") },
				{ TEXT("readonly_properties.hjson"), TEXT("readonly_properties.hjson"), TEXT("readonly_properties_not_authoring") },
				{ TEXT("properties.json"), TEXT("properties.json"), TEXT("legacy_properties_file_not_supported") },
				{ TEXT("raw_properties.json"), TEXT("raw_properties.json"), TEXT("legacy_raw_properties_not_supported") },
				{ TEXT("readonly_properties.json"), TEXT("readonly_properties.json"), TEXT("readonly_properties_not_authoring") }
			};

			const FString AbsoluteRoot = FPaths::ConvertRelativePathToFull(Root);
			for (const FLegacyFileRule& Rule : Rules)
			{
				TArray<FString> Matches;
				IFileManager::Get().FindFilesRecursive(Matches, *AbsoluteRoot, Rule.FileName, true, false, false);
				if (Matches.Num() == 0)
				{
					continue;
				}

				Matches.Sort();
				for (const FString& Match : Matches)
				{
					const FString RelativePath = MakePathRelativeToRoot(Match, AbsoluteRoot);
					if (IsDiagnosticsOrMigrationRelativePath(RelativePath))
					{
						continue;
					}

					OutSurface = Rule.Surface;
					OutJsonPath = RelativePath;
					OutErrorCode = Rule.ErrorCode;
					return true;
				}
			}
			return false;
		}

		TSharedPtr<FJsonObject> MakeValidationIssue(
			const FString& Severity,
			const FString& Code,
			const FString& Path,
			const FString& Message)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), Severity);
			Issue->SetStringField(TEXT("code"), Code);
			Issue->SetStringField(TEXT("path"), Path);
			Issue->SetStringField(TEXT("message"), Message);
			return Issue;
		}

		void AddValidationIssue(
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const FString& Severity,
			const FString& Code,
			const FString& Path,
			const FString& Message)
		{
			Issues.Add(MakeShared<FJsonValueObject>(MakeValidationIssue(Severity, Code, Path, Message)));
		}

		bool HasErrorIssue(const TArray<TSharedPtr<FJsonValue>>& Issues)
		{
			for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
			{
				const TSharedPtr<FJsonObject> Issue = IssueValue.IsValid() ? IssueValue->AsObject() : nullptr;
				FString Severity;
				if (Issue.IsValid()
					&& Issue->TryGetStringField(TEXT("severity"), Severity)
					&& Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return false;
		}

		FString ReadStringField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
		{
			FString Value;
			if (Object.IsValid())
			{
				Object->TryGetStringField(FieldName, Value);
			}
			return Value;
		}

		void ValidateOwnerRef(
			const TSharedPtr<FJsonObject>& Object,
			const FOwnerInfo& ExpectedOwnerInfo,
			const FString& Path,
			TArray<TSharedPtr<FJsonValue>>& Issues)
		{
			const TSharedPtr<FJsonObject>* OwnerRef = nullptr;
			if (!Object.IsValid() || !Object->TryGetObjectField(TEXT("owner_ref"), OwnerRef) || !OwnerRef || !OwnerRef->IsValid())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_owner_ref_missing"), Path / TEXT("owner_ref"), TEXT("Details owner_ref is required"));
				return;
			}

			const FString OwnerKind = ReadStringField(*OwnerRef, TEXT("owner_kind"));
			const FString OwnerId = ReadStringField(*OwnerRef, TEXT("owner_id"));
			if (OwnerKind.IsEmpty())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_owner_kind_missing"), Path / TEXT("owner_ref.owner_kind"), TEXT("Details owner_kind is required"));
			}
			if (OwnerId.IsEmpty())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_owner_id_missing"), Path / TEXT("owner_ref.owner_id"), TEXT("Details owner_id is required"));
			}
			if (!ExpectedOwnerInfo.OwnerKind.IsEmpty() && !OwnerKind.IsEmpty() && OwnerKind != ExpectedOwnerInfo.OwnerKind)
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_owner_kind_mismatch"), Path / TEXT("owner_ref.owner_kind"), TEXT("Details owner_kind does not match the requested owner"));
			}
			if (!ExpectedOwnerInfo.OwnerId.IsEmpty() && !OwnerId.IsEmpty() && OwnerId != ExpectedOwnerInfo.OwnerId)
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_owner_id_mismatch"), Path / TEXT("owner_ref.owner_id"), TEXT("Details owner_id does not match the requested owner"));
			}
		}

		FString MakeSelectorKey(const TSharedPtr<FJsonObject>& Field)
		{
			if (!Field.IsValid())
			{
				return FString();
			}

			const TSharedPtr<FJsonObject>* SelectorObject = nullptr;
			if (Field->TryGetObjectField(TEXT("selector"), SelectorObject) && SelectorObject && SelectorObject->IsValid())
			{
				return FUeAgentStructuredJsonFormatter::WriteObject((*SelectorObject).ToSharedRef());
			}

			FString Selector;
			if (Field->TryGetStringField(TEXT("selector"), Selector) && !Selector.IsEmpty())
			{
				return Selector;
			}
			return FString();
		}

		void ValidateFieldCandidates(
			const FString& DetailsRoot,
			const TSharedPtr<FJsonObject>& Field,
			const FString& FieldPath,
			TArray<TSharedPtr<FJsonValue>>& Issues)
		{
			if (!Field.IsValid())
			{
				return;
			}

			if (Field->HasField(TEXT("candidates")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
				if (!Field->TryGetArrayField(TEXT("candidates"), Candidates) || !Candidates)
				{
					AddValidationIssue(Issues, TEXT("error"), TEXT("details_field_candidates_invalid"), FieldPath / TEXT("candidates"), TEXT("candidates must be an array"));
				}
			}

			FString CandidatesRef;
			if (Field->TryGetStringField(TEXT("candidates_ref"), CandidatesRef) && !CandidatesRef.IsEmpty())
			{
				FString CandidatePath;
				FString ResolveError;
				if (!ResolveDetailsChildPath(DetailsRoot, CandidatesRef, CandidatePath, ResolveError, true))
				{
					AddValidationIssue(
						Issues,
						TEXT("error"),
						TEXT("details_field_candidates_ref_invalid"),
						FieldPath / TEXT("candidates_ref"),
						ResolveError);
				}
			}
		}

		void ValidateDetailsField(
			const FString& DetailsRoot,
			const TSharedPtr<FJsonObject>& Field,
			const FString& FieldPath,
			TSet<FString>& PropertyPaths,
			TSet<FString>& Selectors,
			TArray<TSharedPtr<FJsonValue>>& Issues)
		{
			if (!Field.IsValid())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_field_invalid"), FieldPath, TEXT("Details field must be an object"));
				return;
			}

			const FString PropertyPath = GetPropertyPath(Field);
			const FString SelectorKey = MakeSelectorKey(Field);
			if (PropertyPath.IsEmpty() && SelectorKey.IsEmpty())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_field_identity_missing"), FieldPath, TEXT("Details field requires property_path or selector"));
			}
			if (!PropertyPath.IsEmpty())
			{
				if (PropertyPaths.Contains(PropertyPath))
				{
					AddValidationIssue(Issues, TEXT("error"), TEXT("details_duplicate_property_path"), FieldPath / TEXT("property_path"), FString::Printf(TEXT("Duplicate property_path: %s"), *PropertyPath));
				}
				PropertyPaths.Add(PropertyPath);
			}
			if (!SelectorKey.IsEmpty())
			{
				if (Selectors.Contains(SelectorKey))
				{
					AddValidationIssue(Issues, TEXT("error"), TEXT("details_duplicate_selector"), FieldPath / TEXT("selector"), TEXT("Duplicate selector"));
				}
				Selectors.Add(SelectorKey);
			}

			const TSharedPtr<FJsonObject>* Availability = nullptr;
			if (!Field->HasField(TEXT("availability")))
			{
				AddValidationIssue(Issues, TEXT("warning"), TEXT("details_field_availability_missing"), FieldPath / TEXT("availability"), TEXT("availability is recommended for authoring validation"));
			}
			else if (!Field->TryGetObjectField(TEXT("availability"), Availability) || !Availability || !Availability->IsValid())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_field_availability_invalid"), FieldPath / TEXT("availability"), TEXT("availability must be an object"));
			}

			const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
			if (Field->TryGetArrayField(TEXT("operations"), Operations) && Operations)
			{
				static const TSet<FString> AllowedOperations = {
					TEXT("set"),
					TEXT("add"),
					TEXT("remove"),
					TEXT("replace"),
					TEXT("clear"),
					TEXT("rename"),
					TEXT("set_binding")
				};

				if (Operations->Num() == 0)
				{
					AddValidationIssue(Issues, TEXT("error"), TEXT("details_field_operations_empty"), FieldPath / TEXT("operations"), TEXT("operations must not be empty"));
				}
				for (int32 OperationIndex = 0; OperationIndex < Operations->Num(); ++OperationIndex)
				{
					const TSharedPtr<FJsonValue>& OperationValue = (*Operations)[OperationIndex];
					if (!OperationValue.IsValid() || OperationValue->Type != EJson::String)
					{
						AddValidationIssue(Issues, TEXT("error"), TEXT("details_field_operation_invalid"), FString::Printf(TEXT("%s.operations.%d"), *FieldPath, OperationIndex), TEXT("operation must be a string"));
						continue;
					}

					const FString Operation = OperationValue->AsString();
					if (!AllowedOperations.Contains(Operation))
					{
						AddValidationIssue(Issues, TEXT("error"), TEXT("details_field_operation_unsupported"), FString::Printf(TEXT("%s.operations.%d"), *FieldPath, OperationIndex), FString::Printf(TEXT("Unsupported operation: %s"), *Operation));
					}
				}
			}

			ValidateFieldCandidates(DetailsRoot, Field, FieldPath, Issues);
		}

		TSharedPtr<FJsonObject> MakeCoverageGap(const FString& Gap, const FOwnerInfo& OwnerInfo, const FString& Code)
		{
			TSharedPtr<FJsonObject> CoverageGap = MakeShared<FJsonObject>();
			CoverageGap->SetStringField(TEXT("code"), Code);
			CoverageGap->SetStringField(TEXT("target"), Gap);
			CoverageGap->SetStringField(TEXT("owner_kind"), OwnerInfo.OwnerKind);
			CoverageGap->SetStringField(TEXT("owner_id"), OwnerInfo.OwnerId);
			CoverageGap->SetStringField(TEXT("replacement_gate_required"), TEXT("export/readback, compile, command response, or structured compare"));
			return CoverageGap;
		}

		FString MakeDetailsFieldIdentity(const TSharedPtr<FJsonObject>& Field)
		{
			const FString PropertyPath = GetPropertyPath(Field);
			if (!PropertyPath.IsEmpty())
			{
				return TEXT("property_path:") + PropertyPath;
			}

			const FString SelectorKey = MakeSelectorKey(Field);
			if (!SelectorKey.IsEmpty())
			{
				return TEXT("selector:") + SelectorKey;
			}
			return FString();
		}

		bool LoadDetailsFieldMap(
			const FString& DetailsRoot,
			TMap<FString, FDetailsDiffFieldInfo>& OutFields,
			FString& OutError,
			const FString& IndexFile = FString())
		{
			OutFields.Reset();
			FString NormalizedRoot = DetailsRoot;
			NormalizedRoot.TrimStartAndEndInline();
			if (NormalizedRoot.IsEmpty())
			{
				OutError = TEXT("details_root_missing");
				return false;
			}

			FString IndexPath;
			if (!ResolveDetailsChildPath(NormalizedRoot, GetDetailsIndexFileName(IndexFile), IndexPath, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Index;
			if (!LoadDetailsObjectFile(IndexPath, Index, OutError))
			{
				return false;
			}

			const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
			if (!Index->TryGetArrayField(TEXT("groups"), Groups) || !Groups)
			{
				OutError = TEXT("details_index_missing_groups");
				return false;
			}

			for (int32 GroupIndex = 0; GroupIndex < Groups->Num(); ++GroupIndex)
			{
				const TSharedPtr<FJsonObject> GroupEntry = (*Groups)[GroupIndex].IsValid() ? (*Groups)[GroupIndex]->AsObject() : nullptr;
				FString FileName;
				if (!GroupEntry.IsValid() || !GroupEntry->TryGetStringField(TEXT("file"), FileName) || FileName.IsEmpty())
				{
					OutError = FString::Printf(TEXT("details_index_invalid_group:%d"), GroupIndex);
					return false;
				}

				FString GroupKey;
				GroupEntry->TryGetStringField(TEXT("group_key"), GroupKey);

				FString GroupPath;
				if (!ResolveDetailsChildPath(NormalizedRoot, FileName, GroupPath, OutError))
				{
					return false;
				}
				TSharedPtr<FJsonObject> GroupJson;
				if (!LoadDetailsObjectFile(GroupPath, GroupJson, OutError))
				{
					return false;
				}

				const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
				if (!GroupJson->TryGetArrayField(TEXT("fields"), Fields) || !Fields)
				{
					OutError = FString::Printf(TEXT("details_group_missing_fields:%s"), *FileName);
					return false;
				}

				if (GroupKey.IsEmpty())
				{
					GroupJson->TryGetStringField(TEXT("group_key"), GroupKey);
				}

				for (int32 FieldIndex = 0; FieldIndex < Fields->Num(); ++FieldIndex)
				{
					const TSharedPtr<FJsonObject> Field = (*Fields)[FieldIndex].IsValid() ? (*Fields)[FieldIndex]->AsObject() : nullptr;
					const FString Identity = MakeDetailsFieldIdentity(Field);
					if (Identity.IsEmpty())
					{
						OutError = FString::Printf(TEXT("details_field_missing_identity:%s:%d"), *FileName, FieldIndex);
						return false;
					}
					if (OutFields.Contains(Identity))
					{
						OutError = FString::Printf(TEXT("details_duplicate_field_identity:%s"), *Identity);
						return false;
					}

					FDetailsDiffFieldInfo Info;
					Info.Identity = Identity;
					Info.PropertyPath = GetPropertyPath(Field);
					Info.SelectorKey = MakeSelectorKey(Field);
					Info.GroupKey = GroupKey;
					Info.GroupFile = FileName;
					Info.Field = CloneJsonObject(Field);
					Info.CanonicalJson = Field.IsValid() ? FUeAgentStructuredJsonFormatter::WriteObject(Field.ToSharedRef()) : FString();
					OutFields.Add(Identity, MoveTemp(Info));
				}
			}
			return true;
		}

		TSharedPtr<FJsonObject> MakeDetailsFieldChange(
			const FString& ChangeType,
			const FDetailsDiffFieldInfo* OldField,
			const FDetailsDiffFieldInfo* NewField)
		{
			const FDetailsDiffFieldInfo* Reference = NewField ? NewField : OldField;
			TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("change_type"), ChangeType);
			Change->SetStringField(TEXT("identity"), Reference ? Reference->Identity : FString());
			Change->SetStringField(TEXT("property_path"), Reference ? Reference->PropertyPath : FString());
			Change->SetStringField(TEXT("selector"), Reference ? Reference->SelectorKey : FString());
			Change->SetStringField(TEXT("group_key"), Reference ? Reference->GroupKey : FString());
			Change->SetStringField(TEXT("group_file"), Reference ? Reference->GroupFile : FString());
			if (OldField)
			{
				Change->SetStringField(TEXT("old_group_key"), OldField->GroupKey);
				Change->SetStringField(TEXT("old_group_file"), OldField->GroupFile);
				Change->SetObjectField(TEXT("old_field"), CloneJsonObject(OldField->Field));
			}
			if (NewField)
			{
				Change->SetStringField(TEXT("new_group_key"), NewField->GroupKey);
				Change->SetStringField(TEXT("new_group_file"), NewField->GroupFile);
				Change->SetObjectField(TEXT("new_field"), CloneJsonObject(NewField->Field));
			}
			return Change;
		}

		FOwnerInfo InferOwnerInfo(
			const TSharedPtr<FJsonObject>& Object,
			const FString& SourceCommand,
			const FString& JsonPath)
		{
			FOwnerInfo OwnerInfo;
			OwnerInfo.OwnerKind = FirstStringField(Object, {
				TEXT("owner_kind"),
				TEXT("asset_kind"),
				TEXT("kind"),
				TEXT("type"),
				TEXT("schema")
			});
			if (OwnerInfo.OwnerKind.IsEmpty())
			{
				OwnerInfo.OwnerKind = TEXT("details_owner");
			}

			OwnerInfo.OwnerId = FirstStringField(Object, {
				TEXT("id"),
				TEXT("guid"),
				TEXT("name"),
				TEXT("path"),
				TEXT("object_path"),
				TEXT("asset_path")
			});
			if (OwnerInfo.OwnerId.IsEmpty())
			{
				OwnerInfo.OwnerId = JsonPath.IsEmpty() ? FString(TEXT("root")) : JsonPath;
			}

			OwnerInfo.OwnerName = FirstStringField(Object, {
				TEXT("display_name"),
				TEXT("label"),
				TEXT("name"),
				TEXT("id")
			});
			if (OwnerInfo.OwnerName.IsEmpty())
			{
				OwnerInfo.OwnerName = OwnerInfo.OwnerId;
			}

			OwnerInfo.OwnerPath = FirstStringField(Object, {
				TEXT("object_path"),
				TEXT("path"),
				TEXT("asset_path")
			});
			OwnerInfo.OwnerClassPath = FirstStringField(Object, {
				TEXT("class_path"),
				TEXT("class"),
				TEXT("native_class"),
				TEXT("generated_class")
			});
			OwnerInfo.SourceCommand = SourceCommand;
			return OwnerInfo;
		}

		FString MakeDetailsRelativeRoot(
			const TSharedPtr<FJsonObject>& Object,
			const FString& JsonPath,
			TSet<FString>& UsedDetailsRoots)
		{
			FString RelativeRoot;
			if (JsonPath.IsEmpty())
			{
				RelativeRoot = TEXT("details");
			}
			else
			{
				const FString OwnerKey = FirstStringField(Object, {
					TEXT("id"),
					TEXT("name"),
					TEXT("path"),
					TEXT("object_path"),
					TEXT("asset_path")
				});
				const FString BaseName = !OwnerKey.IsEmpty() ? OwnerKey : JsonPath;
				RelativeRoot = FString::Printf(TEXT("details/%s"), *SafeFileName(BaseName));
			}

			RelativeRoot = NormalizeJsonRelativePath(RelativeRoot);
			const FString BaseRoot = RelativeRoot;
			int32 Suffix = 2;
			while (UsedDetailsRoots.Contains(RelativeRoot.ToLower()))
			{
				RelativeRoot = FString::Printf(TEXT("%s_%d"), *BaseRoot, Suffix++);
			}
			UsedDetailsRoots.Add(RelativeRoot.ToLower());
			return RelativeRoot;
		}

		TSharedPtr<FJsonObject> BuildPropertiesContainer(
			const TArray<TSharedPtr<FJsonValue>>& Properties,
			const FString& Profile)
		{
			TSharedPtr<FJsonObject> PropertiesJson = MakeShared<FJsonObject>();
			PropertiesJson->SetStringField(TEXT("schema"), PropertiesSchema);
			PropertiesJson->SetStringField(TEXT("profile"), Profile.IsEmpty() ? FString(TEXT("authoring")) : Profile);
			PropertiesJson->SetNumberField(TEXT("property_count"), Properties.Num());
			PropertiesJson->SetBoolField(TEXT("truncated"), false);
			PropertiesJson->SetArrayField(TEXT("properties"), CloneJsonArray(Properties));
			return PropertiesJson;
		}

		bool IsDetailsPropertiesFieldName(const FString& FieldName)
		{
			if (FieldName == TEXT("properties"))
			{
				return true;
			}
			if (!FieldName.EndsWith(TEXT("_properties"), ESearchCase::CaseSensitive))
			{
				return false;
			}
			return FieldName != TEXT("raw_properties")
				&& FieldName != TEXT("readonly_properties")
				&& FieldName != TEXT("missing_properties")
				&& FieldName != TEXT("unsupported_properties");
		}

		FString MakeDetailsFieldName(const FString& PropertiesFieldName)
		{
			if (PropertiesFieldName == TEXT("properties"))
			{
				return TEXT("details");
			}
			if (PropertiesFieldName.EndsWith(TEXT("_properties"), ESearchCase::CaseSensitive))
			{
				FString Prefix = PropertiesFieldName.LeftChop(FCString::Strlen(TEXT("_properties")));
				return Prefix + TEXT("_details");
			}
			return PropertiesFieldName + TEXT("_details");
		}

		bool IsDetailsRefFieldName(const FString& FieldName)
		{
			return FieldName == TEXT("details") || FieldName.EndsWith(TEXT("_details"), ESearchCase::CaseSensitive);
		}

		FString MakePropertiesFieldNameFromDetails(const FString& DetailsFieldName)
		{
			if (DetailsFieldName == TEXT("details"))
			{
				return TEXT("properties");
			}
			if (DetailsFieldName.EndsWith(TEXT("_details"), ESearchCase::CaseSensitive))
			{
				FString Prefix = DetailsFieldName.LeftChop(FCString::Strlen(TEXT("_details")));
				return Prefix + TEXT("_properties");
			}
			return TEXT("properties");
		}

		bool ExternalizeInlineDetailsRecursive(
			const FString& EntryDirectory,
			const FString& DetailsIndexFileName,
			const FString& EntryDetailsScope,
			const TSharedPtr<FJsonObject>& Object,
			const FString& SourceCommand,
			const FString& JsonPath,
			TSet<FString>& UsedDetailsRoots,
			FString& OutError)
		{
			if (!Object.IsValid())
			{
				return true;
			}

			TArray<FString> Keys;
			Object->Values.GetKeys(Keys);
			for (const FString& Key : Keys)
			{
				if (!IsDetailsPropertiesFieldName(Key))
				{
					continue;
				}

				const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
				if (!Object->TryGetArrayField(Key, Properties) || !Properties)
				{
					OutError = FString::Printf(TEXT("inline_properties_invalid:%s"), *Key);
					return false;
				}
				const FString DetailsFieldName = MakeDetailsFieldName(Key);
				if (Object->HasField(DetailsFieldName))
				{
					OutError = FString::Printf(TEXT("inline_grouped_details_conflict:%s:%s"), *Key, *DetailsFieldName);
					return false;
				}
				FString InlineErrorCode;
				if (!ValidateInlineDetailsPropertyArray(*Properties, InlineErrorCode))
				{
					OutError = FString::Printf(TEXT("%s:%s"), *InlineErrorCode, *Key);
					return false;
				}

				FString Profile;
				Object->TryGetStringField(TEXT("profile"), Profile);
				TSharedPtr<FJsonObject> DetailsSummary;
				const FString DetailsPath = (Key == TEXT("properties"))
					? JsonPath
					: (JsonPath.IsEmpty() ? Key : FString::Printf(TEXT("%s.%s"), *JsonPath, *Key));
				FString DetailsRelativeRoot = MakeDetailsRelativeRoot(Object, DetailsPath, UsedDetailsRoots);
				if (!EntryDetailsScope.IsEmpty())
				{
					const FString Suffix = DetailsRelativeRoot == TEXT("details")
						? FString()
						: DetailsRelativeRoot.RightChop(FCString::Strlen(TEXT("details/")));
					DetailsRelativeRoot = NormalizeJsonRelativePath(
						Suffix.IsEmpty()
							? FString::Printf(TEXT("details/%s"), *EntryDetailsScope)
							: FString::Printf(TEXT("details/%s/%s"), *EntryDetailsScope, *Suffix));
				}
				const FString DetailsIndexFile = DetailsRelativeRoot / DetailsIndexFileName;
				const FString DetailsRoot = EntryDirectory / DetailsRelativeRoot;
				if (!WriteGroupedDetails(
					DetailsRoot,
					InferOwnerInfo(Object, SourceCommand, DetailsPath),
					BuildPropertiesContainer(*Properties, Profile),
					DetailsSummary,
					OutError,
					DetailsIndexFile))
				{
					return false;
				}

				DetailsSummary->SetStringField(TEXT("index_file"), NormalizeJsonRelativePath(DetailsIndexFile));
				Object->RemoveField(Key);
				Object->SetObjectField(DetailsFieldName, DetailsSummary);
			}

			Keys.Reset();
			Object->Values.GetKeys(Keys);
			for (const FString& Key : Keys)
			{
				if (IsDetailsPropertiesFieldName(Key) || IsDetailsRefFieldName(Key))
				{
					continue;
				}

				const TSharedPtr<FJsonValue>* ValuePtr = Object->Values.Find(Key);
				if (!ValuePtr || !ValuePtr->IsValid())
				{
					continue;
				}

				const FString ChildPath = JsonPath.IsEmpty() ? Key : FString::Printf(TEXT("%s.%s"), *JsonPath, *Key);
				if ((*ValuePtr)->Type == EJson::Object)
				{
					if (!ExternalizeInlineDetailsRecursive(EntryDirectory, DetailsIndexFileName, EntryDetailsScope, (*ValuePtr)->AsObject(), SourceCommand, ChildPath, UsedDetailsRoots, OutError))
					{
						return false;
					}
				}
				else if ((*ValuePtr)->Type == EJson::Array)
				{
					const TArray<TSharedPtr<FJsonValue>>& Array = (*ValuePtr)->AsArray();
					for (int32 Index = 0; Index < Array.Num(); ++Index)
					{
						if (Array[Index].IsValid() && Array[Index]->Type == EJson::Object)
						{
							const FString ArrayPath = FString::Printf(TEXT("%s.%d"), *ChildPath, Index);
							if (!ExternalizeInlineDetailsRecursive(EntryDirectory, DetailsIndexFileName, EntryDetailsScope, Array[Index]->AsObject(), SourceCommand, ArrayPath, UsedDetailsRoots, OutError))
							{
								return false;
							}
						}
					}
				}
			}
			return true;
		}

		bool RehydrateGroupedDetailsRecursive(
			const FString& EntryDirectory,
			const TSharedPtr<FJsonObject>& Object,
			FString& OutError)
		{
			if (!Object.IsValid())
			{
				return true;
			}

			TArray<FString> Keys;
			Object->Values.GetKeys(Keys);
			for (const FString& Key : Keys)
			{
				if (!IsDetailsRefFieldName(Key))
				{
					continue;
				}

				const TSharedPtr<FJsonObject>* DetailsRef = nullptr;
				if (!Object->TryGetObjectField(Key, DetailsRef) || !DetailsRef || !DetailsRef->IsValid())
				{
					continue;
				}

				FString IndexFile;
				if ((*DetailsRef)->TryGetStringField(TEXT("index_file"), IndexFile) && !IndexFile.IsEmpty())
				{
					const FString DetailsRoot = EntryDirectory / FPaths::GetPath(NormalizeJsonRelativePath(IndexFile));
					TSharedPtr<FJsonObject> PropertiesJson;
					if (!LoadGroupedDetails(DetailsRoot, PropertiesJson, OutError, IndexFile))
					{
						return false;
					}

					const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
					if (!PropertiesJson->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
					{
						OutError = FString::Printf(TEXT("details_rehydrate_missing_properties:%s"), *IndexFile);
						return false;
					}
					Object->SetArrayField(MakePropertiesFieldNameFromDetails(Key), CloneJsonArray(*Properties));
				}
			}

			Keys.Reset();
			Object->Values.GetKeys(Keys);
			for (const FString& Key : Keys)
			{
				if (IsDetailsPropertiesFieldName(Key) || IsDetailsRefFieldName(Key))
				{
					continue;
				}

				const TSharedPtr<FJsonValue>* ValuePtr = Object->Values.Find(Key);
				if (!ValuePtr || !ValuePtr->IsValid())
				{
					continue;
				}

				if ((*ValuePtr)->Type == EJson::Object)
				{
					if (!RehydrateGroupedDetailsRecursive(EntryDirectory, (*ValuePtr)->AsObject(), OutError))
					{
						return false;
					}
				}
				else if ((*ValuePtr)->Type == EJson::Array)
				{
					const TArray<TSharedPtr<FJsonValue>>& Array = (*ValuePtr)->AsArray();
					for (const TSharedPtr<FJsonValue>& Child : Array)
					{
						if (Child.IsValid() && Child->Type == EJson::Object)
						{
							if (!RehydrateGroupedDetailsRecursive(EntryDirectory, Child->AsObject(), OutError))
							{
								return false;
							}
						}
					}
				}
			}
			return true;
		}
	}

	TSharedPtr<FJsonObject> CloneObjectTree(const TSharedPtr<FJsonObject>& Object)
	{
		return CloneJsonObject(Object);
	}

	bool ExportOwnerDetails(
		const FOwnerDetailsExportRequest& Request,
		TSharedPtr<FJsonObject>& OutDetailsSummary,
		FString& OutError)
	{
		OutDetailsSummary.Reset();
		if (!Request.Object)
		{
			OutError = TEXT("owner_export_missing_object");
			return false;
		}

		FOwnerInfo OwnerInfo = ResolveOwnerInfoForObject(Request.OwnerInfo, Request.Object);
		if (OwnerInfo.OwnerKind.IsEmpty())
		{
			OutError = TEXT("owner_export_missing_owner_kind");
			return false;
		}
		if (OwnerInfo.OwnerId.IsEmpty())
		{
			OutError = TEXT("owner_export_missing_owner_id");
			return false;
		}

		FString DetailsRoot = Request.DetailsRoot;
		DetailsRoot.TrimStartAndEndInline();
		if (DetailsRoot.IsEmpty())
		{
			OutError = TEXT("owner_export_missing_details_root");
			return false;
		}

		UeAgentAuthoringState::FStateSnapshot Snapshot;
		TSharedPtr<FJsonObject> PropertiesJson;
		if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(
				Request.Object, Request.PropertyOptions, Snapshot, PropertiesJson, OutError))
		{
			return false;
		}
		if (!WriteGroupedDetails(DetailsRoot, OwnerInfo, PropertiesJson, OutDetailsSummary, OutError, Request.IndexFile))
		{
			return false;
		}

		OutDetailsSummary->SetStringField(TEXT("index_file"), NormalizeDetailsIndexFile(Request.IndexFile));
		OutDetailsSummary->SetStringField(TEXT("ref_field"), NormalizeDetailsRefFieldName(Request.RefFieldName));
		OutDetailsSummary->SetStringField(TEXT("owner_kind"), OwnerInfo.OwnerKind);
		OutDetailsSummary->SetStringField(TEXT("owner_id"), OwnerInfo.OwnerId);
		return true;
	}

	bool ApplyOwnerDetails(
		const FOwnerDetailsApplyRequest& Request,
		TSharedPtr<FJsonObject>& OutApplySummary,
		FString& OutError)
	{
		OutApplySummary.Reset();
		if (!Request.Object)
		{
			OutError = TEXT("owner_apply_missing_object");
			return false;
		}

		FOwnerInfo OwnerInfo = ResolveOwnerInfoForObject(Request.OwnerInfo, Request.Object);
		if (OwnerInfo.OwnerKind.IsEmpty())
		{
			OutError = TEXT("owner_apply_missing_owner_kind");
			return false;
		}
		if (OwnerInfo.OwnerId.IsEmpty())
		{
			OutError = TEXT("owner_apply_missing_owner_id");
			return false;
		}

		FString DetailsRoot = Request.DetailsRoot;
		DetailsRoot.TrimStartAndEndInline();
		if (DetailsRoot.IsEmpty())
		{
			OutError = TEXT("owner_apply_missing_details_root");
			return false;
		}

		TSharedPtr<FJsonObject> PropertiesJson;
		if (!LoadGroupedDetails(DetailsRoot, PropertiesJson, OutError, Request.IndexFile))
		{
			return false;
		}

		int32 SkippedUnavailable = 0;
		const TSharedPtr<FJsonObject> EditablePropertiesJson = BuildEditablePropertiesForApply(PropertiesJson, SkippedUnavailable);
		UeAgentAuthoringDirectApply::FResult DirectResult;
		FString ApplyError;
		UeAgentDetailsAuthoringService::ApplyObjectProperties(
			Request.Object, EditablePropertiesJson, Request.ApplyOptions, DirectResult, ApplyError);
		TSharedPtr<FJsonObject> PropertyApply = BuildDirectApplySummary(DirectResult);

		FString ApplyStatus;
		PropertyApply->TryGetStringField(TEXT("status"), ApplyStatus);
		if (ApplyStatus.IsEmpty())
		{
			ApplyStatus = TEXT("applied");
		}

		OutApplySummary = MakeShared<FJsonObject>();
		OutApplySummary->SetStringField(TEXT("schema"), TEXT("uai.grouped_details_apply.v1"));
		OutApplySummary->SetBoolField(TEXT("ok"), !ApplyStatus.Equals(TEXT("failed"), ESearchCase::IgnoreCase));
		OutApplySummary->SetStringField(TEXT("status"), ApplyStatus);
		OutApplySummary->SetStringField(TEXT("property_source"), TEXT("grouped"));
		OutApplySummary->SetObjectField(TEXT("owner_ref"), BuildOwnerRef(OwnerInfo));
		OutApplySummary->SetStringField(TEXT("details_root"), DetailsRoot);
		OutApplySummary->SetStringField(TEXT("index_file"), NormalizeDetailsIndexFile(Request.IndexFile));
		OutApplySummary->SetStringField(TEXT("ref_field"), NormalizeDetailsRefFieldName(Request.RefFieldName));
		OutApplySummary->SetNumberField(TEXT("properties_skipped_unavailable"), SkippedUnavailable);
		for (const TCHAR* CountField : { TEXT("properties_seen"), TEXT("properties_applied"), TEXT("properties_skipped"), TEXT("properties_failed") })
		{
			double Count = 0.0;
			if (PropertyApply->TryGetNumberField(CountField, Count))
			{
				OutApplySummary->SetNumberField(CountField, Count);
			}
		}
		OutApplySummary->SetObjectField(TEXT("property_apply"), PropertyApply);
		return true;
	}

	bool ResolveOwnerPropertiesFromEntry(
		const FString& EntryDirectory,
		const TSharedPtr<FJsonObject>& EntryObject,
		const FString& RefFieldName,
		FOwnerPropertiesInput& OutInput,
		FString& OutError)
	{
		OutInput = FOwnerPropertiesInput();
		OutError.Reset();
		if (!EntryObject.IsValid())
		{
			return true;
		}

		OutInput.RefFieldName = NormalizeDetailsRefFieldName(RefFieldName);
		OutInput.PropertiesFieldName = MakePropertiesFieldNameFromDetails(OutInput.RefFieldName);

		const TArray<TSharedPtr<FJsonValue>>* InlineProperties = nullptr;
		const bool bHasInlineField = EntryObject->HasField(OutInput.PropertiesFieldName);
		if (bHasInlineField)
		{
			if (!EntryObject->TryGetArrayField(OutInput.PropertiesFieldName, InlineProperties) || !InlineProperties)
			{
				OutError = FString::Printf(TEXT("inline_properties_invalid:%s"), *OutInput.PropertiesFieldName);
				return false;
			}

			FString InlineErrorCode;
			if (!ValidateInlineDetailsPropertyArray(*InlineProperties, InlineErrorCode))
			{
				OutError = FString::Printf(TEXT("%s:%s"), *InlineErrorCode, *OutInput.PropertiesFieldName);
				return false;
			}
		}

		bool bHasGroupedDetails = false;
		if (!ResolveDetailsRefRoot(
			EntryDirectory,
			EntryObject,
			OutInput.RefFieldName,
			bHasGroupedDetails,
			OutInput.DetailsRoot,
			OutInput.IndexFile,
			OutError))
		{
			return false;
		}

		if (bHasInlineField && bHasGroupedDetails)
		{
			OutError = FString::Printf(
				TEXT("inline_grouped_details_conflict:%s:%s"),
				*OutInput.PropertiesFieldName,
				*OutInput.RefFieldName);
			return false;
		}

		if (bHasInlineField)
		{
			FString Profile;
			EntryObject->TryGetStringField(TEXT("profile"), Profile);
			OutInput.bPresent = true;
			OutInput.bInline = true;
			OutInput.PropertiesJson = BuildPropertiesContainer(*InlineProperties, Profile);
			return true;
		}

		if (!bHasGroupedDetails)
		{
			return true;
		}

		if (!LoadGroupedDetails(OutInput.DetailsRoot, OutInput.PropertiesJson, OutError, OutInput.IndexFile))
		{
			return false;
		}
		OutInput.bPresent = true;
		OutInput.bInline = false;
		return true;
	}

	bool ApplyOwnerDetailsFromEntryIfPresent(
		UObject* Object,
		const FString& EntryDirectory,
		const TSharedPtr<FJsonObject>& EntryObject,
		const FOwnerInfo& OwnerInfo,
		const FString& RefFieldName,
		bool bApply,
		TSharedPtr<FJsonObject>& OutApplySummary,
		int32& InOutAppliedCount,
		FString& OutError)
	{
		OutApplySummary.Reset();
		FOwnerPropertiesInput Input;
		if (!ResolveOwnerPropertiesFromEntry(EntryDirectory, EntryObject, RefFieldName, Input, OutError))
		{
			return false;
		}
		if (!Input.bPresent)
		{
			return true;
		}

		if (!bApply)
		{
			OutApplySummary = MakeShared<FJsonObject>();
			OutApplySummary->SetStringField(TEXT("schema"), Input.bInline ? FString(TEXT("uai.inline_details_apply.v1")) : FString(TEXT("uai.grouped_details_apply.v1")));
			OutApplySummary->SetBoolField(TEXT("ok"), true);
			OutApplySummary->SetStringField(TEXT("status"), TEXT("skipped"));
			OutApplySummary->SetObjectField(TEXT("owner_ref"), BuildOwnerRef(OwnerInfo));
			OutApplySummary->SetStringField(TEXT("property_source"), Input.bInline ? FString(TEXT("inline")) : FString(TEXT("grouped")));
			OutApplySummary->SetStringField(TEXT("ref_field"), Input.RefFieldName);
			OutApplySummary->SetStringField(TEXT("properties_field"), Input.PropertiesFieldName);
			if (!Input.bInline)
			{
				OutApplySummary->SetStringField(TEXT("details_root"), Input.DetailsRoot);
				OutApplySummary->SetStringField(TEXT("index_file"), NormalizeDetailsIndexFile(Input.IndexFile));
			}
			OutApplySummary->SetNumberField(TEXT("properties_applied"), 0);
			return true;
		}
		if (!Object)
		{
			OutError = TEXT("details_owner_missing");
			return false;
		}

		if (Input.bInline)
		{
			UeAgentAuthoringDirectApply::FResult DirectResult;
			FString ApplyError;
			UeAgentDetailsAuthoringService::ApplyObjectProperties(
				Object, Input.PropertiesJson, UeAgentAuthoringObjectApply::FOptions(),
				DirectResult, ApplyError);
			TSharedPtr<FJsonObject> PropertyApply = BuildDirectApplySummary(DirectResult);

			FString ApplyStatus;
			PropertyApply->TryGetStringField(TEXT("status"), ApplyStatus);
			if (ApplyStatus.IsEmpty())
			{
				ApplyStatus = TEXT("applied");
			}

			OutApplySummary = MakeShared<FJsonObject>();
			OutApplySummary->SetStringField(TEXT("schema"), TEXT("uai.inline_details_apply.v1"));
			OutApplySummary->SetBoolField(TEXT("ok"), !ApplyStatus.Equals(TEXT("failed"), ESearchCase::IgnoreCase));
			OutApplySummary->SetStringField(TEXT("status"), ApplyStatus);
			OutApplySummary->SetStringField(TEXT("property_source"), TEXT("inline"));
			OutApplySummary->SetObjectField(TEXT("owner_ref"), BuildOwnerRef(ResolveOwnerInfoForObject(OwnerInfo, Object)));
			OutApplySummary->SetStringField(TEXT("ref_field"), Input.RefFieldName);
			OutApplySummary->SetStringField(TEXT("properties_field"), Input.PropertiesFieldName);
			for (const TCHAR* CountField : { TEXT("properties_seen"), TEXT("properties_applied"), TEXT("properties_skipped"), TEXT("properties_failed") })
			{
				double Count = 0.0;
				if (PropertyApply->TryGetNumberField(CountField, Count))
				{
					OutApplySummary->SetNumberField(CountField, Count);
				}
			}
			OutApplySummary->SetObjectField(TEXT("property_apply"), PropertyApply);
		}
		else
		{
			FOwnerDetailsApplyRequest Request;
			Request.Object = Object;
			Request.OwnerInfo = OwnerInfo;
			Request.DetailsRoot = Input.DetailsRoot;
			Request.IndexFile = Input.IndexFile;
			Request.RefFieldName = Input.RefFieldName;
			if (!ApplyOwnerDetails(Request, OutApplySummary, OutError))
			{
				return false;
			}
		}

		if (OutApplySummary.IsValid())
		{
			bool bOk = true;
			if (OutApplySummary->HasField(TEXT("ok")))
			{
				bOk = OutApplySummary->GetBoolField(TEXT("ok"));
			}
			if (!bOk)
			{
				OutError = Input.bInline ? FString(TEXT("inline_properties_apply_failed")) : FString(TEXT("grouped_details_apply_failed"));
				const TSharedPtr<FJsonObject>* PropertyApply = nullptr;
				if (OutApplySummary->TryGetObjectField(TEXT("property_apply"), PropertyApply) && PropertyApply && PropertyApply->IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>>* PropertyResults = nullptr;
					if ((*PropertyApply)->TryGetArrayField(TEXT("properties"), PropertyResults) && PropertyResults && PropertyResults->Num() > 0)
					{
						const TSharedPtr<FJsonObject> FirstResult = (*PropertyResults)[0].IsValid() ? (*PropertyResults)[0]->AsObject() : nullptr;
						FString PropertyPath;
						FString PropertyReason;
						if (FirstResult.IsValid())
						{
							FirstResult->TryGetStringField(TEXT("property_path"), PropertyPath);
							FirstResult->TryGetStringField(TEXT("reason"), PropertyReason);
						}
						if (!PropertyReason.IsEmpty()) OutError += TEXT(":") + PropertyPath + TEXT(":") + PropertyReason;
					}
				}
				return false;
			}

			double Applied = 0.0;
			if (OutApplySummary->TryGetNumberField(TEXT("properties_applied"), Applied))
			{
				InOutAppliedCount += static_cast<int32>(Applied);
			}
		}
		return true;
	}

	bool ValidateOwnerDetails(
		const FOwnerDetailsValidateRequest& Request,
		TSharedPtr<FJsonObject>& OutValidateSummary,
		FString& OutError)
	{
		OutValidateSummary.Reset();
		OutError.Reset();

		FOwnerInfo OwnerInfo = Request.OwnerInfo;
		if ((OwnerInfo.OwnerKind.IsEmpty() || OwnerInfo.OwnerId.IsEmpty()) && Request.EntryObject.IsValid())
		{
			const FOwnerInfo Inferred = InferOwnerInfo(Request.EntryObject, OwnerInfo.SourceCommand, FString());
			if (OwnerInfo.OwnerKind.IsEmpty())
			{
				OwnerInfo.OwnerKind = Inferred.OwnerKind;
			}
			if (OwnerInfo.OwnerId.IsEmpty())
			{
				OwnerInfo.OwnerId = Inferred.OwnerId;
			}
			if (OwnerInfo.OwnerName.IsEmpty())
			{
				OwnerInfo.OwnerName = Inferred.OwnerName;
			}
			if (OwnerInfo.OwnerPath.IsEmpty())
			{
				OwnerInfo.OwnerPath = Inferred.OwnerPath;
			}
			if (OwnerInfo.OwnerClassPath.IsEmpty())
			{
				OwnerInfo.OwnerClassPath = Inferred.OwnerClassPath;
			}
		}

		FString DetailsRoot = Request.DetailsRoot;
		DetailsRoot.TrimStartAndEndInline();
		const FString IndexFile = NormalizeDetailsIndexFile(Request.IndexFile);
		const FString RefFieldName = NormalizeDetailsRefFieldName(Request.RefFieldName);
		const FString AllowedTarget = IndexFile;

		TSharedPtr<FJsonObject> Rejection;
		if (BuildAuthoringPropertiesRejectionIfPresent(Request.EntryObject, OwnerInfo, AllowedTarget, Rejection))
		{
			OutValidateSummary = Rejection;
			OutValidateSummary->TryGetStringField(TEXT("error_code"), OutError);
			return false;
		}

		FString FolderRoot = Request.FolderRoot;
		FolderRoot.TrimStartAndEndInline();
		if (!FolderRoot.IsEmpty() && BuildAuthoringPropertiesFileRejectionIfPresent(FolderRoot, OwnerInfo, AllowedTarget, Rejection))
		{
			OutValidateSummary = Rejection;
			OutValidateSummary->TryGetStringField(TEXT("error_code"), OutError);
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> Issues;
		TArray<TSharedPtr<FJsonValue>> CoverageGaps;

		OutValidateSummary = MakeShared<FJsonObject>();
		OutValidateSummary->SetStringField(TEXT("schema"), TEXT("uai.grouped_details_validate.v1"));
		OutValidateSummary->SetObjectField(TEXT("owner_ref"), BuildOwnerRef(OwnerInfo));
		OutValidateSummary->SetStringField(TEXT("details_root"), DetailsRoot);
		OutValidateSummary->SetStringField(TEXT("index_file"), IndexFile);
		OutValidateSummary->SetStringField(TEXT("ref_field"), RefFieldName);

		auto FinalizeSummary = [&OutValidateSummary, &Issues, &CoverageGaps]() -> bool
		{
			const bool bHasError = HasErrorIssue(Issues);
			const bool bHasCoverageGap = CoverageGaps.Num() > 0;
			OutValidateSummary->SetBoolField(TEXT("ok"), !bHasError && !bHasCoverageGap);
			OutValidateSummary->SetStringField(TEXT("status"), bHasError ? FString(TEXT("failed")) : (bHasCoverageGap ? FString(TEXT("coverage_gap")) : FString(TEXT("passed"))));
			OutValidateSummary->SetNumberField(TEXT("issue_count"), Issues.Num());
			OutValidateSummary->SetNumberField(TEXT("coverage_gap_count"), CoverageGaps.Num());
			OutValidateSummary->SetArrayField(TEXT("issues"), MoveTemp(Issues));
			OutValidateSummary->SetArrayField(TEXT("coverage_gaps"), MoveTemp(CoverageGaps));
			return true;
		};

		if (OwnerInfo.OwnerKind.IsEmpty())
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_owner_kind_missing"), TEXT("owner_ref.owner_kind"), TEXT("Validate request requires owner_kind"));
		}
		if (OwnerInfo.OwnerId.IsEmpty())
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_owner_id_missing"), TEXT("owner_ref.owner_id"), TEXT("Validate request requires owner_id"));
		}

		if (Request.EntryObject.IsValid() && Request.EntryObject->HasField(RefFieldName))
		{
			const TSharedPtr<FJsonObject>* DetailsRef = nullptr;
			if (!Request.EntryObject->TryGetObjectField(RefFieldName, DetailsRef) || !DetailsRef || !DetailsRef->IsValid())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_ref_invalid"), RefFieldName, TEXT("details ref must be an object"));
			}
			else
			{
				const FString RefSchema = ReadStringField(*DetailsRef, TEXT("schema"));
				if (!RefSchema.IsEmpty() && RefSchema != DetailsRefSchema)
				{
					AddValidationIssue(Issues, TEXT("error"), TEXT("details_ref_schema_invalid"), RefFieldName / TEXT("schema"), TEXT("details ref schema must be uai.details_ref.v1"));
				}
				const FString RefIndexFile = NormalizeDetailsIndexFile(ReadStringField(*DetailsRef, TEXT("index_file")));
				if (RefIndexFile != IndexFile)
				{
					AddValidationIssue(Issues, TEXT("error"), TEXT("details_ref_index_mismatch"), RefFieldName / TEXT("index_file"), TEXT("details ref index_file does not match validate request"));
				}
			}
		}

		if (DetailsRoot.IsEmpty())
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_root_missing"), TEXT("details_root"), TEXT("Validate request requires details_root"));
			return FinalizeSummary();
		}

		FString IndexPath;
		FString LoadError;
		if (!ResolveDetailsChildPath(DetailsRoot, GetDetailsIndexFileName(IndexFile), IndexPath, LoadError))
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_path_invalid"), IndexFile, LoadError);
			return FinalizeSummary();
		}
		TSharedPtr<FJsonObject> Index;
		if (!LoadDetailsObjectFile(IndexPath, Index, LoadError))
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_read_failed"), IndexFile, LoadError);
			return FinalizeSummary();
		}

		const FString IndexSchema = ReadStringField(Index, TEXT("schema"));
		if (IndexSchema != DetailsIndexSchema)
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_schema_invalid"), IndexFile / TEXT("schema"), TEXT("details index schema must be uai.details_index.v1"));
		}
		ValidateOwnerRef(Index, OwnerInfo, IndexFile, Issues);

		const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
		if (!Index->TryGetArrayField(TEXT("groups"), Groups) || !Groups)
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_groups_missing"), IndexFile / TEXT("groups"), TEXT("details index requires groups array"));
			return FinalizeSummary();
		}

		TSet<FString> GroupFiles;
		TSet<FString> PropertyPaths;
		TSet<FString> Selectors;
		int32 ActualFieldCount = 0;
		int32 ActualEditableFieldCount = 0;

		for (int32 GroupIndex = 0; GroupIndex < Groups->Num(); ++GroupIndex)
		{
			const FString GroupPath = FString::Printf(TEXT("%s.groups.%d"), *IndexFile, GroupIndex);
			const TSharedPtr<FJsonObject> GroupEntry = (*Groups)[GroupIndex].IsValid() ? (*Groups)[GroupIndex]->AsObject() : nullptr;
			if (!GroupEntry.IsValid())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_group_invalid"), GroupPath, TEXT("group index entry must be an object"));
				continue;
			}

			FString FileName;
			if (!GroupEntry->TryGetStringField(TEXT("file"), FileName) || FileName.IsEmpty())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_group_file_missing"), GroupPath / TEXT("file"), TEXT("group index entry requires file"));
				continue;
			}
			if (GroupFiles.Contains(FileName))
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_duplicate_group_file"), GroupPath / TEXT("file"), FString::Printf(TEXT("Duplicate group file: %s"), *FileName));
			}
			GroupFiles.Add(FileName);

			FString GroupPathOnDisk;
			if (!ResolveDetailsChildPath(DetailsRoot, FileName, GroupPathOnDisk, LoadError))
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_group_path_invalid"), GroupPath / TEXT("file"), LoadError);
				continue;
			}
			TSharedPtr<FJsonObject> GroupJson;
			if (!LoadDetailsObjectFile(GroupPathOnDisk, GroupJson, LoadError))
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_group_read_failed"), GroupPath / TEXT("file"), LoadError);
				continue;
			}

			const FString GroupFilePath = FString::Printf(TEXT("details/%s"), *FileName);
			const FString GroupSchema = ReadStringField(GroupJson, TEXT("schema"));
			if (GroupSchema != DetailsGroupSchema)
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_group_schema_invalid"), GroupFilePath / TEXT("schema"), TEXT("details group schema must be uai.details_group.v1"));
			}
			ValidateOwnerRef(GroupJson, OwnerInfo, GroupFilePath, Issues);

			const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
			if (!GroupJson->TryGetArrayField(TEXT("fields"), Fields) || !Fields)
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_group_fields_missing"), GroupFilePath / TEXT("fields"), TEXT("details group requires fields array"));
				continue;
			}

			ActualFieldCount += Fields->Num();
			int32 GroupEditableFieldCount = 0;
			for (int32 FieldIndex = 0; FieldIndex < Fields->Num(); ++FieldIndex)
			{
				const TSharedPtr<FJsonObject> Field = (*Fields)[FieldIndex].IsValid() ? (*Fields)[FieldIndex]->AsObject() : nullptr;
				const FString FieldPath = FString::Printf(TEXT("%s.fields.%d"), *GroupFilePath, FieldIndex);
				if (IsEditableField(Field))
				{
					++GroupEditableFieldCount;
				}
				ValidateDetailsField(DetailsRoot, Field, FieldPath, PropertyPaths, Selectors, Issues);
			}
			ActualEditableFieldCount += GroupEditableFieldCount;

			double DeclaredGroupFieldCount = 0.0;
			if (GroupJson->TryGetNumberField(TEXT("field_count"), DeclaredGroupFieldCount) && static_cast<int32>(DeclaredGroupFieldCount) != Fields->Num())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_group_field_count_mismatch"), GroupFilePath / TEXT("field_count"), TEXT("details group field_count does not match fields length"));
			}
			double DeclaredGroupEditableFieldCount = 0.0;
			if (GroupJson->TryGetNumberField(TEXT("editable_field_count"), DeclaredGroupEditableFieldCount) && static_cast<int32>(DeclaredGroupEditableFieldCount) != GroupEditableFieldCount)
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_group_editable_field_count_mismatch"), GroupFilePath / TEXT("editable_field_count"), TEXT("details group editable_field_count does not match fields"));
			}
		}

		double DeclaredGroupCount = 0.0;
		if (Index->TryGetNumberField(TEXT("group_count"), DeclaredGroupCount) && static_cast<int32>(DeclaredGroupCount) != Groups->Num())
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_group_count_mismatch"), IndexFile / TEXT("group_count"), TEXT("details index group_count does not match groups length"));
		}
		double DeclaredFieldCount = 0.0;
		if (Index->TryGetNumberField(TEXT("field_count"), DeclaredFieldCount) && static_cast<int32>(DeclaredFieldCount) != ActualFieldCount)
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_field_count_mismatch"), IndexFile / TEXT("field_count"), TEXT("details index field_count does not match group fields"));
		}
		double DeclaredEditableFieldCount = 0.0;
		if (Index->TryGetNumberField(TEXT("editable_field_count"), DeclaredEditableFieldCount) && static_cast<int32>(DeclaredEditableFieldCount) != ActualEditableFieldCount)
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_editable_field_count_mismatch"), IndexFile / TEXT("editable_field_count"), TEXT("details index editable_field_count does not match group fields"));
		}

		for (const FString& Gap : Request.CoverageGaps)
		{
			if (!Gap.IsEmpty())
			{
				CoverageGaps.Add(MakeShared<FJsonValueObject>(MakeCoverageGap(Gap, OwnerInfo, TEXT("missing_dedicated_validate"))));
			}
		}

		return FinalizeSummary();
	}

	bool DiffOwnerDetails(
		const FOwnerDetailsDiffRequest& Request,
		TSharedPtr<FJsonObject>& OutDiffSummary,
		FString& OutError)
	{
		OutDiffSummary.Reset();
		OutError.Reset();

		FOwnerInfo OwnerInfo = Request.OwnerInfo;
		if ((OwnerInfo.OwnerKind.IsEmpty() || OwnerInfo.OwnerId.IsEmpty()) && Request.NewEntryObject.IsValid())
		{
			const FOwnerInfo Inferred = InferOwnerInfo(Request.NewEntryObject, OwnerInfo.SourceCommand, FString());
			if (OwnerInfo.OwnerKind.IsEmpty())
			{
				OwnerInfo.OwnerKind = Inferred.OwnerKind;
			}
			if (OwnerInfo.OwnerId.IsEmpty())
			{
				OwnerInfo.OwnerId = Inferred.OwnerId;
			}
			if (OwnerInfo.OwnerName.IsEmpty())
			{
				OwnerInfo.OwnerName = Inferred.OwnerName;
			}
			if (OwnerInfo.OwnerPath.IsEmpty())
			{
				OwnerInfo.OwnerPath = Inferred.OwnerPath;
			}
			if (OwnerInfo.OwnerClassPath.IsEmpty())
			{
				OwnerInfo.OwnerClassPath = Inferred.OwnerClassPath;
			}
		}
		if ((OwnerInfo.OwnerKind.IsEmpty() || OwnerInfo.OwnerId.IsEmpty()) && Request.OldEntryObject.IsValid())
		{
			const FOwnerInfo Inferred = InferOwnerInfo(Request.OldEntryObject, OwnerInfo.SourceCommand, FString());
			if (OwnerInfo.OwnerKind.IsEmpty())
			{
				OwnerInfo.OwnerKind = Inferred.OwnerKind;
			}
			if (OwnerInfo.OwnerId.IsEmpty())
			{
				OwnerInfo.OwnerId = Inferred.OwnerId;
			}
		}

		const FString OldIndexFile = NormalizeDetailsIndexFile(Request.OldIndexFile);
		const FString NewIndexFile = NormalizeDetailsIndexFile(Request.NewIndexFile);
		const FString RefFieldName = NormalizeDetailsRefFieldName(Request.RefFieldName);
		const FString AllowedTarget = NewIndexFile;

		TSharedPtr<FJsonObject> Rejection;
		if (BuildAuthoringPropertiesRejectionIfPresent(Request.OldEntryObject, OwnerInfo, AllowedTarget, Rejection)
			|| BuildAuthoringPropertiesRejectionIfPresent(Request.NewEntryObject, OwnerInfo, AllowedTarget, Rejection))
		{
			OutDiffSummary = Rejection;
			OutDiffSummary->TryGetStringField(TEXT("error_code"), OutError);
			return false;
		}

		FString OldFolderRoot = Request.OldFolderRoot;
		OldFolderRoot.TrimStartAndEndInline();
		FString NewFolderRoot = Request.NewFolderRoot;
		NewFolderRoot.TrimStartAndEndInline();
		if ((!OldFolderRoot.IsEmpty() && BuildAuthoringPropertiesFileRejectionIfPresent(OldFolderRoot, OwnerInfo, AllowedTarget, Rejection))
			|| (!NewFolderRoot.IsEmpty() && BuildAuthoringPropertiesFileRejectionIfPresent(NewFolderRoot, OwnerInfo, AllowedTarget, Rejection)))
		{
			OutDiffSummary = Rejection;
			OutDiffSummary->TryGetStringField(TEXT("error_code"), OutError);
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> Issues;
		TArray<TSharedPtr<FJsonValue>> CoverageGaps;
		TArray<TSharedPtr<FJsonValue>> FieldChanges;
		TArray<TSharedPtr<FJsonValue>> DomainChanges;

		OutDiffSummary = MakeShared<FJsonObject>();
		OutDiffSummary->SetStringField(TEXT("schema"), TEXT("uai.grouped_details_diff.v1"));
		OutDiffSummary->SetObjectField(TEXT("owner_ref"), BuildOwnerRef(OwnerInfo));
		OutDiffSummary->SetStringField(TEXT("old_details_root"), Request.OldDetailsRoot);
		OutDiffSummary->SetStringField(TEXT("new_details_root"), Request.NewDetailsRoot);
		OutDiffSummary->SetStringField(TEXT("old_index_file"), OldIndexFile);
		OutDiffSummary->SetStringField(TEXT("new_index_file"), NewIndexFile);
		OutDiffSummary->SetStringField(TEXT("ref_field"), RefFieldName);

		auto FinalizeSummary = [&OutDiffSummary, &Issues, &CoverageGaps, &FieldChanges, &DomainChanges]() -> bool
		{
			const bool bHasError = HasErrorIssue(Issues);
			const bool bHasCoverageGap = CoverageGaps.Num() > 0;
			OutDiffSummary->SetBoolField(TEXT("ok"), !bHasError && !bHasCoverageGap);
			OutDiffSummary->SetStringField(TEXT("status"), bHasError ? FString(TEXT("failed")) : (bHasCoverageGap ? FString(TEXT("coverage_gap")) : FString(TEXT("diffed"))));
			OutDiffSummary->SetNumberField(TEXT("issue_count"), Issues.Num());
			OutDiffSummary->SetNumberField(TEXT("coverage_gap_count"), CoverageGaps.Num());
			OutDiffSummary->SetNumberField(TEXT("field_change_count"), FieldChanges.Num());
			OutDiffSummary->SetNumberField(TEXT("domain_change_count"), DomainChanges.Num());
			OutDiffSummary->SetArrayField(TEXT("issues"), MoveTemp(Issues));
			OutDiffSummary->SetArrayField(TEXT("coverage_gaps"), MoveTemp(CoverageGaps));
			OutDiffSummary->SetArrayField(TEXT("field_changes"), MoveTemp(FieldChanges));
			OutDiffSummary->SetArrayField(TEXT("domain_changes"), MoveTemp(DomainChanges));
			return true;
		};

		FString OldDetailsRoot = Request.OldDetailsRoot;
		OldDetailsRoot.TrimStartAndEndInline();
		FString NewDetailsRoot = Request.NewDetailsRoot;
		NewDetailsRoot.TrimStartAndEndInline();
		if (OldDetailsRoot.IsEmpty())
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("old_details_root_missing"), TEXT("old_details_root"), TEXT("Diff request requires old_details_root"));
		}
		if (NewDetailsRoot.IsEmpty())
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("new_details_root_missing"), TEXT("new_details_root"), TEXT("Diff request requires new_details_root"));
		}
		if (HasErrorIssue(Issues))
		{
			return FinalizeSummary();
		}

		TMap<FString, FDetailsDiffFieldInfo> OldFields;
		TMap<FString, FDetailsDiffFieldInfo> NewFields;
		FString LoadError;
		if (!LoadDetailsFieldMap(OldDetailsRoot, OldFields, LoadError, OldIndexFile))
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("old_details_read_failed"), TEXT("old_details_root"), LoadError);
			return FinalizeSummary();
		}
		if (!LoadDetailsFieldMap(NewDetailsRoot, NewFields, LoadError, NewIndexFile))
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("new_details_read_failed"), TEXT("new_details_root"), LoadError);
			return FinalizeSummary();
		}

		for (const TPair<FString, FDetailsDiffFieldInfo>& OldPair : OldFields)
		{
			const FDetailsDiffFieldInfo* NewField = NewFields.Find(OldPair.Key);
			if (!NewField)
			{
				FieldChanges.Add(MakeShared<FJsonValueObject>(MakeDetailsFieldChange(TEXT("removed"), &OldPair.Value, nullptr)));
				continue;
			}
			if (OldPair.Value.CanonicalJson != NewField->CanonicalJson)
			{
				FieldChanges.Add(MakeShared<FJsonValueObject>(MakeDetailsFieldChange(TEXT("changed"), &OldPair.Value, NewField)));
			}
		}
		for (const TPair<FString, FDetailsDiffFieldInfo>& NewPair : NewFields)
		{
			if (!OldFields.Contains(NewPair.Key))
			{
				FieldChanges.Add(MakeShared<FJsonValueObject>(MakeDetailsFieldChange(TEXT("added"), nullptr, &NewPair.Value)));
			}
		}

		for (const FString& Gap : Request.CoverageGaps)
		{
			if (!Gap.IsEmpty())
			{
				CoverageGaps.Add(MakeShared<FJsonValueObject>(MakeCoverageGap(Gap, OwnerInfo, TEXT("missing_dedicated_diff"))));
			}
		}

		return FinalizeSummary();
	}

	FString GetDiagnosticsOnlyPath(const FString& Surface)
	{
		const FString SurfaceLower = NormalizeDiagnosticsRelativePath(Surface);
		if (SurfaceLower == TEXT("include_raw_properties"))
		{
			return TEXT("command parameter only");
		}
		if (SurfaceLower.Contains(TEXT("readonly_properties")))
		{
			return TEXT("debug/readonly_properties_snapshot.json or required_readonly summary");
		}
		if (SurfaceLower.Contains(TEXT("missing_properties")) || SurfaceLower.Contains(TEXT("unsupported_properties")))
		{
			return TEXT("validation/issues.json or debug/details_diagnostics.json");
		}
		if (SurfaceLower.Contains(TEXT("raw_properties")))
		{
			return TEXT("debug/raw_properties_snapshot.json");
		}
		if (SurfaceLower.StartsWith(TEXT("legacy/")))
		{
			return TEXT("legacy/ explicit migration artifacts only");
		}
		if (SurfaceLower.StartsWith(TEXT("validation/")))
		{
			return TEXT("validation/ diagnostics only");
		}
		return TEXT("debug/details_diagnostics.json");
	}

	bool IsDiagnosticsOnlyFieldName(const FString& FieldName)
	{
		return IsDiagnosticsOnlyKeyLower(FieldName.ToLower());
	}

	bool IsDiagnosticsOnlyRelativePath(const FString& RelativePath)
	{
		const FString PathLower = NormalizeDiagnosticsRelativePath(RelativePath);
		const FString CleanFileName = FPaths::GetCleanFilename(PathLower);
		return PathLower.StartsWith(TEXT("debug/"))
			|| PathLower.StartsWith(TEXT("validation/"))
			|| PathLower.StartsWith(TEXT("legacy/"))
			|| CleanFileName == TEXT("raw_properties.json")
			|| CleanFileName == TEXT("readonly_properties.json");
	}

	bool BuildDiagnosticsIsolationReport(
		const FDiagnosticsIsolationRequest& Request,
		TSharedPtr<FJsonObject>& OutReport,
		FString& OutError)
	{
		OutReport.Reset();
		OutError.Reset();

		TArray<TSharedPtr<FJsonValue>> Issues;
		TArray<TSharedPtr<FJsonValue>> Surfaces;
		ScanDiagnosticsIsolationObject(Request.EntryObject, FString(), Issues, Surfaces);

		FString FolderRoot = Request.FolderRoot;
		FolderRoot.TrimStartAndEndInline();
		if (!FolderRoot.IsEmpty())
		{
			const FString AbsoluteRoot = FPaths::ConvertRelativePathToFull(FolderRoot);
			AddDiagnosticsFileIssueIfPresent(
				AbsoluteRoot,
				TEXT("raw_properties.json"),
				TEXT("legacy_raw_properties_not_supported"),
				Issues,
				Surfaces);
			AddDiagnosticsFileIssueIfPresent(
				AbsoluteRoot,
				TEXT("readonly_properties.json"),
				TEXT("readonly_properties_not_authoring"),
				Issues,
				Surfaces);

			if (IFileManager::Get().DirectoryExists(*(AbsoluteRoot / TEXT("debug"))))
			{
				UeAgentJsonDiagnostics::AddIssue(
					Issues,
					TEXT("info"),
					TEXT("debug_directory_excluded_from_authoring"),
					TEXT("debug/"),
					TEXT("debug/ is diagnostics-only and is excluded from normal authoring/apply"));
				AddDiagnosticsSurfaceSummary(Surfaces, TEXT("debug/"), TEXT("debug/"), TEXT("debug/<diagnostic>.json"), TEXT("not consumed"), TEXT("excluded unless explicitly requested"));
			}
			if (IFileManager::Get().DirectoryExists(*(AbsoluteRoot / TEXT("validation"))))
			{
				UeAgentJsonDiagnostics::AddIssue(
					Issues,
					TEXT("info"),
					TEXT("validation_directory_excluded_from_authoring"),
					TEXT("validation/"),
					TEXT("validation/ is diagnostics-only and is excluded from normal authoring/apply"));
				AddDiagnosticsSurfaceSummary(Surfaces, TEXT("validation/"), TEXT("validation/"), TEXT("validation/<diagnostic>.json"), TEXT("not consumed"), TEXT("diagnostics only"));
			}
			if (IFileManager::Get().DirectoryExists(*(AbsoluteRoot / TEXT("legacy"))))
			{
				UeAgentJsonDiagnostics::AddIssue(
					Issues,
					TEXT("warning"),
					TEXT("legacy_directory_not_normal_authoring"),
					TEXT("legacy/"),
					TEXT("legacy/ is only for explicit migration artifacts and is not consumed by normal authoring/apply"));
				AddDiagnosticsSurfaceSummary(Surfaces, TEXT("legacy/"), TEXT("legacy/"), TEXT("legacy/ explicit migration artifacts only"), TEXT("not consumed"), TEXT("explicit migration only"));
			}
		}

		const bool bHasError = UeAgentJsonDiagnostics::HasErrorIssue(Issues);
		if (bHasError)
		{
			OutError = TEXT("diagnostics_isolation_failed");
		}

		FString AllowedTarget = Request.AllowedTarget;
		AllowedTarget.TrimStartAndEndInline();
		if (AllowedTarget.IsEmpty())
		{
			AllowedTarget = TEXT("details/index.hjson");
		}

		OutReport = MakeShared<FJsonObject>();
		OutReport->SetStringField(TEXT("schema"), TEXT("uai.details_diagnostics_policy.v1"));
		OutReport->SetBoolField(TEXT("ok"), !bHasError);
		OutReport->SetStringField(TEXT("status"), bHasError ? FString(TEXT("failed")) : FString(TEXT("passed")));
		OutReport->SetObjectField(TEXT("owner_ref"), BuildOwnerRef(Request.OwnerInfo));
		OutReport->SetStringField(TEXT("folder_root"), FolderRoot);
		OutReport->SetStringField(TEXT("allowed_target"), AllowedTarget);
		OutReport->SetBoolField(TEXT("apply_consumable"), false);
		OutReport->SetBoolField(TEXT("authoring_input_allowed"), false);
		OutReport->SetNumberField(TEXT("issue_count"), Issues.Num());
		OutReport->SetNumberField(TEXT("diagnostic_surface_count"), Surfaces.Num());
		OutReport->SetArrayField(TEXT("issues"), MoveTemp(Issues));
		OutReport->SetArrayField(TEXT("diagnostic_surfaces"), MoveTemp(Surfaces));
		return !bHasError;
	}

	TSharedPtr<FJsonObject> MakeAuthoringPropertiesRejection(
		const FString& ErrorCode,
		const FString& Surface,
		const FOwnerInfo& OwnerInfo,
		const FString& JsonPath,
		const FString& ResolutionHint,
		const FString& AllowedTarget)
	{
		TSharedPtr<FJsonObject> Rejection = MakeShared<FJsonObject>();
		Rejection->SetStringField(TEXT("schema"), TEXT("uai.authoring_properties_rejection.v1"));
		Rejection->SetBoolField(TEXT("ok"), false);
		Rejection->SetStringField(TEXT("error_code"), ErrorCode.IsEmpty() ? FString(TEXT("authoring_properties_rejected")) : ErrorCode);
		Rejection->SetStringField(TEXT("message"), FString::Printf(TEXT("Authoring properties surface was rejected: %s"), *Surface));
		Rejection->SetStringField(TEXT("surface"), Surface);
		Rejection->SetStringField(TEXT("owner_kind"), OwnerInfo.OwnerKind);
		Rejection->SetStringField(TEXT("owner_id"), OwnerInfo.OwnerId);
		Rejection->SetStringField(TEXT("json_path"), JsonPath);
		Rejection->SetStringField(TEXT("resolution_hint"), ResolutionHint.IsEmpty() ? ResolutionHintForAuthoringPropertiesError(ErrorCode) : ResolutionHint);
		Rejection->SetStringField(TEXT("allowed_target"), AllowedTarget);
		Rejection->SetBoolField(TEXT("dropped"), false);
		return Rejection;
	}

	bool FindAuthoringPropertiesIssue(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutSurface,
		FString& OutJsonPath,
		FString& OutErrorCode)
	{
		OutSurface.Reset();
		OutJsonPath.Reset();
		OutErrorCode.Reset();
		return FindAuthoringPropertiesIssueRecursive(Object, FString(), OutSurface, OutJsonPath, OutErrorCode);
	}

	bool FindAuthoringPropertiesFileIssue(
		const FString& FolderRoot,
		FString& OutSurface,
		FString& OutJsonPath,
		FString& OutErrorCode)
	{
		OutSurface.Reset();
		OutJsonPath.Reset();
		OutErrorCode.Reset();
		return FindAuthoringPropertiesFileIssueRecursive(FolderRoot, OutSurface, OutJsonPath, OutErrorCode);
	}

	bool BuildAuthoringPropertiesRejectionIfPresent(
		const TSharedPtr<FJsonObject>& Object,
		const FOwnerInfo& OwnerInfo,
		const FString& AllowedTarget,
		TSharedPtr<FJsonObject>& OutRejection)
	{
		OutRejection.Reset();
		FString Surface;
		FString JsonPath;
		FString ErrorCode;
		if (!FindAuthoringPropertiesIssue(Object, Surface, JsonPath, ErrorCode))
		{
			return false;
		}

		OutRejection = MakeAuthoringPropertiesRejection(
			ErrorCode,
			Surface,
			OwnerInfo,
			JsonPath,
			ResolutionHintForAuthoringPropertiesError(ErrorCode),
			AllowedTarget);
		return true;
	}

	bool BuildAuthoringPropertiesFileRejectionIfPresent(
		const FString& FolderRoot,
		const FOwnerInfo& OwnerInfo,
		const FString& AllowedTarget,
		TSharedPtr<FJsonObject>& OutRejection)
	{
		OutRejection.Reset();
		FString Surface;
		FString JsonPath;
		FString ErrorCode;
		if (!FindAuthoringPropertiesFileIssue(FolderRoot, Surface, JsonPath, ErrorCode))
		{
			return false;
		}

		OutRejection = MakeAuthoringPropertiesRejection(
			ErrorCode,
			Surface,
			OwnerInfo,
			JsonPath,
			ResolutionHintForAuthoringPropertiesError(ErrorCode),
			AllowedTarget);
		return true;
	}

	bool BuildAuthoringPropertiesFolderRejectionIfPresent(
		const FString& FolderRoot,
		const FOwnerInfo& OwnerInfo,
		const FString& AllowedTarget,
		TSharedPtr<FJsonObject>& OutRejection,
		FString& OutError)
	{
		OutRejection.Reset();
		OutError.Reset();

		TSharedPtr<FJsonObject> FileRejection;
		if (BuildAuthoringPropertiesFileRejectionIfPresent(FolderRoot, OwnerInfo, AllowedTarget, FileRejection))
		{
			OutRejection = FileRejection;
			if (!OutRejection.IsValid() || !OutRejection->TryGetStringField(TEXT("error_code"), OutError) || OutError.IsEmpty())
			{
				OutError = TEXT("authoring_properties_rejected");
			}
			return true;
		}

		FString Root = FolderRoot;
		Root.TrimStartAndEndInline();
		if (Root.IsEmpty())
		{
			return false;
		}

		const FString AbsoluteRoot = FPaths::ConvertRelativePathToFull(Root);
		TArray<FString> AuthoringFiles;
		IFileManager::Get().FindFilesRecursive(AuthoringFiles, *AbsoluteRoot, TEXT("*.json"), true, false, false);
		IFileManager::Get().FindFilesRecursive(AuthoringFiles, *AbsoluteRoot, TEXT("*.hjson"), true, false, false);
		AuthoringFiles.Sort();
		for (const FString& AuthoringFile : AuthoringFiles)
		{
			FString RelativeFile = MakePathRelativeToRoot(AuthoringFile, AbsoluteRoot);
			if (IsDiagnosticsOrMigrationRelativePath(RelativeFile))
			{
				continue;
			}

			TSharedPtr<FJsonObject> JsonObject;
			FString LoadError;
			const bool bLoaded = IsHjsonFilePath(AuthoringFile)
				? LoadDetailsObjectFile(AuthoringFile, JsonObject, LoadError)
				: LoadLegacyJsonObjectFile(AuthoringFile, JsonObject);
			if (!bLoaded || !JsonObject.IsValid())
			{
				continue;
			}

			FString Surface;
			FString JsonPath;
			FString ErrorCode;
			if (!FindAuthoringPropertiesIssue(JsonObject, Surface, JsonPath, ErrorCode))
			{
				continue;
			}

			RelativeFile = NormalizeJsonRelativePath(RelativeFile);
			const FString FullJsonPath = JsonPath.IsEmpty()
				? RelativeFile
				: FString::Printf(TEXT("%s.%s"), *RelativeFile, *JsonPath);
			OutRejection = MakeAuthoringPropertiesRejection(
				ErrorCode,
				Surface,
				OwnerInfo,
				FullJsonPath,
				ResolutionHintForAuthoringPropertiesError(ErrorCode),
				AllowedTarget);
			if (!OutRejection.IsValid() || !OutRejection->TryGetStringField(TEXT("error_code"), OutError) || OutError.IsEmpty())
			{
				OutError = TEXT("authoring_properties_rejected");
			}
			return true;
		}

		return false;
	}

	bool ResolveDetailsRefRoot(
		const FString& EntryDirectory,
		const TSharedPtr<FJsonObject>& EntryObject,
		const FString& RefFieldName,
		bool& bOutHasDetails,
		FString& OutDetailsRoot,
		FString& OutIndexFile,
		FString& OutError)
	{
		bOutHasDetails = false;
		OutDetailsRoot.Reset();
		OutIndexFile.Reset();
		OutError.Reset();
		if (!EntryObject.IsValid())
		{
			return true;
		}

		const FString NormalizedRefField = NormalizeDetailsRefFieldName(RefFieldName);
		const TSharedPtr<FJsonObject>* DetailsRef = nullptr;
		if (!EntryObject->TryGetObjectField(NormalizedRefField, DetailsRef) || !DetailsRef || !DetailsRef->IsValid())
		{
			return true;
		}

		FString IndexFile;
		if (!(*DetailsRef)->TryGetStringField(TEXT("index_file"), IndexFile) || IndexFile.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("details_ref_missing_index_file");
			return false;
		}

		OutIndexFile = NormalizeDetailsIndexFile(IndexFile);
		UeAgentAuthoringWorkspacePaths::FResolveOptions ResolveOptions;
		ResolveOptions.bRequireHjson = true;
		UeAgentAuthoringWorkspacePaths::FResolvedPath ResolvedIndex;
		if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
			EntryDirectory,
			OutIndexFile,
			ResolveOptions,
			ResolvedIndex,
			OutError))
		{
			return false;
		}
		OutIndexFile = ResolvedIndex.RelativePath;
		OutDetailsRoot = FPaths::GetPath(ResolvedIndex.AbsolutePath);
		double FieldCount = -1.0;
		const FString& ResolvedIndexPath = ResolvedIndex.AbsolutePath;
		if ((*DetailsRef)->TryGetNumberField(TEXT("field_count"), FieldCount)
			&& FieldCount <= 0.0
			&& !IFileManager::Get().FileExists(*ResolvedIndexPath))
		{
			OutDetailsRoot.Reset();
			OutIndexFile.Reset();
			return true;
		}
		bOutHasDetails = true;
		return true;
	}

	static bool PrepareDetailsRootForWrite(
		const FString& DetailsRoot,
		const FString& IndexFileName,
		const FOwnerInfo& OwnerInfo,
		FString& OutError)
	{
		UeAgentAuthoringWorkspacePaths::FResolveOptions ResolveOptions;
		UeAgentAuthoringWorkspacePaths::FResolvedPath ResolvedRoot;
		if (!UeAgentAuthoringWorkspacePaths::ResolveProjectPath(
			DetailsRoot,
			ResolveOptions,
			ResolvedRoot,
			OutError))
		{
			return false;
		}
		const FString& AbsoluteRoot = ResolvedRoot.AbsolutePath;

		const FString ExistingIndexPath = AbsoluteRoot / IndexFileName;
		if (IFileManager::Get().FileExists(*ExistingIndexPath))
		{
			TSharedPtr<FJsonObject> ExistingIndex;
			if (!LoadDetailsObjectFile(ExistingIndexPath, ExistingIndex, OutError))
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* ExistingOwnerRef = nullptr;
			FString ExistingOwnerKind;
			FString ExistingOwnerId;
			FString ExistingOwnerPath;
			if (!ExistingIndex->TryGetObjectField(TEXT("owner_ref"), ExistingOwnerRef)
				|| !ExistingOwnerRef
				|| !ExistingOwnerRef->IsValid()
				|| !(*ExistingOwnerRef)->TryGetStringField(TEXT("owner_kind"), ExistingOwnerKind)
				|| !(*ExistingOwnerRef)->TryGetStringField(TEXT("owner_id"), ExistingOwnerId))
			{
				OutError = FString::Printf(TEXT("details_existing_owner_ref_invalid:%s"), *ExistingIndexPath);
				return false;
			}
			(*ExistingOwnerRef)->TryGetStringField(TEXT("object_path"), ExistingOwnerPath);
			const bool bOwnerPathConflict = !ExistingOwnerPath.IsEmpty()
				&& !OwnerInfo.OwnerPath.IsEmpty()
				&& ExistingOwnerPath != OwnerInfo.OwnerPath;
			if (ExistingOwnerKind != OwnerInfo.OwnerKind
				|| ExistingOwnerId != OwnerInfo.OwnerId
				|| bOwnerPathConflict)
			{
				OutError = FString::Printf(
					TEXT("details_owner_scope_collision:%s:%s:%s:%s:%s"),
					*ExistingIndexPath,
					*ExistingOwnerKind,
					*ExistingOwnerId,
					*OwnerInfo.OwnerKind,
					*OwnerInfo.OwnerId);
				return false;
			}
		}

		if (IFileManager::Get().DirectoryExists(*AbsoluteRoot)
			&& !IFileManager::Get().DeleteDirectory(*AbsoluteRoot, false, true))
		{
			OutError = FString::Printf(TEXT("details_directory_reset_failed:%s"), *AbsoluteRoot);
			return false;
		}
		if (!IFileManager::Get().MakeDirectory(*AbsoluteRoot, true))
		{
			OutError = FString::Printf(TEXT("details_directory_create_failed:%s"), *AbsoluteRoot);
			return false;
		}
		return true;
	}

	bool WriteGroupedDetails(
		const FString& DetailsRoot,
		const FOwnerInfo& OwnerInfo,
		const TSharedPtr<FJsonObject>& PropertiesJson,
		TSharedPtr<FJsonObject>& OutDetailsSummary,
		FString& OutError,
		const FString& IndexFile)
	{
		OutDetailsSummary.Reset();
		if (!PropertiesJson.IsValid())
		{
			OutError = TEXT("properties_json_invalid");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!PropertiesJson->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			OutError = TEXT("properties_array_missing");
			return false;
		}

		const FString NormalizedIndexFile = NormalizeDetailsIndexFile(IndexFile);
		if (!IsHjsonFilePath(NormalizedIndexFile))
		{
			OutError = FString::Printf(TEXT("hjson_file_required:%s"), *NormalizedIndexFile);
			return false;
		}
		const FString IndexFileName = GetDetailsIndexFileName(NormalizedIndexFile);
		if (!PrepareDetailsRootForWrite(DetailsRoot, IndexFileName, OwnerInfo, OutError))
		{
			return false;
		}

		TArray<FDetailsGroup> Groups;
		TMap<FString, int32> GroupIndexByKey;
		TSet<FString> UsedSlugs;
		TSet<FString> PropertyPaths;
		const FString DetailsFileExtension = GetDetailsFileExtensionForIndex(NormalizedIndexFile);

		for (int32 Index = 0; Index < Properties->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> SourceField = (*Properties)[Index].IsValid() ? (*Properties)[Index]->AsObject() : nullptr;
			if (!SourceField.IsValid())
			{
				OutError = FString::Printf(TEXT("invalid_property_descriptor:%d"), Index);
				return false;
			}

			const FString PropertyPath = GetPropertyPath(SourceField);
			if (PropertyPath.IsEmpty())
			{
				OutError = FString::Printf(TEXT("missing_property_path:%d"), Index);
				return false;
			}
			if (PropertyPaths.Contains(PropertyPath))
			{
				OutError = FString::Printf(TEXT("duplicate_property_path:%s"), *PropertyPath);
				return false;
			}
			PropertyPaths.Add(PropertyPath);

			FString GroupKey;
			FString DisplayName;
			FString CategorySource;
			TArray<TSharedPtr<FJsonValue>> CategoryPathValues;
			ResolveCategory(SourceField, GroupKey, DisplayName, CategorySource, CategoryPathValues);

			int32* ExistingIndex = GroupIndexByKey.Find(GroupKey);
			if (!ExistingIndex)
			{
				FDetailsGroup Group;
				Group.GroupKey = GroupKey;
				Group.DisplayName = DisplayName;
				Group.CategorySource = CategorySource;
				Group.CategoryPathValues = CloneJsonArray(CategoryPathValues);

				const FString BaseSlug = SafeFileName(GroupKey);
				FString Slug = BaseSlug;
				int32 Suffix = 2;
				while (UsedSlugs.Contains(Slug.ToLower()))
				{
					Slug = FString::Printf(TEXT("%s_%d"), *BaseSlug, Suffix++);
				}
				UsedSlugs.Add(Slug.ToLower());
				Group.Slug = Slug;
				Group.FileName = Slug + TEXT(".") + DetailsFileExtension;

				const int32 NewIndex = Groups.Add(MoveTemp(Group));
				GroupIndexByKey.Add(GroupKey, NewIndex);
				ExistingIndex = GroupIndexByKey.Find(GroupKey);
			}

			FDetailsGroup& Group = Groups[*ExistingIndex];
			TSharedPtr<FJsonObject> Field = MakeCompactField(SourceField);
			if (!ExternalizeCandidatesIfNeeded(DetailsRoot, OwnerInfo, Group.Slug, DetailsFileExtension, Field, OutError))
			{
				return false;
			}
			if (IsEditableField(Field))
			{
				++Group.EditableFieldCount;
			}
			Group.Fields.Add(MakeShared<FJsonValueObject>(Field));
		}

		TArray<TSharedPtr<FJsonValue>> GroupEntries;
		GroupEntries.Reserve(Groups.Num());
		int32 FieldCount = 0;
		int32 EditableFieldCount = 0;
		for (const FDetailsGroup& Group : Groups)
		{
			if (!SaveDetailsObjectFile(DetailsRoot / Group.FileName, MakeGroupFile(OwnerInfo, Group), OutError))
			{
				return false;
			}
			FieldCount += Group.Fields.Num();
			EditableFieldCount += Group.EditableFieldCount;
			GroupEntries.Add(MakeShared<FJsonValueObject>(MakeGroupIndexEntry(Group)));
		}

		FString SourceSchema;
		PropertiesJson->TryGetStringField(TEXT("schema"), SourceSchema);
		FString Profile;
		PropertiesJson->TryGetStringField(TEXT("profile"), Profile);

		TSharedPtr<FJsonObject> Index = MakeShared<FJsonObject>();
		Index->SetStringField(TEXT("schema"), DetailsIndexSchema);
		Index->SetObjectField(TEXT("owner_ref"), BuildOwnerRef(OwnerInfo));
		Index->SetStringField(TEXT("source_schema"), SourceSchema.IsEmpty() ? FString(PropertiesSchema) : SourceSchema);
		Index->SetStringField(TEXT("profile"), Profile.IsEmpty() ? FString(TEXT("authoring")) : Profile);
		Index->SetStringField(TEXT("status"), TEXT("complete"));
		Index->SetNumberField(TEXT("group_count"), Groups.Num());
		Index->SetNumberField(TEXT("field_count"), FieldCount);
		Index->SetNumberField(TEXT("editable_field_count"), EditableFieldCount);
		Index->SetBoolField(TEXT("field_details_inlined"), false);
		Index->SetArrayField(TEXT("groups"), CloneJsonArray(GroupEntries));
		if (!SaveDetailsObjectFile(DetailsRoot / IndexFileName, Index, OutError))
		{
			return false;
		}

		OutDetailsSummary = MakeShared<FJsonObject>();
		OutDetailsSummary->SetStringField(TEXT("schema"), DetailsRefSchema);
		OutDetailsSummary->SetStringField(TEXT("index_file"), NormalizedIndexFile);
		OutDetailsSummary->SetStringField(TEXT("owner_kind"), OwnerInfo.OwnerKind);
		OutDetailsSummary->SetStringField(TEXT("owner_id"), OwnerInfo.OwnerId);
		OutDetailsSummary->SetStringField(TEXT("status"), TEXT("complete"));
		OutDetailsSummary->SetNumberField(TEXT("group_count"), Groups.Num());
		OutDetailsSummary->SetNumberField(TEXT("field_count"), FieldCount);
		OutDetailsSummary->SetNumberField(TEXT("editable_field_count"), EditableFieldCount);
		OutDetailsSummary->SetBoolField(TEXT("field_details_inlined"), false);
		OutDetailsSummary->SetArrayField(TEXT("groups"), MoveTemp(GroupEntries));
		return true;
	}

	bool LoadGroupedDetails(
		const FString& DetailsRoot,
		TSharedPtr<FJsonObject>& OutPropertiesJson,
		FString& OutError,
		const FString& IndexFile)
	{
		OutPropertiesJson.Reset();
		const FString NormalizedIndexFile = NormalizeDetailsIndexFile(IndexFile);
		if (!IsHjsonFilePath(NormalizedIndexFile))
		{
			OutError = FString::Printf(TEXT("hjson_file_required:%s"), *NormalizedIndexFile);
			return false;
		}
		FString IndexPath;
		if (!ResolveDetailsChildPath(
			DetailsRoot,
			GetDetailsIndexFileName(NormalizedIndexFile),
			IndexPath,
			OutError))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Index;
		if (!LoadDetailsObjectFile(IndexPath, Index, OutError))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
		if (!Index->TryGetArrayField(TEXT("groups"), Groups) || !Groups)
		{
			OutError = TEXT("details_index_missing_groups");
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> Properties;
		TSet<FString> PropertyPaths;
		for (int32 GroupIndex = 0; GroupIndex < Groups->Num(); ++GroupIndex)
		{
			const TSharedPtr<FJsonObject> GroupEntry = (*Groups)[GroupIndex].IsValid() ? (*Groups)[GroupIndex]->AsObject() : nullptr;
			FString FileName;
			if (!GroupEntry.IsValid() || !GroupEntry->TryGetStringField(TEXT("file"), FileName) || FileName.IsEmpty())
			{
				OutError = FString::Printf(TEXT("details_index_invalid_group:%d"), GroupIndex);
				return false;
			}

			FString GroupPath;
			if (!ResolveDetailsChildPath(DetailsRoot, FileName, GroupPath, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> GroupJson;
			if (!LoadDetailsObjectFile(GroupPath, GroupJson, OutError))
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
			if (!GroupJson->TryGetArrayField(TEXT("fields"), Fields) || !Fields)
			{
				OutError = FString::Printf(TEXT("details_group_missing_fields:%s"), *FileName);
				return false;
			}
			for (int32 FieldIndex = 0; FieldIndex < Fields->Num(); ++FieldIndex)
			{
				const TSharedPtr<FJsonObject> Field = (*Fields)[FieldIndex].IsValid() ? (*Fields)[FieldIndex]->AsObject() : nullptr;
				const FString PropertyPath = GetPropertyPath(Field);
				if (PropertyPath.IsEmpty())
				{
					OutError = FString::Printf(TEXT("details_field_missing_property_path:%s:%d"), *FileName, FieldIndex);
					return false;
				}
				if (PropertyPaths.Contains(PropertyPath))
				{
					OutError = FString::Printf(TEXT("details_duplicate_property_path:%s"), *PropertyPath);
					return false;
				}
				PropertyPaths.Add(PropertyPath);
				Properties.Add(CloneJsonValue((*Fields)[FieldIndex]));
			}
		}

		FString Profile;
		Index->TryGetStringField(TEXT("profile"), Profile);
		const TSharedPtr<FJsonObject>* OwnerRef = nullptr;

		OutPropertiesJson = MakeShared<FJsonObject>();
		OutPropertiesJson->SetStringField(TEXT("schema"), PropertiesSchema);
		OutPropertiesJson->SetStringField(TEXT("profile"), Profile.IsEmpty() ? FString(TEXT("authoring")) : Profile);
		if (Index->TryGetObjectField(TEXT("owner_ref"), OwnerRef) && OwnerRef && OwnerRef->IsValid())
		{
			OutPropertiesJson->SetObjectField(TEXT("object"), CloneJsonObject(*OwnerRef));
		}
		OutPropertiesJson->SetNumberField(TEXT("property_count"), Properties.Num());
		OutPropertiesJson->SetBoolField(TEXT("truncated"), false);
		OutPropertiesJson->SetArrayField(TEXT("properties"), MoveTemp(Properties));
		return true;
	}

	bool HasGroupedDetails(const FString& DetailsRoot, const FString& IndexFile)
	{
		const FString NormalizedIndexFile = NormalizeDetailsIndexFile(IndexFile);
		FString IndexPath;
		FString ResolveError;
		return IsHjsonFilePath(NormalizedIndexFile)
			&& ResolveDetailsChildPath(
				DetailsRoot,
				GetDetailsIndexFileName(NormalizedIndexFile),
				IndexPath,
				ResolveError,
				true);
	}

	bool ExternalizeInlineDetailsInObjectTree(
		const FString& EntryFilePath,
		const TSharedPtr<FJsonObject>& EntryObject,
		const FString& SourceCommand,
		FString& OutError,
		bool bScopeToEntryFile)
	{
		if (!EntryObject.IsValid())
		{
			OutError = TEXT("entry_object_invalid");
			return false;
		}

		TSet<FString> UsedDetailsRoots;
		const FString EntryDetailsScope = bScopeToEntryFile
			? SafeFileName(FPaths::GetBaseFilename(EntryFilePath))
			: FString();
		return ExternalizeInlineDetailsRecursive(
			FPaths::GetPath(EntryFilePath),
			MakeDetailsIndexFileForEntryFile(EntryFilePath),
			EntryDetailsScope,
			EntryObject,
			SourceCommand,
			FString(),
			UsedDetailsRoots,
			OutError);
	}

	namespace
	{
		void RemoveDiagnosticAuthoringFieldsIterative(
			const TSharedPtr<FJsonObject>& Object)
		{
			if (!Object.IsValid())
			{
				return;
			}

			TArray<TSharedPtr<FJsonValue>> Pending;
			Pending.Add(MakeShared<FJsonValueObject>(Object));
			TSet<const FJsonObject*> VisitedObjects;
			TSet<const FJsonValue*> VisitedArrays;
			while (!Pending.IsEmpty())
			{
				const TSharedPtr<FJsonValue> Value =
					Pending.Pop(EAllowShrinking::No);
				if (!Value.IsValid())
				{
					continue;
				}

				if (Value->Type == EJson::Array)
				{
					if (VisitedArrays.Contains(Value.Get()))
					{
						continue;
					}
					VisitedArrays.Add(Value.Get());
					for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
					{
						Pending.Add(Item);
					}
					continue;
				}

				if (Value->Type != EJson::Object)
				{
					continue;
				}

				const TSharedPtr<FJsonObject> CurrentObject = Value->AsObject();
				if (!CurrentObject.IsValid()
					|| VisitedObjects.Contains(CurrentObject.Get()))
				{
					continue;
				}
				VisitedObjects.Add(CurrentObject.Get());
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
					CurrentObject->Values)
				{
					Pending.Add(Pair.Value);
				}

				CurrentObject->RemoveField(TEXT("raw_properties"));
				CurrentObject->RemoveField(TEXT("readonly_properties"));
				CurrentObject->RemoveField(TEXT("property_summary"));
				CurrentObject->RemoveField(TEXT("raw_property_summary"));
				CurrentObject->RemoveField(TEXT("raw_properties_count"));
				CurrentObject->RemoveField(TEXT("readonly_properties_count"));
			}
		}
	}

	void RemoveDiagnosticAuthoringFieldsInObjectTree(const TSharedPtr<FJsonObject>& EntryObject)
	{
		RemoveDiagnosticAuthoringFieldsIterative(EntryObject);
	}

	bool RehydrateGroupedDetailsInObjectTree(
		const FString& EntryFilePath,
		const TSharedPtr<FJsonObject>& EntryObject,
		FString& OutError)
	{
		if (!EntryObject.IsValid())
		{
			OutError = TEXT("entry_object_invalid");
			return false;
		}

		return RehydrateGroupedDetailsRecursive(FPaths::GetPath(EntryFilePath), EntryObject, OutError);
	}
}
