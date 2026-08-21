// Copyright Epic Games, Inc. All Rights Reserved.

#include "LegacyMechanics/UeAgentLegacyMechanics.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/UeAgentAuthoringFileIO.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"
#include "Utilities/UeAgentUObjectProperties.h"
#include "Module/UeAgentInterfaceLogger.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/AssetManager.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "FileHelpers.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/PrimaryAssetId.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UeAgentDataDrivenOps
{
	static constexpr const TCHAR* DataAssetSchema = TEXT("ue_agent_interface.data_asset.v1");
	static constexpr const TCHAR* DataTableSchema = TEXT("ue_agent_interface.data_table.v1");
	static constexpr const TCHAR* PrimaryAssetSchema = TEXT("ue_agent_interface.primary_asset.v1");

	static FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();
		OutPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		FString ObjectPath = InPath;
		ObjectPath.TrimStartAndEndInline();
		ObjectPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (ObjectPath.IsEmpty())
		{
			return FString();
		}
		if (ObjectPath.Contains(TEXT(".")))
		{
			return ObjectPath;
		}

		const FString AssetPath = NormalizeAssetPath(ObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		return AssetName.IsEmpty() ? FString() : FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		const FString PackagePath = NormalizeAssetPath(InPath);
		if (!FPackageName::DoesPackageExist(PackagePath))
		{
			return nullptr;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	template<typename TObjectType>
	static TObjectType* LoadAsset(const FString& InPath)
	{
		return Cast<TObjectType>(LoadAssetObject(InPath));
	}

	static bool AssetExists(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return false;
		}
		if (FindObject<UObject>(nullptr, *ObjectPath))
		{
			return true;
		}
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
	}

	static FString JsonStringHash(const FString& Text)
	{
		return FString::Printf(TEXT("md5:%s"), *FMD5::HashAnsiString(*Text));
	}

	static FString ObjectPathForAsset(UObject* Asset)
	{
		return Asset ? Asset->GetPathName() : FString();
	}

	static FString AssetPathForObject(UObject* Asset)
	{
		return Asset && Asset->GetOutermost() ? Asset->GetOutermost()->GetName() : FString();
	}

	static bool SaveAssetPackage(UObject* Asset, FString& OutError)
	{
		if (!Asset || !Asset->GetOutermost())
		{
			OutError = TEXT("asset_not_found");
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Asset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static bool BackupAssetObject(UObject* SourceAsset, FString& OutBackupAssetPath, FString& OutBackupObjectPath, FString& OutError)
	{
		OutBackupAssetPath.Reset();
		OutBackupObjectPath.Reset();
		if (!SourceAsset || !SourceAsset->GetOutermost())
		{
			OutError = TEXT("asset_not_found");
			return false;
		}

		const FString SourceAssetName = SourceAsset->GetName();
		const FString BackupAssetName = FString::Printf(TEXT("%s_Backup_%s"), *SourceAssetName, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		const FString BackupPackageName = FString::Printf(TEXT("/Game/__UeAgentInterfaceBackups/%s"), *BackupAssetName);
		UPackage* BackupPackage = CreatePackage(*BackupPackageName);
		if (!BackupPackage)
		{
			OutError = TEXT("backup_create_package_failed");
			return false;
		}

		UObject* BackupObject = nullptr;
		if (UDataTable* SourceTable = Cast<UDataTable>(SourceAsset))
		{
			UDataTable* BackupTable = NewObject<UDataTable>(BackupPackage, UDataTable::StaticClass(), *BackupAssetName, RF_Public | RF_Standalone);
			if (BackupTable)
			{
				BackupTable->RowStruct = const_cast<UScriptStruct*>(SourceTable->GetRowStruct());
				BackupTable->SetFlags(RF_Transactional);
				if (BackupTable->RowStruct)
				{
					for (const TPair<FName, uint8*>& Pair : SourceTable->GetRowMap())
					{
						if (Pair.Value)
						{
							BackupTable->AddRow(Pair.Key, Pair.Value, BackupTable->RowStruct);
						}
					}
				}
			}
			BackupObject = BackupTable;
		}
		else
		{
			BackupObject = StaticDuplicateObject(SourceAsset, BackupPackage, *BackupAssetName, RF_Public | RF_Standalone | RF_Transactional);
		}
		if (!BackupObject)
		{
			OutError = TEXT("backup_duplicate_failed");
			return false;
		}
		FAssetRegistryModule::AssetCreated(BackupObject);
		BackupObject->MarkPackageDirty();
		BackupPackage->MarkPackageDirty();
		OutBackupAssetPath = BackupPackageName;
		OutBackupObjectPath = BackupObject->GetPathName();
		return true;
	}

	static bool SaveAuthoringFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError)
	{
		UeAgentAuthoringFileIO::FSaveOptions Options;
		Options.SourceCommand = TEXT("data_driven_json_export");
		Options.bExternalizeInlineDetails = false;
		return UeAgentAuthoringFileIO::SaveObject(OutputPathInput, Root, Options, OutOutputPath, OutError);
	}

	static bool LoadJsonFromParams(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutRoot, FString& OutJsonFile, FString& OutError)
	{
		OutRoot.Reset();
		OutJsonFile.Reset();

		FString JsonFile;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("json_file"), JsonFile) && !JsonFile.TrimStartAndEnd().IsEmpty())
		{
			UeAgentAuthoringFileIO::FLoadOptions Options;
			return UeAgentAuthoringFileIO::LoadObject(JsonFile, OutRoot, Options, OutJsonFile, OutError);
		}

		const TSharedPtr<FJsonObject>* JsonObj = nullptr;
		if (Params.IsValid() && Params->TryGetObjectField(TEXT("json"), JsonObj) && JsonObj && JsonObj->IsValid())
		{
			OutRoot = *JsonObj;
			return true;
		}

		if (Params.IsValid())
		{
			OutRoot = Params;
			return true;
		}

		OutError = TEXT("missing_json_root");
		return false;
	}

	static TSharedPtr<FJsonObject> MakeIssueObject(
		const FString& Severity,
		const FString& Code,
		const FString& Path,
		const FString& Message,
		const FString& RowName = FString(),
		const FString& FieldName = FString())
	{
		TSharedPtr<FJsonObject> Issue = UeAgentJsonDiagnostics::MakeIssue(Severity, Code, Path, Message);
		if (!RowName.IsEmpty())
		{
			Issue->SetStringField(TEXT("row_name"), RowName);
		}
		if (!FieldName.IsEmpty())
		{
			Issue->SetStringField(TEXT("field_name"), FieldName);
		}
		return Issue;
	}

	static void AddIssue(
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const FString& Severity,
		const FString& Code,
		const FString& Path,
		const FString& Message,
		const FString& RowName = FString(),
		const FString& FieldName = FString())
	{
		Issues.Add(MakeShared<FJsonValueObject>(MakeIssueObject(Severity, Code, Path, Message, RowName, FieldName)));
	}

	static int32 CountErrors(const TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		return UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error"));
	}

	static void ReadStringArrayField(const TSharedPtr<FJsonObject>& Root, const FString& FieldName, TArray<FString>& OutValues)
	{
		OutValues.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Root.IsValid() || !Root->TryGetArrayField(FieldName, Values) || !Values)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (Value.IsValid() && Value->TryGetString(Text) && !Text.TrimStartAndEnd().IsEmpty())
			{
				OutValues.Add(Text.TrimStartAndEnd());
			}
		}
	}

	static bool PackageMatchesAnyPath(const FString& PackageName, const TArray<FString>& Paths)
	{
		if (Paths.Num() == 0)
		{
			return true;
		}
		for (FString Path : Paths)
		{
			Path.TrimStartAndEndInline();
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (!Path.IsEmpty() && PackageName.StartsWith(Path))
			{
				return true;
			}
		}
		return false;
	}

	static UClass* ResolveClassByPath(const FString& ClassPathInput)
	{
		FString ClassPath = ClassPathInput;
		ClassPath.TrimStartAndEndInline();
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}
		if (UClass* ExistingClass = FindObject<UClass>(nullptr, *ClassPath))
		{
			return ExistingClass;
		}
		if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath))
		{
			return LoadedClass;
		}
		if (UClass* LoadedClass = LoadClass<UObject>(nullptr, *ClassPath))
		{
			return LoadedClass;
		}

		if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ToObjectPath(ClassPath)))
		{
			return Blueprint->GeneratedClass;
		}

		if (!ClassPath.EndsWith(TEXT("_C")))
		{
			const FString ObjectPath = ToObjectPath(ClassPath);
			const FString GeneratedClassPath = ObjectPath + TEXT("_C");
			if (UClass* GeneratedClass = LoadObject<UClass>(nullptr, *GeneratedClassPath))
			{
				return GeneratedClass;
			}
		}
		return nullptr;
	}

	static UScriptStruct* ResolveScriptStruct(const FString& StructPathInput)
	{
		FString StructPath = StructPathInput;
		StructPath.TrimStartAndEndInline();
		if (StructPath.IsEmpty())
		{
			return nullptr;
		}
		if (UScriptStruct* Existing = FindObject<UScriptStruct>(nullptr, *StructPath))
		{
			return Existing;
		}
		return LoadObject<UScriptStruct>(nullptr, *StructPath);
	}

	static FProperty* FindCanonicalProperty(UStruct* StructType, const FString& FieldName)
	{
		return StructType && !FieldName.IsEmpty() ? FindFProperty<FProperty>(StructType, *FieldName) : nullptr;
	}

	static void AddEnumCandidates(FProperty* Property, const TSharedPtr<FJsonObject>& Field)
	{
		UEnum* Enum = nullptr;
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			Enum = EnumProperty->GetEnum();
		}
		else if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			Enum = ByteProperty->Enum;
		}
		if (!Enum || !Field.IsValid())
		{
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Candidates;
		for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
		{
			if (Enum->HasMetaData(TEXT("Hidden"), Index))
			{
				continue;
			}
			TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("name"), Enum->GetNameStringByIndex(Index));
			Candidate->SetStringField(TEXT("display_name"), Enum->GetDisplayNameTextByIndex(Index).ToString());
			Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
		}
		Field->SetArrayField(TEXT("candidates"), Candidates);
	}

	static TSharedPtr<FJsonObject> PropertySchemaToJson(FProperty* Property, const void* Container = nullptr, UObject* Owner = nullptr)
	{
		TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
		if (!Property)
		{
			return Field;
		}

		Field->SetStringField(TEXT("property_path"), Property->GetName());
		Field->SetStringField(TEXT("display_name"), Property->GetDisplayNameText().ToString());
		Field->SetStringField(TEXT("tooltip"), Property->GetToolTipText().ToString());
		Field->SetStringField(TEXT("value_kind"), UeAgentPropertyValueCodec::GetValueKind(Property));
		UeAgentPropertyValueCodec::AddTypeMetadata(Property, Field);
		AddEnumCandidates(Property, Field);
		if (Container)
		{
			FString CodecReason;
			if (Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible) && UeAgentPropertyValueCodec::IsSupported(Property, CodecReason))
			{
				UeAgentPropertyValueCodec::FOptions Options;
				Field->SetField(TEXT("value"), UeAgentPropertyValueCodec::Encode(Property, Property->ContainerPtrToValuePtr<void>(Container), Owner, Options));
			}
			else
			{
				TSharedPtr<FJsonObject> Availability = MakeShared<FJsonObject>();
				Availability->SetBoolField(TEXT("editable"), false);
				Availability->SetStringField(TEXT("reason"), CodecReason.IsEmpty() ? TEXT("not_editable") : CodecReason);
				Field->SetObjectField(TEXT("availability"), Availability);
			}
		}
		return Field;
	}

	static FString StructSchemaHash(UStruct* StructType)
	{
		TArray<TSharedPtr<FJsonValue>> Schema;
		if (StructType)
		{
			for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
				{
					continue;
				}
				Schema.Add(MakeShared<FJsonValueObject>(PropertySchemaToJson(Property)));
			}
		}

		FString SchemaText;
		SchemaText = UeAgentJsonDiagnostics::SerializeJsonArrayCompact(Schema);
		return JsonStringHash(StructType ? StructType->GetPathName() + SchemaText : SchemaText);
	}

	static void ExportEditableFields(UStruct* StructType, const void* Container, UObject* Owner, TArray<TSharedPtr<FJsonValue>>& OutFields)
	{
		OutFields.Reset();
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}
			if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				continue;
			}
			OutFields.Add(MakeShared<FJsonValueObject>(PropertySchemaToJson(Property, Container, Owner)));
		}
	}

	struct FFieldPatch
	{
		FString Name;
		TSharedPtr<FJsonObject> Descriptor;
		TSharedPtr<FJsonValue> Value;
		FString JsonPath;
	};

	static bool ExtractFieldPatches(const TSharedPtr<FJsonObject>& Root, const FString& BasePath, TArray<FFieldPatch>& OutPatches, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		OutPatches.Reset();
		if (!Root.IsValid())
		{
			AddIssue(Issues, TEXT("error"), TEXT("json_root_invalid"), BasePath, TEXT("Root JSON object is invalid."));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* FieldsArray = nullptr;
		if (Root->TryGetArrayField(TEXT("fields"), FieldsArray) && FieldsArray)
		{
			for (int32 Index = 0; Index < FieldsArray->Num(); ++Index)
			{
				const FString ItemPath = FString::Printf(TEXT("%s.fields[%d]"), *BasePath, Index);
				const TSharedPtr<FJsonObject>* FieldObjPtr = nullptr;
				if (!(*FieldsArray)[Index].IsValid() || !(*FieldsArray)[Index]->TryGetObject(FieldObjPtr) || !FieldObjPtr || !FieldObjPtr->IsValid())
				{
					AddIssue(Issues, TEXT("error"), TEXT("json_array_item_type_mismatch"), ItemPath, TEXT("Field item must be an object."));
					continue;
				}

				FString Name;
				(*FieldObjPtr)->TryGetStringField(TEXT("property_path"), Name);
				if (Name.TrimStartAndEnd().IsEmpty())
				{
					AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), ItemPath + TEXT(".property_path"), TEXT("Field item requires property_path."));
					continue;
				}

				const TSharedPtr<FJsonValue> CanonicalValue = (*FieldObjPtr)->TryGetField(TEXT("value"));
				if (!CanonicalValue.IsValid())
				{
					AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), ItemPath + TEXT(".value"), TEXT("Field item requires canonical value."));
					continue;
				}

				FFieldPatch Patch;
				Patch.Name = Name.TrimStartAndEnd();
				Patch.Descriptor = *FieldObjPtr;
				Patch.Value = CanonicalValue;
				Patch.JsonPath = ItemPath;
				OutPatches.Add(Patch);
			}
			return CountErrors(Issues) == 0;
		}

		AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), BasePath + TEXT(".fields"), TEXT("JSON requires fields[]."));
		return false;
	}

	static bool TryImportPatchToContainer(
		UStruct* StructType,
		void* Container,
		UObject* Owner,
		const FFieldPatch& Patch,
		TSharedPtr<FJsonObject>& OutResult,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const TCHAR* MissingPropertyCode = TEXT("property_not_found"),
		const TCHAR* ImportFailedCode = TEXT("field_import_failed"),
		const bool bAllowMissingProperty = false)
	{
		OutResult = MakeShared<FJsonObject>();
		OutResult->SetStringField(TEXT("property_path"), Patch.Name);
		OutResult->SetField(TEXT("requested_value"), Patch.Value);
		OutResult->SetStringField(TEXT("path"), Patch.JsonPath);

		FProperty* Property = FindCanonicalProperty(StructType, Patch.Name);
		if (!Property)
		{
			OutResult->SetStringField(TEXT("property_import_status"), bAllowMissingProperty ? TEXT("property_not_found_ignored") : TEXT("property_not_found"));
			OutResult->SetBoolField(TEXT("property_import_verified"), false);
			AddIssue(
				Issues,
				bAllowMissingProperty ? TEXT("warning") : TEXT("error"),
				MissingPropertyCode,
				Patch.JsonPath,
				FString::Printf(TEXT("Property '%s' was not found."), *Patch.Name),
				FString(),
				Patch.Name);
			return bAllowMissingProperty;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
		FString CodecError;
		if (!UeAgentPropertyValueCodec::IsSupported(Property, CodecError)
			|| !UeAgentPropertyValueCodec::ValidateTypeMetadata(Property, Patch.Descriptor, CodecError))
		{
			OutResult->SetStringField(TEXT("property_import_status"), TEXT("unsupported_or_type_mismatch"));
			OutResult->SetStringField(TEXT("property_import_error"), CodecError);
			OutResult->SetBoolField(TEXT("property_import_verified"), false);
			AddIssue(Issues, TEXT("error"), ImportFailedCode, Patch.JsonPath, CodecError, FString(), Patch.Name);
			return false;
		}
		void* Desired = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
		Property->InitializeValue(Desired);
		Property->CopyCompleteValue(Desired, ValuePtr);
		UeAgentPropertyValueCodec::FOptions Options;
		if (!UeAgentPropertyValueCodec::Decode(Property, Patch.Value, Desired, Owner, Options, CodecError))
		{
			Property->DestroyValue(Desired);
			FMemory::Free(Desired);
			OutResult->SetStringField(TEXT("property_import_status"), TEXT("invalid_value"));
			OutResult->SetStringField(TEXT("property_import_error"), CodecError);
			OutResult->SetBoolField(TEXT("property_import_verified"), false);
			AddIssue(Issues, TEXT("error"), ImportFailedCode, Patch.JsonPath, CodecError, FString(), Patch.Name);
			return false;
		}
		Property->CopyCompleteValue(ValuePtr, Desired);
		bool bReadbackMatches = false;
		FString CompareError;
		const bool bCompared = UeAgentAuthoringSemanticCompare::ArePropertyValuesEquivalent(
			Property, ValuePtr, Desired, Owner, bReadbackMatches, CompareError);
		bReadbackMatches = bCompared && bReadbackMatches;
		Property->DestroyValue(Desired);
		FMemory::Free(Desired);
		OutResult->SetField(TEXT("value_read_back"), UeAgentPropertyValueCodec::Encode(Property, ValuePtr, Owner, Options));
		OutResult->SetStringField(TEXT("property_import_status"), bReadbackMatches ? TEXT("applied_and_read_back") : TEXT("readback_mismatch"));
		OutResult->SetBoolField(TEXT("property_import_verified"), true);
		if (!bReadbackMatches) AddIssue(Issues, TEXT("error"), ImportFailedCode, Patch.JsonPath, TEXT("Canonical value readback mismatch."), FString(), Patch.Name);
		return bReadbackMatches;
	}

	static bool PatchMatchesCurrent(FProperty* Property, const void* ValuePtr, UObject* Owner, const FFieldPatch& Patch)
	{
		if (!Property || !ValuePtr || !Patch.Value.IsValid()) return false;
		FString Error;
		if (!UeAgentPropertyValueCodec::IsSupported(Property, Error)
			|| !UeAgentPropertyValueCodec::ValidateTypeMetadata(Property, Patch.Descriptor, Error)) return false;
		void* Desired = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
		Property->InitializeValue(Desired);
		Property->CopyCompleteValue(Desired, ValuePtr);
		UeAgentPropertyValueCodec::FOptions Options;
		const bool bDecoded = UeAgentPropertyValueCodec::Decode(Property, Patch.Value, Desired, Owner, Options, Error);
		bool bMatches = false;
		if (bDecoded)
		{
			FString CompareError;
			if (!UeAgentAuthoringSemanticCompare::ArePropertyValuesEquivalent(
				Property, ValuePtr, Desired, Owner, bMatches, CompareError))
			{
				bMatches = false;
			}
		}
		Property->DestroyValue(Desired);
		FMemory::Free(Desired);
		return bMatches;
	}

	static void ValidateReferencePatch(
		UStruct* StructType,
		const FFieldPatch& Patch,
		const FString& ReferencePolicy,
		TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		FProperty* Property = FindCanonicalProperty(StructType, Patch.Name);
		if (!Property)
		{
			return;
		}
		const bool bReferenceLike =
			CastField<FSoftObjectProperty>(Property) != nullptr
			|| CastField<FObjectPropertyBase>(Property) != nullptr
			|| CastField<FSoftClassProperty>(Property) != nullptr
			|| CastField<FClassProperty>(Property) != nullptr;
		if (!bReferenceLike)
		{
			return;
		}

		FString RefPath;
		const TSharedPtr<FJsonObject>* RefValue = nullptr;
		if (Patch.Value.IsValid() && Patch.Value->TryGetObject(RefValue) && RefValue && RefValue->IsValid()) (*RefValue)->TryGetStringField(TEXT("path"), RefPath);
		if (RefPath.IsEmpty())
		{
			return;
		}
		if (!AssetExists(RefPath))
		{
			const bool bWarnOnly = ReferencePolicy.Equals(TEXT("warning"), ESearchCase::IgnoreCase)
				|| ReferencePolicy.Equals(TEXT("warn"), ESearchCase::IgnoreCase);
			AddIssue(
				Issues,
				bWarnOnly ? TEXT("warning") : TEXT("error"),
				TEXT("data_asset_reference_missing"),
				Patch.JsonPath,
				FString::Printf(TEXT("Referenced asset '%s' could not be resolved."), *RefPath),
				FString(),
				Patch.Name);
		}
	}

	static TArray<TSharedPtr<FJsonValue>> BuildAssetBundleArray(
		UAssetManager* AssetManager,
		const FPrimaryAssetId& PrimaryAssetId,
		const bool bValidateMissingAssets,
		int32& OutMissingAssetCount,
		TArray<TSharedPtr<FJsonValue>>* Issues);

	static TSharedPtr<FJsonObject> BuildDataAssetExportJson(UDataAsset* Asset, const bool bIncludePrimaryAsset)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), DataAssetSchema);
		Root->SetStringField(TEXT("asset_path"), AssetPathForObject(Asset));
		Root->SetStringField(TEXT("object_path"), ObjectPathForAsset(Asset));
		Root->SetStringField(TEXT("asset_class"), Asset && Asset->GetClass() ? Asset->GetClass()->GetPathName() : FString());
		Root->SetStringField(TEXT("base_class"), Asset && Asset->IsA<UPrimaryDataAsset>() ? TEXT("/Script/Engine.PrimaryDataAsset") : TEXT("/Script/Engine.DataAsset"));

		TArray<TSharedPtr<FJsonValue>> Fields;
		if (Asset)
		{
			ExportEditableFields(Asset->GetClass(), Asset, Asset, Fields);
		}
		Root->SetArrayField(TEXT("fields"), Fields);
		Root->SetNumberField(TEXT("field_count"), Fields.Num());

		FString FieldsText;
		FieldsText = UeAgentJsonDiagnostics::SerializeJsonArrayCompact(Fields);
		Root->SetStringField(TEXT("class_schema_hash"), JsonStringHash(Asset && Asset->GetClass() ? Asset->GetClass()->GetPathName() + FieldsText : FieldsText));

		if (bIncludePrimaryAsset && Asset)
		{
			TSharedPtr<FJsonObject> PrimaryObj = MakeShared<FJsonObject>();
			const FPrimaryAssetId ObjectPrimaryId = Asset->GetPrimaryAssetId();
			PrimaryObj->SetStringField(TEXT("object_primary_asset_id"), ObjectPrimaryId.ToString());
			if (UAssetManager* AssetManager = UAssetManager::GetIfInitialized())
			{
				const FPrimaryAssetId RegisteredId = AssetManager->GetPrimaryAssetIdForPath(FSoftObjectPath(Asset->GetPathName()));
				PrimaryObj->SetBoolField(TEXT("asset_manager_registered"), RegisteredId.IsValid());
				PrimaryObj->SetStringField(TEXT("registered_primary_asset_id"), RegisteredId.ToString());
				int32 MissingBundleAssets = 0;
				TArray<TSharedPtr<FJsonValue>> Bundles = BuildAssetBundleArray(AssetManager, ObjectPrimaryId, false, MissingBundleAssets, nullptr);
				PrimaryObj->SetNumberField(TEXT("bundle_count"), Bundles.Num());
				PrimaryObj->SetNumberField(TEXT("missing_bundle_asset_count"), MissingBundleAssets);
				PrimaryObj->SetArrayField(TEXT("bundles"), Bundles);
			}
			else
			{
				PrimaryObj->SetBoolField(TEXT("asset_manager_registered"), false);
				PrimaryObj->SetStringField(TEXT("registered_primary_asset_id"), TEXT(""));
				PrimaryObj->SetNumberField(TEXT("bundle_count"), 0);
				PrimaryObj->SetNumberField(TEXT("missing_bundle_asset_count"), 0);
				PrimaryObj->SetArrayField(TEXT("bundles"), TArray<TSharedPtr<FJsonValue>>());
			}
			Root->SetObjectField(TEXT("primary_asset"), PrimaryObj);
		}
		return Root;
	}

	static UDataAsset* CreateDataAssetInstance(const FString& AssetPathInput, UClass* DataAssetClass, const FString& OverwritePolicy, FString& OutError)
	{
		if (!DataAssetClass || !DataAssetClass->IsChildOf(UDataAsset::StaticClass()))
		{
			OutError = TEXT("data_asset_class_not_data_asset");
			return nullptr;
		}
		if (DataAssetClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = TEXT("data_asset_class_abstract");
			return nullptr;
		}

		const FString PackageName = NormalizeAssetPath(AssetPathInput);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (AssetName.IsEmpty())
		{
			OutError = TEXT("invalid_asset_name");
			return nullptr;
		}

		if (AssetExists(PackageName))
		{
			if (OverwritePolicy.Equals(TEXT("skip"), ESearchCase::IgnoreCase))
			{
				return LoadAsset<UDataAsset>(PackageName);
			}
			if (!OverwritePolicy.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
			{
				OutError = TEXT("asset_already_exists");
				return nullptr;
			}
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return nullptr;
		}

		if (OverwritePolicy.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
		{
			if (UObject* Existing = LoadAssetObject(PackageName))
			{
				Existing->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			}
		}

		UDataAsset* Asset = NewObject<UDataAsset>(Package, DataAssetClass, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Asset)
		{
			OutError = TEXT("data_asset_create_failed");
			return nullptr;
		}

		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
		Package->MarkPackageDirty();
		return Asset;
	}

	static bool ValidateDataAssetRoot(
		const TSharedPtr<FJsonObject>& Root,
		UDataAsset* ExistingAsset,
		UClass* ExplicitClass,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		TArray<TSharedPtr<FJsonValue>>& FieldResults,
		UClass*& OutClass,
		const bool bValidateReferences = false,
		const FString& ReferencePolicy = TEXT("error"))
	{
		OutClass = ExistingAsset ? ExistingAsset->GetClass() : ExplicitClass;
		FString ClassPath;
		if (Root.IsValid() && Root->TryGetStringField(TEXT("asset_class"), ClassPath) && !ClassPath.TrimStartAndEnd().IsEmpty())
		{
			OutClass = ResolveClassByPath(ClassPath);
		}

		if (!OutClass)
		{
			AddIssue(Issues, TEXT("error"), TEXT("data_asset_class_not_found"), TEXT("$.asset_class"), TEXT("DataAsset class could not be resolved."));
			return false;
		}
		if (!OutClass->IsChildOf(UDataAsset::StaticClass()))
		{
			AddIssue(Issues, TEXT("error"), TEXT("data_asset_class_not_data_asset"), TEXT("$.asset_class"), TEXT("Class must derive from UDataAsset."));
			return false;
		}
		if (OutClass->HasAnyClassFlags(CLASS_Abstract))
		{
			AddIssue(Issues, TEXT("error"), TEXT("data_asset_class_abstract"), TEXT("$.asset_class"), TEXT("Class is abstract and cannot create a DataAsset instance."));
			return false;
		}

		TArray<FFieldPatch> Patches;
		ExtractFieldPatches(Root, TEXT("$"), Patches, Issues);
		UObject* ValidationObject = ExistingAsset
			? StaticDuplicateObject(ExistingAsset, GetTransientPackage(), NAME_None)
			: NewObject<UObject>(GetTransientPackage(), OutClass);
		if (!ValidationObject)
		{
			AddIssue(Issues, TEXT("error"), TEXT("data_asset_validation_object_failed"), TEXT("$"), TEXT("Could not create transient validation object."));
			return false;
		}

		for (const FFieldPatch& Patch : Patches)
		{
			TSharedPtr<FJsonObject> Result;
			TryImportPatchToContainer(
				OutClass,
				ValidationObject,
				ValidationObject,
				Patch,
				Result,
				Issues,
				TEXT("data_asset_property_not_found"),
				TEXT("data_asset_property_import_failed"));
			if (bValidateReferences)
			{
				ValidateReferencePatch(OutClass, Patch, ReferencePolicy, Issues);
			}
			FieldResults.Add(MakeShared<FJsonValueObject>(Result));
		}
		return CountErrors(Issues) == 0;
	}

	struct FDataTableRowPatch
	{
		FName RowName;
		FString RowNameText;
		TArray<FFieldPatch> Fields;
		FString JsonPath;
	};

	static bool ExtractRowsArray(const TArray<TSharedPtr<FJsonValue>>* RowsArray, const FString& Path, TArray<FDataTableRowPatch>& OutRows, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!RowsArray)
		{
			return true;
		}
		for (int32 Index = 0; Index < RowsArray->Num(); ++Index)
		{
			const FString RowPath = FString::Printf(TEXT("%s[%d]"), *Path, Index);
			const TSharedPtr<FJsonObject>* RowObjPtr = nullptr;
			if (!(*RowsArray)[Index].IsValid() || !(*RowsArray)[Index]->TryGetObject(RowObjPtr) || !RowObjPtr || !RowObjPtr->IsValid())
			{
				AddIssue(Issues, TEXT("error"), TEXT("json_array_item_type_mismatch"), RowPath, TEXT("Row item must be an object."));
				continue;
			}
			FString RowNameText;
			if (!(*RowObjPtr)->TryGetStringField(TEXT("row_name"), RowNameText) || RowNameText.TrimStartAndEnd().IsEmpty())
			{
				AddIssue(Issues, TEXT("error"), TEXT("data_table_invalid_row_name"), RowPath + TEXT(".row_name"), TEXT("Row requires non-empty row_name."));
				continue;
			}
			FDataTableRowPatch Row;
			Row.RowNameText = RowNameText.TrimStartAndEnd();
			Row.RowName = FName(*Row.RowNameText);
			Row.JsonPath = RowPath;
			TArray<TSharedPtr<FJsonValue>> RowIssues;
			ExtractFieldPatches(*RowObjPtr, RowPath, Row.Fields, RowIssues);
			for (const TSharedPtr<FJsonValue>& Issue : RowIssues)
			{
				Issues.Add(Issue);
			}
			OutRows.Add(Row);
		}
		return CountErrors(Issues) == 0;
	}

	static UScriptStruct* ResolveDataTableRowStruct(const TSharedPtr<FJsonObject>& Root, UDataTable* ExistingTable, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		FString RowStructPath;
		if (Root.IsValid())
		{
			Root->TryGetStringField(TEXT("row_struct"), RowStructPath);
		}
		UScriptStruct* RowStruct = !RowStructPath.TrimStartAndEnd().IsEmpty()
			? ResolveScriptStruct(RowStructPath)
			: const_cast<UScriptStruct*>(ExistingTable ? ExistingTable->GetRowStruct() : nullptr);
		if (!RowStruct)
		{
			AddIssue(
				Issues,
				TEXT("error"),
				RowStructPath.TrimStartAndEnd().IsEmpty() ? TEXT("data_table_row_struct_missing") : TEXT("data_table_row_struct_not_found"),
				TEXT("$.row_struct"),
				RowStructPath.TrimStartAndEnd().IsEmpty()
					? TEXT("DataTable JSON requires row_struct when no existing table row struct is available.")
					: TEXT("DataTable row_struct could not be resolved."));
			return nullptr;
		}
		if (!RowStruct->IsChildOf(FTableRowBase::StaticStruct()))
		{
			AddIssue(Issues, TEXT("error"), TEXT("data_table_row_struct_not_table_row_base"), TEXT("$.row_struct"), TEXT("DataTable row_struct must inherit FTableRowBase."));
			return nullptr;
		}
		if (ExistingTable && ExistingTable->GetRowStruct() && ExistingTable->GetRowStruct() != RowStruct)
		{
			bool bAllowRecreate = false;
			Root->TryGetBoolField(TEXT("allow_recreate_on_row_struct_change"), bAllowRecreate);
			if (!bAllowRecreate)
			{
				AddIssue(Issues, TEXT("error"), TEXT("data_table_row_struct_mismatch"), TEXT("$.row_struct"), TEXT("Existing DataTable row_struct differs from target JSON."));
				return nullptr;
			}
		}
		return RowStruct;
	}

	static bool ImportRowsIntoTable(UDataTable* Table, UScriptStruct* RowStruct, const TArray<FDataTableRowPatch>& Rows, TArray<TSharedPtr<FJsonValue>>& Issues, TArray<TSharedPtr<FJsonValue>>& RowResults)
	{
		TSet<FName> SeenRows;
		for (const FDataTableRowPatch& Row : Rows)
		{
			TSharedPtr<FJsonObject> RowResult = MakeShared<FJsonObject>();
			RowResult->SetStringField(TEXT("row_name"), Row.RowNameText);
			RowResult->SetStringField(TEXT("path"), Row.JsonPath);
			if (Row.RowName.IsNone() || Row.RowNameText.IsEmpty())
			{
				RowResult->SetStringField(TEXT("status"), TEXT("invalid_row_name"));
				AddIssue(Issues, TEXT("error"), TEXT("data_table_invalid_row_name"), Row.JsonPath + TEXT(".row_name"), TEXT("Invalid row name."), Row.RowNameText);
				RowResults.Add(MakeShared<FJsonValueObject>(RowResult));
				continue;
			}
			if (SeenRows.Contains(Row.RowName))
			{
				RowResult->SetStringField(TEXT("status"), TEXT("duplicate_row_name"));
				AddIssue(Issues, TEXT("error"), TEXT("data_table_duplicate_row_name"), Row.JsonPath + TEXT(".row_name"), TEXT("Duplicate row_name."), Row.RowNameText);
				RowResults.Add(MakeShared<FJsonValueObject>(RowResult));
				continue;
			}
			SeenRows.Add(Row.RowName);

			FStructOnScope RowScope(RowStruct);
			TArray<TSharedPtr<FJsonValue>> FieldResults;
			for (const FFieldPatch& Patch : Row.Fields)
			{
				TSharedPtr<FJsonObject> FieldResult;
				TryImportPatchToContainer(
					RowStruct,
					RowScope.GetStructMemory(),
					Table,
					Patch,
					FieldResult,
					Issues,
					TEXT("data_table_field_not_found"),
					TEXT("data_table_field_import_failed"));
				FieldResult->SetStringField(TEXT("row_name"), Row.RowNameText);
				FieldResults.Add(MakeShared<FJsonValueObject>(FieldResult));
			}
			RowResult->SetArrayField(TEXT("field_results"), FieldResults);
			RowResult->SetStringField(TEXT("status"), CountErrors(Issues) == 0 ? TEXT("ok") : TEXT("validated_with_errors"));
			if (CountErrors(Issues) == 0)
			{
				Table->AddRow(Row.RowName, RowScope.GetStructMemory(), RowStruct);
			}
			RowResults.Add(MakeShared<FJsonValueObject>(RowResult));
		}
		return CountErrors(Issues) == 0;
	}

	static void CopyRowsIntoTable(
		UDataTable* SourceTable,
		UDataTable* TargetTable,
		UScriptStruct* RowStruct,
		const TSet<FName>& ExcludedRows)
	{
		if (!SourceTable || !TargetTable || !RowStruct || SourceTable->GetRowStruct() != RowStruct)
		{
			return;
		}

		TArray<FName> RowNames;
		SourceTable->GetRowMap().GetKeys(RowNames);
		RowNames.Sort([](const FName& Left, const FName& Right)
		{
			return Left.LexicalLess(Right);
		});

		for (const FName& RowName : RowNames)
		{
			if (ExcludedRows.Contains(RowName))
			{
				continue;
			}
			uint8* const* RowDataPtr = SourceTable->GetRowMap().Find(RowName);
			if (RowDataPtr && *RowDataPtr)
			{
				TargetTable->AddRow(RowName, *RowDataPtr, RowStruct);
			}
		}
	}

	static TArray<TSharedPtr<FJsonValue>> BuildAssetBundleArray(
		UAssetManager* AssetManager,
		const FPrimaryAssetId& PrimaryAssetId,
		const bool bValidateMissingAssets,
		int32& OutMissingAssetCount,
		TArray<TSharedPtr<FJsonValue>>* Issues)
	{
		OutMissingAssetCount = 0;
		TArray<TSharedPtr<FJsonValue>> Bundles;
		if (!AssetManager || !PrimaryAssetId.IsValid())
		{
			return Bundles;
		}

		TArray<FAssetBundleEntry> Entries;
		AssetManager->GetAssetBundleEntries(PrimaryAssetId, Entries);
		for (const FAssetBundleEntry& Entry : Entries)
		{
			TSharedPtr<FJsonObject> BundleObj = MakeShared<FJsonObject>();
			BundleObj->SetStringField(TEXT("name"), Entry.BundleName.ToString());
			TArray<TSharedPtr<FJsonValue>> AssetValues;
			int32 MissingInBundle = 0;
			for (const FTopLevelAssetPath& AssetPath : Entry.AssetPaths)
			{
				const FString AssetPathText = AssetPath.ToString();
				TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
				AssetObj->SetStringField(TEXT("asset_path"), AssetPathText);
				const bool bExists = AssetPathText.IsEmpty() || AssetExists(AssetPathText);
				AssetObj->SetBoolField(TEXT("exists"), bExists);
				if (bValidateMissingAssets && !bExists)
				{
					++MissingInBundle;
					++OutMissingAssetCount;
					if (Issues)
					{
						AddIssue(
							*Issues,
							TEXT("error"),
							TEXT("primary_asset_bundle_asset_missing"),
							TEXT("$.bundles"),
							FString::Printf(TEXT("Bundle '%s' references missing asset '%s'."), *Entry.BundleName.ToString(), *AssetPathText));
					}
				}
				AssetValues.Add(MakeShared<FJsonValueObject>(AssetObj));
			}
			BundleObj->SetNumberField(TEXT("asset_count"), AssetValues.Num());
			BundleObj->SetNumberField(TEXT("missing_asset_count"), MissingInBundle);
			BundleObj->SetArrayField(TEXT("assets"), AssetValues);
			Bundles.Add(MakeShared<FJsonValueObject>(BundleObj));
		}
		return Bundles;
	}

	static TSharedPtr<FJsonObject> BuildDataTableExportJson(UDataTable* Table)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), DataTableSchema);
		Root->SetStringField(TEXT("asset_path"), AssetPathForObject(Table));
		Root->SetStringField(TEXT("object_path"), ObjectPathForAsset(Table));
		Root->SetStringField(TEXT("row_struct"), Table && Table->GetRowStruct() ? Table->GetRowStruct()->GetPathName() : FString());
		Root->SetStringField(TEXT("row_struct_schema_hash"), StructSchemaHash(Table ? const_cast<UScriptStruct*>(Table->GetRowStruct()) : nullptr));
		Root->SetStringField(TEXT("row_policy"), TEXT("replace_all"));

		TArray<TSharedPtr<FJsonValue>> RowSchema;
		if (Table && Table->GetRowStruct())
		{
			FStructOnScope DefaultRow(Table->GetRowStruct());
			for (TFieldIterator<FProperty> It(Table->GetRowStruct(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
				{
					continue;
				}
				TSharedPtr<FJsonObject> FieldObj = PropertySchemaToJson(Property, DefaultRow.GetStructMemory(), Table);
				const TSharedPtr<FJsonValue> DefaultValue = FieldObj->TryGetField(TEXT("value"));
				if (DefaultValue.IsValid())
				{
					FieldObj->SetField(TEXT("default_value"), DefaultValue);
					FieldObj->RemoveField(TEXT("value"));
				}
				RowSchema.Add(MakeShared<FJsonValueObject>(FieldObj));
			}
		}
		Root->SetArrayField(TEXT("row_schema"), RowSchema);

		TArray<TSharedPtr<FJsonValue>> Rows;
		if (Table && Table->GetRowStruct())
		{
			const TMap<FName, uint8*>& RowMap = Table->GetRowMap();
			TArray<FName> RowNames;
			RowMap.GetKeys(RowNames);
			RowNames.Sort([](const FName& Left, const FName& Right)
			{
				return Left.LexicalLess(Right);
			});
			for (const FName& RowName : RowNames)
			{
				uint8* const* RowDataPtr = RowMap.Find(RowName);
				if (!RowDataPtr || !*RowDataPtr)
				{
					continue;
				}
				TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
				RowObj->SetStringField(TEXT("row_name"), RowName.ToString());
				UeAgentAuthoringJson::ApplyAuthoringMetadata(RowObj, TEXT("row_name"), RowName.ToString(), RowName.ToString(), TEXT("Data Table|Rows"), TEXT("data_table_row"));
				TArray<TSharedPtr<FJsonValue>> Fields;
				for (TFieldIterator<FProperty> It(Table->GetRowStruct(), EFieldIterationFlags::IncludeSuper); It; ++It)
				{
					FProperty* Property = *It;
					if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
					{
						continue;
					}
					TSharedPtr<FJsonObject> FieldObj = PropertySchemaToJson(Property, *RowDataPtr, Table);
					Fields.Add(MakeShared<FJsonValueObject>(FieldObj));
				}
				RowObj->SetArrayField(TEXT("fields"), Fields);
				FString RowText;
				RowText = UeAgentJsonDiagnostics::SerializeJsonArrayCompact(Fields);
				RowObj->SetStringField(TEXT("row_hash"), JsonStringHash(RowName.ToString() + RowText));
				Rows.Add(MakeShared<FJsonValueObject>(RowObj));
			}
		}
		Root->SetArrayField(TEXT("rows"), Rows);
		Root->SetNumberField(TEXT("row_count"), Rows.Num());
		return Root;
	}

	static UDataTable* CreateDataTableAsset(const FString& AssetPathInput, UScriptStruct* RowStruct, const FString& OverwritePolicy, FString& OutError)
	{
		if (!RowStruct || !RowStruct->IsChildOf(FTableRowBase::StaticStruct()))
		{
			OutError = TEXT("data_table_row_struct_not_found");
			return nullptr;
		}

		const FString PackageName = NormalizeAssetPath(AssetPathInput);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (AssetName.IsEmpty())
		{
			OutError = TEXT("invalid_asset_name");
			return nullptr;
		}
		if (AssetExists(PackageName))
		{
			if (OverwritePolicy.Equals(TEXT("skip"), ESearchCase::IgnoreCase))
			{
				return LoadAsset<UDataTable>(PackageName);
			}
			if (!OverwritePolicy.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
			{
				OutError = TEXT("asset_already_exists");
				return nullptr;
			}
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return nullptr;
		}
		if (OverwritePolicy.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
		{
			if (UObject* Existing = LoadAssetObject(PackageName))
			{
				Existing->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			}
		}
		UDataTable* Table = NewObject<UDataTable>(Package, UDataTable::StaticClass(), *AssetName, RF_Public | RF_Standalone);
		if (!Table)
		{
			OutError = TEXT("data_table_create_failed");
			return nullptr;
		}
		Table->RowStruct = RowStruct;
		Table->SetFlags(RF_Transactional);
		FAssetRegistryModule::AssetCreated(Table);
		Table->MarkPackageDirty();
		Package->MarkPackageDirty();
		return Table;
	}

	static void ExtractDataTableRowsForPolicy(const TSharedPtr<FJsonObject>& Root, TArray<FDataTableRowPatch>& OutRows, TArray<FName>& OutRemoveRows, FString& OutPolicy, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		OutRows.Reset();
		OutRemoveRows.Reset();
		OutPolicy = TEXT("merge");
		if (Root.IsValid())
		{
			Root->TryGetStringField(TEXT("row_policy"), OutPolicy);
		}
		OutPolicy.TrimStartAndEndInline();
		if (OutPolicy.IsEmpty())
		{
			OutPolicy = TEXT("merge");
		}
		if (!OutPolicy.Equals(TEXT("merge"), ESearchCase::IgnoreCase)
			&& !OutPolicy.Equals(TEXT("replace_all"), ESearchCase::IgnoreCase)
			&& !OutPolicy.Equals(TEXT("patch"), ESearchCase::IgnoreCase))
		{
			AddIssue(Issues, TEXT("error"), TEXT("data_table_invalid_row_policy"), TEXT("$.row_policy"), TEXT("row_policy must be merge, replace_all, or patch."));
		}

		const TArray<TSharedPtr<FJsonValue>>* RowsArray = nullptr;
		if (Root.IsValid() && Root->TryGetArrayField(TEXT("rows"), RowsArray))
		{
			ExtractRowsArray(RowsArray, TEXT("$.rows"), OutRows, Issues);
		}
		const TArray<TSharedPtr<FJsonValue>>* AddRowsArray = nullptr;
		if (Root.IsValid() && Root->TryGetArrayField(TEXT("add_rows"), AddRowsArray))
		{
			ExtractRowsArray(AddRowsArray, TEXT("$.add_rows"), OutRows, Issues);
		}
		const TArray<TSharedPtr<FJsonValue>>* UpdateRowsArray = nullptr;
		if (Root.IsValid() && Root->TryGetArrayField(TEXT("update_rows"), UpdateRowsArray))
		{
			ExtractRowsArray(UpdateRowsArray, TEXT("$.update_rows"), OutRows, Issues);
		}
		const TArray<TSharedPtr<FJsonValue>>* RemoveRowsArray = nullptr;
		if (Root.IsValid() && Root->TryGetArrayField(TEXT("remove_rows"), RemoveRowsArray) && RemoveRowsArray)
		{
			if (!OutPolicy.Equals(TEXT("patch"), ESearchCase::IgnoreCase))
			{
				AddIssue(Issues, TEXT("error"), TEXT("data_table_remove_rows_requires_patch_policy"), TEXT("$.remove_rows"), TEXT("remove_rows is only valid when row_policy is patch."));
			}
			for (int32 Index = 0; Index < RemoveRowsArray->Num(); ++Index)
			{
				FString RowName;
				if (!(*RemoveRowsArray)[Index].IsValid() || !(*RemoveRowsArray)[Index]->TryGetString(RowName) || RowName.TrimStartAndEnd().IsEmpty())
				{
					AddIssue(Issues, TEXT("error"), TEXT("data_table_invalid_row_name"), FString::Printf(TEXT("$.remove_rows[%d]"), Index), TEXT("remove_rows items must be non-empty strings."));
					continue;
				}
				OutRemoveRows.Add(FName(*RowName.TrimStartAndEnd()));
			}
		}
	}

	static bool ValidateDataTableRoot(
		const TSharedPtr<FJsonObject>& Root,
		UDataTable* ExistingTable,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		TArray<TSharedPtr<FJsonValue>>& RowResults,
		UScriptStruct*& OutRowStruct,
		FString& OutPolicy,
		TArray<FDataTableRowPatch>& OutRows,
		TArray<FName>& OutRemoveRows)
	{
		OutRowStruct = ResolveDataTableRowStruct(Root, ExistingTable, Issues);
		ExtractDataTableRowsForPolicy(Root, OutRows, OutRemoveRows, OutPolicy, Issues);
		if (!OutRowStruct)
		{
			return false;
		}
		if (ExistingTable && ExistingTable->GetRowStruct() && ExistingTable->GetRowStruct() != OutRowStruct && !OutPolicy.Equals(TEXT("replace_all"), ESearchCase::IgnoreCase))
		{
			AddIssue(Issues, TEXT("error"), TEXT("data_table_row_struct_recreate_requires_replace_all"), TEXT("$.row_policy"), TEXT("Changing row_struct requires row_policy=replace_all."));
			return false;
		}

		UDataTable* TempTable = NewObject<UDataTable>(GetTransientPackage(), NAME_None, RF_Transient);
		TempTable->RowStruct = OutRowStruct;
		if (!ImportRowsIntoTable(TempTable, OutRowStruct, OutRows, Issues, RowResults))
		{
			return false;
		}
		if (ExistingTable && (OutPolicy.Equals(TEXT("patch"), ESearchCase::IgnoreCase) || OutPolicy.Equals(TEXT("replace_all"), ESearchCase::IgnoreCase)))
		{
			for (const FName& RemoveRow : OutRemoveRows)
			{
				if (!ExistingTable->GetRowMap().Contains(RemoveRow))
				{
					AddIssue(Issues, TEXT("error"), TEXT("data_table_remove_row_not_found"), TEXT("$.remove_rows"), FString::Printf(TEXT("Row '%s' cannot be removed because it does not exist."), *RemoveRow.ToString()), RemoveRow.ToString());
				}
			}
		}
		return CountErrors(Issues) == 0;
	}

	static bool CompareFieldFilter(FProperty* Property, const void* RowData, const TSharedPtr<FJsonObject>& FilterObj)
	{
		if (!Property || !RowData || !FilterObj.IsValid())
		{
			return false;
		}
		FString Op = TEXT("==");
		FilterObj->TryGetStringField(TEXT("op"), Op);
		const TSharedPtr<FJsonValue> ExpectedValue = FilterObj->TryGetField(TEXT("value"));
		if (!ExpectedValue.IsValid()) return false;
		const void* ActualValuePtr = Property->ContainerPtrToValuePtr<void>(RowData);
		void* ExpectedValuePtr = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
		Property->InitializeValue(ExpectedValuePtr);
		Property->CopyCompleteValue(ExpectedValuePtr, ActualValuePtr);
		FString CodecError;
		UeAgentPropertyValueCodec::FOptions Options;
		if (!UeAgentPropertyValueCodec::Decode(Property, ExpectedValue, ExpectedValuePtr, nullptr, Options, CodecError))
		{
			Property->DestroyValue(ExpectedValuePtr);
			FMemory::Free(ExpectedValuePtr);
			return false;
		}
		bool bResult = false;

		if (FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
		{
			const double ActualNumber = Numeric->IsFloatingPoint() ? Numeric->GetFloatingPointPropertyValue(ActualValuePtr) : static_cast<double>(Numeric->GetSignedIntPropertyValue(ActualValuePtr));
			const double ExpectedNumber = Numeric->IsFloatingPoint() ? Numeric->GetFloatingPointPropertyValue(ExpectedValuePtr) : static_cast<double>(Numeric->GetSignedIntPropertyValue(ExpectedValuePtr));
			if (Op == TEXT(">")) bResult = ActualNumber > ExpectedNumber;
			else if (Op == TEXT(">=")) bResult = ActualNumber >= ExpectedNumber;
			else if (Op == TEXT("<")) bResult = ActualNumber < ExpectedNumber;
			else if (Op == TEXT("<=")) bResult = ActualNumber <= ExpectedNumber;
			else if (Op == TEXT("!=")) bResult = !FMath::IsNearlyEqual(ActualNumber, ExpectedNumber);
			else bResult = FMath::IsNearlyEqual(ActualNumber, ExpectedNumber);
		}
		else
		{
			bool bIdentical = false;
			FString CompareError;
			UeAgentAuthoringSemanticCompare::ArePropertyValuesEquivalent(
				Property, ActualValuePtr, ExpectedValuePtr, nullptr, bIdentical, CompareError);
			bResult = Op == TEXT("!=") ? !bIdentical : bIdentical;
		}
		Property->DestroyValue(ExpectedValuePtr);
		FMemory::Free(ExpectedValuePtr);
		return bResult;
	}
}

bool FUeAgentLegacyMechanics::ExecuteDataDrivenCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("data_asset_create")) return CmdDataAssetCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_asset_export_json")) return CmdDataAssetExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_asset_validate_json")) return CmdDataAssetValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_asset_diff_json")) return CmdDataAssetDiffJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_asset_apply_json")) return CmdDataAssetApplyJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("primary_asset_validate_asset")) return CmdPrimaryAssetValidateAsset(Ctx, OutData, OutError);
	if (CommandLower == TEXT("primary_asset_scan_assets")) return CmdPrimaryAssetScanAssets(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_table_export_json")) return CmdDataTableExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_table_validate_json")) return CmdDataTableValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_table_diff_json")) return CmdDataTableDiffJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_table_apply_json")) return CmdDataTableApplyJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_table_query_rows")) return CmdDataTableQueryRows(Ctx, OutData, OutError);
	if (CommandLower == TEXT("data_table_sample_rows")) return CmdDataTableSampleRows(Ctx, OutData, OutError);

	OutError = TEXT("unknown_data_driven_command");
	return false;
}

