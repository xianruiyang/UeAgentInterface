#include "DomainAdapters/ExternalImport/Factory/UeAgentFactoryExternalImportProvider.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "DomainAdapters/ExternalImport/Factory/UeAgentNativeImportFactory.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"

#include "AbcImportSettings.h"
#include "AlembicImportFactory.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetToolsModule.h"
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
#include "Factories/FbxStaticMeshImportData.h"
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
#include "Core/AssetDurable/UeAgentPackageSave.h"
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

		Contract::FDiagnostic NativeValueFailure(const TCHAR* Code, const FString& Reason,
			const FString& Target, const FString& Field, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected, const FString& Detail = FString())
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("external_import"), Detail);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.TargetPath = Target;
			Diagnostic.PropertyPath = Field;
			Diagnostic.ActualValuePresent = Actual.IsValid();
			Diagnostic.ExpectedValuePresent = Expected.IsValid();
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		Contract::FDiagnostic NativeConditionFailure(const TCHAR* Code, const FString& Reason,
			const FString& Target, const FString& Field, const bool Actual, const bool Expected,
			const FString& Detail = FString())
		{
			return NativeValueFailure(Code, Reason, Target, Field, MakeShared<FJsonValueBoolean>(Actual),
				MakeShared<FJsonValueBoolean>(Expected), Detail);
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

		bool TryHashBytes(const uint8* Data, const int64 Size, FString& OutHash,
			FFactoryExternalImportError& OutError, const FString& Target = FString())
		{
			OutHash.Reset();
			FSHA256Signature Signature{};
			const uint8 Empty = 0;
			if (!SHA256(Size > 0 ? Data : &Empty, static_cast<size_t>(Size), Signature.Signature))
			{
				OutError = NativeConditionFailure(TEXT("factory_import_sha256_failed"),
					TEXT("OpenSSL could not calculate the SHA256 value."), Target, TEXT("sha256_calculated"), false, true);
				return false;
			}
			OutHash = TEXT("sha256:") + Signature.ToString().ToLower();
			return true;
		}

		bool TryHashFileSha256(const FString& Filename, FString& OutHash,
			FFactoryExternalImportError& OutError, const FString& Target = FString())
		{
			TArray<uint8> Bytes;
			FString PublicFilename = Filename;
			FPaths::MakePathRelativeTo(PublicFilename, *FPaths::ProjectDir());
			if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
			{
				OutHash.Reset();
				OutError = NativeConditionFailure(TEXT("factory_import_file_read_failed"),
					TEXT("The import file could not be read for SHA256 calculation."), Target, TEXT("file_read_succeeded"), false, true);
				OutError.Source.SourceFile = PublicFilename;
				OutError.InternalContext = Filename;
				return false;
			}
			if (!TryHashBytes(Bytes.GetData(), Bytes.Num(), OutHash, OutError, Target))
			{
				OutError.Source.SourceFile = PublicFilename;
				OutError.InternalContext = Filename;
				return false;
			}
			return true;
		}

		bool TryHashUtf8Sha256(const FString& Value, FString& OutHash,
			FFactoryExternalImportError& OutError, const FString& Target)
		{
			const FTCHARToUTF8 Utf8(*Value);
			return TryHashBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), OutHash, OutError, Target);
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
			int64 Value = 0;
			if (!UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(Enum, Name, Value))
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
			case EFactoryExternalImportKind::FbxStaticMesh:
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
			case EFactoryExternalImportKind::FbxStaticMesh:
				return {TEXT("fbx")};
			case EFactoryExternalImportKind::GeometryCache:
				return {TEXT("abc")};
			case EFactoryExternalImportKind::SparseVolumeTexture:
				return {TEXT("vdb")};
			case EFactoryExternalImportKind::StaticMesh:
			case EFactoryExternalImportKind::Texture:
			case EFactoryExternalImportKind::LightProfile:
			case EFactoryExternalImportKind::SoundWave:
				return NativeImportExtensions(ExpectedClassFor(Kind));
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
				OutError = NativeValueFailure(TEXT("factory_import_source_required"), TEXT("An import source file is required."),
					FString(), TEXT("source_ref"), MakeShared<FJsonValueString>(SourceRef), MakeShared<FJsonValueString>(TEXT("nonempty source file path")));
				OutError.PropertyPath = TEXT("source_ref");
				OutError.PublicMessage = TEXT("An import source file is required.");
				return false;
			}
			OutSource = FPaths::IsRelative(SourceRef)
				? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), SourceRef)
				: FPaths::ConvertRelativePathToFull(SourceRef);
			FPaths::NormalizeFilename(OutSource);
			FString PublicSource = OutSource;
			FPaths::MakePathRelativeTo(PublicSource, *FPaths::ProjectDir());
			if (!FPaths::FileExists(OutSource))
			{
				OutError = NativeValueFailure(TEXT("factory_import_source_not_found"), TEXT("The import source file does not exist."),
					FString(), TEXT("source_file_exists"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.Source.SourceFile = PublicSource;
				OutError.PropertyPath = TEXT("source_ref");
				OutError.PublicMessage = TEXT("The import source file does not exist.");
				return false;
			}
			const FString Extension = FPaths::GetExtension(OutSource).ToLower();
			if (!AllowedExtensions(Kind).Contains(Extension))
			{
				OutError = NativeValueFailure(TEXT("factory_import_source_extension_invalid"), TEXT("The source file extension is not supported by this import kind."),
					FString(), TEXT("source_extension"), MakeShared<FJsonValueString>(Extension), MakeShared<FJsonValueString>(TEXT("supported extension for the import kind")), Extension);
				TArray<FString> Extensions = AllowedExtensions(Kind).Array();
				Extensions.Sort();
				TArray<TSharedPtr<FJsonValue>> Choices;
				for (const auto& Allowed : Extensions) Choices.Add(MakeShared<FJsonValueString>(Allowed));
				OutError.Source.SourceFile = PublicSource;
				OutError.PropertyPath = TEXT("source_ref");
				OutError.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Choices));
				OutError.ActualValue = MakeShared<FJsonValueString>(Extension);
				OutError.PublicMessage = TEXT("The source file extension is not supported by this import kind.");
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
					OutError = NativeValueFailure(TEXT("factory_import_animation_state_invalid"), !Skeleton ? TEXT("The imported animation has no skeleton.") : Skeleton->GetPathName() != Spec.SkeletonRef ? TEXT("The imported animation references a different skeleton.") : TEXT("The imported animation has no sampled keys."),
					Asset.GetPathName(), !Skeleton || Skeleton->GetPathName() != Spec.SkeletonRef ? TEXT("skeleton_ref") : TEXT("sampled_key_count"), !Skeleton || Skeleton->GetPathName() != Spec.SkeletonRef ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Skeleton ? Skeleton->GetPathName() : FString())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(OutItemCount)), !Skeleton || Skeleton->GetPathName() != Spec.SkeletonRef ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Spec.SkeletonRef)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("positive sampled key count"))), Spec.SkeletonRef);
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
					OutError = NativeValueFailure(TEXT("factory_import_skeletal_state_invalid"), !Skeleton ? TEXT("The imported skeletal mesh has no skeleton.") : TEXT("The imported skeletal mesh has no reference bones."),
					Asset.GetPathName(), !Skeleton ? TEXT("skeleton_available") : TEXT("reference_bone_count"), !Skeleton ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(OutItemCount)), !Skeleton ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("positive reference bone count"))));
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
			case EFactoryExternalImportKind::FbxStaticMesh:
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
				OutError = OutStateKind.IsEmpty()
					? NativeValueFailure(TEXT("factory_import_business_state_invalid"), TEXT("The native import kind has no supported business state reader."),
						Asset.GetPathName(), TEXT("import_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Spec.Kind)), MakeShared<FJsonValueString>(TEXT("supported native import kind")))
					: NativeValueFailure(TEXT("factory_import_business_state_invalid"), TEXT("The imported asset has no usable items in its required native business state."),
						Asset.GetPathName(), OutStateKind, MakeShared<FJsonValueNumber>(OutItemCount), MakeShared<FJsonValueString>(TEXT("positive item count")));
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
				OutError = NativeValueFailure(TEXT("factory_import_texture_state_invalid"), TEXT("The imported asset is not a Texture2D."),
					Asset.GetPathName(), TEXT("native_asset_class"), MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()), MakeShared<FJsonValueString>(UTexture2D::StaticClass()->GetPathName()));
				return false;
			}
			TextureCompressionSettings Compression =
				static_cast<TextureCompressionSettings>(Texture->CompressionSettings.GetValue());
			TextureMipGenSettings Mip =
				static_cast<TextureMipGenSettings>(Texture->MipGenSettings.GetValue());
			TextureGroup Group =
				static_cast<TextureGroup>(Texture->LODGroup.GetValue());
			const auto ValidateOption = [&](const FString& Name, auto& Value, const TCHAR* Field, const TCHAR* EnumType)
			{
				if (TryEnumByName(Name, Value)) return true;
				OutError = NativeValueFailure(TEXT("factory_import_texture_option_invalid"), TEXT("The texture option is not a member of the required enum."),
					Asset.GetPathName(), Field, MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(EnumType));
				OutError.TargetPath = Asset.GetPathName();
				OutError.PropertyPath = Field;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(EnumType);
				OutError.ActualValue = MakeShared<FJsonValueString>(Name);
				OutError.PublicMessage = TEXT("The texture option is not a member of the required enum.");
				return false;
			};
			if (!ValidateOption(Spec.Options.CompressionSettings, Compression, TEXT("compression_settings"), TEXT("TextureCompressionSettings"))
				|| !ValidateOption(Spec.Options.MipGenSettings, Mip, TEXT("mip_gen_settings"), TEXT("TextureMipGenSettings"))
				|| !ValidateOption(Spec.Options.LodGroup, Group, TEXT("lod_group"), TEXT("TextureGroup"))) return false;
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
				auto Mismatch = [&](const TCHAR* Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
				{
					OutError = NativeValueFailure(TEXT("factory_import_texture_readback_mismatch"),
						TEXT("The reloaded texture option differs from the requested value."), Readback.TargetIdentity,
						Field, MoveTemp(Actual), MoveTemp(Expected));
					return false;
				};
				if (Spec.Options.Srgb.IsSet() && (!Readback.Srgb.IsSet() || Readback.Srgb.GetValue() != Spec.Options.Srgb.GetValue()))
					return Mismatch(TEXT("srgb"), Readback.Srgb.IsSet()
						? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(Readback.Srgb.GetValue())) : nullptr,
						MakeShared<FJsonValueBoolean>(Spec.Options.Srgb.GetValue()));
				if (!Spec.Options.CompressionSettings.IsEmpty() && Readback.CompressionSettings != Spec.Options.CompressionSettings)
					return Mismatch(TEXT("compression_settings"), MakeShared<FJsonValueString>(Readback.CompressionSettings),
						MakeShared<FJsonValueString>(Spec.Options.CompressionSettings));
				if (!Spec.Options.MipGenSettings.IsEmpty() && Readback.MipGenSettings != Spec.Options.MipGenSettings)
					return Mismatch(TEXT("mip_gen_settings"), MakeShared<FJsonValueString>(Readback.MipGenSettings),
						MakeShared<FJsonValueString>(Spec.Options.MipGenSettings));
				if (!Spec.Options.LodGroup.IsEmpty() && Readback.LodGroup != Spec.Options.LodGroup)
					return Mismatch(TEXT("lod_group"), MakeShared<FJsonValueString>(Readback.LodGroup),
						MakeShared<FJsonValueString>(Spec.Options.LodGroup));
			}
			if (Spec.Kind == EFactoryExternalImportKind::FbxSkeletalMesh
				&& Spec.Options.bValidateAfterImport)
			{
				if (Spec.Options.ExpectedLodCount != INDEX_NONE
					&& Readback.LodCount != Spec.Options.ExpectedLodCount)
				{
					OutError = NativeValueFailure(TEXT("factory_import_expected_lod_missing"), TEXT("The reloaded skeletal mesh has a different LOD count."),
					Readback.TargetIdentity, TEXT("lod_count"), MakeShared<FJsonValueNumber>(Readback.LodCount), MakeShared<FJsonValueNumber>(Spec.Options.ExpectedLodCount));
					OutError.TargetPath = Readback.TargetIdentity;
					OutError.PropertyPath = TEXT("lod_count");
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Spec.Options.ExpectedLodCount);
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Readback.LodCount);
					return false;
				}
				for (const FString& Name : Spec.Options.ExpectedMorphTargets)
				{
					if (!Readback.MorphTargetNames.Contains(Name))
					{
						OutError = NativeValueFailure(TEXT("factory_import_expected_morph_target_missing"), TEXT("The reloaded skeletal mesh is missing a requested morph target."),
					Readback.TargetIdentity, TEXT("morph_targets"), nullptr, MakeShared<FJsonValueString>(Name));
						OutError.TargetPath = Readback.TargetIdentity;
						OutError.PropertyPath = TEXT("morph_targets");
						OutError.CandidateName = Name;
						OutError.ExpectedValuePresent = true;
						OutError.ActualValuePresent = false;
						return false;
					}
				}
				for (const FString& Name : Spec.Options.ExpectedMaterialSlots)
				{
					if (!Readback.MaterialSlotNames.Contains(Name))
					{
						OutError = NativeValueFailure(TEXT("factory_import_expected_material_slot_missing"), TEXT("The reloaded skeletal mesh is missing a requested material slot."),
					Readback.TargetIdentity, TEXT("material_slots"), nullptr, MakeShared<FJsonValueString>(Name));
						OutError.TargetPath = Readback.TargetIdentity;
						OutError.PropertyPath = TEXT("material_slots");
						OutError.CandidateName = Name;
						OutError.ExpectedValuePresent = true;
						OutError.ActualValuePresent = false;
						return false;
					}
				}
			}
			return true;
		}

		bool SourceMatchesImportData(
			UAssetImportData& ImportData,
			const FString& Source, TArray<FString>& OutSources)
		{
			OutSources.Reset();
			ImportData.ExtractFilenames(OutSources);
			for (FString& Filename : OutSources)
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

		Contract::FDiagnostic ImportSourceMismatch(const TCHAR* Code, const FString& Target,
			const FString& Source, const TArray<FString>& ActualSources)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (FString Actual : ActualSources)
			{
				FPaths::MakePathRelativeTo(Actual, *FPaths::ProjectDir());
				Values.Add(MakeShared<FJsonValueString>(Actual));
			}
			FString Expected = Source;
			FPaths::MakePathRelativeTo(Expected, *FPaths::ProjectDir());
			return NativeValueFailure(Code, TEXT("None of the native import data sources match the requested source file."),
				Target, TEXT("native_import_sources"), MakeShared<FJsonValueArray>(MoveTemp(Values)), MakeShared<FJsonValueString>(Expected));
		}

		UFactory* CreateImportFactory(
			const FFactoryExternalImportSpec& Spec,
			const FString& Source,
			FFactoryExternalImportError& OutError)
		{
			if (Spec.Kind == EFactoryExternalImportKind::GeometryCache)
			{
				UAlembicImportFactory* const Factory =
					NewObject<UAlembicImportFactory>();
				if (!Factory)
				{
					OutError = NativeValueFailure(TEXT("factory_import_alembic_factory_unavailable"), TEXT("The Alembic import factory could not be created."),
					Spec.DestinationRef, TEXT("alembic_import_factory_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return nullptr;
				}
				Factory->ImportSettings = NewObject<UAbcImportSettings>(Factory);
				if (!Factory->ImportSettings)
				{
					OutError = NativeValueFailure(TEXT("factory_import_alembic_settings_unavailable"), TEXT("The Alembic import factory has no import settings."),
					Spec.DestinationRef, TEXT("alembic_import_settings_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
					OutError = NativeValueFailure(TEXT("factory_import_sparse_volume_factory_unavailable"), TEXT("The sparse volume texture import factory could not be created."),
					Spec.DestinationRef, TEXT("sparse_volume_import_factory_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				}
				return Factory;
			}
			if (Spec.Kind != EFactoryExternalImportKind::FbxAnimation
				&& Spec.Kind != EFactoryExternalImportKind::FbxSkeletalMesh
				&& Spec.Kind != EFactoryExternalImportKind::FbxStaticMesh)
			{
				UFactory* Factory = SelectNativeImportFactory(ExpectedClassFor(Spec.Kind), Source);
				if (!Factory) OutError = NativeValueFailure(TEXT("factory_import_native_factory_unavailable"), TEXT("No native import factory supports this source file and asset class."),
					Spec.DestinationRef, TEXT("native_import_factory_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return Factory;
			}
			UFbxFactory* const Factory = NewObject<UFbxFactory>();
			if (!Factory || !Factory->ImportUI)
			{
				OutError = NativeValueFailure(TEXT("factory_import_fbx_factory_unavailable"), !Factory ? TEXT("The FBX import factory could not be created.") : TEXT("The FBX import factory has no import UI settings."),
					Spec.DestinationRef, !Factory ? TEXT("fbx_import_factory_available") : TEXT("fbx_import_settings_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return nullptr;
			}
			Factory->SetDetectImportTypeOnImport(false);
			Factory->ImportUI->bAutomatedImportShouldDetectType = false;
			Factory->ImportUI->bImportAsSkeletal = Spec.Kind != EFactoryExternalImportKind::FbxStaticMesh;
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
			if (Spec.Kind == EFactoryExternalImportKind::FbxStaticMesh)
			{
				Factory->ImportUI->MeshTypeToImport = FBXIT_StaticMesh;
				Factory->ImportUI->OriginalImportType = FBXIT_StaticMesh;
				Factory->ImportUI->bImportAnimations = false;
				Factory->ImportUI->bImportMesh = true;
				Factory->ImportUI->Skeleton = nullptr;
				Factory->ImportUI->bCreatePhysicsAsset = false;
				// One destination owns all source mesh nodes and their material slots.
				Factory->ImportUI->StaticMeshImportData->bCombineMeshes = true;
				Factory->ImportUI->StaticMeshImportData->bConvertScene = true;
				Factory->ImportUI->StaticMeshImportData->bConvertSceneUnit = true;
				Factory->ImportUI->StaticMeshImportData->bTransformVertexToAbsolute = true;
				Factory->ImportUI->StaticMeshImportData->NormalImportMethod = FBXNIM_ImportNormalsAndTangents;
			}
			else if (Spec.Kind == EFactoryExternalImportKind::FbxAnimation)
			{
				USkeleton* const Skeleton = LoadObject<USkeleton>(
					nullptr,
					*Spec.SkeletonRef);
				if (!Skeleton)
				{
					OutError = NativeValueFailure(TEXT("factory_import_skeleton_not_found"), TEXT("The requested skeleton asset could not be loaded."),
					Spec.DestinationRef, TEXT("skeleton_ref"), MakeShared<FJsonValueString>(Spec.SkeletonRef), MakeShared<FJsonValueString>(TEXT("loadable Skeleton asset reference")));
					OutError.PropertyPath = TEXT("skeleton_ref");
					OutError.ActualValue = MakeShared<FJsonValueString>(Spec.SkeletonRef);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("loadable Skeleton asset reference"));
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
					OutError = NativeValueFailure(TEXT("factory_import_skeleton_not_found"), TEXT("The requested skeleton asset could not be loaded."),
					Spec.DestinationRef, TEXT("skeleton_ref"), MakeShared<FJsonValueString>(Spec.SkeletonRef), MakeShared<FJsonValueString>(TEXT("loadable Skeleton asset reference")));
					OutError.PropertyPath = TEXT("skeleton_ref");
					OutError.ActualValue = MakeShared<FJsonValueString>(Spec.SkeletonRef);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("loadable Skeleton asset reference"));
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
						OutError = NativeValueFailure(TEXT("factory_import_destination_conflict"), TEXT("The import destination already has a persisted package or loaded object."),
					TargetObjectPath, TEXT("destination_present"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					OutError.TargetPath = TargetObjectPath;
						return false;
					}
					return true;
				}
				if (!bPackageExists || !Primary)
				{
					OutError = NativeConditionFailure(TEXT("factory_reimport_version_conflict"),
						!bPackageExists ? TEXT("The reimport target package is no longer persisted.") : TEXT("The reimport native target is unavailable."),
						TargetObjectPath, !bPackageExists ? TEXT("persisted_package_available") : TEXT("native_asset_available"), false, true);
					return false;
				}
				FString CurrentRevision;
				if (!TryHashFileSha256(PackageFilename, CurrentRevision, OutError, TargetObjectPath)) return false;
				if (CurrentRevision != ExpectedRevision)
				{
					OutError = NativeConditionFailure(TEXT("factory_reimport_version_conflict"),
						TEXT("The persisted reimport target changed before execution."), TargetObjectPath, TEXT("package_revision_matches"), false, true);
					OutError.ActualEvidence = MakeShared<FJsonValueString>(CurrentRevision);
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedRevision);
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
				if (!Primary || !Primary->IsA(ExpectedClassFor(Spec.Kind)) || !Primary->GetOutermost()
					|| Primary->GetOutermost()->GetName() != PackageName)
				{
					if (!Primary)
						OutError = NativeConditionFailure(TEXT("factory_import_settle_target_invalid"),
							TEXT("The imported native target is unavailable."), TargetObjectPath, TEXT("native_asset_available"), false, true);
					else if (!Primary->IsA(ExpectedClassFor(Spec.Kind)))
						OutError = NativeValueFailure(TEXT("factory_import_settle_target_invalid"),
							TEXT("The imported native target has the wrong class."), TargetObjectPath, TEXT("native_asset_class"),
							MakeShared<FJsonValueString>(Primary->GetClass()->GetPathName()), MakeShared<FJsonValueString>(ExpectedClassFor(Spec.Kind)->GetPathName()));
					else
						OutError = NativeValueFailure(TEXT("factory_import_settle_target_invalid"),
							TEXT("The imported native target is not in the requested destination package."), TargetObjectPath, TEXT("native_package"),
							Primary->GetOutermost() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Primary->GetOutermost()->GetName())) : nullptr,
							MakeShared<FJsonValueString>(PackageName));
					return false;
				}
				UAssetImportData* const ImportData = FindImportData(*Primary);
				TArray<FString> NativeSources;
				if (!ImportData || !SourceMatchesImportData(*ImportData, Source, NativeSources))
				{
					OutError = !ImportData ? NativeConditionFailure(TEXT("factory_import_settle_import_data_invalid"),
						TEXT("The imported asset has no native import data."), TargetObjectPath, TEXT("native_import_data_available"), false, true)
						: ImportSourceMismatch(TEXT("factory_import_settle_import_data_invalid"), TargetObjectPath, Source, NativeSources);
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
					OutError = NativeValueFailure(TEXT("factory_import_pre_persist_fence_failed"), !Primary ? TEXT("The imported native asset is unavailable before persistence.") : !Primary->GetOutermost() ? TEXT("The imported asset has no package before persistence.") : TEXT("The imported package is not dirty before persistence."),
					TargetObjectPath, !Primary ? TEXT("native_asset_available") : !Primary->GetOutermost() ? TEXT("native_package_available") : TEXT("native_package_dirty"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.TargetPath = TargetObjectPath;
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
					UE::UAI::Contract::FDiagnostic SaveDetail;
					if (!UE::UAI::AssetDurable::TrySaveLoadedPackage(
							*Package,
							Package == Primary->GetOutermost() ? Primary : nullptr,
							Filename,
							Args, SaveDetail))
					{
						OutError = SaveDetail;
						return false;
					}
					PersistedPackageNames.AddUnique(Package->GetName());
					FString PackageHash;
					if (!TryHashFileSha256(Filename, PackageHash, OutError, Package->GetName()))
					{
						OutError = Contract::FDiagnostic::WrapFailure(TEXT("factory_import_persistence_receipt_missing"), OutError);
						return false;
					}
					PersistedStates.Add(Package->GetName() + TEXT("=") + PackageHash);
				}
				if (!TryHashUtf8Sha256(FString::Join(PersistedStates, TEXT("\n")),
					OutPersistenceReceiptId, OutError, TargetObjectPath))
				{
					OutError = Contract::FDiagnostic::WrapFailure(TEXT("factory_import_persistence_receipt_missing"), OutError);
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
					OutError = NativeValueFailure(TEXT("factory_import_unload_failed"), UnloadError.IsEmpty() ? TEXT("The persisted import packages could not be unloaded.") : UnloadError.ToString(),
					TargetObjectPath, TEXT("native_packages_unloaded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), UnloadError.ToString());
					if (!UnloadError.IsEmpty()) OutError.PublicMessage = UnloadError.ToString();
					OutError.InternalContext.Reset();
					OutError.TargetPath = TargetObjectPath;
					return false;
				}
				CollectGarbage(RF_NoFlags, true);
				UObject* const Reloaded = LoadObject<UObject>(
					nullptr,
					*TargetObjectPath);
				if (!Reloaded || !Reloaded->IsA(ExpectedClassFor(Spec.Kind)))
				{
					OutError = NativeValueFailure(TEXT("factory_import_reload_failed"), !Reloaded ? TEXT("The persisted import target could not be reloaded.") : TEXT("The reloaded import target has the wrong native class."),
					TargetObjectPath, TEXT("native_asset_class"), Reloaded ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Reloaded->GetClass()->GetPathName())) : nullptr, MakeShared<FJsonValueString>(ExpectedClassFor(Spec.Kind)->GetPathName()));
				OutError.TargetPath = TargetObjectPath;
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
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(OutError.Code, OutError, TEXT("factory_import_observation"));
					OutError = {};
				}
				TArray<FString> NativeSources;
				const bool bNativeSourceMatches = ImportData && SourceMatchesImportData(*ImportData, Source, NativeSources);
				const bool bEquivalent = bNativeSourceMatches
					&& FPaths::IsSamePath(OutReadback.SourceRef, Source)
					&& OutReadback.SourceSha256 == SourceSha256
					&& OutReadback.OptionsFingerprint
						== Spec.Options.Fingerprint
					&& OutReadback.Operation
						== OperationName(Spec.Operation);
				if (!bEquivalent)
				{
					if (!ImportData || !bNativeSourceMatches)
						OutError = !ImportData ? NativeConditionFailure(TEXT("factory_import_readback_mismatch"),
							TEXT("The reloaded asset has no native import data."), TargetObjectPath, TEXT("native_import_data_available"), false, true)
							: ImportSourceMismatch(TEXT("factory_import_readback_mismatch"), TargetObjectPath, Source, NativeSources);
					else if (!FPaths::IsSamePath(OutReadback.SourceRef, Source))
					{
						FString ActualSource = OutReadback.SourceRef;
						FString ExpectedSource = Source;
						FPaths::MakePathRelativeTo(ActualSource, *FPaths::ProjectDir());
						FPaths::MakePathRelativeTo(ExpectedSource, *FPaths::ProjectDir());
						OutError = NativeValueFailure(TEXT("factory_import_readback_mismatch"),
							TEXT("The reloaded import source metadata differs from the requested source."), TargetObjectPath, TEXT("source_ref"),
							MakeShared<FJsonValueString>(ActualSource), MakeShared<FJsonValueString>(ExpectedSource));
					}
					else if (OutReadback.SourceSha256 != SourceSha256 || OutReadback.OptionsFingerprint != Spec.Options.Fingerprint)
					{
						const bool bSourceMismatch = OutReadback.SourceSha256 != SourceSha256;
						OutError = NativeConditionFailure(TEXT("factory_import_readback_mismatch"),
							bSourceMismatch ? TEXT("The reloaded source fingerprint differs from the imported source.")
								: TEXT("The reloaded options fingerprint differs from the requested options."), TargetObjectPath,
							bSourceMismatch ? TEXT("source_fingerprint_matches") : TEXT("options_fingerprint_matches"), false, true);
						OutError.ActualEvidence = MakeShared<FJsonValueString>(bSourceMismatch ? OutReadback.SourceSha256 : OutReadback.OptionsFingerprint);
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(bSourceMismatch ? SourceSha256 : Spec.Options.Fingerprint);
					}
					else
						OutError = NativeValueFailure(TEXT("factory_import_readback_mismatch"),
							TEXT("The reloaded import operation metadata differs from the requested operation."), TargetObjectPath, TEXT("operation"),
							MakeShared<FJsonValueString>(OutReadback.Operation), MakeShared<FJsonValueString>(OperationName(Spec.Operation)));
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(OutError.Code, OutError, TEXT("factory_import_observation"));
					OutError = {};
				}
				Primary = Reloaded;
				DeleteBackup();
				return true;
			}

			virtual bool Rollback(
				bool& bOutConfirmed,
				FFactoryExternalImportError& OutError) override
			{
				OutError.Reset();
				bOutConfirmed = Spec.Operation == EFactoryExternalImportOperation::Import
					? RollbackImport(OutError) : RollbackReimport(OutError);
				if (!bOutConfirmed && !OutError.IsEmpty())
					OutError = Contract::FDiagnostic::WrapFailure(TEXT("factory_import_rollback_unconfirmed"), OutError);
				return bOutConfirmed;
			}

		private:
			bool ExecuteImport(FFactoryExternalImportError& OutError)
			{
				UFactory* const Factory = CreateImportFactory(Spec, Source, OutError);
				if (!Factory)
				{
					FString PublicSource = Source;
					FPaths::MakePathRelativeTo(PublicSource, *FPaths::ProjectDir());
					OutError.Source.SourceFile = PublicSource;
					return false;
				}
				ImportedObjects = ImportNativeAsset(Factory, Source, PackageName);
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
					OutError = NativeValueFailure(TEXT("factory_import_native_failed"), TEXT("The native import did not produce the requested target asset."),
					TargetObjectPath, TEXT("native_target_produced"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.TargetPath = TargetObjectPath;
					return false;
				}
				return true;
			}

			bool ExecuteReimport(FFactoryExternalImportError& OutError)
			{
				if (!Primary)
				{
					OutError = NativeValueFailure(TEXT("factory_reimport_target_missing"), TEXT("The persisted reimport target or native asset is unavailable."),
					TargetObjectPath, TEXT("native_target_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.TargetPath = TargetObjectPath;
					return false;
				}
				TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
				// Reimport can update AssetImportData through SetReimportPaths before
				// the factory runs. A failed factory still needs the existing backup
				// restoration; its return value does not delimit mutation start.
				bMutationStarted = true;
				if (!FReimportManager::Instance()->Reimport(
						Primary,
						false,
						false,
						Source, nullptr, INDEX_NONE, false, true, false))
				{
					OutError = NativeValueFailure(TEXT("factory_reimport_native_failed"), TEXT("The native reimport operation failed."),
					TargetObjectPath, TEXT("native_reimport_succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.TargetPath = TargetObjectPath;
					return false;
				}
				return true;
			}

			bool RollbackImport(FFactoryExternalImportError& OutError)
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
						const bool bRemoved = IFileManager::Get().Delete(*Filename, false, true, true);
						bFilesRemoved = bRemoved && bFilesRemoved;
						if (!bRemoved && OutError.IsEmpty())
						{
							OutError = NativeConditionFailure(TEXT("factory_import_rollback_delete_failed"),
								TEXT("An imported package file could not be removed during rollback."), Name, TEXT("package_file_removed"), false, true);
							OutError.Source.SourceFile = Filename;
						}
					}
				}
				bool bPackagesAbsent = true;
				for (const FString& Name : PackageNames)
				{
					if (!bPackagesAbsent) continue;
					FString Existing;
					const bool bPersisted = FPackageName::DoesPackageExist(Name, &Existing);
					const bool bLoaded = FindPackage(nullptr, *Name) != nullptr;
					bPackagesAbsent = !bPersisted && !bLoaded;
					if (!bPackagesAbsent && OutError.IsEmpty())
						OutError = NativeConditionFailure(TEXT("factory_import_rollback_package_present"),
							bPersisted ? TEXT("An imported package is still persisted after rollback.") : TEXT("An imported package is still loaded after rollback."),
							Name, bPersisted ? TEXT("persisted_package_present") : TEXT("loaded_package_present"), true, false);
				}
				return bFilesRemoved && bPackagesAbsent;
			}

			bool RollbackReimport(FFactoryExternalImportError& OutError)
			{
				if (BackupFilename.IsEmpty() || !IFileManager::Get().FileExists(*BackupFilename))
				{
					OutError = NativeConditionFailure(TEXT("factory_reimport_rollback_backup_missing"),
						TEXT("The reimport rollback backup is unavailable."), TargetObjectPath, TEXT("rollback_backup_available"), false, true, BackupFilename);
					return false;
				}
				if (Primary && Primary->GetOutermost())
				{
					UPackage* const Package = Primary->GetOutermost();
					Package->SetDirtyFlag(false);
					Primary = nullptr;
					FText NativeError;
					if (!UPackageTools::UnloadPackages({Package}, NativeError, true))
					{
						OutError = NativeConditionFailure(TEXT("factory_reimport_rollback_unload_failed"),
							NativeError.IsEmpty() ? TEXT("The reimport target package could not be unloaded for rollback.") : NativeError.ToString(),
							TargetObjectPath, TEXT("native_package_unloaded"), false, true);
						return false;
					}
				}
				CollectGarbage(RF_NoFlags, true);
				const int32 CopyResult = IFileManager::Get().Copy(*PackageFilename, *BackupFilename, true, true);
				if (CopyResult != COPY_OK)
				{
					OutError = NativeValueFailure(TEXT("factory_reimport_rollback_copy_failed"),
						TEXT("The reimport rollback backup could not be copied back to the target package."), TargetObjectPath, TEXT("native_copy_result"),
						MakeShared<FJsonValueNumber>(CopyResult), MakeShared<FJsonValueNumber>(COPY_OK), BackupFilename);
					return false;
				}
				DeleteBackup();
				Primary = LoadObject<UObject>(nullptr, *TargetObjectPath);
				if (!Primary)
				{
					OutError = NativeConditionFailure(TEXT("factory_reimport_rollback_reload_failed"),
						TEXT("The restored reimport target could not be reloaded."), TargetObjectPath, TEXT("native_asset_available"), false, true);
					return false;
				}
				FString CurrentRevision;
				if (!TryHashFileSha256(PackageFilename, CurrentRevision, OutError, TargetObjectPath)) return false;
				if (CurrentRevision != ExpectedRevision)
				{
					OutError = NativeConditionFailure(TEXT("factory_reimport_rollback_revision_mismatch"),
						TEXT("The restored reimport package does not match its pre-import revision."), TargetObjectPath, TEXT("package_revision_matches"), false, true);
					OutError.ActualEvidence = MakeShared<FJsonValueString>(CurrentRevision);
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedRevision);
					return false;
				}
				return true;
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
					OutError = NativeValueFailure(TEXT("factory_import_game_thread_required"), TEXT("Factory import preparation must run on the game thread."),
					Spec.DestinationRef, TEXT("game_thread"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				if (!Spec.IsValid())
				{
					if (Spec.RequestId.IsEmpty())
						OutError = NativeConditionFailure(TEXT("factory_import_spec_invalid"),
							TEXT("The factory import specification has no request identity."), Spec.DestinationRef, TEXT("request_identity_present"), false, true);
					else if (Spec.SourceRef.IsEmpty() || Spec.DestinationRef.IsEmpty())
						OutError = NativeValueFailure(TEXT("factory_import_spec_invalid"),
							Spec.SourceRef.IsEmpty() ? TEXT("The factory import specification has no source file.") : TEXT("The factory import specification has no destination."),
							Spec.DestinationRef, Spec.SourceRef.IsEmpty() ? TEXT("source_ref") : TEXT("destination_ref"),
							MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("nonempty reference")));
					else if (!Spec.Options.IsValid())
					{
						OutError = NativeConditionFailure(TEXT("factory_import_spec_invalid"),
							TEXT("The import options fingerprint is not a SHA256 value."), Spec.DestinationRef, TEXT("options_fingerprint_valid"), false, true);
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Spec.Options.Fingerprint);
					}
					else
						OutError = NativeValueFailure(TEXT("factory_import_spec_invalid"),
							TEXT("Importing an FBX animation requires a skeleton reference."), Spec.DestinationRef, TEXT("skeleton_ref"),
							MakeShared<FJsonValueString>(Spec.SkeletonRef), MakeShared<FJsonValueString>(TEXT("nonempty Skeleton asset reference")));
					return false;
				}
				FString Source;
				if (!TryNormalizeSource(
						Spec.SourceRef,
						Spec.Kind,
						Source,
						OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Spec.DestinationRef;
					return false;
				}
				FString SourceSha256;
				if (!TryHashFileSha256(Source, SourceSha256, OutError, Spec.DestinationRef))
				{
					OutError = Contract::FDiagnostic::WrapFailure(TEXT("factory_import_source_hash_failed"), OutError);
					return false;
				}
				FString PackageName;
				const FString TargetRef =
					Spec.Operation == EFactoryExternalImportOperation::Import
						? Spec.DestinationRef
						: Spec.DestinationRef;
				if (!TryPackageName(TargetRef, PackageName))
				{
					OutError = NativeValueFailure(TEXT("factory_import_target_invalid"), TEXT("The import destination must be a valid package or object reference under /Game/."),
					TargetRef, TEXT("destination_ref"), MakeShared<FJsonValueString>(TargetRef), MakeShared<FJsonValueString>(TEXT("valid /Game/ package or object reference")));
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
						OutError = NativeValueFailure(TEXT("factory_reimport_target_missing"), TEXT("The persisted reimport target or native asset is unavailable."),
					ObjectPath(PackageName), TEXT("native_target_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						return false;
					}
					ExistingAsset = LoadObject<UObject>(
						nullptr,
						*ObjectPath(PackageName));
					if (!ExistingAsset
						|| !ExistingAsset->IsA(ExpectedClassFor(Spec.Kind)))
					{
						OutError = NativeValueFailure(TEXT("factory_reimport_type_mismatch"), !ExistingAsset ? TEXT("The reimport target could not be loaded.") : TEXT("The reimport target has the wrong native class."),
					ObjectPath(PackageName), TEXT("native_asset_class"), ExistingAsset ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ExistingAsset->GetClass()->GetPathName())) : nullptr, MakeShared<FJsonValueString>(ExpectedClassFor(Spec.Kind)->GetPathName()));
						return false;
					}
					if (ExistingAsset->GetOutermost()->IsDirty())
					{
						OutError = NativeValueFailure(TEXT("factory_reimport_dirty_package"), TEXT("The reimport target package has unsaved changes."),
					ObjectPath(PackageName), TEXT("native_package_dirty"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
						return false;
					}
					if (!TryHashFileSha256(PackageFilename, ExpectedRevision, OutError, ObjectPath(PackageName)))
					{
						OutError = Contract::FDiagnostic::WrapFailure(TEXT("factory_reimport_backup_failed"), OutError);
						return false;
					}
					const FString BackupRoot = FPaths::ProjectSavedDir()
						/ TEXT("UAI/FactoryExternalImportRollback");
					IFileManager::Get().MakeDirectory(*BackupRoot, true);
					BackupFilename = BackupRoot
						/ (FGuid::NewGuid().ToString(EGuidFormats::Digits)
							+ FPackageName::GetAssetPackageExtension());
					const int32 CopyResult = IFileManager::Get().Copy(*BackupFilename, *PackageFilename, true, true);
					if (CopyResult != COPY_OK)
					{
						OutError = NativeValueFailure(TEXT("factory_reimport_backup_failed"),
							TEXT("The persisted reimport package could not be copied to its rollback backup."), ObjectPath(PackageName), TEXT("native_copy_result"),
							MakeShared<FJsonValueNumber>(CopyResult), MakeShared<FJsonValueNumber>(COPY_OK), BackupFilename);
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProvider = MakeShared<FProvider>();
		OutError.Reset();
		return true;
	}
}
