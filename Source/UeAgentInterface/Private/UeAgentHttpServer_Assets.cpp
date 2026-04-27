// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Assets.h"
#include "UeAgentCurveJson.h"
#include "UeAgentJsonDiagnostics.h"
#include "UeAgentInterfaceLogger.h"

#include "AssetToolsModule.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AutomatedAssetImportData.h"
#include "Editor.h"
#include "Containers/Ticker.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Factories/TextureFactory.h"
#include "FileHelpers.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace UeAgentAssetOps
{
	static FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();

		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		FString InObjectPath = InPath;
		InObjectPath.TrimStartAndEndInline();
		if (InObjectPath.IsEmpty())
		{
			return FString();
		}
		if (InObjectPath.Contains(TEXT(".")))
		{
			return InObjectPath;
		}

		const FString AssetPath = NormalizeAssetPath(InObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
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
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	static UPackage* ResolvePackageByAssetPath(const FString& InPath)
	{
		const FString NormalizedAssetPath = NormalizeAssetPath(InPath);
		if (NormalizedAssetPath.IsEmpty())
		{
			return nullptr;
		}

		if (UPackage* ExistingPackage = FindPackage(nullptr, *NormalizedAssetPath))
		{
			return ExistingPackage;
		}

		if (UObject* AssetObj = LoadAssetObject(NormalizedAssetPath))
		{
			return AssetObj->GetOutermost();
		}

		return nullptr;
	}

	static bool ReadStringArrayParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& OutValues)
	{
		OutValues.Reset();
		if (!Params.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}

			FString Item = Value->AsString();
			Item.TrimStartAndEndInline();
			if (!Item.IsEmpty())
			{
				OutValues.Add(Item);
			}
		}

		return true;
	}

	static bool SaveJsonFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError)
	{
		OutOutputPath.Reset();
		if (!Root.IsValid())
		{
			OutError = TEXT("json_root_invalid");
			return false;
		}

		FString OutputPath = OutputPathInput;
		OutputPath.TrimStartAndEndInline();
		if (OutputPath.IsEmpty())
		{
			OutError = TEXT("missing_output_file");
			return false;
		}

		OutputPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutputPath);
		const FString OutputDir = FPaths::GetPath(OutputPath);
		if (!OutputDir.IsEmpty() && !IFileManager::Get().DirectoryExists(*OutputDir))
		{
			IFileManager::Get().MakeDirectory(*OutputDir, true);
		}

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
		{
			OutError = TEXT("json_serialize_failed");
			return false;
		}

		if (!FFileHelper::SaveStringToFile(JsonText, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("save_json_file_failed");
			return false;
		}

		OutOutputPath = OutputPath;
		return true;
	}

	static bool LoadJsonFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError)
	{
		OutRoot.Reset();
		OutResolvedPath.Reset();

		FString InputPath = InputPathInput;
		InputPath.TrimStartAndEndInline();
		if (InputPath.IsEmpty())
		{
			OutError = TEXT("missing_json_file");
			return false;
		}

		InputPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(InputPath);
		if (!FPaths::FileExists(InputPath))
		{
			OutError = TEXT("json_file_not_found");
			return false;
		}

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *InputPath))
		{
			OutError = TEXT("load_json_file_failed");
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
		{
			OutError = TEXT("json_parse_failed");
			return false;
		}

		OutResolvedPath = InputPath;
		return true;
	}

	static FProperty* FindPropertyByNameLoose(UStruct* StructType, const FString& Segment)
	{
		if (!StructType || Segment.IsEmpty())
		{
			return nullptr;
		}

		const FString Normalized = Segment.TrimStartAndEnd();
		const FString NormalizedWithoutB = Normalized.StartsWith(TEXT("b")) ? Normalized.Mid(1) : Normalized;
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}

			const FString PropertyName = Property->GetName();
			const FString PropertyNameWithoutB = PropertyName.StartsWith(TEXT("b")) ? PropertyName.Mid(1) : PropertyName;
			if (PropertyName.Equals(Normalized, ESearchCase::IgnoreCase) ||
				PropertyNameWithoutB.Equals(NormalizedWithoutB, ESearchCase::IgnoreCase) ||
				Property->GetAuthoredName().Equals(Normalized, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}

		return nullptr;
	}

	static bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!Obj || PropertyPath.IsEmpty())
		{
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			return false;
		}

		UStruct* CurrentStruct = Obj->GetClass();
		void* CurrentContainer = Obj;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segments[Index]);
			if (!Property)
			{
				return false;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (Index == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutValuePtr = ValuePtr;
				return true;
			}

			if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				CurrentStruct = StructProperty->Struct;
				CurrentContainer = ValuePtr;
				continue;
			}

			if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				UObject* NestedObject = ObjectProperty->GetObjectPropertyValue(ValuePtr);
				if (!NestedObject)
				{
					return false;
				}

				CurrentStruct = NestedObject->GetClass();
				CurrentContainer = NestedObject;
				continue;
			}

			return false;
		}

		return false;
	}

	static bool GetObjectPropertyText(UObject* Target, const FString& PropertyPath, FString& OutValueText, FString& OutCppType)
	{
		OutValueText.Reset();
		OutCppType.Reset();

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Target, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			return false;
		}

		OutCppType = Property->GetCPPType();
		Property->ExportTextItem_Direct(OutValueText, ValuePtr, nullptr, Target, PPF_None);
		return true;
	}

	static bool SetObjectPropertyText(UObject* Target, const FString& PropertyPath, const FString& ValueText, FString* OutAppliedValueText = nullptr, FString* OutCppType = nullptr, FString* OutImportError = nullptr)
	{
		if (OutImportError)
		{
			OutImportError->Reset();
		}

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Target, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			if (OutImportError)
			{
				*OutImportError = TEXT("property_not_found");
			}
			return false;
		}

		if (OutCppType)
		{
			*OutCppType = Property->GetCPPType();
		}
		if (Property->ImportText_Direct(*ValueText, ValuePtr, Target, PPF_None, GLog) == nullptr)
		{
			if (OutImportError)
			{
				*OutImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyPath, *ValueText);
			}
			return false;
		}
		if (OutAppliedValueText)
		{
			Property->ExportTextItem_Direct(*OutAppliedValueText, ValuePtr, nullptr, Target, PPF_None);
		}
		return true;
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

	static void GetDefaultAssetPropertyPaths(UObject* Asset, TArray<FString>& OutPropertyPaths, FString& OutPresetName)
	{
		OutPropertyPaths.Reset();
		OutPresetName.Reset();
		if (!Asset)
		{
			return;
		}

		if (Asset->IsA<UAnimSequence>())
		{
			OutPresetName = TEXT("anim_sequence_defaults");
			OutPropertyPaths = {
				TEXT("RateScale"),
				TEXT("bEnableRootMotion"),
				TEXT("bForceRootLock"),
				TEXT("RootMotionRootLock"),
				TEXT("Interpolation")
			};
			return;
		}

		if (Asset->IsA<UTexture>())
		{
			OutPresetName = TEXT("texture_defaults");
			OutPropertyPaths = {
				TEXT("SRGB"),
				TEXT("CompressionSettings"),
				TEXT("MipGenSettings"),
				TEXT("LODGroup"),
				TEXT("NeverStream"),
				TEXT("VirtualTextureStreaming"),
				TEXT("Filter")
			};
			return;
		}

		if (Asset->IsA<UStaticMesh>())
		{
			OutPresetName = TEXT("static_mesh_defaults");
			OutPropertyPaths = {
				TEXT("LightMapResolution"),
				TEXT("LightMapCoordinateIndex"),
				TEXT("LODGroup"),
				TEXT("bAllowCPUAccess")
			};
			return;
		}

		if (Asset->IsA<USkeletalMesh>())
		{
			OutPresetName = TEXT("skeletal_mesh_defaults");
			OutPropertyPaths = {
				TEXT("bEnablePerPolyCollision"),
				TEXT("LODSettings"),
				TEXT("DefaultAnimatingRig")
			};
			return;
		}
	}

	static bool ShouldSkipDirtyPackage(UPackage* Package)
	{
		if (!Package)
		{
			return true;
		}

		if (Package->HasAnyFlags(RF_Transient) || Package->HasAnyPackageFlags(PKG_CompiledIn))
		{
			return true;
		}

		const FString PackageName = Package->GetName();
		if (PackageName.IsEmpty() ||
			PackageName.StartsWith(TEXT("/Script/")) ||
			PackageName.StartsWith(TEXT("/Engine/Transient")) ||
			PackageName.StartsWith(TEXT("/Temp/")))
		{
			return true;
		}

		return !FPackageName::IsValidLongPackageName(PackageName, false);
	}

	static void AddPackageIfValid(TSet<UPackage*>& OutSet, UPackage* Package)
	{
		if (!Package || ShouldSkipDirtyPackage(Package))
		{
			return;
		}
		OutSet.Add(Package);
	}

	static int32 ClearDirtyFlagForPackages(const TSet<UPackage*>& Packages)
	{
		int32 ClearedCount = 0;
		for (UPackage* Package : Packages)
		{
			if (!Package || !Package->IsDirty())
			{
				continue;
			}

			Package->SetDirtyFlag(false);
			++ClearedCount;
		}
		return ClearedCount;
	}

	struct FDirtyResolutionOptions
	{
		bool bSaveCurrentLevel = false;
		bool bDiscardCurrentLevel = false;
		bool bSaveAllDirty = false;
		bool bDiscardAllDirty = false;
		bool bCloseAllAssetEditors = false;
		bool bOnlySaveDirty = true;
		TArray<FString> SaveResourcePaths;
		TArray<FString> DiscardResourcePaths;
	};

	struct FDirtyResolutionSummary
	{
		int32 SavedPackageCount = 0;
		int32 ExplicitDiscardedPackageCount = 0;
		int32 AutoDiscardedPackageCount = 0;
		TArray<FString> SaveResourceNotFound;
		TArray<FString> DiscardResourceNotFound;
		TArray<UPackage*> DirtyPackagesBefore;
		TArray<UPackage*> DirtyPackagesAfter;
	};

	static void CollectDirtyPackages(TArray<UPackage*>& OutPackages)
	{
		OutPackages.Reset();
		TSet<UPackage*> UniquePackages;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (!Package || !Package->IsDirty() || ShouldSkipDirtyPackage(Package))
			{
				continue;
			}
			UniquePackages.Add(Package);
		}
		for (UPackage* Package : UniquePackages)
		{
			OutPackages.Add(Package);
		}
		OutPackages.Sort([](const UPackage& A, const UPackage& B)
		{
			return A.GetName() < B.GetName();
		});
	}

	static UObject* FindRepresentativeObjectInPackage(UPackage* Package)
	{
		if (!Package)
		{
			return nullptr;
		}

		const FString PackageName = Package->GetName();
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (!AssetName.IsEmpty())
		{
			if (UObject* NamedObject = StaticFindObjectFast(UObject::StaticClass(), Package, FName(*AssetName)))
			{
				return NamedObject;
			}
		}

		UObject* FirstTopLevelObject = nullptr;
		for (TObjectIterator<UObject> It; It; ++It)
		{
			UObject* Obj = *It;
			if (!Obj || Obj == Package || Obj->HasAnyFlags(RF_Transient))
			{
				continue;
			}
			if (Obj->GetOutermost() != Package || Obj->GetOuter() != Package)
			{
				continue;
			}

			if (Obj->IsA<UWorld>())
			{
				return Obj;
			}
			if (!FirstTopLevelObject)
			{
				FirstTopLevelObject = Obj;
			}
		}

		return FirstTopLevelObject;
	}

	static bool IsPackageOpenInEditor(UPackage* Package, UAssetEditorSubsystem* AssetEditorSubsystem)
	{
		if (!Package || !AssetEditorSubsystem)
		{
			return false;
		}

		const TArray<UObject*> OpenAssets = AssetEditorSubsystem->GetAllEditedAssets();
		for (UObject* Asset : OpenAssets)
		{
			if (Asset && Asset->GetOutermost() == Package)
			{
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> MakeDirtyResourceJson(UPackage* Package, UWorld* EditorWorld, UAssetEditorSubsystem* AssetEditorSubsystem)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		if (!Package)
		{
			return Item;
		}

		UObject* RepresentativeObject = FindRepresentativeObjectInPackage(Package);
		const FString ResourcePath = Package->GetName();
		const bool bIsCurrentLevel = EditorWorld && EditorWorld->GetOutermost() == Package;
		const bool bIsLevel = bIsCurrentLevel || (RepresentativeObject && RepresentativeObject->IsA<UWorld>());

		Item->SetStringField(TEXT("resource_path"), ResourcePath);
		Item->SetStringField(TEXT("asset_path"), ResourcePath);
		Item->SetStringField(TEXT("package_name"), ResourcePath);
		Item->SetBoolField(TEXT("dirty"), Package->IsDirty());
		Item->SetBoolField(TEXT("decision_required"), Package->IsDirty());
		Item->SetBoolField(TEXT("is_current_level"), bIsCurrentLevel);
		Item->SetBoolField(TEXT("is_level"), bIsLevel);
		Item->SetBoolField(TEXT("is_open_in_editor"), IsPackageOpenInEditor(Package, AssetEditorSubsystem));
		Item->SetStringField(TEXT("kind"), bIsLevel ? TEXT("level") : TEXT("asset"));

		if (RepresentativeObject)
		{
			Item->SetStringField(TEXT("display_name"), RepresentativeObject->GetName());
			Item->SetStringField(TEXT("object_path"), RepresentativeObject->GetPathName());
			Item->SetStringField(TEXT("class"), RepresentativeObject->GetClass() ? RepresentativeObject->GetClass()->GetPathName() : TEXT(""));
		}
		else
		{
			Item->SetStringField(TEXT("display_name"), FPackageName::GetLongPackageAssetName(ResourcePath));
			Item->SetStringField(TEXT("object_path"), TEXT(""));
			Item->SetStringField(TEXT("class"), TEXT(""));
		}

		return Item;
	}

	static void MakeDirtyResourceArrayJson(
		const TArray<UPackage*>& Packages,
		UWorld* EditorWorld,
		UAssetEditorSubsystem* AssetEditorSubsystem,
		TArray<TSharedPtr<FJsonValue>>& OutItems)
	{
		OutItems.Reset();
		OutItems.Reserve(Packages.Num());
		for (UPackage* Package : Packages)
		{
			OutItems.Add(MakeShared<FJsonValueObject>(MakeDirtyResourceJson(Package, EditorWorld, AssetEditorSubsystem)));
		}
	}

	static void ParseResourcePathArrays(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutSavePaths, TArray<FString>& OutDiscardPaths)
	{
		OutSavePaths.Reset();
		OutDiscardPaths.Reset();

		if (!ReadStringArrayParam(Params, TEXT("save_resource_paths"), OutSavePaths))
		{
			if (!ReadStringArrayParam(Params, TEXT("save_asset_paths"), OutSavePaths))
			{
				ReadStringArrayParam(Params, TEXT("save_resources"), OutSavePaths);
			}
		}
		if (!ReadStringArrayParam(Params, TEXT("discard_resource_paths"), OutDiscardPaths))
		{
			if (!ReadStringArrayParam(Params, TEXT("discard_asset_paths"), OutDiscardPaths))
			{
				ReadStringArrayParam(Params, TEXT("discard_resources"), OutDiscardPaths);
			}
		}
	}

	static bool ResolveImportSourceFilenameWithExtensions(
		const FString& InFilename,
		const TSet<FString>& AllowedExtensions,
		const TCHAR* ExtensionError,
		FString& OutAbsoluteFilename,
		FString& OutError)
	{
		OutAbsoluteFilename = InFilename;
		OutAbsoluteFilename.TrimStartAndEndInline();
		if (OutAbsoluteFilename.IsEmpty())
		{
			OutError = TEXT("missing_source_filename");
			return false;
		}

		if (FPaths::IsRelative(OutAbsoluteFilename))
		{
			OutAbsoluteFilename = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutAbsoluteFilename);
		}
		else
		{
			OutAbsoluteFilename = FPaths::ConvertRelativePathToFull(OutAbsoluteFilename);
		}
		FPaths::NormalizeFilename(OutAbsoluteFilename);
		if (!FPaths::FileExists(OutAbsoluteFilename))
		{
			OutError = TEXT("source_file_not_found");
			return false;
		}

		const FString Extension = FPaths::GetExtension(OutAbsoluteFilename).ToLower();
		if (AllowedExtensions.Num() > 0 && !AllowedExtensions.Contains(Extension))
		{
			OutError = ExtensionError ? ExtensionError : TEXT("source_file_extension_not_allowed");
			return false;
		}

		return true;
	}

	static bool ResolveImportSourceFilename(const FString& InFilename, FString& OutAbsoluteFilename, FString& OutError)
	{
		const TSet<FString> FbxExtensions = { TEXT("fbx") };
		return ResolveImportSourceFilenameWithExtensions(
			InFilename,
			FbxExtensions,
			TEXT("source_file_is_not_fbx"),
			OutAbsoluteFilename,
			OutError);
	}

	template<typename TEnum>
	static FString EnumValueToString(TEnum Value)
	{
		if (const UEnum* Enum = StaticEnum<TEnum>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(Value));
		}
		return FString::Printf(TEXT("%d"), static_cast<int32>(Value));
	}

	template<typename TEnum>
	static bool TryParseEnumValue(const FString& InText, const TCHAR* FieldName, TEnum& OutValue, FString& OutError)
	{
		FString Text = InText;
		Text.TrimStartAndEndInline();
		if (Text.IsEmpty())
		{
			OutError = FString::Printf(TEXT("empty_enum_value:%s"), FieldName ? FieldName : TEXT("enum"));
			return false;
		}

		const UEnum* Enum = StaticEnum<TEnum>();
		if (!Enum)
		{
			OutError = FString::Printf(TEXT("enum_type_not_found:%s"), FieldName ? FieldName : TEXT("enum"));
			return false;
		}

		int64 Value = Enum->GetValueByNameString(Text, EGetByNameFlags::CheckAuthoredName);
		if (Value == INDEX_NONE)
		{
			Value = Enum->GetValueByNameString(Text, EGetByNameFlags::None);
		}
		if (Value == INDEX_NONE && !Text.Contains(TEXT("::")))
		{
			const FString QualifiedText = FString::Printf(TEXT("%s::%s"), *Enum->GetName(), *Text);
			Value = Enum->GetValueByNameString(QualifiedText, EGetByNameFlags::CheckAuthoredName);
			if (Value == INDEX_NONE)
			{
				Value = Enum->GetValueByNameString(QualifiedText, EGetByNameFlags::None);
			}
		}
		if (Value == INDEX_NONE)
		{
			int64 NumericValue = 0;
			if (LexTryParseString(NumericValue, *Text) && Enum->IsValidEnumValue(NumericValue))
			{
				Value = NumericValue;
			}
		}
		if (Value == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("invalid_enum_value:%s:%s"), FieldName ? FieldName : TEXT("enum"), *Text);
			return false;
		}

		OutValue = static_cast<TEnum>(Value);
		return true;
	}

	static bool ResolveImportDestinationPath(const FString& InDestinationPath, FString& OutDestinationPath, FString& OutError)
	{
		OutDestinationPath = NormalizeAssetPath(InDestinationPath);
		if (!FPackageName::IsValidLongPackageName(OutDestinationPath))
		{
			OutError = TEXT("invalid_destination_path");
			return false;
		}
		return true;
	}

	static void GatherImportedObjects(const TArray<UObject*>& ImportedObjects, TArray<UObject*>& OutImportedObjects)
	{
		OutImportedObjects.Reset();

		TSet<UObject*> UniqueObjects;
		for (UObject* ImportedObject : ImportedObjects)
		{
			if (ImportedObject)
			{
				UniqueObjects.Add(ImportedObject);
			}
		}

		for (UObject* ImportedObject : UniqueObjects)
		{
			if (ImportedObject)
			{
				OutImportedObjects.Add(ImportedObject);
			}
		}

		OutImportedObjects.Sort([](const UObject& A, const UObject& B)
		{
			return A.GetPathName() < B.GetPathName();
		});
	}

	static void FillImportedObjectsResponse(const TArray<UObject*>& ImportedObjectsIn, TSharedPtr<FJsonObject>& OutData)
	{
		TArray<UObject*> ImportedObjects;
		GatherImportedObjects(ImportedObjectsIn, ImportedObjects);

		TArray<TSharedPtr<FJsonValue>> ImportedObjectsJson;
		TArray<TSharedPtr<FJsonValue>> ImportedObjectPathsJson;
		TArray<TSharedPtr<FJsonValue>> SkeletalMeshPathsJson;
		TArray<TSharedPtr<FJsonValue>> SkeletonPathsJson;
		TArray<TSharedPtr<FJsonValue>> AnimSequencePathsJson;
		TArray<TSharedPtr<FJsonValue>> PhysicsAssetPathsJson;
		TSet<FString> SeenSkeletalMeshPaths;
		TSet<FString> SeenSkeletonPaths;
		TSet<FString> SeenAnimSequencePaths;
		TSet<FString> SeenPhysicsAssetPaths;

		UObject* PrimaryObject = nullptr;
		for (UObject* ImportedObject : ImportedObjects)
		{
			if (!ImportedObject)
			{
				continue;
			}

			if (!PrimaryObject)
			{
				PrimaryObject = ImportedObject;
			}

			const FString AssetPath = ImportedObject->GetOutermost() ? ImportedObject->GetOutermost()->GetName() : FString();
			const FString ObjectPath = ImportedObject->GetPathName();
			const FString ClassPath = ImportedObject->GetClass() ? ImportedObject->GetClass()->GetPathName() : FString();

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("asset_path"), AssetPath);
			Item->SetStringField(TEXT("object_path"), ObjectPath);
			Item->SetStringField(TEXT("name"), ImportedObject->GetName());
			Item->SetStringField(TEXT("class"), ClassPath);
			ImportedObjectsJson.Add(MakeShared<FJsonValueObject>(Item));
			ImportedObjectPathsJson.Add(MakeShared<FJsonValueString>(ObjectPath));

			if (ImportedObject->IsA<USkeletalMesh>())
			{
				if (!SeenSkeletalMeshPaths.Contains(AssetPath))
				{
					SeenSkeletalMeshPaths.Add(AssetPath);
					SkeletalMeshPathsJson.Add(MakeShared<FJsonValueString>(AssetPath));
				}
				if (const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(ImportedObject))
				{
					if (const USkeleton* Skeleton = SkeletalMesh->GetSkeleton())
					{
						const FString SkeletonAssetPath = Skeleton->GetOutermost() ? Skeleton->GetOutermost()->GetName() : FString();
						if (!SkeletonAssetPath.IsEmpty() && !SeenSkeletonPaths.Contains(SkeletonAssetPath))
						{
							SeenSkeletonPaths.Add(SkeletonAssetPath);
							SkeletonPathsJson.Add(MakeShared<FJsonValueString>(SkeletonAssetPath));
						}
					}
					if (UPhysicsAsset* PhysicsAsset = SkeletalMesh->GetPhysicsAsset())
					{
						const FString PhysicsAssetPath = PhysicsAsset->GetOutermost() ? PhysicsAsset->GetOutermost()->GetName() : FString();
						if (!PhysicsAssetPath.IsEmpty() && !SeenPhysicsAssetPaths.Contains(PhysicsAssetPath))
						{
							SeenPhysicsAssetPaths.Add(PhysicsAssetPath);
							PhysicsAssetPathsJson.Add(MakeShared<FJsonValueString>(PhysicsAssetPath));
						}
					}
				}
			}
			else if (ImportedObject->IsA<USkeleton>())
			{
				if (!SeenSkeletonPaths.Contains(AssetPath))
				{
					SeenSkeletonPaths.Add(AssetPath);
					SkeletonPathsJson.Add(MakeShared<FJsonValueString>(AssetPath));
				}
			}
			else if (ImportedObject->IsA<UAnimSequence>())
			{
				if (!SeenAnimSequencePaths.Contains(AssetPath))
				{
					SeenAnimSequencePaths.Add(AssetPath);
					AnimSequencePathsJson.Add(MakeShared<FJsonValueString>(AssetPath));
				}
			}
			else if (ImportedObject->IsA<UPhysicsAsset>())
			{
				if (!SeenPhysicsAssetPaths.Contains(AssetPath))
				{
					SeenPhysicsAssetPaths.Add(AssetPath);
					PhysicsAssetPathsJson.Add(MakeShared<FJsonValueString>(AssetPath));
				}
			}
		}

		OutData->SetNumberField(TEXT("imported_object_count"), ImportedObjectsJson.Num());
		OutData->SetArrayField(TEXT("imported_objects"), ImportedObjectsJson);
		OutData->SetArrayField(TEXT("imported_object_paths"), ImportedObjectPathsJson);
		OutData->SetArrayField(TEXT("skeletal_mesh_asset_paths"), SkeletalMeshPathsJson);
		OutData->SetArrayField(TEXT("skeleton_asset_paths"), SkeletonPathsJson);
		OutData->SetArrayField(TEXT("anim_sequence_asset_paths"), AnimSequencePathsJson);
		OutData->SetArrayField(TEXT("physics_asset_paths"), PhysicsAssetPathsJson);

		if (PrimaryObject)
		{
			OutData->SetStringField(TEXT("primary_asset_path"), PrimaryObject->GetOutermost()->GetName());
			OutData->SetStringField(TEXT("primary_object_path"), PrimaryObject->GetPathName());
			OutData->SetStringField(TEXT("primary_class"), PrimaryObject->GetClass() ? PrimaryObject->GetClass()->GetPathName() : TEXT(""));
		}
	}

	static bool ResolveDirtyResources(
		const FDirtyResolutionOptions& Options,
		UWorld* EditorWorld,
		FDirtyResolutionSummary& OutSummary,
		FString& OutError)
	{
		OutSummary = FDirtyResolutionSummary();

		if (Options.bSaveCurrentLevel && Options.bDiscardCurrentLevel)
		{
			OutError = TEXT("save_and_discard_current_level_conflict");
			return false;
		}

		TSet<UPackage*> DirtyPackagesBeforeSet;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (!Package || !Package->IsDirty() || ShouldSkipDirtyPackage(Package))
			{
				continue;
			}
			DirtyPackagesBeforeSet.Add(Package);
		}
		for (UPackage* Package : DirtyPackagesBeforeSet)
		{
			OutSummary.DirtyPackagesBefore.Add(Package);
		}
		OutSummary.DirtyPackagesBefore.Sort([](const UPackage& A, const UPackage& B)
		{
			return A.GetName() < B.GetName();
		});

		TSet<UPackage*> SavePackagesSet;
		TSet<UPackage*> DiscardPackagesSet;

		for (const FString& SavePath : Options.SaveResourcePaths)
		{
			UPackage* Package = ResolvePackageByAssetPath(SavePath);
			if (!Package)
			{
				OutSummary.SaveResourceNotFound.Add(SavePath);
				continue;
			}
			AddPackageIfValid(SavePackagesSet, Package);
		}

		for (const FString& DiscardPath : Options.DiscardResourcePaths)
		{
			UPackage* Package = ResolvePackageByAssetPath(DiscardPath);
			if (!Package)
			{
				OutSummary.DiscardResourceNotFound.Add(DiscardPath);
				continue;
			}
			AddPackageIfValid(DiscardPackagesSet, Package);
		}

		if (EditorWorld && (Options.bSaveCurrentLevel || Options.bDiscardCurrentLevel))
		{
			if (UPackage* WorldPackage = EditorWorld->GetOutermost())
			{
				if (Options.bSaveCurrentLevel)
				{
					SavePackagesSet.Add(WorldPackage);
				}
				if (Options.bDiscardCurrentLevel)
				{
					DiscardPackagesSet.Add(WorldPackage);
				}
			}
		}

		for (UPackage* DiscardPackage : DiscardPackagesSet)
		{
			SavePackagesSet.Remove(DiscardPackage);
		}

		if (Options.bSaveAllDirty)
		{
			for (UPackage* DirtyPackage : DirtyPackagesBeforeSet)
			{
				if (!DiscardPackagesSet.Contains(DirtyPackage))
				{
					SavePackagesSet.Add(DirtyPackage);
				}
			}
		}

		TArray<UPackage*> PackagesToSave;
		for (UPackage* Package : SavePackagesSet)
		{
			if (!Package)
			{
				continue;
			}
			if (Options.bOnlySaveDirty && !Package->IsDirty())
			{
				continue;
			}
			PackagesToSave.Add(Package);
		}

		if (PackagesToSave.Num() > 0)
		{
			if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
			{
				OutError = TEXT("save_packages_failed");
				return false;
			}
			OutSummary.SavedPackageCount = PackagesToSave.Num();
		}

		OutSummary.ExplicitDiscardedPackageCount = ClearDirtyFlagForPackages(DiscardPackagesSet);

		if (Options.bDiscardAllDirty)
		{
			TSet<UPackage*> RemainingDirty;
			for (TObjectIterator<UPackage> It; It; ++It)
			{
				UPackage* Package = *It;
				if (!Package || !Package->IsDirty() || ShouldSkipDirtyPackage(Package))
				{
					continue;
				}
				if (SavePackagesSet.Contains(Package))
				{
					continue;
				}
				RemainingDirty.Add(Package);
			}
			OutSummary.AutoDiscardedPackageCount = ClearDirtyFlagForPackages(RemainingDirty);
		}

		if (Options.bCloseAllAssetEditors && GEditor)
		{
			if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				AssetEditorSubsystem->CloseAllAssetEditors();
			}
		}

		CollectDirtyPackages(OutSummary.DirtyPackagesAfter);
		return true;
	}
}

