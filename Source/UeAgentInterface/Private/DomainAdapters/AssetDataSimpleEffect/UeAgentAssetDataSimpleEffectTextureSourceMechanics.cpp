#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTextureSourceMechanics.h"

#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureCubeArray.h"
#include "Engine/VolumeTexture.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		constexpr int32 MaximumVolumeTextureDepth = 512;

		enum class ETextureSourceKind : uint8
		{
			Texture2DArray,
			TextureCubeArray,
			TextureCube,
			VolumeTexture,
		};

		struct FTextureFormatSettings final
		{
			bool bSRGB = true;
			TextureCompressionSettings Compression = TC_Default;
			TextureMipGenSettings MipGeneration =
				TMGS_FromTextureGroup;
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

		bool TryParseCompression(
			const FUeAgentCanonicalValue& Value,
			TextureCompressionSettings& OutCompression)
		{
			if (Value.GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			const FString Name =
				Value.GetText().TrimStartAndEnd().ToLower();
			if (Name == TEXT("default"))
			{
				OutCompression = TC_Default;
				return true;
			}
			if (Name == TEXT("masks"))
			{
				OutCompression = TC_Masks;
				return true;
			}
			if (Name == TEXT("normal_map"))
			{
				OutCompression = TC_Normalmap;
				return true;
			}
			if (Name == TEXT("grayscale"))
			{
				OutCompression = TC_Grayscale;
				return true;
			}
			if (Name == TEXT("hdr"))
			{
				OutCompression = TC_HDR;
				return true;
			}
			return false;
		}

		bool TryParseMipGeneration(
			const FUeAgentCanonicalValue& Value,
			TextureMipGenSettings& OutMipGeneration)
		{
			if (Value.GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			const FString Name =
				Value.GetText().TrimStartAndEnd().ToLower();
			if (Name == TEXT("from_texture_group"))
			{
				OutMipGeneration = TMGS_FromTextureGroup;
				return true;
			}
			if (Name == TEXT("simple_average"))
			{
				OutMipGeneration = TMGS_SimpleAverage;
				return true;
			}
			if (Name == TEXT("no_mipmaps"))
			{
				OutMipGeneration = TMGS_NoMipmaps;
				return true;
			}
			return false;
		}

		bool TryParseFormatSettings(
			const FTextureSourceChangeView& Change,
			FTextureFormatSettings& OutSettings,
			FExecutionMutationError& OutError)
		{
			if (!Change.bSpecified
				|| Change.Presence != TEXT("replace")
				|| !Change.CanonicalValue
				|| Change.CanonicalValue->GetKind()
					!= EUeAgentValueKind::Object
				|| Change.CanonicalValue->GetObjectItems().Num() != 3)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_texture_source_"
						"format_settings_invalid"));
			}
			const FUeAgentCanonicalValue* SRGB =
				FindObjectField(
					*Change.CanonicalValue,
					TEXT("srgb"));
			const FUeAgentCanonicalValue* Compression =
				FindObjectField(
					*Change.CanonicalValue,
					TEXT("compression"));
			const FUeAgentCanonicalValue* MipGeneration =
				FindObjectField(
					*Change.CanonicalValue,
					TEXT("mip_generation"));
			if (!SRGB
				|| SRGB->GetKind() != EUeAgentValueKind::Boolean
				|| !Compression
				|| !MipGeneration
				|| !TryParseCompression(
					*Compression,
					OutSettings.Compression)
				|| !TryParseMipGeneration(
					*MipGeneration,
					OutSettings.MipGeneration))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_texture_source_"
						"format_settings_invalid"));
			}
			OutSettings.bSRGB = SRGB->GetBoolean();
			if (OutSettings.bSRGB
				&& (OutSettings.Compression == TC_Normalmap
					|| OutSettings.Compression == TC_HDR))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_texture_source_"
						"format_settings_incompatible"));
			}
			return true;
		}

		void Pin(
			TArray<TStrongObjectPtr<UObject>>& Pins,
			UObject* Object)
		{
			if (Object)
			{
				Pins.Emplace(Object);
			}
		}
	}

	struct FTextureSourcePreparedData final
	{
		FTextureSourcePreparedData(
			const ETextureSourceKind InKind,
			UTexture& Target)
			: Kind(InKind)
			, OriginalSource(Target.Source.CopyTornOff())
			, bOriginalSRGB(Target.SRGB)
			, OriginalCompression(Target.CompressionSettings)
			, OriginalMipGeneration(Target.MipGenSettings)
			, OriginalLightingGuid(Target.GetLightingGuid())
			, bOriginalPackageDirty(
				Target.GetOutermost()
					&& Target.GetOutermost()->IsDirty())
		{
		}

		ETextureSourceKind Kind;
		TArray<UObject*> OriginalSourceObjects;
		TArray<UObject*> NewSourceObjects;
		TArray<TStrongObjectPtr<UObject>> Pins;
		FTextureSource OriginalSource;
		UTexture2D* OriginalVolumeSource2DTexture = nullptr;
		int32 OriginalVolumeTileSizeX = 0;
		int32 OriginalVolumeTileSizeY = 0;
		bool bOriginalGeneratedFromSourceArray = false;
		bool bOriginalSRGB = true;
		TextureCompressionSettings OriginalCompression = TC_Default;
		TextureMipGenSettings OriginalMipGeneration =
			TMGS_FromTextureGroup;
		FGuid OriginalLightingGuid;
		bool bOriginalPackageDirty = false;
		FTextureFormatSettings NewSettings;
		int32 NewSourceSizeX = 0;
		int32 NewSourceSizeY = 0;
		int32 NewSourceSlices = 0;
		ETextureSourceFormat NewSourceFormat = TSF_Invalid;
		TArray64<uint8> NewSourceTopMipData;
	};

	namespace
	{
		template <typename TTarget, typename TSource>
		bool TryPrepareReplacement(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			UObject* TargetObject,
			const ETextureSourceKind Kind,
			const FTextureSourceChangeView& SourceRefs,
			const FTextureSourceChangeView& FormatSettings,
			FTextureSourcePreparedState& OutState,
			FExecutionMutationError& OutError)
		{
			OutState.Data.Reset();
			TTarget* Target = Cast<TTarget>(TargetObject);
			if (!Target
				|| Target->GetClass() != TTarget::StaticClass())
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_array_target_invalid"));
			}
			if (!SourceRefs.bSpecified
				|| SourceRefs.Presence != TEXT("replace")
				|| !SourceRefs.CanonicalValue
				|| SourceRefs.CanonicalValue->GetKind()
					!= EUeAgentValueKind::Array
				|| SourceRefs.CanonicalValue->GetArrayItems().IsEmpty())
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_texture_array_"
						"source_refs_invalid"));
			}

			TSharedPtr<
				FTextureSourcePreparedData,
				ESPMode::ThreadSafe> Data =
					MakeShared<
						FTextureSourcePreparedData,
						ESPMode::ThreadSafe>(
							Kind,
							*Target);
			if (!TryParseFormatSettings(
					FormatSettings,
					Data->NewSettings,
					OutError))
			{
				return false;
			}

			Data->OriginalSourceObjects.Reserve(
				Target->SourceTextures.Num());
			for (TSource* Source : Target->SourceTextures)
			{
				Data->OriginalSourceObjects.Add(Source);
				Pin(Data->Pins, Source);
			}

			Data->NewSourceObjects.Reserve(
				SourceRefs.CanonicalValue->GetArrayItems().Num());
			for (const FUeAgentCanonicalValueRef& RefValue
				: SourceRefs.CanonicalValue->GetArrayItems())
			{
				if (RefValue->GetKind()
						!= EUeAgentValueKind::String
					|| RefValue->GetText().IsEmpty())
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_texture_array_"
							"source_ref_invalid"));
				}
				UObject* Resolved = nullptr;
				if (!Resolver.TryResolveExisting(
						RefValue->GetText(),
						Resolved,
						OutError))
				{
					return false;
				}
				TSource* Source = Cast<TSource>(Resolved);
				if (!Source
					|| Source->GetClass() != TSource::StaticClass()
					|| !Source->Source.IsValid())
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_texture_array_"
							"source_invalid"),
						RefValue->GetText());
				}
				Data->NewSourceObjects.Add(Source);
				Pin(Data->Pins, Source);
			}

			Data->bOriginalGeneratedFromSourceArray =
				Target->bSourceGeneratedFromSourceTexturesArray;
			OutState.Data = MoveTemp(Data);
			return true;
		}

		bool ValidateCubeArraySources(
			const FTextureSourcePreparedData& Data,
			FExecutionMutationError& OutError)
		{
			const UTextureCube* First =
				Data.NewSourceObjects.IsEmpty()
					? nullptr
					: Cast<UTextureCube>(
						Data.NewSourceObjects[0]);
			if (!First
				|| !First->Source.IsValid()
				|| (First->Source.GetNumSlices() != 1
					&& First->Source.GetNumSlices() != 6))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_texture_cube_array_"
						"source_invalid"));
			}
			const int32 SizeX = First->Source.GetSizeX();
			const int32 SizeY = First->Source.GetSizeY();
			const int32 NumSlices =
				First->Source.GetNumSlices();
			const ETextureSourceFormat Format =
				First->Source.GetFormat();
			for (UObject* Object : Data.NewSourceObjects)
			{
				const UTextureCube* Source =
					Cast<UTextureCube>(Object);
				if (!Source
					|| !Source->Source.IsValid()
					|| Source->Source.GetSizeX() != SizeX
					|| Source->Source.GetSizeY() != SizeY
					|| Source->Source.GetNumSlices() != NumSlices
					|| Source->Source.GetFormat() != Format)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_texture_cube_array_"
							"sources_incompatible"));
				}
			}
			return true;
		}

		bool TryResolveTexture2DSources(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const TArray<const FUeAgentCanonicalValue*>& RefValues,
			FTextureSourcePreparedData& Data,
			FExecutionMutationError& OutError)
		{
			if (RefValues.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_source_refs_invalid"));
			}
			Data.NewSourceObjects.Reserve(RefValues.Num());
			for (const FUeAgentCanonicalValue* RefValue : RefValues)
			{
				if (!RefValue
					|| RefValue->GetKind()
						!= EUeAgentValueKind::String
					|| RefValue->GetText().IsEmpty())
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_texture_source_"
							"ref_invalid"));
				}
				UObject* Resolved = nullptr;
				if (!Resolver.TryResolveExisting(
						RefValue->GetText(),
						Resolved,
						OutError))
				{
					return false;
				}
				UTexture2D* Source = Cast<UTexture2D>(Resolved);
				if (!Source
					|| Source->GetClass()
						!= UTexture2D::StaticClass()
					|| !Source->Source.IsValid()
					|| Source->Source.GetNumSlices() != 1)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_texture_source_"
							"source_invalid"),
						RefValue->GetText());
				}
				Data.NewSourceObjects.Add(Source);
				Pin(Data.Pins, Source);
			}
			return true;
		}

		bool TryFreezeTexture2DSourceData(
			FTextureSourcePreparedData& Data,
			FExecutionMutationError& OutError)
		{
			UTexture2D* First =
				Data.NewSourceObjects.IsEmpty()
					? nullptr
					: Cast<UTexture2D>(
						Data.NewSourceObjects[0]);
			if (!First || !First->Source.IsValid())
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_texture_source_"
						"source_invalid"));
			}
			Data.NewSourceSizeX = First->Source.GetSizeX();
			Data.NewSourceSizeY = First->Source.GetSizeY();
			Data.NewSourceFormat = First->Source.GetFormat();
			if (Data.NewSourceSizeX <= 0
				|| Data.NewSourceSizeY <= 0
				|| Data.NewSourceFormat == TSF_Invalid)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_texture_source_"
						"source_invalid"));
			}
			int64 ExpectedSourceBytes = -1;
			for (UObject* Object : Data.NewSourceObjects)
			{
				UTexture2D* Source = Cast<UTexture2D>(Object);
				if (!Source
					|| !Source->Source.IsValid()
					|| Source->Source.GetNumSlices() != 1
					|| Source->Source.GetSizeX()
						!= Data.NewSourceSizeX
					|| Source->Source.GetSizeY()
						!= Data.NewSourceSizeY
					|| Source->Source.GetFormat()
						!= Data.NewSourceFormat)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_texture_source_"
							"sources_incompatible"));
				}
				TArray64<uint8> SourceData;
				if (!Source->Source.GetMipData(SourceData, 0)
					|| SourceData.IsEmpty()
					|| (ExpectedSourceBytes >= 0
						&& SourceData.Num()
							!= ExpectedSourceBytes))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_texture_source_"
							"source_data_invalid"));
				}
				if (ExpectedSourceBytes < 0)
				{
					ExpectedSourceBytes = SourceData.Num();
				}
				Data.NewSourceTopMipData.Append(
					SourceData.GetData(),
					SourceData.Num());
			}
			Data.NewSourceSlices = Data.NewSourceObjects.Num();
			return true;
		}

		bool TryPrepareAssembledReplacement(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			UObject* TargetObject,
			const ETextureSourceKind Kind,
			const FTextureSourceChangeView& SourceRefs,
			const FTextureSourceChangeView& FormatSettings,
			FTextureSourcePreparedState& OutState,
			FExecutionMutationError& OutError)
		{
			OutState.Data.Reset();
			UTexture* Target = nullptr;
			if (Kind == ETextureSourceKind::TextureCube)
			{
				UTextureCube* Texture = Cast<UTextureCube>(
					TargetObject);
				if (Texture
					&& Texture->GetClass()
						== UTextureCube::StaticClass())
				{
					Target = Texture;
				}
			}
			else if (Kind
				== ETextureSourceKind::VolumeTexture)
			{
				UVolumeTexture* Texture =
					Cast<UVolumeTexture>(TargetObject);
				if (Texture
					&& Texture->GetClass()
						== UVolumeTexture::StaticClass())
				{
					Target = Texture;
				}
			}
			if (!Target)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_texture_source_"
						"target_invalid"));
			}
			if (!SourceRefs.bSpecified
				|| SourceRefs.Presence != TEXT("replace")
				|| !SourceRefs.CanonicalValue)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_texture_source_"
						"refs_invalid"));
			}

			TSharedPtr<
				FTextureSourcePreparedData,
				ESPMode::ThreadSafe> Data =
					MakeShared<
						FTextureSourcePreparedData,
						ESPMode::ThreadSafe>(
							Kind,
							*Target);
			if (!TryParseFormatSettings(
					FormatSettings,
					Data->NewSettings,
					OutError))
			{
				return false;
			}

			TArray<const FUeAgentCanonicalValue*> RefValues;
			if (Kind == ETextureSourceKind::TextureCube)
			{
				if (SourceRefs.CanonicalValue->GetKind()
						!= EUeAgentValueKind::Object
					|| SourceRefs.CanonicalValue
						->GetObjectItems().Num() != 6)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_texture_cube_"
							"face_refs_invalid"));
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
					const FUeAgentCanonicalValue* Value =
						FindObjectField(
							*SourceRefs.CanonicalValue,
							FaceField);
					if (!Value)
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_texture_cube_"
								"face_refs_invalid"),
							FaceField);
					}
					RefValues.Add(Value);
				}
			}
			else
			{
				if (SourceRefs.CanonicalValue->GetKind()
						!= EUeAgentValueKind::Array
					|| SourceRefs.CanonicalValue
						->GetArrayItems().IsEmpty()
					|| SourceRefs.CanonicalValue
						->GetArrayItems().Num()
						> MaximumVolumeTextureDepth)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_volume_texture_"
							"slice_refs_invalid"));
				}
				for (const FUeAgentCanonicalValueRef& Value
					: SourceRefs.CanonicalValue
						->GetArrayItems())
				{
					RefValues.Add(&Value.Get());
				}
				UVolumeTexture* Volume =
					CastChecked<UVolumeTexture>(Target);
				Data->OriginalVolumeSource2DTexture =
					Volume->Source2DTexture;
				Data->OriginalVolumeTileSizeX =
					Volume->Source2DTileSizeX;
				Data->OriginalVolumeTileSizeY =
					Volume->Source2DTileSizeY;
				Pin(
					Data->Pins,
					Data->OriginalVolumeSource2DTexture);
			}
			if (!TryResolveTexture2DSources(
					Resolver,
					RefValues,
					*Data,
					OutError)
				|| !TryFreezeTexture2DSourceData(
					*Data,
					OutError))
			{
				return false;
			}
			if (Kind == ETextureSourceKind::TextureCube
				&& Data->NewSourceSlices != 6)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_texture_cube_"
						"face_refs_invalid"));
			}
			OutState.Data = MoveTemp(Data);
			return true;
		}

		template <typename TTarget, typename TSource>
		bool RestoreTyped(
			TTarget& Target,
			const FTextureSourcePreparedData& Data,
			const bool bRestorePackageDirty)
		{
			Target.SourceTextures.Reset(
				Data.OriginalSourceObjects.Num());
			for (UObject* Object : Data.OriginalSourceObjects)
			{
				if (Object && !IsValid(Object))
				{
					return false;
				}
				Target.SourceTextures.Add(Cast<TSource>(Object));
			}
			Target.bSourceGeneratedFromSourceTexturesArray =
				Data.bOriginalGeneratedFromSourceArray;
			Target.Source = Data.OriginalSource;
			Target.Source.SetOwner(&Target);
			Target.SRGB = Data.bOriginalSRGB;
			Target.CompressionSettings = Data.OriginalCompression;
			Target.MipGenSettings = Data.OriginalMipGeneration;
			Target.PostEditChange();
			Target.SetLightingGuid(Data.OriginalLightingGuid);
			if (bRestorePackageDirty)
			{
				if (UPackage* Package = Target.GetOutermost())
				{
					Package->SetDirtyFlag(
						Data.bOriginalPackageDirty);
				}
			}
			return true;
		}

		bool Restore(
			UObject* Target,
			const FTextureSourcePreparedData& Data,
			const bool bRestorePackageDirty)
		{
			if (Data.Kind == ETextureSourceKind::Texture2DArray)
			{
				UTexture2DArray* Texture =
					Cast<UTexture2DArray>(Target);
				return Texture
					&& RestoreTyped<
						UTexture2DArray,
						UTexture2D>(
							*Texture,
							Data,
							bRestorePackageDirty);
			}
			if (Data.Kind
				== ETextureSourceKind::TextureCubeArray)
			{
				UTextureCubeArray* Texture =
					Cast<UTextureCubeArray>(Target);
				return Texture
					&& RestoreTyped<
						UTextureCubeArray,
						UTextureCube>(
							*Texture,
							Data,
							bRestorePackageDirty);
			}

			UTexture* Texture = nullptr;
			if (Data.Kind == ETextureSourceKind::TextureCube)
			{
				Texture = Cast<UTextureCube>(Target);
			}
			else if (Data.Kind
				== ETextureSourceKind::VolumeTexture)
			{
				UVolumeTexture* Volume =
					Cast<UVolumeTexture>(Target);
				if (!Volume)
				{
					return false;
				}
				if (Data.OriginalVolumeSource2DTexture
					&& !IsValid(
						Data.OriginalVolumeSource2DTexture))
				{
					return false;
				}
				Volume->Source2DTexture =
					Data.OriginalVolumeSource2DTexture;
				Volume->Source2DTileSizeX =
					Data.OriginalVolumeTileSizeX;
				Volume->Source2DTileSizeY =
					Data.OriginalVolumeTileSizeY;
				Texture = Volume;
			}
			if (!Texture)
			{
				return false;
			}
			Texture->Source = Data.OriginalSource;
			Texture->Source.SetOwner(Texture);
			Texture->SRGB = Data.bOriginalSRGB;
			Texture->CompressionSettings =
				Data.OriginalCompression;
			Texture->MipGenSettings =
				Data.OriginalMipGeneration;
			Texture->PostEditChange();
			Texture->SetLightingGuid(
				Data.OriginalLightingGuid);
			if (bRestorePackageDirty)
			{
				if (UPackage* Package =
					Texture->GetOutermost())
				{
					Package->SetDirtyFlag(
						Data.bOriginalPackageDirty);
				}
			}
			return true;
		}

		template <typename TTarget, typename TSource>
		bool ApplyTyped(
			TTarget& Target,
			const FTextureSourcePreparedData& Data,
			FExecutionMutationError& OutError)
		{
			Target.SourceTextures.Reset(
				Data.NewSourceObjects.Num());
			for (UObject* Object : Data.NewSourceObjects)
			{
				TSource* Source = Cast<TSource>(Object);
				if (!Source || !IsValid(Source))
				{
					Restore(&Target, Data, true);
					return Fail(
						OutError,
						TEXT(
							"asset_data_texture_array_"
							"resolved_source_expired"));
				}
				Target.SourceTextures.Add(Source);
			}
			Target.bSourceGeneratedFromSourceTexturesArray = true;
			if (!Target.UpdateSourceFromSourceTextures(false))
			{
				Restore(&Target, Data, true);
				return Fail(
					OutError,
					TEXT(
						"asset_data_texture_array_"
						"native_source_update_failed"));
			}
			Target.SRGB = Data.NewSettings.bSRGB;
			Target.CompressionSettings =
				Data.NewSettings.Compression;
			Target.MipGenSettings =
				Data.NewSettings.MipGeneration;
			Target.ValidateSettingsAfterImportOrEdit();
			Target.SetLightingGuid();
			Target.PostEditChange();
			return true;
		}

		bool ApplyAssembledTexture(
			UTexture& Target,
			const FTextureSourcePreparedData& Data,
			FExecutionMutationError& OutError)
		{
			for (UObject* Object : Data.NewSourceObjects)
			{
				UTexture2D* Source = Cast<UTexture2D>(Object);
				if (!Source
					|| Source->GetClass()
						!= UTexture2D::StaticClass()
					|| !IsValid(Source))
				{
					Restore(&Target, Data, true);
					return Fail(
						OutError,
						TEXT(
							"asset_data_texture_source_"
							"resolved_source_expired"));
				}
			}
			if (Data.NewSourceSizeX <= 0
				|| Data.NewSourceSizeY <= 0
				|| Data.NewSourceSlices <= 0
				|| Data.NewSourceFormat == TSF_Invalid
				|| Data.NewSourceTopMipData.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_texture_source_"
						"prepared_data_invalid"));
			}

			Target.PreEditChange(nullptr);
			Target.Source.Init(
				Data.NewSourceSizeX,
				Data.NewSourceSizeY,
				Data.NewSourceSlices,
				1,
				Data.NewSourceFormat,
				Data.NewSourceTopMipData.GetData());
			if (Data.Kind
				== ETextureSourceKind::VolumeTexture)
			{
				UVolumeTexture& Volume =
					*CastChecked<UVolumeTexture>(&Target);
				Volume.Source2DTexture = nullptr;
				Volume.Source2DTileSizeX = 0;
				Volume.Source2DTileSizeY = 0;
			}
			Target.SRGB = Data.NewSettings.bSRGB;
			Target.CompressionSettings =
				Data.NewSettings.Compression;
			Target.MipGenSettings =
				Data.NewSettings.MipGeneration;
			Target.ValidateSettingsAfterImportOrEdit();
			Target.SetLightingGuid();
			Target.PostEditChange();
			return true;
		}
	}

	bool TryPrepareTexture2DArrayReplacement(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		UObject* Target,
		const FTextureSourceChangeView& SourceTextureRefs,
		const FTextureSourceChangeView& FormatSettings,
		FTextureSourcePreparedState& OutState,
		FExecutionMutationError& OutError)
	{
		return TryPrepareReplacement<
			UTexture2DArray,
			UTexture2D>(
				Resolver,
				Target,
				ETextureSourceKind::Texture2DArray,
				SourceTextureRefs,
				FormatSettings,
				OutState,
				OutError);
	}

	bool TryPrepareTextureCubeArrayReplacement(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		UObject* Target,
		const FTextureSourceChangeView& SourceCubeRefs,
		const FTextureSourceChangeView& FormatSettings,
		FTextureSourcePreparedState& OutState,
		FExecutionMutationError& OutError)
	{
		if (!TryPrepareReplacement<
				UTextureCubeArray,
				UTextureCube>(
					Resolver,
					Target,
					ETextureSourceKind::TextureCubeArray,
					SourceCubeRefs,
					FormatSettings,
					OutState,
					OutError))
		{
			return false;
		}
		if (!OutState.Data.IsValid()
			|| !ValidateCubeArraySources(
				*OutState.Data,
				OutError))
		{
			OutState.Data.Reset();
			return false;
		}
		return true;
	}

	bool TryPrepareTextureCubeReplacement(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		UObject* Target,
		const FTextureSourceChangeView& FaceTextureRefs,
		const FTextureSourceChangeView& FormatSettings,
		FTextureSourcePreparedState& OutState,
		FExecutionMutationError& OutError)
	{
		return TryPrepareAssembledReplacement(
			Resolver,
			Target,
			ETextureSourceKind::TextureCube,
			FaceTextureRefs,
			FormatSettings,
			OutState,
			OutError);
	}

	bool TryPrepareVolumeTextureReplacement(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		UObject* Target,
		const FTextureSourceChangeView& SliceTextureRefs,
		const FTextureSourceChangeView& FormatSettings,
		FTextureSourcePreparedState& OutState,
		FExecutionMutationError& OutError)
	{
		return TryPrepareAssembledReplacement(
			Resolver,
			Target,
			ETextureSourceKind::VolumeTexture,
			SliceTextureRefs,
			FormatSettings,
			OutState,
			OutError);
	}

	bool TryApplyTextureSourceReplacement(
		UObject* Target,
		const FTextureSourcePreparedState& State,
		FExecutionMutationError& OutError)
	{
		if (!Target || !State.Data.IsValid())
		{
			return Fail(
				OutError,
				TEXT("asset_data_texture_array_state_invalid"));
		}
		if (State.Data->Kind
			== ETextureSourceKind::Texture2DArray)
		{
			UTexture2DArray* Texture =
				Cast<UTexture2DArray>(Target);
			return Texture
				? ApplyTyped<UTexture2DArray, UTexture2D>(
					*Texture,
					*State.Data,
					OutError)
				: Fail(
					OutError,
					TEXT(
						"asset_data_texture_array_"
						"target_invalid"));
		}
		if (State.Data->Kind
			== ETextureSourceKind::TextureCubeArray)
		{
			UTextureCubeArray* Texture =
				Cast<UTextureCubeArray>(Target);
			return Texture
				? ApplyTyped<
					UTextureCubeArray,
					UTextureCube>(
						*Texture,
						*State.Data,
						OutError)
				: Fail(
					OutError,
					TEXT(
						"asset_data_texture_array_"
						"target_invalid"));
		}

		UTexture* Texture = nullptr;
		if (State.Data->Kind
			== ETextureSourceKind::TextureCube)
		{
			UTextureCube* Cube = Cast<UTextureCube>(Target);
			if (Cube
				&& Cube->GetClass()
					== UTextureCube::StaticClass())
			{
				Texture = Cube;
			}
		}
		else if (State.Data->Kind
			== ETextureSourceKind::VolumeTexture)
		{
			UVolumeTexture* Volume =
				Cast<UVolumeTexture>(Target);
			if (Volume
				&& Volume->GetClass()
					== UVolumeTexture::StaticClass())
			{
				Texture = Volume;
			}
		}
		return Texture
			? ApplyAssembledTexture(
				*Texture,
				*State.Data,
				OutError)
			: Fail(
				OutError,
				TEXT(
					"asset_data_texture_source_target_invalid"));
	}

	bool TryRollbackTextureSourceReplacement(
		UObject* Target,
		const FTextureSourcePreparedState& State)
	{
		return Target
			&& State.Data.IsValid()
			&& Restore(Target, *State.Data, false);
	}
}
