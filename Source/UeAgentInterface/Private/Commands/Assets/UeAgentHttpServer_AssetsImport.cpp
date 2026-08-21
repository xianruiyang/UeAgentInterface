// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Assets.h"
#include "UeAgentHttpServer_AssetsOps.h"
#include "Commands/Level/UeAgentCommandSafety.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Module/UeAgentInterfaceLogger.h"

#include "AssetToolsModule.h"
#include "Animation/AnimSequence.h"
#include "Animation/MorphTarget.h"
#include "Animation/SkinWeightProfile.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AutomatedAssetImportData.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Factories/TextureFactory.h"
#include "FileHelpers.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace UeAgentAssetOps
{

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

}
bool FUeAgentLegacyMechanics::CmdAssetImportTexture(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	bool bReplaceExisting = false;
	bool bAllowOverwrite = false;
	if (!UeAgentCommandSafety::ResolveOverwriteConfirmation(Ctx.Params, bReplaceExisting, bAllowOverwrite, OutError))
	{
		return false;
	}
	if (!UeAgentCommandSafety::EnsureImportTargetCanBeWritten(
		DestinationPath, DestinationName, SourceFilename, bReplaceExisting, OutError))
	{
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
	OutData->SetBoolField(TEXT("allow_overwrite"), bAllowOverwrite);
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

bool FUeAgentLegacyMechanics::CmdAssetImportFbxSkeletalMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	bool bReplaceExisting = false;
	bool bAllowOverwrite = false;
	if (!UeAgentCommandSafety::ResolveOverwriteConfirmation(Ctx.Params, bReplaceExisting, bAllowOverwrite, OutError))
	{
		return false;
	}
	if (!UeAgentCommandSafety::EnsureImportTargetCanBeWritten(
		DestinationPath, DestinationName, SourceFilename, bReplaceExisting, OutError))
	{
		return false;
	}

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

	bool bImportMorphTargets = false;
	JsonTryGetBool(Ctx.Params, TEXT("import_morph_targets"), bImportMorphTargets);

	bool bImportSkinWeightProfiles = false;
	JsonTryGetBool(Ctx.Params, TEXT("import_skin_weight_profiles"), bImportSkinWeightProfiles);

	bool bUpdateSkeletonReferencePose = false;
	JsonTryGetBool(Ctx.Params, TEXT("update_skeleton_reference_pose"), bUpdateSkeletonReferencePose);

	bool bPreserveExistingMorphTargets = true;
	JsonTryGetBool(Ctx.Params, TEXT("preserve_existing_morph_targets"), bPreserveExistingMorphTargets);

	bool bValidateAfterImport = true;
	JsonTryGetBool(Ctx.Params, TEXT("validate_after_import"), bValidateAfterImport);

	int32 ExpectedLodCount = INDEX_NONE;
	double ExpectedLodCountNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("expected_lod_count"), ExpectedLodCountNumber))
	{
		ExpectedLodCount = static_cast<int32>(ExpectedLodCountNumber);
	}

	TSet<FString> ExpectedMorphTargets;
	if (const TArray<TSharedPtr<FJsonValue>>* ExpectedMorphArray = nullptr; Ctx.Params->TryGetArrayField(TEXT("expected_morph_targets"), ExpectedMorphArray) && ExpectedMorphArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ExpectedMorphArray)
		{
			if (Value.IsValid())
			{
				FString Name = Value->AsString();
				Name.TrimStartAndEndInline();
				if (!Name.IsEmpty())
				{
					ExpectedMorphTargets.Add(Name);
				}
			}
		}
	}

	TSet<FString> ExpectedMaterialSlots;
	if (const TArray<TSharedPtr<FJsonValue>>* ExpectedMaterialArray = nullptr; Ctx.Params->TryGetArrayField(TEXT("expected_material_slots"), ExpectedMaterialArray) && ExpectedMaterialArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ExpectedMaterialArray)
		{
			if (Value.IsValid())
			{
				FString Name = Value->AsString();
				Name.TrimStartAndEndInline();
				if (!Name.IsEmpty())
				{
					ExpectedMaterialSlots.Add(Name);
				}
			}
		}
	}

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
		FbxFactory->ImportUI->SkeletalMeshImportData->bImportMorphTargets = bImportMorphTargets;
		FbxFactory->ImportUI->SkeletalMeshImportData->bUpdateSkeletonReferencePose = bUpdateSkeletonReferencePose;
		FbxFactory->ImportUI->SkeletalMeshImportData->ImportContentType = EFBXImportContentType::FBXICT_All;
	}

	ImportData->Filenames = { SourceFilename };
	ImportData->DestinationPath = DestinationPath;
	ImportData->FactoryName = FbxFactory->GetClass()->GetName();
	ImportData->bReplaceExisting = bReplaceExisting;
	ImportData->Factory = FbxFactory;

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<UObject*> ImportedObjects = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);
	TArray<UObject*> ImportedObjectsUnique;
	UeAgentAssetOps::GatherImportedObjects(ImportedObjects, ImportedObjectsUnique);

	if (bSaveAfterImport && ImportedObjects.Num() > 0)
	{
		TArray<UPackage*> PackagesToSave;
		for (UObject* ImportedObject : ImportedObjectsUnique)
		{
			if (ImportedObject && ImportedObject->GetOutermost())
			{
				PackagesToSave.AddUnique(ImportedObject->GetOutermost());
			}
			if (USkeletalMesh* ImportedMesh = Cast<USkeletalMesh>(ImportedObject))
			{
				if (USkeleton* ImportedSkeleton = ImportedMesh->GetSkeleton())
				{
					if (ImportedSkeleton->GetOutermost())
					{
						PackagesToSave.AddUnique(ImportedSkeleton->GetOutermost());
					}
				}
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
	OutData->SetBoolField(TEXT("allow_overwrite"), bAllowOverwrite);
	OutData->SetBoolField(TEXT("save_after_import"), bSaveAfterImport);
	OutData->SetBoolField(TEXT("import_morph_targets"), bImportMorphTargets);
	OutData->SetBoolField(TEXT("import_skin_weight_profiles"), bImportSkinWeightProfiles);
	OutData->SetBoolField(TEXT("update_skeleton_reference_pose"), bUpdateSkeletonReferencePose);
	OutData->SetBoolField(TEXT("preserve_existing_morph_targets"), bPreserveExistingMorphTargets);
	OutData->SetBoolField(TEXT("validate_after_import"), bValidateAfterImport);

	TArray<TSharedPtr<FJsonValue>> ValidationIssues;
	TArray<TSharedPtr<FJsonValue>> ImportedSkinWeightProfilesJson;
	USkeletalMesh* ImportedSkeletalMesh = nullptr;
	for (UObject* ImportedObject : ImportedObjectsUnique)
	{
		if (USkeletalMesh* CandidateMesh = Cast<USkeletalMesh>(ImportedObject))
		{
			ImportedSkeletalMesh = CandidateMesh;
			break;
		}
	}

	if (bImportSkinWeightProfiles)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("warning"));
		Issue->SetStringField(TEXT("code"), TEXT("skin_weight_profiles_use_explicit_command"));
		Issue->SetStringField(TEXT("field"), TEXT("import_skin_weight_profiles"));
		Issue->SetStringField(TEXT("message"), TEXT("FBX skeletal mesh import does not implicitly import skin weight profiles. Use skeletal_mesh_import_skin_weight_profile after mesh import."));
		ValidationIssues.Add(MakeShared<FJsonValueObject>(Issue));
	}

	if (!bPreserveExistingMorphTargets)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), TEXT("warning"));
		Issue->SetStringField(TEXT("code"), TEXT("preserve_existing_morph_targets_not_supported_by_new_import"));
		Issue->SetStringField(TEXT("field"), TEXT("preserve_existing_morph_targets"));
		Issue->SetStringField(TEXT("message"), TEXT("This parameter is reported for design compatibility. New FBX import has no existing morph targets to preserve; reimport-specific preservation needs a dedicated reimport path."));
		ValidationIssues.Add(MakeShared<FJsonValueObject>(Issue));
	}

	if (bValidateAfterImport && ImportedSkeletalMesh)
	{
		OutData->SetStringField(TEXT("skeletal_mesh_asset_path"), ImportedSkeletalMesh->GetOutermost() ? ImportedSkeletalMesh->GetOutermost()->GetName() : FString());
		OutData->SetNumberField(TEXT("lod_count"), ImportedSkeletalMesh->GetLODNum());
		OutData->SetNumberField(TEXT("material_slot_count"), ImportedSkeletalMesh->GetMaterials().Num());
		OutData->SetNumberField(TEXT("morph_target_count"), ImportedSkeletalMesh->GetMorphTargets().Num());
		OutData->SetNumberField(TEXT("skin_weight_profile_count"), ImportedSkeletalMesh->GetNumSkinWeightProfiles());
		if (USkeleton* ImportedSkeleton = ImportedSkeletalMesh->GetSkeleton())
		{
			OutData->SetStringField(TEXT("skeleton_asset_path"), ImportedSkeleton->GetOutermost() ? ImportedSkeleton->GetOutermost()->GetName() : FString());
		}
		else
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("error"));
			Issue->SetStringField(TEXT("code"), TEXT("imported_skeletal_mesh_missing_skeleton"));
			Issue->SetStringField(TEXT("field"), TEXT("skeleton"));
			Issue->SetStringField(TEXT("message"), TEXT("Imported skeletal mesh has no USkeleton reference; animation authoring assets cannot use it."));
			ValidationIssues.Add(MakeShared<FJsonValueObject>(Issue));
		}
		int32 SectionCount = 0;
		if (const FSkeletalMeshModel* ImportedModel = ImportedSkeletalMesh->GetImportedModel())
		{
			for (const FSkeletalMeshLODModel& LodModel : ImportedModel->LODModels)
			{
				SectionCount += LodModel.Sections.Num();
			}
		}
		OutData->SetNumberField(TEXT("section_count"), SectionCount);

		for (const FSkinWeightProfileInfo& ProfileInfo : ImportedSkeletalMesh->GetSkinWeightProfiles())
		{
			ImportedSkinWeightProfilesJson.Add(MakeShared<FJsonValueString>(ProfileInfo.Name.ToString()));
		}
		OutData->SetArrayField(TEXT("skin_weight_profiles_imported"), ImportedSkinWeightProfilesJson);

		TSet<FString> ImportedMorphNames;
		TArray<TSharedPtr<FJsonValue>> ImportedMorphNamesJson;
		for (const TObjectPtr<UMorphTarget>& MorphTarget : ImportedSkeletalMesh->GetMorphTargets())
		{
			if (MorphTarget)
			{
				ImportedMorphNames.Add(MorphTarget->GetName());
				ImportedMorphNamesJson.Add(MakeShared<FJsonValueString>(MorphTarget->GetName()));
			}
		}
		OutData->SetArrayField(TEXT("morph_targets_imported"), ImportedMorphNamesJson);

		TSet<FString> ImportedMaterialSlotNames;
		TArray<TSharedPtr<FJsonValue>> ImportedMaterialSlotNamesJson;
		for (const FSkeletalMaterial& Material : ImportedSkeletalMesh->GetMaterials())
		{
			const FString SlotName = Material.MaterialSlotName.ToString();
			ImportedMaterialSlotNames.Add(SlotName);
			ImportedMaterialSlotNamesJson.Add(MakeShared<FJsonValueString>(SlotName));
		}
		OutData->SetArrayField(TEXT("material_slots_imported"), ImportedMaterialSlotNamesJson);

		if (ExpectedLodCount != INDEX_NONE && ImportedSkeletalMesh->GetLODNum() != ExpectedLodCount)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), TEXT("error"));
			Issue->SetStringField(TEXT("code"), TEXT("expected_lod_count_mismatch"));
			Issue->SetStringField(TEXT("field"), TEXT("expected_lod_count"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Expected %d LODs but imported %d."), ExpectedLodCount, ImportedSkeletalMesh->GetLODNum()));
			ValidationIssues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		for (const FString& ExpectedMorph : ExpectedMorphTargets)
		{
			if (!ImportedMorphNames.Contains(ExpectedMorph))
			{
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("severity"), TEXT("error"));
				Issue->SetStringField(TEXT("code"), TEXT("expected_morph_target_missing"));
				Issue->SetStringField(TEXT("field"), TEXT("expected_morph_targets"));
				Issue->SetStringField(TEXT("message"), ExpectedMorph);
				ValidationIssues.Add(MakeShared<FJsonValueObject>(Issue));
			}
		}

		for (const FString& ExpectedSlot : ExpectedMaterialSlots)
		{
			if (!ImportedMaterialSlotNames.Contains(ExpectedSlot))
			{
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("severity"), TEXT("error"));
				Issue->SetStringField(TEXT("code"), TEXT("expected_material_slot_missing"));
				Issue->SetStringField(TEXT("field"), TEXT("expected_material_slots"));
				Issue->SetStringField(TEXT("message"), ExpectedSlot);
				ValidationIssues.Add(MakeShared<FJsonValueObject>(Issue));
			}
		}
	}

	if (OutData->GetIntegerField(TEXT("imported_object_count")) <= 0)
	{
		ImportData->RemoveFromRoot();
		FbxFactory->RemoveFromRoot();
		OutError = TEXT("fbx_import_created_no_assets");
		return false;
	}

	int32 ValidationErrorCount = 0;
	for (const TSharedPtr<FJsonValue>& IssueValue : ValidationIssues)
	{
		const TSharedPtr<FJsonObject> IssueObj = IssueValue.IsValid() ? IssueValue->AsObject() : nullptr;
		FString Severity;
		if (IssueObj.IsValid() && IssueObj->TryGetStringField(TEXT("severity"), Severity) && Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
		{
			++ValidationErrorCount;
		}
	}
	OutData->SetArrayField(TEXT("validation_issues"), ValidationIssues);
	OutData->SetNumberField(TEXT("validation_issue_count"), ValidationIssues.Num());
	OutData->SetNumberField(TEXT("validation_error_count"), ValidationErrorCount);
	OutData->SetBoolField(TEXT("validation_passed"), ValidationErrorCount == 0);
	if (!OutData->HasField(TEXT("skin_weight_profiles_imported")))
	{
		OutData->SetArrayField(TEXT("skin_weight_profiles_imported"), ImportedSkinWeightProfilesJson);
	}
	TSharedPtr<FJsonObject> ValidationReport = MakeShared<FJsonObject>();
	ValidationReport->SetBoolField(TEXT("validate_after_import"), bValidateAfterImport);
	ValidationReport->SetNumberField(TEXT("validation_issue_count"), ValidationIssues.Num());
	ValidationReport->SetNumberField(TEXT("validation_error_count"), ValidationErrorCount);
	ValidationReport->SetBoolField(TEXT("validation_passed"), ValidationErrorCount == 0);
	ValidationReport->SetArrayField(TEXT("issues"), ValidationIssues);
	ValidationReport->SetNumberField(TEXT("imported_object_count"), OutData->GetIntegerField(TEXT("imported_object_count")));
	ValidationReport->SetNumberField(TEXT("skin_weight_profile_count"), ImportedSkeletalMesh ? ImportedSkeletalMesh->GetNumSkinWeightProfiles() : 0);
	ValidationReport->SetNumberField(TEXT("section_count"), ImportedSkeletalMesh && OutData->HasField(TEXT("section_count")) ? OutData->GetIntegerField(TEXT("section_count")) : 0);
	OutData->SetObjectField(TEXT("validation_report"), ValidationReport);
	if (ValidationErrorCount > 0)
	{
		ImportData->RemoveFromRoot();
		FbxFactory->RemoveFromRoot();
		OutError = TEXT("fbx_skeletal_mesh_import_validation_failed");
		return false;
	}

	if (bOpenEditor)
	{
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

bool FUeAgentLegacyMechanics::CmdAssetImportFbxAnimation(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	bool bReplaceExisting = false;
	bool bAllowOverwrite = false;
	if (!UeAgentCommandSafety::ResolveOverwriteConfirmation(Ctx.Params, bReplaceExisting, bAllowOverwrite, OutError))
	{
		return false;
	}
	if (!UeAgentCommandSafety::EnsureImportTargetCanBeWritten(
		DestinationPath, DestinationName, SourceFilename, bReplaceExisting, OutError))
	{
		return false;
	}

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
	OutData->SetBoolField(TEXT("allow_overwrite"), bAllowOverwrite);
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
