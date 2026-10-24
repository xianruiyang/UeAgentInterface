#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCoreAssetMechanics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringPropertyValues.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentDataObjectEligibility.h"
#include "Dom/JsonObject.h"

#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "FileMediaSource.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundEffectSubmix.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundSubmix.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "SubmixEffects/AudioMixerSubmixEffectEQ.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "VT/RuntimeVirtualTexture.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	using namespace ExecutionMutation;
	using namespace ContextResource;
	using namespace StateSemantic;

	namespace
	{

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


        FString ChildPath(const FString& Parent, const FString& Name)
        {
            return Parent + TEXT("/") + Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
        }

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

		enum class ECoreAssetKind : uint8
		{
			AudioEffectPreset,
			SoundAttenuation,
			SoundClass,
			SoundConcurrency,
			SoundMix,
			SoundSubmix,
			Curve,
			DataObject,
			MediaSource,
			RuntimeVirtualTexture,
			SparseVolumeTexture,
		};

		const ECoreAssetKind* FindCoreAssetKind(
			const FString& NativeMechanicId)
		{
			static const TMap<FString, ECoreAssetKind> Kinds{
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_audio_effect_preset:v1"),
					ECoreAssetKind::AudioEffectPreset,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_sound_attenuation:v1"),
					ECoreAssetKind::SoundAttenuation,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_sound_class:v1"),
					ECoreAssetKind::SoundClass,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_sound_concurrency:v1"),
					ECoreAssetKind::SoundConcurrency,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_sound_mix:v1"),
					ECoreAssetKind::SoundMix,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_sound_submix:v1"),
					ECoreAssetKind::SoundSubmix,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"replace_curve:v1"),
					ECoreAssetKind::Curve,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_data_object:v1"),
					ECoreAssetKind::DataObject,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_media_source:v1"),
					ECoreAssetKind::MediaSource,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_runtime_virtual_texture:v1"),
					ECoreAssetKind::RuntimeVirtualTexture,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_sparse_volume_texture:v1"),
					ECoreAssetKind::SparseVolumeTexture,
				},
			};
			return Kinds.Find(NativeMechanicId);
		}

		bool Fail(FExecutionMutationError& OutError, FExecutionMutationError Diagnostic,
            const FString& PropertyPath = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = PropertyPath;
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

		const FUeAgentCanonicalValueRef* FindObjectFieldRef(
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
					return &Item.Value;
				}
			}
			return nullptr;
		}

		const FCoreAssetChangeView* FindChange(
			const TArray<FCoreAssetChangeView>& Changes,
			const FString& FieldId)
		{
			for (const FCoreAssetChangeView& Change : Changes)
			{
				if (Change.FieldId == FieldId)
				{
					return &Change;
				}
			}
			return nullptr;
		}

        TSharedPtr<FJsonValue> ChangeFieldsValue(const TArray<FCoreAssetChangeView>& Changes)
        {
            TArray<TSharedPtr<FJsonValue>> Fields;
            for (const auto& Change : Changes) Fields.Add(MakeShared<FJsonValueString>(Change.FieldId));
            return MakeShared<FJsonValueArray>(Fields);
        }

        bool TryRequireChangeValue(const FCoreAssetChangeView& Change, FExecutionMutationError& OutError)
        {
            if (Change.CanonicalValue) return true;
            return Fail(OutError, ValueFailure(TEXT("asset_data_core_change_value_missing"),
                TEXT("The field change requires a value."), Change.FieldId, nullptr, TEXT("present value")));
        }

        bool TryRequirePresence(const FCoreAssetChangeView& Change,
            std::initializer_list<const TCHAR*> Allowed, FExecutionMutationError& OutError)
        {
            TArray<TSharedPtr<FJsonValue>> Expected;
            for (const TCHAR* Name : Allowed)
            {
                if (Change.Presence == Name) return true;
                Expected.Add(MakeShared<FJsonValueString>(Name));
            }
            return Fail(OutError, JsonFailure(TEXT("asset_data_core_presence_invalid"),
                TEXT("The field change has an unsupported presence."), Change.FieldId,
                MakeShared<FJsonValueString>(Change.Presence), MakeShared<FJsonValueArray>(Expected)));
        }

        FExecutionMutationError ChangeSetFailure(const TArray<FCoreAssetChangeView>& Changes,
            const FString& Expected)
        {
            return JsonFailure(TEXT("asset_data_core_changes_invalid"),
                TEXT("The submitted change fields do not match the supported change set."),
                TEXT("changes"), ChangeFieldsValue(Changes), MakeShared<FJsonValueString>(Expected));
        }

        bool TryReadRequiredString(const FUeAgentCanonicalValue* Value, FString& Out,
            const FString& Field, FExecutionMutationError& OutError,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            if (!Value || Value->GetKind() != EUeAgentValueKind::String)
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_string_invalid"),
                    Value ? TEXT("A string is required.") : TEXT("The required string is missing."),
                    Field, Value, TEXT("string"), SourceKind));
            Out = Value->GetText();
            return true;
        }

        void QualifyCoreValueFailure(FExecutionMutationError& Error, const FString& Field, const bool bNative)
        {
            if (bNative)
            {
                Error.SourceKind = TEXT("native_result");
                if (Error.Source.InternalJsonPath.IsEmpty())
                    Error.Source.InternalJsonPath = Error.SemanticPointer.IsEmpty() ? Error.Source.JsonPath : Error.SemanticPointer;
                static_cast<Contract::FPublicSourceLocation&>(Error.Source) = {};
                Error.SemanticPointer.Reset();
            }
            if (!Field.IsEmpty()) Error.PropertyPath = Error.PropertyPath.IsEmpty() ? Field : Field + TEXT("/") + Error.PropertyPath;
            for (auto& Child : Error.Diagnostics) QualifyCoreValueFailure(Child, Field, bNative);
            if (Error.PublicCause)
            {
                auto Cause = MakeShared<FExecutionMutationError>(*Error.PublicCause);
                QualifyCoreValueFailure(*Cause, Field, bNative);
                Error.PublicCause = MoveTemp(Cause);
            }
        }

        bool FailRegisteredValue(FExecutionMutationError& OutError, const FString& Field, const bool bNative)
        {
            if (!bNative) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/") + Field);
            QualifyCoreValueFailure(OutError, Field, bNative);
            return false;
        }

		bool HasExactObjectFields(
			const FUeAgentCanonicalValue& Object,
			std::initializer_list<const TCHAR*> Names)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object
				|| Object.GetObjectItems().Num()
					!= static_cast<int32>(Names.size()))
			{
				return false;
			}
			for (const TCHAR* Name : Names)
			{
				if (!FindObjectField(Object, Name))
				{
					return false;
				}
			}
			return true;
		}

        bool TryRequireObjectFields(const FUeAgentCanonicalValue& Value,
            std::initializer_list<const TCHAR*> Names, const FString& Field, FExecutionMutationError& OutError)
        {
            if (HasExactObjectFields(Value, Names)) return true;
            if (Value.GetKind() != EUeAgentValueKind::Object)
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_object_invalid"),
                    TEXT("An object with the required fields is required."), Field, &Value, TEXT("object")));
            TArray<TSharedPtr<FJsonValue>> ActualNames;
            TArray<TSharedPtr<FJsonValue>> ExpectedNames;
            for (const auto& Item : Value.GetObjectItems()) ActualNames.Add(MakeShared<FJsonValueString>(Item.Key));
            for (const TCHAR* Name : Names) ExpectedNames.Add(MakeShared<FJsonValueString>(Name));
            for (const TCHAR* Name : Names)
                if (!FindObjectField(Value, Name))
                    return Fail(OutError, JsonFailure(TEXT("asset_data_core_field_missing"),
                        TEXT("A required object field is missing."), ChildPath(Field, Name), nullptr,
                        MakeShared<FJsonValueString>(TEXT("present field"))));
            return Fail(OutError, JsonFailure(TEXT("asset_data_core_object_fields_invalid"),
                TEXT("The object must contain exactly the supported fields."), Field,
                MakeShared<FJsonValueArray>(ActualNames), MakeShared<FJsonValueArray>(ExpectedNames)));
        }

        bool TryReadNumber(const FUeAgentCanonicalValue& Value, double& OutNumber,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::Integer && Value.GetKind() != EUeAgentValueKind::Decimal)
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_number_invalid"),
                    TEXT("A numeric value is required."), Field, &Value, TEXT("finite number")));
            if (!TryParseFiniteDecimal(Value.GetLexeme(), OutNumber))
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_number_invalid"),
                    TEXT("The number cannot be represented as a finite native value."), Field, &Value, TEXT("finite number")));
            return true;
        }

        bool TryReadFloat(const FUeAgentCanonicalValue& Value, float& OutValue,
            const double MinValue, const double MaxValue, const FString& Field, FExecutionMutationError& OutError)
        {
            double Number = 0.0;
            if (!TryReadNumber(Value, Number, Field, OutError)) return false;
            if (Number < MinValue || Number > MaxValue)
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_float_range_invalid"),
                    TEXT("The number is outside the supported range."), Field, &Value,
                    FString::Printf(TEXT("%g <= value <= %g"), MinValue, MaxValue)));
            if (UeAgentPropertyValueCodec::TryConvertFloat32(Number, OutValue, OutError)) return true;
            OutError.PropertyPath = Field;
            return false;
        }

        bool TryReadInt32(const FUeAgentCanonicalValue& Value, int32& OutValue,
            const int32 MinValue, const int32 MaxValue, const FString& Field, FExecutionMutationError& OutError)
        {
            int64 Number = 0;
            if ((Value.GetKind() != EUeAgentValueKind::Integer && Value.GetKind() != EUeAgentValueKind::Decimal)
                || !TryParseInt64Number(Value.GetLexeme(), Number) || Number < MinValue || Number > MaxValue)
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_integer_invalid"),
                    TEXT("The value must be an integer within the supported range."), Field, &Value,
                    FString::Printf(TEXT("integer in [%d, %d]"), MinValue, MaxValue)));
            OutValue = static_cast<int32>(Number);
            return true;
        }

        bool TryReadBool(const FUeAgentCanonicalValue& Value, bool& bOutValue,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::Boolean)
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_boolean_invalid"),
                    TEXT("A boolean is required."), Field, &Value, TEXT("boolean")));
            bOutValue = Value.GetBoolean();
            return true;
        }

        bool TryReadString(const FUeAgentCanonicalValue& Value, FString& OutValue,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::String)
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_string_invalid"),
                    TEXT("A string is required."), Field, &Value, TEXT("string")));
            OutValue = Value.GetText();
            return true;
        }

        template <typename TObjectType>
        bool TryResolveObject(IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
            const FUeAgentCanonicalValue& Value, TObjectType*& OutObject,
            FExecutionMutationError& OutError, const FString& Field)
        {
            OutObject = nullptr;
            if (Value.GetKind() != EUeAgentValueKind::String)
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_reference_invalid"),
                    TEXT("The asset reference must be a string."), Field, &Value, TObjectType::StaticClass()->GetPathName()));
            UObject* Resolved = nullptr;
            if (!Resolver.TryResolveExisting(Value.GetText(), Resolved, OutError))
            {
                if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Field;
                return false;
            }
            OutObject = Cast<TObjectType>(Resolved);
            if (!OutObject)
            {
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetStringField(TEXT("reference"), Value.GetText());
                Actual->SetField(TEXT("resolved_class"), ObjectClassValue(Resolved));
                return Fail(OutError, JsonFailure(TEXT("asset_data_core_reference_type_invalid"),
                    TEXT("The reference did not resolve to the required asset class."), Field,
                    MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TObjectType::StaticClass()->GetPathName())));
            }
            return true;
        }

        template <typename TObjectType>
        bool TryResolveUniqueArray(IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
            const FUeAgentCanonicalValue& Value, TArray<TObjectType*>& OutObjects,
            FExecutionMutationError& OutError, const FString& Field)
        {
            OutObjects.Reset();
            if (Value.GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_reference_array_invalid"),
                    TEXT("Asset references must be an array."), Field, &Value, TEXT("array of unique asset references")));
            for (int32 Index = 0; Index < Value.GetArrayItems().Num(); ++Index)
            {
                const auto& Item = Value.GetArrayItems()[Index];
                const FString Path = Field + TEXT("/") + FString::FromInt(Index);
                TObjectType* Object = nullptr;
                if (!TryResolveObject(Resolver, *Item, Object, OutError, Path)) return false;
                if (OutObjects.Contains(Object))
                    return Fail(OutError, ValueFailure(TEXT("asset_data_core_reference_duplicate"),
                        TEXT("The reference resolves to an asset already present in this array."), Path, &Item.Get(), TEXT("unique resolved asset")));
                OutObjects.Add(Object);
            }
            return true;
        }

		void PinUnique(
			TArray<TStrongObjectPtr<UObject>>& Pins,
			UObject* Object)
		{
			if (!Object)
			{
				return;
			}
			for (const TStrongObjectPtr<UObject>& Pin : Pins)
			{
				if (Pin.Get() == Object)
				{
					return;
				}
			}
			Pins.Emplace(Object);
		}

		struct FPackageDirtySnapshot final
		{
			TWeakObjectPtr<UPackage> Package;
			bool bWasDirty = false;
		};

		void CapturePackage(
			TArray<FPackageDirtySnapshot>& Snapshots,
			UObject* Object)
		{
			UPackage* Package = Object ? Object->GetOutermost() : nullptr;
			if (!Package)
			{
				return;
			}
			for (const FPackageDirtySnapshot& Snapshot : Snapshots)
			{
				if (Snapshot.Package.Get() == Package)
				{
					return;
				}
			}
			FPackageDirtySnapshot Snapshot;
			Snapshot.Package = Package;
			Snapshot.bWasDirty = Package->IsDirty();
			Snapshots.Add(Snapshot);
		}

		void RestorePackageFlags(
			const TArray<FPackageDirtySnapshot>& Snapshots)
		{
			for (const FPackageDirtySnapshot& Snapshot : Snapshots)
			{
				if (UPackage* Package = Snapshot.Package.Get())
				{
					Package->SetDirtyFlag(Snapshot.bWasDirty);
				}
			}
		}

		bool TryParseEqSettings(
			const FUeAgentCanonicalValue& Value,
			FSubmixEffectSubmixEQSettings& OutSettings, const FString& Field, FExecutionMutationError& OutError)
		{
			if (!TryRequireObjectFields(Value, {TEXT("eq_bands")}, Field, OutError))
			{
				return false;
			}
			const FUeAgentCanonicalValue* Bands =
				FindObjectField(Value, TEXT("eq_bands"));
			if (!Bands
				|| Bands->GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(OutError, ValueFailure(TEXT("asset_data_core_eq_bands_invalid"), TEXT("EQ bands must be an array."), ChildPath(Field, TEXT("eq_bands")), Bands, TEXT("array")));
			}
			OutSettings.EQBands.Reset();
			for (const FUeAgentCanonicalValueRef& Item
				: Bands->GetArrayItems())
			{
				const FUeAgentCanonicalValue& BandValue = Item.Get();
                const FString BandPath = ChildPath(Field, TEXT("eq_bands")) + TEXT("/") + FString::FromInt(OutSettings.EQBands.Num());
				if (!TryRequireObjectFields(
						BandValue,
						{
							TEXT("frequency_hz"),
							TEXT("bandwidth_octaves"),
							TEXT("gain_db"),
							TEXT("enabled"),
						}, BandPath, OutError))
				{
					return false;
				}
				FSubmixEffectEQBand Band;
				bool bEnabled = false;
				if (!TryReadFloat(
						*FindObjectField(
							BandValue,
							TEXT("frequency_hz")),
						Band.Frequency,
						20.0,
						20000.0, ChildPath(BandPath, TEXT("frequency_hz")), OutError)
					|| !TryReadFloat(
						*FindObjectField(
							BandValue,
							TEXT("bandwidth_octaves")),
						Band.Bandwidth,
						0.1,
						2.0, ChildPath(BandPath, TEXT("bandwidth_octaves")), OutError)
					|| !TryReadFloat(
						*FindObjectField(
							BandValue,
							TEXT("gain_db")),
						Band.GainDb,
						-90.0,
						20.0, ChildPath(BandPath, TEXT("gain_db")), OutError)
					|| !TryReadBool(
						*FindObjectField(
							BandValue,
							TEXT("enabled")),
						bEnabled, ChildPath(BandPath, TEXT("enabled")), OutError))
				{
					return false;
				}
				Band.bEnabled = bEnabled;
				OutSettings.EQBands.Add(Band);
			}
			return true;
		}

		bool TryParseDistanceAlgorithm(
			const FString& Name,
			EAttenuationDistanceModel& OutValue, const FString& Field, FExecutionMutationError& OutError)
		{
			const FString Normalized =
				Name.TrimStartAndEnd().ToLower();
			if (Normalized == TEXT("linear"))
			{
				OutValue = EAttenuationDistanceModel::Linear;
			}
			else if (Normalized == TEXT("logarithmic"))
			{
				OutValue = EAttenuationDistanceModel::Logarithmic;
			}
			else if (Normalized == TEXT("inverse"))
			{
				OutValue = EAttenuationDistanceModel::Inverse;
			}
			else if (Normalized == TEXT("log_reverse"))
			{
				OutValue = EAttenuationDistanceModel::LogReverse;
			}
			else if (Normalized == TEXT("natural_sound"))
			{
				OutValue = EAttenuationDistanceModel::NaturalSound;
			}
			else if (Normalized == TEXT("custom"))
			{
				OutValue = EAttenuationDistanceModel::Custom;
			}
			else
			{
				return Fail(OutError, JsonFailure(TEXT("asset_data_core_enum_invalid"), TEXT("EAttenuationDistanceModel name is not supported."), Field, MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("linear, logarithmic, inverse, log_reverse, natural_sound, custom"))));
			}
			return true;
		}

		bool TryParseAttenuationSettings(
			const FUeAgentCanonicalValue& Value,
			FSoundAttenuationSettings& OutSettings, const FString& Field, FExecutionMutationError& OutError)
		{
			if (!TryRequireObjectFields(
					Value,
					{
						TEXT("attenuate"),
						TEXT("spatialize"),
						TEXT("distance_algorithm"),
						TEXT("attenuation_shape"),
						TEXT("inner_radius"),
						TEXT("falloff_distance"),
						TEXT("spatialization_algorithm"),
						TEXT("enable_occlusion"),
						TEXT("occlusion_low_pass_filter_frequency"),
						TEXT("occlusion_volume_attenuation"),
						TEXT("occlusion_interpolation_time"),
					}, Field, OutError))
			{
				return false;
			}
			bool bAttenuate = false;
			bool bSpatialize = false;
			bool bEnableOcclusion = false;
			FString DistanceName;
			FString ShapeName;
			FString SpatializationName;
			float InnerRadius = 0.0f;
			if (!TryReadBool(
					*FindObjectField(Value, TEXT("attenuate")),
					bAttenuate, ChildPath(Field, TEXT("attenuate")), OutError)
				|| !TryReadBool(
					*FindObjectField(Value, TEXT("spatialize")),
					bSpatialize, ChildPath(Field, TEXT("spatialize")), OutError)
				|| !TryReadString(
					*FindObjectField(
						Value,
						TEXT("distance_algorithm")),
					DistanceName, ChildPath(Field, TEXT("distance_algorithm")), OutError)
				|| !TryParseDistanceAlgorithm(
					DistanceName,
					OutSettings.DistanceAlgorithm, ChildPath(Field, TEXT("distance_algorithm")), OutError)
				|| !TryReadString(
					*FindObjectField(
						Value,
						TEXT("attenuation_shape")),
					ShapeName, ChildPath(Field, TEXT("attenuation_shape")), OutError)) return false;
            if (ShapeName.TrimStartAndEnd().ToLower() != TEXT("sphere"))
                return Fail(OutError, JsonFailure(TEXT("asset_data_core_enum_invalid"), TEXT("EAttenuationShape name is not supported."), ChildPath(Field, TEXT("attenuation_shape")), MakeShared<FJsonValueString>(ShapeName), MakeShared<FJsonValueString>(TEXT("sphere"))));
            if (!TryReadFloat(
					*FindObjectField(Value, TEXT("inner_radius")),
					InnerRadius,
					0.0,
					1000000000.0, ChildPath(Field, TEXT("inner_radius")), OutError)
				|| !TryReadFloat(
					*FindObjectField(
						Value,
						TEXT("falloff_distance")),
					OutSettings.FalloffDistance,
					0.0,
					1000000000.0, ChildPath(Field, TEXT("falloff_distance")), OutError)
				|| !TryReadString(
					*FindObjectField(
						Value,
						TEXT("spatialization_algorithm")),
					SpatializationName, ChildPath(Field, TEXT("spatialization_algorithm")), OutError)
				|| !TryReadBool(
					*FindObjectField(
						Value,
						TEXT("enable_occlusion")),
					bEnableOcclusion, ChildPath(Field, TEXT("enable_occlusion")), OutError)
				|| !TryReadFloat(
					*FindObjectField(
						Value,
						TEXT(
							"occlusion_low_pass_filter_frequency")),
					OutSettings.OcclusionLowPassFilterFrequency,
					0.0,
					20000.0, ChildPath(Field, TEXT("occlusion_low_pass_filter_frequency")), OutError)
				|| !TryReadFloat(
					*FindObjectField(
						Value,
						TEXT("occlusion_volume_attenuation")),
					OutSettings.OcclusionVolumeAttenuation,
					0.0,
					1.0, ChildPath(Field, TEXT("occlusion_volume_attenuation")), OutError)
				|| !TryReadFloat(
					*FindObjectField(
						Value,
						TEXT("occlusion_interpolation_time")),
					OutSettings.OcclusionInterpolationTime,
					0.0,
					1000000.0, ChildPath(Field, TEXT("occlusion_interpolation_time")), OutError))
			{
				return false;
			}
			const FString Spatialization =
				SpatializationName.TrimStartAndEnd().ToLower();
			if (Spatialization == TEXT("panning"))
			{
				OutSettings.SpatializationAlgorithm =
					SPATIALIZATION_Default;
			}
			else if (Spatialization == TEXT("binaural"))
			{
				OutSettings.SpatializationAlgorithm =
					SPATIALIZATION_HRTF;
			}
			else
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_core_enum_invalid"), TEXT("ESoundSpatializationAlgorithm name is not supported."), ChildPath(Field, TEXT("spatialization_algorithm")), MakeShared<FJsonValueString>(SpatializationName), MakeShared<FJsonValueString>(TEXT("panning, binaural"))));
			}
			OutSettings.bAttenuate = bAttenuate;
			OutSettings.bSpatialize = bSpatialize;
			OutSettings.bEnableOcclusion = bEnableOcclusion;
			OutSettings.AttenuationShape = EAttenuationShape::Sphere;
			OutSettings.AttenuationShapeExtents.X = InnerRadius;
			return true;
		}

		bool TryParseSoundClassProperties(
			const FUeAgentCanonicalValue& Value,
			FSoundClassProperties& OutProperties, const FString& Field, FExecutionMutationError& OutError)
		{
			bool bIsUiSound = false;
			bool bIsMusic = false;
			if (!TryRequireObjectFields(
					Value,
					{
						TEXT("volume"),
						TEXT("pitch"),
						TEXT("is_ui_sound"),
						TEXT("is_music"),
					}, Field, OutError)
				|| !TryReadFloat(
					*FindObjectField(Value, TEXT("volume")),
					OutProperties.Volume,
					0.0,
					1000.0, ChildPath(Field, TEXT("volume")), OutError)
				|| !TryReadFloat(
					*FindObjectField(Value, TEXT("pitch")),
					OutProperties.Pitch,
					0.0,
					8.0, ChildPath(Field, TEXT("pitch")), OutError)
				|| !TryReadBool(
					*FindObjectField(Value, TEXT("is_ui_sound")),
					bIsUiSound, ChildPath(Field, TEXT("is_ui_sound")), OutError)
				|| !TryReadBool(
					*FindObjectField(Value, TEXT("is_music")),
					bIsMusic, ChildPath(Field, TEXT("is_music")), OutError))
			{
				return false;
			}
			OutProperties.bIsUISound = bIsUiSound;
			OutProperties.bIsMusic = bIsMusic;
			return true;
		}

		struct FSoundClassNodeSnapshot final
		{
			TWeakObjectPtr<USoundClass> Node;
			TWeakObjectPtr<USoundClass> Parent;
			TArray<TWeakObjectPtr<USoundClass>> Children;
		};

		void CaptureSoundClassNode(
			TArray<FSoundClassNodeSnapshot>& Snapshots,
			TArray<FPackageDirtySnapshot>& Packages,
			USoundClass* Node)
		{
			if (!Node)
			{
				return;
			}
			for (const FSoundClassNodeSnapshot& Snapshot : Snapshots)
			{
				if (Snapshot.Node.Get() == Node)
				{
					return;
				}
			}
			FSoundClassNodeSnapshot Snapshot;
			Snapshot.Node = Node;
			Snapshot.Parent = Node->ParentClass;
			for (USoundClass* Child : Node->ChildClasses)
			{
				Snapshot.Children.Add(Child);
			}
			Snapshots.Add(MoveTemp(Snapshot));
			CapturePackage(Packages, Node);
		}

		bool TryParseConcurrencySettings(
			const FUeAgentCanonicalValue& Value,
			FSoundConcurrencySettings& OutSettings, const FString& Field, FExecutionMutationError& OutError)
		{
			if (!TryRequireObjectFields(
					Value,
					{
						TEXT("max_count"),
						TEXT("limit_to_owner"),
						TEXT("retrigger_time"),
						TEXT("resolution_rule"),
						TEXT("volume_scale"),
						TEXT("volume_scale_mode"),
						TEXT("volume_scale_attack_time"),
						TEXT("volume_scale_release_time"),
						TEXT("voice_steal_release_time"),
					}, Field, OutError))
			{
				return false;
			}
			bool bLimitToOwner = false;
			FString ResolutionRule;
			FString VolumeScaleMode;
			float VolumeScale = 1.0f;
			if (!TryReadInt32(
					*FindObjectField(Value, TEXT("max_count")),
					OutSettings.MaxCount,
					1,
					MAX_int32, ChildPath(Field, TEXT("max_count")), OutError)
				|| !TryReadBool(
					*FindObjectField(Value, TEXT("limit_to_owner")),
					bLimitToOwner, ChildPath(Field, TEXT("limit_to_owner")), OutError)
				|| !TryReadFloat(
					*FindObjectField(Value, TEXT("retrigger_time")),
					OutSettings.RetriggerTime,
					0.0,
					1000000.0, ChildPath(Field, TEXT("retrigger_time")), OutError)
				|| !TryReadString(
					*FindObjectField(Value, TEXT("resolution_rule")),
					ResolutionRule, ChildPath(Field, TEXT("resolution_rule")), OutError)
				|| !TryReadFloat(
					*FindObjectField(Value, TEXT("volume_scale")),
					VolumeScale,
					0.0,
					1.0, ChildPath(Field, TEXT("volume_scale")), OutError)
				|| !TryReadString(
					*FindObjectField(Value, TEXT("volume_scale_mode")),
					VolumeScaleMode, ChildPath(Field, TEXT("volume_scale_mode")), OutError)
				|| !TryReadFloat(
					*FindObjectField(
						Value,
						TEXT("volume_scale_attack_time")),
					OutSettings.VolumeScaleAttackTime,
					0.0,
					1000000.0, ChildPath(Field, TEXT("volume_scale_attack_time")), OutError)
				|| !TryReadFloat(
					*FindObjectField(
						Value,
						TEXT("volume_scale_release_time")),
					OutSettings.VolumeScaleReleaseTime,
					0.0,
					1000000.0, ChildPath(Field, TEXT("volume_scale_release_time")), OutError)
				|| !TryReadFloat(
					*FindObjectField(
						Value,
						TEXT("voice_steal_release_time")),
					OutSettings.VoiceStealReleaseTime,
					0.0,
					1000000.0, ChildPath(Field, TEXT("voice_steal_release_time")), OutError))
			{
				return false;
			}
			const FString Rule =
				ResolutionRule.TrimStartAndEnd().ToLower();
			if (Rule == TEXT("prevent_new"))
			{
				OutSettings.ResolutionRule =
					EMaxConcurrentResolutionRule::PreventNew;
			}
			else if (Rule == TEXT("stop_oldest"))
			{
				OutSettings.ResolutionRule =
					EMaxConcurrentResolutionRule::StopOldest;
			}
			else if (Rule == TEXT("stop_farthest_then_oldest"))
			{
				OutSettings.ResolutionRule =
					EMaxConcurrentResolutionRule::StopFarthestThenOldest;
			}
			else if (Rule == TEXT("stop_farthest_then_prevent_new"))
			{
				OutSettings.ResolutionRule =
					EMaxConcurrentResolutionRule::StopFarthestThenPreventNew;
			}
			else if (Rule == TEXT("stop_lowest_priority"))
			{
				OutSettings.ResolutionRule =
					EMaxConcurrentResolutionRule::StopLowestPriority;
			}
			else if (Rule == TEXT("stop_quietest"))
			{
				OutSettings.ResolutionRule =
					EMaxConcurrentResolutionRule::StopQuietest;
			}
			else if (Rule == TEXT("stop_lowest_priority_then_prevent_new"))
			{
				OutSettings.ResolutionRule =
					EMaxConcurrentResolutionRule::
						StopLowestPriorityThenPreventNew;
			}
			else
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_core_enum_invalid"), TEXT("EMaxConcurrentResolutionRule name is not supported."), ChildPath(Field, TEXT("resolution_rule")), MakeShared<FJsonValueString>(ResolutionRule), MakeShared<FJsonValueString>(TEXT("prevent_new, stop_oldest, stop_farthest_then_oldest, stop_farthest_then_prevent_new, stop_lowest_priority, stop_quietest, stop_lowest_priority_then_prevent_new"))));
			}
			const FString ScaleMode =
				VolumeScaleMode.TrimStartAndEnd().ToLower();
			if (ScaleMode == TEXT("default"))
			{
				OutSettings.VolumeScaleMode =
					EConcurrencyVolumeScaleMode::Default;
			}
			else if (ScaleMode == TEXT("distance"))
			{
				OutSettings.VolumeScaleMode =
					EConcurrencyVolumeScaleMode::Distance;
			}
			else if (ScaleMode == TEXT("priority"))
			{
				OutSettings.VolumeScaleMode =
					EConcurrencyVolumeScaleMode::Priority;
			}
			else
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_core_enum_invalid"), TEXT("EConcurrencyVolumeScaleMode name is not supported."), ChildPath(Field, TEXT("volume_scale_mode")), MakeShared<FJsonValueString>(VolumeScaleMode), MakeShared<FJsonValueString>(TEXT("default, distance, priority"))));
			}
			FFloatProperty* VolumeScaleProperty =
				FindFProperty<FFloatProperty>(
					FSoundConcurrencySettings::StaticStruct(),
					TEXT("VolumeScale"));
			if (!VolumeScaleProperty)
			{
                return Fail(OutError, NativeFailure(TEXT("asset_data_core_concurrency_property_missing"), TEXT("FSoundConcurrencySettings.VolumeScale is missing or is not a float property."), TEXT("VolumeScale"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("float property"))));
			}
			VolumeScaleProperty->SetPropertyValue_InContainer(
				&OutSettings,
				VolumeScale);
			OutSettings.bLimitToOwner = bLimitToOwner;
			return true;
		}

		bool TryParseSoundMixModifiers(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Value,
			TArray<FSoundClassAdjuster>& OutModifiers,
			TArray<TStrongObjectPtr<UObject>>& Pins,
			FExecutionMutationError& OutError, const FString& Field)
		{
			OutModifiers.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_modifiers_invalid"), TEXT("Sound mix modifiers must be an array."), Field, &Value, TEXT("array")));
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				const FUeAgentCanonicalValue& Object = Item.Get();
                const FString ItemPath = Field + TEXT("/") + FString::FromInt(OutModifiers.Num());
				if (!TryRequireObjectFields(
						Object,
						{
							TEXT("sound_class_ref"),
							TEXT("volume_adjuster"),
							TEXT("pitch_adjuster"),
							TEXT("low_pass_filter_frequency"),
							TEXT("apply_to_children"),
							TEXT(
								"voice_center_channel_volume_adjuster"),
						}, ItemPath, OutError))
				{
					return false;
				}
				USoundClass* SoundClass = nullptr;
				FSoundClassAdjuster Adjuster;
				bool bApplyToChildren = false;
				if (!TryResolveObject(
						Resolver,
						*FindObjectField(
							Object,
							TEXT("sound_class_ref")),
						SoundClass,
						OutError, ChildPath(ItemPath, TEXT("sound_class_ref")))
					|| !TryReadFloat(
						*FindObjectField(
							Object,
							TEXT("volume_adjuster")),
						Adjuster.VolumeAdjuster,
						0.0,
						4.0, ChildPath(ItemPath, TEXT("volume_adjuster")), OutError)
					|| !TryReadFloat(
						*FindObjectField(
							Object,
							TEXT("pitch_adjuster")),
						Adjuster.PitchAdjuster,
						0.0,
						8.0, ChildPath(ItemPath, TEXT("pitch_adjuster")), OutError)
					|| !TryReadFloat(
						*FindObjectField(
							Object,
							TEXT("low_pass_filter_frequency")),
						Adjuster.LowPassFilterFrequency,
						0.0,
						20000.0, ChildPath(ItemPath, TEXT("low_pass_filter_frequency")), OutError)
					|| !TryReadBool(
						*FindObjectField(
							Object,
							TEXT("apply_to_children")),
						bApplyToChildren, ChildPath(ItemPath, TEXT("apply_to_children")), OutError)
					|| !TryReadFloat(
						*FindObjectField(
							Object,
							TEXT(
								"voice_center_channel_volume_adjuster")),
						Adjuster.VoiceCenterChannelVolumeAdjuster,
						0.0,
						4.0, ChildPath(ItemPath, TEXT("voice_center_channel_volume_adjuster")), OutError))
				{
					return false;
				}
				for (const FSoundClassAdjuster& Existing : OutModifiers)
				{
					if (Existing.SoundClassObject == SoundClass)
					{
                        return Fail(OutError, JsonFailure(TEXT("asset_data_core_modifier_duplicate"), TEXT("A modifier already refers to this sound class."), ChildPath(ItemPath, TEXT("sound_class_ref")), MakeShared<FJsonValueString>(SoundClass->GetPathName()), MakeShared<FJsonValueString>(TEXT("unique resolved sound class"))));
					}
				}
				Adjuster.SoundClassObject = SoundClass;
				Adjuster.bApplyToChildren = bApplyToChildren;
				OutModifiers.Add(Adjuster);
				PinUnique(Pins, SoundClass);
			}
			return true;
		}

		bool TryApplyCollectionPresence(
			const FString& Presence,
			const TArray<FSoundClassAdjuster>& Requested,
			TArray<FSoundClassAdjuster>& InOutModifiers, const FString& Field, FExecutionMutationError& OutError)
		{
			if (Presence == TEXT("clear"))
			{
				InOutModifiers.Reset();
				return true;
			}
			if (Presence == TEXT("replace"))
			{
				InOutModifiers = Requested;
				return true;
			}
			if (Presence == TEXT("remove"))
			{
				for (const FSoundClassAdjuster& Item : Requested)
				{
					InOutModifiers.RemoveAll(
						[&Item](const FSoundClassAdjuster& Existing)
						{
							return Existing.SoundClassObject
								== Item.SoundClassObject;
						});
				}
				return true;
			}
			if (Presence == TEXT("set"))
			{
				for (const FSoundClassAdjuster& Item : Requested)
				{
					const int32 Index =
						InOutModifiers.IndexOfByPredicate(
							[&Item](
								const FSoundClassAdjuster& Existing)
							{
								return Existing.SoundClassObject
									== Item.SoundClassObject;
							});
					if (Index == INDEX_NONE)
					{
						InOutModifiers.Add(Item);
					}
					else
					{
						InOutModifiers[Index] = Item;
					}
				}
				return true;
			}
			return Fail(OutError, JsonFailure(TEXT("asset_data_core_presence_invalid"), TEXT("The modifier collection presence is not supported."), Field, MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("clear, replace, remove, set"))));
		}

		struct FSoundSubmixNodeSnapshot final
		{
			TWeakObjectPtr<USoundSubmixBase> Node;
			TArray<TWeakObjectPtr<USoundSubmixBase>> Children;
			TWeakObjectPtr<USoundSubmixBase> Parent;
		};

		void CaptureSoundSubmixNode(
			TArray<FSoundSubmixNodeSnapshot>& Snapshots,
			TArray<FPackageDirtySnapshot>& Packages,
			USoundSubmixBase* Node)
		{
			if (!Node)
			{
				return;
			}
			for (const FSoundSubmixNodeSnapshot& Snapshot : Snapshots)
			{
				if (Snapshot.Node.Get() == Node)
				{
					return;
				}
			}
			FSoundSubmixNodeSnapshot Snapshot;
			Snapshot.Node = Node;
			for (USoundSubmixBase* Child : Node->ChildSubmixes)
			{
				Snapshot.Children.Add(Child);
			}
			if (USoundSubmixWithParentBase* WithParent =
				Cast<USoundSubmixWithParentBase>(Node))
			{
				Snapshot.Parent = WithParent->ParentSubmix;
			}
			Snapshots.Add(MoveTemp(Snapshot));
			CapturePackage(Packages, Node);
		}

		bool WouldCreateSubmixCycle(
			USoundSubmix* Target,
			USoundSubmixBase* NewParent)
		{
			for (USoundSubmixBase* Node = NewParent;
				Node;
				)
			{
				if (Node == Target)
				{
					return true;
				}
				const USoundSubmixWithParentBase* WithParent =
					Cast<USoundSubmixWithParentBase>(Node);
				Node = WithParent
					? WithParent->ParentSubmix.Get()
					: nullptr;
			}
			return false;
		}

		bool TryParseInterpMode(
			const FString& Name,
			ERichCurveInterpMode& OutMode, const FString& Field, FExecutionMutationError& OutError)
		{
			const FString Value = Name.TrimStartAndEnd().ToLower();
			if (Value == TEXT("constant"))
			{
				OutMode = RCIM_Constant;
			}
			else if (Value == TEXT("linear"))
			{
				OutMode = RCIM_Linear;
			}
			else if (Value == TEXT("cubic"))
			{
				OutMode = RCIM_Cubic;
			}
			else
			{
				return Fail(OutError, JsonFailure(TEXT("asset_data_core_enum_invalid"), TEXT("ERichCurveInterpMode name is not supported."), Field, MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("constant, linear, cubic"))));
			}
			return true;
		}

		bool TryParseTangentMode(
			const FString& Name,
			ERichCurveTangentMode& OutMode, const FString& Field, FExecutionMutationError& OutError)
		{
			const FString Value = Name.TrimStartAndEnd().ToLower();
			if (Value == TEXT("auto"))
			{
				OutMode = RCTM_Auto;
			}
			else if (Value == TEXT("user"))
			{
				OutMode = RCTM_User;
			}
			else if (Value == TEXT("break"))
			{
				OutMode = RCTM_Break;
			}
			else if (Value == TEXT("smart_auto"))
			{
				OutMode = RCTM_SmartAuto;
			}
			else if (Value == TEXT("none"))
			{
				OutMode = RCTM_None;
			}
			else
			{
				return Fail(OutError, JsonFailure(TEXT("asset_data_core_enum_invalid"), TEXT("ERichCurveTangentMode name is not supported."), Field, MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("auto, user, break, smart_auto, none"))));
			}
			return true;
		}

		bool TryParseTangentWeightMode(
			const FString& Name,
			ERichCurveTangentWeightMode& OutMode, const FString& Field, FExecutionMutationError& OutError)
		{
			const FString Value = Name.TrimStartAndEnd().ToLower();
			if (Value == TEXT("none"))
			{
				OutMode = RCTWM_WeightedNone;
			}
			else if (Value == TEXT("arrive"))
			{
				OutMode = RCTWM_WeightedArrive;
			}
			else if (Value == TEXT("leave"))
			{
				OutMode = RCTWM_WeightedLeave;
			}
			else if (Value == TEXT("both"))
			{
				OutMode = RCTWM_WeightedBoth;
			}
			else
			{
				return Fail(OutError, JsonFailure(TEXT("asset_data_core_enum_invalid"), TEXT("ERichCurveTangentWeightMode name is not supported."), Field, MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("none, arrive, leave, both"))));
			}
			return true;
		}

		bool TryReadFloatArray(
			const FUeAgentCanonicalValue& Value,
			const int32 ExpectedCount,
			TArray<float>& OutValues, const FString& Field, FExecutionMutationError& OutError)
		{
			if (Value.GetKind() != EUeAgentValueKind::Array
				|| Value.GetArrayItems().Num() != ExpectedCount)
			{
				return Fail(OutError, ValueFailure(TEXT("asset_data_core_component_count_invalid"), TEXT("The numeric component array has an invalid type or length."), Field, &Value, FString::Printf(TEXT("array of %d numbers"), ExpectedCount)));
			}
			OutValues.Reset(ExpectedCount);
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				float Number = 0.0f;
				if (!TryReadFloat(
						Item.Get(),
						Number,
						-TNumericLimits<float>::Max(),
						TNumericLimits<float>::Max(), Field + TEXT("/") + FString::FromInt(OutValues.Num()), OutError))
				{
					return false;
				}
				OutValues.Add(Number);
			}
			return true;
		}

		int32 GetCurveDimension(UObject* Target)
		{
			if (Cast<UCurveFloat>(Target))
			{
				return 1;
			}
			if (Cast<UCurveVector>(Target))
			{
				return 3;
			}
			if (Cast<UCurveLinearColor>(Target))
			{
				return 4;
			}
			return 0;
		}

		FRichCurve* GetCurveChannel(
			UObject* Target,
			const int32 Index)
		{
			if (UCurveFloat* Curve = Cast<UCurveFloat>(Target))
			{
				return Index == 0 ? &Curve->FloatCurve : nullptr;
			}
			if (UCurveVector* Curve = Cast<UCurveVector>(Target))
			{
				return Index >= 0 && Index < 3
					? &Curve->FloatCurves[Index]
					: nullptr;
			}
			if (UCurveLinearColor* Curve =
				Cast<UCurveLinearColor>(Target))
			{
				return Index >= 0 && Index < 4
					? &Curve->FloatCurves[Index]
					: nullptr;
			}
			return nullptr;
		}

		bool TryParseCurveKeys(
			const FUeAgentCanonicalValue& Value,
			const int32 Dimension,
			TArray<TArray<FRichCurveKey>>& OutChannels, const FString& Field, FExecutionMutationError& OutError)
		{
            if (Value.GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_curve_keys_invalid"),
                    TEXT("Curve keys must be an array."), Field, &Value, TEXT("array")));
            if (Dimension < 1 || Dimension > 4)
                return Fail(OutError, NativeFailure(TEXT("asset_data_core_curve_dimension_invalid"),
                    TEXT("The curve dimension is outside the supported range."), TEXT("curve_dimension"),
                    MakeShared<FJsonValueNumber>(Dimension), MakeShared<FJsonValueString>(TEXT("integer from 1 to 4"))));
			OutChannels.SetNum(Dimension);
			float PreviousTime = 0.0f;
			bool bHasPreviousTime = false;
            int32 InputIndex = 0;
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				const FUeAgentCanonicalValue& KeyObject = Item.Get();
                const FString KeyPath = Field + TEXT("/") + FString::FromInt(InputIndex++);
				if (!TryRequireObjectFields(
						KeyObject,
						{
							TEXT("time"),
							TEXT("values"),
							TEXT("interp_mode"),
							TEXT("tangent_mode"),
							TEXT("tangent_weight_mode"),
							TEXT("arrive_tangents"),
							TEXT("leave_tangents"),
							TEXT("arrive_tangent_weights"),
							TEXT("leave_tangent_weights"),
						}, KeyPath, OutError))
				{
					return false;
				}
				float Time = 0.0f;
				FString InterpName;
				FString TangentName;
				FString WeightName;
				ERichCurveInterpMode InterpMode = RCIM_Linear;
				ERichCurveTangentMode TangentMode = RCTM_Auto;
				ERichCurveTangentWeightMode WeightMode =
					RCTWM_WeightedNone;
				TArray<float> Values;
				TArray<float> ArriveTangents;
				TArray<float> LeaveTangents;
				TArray<float> ArriveWeights;
				TArray<float> LeaveWeights;
				if (!TryReadFloat(
						*FindObjectField(KeyObject, TEXT("time")),
						Time,
						-TNumericLimits<float>::Max(),
						TNumericLimits<float>::Max(), ChildPath(KeyPath, TEXT("time")), OutError)) return false;
                if (bHasPreviousTime && Time <= PreviousTime)
                    return Fail(OutError, JsonFailure(TEXT("asset_data_core_curve_time_order_invalid"), TEXT("Curve key times must be strictly increasing."), ChildPath(KeyPath, TEXT("time")), MakeShared<FJsonValueNumber>(Time), MakeShared<FJsonValueString>(FString::Printf(TEXT("greater than previous time %g"), PreviousTime))));
                if (!TryReadString(
						*FindObjectField(
							KeyObject,
							TEXT("interp_mode")),
						InterpName, ChildPath(KeyPath, TEXT("interp_mode")), OutError)
					|| !TryParseInterpMode(InterpName, InterpMode, ChildPath(KeyPath, TEXT("interp_mode")), OutError)
					|| !TryReadString(
						*FindObjectField(
							KeyObject,
							TEXT("tangent_mode")),
						TangentName, ChildPath(KeyPath, TEXT("tangent_mode")), OutError)
					|| !TryParseTangentMode(
						TangentName,
						TangentMode, ChildPath(KeyPath, TEXT("tangent_mode")), OutError)
					|| !TryReadString(
						*FindObjectField(
							KeyObject,
							TEXT("tangent_weight_mode")),
						WeightName, ChildPath(KeyPath, TEXT("tangent_weight_mode")), OutError)
					|| !TryParseTangentWeightMode(
						WeightName,
						WeightMode, ChildPath(KeyPath, TEXT("tangent_weight_mode")), OutError)
					|| !TryReadFloatArray(
						*FindObjectField(
							KeyObject,
							TEXT("values")),
						Dimension,
						Values, ChildPath(KeyPath, TEXT("values")), OutError)
					|| !TryReadFloatArray(
						*FindObjectField(
							KeyObject,
							TEXT("arrive_tangents")),
						Dimension,
						ArriveTangents, ChildPath(KeyPath, TEXT("arrive_tangents")), OutError)
					|| !TryReadFloatArray(
						*FindObjectField(
							KeyObject,
							TEXT("leave_tangents")),
						Dimension,
						LeaveTangents, ChildPath(KeyPath, TEXT("leave_tangents")), OutError)
					|| !TryReadFloatArray(
						*FindObjectField(
							KeyObject,
							TEXT("arrive_tangent_weights")),
						Dimension,
						ArriveWeights, ChildPath(KeyPath, TEXT("arrive_tangent_weights")), OutError)
					|| !TryReadFloatArray(
						*FindObjectField(
							KeyObject,
							TEXT("leave_tangent_weights")),
						Dimension,
						LeaveWeights, ChildPath(KeyPath, TEXT("leave_tangent_weights")), OutError))
				{
					return false;
				}
				for (int32 ChannelIndex = 0;
					ChannelIndex < Dimension;
					++ChannelIndex)
				{
					FRichCurveKey Key(Time, Values[ChannelIndex]);
					Key.InterpMode = InterpMode;
					Key.TangentMode = TangentMode;
					Key.TangentWeightMode = WeightMode;
					Key.ArriveTangent = ArriveTangents[ChannelIndex];
					Key.LeaveTangent = LeaveTangents[ChannelIndex];
					Key.ArriveTangentWeight =
						ArriveWeights[ChannelIndex];
					Key.LeaveTangentWeight =
						LeaveWeights[ChannelIndex];
					OutChannels[ChannelIndex].Add(Key);
				}
				PreviousTime = Time;
				bHasPreviousTime = true;
			}
			return true;
		}

		bool WriteCurveChannels(
			UObject* Target,
			const TArray<TArray<FRichCurveKey>>& Channels,
			const bool bModify, FExecutionMutationError& OutError)
		{
			if (!Target
				|| Channels.Num() != GetCurveDimension(Target))
			{
                OutError = NativeFailure(TEXT("asset_data_curve_write_invalid"),
                    !Target ? TEXT("The curve target is unavailable.") : TEXT("The prepared channel count does not match the curve dimension."),
                    !Target ? TEXT("target") : TEXT("channel_count"),
                    !Target ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Channels.Num())),
                    Target ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(GetCurveDimension(Target)))
                        : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("live supported curve target"))));
                if (Target) OutError.TargetPath = Target->GetPathName();
                return false;
			}
			if (bModify)
			{
				Target->Modify();
			}
			for (int32 Index = 0; Index < Channels.Num(); ++Index)
			{
				FRichCurve* Curve = GetCurveChannel(Target, Index);
				if (!Curve)
				{
                    OutError = NativeFailure(TEXT("asset_data_curve_channel_unavailable"),
                        TEXT("The native curve channel is unavailable for writing."), TEXT("channels/") + FString::FromInt(Index),
                        MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("writable rich curve channel")));
                    OutError.TargetPath = Target->GetPathName();
                    OutError.OperationIndex = Index;
                    OutError.MutationMayHaveStarted = bModify || Index > 0;
                    return false;
				}
				Curve->SetKeys(Channels[Index]);
			}
			Target->PostEditChange();
			return true;
		}

		struct FRegisteredFieldValue final
		{
			FProperty* Property = nullptr;
			TSharedPtr<UeAgentAuthoringPropertyValues::FPropertyValueStorage> Storage;
		};

		bool UsesLegacyRegisteredAssetReference(const FProperty* Property)
		{
			return Property && Property->ArrayDim == 1
				&& CastField<FObjectPropertyBase>(Property)
				&& !CastField<FSoftObjectProperty>(Property)
				&& Property->GetMetaData(TEXT("UAIAssetDataType")) == TEXT("asset_ref");
		}

		bool EncodeRegisteredValue(FProperty* Property, const void* ValuePtr, UObject* Owner,
			TArray<TStrongObjectPtr<UObject>>* Pins, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& Error)
		{
			UeAgentPropertyValueCodec::FOptions Options;
			if (Pins)
				Options.ScopedReferenceEncoder = [Pins](UObject* Object, const FString& Path) {
					PinUnique(*Pins, Object);
					return UeAgentPropertyValueCodec::EncodeReferencePath(Path);
				};
			OutValue = UeAgentPropertyValueCodec::Encode(Property, ValuePtr, Owner, Options);
			if (!UeAgentPropertyValueCodec::IsReliableCanonicalValue(Property, OutValue, &Error)) return false;
			if (OutValue->Type == EJson::Object && OutValue->AsObject()->Values.Num() == 1)
				if (const auto Literal = OutValue->AsObject()->TryGetField(TEXT("literal"))) OutValue = Literal;
			// Preserve the existing typed operation's top-level string reference.
			// All other types and nested references use the common codec shape.
			if (UsesLegacyRegisteredAssetReference(Property))
			{
				UObject* Object = CastFieldChecked<FObjectPropertyBase>(Property)->GetObjectPropertyValue(ValuePtr);
				if (Object) OutValue = MakeShared<FJsonValueString>(Object->GetPathName());
				else OutValue = MakeShared<FJsonValueNull>();
			}
			return true;
		}

		bool DecodeRegisteredValue(IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			FProperty* Property, const FUeAgentCanonicalValue& Requested, UObject* Owner,
			void* ValuePtr, TArray<TStrongObjectPtr<UObject>>& Pins, FExecutionMutationError& OutError,
            const FString& Field, const bool bNative)
		{
			TSharedPtr<FJsonValue> Json;
			if (!CanonicalJson::TryToJsonValue(Requested, Json, OutError)) return FailRegisteredValue(OutError, Field, bNative);
			if (UsesLegacyRegisteredAssetReference(Property)
				&& (Requested.GetKind() == EUeAgentValueKind::String || Requested.GetKind() == EUeAgentValueKind::Null))
			{
				UObject* Resolved = nullptr;
				if (Requested.GetKind() == EUeAgentValueKind::String
					&& !Resolver.TryResolveExisting(Requested.GetText(), Resolved, OutError)) return FailRegisteredValue(OutError, Field, bNative);
				PinUnique(Pins, Resolved);
				Json = UeAgentPropertyValueCodec::EncodeReferencePath(Resolved ? Resolved->GetPathName() : FString());
			}
			UeAgentPropertyValueCodec::FOptions Options;
			Options.bAllowPartialStructFields = true;
			if (!UeAgentPropertyValueCodec::Decode(Property, Json, ValuePtr, Owner, Options, OutError)) return FailRegisteredValue(OutError, Field, bNative);
			TSharedPtr<FJsonValue> Captured;
			if (!EncodeRegisteredValue(Property, ValuePtr, Owner, &Pins, Captured, OutError)) return FailRegisteredValue(OutError, Field, true);
            return true;
		}

		void WriteRegisteredValue(UObject* Container, const FRegisteredFieldValue& Value)
		{
			Value.Property->CopyCompleteValue(Value.Property->ContainerPtrToValuePtr<void>(Container),
				Value.Storage->GetValuePtr());
		}

		bool TryParseRegisteredFields(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver, UObject* Target,
			const FUeAgentCanonicalValue* Value, const bool bClear,
			TArray<FRegisteredFieldValue>& OutOriginal, TArray<FRegisteredFieldValue>& OutNew,
			TArray<TStrongObjectPtr<UObject>>& Pins, FExecutionMutationError& OutError)
		{
            if (!Target) return Fail(OutError, NativeFailure(TEXT("asset_data_registered_target_missing"),
                TEXT("Registered property preparation requires a live target."), TEXT("target"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("live UObject"))));
            if (!bClear && (!Value || Value->GetKind() != EUeAgentValueKind::Object))
                return Fail(OutError, ValueFailure(TEXT("asset_data_registered_fields_invalid"),
                    TEXT("Registered field changes must be an object."), TEXT("registered_fields"), Value, TEXT("object")));
            TMap<FString, FProperty*> Registered;
            if (!CollectRegisteredDataObjectFields(Target->GetClass(), Registered, OutError))
                return FailRegisteredValue(OutError, TEXT("registered_fields"), true);
			if (!bClear)
				for (const auto& Item : Value->GetObjectItems())
					if (!Registered.Contains(Item.Key))
                        return Fail(OutError, ValueFailure(TEXT("asset_data_registered_field_unknown"),
                            TEXT("The field is not registered for this target class."), ChildPath(TEXT("registered_fields"), Item.Key),
                            &Item.Value.Get(), Target->GetClass()->GetPathName() + TEXT(" registered property")));
			UObject* Default = Target->GetClass()->GetDefaultObject();
			TArray<FString> Ids;
			Registered.GetKeys(Ids);
			Ids.Sort();
			for (const FString& Id : Ids)
			{
				const FUeAgentCanonicalValue* Requested = bClear ? nullptr : FindObjectField(*Value, Id);
				if (!bClear && !Requested) continue;
				FProperty* Property = Registered[Id];
				const void* Current = Property->ContainerPtrToValuePtr<void>(Target);
                const FString Field = ChildPath(TEXT("registered_fields"), Id);
                FRegisteredFieldValue Original{Property, UeAgentAuthoringPropertyValues::FPropertyValueStorage::CopyFrom(Property, Current, OutError)};
                if (!Original.Storage) return FailRegisteredValue(OutError, Field, true);
                FRegisteredFieldValue Desired{Property, UeAgentAuthoringPropertyValues::FPropertyValueStorage::CopyFrom(Property,
                    bClear ? Property->ContainerPtrToValuePtr<void>(Default) : Current, OutError)};
                if (!Desired.Storage) return FailRegisteredValue(OutError, Field, true);
                TSharedPtr<FJsonValue> Captured;
                if (!EncodeRegisteredValue(Property, Current, Target, &Pins, Captured, OutError))
                    return FailRegisteredValue(OutError, Field, true);
                if (!bClear && !DecodeRegisteredValue(Resolver, Property, *Requested, Target,
                    Desired.Storage->GetMutableValuePtr(), Pins, OutError, Field, false)) return false;
                if (!EncodeRegisteredValue(Property, Desired.Storage->GetValuePtr(), Target, &Pins, Captured, OutError))
                    return FailRegisteredValue(OutError, Field, true);
				OutOriginal.Add(MoveTemp(Original));
				OutNew.Add(MoveTemp(Desired));
			}
			if (OutNew.IsEmpty()) return Fail(OutError, ValueFailure(TEXT("asset_data_registered_changes_empty"),
                TEXT("The operation did not select any registered properties."), TEXT("registered_fields"), Value, TEXT("at least one registered property"), bClear ? TEXT("native_result") : TEXT("authored_value")));
            return true;
		}

		bool TryBuildRegisteredFieldProjection(UObject* Container, FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutFields, FUeAgentCanonicalValuePtr& OutContext,
			FExecutionMutationError& OutError)
		{
			OutFields.Reset(); OutContext.Reset();
            if (!Container) return Fail(OutError, NativeFailure(TEXT("asset_data_registered_projection_target_missing"),
                TEXT("Projecting registered properties requires a live container."), TEXT("target"),
                MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("live UObject"))));
			TMap<FString, FProperty*> Registered;
            if (!CollectRegisteredDataObjectFields(Container->GetClass(), Registered, OutError)) return FailRegisteredValue(OutError, TEXT("registered_fields"), true);
			TArray<FString> Ids;
			Registered.GetKeys(Ids); Ids.Sort();
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Values, Contexts;
			for (const FString& Id : Ids)
			{
				FProperty* Property = Registered[Id];
				TSharedPtr<FJsonValue> Json;
				FUeAgentCanonicalValuePtr Value, Context;
				auto TypeContext = MakeShared<FJsonObject>();
				TypeContext->SetStringField(TEXT("owner_class"), Container->GetClass()->GetPathName());
				TypeContext->SetStringField(TEXT("property_name"), Property->GetName());
				if (!EncodeRegisteredValue(Property, Property->ContainerPtrToValuePtr<void>(Container), Container, nullptr, Json, OutError)
					|| !CanonicalJson::TryFromJsonValue(Json, Resources, Value, OutError)
					|| !CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(TypeContext), Resources, Context, OutError))
					return FailRegisteredValue(OutError, ChildPath(TEXT("registered_fields"), Id), true);
				Values.Emplace(Id, Value.ToSharedRef()); Contexts.Emplace(Id, Context.ToSharedRef());
			}
            if (FUeAgentCanonicalValue::TryObject(Values, Resources, OutFields, OutError)
                && FUeAgentCanonicalValue::TryObject(Contexts, Resources, OutContext, OutError)) return true;
            return FailRegisteredValue(OutError, TEXT("registered_fields"), true);
		}

		bool TryWrapRegisteredFieldProjection(
			const FUeAgentCanonicalValueRef& RegisteredFields,
			const FUeAgentCanonicalValueRef& RegisteredContext,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutFields,
			FUeAgentCanonicalValuePtr& OutContext,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			if (FUeAgentCanonicalValue::TryObject(
					{
						TPair<FString, FUeAgentCanonicalValueRef>(
							TEXT("registered_fields"),
							RegisteredFields),
					},
					Resources,
					OutFields,
					StateError)
				&& FUeAgentCanonicalValue::TryObject(
					{
						TPair<FString, FUeAgentCanonicalValueRef>(
							TEXT("registered_fields"),
							RegisteredContext),
					},
					Resources,
					OutContext,
					StateError))
			{
				return true;
			}
            OutError = MoveTemp(StateError);
            return FailRegisteredValue(OutError, FString(), true);
		}

		bool TryBuildExpectedRegisteredFields(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& CurrentFields,
			const FUeAgentCanonicalValue& DefaultFields,
			const FUeAgentCanonicalValue& ProjectionContext,
			const FUeAgentCanonicalValue& ProposedAfter,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			OutExpected.Reset();
			const FUeAgentCanonicalValue* Current =
				FindObjectField(
					CurrentFields,
					TEXT("registered_fields"));
			const FUeAgentCanonicalValueRef* DefaultsRef =
				FindObjectFieldRef(
					DefaultFields,
					TEXT("registered_fields"));
			const FUeAgentCanonicalValueRef* ContextRef =
				FindObjectFieldRef(
					ProjectionContext,
					TEXT("registered_fields"));
			const FUeAgentCanonicalValue* Defaults =
				DefaultsRef ? &DefaultsRef->Get() : nullptr;
			const FUeAgentCanonicalValue* Context =
				ContextRef ? &ContextRef->Get() : nullptr;
			const FUeAgentCanonicalValue* TypedChanges =
				FindObjectField(
					ProposedAfter,
					TEXT("typed_changes"));
            if (!Current || Current->GetKind() != EUeAgentValueKind::Object)
                return Fail(OutError, ValueFailure(TEXT("asset_data_registered_expected_state_invalid"), TEXT("The captured registered properties must be an object."),
                    TEXT("current_fields/registered_fields"), Current, TEXT("object"), TEXT("native_result")));
            if (!Defaults || Defaults->GetKind() != EUeAgentValueKind::Object)
                return Fail(OutError, ValueFailure(TEXT("asset_data_registered_expected_state_invalid"), TEXT("The default registered properties must be an object."),
                    TEXT("default_fields/registered_fields"), Defaults, TEXT("object"), TEXT("native_result")));
            if (!Context || Context->GetKind() != EUeAgentValueKind::Object)
                return Fail(OutError, ValueFailure(TEXT("asset_data_registered_expected_state_invalid"), TEXT("The registered property context must be an object."),
                    TEXT("projection_context/registered_fields"), Context, TEXT("object"), TEXT("native_result")));
            if (!TypedChanges || TypedChanges->GetKind() != EUeAgentValueKind::Array || TypedChanges->GetArrayItems().Num() != 1)
                return Fail(OutError, ValueFailure(TEXT("asset_data_registered_expected_state_invalid"), TEXT("Expected registered properties require exactly one typed change."),
                    TEXT("typed_changes"), TypedChanges, TEXT("array with one registered_fields change")));
			const FUeAgentCanonicalValue& Change =
				TypedChanges->GetArrayItems()[0].Get();
			FString FieldId;
			FString Presence;
			const FUeAgentCanonicalValue* Field =
				FindObjectField(Change, TEXT("field_id"));
			const FUeAgentCanonicalValue* PresenceValue =
				FindObjectField(Change, TEXT("presence"));
            if (!TryReadRequiredString(Field, FieldId, TEXT("typed_changes/0/field_id"), OutError)) return false;
            if (FieldId != TEXT("registered_fields"))
                return Fail(OutError, ValueFailure(TEXT("asset_data_registered_expected_change_invalid"), TEXT("This change must address registered_fields."),
                    TEXT("typed_changes/0/field_id"), Field, TEXT("registered_fields")));
            if (!TryReadRequiredString(PresenceValue, Presence, TEXT("typed_changes/0/presence"), OutError)) return false;

			if (Presence == TEXT("clear"))
			{
				FUeAgentCanonicalValuePtr WrappedContext;
				return TryWrapRegisteredFieldProjection(
					*DefaultsRef,
					*ContextRef,
					Resources,
					OutExpected,
					WrappedContext,
					OutError);
			}
			const FUeAgentCanonicalValue* TypedValue =
				FindObjectField(Change, TEXT("value"));
			const FUeAgentCanonicalValue* Requested =
				TypedValue
					? FindObjectField(
						*TypedValue,
						TEXT("canonical_value"))
					: nullptr;
            if (Presence != TEXT("set"))
                return Fail(OutError, ValueFailure(TEXT("asset_data_registered_expected_presence_invalid"), TEXT("The registered field presence is not supported."),
                    TEXT("typed_changes/0/presence"), PresenceValue, TEXT("set or clear")));
            if (!Requested || Requested->GetKind() != EUeAgentValueKind::Object)
                return Fail(OutError, ValueFailure(TEXT("asset_data_registered_expected_value_invalid"), TEXT("The registered field change value must be an object."),
                    TEXT("registered_fields"), Requested, TEXT("object")));

			TMap<FString, FUeAgentCanonicalValueRef> Merged;
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Item
				: Current->GetObjectItems())
			{
				Merged.Add(Item.Key, Item.Value);
			}
			FUeAgentStateSemanticError StateError;
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Item
				: Requested->GetObjectItems())
			{
				const FUeAgentCanonicalValue* FieldContext = FindObjectField(*Context, Item.Key);
				const FUeAgentCanonicalValue* ClassValue = FieldContext ? FindObjectField(*FieldContext, TEXT("owner_class")) : nullptr;
				const FUeAgentCanonicalValue* PropertyValue = FieldContext ? FindObjectField(*FieldContext, TEXT("property_name")) : nullptr;
                const FString FieldPath = ChildPath(TEXT("registered_fields"), Item.Key);
                FString ClassPath, PropertyName;
                if (!TryReadRequiredString(ClassValue, ClassPath, ChildPath(FieldPath, TEXT("owner_class")), OutError, TEXT("native_result"))
                    || !TryReadRequiredString(PropertyValue, PropertyName, ChildPath(FieldPath, TEXT("property_name")), OutError, TEXT("native_result"))) return false;
                if (!Merged.Contains(Item.Key))
                    return Fail(OutError, ValueFailure(TEXT("asset_data_registered_expected_field_unknown"), TEXT("The requested property is absent from the captured registered fields."),
                        FieldPath, &Item.Value.Get(), TEXT("captured registered property")));
                UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
                if (!Class) Class = LoadObject<UClass>(nullptr, *ClassPath);
                TMap<FString, FProperty*> Registered;
                if (!CollectRegisteredDataObjectFields(Class, Registered, OutError)) return FailRegisteredValue(OutError, FieldPath, true);
                if (!Registered.Contains(Item.Key) || Registered[Item.Key]->GetName() != PropertyName)
                    return Fail(OutError, JsonFailure(TEXT("asset_data_registered_expected_type_invalid"),
                        TEXT("The captured property context does not match the class registration."), FieldPath,
                        MakeShared<FJsonValueString>(ClassPath + TEXT(".") + PropertyName),
                        MakeShared<FJsonValueString>(Registered.Contains(Item.Key) ? Registered[Item.Key]->GetPathName() : TEXT("registered property")), TEXT("native_result")));
				FProperty* Property = Registered[Item.Key];
				UObject* Owner = Class->GetDefaultObject();
				auto Storage = UeAgentAuthoringPropertyValues::FPropertyValueStorage::CopyFrom(
					Property, Property->ContainerPtrToValuePtr<void>(Owner), OutError);
				TArray<TStrongObjectPtr<UObject>> Pins;
				FUeAgentCanonicalValuePtr Normalized;
				TSharedPtr<FJsonValue> Json;
				// Start from the captured native value, then decode the original request;
				// do not derive expected state from the action's converted desired buffer.
                if (!Storage) return FailRegisteredValue(OutError, FieldPath, true);
                if (!DecodeRegisteredValue(Resolver, Property, Merged[Item.Key].Get(), Owner,
                    Storage->GetMutableValuePtr(), Pins, OutError, FieldPath, true)
                    || !DecodeRegisteredValue(Resolver, Property, Item.Value.Get(), Owner,
                    Storage->GetMutableValuePtr(), Pins, OutError, FieldPath, false)) return false;
                if (!EncodeRegisteredValue(Property, Storage->GetValuePtr(), Owner, nullptr, Json, OutError)
                    || !CanonicalJson::TryFromJsonValue(Json, Resources, Normalized, OutError))
                    return FailRegisteredValue(OutError, FieldPath, true);
				Merged[Item.Key] =
					Normalized.ToSharedRef();
			}

			TArray<FString> Keys;
			Merged.GetKeys(Keys);
			Keys.Sort();
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				MergedItems;
			for (const FString& Key : Keys)
			{
				MergedItems.Emplace(Key, Merged[Key]);
			}
			FUeAgentCanonicalValuePtr MergedObject;
			if (!FUeAgentCanonicalValue::TryObject(
					MergedItems,
					Resources,
					MergedObject,
					StateError))
			{
                OutError = MoveTemp(StateError);
                return FailRegisteredValue(OutError, FString(), true);
			}
			FUeAgentCanonicalValuePtr WrappedContext;
			return TryWrapRegisteredFieldProjection(
				MergedObject.ToSharedRef(),
				*ContextRef,
				Resources,
				OutExpected,
				WrappedContext,
				OutError);
		}

		bool TryNormalizeFileUri(
			const FUeAgentCanonicalValue& Value,
			FString& OutPath, const FString& Field, FExecutionMutationError& OutError)
		{
			FString Uri;
			if (!TryReadString(Value, Uri, Field, OutError))
			{
				return false;
			}
			Uri.TrimStartAndEndInline();
			if (Uri.IsEmpty())
			{
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_file_uri_empty"), TEXT("The file path or URI cannot be empty."), Field, &Value, TEXT("nonempty file path or URI")));
			}
			if (Uri.StartsWith(
					TEXT("file:"),
					ESearchCase::IgnoreCase))
			{
				const FString Remainder = Uri.Mid(5);
				if (Remainder.StartsWith(TEXT("///")))
				{
					const FString AbsolutePart =
						Remainder.Mid(3);
					const bool bWindowsDrive =
						AbsolutePart.Len() >= 2
						&& FChar::IsAlpha(AbsolutePart[0])
						&& AbsolutePart[1] == TEXT(':');
					OutPath = bWindowsDrive
						? AbsolutePart
						: TEXT("/") + AbsolutePart;
				}
				else if (Remainder.StartsWith(TEXT("//")))
				{
					OutPath = Remainder;
				}
				else if (Remainder.StartsWith(TEXT("/")))
				{
					OutPath = Remainder;
				}
				else
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_core_file_uri_invalid"), TEXT("A file URI must have an absolute path or a network path after the file scheme."), Field, &Value, TEXT("file:/path, file:///path or file://server/path")));
				}
			}
			else
			{
				OutPath = Uri;
			}
			if (OutPath.IsEmpty() || OutPath.Contains(TEXT("\r")) || OutPath.Contains(TEXT("\n")))
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_file_path_invalid"), TEXT("The normalized file path must be nonempty and must not contain a line break."), Field, &Value, TEXT("nonempty single-line file path")));
            return true;
		}

		bool TryParseRuntimeVirtualTextureMaterialType(
			const FString& Name,
			ERuntimeVirtualTextureMaterialType& OutType, const FString& Field, FExecutionMutationError& OutError)
		{
			const FString Value = Name.TrimStartAndEnd().ToLower();
			if (Value == TEXT("base_color"))
			{
				OutType =
					ERuntimeVirtualTextureMaterialType::BaseColor;
			}
			else if (Value == TEXT("mask4"))
			{
				OutType =
					ERuntimeVirtualTextureMaterialType::Mask4;
			}
			else if (Value == TEXT("base_color_normal_roughness"))
			{
				OutType = ERuntimeVirtualTextureMaterialType::
					BaseColor_Normal_Roughness;
			}
			else if (Value == TEXT("base_color_normal_specular"))
			{
				OutType = ERuntimeVirtualTextureMaterialType::
					BaseColor_Normal_Specular;
			}
			else if (
				Value
				== TEXT("base_color_normal_specular_ycocg"))
			{
				OutType = ERuntimeVirtualTextureMaterialType::
					BaseColor_Normal_Specular_YCoCg;
			}
			else if (
				Value
				== TEXT(
					"base_color_normal_specular_mask_ycocg"))
			{
				OutType = ERuntimeVirtualTextureMaterialType::
					BaseColor_Normal_Specular_Mask_YCoCg;
			}
			else if (Value == TEXT("world_height"))
			{
				OutType =
					ERuntimeVirtualTextureMaterialType::WorldHeight;
			}
			else if (Value == TEXT("displacement"))
			{
				OutType =
					ERuntimeVirtualTextureMaterialType::Displacement;
			}
			else
			{
				return Fail(OutError, JsonFailure(TEXT("asset_data_core_enum_invalid"), TEXT("ERuntimeVirtualTextureMaterialType name is not supported."), Field, MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("base_color, mask4, base_color_normal_roughness, base_color_normal_specular, base_color_normal_specular_ycocg, base_color_normal_specular_mask_ycocg, world_height, displacement"))));
			}
			return true;
		}

		bool TryReadRvtProperties(
			URuntimeVirtualTexture* Target,
			int32& OutTileCount,
			ERuntimeVirtualTextureMaterialType& OutMaterialType,
			bool& bOutCompression, FExecutionMutationError& OutError)
		{
			FIntProperty* TileCount =
				FindFProperty<FIntProperty>(
					URuntimeVirtualTexture::StaticClass(),
					TEXT("TileCount"));
			FEnumProperty* MaterialType =
				FindFProperty<FEnumProperty>(
					URuntimeVirtualTexture::StaticClass(),
					TEXT("MaterialType"));
			FBoolProperty* Compression =
				FindFProperty<FBoolProperty>(
					URuntimeVirtualTexture::StaticClass(),
					TEXT("bCompressTextures"));
			if (!Target || !TileCount || !MaterialType || !Compression)
			{
				const FString Field = !Target ? TEXT("target") : !TileCount ? TEXT("TileCount") : !MaterialType ? TEXT("MaterialType") : TEXT("bCompressTextures");
                OutError = NativeFailure(TEXT("asset_data_core_rvt_property_invalid"), !Target ? TEXT("Reading virtual texture properties requires a target.") : TEXT("A required native virtual texture property is missing or has an incompatible type."), Field, !Target ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()), MakeShared<FJsonValueString>(!Target ? URuntimeVirtualTexture::StaticClass()->GetPathName() : !TileCount ? TEXT("int32 property") : !MaterialType ? TEXT("enum property") : TEXT("boolean property")));
                if (Target) OutError.TargetPath = Target->GetPathName();
                return false;
			}
			OutTileCount =
				TileCount->GetPropertyValue_InContainer(Target);
			OutMaterialType =
				static_cast<ERuntimeVirtualTextureMaterialType>(
					MaterialType->GetUnderlyingProperty()
						->GetUnsignedIntPropertyValue(
							MaterialType->ContainerPtrToValuePtr<void>(
								Target)));
			bOutCompression =
				Compression->GetPropertyValue_InContainer(Target);
			return true;
		}

		bool TryWriteRvtProperties(
			UObject* Object,
			const int32 TileCountValue,
			const ERuntimeVirtualTextureMaterialType MaterialTypeValue,
			const bool bCompression,
			const bool bModify, FExecutionMutationError& OutError)
		{
            URuntimeVirtualTexture* Target = Cast<URuntimeVirtualTexture>(Object);
			FIntProperty* TileCount =
				FindFProperty<FIntProperty>(
					URuntimeVirtualTexture::StaticClass(),
					TEXT("TileCount"));
			FEnumProperty* MaterialType =
				FindFProperty<FEnumProperty>(
					URuntimeVirtualTexture::StaticClass(),
					TEXT("MaterialType"));
			FBoolProperty* Compression =
				FindFProperty<FBoolProperty>(
					URuntimeVirtualTexture::StaticClass(),
					TEXT("bCompressTextures"));
			if (!Target || !TileCount || !MaterialType || !Compression)
			{
                const FString Field = !Target ? TEXT("target") : !TileCount ? TEXT("TileCount")
                    : !MaterialType ? TEXT("MaterialType") : TEXT("bCompressTextures");
                OutError = NativeFailure(TEXT("asset_data_runtime_virtual_texture_write_invalid"),
                    !Target ? TEXT("Virtual texture writing requires a RuntimeVirtualTexture target.")
                        : TEXT("The required native virtual texture property is missing or has an incompatible type."),
                    Field, !Target ? ObjectClassValue(Object) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                    MakeShared<FJsonValueString>(!Target ? URuntimeVirtualTexture::StaticClass()->GetPathName()
                        : !TileCount ? TEXT("int32 property") : !MaterialType ? TEXT("enum property") : TEXT("boolean property")));
                if (Object) OutError.TargetPath = Object->GetPathName();
                return false;
			}
			if (bModify)
			{
				Target->Modify();
			}
			TileCount->SetPropertyValue_InContainer(
				Target,
				TileCountValue);
			MaterialType->GetUnderlyingProperty()
				->SetIntPropertyValue(
					MaterialType->ContainerPtrToValuePtr<void>(
						Target),
					static_cast<uint64>(MaterialTypeValue));
			Compression->SetPropertyValue_InContainer(
				Target,
				bCompression);
			Target->PostEditChange();
			return true;
		}

		bool TryParseTextureAddress(
			const FString& Name,
			TEnumAsByte<TextureAddress>& OutAddress, const FString& Field, FExecutionMutationError& OutError)
		{
			const FString Value = Name.TrimStartAndEnd().ToLower();
			if (Value == TEXT("wrap"))
			{
				OutAddress = TA_Wrap;
			}
			else if (Value == TEXT("clamp"))
			{
				OutAddress = TA_Clamp;
			}
			else if (Value == TEXT("mirror"))
			{
				OutAddress = TA_Mirror;
			}
			else
			{
				return Fail(OutError, JsonFailure(TEXT("asset_data_core_enum_invalid"), TEXT("TextureAddress name is not supported."), Field, MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("wrap, clamp, mirror"))));
			}
			return true;
		}

		bool TryFormatTextureAddress(
			const TEnumAsByte<TextureAddress> Address,
			FString& OutName, const FString& Field, FExecutionMutationError& OutError)
		{
			switch (Address.GetValue())
			{
			case TA_Wrap:
				OutName = TEXT("wrap");
				return true;
			case TA_Clamp:
				OutName = TEXT("clamp");
				return true;
			case TA_Mirror:
				OutName = TEXT("mirror");
				return true;
			default:
				OutName.Reset();
				return Fail(OutError, NativeFailure(TEXT("asset_data_core_native_enum_invalid"), TEXT("The native TextureAddress value is not supported by this projection."), Field, MakeShared<FJsonValueNumber>(static_cast<int32>(Address.GetValue())), MakeShared<FJsonValueString>(TEXT("wrap, clamp, mirror"))));
			}
		}

		bool TryFormatRuntimeVirtualTextureMaterialType(
			const ERuntimeVirtualTextureMaterialType Type,
			FString& OutName, const FString& Field, FExecutionMutationError& OutError)
		{
			switch (Type)
			{
			case ERuntimeVirtualTextureMaterialType::BaseColor:
				OutName = TEXT("base_color");
				return true;
			case ERuntimeVirtualTextureMaterialType::Mask4:
				OutName = TEXT("mask4");
				return true;
			case ERuntimeVirtualTextureMaterialType::
				BaseColor_Normal_Roughness:
				OutName = TEXT("base_color_normal_roughness");
				return true;
			case ERuntimeVirtualTextureMaterialType::
				BaseColor_Normal_Specular:
				OutName = TEXT("base_color_normal_specular");
				return true;
			case ERuntimeVirtualTextureMaterialType::
				BaseColor_Normal_Specular_YCoCg:
				OutName =
					TEXT("base_color_normal_specular_ycocg");
				return true;
			case ERuntimeVirtualTextureMaterialType::
				BaseColor_Normal_Specular_Mask_YCoCg:
				OutName = TEXT(
					"base_color_normal_specular_mask_ycocg");
				return true;
			case ERuntimeVirtualTextureMaterialType::WorldHeight:
				OutName = TEXT("world_height");
				return true;
			case ERuntimeVirtualTextureMaterialType::Displacement:
				OutName = TEXT("displacement");
				return true;
			default:
				OutName.Reset();
				return Fail(OutError, NativeFailure(TEXT("asset_data_core_native_enum_invalid"), TEXT("The native ERuntimeVirtualTextureMaterialType value is not supported by this projection."), Field, MakeShared<FJsonValueNumber>(static_cast<int32>(Type)), MakeShared<FJsonValueString>(TEXT("base_color, mask4, base_color_normal_roughness, base_color_normal_specular, base_color_normal_specular_ycocg, base_color_normal_specular_mask_ycocg, world_height, displacement"))));
			}
		}

		bool TryBuildStringProjectionValue(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError, const FString& Field)
		{
			if (FUeAgentCanonicalValue::TryString(
					Text,
					Resources,
					OutValue,
					OutError)) return true;
            QualifyCoreValueFailure(OutError, Field, true);
            OutError.PropertyPath = Field;
            return false;
		}

		bool TryBuildIntegerProjectionValue(
			const int32 Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError, const FString& Field)
		{
			if (FUeAgentCanonicalValue::TryInteger(
					FString::FromInt(Value),
					Resources,
					OutValue,
					OutError)) return true;
            QualifyCoreValueFailure(OutError, Field, true);
            OutError.PropertyPath = Field;
            return false;
		}

		bool TryBuildDecimalProjectionValue(
			const float Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError, const FString& Field)
		{
			if (!FMath::IsFinite(Value))
                return Fail(OutError, NativeFailure(TEXT("asset_data_core_projection_decimal_invalid"),
                    TEXT("The native number must be finite to build the projection."), Field,
                    MakeShared<FJsonValueString>(FString::Printf(TEXT("%g"), static_cast<double>(Value))),
                    MakeShared<FJsonValueString>(TEXT("finite float"))));
            if (FUeAgentCanonicalValue::TryDecimal(
					FString::SanitizeFloat(
						static_cast<double>(Value)),
					Resources,
					OutValue,
					OutError)) return true;
            QualifyCoreValueFailure(OutError, Field, true);
            OutError.PropertyPath = Field;
            return false;
		}

		bool TryBuildBooleanProjectionValue(
			const bool bValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError, const FString& Field)
		{
			if (FUeAgentCanonicalValue::TryBoolean(
					bValue,
					Resources,
					OutValue,
					OutError)) return true;
            QualifyCoreValueFailure(OutError, Field, true);
            OutError.PropertyPath = Field;
            return false;
		}

		bool TryBuildObjectProjectionValue(
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError, const FString& Field)
		{
			Items.Sort(
				[](
					const TPair<
						FString,
						FUeAgentCanonicalValueRef>& Left,
					const TPair<
						FString,
						FUeAgentCanonicalValueRef>& Right)
				{
					return Left.Key < Right.Key;
				});
			if (FUeAgentCanonicalValue::TryObject(
					Items,
					Resources,
					OutValue,
					OutError)) return true;
            QualifyCoreValueFailure(OutError, Field, true);
            return false;
		}

		bool TryBuildArrayProjectionValue(
			const TArray<FUeAgentCanonicalValueRef>& Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError, const FString& Field)
		{
			if (FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					OutValue,
					OutError)) return true;
            QualifyCoreValueFailure(OutError, Field, true);
            return false;
		}

		bool TryBuildNullProjectionValue(
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError, const FString& Field)
		{
			if (FUeAgentCanonicalValue::TryNull(
					Resources,
					OutValue,
					OutError)) return true;
            QualifyCoreValueFailure(OutError, Field, true);
            OutError.PropertyPath = Field;
            return false;
		}

		bool TryBuildObjectReferenceProjection(
			const UObject* Object,
			const bool bNullable,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError, const FString& Field)
		{
			if (!Object)
			{
				return bNullable
					? TryBuildNullProjectionValue(
						Resources,
						OutValue,
						OutError, Field)
                    : Fail(OutError, NativeFailure(TEXT("asset_data_core_projection_reference_missing"),
                        TEXT("The native object reference is unavailable and this field is not nullable."),
                        Field, MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("live object reference"))));
			}
			return TryBuildStringProjectionValue(
				Object->GetPathName(),
				Resources,
				OutValue,
				OutError, Field);
		}

		bool TryBuildObjectReferenceArrayProjection(
			const TArray<UObject*>& Objects,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError, const FString& Field)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			TSet<const UObject*> Seen;
			for (const UObject* Object : Objects)
			{
                const FString ItemPath = Field + TEXT("/") + FString::FromInt(Items.Num());
                if (!Object) return Fail(OutError, NativeFailure(TEXT("asset_data_core_projection_reference_missing"),
                    TEXT("The native reference array contains an unavailable object."), ItemPath,
                    MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("live object reference"))));
                if (Seen.Contains(Object)) return Fail(OutError, NativeFailure(TEXT("asset_data_core_projection_reference_duplicate"),
                    TEXT("The native reference array contains the same object more than once."), ItemPath,
                    MakeShared<FJsonValueString>(Object->GetPathName()), MakeShared<FJsonValueString>(TEXT("unique object reference"))));
                FUeAgentCanonicalValuePtr Item;
                if (!TryBuildObjectReferenceProjection(Object, false, Resources, Item, OutError, ItemPath)) return false;
				Seen.Add(Object);
				Items.Add(Item.ToSharedRef());
			}
			return TryBuildArrayProjectionValue(
				Items,
				Resources,
				OutValue,
				OutError, Field);
		}

		bool TryFormatDistanceAlgorithm(
			const EAttenuationDistanceModel Value,
			FString& OutName, const FString& Field, FExecutionMutationError& OutError)
		{
			switch (Value)
			{
			case EAttenuationDistanceModel::Linear:
				OutName = TEXT("linear");
				return true;
			case EAttenuationDistanceModel::Logarithmic:
				OutName = TEXT("logarithmic");
				return true;
			case EAttenuationDistanceModel::Inverse:
				OutName = TEXT("inverse");
				return true;
			case EAttenuationDistanceModel::LogReverse:
				OutName = TEXT("log_reverse");
				return true;
			case EAttenuationDistanceModel::NaturalSound:
				OutName = TEXT("natural_sound");
				return true;
			case EAttenuationDistanceModel::Custom:
				OutName = TEXT("custom");
				return true;
			default:
				return Fail(OutError, NativeFailure(TEXT("asset_data_core_native_enum_invalid"), TEXT("The native EAttenuationDistanceModel value is not supported by this projection."), Field, MakeShared<FJsonValueNumber>(static_cast<int32>(Value)), MakeShared<FJsonValueString>(TEXT("linear, logarithmic, inverse, log_reverse, natural_sound, custom"))));
			}
		}

		bool TryFormatConcurrencyResolutionRule(
			const EMaxConcurrentResolutionRule::Type Value,
			FString& OutName, const FString& Field, FExecutionMutationError& OutError)
		{
			switch (Value)
			{
			case EMaxConcurrentResolutionRule::PreventNew:
				OutName = TEXT("prevent_new");
				return true;
			case EMaxConcurrentResolutionRule::StopOldest:
				OutName = TEXT("stop_oldest");
				return true;
			case EMaxConcurrentResolutionRule::
				StopFarthestThenOldest:
				OutName = TEXT("stop_farthest_then_oldest");
				return true;
			case EMaxConcurrentResolutionRule::
				StopFarthestThenPreventNew:
				OutName =
					TEXT("stop_farthest_then_prevent_new");
				return true;
			case EMaxConcurrentResolutionRule::StopLowestPriority:
				OutName = TEXT("stop_lowest_priority");
				return true;
			case EMaxConcurrentResolutionRule::StopQuietest:
				OutName = TEXT("stop_quietest");
				return true;
			case EMaxConcurrentResolutionRule::
				StopLowestPriorityThenPreventNew:
				OutName =
					TEXT(
						"stop_lowest_priority_then_prevent_new");
				return true;
			default:
				return Fail(OutError, NativeFailure(TEXT("asset_data_core_native_enum_invalid"), TEXT("The native EMaxConcurrentResolutionRule value is not supported by this projection."), Field, MakeShared<FJsonValueNumber>(static_cast<int32>(Value)), MakeShared<FJsonValueString>(TEXT("prevent_new, stop_oldest, stop_farthest_then_oldest, stop_farthest_then_prevent_new, stop_lowest_priority, stop_quietest, stop_lowest_priority_then_prevent_new"))));
			}
		}

		bool TryFormatConcurrencyVolumeScaleMode(
			const EConcurrencyVolumeScaleMode Value,
			FString& OutName, const FString& Field, FExecutionMutationError& OutError)
		{
			switch (Value)
			{
			case EConcurrencyVolumeScaleMode::Default:
				OutName = TEXT("default");
				return true;
			case EConcurrencyVolumeScaleMode::Distance:
				OutName = TEXT("distance");
				return true;
			case EConcurrencyVolumeScaleMode::Priority:
				OutName = TEXT("priority");
				return true;
			default:
				return Fail(OutError, NativeFailure(TEXT("asset_data_core_native_enum_invalid"), TEXT("The native EConcurrencyVolumeScaleMode value is not supported by this projection."), Field, MakeShared<FJsonValueNumber>(static_cast<int32>(Value)), MakeShared<FJsonValueString>(TEXT("default, distance, priority"))));
			}
		}

		bool TryFormatInterpMode(
			const ERichCurveInterpMode Value,
			FString& OutName, const FString& Field, FExecutionMutationError& OutError)
		{
			switch (Value)
			{
			case RCIM_Constant:
				OutName = TEXT("constant");
				return true;
			case RCIM_Linear:
				OutName = TEXT("linear");
				return true;
			case RCIM_Cubic:
				OutName = TEXT("cubic");
				return true;
			default:
				return Fail(OutError, NativeFailure(TEXT("asset_data_core_native_enum_invalid"), TEXT("The native ERichCurveInterpMode value is not supported by this projection."), Field, MakeShared<FJsonValueNumber>(static_cast<int32>(Value)), MakeShared<FJsonValueString>(TEXT("constant, linear, cubic"))));
			}
		}

		bool TryFormatTangentMode(
			const ERichCurveTangentMode Value,
			FString& OutName, const FString& Field, FExecutionMutationError& OutError)
		{
			switch (Value)
			{
			case RCTM_Auto:
				OutName = TEXT("auto");
				return true;
			case RCTM_User:
				OutName = TEXT("user");
				return true;
			case RCTM_Break:
				OutName = TEXT("break");
				return true;
			case RCTM_SmartAuto:
				OutName = TEXT("smart_auto");
				return true;
			case RCTM_None:
				OutName = TEXT("none");
				return true;
			default:
				return Fail(OutError, NativeFailure(TEXT("asset_data_core_native_enum_invalid"), TEXT("The native ERichCurveTangentMode value is not supported by this projection."), Field, MakeShared<FJsonValueNumber>(static_cast<int32>(Value)), MakeShared<FJsonValueString>(TEXT("auto, user, break, smart_auto, none"))));
			}
		}

		bool TryFormatTangentWeightMode(
			const ERichCurveTangentWeightMode Value,
			FString& OutName, const FString& Field, FExecutionMutationError& OutError)
		{
			switch (Value)
			{
			case RCTWM_WeightedNone:
				OutName = TEXT("none");
				return true;
			case RCTWM_WeightedArrive:
				OutName = TEXT("arrive");
				return true;
			case RCTWM_WeightedLeave:
				OutName = TEXT("leave");
				return true;
			case RCTWM_WeightedBoth:
				OutName = TEXT("both");
				return true;
			default:
				return Fail(OutError, NativeFailure(TEXT("asset_data_core_native_enum_invalid"), TEXT("The native ERichCurveTangentWeightMode value is not supported by this projection."), Field, MakeShared<FJsonValueNumber>(static_cast<int32>(Value)), MakeShared<FJsonValueString>(TEXT("none, arrive, leave, both"))));
			}
		}

		bool TryBuildFloatArrayProjection(
			const TArray<float>& Values,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError, const FString& Field)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const float Value : Values)
			{
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildDecimalProjectionValue(
						Value,
						Resources,
						Item,
						OutError, Field + TEXT("/") + FString::FromInt(Items.Num())))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return TryBuildArrayProjectionValue(
				Items,
				Resources,
				OutValue,
				OutError, Field);
		}

		bool TryBuildEqSettingsProjection(
			const FSubmixEffectSubmixEQSettings& Settings,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
            const FString Field = TEXT("settings");
			TArray<FUeAgentCanonicalValueRef> Bands;
			for (const FSubmixEffectEQBand& Band : Settings.EQBands)
			{
                const FString BandPath = ChildPath(Field, TEXT("eq_bands")) + TEXT("/") + FString::FromInt(Bands.Num());
				FUeAgentCanonicalValuePtr Frequency;
				FUeAgentCanonicalValuePtr Bandwidth;
				FUeAgentCanonicalValuePtr Gain;
				FUeAgentCanonicalValuePtr Enabled;
				FUeAgentCanonicalValuePtr BandValue;
				if (!TryBuildDecimalProjectionValue(
						Band.Frequency,
						Resources,
						Frequency,
						OutError, ChildPath(BandPath, TEXT("frequency_hz")))
					|| !TryBuildDecimalProjectionValue(
						Band.Bandwidth,
						Resources,
						Bandwidth,
						OutError, ChildPath(BandPath, TEXT("bandwidth_octaves")))
					|| !TryBuildDecimalProjectionValue(
						Band.GainDb,
						Resources,
						Gain,
						OutError, ChildPath(BandPath, TEXT("gain_db")))
					|| !TryBuildBooleanProjectionValue(
						Band.bEnabled,
						Resources,
						Enabled,
						OutError, ChildPath(BandPath, TEXT("enabled")))
					|| !TryBuildObjectProjectionValue(
						{
							{
								TEXT("frequency_hz"),
								Frequency.ToSharedRef(),
							},
							{
								TEXT("bandwidth_octaves"),
								Bandwidth.ToSharedRef(),
							},
							{
								TEXT("gain_db"),
								Gain.ToSharedRef(),
							},
							{
								TEXT("enabled"),
								Enabled.ToSharedRef(),
							},
						},
						Resources,
						BandValue,
						OutError, BandPath))
				{
					return false;
				}
				Bands.Add(BandValue.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr BandArray;
			return TryBuildArrayProjectionValue(
					Bands,
					Resources,
					BandArray,
					OutError, ChildPath(Field, TEXT("eq_bands")))
				&& TryBuildObjectProjectionValue(
					{
						{
							TEXT("eq_bands"),
							BandArray.ToSharedRef(),
						},
					},
					Resources,
					OutValue,
					OutError, Field);
		}

		bool TryBuildAttenuationProjection(
			const FSoundAttenuationSettings& Settings,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
            const FString Field = TEXT("attenuation");
			FString DistanceName;
			const FString SpatializationName =
				Settings.SpatializationAlgorithm
						== SPATIALIZATION_Default
					? TEXT("panning")
					: Settings.SpatializationAlgorithm
							== SPATIALIZATION_HRTF
						? TEXT("binaural")
						: FString();
            if (!TryFormatDistanceAlgorithm(Settings.DistanceAlgorithm, DistanceName, ChildPath(Field, TEXT("distance_algorithm")), OutError)) return false;
            if (Settings.AttenuationShape != EAttenuationShape::Sphere)
                return Fail(OutError, NativeFailure(TEXT("asset_data_sound_attenuation_projection_unsupported"),
                    TEXT("This projection supports only the sphere attenuation shape."), ChildPath(Field, TEXT("attenuation_shape")),
                    MakeShared<FJsonValueNumber>(static_cast<int32>(Settings.AttenuationShape)), MakeShared<FJsonValueString>(TEXT("sphere"))));
            if (SpatializationName.IsEmpty())
                return Fail(OutError, NativeFailure(TEXT("asset_data_sound_attenuation_projection_unsupported"),
                    TEXT("The native spatialization algorithm is not supported."), ChildPath(Field, TEXT("spatialization_algorithm")),
                    MakeShared<FJsonValueNumber>(static_cast<int32>(Settings.SpatializationAlgorithm)), MakeShared<FJsonValueString>(TEXT("panning or binaural"))));
			FUeAgentCanonicalValuePtr Attenuate;
			FUeAgentCanonicalValuePtr Spatialize;
			FUeAgentCanonicalValuePtr Distance;
			FUeAgentCanonicalValuePtr Shape;
			FUeAgentCanonicalValuePtr InnerRadius;
			FUeAgentCanonicalValuePtr Falloff;
			FUeAgentCanonicalValuePtr Spatialization;
			FUeAgentCanonicalValuePtr Occlusion;
			FUeAgentCanonicalValuePtr OcclusionLowPass;
			FUeAgentCanonicalValuePtr OcclusionVolume;
			FUeAgentCanonicalValuePtr OcclusionTime;
			return TryBuildBooleanProjectionValue(
					Settings.bAttenuate,
					Resources,
					Attenuate,
					OutError, ChildPath(Field, TEXT("attenuate")))
				&& TryBuildBooleanProjectionValue(
					Settings.bSpatialize,
					Resources,
					Spatialize,
					OutError, ChildPath(Field, TEXT("spatialize")))
				&& TryBuildStringProjectionValue(
					DistanceName,
					Resources,
					Distance,
					OutError, ChildPath(Field, TEXT("distance_algorithm")))
				&& TryBuildStringProjectionValue(
					TEXT("sphere"),
					Resources,
					Shape,
					OutError, ChildPath(Field, TEXT("attenuation_shape")))
				&& TryBuildDecimalProjectionValue(
					Settings.AttenuationShapeExtents.X,
					Resources,
					InnerRadius,
					OutError, ChildPath(Field, TEXT("inner_radius")))
				&& TryBuildDecimalProjectionValue(
					Settings.FalloffDistance,
					Resources,
					Falloff,
					OutError, ChildPath(Field, TEXT("falloff_distance")))
				&& TryBuildStringProjectionValue(
					SpatializationName,
					Resources,
					Spatialization,
					OutError, ChildPath(Field, TEXT("spatialization_algorithm")))
				&& TryBuildBooleanProjectionValue(
					Settings.bEnableOcclusion,
					Resources,
					Occlusion,
					OutError, ChildPath(Field, TEXT("enable_occlusion")))
				&& TryBuildDecimalProjectionValue(
					Settings.OcclusionLowPassFilterFrequency,
					Resources,
					OcclusionLowPass,
					OutError, ChildPath(Field, TEXT("occlusion_low_pass_filter_frequency")))
				&& TryBuildDecimalProjectionValue(
					Settings.OcclusionVolumeAttenuation,
					Resources,
					OcclusionVolume,
					OutError, ChildPath(Field, TEXT("occlusion_volume_attenuation")))
				&& TryBuildDecimalProjectionValue(
					Settings.OcclusionInterpolationTime,
					Resources,
					OcclusionTime,
					OutError, ChildPath(Field, TEXT("occlusion_interpolation_time")))
				&& TryBuildObjectProjectionValue(
					{
						{TEXT("attenuate"), Attenuate.ToSharedRef()},
						{TEXT("spatialize"), Spatialize.ToSharedRef()},
						{
							TEXT("distance_algorithm"),
							Distance.ToSharedRef(),
						},
						{
							TEXT("attenuation_shape"),
							Shape.ToSharedRef(),
						},
						{
							TEXT("inner_radius"),
							InnerRadius.ToSharedRef(),
						},
						{
							TEXT("falloff_distance"),
							Falloff.ToSharedRef(),
						},
						{
							TEXT("spatialization_algorithm"),
							Spatialization.ToSharedRef(),
						},
						{
							TEXT("enable_occlusion"),
							Occlusion.ToSharedRef(),
						},
						{
							TEXT(
								"occlusion_low_pass_filter_frequency"),
							OcclusionLowPass.ToSharedRef(),
						},
						{
							TEXT(
								"occlusion_volume_attenuation"),
							OcclusionVolume.ToSharedRef(),
						},
						{
							TEXT(
								"occlusion_interpolation_time"),
							OcclusionTime.ToSharedRef(),
						},
					},
					Resources,
					OutValue,
					OutError, Field);
		}

		bool TryBuildSoundClassPropertiesProjection(
			const FSoundClassProperties& Properties,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
            const FString Field = TEXT("properties");
			FUeAgentCanonicalValuePtr Volume;
			FUeAgentCanonicalValuePtr Pitch;
			FUeAgentCanonicalValuePtr UiSound;
			FUeAgentCanonicalValuePtr Music;
			return TryBuildDecimalProjectionValue(
					Properties.Volume,
					Resources,
					Volume,
					OutError, ChildPath(Field, TEXT("volume")))
				&& TryBuildDecimalProjectionValue(
					Properties.Pitch,
					Resources,
					Pitch,
					OutError, ChildPath(Field, TEXT("pitch")))
				&& TryBuildBooleanProjectionValue(
					Properties.bIsUISound,
					Resources,
					UiSound,
					OutError, ChildPath(Field, TEXT("is_ui_sound")))
				&& TryBuildBooleanProjectionValue(
					Properties.bIsMusic,
					Resources,
					Music,
					OutError, ChildPath(Field, TEXT("is_music")))
				&& TryBuildObjectProjectionValue(
					{
						{TEXT("volume"), Volume.ToSharedRef()},
						{TEXT("pitch"), Pitch.ToSharedRef()},
						{
							TEXT("is_ui_sound"),
							UiSound.ToSharedRef(),
						},
						{TEXT("is_music"), Music.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError, Field);
		}

		bool TryReadConcurrencyVolumeScale(
			const FSoundConcurrencySettings& Settings,
			float& OutValue, const FString& Field, FExecutionMutationError& OutError)
		{
			const FFloatProperty* Property =
				FindFProperty<FFloatProperty>(
					FSoundConcurrencySettings::StaticStruct(),
					TEXT("VolumeScale"));
			if (!Property)
			{
				return Fail(OutError, NativeFailure(TEXT("asset_data_core_concurrency_property_missing"),
                    TEXT("FSoundConcurrencySettings.VolumeScale is missing or is not a float property."),
                    Field, MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("float property"))));
			}
			OutValue =
				Property->GetPropertyValue_InContainer(&Settings);
			if (!FMath::IsFinite(OutValue))
                return Fail(OutError, NativeFailure(TEXT("asset_data_core_concurrency_volume_invalid"),
                    TEXT("The native concurrency volume scale must be finite."), Field,
                    MakeShared<FJsonValueString>(FString::Printf(TEXT("%g"), static_cast<double>(OutValue))), MakeShared<FJsonValueString>(TEXT("finite float"))));
            return true;
		}

		bool TryBuildConcurrencyProjection(
			const FSoundConcurrencySettings& Settings,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
            const FString Field = TEXT("concurrency");
			FString ResolutionName;
			FString ScaleModeName;
			float VolumeScale = 0.0f;
			if (!TryFormatConcurrencyResolutionRule(
					Settings.ResolutionRule,
					ResolutionName, ChildPath(Field, TEXT("resolution_rule")), OutError)
				|| !TryFormatConcurrencyVolumeScaleMode(
					Settings.VolumeScaleMode,
					ScaleModeName, ChildPath(Field, TEXT("volume_scale_mode")), OutError)
				|| !TryReadConcurrencyVolumeScale(
					Settings,
					VolumeScale, ChildPath(Field, TEXT("volume_scale")), OutError))
			{
                return false;
			}
			FUeAgentCanonicalValuePtr MaxCount;
			FUeAgentCanonicalValuePtr LimitToOwner;
			FUeAgentCanonicalValuePtr RetriggerTime;
			FUeAgentCanonicalValuePtr Resolution;
			FUeAgentCanonicalValuePtr Scale;
			FUeAgentCanonicalValuePtr ScaleMode;
			FUeAgentCanonicalValuePtr Attack;
			FUeAgentCanonicalValuePtr Release;
			FUeAgentCanonicalValuePtr VoiceSteal;
			return TryBuildIntegerProjectionValue(
					Settings.MaxCount,
					Resources,
					MaxCount,
					OutError, ChildPath(Field, TEXT("max_count")))
				&& TryBuildBooleanProjectionValue(
					Settings.bLimitToOwner,
					Resources,
					LimitToOwner,
					OutError, ChildPath(Field, TEXT("limit_to_owner")))
				&& TryBuildDecimalProjectionValue(
					Settings.RetriggerTime,
					Resources,
					RetriggerTime,
					OutError, ChildPath(Field, TEXT("retrigger_time")))
				&& TryBuildStringProjectionValue(
					ResolutionName,
					Resources,
					Resolution,
					OutError, ChildPath(Field, TEXT("resolution_rule")))
				&& TryBuildDecimalProjectionValue(
					VolumeScale,
					Resources,
					Scale,
					OutError, ChildPath(Field, TEXT("volume_scale")))
				&& TryBuildStringProjectionValue(
					ScaleModeName,
					Resources,
					ScaleMode,
					OutError, ChildPath(Field, TEXT("volume_scale_mode")))
				&& TryBuildDecimalProjectionValue(
					Settings.VolumeScaleAttackTime,
					Resources,
					Attack,
					OutError, ChildPath(Field, TEXT("volume_scale_attack_time")))
				&& TryBuildDecimalProjectionValue(
					Settings.VolumeScaleReleaseTime,
					Resources,
					Release,
					OutError, ChildPath(Field, TEXT("volume_scale_release_time")))
				&& TryBuildDecimalProjectionValue(
					Settings.VoiceStealReleaseTime,
					Resources,
					VoiceSteal,
					OutError, ChildPath(Field, TEXT("voice_steal_release_time")))
				&& TryBuildObjectProjectionValue(
					{
						{TEXT("max_count"), MaxCount.ToSharedRef()},
						{
							TEXT("limit_to_owner"),
							LimitToOwner.ToSharedRef(),
						},
						{
							TEXT("retrigger_time"),
							RetriggerTime.ToSharedRef(),
						},
						{
							TEXT("resolution_rule"),
							Resolution.ToSharedRef(),
						},
						{
							TEXT("volume_scale"),
							Scale.ToSharedRef(),
						},
						{
							TEXT("volume_scale_mode"),
							ScaleMode.ToSharedRef(),
						},
						{
							TEXT("volume_scale_attack_time"),
							Attack.ToSharedRef(),
						},
						{
							TEXT("volume_scale_release_time"),
							Release.ToSharedRef(),
						},
						{
							TEXT("voice_steal_release_time"),
							VoiceSteal.ToSharedRef(),
						},
					},
					Resources,
					OutValue,
					OutError, Field);
		}

		bool TryBuildSoundMixModifierProjection(
			const FSoundClassAdjuster& Modifier,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError, const FString& Field)
		{
			FUeAgentCanonicalValuePtr SoundClass;
			FUeAgentCanonicalValuePtr Volume;
			FUeAgentCanonicalValuePtr Pitch;
			FUeAgentCanonicalValuePtr LowPass;
			FUeAgentCanonicalValuePtr ApplyToChildren;
			FUeAgentCanonicalValuePtr CenterVolume;
			return TryBuildObjectReferenceProjection(
					Modifier.SoundClassObject,
					false,
					Resources,
					SoundClass,
					OutError, ChildPath(Field, TEXT("sound_class_ref")))
				&& TryBuildDecimalProjectionValue(
					Modifier.VolumeAdjuster,
					Resources,
					Volume,
					OutError, ChildPath(Field, TEXT("volume_adjuster")))
				&& TryBuildDecimalProjectionValue(
					Modifier.PitchAdjuster,
					Resources,
					Pitch,
					OutError, ChildPath(Field, TEXT("pitch_adjuster")))
				&& TryBuildDecimalProjectionValue(
					Modifier.LowPassFilterFrequency,
					Resources,
					LowPass,
					OutError, ChildPath(Field, TEXT("low_pass_filter_frequency")))
				&& TryBuildBooleanProjectionValue(
					Modifier.bApplyToChildren,
					Resources,
					ApplyToChildren,
					OutError, ChildPath(Field, TEXT("apply_to_children")))
				&& TryBuildDecimalProjectionValue(
					Modifier.VoiceCenterChannelVolumeAdjuster,
					Resources,
					CenterVolume,
					OutError, ChildPath(Field, TEXT("voice_center_channel_volume_adjuster")))
				&& TryBuildObjectProjectionValue(
					{
						{
							TEXT("sound_class_ref"),
							SoundClass.ToSharedRef(),
						},
						{
							TEXT("volume_adjuster"),
							Volume.ToSharedRef(),
						},
						{
							TEXT("pitch_adjuster"),
							Pitch.ToSharedRef(),
						},
						{
							TEXT("low_pass_filter_frequency"),
							LowPass.ToSharedRef(),
						},
						{
							TEXT("apply_to_children"),
							ApplyToChildren.ToSharedRef(),
						},
						{
							TEXT(
								"voice_center_channel_volume_adjuster"),
							CenterVolume.ToSharedRef(),
						},
					},
					Resources,
					OutValue,
					OutError, Field);
		}

		bool TryBuildSoundMixModifiersProjection(
			const TArray<FSoundClassAdjuster>& Modifiers,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
            const FString Field = TEXT("modifiers");
			TArray<FUeAgentCanonicalValueRef> Items;
			TSet<const USoundClass*> Seen;
			for (const FSoundClassAdjuster& Modifier : Modifiers)
			{
                const FString ItemPath = Field + TEXT("/") + FString::FromInt(Items.Num());
				if (!Modifier.SoundClassObject
					|| Seen.Contains(Modifier.SoundClassObject))
				{
                    return Fail(OutError, NativeFailure(TEXT("asset_data_sound_mix_projection_identity_invalid"),
                        !Modifier.SoundClassObject ? TEXT("The native modifier has no sound class.") : TEXT("More than one native modifier addresses this sound class."),
                        ChildPath(ItemPath, TEXT("sound_class_ref")),
                        Modifier.SoundClassObject ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Modifier.SoundClassObject->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                        MakeShared<FJsonValueString>(TEXT("unique live sound class"))));
				}
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildSoundMixModifierProjection(
						Modifier,
						Resources,
						Item,
						OutError, ItemPath))
				{
					return false;
				}
				Seen.Add(Modifier.SoundClassObject);
				Items.Add(Item.ToSharedRef());
			}
			return TryBuildArrayProjectionValue(
				Items,
				Resources,
				OutValue,
				OutError, Field);
		}

        TSharedPtr<FJsonValue> NativeNumberFact(const double Value)
        {
            if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
            return MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value));
        }

		bool TryBuildCurveChannelsProjection(
			const TArray<TArray<FRichCurveKey>>& ChannelValues,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
            const FString Field = TEXT("keys");
			const int32 Dimension = ChannelValues.Num();
            if (Dimension < 1 || Dimension > 4)
                return Fail(OutError, NativeFailure(TEXT("asset_data_curve_projection_target_invalid"),
                    TEXT("The native curve must have one to four channels."), TEXT("curve_dimension"),
                    MakeShared<FJsonValueNumber>(Dimension), MakeShared<FJsonValueString>(TEXT("integer from 1 to 4"))));
			TArray<const TArray<FRichCurveKey>*> Channels;
			for (const TArray<FRichCurveKey>& Channel
				: ChannelValues)
			{
				Channels.Add(&Channel);
			}
			const int32 KeyCount = Channels[0]->Num();
			for (int32 Index = 1; Index < Channels.Num(); ++Index)
			{
				if (Channels[Index]->Num() != KeyCount)
				{
                    return Fail(OutError, NativeFailure(TEXT("asset_data_curve_projection_channel_topology_unsupported"),
                        TEXT("Every curve channel must have the same number of keys as channel zero."),
                        Field + TEXT("/channels/") + FString::FromInt(Index),
                        MakeShared<FJsonValueNumber>(Channels[Index]->Num()), MakeShared<FJsonValueNumber>(KeyCount)));
				}
			}
			TArray<FUeAgentCanonicalValueRef> ProjectedKeys;
			for (int32 KeyIndex = 0;
				KeyIndex < KeyCount;
				++KeyIndex)
			{
				const FString KeyPath = Field + TEXT("/") + FString::FromInt(KeyIndex);
                const FRichCurveKey& First =
					(*Channels[0])[KeyIndex];
				FString InterpName;
				FString TangentName;
				FString WeightName;
				TArray<float> Values;
				TArray<float> ArriveTangents;
				TArray<float> LeaveTangents;
				TArray<float> ArriveWeights;
				TArray<float> LeaveWeights;
				for (int32 ChannelIndex = 0;
					ChannelIndex < Channels.Num();
					++ChannelIndex)
				{
					const FRichCurveKey& Key =
						(*Channels[ChannelIndex])[KeyIndex];
                    const FString ChannelPath = KeyPath + TEXT("/channels/") + FString::FromInt(ChannelIndex);
                    if (!FMath::IsNearlyEqual(Key.Time, First.Time))
                        return Fail(OutError, NativeFailure(TEXT("asset_data_curve_projection_channel_semantics_unsupported"),
                            TEXT("The key time must match the corresponding key in channel zero within the native tolerance."),
                            ChildPath(ChannelPath, TEXT("time")),
                            NativeNumberFact(Key.Time),
                            NativeNumberFact(First.Time)));
                    if (Key.InterpMode != First.InterpMode)
                        return Fail(OutError, NativeFailure(TEXT("asset_data_curve_projection_channel_semantics_unsupported"),
                            TEXT("Interpolation mode must match the corresponding key in channel zero."), ChildPath(ChannelPath, TEXT("interp_mode")),
                            MakeShared<FJsonValueNumber>(static_cast<int32>(Key.InterpMode)), MakeShared<FJsonValueNumber>(static_cast<int32>(First.InterpMode))));
                    if (Key.TangentMode != First.TangentMode)
                        return Fail(OutError, NativeFailure(TEXT("asset_data_curve_projection_channel_semantics_unsupported"),
                            TEXT("Tangent mode must match the corresponding key in channel zero."), ChildPath(ChannelPath, TEXT("tangent_mode")),
                            MakeShared<FJsonValueNumber>(static_cast<int32>(Key.TangentMode)), MakeShared<FJsonValueNumber>(static_cast<int32>(First.TangentMode))));
                    if (Key.TangentWeightMode != First.TangentWeightMode)
                        return Fail(OutError, NativeFailure(TEXT("asset_data_curve_projection_channel_semantics_unsupported"),
                            TEXT("Tangent weight mode must match the corresponding key in channel zero."), ChildPath(ChannelPath, TEXT("tangent_weight_mode")),
                            MakeShared<FJsonValueNumber>(static_cast<int32>(Key.TangentWeightMode)), MakeShared<FJsonValueNumber>(static_cast<int32>(First.TangentWeightMode))));
					Values.Add(Key.Value);
					ArriveTangents.Add(Key.ArriveTangent);
					LeaveTangents.Add(Key.LeaveTangent);
					ArriveWeights.Add(
						Key.ArriveTangentWeight);
					LeaveWeights.Add(
						Key.LeaveTangentWeight);
				}
				FUeAgentCanonicalValuePtr Time;
				FUeAgentCanonicalValuePtr ValueArray;
				FUeAgentCanonicalValuePtr Interp;
				FUeAgentCanonicalValuePtr Tangent;
				FUeAgentCanonicalValuePtr Weight;
				FUeAgentCanonicalValuePtr ArriveTangentArray;
				FUeAgentCanonicalValuePtr LeaveTangentArray;
				FUeAgentCanonicalValuePtr ArriveWeightArray;
				FUeAgentCanonicalValuePtr LeaveWeightArray;
				FUeAgentCanonicalValuePtr ProjectedKey;
				if (!TryFormatInterpMode(
						First.InterpMode,
						InterpName, ChildPath(KeyPath, TEXT("interp_mode")), OutError)
					|| !TryFormatTangentMode(
						First.TangentMode,
						TangentName, ChildPath(KeyPath, TEXT("tangent_mode")), OutError)
					|| !TryFormatTangentWeightMode(
						First.TangentWeightMode,
						WeightName, ChildPath(KeyPath, TEXT("tangent_weight_mode")), OutError)
					|| !TryBuildDecimalProjectionValue(
						First.Time,
						Resources,
						Time,
						OutError, ChildPath(KeyPath, TEXT("time")))
					|| !TryBuildFloatArrayProjection(
						Values,
						Resources,
						ValueArray,
						OutError, ChildPath(KeyPath, TEXT("values")))
					|| !TryBuildStringProjectionValue(
						InterpName,
						Resources,
						Interp,
						OutError, ChildPath(KeyPath, TEXT("interp_mode")))
					|| !TryBuildStringProjectionValue(
						TangentName,
						Resources,
						Tangent,
						OutError, ChildPath(KeyPath, TEXT("tangent_mode")))
					|| !TryBuildStringProjectionValue(
						WeightName,
						Resources,
						Weight,
						OutError, ChildPath(KeyPath, TEXT("tangent_weight_mode")))
					|| !TryBuildFloatArrayProjection(
						ArriveTangents,
						Resources,
						ArriveTangentArray,
						OutError, ChildPath(KeyPath, TEXT("arrive_tangents")))
					|| !TryBuildFloatArrayProjection(
						LeaveTangents,
						Resources,
						LeaveTangentArray,
						OutError, ChildPath(KeyPath, TEXT("leave_tangents")))
					|| !TryBuildFloatArrayProjection(
						ArriveWeights,
						Resources,
						ArriveWeightArray,
						OutError, ChildPath(KeyPath, TEXT("arrive_tangent_weights")))
					|| !TryBuildFloatArrayProjection(
						LeaveWeights,
						Resources,
						LeaveWeightArray,
						OutError, ChildPath(KeyPath, TEXT("leave_tangent_weights")))
					|| !TryBuildObjectProjectionValue(
						{
							{TEXT("time"), Time.ToSharedRef()},
							{
								TEXT("values"),
								ValueArray.ToSharedRef(),
							},
							{
								TEXT("interp_mode"),
								Interp.ToSharedRef(),
							},
							{
								TEXT("tangent_mode"),
								Tangent.ToSharedRef(),
							},
							{
								TEXT("tangent_weight_mode"),
								Weight.ToSharedRef(),
							},
							{
								TEXT("arrive_tangents"),
								ArriveTangentArray.ToSharedRef(),
							},
							{
								TEXT("leave_tangents"),
								LeaveTangentArray.ToSharedRef(),
							},
							{
								TEXT(
									"arrive_tangent_weights"),
								ArriveWeightArray.ToSharedRef(),
							},
							{
								TEXT(
									"leave_tangent_weights"),
								LeaveWeightArray.ToSharedRef(),
							},
						},
						Resources,
						ProjectedKey,
						OutError, KeyPath))
				{
					return false;
				}
				ProjectedKeys.Add(ProjectedKey.ToSharedRef());
			}
			return TryBuildArrayProjectionValue(
				ProjectedKeys,
				Resources,
				OutValue,
				OutError, Field);
		}

		bool TryBuildCurveKeysProjection(
			UObject* Target,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			const int32 Dimension = GetCurveDimension(Target);
            if (Dimension < 1 || Dimension > 4)
            {
                OutError = NativeFailure(TEXT("asset_data_curve_projection_target_invalid"),
                    TEXT("The target does not provide one to four supported curve channels."), TEXT("curve_dimension"),
                    MakeShared<FJsonValueNumber>(Dimension), MakeShared<FJsonValueString>(TEXT("CurveFloat, CurveVector or CurveLinearColor")));
                if (Target) OutError.TargetPath = Target->GetPathName();
                return false;
            }
			TArray<TArray<FRichCurveKey>> Channels;
			Channels.SetNum(Dimension);
			for (int32 Index = 0; Index < Dimension; ++Index)
			{
				FRichCurve* Curve = GetCurveChannel(Target, Index);
                if (!Curve)
                {
                    OutError = NativeFailure(TEXT("asset_data_curve_projection_channel_missing"),
                        TEXT("A native curve channel is unavailable."), TEXT("keys/channels/") + FString::FromInt(Index),
                        MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("rich curve channel")));
                    OutError.TargetPath = Target->GetPathName();
                    return false;
                }
				Channels[Index] = Curve->GetCopyOfKeys();
			}
			return TryBuildCurveChannelsProjection(
				Channels,
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildMediaSourceOptionsProjection(
			const bool bPrecacheFile,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
            const FString Field = TEXT("options");
			FUeAgentCanonicalValuePtr Precache;
			return TryBuildBooleanProjectionValue(
					bPrecacheFile,
					Resources,
					Precache,
					OutError, ChildPath(Field, TEXT("precache_file")))
				&& TryBuildObjectProjectionValue(
					{
						{
							TEXT("precache_file"),
							Precache.ToSharedRef(),
						},
					},
					Resources,
					OutValue,
					OutError, Field);
		}

		bool TryBuildSparseAddressingProjection(
			const TEnumAsByte<TextureAddress> X,
			const TEnumAsByte<TextureAddress> Y,
			const TEnumAsByte<TextureAddress> Z,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
            const FString Field = TEXT("addressing");
			FString XName;
			FString YName;
			FString ZName;
			FUeAgentCanonicalValuePtr XValue;
			FUeAgentCanonicalValuePtr YValue;
			FUeAgentCanonicalValuePtr ZValue;
			return TryFormatTextureAddress(X, XName, ChildPath(Field, TEXT("x")), OutError)
				&& TryFormatTextureAddress(Y, YName, ChildPath(Field, TEXT("y")), OutError)
				&& TryFormatTextureAddress(Z, ZName, ChildPath(Field, TEXT("z")), OutError)
				&& TryBuildStringProjectionValue(
					XName,
					Resources,
					XValue,
					OutError, ChildPath(Field, TEXT("x")))
				&& TryBuildStringProjectionValue(
					YName,
					Resources,
					YValue,
					OutError, ChildPath(Field, TEXT("y")))
				&& TryBuildStringProjectionValue(
					ZName,
					Resources,
					ZValue,
					OutError, ChildPath(Field, TEXT("z")))
				&& TryBuildObjectProjectionValue(
					{
						{TEXT("x"), XValue.ToSharedRef()},
						{TEXT("y"), YValue.ToSharedRef()},
						{TEXT("z"), ZValue.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError, Field);
		}

		bool TryBuildSparseStreamingProjection(
			const bool bLocalDdcOnly,
			const float PoolSizeFactor,
			const int32 PrefetchFrames,
			const float PrefetchPercentageStep,
			const float PrefetchPercentageBias,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
            const FString Field = TEXT("streaming");
			FUeAgentCanonicalValuePtr LocalDdcOnly;
			FUeAgentCanonicalValuePtr PoolSize;
			FUeAgentCanonicalValuePtr Frames;
			FUeAgentCanonicalValuePtr PercentageStep;
			FUeAgentCanonicalValuePtr PercentageBias;
			return TryBuildBooleanProjectionValue(
					bLocalDdcOnly,
					Resources,
					LocalDdcOnly,
					OutError, ChildPath(Field, TEXT("local_ddc_only")))
				&& TryBuildDecimalProjectionValue(
					PoolSizeFactor,
					Resources,
					PoolSize,
					OutError, ChildPath(Field, TEXT("pool_size_factor")))
				&& TryBuildIntegerProjectionValue(
					PrefetchFrames,
					Resources,
					Frames,
					OutError, ChildPath(Field, TEXT("prefetch_frames")))
				&& TryBuildDecimalProjectionValue(
					PrefetchPercentageStep,
					Resources,
					PercentageStep,
					OutError, ChildPath(Field, TEXT("prefetch_percentage_step")))
				&& TryBuildDecimalProjectionValue(
					PrefetchPercentageBias,
					Resources,
					PercentageBias,
					OutError, ChildPath(Field, TEXT("prefetch_percentage_bias")))
				&& TryBuildObjectProjectionValue(
					{
						{
							TEXT("local_ddc_only"),
							LocalDdcOnly.ToSharedRef(),
						},
						{
							TEXT("pool_size_factor"),
							PoolSize.ToSharedRef(),
						},
						{
							TEXT("prefetch_frames"),
							Frames.ToSharedRef(),
						},
						{
							TEXT("prefetch_percentage_step"),
							PercentageStep.ToSharedRef(),
						},
						{
							TEXT("prefetch_percentage_bias"),
							PercentageBias.ToSharedRef(),
						},
					},
					Resources,
					OutValue,
					OutError, Field);
		}

		const TCHAR* FindCoreAudioCurveContextTag(
			const ECoreAssetKind Kind,
			const FString& FieldId)
		{
			switch (Kind)
			{
			case ECoreAssetKind::AudioEffectPreset:
				return FieldId == TEXT("settings")
					? TEXT("eq_settings")
					: nullptr;
			case ECoreAssetKind::SoundAttenuation:
				return FieldId == TEXT("attenuation")
					? TEXT("attenuation_settings")
					: nullptr;
			case ECoreAssetKind::SoundClass:
				if (FieldId == TEXT("properties"))
				{
					return TEXT("sound_class_properties");
				}
				if (FieldId == TEXT("child_classes"))
				{
					return TEXT("ordered_asset_refs");
				}
				return nullptr;
			case ECoreAssetKind::SoundConcurrency:
				return FieldId == TEXT("concurrency")
					? TEXT("concurrency_settings")
					: nullptr;
			case ECoreAssetKind::SoundMix:
				if (FieldId == TEXT("modifiers"))
				{
					return TEXT("sound_mix_modifiers");
				}
				if (FieldId == TEXT("initial_delay"))
				{
					return TEXT("decimal");
				}
				return nullptr;
			case ECoreAssetKind::SoundSubmix:
				if (FieldId == TEXT("parent"))
				{
					return TEXT("asset_ref_or_null");
				}
				if (FieldId == TEXT("effect_chain"))
				{
					return TEXT("ordered_asset_refs");
				}
				return nullptr;
			case ECoreAssetKind::Curve:
				return FieldId == TEXT("keys")
					? TEXT("curve_rich_keys")
					: nullptr;
			default:
				return nullptr;
			}
		}

        TSharedPtr<FJsonValue> ProjectionFieldIdsValue(const TMap<FString, FUeAgentCanonicalValueRef>& Fields)
        {
            TArray<FString> Names;
            Fields.GetKeys(Names);
            Names.Sort();
            TArray<TSharedPtr<FJsonValue>> Values;
            for (const auto& Name : Names) Values.Add(MakeShared<FJsonValueString>(Name));
            return MakeShared<FJsonValueArray>(Values);
        }

        bool FailProjectionTarget(UObject* Target, UClass* ExpectedClass, FExecutionMutationError& OutError)
        {
            OutError = NativeFailure(TEXT("asset_data_core_projection_target_type_invalid"),
                TEXT("The target class is incompatible with this projection mechanic."), TEXT("target"),
                ObjectClassValue(Target), MakeShared<FJsonValueString>(ExpectedClass->GetPathName()));
            if (Target) OutError.TargetPath = Target->GetPathName();
            return false;
        }

		bool TryBuildCoreAudioCurveValueMap(
			const ECoreAssetKind Kind,
			UObject* Target,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
            ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty() && Target) OutError.TargetPath = Target->GetPathName(); };
			FUeAgentCanonicalValuePtr First;
			FUeAgentCanonicalValuePtr Second;
			switch (Kind)
			{
			case ECoreAssetKind::AudioEffectPreset:
				{
					const USubmixEffectSubmixEQPreset* Preset =
						Cast<USubmixEffectSubmixEQPreset>(Target);
					if (!Preset) return FailProjectionTarget(Target, USubmixEffectSubmixEQPreset::StaticClass(), OutError);
                    if (!TryBuildEqSettingsProjection(
							Preset->Settings,
							Resources,
							First,
							OutError))
					{
						return false;
					}
					OutValues.Add(
						TEXT("settings"),
						First.ToSharedRef());
					return true;
				}
			case ECoreAssetKind::SoundAttenuation:
				{
					const USoundAttenuation* Attenuation =
						Cast<USoundAttenuation>(Target);
					if (!Attenuation) return FailProjectionTarget(Target, USoundAttenuation::StaticClass(), OutError);
                    if (!TryBuildAttenuationProjection(
							Attenuation->Attenuation,
							Resources,
							First,
							OutError))
					{
						return false;
					}
					OutValues.Add(
						TEXT("attenuation"),
						First.ToSharedRef());
					return true;
				}
			case ECoreAssetKind::SoundClass:
				{
					const USoundClass* SoundClass =
						Cast<USoundClass>(Target);
					TArray<UObject*> Children;
                    if (!SoundClass) return FailProjectionTarget(Target, USoundClass::StaticClass(), OutError);
					for (USoundClass* Child
						: SoundClass->ChildClasses)
					{
						Children.Add(Child);
					}
					if (!TryBuildSoundClassPropertiesProjection(
							SoundClass->Properties,
							Resources,
							First,
							OutError)
						|| !TryBuildObjectReferenceArrayProjection(
							Children,
							Resources,
							Second,
							OutError, TEXT("child_classes")))
					{
						return false;
					}
					OutValues.Add(
						TEXT("properties"),
						First.ToSharedRef());
					OutValues.Add(
						TEXT("child_classes"),
						Second.ToSharedRef());
					return true;
				}
			case ECoreAssetKind::SoundConcurrency:
				{
					const USoundConcurrency* Concurrency =
						Cast<USoundConcurrency>(Target);
					if (!Concurrency) return FailProjectionTarget(Target, USoundConcurrency::StaticClass(), OutError);
                    if (!TryBuildConcurrencyProjection(
							Concurrency->Concurrency,
							Resources,
							First,
							OutError))
					{
						return false;
					}
					OutValues.Add(
						TEXT("concurrency"),
						First.ToSharedRef());
					return true;
				}
			case ECoreAssetKind::SoundMix:
				{
					const USoundMix* SoundMix =
						Cast<USoundMix>(Target);
					if (!SoundMix) return FailProjectionTarget(Target, USoundMix::StaticClass(), OutError);
                    if (!TryBuildSoundMixModifiersProjection(
							SoundMix->SoundClassEffects,
							Resources,
							First,
							OutError)
						|| !TryBuildDecimalProjectionValue(
							SoundMix->InitialDelay,
							Resources,
							Second,
							OutError, TEXT("initial_delay")))
					{
						return false;
					}
					OutValues.Add(
						TEXT("modifiers"),
						First.ToSharedRef());
					OutValues.Add(
						TEXT("initial_delay"),
						Second.ToSharedRef());
					return true;
				}
			case ECoreAssetKind::SoundSubmix:
				{
					const USoundSubmix* Submix =
						Cast<USoundSubmix>(Target);
					TArray<UObject*> Effects;
                    if (!Submix) return FailProjectionTarget(Target, USoundSubmix::StaticClass(), OutError);
					for (USoundEffectSubmixPreset* Effect
						: Submix->SubmixEffectChain)
					{
						Effects.Add(Effect);
					}
					if (!TryBuildObjectReferenceProjection(
							Submix->ParentSubmix,
							true,
							Resources,
							First,
							OutError, TEXT("parent"))
						|| !TryBuildObjectReferenceArrayProjection(
							Effects,
							Resources,
							Second,
							OutError, TEXT("effect_chain")))
					{
						return false;
					}
					OutValues.Add(
						TEXT("parent"),
						First.ToSharedRef());
					OutValues.Add(
						TEXT("effect_chain"),
						Second.ToSharedRef());
					return true;
				}
			case ECoreAssetKind::Curve:
				if (!TryBuildCurveKeysProjection(
						Target,
						Resources,
						First,
						OutError))
				{
					return false;
				}
				OutValues.Add(TEXT("keys"), First.ToSharedRef());
				return true;
			default:
                return Fail(OutError, NativeFailure(TEXT("asset_data_core_audio_curve_projection_unsupported"),
                    TEXT("This asset kind has no audio or curve projection."), TEXT("asset_kind"),
                    MakeShared<FJsonValueNumber>(static_cast<int32>(Kind)), MakeShared<FJsonValueString>(TEXT("supported audio or curve asset kind"))));
			}
		}

		bool TryBuildCoreAudioCurveProjection(
			const ECoreAssetKind Kind,
			UObject* Target,
			const TArray<FString>& SelectedFieldIds,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutCurrentFields,
			FUeAgentCanonicalValuePtr& OutDefaultFields,
			FUeAgentCanonicalValuePtr& OutProjectionContext,
			FExecutionMutationError& OutError)
		{
			UObject* Default =
				Target
					? Target->GetClass()->GetDefaultObject()
					: nullptr;
			TMap<FString, FUeAgentCanonicalValueRef> CurrentValues;
			TMap<FString, FUeAgentCanonicalValueRef> DefaultValues;
            if (!Target) return Fail(OutError, NativeFailure(TEXT("asset_data_core_projection_target_missing"),
                TEXT("Projection requires a live target."), TEXT("target"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("live asset"))));
            if (!Default) return Fail(OutError, NativeFailure(TEXT("asset_data_core_projection_default_missing"),
                TEXT("The native default object is unavailable for this projection."), TEXT("default_target"),
                MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(Target->GetClass()->GetPathName())));
            if (SelectedFieldIds.IsEmpty()) return Fail(OutError, NativeFailure(TEXT("asset_data_core_selected_fields_empty"),
                TEXT("Projection requires at least one selected field."), TEXT("selected_field_ids"),
                MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("nonempty supported field list"))));
            if (!TryBuildCoreAudioCurveValueMap(
					Kind,
					Target,
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildCoreAudioCurveValueMap(
					Kind,
					Default,
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
			TSet<FString> Seen;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				CurrentItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				DefaultItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				ContextItems;
			for (const FString& FieldId : SelectedFieldIds)
			{
				const FUeAgentCanonicalValueRef* Current =
					CurrentValues.Find(FieldId);
				const FUeAgentCanonicalValueRef* DefaultValue =
					DefaultValues.Find(FieldId);
				const TCHAR* ContextTag =
					FindCoreAudioCurveContextTag(Kind, FieldId);
				const FString ContextText =
					Kind == ECoreAssetKind::Curve
							&& FieldId == TEXT("keys")
						? FString::Printf(
							TEXT("%s:%d"),
							ContextTag
								? ContextTag
								: TEXT(""),
							GetCurveDimension(Target))
						: FString(ContextTag ? ContextTag : TEXT(""));
				FUeAgentCanonicalValuePtr ContextValue;
                if (Seen.Contains(FieldId))
                    return Fail(OutError, NativeFailure(TEXT("asset_data_core_selected_field_duplicate"), TEXT("The field is selected more than once."),
                        TEXT("selected_field_ids/") + FString::FromInt(Seen.Num()), MakeShared<FJsonValueString>(FieldId), MakeShared<FJsonValueString>(TEXT("unique field id"))));
                if (!Current) return Fail(OutError, NativeFailure(TEXT("asset_data_core_selected_field_invalid"),
                    TEXT("The selected field is not provided by this asset projection."), TEXT("selected_field_ids/") + FString::FromInt(Seen.Num()),
                    MakeShared<FJsonValueString>(FieldId), ProjectionFieldIdsValue(CurrentValues)));
                if (!DefaultValue) return Fail(OutError, NativeFailure(TEXT("asset_data_core_projection_default_field_missing"),
                    TEXT("The native default projection is missing a selected field."), ChildPath(TEXT("default_fields"), FieldId),
                    MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("captured default field"))));
                if (!ContextTag || ContextText.IsEmpty()) return Fail(OutError, NativeFailure(TEXT("asset_data_core_projection_context_missing"),
                    TEXT("The projection has no context tag for the selected field."), ChildPath(TEXT("projection_context"), FieldId),
                    MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("registered context tag"))));
                if (!TryBuildStringProjectionValue(ContextText, Resources, ContextValue, OutError, ChildPath(TEXT("projection_context"), FieldId))) return false;
				Seen.Add(FieldId);
				CurrentItems.Emplace(FieldId, *Current);
				DefaultItems.Emplace(FieldId, *DefaultValue);
				ContextItems.Emplace(
					FieldId,
					ContextValue.ToSharedRef());
			}
			return TryBuildObjectProjectionValue(
					MoveTemp(CurrentItems),
					Resources,
					OutCurrentFields,
					OutError, TEXT("current_fields"))
				&& TryBuildObjectProjectionValue(
					MoveTemp(DefaultItems),
					Resources,
					OutDefaultFields,
					OutError, TEXT("default_fields"))
				&& TryBuildObjectProjectionValue(
					MoveTemp(ContextItems),
					Resources,
					OutProjectionContext,
					OutError, TEXT("projection_context"));
		}

		const TCHAR* FindCoreConfigurationContextTag(
			const ECoreAssetKind Kind,
			const FString& FieldId)
		{
			switch (Kind)
			{
			case ECoreAssetKind::MediaSource:
				if (FieldId == TEXT("source_uri"))
				{
					return TEXT("file_path");
				}
				if (FieldId == TEXT("options"))
				{
					return TEXT("media_source_options");
				}
				break;
			case ECoreAssetKind::RuntimeVirtualTexture:
				if (FieldId == TEXT("size"))
				{
					return TEXT("rvt_size");
				}
				if (FieldId == TEXT("material_type"))
				{
					return TEXT("rvt_material_type");
				}
				if (FieldId == TEXT("compression"))
				{
					return TEXT("boolean");
				}
				break;
			case ECoreAssetKind::SparseVolumeTexture:
				if (FieldId == TEXT("addressing"))
				{
					return TEXT("texture_addressing");
				}
				if (FieldId == TEXT("streaming"))
				{
					return TEXT("sparse_streaming");
				}
				break;
			default:
				break;
			}
			return nullptr;
		}

		bool TryBuildCoreConfigurationValueMap(
			const ECoreAssetKind Kind,
			UObject* Target,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
            ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty() && Target) OutError.TargetPath = Target->GetPathName(); };
			if (Kind == ECoreAssetKind::MediaSource)
			{
				const UFileMediaSource* MediaSource =
					Cast<UFileMediaSource>(Target);
				FUeAgentCanonicalValuePtr SourceUri;
				FUeAgentCanonicalValuePtr Options;
				if (!MediaSource) return FailProjectionTarget(Target, UFileMediaSource::StaticClass(), OutError);
                if (!TryBuildStringProjectionValue(
						MediaSource->GetFilePath(),
						Resources,
						SourceUri,
						OutError, TEXT("source_uri"))
					|| !TryBuildMediaSourceOptionsProjection(
						MediaSource->PrecacheFile,
						Resources,
						Options,
						OutError))
				{
                    return false;
				}
				OutValues.Add(
					TEXT("source_uri"),
					SourceUri.ToSharedRef());
				OutValues.Add(
					TEXT("options"),
					Options.ToSharedRef());
				return true;
			}
			if (Kind == ECoreAssetKind::RuntimeVirtualTexture)
			{
				URuntimeVirtualTexture* VirtualTexture =
					Cast<URuntimeVirtualTexture>(Target);
				int32 TileCount = 0;
				ERuntimeVirtualTextureMaterialType MaterialType =
					ERuntimeVirtualTextureMaterialType::BaseColor;
				bool bCompression = false;
				FString MaterialTypeName;
				FUeAgentCanonicalValuePtr Size;
				FUeAgentCanonicalValuePtr Material;
				FUeAgentCanonicalValuePtr Compression;
				if (!VirtualTexture) return FailProjectionTarget(Target, URuntimeVirtualTexture::StaticClass(), OutError);
                if (!TryReadRvtProperties(
						VirtualTexture,
						TileCount,
						MaterialType,
						bCompression, OutError)
					|| !TryFormatRuntimeVirtualTextureMaterialType(
						MaterialType,
						MaterialTypeName, TEXT("material_type"), OutError)
					|| !TryBuildIntegerProjectionValue(
						VirtualTexture->GetSize(),
						Resources,
						Size,
						OutError, TEXT("size"))
					|| !TryBuildStringProjectionValue(
						MaterialTypeName,
						Resources,
						Material,
						OutError, TEXT("material_type"))
					|| !TryBuildBooleanProjectionValue(
						bCompression,
						Resources,
						Compression,
						OutError, TEXT("compression")))
				{
                    return false;
				}
				OutValues.Add(TEXT("size"), Size.ToSharedRef());
				OutValues.Add(
					TEXT("material_type"),
					Material.ToSharedRef());
				OutValues.Add(
					TEXT("compression"),
					Compression.ToSharedRef());
				return true;
			}
			if (Kind == ECoreAssetKind::SparseVolumeTexture)
			{
				const UStreamableSparseVolumeTexture* Sparse =
					Cast<UStreamableSparseVolumeTexture>(Target);
				FUeAgentCanonicalValuePtr Addressing;
				FUeAgentCanonicalValuePtr Streaming;
				if (!Sparse) return FailProjectionTarget(Target, UStreamableSparseVolumeTexture::StaticClass(), OutError);
                if (!TryBuildSparseAddressingProjection(
						Sparse->AddressX,
						Sparse->AddressY,
						Sparse->AddressZ,
						Resources,
						Addressing,
						OutError)
					|| !TryBuildSparseStreamingProjection(
						Sparse->bLocalDDCOnly,
						Sparse->StreamingPoolSizeFactor,
						Sparse->NumberOfPrefetchFrames,
						Sparse->PrefetchPercentageStepSize,
						Sparse->PrefetchPercentageBias,
						Resources,
						Streaming,
						OutError))
				{
                    return false;
				}
				OutValues.Add(
					TEXT("addressing"),
					Addressing.ToSharedRef());
				OutValues.Add(
					TEXT("streaming"),
					Streaming.ToSharedRef());
				return true;
			}
            return Fail(OutError, NativeFailure(TEXT("asset_data_core_configuration_projection_unsupported"),
                TEXT("This asset kind has no configuration projection."), TEXT("asset_kind"),
                MakeShared<FJsonValueNumber>(static_cast<int32>(Kind)), MakeShared<FJsonValueString>(TEXT("MediaSource, RuntimeVirtualTexture or SparseVolumeTexture"))));
		}

		bool TryBuildCoreConfigurationProjection(
			const ECoreAssetKind Kind,
			UObject* Target,
			const TArray<FString>& SelectedFieldIds,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutCurrentFields,
			FUeAgentCanonicalValuePtr& OutDefaultFields,
			FUeAgentCanonicalValuePtr& OutProjectionContext,
			FExecutionMutationError& OutError)
		{
			UObject* Default = nullptr;
			switch (Kind)
			{
			case ECoreAssetKind::MediaSource:
				Default = GetMutableDefault<UFileMediaSource>();
				break;
			case ECoreAssetKind::RuntimeVirtualTexture:
				Default = GetMutableDefault<URuntimeVirtualTexture>();
				break;
			case ECoreAssetKind::SparseVolumeTexture:
				Default =
					GetMutableDefault<
						UStreamableSparseVolumeTexture>();
				break;
			default:
				break;
			}
			TMap<FString, FUeAgentCanonicalValueRef> CurrentValues;
			TMap<FString, FUeAgentCanonicalValueRef> DefaultValues;
            if (!Target) return Fail(OutError, NativeFailure(TEXT("asset_data_core_projection_target_missing"),
                TEXT("Projection requires a live target."), TEXT("target"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("live asset"))));
            if (!Default) return Fail(OutError, NativeFailure(TEXT("asset_data_core_projection_default_missing"),
                TEXT("The native default object is unavailable for this projection."), TEXT("default_target"),
                MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(Target->GetClass()->GetPathName())));
            if (SelectedFieldIds.IsEmpty()) return Fail(OutError, NativeFailure(TEXT("asset_data_core_selected_fields_empty"),
                TEXT("Projection requires at least one selected field."), TEXT("selected_field_ids"),
                MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("nonempty supported field list"))));
            if (!TryBuildCoreConfigurationValueMap(
					Kind,
					Target,
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildCoreConfigurationValueMap(
					Kind,
					Default,
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
			TSet<FString> Seen;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				CurrentItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				DefaultItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				ContextItems;
			for (const FString& FieldId : SelectedFieldIds)
			{
				const FUeAgentCanonicalValueRef* Current =
					CurrentValues.Find(FieldId);
				const FUeAgentCanonicalValueRef* DefaultValue =
					DefaultValues.Find(FieldId);
				const TCHAR* ContextTag =
					FindCoreConfigurationContextTag(
						Kind,
						FieldId);
				FUeAgentCanonicalValuePtr ContextValue;
                if (Seen.Contains(FieldId))
                    return Fail(OutError, NativeFailure(TEXT("asset_data_core_selected_field_duplicate"), TEXT("The field is selected more than once."),
                        TEXT("selected_field_ids/") + FString::FromInt(Seen.Num()), MakeShared<FJsonValueString>(FieldId), MakeShared<FJsonValueString>(TEXT("unique field id"))));
                if (!Current) return Fail(OutError, NativeFailure(TEXT("asset_data_core_selected_field_invalid"),
                    TEXT("The selected field is not provided by this asset projection."), TEXT("selected_field_ids/") + FString::FromInt(Seen.Num()),
                    MakeShared<FJsonValueString>(FieldId), ProjectionFieldIdsValue(CurrentValues)));
                if (!DefaultValue) return Fail(OutError, NativeFailure(TEXT("asset_data_core_projection_default_field_missing"),
                    TEXT("The native default projection is missing a selected field."), ChildPath(TEXT("default_fields"), FieldId),
                    MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("captured default field"))));
                if (!ContextTag) return Fail(OutError, NativeFailure(TEXT("asset_data_core_projection_context_missing"),
                    TEXT("The projection has no context tag for the selected field."), ChildPath(TEXT("projection_context"), FieldId),
                    MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("registered context tag"))));
                if (!TryBuildStringProjectionValue(ContextTag, Resources, ContextValue, OutError, ChildPath(TEXT("projection_context"), FieldId))) return false;
				Seen.Add(FieldId);
				CurrentItems.Emplace(FieldId, *Current);
				DefaultItems.Emplace(FieldId, *DefaultValue);
				ContextItems.Emplace(
					FieldId,
					ContextValue.ToSharedRef());
			}
			return TryBuildObjectProjectionValue(
					MoveTemp(CurrentItems),
					Resources,
					OutCurrentFields,
					OutError, TEXT("current_fields"))
				&& TryBuildObjectProjectionValue(
					MoveTemp(DefaultItems),
					Resources,
					OutDefaultFields,
					OutError, TEXT("default_fields"))
				&& TryBuildObjectProjectionValue(
					MoveTemp(ContextItems),
					Resources,
					OutProjectionContext,
					OutError, TEXT("projection_context"));
		}

		bool TryNormalizeCoreConfigurationValue(
			const ECoreAssetKind Kind,
			const FString& FieldId,
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			if (Kind == ECoreAssetKind::MediaSource)
			{
				if (FieldId == TEXT("source_uri"))
				{
					FString FilePath;
					return TryNormalizeFileUri(
							Requested,
							FilePath, FieldId, OutError)
						&& TryBuildStringProjectionValue(
							FilePath,
							Resources,
							OutValue,
							OutError, FieldId);
				}
				if (FieldId == TEXT("options"))
				{
					const FUeAgentCanonicalValue* Precache =
						FindObjectField(
							Requested,
							TEXT("precache_file"));
					bool bPrecache = false;
					return TryRequireObjectFields(
							Requested,
							{TEXT("precache_file")}, FieldId, OutError)
						&& Precache
						&& TryReadBool(*Precache, bPrecache, ChildPath(FieldId, TEXT("precache_file")), OutError)
						&& TryBuildMediaSourceOptionsProjection(
							bPrecache,
							Resources,
							OutValue,
							OutError);
				}
			}
			else if (Kind
				== ECoreAssetKind::RuntimeVirtualTexture)
			{
				if (FieldId == TEXT("size"))
				{
					int32 Size = 0;
					return TryReadInt32(
							Requested,
							Size,
							1,
							MAX_int32, FieldId, OutError)
						&& TryBuildIntegerProjectionValue(
							Size,
							Resources,
							OutValue,
							OutError, FieldId);
				}
				if (FieldId == TEXT("material_type"))
				{
					FString RequestedName;
					FString CanonicalName;
					ERuntimeVirtualTextureMaterialType MaterialType =
						ERuntimeVirtualTextureMaterialType::
							BaseColor;
					return TryReadString(
							Requested,
							RequestedName, FieldId, OutError)
						&& TryParseRuntimeVirtualTextureMaterialType(
							RequestedName,
							MaterialType, FieldId, OutError)
						&& TryFormatRuntimeVirtualTextureMaterialType(
							MaterialType,
							CanonicalName, FieldId, OutError)
						&& TryBuildStringProjectionValue(
							CanonicalName,
							Resources,
							OutValue,
							OutError, FieldId);
				}
				if (FieldId == TEXT("compression"))
				{
					bool bCompression = false;
					return TryReadBool(
							Requested,
							bCompression, FieldId, OutError)
						&& TryBuildBooleanProjectionValue(
							bCompression,
							Resources,
							OutValue,
							OutError, FieldId);
				}
			}
			else if (Kind == ECoreAssetKind::SparseVolumeTexture)
			{
				if (FieldId == TEXT("addressing"))
				{
					const FUeAgentCanonicalValue* XValue =
						FindObjectField(Requested, TEXT("x"));
					const FUeAgentCanonicalValue* YValue =
						FindObjectField(Requested, TEXT("y"));
					const FUeAgentCanonicalValue* ZValue =
						FindObjectField(Requested, TEXT("z"));
					FString XName;
					FString YName;
					FString ZName;
					TEnumAsByte<TextureAddress> X;
					TEnumAsByte<TextureAddress> Y;
					TEnumAsByte<TextureAddress> Z;
					return TryRequireObjectFields(
							Requested,
							{
								TEXT("x"),
								TEXT("y"),
								TEXT("z"),
							}, FieldId, OutError)
						&& XValue
						&& YValue
						&& ZValue
						&& TryReadString(*XValue, XName, ChildPath(FieldId, TEXT("x")), OutError)
						&& TryReadString(*YValue, YName, ChildPath(FieldId, TEXT("y")), OutError)
						&& TryReadString(*ZValue, ZName, ChildPath(FieldId, TEXT("z")), OutError)
						&& TryParseTextureAddress(XName, X, ChildPath(FieldId, TEXT("x")), OutError)
						&& TryParseTextureAddress(YName, Y, ChildPath(FieldId, TEXT("y")), OutError)
						&& TryParseTextureAddress(ZName, Z, ChildPath(FieldId, TEXT("z")), OutError)
						&& TryBuildSparseAddressingProjection(
							X,
							Y,
							Z,
							Resources,
							OutValue,
							OutError);
				}
				if (FieldId == TEXT("streaming"))
				{
					const FUeAgentCanonicalValue* LocalDdc =
						FindObjectField(
							Requested,
							TEXT("local_ddc_only"));
					const FUeAgentCanonicalValue* PoolSize =
						FindObjectField(
							Requested,
							TEXT("pool_size_factor"));
					const FUeAgentCanonicalValue* Frames =
						FindObjectField(
							Requested,
							TEXT("prefetch_frames"));
					const FUeAgentCanonicalValue* Step =
						FindObjectField(
							Requested,
							TEXT("prefetch_percentage_step"));
					const FUeAgentCanonicalValue* Bias =
						FindObjectField(
							Requested,
							TEXT("prefetch_percentage_bias"));
					bool bLocalDdc = false;
					float PoolSizeFactor = 0.0f;
					int32 PrefetchFrames = 0;
					float PercentageStep = 0.0f;
					float PercentageBias = 0.0f;
					return TryRequireObjectFields(
							Requested,
							{
								TEXT("local_ddc_only"),
								TEXT("pool_size_factor"),
								TEXT("prefetch_frames"),
								TEXT(
									"prefetch_percentage_step"),
								TEXT(
									"prefetch_percentage_bias"),
							}, FieldId, OutError)
						&& LocalDdc
						&& PoolSize
						&& Frames
						&& Step
						&& Bias
						&& TryReadBool(*LocalDdc, bLocalDdc, ChildPath(FieldId, TEXT("local_ddc_only")), OutError)
						&& TryReadFloat(
							*PoolSize,
							PoolSizeFactor,
							1.0,
							1000.0, ChildPath(FieldId, TEXT("pool_size_factor")), OutError)
						&& TryReadInt32(
							*Frames,
							PrefetchFrames,
							0,
							1024, ChildPath(FieldId, TEXT("prefetch_frames")), OutError)
						&& TryReadFloat(
							*Step,
							PercentageStep,
							0.0,
							100.0, ChildPath(FieldId, TEXT("prefetch_percentage_step")), OutError)
						&& TryReadFloat(
							*Bias,
							PercentageBias,
							0.0,
							100.0, ChildPath(FieldId, TEXT("prefetch_percentage_bias")), OutError)
						&& TryBuildSparseStreamingProjection(
							bLocalDdc,
							PoolSizeFactor,
							PrefetchFrames,
							PercentageStep,
							PercentageBias,
							Resources,
							OutValue,
							OutError);
				}
			}
            return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_field_invalid"),
                TEXT("The requested field is not supported by this configuration mechanic."), FieldId,
                &Requested, TEXT("supported field for the selected mechanic")));
		}

        bool TryRequireExpectedState(const FUeAgentCanonicalValue& CurrentFields,
            const FUeAgentCanonicalValue& DefaultFields, const FUeAgentCanonicalValue& ProjectionContext,
            const FUeAgentCanonicalValue* TypedChanges, FExecutionMutationError& OutError)
        {
            const FUeAgentCanonicalValue* Values[] = {&CurrentFields, &DefaultFields, &ProjectionContext};
            const TCHAR* Names[] = {TEXT("current_fields"), TEXT("default_fields"), TEXT("projection_context")};
            for (int32 Index = 0; Index < 3; ++Index)
                if (Values[Index]->GetKind() != EUeAgentValueKind::Object)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_state_invalid"),
                        TEXT("The captured projection component must be an object."),
                        Names[Index], Values[Index], TEXT("object"), TEXT("native_result")));
            if (!TypedChanges || TypedChanges->GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_state_invalid"),
                    TEXT("Typed changes must be an array."), TEXT("typed_changes"), TypedChanges, TEXT("array")));
            if (TypedChanges->GetArrayItems().Num() != CurrentFields.GetObjectItems().Num())
                return Fail(OutError, JsonFailure(TEXT("asset_data_core_expected_change_count_invalid"),
                    TEXT("The number of typed changes must match the selected current fields."), TEXT("typed_changes"),
                    MakeShared<FJsonValueNumber>(TypedChanges->GetArrayItems().Num()),
                    MakeShared<FJsonValueNumber>(CurrentFields.GetObjectItems().Num())));
            return true;
        }

		bool TryBuildExpectedCoreConfigurationFields(
			const ECoreAssetKind Kind,
			const FUeAgentCanonicalValue& CurrentFields,
			const FUeAgentCanonicalValue& DefaultFields,
			const FUeAgentCanonicalValue& ProjectionContext,
			const FUeAgentCanonicalValue& ProposedAfter,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpectedFields,
			FExecutionMutationError& OutError)
		{
			OutExpectedFields.Reset();
			const FUeAgentCanonicalValue* TypedChanges =
				FindObjectField(
					ProposedAfter,
					TEXT("typed_changes"));
            if (!TryRequireExpectedState(CurrentFields, DefaultFields, ProjectionContext, TypedChanges, OutError)) return false;
			TMap<FString, const FUeAgentCanonicalValue*> Changes;
			for (const FUeAgentCanonicalValueRef& ChangeRef
				: TypedChanges->GetArrayItems())
			{
				const FUeAgentCanonicalValue& Change =
					ChangeRef.Get();
				const FUeAgentCanonicalValue* Field =
					FindObjectField(Change, TEXT("field_id"));
				FString FieldId;
                const FString ChangePath = TEXT("typed_changes/") + FString::FromInt(Changes.Num()) + TEXT("/field_id");
                if (!TryReadRequiredString(Field, FieldId, ChangePath, OutError)) return false;
                if (Changes.Contains(FieldId))
                    return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_change_duplicate"),
                        TEXT("More than one change addresses the same field."), ChangePath, Field, TEXT("unique field_id")));
				Changes.Add(FieldId, &Change);
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				ExpectedItems;
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Current
				: CurrentFields.GetObjectItems())
			{
				const FUeAgentCanonicalValue* const* ChangePtr =
					Changes.Find(Current.Key);
				const FUeAgentCanonicalValueRef* DefaultValueRef =
					FindObjectFieldRef(
						DefaultFields,
						Current.Key);
				const FUeAgentCanonicalValue* DefaultValue =
					DefaultValueRef
						? &DefaultValueRef->Get()
						: nullptr;
				const FUeAgentCanonicalValue* ContextValue =
					FindObjectField(
						ProjectionContext,
						Current.Key);
				const TCHAR* ExpectedContextTag =
					FindCoreConfigurationContextTag(
						Kind,
						Current.Key);
				FString ContextTag;
                if (!ChangePtr) return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_change_missing"),
                    TEXT("The selected field has no typed change."), Current.Key, nullptr, TEXT("field change")));
                if (!DefaultValue) return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_default_missing"),
                    TEXT("The selected field has no captured default value."), ChildPath(TEXT("default_fields"), Current.Key), nullptr, TEXT("captured default"), TEXT("native_result")));
                if (!ContextValue) return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_context_missing"),
                    TEXT("The selected field has no projection context."), ChildPath(TEXT("projection_context"), Current.Key), nullptr, TEXT("context string"), TEXT("native_result")));
                if (!ExpectedContextTag) return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_field_unsupported"),
                    TEXT("The selected field is not supported by this configuration mechanic."), Current.Key, &Current.Value.Get(), TEXT("supported configuration field"), TEXT("native_result")));
                if (!TryReadRequiredString(ContextValue, ContextTag, ChildPath(TEXT("projection_context"), Current.Key), OutError, TEXT("native_result"))) return false;
                if (ContextTag != ExpectedContextTag) return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_context_invalid"),
                    TEXT("The captured context tag does not match the field mechanic."), ChildPath(TEXT("projection_context"), Current.Key), ContextValue, ExpectedContextTag, TEXT("native_result")));
				const FUeAgentCanonicalValue& Change =
					**ChangePtr;
				const FUeAgentCanonicalValue* PresenceValue =
					FindObjectField(
						Change,
						TEXT("presence"));
				FString Presence;
                if (!TryReadRequiredString(PresenceValue, Presence, ChildPath(Current.Key, TEXT("presence")), OutError)) return false;
				if (Presence == TEXT("clear"))
				{
					ExpectedItems.Emplace(
						Current.Key,
						*DefaultValueRef);
					continue;
				}
				const FUeAgentCanonicalValue* TypedValue =
					FindObjectField(Change, TEXT("value"));
				const FUeAgentCanonicalValue* Requested =
					TypedValue
						? FindObjectField(
							*TypedValue,
							TEXT("canonical_value"))
						: nullptr;
				FUeAgentCanonicalValuePtr Normalized;
                if (Presence != TEXT("set") && Presence != TEXT("replace"))
                    return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_presence_invalid"), TEXT("The configuration presence is not supported."), ChildPath(Current.Key, TEXT("presence")), PresenceValue, TEXT("clear, set or replace")));
                if (!Requested) return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_value_missing"), TEXT("The field change requires a canonical value."), Current.Key, nullptr, TEXT("present value")));
                if (!TryNormalizeCoreConfigurationValue(
						Kind,
						Current.Key,
						*Requested,
						Resources,
						Normalized,
						OutError))
				{
					return false;
				}
				ExpectedItems.Emplace(
					Current.Key,
					Normalized.ToSharedRef());
			}
			return TryBuildObjectProjectionValue(
				MoveTemp(ExpectedItems),
				Resources,
				OutExpectedFields,
				OutError, TEXT("expected_fields"));
		}

        const FUeAgentCanonicalValue* FindCollectionIdentity(const FUeAgentCanonicalValue& Value, const FString& IdentityField)
        {
            return IdentityField.IsEmpty() ? &Value : FindObjectField(Value, IdentityField);
        }

        bool TryReadCollectionIdentity(const FUeAgentCanonicalValue& Value, const FString& IdentityField,
            FString& OutIdentity, const FString& Field, FExecutionMutationError& OutError)
        {
            return TryReadRequiredString(FindCollectionIdentity(Value, IdentityField), OutIdentity,
                IdentityField.IsEmpty() ? Field : ChildPath(Field, IdentityField), OutError);
        }

        bool TryApplyProjectedCollectionPresence(
            const FString& Presence, const FUeAgentCanonicalValue& Current,
            const FUeAgentCanonicalValue* Requested, const FString& IdentityField,
            FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& OutValue,
            FExecutionMutationError& OutError, const FString& Field)
        {
            if (Current.GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_collection_invalid"),
                    TEXT("The captured collection must be an array."), Field, &Current, TEXT("array"), TEXT("native_result")));
            if (Presence != TEXT("clear") && (!Requested || Requested->GetKind() != EUeAgentValueKind::Array))
                return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_collection_invalid"),
                    TEXT("The requested collection must be an array."), Field, Requested, TEXT("array")));
            TArray<FUeAgentCanonicalValueRef> Items = Current.GetArrayItems();
            if (Presence == TEXT("clear"))
            {
                Items.Reset();
            }
            else if (Presence == TEXT("replace"))
            {
                Items = Requested->GetArrayItems();
            }
            else if (Presence == TEXT("remove"))
            {
                TSet<FString> Removed;
                for (int32 Index = 0; Index < Requested->GetArrayItems().Num(); ++Index)
                {
                    const auto& Item = Requested->GetArrayItems()[Index];
                    const FString ItemPath = Field + TEXT("/") + FString::FromInt(Index);
                    FString Identity;
                    if (!TryReadCollectionIdentity(*Item, IdentityField, Identity, ItemPath, OutError)) return false;
                    if (Removed.Contains(Identity))
                        return Fail(OutError, JsonFailure(TEXT("asset_data_core_expected_collection_identity_duplicate"),
                            TEXT("The removal list repeats the same collection identity."),
                            IdentityField.IsEmpty() ? ItemPath : ChildPath(ItemPath, IdentityField),
                            MakeShared<FJsonValueString>(Identity), MakeShared<FJsonValueString>(TEXT("unique removal identity"))));
                    Removed.Add(Identity);
                }
                Items.RemoveAll([&Removed, &IdentityField](const FUeAgentCanonicalValueRef& Item)
                {
                    const auto* Identity = FindCollectionIdentity(*Item, IdentityField);
                    return Identity && Identity->GetKind() == EUeAgentValueKind::String && Removed.Contains(Identity->GetText());
                });
            }
            else if (Presence == TEXT("set"))
            {
                for (int32 RequestedIndex = 0; RequestedIndex < Requested->GetArrayItems().Num(); ++RequestedIndex)
                {
                    const auto& RequestedItem = Requested->GetArrayItems()[RequestedIndex];
                    FString RequestedIdentity;
                    if (!TryReadCollectionIdentity(*RequestedItem, IdentityField, RequestedIdentity,
                        Field + TEXT("/") + FString::FromInt(RequestedIndex), OutError)) return false;
                    const int32 Index = Items.IndexOfByPredicate([&RequestedIdentity, &IdentityField](const FUeAgentCanonicalValueRef& Existing)
                    {
                        const auto* Identity = FindCollectionIdentity(*Existing, IdentityField);
                        return Identity && Identity->GetKind() == EUeAgentValueKind::String && Identity->GetText() == RequestedIdentity;
                    });
                    if (Index == INDEX_NONE) Items.Add(RequestedItem);
                    else Items[Index] = RequestedItem;
                }
            }
            else
            {
                return Fail(OutError, JsonFailure(TEXT("asset_data_core_expected_collection_presence_invalid"),
                    TEXT("The collection presence is not supported."), Field,
                    MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("clear, replace, remove, set"))));
            }
            return TryBuildArrayProjectionValue(Items, Resources, OutValue, OutError, Field);
        }

		bool TryNormalizeCoreAudioCurveValue(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const ECoreAssetKind Kind,
			const FString& FieldId,
			const FUeAgentCanonicalValue& Requested,
			const int32 CurveDimension,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			if (Kind == ECoreAssetKind::AudioEffectPreset
				&& FieldId == TEXT("settings"))
			{
				FSubmixEffectSubmixEQSettings Settings;
				return TryParseEqSettings(Requested, Settings, FieldId, OutError)
					&& TryBuildEqSettingsProjection(
						Settings,
						Resources,
						OutValue,
						OutError);
			}
			if (Kind == ECoreAssetKind::SoundAttenuation
				&& FieldId == TEXT("attenuation"))
			{
				FSoundAttenuationSettings Settings;
				return TryParseAttenuationSettings(
						Requested,
						Settings, FieldId, OutError)
					&& TryBuildAttenuationProjection(
						Settings,
						Resources,
						OutValue,
						OutError);
			}
			if (Kind == ECoreAssetKind::SoundClass)
			{
				if (FieldId == TEXT("properties"))
				{
					FSoundClassProperties Properties =
						GetDefault<USoundClass>()->Properties;
					return TryParseSoundClassProperties(
							Requested,
							Properties, FieldId, OutError)
						&& TryBuildSoundClassPropertiesProjection(
							Properties,
							Resources,
							OutValue,
							OutError);
				}
				if (FieldId == TEXT("child_classes"))
				{
					TArray<USoundClass*> Resolved;
					TArray<UObject*> Objects;
					if (!TryResolveUniqueArray(
							Resolver,
							Requested,
							Resolved,
							OutError, FieldId))
					{
						return false;
					}
					for (USoundClass* Object : Resolved)
					{
						Objects.Add(Object);
					}
					return TryBuildObjectReferenceArrayProjection(
						Objects,
						Resources,
						OutValue,
						OutError, FieldId);
				}
			}
			if (Kind == ECoreAssetKind::SoundConcurrency
				&& FieldId == TEXT("concurrency"))
			{
				FSoundConcurrencySettings Settings;
				return TryParseConcurrencySettings(
						Requested,
						Settings, FieldId, OutError)
					&& TryBuildConcurrencyProjection(
						Settings,
						Resources,
						OutValue,
						OutError);
			}
			if (Kind == ECoreAssetKind::SoundMix)
			{
				if (FieldId == TEXT("modifiers"))
				{
					TArray<FSoundClassAdjuster> Modifiers;
					TArray<TStrongObjectPtr<UObject>> Pins;
					return TryParseSoundMixModifiers(
							Resolver,
							Requested,
							Modifiers,
							Pins,
							OutError, FieldId)
						&& TryBuildSoundMixModifiersProjection(
							Modifiers,
							Resources,
							OutValue,
							OutError);
				}
				if (FieldId == TEXT("initial_delay"))
				{
					float Delay = 0.0f;
					return TryReadFloat(
							Requested,
							Delay,
							0.0,
							1000000.0, FieldId, OutError)
						&& TryBuildDecimalProjectionValue(
							Delay,
							Resources,
							OutValue,
							OutError, FieldId);
				}
			}
			if (Kind == ECoreAssetKind::SoundSubmix)
			{
				if (FieldId == TEXT("parent"))
				{
					USoundSubmixBase* Parent = nullptr;
					return TryResolveObject(
							Resolver,
							Requested,
							Parent,
							OutError, FieldId)
						&& TryBuildObjectReferenceProjection(
							Parent,
							false,
							Resources,
							OutValue,
							OutError, FieldId);
				}
				if (FieldId == TEXT("effect_chain"))
				{
					TArray<USoundEffectSubmixPreset*> Resolved;
					TArray<UObject*> Objects;
					if (!TryResolveUniqueArray(
							Resolver,
							Requested,
							Resolved,
							OutError, FieldId))
					{
						return false;
					}
					for (USoundEffectSubmixPreset* Object : Resolved)
					{
						Objects.Add(Object);
					}
					return TryBuildObjectReferenceArrayProjection(
						Objects,
						Resources,
						OutValue,
						OutError, FieldId);
				}
			}
			if (Kind == ECoreAssetKind::Curve
				&& FieldId == TEXT("keys"))
			{
				TArray<TArray<FRichCurveKey>> Channels;
				if (CurveDimension < 1 || CurveDimension > 4)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_core_curve_dimension_invalid"),
                        TEXT("The captured curve dimension is outside the supported range."), TEXT("curve_dimension"),
                        MakeShared<FJsonValueNumber>(CurveDimension), MakeShared<FJsonValueString>(TEXT("integer from 1 to 4"))));
                return TryParseCurveKeys(
						Requested,
						CurveDimension,
						Channels, FieldId, OutError)
					&& TryBuildCurveChannelsProjection(
						Channels,
						Resources,
						OutValue,
						OutError);
			}
			return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_field_invalid"),
                TEXT("The requested field is not supported by this audio or curve mechanic."), FieldId,
                &Requested, TEXT("supported field for the selected mechanic")));
		}

		bool TryBuildExpectedCoreAudioCurveFields(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const ECoreAssetKind Kind,
			const FUeAgentCanonicalValue& CurrentFields,
			const FUeAgentCanonicalValue& DefaultFields,
			const FUeAgentCanonicalValue& ProjectionContext,
			const FUeAgentCanonicalValue& ProposedAfter,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpectedFields,
			FExecutionMutationError& OutError)
		{
			OutExpectedFields.Reset();
			const FUeAgentCanonicalValue* TypedChanges =
				FindObjectField(
					ProposedAfter,
					TEXT("typed_changes"));
            if (!TryRequireExpectedState(CurrentFields, DefaultFields, ProjectionContext, TypedChanges, OutError)) return false;
			TMap<FString, const FUeAgentCanonicalValue*> Changes;
			for (const FUeAgentCanonicalValueRef& ChangeRef
				: TypedChanges->GetArrayItems())
			{
				const FUeAgentCanonicalValue& Change =
					ChangeRef.Get();
				const FUeAgentCanonicalValue* Field =
					FindObjectField(Change, TEXT("field_id"));
				FString FieldId;
                const FString ChangePath = TEXT("typed_changes/") + FString::FromInt(Changes.Num()) + TEXT("/field_id");
                if (!TryReadRequiredString(Field, FieldId, ChangePath, OutError)) return false;
                if (Changes.Contains(FieldId))
                    return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_change_duplicate"),
                        TEXT("More than one change addresses the same field."), ChangePath, Field, TEXT("unique field_id")));
				Changes.Add(FieldId, &Change);
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				ExpectedItems;
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Current
				: CurrentFields.GetObjectItems())
			{
				const FUeAgentCanonicalValue* const* ChangePtr =
					Changes.Find(Current.Key);
				const FUeAgentCanonicalValueRef* DefaultValueRef =
					FindObjectFieldRef(
						DefaultFields,
						Current.Key);
				const FUeAgentCanonicalValue* ContextValue =
					FindObjectField(
						ProjectionContext,
						Current.Key);
				FString ContextText;
				const TCHAR* ExpectedContextTag =
					FindCoreAudioCurveContextTag(
						Kind,
						Current.Key);
                if (!ChangePtr) return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_change_missing"),
                    TEXT("The selected field has no typed change."), Current.Key, nullptr, TEXT("field change")));
                if (!DefaultValueRef) return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_default_missing"),
                    TEXT("The selected field has no captured default value."), ChildPath(TEXT("default_fields"), Current.Key), nullptr, TEXT("captured default"), TEXT("native_result")));
                if (!ContextValue) return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_context_missing"),
                    TEXT("The selected field has no projection context."), ChildPath(TEXT("projection_context"), Current.Key), nullptr, TEXT("context string"), TEXT("native_result")));
                if (!ExpectedContextTag) return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_field_unsupported"),
                    TEXT("The selected field is not supported by this audio or curve mechanic."), Current.Key, &Current.Value.Get(), TEXT("supported audio or curve field"), TEXT("native_result")));
                if (!TryReadRequiredString(ContextValue, ContextText, ChildPath(TEXT("projection_context"), Current.Key), OutError, TEXT("native_result"))) return false;
				int32 CurveDimension = 0;
				if (Kind == ECoreAssetKind::Curve)
				{
					const FString Prefix =
						FString(ExpectedContextTag) + TEXT(":");
					if (!ContextText.StartsWith(Prefix))
					{
                        return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_curve_context_invalid"),
                            TEXT("The captured curve context has an unexpected prefix."), ChildPath(TEXT("projection_context"), Current.Key), ContextValue, Prefix + TEXT("dimension"), TEXT("native_result")));
					}
					CurveDimension =
						FCString::Atoi(
							*ContextText.RightChop(
								Prefix.Len()));
					if (CurveDimension < 1 || CurveDimension > 4)
					{
                        return Fail(OutError, JsonFailure(TEXT("asset_data_core_expected_curve_dimension_invalid"),
                            TEXT("The captured curve dimension is outside the supported range."), ChildPath(TEXT("projection_context"), Current.Key),
                            MakeShared<FJsonValueNumber>(CurveDimension), MakeShared<FJsonValueString>(TEXT("integer from 1 to 4")), TEXT("native_result")));
					}
				}
				else if (ContextText != ExpectedContextTag)
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_context_invalid"),
                        TEXT("The captured context tag does not match the field mechanic."), ChildPath(TEXT("projection_context"), Current.Key), ContextValue, ExpectedContextTag, TEXT("native_result")));
				}
				const FUeAgentCanonicalValue& Change =
					**ChangePtr;
				const FUeAgentCanonicalValue* PresenceValue =
					FindObjectField(
						Change,
						TEXT("presence"));
				FString Presence;
                if (!TryReadRequiredString(PresenceValue, Presence, ChildPath(Current.Key, TEXT("presence")), OutError)) return false;
				const bool bCollection =
					(Kind == ECoreAssetKind::SoundClass
							&& Current.Key
								== TEXT("child_classes"))
					|| (Kind == ECoreAssetKind::SoundMix
							&& Current.Key == TEXT("modifiers"))
					|| (Kind == ECoreAssetKind::SoundSubmix
							&& Current.Key
								== TEXT("effect_chain"));
				if (Presence == TEXT("clear"))
				{
					if (bCollection)
					{
						FUeAgentCanonicalValuePtr Empty;
						if (!TryApplyProjectedCollectionPresence(
								Presence,
								Current.Value.Get(),
								nullptr,
								Kind
											== ECoreAssetKind::SoundMix
										? TEXT("sound_class_ref")
										: FString(),
								Resources,
								Empty,
								OutError, Current.Key))
						{
							return false;
						}
						ExpectedItems.Emplace(
							Current.Key,
							Empty.ToSharedRef());
					}
					else if (
						Kind == ECoreAssetKind::SoundSubmix
						&& Current.Key == TEXT("parent"))
					{
						FUeAgentCanonicalValuePtr NullValue;
						if (!TryBuildNullProjectionValue(
								Resources,
								NullValue,
								OutError, Current.Key))
						{
							return false;
						}
						ExpectedItems.Emplace(
							Current.Key,
							NullValue.ToSharedRef());
					}
					else
					{
						ExpectedItems.Emplace(
							Current.Key,
							*DefaultValueRef);
					}
					continue;
				}
				if (Presence == TEXT("null")
					&& Kind == ECoreAssetKind::SoundSubmix
					&& Current.Key == TEXT("parent"))
				{
					FUeAgentCanonicalValuePtr NullValue;
					if (!TryBuildNullProjectionValue(
							Resources,
							NullValue,
							OutError, Current.Key))
					{
						return false;
					}
					ExpectedItems.Emplace(
						Current.Key,
						NullValue.ToSharedRef());
					continue;
				}
				const FUeAgentCanonicalValue* TypedValue =
					FindObjectField(Change, TEXT("value"));
				const FUeAgentCanonicalValue* Requested =
					TypedValue
						? FindObjectField(
							*TypedValue,
							TEXT("canonical_value"))
						: nullptr;
				FUeAgentCanonicalValuePtr Normalized;
                if (!Requested) return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_value_missing"),
                    TEXT("The field change requires a canonical value."), Current.Key, nullptr, TEXT("present value")));
                if (!TryNormalizeCoreAudioCurveValue(
						Resolver,
						Kind,
						Current.Key,
						*Requested,
						CurveDimension,
						Resources,
						Normalized,
						OutError))
				{
					return false;
				}
				if (bCollection)
				{
					FUeAgentCanonicalValuePtr Applied;
					if (!TryApplyProjectedCollectionPresence(
							Presence,
							Current.Value.Get(),
							Normalized.Get(),
							Kind == ECoreAssetKind::SoundMix
								? TEXT("sound_class_ref")
								: FString(),
							Resources,
							Applied,
							OutError, Current.Key))
					{
						return false;
					}
					ExpectedItems.Emplace(
						Current.Key,
						Applied.ToSharedRef());
				}
				else
				{
					if (Presence != TEXT("set")
						&& Presence != TEXT("replace"))
					{
                        return Fail(OutError, ValueFailure(TEXT("asset_data_core_expected_presence_invalid"),
                            TEXT("This scalar field does not support the requested presence."), ChildPath(Current.Key, TEXT("presence")), PresenceValue, TEXT("set or replace")));
					}
					ExpectedItems.Emplace(
						Current.Key,
						Normalized.ToSharedRef());
				}
			}
			return TryBuildObjectProjectionValue(
				MoveTemp(ExpectedItems),
				Resources,
				OutExpectedFields,
				OutError, TEXT("expected_fields"));
		}
	}

	bool IsCoreAssetMechanicId(
		const FString& NativeMechanicId)
	{
		return FindCoreAssetKind(NativeMechanicId) != nullptr;
	}

	struct FCoreAssetPreparedData
	{
		ECoreAssetKind Kind = ECoreAssetKind::AudioEffectPreset;
		TArray<TStrongObjectPtr<UObject>> Pins;
		TArray<FPackageDirtySnapshot> RelatedPackages;

		FSubmixEffectSubmixEQSettings OriginalEqSettings;
		FSubmixEffectSubmixEQSettings NewEqSettings;

		FSoundAttenuationSettings OriginalAttenuation;
		FSoundAttenuationSettings NewAttenuation;

		TOptional<FSoundClassProperties>
			OriginalSoundClassProperties;
		TOptional<FSoundClassProperties>
			NewSoundClassProperties;
		bool bChangeSoundClassProperties = false;
		bool bChangeSoundClassChildren = false;
		TArray<TWeakObjectPtr<USoundClass>> NewSoundClassChildren;
		TArray<FSoundClassNodeSnapshot> SoundClassSnapshots;

		FSoundConcurrencySettings OriginalConcurrency;
		FSoundConcurrencySettings NewConcurrency;

		TArray<FSoundClassAdjuster> OriginalSoundMixModifiers;
		TArray<FSoundClassAdjuster> NewSoundMixModifiers;
		float OriginalSoundMixInitialDelay = 0.0f;
		float NewSoundMixInitialDelay = 0.0f;
		bool bChangeSoundMixModifiers = false;
		bool bChangeSoundMixInitialDelay = false;

		TWeakObjectPtr<USoundSubmixBase> NewSoundSubmixParent;
		TArray<TWeakObjectPtr<USoundEffectSubmixPreset>>
			OriginalSoundSubmixEffects;
		TArray<TWeakObjectPtr<USoundEffectSubmixPreset>>
			NewSoundSubmixEffects;
		bool bChangeSoundSubmixParent = false;
		bool bChangeSoundSubmixEffects = false;
		TArray<FSoundSubmixNodeSnapshot> SoundSubmixSnapshots;

		TArray<TArray<FRichCurveKey>> OriginalCurveChannels;
		TArray<TArray<FRichCurveKey>> NewCurveChannels;

		TArray<FRegisteredFieldValue> OriginalRegisteredFields;
		TArray<FRegisteredFieldValue> NewRegisteredFields;

		FString OriginalMediaFilePath;
		FString NewMediaFilePath;
		bool bOriginalMediaPrecache = false;
		bool bNewMediaPrecache = false;
		bool bChangeMediaFilePath = false;
		bool bChangeMediaPrecache = false;

		int32 OriginalRvtTileCount = 0;
		int32 NewRvtTileCount = 0;
		ERuntimeVirtualTextureMaterialType OriginalRvtMaterialType =
			ERuntimeVirtualTextureMaterialType::BaseColor;
		ERuntimeVirtualTextureMaterialType NewRvtMaterialType =
			ERuntimeVirtualTextureMaterialType::BaseColor;
		bool bOriginalRvtCompression = false;
		bool bNewRvtCompression = false;

		TEnumAsByte<TextureAddress> OriginalSparseAddressX = TA_Wrap;
		TEnumAsByte<TextureAddress> OriginalSparseAddressY = TA_Wrap;
		TEnumAsByte<TextureAddress> OriginalSparseAddressZ = TA_Wrap;
		TEnumAsByte<TextureAddress> NewSparseAddressX = TA_Wrap;
		TEnumAsByte<TextureAddress> NewSparseAddressY = TA_Wrap;
		TEnumAsByte<TextureAddress> NewSparseAddressZ = TA_Wrap;
		bool bOriginalSparseLocalDdcOnly = true;
		bool bNewSparseLocalDdcOnly = true;
		float OriginalSparsePoolSizeFactor = 3.0f;
		float NewSparsePoolSizeFactor = 3.0f;
		int32 OriginalSparsePrefetchFrames = 3;
		int32 NewSparsePrefetchFrames = 3;
		float OriginalSparsePrefetchPercentageStep = 20.0f;
		float NewSparsePrefetchPercentageStep = 20.0f;
		float OriginalSparsePrefetchPercentageBias = 20.0f;
		float NewSparsePrefetchPercentageBias = 20.0f;
		bool bChangeSparseAddressing = false;
		bool bChangeSparseStreaming = false;
	};

	bool TryPrepareCoreAssetUpdate(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FCoreAssetChangeView>& Changes,
		FCoreAssetPreparedState& OutState,
		FExecutionMutationError& OutError)
	{
		OutState.Data.Reset();
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty() && Target) OutError.TargetPath = Target->GetPathName(); };
		const ECoreAssetKind* Kind =
			FindCoreAssetKind(NativeMechanicId);
        if (!Kind)
            return Fail(OutError, JsonFailure(TEXT("asset_data_native_profile_unsupported"),
                TEXT("The native mechanic is not a supported core asset mechanic."), TEXT("native_mechanic_id"),
                MakeShared<FJsonValueString>(NativeMechanicId), MakeShared<FJsonValueString>(TEXT("registered core asset mechanic"))));
        if (!Target)
            return Fail(OutError, NativeFailure(TEXT("asset_data_core_target_missing"),
                TEXT("Preparing the core asset change requires a live target."), TEXT("target"),
                MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("live asset"))));
        if (Changes.IsEmpty()) return Fail(OutError, ChangeSetFailure(Changes, TEXT("at least one supported field change")));
		const TSharedRef<FCoreAssetPreparedData, ESPMode::ThreadSafe>
			Data =
				MakeShared<
					FCoreAssetPreparedData,
					ESPMode::ThreadSafe>();
		Data->Kind = *Kind;

		if (Data->Kind == ECoreAssetKind::AudioEffectPreset)
		{
			USubmixEffectSubmixEQPreset* Preset =
				Cast<USubmixEffectSubmixEQPreset>(Target);
			const FCoreAssetChangeView* Settings =
				FindChange(Changes, TEXT("settings"));
            if (!Preset) return Fail(OutError, NativeFailure(TEXT("asset_data_core_target_type_invalid"), TEXT("The target class is incompatible with this core asset mechanic."), TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(USubmixEffectSubmixEQPreset::StaticClass()->GetPathName())));
            if (!Settings) return Fail(OutError, ValueFailure(TEXT("asset_data_core_change_missing"), TEXT("The required field change is missing."), TEXT("settings"), nullptr, TEXT("field change")));
            if (Changes.Num() != 1) return Fail(OutError, ChangeSetFailure(Changes, TEXT("exactly one settings change")));
			Data->OriginalEqSettings = Preset->Settings;
			if (Settings->Presence == TEXT("clear"))
			{
				Data->NewEqSettings =
					GetDefault<USubmixEffectSubmixEQPreset>()
						->Settings;
			}
			else if (
				!TryRequirePresence(*Settings, {TEXT("set")}, OutError)
				|| !TryRequireChangeValue(*Settings, OutError)
				|| !TryParseEqSettings(
					*Settings->CanonicalValue,
					Data->NewEqSettings, Settings->FieldId, OutError))
			{
				return false;
			}
		}
		else if (Data->Kind == ECoreAssetKind::SoundAttenuation)
		{
			USoundAttenuation* Attenuation =
				Cast<USoundAttenuation>(Target);
			const FCoreAssetChangeView* Settings =
				FindChange(Changes, TEXT("attenuation"));
            if (!Attenuation) return Fail(OutError, NativeFailure(TEXT("asset_data_core_target_type_invalid"), TEXT("The target class is incompatible with this core asset mechanic."), TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundAttenuation::StaticClass()->GetPathName())));
            if (!Settings) return Fail(OutError, ValueFailure(TEXT("asset_data_core_change_missing"), TEXT("The required field change is missing."), TEXT("attenuation"), nullptr, TEXT("field change")));
            if (Changes.Num() != 1) return Fail(OutError, ChangeSetFailure(Changes, TEXT("exactly one attenuation change")));
			Data->OriginalAttenuation = Attenuation->Attenuation;
			Data->NewAttenuation = Data->OriginalAttenuation;
			if (Settings->Presence == TEXT("clear"))
			{
				Data->NewAttenuation =
					GetDefault<USoundAttenuation>()->Attenuation;
			}
			else if (
				!TryRequirePresence(*Settings, {TEXT("set")}, OutError)
				|| !TryRequireChangeValue(*Settings, OutError)
				|| !TryParseAttenuationSettings(
					*Settings->CanonicalValue,
					Data->NewAttenuation, Settings->FieldId, OutError))
			{
				return false;
			}
		}
		else if (Data->Kind == ECoreAssetKind::SoundClass)
		{
			USoundClass* SoundClass = Cast<USoundClass>(Target);
            if (!SoundClass) return Fail(OutError, NativeFailure(TEXT("asset_data_core_target_type_invalid"), TEXT("The target class is incompatible with this core asset mechanic."), TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundClass::StaticClass()->GetPathName())));
            if (Changes.Num() > 2) return Fail(OutError, ChangeSetFailure(Changes, TEXT("at most 2 field changes")));
			Data->OriginalSoundClassProperties =
				SoundClass->Properties;
			Data->NewSoundClassProperties =
				SoundClass->Properties;
			for (USoundClass* Child : SoundClass->ChildClasses)
			{
				Data->NewSoundClassChildren.Add(Child);
				PinUnique(Data->Pins, Child);
			}
			if (const FCoreAssetChangeView* Properties =
				FindChange(Changes, TEXT("properties")))
			{
				Data->bChangeSoundClassProperties = true;
				if (Properties->Presence == TEXT("clear"))
				{
					const FSoundClassProperties& Default =
						GetDefault<USoundClass>()->Properties;
					Data->NewSoundClassProperties->Volume =
						Default.Volume;
					Data->NewSoundClassProperties->Pitch =
						Default.Pitch;
					Data->NewSoundClassProperties->bIsUISound =
						Default.bIsUISound;
					Data->NewSoundClassProperties->bIsMusic =
						Default.bIsMusic;
				}
				else if (
					!TryRequirePresence(*Properties, {TEXT("set")}, OutError)
					|| !TryRequireChangeValue(*Properties, OutError)
					|| !TryParseSoundClassProperties(
						*Properties->CanonicalValue,
						*Data->NewSoundClassProperties, Properties->FieldId, OutError))
				{
					return false;
				}
			}
			if (const FCoreAssetChangeView* Children =
				FindChange(Changes, TEXT("child_classes")))
			{
				Data->bChangeSoundClassChildren = true;
				TArray<USoundClass*> Requested;
				if (Children->Presence != TEXT("clear")
					&& (!TryRequireChangeValue(*Children, OutError)
						|| !TryResolveUniqueArray(
							Resolver,
							*Children->CanonicalValue,
							Requested,
							OutError, Children->FieldId)))
				{
					return false;
				}
				TArray<USoundClass*> NewChildren;
				for (const TWeakObjectPtr<USoundClass>& Child
					: Data->NewSoundClassChildren)
				{
                    if (!Child.IsValid())
                        return Fail(OutError, NativeFailure(TEXT("asset_data_sound_class_child_unavailable"),
                            TEXT("A captured sound class child is no longer available."),
                            TEXT("child_classes/") + FString::FromInt(NewChildren.Num()),
                            MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("live SoundClass"))));
					NewChildren.Add(Child.Get());
				}
				if (Children->Presence == TEXT("clear"))
				{
					NewChildren.Reset();
				}
				else if (Children->Presence == TEXT("replace"))
				{
					NewChildren = Requested;
				}
				else if (Children->Presence == TEXT("remove"))
				{
					NewChildren.RemoveAll(
						[&Requested](USoundClass* Child)
						{
							return Requested.Contains(Child);
						});
				}
				else if (Children->Presence == TEXT("set"))
				{
					for (USoundClass* Child : Requested)
					{
						NewChildren.AddUnique(Child);
					}
				}
                else
                {
                    return TryRequirePresence(*Children, {TEXT("clear"), TEXT("replace"), TEXT("remove"), TEXT("set")}, OutError);
                }
				for (USoundClass* Child : NewChildren)
				{
					if (!Child
						|| Child == SoundClass
						|| Child->RecurseCheckChild(SoundClass))
					{
                        return Fail(OutError, NativeFailure(TEXT("asset_data_sound_class_cycle"),
                            !Child ? TEXT("The resulting child list contains an unavailable sound class.")
                                : Child == SoundClass ? TEXT("A sound class cannot be its own child.")
                                : TEXT("The child relationship would create a sound class cycle."),
                            TEXT("child_classes"), Child ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Child->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                            MakeShared<FJsonValueString>(TEXT("live child that is neither the target nor an ancestor of it"))));
					}
					PinUnique(Data->Pins, Child);
				}
				Data->NewSoundClassChildren.Reset();
				for (USoundClass* Child : NewChildren)
				{
					Data->NewSoundClassChildren.Add(Child);
				}
			}
			if (!Data->bChangeSoundClassProperties
				&& !Data->bChangeSoundClassChildren)
			{
				return Fail(OutError, ChangeSetFailure(Changes, TEXT("properties or child_classes")));
			}
			CaptureSoundClassNode(
				Data->SoundClassSnapshots,
				Data->RelatedPackages,
				SoundClass);
			for (USoundClass* Child : SoundClass->ChildClasses)
			{
				CaptureSoundClassNode(
					Data->SoundClassSnapshots,
					Data->RelatedPackages,
					Child);
				CaptureSoundClassNode(
					Data->SoundClassSnapshots,
					Data->RelatedPackages,
					Child ? Child->ParentClass.Get() : nullptr);
			}
			for (const TWeakObjectPtr<USoundClass>& Child
				: Data->NewSoundClassChildren)
			{
				CaptureSoundClassNode(
					Data->SoundClassSnapshots,
					Data->RelatedPackages,
					Child.Get());
				CaptureSoundClassNode(
					Data->SoundClassSnapshots,
					Data->RelatedPackages,
					Child.IsValid()
						? Child->ParentClass.Get()
						: nullptr);
			}
		}
		else if (Data->Kind == ECoreAssetKind::SoundConcurrency)
		{
			USoundConcurrency* Concurrency =
				Cast<USoundConcurrency>(Target);
			const FCoreAssetChangeView* Settings =
				FindChange(Changes, TEXT("concurrency"));
            if (!Concurrency) return Fail(OutError, NativeFailure(TEXT("asset_data_core_target_type_invalid"), TEXT("The target class is incompatible with this core asset mechanic."), TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundConcurrency::StaticClass()->GetPathName())));
            if (!Settings) return Fail(OutError, ValueFailure(TEXT("asset_data_core_change_missing"), TEXT("The required field change is missing."), TEXT("concurrency"), nullptr, TEXT("field change")));
            if (Changes.Num() != 1) return Fail(OutError, ChangeSetFailure(Changes, TEXT("exactly one concurrency change")));
			Data->OriginalConcurrency = Concurrency->Concurrency;
			Data->NewConcurrency = Data->OriginalConcurrency;
			if (Settings->Presence == TEXT("clear"))
			{
				Data->NewConcurrency =
					GetDefault<USoundConcurrency>()->Concurrency;
			}
			else if (
				!TryRequirePresence(*Settings, {TEXT("set")}, OutError)
				|| !TryRequireChangeValue(*Settings, OutError)
				|| !TryParseConcurrencySettings(
					*Settings->CanonicalValue,
					Data->NewConcurrency, Settings->FieldId, OutError))
			{
				return false;
			}
		}
		else if (Data->Kind == ECoreAssetKind::SoundMix)
		{
			USoundMix* SoundMix = Cast<USoundMix>(Target);
            if (!SoundMix) return Fail(OutError, NativeFailure(TEXT("asset_data_core_target_type_invalid"), TEXT("The target class is incompatible with this core asset mechanic."), TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundMix::StaticClass()->GetPathName())));
            if (Changes.Num() > 2) return Fail(OutError, ChangeSetFailure(Changes, TEXT("at most 2 field changes")));
			Data->OriginalSoundMixModifiers =
				SoundMix->SoundClassEffects;
			Data->NewSoundMixModifiers =
				SoundMix->SoundClassEffects;
			Data->OriginalSoundMixInitialDelay =
				SoundMix->InitialDelay;
			Data->NewSoundMixInitialDelay =
				SoundMix->InitialDelay;
			if (const FCoreAssetChangeView* Modifiers =
				FindChange(Changes, TEXT("modifiers")))
			{
				Data->bChangeSoundMixModifiers = true;
				TArray<FSoundClassAdjuster> Requested;
				if (Modifiers->Presence != TEXT("clear")
					&& (!TryRequireChangeValue(*Modifiers, OutError)
						|| !TryParseSoundMixModifiers(
							Resolver,
							*Modifiers->CanonicalValue,
							Requested,
							Data->Pins,
							OutError, Modifiers->FieldId)))
				{
					return false;
				}
				if (!TryApplyCollectionPresence(
						Modifiers->Presence,
						Requested,
						Data->NewSoundMixModifiers, Modifiers->FieldId, OutError))
				{
					return false;
				}
			}
			if (const FCoreAssetChangeView* Delay =
				FindChange(Changes, TEXT("initial_delay")))
			{
				Data->bChangeSoundMixInitialDelay = true;
				if (Delay->Presence == TEXT("clear"))
				{
					Data->NewSoundMixInitialDelay =
						GetDefault<USoundMix>()->InitialDelay;
				}
				else if (
					!TryRequirePresence(*Delay, {TEXT("set")}, OutError)
					|| !TryRequireChangeValue(*Delay, OutError)
					|| !TryReadFloat(
						*Delay->CanonicalValue,
						Data->NewSoundMixInitialDelay,
						0.0,
						1000000.0, Delay->FieldId, OutError))
				{
					return false;
				}
			}
			if (!Data->bChangeSoundMixModifiers
				&& !Data->bChangeSoundMixInitialDelay)
			{
				return Fail(OutError, ChangeSetFailure(Changes, TEXT("modifiers or initial_delay")));
			}
		}
		else if (Data->Kind == ECoreAssetKind::SoundSubmix)
		{
			USoundSubmix* Submix = Cast<USoundSubmix>(Target);
            if (!Submix) return Fail(OutError, NativeFailure(TEXT("asset_data_core_target_type_invalid"), TEXT("The target class is incompatible with this core asset mechanic."), TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundSubmix::StaticClass()->GetPathName())));
            if (Changes.Num() > 2) return Fail(OutError, ChangeSetFailure(Changes, TEXT("at most 2 field changes")));
			Data->NewSoundSubmixParent = Submix->ParentSubmix;
			for (USoundEffectSubmixPreset* Effect
				: Submix->SubmixEffectChain)
			{
				Data->OriginalSoundSubmixEffects.Add(Effect);
				Data->NewSoundSubmixEffects.Add(Effect);
				PinUnique(Data->Pins, Effect);
			}
			if (const FCoreAssetChangeView* Parent =
				FindChange(Changes, TEXT("parent")))
			{
				Data->bChangeSoundSubmixParent = true;
				USoundSubmixBase* NewParent = nullptr;
				if (Parent->Presence == TEXT("set"))
				{
					if (!TryRequireChangeValue(*Parent, OutError)
						|| !TryResolveObject(
							Resolver,
							*Parent->CanonicalValue,
							NewParent,
							OutError, Parent->FieldId))
					{
						return false;
					}
				}
                else if (!TryRequirePresence(*Parent, {TEXT("null"), TEXT("clear")}, OutError))
                {
                    return false;
                }
				if (WouldCreateSubmixCycle(Submix, NewParent))
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_sound_submix_cycle"),
                        TEXT("The requested parent relationship would create a sound submix cycle."), Parent->FieldId,
                        MakeShared<FJsonValueString>(NewParent->GetPathName()),
                        MakeShared<FJsonValueString>(TEXT("parent outside the target submix descendant chain"))));
				}
				Data->NewSoundSubmixParent = NewParent;
				PinUnique(Data->Pins, NewParent);
			}
			if (const FCoreAssetChangeView* Effects =
				FindChange(Changes, TEXT("effect_chain")))
			{
				Data->bChangeSoundSubmixEffects = true;
				TArray<USoundEffectSubmixPreset*> Requested;
				if (Effects->Presence != TEXT("clear")
					&& (!TryRequireChangeValue(*Effects, OutError)
						|| !TryResolveUniqueArray(
							Resolver,
							*Effects->CanonicalValue,
							Requested,
							OutError, Effects->FieldId)))
				{
					return false;
				}
				TArray<USoundEffectSubmixPreset*> NewEffects;
				for (const TWeakObjectPtr<USoundEffectSubmixPreset>&
					Effect : Data->NewSoundSubmixEffects)
				{
                    if (!Effect.IsValid())
                        return Fail(OutError, NativeFailure(TEXT("asset_data_sound_submix_effect_unavailable"),
                            TEXT("A captured sound submix effect is no longer available."),
                            TEXT("effect_chain/") + FString::FromInt(NewEffects.Num()),
                            MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("live SoundEffectSubmixPreset"))));
					NewEffects.Add(Effect.Get());
				}
				if (Effects->Presence == TEXT("clear"))
				{
					NewEffects.Reset();
				}
				else if (Effects->Presence == TEXT("replace"))
				{
					NewEffects = Requested;
				}
				else if (Effects->Presence == TEXT("remove"))
				{
					NewEffects.RemoveAll(
						[&Requested](
							USoundEffectSubmixPreset* Effect)
						{
							return Requested.Contains(Effect);
						});
				}
				else if (Effects->Presence == TEXT("set"))
				{
					for (USoundEffectSubmixPreset* Effect : Requested)
					{
						NewEffects.AddUnique(Effect);
					}
				}
                else
                {
                    return TryRequirePresence(*Effects, {TEXT("clear"), TEXT("replace"), TEXT("remove"), TEXT("set")}, OutError);
                }
				Data->NewSoundSubmixEffects.Reset();
				for (USoundEffectSubmixPreset* Effect : NewEffects)
				{
					Data->NewSoundSubmixEffects.Add(Effect);
					PinUnique(Data->Pins, Effect);
				}
			}
			if (!Data->bChangeSoundSubmixParent
				&& !Data->bChangeSoundSubmixEffects)
			{
				return Fail(OutError, ChangeSetFailure(Changes, TEXT("parent or effect_chain")));
			}
			CaptureSoundSubmixNode(
				Data->SoundSubmixSnapshots,
				Data->RelatedPackages,
				Submix);
			CaptureSoundSubmixNode(
				Data->SoundSubmixSnapshots,
				Data->RelatedPackages,
				Submix->ParentSubmix);
			CaptureSoundSubmixNode(
				Data->SoundSubmixSnapshots,
				Data->RelatedPackages,
				Data->NewSoundSubmixParent.Get());
		}
		else if (Data->Kind == ECoreAssetKind::Curve)
		{
			const FCoreAssetChangeView* Keys =
				FindChange(Changes, TEXT("keys"));
			const int32 Dimension = GetCurveDimension(Target);
            if (!Keys) return Fail(OutError, ValueFailure(TEXT("asset_data_curve_keys_missing"), TEXT("A curve key change is required."), TEXT("keys"), nullptr, TEXT("field change")));
            if (Changes.Num() != 1) return Fail(OutError, ChangeSetFailure(Changes, TEXT("exactly one keys change")));
            if (!TryRequirePresence(*Keys, {TEXT("replace")}, OutError) || !TryRequireChangeValue(*Keys, OutError)) return false;
            if (Dimension == 0) return Fail(OutError, NativeFailure(TEXT("asset_data_curve_dimension_invalid"),
                TEXT("The target does not provide a supported curve dimension."), TEXT("target"),
                ObjectClassValue(Target), MakeShared<FJsonValueString>(TEXT("CurveFloat, CurveVector or CurveLinearColor"))));
			Data->OriginalCurveChannels.SetNum(Dimension);
			for (int32 Index = 0; Index < Dimension; ++Index)
			{
				FRichCurve* Curve = GetCurveChannel(Target, Index);
                if (!Curve) return Fail(OutError, NativeFailure(TEXT("asset_data_curve_channel_missing"),
                    TEXT("The target curve channel is unavailable."), TEXT("channels/") + FString::FromInt(Index),
                    MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("rich curve channel"))));
				Data->OriginalCurveChannels[Index] =
					Curve->GetCopyOfKeys();
			}
			if (!TryParseCurveKeys(
					*Keys->CanonicalValue,
					Dimension,
					Data->NewCurveChannels, Keys->FieldId, OutError))
			{
				return false;
			}
		}
		else if (Data->Kind == ECoreAssetKind::DataObject)
		{
			const FCoreAssetChangeView* Fields =
				FindChange(Changes, TEXT("registered_fields"));
            if (!Fields) return Fail(OutError, ValueFailure(TEXT("asset_data_registered_fields_missing"), TEXT("The registered field change is missing."), TEXT("registered_fields"), nullptr, TEXT("field change")));
            if (Changes.Num() != 1) return Fail(OutError, ChangeSetFailure(Changes, TEXT("exactly one registered_fields change")));
            if (!TryRequirePresence(*Fields, {TEXT("set"), TEXT("clear")}, OutError)) return false;
			if (!TryParseRegisteredFields(
					Resolver,
					Target,
					Fields->CanonicalValue,
					Fields->Presence == TEXT("clear"),
					Data->OriginalRegisteredFields,
					Data->NewRegisteredFields,
					Data->Pins,
					OutError))
			{
				return false;
			}
		}
		else if (Data->Kind == ECoreAssetKind::MediaSource)
		{
			UFileMediaSource* MediaSource =
				Cast<UFileMediaSource>(Target);
            if (!MediaSource) return Fail(OutError, NativeFailure(TEXT("asset_data_core_target_type_invalid"), TEXT("The target class is incompatible with this core asset mechanic."), TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(UFileMediaSource::StaticClass()->GetPathName())));
            if (Changes.Num() > 2) return Fail(OutError, ChangeSetFailure(Changes, TEXT("at most 2 field changes")));
			Data->OriginalMediaFilePath =
				MediaSource->GetFilePath();
			Data->NewMediaFilePath =
				Data->OriginalMediaFilePath;
			Data->bOriginalMediaPrecache =
				MediaSource->PrecacheFile;
			Data->bNewMediaPrecache =
				Data->bOriginalMediaPrecache;
			if (const FCoreAssetChangeView* Uri =
				FindChange(Changes, TEXT("source_uri")))
			{
				Data->bChangeMediaFilePath = true;
				if (Uri->Presence == TEXT("clear"))
				{
					Data->NewMediaFilePath.Reset();
				}
				else if (
					!TryRequirePresence(*Uri, {TEXT("set")}, OutError)
					|| !TryRequireChangeValue(*Uri, OutError)
					|| !TryNormalizeFileUri(
						*Uri->CanonicalValue,
						Data->NewMediaFilePath, Uri->FieldId, OutError))
				{
					return false;
				}
			}
			if (const FCoreAssetChangeView* Options =
				FindChange(Changes, TEXT("options")))
			{
				Data->bChangeMediaPrecache = true;
				if (Options->Presence == TEXT("clear"))
				{
					Data->bNewMediaPrecache =
						GetDefault<UFileMediaSource>()
							->PrecacheFile;
				}
				else if (
					!TryRequirePresence(*Options, {TEXT("set"), TEXT("replace")}, OutError)
					|| !TryRequireChangeValue(*Options, OutError)
					|| !TryRequireObjectFields(
						*Options->CanonicalValue,
						{TEXT("precache_file")}, Options->FieldId, OutError)
					|| !TryReadBool(
						*FindObjectField(
							*Options->CanonicalValue,
							TEXT("precache_file")),
						Data->bNewMediaPrecache, ChildPath(Options->FieldId, TEXT("precache_file")), OutError))
				{
					return false;
				}
			}
			if (!Data->bChangeMediaFilePath
				&& !Data->bChangeMediaPrecache)
			{
				return Fail(OutError, ChangeSetFailure(Changes, TEXT("source_uri or options")));
			}
		}
		else if (Data->Kind == ECoreAssetKind::RuntimeVirtualTexture)
		{
			URuntimeVirtualTexture* VirtualTexture =
				Cast<URuntimeVirtualTexture>(Target);
            if (!VirtualTexture) return Fail(OutError, NativeFailure(TEXT("asset_data_core_target_type_invalid"),
                TEXT("The target class is incompatible with the runtime virtual texture mechanic."), TEXT("target"),
                ObjectClassValue(Target), MakeShared<FJsonValueString>(URuntimeVirtualTexture::StaticClass()->GetPathName())));
            if (Changes.Num() > 3) return Fail(OutError, ChangeSetFailure(Changes, TEXT("at most three field changes")));
            if (!TryReadRvtProperties(VirtualTexture, Data->OriginalRvtTileCount, Data->OriginalRvtMaterialType, Data->bOriginalRvtCompression, OutError)) return false;
			Data->NewRvtTileCount = Data->OriginalRvtTileCount;
			Data->NewRvtMaterialType =
				Data->OriginalRvtMaterialType;
			Data->bNewRvtCompression =
				Data->bOriginalRvtCompression;
			if (const FCoreAssetChangeView* Size =
				FindChange(Changes, TEXT("size")))
			{
				int32 DesiredSize = 0;
				const int32 TileSize = VirtualTexture->GetTileSize();
				if (!TryRequirePresence(*Size, {TEXT("set")}, OutError)
					|| !TryRequireChangeValue(*Size, OutError)
					|| !TryReadInt32(
						*Size->CanonicalValue,
						DesiredSize,
						TileSize,
						TileSize
							* (1
								<< URuntimeVirtualTexture::
									GetMaxTileCountLog2(
										VirtualTexture
											->GetAdaptivePageTable())), Size->FieldId, OutError) ) return false;
                if (DesiredSize % TileSize != 0
					|| !FMath::IsPowerOfTwo(
						DesiredSize / TileSize))
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_runtime_virtual_texture_size_invalid"),
                        TEXT("The size must be the tile size multiplied by a power of two."), Size->FieldId,
                        Size->CanonicalValue, FString::Printf(TEXT("%d * 2^n within the supported size range"), TileSize)));
				}
				Data->NewRvtTileCount =
					FMath::FloorLog2(DesiredSize / TileSize);
			}
			if (const FCoreAssetChangeView* Material =
				FindChange(Changes, TEXT("material_type")))
			{
				FString Name;
				if (!TryRequirePresence(*Material, {TEXT("set")}, OutError)
					|| !TryRequireChangeValue(*Material, OutError)
					|| !TryReadString(
						*Material->CanonicalValue,
						Name, Material->FieldId, OutError)
					|| !TryParseRuntimeVirtualTextureMaterialType(
						Name,
						Data->NewRvtMaterialType, Material->FieldId, OutError))
				{
					return false;
				}
			}
			if (const FCoreAssetChangeView* Compression =
				FindChange(Changes, TEXT("compression")))
			{
				if (!TryRequirePresence(*Compression, {TEXT("set")}, OutError)
					|| !TryRequireChangeValue(*Compression, OutError)
					|| !TryReadBool(
						*Compression->CanonicalValue,
						Data->bNewRvtCompression, Compression->FieldId, OutError))
				{
					return false;
				}
			}
		}
		else if (Data->Kind == ECoreAssetKind::SparseVolumeTexture)
		{
			UStreamableSparseVolumeTexture* Sparse =
				Cast<UStreamableSparseVolumeTexture>(Target);
            if (!Sparse) return Fail(OutError, NativeFailure(TEXT("asset_data_core_target_type_invalid"), TEXT("The target class is incompatible with this core asset mechanic."), TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(UStreamableSparseVolumeTexture::StaticClass()->GetPathName())));
            if (Changes.Num() > 2) return Fail(OutError, ChangeSetFailure(Changes, TEXT("at most 2 field changes")));
			Data->OriginalSparseAddressX = Sparse->AddressX;
			Data->OriginalSparseAddressY = Sparse->AddressY;
			Data->OriginalSparseAddressZ = Sparse->AddressZ;
			Data->NewSparseAddressX = Sparse->AddressX;
			Data->NewSparseAddressY = Sparse->AddressY;
			Data->NewSparseAddressZ = Sparse->AddressZ;
			Data->bOriginalSparseLocalDdcOnly =
				Sparse->bLocalDDCOnly;
			Data->bNewSparseLocalDdcOnly =
				Sparse->bLocalDDCOnly;
			Data->OriginalSparsePoolSizeFactor =
				Sparse->StreamingPoolSizeFactor;
			Data->NewSparsePoolSizeFactor =
				Sparse->StreamingPoolSizeFactor;
			Data->OriginalSparsePrefetchFrames =
				Sparse->NumberOfPrefetchFrames;
			Data->NewSparsePrefetchFrames =
				Sparse->NumberOfPrefetchFrames;
			Data->OriginalSparsePrefetchPercentageStep =
				Sparse->PrefetchPercentageStepSize;
			Data->NewSparsePrefetchPercentageStep =
				Sparse->PrefetchPercentageStepSize;
			Data->OriginalSparsePrefetchPercentageBias =
				Sparse->PrefetchPercentageBias;
			Data->NewSparsePrefetchPercentageBias =
				Sparse->PrefetchPercentageBias;
			if (const FCoreAssetChangeView* Addressing =
				FindChange(Changes, TEXT("addressing")))
			{
				Data->bChangeSparseAddressing = true;
				if (Addressing->Presence == TEXT("clear"))
				{
					const UStreamableSparseVolumeTexture* Default =
						GetDefault<UStreamableSparseVolumeTexture>();
					Data->NewSparseAddressX = Default->AddressX;
					Data->NewSparseAddressY = Default->AddressY;
					Data->NewSparseAddressZ = Default->AddressZ;
				}
				else
				{
					FString X;
					FString Y;
					FString Z;
					if (!TryRequirePresence(*Addressing, {TEXT("set")}, OutError)
						|| !TryRequireChangeValue(*Addressing, OutError)
						|| !TryRequireObjectFields(
							*Addressing->CanonicalValue,
							{
								TEXT("x"),
								TEXT("y"),
								TEXT("z"),
							}, Addressing->FieldId, OutError)
						|| !TryReadString(
							*FindObjectField(
								*Addressing->CanonicalValue,
								TEXT("x")),
							X, ChildPath(Addressing->FieldId, TEXT("x")), OutError)
						|| !TryReadString(
							*FindObjectField(
								*Addressing->CanonicalValue,
								TEXT("y")),
							Y, ChildPath(Addressing->FieldId, TEXT("y")), OutError)
						|| !TryReadString(
							*FindObjectField(
								*Addressing->CanonicalValue,
								TEXT("z")),
							Z, ChildPath(Addressing->FieldId, TEXT("z")), OutError)
						|| !TryParseTextureAddress(
							X,
							Data->NewSparseAddressX, ChildPath(Addressing->FieldId, TEXT("x")), OutError)
						|| !TryParseTextureAddress(
							Y,
							Data->NewSparseAddressY, ChildPath(Addressing->FieldId, TEXT("y")), OutError)
						|| !TryParseTextureAddress(
							Z,
							Data->NewSparseAddressZ, ChildPath(Addressing->FieldId, TEXT("z")), OutError))
					{
						return false;
					}
				}
			}
			if (const FCoreAssetChangeView* Streaming =
				FindChange(Changes, TEXT("streaming")))
			{
				Data->bChangeSparseStreaming = true;
				if (Streaming->Presence == TEXT("clear"))
				{
					const UStreamableSparseVolumeTexture* Default =
						GetDefault<UStreamableSparseVolumeTexture>();
					Data->bNewSparseLocalDdcOnly =
						Default->bLocalDDCOnly;
					Data->NewSparsePoolSizeFactor =
						Default->StreamingPoolSizeFactor;
					Data->NewSparsePrefetchFrames =
						Default->NumberOfPrefetchFrames;
					Data->NewSparsePrefetchPercentageStep =
						Default->PrefetchPercentageStepSize;
					Data->NewSparsePrefetchPercentageBias =
						Default->PrefetchPercentageBias;
				}
				else if (
					!TryRequirePresence(*Streaming, {TEXT("set")}, OutError)
					|| !TryRequireChangeValue(*Streaming, OutError)
					|| !TryRequireObjectFields(
						*Streaming->CanonicalValue,
						{
							TEXT("local_ddc_only"),
							TEXT("pool_size_factor"),
							TEXT("prefetch_frames"),
							TEXT("prefetch_percentage_step"),
							TEXT("prefetch_percentage_bias"),
						}, Streaming->FieldId, OutError)
					|| !TryReadBool(
						*FindObjectField(
							*Streaming->CanonicalValue,
							TEXT("local_ddc_only")),
						Data->bNewSparseLocalDdcOnly, ChildPath(Streaming->FieldId, TEXT("local_ddc_only")), OutError)
					|| !TryReadFloat(
						*FindObjectField(
							*Streaming->CanonicalValue,
							TEXT("pool_size_factor")),
						Data->NewSparsePoolSizeFactor,
						1.0,
						1000.0, ChildPath(Streaming->FieldId, TEXT("pool_size_factor")), OutError)
					|| !TryReadInt32(
						*FindObjectField(
							*Streaming->CanonicalValue,
							TEXT("prefetch_frames")),
						Data->NewSparsePrefetchFrames,
						0,
						1024, ChildPath(Streaming->FieldId, TEXT("prefetch_frames")), OutError)
					|| !TryReadFloat(
						*FindObjectField(
							*Streaming->CanonicalValue,
							TEXT("prefetch_percentage_step")),
						Data->NewSparsePrefetchPercentageStep,
						0.0,
						100.0, ChildPath(Streaming->FieldId, TEXT("prefetch_percentage_step")), OutError)
					|| !TryReadFloat(
						*FindObjectField(
							*Streaming->CanonicalValue,
							TEXT("prefetch_percentage_bias")),
						Data->NewSparsePrefetchPercentageBias,
						0.0,
						100.0, ChildPath(Streaming->FieldId, TEXT("prefetch_percentage_bias")), OutError))
				{
					return false;
				}
			}
			if (!Data->bChangeSparseAddressing
				&& !Data->bChangeSparseStreaming)
			{
				return Fail(OutError, ChangeSetFailure(Changes, TEXT("addressing or streaming")));
			}
		}
		else
		{
            return Fail(OutError, JsonFailure(TEXT("asset_data_core_asset_mechanic_invalid"),
                TEXT("The registered mechanic has no core asset preparation implementation."), TEXT("native_mechanic_id"),
                MakeShared<FJsonValueString>(NativeMechanicId), MakeShared<FJsonValueString>(TEXT("implemented core asset mechanic"))));
		}
		OutState.Data = Data;
		return true;
	}

	bool TryApplyCoreAssetUpdate(
		UObject* Target,
		const FCoreAssetPreparedState& State,
		FExecutionMutationError& OutError)
	{
        ON_SCOPE_EXIT
        {
            if (!OutError.Code.IsEmpty() && Target && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target->GetPathName();
        };
        if (!Target || !State.Data.IsValid())
            return Fail(OutError, NativeFailure(TEXT("asset_data_core_asset_state_invalid"),
                !Target ? TEXT("The core asset apply target is no longer available.") : TEXT("The prepared core asset state is missing."),
                !Target ? TEXT("target") : TEXT("prepared_state"), MakeShared<FJsonValueNull>(),
                MakeShared<FJsonValueString>(!Target ? TEXT("available target asset") : TEXT("prepared state"))));
		const FCoreAssetPreparedData& Data = *State.Data;
		switch (Data.Kind)
		{
		case ECoreAssetKind::AudioEffectPreset:
			{
				USubmixEffectSubmixEQPreset* Preset =
					Cast<USubmixEffectSubmixEQPreset>(Target);
				if (!Preset)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_core_asset_target_type_invalid"),
                        TEXT("The apply target does not have the prepared asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(USubmixEffectSubmixEQPreset::StaticClass()->GetPathName())));
				Preset->Modify();
				Preset->Settings = Data.NewEqSettings;
				Preset->SetSettings(Data.NewEqSettings);
				Preset->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundAttenuation:
			{
				USoundAttenuation* Attenuation =
					Cast<USoundAttenuation>(Target);
				if (!Attenuation)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_core_asset_target_type_invalid"),
                        TEXT("The apply target does not have the prepared asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundAttenuation::StaticClass()->GetPathName())));
				Attenuation->Modify();
				Attenuation->Attenuation = Data.NewAttenuation;
				Attenuation->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundClass:
			{
				USoundClass* SoundClass = Cast<USoundClass>(Target);
				if (!SoundClass)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_core_asset_target_type_invalid"),
                        TEXT("The apply target does not have the prepared asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundClass::StaticClass()->GetPathName())));
				SoundClass->Modify();
				if (Data.bChangeSoundClassProperties)
				{
					SoundClass->Properties =
						*Data.NewSoundClassProperties;
				}
				if (Data.bChangeSoundClassChildren)
				{
					TArray<USoundClass*> NewChildren;
					for (const TWeakObjectPtr<USoundClass>& Child
						: Data.NewSoundClassChildren)
					{
						if (!Child.IsValid())
                    {
                        auto Error = NativeFailure(TEXT("asset_data_sound_class_child_unavailable"),
                            TEXT("A prepared child SoundClass is no longer available."), TEXT("child_classes/") + FString::FromInt(NewChildren.Num()),
                            MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(USoundClass::StaticClass()->GetPathName()));
                        Error.MutationMayHaveStarted = true;
                        return Fail(OutError, MoveTemp(Error));
                    }
						NewChildren.Add(Child.Get());
					}
					for (USoundClass* OldChild
						: SoundClass->ChildClasses)
					{
						if (OldChild
							&& !NewChildren.Contains(OldChild)
							&& OldChild->ParentClass == SoundClass)
						{
							OldChild->Modify();
							OldChild->ParentClass = nullptr;
							OldChild->PostEditChange();
						}
					}
					for (USoundClass* Child : NewChildren)
					{
						if (Child->ParentClass
							&& Child->ParentClass != SoundClass)
						{
							Child->ParentClass->Modify();
							Child->ParentClass
								->ChildClasses.Remove(Child);
							Child->ParentClass->PostEditChange();
						}
						Child->Modify();
						Child->ParentClass = SoundClass;
						Child->PostEditChange();
					}
					SoundClass->ChildClasses.Reset(
						NewChildren.Num());
					for (USoundClass* Child : NewChildren)
					{
						SoundClass->ChildClasses.Add(Child);
					}
				}
				SoundClass->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundConcurrency:
			{
				USoundConcurrency* Concurrency =
					Cast<USoundConcurrency>(Target);
				if (!Concurrency)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_core_asset_target_type_invalid"),
                        TEXT("The apply target does not have the prepared asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundConcurrency::StaticClass()->GetPathName())));
				Concurrency->Modify();
				Concurrency->Concurrency = Data.NewConcurrency;
				Concurrency->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundMix:
			{
				USoundMix* SoundMix = Cast<USoundMix>(Target);
				if (!SoundMix)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_core_asset_target_type_invalid"),
                        TEXT("The apply target does not have the prepared asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundMix::StaticClass()->GetPathName())));
				SoundMix->Modify();
				if (Data.bChangeSoundMixModifiers)
				{
					SoundMix->SoundClassEffects =
						Data.NewSoundMixModifiers;
				}
				if (Data.bChangeSoundMixInitialDelay)
				{
					SoundMix->InitialDelay =
						Data.NewSoundMixInitialDelay;
				}
				SoundMix->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundSubmix:
			{
				USoundSubmix* Submix = Cast<USoundSubmix>(Target);
				if (!Submix)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_core_asset_target_type_invalid"),
                        TEXT("The apply target does not have the prepared asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundSubmix::StaticClass()->GetPathName())));
				Submix->Modify();
				if (Data.bChangeSoundSubmixParent)
				{
					Submix->SetParentSubmix(
						Data.NewSoundSubmixParent.Get(),
						true);
				}
				if (Data.bChangeSoundSubmixEffects)
				{
					Submix->SubmixEffectChain.Reset(
						Data.NewSoundSubmixEffects.Num());
					for (const TWeakObjectPtr<
							USoundEffectSubmixPreset>& Effect
						: Data.NewSoundSubmixEffects)
					{
						if (!Effect.IsValid())
                    {
                        auto Error = NativeFailure(TEXT("asset_data_sound_submix_effect_unavailable"),
                            TEXT("A prepared submix effect is no longer available."), TEXT("effect_chain/") + FString::FromInt(Submix->SubmixEffectChain.Num()),
                            MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(USoundEffectSubmixPreset::StaticClass()->GetPathName()));
                        Error.MutationMayHaveStarted = true;
                        return Fail(OutError, MoveTemp(Error));
                    }
						Submix->SubmixEffectChain.Add(Effect.Get());
					}
				}
				Submix->PostEditChange();
				break;
			}
		case ECoreAssetKind::Curve:
            return WriteCurveChannels(Target, Data.NewCurveChannels, true, OutError);
		case ECoreAssetKind::DataObject:
			{
				Target->Modify();
				for (const FRegisteredFieldValue& Value
					: Data.NewRegisteredFields)
				{
					WriteRegisteredValue(Target, Value);
				}
				Target->PostEditChange();
				break;
			}
		case ECoreAssetKind::MediaSource:
			{
				UFileMediaSource* MediaSource =
					Cast<UFileMediaSource>(Target);
				if (!MediaSource)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_core_asset_target_type_invalid"),
                        TEXT("The apply target does not have the prepared asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(UFileMediaSource::StaticClass()->GetPathName())));
				MediaSource->Modify();
				if (Data.bChangeMediaFilePath)
				{
					MediaSource->SetFilePath(
						Data.NewMediaFilePath);
				}
				if (Data.bChangeMediaPrecache)
				{
					MediaSource->PrecacheFile =
						Data.bNewMediaPrecache;
				}
				MediaSource->PostEditChange();
				break;
			}
		case ECoreAssetKind::RuntimeVirtualTexture:
            return TryWriteRvtProperties(Target, Data.NewRvtTileCount, Data.NewRvtMaterialType,
                Data.bNewRvtCompression, true, OutError);
		case ECoreAssetKind::SparseVolumeTexture:
			{
				UStreamableSparseVolumeTexture* Sparse =
					Cast<UStreamableSparseVolumeTexture>(Target);
				if (!Sparse)
                    return Fail(OutError, NativeFailure(TEXT("asset_data_core_asset_target_type_invalid"),
                        TEXT("The apply target does not have the prepared asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(UStreamableSparseVolumeTexture::StaticClass()->GetPathName())));
				Sparse->Modify();
				if (Data.bChangeSparseAddressing)
				{
					Sparse->AddressX = Data.NewSparseAddressX;
					Sparse->AddressY = Data.NewSparseAddressY;
					Sparse->AddressZ = Data.NewSparseAddressZ;
				}
				if (Data.bChangeSparseStreaming)
				{
					Sparse->bLocalDDCOnly =
						Data.bNewSparseLocalDdcOnly;
					Sparse->StreamingPoolSizeFactor =
						Data.NewSparsePoolSizeFactor;
					Sparse->NumberOfPrefetchFrames =
						Data.NewSparsePrefetchFrames;
					Sparse->PrefetchPercentageStepSize =
						Data.NewSparsePrefetchPercentageStep;
					Sparse->PrefetchPercentageBias =
						Data.NewSparsePrefetchPercentageBias;
				}
				Sparse->PostEditChange();
				break;
			}
		}
		return true;
	}

	bool TryRollbackCoreAssetUpdate(
		UObject* Target,
		const FCoreAssetPreparedState& State,
		FExecutionMutationError& OutError)
	{
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && Target && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target->GetPathName(); };
		if (!Target || !State.Data.IsValid())
		{
            OutError = NativeFailure(TEXT("asset_data_core_rollback_state_invalid"),
                !Target ? TEXT("The asset rollback target is unavailable.") : TEXT("Asset rollback has no captured state."),
                !Target ? TEXT("target") : TEXT("prepared_state"),
                !Target ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
                MakeShared<FJsonValueString>(!Target ? TEXT("live asset target") : TEXT("captured rollback state")));
            return false;
		}
		const FCoreAssetPreparedData& Data = *State.Data;
		bool bRestored = true;
		switch (Data.Kind)
		{
		case ECoreAssetKind::AudioEffectPreset:
			{
				USubmixEffectSubmixEQPreset* Preset =
					Cast<USubmixEffectSubmixEQPreset>(Target);
				if (!Preset)
				{
                    OutError = NativeFailure(TEXT("asset_data_core_rollback_target_invalid"),
                        TEXT("The rollback target does not match the captured asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(USubmixEffectSubmixEQPreset::StaticClass()->GetPathName()));
                    return false;
                }
				Preset->Settings = Data.OriginalEqSettings;
				Preset->SetSettings(Data.OriginalEqSettings);
				Preset->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundAttenuation:
			{
				USoundAttenuation* Attenuation =
					Cast<USoundAttenuation>(Target);
				if (!Attenuation)
				{
                    OutError = NativeFailure(TEXT("asset_data_core_rollback_target_invalid"),
                        TEXT("The rollback target does not match the captured asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundAttenuation::StaticClass()->GetPathName()));
                    return false;
                }
				Attenuation->Attenuation =
					Data.OriginalAttenuation;
				Attenuation->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundClass:
			{
				USoundClass* SoundClass = Cast<USoundClass>(Target);
				if (!SoundClass)
				{
                    OutError = NativeFailure(TEXT("asset_data_core_rollback_target_invalid"),
                        TEXT("The rollback target does not match the captured asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundClass::StaticClass()->GetPathName()));
                    return false;
                }
				SoundClass->Properties =
					*Data.OriginalSoundClassProperties;
                for (int32 NodeIndex = 0; NodeIndex < Data.SoundClassSnapshots.Num(); ++NodeIndex)
                {
                    const FSoundClassNodeSnapshot& Snapshot = Data.SoundClassSnapshots[NodeIndex];
					USoundClass* Node = Snapshot.Node.Get();
					if (!Node)
					{
						auto Error = NativeFailure(TEXT("asset_data_core_rollback_reference_expired"),
                            TEXT("A captured SoundClass hierarchy node is no longer available."), TEXT("sound_class_nodes/") + FString::FromInt(NodeIndex),
                            MakeShared<FJsonValueString>(TEXT("expired")), MakeShared<FJsonValueString>(USoundClass::StaticClass()->GetPathName()));
                        Error.MutationMayHaveStarted = true;
                        Error.TargetPath = Target->GetPathName();
                        OutError.Append(Error);
                        bRestored = false;
						continue;
					}
					Node->ParentClass = Snapshot.Parent.Get();
					Node->ChildClasses.Reset(
						Snapshot.Children.Num());
                    for (int32 ChildIndex = 0; ChildIndex < Snapshot.Children.Num(); ++ChildIndex)
                    {
                        const TWeakObjectPtr<USoundClass>& Child = Snapshot.Children[ChildIndex];
						if (!Child.IsValid())
						{
							auto Error = NativeFailure(TEXT("asset_data_core_rollback_reference_expired"),
                            TEXT("A captured SoundClass child is no longer available."), TEXT("sound_class_nodes/") + FString::FromInt(NodeIndex) + TEXT("/children/") + FString::FromInt(ChildIndex),
                            MakeShared<FJsonValueString>(TEXT("expired")), MakeShared<FJsonValueString>(USoundClass::StaticClass()->GetPathName()));
                        Error.MutationMayHaveStarted = true;
                        Error.TargetPath = Node->GetPathName();
                        OutError.Append(Error);
                        bRestored = false;
							continue;
						}
						Node->ChildClasses.Add(Child.Get());
					}
					Node->PostEditChange();
				}
				SoundClass->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundConcurrency:
			{
				USoundConcurrency* Concurrency =
					Cast<USoundConcurrency>(Target);
				if (!Concurrency)
				{
                    OutError = NativeFailure(TEXT("asset_data_core_rollback_target_invalid"),
                        TEXT("The rollback target does not match the captured asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundConcurrency::StaticClass()->GetPathName()));
                    return false;
                }
				Concurrency->Concurrency =
					Data.OriginalConcurrency;
				Concurrency->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundMix:
			{
				USoundMix* SoundMix = Cast<USoundMix>(Target);
				if (!SoundMix)
				{
                    OutError = NativeFailure(TEXT("asset_data_core_rollback_target_invalid"),
                        TEXT("The rollback target does not match the captured asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundMix::StaticClass()->GetPathName()));
                    return false;
                }
				SoundMix->SoundClassEffects =
					Data.OriginalSoundMixModifiers;
				SoundMix->InitialDelay =
					Data.OriginalSoundMixInitialDelay;
				SoundMix->PostEditChange();
				break;
			}
		case ECoreAssetKind::SoundSubmix:
			{
				USoundSubmix* Submix = Cast<USoundSubmix>(Target);
				if (!Submix)
				{
                    OutError = NativeFailure(TEXT("asset_data_core_rollback_target_invalid"),
                        TEXT("The rollback target does not match the captured asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(USoundSubmix::StaticClass()->GetPathName()));
                    return false;
                }
				Submix->SubmixEffectChain.Reset(
					Data.OriginalSoundSubmixEffects.Num());
                for (int32 EffectIndex = 0; EffectIndex < Data.OriginalSoundSubmixEffects.Num(); ++EffectIndex)
                {
                    const TWeakObjectPtr<USoundEffectSubmixPreset>& Effect = Data.OriginalSoundSubmixEffects[EffectIndex];
					if (!Effect.IsValid())
					{
						auto Error = NativeFailure(TEXT("asset_data_core_rollback_reference_expired"),
                            TEXT("A captured sound submix effect is no longer available."), TEXT("effects/") + FString::FromInt(EffectIndex),
                            MakeShared<FJsonValueString>(TEXT("expired")), MakeShared<FJsonValueString>(USoundEffectSubmixPreset::StaticClass()->GetPathName()));
                        Error.MutationMayHaveStarted = true;
                        Error.TargetPath = Target->GetPathName();
                        OutError.Append(Error);
                        bRestored = false;
						continue;
					}
					Submix->SubmixEffectChain.Add(Effect.Get());
				}
                for (int32 NodeIndex = 0; NodeIndex < Data.SoundSubmixSnapshots.Num(); ++NodeIndex)
                {
                    const FSoundSubmixNodeSnapshot& Snapshot = Data.SoundSubmixSnapshots[NodeIndex];
					USoundSubmixBase* Node = Snapshot.Node.Get();
					if (!Node)
					{
						auto Error = NativeFailure(TEXT("asset_data_core_rollback_reference_expired"),
                            TEXT("A captured sound submix hierarchy node is no longer available."), TEXT("sound_submix_nodes/") + FString::FromInt(NodeIndex),
                            MakeShared<FJsonValueString>(TEXT("expired")), MakeShared<FJsonValueString>(USoundSubmixBase::StaticClass()->GetPathName()));
                        Error.MutationMayHaveStarted = true;
                        Error.TargetPath = Target->GetPathName();
                        OutError.Append(Error);
                        bRestored = false;
						continue;
					}
					Node->ChildSubmixes.Reset(
						Snapshot.Children.Num());
                    for (int32 ChildIndex = 0; ChildIndex < Snapshot.Children.Num(); ++ChildIndex)
                    {
                        const TWeakObjectPtr<USoundSubmixBase>& Child = Snapshot.Children[ChildIndex];
						if (!Child.IsValid())
						{
							auto Error = NativeFailure(TEXT("asset_data_core_rollback_reference_expired"),
                            TEXT("A captured sound submix child is no longer available."), TEXT("sound_submix_nodes/") + FString::FromInt(NodeIndex) + TEXT("/children/") + FString::FromInt(ChildIndex),
                            MakeShared<FJsonValueString>(TEXT("expired")), MakeShared<FJsonValueString>(USoundSubmixBase::StaticClass()->GetPathName()));
                        Error.MutationMayHaveStarted = true;
                        Error.TargetPath = Node->GetPathName();
                        OutError.Append(Error);
                        bRestored = false;
							continue;
						}
						Node->ChildSubmixes.Add(Child.Get());
					}
					if (USoundSubmixWithParentBase* WithParent =
						Cast<USoundSubmixWithParentBase>(Node))
					{
						WithParent->ParentSubmix =
							Snapshot.Parent.Get();
					}
					Node->PostEditChange();
				}
				Submix->PostEditChange();
				break;
			}
		case ECoreAssetKind::Curve:
			bRestored = WriteCurveChannels(
				Target,
				Data.OriginalCurveChannels,
				false, OutError);
			break;
		case ECoreAssetKind::DataObject:
			for (const FRegisteredFieldValue& Value
				: Data.OriginalRegisteredFields)
			{
				WriteRegisteredValue(Target, Value);
			}
			Target->PostEditChange();
			break;
		case ECoreAssetKind::MediaSource:
			{
				UFileMediaSource* MediaSource =
					Cast<UFileMediaSource>(Target);
				if (!MediaSource)
				{
                    OutError = NativeFailure(TEXT("asset_data_core_rollback_target_invalid"),
                        TEXT("The rollback target does not match the captured asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(UFileMediaSource::StaticClass()->GetPathName()));
                    return false;
                }
				MediaSource->SetFilePath(
					Data.OriginalMediaFilePath);
				MediaSource->PrecacheFile =
					Data.bOriginalMediaPrecache;
				MediaSource->PostEditChange();
				break;
			}
		case ECoreAssetKind::RuntimeVirtualTexture:
			bRestored = TryWriteRvtProperties(
				Target,
				Data.OriginalRvtTileCount,
				Data.OriginalRvtMaterialType,
				Data.bOriginalRvtCompression,
				false, OutError);
			break;
		case ECoreAssetKind::SparseVolumeTexture:
			{
				UStreamableSparseVolumeTexture* Sparse =
					Cast<UStreamableSparseVolumeTexture>(Target);
				if (!Sparse)
				{
                    OutError = NativeFailure(TEXT("asset_data_core_rollback_target_invalid"),
                        TEXT("The rollback target does not match the captured asset type."), TEXT("target"),
                        ObjectClassValue(Target), MakeShared<FJsonValueString>(UStreamableSparseVolumeTexture::StaticClass()->GetPathName()));
                    return false;
                }
				Sparse->AddressX = Data.OriginalSparseAddressX;
				Sparse->AddressY = Data.OriginalSparseAddressY;
				Sparse->AddressZ = Data.OriginalSparseAddressZ;
				Sparse->bLocalDDCOnly =
					Data.bOriginalSparseLocalDdcOnly;
				Sparse->StreamingPoolSizeFactor =
					Data.OriginalSparsePoolSizeFactor;
				Sparse->NumberOfPrefetchFrames =
					Data.OriginalSparsePrefetchFrames;
				Sparse->PrefetchPercentageStepSize =
					Data.OriginalSparsePrefetchPercentageStep;
				Sparse->PrefetchPercentageBias =
					Data.OriginalSparsePrefetchPercentageBias;
				Sparse->PostEditChange();
				break;
			}
		}
		RestorePackageFlags(Data.RelatedPackages);
		return bRestored;
	}

	bool CanProjectCoreAssetFieldState(
		const FString& NativeMechanicId)
	{
		const ECoreAssetKind* Kind =
			FindCoreAssetKind(NativeMechanicId);
		return Kind != nullptr;
	}

	bool TryProjectCoreAssetCurrentFieldState(
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
		OutCurrentFields.Reset();
		OutDefaultFields.Reset();
		OutProjectionContext.Reset();
		const ECoreAssetKind* Kind =
			FindCoreAssetKind(NativeMechanicId);
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty() && Target) OutError.TargetPath = Target->GetPathName(); };
        if (!CanProjectCoreAssetFieldState(NativeMechanicId) || !Kind)
            return Fail(OutError, NativeFailure(TEXT("asset_data_core_field_projection_unsupported"),
                TEXT("The native mechanic does not support core asset field projection."), TEXT("native_mechanic_id"),
                MakeShared<FJsonValueString>(NativeMechanicId), MakeShared<FJsonValueString>(TEXT("registered core field projection mechanic"))));
        if (!Target) return Fail(OutError, NativeFailure(TEXT("asset_data_core_projection_target_missing"),
            TEXT("Core asset projection requires a live target."), TEXT("target"),
            MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("live asset"))));
		if (*Kind == ECoreAssetKind::MediaSource
			|| *Kind
				== ECoreAssetKind::RuntimeVirtualTexture
			|| *Kind
				== ECoreAssetKind::SparseVolumeTexture)
		{
			return TryBuildCoreConfigurationProjection(
				*Kind,
				Target,
				SelectedFieldIds,
				Resources,
				OutCurrentFields,
				OutDefaultFields,
				OutProjectionContext,
				OutError);
		}
		if (*Kind != ECoreAssetKind::DataObject)
		{
			return TryBuildCoreAudioCurveProjection(
				*Kind,
				Target,
				SelectedFieldIds,
				Resources,
				OutCurrentFields,
				OutDefaultFields,
				OutProjectionContext,
				OutError);
		}
		if (SelectedFieldIds
			!= TArray<FString>{TEXT("registered_fields")})
		{
            TArray<TSharedPtr<FJsonValue>> Actual;
            for (const auto& Field : SelectedFieldIds) Actual.Add(MakeShared<FJsonValueString>(Field));
            return Fail(OutError, NativeFailure(TEXT("asset_data_core_field_projection_unsupported"),
                TEXT("The data object projection requires exactly the registered_fields selection."), TEXT("selected_field_ids"),
                MakeShared<FJsonValueArray>(Actual), MakeShared<FJsonValueString>(TEXT("one field: registered_fields"))));
		}

		FUeAgentCanonicalValuePtr CurrentRegistered;
		FUeAgentCanonicalValuePtr CurrentContext;
		if (!TryBuildRegisteredFieldProjection(
				Target,
				Resources,
				CurrentRegistered,
				CurrentContext,
				OutError))
		{
			return false;
		}
		UObject* Default =
			Target->GetClass()->GetDefaultObject();
		FUeAgentCanonicalValuePtr DefaultRegistered;
		FUeAgentCanonicalValuePtr DefaultContext;
		if (!TryBuildRegisteredFieldProjection(
				Default,
				Resources,
				DefaultRegistered,
				DefaultContext,
				OutError))
		{
			return false;
		}
		FUeAgentCanonicalValuePtr IgnoredContext;
		if (!TryWrapRegisteredFieldProjection(
				CurrentRegistered.ToSharedRef(),
				CurrentContext.ToSharedRef(),
				Resources,
				OutCurrentFields,
				OutProjectionContext,
				OutError)
			|| !TryWrapRegisteredFieldProjection(
				DefaultRegistered.ToSharedRef(),
				DefaultContext.ToSharedRef(),
				Resources,
				OutDefaultFields,
				IgnoredContext,
				OutError))
		{
			return false;
		}
        if (OutCurrentFields.IsValid() && OutDefaultFields.IsValid() && OutProjectionContext.IsValid()) return true;
        return Fail(OutError, NativeFailure(TEXT("asset_data_core_projection_result_missing"),
            TEXT("A required projection result was not produced."),
            !OutCurrentFields.IsValid() ? TEXT("current_fields") : !OutDefaultFields.IsValid() ? TEXT("default_fields") : TEXT("projection_context"),
            MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("canonical object"))));
	}

	bool TryProjectCoreAssetExpectedFieldState(
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
		const ECoreAssetKind* Kind =
			FindCoreAssetKind(NativeMechanicId);
		if (!CanProjectCoreAssetFieldState(NativeMechanicId)
			|| !Kind)
		{
            return Fail(OutError, NativeFailure(TEXT("asset_data_core_field_projection_unsupported"),
                TEXT("The native mechanic does not support expected core asset field projection."), TEXT("native_mechanic_id"),
                MakeShared<FJsonValueString>(NativeMechanicId), MakeShared<FJsonValueString>(TEXT("registered core field projection mechanic"))));
		}
		if (*Kind == ECoreAssetKind::MediaSource
			|| *Kind
				== ECoreAssetKind::RuntimeVirtualTexture
			|| *Kind
				== ECoreAssetKind::SparseVolumeTexture)
		{
			return TryBuildExpectedCoreConfigurationFields(
				*Kind,
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Resources,
				OutExpectedFields,
				OutError);
		}
		if (*Kind != ECoreAssetKind::DataObject)
		{
			return TryBuildExpectedCoreAudioCurveFields(
				Resolver,
				*Kind,
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Resources,
				OutExpectedFields,
				OutError);
		}
		return TryBuildExpectedRegisteredFields(
			Resolver,
			CurrentFields,
			DefaultFields,
			ProjectionContext,
			ProposedAfter,
			Resources,
			OutExpectedFields,
			OutError);
	}
}