bool FUeAgentLegacyMechanics::CmdDataAssetCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString ClassPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("class_path"), ClassPath) || ClassPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_class_path");
		return false;
	}
	FString OverwritePolicy = TEXT("fail");
	JsonTryGetString(Ctx.Params, TEXT("overwrite_policy"), OverwritePolicy);
	bool bBackup = false;
	JsonTryGetBool(Ctx.Params, TEXT("backup"), bBackup);

	UClass* DataAssetClass = UeAgentDataDrivenOps::ResolveClassByPath(ClassPath);
	if (!DataAssetClass)
	{
		OutError = TEXT("data_asset_class_not_found");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "DataAssetCreate", "UeAgentInterface DataAsset Create"));
	FString BackupAssetPath;
	FString BackupObjectPath;
	if (bBackup)
	{
		if (UObject* ExistingAsset = UeAgentDataDrivenOps::LoadAssetObject(AssetPath))
		{
			if (!UeAgentDataDrivenOps::BackupAssetObject(ExistingAsset, BackupAssetPath, BackupObjectPath, OutError))
			{
				return false;
			}
		}
	}
	UDataAsset* Asset = UeAgentDataDrivenOps::CreateDataAssetInstance(AssetPath, DataAssetClass, OverwritePolicy, OutError);
	if (!Asset)
	{
		return false;
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentDataDrivenOps::SaveAssetPackage(Asset, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("schema"), UeAgentDataDrivenOps::DataAssetSchema);
	OutData->SetStringField(TEXT("asset_path"), UeAgentDataDrivenOps::AssetPathForObject(Asset));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetPathName());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	OutData->SetBoolField(TEXT("backup_created"), !BackupAssetPath.IsEmpty());
	OutData->SetStringField(TEXT("backup_asset_path"), BackupAssetPath);
	OutData->SetStringField(TEXT("backup_object_path"), BackupObjectPath);
	OutData->SetStringField(TEXT("status"), TEXT("created"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdDataAssetExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UDataAsset* Asset = UeAgentDataDrivenOps::LoadAsset<UDataAsset>(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found_or_not_data_asset");
		return false;
	}
	bool bIncludePrimaryAsset = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_primary_asset"), bIncludePrimaryAsset);
	TSharedPtr<FJsonObject> Root = UeAgentDataDrivenOps::BuildDataAssetExportJson(Asset, bIncludePrimaryAsset);

	FString OutputFile;
	if (JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile) && !OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString SavedPath;
		if (!UeAgentDataDrivenOps::SaveAuthoringFile(OutputFile, Root, SavedPath, OutError))
		{
			return false;
		}
		Root->SetStringField(TEXT("output_file"), SavedPath);
	}
	OutData = Root;
	return true;
}