bool FUeAgentHttpServer::CmdEditorGetOpenAssets(TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!AssetEditorSubsystem)
	{
		OutError = TEXT("missing_asset_editor_subsystem");
		return false;
	}
	const TArray<UObject*> OpenAssets = AssetEditorSubsystem->GetAllEditedAssets();
	TArray<TSharedPtr<FJsonValue>> Assets;
	for (UObject* Asset : OpenAssets)
	{
		if (!Asset)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Asset->GetName());
		Item->SetStringField(TEXT("object_path"), Asset->GetPathName());
		Item->SetStringField(TEXT("class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
		Assets.Add(MakeShared<FJsonValueObject>(Item));
	}
	OutData->SetArrayField(TEXT("open_assets"), Assets);
	OutData->SetNumberField(TEXT("count"), Assets.Num());
	return true;
}

bool FUeAgentHttpServer::CmdOpenAssetEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!AssetEditorSubsystem)
	{
		OutError = TEXT("missing_asset_editor_subsystem");
		return false;
	}
	AssetEditorSubsystem->OpenEditorForAsset(Asset);

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	return true;
}

bool FUeAgentHttpServer::CmdSaveAsset(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	UPackage* Package = Asset->GetOutermost();
	if (!Package)
	{
		OutError = TEXT("missing_package");
		return false;
	}

	bool bOnlyIfDirty = true;
	JsonTryGetBool(Ctx.Params, TEXT("only_if_dirty"), bOnlyIfDirty);
	const bool bDirty = Package->IsDirty();
	if (bOnlyIfDirty && !bDirty)
	{
		OutData->SetBoolField(TEXT("saved"), false);
		OutData->SetStringField(TEXT("reason"), TEXT("not_dirty"));
		OutData->SetStringField(TEXT("asset_path"), Package->GetName());
		return true;
	}

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);
	if (!bSaved)
	{
		OutError = TEXT("save_asset_failed");
		return false;
	}

	OutData->SetBoolField(TEXT("saved"), true);
	OutData->SetStringField(TEXT("asset_path"), Package->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetBoolField(TEXT("was_dirty"), bDirty);
	return true;
}

