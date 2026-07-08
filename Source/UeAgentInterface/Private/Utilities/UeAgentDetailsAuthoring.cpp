#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
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
	FOwnerDetailsExportRequest::FOwnerDetailsExportRequest()
		: Object(nullptr)
		, IndexFile(TEXT("details/index.json"))
		, RefFieldName(TEXT("details"))
	{
	}

	FOwnerDetailsApplyRequest::FOwnerDetailsApplyRequest()
		: Object(nullptr)
		, IndexFile(TEXT("details/index.json"))
		, RefFieldName(TEXT("details"))
	{
	}

	FOwnerDetailsValidateRequest::FOwnerDetailsValidateRequest()
		: IndexFile(TEXT("details/index.json"))
		, RefFieldName(TEXT("details"))
	{
	}

	FOwnerDetailsDiffRequest::FOwnerDetailsDiffRequest()
		: OldIndexFile(TEXT("details/index.json"))
		, NewIndexFile(TEXT("details/index.json"))
		, RefFieldName(TEXT("details"))
	{
	}

	FDiagnosticsIsolationRequest::FDiagnosticsIsolationRequest()
		: AllowedTarget(TEXT("details/index.json"))
	{
	}

	namespace
	{
		constexpr const TCHAR* DetailsRefSchema = TEXT("uai.details_ref.v1");
		constexpr const TCHAR* DetailsIndexSchema = TEXT("uai.details_index.v1");
		constexpr const TCHAR* DetailsGroupSchema = TEXT("uai.details_group.v1");
		constexpr const TCHAR* DetailsCandidatesSchema = TEXT("uai.details_candidates.v1");
		constexpr const TCHAR* PropertiesSchema = TEXT("uai.uobject_properties.v1");
		constexpr int32 MaxInlineCandidateCount = 64;
		constexpr int32 MaxInlineCandidateJsonLength = 6000;

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

		TSharedPtr<FJsonValue> CloneJsonValue(const TSharedPtr<FJsonValue>& Value);

		TSharedPtr<FJsonObject> CloneJsonObject(const TSharedPtr<FJsonObject>& Object, const TSet<FString>* OmittedFields = nullptr)
		{
			TSharedPtr<FJsonObject> Clone = MakeShared<FJsonObject>();
			if (!Object.IsValid())
			{
				return Clone;
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				if (OmittedFields && OmittedFields->Contains(Pair.Key))
				{
					continue;
				}
				Clone->SetField(Pair.Key, CloneJsonValue(Pair.Value));
			}
			return Clone;
		}

		TSharedPtr<FJsonValue> CloneJsonValue(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid() || Value->IsNull())
			{
				return MakeShared<FJsonValueNull>();
			}

			switch (Value->Type)
			{
			case EJson::String:
				return MakeShared<FJsonValueString>(Value->AsString());
			case EJson::Number:
				return MakeShared<FJsonValueNumber>(Value->AsNumber());
			case EJson::Boolean:
				return MakeShared<FJsonValueBoolean>(Value->AsBool());
			case EJson::Array:
			{
				TArray<TSharedPtr<FJsonValue>> ClonedValues;
				for (const TSharedPtr<FJsonValue>& Child : Value->AsArray())
				{
					ClonedValues.Add(CloneJsonValue(Child));
				}
				return MakeShared<FJsonValueArray>(ClonedValues);
			}
			case EJson::Object:
				return MakeShared<FJsonValueObject>(CloneJsonObject(Value->AsObject()));
			default:
				return MakeShared<FJsonValueNull>();
			}
		}

		TArray<TSharedPtr<FJsonValue>> CloneJsonArray(const TArray<TSharedPtr<FJsonValue>>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> ClonedValues;
			ClonedValues.Reserve(Values.Num());
			for (const TSharedPtr<FJsonValue>& Value : Values)
			{
				ClonedValues.Add(CloneJsonValue(Value));
			}
			return ClonedValues;
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
			if (Field->TryGetStringField(TEXT("property_name"), PropertyPath) && !PropertyPath.IsEmpty())
			{
				return PropertyPath;
			}
			if (Field->TryGetStringField(TEXT("name"), PropertyPath) && !PropertyPath.IsEmpty())
			{
				return PropertyPath;
			}
			const TSharedPtr<FJsonObject>* Selector = nullptr;
			if (Field->TryGetObjectField(TEXT("selector"), Selector) && Selector && Selector->IsValid())
			{
				if ((*Selector)->TryGetStringField(TEXT("property_path"), PropertyPath) && !PropertyPath.IsEmpty())
				{
					return PropertyPath;
				}
				if ((*Selector)->TryGetStringField(TEXT("property_name"), PropertyPath) && !PropertyPath.IsEmpty())
				{
					return PropertyPath;
				}
				if ((*Selector)->TryGetStringField(TEXT("name"), PropertyPath) && !PropertyPath.IsEmpty())
				{
					return PropertyPath;
				}
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
			const FString CandidateFileName = FString::Printf(TEXT("candidates/%s_%s.json"), *SafeFileName(GroupSlug), *PropertySlug);
			TSharedPtr<FJsonObject> CandidateJson = MakeShared<FJsonObject>();
			CandidateJson->SetStringField(TEXT("schema"), DetailsCandidatesSchema);
			CandidateJson->SetObjectField(TEXT("owner_ref"), BuildOwnerRef(OwnerInfo));
			CandidateJson->SetStringField(TEXT("property_path"), GetPropertyPath(Field));
			CandidateJson->SetNumberField(TEXT("candidate_count"), CandidateCount);
			CandidateJson->SetArrayField(TEXT("candidates"), CloneJsonArray(*Candidates));
			if (!FUeAgentStructuredJsonFormatter::SaveObjectToFile(DetailsRoot / CandidateFileName, CandidateJson, &OutError))
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
			Entry->SetArrayField(TEXT("category_path"), CloneJsonArray(Group.CategoryPathValues));
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
			Object->SetArrayField(TEXT("category_path"), CloneJsonArray(Group.CategoryPathValues));
			Object->SetStringField(TEXT("category_source"), Group.CategorySource);
			Object->SetNumberField(TEXT("field_count"), Group.Fields.Num());
			Object->SetNumberField(TEXT("editable_field_count"), Group.EditableFieldCount);
			Object->SetArrayField(TEXT("fields"), CloneJsonArray(Group.Fields));
			return Object;
		}

		TSet<FString> RedundantFieldNames()
		{
			TSet<FString> Fields;
			return Fields;
		}

		TSharedPtr<FJsonObject> MakeCompactField(const TSharedPtr<FJsonObject>& Field)
		{
			static const TSet<FString> OmittedFields = RedundantFieldNames();
			return CloneJsonObject(Field, &OmittedFields);
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

			return
				Field->HasField(TEXT("property_path")) ||
				Field->HasField(TEXT("property_name")) ||
				Field->HasField(TEXT("display_name")) ||
				Field->HasField(TEXT("category_path")) ||
				Field->HasField(TEXT("control_type")) ||
				Field->HasField(TEXT("value_kind")) ||
				Field->HasField(TEXT("value_text")) ||
				Field->HasField(TEXT("default_value_text")) ||
				Field->HasField(TEXT("availability")) ||
				Field->HasField(TEXT("operations")) ||
				Field->HasField(TEXT("tooltip")) ||
				Field->HasField(TEXT("candidates"));
		}

		bool IsDetailsPropertyArray(const TArray<TSharedPtr<FJsonValue>>& Properties)
		{
			if (Properties.Num() == 0)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& PropertyValue : Properties)
			{
				if (!PropertyValue.IsValid() || PropertyValue->Type != EJson::Object)
				{
					return false;
				}
				if (!IsDetailsPropertyDescriptor(PropertyValue->AsObject()))
				{
					return false;
				}
			}
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
				Normalized = TEXT("details/index.json");
			}
			return NormalizeJsonRelativePath(Normalized);
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
				Resolved.OwnerClassPath = UeAgentUObjectProperties::GetClassPath(Object->GetClass());
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
			const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
			return Object->TryGetObjectField(TEXT("selector"), Selector)
				&& Selector
				&& Selector->IsValid()
				&& Object->TryGetArrayField(TEXT("operations"), Operations)
				&& Operations;
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

		bool FindLegacyPropertiesSurfaceRecursive(
			const TSharedPtr<FJsonObject>& Object,
			const FString& JsonPath,
			FString& OutLegacySurface,
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
				if (KeyLower == TEXT("properties") && JsonValueIsArray(Pair.Value))
				{
					OutLegacySurface = TEXT("properties[]");
					OutJsonPath = FieldPath;
					OutErrorCode = TEXT("legacy_inline_properties_not_supported");
					return true;
				}
				if (KeyLower == TEXT("settings_properties") && JsonValueIsArray(Pair.Value))
				{
					OutLegacySurface = TEXT("settings_properties[]");
					OutJsonPath = FieldPath;
					OutErrorCode = TEXT("legacy_settings_properties_not_supported");
					return true;
				}
				if (JsonValueIsArray(Pair.Value) && IsDiagnosticsOnlyKeyLower(KeyLower))
				{
					continue;
				}
				if (KeyLower.EndsWith(TEXT("_properties"), ESearchCase::CaseSensitive) && JsonValueIsArray(Pair.Value) && IsAuthoringMetadataObject(Object))
				{
					continue;
				}
				if (KeyLower.EndsWith(TEXT("_properties"), ESearchCase::CaseSensitive) && JsonValueIsArray(Pair.Value))
				{
					OutLegacySurface = Pair.Key + TEXT("[]");
					OutJsonPath = FieldPath;
					OutErrorCode = TEXT("legacy_inline_properties_not_supported");
					return true;
				}
				const TSharedPtr<FJsonObject>* ChildObject = nullptr;
				if (Pair.Value.IsValid() && Pair.Value->TryGetObject(ChildObject) && ChildObject && ChildObject->IsValid())
				{
					if (FindLegacyPropertiesSurfaceRecursive(*ChildObject, FieldPath, OutLegacySurface, OutJsonPath, OutErrorCode))
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
						if (FindLegacyPropertiesSurfaceRecursive(*ArrayObject, ArrayPath, OutLegacySurface, OutJsonPath, OutErrorCode))
						{
							return true;
						}
					}
				}
			}
			return false;
		}

		FString MigrationHintForLegacyError(const FString& ErrorCode)
		{
			if (ErrorCode == TEXT("legacy_settings_properties_not_supported"))
			{
				return TEXT("Use settings_details.index_file and edit grouped fields");
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
			return TEXT("Move editable fields into the referenced grouped Details owner");
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

		bool FindLegacyPropertiesFileRecursive(
			const FString& FolderRoot,
			FString& OutLegacySurface,
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
				const TCHAR* LegacySurface;
				const TCHAR* ErrorCode;
			};

			static const FLegacyFileRule Rules[] = {
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

					OutLegacySurface = Rule.LegacySurface;
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
				if (!IFileManager::Get().FileExists(*(DetailsRoot / CandidatesRef)))
				{
					AddValidationIssue(Issues, TEXT("error"), TEXT("details_field_candidates_ref_missing"), FieldPath / TEXT("candidates_ref"), TEXT("candidates_ref file does not exist"));
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
			if (!Field->TryGetArrayField(TEXT("operations"), Operations) || !Operations)
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("details_field_operations_missing"), FieldPath / TEXT("operations"), TEXT("operations array is required"));
			}
			else
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
			FString& OutError)
		{
			OutFields.Reset();
			FString NormalizedRoot = DetailsRoot;
			NormalizedRoot.TrimStartAndEndInline();
			if (NormalizedRoot.IsEmpty())
			{
				OutError = TEXT("details_root_missing");
				return false;
			}

			TSharedPtr<FJsonObject> Index;
			if (!LoadJsonObjectFile(NormalizedRoot / TEXT("index.json"), Index, OutError))
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

				TSharedPtr<FJsonObject> GroupJson;
				if (!LoadJsonObjectFile(NormalizedRoot / FileName, GroupJson, OutError))
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
				if (!Object->TryGetArrayField(Key, Properties) || !Properties || !IsDetailsPropertyArray(*Properties))
				{
					continue;
				}

				FString Profile;
				Object->TryGetStringField(TEXT("profile"), Profile);
				TSharedPtr<FJsonObject> DetailsSummary;
				const FString DetailsPath = (Key == TEXT("properties"))
					? JsonPath
					: (JsonPath.IsEmpty() ? Key : FString::Printf(TEXT("%s.%s"), *JsonPath, *Key));
				const FString DetailsRelativeRoot = MakeDetailsRelativeRoot(Object, DetailsPath, UsedDetailsRoots);
				const FString DetailsRoot = EntryDirectory / DetailsRelativeRoot;
				if (!WriteGroupedDetails(
					DetailsRoot,
					InferOwnerInfo(Object, SourceCommand, DetailsPath),
					BuildPropertiesContainer(*Properties, Profile),
					DetailsSummary,
					OutError))
				{
					return false;
				}

				DetailsSummary->SetStringField(TEXT("index_file"), NormalizeJsonRelativePath(DetailsRelativeRoot / TEXT("index.json")));
				Object->RemoveField(Key);
				Object->SetObjectField(MakeDetailsFieldName(Key), DetailsSummary);
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
					if (!ExternalizeInlineDetailsRecursive(EntryDirectory, (*ValuePtr)->AsObject(), SourceCommand, ChildPath, UsedDetailsRoots, OutError))
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
							if (!ExternalizeInlineDetailsRecursive(EntryDirectory, Array[Index]->AsObject(), SourceCommand, ArrayPath, UsedDetailsRoots, OutError))
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
					if (!LoadGroupedDetails(DetailsRoot, PropertiesJson, OutError))
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

	TSharedPtr<FJsonObject> BuildOwnerRef(const FOwnerInfo& OwnerInfo)
	{
		TSharedPtr<FJsonObject> OwnerRef = MakeShared<FJsonObject>();
		OwnerRef->SetStringField(TEXT("owner_id"), OwnerInfo.OwnerId);
		OwnerRef->SetStringField(TEXT("owner_kind"), OwnerInfo.OwnerKind);
		OwnerRef->SetStringField(TEXT("name"), OwnerInfo.OwnerName);
		OwnerRef->SetStringField(TEXT("object_path"), OwnerInfo.OwnerPath);
		OwnerRef->SetStringField(TEXT("class_path"), OwnerInfo.OwnerClassPath);
		if (!OwnerInfo.SourceCommand.IsEmpty())
		{
			OwnerRef->SetStringField(TEXT("source_command"), OwnerInfo.SourceCommand);
		}
		return OwnerRef;
	}

	FOwnerInfo MakeOwnerInfoForObject(
		UObject* Object,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& SourceCommand,
		const FString& OwnerName)
	{
		FOwnerInfo OwnerInfo;
		OwnerInfo.OwnerKind = OwnerKind;
		OwnerInfo.OwnerId = OwnerId;
		OwnerInfo.OwnerName = OwnerName;
		OwnerInfo.SourceCommand = SourceCommand;
		return ResolveOwnerInfoForObject(OwnerInfo, Object);
	}

	FOwnerDetailsExportRequest MakeOwnerDetailsExportRequest(
		UObject* Object,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& DetailsRoot,
		const FString& SourceCommand,
		const FString& IndexFile,
		const FString& RefFieldName)
	{
		FOwnerDetailsExportRequest Request;
		Request.Object = Object;
		Request.OwnerInfo = MakeOwnerInfoForObject(Object, OwnerKind, OwnerId, SourceCommand);
		Request.DetailsRoot = DetailsRoot;
		Request.IndexFile = NormalizeDetailsIndexFile(IndexFile);
		Request.RefFieldName = NormalizeDetailsRefFieldName(RefFieldName);
		return Request;
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

		TSharedPtr<FJsonObject> PropertiesJson = UeAgentUObjectProperties::ExportObjectProperties(Request.Object, Request.PropertyOptions);
		if (!WriteGroupedDetails(DetailsRoot, OwnerInfo, PropertiesJson, OutDetailsSummary, OutError))
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
		if (!LoadGroupedDetails(DetailsRoot, PropertiesJson, OutError))
		{
			return false;
		}

		TSharedPtr<FJsonObject> PropertyApply = UeAgentUObjectProperties::ApplyObjectProperties(Request.Object, PropertiesJson, Request.ApplyOptions);
		if (!PropertyApply.IsValid())
		{
			OutError = TEXT("owner_apply_failed");
			return false;
		}

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
		OutApplySummary->SetObjectField(TEXT("owner_ref"), BuildOwnerRef(OwnerInfo));
		OutApplySummary->SetStringField(TEXT("details_root"), DetailsRoot);
		OutApplySummary->SetStringField(TEXT("index_file"), NormalizeDetailsIndexFile(Request.IndexFile));
		OutApplySummary->SetStringField(TEXT("ref_field"), NormalizeDetailsRefFieldName(Request.RefFieldName));
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
		bool bHasDetails = false;
		FString DetailsRoot;
		FString IndexFile;
		if (!ResolveDetailsRefRoot(EntryDirectory, EntryObject, RefFieldName, bHasDetails, DetailsRoot, IndexFile, OutError))
		{
			return false;
		}
		if (!bHasDetails)
		{
			return true;
		}

		if (!bApply)
		{
			OutApplySummary = MakeShared<FJsonObject>();
			OutApplySummary->SetStringField(TEXT("schema"), TEXT("uai.grouped_details_apply.v1"));
			OutApplySummary->SetBoolField(TEXT("ok"), true);
			OutApplySummary->SetStringField(TEXT("status"), TEXT("skipped"));
			OutApplySummary->SetObjectField(TEXT("owner_ref"), BuildOwnerRef(OwnerInfo));
			OutApplySummary->SetStringField(TEXT("details_root"), DetailsRoot);
			OutApplySummary->SetStringField(TEXT("index_file"), NormalizeDetailsIndexFile(IndexFile));
			OutApplySummary->SetStringField(TEXT("ref_field"), NormalizeDetailsRefFieldName(RefFieldName));
			OutApplySummary->SetNumberField(TEXT("properties_applied"), 0);
			return true;
		}

		FOwnerDetailsApplyRequest Request;
		Request.Object = Object;
		Request.OwnerInfo = OwnerInfo;
		Request.DetailsRoot = DetailsRoot;
		Request.IndexFile = IndexFile;
		Request.RefFieldName = RefFieldName;
		if (!ApplyOwnerDetails(Request, OutApplySummary, OutError))
		{
			return false;
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
				OutError = TEXT("grouped_details_apply_failed");
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
		if (BuildLegacyPropertiesRejectionIfPresent(Request.EntryObject, OwnerInfo, AllowedTarget, Rejection))
		{
			OutValidateSummary = Rejection;
			OutValidateSummary->TryGetStringField(TEXT("error_code"), OutError);
			return false;
		}

		FString FolderRoot = Request.FolderRoot;
		FolderRoot.TrimStartAndEndInline();
		if (!FolderRoot.IsEmpty() && BuildLegacyPropertiesFileRejectionIfPresent(FolderRoot, OwnerInfo, AllowedTarget, Rejection))
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

		const FString IndexPath = DetailsRoot / TEXT("index.json");
		TSharedPtr<FJsonObject> Index;
		FString LoadError;
		if (!LoadJsonObjectFile(IndexPath, Index, LoadError))
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_read_failed"), TEXT("details/index.json"), LoadError);
			return FinalizeSummary();
		}

		const FString IndexSchema = ReadStringField(Index, TEXT("schema"));
		if (IndexSchema != DetailsIndexSchema)
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_schema_invalid"), TEXT("details/index.json.schema"), TEXT("details index schema must be uai.details_index.v1"));
		}
		ValidateOwnerRef(Index, OwnerInfo, TEXT("details/index.json"), Issues);

		const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
		if (!Index->TryGetArrayField(TEXT("groups"), Groups) || !Groups)
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_groups_missing"), TEXT("details/index.json.groups"), TEXT("details index requires groups array"));
			return FinalizeSummary();
		}

		TSet<FString> GroupFiles;
		TSet<FString> PropertyPaths;
		TSet<FString> Selectors;
		int32 ActualFieldCount = 0;
		int32 ActualEditableFieldCount = 0;

		for (int32 GroupIndex = 0; GroupIndex < Groups->Num(); ++GroupIndex)
		{
			const FString GroupPath = FString::Printf(TEXT("details/index.json.groups.%d"), GroupIndex);
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

			TSharedPtr<FJsonObject> GroupJson;
			if (!LoadJsonObjectFile(DetailsRoot / FileName, GroupJson, LoadError))
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
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_group_count_mismatch"), TEXT("details/index.json.group_count"), TEXT("details index group_count does not match groups length"));
		}
		double DeclaredFieldCount = 0.0;
		if (Index->TryGetNumberField(TEXT("field_count"), DeclaredFieldCount) && static_cast<int32>(DeclaredFieldCount) != ActualFieldCount)
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_field_count_mismatch"), TEXT("details/index.json.field_count"), TEXT("details index field_count does not match group fields"));
		}
		double DeclaredEditableFieldCount = 0.0;
		if (Index->TryGetNumberField(TEXT("editable_field_count"), DeclaredEditableFieldCount) && static_cast<int32>(DeclaredEditableFieldCount) != ActualEditableFieldCount)
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("details_index_editable_field_count_mismatch"), TEXT("details/index.json.editable_field_count"), TEXT("details index editable_field_count does not match group fields"));
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
		if (BuildLegacyPropertiesRejectionIfPresent(Request.OldEntryObject, OwnerInfo, AllowedTarget, Rejection)
			|| BuildLegacyPropertiesRejectionIfPresent(Request.NewEntryObject, OwnerInfo, AllowedTarget, Rejection))
		{
			OutDiffSummary = Rejection;
			OutDiffSummary->TryGetStringField(TEXT("error_code"), OutError);
			return false;
		}

		FString OldFolderRoot = Request.OldFolderRoot;
		OldFolderRoot.TrimStartAndEndInline();
		FString NewFolderRoot = Request.NewFolderRoot;
		NewFolderRoot.TrimStartAndEndInline();
		if ((!OldFolderRoot.IsEmpty() && BuildLegacyPropertiesFileRejectionIfPresent(OldFolderRoot, OwnerInfo, AllowedTarget, Rejection))
			|| (!NewFolderRoot.IsEmpty() && BuildLegacyPropertiesFileRejectionIfPresent(NewFolderRoot, OwnerInfo, AllowedTarget, Rejection)))
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
		if (!LoadDetailsFieldMap(OldDetailsRoot, OldFields, LoadError))
		{
			AddValidationIssue(Issues, TEXT("error"), TEXT("old_details_read_failed"), TEXT("old_details_root"), LoadError);
			return FinalizeSummary();
		}
		if (!LoadDetailsFieldMap(NewDetailsRoot, NewFields, LoadError))
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
			AllowedTarget = TEXT("details/index.json");
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

	TSharedPtr<FJsonObject> MakeLegacyPropertiesRejection(
		const FString& ErrorCode,
		const FString& LegacySurface,
		const FOwnerInfo& OwnerInfo,
		const FString& JsonPath,
		const FString& MigrationHint,
		const FString& AllowedTarget)
	{
		TSharedPtr<FJsonObject> Rejection = MakeShared<FJsonObject>();
		Rejection->SetStringField(TEXT("schema"), TEXT("uai.legacy_rejection.v1"));
		Rejection->SetBoolField(TEXT("ok"), false);
		Rejection->SetStringField(TEXT("error_code"), ErrorCode.IsEmpty() ? FString(TEXT("legacy_properties_not_supported")) : ErrorCode);
		Rejection->SetStringField(TEXT("message"), FString::Printf(TEXT("Legacy properties surface is not supported by grouped Details authoring: %s"), *LegacySurface));
		Rejection->SetStringField(TEXT("legacy_surface"), LegacySurface);
		Rejection->SetStringField(TEXT("owner_kind"), OwnerInfo.OwnerKind);
		Rejection->SetStringField(TEXT("owner_id"), OwnerInfo.OwnerId);
		Rejection->SetStringField(TEXT("json_path"), JsonPath);
		Rejection->SetStringField(TEXT("migration_hint"), MigrationHint.IsEmpty() ? MigrationHintForLegacyError(ErrorCode) : MigrationHint);
		Rejection->SetStringField(TEXT("allowed_target"), AllowedTarget);
		Rejection->SetBoolField(TEXT("dropped"), false);
		return Rejection;
	}

	bool FindLegacyPropertiesSurface(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutLegacySurface,
		FString& OutJsonPath,
		FString& OutErrorCode)
	{
		OutLegacySurface.Reset();
		OutJsonPath.Reset();
		OutErrorCode.Reset();
		return FindLegacyPropertiesSurfaceRecursive(Object, FString(), OutLegacySurface, OutJsonPath, OutErrorCode);
	}

	bool FindLegacyPropertiesFile(
		const FString& FolderRoot,
		FString& OutLegacySurface,
		FString& OutJsonPath,
		FString& OutErrorCode)
	{
		OutLegacySurface.Reset();
		OutJsonPath.Reset();
		OutErrorCode.Reset();
		return FindLegacyPropertiesFileRecursive(FolderRoot, OutLegacySurface, OutJsonPath, OutErrorCode);
	}

	bool BuildLegacyPropertiesRejectionIfPresent(
		const TSharedPtr<FJsonObject>& Object,
		const FOwnerInfo& OwnerInfo,
		const FString& AllowedTarget,
		TSharedPtr<FJsonObject>& OutRejection)
	{
		OutRejection.Reset();
		FString LegacySurface;
		FString JsonPath;
		FString ErrorCode;
		if (!FindLegacyPropertiesSurface(Object, LegacySurface, JsonPath, ErrorCode))
		{
			return false;
		}

		OutRejection = MakeLegacyPropertiesRejection(
			ErrorCode,
			LegacySurface,
			OwnerInfo,
			JsonPath,
			MigrationHintForLegacyError(ErrorCode),
			AllowedTarget);
		return true;
	}

	bool BuildLegacyPropertiesFileRejectionIfPresent(
		const FString& FolderRoot,
		const FOwnerInfo& OwnerInfo,
		const FString& AllowedTarget,
		TSharedPtr<FJsonObject>& OutRejection)
	{
		OutRejection.Reset();
		FString LegacySurface;
		FString JsonPath;
		FString ErrorCode;
		if (!FindLegacyPropertiesFile(FolderRoot, LegacySurface, JsonPath, ErrorCode))
		{
			return false;
		}

		OutRejection = MakeLegacyPropertiesRejection(
			ErrorCode,
			LegacySurface,
			OwnerInfo,
			JsonPath,
			MigrationHintForLegacyError(ErrorCode),
			AllowedTarget);
		return true;
	}

	bool BuildLegacyPropertiesFolderRejectionIfPresent(
		const FString& FolderRoot,
		const FOwnerInfo& OwnerInfo,
		const FString& AllowedTarget,
		TSharedPtr<FJsonObject>& OutRejection,
		FString& OutError)
	{
		OutRejection.Reset();
		OutError.Reset();

		TSharedPtr<FJsonObject> FileRejection;
		if (BuildLegacyPropertiesFileRejectionIfPresent(FolderRoot, OwnerInfo, AllowedTarget, FileRejection))
		{
			OutRejection = FileRejection;
			if (!OutRejection.IsValid() || !OutRejection->TryGetStringField(TEXT("error_code"), OutError) || OutError.IsEmpty())
			{
				OutError = TEXT("legacy_properties_not_supported");
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
		TArray<FString> JsonFiles;
		IFileManager::Get().FindFilesRecursive(JsonFiles, *AbsoluteRoot, TEXT("*.json"), true, false, false);
		JsonFiles.Sort();
		for (const FString& JsonFile : JsonFiles)
		{
			FString RelativeFile = MakePathRelativeToRoot(JsonFile, AbsoluteRoot);
			if (IsDiagnosticsOrMigrationRelativePath(RelativeFile))
			{
				continue;
			}

			FString JsonText;
			if (!FFileHelper::LoadFileToString(JsonText, *JsonFile))
			{
				continue;
			}

			TSharedPtr<FJsonObject> JsonObject;
			const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
			if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
			{
				continue;
			}

			FString LegacySurface;
			FString JsonPath;
			FString ErrorCode;
			if (!FindLegacyPropertiesSurface(JsonObject, LegacySurface, JsonPath, ErrorCode))
			{
				continue;
			}

			RelativeFile = NormalizeJsonRelativePath(RelativeFile);
			const FString FullJsonPath = JsonPath.IsEmpty()
				? RelativeFile
				: FString::Printf(TEXT("%s.%s"), *RelativeFile, *JsonPath);
			OutRejection = MakeLegacyPropertiesRejection(
				ErrorCode,
				LegacySurface,
				OwnerInfo,
				FullJsonPath,
				MigrationHintForLegacyError(ErrorCode),
				AllowedTarget);
			if (!OutRejection.IsValid() || !OutRejection->TryGetStringField(TEXT("error_code"), OutError) || OutError.IsEmpty())
			{
				OutError = TEXT("legacy_properties_not_supported");
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
		OutDetailsRoot = EntryDirectory / FPaths::GetPath(OutIndexFile);
		double FieldCount = -1.0;
		const FString ResolvedIndexPath = EntryDirectory / OutIndexFile;
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

	bool WriteGroupedDetails(
		const FString& DetailsRoot,
		const FOwnerInfo& OwnerInfo,
		const TSharedPtr<FJsonObject>& PropertiesJson,
		TSharedPtr<FJsonObject>& OutDetailsSummary,
		FString& OutError)
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

		if (!IFileManager::Get().MakeDirectory(*DetailsRoot, true))
		{
			OutError = FString::Printf(TEXT("details_directory_create_failed:%s"), *DetailsRoot);
			return false;
		}

		TArray<FDetailsGroup> Groups;
		TMap<FString, int32> GroupIndexByKey;
		TSet<FString> UsedSlugs;
		TSet<FString> PropertyPaths;

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
				Group.FileName = Slug + TEXT(".json");

				const int32 NewIndex = Groups.Add(MoveTemp(Group));
				GroupIndexByKey.Add(GroupKey, NewIndex);
				ExistingIndex = GroupIndexByKey.Find(GroupKey);
			}

			FDetailsGroup& Group = Groups[*ExistingIndex];
			TSharedPtr<FJsonObject> Field = MakeCompactField(SourceField);
			if (!ExternalizeCandidatesIfNeeded(DetailsRoot, OwnerInfo, Group.Slug, Field, OutError))
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
			if (!FUeAgentStructuredJsonFormatter::SaveObjectToFile(DetailsRoot / Group.FileName, MakeGroupFile(OwnerInfo, Group), &OutError))
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
		if (!FUeAgentStructuredJsonFormatter::SaveObjectToFile(DetailsRoot / TEXT("index.json"), Index, &OutError))
		{
			return false;
		}

		OutDetailsSummary = MakeShared<FJsonObject>();
		OutDetailsSummary->SetStringField(TEXT("schema"), DetailsRefSchema);
		OutDetailsSummary->SetStringField(TEXT("index_file"), TEXT("details/index.json"));
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
		FString& OutError)
	{
		OutPropertiesJson.Reset();
		const FString IndexPath = DetailsRoot / TEXT("index.json");
		TSharedPtr<FJsonObject> Index;
		if (!LoadJsonObjectFile(IndexPath, Index, OutError))
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

			TSharedPtr<FJsonObject> GroupJson;
			if (!LoadJsonObjectFile(DetailsRoot / FileName, GroupJson, OutError))
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

	bool HasGroupedDetails(const FString& DetailsRoot)
	{
		return IFileManager::Get().FileExists(*(DetailsRoot / TEXT("index.json")));
	}

	bool ExternalizeInlineDetailsInObjectTree(
		const FString& EntryFilePath,
		const TSharedPtr<FJsonObject>& EntryObject,
		const FString& SourceCommand,
		FString& OutError)
	{
		if (!EntryObject.IsValid())
		{
			OutError = TEXT("entry_object_invalid");
			return false;
		}

		TSet<FString> UsedDetailsRoots;
		return ExternalizeInlineDetailsRecursive(
			FPaths::GetPath(EntryFilePath),
			EntryObject,
			SourceCommand,
			FString(),
			UsedDetailsRoots,
			OutError);
	}

	namespace
	{
		bool IsLegacyAuthoringPropertyDescriptor(const TSharedPtr<FJsonObject>& Object)
		{
			if (!Object.IsValid())
			{
				return false;
			}

			FString Unused;
			return Object->TryGetStringField(TEXT("property_path"), Unused)
				|| Object->TryGetStringField(TEXT("property_name"), Unused);
		}

		bool IsLegacyAuthoringPropertyArray(const TArray<TSharedPtr<FJsonValue>>& Values)
		{
			if (Values.Num() == 0)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& Value : Values)
			{
				if (IsLegacyAuthoringPropertyDescriptor(Value.IsValid() ? Value->AsObject() : nullptr))
				{
					return true;
				}
			}
			return false;
		}

		void RemoveLegacyAuthoringFieldsRecursive(const TSharedPtr<FJsonObject>& Object)
		{
			if (!Object.IsValid())
			{
				return;
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				if (!Pair.Value.IsValid())
				{
					continue;
				}

				if (Pair.Value->Type == EJson::Object)
				{
					RemoveLegacyAuthoringFieldsRecursive(Pair.Value->AsObject());
				}
				else if (Pair.Value->Type == EJson::Array)
				{
					for (const TSharedPtr<FJsonValue>& Item : Pair.Value->AsArray())
					{
						if (Item.IsValid() && Item->Type == EJson::Object)
						{
							RemoveLegacyAuthoringFieldsRecursive(Item->AsObject());
						}
					}
				}
			}

			const bool bHasGroupedDetailsRef = Object->HasField(TEXT("details"));

			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if (Object->TryGetArrayField(TEXT("properties"), Properties)
				&& (bHasGroupedDetailsRef || (Properties && IsLegacyAuthoringPropertyArray(*Properties))))
			{
				Object->RemoveField(TEXT("properties"));
			}

			const TArray<TSharedPtr<FJsonValue>>* SettingsProperties = nullptr;
			if (Object->TryGetArrayField(TEXT("settings_properties"), SettingsProperties)
				&& (bHasGroupedDetailsRef || (SettingsProperties && IsLegacyAuthoringPropertyArray(*SettingsProperties))))
			{
				Object->RemoveField(TEXT("settings_properties"));
			}

			Object->RemoveField(TEXT("raw_properties"));
			Object->RemoveField(TEXT("readonly_properties"));
			Object->RemoveField(TEXT("property_summary"));
			Object->RemoveField(TEXT("raw_property_summary"));
			Object->RemoveField(TEXT("raw_properties_count"));
			Object->RemoveField(TEXT("readonly_properties_count"));
		}
	}

	void RemoveLegacyAuthoringFieldsInObjectTree(const TSharedPtr<FJsonObject>& EntryObject)
	{
		RemoveLegacyAuthoringFieldsRecursive(EntryObject);
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