bool FUeAgentLegacyMechanics::CmdDataAssetValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!UeAgentDataDrivenOps::LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}

	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) && Root.IsValid())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	UDataAsset* ExistingAsset = AssetPath.TrimStartAndEnd().IsEmpty() ? nullptr : UeAgentDataDrivenOps::LoadAsset<UDataAsset>(AssetPath);
	FString ClassPath;
	JsonTryGetString(Ctx.Params, TEXT("class_path"), ClassPath);
	UClass* ExplicitClass = ClassPath.IsEmpty() ? nullptr : UeAgentDataDrivenOps::ResolveClassByPath(ClassPath);
	bool bValidateReferences = false;
	JsonTryGetBool(Ctx.Params, TEXT("validate_references"), bValidateReferences);
	FString ReferencePolicy = TEXT("error");
	JsonTryGetString(Ctx.Params, TEXT("reference_policy"), ReferencePolicy);

	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> FieldResults;
	UClass* EffectiveClass = nullptr;
	const bool bValid = UeAgentDataDrivenOps::ValidateDataAssetRoot(
		Root,
		ExistingAsset,
		ExplicitClass,
		Issues,
		FieldResults,
		EffectiveClass,
		bValidateReferences,
		ReferencePolicy);
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.data_asset.validate_result.v1"));
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetStringField(TEXT("status"), bValid ? TEXT("ok") : TEXT("validate_failed"));
	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("asset_class"), EffectiveClass ? EffectiveClass->GetPathName() : FString());
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetArrayField(TEXT("field_results"), FieldResults);
	OutData->SetNumberField(TEXT("error_count"), UeAgentDataDrivenOps::CountErrors(Issues));
	OutData->SetNumberField(TEXT("warning_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("warning")));
	if (!JsonFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), JsonFile);
	}
	if (!bValid)
	{
		OutError = TEXT("data_asset_validate_failed");
	}
	return bValid;
}