bool FUeAgentHttpServer::CmdAssetDuplicate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SourceAssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_asset_path"), SourceAssetPath) || SourceAssetPath.IsEmpty())
	{
		OutError = TEXT("missing_source_asset_path");
		return false;
	}

	FString DestinationAssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("destination_asset_path"), DestinationAssetPath) || DestinationAssetPath.IsEmpty())
	{
		OutError = TEXT("missing_destination_asset_path");
		return false;
	}

	UObject* SourceAsset = UeAgentAssetOps::LoadAssetObject(SourceAssetPath);
	if (!SourceAsset)
	{
		OutError = TEXT("source_asset_not_found");
		return false;
	}

	const FString NormalizedDestinationAssetPath = UeAgentAssetOps::NormalizeAssetPath(DestinationAssetPath);
	if (!FPackageName::IsValidLongPackageName(NormalizedDestinationAssetPath))
	{
		OutError = TEXT("invalid_destination_asset_path");
		return false;
	}

	const FString DestinationAssetName = FPackageName::GetLongPackageAssetName(NormalizedDestinationAssetPath);
	const FString DestinationPackagePath = FPackageName::GetLongPackagePath(NormalizedDestinationAssetPath);
	if (DestinationAssetName.IsEmpty() || DestinationPackagePath.IsEmpty())
	{
		OutError = TEXT("invalid_destination_asset_path");
		return false;
	}

	if (UeAgentAssetOps::LoadAssetObject(NormalizedDestinationAssetPath))
	{
		OutError = TEXT("destination_asset_already_exists");
		return false;
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UObject* DuplicatedAsset = AssetToolsModule.Get().DuplicateAsset(DestinationAssetName, DestinationPackagePath, SourceAsset);
	if (!DuplicatedAsset)
	{
		OutError = TEXT("duplicate_asset_failed");
		return false;
	}

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
		{
			AssetEditorSubsystem->OpenEditorForAsset(DuplicatedAsset);
		}
	}

	bool bSaveAfterDuplicate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_duplicate"), bSaveAfterDuplicate);
	if (bSaveAfterDuplicate)
	{
		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(DuplicatedAsset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
	}

	OutData->SetStringField(TEXT("source_asset_path"), UeAgentAssetOps::NormalizeAssetPath(SourceAsset->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("asset_path"), NormalizedDestinationAssetPath);
	OutData->SetStringField(TEXT("object_path"), DuplicatedAsset->GetPathName());
	OutData->SetStringField(TEXT("class"), DuplicatedAsset->GetClass()->GetPathName());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterDuplicate);
	return true;
}

