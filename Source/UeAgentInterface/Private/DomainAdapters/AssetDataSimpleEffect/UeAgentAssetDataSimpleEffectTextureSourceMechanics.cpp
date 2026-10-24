#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTextureSourceMechanics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Misc/ScopeExit.h"

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

        FExecutionMutationError NativeFailure(const FString& Code, const FString& Reason, const FString& Field,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
        {
            auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual, TEXT("execution"), Reason);
            Error.SourceKind = TEXT("native_result");
            Error.Source = {};
            Error.SemanticPointer.Reset();
            Error.ActualValuePresent = Actual.IsValid();
            Error.ExpectedValuePresent = Expected.IsValid();
            return Error;
        }

        TSharedPtr<FJsonValue> ObjectClassValue(const UObject* Object)
        {
            return Object ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()))
                : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
        }

        bool Fail(FExecutionMutationError& OutError, FExecutionMutationError Error)
        {
            OutError = MoveTemp(Error);
            return false;
        }


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

        FExecutionMutationError SourceFailure(const FString& Code, const FString& Reason, const UTexture* Source,
            const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
        {
            auto Error = NativeFailure(Code, Reason, Field, Actual, Expected);
            if (Source) Error.TargetPath = Source->GetPathName();
            return Error;
        }

        TSharedPtr<FJsonValue> ReferenceClassValue(const FString& Reference, const UObject* Object)
        {
            auto Actual = MakeShared<FJsonObject>();
            Actual->SetStringField(TEXT("reference"), Reference);
            Actual->SetField(TEXT("resolved_class"), ObjectClassValue(Object));
            return MakeShared<FJsonValueObject>(Actual);
        }

        FExecutionMutationError ChangeFailure(const FString& Code, const FString& Field,
            const FTextureSourceChangeView& Change, const FString& Expected)
        {
            if (!Change.bSpecified)
                return ValueFailure(Code, TEXT("The required field change is missing."), Field, nullptr, Expected);
            if (Change.Presence != TEXT("replace"))
                return JsonFailure(Code, TEXT("The field requires the replace operation."), Field + TEXT("/presence"),
                    MakeShared<FJsonValueString>(Change.Presence), MakeShared<FJsonValueString>(TEXT("replace")));
            return ValueFailure(Code, Change.CanonicalValue ? TEXT("The replacement value has an incompatible shape.")
                : TEXT("The replacement value is missing."), Field, Change.CanonicalValue, Expected);
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
            TextureCompressionSettings& OutCompression, FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::String)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_compression_invalid"),
                    TEXT("Compression must be one of the supported names."), TEXT("format_settings/compression"), &Value,
                    TEXT("default, masks, normal_map, grayscale or hdr")));
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
            return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_compression_invalid"),
                TEXT("The compression name is not supported."), TEXT("format_settings/compression"), &Value,
                TEXT("default, masks, normal_map, grayscale or hdr")));
		}

		bool TryParseMipGeneration(
			const FUeAgentCanonicalValue& Value,
            TextureMipGenSettings& OutMipGeneration, FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::String)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_mip_generation_invalid"),
                    TEXT("Mip generation must be one of the supported names."), TEXT("format_settings/mip_generation"), &Value,
                    TEXT("from_texture_group, simple_average or no_mipmaps")));
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
            return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_mip_generation_invalid"),
                TEXT("The mip generation name is not supported."), TEXT("format_settings/mip_generation"), &Value,
                TEXT("from_texture_group, simple_average or no_mipmaps")));
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
                return Fail(OutError, ChangeFailure(TEXT("asset_data_texture_source_format_settings_invalid"),
                    TEXT("format_settings"), Change, TEXT("object with srgb, compression and mip_generation")));
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
            if (!SRGB || SRGB->GetKind() != EUeAgentValueKind::Boolean)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_format_settings_invalid"),
                    TEXT("The format settings require a boolean srgb field."), TEXT("format_settings/srgb"), SRGB, TEXT("boolean")));
            if (!Compression || !MipGeneration)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_format_settings_invalid"),
                    TEXT("A required format settings field is missing."),
                    !Compression ? TEXT("format_settings/compression") : TEXT("format_settings/mip_generation"), nullptr,
                    !Compression ? TEXT("compression name") : TEXT("mip generation name")));
            if (!TryParseCompression(*Compression, OutSettings.Compression, OutError)
                || !TryParseMipGeneration(*MipGeneration, OutSettings.MipGeneration, OutError)) return false;
			OutSettings.bSRGB = SRGB->GetBoolean();
			if (OutSettings.bSRGB
				&& (OutSettings.Compression == TC_Normalmap
					|| OutSettings.Compression == TC_HDR))
			{
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetBoolField(TEXT("srgb"), OutSettings.bSRGB);
                Actual->SetStringField(TEXT("compression"), Compression->GetText());
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_format_settings_incompatible"),
                    TEXT("Normal map and HDR compression require sRGB to be disabled."), TEXT("format_settings/srgb"),
                    MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueBoolean>(false)));
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
			UTexture& Target,
			const bool bInCaptureNativeRollback = true)
			: Kind(InKind)
			, OriginalSource(bInCaptureNativeRollback ? Target.Source.CopyTornOff() : FTextureSource())
			, bOriginalSRGB(Target.SRGB)
			, OriginalCompression(Target.CompressionSettings)
			, OriginalMipGeneration(Target.MipGenSettings)
			, OriginalLightingGuid(Target.GetLightingGuid())
			, bOriginalPackageDirty(
				Target.GetOutermost()
					&& Target.GetOutermost()->IsDirty())
			, bCaptureNativeRollback(bInCaptureNativeRollback)
		{
		}

		ETextureSourceKind Kind;
		TArray<UObject*> OriginalSourceObjects;
		TArray<UObject*> NewSourceObjects;
        TArray<FString> NewSourceFields;
        FString SourceFieldAt(const int32 Index) const
        {
            return NewSourceFields.IsValidIndex(Index) ? NewSourceFields[Index]
                : TEXT("source_refs/") + FString::FromInt(Index);
        }
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
		bool bCaptureNativeRollback = true;
		FTextureFormatSettings NewSettings;
		bool bApplyPreparedFormatSettings = true;
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
            const FString Field = Kind == ETextureSourceKind::Texture2DArray ? TEXT("source_texture_refs") : TEXT("source_cube_refs");
            ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty() && TargetObject)
                OutError.TargetPath = TargetObject->GetPathName(); };
            TTarget* Target = Cast<TTarget>(TargetObject);
			if (!Target
				|| Target->GetClass() != TTarget::StaticClass())
			{
                return Fail(OutError, NativeFailure(TEXT("asset_data_texture_array_target_invalid"),
                    TEXT("The target must have the exact supported texture array class."), TEXT("target"),
                    ObjectClassValue(TargetObject), MakeShared<FJsonValueString>(TTarget::StaticClass()->GetPathName())));
			}
			if (!SourceRefs.bSpecified
				|| SourceRefs.Presence != TEXT("replace")
				|| !SourceRefs.CanonicalValue
				|| SourceRefs.CanonicalValue->GetKind()
					!= EUeAgentValueKind::Array
				|| SourceRefs.CanonicalValue->GetArrayItems().IsEmpty())
			{
                return Fail(OutError, ChangeFailure(TEXT("asset_data_texture_array_source_refs_invalid"),
                    Field, SourceRefs, TEXT("nonempty array of texture asset references")));
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
            for (int32 Index = 0; Index < SourceRefs.CanonicalValue->GetArrayItems().Num(); ++Index)
            {
                const FUeAgentCanonicalValueRef& RefValue = SourceRefs.CanonicalValue->GetArrayItems()[Index];
                const FString RefField = Field + TEXT("/") + FString::FromInt(Index);
				if (RefValue->GetKind()
						!= EUeAgentValueKind::String
					|| RefValue->GetText().IsEmpty())
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_array_source_ref_invalid"),
                        TEXT("The source reference must be a nonempty string."), RefField, &RefValue.Get(), TEXT("texture asset reference")));
				}
				UObject* Resolved = nullptr;
                if (!Resolver.TryResolveExisting(RefValue->GetText(), Resolved, OutError))
                {
                    if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = RefField;
                    return false;
                }
                TSource* Source = Cast<TSource>(Resolved);
                if (!Source || Source->GetClass() != TSource::StaticClass())
                    return Fail(OutError, JsonFailure(TEXT("asset_data_texture_array_source_invalid"),
                        TEXT("The source reference must resolve to the exact supported texture class."), RefField,
                        ReferenceClassValue(RefValue->GetText(), Resolved), MakeShared<FJsonValueString>(TSource::StaticClass()->GetPathName())));
                if (!Source->Source.IsValid())
                    return Fail(OutError, SourceFailure(TEXT("asset_data_texture_array_source_invalid"),
                        TEXT("The referenced texture has no valid native source data."), Source, RefField,
                        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("valid native texture source"))));
				Data->NewSourceObjects.Add(Source);
				Data->NewSourceFields.Add(RefField);
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
                return Fail(OutError, SourceFailure(TEXT("asset_data_texture_cube_array_source_invalid"),
                    !First ? TEXT("The first prepared cube source is unavailable.") : !First->Source.IsValid()
                        ? TEXT("The first cube texture has no valid source data.") : TEXT("A cube source must have one or six native slices."),
                    First, TEXT("source_cube_refs/0"),
                    !First ? ObjectClassValue(First) : !First->Source.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
                        : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(First->Source.GetNumSlices())),
                    MakeShared<FJsonValueString>(TEXT("valid TextureCube source with 1 or 6 slices"))));
			}
			const int32 SizeX = First->Source.GetSizeX();
			const int32 SizeY = First->Source.GetSizeY();
			const int32 NumSlices =
				First->Source.GetNumSlices();
			const ETextureSourceFormat Format =
				First->Source.GetFormat();
            for (int32 Index = 0; Index < Data.NewSourceObjects.Num(); ++Index)
            {
                UObject* Object = Data.NewSourceObjects[Index];
                const UTextureCube* Source = Cast<UTextureCube>(Object);
                const FString Field = TEXT("source_cube_refs/") + FString::FromInt(Index);
                if (!Source || !Source->Source.IsValid())
                    return Fail(OutError, SourceFailure(TEXT("asset_data_texture_cube_array_sources_incompatible"),
                        !Source ? TEXT("The prepared source is not an available cube texture.") : TEXT("The cube texture has no valid source data."),
                        Source, Field, !Source ? ObjectClassValue(Object) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
                        MakeShared<FJsonValueString>(TEXT("valid TextureCube source"))));
                const TCHAR* Names[] = {TEXT("width"), TEXT("height"), TEXT("slices"), TEXT("format")};
                const int32 Actual[] = {Source->Source.GetSizeX(), Source->Source.GetSizeY(), Source->Source.GetNumSlices(), static_cast<int32>(Source->Source.GetFormat())};
                const int32 Expected[] = {SizeX, SizeY, NumSlices, static_cast<int32>(Format)};
                for (int32 Component = 0; Component < 4; ++Component)
                    if (Actual[Component] != Expected[Component])
                        return Fail(OutError, SourceFailure(TEXT("asset_data_texture_cube_array_sources_incompatible"),
                            TEXT("The cube source differs from the first source in the array."), Source, Field + TEXT("/") + Names[Component],
                            MakeShared<FJsonValueNumber>(Actual[Component]), MakeShared<FJsonValueNumber>(Expected[Component])));
			}
			return true;
		}

		bool TryResolveTexture2DSources(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const TArray<const FUeAgentCanonicalValue*>& RefValues,
			FTextureSourcePreparedData& Data,
            FExecutionMutationError& OutError, const TArray<FString>& RefFields)
        {
            if (RefValues.IsEmpty())
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_refs_invalid"),
                    TEXT("At least one source texture reference is required."), TEXT("source_refs"), MakeShared<FJsonValueNumber>(0),
                    MakeShared<FJsonValueString>(TEXT("nonempty source reference list"))));
            Data.NewSourceObjects.Reserve(RefValues.Num());
            for (int32 Index = 0; Index < RefValues.Num(); ++Index)
            {
                const FUeAgentCanonicalValue* RefValue = RefValues[Index];
                const FString& RefField = RefFields[Index];
				if (!RefValue
					|| RefValue->GetKind()
						!= EUeAgentValueKind::String
					|| RefValue->GetText().IsEmpty())
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_source_ref_invalid"),
                        TEXT("The source reference must be a nonempty string."), RefField, RefValue, TEXT("Texture2D reference")));
				}
				UObject* Resolved = nullptr;
                if (!Resolver.TryResolveExisting(RefValue->GetText(), Resolved, OutError))
                {
                    if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = RefField;
                    return false;
                }
                UTexture2D* Source = Cast<UTexture2D>(Resolved);
                if (!Source || Source->GetClass() != UTexture2D::StaticClass())
                    return Fail(OutError, JsonFailure(TEXT("asset_data_texture_source_source_invalid"),
                        TEXT("The source reference must resolve to the exact Texture2D class."), RefField,
                        ReferenceClassValue(RefValue->GetText(), Resolved), MakeShared<FJsonValueString>(UTexture2D::StaticClass()->GetPathName())));
                if (!Source->Source.IsValid() || Source->Source.GetNumSlices() != 1)
                    return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_source_invalid"),
                        !Source->Source.IsValid() ? TEXT("The referenced texture has no valid source data.")
                            : TEXT("The referenced texture source must contain exactly one slice."),
                        Source, RefField, !Source->Source.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
                            : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Source->Source.GetNumSlices())),
                        MakeShared<FJsonValueString>(TEXT("valid native source with one slice"))));
				Data.NewSourceObjects.Add(Source);
				Data.NewSourceFields.Add(RefField);
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
                return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_source_invalid"),
                    !First ? TEXT("The first prepared 2D source is unavailable.") : TEXT("The first texture has no valid native source data."),
                    First, Data.SourceFieldAt(0), !First ? ObjectClassValue(First) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
                    MakeShared<FJsonValueString>(TEXT("valid Texture2D source"))));
			}
			Data.NewSourceSizeX = First->Source.GetSizeX();
			Data.NewSourceSizeY = First->Source.GetSizeY();
			Data.NewSourceFormat = First->Source.GetFormat();
			if (Data.NewSourceSizeX <= 0
				|| Data.NewSourceSizeY <= 0
				|| Data.NewSourceFormat == TSF_Invalid)
			{
                const FString Field = Data.NewSourceSizeX <= 0 ? TEXT("width") : Data.NewSourceSizeY <= 0 ? TEXT("height") : TEXT("format");
                return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_source_invalid"),
                    Data.NewSourceFormat == TSF_Invalid && Data.NewSourceSizeX > 0 && Data.NewSourceSizeY > 0
                        ? TEXT("The first texture source format is invalid.") : TEXT("The first texture source dimension must be positive."),
                    First, Data.SourceFieldAt(0) + TEXT("/") + Field,
                    MakeShared<FJsonValueNumber>(Data.NewSourceSizeX <= 0 ? Data.NewSourceSizeX : Data.NewSourceSizeY <= 0 ? Data.NewSourceSizeY : static_cast<int32>(Data.NewSourceFormat)),
                    MakeShared<FJsonValueString>(Field == TEXT("format") ? TEXT("valid texture source format") : TEXT("integer >= 1"))));
			}
			int64 ExpectedSourceBytes = -1;
            for (int32 Index = 0; Index < Data.NewSourceObjects.Num(); ++Index)
            {
                UObject* Object = Data.NewSourceObjects[Index];
                UTexture2D* Source = Cast<UTexture2D>(Object);
                const FString Field = Data.SourceFieldAt(Index);
                if (!Source || !Source->Source.IsValid())
                    return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_sources_incompatible"),
                        !Source ? TEXT("The prepared 2D source is unavailable.") : TEXT("The texture has no valid native source data."),
                        Source, Field, !Source ? ObjectClassValue(Object) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
                        MakeShared<FJsonValueString>(TEXT("valid Texture2D source"))));
                const TCHAR* Names[] = {TEXT("slices"), TEXT("width"), TEXT("height"), TEXT("format")};
                const int32 Actual[] = {Source->Source.GetNumSlices(), Source->Source.GetSizeX(), Source->Source.GetSizeY(), static_cast<int32>(Source->Source.GetFormat())};
                const int32 Expected[] = {1, Data.NewSourceSizeX, Data.NewSourceSizeY, static_cast<int32>(Data.NewSourceFormat)};
                for (int32 Component = 0; Component < 4; ++Component)
                    if (Actual[Component] != Expected[Component])
                        return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_sources_incompatible"),
                            TEXT("The source texture must have one slice and match the first source dimensions and format."),
                            Source, Field + TEXT("/") + Names[Component], MakeShared<FJsonValueNumber>(Actual[Component]),
                            MakeShared<FJsonValueNumber>(Expected[Component])));
                TArray64<uint8> SourceData;
                if (!Source->Source.GetMipData(SourceData, 0))
                    return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_source_data_invalid"),
                        TEXT("The native texture source mip could not be read."), Source, Field + TEXT("/mip/0"),
                        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("readable top mip"))));
                if (SourceData.IsEmpty() || (ExpectedSourceBytes >= 0 && SourceData.Num() != ExpectedSourceBytes))
                    return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_source_data_invalid"),
                        SourceData.IsEmpty() ? TEXT("The native source top mip contains no bytes.")
                            : TEXT("The source top mip byte count differs from the first source."),
                        Source, Field + TEXT("/mip/0/byte_count"),
                        MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), SourceData.Num())),
                        SourceData.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("nonzero byte count")))
                            : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), ExpectedSourceBytes)))));
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
            const FString Field = Kind == ETextureSourceKind::TextureCube ? TEXT("face_texture_refs") : TEXT("slice_texture_refs");
            ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty() && TargetObject)
                OutError.TargetPath = TargetObject->GetPathName(); };
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
                return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_target_invalid"),
                    TEXT("The target must have the exact supported assembled texture class."), TEXT("target"), ObjectClassValue(TargetObject),
                    MakeShared<FJsonValueString>(Kind == ETextureSourceKind::TextureCube ? UTextureCube::StaticClass()->GetPathName()
                        : UVolumeTexture::StaticClass()->GetPathName())));
			}
			if (!SourceRefs.bSpecified
				|| SourceRefs.Presence != TEXT("replace")
				|| !SourceRefs.CanonicalValue)
			{
                return Fail(OutError, ChangeFailure(TEXT("asset_data_texture_source_refs_invalid"),
                    Field, SourceRefs, Kind == ETextureSourceKind::TextureCube ? TEXT("six named face texture references") : TEXT("nonempty array of slice texture references")));
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
            TArray<FString> RefFields;
			if (Kind == ETextureSourceKind::TextureCube)
			{
				if (SourceRefs.CanonicalValue->GetKind()
						!= EUeAgentValueKind::Object
					|| SourceRefs.CanonicalValue
						->GetObjectItems().Num() != 6)
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_cube_face_refs_invalid"),
                        TEXT("Cube face references must contain exactly the six named faces."), Field, SourceRefs.CanonicalValue,
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
					const FUeAgentCanonicalValue* Value =
						FindObjectField(
							*SourceRefs.CanonicalValue,
							FaceField);
					if (!Value)
					{
                        return Fail(OutError, ValueFailure(TEXT("asset_data_texture_cube_face_refs_invalid"),
                            TEXT("A required cube face reference is missing."), Field + TEXT("/") + FaceField, nullptr, TEXT("Texture2D reference")));
					}
                    RefValues.Add(Value);
                    RefFields.Add(Field + TEXT("/") + FaceField);
				}
			}
			else
			{
				if (SourceRefs.CanonicalValue->GetKind()
						!= EUeAgentValueKind::Array
					|| SourceRefs.CanonicalValue
						->GetArrayItems().IsEmpty())
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_volume_texture_slice_refs_invalid"),
                        TEXT("Volume texture slices require a nonempty array of source references."), Field, SourceRefs.CanonicalValue,
                        TEXT("nonempty array of Texture2D references")));
				}
                for (int32 Index = 0; Index < SourceRefs.CanonicalValue->GetArrayItems().Num(); ++Index)
                {
                    RefValues.Add(&SourceRefs.CanonicalValue->GetArrayItems()[Index].Get());
                    RefFields.Add(Field + TEXT("/") + FString::FromInt(Index));
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
            if (!TryResolveTexture2DSources(Resolver, RefValues, *Data, OutError, RefFields)
                || !TryFreezeTexture2DSourceData(*Data, OutError))
			{
				return false;
			}
			if (Kind == ETextureSourceKind::TextureCube
				&& Data->NewSourceSlices != 6)
			{
                return Fail(OutError, NativeFailure(TEXT("asset_data_texture_cube_face_refs_invalid"),
                    TEXT("The prepared cube source does not contain six slices."), Field,
                    MakeShared<FJsonValueNumber>(Data->NewSourceSlices), MakeShared<FJsonValueNumber>(6)));
			}
			OutState.Data = MoveTemp(Data);
			return true;
		}

		template <typename TTarget, typename TSource>
		bool RestoreTyped(
			TTarget& Target,
			const FTextureSourcePreparedData& Data,
			const bool bRestorePackageDirty,
            FExecutionMutationError& OutError)
		{
			Target.SourceTextures.Reset(
				Data.OriginalSourceObjects.Num());
			for (int32 Index = 0; Index < Data.OriginalSourceObjects.Num(); ++Index)
			{
                UObject* Object = Data.OriginalSourceObjects[Index];
				if (Object && !IsValid(Object))
				{
                    OutError = NativeFailure(TEXT("asset_data_texture_source_rollback_reference_expired"),
                        TEXT("A captured texture source is no longer valid."), TEXT("source_refs/") + FString::FromInt(Index),
                        MakeShared<FJsonValueString>(TEXT("expired")), MakeShared<FJsonValueString>(TSource::StaticClass()->GetPathName()));
                    OutError.TargetPath = Target.GetPathName();
                    OutError.OperationIndex = Index;
                    OutError.MutationMayHaveStarted = true;
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
			const bool bRestorePackageDirty,
            FExecutionMutationError& OutError)
		{
            ON_SCOPE_EXIT { if (!OutError.IsEmpty() && Target && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target->GetPathName(); };
            if (!Data.bCaptureNativeRollback)
                return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_rollback_not_captured"),
                    TEXT("Texture source rollback has no captured native state."), TEXT("native_rollback_captured"),
                    MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			if (Data.Kind == ETextureSourceKind::Texture2DArray)
			{
				UTexture2DArray* Texture =
					Cast<UTexture2DArray>(Target);
                if (!Texture)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_rollback_target_invalid"),
                        TEXT("The rollback target does not match the captured texture type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(UTexture2DArray::StaticClass()->GetPathName())));
                return RestoreTyped<
						UTexture2DArray,
						UTexture2D>(
							*Texture,
							Data,
							bRestorePackageDirty, OutError);
			}
			if (Data.Kind
				== ETextureSourceKind::TextureCubeArray)
			{
				UTextureCubeArray* Texture =
					Cast<UTextureCubeArray>(Target);
                if (!Texture)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_rollback_target_invalid"),
                        TEXT("The rollback target does not match the captured texture type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(UTextureCubeArray::StaticClass()->GetPathName())));
                return RestoreTyped<
						UTextureCubeArray,
						UTextureCube>(
							*Texture,
							Data,
							bRestorePackageDirty, OutError);
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
                    return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_rollback_target_invalid"),
                        TEXT("Volume texture rollback requires a VolumeTexture target."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(UVolumeTexture::StaticClass()->GetPathName())));
				}
				if (Data.OriginalVolumeSource2DTexture
					&& !IsValid(
						Data.OriginalVolumeSource2DTexture))
				{
                    return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_rollback_reference_expired"),
                        TEXT("The captured volume source texture is no longer valid."), TEXT("source_2d_texture"),
                        MakeShared<FJsonValueString>(TEXT("expired")), MakeShared<FJsonValueString>(UTexture2D::StaticClass()->GetPathName())));
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
                auto Error = NativeFailure(TEXT("asset_data_texture_source_rollback_target_invalid"),
                    TEXT("The captured texture kind cannot be restored to the current target."), TEXT("target"),
                    ObjectClassValue(Target), MakeShared<FJsonValueString>(TEXT("target matching captured texture kind")));
                Error.InternalContext = FString::Printf(TEXT("texture_kind=%d"), static_cast<int32>(Data.Kind));
                return Fail(OutError, MoveTemp(Error));
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

        void RestoreAfterFailure(UObject* Target, const FTextureSourcePreparedData& Data,
            const bool bMutationAlreadyStarted, FExecutionMutationError& OutError)
        {
            FExecutionMutationError RollbackError;
            OutError.RollbackAttempted = true;
            const bool bRestored = Restore(Target, Data, true, RollbackError);
            OutError.RollbackSucceeded = bRestored;
            const bool bRestoreStarted = bRestored
                || (RollbackError.MutationMayHaveStarted.IsSet() && RollbackError.MutationMayHaveStarted.GetValue());
            OutError.MutationMayHaveStarted = bMutationAlreadyStarted || bRestoreStarted;
            if (!bRestored)
            {
                RollbackError.Stage = TEXT("rollback");
                OutError.Diagnostics.Add(MoveTemp(RollbackError));
            }
        }


		template <typename TTarget, typename TSource>
		bool ApplyTyped(
			TTarget& Target,
			const FTextureSourcePreparedData& Data,
			FExecutionMutationError& OutError)
		{
			Target.SourceTextures.Reset(
				Data.NewSourceObjects.Num());
			for (int32 Index = 0; Index < Data.NewSourceObjects.Num(); ++Index)
			{
                UObject* Object = Data.NewSourceObjects[Index];
				TSource* Source = Cast<TSource>(Object);
				if (!Source || !IsValid(Source))
				{
                    OutError = NativeFailure(TEXT("asset_data_texture_array_resolved_source_expired"),
                        TEXT("A prepared texture array source is unavailable or has an incompatible type."),
                        Data.SourceFieldAt(Index),
                        IsValid(Object) ? ObjectClassValue(Object) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("unavailable"))),
                        MakeShared<FJsonValueString>(TSource::StaticClass()->GetPathName()));
                    OutError.TargetPath = Target.GetPathName();
                    OutError.OperationIndex = Index;
                    RestoreAfterFailure(&Target, Data, true, OutError);
                    return false;
				}
				Target.SourceTextures.Add(Source);
			}
			Target.bSourceGeneratedFromSourceTexturesArray = true;
			if (!Target.UpdateSourceFromSourceTextures(false))
			{
                OutError = NativeFailure(TEXT("asset_data_texture_array_native_source_update_failed"),
                    TEXT("Unreal Engine could not rebuild the texture array from its prepared source textures."),
                    TEXT("source_updated"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
                OutError.TargetPath = Target.GetPathName();
                RestoreAfterFailure(&Target, Data, true, OutError);
                return false;
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
			for (int32 Index = 0; Index < Data.NewSourceObjects.Num(); ++Index)
			{
                UObject* Object = Data.NewSourceObjects[Index];
				UTexture2D* Source = Cast<UTexture2D>(Object);
				if (!Source
					|| Source->GetClass()
						!= UTexture2D::StaticClass()
					|| !IsValid(Source))
				{
                    OutError = NativeFailure(TEXT("asset_data_texture_source_resolved_source_expired"),
                        TEXT("A prepared source must remain a live Texture2D with the exact supported native class."),
                        Data.SourceFieldAt(Index),
                        IsValid(Object) ? ObjectClassValue(Object) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                        MakeShared<FJsonValueString>(UTexture2D::StaticClass()->GetPathName()));
                    OutError.TargetPath = Target.GetPathName();
                    OutError.OperationIndex = Index;
                    RestoreAfterFailure(&Target, Data, false, OutError);
                    return false;
				}
			}
			if (Data.NewSourceSizeX <= 0
				|| Data.NewSourceSizeY <= 0
				|| Data.NewSourceSlices <= 0
				|| Data.NewSourceFormat == TSF_Invalid
				|| Data.NewSourceTopMipData.IsEmpty())
			{
                const FString Field = Data.NewSourceSizeX <= 0 ? TEXT("width") : Data.NewSourceSizeY <= 0 ? TEXT("height")
                    : Data.NewSourceSlices <= 0 ? TEXT("slices") : Data.NewSourceFormat == TSF_Invalid ? TEXT("format") : TEXT("mip/0/byte_count");
                const int64 Actual = Data.NewSourceSizeX <= 0 ? Data.NewSourceSizeX : Data.NewSourceSizeY <= 0 ? Data.NewSourceSizeY
                    : Data.NewSourceSlices <= 0 ? Data.NewSourceSlices : Data.NewSourceFormat == TSF_Invalid ? static_cast<int32>(Data.NewSourceFormat) : Data.NewSourceTopMipData.Num();
                return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_prepared_data_invalid"),
                    TEXT("The prepared texture source is incomplete or invalid."), &Target, Field,
                    MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), Actual)),
                    MakeShared<FJsonValueString>(Field == TEXT("format") ? TEXT("valid texture source format") : TEXT("positive value"))));
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
			if (Data.bApplyPreparedFormatSettings)
			{
				Target.SRGB = Data.NewSettings.bSRGB;
				Target.CompressionSettings = Data.NewSettings.Compression;
				Target.MipGenSettings = Data.NewSettings.MipGeneration;
			}
			Target.ValidateSettingsAfterImportOrEdit();
			Target.SetLightingGuid();
			Target.PostEditChange();
			return true;
		}
	}

	bool TryVerifyTextureSourceAssembly(
		UObject* Target,
		const FTextureSourcePreparedState& State,
		FExecutionMutationError& OutError)
	{
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty() && Target)
            OutError.TargetPath = Target->GetPathName(); };
        if (!State.Data.IsValid())
            return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_state_invalid"),
                TEXT("Source verification has no prepared assembly state."), TEXT("prepared_state"), MakeShared<FJsonValueBoolean>(false),
                MakeShared<FJsonValueString>(TEXT("available prepared assembly state"))));
        const FTextureSourcePreparedData& Data = *State.Data;
        UClass* ExpectedClass = Data.Kind == ETextureSourceKind::TextureCube ? UTextureCube::StaticClass()
            : Data.Kind == ETextureSourceKind::VolumeTexture ? UVolumeTexture::StaticClass() : nullptr;
        if (!ExpectedClass || !IsValid(Target) || Target->GetClass() != ExpectedClass)
            return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_target_invalid"),
                TEXT("Source verification requires a live target with the prepared assembly class."), TEXT("target"),
                IsValid(Target) ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                MakeShared<FJsonValueString>(ExpectedClass ? ExpectedClass->GetPathName() : TEXT("TextureCube or VolumeTexture"))));
        FTextureSource& Source = CastChecked<UTexture>(Target)->Source;
        if (!Source.IsValid())
            return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_intent_mismatch"),
                TEXT("The assembled texture has no valid native source data."), TEXT("source"),
                MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
        const TCHAR* Names[] = {TEXT("width"), TEXT("height"), TEXT("slices"), TEXT("format"), TEXT("layers"), TEXT("blocks")};
        const int32 Actual[] = {Source.GetSizeX(), Source.GetSizeY(), Source.GetNumSlices(), static_cast<int32>(Source.GetFormat()), Source.GetNumLayers(), Source.GetNumBlocks()};
        const int32 Expected[] = {Data.NewSourceSizeX, Data.NewSourceSizeY, Data.NewSourceSlices, static_cast<int32>(Data.NewSourceFormat), 1, 1};
        for (int32 Index = 0; Index < 6; ++Index)
            if (Actual[Index] != Expected[Index])
                return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_intent_mismatch"),
                    TEXT("The assembled source differs from the prepared texture shape."), FString(TEXT("source/")) + Names[Index],
                    MakeShared<FJsonValueNumber>(Actual[Index]), MakeShared<FJsonValueNumber>(Expected[Index])));
        TArray64<uint8> ActualPixels;
        if (!Source.GetMipData(ActualPixels, 0))
            return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_readback_failed"),
                TEXT("The assembled texture top mip could not be read back."), TEXT("source/mip/0"),
                MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("readable top mip"))));
        if (ActualPixels != Data.NewSourceTopMipData)
        {
            if (ActualPixels.Num() != Data.NewSourceTopMipData.Num())
                return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_intent_mismatch"),
                    TEXT("The assembled top mip byte count differs from the prepared data."), TEXT("source/mip/0/byte_count"),
                    MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), ActualPixels.Num())),
                    MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), Data.NewSourceTopMipData.Num()))));
            int64 Difference = 0;
            while (Difference < ActualPixels.Num() && ActualPixels[Difference] == Data.NewSourceTopMipData[Difference]) ++Difference;
            return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_intent_mismatch"),
                TEXT("The assembled top mip differs from the prepared data at this byte offset."),
                TEXT("source/mip/0/bytes/") + FString::Printf(TEXT("%lld"), Difference),
                MakeShared<FJsonValueNumber>(ActualPixels[Difference]), MakeShared<FJsonValueNumber>(Data.NewSourceTopMipData[Difference])));
        }
		return true;
	}

	bool TryPrepareResolvedTextureSourceAssembly(
		UTexture& Target,
		const TArray<UTexture2D*>& OrderedSources,
		FTextureSourcePreparedState& OutState,
		FExecutionMutationError& OutError)
	{
        OutState.Data.Reset();
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.GetPathName(); };
        const bool bCube = Target.GetClass() == UTextureCube::StaticClass();
        const bool bVolume = Target.GetClass() == UVolumeTexture::StaticClass();
        if (!bCube && !bVolume)
            return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_target_invalid"),
                TEXT("Resolved source assembly requires an exact TextureCube or VolumeTexture target."), TEXT("target"),
                ObjectClassValue(&Target), MakeShared<FJsonValueString>(TEXT("TextureCube or VolumeTexture"))));
        if ((bCube && OrderedSources.Num() != 6) || (bVolume && OrderedSources.IsEmpty()))
            return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_refs_invalid"),
                bCube ? TEXT("Cube assembly requires six resolved face textures.") : TEXT("Volume assembly requires at least one resolved slice texture."),
                TEXT("source_refs"), MakeShared<FJsonValueNumber>(OrderedSources.Num()),
                MakeShared<FJsonValueString>(bCube ? TEXT("6 resolved textures") : TEXT("at least 1 resolved texture"))));
		auto Data = MakeShared<FTextureSourcePreparedData, ESPMode::ThreadSafe>(
			bCube ? ETextureSourceKind::TextureCube : ETextureSourceKind::VolumeTexture, Target, false);
		// Reflection applies properties before generation on its preview, and after
		// generation on the actual update target. Never replay captured old settings.
		Data->bApplyPreparedFormatSettings = false;
        for (int32 Index = 0; Index < OrderedSources.Num(); ++Index)
        {
            UTexture2D* Source = OrderedSources[Index];
            const FString Field = TEXT("source_refs/") + FString::FromInt(Index);
            if (!IsValid(Source) || Source->GetClass() != UTexture2D::StaticClass())
                return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_source_invalid"),
                    TEXT("The resolved source must remain a live texture with the exact Texture2D class."), Field,
                    IsValid(Source) ? ObjectClassValue(Source) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                    MakeShared<FJsonValueString>(UTexture2D::StaticClass()->GetPathName())));
            if (!Source->Source.IsValid() || Source->Source.GetNumSlices() != 1)
                return Fail(OutError, SourceFailure(TEXT("asset_data_texture_source_source_invalid"),
                    !Source->Source.IsValid() ? TEXT("The resolved texture has no valid native source data.")
                        : TEXT("The resolved 2D source must contain exactly one slice."), Source, Field,
                    !Source->Source.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
                        : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Source->Source.GetNumSlices())),
                    MakeShared<FJsonValueString>(TEXT("valid native source with one slice"))));
			Data->NewSourceObjects.Add(Source);
			Pin(Data->Pins, Source);
		}
		if (!TryFreezeTexture2DSourceData(*Data, OutError)) return false;
		if (bCube && Data->NewSourceSizeX != Data->NewSourceSizeY)
            return Fail(OutError, NativeFailure(TEXT("asset_data_texture_cube_faces_not_square"),
                TEXT("Cube face textures must have equal width and height."), TEXT("source_refs/0/height"),
                MakeShared<FJsonValueNumber>(Data->NewSourceSizeY), MakeShared<FJsonValueNumber>(Data->NewSourceSizeX)));
		OutState.Data = MoveTemp(Data);
		return true;
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
        if (!OutState.Data.IsValid())
            return Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_state_invalid"),
                TEXT("Texture cube array preparation produced no assembly state."), TEXT("prepared_state"),
                MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
        if (!ValidateCubeArraySources(*OutState.Data, OutError))
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
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty() && Target)
            OutError.TargetPath = Target->GetPathName(); };
        if (!Target || !State.Data.IsValid())
            return Fail(OutError, NativeFailure(TEXT("asset_data_texture_array_state_invalid"),
                !Target ? TEXT("The texture replacement target is unavailable.") : TEXT("The prepared texture replacement state is unavailable."),
                !Target ? TEXT("target") : TEXT("prepared_state"),
                !Target ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
                MakeShared<FJsonValueString>(!Target ? TEXT("live texture target") : TEXT("available prepared replacement state"))));
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
                : Fail(OutError, NativeFailure(TEXT("asset_data_texture_array_target_invalid"),
                    TEXT("The prepared replacement requires a Texture2DArray target."), TEXT("target"), ObjectClassValue(Target),
                    MakeShared<FJsonValueString>(UTexture2DArray::StaticClass()->GetPathName())));
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
                : Fail(OutError, NativeFailure(TEXT("asset_data_texture_array_target_invalid"),
                    TEXT("The prepared replacement requires a TextureCubeArray target."), TEXT("target"), ObjectClassValue(Target),
                    MakeShared<FJsonValueString>(UTextureCubeArray::StaticClass()->GetPathName())));
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
			? (ApplyAssembledTexture(
				*Texture,
				*State.Data,
				OutError) && TryVerifyTextureSourceAssembly(Texture, State, OutError))
            : Fail(OutError, NativeFailure(TEXT("asset_data_texture_source_target_invalid"),
                TEXT("The target class does not match the prepared texture assembly."), TEXT("target"), ObjectClassValue(Target),
                MakeShared<FJsonValueString>(State.Data->Kind == ETextureSourceKind::TextureCube
                    ? UTextureCube::StaticClass()->GetPathName() : UVolumeTexture::StaticClass()->GetPathName())));
	}

	bool TryRollbackTextureSourceReplacement(
        UObject* Target, const FTextureSourcePreparedState& State, FExecutionMutationError& OutError)
    {
        if (!Target || !State.Data.IsValid())
        {
            OutError = NativeFailure(TEXT("asset_data_texture_source_rollback_state_invalid"),
                !Target ? TEXT("The texture rollback target is unavailable.") : TEXT("Texture rollback has no prepared state."),
                !Target ? TEXT("target") : TEXT("prepared_state"),
                !Target ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
                MakeShared<FJsonValueString>(!Target ? TEXT("live texture target") : TEXT("captured rollback state")));
            if (Target) OutError.TargetPath = Target->GetPathName();
            return false;
        }
        return Restore(Target, *State.Data, false, OutError);
    }
}
