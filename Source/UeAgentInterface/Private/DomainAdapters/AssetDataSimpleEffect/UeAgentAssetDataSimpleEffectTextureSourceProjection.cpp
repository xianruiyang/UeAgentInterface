#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTextureSourceProjection.h"

#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureCubeArray.h"
#include "Engine/VolumeTexture.h"
#include "Misc/SecureHash.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		const TCHAR* TextureArrayMechanicId =
			TEXT("uai.asset_data.mechanic:replace_texture_array:v1");
		const TCHAR* TextureCubeArrayMechanicId =
			TEXT("uai.asset_data.mechanic:replace_texture_cube_array:v1");
		const TCHAR* TextureCubeMechanicId =
			TEXT("uai.asset_data.mechanic:replace_texture_cube:v1");
		const TCHAR* VolumeTextureMechanicId =
			TEXT("uai.asset_data.mechanic:replace_volume_texture:v1");
		constexpr int32 MaximumVolumeTextureDepth = 512;

		enum class ETextureSourceProjectionKind : uint8
		{
			Texture2DArray,
			TextureCubeArray,
			TextureCube,
			VolumeTexture,
		};

		struct FResolvedSourceSet final
		{
			TArray<FString> Paths;
			int32 SizeX = 0;
			int32 SizeY = 0;
			int32 NumSlices = 0;
			int32 NumMips = 0;
			ETextureSourceFormat Format = TSF_Invalid;
			FString TopMipSha1;
		};

		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
		}

		const FUeAgentCanonicalValue* FindObjectField(
			const FUeAgentCanonicalValue& Object,
			const FString& Field)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		bool TryBuildString(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryString(
					Text,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_texture_source_projection_string_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildBoolean(
			const bool bValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryBoolean(
					bValue,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_texture_source_projection_boolean_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildInteger(
			const int32 Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryInteger(
					FString::FromInt(Value),
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_texture_source_projection_integer_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildNull(
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryNull(
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_texture_source_projection_null_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildArray(
			TArray<FUeAgentCanonicalValueRef> Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_texture_source_projection_array_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildObject(
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			Items.Sort(
				[](const TPair<FString, FUeAgentCanonicalValueRef>& Left,
					const TPair<FString, FUeAgentCanonicalValueRef>& Right)
				{
					return Left.Key < Right.Key;
				});
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryObject(
					Items,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_texture_source_projection_object_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildStringArray(
			const TArray<FString>& Strings,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			Items.Reserve(Strings.Num());
			for (const FString& String : Strings)
			{
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildString(String, Resources, Item, OutError))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return TryBuildArray(
				MoveTemp(Items),
				Resources,
				OutValue,
				OutError);
		}

		bool TryReadString(
			const FUeAgentCanonicalValue& Value,
			FString& OutText)
		{
			if (Value.GetKind() != EUeAgentValueKind::String
				|| Value.GetText().IsEmpty())
			{
				OutText.Reset();
				return false;
			}
			OutText = Value.GetText();
			return true;
		}

		ETextureSourceProjectionKind GetProjectionKind(
			const FString& NativeMechanicId)
		{
			if (NativeMechanicId == TextureArrayMechanicId)
			{
				return ETextureSourceProjectionKind::Texture2DArray;
			}
			if (NativeMechanicId == TextureCubeArrayMechanicId)
			{
				return ETextureSourceProjectionKind::TextureCubeArray;
			}
			if (NativeMechanicId == TextureCubeMechanicId)
			{
				return ETextureSourceProjectionKind::TextureCube;
			}
			return ETextureSourceProjectionKind::VolumeTexture;
		}

		const TCHAR* GetSourceFieldId(
			const ETextureSourceProjectionKind Kind)
		{
			switch (Kind)
			{
			case ETextureSourceProjectionKind::Texture2DArray:
				return TEXT("source_texture_refs");
			case ETextureSourceProjectionKind::TextureCubeArray:
				return TEXT("source_cube_refs");
			case ETextureSourceProjectionKind::TextureCube:
				return TEXT("face_texture_refs");
			case ETextureSourceProjectionKind::VolumeTexture:
				return TEXT("slice_texture_refs");
			}
			return nullptr;
		}

		void UpdateSha1(
			FSHA1& Sha1,
			const uint8* Data,
			int64 Size)
		{
			int64 Offset = 0;
			while (Offset < Size)
			{
				const uint32 ChunkSize = static_cast<uint32>(
					FMath::Min<int64>(
						Size - Offset,
						static_cast<int64>(MAX_uint32)));
				Sha1.Update(Data + Offset, ChunkSize);
				Offset += ChunkSize;
			}
		}

		FString FinishSha1(FSHA1& Sha1)
		{
			Sha1.Final();
			uint8 Digest[FSHA1::DigestSize];
			Sha1.GetHash(Digest);
			return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
		}

		bool TryBuildSourceSignature(
			UTexture& Texture,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Valid;
			if (!TryBuildBoolean(
					Texture.Source.IsValid(),
					Resources,
					Valid,
					OutError))
			{
				return false;
			}
			if (!Texture.Source.IsValid())
			{
				return TryBuildObject(
					{{TEXT("valid"), Valid.ToSharedRef()}},
					Resources,
					OutValue,
					OutError);
			}
			TArray64<uint8> TopMip;
			if (!Texture.Source.GetMipData(TopMip, 0)
				|| TopMip.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_source_projection_data_invalid"),
					Texture.GetPathName());
			}
			FSHA1 Sha1;
			UpdateSha1(Sha1, TopMip.GetData(), TopMip.Num());
			const FString Hash = FinishSha1(Sha1);
			FUeAgentCanonicalValuePtr SizeX;
			FUeAgentCanonicalValuePtr SizeY;
			FUeAgentCanonicalValuePtr NumSlices;
			FUeAgentCanonicalValuePtr NumMips;
			FUeAgentCanonicalValuePtr Format;
			FUeAgentCanonicalValuePtr Sha;
			return TryBuildInteger(
					Texture.Source.GetSizeX(), Resources, SizeX, OutError)
				&& TryBuildInteger(
					Texture.Source.GetSizeY(), Resources, SizeY, OutError)
				&& TryBuildInteger(
					Texture.Source.GetNumSlices(), Resources, NumSlices, OutError)
				&& TryBuildInteger(
					Texture.Source.GetNumMips(), Resources, NumMips, OutError)
				&& TryBuildInteger(
					static_cast<int32>(Texture.Source.GetFormat()),
					Resources,
					Format,
					OutError)
				&& TryBuildString(Hash, Resources, Sha, OutError)
				&& TryBuildObject(
					{
						{TEXT("format"), Format.ToSharedRef()},
						{TEXT("num_mips"), NumMips.ToSharedRef()},
						{TEXT("num_slices"), NumSlices.ToSharedRef()},
						{TEXT("size_x"), SizeX.ToSharedRef()},
						{TEXT("size_y"), SizeY.ToSharedRef()},
						{TEXT("top_mip_sha1"), Sha.ToSharedRef()},
						{TEXT("valid"), Valid.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildResolvedSignature(
			const FResolvedSourceSet& SourceSet,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Valid;
			FUeAgentCanonicalValuePtr SizeX;
			FUeAgentCanonicalValuePtr SizeY;
			FUeAgentCanonicalValuePtr NumSlices;
			FUeAgentCanonicalValuePtr NumMips;
			FUeAgentCanonicalValuePtr Format;
			FUeAgentCanonicalValuePtr Sha;
			return TryBuildBoolean(true, Resources, Valid, OutError)
				&& TryBuildInteger(SourceSet.SizeX, Resources, SizeX, OutError)
				&& TryBuildInteger(SourceSet.SizeY, Resources, SizeY, OutError)
				&& TryBuildInteger(
					SourceSet.NumSlices,
					Resources,
					NumSlices,
					OutError)
				&& TryBuildInteger(
					SourceSet.NumMips,
					Resources,
					NumMips,
					OutError)
				&& TryBuildInteger(
					static_cast<int32>(SourceSet.Format),
					Resources,
					Format,
					OutError)
				&& TryBuildString(
					SourceSet.TopMipSha1,
					Resources,
					Sha,
					OutError)
				&& TryBuildObject(
					{
						{TEXT("format"), Format.ToSharedRef()},
						{TEXT("num_mips"), NumMips.ToSharedRef()},
						{TEXT("num_slices"), NumSlices.ToSharedRef()},
						{TEXT("size_x"), SizeX.ToSharedRef()},
						{TEXT("size_y"), SizeY.ToSharedRef()},
						{TEXT("top_mip_sha1"), Sha.ToSharedRef()},
						{TEXT("valid"), Valid.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildFormatSettings(
			const UTexture& Texture,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Srgb;
			FUeAgentCanonicalValuePtr Compression;
			FUeAgentCanonicalValuePtr MipGeneration;
			return TryBuildBoolean(
					Texture.SRGB, Resources, Srgb, OutError)
				&& TryBuildInteger(
					static_cast<int32>(Texture.CompressionSettings),
					Resources,
					Compression,
					OutError)
				&& TryBuildInteger(
					static_cast<int32>(Texture.MipGenSettings),
					Resources,
					MipGeneration,
					OutError)
				&& TryBuildObject(
					{
						{TEXT("compression"), Compression.ToSharedRef()},
						{TEXT("mip_generation"), MipGeneration.ToSharedRef()},
						{TEXT("srgb"), Srgb.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildExpectedFormatSettings(
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			if (Requested.GetKind() != EUeAgentValueKind::Object
				|| Requested.GetObjectItems().Num() != 3)
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_source_expected_format_invalid"));
			}
			const FUeAgentCanonicalValue* Srgb =
				FindObjectField(Requested, TEXT("srgb"));
			const FUeAgentCanonicalValue* Compression =
				FindObjectField(Requested, TEXT("compression"));
			const FUeAgentCanonicalValue* MipGeneration =
				FindObjectField(Requested, TEXT("mip_generation"));
			FString CompressionName;
			FString MipGenerationName;
			if (!Srgb
				|| Srgb->GetKind() != EUeAgentValueKind::Boolean
				|| !Compression
				|| !TryReadString(*Compression, CompressionName)
				|| !MipGeneration
				|| !TryReadString(*MipGeneration, MipGenerationName))
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_source_expected_format_invalid"));
			}
			CompressionName.TrimStartAndEndInline();
			CompressionName.ToLowerInline();
			MipGenerationName.TrimStartAndEndInline();
			MipGenerationName.ToLowerInline();
			TextureCompressionSettings CompressionValue = TC_Default;
			TextureMipGenSettings MipGenerationValue = TMGS_FromTextureGroup;
			if (CompressionName == TEXT("default"))
			{
				CompressionValue = TC_Default;
			}
			else if (CompressionName == TEXT("masks"))
			{
				CompressionValue = TC_Masks;
			}
			else if (CompressionName == TEXT("normal_map"))
			{
				CompressionValue = TC_Normalmap;
			}
			else if (CompressionName == TEXT("grayscale"))
			{
				CompressionValue = TC_Grayscale;
			}
			else if (CompressionName == TEXT("hdr"))
			{
				CompressionValue = TC_HDR;
			}
			else
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_source_expected_format_invalid"),
					CompressionName);
			}
			if (MipGenerationName == TEXT("from_texture_group"))
			{
				MipGenerationValue = TMGS_FromTextureGroup;
			}
			else if (MipGenerationName == TEXT("simple_average"))
			{
				MipGenerationValue = TMGS_SimpleAverage;
			}
			else if (MipGenerationName == TEXT("no_mipmaps"))
			{
				MipGenerationValue = TMGS_NoMipmaps;
			}
			else
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_source_expected_format_invalid"),
					MipGenerationName);
			}
			if (Srgb->GetBoolean()
				&& (CompressionValue == TC_Normalmap
					|| CompressionValue == TC_HDR))
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_source_expected_format_incompatible"));
			}
			FUeAgentCanonicalValuePtr CanonicalSrgb;
			FUeAgentCanonicalValuePtr CanonicalCompression;
			FUeAgentCanonicalValuePtr CanonicalMipGeneration;
			return TryBuildBoolean(
					Srgb->GetBoolean(), Resources, CanonicalSrgb, OutError)
				&& TryBuildInteger(
					static_cast<int32>(CompressionValue),
					Resources,
					CanonicalCompression,
					OutError)
				&& TryBuildInteger(
					static_cast<int32>(MipGenerationValue),
					Resources,
					CanonicalMipGeneration,
					OutError)
				&& TryBuildObject(
					{
						{TEXT("compression"), CanonicalCompression.ToSharedRef()},
						{TEXT("mip_generation"), CanonicalMipGeneration.ToSharedRef()},
						{TEXT("srgb"), CanonicalSrgb.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryResolveSourceSet(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const TArray<const FUeAgentCanonicalValue*>& RefValues,
			const ETextureSourceProjectionKind Kind,
			FResolvedSourceSet& OutSet,
			FExecutionMutationError& OutError)
		{
			OutSet = FResolvedSourceSet();
			if (RefValues.IsEmpty()
				|| (Kind == ETextureSourceProjectionKind::TextureCube
					&& RefValues.Num() != 6)
				|| (Kind == ETextureSourceProjectionKind::VolumeTexture
					&& RefValues.Num() > MaximumVolumeTextureDepth))
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_source_expected_refs_invalid"));
			}
			FSHA1 Sha1;
			int64 SourceDataSize = -1;
			int32 SourceSlices = -1;
			for (const FUeAgentCanonicalValue* RefValue : RefValues)
			{
				FString CanonicalRef;
				UObject* Resolved = nullptr;
				if (!RefValue
					|| !TryReadString(*RefValue, CanonicalRef)
					|| !Resolver.TryResolveExisting(
						CanonicalRef,
						Resolved,
						OutError))
				{
					return Fail(
						OutError,
						TEXT("asset_data_texture_source_expected_ref_invalid"),
						CanonicalRef);
				}
				UTexture* Source = nullptr;
				if (Kind == ETextureSourceProjectionKind::TextureCubeArray)
				{
					UTextureCube* Cube = Cast<UTextureCube>(Resolved);
					if (Cube && Cube->GetClass() == UTextureCube::StaticClass())
					{
						Source = Cube;
					}
				}
				else
				{
					UTexture2D* Texture2D = Cast<UTexture2D>(Resolved);
					if (Texture2D
						&& Texture2D->GetClass() == UTexture2D::StaticClass())
					{
						Source = Texture2D;
					}
				}
				if (!Source
					|| !Source->Source.IsValid()
					|| (Kind != ETextureSourceProjectionKind::TextureCubeArray
						&& Source->Source.GetNumSlices() != 1)
					|| (Kind == ETextureSourceProjectionKind::TextureCubeArray
						&& Source->Source.GetNumSlices() != 1
						&& Source->Source.GetNumSlices() != 6))
				{
					return Fail(
						OutError,
						TEXT("asset_data_texture_source_expected_source_invalid"),
						CanonicalRef);
				}
				TArray64<uint8> SourceData;
				if (!Source->Source.GetMipData(SourceData, 0)
					|| SourceData.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("asset_data_texture_source_expected_data_invalid"),
						CanonicalRef);
				}
				if (OutSet.Paths.IsEmpty())
				{
					OutSet.SizeX = Source->Source.GetSizeX();
					OutSet.SizeY = Source->Source.GetSizeY();
					OutSet.NumMips = Source->Source.GetNumMips();
					OutSet.Format = Source->Source.GetFormat();
					SourceSlices = Source->Source.GetNumSlices();
					SourceDataSize = SourceData.Num();
				}
				else if (Source->Source.GetSizeX() != OutSet.SizeX
					|| Source->Source.GetSizeY() != OutSet.SizeY
					|| Source->Source.GetNumMips() != OutSet.NumMips
					|| Source->Source.GetNumSlices() != SourceSlices
					|| Source->Source.GetFormat() != OutSet.Format
					|| SourceData.Num() != SourceDataSize)
				{
					return Fail(
						OutError,
						TEXT("asset_data_texture_source_expected_sources_incompatible"),
						CanonicalRef);
				}
				OutSet.Paths.Add(Source->GetPathName());
				UpdateSha1(Sha1, SourceData.GetData(), SourceData.Num());
			}
			OutSet.NumSlices =
				Kind == ETextureSourceProjectionKind::TextureCubeArray
					? SourceSlices * OutSet.Paths.Num()
					: OutSet.Paths.Num();
			if (Kind == ETextureSourceProjectionKind::TextureCube
				|| Kind == ETextureSourceProjectionKind::VolumeTexture)
			{
				OutSet.NumMips = 1;
			}
			OutSet.TopMipSha1 = FinishSha1(Sha1);
			return OutSet.SizeX > 0
				&& OutSet.SizeY > 0
				&& OutSet.NumSlices > 0
				&& OutSet.Format != TSF_Invalid;
		}

		bool TryBuildArraySourceState(
			UTexture& Target,
			const TArray<FString>& Paths,
			const bool bGeneratedFromSources,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Refs;
			FUeAgentCanonicalValuePtr Generated;
			FUeAgentCanonicalValuePtr Signature;
			return TryBuildStringArray(Paths, Resources, Refs, OutError)
				&& TryBuildBoolean(
					bGeneratedFromSources,
					Resources,
					Generated,
					OutError)
				&& TryBuildSourceSignature(
					Target,
					Resources,
					Signature,
					OutError)
				&& TryBuildObject(
					{
						{TEXT("generated_from_sources"), Generated.ToSharedRef()},
						{TEXT("resolved_paths"), Refs.ToSharedRef()},
						{TEXT("source_signature"), Signature.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildExpectedArraySourceState(
			const FResolvedSourceSet& SourceSet,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Refs;
			FUeAgentCanonicalValuePtr Generated;
			FUeAgentCanonicalValuePtr Signature;
			return TryBuildStringArray(
					SourceSet.Paths,
					Resources,
					Refs,
					OutError)
				&& TryBuildBoolean(true, Resources, Generated, OutError)
				&& TryBuildResolvedSignature(
					SourceSet,
					Resources,
					Signature,
					OutError)
				&& TryBuildObject(
					{
						{TEXT("generated_from_sources"), Generated.ToSharedRef()},
						{TEXT("resolved_paths"), Refs.ToSharedRef()},
						{TEXT("source_signature"), Signature.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildAssembledSourceState(
			UTexture& Target,
			const ETextureSourceProjectionKind Kind,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Signature;
			if (!TryBuildSourceSignature(
					Target,
					Resources,
					Signature,
					OutError))
			{
				return false;
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items{
				{TEXT("source_signature"), Signature.ToSharedRef()},
			};
			if (Kind == ETextureSourceProjectionKind::VolumeTexture)
			{
				const UVolumeTexture& Volume =
					*CastChecked<UVolumeTexture>(&Target);
				FUeAgentCanonicalValuePtr Source2D;
				FUeAgentCanonicalValuePtr TileSizeX;
				FUeAgentCanonicalValuePtr TileSizeY;
				if ((Volume.Source2DTexture
						&& !TryBuildString(
							Volume.Source2DTexture->GetPathName(),
							Resources,
							Source2D,
							OutError))
					|| (!Volume.Source2DTexture
						&& !TryBuildNull(Resources, Source2D, OutError))
					|| !TryBuildInteger(
						Volume.Source2DTileSizeX,
						Resources,
						TileSizeX,
						OutError)
					|| !TryBuildInteger(
						Volume.Source2DTileSizeY,
						Resources,
						TileSizeY,
						OutError))
				{
					return false;
				}
				Items.Emplace(TEXT("source_2d_path"), Source2D.ToSharedRef());
				Items.Emplace(TEXT("source_2d_tile_size_x"), TileSizeX.ToSharedRef());
				Items.Emplace(TEXT("source_2d_tile_size_y"), TileSizeY.ToSharedRef());
			}
			return TryBuildObject(
				MoveTemp(Items),
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildExpectedAssembledSourceState(
			const FResolvedSourceSet& SourceSet,
			const ETextureSourceProjectionKind Kind,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Signature;
			if (!TryBuildResolvedSignature(
					SourceSet,
					Resources,
					Signature,
					OutError))
			{
				return false;
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items{
				{TEXT("source_signature"), Signature.ToSharedRef()},
			};
			if (Kind == ETextureSourceProjectionKind::VolumeTexture)
			{
				FUeAgentCanonicalValuePtr Source2D;
				FUeAgentCanonicalValuePtr TileSizeX;
				FUeAgentCanonicalValuePtr TileSizeY;
				if (!TryBuildNull(Resources, Source2D, OutError)
					|| !TryBuildInteger(0, Resources, TileSizeX, OutError)
					|| !TryBuildInteger(0, Resources, TileSizeY, OutError))
				{
					return false;
				}
				Items.Emplace(TEXT("source_2d_path"), Source2D.ToSharedRef());
				Items.Emplace(TEXT("source_2d_tile_size_x"), TileSizeX.ToSharedRef());
				Items.Emplace(TEXT("source_2d_tile_size_y"), TileSizeY.ToSharedRef());
			}
			return TryBuildObject(
				MoveTemp(Items),
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildCurrentProjectionMap(
			const ETextureSourceProjectionKind Kind,
			UObject* TargetObject,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			UTexture* Target = nullptr;
			FUeAgentCanonicalValuePtr SourceState;
			if (Kind == ETextureSourceProjectionKind::Texture2DArray)
			{
				UTexture2DArray* Texture = Cast<UTexture2DArray>(TargetObject);
				if (!Texture
					|| Texture->GetClass() != UTexture2DArray::StaticClass())
				{
					return Fail(OutError, TEXT("asset_data_texture_source_projection_target_invalid"));
				}
				TArray<FString> Paths;
				for (UTexture2D* Source : Texture->SourceTextures)
				{
					if (!Source || Source->GetClass() != UTexture2D::StaticClass())
					{
						return Fail(OutError, TEXT("asset_data_texture_source_projection_current_ref_invalid"));
					}
					Paths.Add(Source->GetPathName());
				}
				Target = Texture;
				if (!TryBuildArraySourceState(
						*Texture,
						Paths,
						Texture->bSourceGeneratedFromSourceTexturesArray,
						Resources,
						SourceState,
						OutError))
				{
					return false;
				}
			}
			else if (Kind == ETextureSourceProjectionKind::TextureCubeArray)
			{
				UTextureCubeArray* Texture = Cast<UTextureCubeArray>(TargetObject);
				if (!Texture
					|| Texture->GetClass() != UTextureCubeArray::StaticClass())
				{
					return Fail(OutError, TEXT("asset_data_texture_source_projection_target_invalid"));
				}
				TArray<FString> Paths;
				for (UTextureCube* Source : Texture->SourceTextures)
				{
					if (!Source || Source->GetClass() != UTextureCube::StaticClass())
					{
						return Fail(OutError, TEXT("asset_data_texture_source_projection_current_ref_invalid"));
					}
					Paths.Add(Source->GetPathName());
				}
				Target = Texture;
				if (!TryBuildArraySourceState(
						*Texture,
						Paths,
						Texture->bSourceGeneratedFromSourceTexturesArray,
						Resources,
						SourceState,
						OutError))
				{
					return false;
				}
			}
			else
			{
				if (Kind == ETextureSourceProjectionKind::TextureCube)
				{
					Target = Cast<UTextureCube>(TargetObject);
				}
				else
				{
					Target = Cast<UVolumeTexture>(TargetObject);
				}
				const UClass* ExpectedClass =
					Kind == ETextureSourceProjectionKind::TextureCube
						? UTextureCube::StaticClass()
						: UVolumeTexture::StaticClass();
				if (!Target || Target->GetClass() != ExpectedClass
					|| !TryBuildAssembledSourceState(
						*Target,
						Kind,
						Resources,
						SourceState,
						OutError))
				{
					return Target
						? false
						: Fail(OutError, TEXT("asset_data_texture_source_projection_target_invalid"));
				}
			}
			FUeAgentCanonicalValuePtr FormatSettings;
			if (!TryBuildFormatSettings(
					*Target,
					Resources,
					FormatSettings,
					OutError))
			{
				return false;
			}
			OutValues.Add(GetSourceFieldId(Kind), SourceState.ToSharedRef());
			OutValues.Add(TEXT("format_settings"), FormatSettings.ToSharedRef());
			return true;
		}

		const TCHAR* FindContextTag(
			const ETextureSourceProjectionKind Kind,
			const FString& FieldId)
		{
			if (FieldId == TEXT("format_settings"))
			{
				return TEXT("texture_format_settings");
			}
			if (FieldId != GetSourceFieldId(Kind))
			{
				return nullptr;
			}
			return Kind == ETextureSourceProjectionKind::Texture2DArray
				|| Kind == ETextureSourceProjectionKind::TextureCubeArray
				? TEXT("ordered_texture_source_state")
				: TEXT("assembled_texture_source_state");
		}

		bool TrySelectFields(
			const ETextureSourceProjectionKind Kind,
			const TArray<FString>& SelectedFieldIds,
			const TMap<FString, FUeAgentCanonicalValueRef>& Values,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutCurrentFields,
			FUeAgentCanonicalValuePtr& OutDefaultFields,
			FUeAgentCanonicalValuePtr& OutProjectionContext,
			FExecutionMutationError& OutError)
		{
			if (SelectedFieldIds.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_source_selected_fields_missing"));
			}
			TSet<FString> Seen;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> CurrentItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> DefaultItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> ContextItems;
			for (const FString& FieldId : SelectedFieldIds)
			{
				const FUeAgentCanonicalValueRef* Value = Values.Find(FieldId);
				const TCHAR* ContextTag = FindContextTag(Kind, FieldId);
				FUeAgentCanonicalValuePtr Context;
				if (Seen.Contains(FieldId)
					|| !Value
					|| !ContextTag
					|| !TryBuildString(
						ContextTag,
						Resources,
						Context,
						OutError))
				{
					return Fail(
						OutError,
						TEXT("asset_data_texture_source_selected_field_invalid"),
						FieldId);
				}
				Seen.Add(FieldId);
				CurrentItems.Emplace(FieldId, *Value);
				DefaultItems.Emplace(FieldId, *Value);
				ContextItems.Emplace(FieldId, Context.ToSharedRef());
			}
			return TryBuildObject(
					MoveTemp(CurrentItems),
					Resources,
					OutCurrentFields,
					OutError)
				&& TryBuildObject(
					MoveTemp(DefaultItems),
					Resources,
					OutDefaultFields,
					OutError)
				&& TryBuildObject(
					MoveTemp(ContextItems),
					Resources,
					OutProjectionContext,
					OutError);
		}

		bool TryReadTypedChangeMap(
			const FUeAgentCanonicalValue& CurrentFields,
			const FUeAgentCanonicalValue& DefaultFields,
			const FUeAgentCanonicalValue& ProjectionContext,
			const FUeAgentCanonicalValue& ProposedAfter,
			TMap<FString, const FUeAgentCanonicalValue*>& OutChanges,
			FExecutionMutationError& OutError)
		{
			OutChanges.Reset();
			const FUeAgentCanonicalValue* TypedChanges =
				FindObjectField(ProposedAfter, TEXT("typed_changes"));
			if (CurrentFields.GetKind() != EUeAgentValueKind::Object
				|| DefaultFields.GetKind() != EUeAgentValueKind::Object
				|| ProjectionContext.GetKind() != EUeAgentValueKind::Object
				|| CurrentFields.GetObjectItems().Num()
					!= DefaultFields.GetObjectItems().Num()
				|| CurrentFields.GetObjectItems().Num()
					!= ProjectionContext.GetObjectItems().Num()
				|| !TypedChanges
				|| TypedChanges->GetKind() != EUeAgentValueKind::Array
				|| TypedChanges->GetArrayItems().Num()
					!= CurrentFields.GetObjectItems().Num())
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_source_expected_state_invalid"));
			}
			for (const FUeAgentCanonicalValueRef& Change
				: TypedChanges->GetArrayItems())
			{
				const FUeAgentCanonicalValue* Field =
					FindObjectField(Change.Get(), TEXT("field_id"));
				FString FieldId;
				if (!Field
					|| !TryReadString(*Field, FieldId)
					|| OutChanges.Contains(FieldId)
					|| !FindObjectField(CurrentFields, FieldId))
				{
					return Fail(
						OutError,
						TEXT("asset_data_texture_source_expected_change_invalid"));
				}
				OutChanges.Add(FieldId, &Change.Get());
			}
			return true;
		}

		bool TryReadReplaceValue(
			const FUeAgentCanonicalValue& Change,
			const FUeAgentCanonicalValue*& OutRequested)
		{
			OutRequested = nullptr;
			const FUeAgentCanonicalValue* Presence =
				FindObjectField(Change, TEXT("presence"));
			FString PresenceText;
			if (!Presence
				|| !TryReadString(*Presence, PresenceText)
				|| PresenceText != TEXT("replace"))
			{
				return false;
			}
			const FUeAgentCanonicalValue* TypedValue =
				FindObjectField(Change, TEXT("value"));
			OutRequested = TypedValue
				? FindObjectField(*TypedValue, TEXT("canonical_value"))
				: nullptr;
			return OutRequested != nullptr;
		}

		bool TryCollectRequestedRefs(
			const FUeAgentCanonicalValue& Requested,
			const ETextureSourceProjectionKind Kind,
			TArray<const FUeAgentCanonicalValue*>& OutRefs,
			FExecutionMutationError& OutError)
		{
			OutRefs.Reset();
			if (Kind == ETextureSourceProjectionKind::TextureCube)
			{
				if (Requested.GetKind() != EUeAgentValueKind::Object
					|| Requested.GetObjectItems().Num() != 6)
				{
					return Fail(
						OutError,
						TEXT("asset_data_texture_source_expected_refs_invalid"));
				}
				const TCHAR* FaceFields[] = {
					TEXT("positive_x"),
					TEXT("negative_x"),
					TEXT("positive_y"),
					TEXT("negative_y"),
					TEXT("positive_z"),
					TEXT("negative_z"),
				};
				for (const TCHAR* FaceField : FaceFields)
				{
					const FUeAgentCanonicalValue* Ref =
						FindObjectField(Requested, FaceField);
					if (!Ref)
					{
						return Fail(
							OutError,
							TEXT("asset_data_texture_source_expected_refs_invalid"),
							FaceField);
					}
					OutRefs.Add(Ref);
				}
				return true;
			}
			if (Requested.GetKind() != EUeAgentValueKind::Array
				|| Requested.GetArrayItems().IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_source_expected_refs_invalid"));
			}
			for (const FUeAgentCanonicalValueRef& Ref
				: Requested.GetArrayItems())
			{
				OutRefs.Add(&Ref.Get());
			}
			return true;
		}
	}

	bool CanProjectTextureSourceState(
		const FString& NativeMechanicId)
	{
		return NativeMechanicId == TextureArrayMechanicId
			|| NativeMechanicId == TextureCubeArrayMechanicId
			|| NativeMechanicId == TextureCubeMechanicId
			|| NativeMechanicId == VolumeTextureMechanicId;
	}

	bool TryProjectTextureSourceCurrentState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FString>& SelectedFieldIds,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutCurrentFields,
		FUeAgentCanonicalValuePtr& OutDefaultFields,
		FUeAgentCanonicalValuePtr& OutProjectionContext,
		FExecutionMutationError& OutError)
	{
		(void)Resolver;
		OutCurrentFields.Reset();
		OutDefaultFields.Reset();
		OutProjectionContext.Reset();
		if (!CanProjectTextureSourceState(NativeMechanicId) || !Target)
		{
			return Fail(
				OutError,
				TEXT("asset_data_texture_source_projection_unsupported"),
				NativeMechanicId);
		}
		const ETextureSourceProjectionKind Kind =
			GetProjectionKind(NativeMechanicId);
		TMap<FString, FUeAgentCanonicalValueRef> Values;
		return TryBuildCurrentProjectionMap(
				Kind,
				Target,
				Resources,
				Values,
				OutError)
			&& TrySelectFields(
				Kind,
				SelectedFieldIds,
				Values,
				Resources,
				OutCurrentFields,
				OutDefaultFields,
				OutProjectionContext,
				OutError);
	}

	bool TryProjectTextureSourceExpectedState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		const FUeAgentCanonicalValue& CurrentFields,
		const FUeAgentCanonicalValue& DefaultFields,
		const FUeAgentCanonicalValue& ProjectionContext,
		const FUeAgentCanonicalValue& ProposedAfter,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutExpectedFields,
		FExecutionMutationError& OutError)
	{
		OutExpectedFields.Reset();
		if (!CanProjectTextureSourceState(NativeMechanicId))
		{
			return Fail(
				OutError,
				TEXT("asset_data_texture_source_projection_unsupported"),
				NativeMechanicId);
		}
		const ETextureSourceProjectionKind Kind =
			GetProjectionKind(NativeMechanicId);
		TMap<FString, const FUeAgentCanonicalValue*> Changes;
		if (!TryReadTypedChangeMap(
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Changes,
				OutError))
		{
			return false;
		}

		TArray<TPair<FString, FUeAgentCanonicalValueRef>> ExpectedItems;
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Current
			: CurrentFields.GetObjectItems())
		{
			const FUeAgentCanonicalValue* const* Change =
				Changes.Find(Current.Key);
			const FUeAgentCanonicalValue* Context =
				FindObjectField(ProjectionContext, Current.Key);
			const TCHAR* ExpectedContext =
				FindContextTag(Kind, Current.Key);
			FString ContextText;
			const FUeAgentCanonicalValue* Requested = nullptr;
			if (!Change
				|| !Context
				|| !ExpectedContext
				|| !TryReadString(*Context, ContextText)
				|| ContextText != ExpectedContext
				|| !TryReadReplaceValue(**Change, Requested))
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_source_expected_change_invalid"),
					Current.Key);
			}

			FUeAgentCanonicalValuePtr Expected;
			if (Current.Key == TEXT("format_settings"))
			{
				if (!TryBuildExpectedFormatSettings(
						*Requested,
						Resources,
						Expected,
						OutError))
				{
					return false;
				}
			}
			else if (Current.Key == GetSourceFieldId(Kind))
			{
				TArray<const FUeAgentCanonicalValue*> RefValues;
				FResolvedSourceSet SourceSet;
				if (!TryCollectRequestedRefs(
						*Requested,
						Kind,
						RefValues,
						OutError)
					|| !TryResolveSourceSet(
						Resolver,
						RefValues,
						Kind,
						SourceSet,
						OutError))
				{
					return false;
				}
				if (Kind == ETextureSourceProjectionKind::Texture2DArray
					|| Kind == ETextureSourceProjectionKind::TextureCubeArray)
				{
					if (!TryBuildExpectedArraySourceState(
							SourceSet,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (!TryBuildExpectedAssembledSourceState(
						SourceSet,
						Kind,
						Resources,
						Expected,
						OutError))
				{
					return false;
				}
			}
			else
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_source_expected_field_invalid"),
					Current.Key);
			}
			ExpectedItems.Emplace(Current.Key, Expected.ToSharedRef());
		}
		return TryBuildObject(
			MoveTemp(ExpectedItems),
			Resources,
			OutExpectedFields,
			OutError);
	}
}
