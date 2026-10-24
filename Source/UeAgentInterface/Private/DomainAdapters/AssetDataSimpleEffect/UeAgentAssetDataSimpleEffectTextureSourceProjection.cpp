#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTextureSourceProjection.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Misc/ScopeExit.h"

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

		bool Fail(FExecutionMutationError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			OutError = MoveTemp(Diagnostic);
			return false;
		}

        FExecutionMutationError JsonFailure(const FString& Code, const FString& Reason, const FString& Field,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual, TEXT("execution"), Reason);
            Error.SourceKind = SourceKind;
            Error.ActualValuePresent = Actual.IsValid();
            Error.ExpectedValuePresent = Expected.IsValid();
            if (Error.SourceKind == TEXT("native_result"))
            {
                Error.Source = {};
                Error.SemanticPointer.Reset();
            }
            return Error;
        }

        FExecutionMutationError ValueFailure(const FString& Code, const FString& Reason, const FString& Field,
            const FUeAgentCanonicalValue* Actual, const FString& Expected,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            auto Error = JsonFailure(Code, Reason, Field, nullptr, MakeShared<FJsonValueString>(Expected), SourceKind);
            Error.ActualValuePresent = Actual != nullptr;
            Contract::FDiagnostic ProjectionError;
            if (Actual && !CanonicalJson::TryToJsonValue(*Actual, Error.ActualValue, ProjectionError))
            {
                ProjectionError.SourceKind = TEXT("native_result");
                Error.Diagnostics.Add(MoveTemp(ProjectionError));
            }
            return Error;
        }

        TSharedPtr<FJsonValue> ObjectClassValue(const UObject* Object)
        {
            return Object ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()))
                : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
        }

        FExecutionMutationError SourceFailure(const FString& Code, const FString& Reason, const UTexture& Source,
            const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
        {
            auto Error = JsonFailure(Code, Reason, Field, Actual, Expected, TEXT("native_result"));
            Error.TargetPath = Source.GetPathName();
            return Error;
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
				: Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
						? TEXT("asset_data_texture_source_projection_string_invalid")
						: StateError.Code, StateError, TEXT("execution")));
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
				: Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
						? TEXT("asset_data_texture_source_projection_boolean_invalid")
						: StateError.Code, StateError, TEXT("execution")));
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
				: Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
						? TEXT("asset_data_texture_source_projection_integer_invalid")
						: StateError.Code, StateError, TEXT("execution")));
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
				: Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
						? TEXT("asset_data_texture_source_projection_null_invalid")
						: StateError.Code, StateError, TEXT("execution")));
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
				: Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
						? TEXT("asset_data_texture_source_projection_array_invalid")
						: StateError.Code, StateError, TEXT("execution")));
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
				: Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
						? TEXT("asset_data_texture_source_projection_object_invalid")
						: StateError.Code, StateError, TEXT("execution")));
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

        bool TryReadString(const FUeAgentCanonicalValue& Value, FString& OutText,
            const FString& Field, FExecutionMutationError& OutError,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            if (Value.GetKind() != EUeAgentValueKind::String || Value.GetText().IsEmpty())
            {
                OutText.Reset();
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_string_invalid"),
                    TEXT("A nonempty string is required."), Field, &Value, TEXT("nonempty string"), SourceKind));
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
            ON_SCOPE_EXIT { if (!OutError.IsEmpty()) { OutError.SourceKind = TEXT("native_result");
                if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Texture.GetPathName(); } };
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
            if (!Texture.Source.GetMipData(TopMip, 0))
                return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_projection_data_invalid"),
                    TEXT("The native texture source top mip could not be read."), Texture, TEXT("source_signature/mip/0"),
                    MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("readable top mip"))));
            if (TopMip.IsEmpty())
                return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_projection_data_invalid"),
                    TEXT("The native texture source top mip is empty."), Texture, TEXT("source_signature/mip/0/byte_count"),
                    MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("nonzero byte count"))));
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
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_format_invalid"),
                    TEXT("Texture format settings require exactly the three declared fields."), TEXT("format_settings"), &Requested,
                    TEXT("object with srgb, compression and mip_generation")));
			}
			const FUeAgentCanonicalValue* Srgb =
				FindObjectField(Requested, TEXT("srgb"));
			const FUeAgentCanonicalValue* Compression =
				FindObjectField(Requested, TEXT("compression"));
			const FUeAgentCanonicalValue* MipGeneration =
				FindObjectField(Requested, TEXT("mip_generation"));
			FString CompressionName;
			FString MipGenerationName;
            if (!Srgb || Srgb->GetKind() != EUeAgentValueKind::Boolean)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_format_invalid"),
                    TEXT("The format settings require a boolean srgb field."), TEXT("format_settings/srgb"), Srgb, TEXT("boolean")));
            if (!Compression)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_format_invalid"),
                    TEXT("The compression field is missing."), TEXT("format_settings/compression"), nullptr, TEXT("nonempty compression name")));
            if (!TryReadString(*Compression, CompressionName, TEXT("format_settings/compression"), OutError)) return false;
            if (!MipGeneration)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_format_invalid"),
                    TEXT("The mip generation field is missing."), TEXT("format_settings/mip_generation"), nullptr, TEXT("nonempty mip generation name")));
            if (!TryReadString(*MipGeneration, MipGenerationName, TEXT("format_settings/mip_generation"), OutError)) return false;
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
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_format_invalid"),
                    TEXT("The texture compression mode is unsupported."), TEXT("format_settings/compression"), Compression,
                    TEXT("default, masks, normal_map, grayscale or hdr")));
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
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_format_invalid"),
                    TEXT("The texture mip generation mode is unsupported."), TEXT("format_settings/mip_generation"), MipGeneration,
                    TEXT("from_texture_group, simple_average or no_mipmaps")));
			}
			if (Srgb->GetBoolean()
				&& (CompressionValue == TC_Normalmap
					|| CompressionValue == TC_HDR))
			{
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetBoolField(TEXT("srgb"), true);
                Actual->SetStringField(TEXT("compression"), Compression->GetText());
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_expected_format_incompatible"),
                    TEXT("Normal map and HDR compression require sRGB to be disabled."), TEXT("format_settings/srgb"),
                    MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueBoolean>(false)));
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
			FExecutionMutationError& OutError, const TArray<FString>& RefFields)
		{
			OutSet = FResolvedSourceSet();
			if (RefValues.IsEmpty()
				|| (Kind == ETextureSourceProjectionKind::TextureCube
					&& RefValues.Num() != 6))
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_expected_refs_invalid"),
                    Kind == ETextureSourceProjectionKind::TextureCube ? TEXT("Cube projection requires exactly six face references.")
                        : TEXT("At least one source texture reference is required."),
                    GetSourceFieldId(Kind), MakeShared<FJsonValueNumber>(RefValues.Num()),
                    MakeShared<FJsonValueString>(Kind == ETextureSourceProjectionKind::TextureCube ? TEXT("6 references") : TEXT("at least 1 reference"))));
			}
			FSHA1 Sha1;
			int64 SourceDataSize = -1;
			int32 SourceSlices = -1;
            for (int32 Index = 0; Index < RefValues.Num(); ++Index)
            {
                const FUeAgentCanonicalValue* RefValue = RefValues[Index];
                const FString& Field = RefFields[Index];
                FString CanonicalRef;
                UObject* Resolved = nullptr;
                if (!RefValue)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_ref_invalid"),
                        TEXT("The source texture reference is missing."), Field, nullptr, TEXT("texture asset reference")));
                if (!TryReadString(*RefValue, CanonicalRef, Field, OutError)) return false;
                if (!Resolver.TryResolveExisting(CanonicalRef, Resolved, OutError))
                {
                    if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Field;
                    return false;
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
                if (!Source)
                {
                    auto Actual = MakeShared<FJsonObject>();
                    Actual->SetStringField(TEXT("reference"), CanonicalRef);
                    Actual->SetField(TEXT("resolved_class"), ObjectClassValue(Resolved));
                    return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_expected_source_invalid"),
                        TEXT("The source reference must resolve to the exact supported texture class."), Field,
                        MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(Kind == ETextureSourceProjectionKind::TextureCubeArray
                            ? UTextureCube::StaticClass()->GetPathName() : UTexture2D::StaticClass()->GetPathName())));
                }
                if (!Source->Source.IsValid())
                    return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_expected_source_invalid"),
                        TEXT("The referenced texture has no valid native source data."), *Source, Field,
                        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("valid native texture source"))));
                if ((Kind != ETextureSourceProjectionKind::TextureCubeArray && Source->Source.GetNumSlices() != 1)
                    || (Kind == ETextureSourceProjectionKind::TextureCubeArray
                        && Source->Source.GetNumSlices() != 1 && Source->Source.GetNumSlices() != 6))
                    return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_expected_source_invalid"),
                        TEXT("The native texture source has an incompatible slice count."), *Source, Field + TEXT("/slices"),
                        MakeShared<FJsonValueNumber>(Source->Source.GetNumSlices()),
                        MakeShared<FJsonValueString>(Kind == ETextureSourceProjectionKind::TextureCubeArray ? TEXT("1 or 6 slices") : TEXT("1 slice"))));
                TArray64<uint8> SourceData;
                if (!Source->Source.GetMipData(SourceData, 0))
                    return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_expected_data_invalid"),
                        TEXT("The native texture source top mip could not be read."), *Source, Field + TEXT("/mip/0"),
                        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("readable top mip"))));
                if (SourceData.IsEmpty())
                    return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_expected_data_invalid"),
                        TEXT("The native texture source top mip is empty."), *Source, Field + TEXT("/mip/0/byte_count"),
                        MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("nonzero byte count"))));
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
                    const TCHAR* Names[] = {TEXT("width"), TEXT("height"), TEXT("mips"), TEXT("slices"), TEXT("format"), TEXT("mip/0/byte_count")};
                    const int64 Actual[] = {Source->Source.GetSizeX(), Source->Source.GetSizeY(), Source->Source.GetNumMips(),
                        Source->Source.GetNumSlices(), static_cast<int32>(Source->Source.GetFormat()), SourceData.Num()};
                    const int64 Expected[] = {OutSet.SizeX, OutSet.SizeY, OutSet.NumMips, SourceSlices, static_cast<int32>(OutSet.Format), SourceDataSize};
                    for (int32 Component = 0; Component < 6; ++Component)
                        if (Actual[Component] != Expected[Component])
                            return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_expected_sources_incompatible"),
                                TEXT("The texture source differs from the first source in the set."), *Source, Field + TEXT("/") + Names[Component],
                                MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), Actual[Component])),
                                MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), Expected[Component]))));
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
            if (OutSet.SizeX <= 0 || OutSet.SizeY <= 0 || OutSet.NumSlices <= 0 || OutSet.Format == TSF_Invalid)
            {
                const FString Component = OutSet.SizeX <= 0 ? TEXT("width") : OutSet.SizeY <= 0 ? TEXT("height")
                    : OutSet.NumSlices <= 0 ? TEXT("slices") : TEXT("format");
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_expected_signature_invalid"),
                    TEXT("The resolved source signature has an invalid dimension, slice count or format."),
                    FString(GetSourceFieldId(Kind)) + TEXT("/source_signature/") + Component,
                    MakeShared<FJsonValueNumber>(OutSet.SizeX <= 0 ? OutSet.SizeX : OutSet.SizeY <= 0 ? OutSet.SizeY
                        : OutSet.NumSlices <= 0 ? OutSet.NumSlices : static_cast<int32>(OutSet.Format)),
                    MakeShared<FJsonValueString>(Component == TEXT("format") ? TEXT("valid texture source format") : TEXT("positive integer")), TEXT("native_result")));
            }
            return true;
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
            ON_SCOPE_EXIT { if (!OutError.IsEmpty()) { OutError.SourceKind = TEXT("native_result");
                if (OutError.TargetPath.IsEmpty() && TargetObject) OutError.TargetPath = TargetObject->GetPathName(); } };
            UTexture* Target = nullptr;
			FUeAgentCanonicalValuePtr SourceState;
			if (Kind == ETextureSourceProjectionKind::Texture2DArray)
			{
				UTexture2DArray* Texture = Cast<UTexture2DArray>(TargetObject);
				if (!Texture
					|| Texture->GetClass() != UTexture2DArray::StaticClass())
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_projection_target_invalid"),
                        TEXT("The projection target must have the exact supported texture class."), TEXT("target"), ObjectClassValue(TargetObject),
                        MakeShared<FJsonValueString>(UTexture2DArray::StaticClass()->GetPathName()), TEXT("native_result")));
				}
				TArray<FString> Paths;
                for (int32 Index = 0; Index < Texture->SourceTextures.Num(); ++Index)
                {
                    UTexture2D* Source = Texture->SourceTextures[Index];
                    if (!Source || Source->GetClass() != UTexture2D::StaticClass())
                        return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_projection_current_ref_invalid"),
                            TEXT("The native source array contains an unavailable texture or an incompatible class."),
                            FString(GetSourceFieldId(Kind)) + TEXT("/") + FString::FromInt(Index), ObjectClassValue(Source),
                            MakeShared<FJsonValueString>(UTexture2D::StaticClass()->GetPathName()), TEXT("native_result")));
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
                    return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_projection_target_invalid"),
                        TEXT("The projection target must have the exact supported texture class."), TEXT("target"), ObjectClassValue(TargetObject),
                        MakeShared<FJsonValueString>(UTextureCubeArray::StaticClass()->GetPathName()), TEXT("native_result")));
				}
				TArray<FString> Paths;
                for (int32 Index = 0; Index < Texture->SourceTextures.Num(); ++Index)
                {
                    UTextureCube* Source = Texture->SourceTextures[Index];
                    if (!Source || Source->GetClass() != UTextureCube::StaticClass())
                        return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_projection_current_ref_invalid"),
                            TEXT("The native source array contains an unavailable texture or an incompatible class."),
                            FString(GetSourceFieldId(Kind)) + TEXT("/") + FString::FromInt(Index), ObjectClassValue(Source),
                            MakeShared<FJsonValueString>(UTextureCube::StaticClass()->GetPathName()), TEXT("native_result")));
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
                if (!Target || Target->GetClass() != ExpectedClass)
                    return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_projection_target_invalid"),
                        TEXT("The projection target must have the exact supported assembled texture class."), TEXT("target"),
                        ObjectClassValue(TargetObject), MakeShared<FJsonValueString>(ExpectedClass->GetPathName()), TEXT("native_result")));
                if (!TryBuildAssembledSourceState(*Target, Kind, Resources, SourceState, OutError)) return false;
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
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_selected_fields_missing"),
                    TEXT("At least one texture source field must be selected."), TEXT("selected_field_ids"),
                    MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("nonempty field list"))));
			}
			TSet<FString> Seen;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> CurrentItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> DefaultItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> ContextItems;
            for (int32 Index = 0; Index < SelectedFieldIds.Num(); ++Index)
            {
                const FString& FieldId = SelectedFieldIds[Index];
				const FUeAgentCanonicalValueRef* Value = Values.Find(FieldId);
				const TCHAR* ContextTag = FindContextTag(Kind, FieldId);
				FUeAgentCanonicalValuePtr Context;
				if (Seen.Contains(FieldId)
					|| !Value
					|| !ContextTag)
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_selected_field_invalid"),
                        Seen.Contains(FieldId) ? TEXT("The field is selected more than once.") : !Value
                            ? TEXT("The selected texture field has no captured value.") : TEXT("The selected field has no registered projection context."),
                        TEXT("selected_field_ids/") + FString::FromInt(Index), MakeShared<FJsonValueString>(FieldId),
                        MakeShared<FJsonValueString>(FString(GetSourceFieldId(Kind)) + TEXT(", format_settings"))));
				}
				if (!TryBuildString(ContextTag, Resources, Context, OutError)) return false;
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
            const FUeAgentCanonicalValue* States[] = {&CurrentFields, &DefaultFields, &ProjectionContext};
            const TCHAR* Names[] = {TEXT("current_fields"), TEXT("default_fields"), TEXT("projection_context")};
            for (int32 Index = 0; Index < 3; ++Index)
                if (States[Index]->GetKind() != EUeAgentValueKind::Object)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_state_invalid"),
                        TEXT("The captured texture field state must be an object."), Names[Index], States[Index], TEXT("object"), TEXT("native_result")));
            for (int32 Index = 1; Index < 3; ++Index)
                if (CurrentFields.GetObjectItems().Num() != States[Index]->GetObjectItems().Num())
                    return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_expected_state_invalid"),
                        TEXT("The captured field state count does not match the current field count."), Names[Index],
                        MakeShared<FJsonValueNumber>(States[Index]->GetObjectItems().Num()),
                        MakeShared<FJsonValueNumber>(CurrentFields.GetObjectItems().Num()), TEXT("native_result")));
            if (!TypedChanges || TypedChanges->GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_state_invalid"),
                    TEXT("The proposed state requires a typed_changes array."), TEXT("typed_changes"), TypedChanges, TEXT("array")));
            if (TypedChanges->GetArrayItems().Num() != CurrentFields.GetObjectItems().Num())
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_expected_state_invalid"),
                    TEXT("The typed change count does not match the selected field count."), TEXT("typed_changes"),
                    MakeShared<FJsonValueNumber>(TypedChanges->GetArrayItems().Num()), MakeShared<FJsonValueNumber>(CurrentFields.GetObjectItems().Num())));
            for (int32 Index = 0; Index < TypedChanges->GetArrayItems().Num(); ++Index)
            {
                const FUeAgentCanonicalValue& Change = TypedChanges->GetArrayItems()[Index].Get();
                const FUeAgentCanonicalValue* Field = FindObjectField(Change, TEXT("field_id"));
                const FString Path = TEXT("typed_changes/") + FString::FromInt(Index) + TEXT("/field_id");
                FString FieldId;
                if (!Field)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_change_invalid"),
                        TEXT("The typed change has no field_id."), Path, nullptr, TEXT("selected field_id")));
                if (!TryReadString(*Field, FieldId, Path, OutError)) return false;
                if (OutChanges.Contains(FieldId) || !FindObjectField(CurrentFields, FieldId))
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_change_invalid"),
                        OutChanges.Contains(FieldId) ? TEXT("The field has more than one typed change.") : TEXT("The typed change refers to an unselected field."),
                        Path, Field, TEXT("unique selected field_id")));
                OutChanges.Add(FieldId, &Change);
            }
			return true;
		}

        bool TryReadReplaceValue(const FUeAgentCanonicalValue& Change, const FUeAgentCanonicalValue*& OutRequested,
            const FString& Field, FExecutionMutationError& OutError)
        {
            OutRequested = nullptr;
            const FUeAgentCanonicalValue* Presence = FindObjectField(Change, TEXT("presence"));
            FString PresenceText;
            if (!Presence)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_change_invalid"),
                    TEXT("The field change has no presence value."), Field + TEXT("/presence"), nullptr, TEXT("replace")));
            if (!TryReadString(*Presence, PresenceText, Field + TEXT("/presence"), OutError)) return false;
            if (PresenceText != TEXT("replace"))
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_change_invalid"),
                    TEXT("This texture field requires the replace operation."), Field + TEXT("/presence"), Presence, TEXT("replace")));
            const FUeAgentCanonicalValue* TypedValue = FindObjectField(Change, TEXT("value"));
            OutRequested = TypedValue ? FindObjectField(*TypedValue, TEXT("canonical_value")) : nullptr;
            if (!OutRequested)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_change_invalid"),
                    TEXT("The replacement canonical value is missing."), Field, nullptr, TEXT("replacement canonical value")));
            return true;
        }

        bool TryCollectRequestedRefs(
            const FUeAgentCanonicalValue& Requested,
            const ETextureSourceProjectionKind Kind,
            TArray<const FUeAgentCanonicalValue*>& OutRefs,
            FExecutionMutationError& OutError, TArray<FString>& OutRefFields)
        {
            OutRefs.Reset();
            OutRefFields.Reset();
            const FString Field = GetSourceFieldId(Kind);
			if (Kind == ETextureSourceProjectionKind::TextureCube)
			{
				if (Requested.GetKind() != EUeAgentValueKind::Object
					|| Requested.GetObjectItems().Num() != 6)
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_refs_invalid"),
                        TEXT("Cube face references must contain exactly the six named faces."), Field, &Requested,
                        TEXT("object with positive_x, negative_x, positive_y, negative_y, positive_z and negative_z")));
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
                        return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_refs_invalid"),
                            TEXT("The cube source is missing this face reference."), Field + TEXT("/") + FaceField, nullptr, TEXT("Texture2D reference")));
                    }
                    OutRefs.Add(Ref);
                    OutRefFields.Add(Field + TEXT("/") + FaceField);
				}
				return true;
			}
			if (Requested.GetKind() != EUeAgentValueKind::Array
				|| Requested.GetArrayItems().IsEmpty())
			{
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_refs_invalid"),
                    TEXT("Texture sources require a nonempty array of references."), Field, &Requested, TEXT("nonempty reference array")));
			}
            for (int32 Index = 0; Index < Requested.GetArrayItems().Num(); ++Index)
            {
                OutRefs.Add(&Requested.GetArrayItems()[Index].Get());
                OutRefFields.Add(Field + TEXT("/") + FString::FromInt(Index));
            }
			return true;
		}
	}

	bool TryReadAssembledTextureSourceState(
		UTexture& Target,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutState,
		FExecutionMutationError& OutError)
	{
        OutState.Reset();
        ON_SCOPE_EXIT { if (!OutError.IsEmpty()) { OutError.SourceKind = TEXT("native_result");
            if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.GetPathName(); } };
        if (Target.GetClass() != UTextureCube::StaticClass() && Target.GetClass() != UVolumeTexture::StaticClass())
            return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_target_invalid"),
                TEXT("Assembled source readback requires an exact TextureCube or VolumeTexture target."), TEXT("target"),
                ObjectClassValue(&Target), MakeShared<FJsonValueString>(TEXT("TextureCube or VolumeTexture")), TEXT("native_result")));
		return TryBuildAssembledSourceState(Target,
			Target.IsA<UTextureCube>() ? ETextureSourceProjectionKind::TextureCube
				: ETextureSourceProjectionKind::VolumeTexture,
			Resources, OutState, OutError);
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
            OutError = JsonFailure(TEXT("asset_data_texture_source_projection_unsupported"),
                !CanProjectTextureSourceState(NativeMechanicId) ? TEXT("No texture source projection is registered for this operation.")
                    : TEXT("The texture source projection target is unavailable."),
                TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(TEXT("supported texture source projection and live target")), TEXT("native_result"));
            if (Target) OutError.TargetPath = Target->GetPathName();
            OutError.InternalContext = NativeMechanicId;
            return false;
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
            OutError = JsonFailure(TEXT("asset_data_texture_source_projection_unsupported"),
                TEXT("No expected texture source projection is registered for this operation."), FString(), nullptr,
                MakeShared<FJsonValueString>(TEXT("registered texture source projection")), TEXT("native_result"));
            OutError.InternalContext = NativeMechanicId;
            return false;
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
            if (!Change)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_change_invalid"),
                    TEXT("The selected field has no typed change."), Current.Key, nullptr, TEXT("typed change for the selected field")));
            if (!Context || !ExpectedContext)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_expected_change_invalid"),
                    !Context ? TEXT("The captured field projection context is missing.") : TEXT("The field has no registered projection context."),
                    Current.Key, Context, TEXT("registered projection context"), TEXT("native_result")));
            if (!TryReadString(*Context, ContextText, Current.Key, OutError, TEXT("native_result"))) return false;
            if (ContextText != ExpectedContext)
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_expected_change_invalid"),
                    TEXT("The captured context tag does not match the selected texture field."), Current.Key,
                    MakeShared<FJsonValueString>(ContextText), MakeShared<FJsonValueString>(ExpectedContext), TEXT("native_result")));
            if (!TryReadReplaceValue(**Change, Requested, Current.Key, OutError)) return false;

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
                TArray<FString> RefFields;
                FResolvedSourceSet SourceSet;
                if (!TryCollectRequestedRefs(*Requested, Kind, RefValues, OutError, RefFields)
                    || !TryResolveSourceSet(Resolver, RefValues, Kind, SourceSet, OutError, RefFields))
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
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_expected_field_invalid"),
                    TEXT("This field is not supported by the asset type."), Current.Key, MakeShared<FJsonValueString>(Current.Key),
                    MakeShared<FJsonValueString>(FString(GetSourceFieldId(Kind)) + TEXT(", format_settings")), TEXT("native_result")));
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