bool FUeAgentLegacyMechanics::CmdDataAssetDiffJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!UeAgentDataDrivenOps::LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) && Root.IsValid())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	UDataAsset* Asset = UeAgentDataDrivenOps::LoadAsset<UDataAsset>(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found_or_not_data_asset");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<UeAgentDataDrivenOps::FFieldPatch> Patches;
	UeAgentDataDrivenOps::ExtractFieldPatches(Root, TEXT("$"), Patches, Issues);
	TArray<TSharedPtr<FJsonValue>> Diffs;
	for (const UeAgentDataDrivenOps::FFieldPatch& Patch : Patches)
	{
		FProperty* Property = UeAgentDataDrivenOps::FindCanonicalProperty(Asset->GetClass(), Patch.Name);
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetStringField(TEXT("property_path"), Patch.Name);
		Diff->SetField(TEXT("target_value"), Patch.Value);
		if (!Property)
		{
			Diff->SetStringField(TEXT("status"), TEXT("property_not_found"));
			Diff->SetBoolField(TEXT("changed"), true);
		}
		else
		{
			UeAgentPropertyValueCodec::FOptions Options;
			Diff->SetField(TEXT("current_value"), UeAgentPropertyValueCodec::Encode(Property, Property->ContainerPtrToValuePtr<void>(Asset), Asset, Options));
			const bool bMatches = UeAgentDataDrivenOps::PatchMatchesCurrent(Property, Property->ContainerPtrToValuePtr<void>(Asset), Asset, Patch);
			Diff->SetBoolField(TEXT("changed"), !bMatches);
			Diff->SetStringField(TEXT("status"), bMatches ? TEXT("same") : TEXT("changed"));
		}
		Diffs.Add(MakeShared<FJsonValueObject>(Diff));
	}

	int32 ChangedCount = 0;
	for (const TSharedPtr<FJsonValue>& DiffValue : Diffs)
	{
		const TSharedPtr<FJsonObject>* DiffObj = nullptr;
		if (DiffValue.IsValid() && DiffValue->TryGetObject(DiffObj) && DiffObj && DiffObj->IsValid() && (*DiffObj)->GetBoolField(TEXT("changed")))
		{
			++ChangedCount;
		}
	}

	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.data_asset.diff_result.v1"));
	OutData->SetStringField(TEXT("asset_path"), UeAgentDataDrivenOps::AssetPathForObject(Asset));
	OutData->SetBoolField(TEXT("changed"), ChangedCount > 0);
	OutData->SetNumberField(TEXT("changed_count"), ChangedCount);
	OutData->SetArrayField(TEXT("field_diffs"), Diffs);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), UeAgentDataDrivenOps::CountErrors(Issues));
	return UeAgentDataDrivenOps::CountErrors(Issues) == 0;
}