bool FUeAgentHttpServer::CmdAssetImportTexture(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SourceFilenameInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_filename"), SourceFilenameInput) || SourceFilenameInput.IsEmpty())
	{
		OutError = TEXT("missing_source_filename");
		return false;
	}

	FString DestinationPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("destination_path"), DestinationPathInput) || DestinationPathInput.IsEmpty())
	{
		OutError = TEXT("missing_destination_path");
		return false;
	}

	const TSet<FString> TextureExtensions = {
		TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("tga"), TEXT("bmp"),
		TEXT("exr"), TEXT("hdr"), TEXT("dds"), TEXT("psd"), TEXT("tif"), TEXT("tiff")
	};

	FString SourceFilename;
	if (!UeAgentAssetOps::ResolveImportSourceFilenameWithExtensions(
		SourceFilenameInput,
		TextureExtensions,
		TEXT("source_file_is_not_supported_texture_extension"),
		SourceFilename,
		OutError))
	{
		return false;
	}

	FString DestinationPath;
	if (!UeAgentAssetOps::ResolveImportDestinationPath(DestinationPathInput, DestinationPath, OutError))
	{
		return false;
	}

	FString DestinationName;
	JsonTryGetString(Ctx.Params, TEXT("destination_name"), DestinationName);
	DestinationName.TrimStartAndEndInline();
	if (!DestinationName.IsEmpty())
	{
		const FString DestinationAssetPath = DestinationPath / DestinationName;
		if (!FPackageName::IsValidLongPackageName(DestinationAssetPath))
		{
			OutError = TEXT("invalid_destination_name");
			return false;
		}
	}

	bool bReplaceExisting = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("replace_existing")) && !JsonTryGetBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting))
	{
		OutError = TEXT("invalid_replace_existing");
		return false;
	}

	bool bReplaceExistingSettings = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("replace_existing_settings")) && !JsonTryGetBool(Ctx.Params, TEXT("replace_existing_settings"), bReplaceExistingSettings))
	{
		OutError = TEXT("invalid_replace_existing_settings");
		return false;
	}

	bool bSaveAfterImport = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("save_after_import")) && !JsonTryGetBool(Ctx.Params, TEXT("save_after_import"), bSaveAfterImport))
	{
		OutError = TEXT("invalid_save_after_import");
		return false;
	}

	bool bOpenEditor = false;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("open_editor")) && !JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor))
	{
		OutError = TEXT("invalid_open_editor");
		return false;
	}

	UTextureFactory* TextureFactory = NewObject<UTextureFactory>();
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	if (!TextureFactory || !ImportData)
	{
		OutError = TEXT("create_texture_import_task_failed");
		return false;
	}

	TextureFactory->AddToRoot();
	ImportData->AddToRoot();

	bool bHasSRGB = false;
	bool bSRGB = true;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("srgb")))
	{
		bHasSRGB = true;
		if (!JsonTryGetBool(Ctx.Params, TEXT("srgb"), bSRGB))
		{
			ImportData->RemoveFromRoot();
			TextureFactory->RemoveFromRoot();
			OutError = TEXT("invalid_srgb");
			return false;
		}
		TextureFactory->ColorSpaceMode = bSRGB ? ETextureSourceColorSpace::SRGB : ETextureSourceColorSpace::Linear;
	}

	auto ReadOptionalBool = [&](const TCHAR* FieldName, bool& InOutValue) -> bool
	{
		if (!Ctx.Params.IsValid() || !Ctx.Params->HasField(FieldName))
		{
			return true;
		}
		if (!JsonTryGetBool(Ctx.Params, FieldName, InOutValue))
		{
			OutError = FString::Printf(TEXT("invalid_%s"), FieldName);
			return false;
		}
		return true;
	};

	bool bNoCompression = false;
	if (!ReadOptionalBool(TEXT("no_compression"), bNoCompression))
	{
		ImportData->RemoveFromRoot();
		TextureFactory->RemoveFromRoot();
		return false;
	}
	TextureFactory->NoCompression = bNoCompression;

	bool bNoAlpha = false;
	if (!ReadOptionalBool(TEXT("no_alpha"), bNoAlpha))
	{
		ImportData->RemoveFromRoot();
		TextureFactory->RemoveFromRoot();
		return false;
	}
	TextureFactory->NoAlpha = bNoAlpha;

	bool bDeferCompression = false;
	if (!ReadOptionalBool(TEXT("defer_compression"), bDeferCompression))
	{
		ImportData->RemoveFromRoot();
		TextureFactory->RemoveFromRoot();
		return false;
	}
	TextureFactory->bDeferCompression = bDeferCompression;

	bool bCreateMaterial = false;
	if (!ReadOptionalBool(TEXT("create_material"), bCreateMaterial))
	{
		ImportData->RemoveFromRoot();
		TextureFactory->RemoveFromRoot();
		return false;
	}
	TextureFactory->bCreateMaterial = bCreateMaterial;

	bool bHasCompressionSettings = false;
	TextureCompressionSettings CompressionSettings = TC_Default;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("compression_settings")))
	{
		FString CompressionSettingsText;
		if (!JsonTryGetString(Ctx.Params, TEXT("compression_settings"), CompressionSettingsText)
			|| !UeAgentAssetOps::TryParseEnumValue<TextureCompressionSettings>(CompressionSettingsText, TEXT("compression_settings"), CompressionSettings, OutError))
		{
			ImportData->RemoveFromRoot();
			TextureFactory->RemoveFromRoot();
			return false;
		}
		bHasCompressionSettings = true;
		TextureFactory->CompressionSettings = CompressionSettings;
	}

	bool bHasMipGenSettings = false;
	TextureMipGenSettings MipGenSettings = TMGS_FromTextureGroup;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("mip_gen_settings")))
	{
		FString MipGenSettingsText;
		if (!JsonTryGetString(Ctx.Params, TEXT("mip_gen_settings"), MipGenSettingsText)
			|| !UeAgentAssetOps::TryParseEnumValue<TextureMipGenSettings>(MipGenSettingsText, TEXT("mip_gen_settings"), MipGenSettings, OutError))
		{
			ImportData->RemoveFromRoot();
			TextureFactory->RemoveFromRoot();
			return false;
		}
		bHasMipGenSettings = true;
		TextureFactory->MipGenSettings = MipGenSettings;
	}

	bool bHasLODGroup = false;
	TextureGroup LODGroup = TEXTUREGROUP_World;
	if (Ctx.Params.IsValid() && Ctx.Params->HasField(TEXT("lod_group")))
	{
		FString LODGroupText;
		if (!JsonTryGetString(Ctx.Params, TEXT("lod_group"), LODGroupText)
			|| !UeAgentAssetOps::TryParseEnumValue<TextureGroup>(LODGroupText, TEXT("lod_group"), LODGroup, OutError))
		{
			ImportData->RemoveFromRoot();
			TextureFactory->RemoveFromRoot();
			return false;
		}
		bHasLODGroup = true;
		TextureFactory->LODGroup = LODGroup;
	}

	if (!TextureFactory->FactoryCanImport(SourceFilename))
	{
		ImportData->RemoveFromRoot();
		TextureFactory->RemoveFromRoot();
		OutError = TEXT("source_file_not_supported_by_texture_factory");
		return false;
	}

	UTextureFactory::SuppressImportOverwriteDialog(bReplaceExistingSettings);
	ImportData->Filenames = { SourceFilename };
	ImportData->DestinationPath = DestinationPath;
	ImportData->FactoryName = TextureFactory->GetClass()->GetName();
	ImportData->bReplaceExisting = bReplaceExisting;
	ImportData->Factory = TextureFactory;

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<UObject*> ImportedObjects = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);

	TArray<UTexture2D*> ImportedTextures;
	for (UObject* ImportedObject : ImportedObjects)
	{
		if (UTexture2D* Texture = Cast<UTexture2D>(ImportedObject))
		{
			ImportedTextures.AddUnique(Texture);
		}
	}

	if (ImportedTextures.Num() <= 0)
	{
		UeAgentAssetOps::FillImportedObjectsResponse(ImportedObjects, OutData);
		OutData->SetStringField(TEXT("source_filename"), SourceFilename);
		OutData->SetStringField(TEXT("destination_path"), DestinationPath);
		OutData->SetStringField(TEXT("destination_name"), DestinationName);
		ImportData->RemoveFromRoot();
		TextureFactory->RemoveFromRoot();
		OutError = TEXT("texture_import_created_no_texture2d");
		return false;
	}

	for (UTexture2D* Texture : ImportedTextures)
	{
		if (!Texture)
		{
			continue;
		}
		if (bHasSRGB)
		{
			Texture->SRGB = bSRGB;
		}
		if (bHasCompressionSettings)
		{
			Texture->CompressionSettings = CompressionSettings;
		}
		if (bHasMipGenSettings)
		{
			Texture->MipGenSettings = MipGenSettings;
		}
		if (bHasLODGroup)
		{
			Texture->LODGroup = LODGroup;
		}
		Texture->PostEditChange();
		Texture->MarkPackageDirty();
	}

	if (bSaveAfterImport)
	{
		TArray<UPackage*> PackagesToSave;
		for (UObject* ImportedObject : ImportedObjects)
		{
			if (ImportedObject && ImportedObject->GetOutermost())
			{
				PackagesToSave.AddUnique(ImportedObject->GetOutermost());
			}
		}
		for (UTexture2D* Texture : ImportedTextures)
		{
			if (Texture && Texture->GetOutermost())
			{
				PackagesToSave.AddUnique(Texture->GetOutermost());
			}
		}
		if (PackagesToSave.Num() > 0 && !UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true))
		{
			ImportData->RemoveFromRoot();
			TextureFactory->RemoveFromRoot();
			OutError = TEXT("save_asset_failed");
			return false;
		}
	}

	UeAgentAssetOps::FillImportedObjectsResponse(ImportedObjects, OutData);
	OutData->SetStringField(TEXT("source_filename"), SourceFilename);
	OutData->SetStringField(TEXT("destination_path"), DestinationPath);
	OutData->SetStringField(TEXT("destination_name"), DestinationName);
	OutData->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
	OutData->SetBoolField(TEXT("replace_existing_settings"), bReplaceExistingSettings);
	OutData->SetBoolField(TEXT("save_after_import"), bSaveAfterImport);
	OutData->SetBoolField(TEXT("factory_can_import"), true);
	OutData->SetStringField(TEXT("factory_class"), TextureFactory->GetClass()->GetPathName());

	TArray<TSharedPtr<FJsonValue>> TextureAssetPathsJson;
	TArray<TSharedPtr<FJsonValue>> TextureObjectPathsJson;
	for (UTexture2D* Texture : ImportedTextures)
	{
		if (!Texture)
		{
			continue;
		}
		TextureAssetPathsJson.Add(MakeShared<FJsonValueString>(Texture->GetOutermost() ? Texture->GetOutermost()->GetName() : FString()));
		TextureObjectPathsJson.Add(MakeShared<FJsonValueString>(Texture->GetPathName()));
	}
	OutData->SetNumberField(TEXT("texture_count"), ImportedTextures.Num());
	OutData->SetArrayField(TEXT("texture_asset_paths"), TextureAssetPathsJson);
	OutData->SetArrayField(TEXT("texture_object_paths"), TextureObjectPathsJson);

	if (UTexture2D* PrimaryTexture = ImportedTextures[0])
	{
		OutData->SetStringField(TEXT("texture_asset_path"), PrimaryTexture->GetOutermost() ? PrimaryTexture->GetOutermost()->GetName() : FString());
		OutData->SetStringField(TEXT("texture_object_path"), PrimaryTexture->GetPathName());
		OutData->SetNumberField(TEXT("texture_size_x"), PrimaryTexture->GetSizeX());
		OutData->SetNumberField(TEXT("texture_size_y"), PrimaryTexture->GetSizeY());
		OutData->SetNumberField(TEXT("texture_num_mips"), PrimaryTexture->GetNumMips());
		OutData->SetBoolField(TEXT("texture_has_alpha"), PrimaryTexture->HasAlphaChannel());
		OutData->SetBoolField(TEXT("srgb"), PrimaryTexture->SRGB != 0);
		OutData->SetStringField(TEXT("compression_settings"), UeAgentAssetOps::EnumValueToString<TextureCompressionSettings>(static_cast<TextureCompressionSettings>(PrimaryTexture->CompressionSettings.GetValue())));
		OutData->SetStringField(TEXT("mip_gen_settings"), UeAgentAssetOps::EnumValueToString<TextureMipGenSettings>(static_cast<TextureMipGenSettings>(PrimaryTexture->MipGenSettings.GetValue())));
		OutData->SetStringField(TEXT("lod_group"), UeAgentAssetOps::EnumValueToString<TextureGroup>(static_cast<TextureGroup>(PrimaryTexture->LODGroup.GetValue())));
	}

	if (bOpenEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
		{
			AssetEditorSubsystem->OpenEditorForAsset(ImportedTextures[0]);
		}
	}

	ImportData->RemoveFromRoot();
	TextureFactory->RemoveFromRoot();
	return true;
}

