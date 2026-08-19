#include "DomainAdapters/ExternalImport/Factory/UeAgentFactoryExternalImportProvider.h"

#include "AbcImportSettings.h"
#include "AlembicImportFactory.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetToolsModule.h"
#include "AutomatedAssetImportData.h"
#include "EditorFramework/AssetImportData.h"
#include "EditorReimportHandler.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureLightProfile.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "GeometryCache.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Sound/SoundWave.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "SparseVolumeTextureFactory.h"
#include "UObject/GarbageCollection.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include <openssl/sha.h>

namespace UE::UAI::DomainAdapters::ExternalImport::Factory
{
	namespace
	{
		constexpr const TCHAR* MetadataSource =
			TEXT("UAI.FactoryExternalImport.Source");
		constexpr const TCHAR* MetadataSourceSha256 =
			TEXT("UAI.FactoryExternalImport.SourceSha256");
		constexpr const TCHAR* MetadataOptionsFingerprint =
			TEXT("UAI.FactoryExternalImport.OptionsFingerprint");
		constexpr const TCHAR* MetadataOperation =
			TEXT("UAI.FactoryExternalImport.Operation");

		void SetError(
			FFactoryExternalImportError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		bool IsSha256(const FString& Value)
		{
			if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:")))
			{
				return false;
			}
			for (int32 Index = 7; Index < Value.Len(); ++Index)
			{
				if (!FChar::IsHexDigit(Value[Index]))
				{
					return false;
				}
			}
			return true;
		}

		bool TryPackageName(FString Ref, FString& OutPackageName)
		{
			Ref.TrimStartAndEndInline();
			OutPackageName = Ref.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Ref)
				: MoveTemp(Ref);
			return OutPackageName.StartsWith(TEXT("/Game/"))
				&& FPackageName::IsValidLongPackageName(OutPackageName, true);
		}

		FString ObjectPath(const FString& PackageName)
		{
			return PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
		}

		FString HashFileSha256(const FString& Filename)
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
			{
				return FString();
			}
			FSHA256Signature Signature{};
			const uint8 Empty = 0;
			const uint8* Data = Bytes.IsEmpty() ? &Empty : Bytes.GetData();
			if (!SHA256(
					Data,
					static_cast<size_t>(Bytes.Num()),
					Signature.Signature))
			{
				return FString();
			}
			return TEXT("sha256:") + Signature.ToString().ToLower();
		}