bool FUeAgentLegacyMechanics::CmdDataAssetApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!UeAgentDataDrivenOps::LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}

	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) && Root.IsValid())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UDataAsset* Asset = UeAgentDataDrivenOps::LoadAsset<UDataAsset>(AssetPath);
	bool bCreateIfMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	bool bBackup = false;
	JsonTryGetBool(Ctx.Params, TEXT("backup"), bBackup);
	bool bValidateReferences = false;
	JsonTryGetBool(Ctx.Params, TEXT("validate_references"), bValidateReferences);
	FString ReferencePolicy = TEXT("error");
	JsonTryGetString(Ctx.Params, TEXT("reference_policy"), ReferencePolicy);
	FString ClassPath;
	JsonTryGetString(Ctx.Params, TEXT("class_path"), ClassPath);
	if (ClassPath.IsEmpty() && Root.IsValid())
	{
		Root->TryGetStringField(TEXT("asset_class"), ClassPath);
	}
	UClass* ExplicitClass = ClassPath.IsEmpty() ? nullptr : UeAgentDataDrivenOps::ResolveClassByPath(ClassPath);

	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> ValidationFieldResults;
	UClass* EffectiveClass = nullptr;
	if (!UeAgentDataDrivenOps::ValidateDataAssetRoot(
		Root,
		Asset,
		ExplicitClass,
		Issues,
		ValidationFieldResults,
		EffectiveClass,
		bValidateReferences,
		ReferencePolicy))
	{
		OutData->SetStringField(TEXT("status"), TEXT("validate_failed"));
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutData->SetArrayField(TEXT("field_results"), ValidationFieldResults);
		OutData->SetNumberField(TEXT("error_count"), UeAgentDataDrivenOps::CountErrors(Issues));
		OutError = TEXT("data_asset_validate_failed");
		return false;
	}

	FString BackupAssetPath;
	FString BackupObjectPath;
	if (bBackup && Asset)
	{
		if (!UeAgentDataDrivenOps::BackupAssetObject(Asset, BackupAssetPath, BackupObjectPath, OutError))
		{
			return false;
		}
	}
	if (!Asset)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("asset_not_found");
			return false;
		}
		FString OverwritePolicy = TEXT("fail");
		JsonTryGetString(Ctx.Params, TEXT("overwrite_policy"), OverwritePolicy);
		Asset = UeAgentDataDrivenOps::CreateDataAssetInstance(AssetPath, EffectiveClass, OverwritePolicy, OutError);
		if (!Asset)
		{
			return false;
		}
	}

	TArray<UeAgentDataDrivenOps::FFieldPatch> Patches;
	TArray<TSharedPtr<FJsonValue>> ExtractIssues;
	UeAgentDataDrivenOps::ExtractFieldPatches(Root, TEXT("$"), Patches, ExtractIssues);
	TArray<TSharedPtr<FJsonValue>> CanonicalDescriptors;
	for (const UeAgentDataDrivenOps::FFieldPatch& Patch : Patches)
	{
		if (UeAgentDataDrivenOps::FindCanonicalProperty(Asset->GetClass(), Patch.Name))
		{
			CanonicalDescriptors.Add(MakeShared<FJsonValueObject>(Patch.Descriptor));
		}
	}
	TSharedPtr<FJsonObject> ApplyPayload = MakeShared<FJsonObject>();
	ApplyPayload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
	ApplyPayload->SetArrayField(TEXT("properties"), CanonicalDescriptors);
	UeAgentUObjectProperties::FApplyOptions ApplyOptions;
	const TSharedPtr<FJsonObject> ApplySummary = UeAgentUObjectProperties::ApplyObjectProperties(Asset, ApplyPayload, ApplyOptions);
	FString ApplyStatus;
	ApplySummary->TryGetStringField(TEXT("status"), ApplyStatus);
	const TArray<TSharedPtr<FJsonValue>>* ApplyResults = nullptr;
	TArray<TSharedPtr<FJsonValue>> FieldResults;
	if (ApplySummary->TryGetArrayField(TEXT("properties"), ApplyResults) && ApplyResults) FieldResults = *ApplyResults;
	if (ApplyStatus != TEXT("applied"))
	{
		OutData->SetStringField(TEXT("status"), TEXT("apply_failed"));
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutData->SetArrayField(TEXT("field_results"), FieldResults);
		OutData->SetObjectField(TEXT("apply_summary"), ApplySummary);
		OutData->SetNumberField(TEXT("error_count"), 1);
		ApplySummary->TryGetStringField(TEXT("reason"), OutError);
		if (OutError.IsEmpty()) OutError = TEXT("data_asset_apply_failed");
		return false;
	}

	if (bSaveAfterApply && !UeAgentDataDrivenOps::SaveAssetPackage(Asset, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("schema"), UeAgentDataDrivenOps::DataAssetSchema);
	OutData->SetStringField(TEXT("asset_path"), UeAgentDataDrivenOps::AssetPathForObject(Asset));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetPathName());
	OutData->SetStringField(TEXT("status"), TEXT("applied"));
	OutData->SetNumberField(TEXT("applied_field_count"), FieldResults.Num());
	OutData->SetArrayField(TEXT("field_results"), FieldResults);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), 0);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	OutData->SetBoolField(TEXT("backup_created"), !BackupAssetPath.IsEmpty());
	OutData->SetStringField(TEXT("backup_asset_path"), BackupAssetPath);
	OutData->SetStringField(TEXT("backup_object_path"), BackupObjectPath);
	FString ReadBackText;
	UeAgentJsonDiagnostics::SerializeJsonObjectCompact(UeAgentDataDrivenOps::BuildDataAssetExportJson(Asset, true), ReadBackText);
	OutData->SetStringField(TEXT("read_back_hash"), UeAgentDataDrivenOps::JsonStringHash(ReadBackText));
	if (!JsonFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), JsonFile);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPrimaryAssetValidateAsset(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UObject* Asset = UeAgentDataDrivenOps::LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	FString ExpectedType;
	JsonTryGetString(Ctx.Params, TEXT("expected_type"), ExpectedType);
	FString ExpectedId;
	JsonTryGetString(Ctx.Params, TEXT("expected_id"), ExpectedId);
	bool bLoadProbe = false;
	JsonTryGetBool(Ctx.Params, TEXT("load_probe"), bLoadProbe);
	bool bValidateBundles = false;
	JsonTryGetBool(Ctx.Params, TEXT("validate_bundles"), bValidateBundles);

	TArray<TSharedPtr<FJsonValue>> Issues;
	const FPrimaryAssetId ObjectId = Asset->GetPrimaryAssetId();
	FPrimaryAssetId RegisteredId;
	FAssetData RegisteredData;
	bool bRegistered = false;
	TArray<TSharedPtr<FJsonValue>> Bundles;
	int32 MissingBundleAssets = 0;
	if (UAssetManager* AssetManager = UAssetManager::GetIfInitialized())
	{
		RegisteredId = AssetManager->GetPrimaryAssetIdForPath(FSoftObjectPath(Asset->GetPathName()));
		bRegistered = RegisteredId.IsValid() && AssetManager->GetPrimaryAssetData(RegisteredId, RegisteredData);
		Bundles = UeAgentDataDrivenOps::BuildAssetBundleArray(AssetManager, ObjectId, bValidateBundles, MissingBundleAssets, &Issues);
	}

	const bool bTypeMatches = ExpectedType.IsEmpty() || ObjectId.PrimaryAssetType.ToString() == ExpectedType;
	const bool bIdMatches = ExpectedId.IsEmpty() || ObjectId.ToString() == ExpectedId;
	if (!ObjectId.IsValid())
	{
		UeAgentDataDrivenOps::AddIssue(Issues, TEXT("error"), TEXT("primary_asset_id_invalid"), TEXT("$.asset_path"), TEXT("Object GetPrimaryAssetId returned invalid id."));
	}
	if (!bRegistered)
	{
		UeAgentDataDrivenOps::AddIssue(Issues, TEXT("error"), TEXT("primary_asset_not_registered"), TEXT("$.asset_path"), TEXT("Asset Manager did not register this asset as a primary asset."));
	}
	if (!bTypeMatches)
	{
		UeAgentDataDrivenOps::AddIssue(Issues, TEXT("error"), TEXT("primary_asset_type_mismatch"), TEXT("$.expected_type"), TEXT("Object primary asset type does not match expected_type."));
	}
	if (!bIdMatches)
	{
		UeAgentDataDrivenOps::AddIssue(Issues, TEXT("error"), TEXT("primary_asset_id_mismatch"), TEXT("$.expected_id"), TEXT("Object primary asset id does not match expected_id."));
	}

	TSharedPtr<FJsonObject> LoadProbe = MakeShared<FJsonObject>();
	LoadProbe->SetBoolField(TEXT("requested"), bLoadProbe);
	LoadProbe->SetBoolField(TEXT("loaded"), false);
	LoadProbe->SetStringField(TEXT("error"), TEXT(""));
	if (bLoadProbe)
	{
		UObject* Loaded = UeAgentDataDrivenOps::LoadAssetObject(AssetPath);
		LoadProbe->SetBoolField(TEXT("loaded"), Loaded != nullptr);
		if (!Loaded)
		{
			LoadProbe->SetStringField(TEXT("error"), TEXT("load_failed"));
			UeAgentDataDrivenOps::AddIssue(Issues, TEXT("error"), TEXT("primary_asset_load_failed"), TEXT("$.load_probe"), TEXT("Asset load probe failed."));
		}
	}

	const int32 ErrorCount = UeAgentDataDrivenOps::CountErrors(Issues);
	OutData->SetStringField(TEXT("schema"), UeAgentDataDrivenOps::PrimaryAssetSchema);
	OutData->SetStringField(TEXT("asset_path"), UeAgentDataDrivenOps::AssetPathForObject(Asset));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("object_primary_asset_id"), ObjectId.ToString());
	OutData->SetStringField(TEXT("registered_primary_asset_id"), RegisteredId.ToString());
	OutData->SetBoolField(TEXT("asset_manager_registered"), bRegistered);
	OutData->SetStringField(TEXT("expected_type"), ExpectedType);
	OutData->SetStringField(TEXT("expected_id"), ExpectedId);
	OutData->SetBoolField(TEXT("type_matches"), bTypeMatches);
	OutData->SetBoolField(TEXT("id_matches"), bIdMatches);
	OutData->SetBoolField(TEXT("validate_bundles"), bValidateBundles);
	OutData->SetNumberField(TEXT("bundle_count"), Bundles.Num());
	OutData->SetNumberField(TEXT("missing_bundle_asset_count"), MissingBundleAssets);
	OutData->SetArrayField(TEXT("bundles"), Bundles);
	OutData->SetBoolField(TEXT("valid"), ErrorCount == 0);
	OutData->SetNumberField(TEXT("error_count"), ErrorCount);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetObjectField(TEXT("load_probe"), LoadProbe);
	if (ErrorCount > 0)
	{
		OutError = TEXT("primary_asset_validate_failed");
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPrimaryAssetScanAssets(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString TypeText;
	if (!JsonTryGetString(Ctx.Params, TEXT("primary_asset_type"), TypeText) || TypeText.TrimStartAndEnd().IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("type"), TypeText);
	}
	if (TypeText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_primary_asset_type");
		return false;
	}
	UAssetManager* AssetManager = UAssetManager::GetIfInitialized();
	if (!AssetManager)
	{
		OutError = TEXT("asset_manager_not_initialized");
		return false;
	}
	TArray<FString> Paths;
	UeAgentDataDrivenOps::ReadStringArrayField(Ctx.Params, TEXT("paths"), Paths);
	bool bLoadAssets = false;
	JsonTryGetBool(Ctx.Params, TEXT("load_assets"), bLoadAssets);

	TArray<FPrimaryAssetId> AssetIds;
	const bool bFound = AssetManager->GetPrimaryAssetIdList(FPrimaryAssetType(*TypeText), AssetIds);
	TArray<TSharedPtr<FJsonValue>> Items;
	int32 LoadedCount = 0;
	for (const FPrimaryAssetId& AssetId : AssetIds)
	{
		FAssetData AssetData;
		AssetManager->GetPrimaryAssetData(AssetId, AssetData);
		if (!UeAgentDataDrivenOps::PackageMatchesAnyPath(AssetData.PackageName.ToString(), Paths))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("primary_asset_id"), AssetId.ToString());
		Item->SetStringField(TEXT("asset_path"), AssetData.PackageName.ToString());
		Item->SetStringField(TEXT("object_path"), AssetData.GetSoftObjectPath().ToString());
		Item->SetStringField(TEXT("asset_class"), AssetData.AssetClassPath.ToString());
		Item->SetBoolField(TEXT("loaded"), false);
		if (bLoadAssets)
		{
			UObject* LoadedAsset = AssetData.GetAsset();
			const bool bLoaded = LoadedAsset != nullptr;
			Item->SetBoolField(TEXT("loaded"), bLoaded);
			if (bLoaded)
			{
				++LoadedCount;
			}
		}
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.primary_asset.scan_result.v1"));
	OutData->SetStringField(TEXT("primary_asset_type"), TypeText);
	OutData->SetBoolField(TEXT("found"), bFound);
	OutData->SetNumberField(TEXT("asset_count"), Items.Num());
	OutData->SetBoolField(TEXT("load_assets"), bLoadAssets);
	OutData->SetNumberField(TEXT("loaded_count"), LoadedCount);
	TArray<TSharedPtr<FJsonValue>> PathValues;
	for (const FString& Path : Paths)
	{
		PathValues.Add(MakeShared<FJsonValueString>(Path));
	}
	OutData->SetArrayField(TEXT("paths"), PathValues);
	OutData->SetArrayField(TEXT("assets"), Items);
	return true;
}