bool FUeAgentHttpServer::CmdAssetImportFbxSkeletalMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SourceFilenameInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_filename"), SourceFilenameInput) || SourceFilenameInput.IsEmpty())
	{
		OutError = TEXT("missing_source_filename");
		return false;
	}

	FString DestinationPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("destination_path"), DestinationPathInput) || DestinationPathInput.IsEmpty())
	{
		OutError = TEXT("missing_destination_path");
		return false;
	}

	FString SourceFilename;
	if (!UeAgentAssetOps::ResolveImportSourceFilename(SourceFilenameInput, SourceFilename, OutError))
	{
		return false;
	}

	FString DestinationPath;
	if (!UeAgentAssetOps::ResolveImportDestinationPath(DestinationPathInput, DestinationPath, OutError))
	{
		return false;
	}

	FString DestinationName;
	JsonTryGetString(Ctx.Params, TEXT("destination_name"), DestinationName);
	DestinationName.TrimStartAndEndInline();

	bool bReplaceExisting = true;
	JsonTryGetBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting);

	bool bReplaceExistingSettings = true;
	JsonTryGetBool(Ctx.Params, TEXT("replace_existing_settings"), bReplaceExistingSettings);

	bool bSaveAfterImport = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_import"), bSaveAfterImport);

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);

	bool bImportMaterials = false;
	JsonTryGetBool(Ctx.Params, TEXT("import_materials"), bImportMaterials);

	bool bImportTextures = false;
	JsonTryGetBool(Ctx.Params, TEXT("import_textures"), bImportTextures);

	bool bCreatePhysicsAsset = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_physics_asset"), bCreatePhysicsAsset);

	bool bImportAnimations = false;
	JsonTryGetBool(Ctx.Params, TEXT("import_animations"), bImportAnimations);

	FString SkeletonPath;
	JsonTryGetString(Ctx.Params, TEXT("skeleton_path"), SkeletonPath);
	SkeletonPath.TrimStartAndEndInline();

	USkeleton* ExistingSkeleton = nullptr;
	if (!SkeletonPath.IsEmpty())
	{
		ExistingSkeleton = Cast<USkeleton>(UeAgentAssetOps::LoadAssetObject(SkeletonPath));
		if (!ExistingSkeleton)
		{
			OutError = TEXT("skeleton_not_found");
			return false;
		}
	}

	UFbxFactory* FbxFactory = NewObject<UFbxFactory>();
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	if (!FbxFactory || !ImportData || !FbxFactory->ImportUI)
	{
		OutError = TEXT("create_fbx_import_task_failed");
		return false;
	}

	FbxFactory->AddToRoot();
	ImportData->AddToRoot();

	FbxFactory->SetDetectImportTypeOnImport(false);
	FbxFactory->ImportUI->bAutomatedImportShouldDetectType = false;
	FbxFactory->ImportUI->bImportAsSkeletal = true;
	FbxFactory->ImportUI->MeshTypeToImport = FBXIT_SkeletalMesh;
	FbxFactory->ImportUI->OriginalImportType = FBXIT_SkeletalMesh;
	FbxFactory->ImportUI->bIsReimport = false;
	FbxFactory->ImportUI->ReimportMesh = nullptr;
	FbxFactory->ImportUI->bAllowContentTypeImport = true;
	FbxFactory->ImportUI->bImportAnimations = bImportAnimations;
	FbxFactory->ImportUI->bCreatePhysicsAsset = bCreatePhysicsAsset;
	FbxFactory->ImportUI->bImportMaterials = bImportMaterials;
	FbxFactory->ImportUI->bImportTextures = bImportTextures;
	FbxFactory->ImportUI->bImportMesh = true;
	FbxFactory->ImportUI->bImportRigidMesh = false;
	FbxFactory->ImportUI->bIsObjImport = false;
	FbxFactory->ImportUI->bOverrideFullName = true;
	FbxFactory->ImportUI->Skeleton = ExistingSkeleton;

	if (FbxFactory->ImportUI->SkeletalMeshImportData)
	{
		FbxFactory->ImportUI->SkeletalMeshImportData->bImportMeshLODs = false;
		FbxFactory->ImportUI->SkeletalMeshImportData->bImportMorphTargets = false;
		FbxFactory->ImportUI->SkeletalMeshImportData->bUpdateSkeletonReferencePose = false;
		FbxFactory->ImportUI->SkeletalMeshImportData->ImportContentType = EFBXImportContentType::FBXICT_All;
	}

	ImportData->Filenames = { SourceFilename };
	ImportData->DestinationPath = DestinationPath;
	ImportData->FactoryName = FbxFactory->GetClass()->GetName();
	ImportData->bReplaceExisting = bReplaceExisting;
	ImportData->Factory = FbxFactory;

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<UObject*> ImportedObjects = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);

	if (bSaveAfterImport && ImportedObjects.Num() > 0)
	{
		TArray<UPackage*> PackagesToSave;
		for (UObject* ImportedObject : ImportedObjects)
		{
			if (ImportedObject && ImportedObject->GetOutermost())
			{
				PackagesToSave.AddUnique(ImportedObject->GetOutermost());
			}
		}
		if (PackagesToSave.Num() > 0 && !UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true))
		{
			ImportData->RemoveFromRoot();
			FbxFactory->RemoveFromRoot();
			OutError = TEXT("save_asset_failed");
			return false;
		}
	}

	UeAgentAssetOps::FillImportedObjectsResponse(ImportedObjects, OutData);
	OutData->SetStringField(TEXT("source_filename"), SourceFilename);
	OutData->SetStringField(TEXT("destination_path"), DestinationPath);
	OutData->SetStringField(TEXT("destination_name"), DestinationName);
	OutData->SetBoolField(TEXT("replace_existing_settings"), bReplaceExistingSettings);
	OutData->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
	OutData->SetBoolField(TEXT("save_after_import"), bSaveAfterImport);

	if (OutData->GetIntegerField(TEXT("imported_object_count")) <= 0)
	{
		ImportData->RemoveFromRoot();
		FbxFactory->RemoveFromRoot();
		OutError = TEXT("fbx_import_created_no_assets");
		return false;
	}

	if (bOpenEditor)
	{
		TArray<UObject*> ImportedObjectsUnique;
		UeAgentAssetOps::GatherImportedObjects(ImportedObjects, ImportedObjectsUnique);
		for (UObject* ImportedObject : ImportedObjectsUnique)
		{
			if (ImportedObject && ImportedObject->IsA<USkeletalMesh>())
			{
				if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
				{
					AssetEditorSubsystem->OpenEditorForAsset(ImportedObject);
				}
				break;
			}
		}
	}

	ImportData->RemoveFromRoot();
	FbxFactory->RemoveFromRoot();
	return true;
}