		FString HashUtf8Sha256(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value);
			FSHA256Signature Signature{};
			const uint8 Empty = 0;
			const uint8* Data = Utf8.Length() > 0
				? reinterpret_cast<const uint8*>(Utf8.Get())
				: &Empty;
			if (!SHA256(
					Data,
					static_cast<size_t>(Utf8.Length()),
					Signature.Signature))
			{
				return FString();
			}
			return TEXT("sha256:") + Signature.ToString().ToLower();
		}

		FString OperationName(const EFactoryExternalImportOperation Operation)
		{
			return Operation == EFactoryExternalImportOperation::Import
				? TEXT("import")
				: TEXT("reimport");
		}

		template <typename TEnum>
		bool TryEnumByName(
			const FString& Name,
			TEnum& OutValue)
		{
			if (Name.IsEmpty())
			{
				return true;
			}
			const UEnum* const Enum = StaticEnum<TEnum>();
			if (!Enum)
			{
				return false;
			}
			const int64 Value = Enum->GetValueByNameString(Name);
			if (Value == INDEX_NONE)
			{
				return false;
			}
			OutValue = static_cast<TEnum>(Value);
			return true;
		}

		template <typename TEnum>
		FString EnumName(const TEnum Value)
		{
			const UEnum* const Enum = StaticEnum<TEnum>();
			return Enum ? Enum->GetNameStringByValue(static_cast<int64>(Value)) : FString();
		}

		UClass* ExpectedClassFor(const EFactoryExternalImportKind Kind)
		{
			switch (Kind)
			{
			case EFactoryExternalImportKind::FbxAnimation:
				return UAnimSequence::StaticClass();
			case EFactoryExternalImportKind::FbxSkeletalMesh:
				return USkeletalMesh::StaticClass();
			case EFactoryExternalImportKind::GeometryCache:
				return UGeometryCache::StaticClass();
			case EFactoryExternalImportKind::LightProfile:
				return UTextureLightProfile::StaticClass();
			case EFactoryExternalImportKind::SoundWave:
				return USoundWave::StaticClass();
			case EFactoryExternalImportKind::SparseVolumeTexture:
				return USparseVolumeTexture::StaticClass();
			case EFactoryExternalImportKind::StaticMesh:
				return UStaticMesh::StaticClass();
			case EFactoryExternalImportKind::Texture:
				return UTexture::StaticClass();
			default:
				return nullptr;
			}
		}

		TSet<FString> AllowedExtensions(const EFactoryExternalImportKind Kind)
		{
			switch (Kind)
			{
			case EFactoryExternalImportKind::FbxAnimation:
			case EFactoryExternalImportKind::FbxSkeletalMesh:
				return {TEXT("fbx")};
			case EFactoryExternalImportKind::GeometryCache:
				return {TEXT("abc")};
			case EFactoryExternalImportKind::LightProfile:
				return {TEXT("ies")};
			case EFactoryExternalImportKind::SoundWave:
				return {TEXT("wav")};
			case EFactoryExternalImportKind::SparseVolumeTexture:
				return {TEXT("vdb")};
			case EFactoryExternalImportKind::StaticMesh:
				return {TEXT("fbx"), TEXT("obj")};
			case EFactoryExternalImportKind::Texture:
				return {
					TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("tga"),
					TEXT("bmp"), TEXT("exr"), TEXT("hdr"), TEXT("dds"),
					TEXT("psd"), TEXT("tif"), TEXT("tiff")};
			default:
				return {};
			}
		}

		bool TryNormalizeSource(
			FString SourceRef,
			const EFactoryExternalImportKind Kind,
			FString& OutSource,
			FFactoryExternalImportError& OutError)
		{
			SourceRef.TrimStartAndEndInline();
			if (SourceRef.IsEmpty())
			{
				SetError(OutError, TEXT("factory_import_source_required"));
				return false;
			}
			OutSource = FPaths::IsRelative(SourceRef)
				? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), SourceRef)
				: FPaths::ConvertRelativePathToFull(SourceRef);
			FPaths::NormalizeFilename(OutSource);
			if (!FPaths::FileExists(OutSource))
			{
				SetError(
					OutError,
					TEXT("factory_import_source_not_found"),
					OutSource);
				return false;
			}
			const FString Extension = FPaths::GetExtension(OutSource).ToLower();
			if (!AllowedExtensions(Kind).Contains(Extension))
			{
				SetError(
					OutError,
					TEXT("factory_import_source_extension_invalid"),
					OutSource,
					Extension);
				return false;
			}
			return true;
		}

		UAssetImportData* FindImportData(UObject& Asset)
		{
			const FObjectPropertyBase* Property =
				FindFProperty<FObjectPropertyBase>(
					Asset.GetClass(),
					TEXT("AssetImportData"));
			return Property
				? Cast<UAssetImportData>(
					Property->GetObjectPropertyValue_InContainer(&Asset))
				: nullptr;
		}

		bool TryReadBusinessState(
			UObject& Asset,
			const FFactoryExternalImportSpec& Spec,
			FString& OutStateKind,
			int32& OutItemCount,
			TArray<FString>& OutDependencyIdentities,
			FFactoryExternalImportError& OutError)
		{
			OutStateKind.Reset();
			OutItemCount = 0;
			OutDependencyIdentities.Reset();
			switch (Spec.Kind)
			{
			case EFactoryExternalImportKind::FbxAnimation:
			{
				UAnimSequence* const Sequence = Cast<UAnimSequence>(&Asset);
				USkeleton* const Skeleton = Sequence
					? Sequence->GetSkeleton()
					: nullptr;
				if (Skeleton)
				{
					OutDependencyIdentities.Add(Skeleton->GetPathName());
				}
				OutStateKind = TEXT("animation_sampled_keys");
				OutItemCount = Sequence
					? Sequence->GetNumberOfSampledKeys()
					: 0;
				if (!Skeleton
					|| Skeleton->GetPathName() != Spec.SkeletonRef
					|| OutItemCount <= 0)
				{
					SetError(
						OutError,
						TEXT("factory_import_animation_state_invalid"),
						Asset.GetPathName(),
						Spec.SkeletonRef);
					return false;
				}
				return true;
			}
			case EFactoryExternalImportKind::FbxSkeletalMesh:
			{
				USkeletalMesh* const Mesh = Cast<USkeletalMesh>(&Asset);
				USkeleton* const Skeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
				if (Skeleton)
				{
					OutDependencyIdentities.Add(Skeleton->GetPathName());
				}
				OutStateKind = TEXT("skeletal_mesh_bones");
				OutItemCount = Mesh ? Mesh->GetRefSkeleton().GetNum() : 0;
				if (!Skeleton || OutItemCount <= 0)
				{
					SetError(
						OutError,
						TEXT("factory_import_skeletal_state_invalid"),
						Asset.GetPathName());
					return false;
				}
				return true;
			}
			case EFactoryExternalImportKind::GeometryCache:
			{
				UGeometryCache* const Cache = Cast<UGeometryCache>(&Asset);
				OutStateKind = TEXT("geometry_cache_tracks");
				OutItemCount = Cache ? Cache->Tracks.Num() : 0;
				break;
			}
			case EFactoryExternalImportKind::LightProfile:
			{
				UTextureLightProfile* const Profile =
					Cast<UTextureLightProfile>(&Asset);
				OutStateKind = TEXT("light_profile_width");
				OutItemCount = Profile && Profile->Source.IsValid()
					? static_cast<int32>(Profile->Source.GetSizeX())
					: 0;
				break;
			}
			case EFactoryExternalImportKind::SoundWave:
			{
				USoundWave* const Sound = Cast<USoundWave>(&Asset);
				OutStateKind = TEXT("sound_duration_milliseconds");
				OutItemCount = Sound
					? FMath::Max(0, FMath::RoundToInt(Sound->GetDuration() * 1000.0f))
					: 0;
				break;
			}
			case EFactoryExternalImportKind::SparseVolumeTexture:
			{
				USparseVolumeTexture* const Volume =
					Cast<USparseVolumeTexture>(&Asset);
				const FIntVector Resolution = Volume
					? Volume->GetVolumeResolution()
					: FIntVector::ZeroValue;
				OutStateKind = TEXT("sparse_volume_frames");
				OutItemCount = Volume ? Volume->GetNumFrames() : 0;
				if (OutItemCount > 0
					&& Resolution.X > 0
					&& Resolution.Y > 0
					&& Resolution.Z > 0)
				{
					return true;
				}
				break;
			}
			case EFactoryExternalImportKind::StaticMesh:
			{
				UStaticMesh* const Mesh = Cast<UStaticMesh>(&Asset);
				OutStateKind = TEXT("static_mesh_lods");
				OutItemCount = Mesh ? Mesh->GetNumLODs() : 0;
				break;
			}
			case EFactoryExternalImportKind::Texture:
			{
				UTexture2D* const Texture = Cast<UTexture2D>(&Asset);
				OutStateKind = TEXT("texture_width");
				OutItemCount = Texture && Texture->Source.IsValid()
					? static_cast<int32>(Texture->Source.GetSizeX())
					: 0;
				break;
			}
			default:
				break;
			}
			if (OutItemCount <= 0)
			{
				SetError(
					OutError,
					TEXT("factory_import_business_state_invalid"),
					Asset.GetPathName(),
					OutStateKind);
				return false;
			}
			return true;
		}

		bool ApplyPostImportOptions(
			UObject& Asset,
			const FFactoryExternalImportSpec& Spec,
			FFactoryExternalImportError& OutError)
		{
			if (Spec.Kind != EFactoryExternalImportKind::Texture)
			{
				return true;
			}
			UTexture2D* const Texture = Cast<UTexture2D>(&Asset);
			if (!Texture)
			{
				SetError(OutError, TEXT("factory_import_texture_state_invalid"), Asset.GetPathName());
				return false;
			}
			TextureCompressionSettings Compression =
				static_cast<TextureCompressionSettings>(Texture->CompressionSettings.GetValue());
			TextureMipGenSettings Mip =
				static_cast<TextureMipGenSettings>(Texture->MipGenSettings.GetValue());
			TextureGroup Group =
				static_cast<TextureGroup>(Texture->LODGroup.GetValue());
			if (!TryEnumByName(Spec.Options.CompressionSettings, Compression)
				|| !TryEnumByName(Spec.Options.MipGenSettings, Mip)
				|| !TryEnumByName(Spec.Options.LodGroup, Group))
			{
				SetError(OutError, TEXT("factory_import_texture_option_invalid"), Asset.GetPathName());
				return false;
			}
			if (Spec.Options.Srgb.IsSet()) Texture->SRGB = Spec.Options.Srgb.GetValue();
			if (!Spec.Options.CompressionSettings.IsEmpty()) Texture->CompressionSettings = Compression;
			if (!Spec.Options.MipGenSettings.IsEmpty()) Texture->MipGenSettings = Mip;
			if (!Spec.Options.LodGroup.IsEmpty()) Texture->LODGroup = Group;
			Texture->PostEditChange();
			return true;
		}

		void ReadDetailedState(
			UObject& Asset,
			FFactoryExternalImportReadback& OutReadback)
		{
			if (const UTexture2D* const Texture = Cast<UTexture2D>(&Asset))
			{
				OutReadback.Srgb = Texture->SRGB != 0;
				OutReadback.CompressionSettings = EnumName(static_cast<TextureCompressionSettings>(Texture->CompressionSettings.GetValue()));
				OutReadback.MipGenSettings = EnumName(static_cast<TextureMipGenSettings>(Texture->MipGenSettings.GetValue()));
				OutReadback.LodGroup = EnumName(static_cast<TextureGroup>(Texture->LODGroup.GetValue()));
			}
			if (const USkeletalMesh* const Mesh = Cast<USkeletalMesh>(&Asset))
			{
				OutReadback.LodCount = Mesh->GetLODNum();
				for (const TObjectPtr<UMorphTarget>& Morph : Mesh->GetMorphTargets())
				{
					if (Morph) OutReadback.MorphTargetNames.Add(Morph->GetName());
				}
				for (const FSkeletalMaterial& Material : Mesh->GetMaterials())
				{
					OutReadback.MaterialSlotNames.Add(Material.MaterialSlotName.ToString());
				}
				OutReadback.MorphTargetNames.Sort();
				OutReadback.MaterialSlotNames.Sort();
			}
		}

		bool ValidateDetailedState(
			const FFactoryExternalImportSpec& Spec,
			const FFactoryExternalImportReadback& Readback,
			FFactoryExternalImportError& OutError)
		{
			if (Spec.Kind == EFactoryExternalImportKind::Texture)
			{
				const bool bMatches =
					(!Spec.Options.Srgb.IsSet()
						|| (Readback.Srgb.IsSet()
							&& Readback.Srgb.GetValue() == Spec.Options.Srgb.GetValue()))
					&& (Spec.Options.CompressionSettings.IsEmpty() || Readback.CompressionSettings == Spec.Options.CompressionSettings)
					&& (Spec.Options.MipGenSettings.IsEmpty() || Readback.MipGenSettings == Spec.Options.MipGenSettings)
					&& (Spec.Options.LodGroup.IsEmpty() || Readback.LodGroup == Spec.Options.LodGroup);
				if (!bMatches)
				{
					SetError(OutError, TEXT("factory_import_texture_readback_mismatch"), Readback.TargetIdentity);
					return false;
				}
			}
			if (Spec.Kind == EFactoryExternalImportKind::FbxSkeletalMesh
				&& Spec.Options.bValidateAfterImport)
			{
				if (Spec.Options.ExpectedLodCount != INDEX_NONE
					&& Readback.LodCount != Spec.Options.ExpectedLodCount)
				{
					SetError(OutError, TEXT("factory_import_expected_lod_missing"), Readback.TargetIdentity);
					return false;
				}
				for (const FString& Name : Spec.Options.ExpectedMorphTargets)
				{
					if (!Readback.MorphTargetNames.Contains(Name))
					{
						SetError(OutError, TEXT("factory_import_expected_morph_target_missing"), Name);
						return false;
					}
				}
				for (const FString& Name : Spec.Options.ExpectedMaterialSlots)
				{
					if (!Readback.MaterialSlotNames.Contains(Name))
					{
						SetError(OutError, TEXT("factory_import_expected_material_slot_missing"), Name);
						return false;
					}
				}
			}
			return true;
		}

		bool SourceMatchesImportData(
			UAssetImportData& ImportData,
			const FString& Source)
		{
			TArray<FString> Filenames;
			ImportData.ExtractFilenames(Filenames);
			for (FString Filename : Filenames)
			{
				Filename = FPaths::ConvertRelativePathToFull(Filename);
				FPaths::NormalizeFilename(Filename);
				if (FPaths::IsSamePath(Filename, Source))
				{
					return true;
				}
			}
			return false;
		}

		UFactory* CreateImportFactory(
			const FFactoryExternalImportSpec& Spec,
			FFactoryExternalImportError& OutError)
		{
			if (Spec.Kind == EFactoryExternalImportKind::GeometryCache)
			{
				UAlembicImportFactory* const Factory =
					NewObject<UAlembicImportFactory>();
				if (!Factory)
				{
					SetError(
						OutError,
						TEXT("factory_import_alembic_factory_unavailable"));
					return nullptr;
				}
				Factory->ImportSettings = NewObject<UAbcImportSettings>(Factory);
				if (!Factory->ImportSettings)
				{
					SetError(
						OutError,
						TEXT("factory_import_alembic_settings_unavailable"));
					return nullptr;
				}
				Factory->ImportSettings->ImportType =
					EAlembicImportType::GeometryCache;
				Factory->bShowOption = false;
				return Factory;
			}
			if (Spec.Kind == EFactoryExternalImportKind::SparseVolumeTexture)
			{
				USparseVolumeTextureFactory* const Factory =
					NewObject<USparseVolumeTextureFactory>();
				if (!Factory)
				{
					SetError(
						OutError,
						TEXT("factory_import_sparse_volume_factory_unavailable"));
				}
				return Factory;
			}
			if (Spec.Kind != EFactoryExternalImportKind::FbxAnimation
				&& Spec.Kind != EFactoryExternalImportKind::FbxSkeletalMesh)
			{
				return nullptr;
			}
			UFbxFactory* const Factory = NewObject<UFbxFactory>();
			if (!Factory || !Factory->ImportUI)
			{
				SetError(
					OutError,
					TEXT("factory_import_fbx_factory_unavailable"));
				return nullptr;
			}
			Factory->SetDetectImportTypeOnImport(false);
			Factory->ImportUI->bAutomatedImportShouldDetectType = false;
			Factory->ImportUI->bImportAsSkeletal = true;
			Factory->ImportUI->bIsReimport = false;
			Factory->ImportUI->ReimportMesh = nullptr;
			Factory->ImportUI->bAllowContentTypeImport = true;
			Factory->ImportUI->bImportMaterials =
				Spec.Options.bImportMaterials;
			Factory->ImportUI->bImportTextures =
				Spec.Options.bImportTextures;
				Factory->ImportUI->bCreatePhysicsAsset =
				Spec.Options.bCreatePhysicsAsset;
			Factory->ImportUI->bImportRigidMesh = false;
			Factory->ImportUI->bIsObjImport = false;
			Factory->ImportUI->bOverrideFullName = true;
			if (Spec.Kind == EFactoryExternalImportKind::FbxAnimation)
			{
				USkeleton* const Skeleton = LoadObject<USkeleton>(
					nullptr,
					*Spec.SkeletonRef);
				if (!Skeleton)
				{
					SetError(
						OutError,
						TEXT("factory_import_skeleton_not_found"),
						Spec.SkeletonRef);
					return nullptr;
				}
				Factory->ImportUI->MeshTypeToImport = FBXIT_Animation;
				Factory->ImportUI->OriginalImportType = FBXIT_Animation;
				Factory->ImportUI->bImportAnimations = true;
				Factory->ImportUI->bImportMesh = false;
				Factory->ImportUI->Skeleton = Skeleton;
				Factory->ImportUI->OverrideAnimationName =
					FPackageName::GetLongPackageAssetName(Spec.DestinationRef);
			}
			else
			{
				Factory->ImportUI->MeshTypeToImport = FBXIT_SkeletalMesh;
				Factory->ImportUI->OriginalImportType = FBXIT_SkeletalMesh;
				Factory->ImportUI->bImportAnimations = Spec.Options.bImportAnimations;
				Factory->ImportUI->bImportMesh = true;
				Factory->ImportUI->Skeleton = Spec.SkeletonRef.IsEmpty()
					? nullptr
					: LoadObject<USkeleton>(nullptr, *Spec.SkeletonRef);
				if (!Spec.SkeletonRef.IsEmpty() && !Factory->ImportUI->Skeleton)
				{
					SetError(OutError, TEXT("factory_import_skeleton_not_found"), Spec.SkeletonRef);
					return nullptr;
				}
				if (Factory->ImportUI->SkeletalMeshImportData)
				{
					Factory->ImportUI->SkeletalMeshImportData->bImportMorphTargets = Spec.Options.bImportMorphTargets;
					Factory->ImportUI->SkeletalMeshImportData->bUpdateSkeletonReferencePose = Spec.Options.bUpdateSkeletonReferencePose;
				}
			}
			return Factory;
		}

		class FSession final : public IFactoryExternalImportSession
		{
		public:
			FSession(
				FFactoryExternalImportSpec InSpec,
				FString InSource,
				FString InSourceSha256,
				FString InPackageName,
				FString InExpectedRevision,
				FString InPackageFilename,
				FString InBackupFilename,
				UObject* InExistingAsset)
				: Spec(MoveTemp(InSpec))
				, Source(MoveTemp(InSource))
				, SourceSha256(MoveTemp(InSourceSha256))
				, PackageName(MoveTemp(InPackageName))
				, TargetObjectPath(ObjectPath(PackageName))
				, ExpectedRevision(MoveTemp(InExpectedRevision))
				, PackageFilename(MoveTemp(InPackageFilename))
				, BackupFilename(MoveTemp(InBackupFilename))
				, Primary(InExistingAsset)
			{
			}

			virtual const FString& GetTargetIdentity() const override
			{
				return TargetObjectPath;
			}

			virtual const FString& GetExpectedRevision() const override
			{
				return ExpectedRevision;
			}

			virtual const FString& GetSourceSha256() const override
			{
				return SourceSha256;
			}

			virtual bool CheckPreExecute(
				FFactoryExternalImportError& OutError) override
			{
				FString ExistingFilename;
				const bool bPackageExists =
					FPackageName::DoesPackageExist(
						PackageName,
						&ExistingFilename);
				if (Spec.Operation == EFactoryExternalImportOperation::Import)
				{
					if (bPackageExists
						|| StaticFindObject(
							UObject::StaticClass(),
							nullptr,
							*TargetObjectPath) != nullptr)
					{
						SetError(
							OutError,
							TEXT("factory_import_destination_conflict"),
							TargetObjectPath);
						return false;
					}
					return true;
				}
				if (!bPackageExists
					|| !Primary
					|| HashFileSha256(PackageFilename) != ExpectedRevision)
				{
					SetError(
						OutError,
						TEXT("factory_reimport_version_conflict"),
						TargetObjectPath);
					return false;
				}
				return true;
			}

			virtual bool Execute(
				FFactoryExternalImportError& OutError) override
			{
				return Spec.Operation == EFactoryExternalImportOperation::Import
					? ExecuteImport(OutError)
					: ExecuteReimport(OutError);
			}

			virtual bool DidMutationStart() const override
			{
				return bMutationStarted;
			}

			virtual bool Settle(
				FFactoryExternalImportError& OutError) override
			{
				if (!Primary
					|| !Primary->IsA(ExpectedClassFor(Spec.Kind))
					|| !Primary->GetOutermost()
					|| Primary->GetOutermost()->GetName() != PackageName)
				{
					SetError(
						OutError,
						TEXT("factory_import_settle_target_invalid"),
						TargetObjectPath);
					return false;
				}
				UAssetImportData* const ImportData = FindImportData(*Primary);
				if (!ImportData || !SourceMatchesImportData(*ImportData, Source))
				{
					SetError(
						OutError,
						TEXT("factory_import_settle_import_data_invalid"),
						TargetObjectPath);
					return false;
				}
				if (!ApplyPostImportOptions(*Primary, Spec, OutError))
				{
					return false;
				}
				Primary->PostEditChange();
				if (Spec.Operation == EFactoryExternalImportOperation::Import)
				{
					if (Spec.Kind == EFactoryExternalImportKind::FbxSkeletalMesh)
					{
						if (USkeletalMesh* const Mesh = Cast<USkeletalMesh>(Primary))
						{
							if (USkeleton* const Skeleton = Mesh->GetSkeleton())
							{
								ImportedObjects.AddUnique(Skeleton);
							}
						}
					}
					ImportedObjects.AddUnique(Primary);
					for (UObject* Object : ImportedObjects)
					{
						if (IsValid(Object) && Object->GetOutermost())
						{
							OwnedPackageNames.AddUnique(
								Object->GetOutermost()->GetName());
						}
					}
				}
				FString BusinessStateKind;
				int32 BusinessItemCount = 0;
				TArray<FString> DependencyIdentities;
				if (!TryReadBusinessState(
						*Primary,
						Spec,
						BusinessStateKind,
						BusinessItemCount,
						DependencyIdentities,
						OutError))
				{
					return false;
				}
				UPackage* const Package = Primary->GetOutermost();
				FMetaData& Metadata = Package->GetMetaData();
				Metadata.SetValue(Primary, MetadataSource, *Source);
				Metadata.SetValue(Primary, MetadataSourceSha256, *SourceSha256);
				Metadata.SetValue(
					Primary,
					MetadataOptionsFingerprint,
					*Spec.Options.Fingerprint);
				Metadata.SetValue(
					Primary,
					MetadataOperation,
					*OperationName(Spec.Operation));
				Package->SetDirtyFlag(true);
				return true;
			}

			virtual bool CheckPrePersist(
				FFactoryExternalImportError& OutError) override
			{
				if (!Primary
					|| !Primary->GetOutermost()
					|| !Primary->GetOutermost()->IsDirty())
				{
					SetError(
						OutError,
						TEXT("factory_import_pre_persist_fence_failed"),
						TargetObjectPath);
					return false;
				}
				return true;
			}

			virtual bool Persist(
				FString& OutPersistenceReceiptId,
				FFactoryExternalImportError& OutError) override
			{
				TSet<UPackage*> Packages;
				if (Primary && Primary->GetOutermost())
				{
					Packages.Add(Primary->GetOutermost());
				}
				for (UObject* Object : ImportedObjects)
				{
					if (IsValid(Object) && Object->GetOutermost())
					{
						Packages.Add(Object->GetOutermost());
					}
				}
				TArray<UPackage*> OrderedPackages = Packages.Array();
				OrderedPackages.Sort([](const UPackage& Left, const UPackage& Right)
				{
					return Left.GetName() < Right.GetName();
				});
				TArray<FString> PersistedStates;
				for (UPackage* Package : OrderedPackages)
				{
					const FString Filename =
						FPackageName::LongPackageNameToFilename(
							Package->GetName(),
							FPackageName::GetAssetPackageExtension());
					FSavePackageArgs Args;
					Args.TopLevelFlags = RF_Public | RF_Standalone;
					Args.SaveFlags = SAVE_NoError;
					if (!UPackage::SavePackage(
							Package,
							Package == Primary->GetOutermost() ? Primary : nullptr,
							*Filename,
							Args))
					{
						SetError(
							OutError,
							TEXT("factory_import_persist_failed"),
							Package->GetName(),
							Filename);
						return false;
					}
					PersistedPackageNames.AddUnique(Package->GetName());
					const FString PackageHash = HashFileSha256(Filename);
					if (PackageHash.IsEmpty())
					{
						SetError(
							OutError,
							TEXT("factory_import_persistence_receipt_missing"),
							Package->GetName(),
							Filename);
						return false;
					}
					PersistedStates.Add(Package->GetName() + TEXT("=") + PackageHash);
				}
				OutPersistenceReceiptId = HashUtf8Sha256(
					FString::Join(PersistedStates, TEXT("\n")));
				if (OutPersistenceReceiptId.IsEmpty())
				{
					SetError(
						OutError,
						TEXT("factory_import_persistence_receipt_missing"),
						PackageFilename);
					return false;
				}
				return true;
			}

			virtual bool Verify(
				FFactoryExternalImportReadback& OutReadback,
				FFactoryExternalImportError& OutError) override
			{
				TArray<UPackage*> Packages;
				for (const FString& PersistedPackageName : PersistedPackageNames)
				{
					if (UPackage* const Package =
						FindPackage(nullptr, *PersistedPackageName))
					{
						Package->SetDirtyFlag(false);
						Packages.Add(Package);
					}
				}
				ImportedObjects.Reset();
				Primary = nullptr;
				FText UnloadError;
				if (!Packages.IsEmpty()
					&& !UPackageTools::UnloadPackages(
						Packages,
						UnloadError,
						true))
				{
					SetError(
						OutError,
						TEXT("factory_import_unload_failed"),
						TargetObjectPath,
						UnloadError.ToString());
					return false;
				}
				CollectGarbage(RF_NoFlags, true);
				UObject* const Reloaded = LoadObject<UObject>(
					nullptr,
					*TargetObjectPath);
				if (!Reloaded || !Reloaded->IsA(ExpectedClassFor(Spec.Kind)))
				{
					SetError(
						OutError,
						TEXT("factory_import_reload_failed"),
						TargetObjectPath);
					return false;
				}
				UAssetImportData* const ImportData = FindImportData(*Reloaded);
				FMetaData& Metadata = Reloaded->GetOutermost()->GetMetaData();
				OutReadback.Operation = Metadata.GetValue(
					Reloaded,
					MetadataOperation);
				OutReadback.TargetIdentity = Reloaded->GetPathName();
				OutReadback.AssetClass = Reloaded->GetClass()->GetPathName();
				OutReadback.SourceRef = Metadata.GetValue(
					Reloaded,
					MetadataSource);
				OutReadback.SourceSha256 = Metadata.GetValue(
					Reloaded,
					MetadataSourceSha256);
				OutReadback.ImportDataClass = ImportData
					? ImportData->GetClass()->GetPathName()
					: FString();
				OutReadback.OptionsFingerprint = Metadata.GetValue(
					Reloaded,
					MetadataOptionsFingerprint);
				if (!TryReadBusinessState(
						*Reloaded,
						Spec,
						OutReadback.BusinessStateKind,
						OutReadback.BusinessItemCount,
						OutReadback.DependencyIdentities,
						OutError))
				{
					return false;
				}
				OutReadback.bReloaded = true;
				ReadDetailedState(*Reloaded, OutReadback);
				if (!ValidateDetailedState(Spec, OutReadback, OutError))
				{
					return false;
				}
				const bool bEquivalent = ImportData
					&& SourceMatchesImportData(*ImportData, Source)
					&& FPaths::IsSamePath(OutReadback.SourceRef, Source)
					&& OutReadback.SourceSha256 == SourceSha256
					&& OutReadback.OptionsFingerprint
						== Spec.Options.Fingerprint
					&& OutReadback.Operation
						== OperationName(Spec.Operation);
				if (!bEquivalent)
				{
					SetError(
						OutError,
						TEXT("factory_import_readback_mismatch"),
						TargetObjectPath);
					return false;
				}
				Primary = Reloaded;
				DeleteBackup();
				return true;
			}

			virtual bool Rollback(
				bool& bOutConfirmed,
				FFactoryExternalImportError& OutError) override
			{
				bOutConfirmed = Spec.Operation
					== EFactoryExternalImportOperation::Import
					? RollbackImport()
					: RollbackReimport();
				if (!bOutConfirmed)
				{
					SetError(
						OutError,
						TEXT("factory_import_rollback_unconfirmed"),
						TargetObjectPath,
						BackupFilename);
				}
				return bOutConfirmed;
			}

		private:
			bool ExecuteImport(FFactoryExternalImportError& OutError)
			{
				UFactory* const Factory = CreateImportFactory(Spec, OutError);
				if ((Spec.Kind == EFactoryExternalImportKind::FbxAnimation
						|| Spec.Kind
							== EFactoryExternalImportKind::FbxSkeletalMesh
						|| Spec.Kind == EFactoryExternalImportKind::GeometryCache
						|| Spec.Kind
							== EFactoryExternalImportKind::SparseVolumeTexture)
					&& !Factory)
				{
					return false;
				}
				UAutomatedAssetImportData* const ImportData =
					NewObject<UAutomatedAssetImportData>();
				if (!ImportData)
				{
					SetError(
						OutError,
						TEXT("factory_import_data_unavailable"));
					return false;
				}
				ImportData->AddToRoot();
				if (Factory)
				{
					Factory->AddToRoot();
				}
				ImportData->Filenames = {Source};
				ImportData->DestinationPath =
					FPackageName::GetLongPackagePath(PackageName);
				ImportData->bReplaceExisting = false;
				if (Factory)
				{
					ImportData->FactoryName = Factory->GetClass()->GetName();
					ImportData->Factory = Factory;
				}
				{
					TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
					ImportedObjects =
						FModuleManager::LoadModuleChecked<FAssetToolsModule>(
							TEXT("AssetTools"))
							.Get()
							.ImportAssetsAutomated(ImportData);
				}
				ImportData->RemoveFromRoot();
				if (Factory)
				{
					Factory->RemoveFromRoot();
				}
				for (UObject* Object : ImportedObjects)
				{
					if (IsValid(Object)
						&& Object->IsA(ExpectedClassFor(Spec.Kind))
						&& Object->GetOutermost()
						&& Object->GetOutermost()->GetName() == PackageName)
					{
						Primary = Object;
						break;
					}
				}
				if (!Primary)
				{
					Primary = LoadObject<UObject>(
						nullptr,
						*TargetObjectPath);
				}
				bMutationStarted = Primary != nullptr
					|| !ImportedObjects.IsEmpty();
				if (!Primary)
				{
					SetError(
						OutError,
						TEXT("factory_import_native_failed"),
						TargetObjectPath);
					return false;
				}
				return true;
			}

			bool ExecuteReimport(FFactoryExternalImportError& OutError)
			{
				if (!Primary)
				{
					SetError(
						OutError,
						TEXT("factory_reimport_target_missing"),
						TargetObjectPath);
					return false;
				}
				TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
				if (!FReimportManager::Instance()->Reimport(
						Primary,
						false,
						false,
						Source))
				{
					SetError(
						OutError,
						TEXT("factory_reimport_native_failed"),
						TargetObjectPath);
					return false;
				}
				bMutationStarted = true;
				return true;
			}

			bool RollbackImport()
			{
				TArray<UObject*> Objects = ImportedObjects;
				if (Primary)
				{
					Objects.AddUnique(Primary);
				}
				if (!Objects.IsEmpty())
				{
					TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
					ObjectTools::DeleteObjectsUnchecked(Objects);
				}
				ImportedObjects.Reset();
				Primary = nullptr;
				CollectGarbage(RF_NoFlags, true);
				TSet<FString> PackageNames;
				for (const FString& PersistedPackageName : PersistedPackageNames)
				{
					PackageNames.Add(PersistedPackageName);
				}
				for (const FString& OwnedPackageName : OwnedPackageNames)
				{
					PackageNames.Add(OwnedPackageName);
				}
				PackageNames.Add(PackageName);
				bool bFilesRemoved = true;
				for (const FString& Name : PackageNames)
				{
					const FString Filename =
						FPackageName::LongPackageNameToFilename(
							Name,
							FPackageName::GetAssetPackageExtension());
					if (IFileManager::Get().FileExists(*Filename))
					{
						bFilesRemoved = IFileManager::Get().Delete(
							*Filename,
							false,
							true,
							true) && bFilesRemoved;
					}
				}
				bool bPackagesAbsent = true;
				for (const FString& Name : PackageNames)
				{
					FString Existing;
					bPackagesAbsent = bPackagesAbsent
						&& !FPackageName::DoesPackageExist(Name, &Existing)
						&& FindPackage(nullptr, *Name) == nullptr;
				}
				return bFilesRemoved && bPackagesAbsent;
			}

			bool RollbackReimport()
			{
				if (BackupFilename.IsEmpty()
					|| !IFileManager::Get().FileExists(*BackupFilename))
				{
					return false;
				}
				if (Primary && Primary->GetOutermost())
				{
					UPackage* const Package = Primary->GetOutermost();
					Package->SetDirtyFlag(false);
					Primary = nullptr;
					FText Error;
					if (!UPackageTools::UnloadPackages({Package}, Error, true))
					{
						return false;
					}
				}
				CollectGarbage(RF_NoFlags, true);
				if (IFileManager::Get().Copy(
						*PackageFilename,
						*BackupFilename,
						true,
						true)
					!= COPY_OK)
				{
					return false;
				}
				DeleteBackup();
				Primary = LoadObject<UObject>(nullptr, *TargetObjectPath);
				return Primary
					&& HashFileSha256(PackageFilename) == ExpectedRevision;
			}

			void DeleteBackup()
			{
				if (!BackupFilename.IsEmpty()
					&& IFileManager::Get().FileExists(*BackupFilename))
				{
					IFileManager::Get().Delete(
						*BackupFilename,
						false,
						true,
						true);
				}
				BackupFilename.Reset();
			}

			FFactoryExternalImportSpec Spec;
			FString Source;
			FString SourceSha256;
			FString PackageName;
			FString TargetObjectPath;
			FString ExpectedRevision;
			FString PackageFilename;
			FString BackupFilename;
			UObject* Primary = nullptr;
			TArray<UObject*> ImportedObjects;
			TArray<FString> OwnedPackageNames;
			TArray<FString> PersistedPackageNames;
			bool bMutationStarted = false;
		};

		class FProvider final : public IFactoryExternalImportProvider
		{
		public:
			virtual bool Prepare(
				const FFactoryExternalImportSpec& Spec,
				TSharedPtr<IFactoryExternalImportSession>& OutSession,
				FFactoryExternalImportError& OutError) override
			{
				OutSession.Reset();
				OutError = {};
				if (!IsInGameThread())
				{
					SetError(
						OutError,
						TEXT("factory_import_game_thread_required"));
					return false;
				}
				if (!Spec.IsValid())
				{
					SetError(
						OutError,
						TEXT("factory_import_spec_invalid"));
					return false;
				}
				FString Source;
				if (!TryNormalizeSource(
						Spec.SourceRef,
						Spec.Kind,
						Source,
						OutError))
				{
					return false;
				}
				const FString SourceSha256 = HashFileSha256(Source);
				if (SourceSha256.IsEmpty())
				{
					SetError(
						OutError,
						TEXT("factory_import_source_hash_failed"),
						Source);
					return false;
				}
				FString PackageName;
				const FString TargetRef =
					Spec.Operation == EFactoryExternalImportOperation::Import
						? Spec.DestinationRef
						: Spec.DestinationRef;
				if (!TryPackageName(TargetRef, PackageName))
				{
					SetError(
						OutError,
						TEXT("factory_import_target_invalid"),
						TargetRef);
					return false;
				}
				if (Spec.Operation == EFactoryExternalImportOperation::Import
					&& FPaths::GetBaseFilename(Source)
						!= FPackageName::GetLongPackageAssetName(PackageName))
				{
					SetError(
						OutError,
						TEXT("factory_import_source_name_mismatch"),
						Source,
						TEXT("source basename must match destination asset name"));
					return false;
				}
				FString PackageFilename;
				FString ExpectedRevision = TEXT("absent");
				FString BackupFilename;
				UObject* ExistingAsset = nullptr;
				if (Spec.Operation == EFactoryExternalImportOperation::Reimport)
				{
					if (!FPackageName::DoesPackageExist(
							PackageName,
							&PackageFilename))
					{
						SetError(
							OutError,
							TEXT("factory_reimport_target_missing"),
							ObjectPath(PackageName));
						return false;
					}
					ExistingAsset = LoadObject<UObject>(
						nullptr,
						*ObjectPath(PackageName));
					if (!ExistingAsset
						|| !ExistingAsset->IsA(ExpectedClassFor(Spec.Kind)))
					{
						SetError(
							OutError,
							TEXT("factory_reimport_type_mismatch"),
							ObjectPath(PackageName));
						return false;
					}
					if (ExistingAsset->GetOutermost()->IsDirty())
					{
						SetError(
							OutError,
							TEXT("factory_reimport_dirty_package"),
							ObjectPath(PackageName));
						return false;
					}
					ExpectedRevision = HashFileSha256(PackageFilename);
					const FString BackupRoot = FPaths::ProjectSavedDir()
						/ TEXT("UAI/FactoryExternalImportRollback");
					IFileManager::Get().MakeDirectory(*BackupRoot, true);
					BackupFilename = BackupRoot
						/ (FGuid::NewGuid().ToString(EGuidFormats::Digits)
							+ FPackageName::GetAssetPackageExtension());
					if (ExpectedRevision.IsEmpty()
						|| IFileManager::Get().Copy(
								*BackupFilename,
								*PackageFilename,
								true,
								true)
							!= COPY_OK)
					{
						SetError(
							OutError,
							TEXT("factory_reimport_backup_failed"),
							PackageFilename,
							BackupFilename);
						return false;
					}
				}
				else
				{
					PackageFilename =
						FPackageName::LongPackageNameToFilename(
							PackageName,
							FPackageName::GetAssetPackageExtension());
				}
				OutSession = MakeShared<FSession>(
					Spec,
					MoveTemp(Source),
					SourceSha256,
					MoveTemp(PackageName),
					MoveTemp(ExpectedRevision),
					MoveTemp(PackageFilename),
					MoveTemp(BackupFilename),
					ExistingAsset);
				return true;
			}
		};
	}

	bool FFactoryExternalImportOptions::IsValid() const
	{
		return IsSha256(Fingerprint);
	}

	bool FFactoryExternalImportSpec::IsValid() const
	{
		return !RequestId.IsEmpty()
			&& !SourceRef.IsEmpty()
			&& !DestinationRef.IsEmpty()
			&& Options.IsValid()
			&& (Kind != EFactoryExternalImportKind::FbxAnimation
				|| Operation != EFactoryExternalImportOperation::Import
				|| !SkeletonRef.IsEmpty());
	}

	bool TryCreateFactoryExternalImportProvider(
		TSharedPtr<IFactoryExternalImportProvider>& OutProvider,
		FString& OutError)
	{
		OutProvider = MakeShared<FProvider>();
		OutError.Reset();
		return true;
	}
}