bool FUeAgentLegacyMechanics::CmdDataTableExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UDataTable* Table = UeAgentDataDrivenOps::LoadAsset<UDataTable>(AssetPath);
	if (!Table)
	{
		OutError = TEXT("asset_not_found_or_not_data_table");
		return false;
	}
	TSharedPtr<FJsonObject> Root = UeAgentDataDrivenOps::BuildDataTableExportJson(Table);
	FString OutputFile;
	if (JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile) && !OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString SavedPath;
		if (!UeAgentDataDrivenOps::SaveAuthoringFile(OutputFile, Root, SavedPath, OutError))
		{
			return false;
		}
		Root->SetStringField(TEXT("output_file"), SavedPath);
	}
	OutData = Root;
	return true;
}

bool FUeAgentLegacyMechanics::CmdDataTableValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!UeAgentDataDrivenOps::LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) && Root.IsValid())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	UDataTable* ExistingTable = AssetPath.TrimStartAndEnd().IsEmpty() ? nullptr : UeAgentDataDrivenOps::LoadAsset<UDataTable>(AssetPath);
	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> RowResults;
	UScriptStruct* RowStruct = nullptr;
	FString Policy;
	TArray<UeAgentDataDrivenOps::FDataTableRowPatch> Rows;
	TArray<FName> RemoveRows;
	const bool bValid = UeAgentDataDrivenOps::ValidateDataTableRoot(Root, ExistingTable, Issues, RowResults, RowStruct, Policy, Rows, RemoveRows);
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.data_table.validate_result.v1"));
	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("row_struct"), RowStruct ? RowStruct->GetPathName() : FString());
	OutData->SetStringField(TEXT("row_policy"), Policy);
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetStringField(TEXT("status"), bValid ? TEXT("ok") : TEXT("validate_failed"));
	OutData->SetNumberField(TEXT("row_count"), Rows.Num());
	OutData->SetArrayField(TEXT("row_results"), RowResults);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), UeAgentDataDrivenOps::CountErrors(Issues));
	if (!JsonFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), JsonFile);
	}
	if (!bValid)
	{
		OutError = TEXT("data_table_validate_failed");
	}
	return bValid;
}