bool FUeAgentHttpServer::CmdAssetImportFbxAnimation(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SourceFilenameInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_filename"), SourceFilenameInput) || SourceFilenameInput.IsEmpty())
	{
		OutError = TEXT("missing_source_filename");
		return false;
	}

	FString DestinationPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("destination_path"), DestinationPathInput) || DestinationPathInput.IsEmpty())
	{
		OutError = TEXT("missing_destination_path");
		return false;
	}

	FString SkeletonPath;
	JsonTryGetString(Ctx.Params, TEXT("skeleton_path"), SkeletonPath);
	SkeletonPath.TrimStartAndEndInline();

	FString SkeletalMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("skeletal_mesh_path"), SkeletalMeshPath);
	SkeletalMeshPath.TrimStartAndEndInline();

	if (SkeletonPath.IsEmpty() && SkeletalMeshPath.IsEmpty())
	{
		OutError = TEXT("missing_skeleton_or_skeletal_mesh_path");
		return false;
	}

	FString SourceFilename;
	if (!UeAgentAssetOps::ResolveImportSourceFilename(SourceFilenameInput, SourceFilename, OutError))
	{
		return false;
	}

	FString DestinationPath;
	if (!UeAgentAssetOps::ResolveImportDestinationPath(DestinationPathInput, DestinationPath, OutError))
	{
		return false;
	}

	USkeleton* TargetSkeleton = nullptr;
	if (!SkeletonPath.IsEmpty())
	{
		TargetSkeleton = Cast<USkeleton>(UeAgentAssetOps::LoadAssetObject(SkeletonPath));
	}
	if (!TargetSkeleton && !SkeletalMeshPath.IsEmpty())
	{
		if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(UeAgentAssetOps::LoadAssetObject(SkeletalMeshPath)))
		{
			TargetSkeleton = SkeletalMesh->GetSkeleton();
			if (TargetSkeleton && SkeletonPath.IsEmpty())
			{
				SkeletonPath = TargetSkeleton->GetOutermost() ? TargetSkeleton->GetOutermost()->GetName() : FString();
			}
		}
	}
	if (!TargetSkeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	FString DestinationName;
	JsonTryGetString(Ctx.Params, TEXT("destination_name"), DestinationName);
	DestinationName.TrimStartAndEndInline();

	bool bReplaceExisting = true;
	JsonTryGetBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting);

	bool bReplaceExistingSettings = true;
	JsonTryGetBool(Ctx.Params, TEXT("replace_existing_settings"), bReplaceExistingSettings);

	bool bSaveAfterImport = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_import"), bSaveAfterImport);

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);

	UFbxFactory* FbxFactory = NewObject<UFbxFactory>();
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	if (!FbxFactory || !ImportData || !FbxFactory->ImportUI)
	{
		OutError = TEXT("create_fbx_import_task_failed");
		return false;
	}

	FbxFactory->AddToRoot();
	ImportData->AddToRoot();

	FbxFactory->SetDetectImportTypeOnImport(false);
	FbxFactory->ImportUI->bAutomatedImportShouldDetectType = false;
	FbxFactory->ImportUI->bImportAsSkeletal = true;
	FbxFactory->ImportUI->MeshTypeToImport = FBXIT_Animation;
	FbxFactory->ImportUI->OriginalImportType = FBXIT_Animation;
	FbxFactory->ImportUI->bIsReimport = false;
	FbxFactory->ImportUI->ReimportMesh = nullptr;
	FbxFactory->ImportUI->bAllowContentTypeImport = true;
	FbxFactory->ImportUI->bImportAnimations = true;
	FbxFactory->ImportUI->bCreatePhysicsAsset = false;
	FbxFactory->ImportUI->bImportMaterials = false;
	FbxFactory->ImportUI->bImportTextures = false;
	FbxFactory->ImportUI->bImportMesh = false;
	FbxFactory->ImportUI->bImportRigidMesh = false;
	FbxFactory->ImportUI->bIsObjImport = false;
	FbxFactory->ImportUI->bOverrideFullName = true;
	FbxFactory->ImportUI->Skeleton = TargetSkeleton;
	if (!DestinationName.IsEmpty())
	{
		FbxFactory->ImportUI->OverrideAnimationName = DestinationName;
	}

	ImportData->Filenames = { SourceFilename };
	ImportData->DestinationPath = DestinationPath;
	ImportData->FactoryName = FbxFactory->GetClass()->GetName();
	ImportData->bReplaceExisting = bReplaceExisting;
	ImportData->Factory = FbxFactory;

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<UObject*> ImportedObjects = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);

	if (bSaveAfterImport && ImportedObjects.Num() > 0)
	{
		TArray<UPackage*> PackagesToSave;
		for (UObject* ImportedObject : ImportedObjects)
		{
			if (ImportedObject && ImportedObject->GetOutermost())
			{
				PackagesToSave.AddUnique(ImportedObject->GetOutermost());
			}
		}
		if (PackagesToSave.Num() > 0 && !UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true))
		{
			ImportData->RemoveFromRoot();
			FbxFactory->RemoveFromRoot();
			OutError = TEXT("save_asset_failed");
			return false;
		}
	}

	UeAgentAssetOps::FillImportedObjectsResponse(ImportedObjects, OutData);
	OutData->SetStringField(TEXT("source_filename"), SourceFilename);
	OutData->SetStringField(TEXT("destination_path"), DestinationPath);
	OutData->SetStringField(TEXT("destination_name"), DestinationName);
	OutData->SetStringField(TEXT("skeleton_path"), TargetSkeleton->GetOutermost()->GetName());
	if (!SkeletalMeshPath.IsEmpty())
	{
		OutData->SetStringField(TEXT("skeletal_mesh_path"), SkeletalMeshPath);
	}
	OutData->SetBoolField(TEXT("replace_existing_settings"), bReplaceExistingSettings);
	OutData->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
	OutData->SetBoolField(TEXT("save_after_import"), bSaveAfterImport);

	if (OutData->GetIntegerField(TEXT("imported_object_count")) <= 0)
	{
		ImportData->RemoveFromRoot();
		FbxFactory->RemoveFromRoot();
		OutError = TEXT("fbx_import_created_no_assets");
		return false;
	}

	if (bOpenEditor)
	{
		TArray<UObject*> ImportedObjectsUnique;
		UeAgentAssetOps::GatherImportedObjects(ImportedObjects, ImportedObjectsUnique);
		for (UObject* ImportedObject : ImportedObjectsUnique)
		{
			if (ImportedObject && ImportedObject->IsA<UAnimSequence>())
			{
				if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
				{
					AssetEditorSubsystem->OpenEditorForAsset(ImportedObject);
				}
				break;
			}
		}
	}

	ImportData->RemoveFromRoot();
	FbxFactory->RemoveFromRoot();
	return true;
}