bool FUeAgentLegacyMechanics::CmdDataTableDiffJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!UeAgentDataDrivenOps::LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) && Root.IsValid())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	UDataTable* Table = UeAgentDataDrivenOps::LoadAsset<UDataTable>(AssetPath);
	if (!Table)
	{
		OutError = TEXT("asset_not_found_or_not_data_table");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> RowResults;
	FString Policy;
	TArray<UeAgentDataDrivenOps::FDataTableRowPatch> Rows;
	TArray<FName> RemoveRows;
	UScriptStruct* RowStruct = nullptr;
	if (!UeAgentDataDrivenOps::ValidateDataTableRoot(Root, Table, Issues, RowResults, RowStruct, Policy, Rows, RemoveRows))
	{
		OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.data_table.diff_result.v1"));
		OutData->SetStringField(TEXT("asset_path"), UeAgentDataDrivenOps::AssetPathForObject(Table));
		OutData->SetStringField(TEXT("row_policy"), Policy);
		OutData->SetBoolField(TEXT("changed"), false);
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutData->SetArrayField(TEXT("row_results"), RowResults);
		OutData->SetNumberField(TEXT("error_count"), UeAgentDataDrivenOps::CountErrors(Issues));
		OutError = TEXT("data_table_diff_validate_failed");
		return false;
	}

	TSet<FName> TargetRows;
	for (const UeAgentDataDrivenOps::FDataTableRowPatch& Row : Rows)
	{
		TargetRows.Add(Row.RowName);
	}

	TArray<TSharedPtr<FJsonValue>> RowDiffs;
	TArray<TSharedPtr<FJsonValue>> RowAdded;
	TArray<TSharedPtr<FJsonValue>> RowRemoved;
	TArray<TSharedPtr<FJsonValue>> RowChanged;
	TArray<TSharedPtr<FJsonValue>> FieldChanged;
	TSet<FName> ChangedRowNames;
	const bool bRowStructChanged = Table->GetRowStruct() != RowStruct;
	for (const UeAgentDataDrivenOps::FDataTableRowPatch& Row : Rows)
	{
		uint8* const* CurrentRowDataPtr = Table->GetRowMap().Find(Row.RowName);
		const bool bExists = CurrentRowDataPtr && *CurrentRowDataPtr;
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetStringField(TEXT("row_name"), Row.RowNameText);
		Diff->SetBoolField(TEXT("exists"), bExists);
		if (!bExists)
		{
			Diff->SetStringField(TEXT("status"), TEXT("added"));
			RowDiffs.Add(MakeShared<FJsonValueObject>(Diff));
			RowAdded.Add(MakeShared<FJsonValueObject>(Diff));
			continue;
		}

		int32 FieldChangeCount = 0;
		for (const UeAgentDataDrivenOps::FFieldPatch& Patch : Row.Fields)
		{
			FProperty* Property = UeAgentDataDrivenOps::FindCanonicalProperty(RowStruct, Patch.Name);
			if (!Property)
			{
				continue;
			}
			const void* CurrentValuePtr = Property->ContainerPtrToValuePtr<void>(*CurrentRowDataPtr);
			if (!UeAgentDataDrivenOps::PatchMatchesCurrent(Property, CurrentValuePtr, Table, Patch))
			{
				TSharedPtr<FJsonObject> FieldDiff = MakeShared<FJsonObject>();
				FieldDiff->SetStringField(TEXT("row_name"), Row.RowNameText);
				FieldDiff->SetStringField(TEXT("property_path"), Patch.Name);
				UeAgentPropertyValueCodec::FOptions Options;
				FieldDiff->SetField(TEXT("current_value"), UeAgentPropertyValueCodec::Encode(Property, CurrentValuePtr, Table, Options));
				FieldDiff->SetField(TEXT("target_value"), Patch.Value);
				FieldChanged.Add(MakeShared<FJsonValueObject>(FieldDiff));
				++FieldChangeCount;
			}
		}
		if (FieldChangeCount > 0)
		{
			Diff->SetStringField(TEXT("status"), TEXT("changed"));
			Diff->SetNumberField(TEXT("field_change_count"), FieldChangeCount);
			RowDiffs.Add(MakeShared<FJsonValueObject>(Diff));
			if (!ChangedRowNames.Contains(Row.RowName))
			{
				ChangedRowNames.Add(Row.RowName);
				RowChanged.Add(MakeShared<FJsonValueObject>(Diff));
			}
		}
	}
	if (Policy.Equals(TEXT("replace_all"), ESearchCase::IgnoreCase))
	{
		for (const TPair<FName, uint8*>& Pair : Table->GetRowMap())
		{
			if (!TargetRows.Contains(Pair.Key))
			{
				TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
				Diff->SetStringField(TEXT("row_name"), Pair.Key.ToString());
				Diff->SetStringField(TEXT("status"), TEXT("removed"));
				Diff->SetBoolField(TEXT("exists"), true);
				RowDiffs.Add(MakeShared<FJsonValueObject>(Diff));
				RowRemoved.Add(MakeShared<FJsonValueObject>(Diff));
			}
		}
	}
	for (const FName& RemoveRow : RemoveRows)
	{
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetStringField(TEXT("row_name"), RemoveRow.ToString());
		Diff->SetStringField(TEXT("status"), TEXT("removed"));
		Diff->SetBoolField(TEXT("exists"), Table->GetRowMap().Contains(RemoveRow));
		RowDiffs.Add(MakeShared<FJsonValueObject>(Diff));
		RowRemoved.Add(MakeShared<FJsonValueObject>(Diff));
	}

	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.data_table.diff_result.v1"));
	OutData->SetStringField(TEXT("asset_path"), UeAgentDataDrivenOps::AssetPathForObject(Table));
	OutData->SetStringField(TEXT("row_policy"), Policy);
	OutData->SetStringField(TEXT("current_row_struct"), Table->GetRowStruct() ? Table->GetRowStruct()->GetPathName() : FString());
	OutData->SetStringField(TEXT("target_row_struct"), RowStruct ? RowStruct->GetPathName() : FString());
	OutData->SetBoolField(TEXT("row_struct_changed"), bRowStructChanged);
	OutData->SetBoolField(TEXT("row_struct_schema_changed"), bRowStructChanged ? true : UeAgentDataDrivenOps::StructSchemaHash(const_cast<UScriptStruct*>(Table->GetRowStruct())) != UeAgentDataDrivenOps::StructSchemaHash(RowStruct));
	OutData->SetArrayField(TEXT("row_diffs"), RowDiffs);
	OutData->SetArrayField(TEXT("row_added"), RowAdded);
	OutData->SetArrayField(TEXT("row_removed"), RowRemoved);
	OutData->SetArrayField(TEXT("row_changed"), RowChanged);
	OutData->SetArrayField(TEXT("field_changed"), FieldChanged);
	const int32 DiffCount = RowAdded.Num() + RowRemoved.Num() + FieldChanged.Num() + (bRowStructChanged ? 1 : 0);
	OutData->SetNumberField(TEXT("diff_count"), DiffCount);
	OutData->SetNumberField(TEXT("destructive_change_count"), RowRemoved.Num() + (bRowStructChanged ? 1 : 0));
	OutData->SetBoolField(TEXT("changed"), DiffCount > 0);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetArrayField(TEXT("row_results"), RowResults);
	OutData->SetNumberField(TEXT("error_count"), UeAgentDataDrivenOps::CountErrors(Issues));
	return UeAgentDataDrivenOps::CountErrors(Issues) == 0;
}