bool FUeAgentHttpServer::CmdAssetExportPropertyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	TArray<FString> PropertyNames;
	const bool bHasExplicitPropertyNames = UeAgentAssetOps::ReadStringArrayParam(Ctx.Params, TEXT("property_names"), PropertyNames) && PropertyNames.Num() > 0;
	FString PresetName;
	if (!bHasExplicitPropertyNames)
	{
		UeAgentAssetOps::GetDefaultAssetPropertyPaths(Asset, PropertyNames, PresetName);
	}

	if (PropertyNames.Num() == 0)
	{
		OutError = TEXT("missing_property_names_and_no_default_preset");
		return false;
	}

	PropertyNames.Sort();
	for (int32 Index = PropertyNames.Num() - 1; Index > 0; --Index)
	{
		if (PropertyNames[Index].Equals(PropertyNames[Index - 1], ESearchCase::CaseSensitive))
		{
			PropertyNames.RemoveAt(Index);
		}
	}

	TArray<TSharedPtr<FJsonValue>> PropertiesJson;
	TArray<TSharedPtr<FJsonValue>> MissingPropertiesJson;
	int32 ExportedPropertyCount = 0;
	for (const FString& PropertyPath : PropertyNames)
	{
		FString ValueText;
		FString CppType;
		if (!UeAgentAssetOps::GetObjectPropertyText(Asset, PropertyPath, ValueText, CppType))
		{
			MissingPropertiesJson.Add(MakeShared<FJsonValueString>(PropertyPath));
			continue;
		}

		TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
		PropertyObj->SetStringField(TEXT("property_name"), PropertyPath);
		PropertyObj->SetStringField(TEXT("value_text"), ValueText);
		if (!CppType.IsEmpty())
		{
			PropertyObj->SetStringField(TEXT("cpp_type"), CppType);
		}
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (UeAgentAssetOps::ResolvePropertyPath(Asset, PropertyPath, Property, ValuePtr) && Property && ValuePtr)
		{
			TSharedPtr<FJsonObject> CurveJson;
			if (UeAgentCurveJson::TryBuildPropertyCurveJson(Property, ValuePtr, CurveJson) && CurveJson.IsValid())
			{
				PropertyObj->SetObjectField(TEXT("value_json"), CurveJson);
				PropertyObj->SetObjectField(TEXT("curve_json"), CurveJson);
				PropertyObj->SetStringField(TEXT("value_json_schema"), UeAgentCurveJson::SchemaName);
			}
		}
		PropertiesJson.Add(MakeShared<FJsonValueObject>(PropertyObj));
		++ExportedPropertyCount;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("format_version"), 1);
	Root->SetStringField(TEXT("asset_path"), UeAgentAssetOps::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	Root->SetStringField(TEXT("object_path"), Asset->GetPathName());
	Root->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	if (!PresetName.IsEmpty())
	{
		Root->SetStringField(TEXT("property_preset"), PresetName);
	}
	Root->SetArrayField(TEXT("properties"), PropertiesJson);
	if (MissingPropertiesJson.Num() > 0)
	{
		Root->SetArrayField(TEXT("missing_properties"), MissingPropertiesJson);
	}

	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty())
	{
		if (!UeAgentAssetOps::SaveJsonFile(OutputFile, Root, SavedOutputFile, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentAssetOps::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	if (!PresetName.IsEmpty())
	{
		OutData->SetStringField(TEXT("property_preset"), PresetName);
	}
	OutData->SetArrayField(TEXT("properties"), PropertiesJson);
	OutData->SetNumberField(TEXT("property_count"), ExportedPropertyCount);
	if (MissingPropertiesJson.Num() > 0)
	{
		OutData->SetArrayField(TEXT("missing_properties"), MissingPropertiesJson);
		OutData->SetNumberField(TEXT("missing_property_count"), MissingPropertiesJson.Num());
	}
	if (!SavedOutputFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdAssetApplyPropertyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> JsonRoot;
	FString JsonFile;
	FString ResolvedJsonFile;
	JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile);
	JsonFile.TrimStartAndEndInline();
	if (!JsonFile.IsEmpty())
	{
		if (!UeAgentAssetOps::LoadJsonFile(JsonFile, JsonRoot, ResolvedJsonFile, OutError))
		{
			return false;
		}
	}

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty() && JsonRoot.IsValid())
	{
		JsonRoot->TryGetStringField(TEXT("asset_path"), AssetPathInput);
	}
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
	if (!Ctx.Params->TryGetArrayField(TEXT("properties"), PropertiesArray) || !PropertiesArray)
	{
		if (!JsonRoot.IsValid() || !JsonRoot->TryGetArrayField(TEXT("properties"), PropertiesArray) || !PropertiesArray)
		{
			OutError = TEXT("missing_properties");
			return false;
		}
	}

	int32 AppliedPropertyCount = 0;
	TArray<TSharedPtr<FJsonValue>> PropertyResultsJson;
	int32 PropertyIndex = 0;
	for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
	{
		const FString PropertyPath = FString::Printf(TEXT("properties[%d]"), PropertyIndex++);
		TArray<TSharedPtr<FJsonValue>> PropertyJsonIssues;
		TSharedPtr<FJsonObject> PropertyObj;
		if (!UeAgentJsonDiagnostics::ReadObjectFromValue(PropertyValue, PropertyPath, PropertyObj, PropertyJsonIssues))
		{
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(TEXT(""), nullptr, TEXT(""), TEXT(""), TEXT("invalid_property_entry"));
			ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
			OutError = TEXT("invalid_property_entry");
			return false;
		}

		UeAgentJsonDiagnostics::WarnUnknownFields(
			PropertyObj,
			PropertyPath,
			{ TEXT("property_name"), TEXT("value_text"), TEXT("cpp_type"), TEXT("value_json"), TEXT("curve_json"), TEXT("value_json_schema") },
			PropertyJsonIssues);

		FString PropertyName;
		FString ValueText;
		if (!UeAgentJsonDiagnostics::ReadStringField(PropertyObj, TEXT("property_name"), PropertyPath + TEXT(".property_name"), PropertyName, PropertyJsonIssues, true)
			|| PropertyName.IsEmpty())
		{
			if (PropertyName.IsEmpty())
			{
				UeAgentJsonDiagnostics::AddIssue(PropertyJsonIssues, TEXT("warning"), TEXT("json_missing_required_field"), PropertyPath + TEXT(".property_name"), TEXT("Required string field is empty."));
			}
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(TEXT(""), nullptr, TEXT(""), TEXT(""), TEXT("missing_property_name"));
			ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
			OutError = TEXT("missing_property_name");
			return false;
		}

		TSharedPtr<FJsonObject> TypedCurveJson;
		bool bHasTypedCurveJson = false;
		const TSharedPtr<FJsonObject>* CurveJsonPtr = nullptr;
		FString TypedCurveJsonFieldName;
		if (PropertyObj->TryGetObjectField(TEXT("curve_json"), CurveJsonPtr) && CurveJsonPtr && CurveJsonPtr->IsValid())
		{
			TypedCurveJson = *CurveJsonPtr;
			bHasTypedCurveJson = true;
			TypedCurveJsonFieldName = TEXT("curve_json");
		}
		else if (PropertyObj->TryGetObjectField(TEXT("value_json"), CurveJsonPtr) && CurveJsonPtr && CurveJsonPtr->IsValid())
		{
			TypedCurveJson = *CurveJsonPtr;
			bHasTypedCurveJson = true;
			TypedCurveJsonFieldName = TEXT("value_json");
		}

		if (!bHasTypedCurveJson && !UeAgentJsonDiagnostics::ReadStringField(PropertyObj, TEXT("value_text"), PropertyPath + TEXT(".value_text"), ValueText, PropertyJsonIssues, true))
		{
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, TEXT(""), TEXT(""), TEXT("missing_value_text"));
			ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
			OutError = TEXT("missing_value_text");
			return false;
		}

		FString AppliedValueText;
		FString CppType;
		FString ImportError;
		if (bHasTypedCurveJson)
		{
			FProperty* Property = nullptr;
			void* ValuePtr = nullptr;
			if (!UeAgentAssetOps::ResolvePropertyPath(Asset, PropertyName, Property, ValuePtr) || !Property || !ValuePtr)
			{
				TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, TEXT(""), TEXT(""), TEXT("property_not_found"), TEXT("property_not_found"));
				ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
				PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
				OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
				OutError = TEXT("property_not_found");
				return false;
			}

			if (!UeAgentCurveJson::TryApplyPropertyCurveJson(Property, ValuePtr, TypedCurveJson, PropertyPath + TEXT(".") + TypedCurveJsonFieldName, PropertyJsonIssues))
			{
				TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, Property, TEXT("<curve_json>"), TEXT(""), TEXT("curve_json_apply_failed"), TEXT("curve_json_apply_failed"));
				ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
				PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
				OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
				OutError = TEXT("curve_json_apply_failed");
				return false;
			}

			Property->ExportTextItem_Direct(AppliedValueText, ValuePtr, nullptr, Asset, PPF_None);
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, Property, TEXT("<curve_json>"), AppliedValueText, TEXT("curve_json_applied_and_read_back"));
			TSharedPtr<FJsonObject> ReadbackCurveJson;
			if (UeAgentCurveJson::TryBuildPropertyCurveJson(Property, ValuePtr, ReadbackCurveJson) && ReadbackCurveJson.IsValid())
			{
				ResultObj->SetObjectField(TEXT("value_json_read_back"), ReadbackCurveJson);
			}
			if (PropertyJsonIssues.Num() > 0)
			{
				ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			}
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			++AppliedPropertyCount;
			continue;
		}

		if (!UeAgentAssetOps::SetObjectPropertyText(Asset, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
		{
			const FString ImportStatus = ImportError.Equals(TEXT("property_not_found"), ESearchCase::CaseSensitive) ? TEXT("property_not_found") : TEXT("import_failed");
			TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, ValueText, TEXT(""), ImportStatus, ImportError);
			ResultObj->SetStringField(TEXT("cpp_type"), CppType);
			if (PropertyJsonIssues.Num() > 0)
			{
				ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
			}
			PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
			OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
			OutError = ImportError.IsEmpty() ? FString::Printf(TEXT("asset_property_apply_failed:%s:%s"), *PropertyName, *ValueText) : ImportError;
			return false;
		}

		TSharedPtr<FJsonObject> ResultObj = FUeAgentHttpServer::MakePropertyImportResultJson(PropertyName, nullptr, ValueText, AppliedValueText, TEXT("imported_and_read_back"));
		ResultObj->SetStringField(TEXT("cpp_type"), CppType);
		if (PropertyJsonIssues.Num() > 0)
		{
			ResultObj->SetArrayField(TEXT("json_issues"), PropertyJsonIssues);
		}
		PropertyResultsJson.Add(MakeShared<FJsonValueObject>(ResultObj));
		++AppliedPropertyCount;
	}

	if (AppliedPropertyCount > 0)
	{
		Asset->Modify();
	}
	Asset->MarkPackageDirty();
	Asset->PostEditChange();

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (bSaveAfterApply && !UeAgentAssetOps::SaveAssetPackage(Asset, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentAssetOps::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetNumberField(TEXT("applied_property_count"), AppliedPropertyCount);
	OutData->SetArrayField(TEXT("property_results"), PropertyResultsJson);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	if (!ResolvedJsonFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), ResolvedJsonFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdCurveExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("curve_asset_not_found");
		return false;
	}

	TSharedPtr<FJsonObject> CurveJson = UeAgentCurveJson::BuildCurveAssetJson(Asset);
	if (!CurveJson.IsValid())
	{
		OutError = TEXT("unsupported_curve_asset");
		return false;
	}

	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty() && !UeAgentAssetOps::SaveJsonFile(OutputFile, CurveJson, SavedOutputFile, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentCurveJson::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetObjectField(TEXT("curve"), CurveJson);
	if (!SavedOutputFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdCurveApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> JsonRoot;
	FString JsonFile;
	FString ResolvedJsonFile;
	JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile);
	JsonFile.TrimStartAndEndInline();
	if (!JsonFile.IsEmpty())
	{
		if (!UeAgentAssetOps::LoadJsonFile(JsonFile, JsonRoot, ResolvedJsonFile, OutError))
		{
			return false;
		}
	}

	TSharedPtr<FJsonObject> CurveJson;
	const TSharedPtr<FJsonObject>* CurveObjPtr = nullptr;
	if (Ctx.Params->TryGetObjectField(TEXT("curve"), CurveObjPtr) && CurveObjPtr && CurveObjPtr->IsValid())
	{
		CurveJson = *CurveObjPtr;
	}
	else if (JsonRoot.IsValid() && JsonRoot->TryGetObjectField(TEXT("curve"), CurveObjPtr) && CurveObjPtr && CurveObjPtr->IsValid())
	{
		CurveJson = *CurveObjPtr;
	}
	else if (JsonRoot.IsValid())
	{
		CurveJson = JsonRoot;
	}
	else
	{
		CurveJson = Ctx.Params;
	}

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty() && CurveJson.IsValid())
	{
		CurveJson->TryGetStringField(TEXT("asset_path"), AssetPathInput);
		AssetPathInput.TrimStartAndEndInline();
	}
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	bool bCreatedAsset = false;
	bool bCreateIfMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	if (!Asset && bCreateIfMissing)
	{
		FString CurveKind;
		JsonTryGetString(Ctx.Params, TEXT("curve_kind"), CurveKind);
		if (CurveKind.TrimStartAndEnd().IsEmpty() && CurveJson.IsValid())
		{
			CurveJson->TryGetStringField(TEXT("curve_kind"), CurveKind);
		}
		Asset = UeAgentCurveJson::CreateCurveAsset(AssetPathInput, CurveKind, OutError);
		bCreatedAsset = Asset != nullptr;
	}
	if (!Asset)
	{
		OutError = TEXT("curve_asset_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> JsonIssues;
	if (!UeAgentCurveJson::ApplyCurveAssetJson(Asset, CurveJson, TEXT("curve"), JsonIssues))
	{
		OutData->SetStringField(TEXT("asset_path"), UeAgentCurveJson::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
		OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
		OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
		OutData->SetArrayField(TEXT("json_issues"), JsonIssues);
		if (!ResolvedJsonFile.IsEmpty())
		{
			OutData->SetStringField(TEXT("json_file"), ResolvedJsonFile);
		}
		OutError = TEXT("curve_json_apply_failed");
		return false;
	}

	Asset->Modify();
	Asset->MarkPackageDirty();
	Asset->PostEditChange();

	TSharedPtr<FJsonObject> ReadbackCurveJson = UeAgentCurveJson::BuildCurveAssetJson(Asset);
	if (ReadbackCurveJson.IsValid())
	{
		OutData->SetObjectField(TEXT("curve_read_back"), ReadbackCurveJson);
	}

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (bSaveAfterApply && !UeAgentAssetOps::SaveAssetPackage(Asset, OutError))
	{
		return false;
	}

	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty() && ReadbackCurveJson.IsValid() && !UeAgentAssetOps::SaveJsonFile(OutputFile, ReadbackCurveJson, SavedOutputFile, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentCurveJson::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("created"), bCreatedAsset);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	OutData->SetArrayField(TEXT("json_issues"), JsonIssues);
	if (!ResolvedJsonFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), ResolvedJsonFile);
	}
	if (!SavedOutputFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdEditorListDirtyResources(TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;

	TArray<UPackage*> DirtyPackages;
	UeAgentAssetOps::CollectDirtyPackages(DirtyPackages);

	TArray<TSharedPtr<FJsonValue>> DirtyResourcesJson;
	UeAgentAssetOps::MakeDirtyResourceArrayJson(DirtyPackages, EditorWorld, AssetEditorSubsystem, DirtyResourcesJson);

	OutData->SetNumberField(TEXT("dirty_resource_count"), DirtyResourcesJson.Num());
	OutData->SetBoolField(TEXT("has_unresolved_dirty_resources"), DirtyResourcesJson.Num() > 0);
	OutData->SetArrayField(TEXT("dirty_resources"), DirtyResourcesJson);
	return true;
}

bool FUeAgentHttpServer::CmdEditorResolveDirtyResources(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;

	UeAgentAssetOps::FDirtyResolutionOptions Options;
	JsonTryGetBool(Ctx.Params, TEXT("save_current_level"), Options.bSaveCurrentLevel);
	JsonTryGetBool(Ctx.Params, TEXT("discard_current_level"), Options.bDiscardCurrentLevel);
	JsonTryGetBool(Ctx.Params, TEXT("save_all_dirty"), Options.bSaveAllDirty);
	JsonTryGetBool(Ctx.Params, TEXT("discard_all_dirty"), Options.bDiscardAllDirty);
	if (!Options.bSaveAllDirty)
	{
		JsonTryGetBool(Ctx.Params, TEXT("save_all"), Options.bSaveAllDirty);
	}
	if (!Options.bDiscardAllDirty)
	{
		JsonTryGetBool(Ctx.Params, TEXT("discard_all"), Options.bDiscardAllDirty);
	}
	JsonTryGetBool(Ctx.Params, TEXT("close_all_asset_editors"), Options.bCloseAllAssetEditors);
	JsonTryGetBool(Ctx.Params, TEXT("only_save_dirty"), Options.bOnlySaveDirty);
	UeAgentAssetOps::ParseResourcePathArrays(Ctx.Params, Options.SaveResourcePaths, Options.DiscardResourcePaths);

	UeAgentAssetOps::FDirtyResolutionSummary Summary;
	if (!UeAgentAssetOps::ResolveDirtyResources(Options, EditorWorld, Summary, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> DirtyBeforeJson;
	TArray<TSharedPtr<FJsonValue>> DirtyAfterJson;
	UeAgentAssetOps::MakeDirtyResourceArrayJson(Summary.DirtyPackagesBefore, EditorWorld, AssetEditorSubsystem, DirtyBeforeJson);
	UeAgentAssetOps::MakeDirtyResourceArrayJson(Summary.DirtyPackagesAfter, EditorWorld, AssetEditorSubsystem, DirtyAfterJson);

	TArray<TSharedPtr<FJsonValue>> SaveNotFoundJson;
	for (const FString& Item : Summary.SaveResourceNotFound)
	{
		SaveNotFoundJson.Add(MakeShared<FJsonValueString>(Item));
	}

	TArray<TSharedPtr<FJsonValue>> DiscardNotFoundJson;
	for (const FString& Item : Summary.DiscardResourceNotFound)
	{
		DiscardNotFoundJson.Add(MakeShared<FJsonValueString>(Item));
	}

	OutData->SetNumberField(TEXT("saved_package_count"), Summary.SavedPackageCount);
	OutData->SetNumberField(TEXT("explicit_discarded_package_count"), Summary.ExplicitDiscardedPackageCount);
	OutData->SetNumberField(TEXT("auto_discarded_package_count"), Summary.AutoDiscardedPackageCount);
	OutData->SetBoolField(TEXT("close_all_asset_editors"), Options.bCloseAllAssetEditors);
	OutData->SetArrayField(TEXT("save_resource_not_found"), SaveNotFoundJson);
	OutData->SetArrayField(TEXT("discard_resource_not_found"), DiscardNotFoundJson);
	OutData->SetArrayField(TEXT("dirty_resources_before"), DirtyBeforeJson);
	OutData->SetArrayField(TEXT("dirty_resources_after"), DirtyAfterJson);
	OutData->SetNumberField(TEXT("remaining_dirty_resource_count"), DirtyAfterJson.Num());
	OutData->SetBoolField(TEXT("all_dirty_resources_resolved"), DirtyAfterJson.Num() == 0);
	return true;
}

bool FUeAgentHttpServer::CmdEditorClose(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;

	bool bCloseAllAssetEditors = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_all_asset_editors"), bCloseAllAssetEditors);

	bool bRequestExit = true;
	JsonTryGetBool(Ctx.Params, TEXT("request_exit"), bRequestExit);

	TArray<UPackage*> DirtyPackages;
	UeAgentAssetOps::CollectDirtyPackages(DirtyPackages);

	TArray<TSharedPtr<FJsonValue>> DirtyResourcesJson;
	UeAgentAssetOps::MakeDirtyResourceArrayJson(DirtyPackages, EditorWorld, AssetEditorSubsystem, DirtyResourcesJson);

	OutData->SetBoolField(TEXT("request_exit"), bRequestExit);
	OutData->SetBoolField(TEXT("close_all_asset_editors"), bCloseAllAssetEditors);
	OutData->SetNumberField(TEXT("dirty_resource_count"), DirtyResourcesJson.Num());
	OutData->SetArrayField(TEXT("dirty_resources"), DirtyResourcesJson);

	if (DirtyResourcesJson.Num() > 0)
	{
		OutData->SetBoolField(TEXT("close_requested"), false);
		OutData->SetBoolField(TEXT("closed"), false);
		OutError = TEXT("editor_has_unresolved_dirty_resources");
		return false;
	}

	if (bCloseAllAssetEditors && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystemLocal = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystemLocal->CloseAllAssetEditors();
		}
	}

	if (bRequestExit)
	{
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float)
			{
				FPlatformMisc::RequestExit(false);
				return false;
			}),
			0.0f);
	}

	OutData->SetBoolField(TEXT("close_requested"), bRequestExit);
	OutData->SetBoolField(TEXT("closed"), bRequestExit);
	return true;
}

bool FUeAgentHttpServer::CmdEditorPrepareExit(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	bool bSaveCurrentLevel = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_current_level"), bSaveCurrentLevel);

	bool bDiscardCurrentLevel = false;
	JsonTryGetBool(Ctx.Params, TEXT("discard_current_level"), bDiscardCurrentLevel);
	if (bSaveCurrentLevel && bDiscardCurrentLevel)
	{
		OutError = TEXT("save_and_discard_current_level_conflict");
		return false;
	}

	bool bSaveAllDirty = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_all_dirty"), bSaveAllDirty);
	if (!bSaveAllDirty)
	{
		JsonTryGetBool(Ctx.Params, TEXT("save_all"), bSaveAllDirty);
	}

	bool bDiscardAllDirty = true;
	JsonTryGetBool(Ctx.Params, TEXT("discard_all_dirty"), bDiscardAllDirty);
	if (!bDiscardAllDirty)
	{
		JsonTryGetBool(Ctx.Params, TEXT("discard_all"), bDiscardAllDirty);
	}

	bool bCloseAllAssetEditors = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_all_asset_editors"), bCloseAllAssetEditors);

	bool bRequestExit = true;
	JsonTryGetBool(Ctx.Params, TEXT("request_exit"), bRequestExit);

	bool bOnlySaveDirty = true;
	JsonTryGetBool(Ctx.Params, TEXT("only_save_dirty"), bOnlySaveDirty);

	TArray<FString> SaveAssetPaths;
	TArray<FString> DiscardAssetPaths;
	UeAgentAssetOps::ParseResourcePathArrays(Ctx.Params, SaveAssetPaths, DiscardAssetPaths);

	TSet<UPackage*> SavePackagesSet;
	TSet<UPackage*> DiscardPackagesSet;
	TArray<FString> SaveAssetNotFound;
	TArray<FString> DiscardAssetNotFound;

	for (const FString& SavePath : SaveAssetPaths)
	{
		UPackage* Package = UeAgentAssetOps::ResolvePackageByAssetPath(SavePath);
		if (!Package)
		{
			SaveAssetNotFound.Add(SavePath);
			continue;
		}
		UeAgentAssetOps::AddPackageIfValid(SavePackagesSet, Package);
	}

	for (const FString& DiscardPath : DiscardAssetPaths)
	{
		UPackage* Package = UeAgentAssetOps::ResolvePackageByAssetPath(DiscardPath);
		if (!Package)
		{
			DiscardAssetNotFound.Add(DiscardPath);
			continue;
		}
		UeAgentAssetOps::AddPackageIfValid(DiscardPackagesSet, Package);
	}

	UWorld* EditorWorld = nullptr;
	if (bSaveCurrentLevel || bDiscardCurrentLevel)
	{
		EditorWorld = GetEditorWorld(OutError);
		if (!EditorWorld)
		{
			return false;
		}
		if (UPackage* WorldPackage = EditorWorld->GetOutermost())
		{
			if (bSaveCurrentLevel)
			{
				SavePackagesSet.Add(WorldPackage);
			}
			if (bDiscardCurrentLevel)
			{
				DiscardPackagesSet.Add(WorldPackage);
			}
		}
	}

	TSet<UPackage*> DirtyPackagesBefore;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		UPackage* Package = *It;
		if (!Package || !Package->IsDirty() || UeAgentAssetOps::ShouldSkipDirtyPackage(Package))
		{
			continue;
		}
		DirtyPackagesBefore.Add(Package);
	}

	for (UPackage* DiscardPackage : DiscardPackagesSet)
	{
		SavePackagesSet.Remove(DiscardPackage);
	}

	if (bSaveAllDirty)
	{
		for (UPackage* DirtyPackage : DirtyPackagesBefore)
		{
			if (!DiscardPackagesSet.Contains(DirtyPackage))
			{
				SavePackagesSet.Add(DirtyPackage);
			}
		}
	}

	TArray<UPackage*> PackagesToSave;
	for (UPackage* Package : SavePackagesSet)
	{
		if (!Package)
		{
			continue;
		}
		if (bOnlySaveDirty && !Package->IsDirty())
		{
			continue;
		}
		PackagesToSave.Add(Package);
	}

	int32 SavedPackageCount = 0;
	if (PackagesToSave.Num() > 0)
	{
		const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);
		if (!bSaved)
		{
			OutError = TEXT("save_packages_failed");
			return false;
		}
		SavedPackageCount = PackagesToSave.Num();
	}

	const int32 ExplicitDiscardedCount = UeAgentAssetOps::ClearDirtyFlagForPackages(DiscardPackagesSet);

	int32 AutoDiscardedCount = 0;
	if (bDiscardAllDirty)
	{
		TSet<UPackage*> RemainingDirty;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (!Package || !Package->IsDirty() || UeAgentAssetOps::ShouldSkipDirtyPackage(Package))
			{
				continue;
			}
			if (SavePackagesSet.Contains(Package))
			{
				continue;
			}
			RemainingDirty.Add(Package);
		}
		AutoDiscardedCount = UeAgentAssetOps::ClearDirtyFlagForPackages(RemainingDirty);
	}

	if (bCloseAllAssetEditors && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->CloseAllAssetEditors();
		}
	}

	TArray<UPackage*> RemainingDirtyPackages;
	UeAgentAssetOps::CollectDirtyPackages(RemainingDirtyPackages);
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	TArray<TSharedPtr<FJsonValue>> RemainingDirtyResourcesJson;
	UeAgentAssetOps::MakeDirtyResourceArrayJson(RemainingDirtyPackages, EditorWorld, AssetEditorSubsystem, RemainingDirtyResourcesJson);

	OutData->SetNumberField(TEXT("saved_package_count"), SavedPackageCount);
	OutData->SetNumberField(TEXT("explicit_discarded_package_count"), ExplicitDiscardedCount);
	OutData->SetNumberField(TEXT("auto_discarded_package_count"), AutoDiscardedCount);
	OutData->SetBoolField(TEXT("request_exit"), bRequestExit);
	OutData->SetBoolField(TEXT("close_all_asset_editors"), bCloseAllAssetEditors);
	OutData->SetNumberField(TEXT("remaining_dirty_resource_count"), RemainingDirtyResourcesJson.Num());
	OutData->SetArrayField(TEXT("dirty_resources"), RemainingDirtyResourcesJson);

	TArray<TSharedPtr<FJsonValue>> SaveNotFoundJson;
	for (const FString& Item : SaveAssetNotFound)
	{
		SaveNotFoundJson.Add(MakeShared<FJsonValueString>(Item));
	}
	OutData->SetArrayField(TEXT("save_asset_not_found"), SaveNotFoundJson);

	TArray<TSharedPtr<FJsonValue>> DiscardNotFoundJson;
	for (const FString& Item : DiscardAssetNotFound)
	{
		DiscardNotFoundJson.Add(MakeShared<FJsonValueString>(Item));
	}
	OutData->SetArrayField(TEXT("discard_asset_not_found"), DiscardNotFoundJson);

	FUeAgentInterfaceLogger::Log(
		TEXT("EditorPrepareExit: save_paths=%d discard_paths=%d saved=%d discard_explicit=%d discard_auto=%d request_exit=%s"),
		SaveAssetPaths.Num(),
		DiscardAssetPaths.Num(),
		SavedPackageCount,
		ExplicitDiscardedCount,
		AutoDiscardedCount,
		bRequestExit ? TEXT("true") : TEXT("false"));

	if (RemainingDirtyResourcesJson.Num() > 0)
	{
		OutError = TEXT("editor_has_unresolved_dirty_resources");
		return false;
	}

	if (bRequestExit)
	{
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float)
			{
				FPlatformMisc::RequestExit(false);
				return false;
			}),
			0.0f);
	}

	return true;
}