bool FUeAgentLegacyMechanics::CmdDataTableApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Root;
	FString JsonFile;
	if (!UeAgentDataDrivenOps::LoadJsonFromParams(Ctx.Params, Root, JsonFile, OutError))
	{
		return false;
	}
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) && Root.IsValid())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UDataTable* Table = UeAgentDataDrivenOps::LoadAsset<UDataTable>(AssetPath);
	bool bCreateIfMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	bool bBackup = false;
	JsonTryGetBool(Ctx.Params, TEXT("backup"), bBackup);

	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> RowResults;
	UScriptStruct* RowStruct = nullptr;
	FString Policy;
	TArray<UeAgentDataDrivenOps::FDataTableRowPatch> Rows;
	TArray<FName> RemoveRows;
	if (!UeAgentDataDrivenOps::ValidateDataTableRoot(Root, Table, Issues, RowResults, RowStruct, Policy, Rows, RemoveRows))
	{
		OutData->SetStringField(TEXT("status"), TEXT("validate_failed"));
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutData->SetArrayField(TEXT("row_results"), RowResults);
		OutData->SetNumberField(TEXT("error_count"), UeAgentDataDrivenOps::CountErrors(Issues));
		OutError = TEXT("data_table_validate_failed");
		return false;
	}
	int32 RemovedRowCount = RemoveRows.Num();
	if (Table && Policy.Equals(TEXT("replace_all"), ESearchCase::IgnoreCase))
	{
		TSet<FName> TargetRows;
		for (const UeAgentDataDrivenOps::FDataTableRowPatch& Row : Rows)
		{
			TargetRows.Add(Row.RowName);
		}
		for (const TPair<FName, uint8*>& Pair : Table->GetRowMap())
		{
			if (!TargetRows.Contains(Pair.Key))
			{
				++RemovedRowCount;
			}
		}
	}
	const bool bDestructive = RemovedRowCount > 0 || (Table && Table->GetRowStruct() && Table->GetRowStruct() != RowStruct);

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "DataTableApplyJson", "UeAgentInterface DataTable Apply JSON"));
	FString BackupAssetPath;
	FString BackupObjectPath;
	if (bBackup && Table)
	{
		if (!UeAgentDataDrivenOps::BackupAssetObject(Table, BackupAssetPath, BackupObjectPath, OutError))
		{
			return false;
		}
	}
	if (!Table)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("asset_not_found");
			return false;
		}
		FString OverwritePolicy = TEXT("fail");
		JsonTryGetString(Ctx.Params, TEXT("overwrite_policy"), OverwritePolicy);
		Table = UeAgentDataDrivenOps::CreateDataTableAsset(AssetPath, RowStruct, OverwritePolicy, OutError);
		if (!Table)
		{
			return false;
		}
	}

	TArray<TSharedPtr<FJsonValue>> ApplyIssues;
	TArray<TSharedPtr<FJsonValue>> ApplyRowResults;
	UDataTable* StagedTable = NewObject<UDataTable>(GetTransientPackage(), NAME_None, RF_Transient);
	StagedTable->RowStruct = RowStruct;

	if (!Policy.Equals(TEXT("replace_all"), ESearchCase::IgnoreCase))
	{
		TSet<FName> ExcludedRows;
		for (const UeAgentDataDrivenOps::FDataTableRowPatch& Row : Rows)
		{
			ExcludedRows.Add(Row.RowName);
		}
		for (const FName& RemoveRow : RemoveRows)
		{
			ExcludedRows.Add(RemoveRow);
		}
		UeAgentDataDrivenOps::CopyRowsIntoTable(Table, StagedTable, RowStruct, ExcludedRows);
	}

	if (!UeAgentDataDrivenOps::ImportRowsIntoTable(StagedTable, RowStruct, Rows, ApplyIssues, ApplyRowResults))
	{
		OutData->SetStringField(TEXT("status"), TEXT("apply_failed"));
		OutData->SetArrayField(TEXT("issues"), ApplyIssues);
		OutData->SetArrayField(TEXT("row_results"), ApplyRowResults);
		OutData->SetNumberField(TEXT("error_count"), UeAgentDataDrivenOps::CountErrors(ApplyIssues));
		OutError = TEXT("data_table_apply_failed");
		return false;
	}

	if (Table->GetRowStruct() && Table->GetRowStruct() != RowStruct)
	{
		Table->Modify();
		Table->EmptyTable();
		Table->RowStruct = RowStruct;
	}
	else
	{
		Table->RowStruct = RowStruct;
		Table->Modify();
		Table->EmptyTable();
	}
	UeAgentDataDrivenOps::CopyRowsIntoTable(StagedTable, Table, RowStruct, TSet<FName>());
	Table->MarkPackageDirty();
	if (bSaveAfterApply && !UeAgentDataDrivenOps::SaveAssetPackage(Table, OutError))
	{
		return false;
	}

	OutData = UeAgentDataDrivenOps::BuildDataTableExportJson(Table);
	OutData->SetStringField(TEXT("status"), TEXT("applied"));
	OutData->SetStringField(TEXT("row_policy"), Policy);
	OutData->SetNumberField(TEXT("applied_row_count"), Rows.Num());
	OutData->SetNumberField(TEXT("removed_row_count"), RemovedRowCount);
	OutData->SetBoolField(TEXT("destructive"), bDestructive);
	OutData->SetBoolField(TEXT("requires_explicit_policy"), bDestructive);
	OutData->SetBoolField(TEXT("backup_created"), !BackupAssetPath.IsEmpty());
	OutData->SetStringField(TEXT("backup_asset_path"), BackupAssetPath);
	OutData->SetStringField(TEXT("backup_object_path"), BackupObjectPath);
	OutData->SetArrayField(TEXT("row_results"), ApplyRowResults);
	OutData->SetArrayField(TEXT("issues"), ApplyIssues);
	OutData->SetNumberField(TEXT("error_count"), 0);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	FString ReadBackText;
	UeAgentJsonDiagnostics::SerializeJsonObjectCompact(OutData, ReadBackText);
	OutData->SetStringField(TEXT("read_back_hash"), UeAgentDataDrivenOps::JsonStringHash(ReadBackText));
	if (!JsonFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), JsonFile);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdDataTableQueryRows(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UDataTable* Table = UeAgentDataDrivenOps::LoadAsset<UDataTable>(AssetPath);
	if (!Table || !Table->GetRowStruct())
	{
		OutError = TEXT("asset_not_found_or_not_data_table");
		return false;
	}

	TSet<FName> RequestedRows;
	const TArray<TSharedPtr<FJsonValue>>* RowNamesArray = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("row_names"), RowNamesArray) && RowNamesArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *RowNamesArray)
		{
			FString RowName;
			if (Value.IsValid() && Value->TryGetString(RowName) && !RowName.TrimStartAndEnd().IsEmpty())
			{
				RequestedRows.Add(FName(*RowName.TrimStartAndEnd()));
			}
		}
	}
	TSet<FString> RequestedFields;
	const TArray<TSharedPtr<FJsonValue>>* FieldNamesArray = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("field_names"), FieldNamesArray) && FieldNamesArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *FieldNamesArray)
		{
			FString FieldName;
			if (Value.IsValid() && Value->TryGetString(FieldName) && !FieldName.TrimStartAndEnd().IsEmpty())
			{
				RequestedFields.Add(FieldName.TrimStartAndEnd());
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* FiltersArray = nullptr;
	Ctx.Params->TryGetArrayField(TEXT("field_filters"), FiltersArray);
	int32 Limit = 1000;
	double LimitNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("limit"), LimitNumber))
	{
		Limit = FMath::Max(0, static_cast<int32>(LimitNumber));
	}

	TArray<TSharedPtr<FJsonValue>> RowsJson;
	TArray<FName> RowNames;
	Table->GetRowMap().GetKeys(RowNames);
	RowNames.Sort([](const FName& Left, const FName& Right)
	{
		return Left.LexicalLess(Right);
	});
	for (const FName& RowName : RowNames)
	{
		if (Limit > 0 && RowsJson.Num() >= Limit)
		{
			break;
		}
		if (RequestedRows.Num() > 0 && !RequestedRows.Contains(RowName))
		{
			continue;
		}
		uint8* const* RowDataPtr = Table->GetRowMap().Find(RowName);
		if (!RowDataPtr || !*RowDataPtr)
		{
			continue;
		}

		bool bPassFilters = true;
		if (FiltersArray)
		{
			for (const TSharedPtr<FJsonValue>& FilterValue : *FiltersArray)
			{
				const TSharedPtr<FJsonObject>* FilterObj = nullptr;
				if (!FilterValue.IsValid() || !FilterValue->TryGetObject(FilterObj) || !FilterObj || !FilterObj->IsValid())
				{
					continue;
				}
				FString FieldName;
				if (!(*FilterObj)->TryGetStringField(TEXT("field"), FieldName))
				{
					continue;
				}
				FProperty* Property = UeAgentDataDrivenOps::FindCanonicalProperty(const_cast<UScriptStruct*>(Table->GetRowStruct()), FieldName);
				if (!UeAgentDataDrivenOps::CompareFieldFilter(Property, *RowDataPtr, *FilterObj))
				{
					bPassFilters = false;
					break;
				}
			}
		}
		if (!bPassFilters)
		{
			continue;
		}

		TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
		RowObj->SetStringField(TEXT("row_name"), RowName.ToString());
		TArray<TSharedPtr<FJsonValue>> Fields;
		for (TFieldIterator<FProperty> It(Table->GetRowStruct(), EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}
			if (RequestedFields.Num() > 0 && !RequestedFields.Contains(Property->GetName()) && !RequestedFields.Contains(Property->GetAuthoredName()))
			{
				continue;
			}
			Fields.Add(MakeShared<FJsonValueObject>(UeAgentDataDrivenOps::PropertySchemaToJson(Property, *RowDataPtr, Table)));
		}
		RowObj->SetArrayField(TEXT("fields"), Fields);
		RowsJson.Add(MakeShared<FJsonValueObject>(RowObj));
	}

	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.data_table.query_result.v1"));
	OutData->SetStringField(TEXT("asset_path"), UeAgentDataDrivenOps::AssetPathForObject(Table));
	OutData->SetStringField(TEXT("row_struct"), Table->GetRowStruct()->GetPathName());
	OutData->SetNumberField(TEXT("row_count"), RowsJson.Num());
	OutData->SetArrayField(TEXT("rows"), RowsJson);
	return true;
}

bool FUeAgentLegacyMechanics::CmdDataTableSampleRows(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UDataTable* Table = UeAgentDataDrivenOps::LoadAsset<UDataTable>(AssetPath);
	if (!Table || !Table->GetRowStruct())
	{
		OutError = TEXT("asset_not_found_or_not_data_table");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* ChecksArray = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("checks"), ChecksArray) || !ChecksArray)
	{
		OutError = TEXT("missing_checks");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> CheckResults;
	int32 FailedCount = 0;
	for (int32 Index = 0; Index < ChecksArray->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* CheckObj = nullptr;
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("index"), Index);
		if (!(*ChecksArray)[Index].IsValid() || !(*ChecksArray)[Index]->TryGetObject(CheckObj) || !CheckObj || !CheckObj->IsValid())
		{
			Result->SetBoolField(TEXT("passed"), false);
			Result->SetStringField(TEXT("error"), TEXT("invalid_check_item"));
			++FailedCount;
			CheckResults.Add(MakeShared<FJsonValueObject>(Result));
			continue;
		}
		FString RowNameText;
		FString FieldName;
		if (!(*CheckObj)->TryGetStringField(TEXT("row_name"), RowNameText) || !(*CheckObj)->TryGetStringField(TEXT("field"), FieldName))
		{
			Result->SetBoolField(TEXT("passed"), false);
			Result->SetStringField(TEXT("error"), TEXT("missing_row_name_or_field"));
			++FailedCount;
			CheckResults.Add(MakeShared<FJsonValueObject>(Result));
			continue;
		}
		uint8* const* RowDataPtr = Table->GetRowMap().Find(FName(*RowNameText));
		FProperty* Property = UeAgentDataDrivenOps::FindCanonicalProperty(const_cast<UScriptStruct*>(Table->GetRowStruct()), FieldName);
		if (!RowDataPtr || !*RowDataPtr || !Property)
		{
			Result->SetBoolField(TEXT("passed"), false);
			Result->SetStringField(TEXT("error"), !RowDataPtr ? TEXT("row_not_found") : TEXT("field_not_found"));
			++FailedCount;
			CheckResults.Add(MakeShared<FJsonValueObject>(Result));
			continue;
		}
		const bool bPassed = UeAgentDataDrivenOps::CompareFieldFilter(Property, *RowDataPtr, *CheckObj);
		Result->SetBoolField(TEXT("passed"), bPassed);
		Result->SetStringField(TEXT("row_name"), RowNameText);
		Result->SetStringField(TEXT("field"), FieldName);
		UeAgentPropertyValueCodec::FOptions CodecOptions;
		Result->SetField(TEXT("actual_value"), UeAgentPropertyValueCodec::Encode(Property, Property->ContainerPtrToValuePtr<void>(*RowDataPtr), Table, CodecOptions));
		Result->SetField(TEXT("expected_value"), (*CheckObj)->TryGetField(TEXT("value")));
		if (!bPassed)
		{
			++FailedCount;
		}
		CheckResults.Add(MakeShared<FJsonValueObject>(Result));
	}

	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.data_table.sample_result.v1"));
	OutData->SetStringField(TEXT("asset_path"), UeAgentDataDrivenOps::AssetPathForObject(Table));
	OutData->SetBoolField(TEXT("passed"), FailedCount == 0);
	OutData->SetNumberField(TEXT("check_count"), CheckResults.Num());
	OutData->SetNumberField(TEXT("failed_count"), FailedCount);
	OutData->SetArrayField(TEXT("checks"), CheckResults);
	if (FailedCount > 0)
	{
		OutError = TEXT("data_table_sample_failed");
		return false;
	}
	return true;
}
