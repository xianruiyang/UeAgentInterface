#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectPaperMechanics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Misc/ScopeExit.h"

#include "Engine/Texture2D.h"
#include "PaperFlipbook.h"
#include "PaperSprite.h"
#include "PaperTileLayer.h"
#include "PaperTileMap.h"
#include "PaperTileSet.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		const TCHAR* PaperFlipbookMechanicId =
			TEXT("uai.asset_data.mechanic:replace_paper_flipbook:v1");
		const TCHAR* PaperSpriteMechanicId =
			TEXT("uai.asset_data.mechanic:update_paper_sprite:v1");
		const TCHAR* PaperTileMapMechanicId =
			TEXT("uai.asset_data.mechanic:replace_paper_tilemap:v1");
		const TCHAR* PaperTileSetMechanicId =
			TEXT("uai.asset_data.mechanic:replace_paper_tileset:v1");

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

        TSharedPtr<FJsonValue> NumberValue(const double Value)
        {
            return FMath::IsFinite(Value) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Value))
                : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FString::SanitizeFloat(Value)));
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
						? TEXT("asset_data_paper_projection_string_invalid")
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
						? TEXT("asset_data_paper_projection_boolean_invalid")
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
						? TEXT("asset_data_paper_projection_integer_invalid")
						: StateError.Code, StateError, TEXT("execution")));
		}

        bool TryBuildDecimal(const double Value, FUeAgentResourceTracker& Resources,
            FUeAgentCanonicalValuePtr& OutValue, FExecutionMutationError& OutError,
            const FString& Field = FString())
        {
            if (!FMath::IsFinite(Value))
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_projection_decimal_invalid"),
                    TEXT("The native value cannot be represented as a finite decimal."), Field,
                    NumberValue(Value), MakeShared<FJsonValueString>(TEXT("finite number")), TEXT("native_result")));
            FUeAgentStateSemanticError StateError;
            if (!FUeAgentCanonicalValue::TryDecimal(FString::SanitizeFloat(Value), Resources, OutValue, StateError))
            {
                OutError = Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
                    ? TEXT("asset_data_paper_projection_decimal_invalid") : StateError.Code, StateError, TEXT("execution"));
                if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Field;
                return false;
            }
            return true;
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
						? TEXT("asset_data_paper_projection_null_invalid")
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
						? TEXT("asset_data_paper_projection_array_invalid")
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
						? TEXT("asset_data_paper_projection_object_invalid")
						: StateError.Code, StateError, TEXT("execution")));
		}

		bool TryBuildReference(
			const UObject* Object,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			return Object
				? TryBuildString(
					Object->GetPathName(),
					Resources,
					OutValue,
					OutError)
				: TryBuildNull(Resources, OutValue, OutError);
		}

        bool TryReadString(const FUeAgentCanonicalValue& Value, FString& OutText,
            const FString& Field, FExecutionMutationError& OutError,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            if (Value.GetKind() != EUeAgentValueKind::String || Value.GetText().IsEmpty())
            {
                OutText.Reset();
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_string_invalid"),
                    TEXT("A nonempty string is required."), Field, &Value, TEXT("nonempty string"), SourceKind));
            }
            OutText = Value.GetText();
            return true;
        }

        bool TryReadNumber(const FUeAgentCanonicalValue& Value, double& OutNumber,
            const FString& Field, FExecutionMutationError& OutError,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            if (Value.GetKind() != EUeAgentValueKind::Integer && Value.GetKind() != EUeAgentValueKind::Decimal)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_number_invalid"),
                    TEXT("A numeric value is required."), Field, &Value, TEXT("finite number"), SourceKind));
            if (!LexTryParseString(OutNumber, *Value.GetLexeme()) || !FMath::IsFinite(OutNumber))
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_number_invalid"),
                    TEXT("The number cannot be represented as a finite native value."), Field, &Value, TEXT("finite number"), SourceKind));
            return true;
        }

        bool TryReadInt32(const FUeAgentCanonicalValue& Value, int32& OutValue,
            const FString& Field, FExecutionMutationError& OutError,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            int64 Number = 0;
            if ((Value.GetKind() != EUeAgentValueKind::Integer && Value.GetKind() != EUeAgentValueKind::Decimal)
                || !TryParseInt64Number(Value.GetLexeme(), Number) || Number < MIN_int32 || Number > MAX_int32)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_int32_invalid"),
                    TEXT("The value must be an integer representable as int32."), Field, &Value,
                    TEXT("integer in [-2147483648, 2147483647]"), SourceKind));
            OutValue = static_cast<int32>(Number);
            return true;
        }

        bool TryReadBooleanField(const FUeAgentCanonicalValue& Object, const FString& Field, bool& bOutValue,
            const FString& Parent, FExecutionMutationError& OutError)
        {
            const FUeAgentCanonicalValue* Value = FindObjectField(Object, Field);
            if (!Value || Value->GetKind() != EUeAgentValueKind::Boolean)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_boolean_invalid"),
                    Value ? TEXT("The field must be a boolean.") : TEXT("The required boolean field is missing."),
                    Parent + TEXT("/") + Field, Value, TEXT("boolean")));
            bOutValue = Value->GetBoolean();
            return true;
        }

        bool TryReadExtent2i(const FUeAgentCanonicalValue& Value, const int32 Minimum, const int32 Maximum,
            FIntPoint& OutExtent, const FString& Field, FExecutionMutationError& OutError,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            if (Value.GetKind() != EUeAgentValueKind::Object || Value.GetObjectItems().Num() != 2)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_extent_invalid"),
                    TEXT("The extent must contain exactly x and y."), Field, &Value, TEXT("object with x and y"), SourceKind));
            const FUeAgentCanonicalValue* X = FindObjectField(Value, TEXT("x"));
            const FUeAgentCanonicalValue* Y = FindObjectField(Value, TEXT("y"));
            if (!X || !Y)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_extent_invalid"),
                    TEXT("A required extent component is missing."), Field + (!X ? TEXT("/x") : TEXT("/y")),
                    nullptr, TEXT("int32 component"), SourceKind));
            if (!TryReadInt32(*X, OutExtent.X, Field + TEXT("/x"), OutError, SourceKind)
                || !TryReadInt32(*Y, OutExtent.Y, Field + TEXT("/y"), OutError, SourceKind)) return false;
            if (OutExtent.X < Minimum || OutExtent.Y < Minimum || OutExtent.X > Maximum || OutExtent.Y > Maximum)
            {
                const bool bX = OutExtent.X < Minimum || (OutExtent.Y >= Minimum && OutExtent.X > Maximum);
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_extent_invalid"),
                    TEXT("The extent component is outside the supported range."), Field + (bX ? TEXT("/x") : TEXT("/y")),
                    bX ? X : Y, FString::Printf(TEXT("integer in [%d, %d]"), Minimum, Maximum), SourceKind));
            }
            return true;
        }

		bool TryBuildExtent2i(
			const FIntPoint& Extent,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr X;
			FUeAgentCanonicalValuePtr Y;
			return TryBuildInteger(Extent.X, Resources, X, OutError)
				&& TryBuildInteger(Extent.Y, Resources, Y, OutError)
				&& TryBuildObject(
					{
						{TEXT("x"), X.ToSharedRef()},
						{TEXT("y"), Y.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildRect2d(
			const FIntPoint& Offset,
			const FIntPoint& Size,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr X;
			FUeAgentCanonicalValuePtr Y;
			FUeAgentCanonicalValuePtr Width;
			FUeAgentCanonicalValuePtr Height;
			return TryBuildInteger(Offset.X, Resources, X, OutError)
				&& TryBuildInteger(Offset.Y, Resources, Y, OutError)
				&& TryBuildInteger(Size.X, Resources, Width, OutError)
				&& TryBuildInteger(Size.Y, Resources, Height, OutError)
				&& TryBuildObject(
					{
						{TEXT("height"), Height.ToSharedRef()},
						{TEXT("width"), Width.ToSharedRef()},
						{TEXT("x"), X.ToSharedRef()},
						{TEXT("y"), Y.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

        bool TryReadRect2d(const FUeAgentCanonicalValue& Value, FIntPoint& OutOffset, FIntPoint& OutSize,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::Object || Value.GetObjectItems().Num() != 4)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_region_invalid"),
                    TEXT("The sprite region must contain exactly x, y, width and height."), Field, &Value,
                    TEXT("object with x, y, width and height")));
            const TCHAR* Names[] = {TEXT("x"), TEXT("y"), TEXT("width"), TEXT("height")};
            const FUeAgentCanonicalValue* Values[] = {FindObjectField(Value, Names[0]), FindObjectField(Value, Names[1]),
                FindObjectField(Value, Names[2]), FindObjectField(Value, Names[3])};
            for (int32 Index = 0; Index < 4; ++Index)
                if (!Values[Index])
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_region_invalid"),
                        TEXT("A required sprite region component is missing."), Field + TEXT("/") + Names[Index],
                        nullptr, TEXT("int32 component")));
            int32* Components[] = {&OutOffset.X, &OutOffset.Y, &OutSize.X, &OutSize.Y};
            for (int32 Index = 0; Index < 4; ++Index)
                if (!TryReadInt32(*Values[Index], *Components[Index], Field + TEXT("/") + Names[Index], OutError)) return false;
            for (int32 Index = 0; Index < 4; ++Index)
            {
                if (*Components[Index] < (Index < 2 ? 0 : 1))
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_region_invalid"),
                        Index < 2 ? TEXT("The sprite region offset must be nonnegative.") : TEXT("The sprite region size must be positive."),
                        Field + TEXT("/") + Names[Index], Values[Index], Index < 2 ? TEXT("integer >= 0") : TEXT("integer >= 1")));
            }
            return true;
        }

		bool TryBuildLinearColor(
			const FLinearColor& Color,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError, const FString& Field)
		{
			FUeAgentCanonicalValuePtr R;
			FUeAgentCanonicalValuePtr G;
			FUeAgentCanonicalValuePtr B;
			FUeAgentCanonicalValuePtr A;
			return TryBuildDecimal(Color.R, Resources, R, OutError, Field + TEXT("/r"))
				&& TryBuildDecimal(Color.G, Resources, G, OutError, Field + TEXT("/g"))
				&& TryBuildDecimal(Color.B, Resources, B, OutError, Field + TEXT("/b"))
				&& TryBuildDecimal(Color.A, Resources, A, OutError, Field + TEXT("/a"))
				&& TryBuildObject(
					{
						{TEXT("a"), A.ToSharedRef()},
						{TEXT("b"), B.ToSharedRef()},
						{TEXT("g"), G.ToSharedRef()},
						{TEXT("r"), R.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

        bool TryReadLinearColor(const FUeAgentCanonicalValue& Value, FLinearColor& OutColor,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::Object || Value.GetObjectItems().Num() != 4)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_color_invalid"),
                    TEXT("A color must contain exactly r, g, b and a."), Field, &Value, TEXT("object with r, g, b and a")));
            const TCHAR* Names[] = {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")};
            const FUeAgentCanonicalValue* Values[] = {FindObjectField(Value, Names[0]), FindObjectField(Value, Names[1]),
                FindObjectField(Value, Names[2]), FindObjectField(Value, Names[3])};
            double Components[4] = {};
            for (int32 Index = 0; Index < 4; ++Index)
                if (!Values[Index])
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_color_invalid"),
                        TEXT("A required color component is missing."), Field + TEXT("/") + Names[Index], nullptr, TEXT("finite float32 number")));
            for (int32 Index = 0; Index < 4; ++Index)
                if (!TryReadNumber(*Values[Index], Components[Index], Field + TEXT("/") + Names[Index], OutError)) return false;
            for (int32 Index = 0; Index < 4; ++Index)
                if (Components[Index] < -MAX_flt || Components[Index] > MAX_flt)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_color_invalid"),
                        TEXT("The color component is outside the finite float32 range."), Field + TEXT("/") + Names[Index],
                        Values[Index], TEXT("finite float32 number")));
            OutColor = FLinearColor(static_cast<float>(Components[0]), static_cast<float>(Components[1]),
                static_cast<float>(Components[2]), static_cast<float>(Components[3]));
            return true;
        }

        bool TryReadExactBoolProperty(const UObject& Object, const FName PropertyName, bool& bOutValue,
            const FString& Field, FExecutionMutationError& OutError)
        {
            const FProperty* Raw = FindFProperty<FProperty>(Object.GetClass(), PropertyName);
            const FBoolProperty* Property = CastField<FBoolProperty>(Raw);
            if (!Property)
            {
                OutError = JsonFailure(TEXT("asset_data_paper_tilemap_private_binding_invalid"),
                    TEXT("The required native boolean property is missing or has an incompatible type."), Field,
                    MakeShared<FJsonValueString>(Raw ? Raw->GetCPPType(nullptr, 0) : TEXT("missing")),
                    MakeShared<FJsonValueString>(TEXT("boolean property")), TEXT("native_result"));
                OutError.TargetPath = Object.GetPathName();
                return false;
            }
            bOutValue = Property->GetPropertyValue_InContainer(&Object);
            return true;
        }

        bool TryReadExactFloatProperty(const UObject& Object, const FName PropertyName, float& OutValue,
            const FString& Field, FExecutionMutationError& OutError)
        {
            const FProperty* Raw = FindFProperty<FProperty>(Object.GetClass(), PropertyName);
            const FFloatProperty* Property = CastField<FFloatProperty>(Raw);
            if (!Property)
            {
                OutError = JsonFailure(TEXT("asset_data_paper_tilemap_private_binding_invalid"),
                    TEXT("The required native float property is missing or has an incompatible type."), Field,
                    MakeShared<FJsonValueString>(Raw ? Raw->GetCPPType(nullptr, 0) : TEXT("missing")),
                    MakeShared<FJsonValueString>(TEXT("float property")), TEXT("native_result"));
                OutError.TargetPath = Object.GetPathName();
                return false;
            }
            OutValue = Property->GetPropertyValue_InContainer(&Object);
            if (!FMath::IsFinite(OutValue))
            {
                OutError = JsonFailure(TEXT("asset_data_paper_tilemap_private_value_invalid"),
                    TEXT("The native property contains a non-finite value."), Field, NumberValue(OutValue),
                    MakeShared<FJsonValueString>(TEXT("finite float32 number")), TEXT("native_result"));
                OutError.TargetPath = Object.GetPathName();
                return false;
            }
            return true;
        }

		bool TryBuildPaperFlipbookFrames(
			const UPaperFlipbook& Flipbook,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Frames;
			Frames.Reserve(Flipbook.GetNumKeyFrames());
			int64 TotalFrames = 0;
			for (int32 Index = 0; Index < Flipbook.GetNumKeyFrames(); ++Index)
			{
				const FPaperFlipbookKeyFrame& Frame =
					Flipbook.GetKeyFrameChecked(Index);
				TotalFrames += Frame.FrameRun;
				if (Frame.FrameRun < 1 || TotalFrames > MAX_int32)
				{
                    OutError = JsonFailure(TEXT("asset_data_paper_flipbook_existing_frame_invalid"),
                        Frame.FrameRun < 1 ? TEXT("The existing frame duration must be at least one.")
                            : TEXT("The total flipbook frame duration exceeds int32 range."),
                        TEXT("frames/") + FString::FromInt(Index) + TEXT("/frame_run"),
                        MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), Frame.FrameRun < 1 ? static_cast<int64>(Frame.FrameRun) : TotalFrames)),
                        MakeShared<FJsonValueString>(Frame.FrameRun < 1 ? TEXT("integer >= 1") : TEXT("total frame duration <= 2147483647")), TEXT("native_result"));
					return false;
				}
				FUeAgentCanonicalValuePtr Sprite;
				FUeAgentCanonicalValuePtr FrameRun;
				FUeAgentCanonicalValuePtr FrameValue;
				if (!TryBuildReference(
						Frame.Sprite,
						Resources,
						Sprite,
						OutError)
					|| !TryBuildInteger(
						Frame.FrameRun,
						Resources,
						FrameRun,
						OutError)
					|| !TryBuildObject(
						{
							{TEXT("frame_run"), FrameRun.ToSharedRef()},
							{TEXT("sprite_ref"), Sprite.ToSharedRef()},
						},
						Resources,
						FrameValue,
						OutError))
				{
					return false;
				}
				Frames.Add(FrameValue.ToSharedRef());
			}
			return TryBuildArray(
				MoveTemp(Frames),
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildPaperFlipbookProjectionMap(
			const UPaperFlipbook* Flipbook,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
            OutValues.Reset();
            ON_SCOPE_EXIT { if (!OutError.IsEmpty()) { OutError.SourceKind = TEXT("native_result");
                if (OutError.TargetPath.IsEmpty() && Flipbook) OutError.TargetPath = Flipbook->GetPathName(); } };
            if (!Flipbook)
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_flipbook_projection_invalid"),
                    TEXT("The native flipbook is unavailable."), FString(), ObjectClassValue(Flipbook),
                    MakeShared<FJsonValueString>(TEXT("PaperFlipbook")), TEXT("native_result")));
            if (!FMath::IsFinite(Flipbook->GetFramesPerSecond()) || Flipbook->GetFramesPerSecond() < 0.0f)
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_flipbook_projection_invalid"),
                    TEXT("The native frames per second must be finite and nonnegative."), TEXT("frames_per_second"),
                    NumberValue(Flipbook->GetFramesPerSecond()), MakeShared<FJsonValueString>(TEXT("finite number >= 0")), TEXT("native_result")));
			FUeAgentCanonicalValuePtr Frames;
			FUeAgentCanonicalValuePtr FramesPerSecond;
			if (!TryBuildPaperFlipbookFrames(
					*Flipbook,
					Resources,
					Frames,
					OutError)
				|| !TryBuildDecimal(
					Flipbook->GetFramesPerSecond(),
					Resources,
					FramesPerSecond,
					OutError))
			{
				return false;
			}
			OutValues.Add(TEXT("frames"), Frames.ToSharedRef());
			OutValues.Add(
				TEXT("frames_per_second"),
				FramesPerSecond.ToSharedRef());
			return true;
		}

        bool TryReadExistingSpriteRegion(const UPaperSprite& Sprite, FIntPoint& OutOffset,
            FIntPoint& OutSize, FExecutionMutationError& OutError)
        {
            const FVector2D UV = Sprite.GetSourceUV();
            const FVector2D Size = Sprite.GetSourceSize();
            const TCHAR* Names[] = {TEXT("x"), TEXT("y"), TEXT("width"), TEXT("height")};
            const double Values[] = {UV.X, UV.Y, Size.X, Size.Y};
            for (int32 Check = 0; Check < 3; ++Check)
            {
                for (int32 Index = 0; Index < 4; ++Index)
                {
                    const double Value = Values[Index];
                    if ((Check == 0 && !FMath::IsFinite(Value))
                        || (Check == 1 && FMath::FloorToDouble(Value) != Value)
                        || (Check == 2 && (Value < MIN_int32 || Value > MAX_int32)))
                        return Fail(OutError, JsonFailure(TEXT("asset_data_paper_sprite_existing_region_invalid"),
                            TEXT("The native sprite region component must be an integer representable as int32."),
                            FString(TEXT("source_region/")) + Names[Index], NumberValue(Value),
                            MakeShared<FJsonValueString>(TEXT("integer in [-2147483648, 2147483647]")), TEXT("native_result")));
                }
            }
            OutOffset = FIntPoint(static_cast<int32>(UV.X), static_cast<int32>(UV.Y));
            OutSize = FIntPoint(static_cast<int32>(Size.X), static_cast<int32>(Size.Y));
            return true;
        }

		bool TryBuildPaperSpritePivotState(
			const ESpritePivotMode::Type Mode,
			const FVector2D& Pivot,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr ModeValue;
			FUeAgentCanonicalValuePtr X;
			FUeAgentCanonicalValuePtr Y;
			return TryBuildInteger(
					static_cast<int32>(Mode),
					Resources,
					ModeValue,
					OutError)
                && TryBuildDecimal(Pivot.X, Resources, X, OutError, TEXT("pivot/x"))
                && TryBuildDecimal(Pivot.Y, Resources, Y, OutError, TEXT("pivot/y"))
				&& TryBuildObject(
					{
						{TEXT("mode"), ModeValue.ToSharedRef()},
						{TEXT("x"), X.ToSharedRef()},
						{TEXT("y"), Y.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildPaperSpriteProjectionMap(
			const UPaperSprite* Sprite,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
            OutValues.Reset();
            ON_SCOPE_EXIT { if (!OutError.IsEmpty()) { OutError.SourceKind = TEXT("native_result");
                if (OutError.TargetPath.IsEmpty() && Sprite) OutError.TargetPath = Sprite->GetPathName(); } };
            if (!Sprite)
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_sprite_projection_invalid"),
                    TEXT("The native sprite is unavailable."), FString(), ObjectClassValue(Sprite),
                    MakeShared<FJsonValueString>(TEXT("PaperSprite")), TEXT("native_result")));
			FIntPoint Offset;
			FIntPoint Size;
			FVector2D CustomPivot;
			const ESpritePivotMode::Type PivotMode =
				Sprite->GetPivotMode(CustomPivot);
			FUeAgentCanonicalValuePtr Source;
			FUeAgentCanonicalValuePtr Region;
			FUeAgentCanonicalValuePtr Pivot;
            if (!TryReadExistingSpriteRegion(*Sprite, Offset, Size, OutError)
				|| !TryBuildReference(
					Sprite->GetSourceTexture(),
					Resources,
					Source,
					OutError)
				|| !TryBuildRect2d(
					Offset,
					Size,
					Resources,
					Region,
					OutError)
				|| !TryBuildPaperSpritePivotState(
					PivotMode,
					CustomPivot,
					Resources,
					Pivot,
					OutError))
			{
				return false;
			}
			OutValues.Add(TEXT("source_texture_ref"), Source.ToSharedRef());
			OutValues.Add(TEXT("source_region"), Region.ToSharedRef());
			OutValues.Add(TEXT("pivot"), Pivot.ToSharedRef());
			return true;
		}

		bool TryBuildPaperTileMapCell(
			const int32 X,
			const int32 Y,
			const FPaperTileInfo& Cell,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
            FExecutionMutationError& OutError,
            const FString& Field)
        {
            if (!Cell.IsValid() || !Cell.TileSet)
            {
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetNumberField(TEXT("x"), X);
                Actual->SetNumberField(TEXT("y"), Y);
                Actual->SetNumberField(TEXT("tile_index"), Cell.GetTileIndex());
                Actual->SetField(TEXT("tile_set_class"), ObjectClassValue(Cell.TileSet));
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tilemap_existing_cell_invalid"),
                    TEXT("The occupied native cell has no valid tile and tile set."), Field,
                    MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("valid tile with a live PaperTileSet")), TEXT("native_result")));
            }
			FUeAgentCanonicalValuePtr XValue;
			FUeAgentCanonicalValuePtr YValue;
			FUeAgentCanonicalValuePtr TileSet;
			FUeAgentCanonicalValuePtr TileIndex;
			FUeAgentCanonicalValuePtr FlipHorizontal;
			FUeAgentCanonicalValuePtr FlipVertical;
			FUeAgentCanonicalValuePtr FlipDiagonal;
			return TryBuildInteger(X, Resources, XValue, OutError)
				&& TryBuildInteger(Y, Resources, YValue, OutError)
				&& TryBuildString(
					Cell.TileSet->GetPathName(),
					Resources,
					TileSet,
					OutError)
				&& TryBuildInteger(
					Cell.GetTileIndex(),
					Resources,
					TileIndex,
					OutError)
				&& TryBuildBoolean(
					Cell.HasFlag(EPaperTileFlags::FlipHorizontal),
					Resources,
					FlipHorizontal,
					OutError)
				&& TryBuildBoolean(
					Cell.HasFlag(EPaperTileFlags::FlipVertical),
					Resources,
					FlipVertical,
					OutError)
				&& TryBuildBoolean(
					Cell.HasFlag(EPaperTileFlags::FlipDiagonal),
					Resources,
					FlipDiagonal,
					OutError)
				&& TryBuildObject(
					{
						{TEXT("flip_diagonal"), FlipDiagonal.ToSharedRef()},
						{TEXT("flip_horizontal"), FlipHorizontal.ToSharedRef()},
						{TEXT("flip_vertical"), FlipVertical.ToSharedRef()},
						{TEXT("tile_index"), TileIndex.ToSharedRef()},
						{TEXT("tile_set_ref"), TileSet.ToSharedRef()},
						{TEXT("x"), XValue.ToSharedRef()},
						{TEXT("y"), YValue.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildPaperTileMapLayer(
			const UPaperTileLayer& Layer,
			const FIntPoint& MapSize,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
            FExecutionMutationError& OutError,
            const FString& Field)
        {
            bool bHiddenInGame = false;
            bool bOverrideThickness = false;
            bool bOverrideOffset = false;
            float Thickness = 0.0f;
            float Offset = 0.0f;
            if (!TryReadExactBoolProperty(Layer, TEXT("bHiddenInGame"), bHiddenInGame, Field + TEXT("/visible_in_game"), OutError)
                || !TryReadExactBoolProperty(Layer, TEXT("bOverrideCollisionThickness"), bOverrideThickness, Field + TEXT("/collision_thickness_override"), OutError)
                || !TryReadExactBoolProperty(Layer, TEXT("bOverrideCollisionOffset"), bOverrideOffset, Field + TEXT("/collision_offset_override"), OutError)
                || !TryReadExactFloatProperty(Layer, TEXT("CollisionThicknessOverride"), Thickness, Field + TEXT("/collision_thickness_override"), OutError)
                || !TryReadExactFloatProperty(Layer, TEXT("CollisionOffsetOverride"), Offset, Field + TEXT("/collision_offset_override"), OutError))
                return false;

			TArray<FUeAgentCanonicalValueRef> Cells;
			for (int32 Y = 0; Y < MapSize.Y; ++Y)
			{
				for (int32 X = 0; X < MapSize.X; ++X)
				{
					const FPaperTileInfo Cell = Layer.GetCell(X, Y);
					if (!Cell.IsValid())
					{
						continue;
					}
					FUeAgentCanonicalValuePtr CellValue;
                    if (!TryBuildPaperTileMapCell(X, Y, Cell, Resources, CellValue, OutError,
                        Field + TEXT("/cells/") + FString::FromInt(Cells.Num())))
					{
						return false;
					}
					Cells.Add(CellValue.ToSharedRef());
				}
			}
            if (Cells.Num() != Layer.GetNumOccupiedCells())
            {
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetStringField(TEXT("layer"), Layer.LayerName.ToString());
                Actual->SetNumberField(TEXT("occupied_cells"), Layer.GetNumOccupiedCells());
                Actual->SetNumberField(TEXT("cells_within_map"), Cells.Num());
                Actual->SetNumberField(TEXT("map_width"), MapSize.X);
                Actual->SetNumberField(TEXT("map_height"), MapSize.Y);
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tilemap_existing_cell_out_of_bounds"),
                    TEXT("The native layer has occupied cells outside the map dimensions."), Field + TEXT("/cells"),
                    MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("all occupied cells within the map")), TEXT("native_result")));
            }

			FUeAgentCanonicalValuePtr Name;
			FUeAgentCanonicalValuePtr VisibleInEditor;
			FUeAgentCanonicalValuePtr VisibleInGame;
			FUeAgentCanonicalValuePtr Collides;
			FUeAgentCanonicalValuePtr ThicknessValue;
			FUeAgentCanonicalValuePtr OffsetValue;
			FUeAgentCanonicalValuePtr Color;
			FUeAgentCanonicalValuePtr CellArray;
			if (!TryBuildString(
					Layer.LayerName.ToString(),
					Resources,
					Name,
					OutError)
				|| !TryBuildBoolean(
					Layer.ShouldRenderInEditor(),
					Resources,
					VisibleInEditor,
					OutError)
				|| !TryBuildBoolean(
					!bHiddenInGame,
					Resources,
					VisibleInGame,
					OutError)
				|| !TryBuildBoolean(
					Layer.GetLayerCollides(),
					Resources,
					Collides,
					OutError)
				|| !(bOverrideThickness
                    ? TryBuildDecimal(Thickness, Resources, ThicknessValue, OutError, Field + TEXT("/collision_thickness_override"))
					: TryBuildNull(
						Resources,
						ThicknessValue,
						OutError))
				|| !(bOverrideOffset
                    ? TryBuildDecimal(Offset, Resources, OffsetValue, OutError, Field + TEXT("/collision_offset_override"))
					: TryBuildNull(
						Resources,
						OffsetValue,
						OutError))
				|| !TryBuildLinearColor(
					Layer.GetLayerColor(),
					Resources,
					Color,
					OutError, Field + TEXT("/color"))
				|| !TryBuildArray(
					MoveTemp(Cells),
					Resources,
					CellArray,
					OutError))
			{
				return false;
			}
			return TryBuildObject(
				{
					{TEXT("cells"), CellArray.ToSharedRef()},
					{TEXT("collides"), Collides.ToSharedRef()},
					{TEXT("collision_offset_override"), OffsetValue.ToSharedRef()},
					{TEXT("collision_thickness_override"), ThicknessValue.ToSharedRef()},
					{TEXT("color"), Color.ToSharedRef()},
					{TEXT("name"), Name.ToSharedRef()},
					{TEXT("visible_in_editor"), VisibleInEditor.ToSharedRef()},
					{TEXT("visible_in_game"), VisibleInGame.ToSharedRef()},
				},
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildPaperTileMapLayers(
			const UPaperTileMap& TileMap,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			const FIntPoint MapSize(TileMap.MapWidth, TileMap.MapHeight);
			TArray<FUeAgentCanonicalValueRef> Layers;
			Layers.Reserve(TileMap.TileLayers.Num());
			for (int32 Index = 0; Index < TileMap.TileLayers.Num(); ++Index)
			{
                const UPaperTileLayer* Layer = TileMap.TileLayers[Index];
                const FString Field = TEXT("layers/") + FString::FromInt(Index);
                FUeAgentCanonicalValuePtr LayerValue;
                if (!Layer)
                    return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tilemap_existing_layer_invalid"),
                        TEXT("The native tile map layer is unavailable."), Field, ObjectClassValue(Layer),
                        MakeShared<FJsonValueString>(TEXT("live PaperTileLayer")), TEXT("native_result")));
                if (!TryBuildPaperTileMapLayer(*Layer, MapSize, Resources, LayerValue, OutError, Field))
                    return false;
				Layers.Add(LayerValue.ToSharedRef());
			}
			return TryBuildArray(
				MoveTemp(Layers),
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildPaperTileMapProjectionMap(
			const UPaperTileMap* TileMap,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
            OutValues.Reset();
            ON_SCOPE_EXIT { if (!OutError.IsEmpty()) { OutError.SourceKind = TEXT("native_result");
                if (OutError.TargetPath.IsEmpty() && TileMap) OutError.TargetPath = TileMap->GetPathName(); } };
            if (!TileMap)
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tilemap_projection_invalid"),
                    TEXT("The native tile map is unavailable."), FString(), ObjectClassValue(TileMap),
                    MakeShared<FJsonValueString>(TEXT("PaperTileMap")), TEXT("native_result")));
            const TCHAR* Fields[] = {TEXT("map_size/x"), TEXT("map_size/y"), TEXT("tile_size/x"), TEXT("tile_size/y")};
            const int32 Dimensions[] = {TileMap->MapWidth, TileMap->MapHeight, TileMap->TileWidth, TileMap->TileHeight};
            for (int32 Index = 0; Index < 4; ++Index)
                if (Dimensions[Index] <= 0)
                    return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tilemap_projection_invalid"),
                        TEXT("The native tile map dimension must be positive."), Fields[Index], NumberValue(Dimensions[Index]),
                        MakeShared<FJsonValueString>(TEXT("integer >= 1")), TEXT("native_result")));
			FUeAgentCanonicalValuePtr MapSize;
			FUeAgentCanonicalValuePtr TileSize;
			FUeAgentCanonicalValuePtr Layers;
			if (!TryBuildExtent2i(
					FIntPoint(TileMap->MapWidth, TileMap->MapHeight),
					Resources,
					MapSize,
					OutError)
				|| !TryBuildExtent2i(
					FIntPoint(TileMap->TileWidth, TileMap->TileHeight),
					Resources,
					TileSize,
					OutError)
				|| !TryBuildPaperTileMapLayers(
					*TileMap,
					Resources,
					Layers,
					OutError))
			{
				return false;
			}
			OutValues.Add(TEXT("map_size"), MapSize.ToSharedRef());
			OutValues.Add(TEXT("tile_size"), TileSize.ToSharedRef());
			OutValues.Add(TEXT("layers"), Layers.ToSharedRef());
			return true;
		}

		bool TryBuildPaperTileSetMarginState(
			const FIntMargin& Margin,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Left;
			FUeAgentCanonicalValuePtr Top;
			FUeAgentCanonicalValuePtr Right;
			FUeAgentCanonicalValuePtr Bottom;
			return TryBuildInteger(Margin.Left, Resources, Left, OutError)
				&& TryBuildInteger(Margin.Top, Resources, Top, OutError)
				&& TryBuildInteger(Margin.Right, Resources, Right, OutError)
				&& TryBuildInteger(Margin.Bottom, Resources, Bottom, OutError)
				&& TryBuildObject(
					{
						{TEXT("bottom"), Bottom.ToSharedRef()},
						{TEXT("left"), Left.ToSharedRef()},
						{TEXT("right"), Right.ToSharedRef()},
						{TEXT("top"), Top.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildPaperTileSetTerrain(
			const UPaperTileSet& TileSet,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			if (TileSet.GetNumTerrains() > 254)
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tileset_existing_terrain_invalid"),
                    TEXT("The native tile set contains more than 254 terrains."), TEXT("terrain"),
                    NumberValue(TileSet.GetNumTerrains()), NumberValue(254), TEXT("native_result")));
			}
			TArray<FUeAgentCanonicalValueRef> Terrains;
			Terrains.Reserve(TileSet.GetNumTerrains());
			for (int32 Index = 0; Index < TileSet.GetNumTerrains(); ++Index)
			{
				const FPaperTileSetTerrain& Terrain = TileSet.GetTerrain(Index);
				FUeAgentCanonicalValuePtr Name;
				FUeAgentCanonicalValuePtr Center;
				FUeAgentCanonicalValuePtr TerrainValue;
                const FString Field = TEXT("terrain/") + FString::FromInt(Index);
                if (Terrain.TerrainName.IsEmpty())
                    return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tileset_existing_terrain_invalid"),
                        TEXT("The native terrain name is empty."), Field + TEXT("/name"), MakeShared<FJsonValueString>(Terrain.TerrainName),
                        MakeShared<FJsonValueString>(TEXT("nonempty string")), TEXT("native_result")));
                if (Terrain.CenterTileIndex < 0)
                    return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tileset_existing_terrain_invalid"),
                        TEXT("The native terrain center tile index must be nonnegative."), Field + TEXT("/center_tile_index"),
                        NumberValue(Terrain.CenterTileIndex), MakeShared<FJsonValueString>(TEXT("integer >= 0")), TEXT("native_result")));
                if (!TryBuildString(
						Terrain.TerrainName,
						Resources,
						Name,
						OutError)
					|| !TryBuildInteger(
						Terrain.CenterTileIndex,
						Resources,
						Center,
						OutError)
					|| !TryBuildObject(
						{
							{TEXT("center_tile_index"), Center.ToSharedRef()},
							{TEXT("name"), Name.ToSharedRef()},
						},
						Resources,
						TerrainValue,
						OutError))
				{
					return false;
				}
				Terrains.Add(TerrainValue.ToSharedRef());
			}
			return TryBuildArray(
				MoveTemp(Terrains),
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildPaperTileSetProjectionMap(
			const UPaperTileSet* TileSet,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
            OutValues.Reset();
            ON_SCOPE_EXIT { if (!OutError.IsEmpty()) { OutError.SourceKind = TEXT("native_result");
                if (OutError.TargetPath.IsEmpty() && TileSet) OutError.TargetPath = TileSet->GetPathName(); } };
            if (!TileSet)
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tileset_projection_invalid"),
                    TEXT("The native tile set is unavailable."), FString(), ObjectClassValue(TileSet),
                    MakeShared<FJsonValueString>(TEXT("PaperTileSet")), TEXT("native_result")));
			FUeAgentCanonicalValuePtr Source;
			FUeAgentCanonicalValuePtr TileSize;
			FUeAgentCanonicalValuePtr Margin;
			FUeAgentCanonicalValuePtr Spacing;
			FUeAgentCanonicalValuePtr Terrain;
			if (!TryBuildReference(
					TileSet->GetTileSheetTexture(),
					Resources,
					Source,
					OutError)
				|| !TryBuildExtent2i(
					TileSet->GetTileSize(),
					Resources,
					TileSize,
					OutError)
				|| !TryBuildPaperTileSetMarginState(
					TileSet->GetMargin(),
					Resources,
					Margin,
					OutError)
				|| !TryBuildExtent2i(
					TileSet->GetPerTileSpacing(),
					Resources,
					Spacing,
					OutError)
				|| !TryBuildPaperTileSetTerrain(
					*TileSet,
					Resources,
					Terrain,
					OutError))
			{
				return false;
			}
			OutValues.Add(TEXT("source_texture_ref"), Source.ToSharedRef());
			OutValues.Add(TEXT("tile_size"), TileSize.ToSharedRef());
			OutValues.Add(TEXT("margin"), Margin.ToSharedRef());
			OutValues.Add(TEXT("spacing"), Spacing.ToSharedRef());
			OutValues.Add(TEXT("terrain"), Terrain.ToSharedRef());
			return true;
		}

		const TCHAR* FindContextTag(
			const FString& NativeMechanicId,
			const FString& FieldId)
		{
			if (NativeMechanicId == PaperFlipbookMechanicId)
			{
				if (FieldId == TEXT("frames"))
				{
					return TEXT("paper_flipbook_frames");
				}
				if (FieldId == TEXT("frames_per_second"))
				{
					return TEXT("paper_flipbook_fps");
				}
			}
			else if (NativeMechanicId == PaperSpriteMechanicId)
			{
				if (FieldId == TEXT("source_texture_ref"))
				{
					return TEXT("paper_sprite_texture_ref");
				}
				if (FieldId == TEXT("source_region"))
				{
					return TEXT("paper_sprite_region");
				}
				if (FieldId == TEXT("pivot"))
				{
					return TEXT("paper_sprite_pivot_state");
				}
			}
			else if (NativeMechanicId == PaperTileMapMechanicId)
			{
				if (FieldId == TEXT("map_size"))
				{
					return TEXT("paper_tilemap_extent");
				}
				if (FieldId == TEXT("tile_size"))
				{
					return TEXT("paper_tilemap_tile_extent");
				}
				if (FieldId == TEXT("layers"))
				{
					return TEXT("paper_tilemap_layers");
				}
			}
			else if (NativeMechanicId == PaperTileSetMechanicId)
			{
				if (FieldId == TEXT("source_texture_ref"))
				{
					return TEXT("paper_tileset_texture_ref");
				}
				if (FieldId == TEXT("tile_size"))
				{
					return TEXT("paper_tileset_extent");
				}
				if (FieldId == TEXT("margin"))
				{
					return TEXT("paper_tileset_margin_state");
				}
				if (FieldId == TEXT("spacing"))
				{
					return TEXT("paper_tileset_spacing_state");
				}
				if (FieldId == TEXT("terrain"))
				{
					return TEXT("paper_tileset_terrain");
				}
			}
			return nullptr;
		}

		bool TryBuildContextValue(
			const FString& NativeMechanicId,
			const FString& FieldId,
			UObject* Target,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			const TCHAR* Tag = FindContextTag(NativeMechanicId, FieldId);
			if (!Tag)
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_projection_context_invalid"),
                    TEXT("No projection context is registered for this field."), FieldId, MakeShared<FJsonValueString>(FieldId),
                    MakeShared<FJsonValueString>(TEXT("registered Paper field")), TEXT("native_result")));
			}
			if (NativeMechanicId != PaperTileMapMechanicId
				|| FieldId != TEXT("layers"))
			{
				return TryBuildString(
					Tag,
					Resources,
					OutValue,
					OutError);
			}
			const UPaperTileMap* TileMap = Cast<UPaperTileMap>(Target);
            if (!TileMap)
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tilemap_projection_context_invalid"),
                    TEXT("The layer projection context requires a PaperTileMap."), FieldId, ObjectClassValue(Target),
                    MakeShared<FJsonValueString>(TEXT("PaperTileMap")), TEXT("native_result")));
            if (TileMap->MapWidth <= 0 || TileMap->MapHeight <= 0)
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tilemap_projection_context_invalid"),
                    TEXT("The native map dimension must be positive."), TileMap->MapWidth <= 0 ? TEXT("map_size/x") : TEXT("map_size/y"),
                    NumberValue(TileMap->MapWidth <= 0 ? TileMap->MapWidth : TileMap->MapHeight),
                    MakeShared<FJsonValueString>(TEXT("integer >= 1")), TEXT("native_result")));
			FUeAgentCanonicalValuePtr TagValue;
			FUeAgentCanonicalValuePtr MapSize;
			return TryBuildString(Tag, Resources, TagValue, OutError)
				&& TryBuildExtent2i(
					FIntPoint(TileMap->MapWidth, TileMap->MapHeight),
					Resources,
					MapSize,
					OutError)
				&& TryBuildObject(
					{
						{TEXT("map_size"), MapSize.ToSharedRef()},
						{TEXT("tag"), TagValue.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

        bool TryReadContextTag(const FUeAgentCanonicalValue& Context, FString& OutTag,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (Context.GetKind() == EUeAgentValueKind::String)
                return TryReadString(Context, OutTag, Field, OutError, TEXT("native_result"));
            const FUeAgentCanonicalValue* Tag = FindObjectField(Context, TEXT("tag"));
            if (!Tag)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_projection_context_invalid"),
                    TEXT("The field projection context has no tag."), Field, &Context,
                    TEXT("context with a nonempty tag"), TEXT("native_result")));
            return TryReadString(*Tag, OutTag, Field + TEXT("/tag"), OutError, TEXT("native_result"));
        }

		bool TrySelectFields(
			const FString& NativeMechanicId,
			UObject* Target,
			const TArray<FString>& SelectedFieldIds,
			const TMap<FString, FUeAgentCanonicalValueRef>& CurrentValues,
			const TMap<FString, FUeAgentCanonicalValueRef>& DefaultValues,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutCurrentFields,
			FUeAgentCanonicalValuePtr& OutDefaultFields,
			FUeAgentCanonicalValuePtr& OutProjectionContext,
			FExecutionMutationError& OutError)
		{
			if (SelectedFieldIds.IsEmpty())
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_selected_fields_missing"),
                    TEXT("At least one Paper field must be selected."), TEXT("selected_field_ids"),
                    NumberValue(SelectedFieldIds.Num()), MakeShared<FJsonValueString>(TEXT("nonempty field list"))));
			}
			TSet<FString> Seen;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> CurrentItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> DefaultItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> ContextItems;
            for (int32 Index = 0; Index < SelectedFieldIds.Num(); ++Index)
            {
                const FString& FieldId = SelectedFieldIds[Index];
				const FUeAgentCanonicalValueRef* Current =
					CurrentValues.Find(FieldId);
				const FUeAgentCanonicalValueRef* Default =
					DefaultValues.Find(FieldId);
				FUeAgentCanonicalValuePtr Context;
				if (Seen.Contains(FieldId)
					|| !Current
					|| !Default)
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_paper_selected_field_invalid"),
                        Seen.Contains(FieldId) ? TEXT("The field is selected more than once.")
                            : !Current ? TEXT("The selected field has no current projection.") : TEXT("The selected field has no default projection."),
                        TEXT("selected_field_ids/") + FString::FromInt(Index), MakeShared<FJsonValueString>(FieldId),
                        MakeShared<FJsonValueString>(TEXT("unique field with current and default projections"))));
				}
				if (!TryBuildContextValue(NativeMechanicId, FieldId, Target, Resources, Context, OutError)) return false;
				Seen.Add(FieldId);
				CurrentItems.Emplace(FieldId, *Current);
				DefaultItems.Emplace(FieldId, *Default);
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
            const TCHAR* StateNames[] = {TEXT("current_fields"), TEXT("default_fields"), TEXT("projection_context")};
            for (int32 Index = 0; Index < 3; ++Index)
                if (States[Index]->GetKind() != EUeAgentValueKind::Object)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_expected_state_invalid"),
                        TEXT("The captured Paper field state must be an object."), StateNames[Index], States[Index],
                        TEXT("object"), TEXT("native_result")));
            if (!TypedChanges || TypedChanges->GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_expected_state_invalid"),
                    TEXT("The proposed state requires a typed_changes array."), TEXT("typed_changes"), TypedChanges, TEXT("array")));
            if (TypedChanges->GetArrayItems().Num() != CurrentFields.GetObjectItems().Num())
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_expected_state_invalid"),
                    TEXT("The number of typed changes must match the number of selected fields."), TEXT("typed_changes"),
                    NumberValue(TypedChanges->GetArrayItems().Num()), NumberValue(CurrentFields.GetObjectItems().Num())));
            for (int32 Index = 0; Index < TypedChanges->GetArrayItems().Num(); ++Index)
            {
                const FUeAgentCanonicalValue& Change = TypedChanges->GetArrayItems()[Index].Get();
                const FUeAgentCanonicalValue* Field = FindObjectField(Change, TEXT("field_id"));
                const FString Path = TEXT("typed_changes/") + FString::FromInt(Index) + TEXT("/field_id");
                FString FieldId;
                if (!Field)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_expected_change_invalid"),
                        TEXT("The typed change has no field_id."), Path, nullptr, TEXT("nonempty string")));
                if (!TryReadString(*Field, FieldId, Path, OutError)) return false;
                if (OutChanges.Contains(FieldId))
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_expected_change_invalid"),
                        TEXT("The field has more than one typed change."), Path, Field, TEXT("unique field_id")));
                OutChanges.Add(FieldId, &Change);
            }
			return true;
		}

		bool TryReadPresenceAndValue(
			const FUeAgentCanonicalValue& Change,
			FString& OutPresence,
            const FUeAgentCanonicalValue*& OutRequested,
            const FString& Field, FExecutionMutationError& OutError)
        {
            OutPresence.Reset();
            OutRequested = nullptr;
            const FUeAgentCanonicalValue* Presence = FindObjectField(Change, TEXT("presence"));
            if (!Presence)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_presence_invalid"),
                    TEXT("The typed change has no presence value."), Field + TEXT("/presence"), nullptr, TEXT("nonempty string")));
            if (!TryReadString(*Presence, OutPresence, Field + TEXT("/presence"), OutError)) return false;
			const FUeAgentCanonicalValue* TypedValue =
				FindObjectField(Change, TEXT("value"));
			OutRequested = TypedValue
				? FindObjectField(*TypedValue, TEXT("canonical_value"))
				: nullptr;
			return true;
		}

		bool TryBuildExpectedTextureReference(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			FString Reference;
			UObject* Resolved = nullptr;
            if (!TryReadString(Requested, Reference, TEXT("source_texture_ref"), OutError)) return false;
            if (!Resolver.TryResolveExisting(Reference, Resolved, OutError))
            {
                if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = TEXT("source_texture_ref");
                return false;
            }
            UTexture2D* Texture = Cast<UTexture2D>(Resolved);
            if (!Texture)
            {
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetStringField(TEXT("reference"), Reference);
                Actual->SetField(TEXT("resolved_class"), ObjectClassValue(Resolved));
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_texture_reference_class_invalid"),
                    TEXT("The reference does not resolve to a 2D texture."), TEXT("source_texture_ref"),
                    MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("Texture2D"))));
            }
            return TryBuildString(Texture->GetPathName(), Resources, OutExpected, OutError);
		}

		bool TryBuildExpectedFlipbookFrames(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			if (Requested.GetKind() != EUeAgentValueKind::Array)
			{
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_flipbook_frames_invalid"),
                    TEXT("Flipbook frames must be an array."), TEXT("frames"), &Requested, TEXT("array")));
			}
			TArray<FUeAgentCanonicalValueRef> Frames;
			Frames.Reserve(Requested.GetArrayItems().Num());
			int64 TotalFrames = 0;
			for (int32 Index = 0;
				Index < Requested.GetArrayItems().Num();
				++Index)
			{
				const FUeAgentCanonicalValue& Frame =
					Requested.GetArrayItems()[Index].Get();
				const FUeAgentCanonicalValue* SpriteRef =
					FindObjectField(Frame, TEXT("sprite_ref"));
				const FUeAgentCanonicalValue* FrameRunValue =
					FindObjectField(Frame, TEXT("frame_run"));
				int32 FrameRun = 0;
                const FString Field = TEXT("frames/") + FString::FromInt(Index);
                if (Frame.GetKind() != EUeAgentValueKind::Object || Frame.GetObjectItems().Num() != 2)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_flipbook_frame_invalid"),
                        TEXT("A frame must contain exactly sprite_ref and frame_run."), Field, &Frame,
                        TEXT("object with sprite_ref and frame_run")));
                if (!SpriteRef || !FrameRunValue)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_flipbook_frame_invalid"),
                        TEXT("A required frame field is missing."), Field + (!SpriteRef ? TEXT("/sprite_ref") : TEXT("/frame_run")),
                        nullptr, !SpriteRef ? TEXT("PaperSprite reference or null") : TEXT("integer >= 1")));
                if (!TryReadInt32(*FrameRunValue, FrameRun, Field + TEXT("/frame_run"), OutError)) return false;
                if (FrameRun < 1)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_flipbook_frame_invalid"),
                        TEXT("The frame duration must be at least one."), Field + TEXT("/frame_run"), FrameRunValue, TEXT("integer >= 1")));
				TotalFrames += FrameRun;
				if (TotalFrames > MAX_int32)
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_paper_flipbook_total_frames_overflow"),
                        TEXT("The accumulated frame duration exceeds int32 range."), Field + TEXT("/frame_run"),
                        MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), TotalFrames)),
                        MakeShared<FJsonValueString>(TEXT("total frame duration <= 2147483647"))));
				}

				FUeAgentCanonicalValuePtr CanonicalSprite;
				if (SpriteRef->GetKind() == EUeAgentValueKind::Null)
				{
					if (!TryBuildNull(
							Resources,
							CanonicalSprite,
							OutError))
					{
						return false;
					}
				}
				else
				{
					FString Reference;
					UObject* Resolved = nullptr;
                    const FString RefField = Field + TEXT("/sprite_ref");
                    if (!TryReadString(*SpriteRef, Reference, RefField, OutError)) return false;
                    if (!Resolver.TryResolveExisting(Reference, Resolved, OutError))
                    {
                        if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = RefField;
                        return false;
                    }
                    if (!Cast<UPaperSprite>(Resolved))
                    {
                        auto Actual = MakeShared<FJsonObject>();
                        Actual->SetStringField(TEXT("reference"), Reference);
                        Actual->SetField(TEXT("resolved_class"), ObjectClassValue(Resolved));
                        return Fail(OutError, JsonFailure(TEXT("asset_data_paper_flipbook_sprite_ref_invalid"),
                            TEXT("The frame reference does not resolve to a Paper Sprite."), RefField,
                            MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("PaperSprite"))));
                    }
                    if (!TryBuildString(Resolved->GetPathName(), Resources, CanonicalSprite, OutError)) return false;
				}
				FUeAgentCanonicalValuePtr CanonicalFrameRun;
				FUeAgentCanonicalValuePtr CanonicalFrame;
				if (!TryBuildInteger(
						FrameRun,
						Resources,
						CanonicalFrameRun,
						OutError)
					|| !TryBuildObject(
						{
							{TEXT("frame_run"), CanonicalFrameRun.ToSharedRef()},
							{TEXT("sprite_ref"), CanonicalSprite.ToSharedRef()},
						},
						Resources,
						CanonicalFrame,
						OutError))
				{
					return false;
				}
				Frames.Add(CanonicalFrame.ToSharedRef());
			}
			return TryBuildArray(
				MoveTemp(Frames),
				Resources,
				OutExpected,
				OutError);
		}

		bool TryBuildExpectedSpritePivot(
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			if (Requested.GetKind() != EUeAgentValueKind::Object
				|| Requested.GetObjectItems().Num() != 2)
			{
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_sprite_pivot_invalid"),
                    TEXT("The sprite pivot must contain exactly x and y."), TEXT("pivot"), &Requested, TEXT("object with x and y")));
			}
			const FUeAgentCanonicalValue* X =
				FindObjectField(Requested, TEXT("x"));
			const FUeAgentCanonicalValue* Y =
				FindObjectField(Requested, TEXT("y"));
			int32 PixelX = 0;
			int32 PixelY = 0;
            if (!X || !Y)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_sprite_pivot_invalid"),
                    TEXT("A required pivot coordinate is missing."), !X ? TEXT("pivot/x") : TEXT("pivot/y"), nullptr, TEXT("int32 pixel coordinate")));
            if (!TryReadInt32(*X, PixelX, TEXT("pivot/x"), OutError)
                || !TryReadInt32(*Y, PixelY, TEXT("pivot/y"), OutError)) return false;
			return TryBuildPaperSpritePivotState(
				ESpritePivotMode::Custom,
				FVector2D(PixelX, PixelY),
				Resources,
				OutExpected,
				OutError);
		}

		bool TryBuildUniformMarginState(
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
            int32 Value = 0;
            if (!TryReadInt32(Requested, Value, TEXT("margin"), OutError)) return false;
            if (Value < 0)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_margin_invalid"),
                    TEXT("The tile set margin must be nonnegative."), TEXT("margin"), &Requested, TEXT("integer >= 0")));
            return TryBuildPaperTileSetMarginState(FIntMargin(Value), Resources, OutExpected, OutError);
		}

		bool TryBuildUniformSpacingState(
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
            int32 Value = 0;
            if (!TryReadInt32(Requested, Value, TEXT("spacing"), OutError)) return false;
            if (Value < 0)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_spacing_invalid"),
                    TEXT("The tile set spacing must be nonnegative."), TEXT("spacing"), &Requested, TEXT("integer >= 0")));
            return TryBuildExtent2i(FIntPoint(Value, Value), Resources, OutExpected, OutError);
		}

		bool TryBuildRequestedOptionalFloat(
			const FUeAgentCanonicalValue& Value,
			const bool bRequireNonNegative,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
            FExecutionMutationError& OutError,
            const FString& Field)
        {
            if (Value.GetKind() == EUeAgentValueKind::Null)
                return TryBuildNull(Resources, OutExpected, OutError);
            double Number = 0.0;
            if (!TryReadNumber(Value, Number, Field, OutError)) return false;
            if (Number < -static_cast<double>(MAX_flt) || Number > static_cast<double>(MAX_flt)
                || (bRequireNonNegative && Number < 0.0))
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_optional_float_invalid"),
                    Number < -static_cast<double>(MAX_flt) || Number > static_cast<double>(MAX_flt)
                        ? TEXT("The override is outside the finite float32 range.") : TEXT("The collision thickness override must be nonnegative."),
                    Field, &Value, bRequireNonNegative ? TEXT("finite float32 number >= 0, or null") : TEXT("finite float32 number, or null")));
            return TryBuildDecimal(Number, Resources, OutExpected, OutError, Field);
		}

		bool TryBuildExpectedPaperTileMapCell(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Requested,
			const FIntPoint& MapSize,
			TSet<uint64>& SeenCells,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
            FExecutionMutationError& OutError,
            const FString& Field)
        {
            if (Requested.GetKind() != EUeAgentValueKind::Object || Requested.GetObjectItems().Num() != 7)
            {
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_cell_shape_invalid"),
                    TEXT("A tile map cell must contain exactly its seven supported fields."), Field, &Requested,
                    TEXT("object with x, y, tile_set_ref, tile_index, flip_horizontal, flip_vertical and flip_diagonal")));
			}
			const FUeAgentCanonicalValue* XValue =
				FindObjectField(Requested, TEXT("x"));
			const FUeAgentCanonicalValue* YValue =
				FindObjectField(Requested, TEXT("y"));
			const FUeAgentCanonicalValue* TileSetRef =
				FindObjectField(Requested, TEXT("tile_set_ref"));
			const FUeAgentCanonicalValue* TileIndexValue =
				FindObjectField(Requested, TEXT("tile_index"));
			int32 X = 0;
			int32 Y = 0;
			int32 TileIndex = 0;
			bool bFlipHorizontal = false;
			bool bFlipVertical = false;
			bool bFlipDiagonal = false;
			FString Reference;
            const TCHAR* Names[] = {TEXT("x"), TEXT("y"), TEXT("tile_set_ref"), TEXT("tile_index")};
            const FUeAgentCanonicalValue* Values[] = {XValue, YValue, TileSetRef, TileIndexValue};
            for (int32 Index = 0; Index < 4; ++Index)
                if (!Values[Index])
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_cell_value_invalid"),
                        TEXT("A required cell field is missing."), Field + TEXT("/") + Names[Index], nullptr,
                        Index == 2 ? TEXT("PaperTileSet reference") : TEXT("int32 value")));
            if (!TryReadInt32(*XValue, X, Field + TEXT("/x"), OutError)
                || !TryReadInt32(*YValue, Y, Field + TEXT("/y"), OutError)) return false;
            if (X < 0 || Y < 0 || X >= MapSize.X || Y >= MapSize.Y)
            {
                const bool bX = X < 0 || (Y >= 0 && X >= MapSize.X);
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_cell_value_invalid"),
                    TEXT("The cell coordinate is outside the map dimensions."), Field + (bX ? TEXT("/x") : TEXT("/y")),
                    bX ? XValue : YValue, FString::Printf(TEXT("integer in [0, %d)"), bX ? MapSize.X : MapSize.Y)));
            }
            if (!TryReadString(*TileSetRef, Reference, Field + TEXT("/tile_set_ref"), OutError)
                || !TryReadInt32(*TileIndexValue, TileIndex, Field + TEXT("/tile_index"), OutError)) return false;
            if (TileIndex < 0 || TileIndex > static_cast<int32>(EPaperTileFlags::TileIndexMask))
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_cell_value_invalid"),
                    TEXT("The tile index is outside the range supported by Paper tile flags."), Field + TEXT("/tile_index"),
                    TileIndexValue, FString::Printf(TEXT("integer in [0, %d]"), static_cast<int32>(EPaperTileFlags::TileIndexMask))));
            if (!TryReadBooleanField(Requested, TEXT("flip_horizontal"), bFlipHorizontal, Field, OutError)
                || !TryReadBooleanField(Requested, TEXT("flip_vertical"), bFlipVertical, Field, OutError)
                || !TryReadBooleanField(Requested, TEXT("flip_diagonal"), bFlipDiagonal, Field, OutError)) return false;
			const uint64 CellKey =
				(static_cast<uint64>(static_cast<uint32>(Y)) << 32)
				| static_cast<uint32>(X);
			if (SeenCells.Contains(CellKey))
			{
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetNumberField(TEXT("x"), X);
                Actual->SetNumberField(TEXT("y"), Y);
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tilemap_cell_duplicate"),
                    TEXT("Another cell in this layer uses the same coordinates."), Field, MakeShared<FJsonValueObject>(Actual),
                    MakeShared<FJsonValueString>(TEXT("unique cell coordinates within the layer"))));
			}
			SeenCells.Add(CellKey);
			UObject* Resolved = nullptr;
            if (!Resolver.TryResolveExisting(Reference, Resolved, OutError))
            {
                if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Field + TEXT("/tile_set_ref");
                return false;
            }
            if (!Cast<UPaperTileSet>(Resolved))
            {
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetStringField(TEXT("reference"), Reference);
                Actual->SetField(TEXT("resolved_class"), ObjectClassValue(Resolved));
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tilemap_tileset_invalid"),
                    TEXT("The cell reference does not resolve to a Paper Tile Set."), Field + TEXT("/tile_set_ref"),
                    MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("PaperTileSet"))));
            }

			FUeAgentCanonicalValuePtr XCanonical;
			FUeAgentCanonicalValuePtr YCanonical;
			FUeAgentCanonicalValuePtr TileSetCanonical;
			FUeAgentCanonicalValuePtr TileIndexCanonical;
			FUeAgentCanonicalValuePtr FlipHorizontal;
			FUeAgentCanonicalValuePtr FlipVertical;
			FUeAgentCanonicalValuePtr FlipDiagonal;
			return TryBuildInteger(X, Resources, XCanonical, OutError)
				&& TryBuildInteger(Y, Resources, YCanonical, OutError)
				&& TryBuildString(
					Resolved->GetPathName(),
					Resources,
					TileSetCanonical,
					OutError)
				&& TryBuildInteger(
					TileIndex,
					Resources,
					TileIndexCanonical,
					OutError)
				&& TryBuildBoolean(
					bFlipHorizontal,
					Resources,
					FlipHorizontal,
					OutError)
				&& TryBuildBoolean(
					bFlipVertical,
					Resources,
					FlipVertical,
					OutError)
				&& TryBuildBoolean(
					bFlipDiagonal,
					Resources,
					FlipDiagonal,
					OutError)
				&& TryBuildObject(
					{
						{TEXT("flip_diagonal"), FlipDiagonal.ToSharedRef()},
						{TEXT("flip_horizontal"), FlipHorizontal.ToSharedRef()},
						{TEXT("flip_vertical"), FlipVertical.ToSharedRef()},
						{TEXT("tile_index"), TileIndexCanonical.ToSharedRef()},
						{TEXT("tile_set_ref"), TileSetCanonical.ToSharedRef()},
						{TEXT("x"), XCanonical.ToSharedRef()},
						{TEXT("y"), YCanonical.ToSharedRef()},
					},
					Resources,
					OutExpected,
					OutError);
		}

		bool TryBuildExpectedPaperTileMapLayers(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Requested,
			const FIntPoint& MapSize,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			if (Requested.GetKind() != EUeAgentValueKind::Array)
			{
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_layers_invalid"),
                    TEXT("Tile map layers must be an array."), TEXT("layers"), &Requested, TEXT("array")));
			}
			TSet<FString> SeenNames;
			TArray<FUeAgentCanonicalValueRef> Layers;
			Layers.Reserve(Requested.GetArrayItems().Num());
			for (int32 LayerIndex = 0;
				LayerIndex < Requested.GetArrayItems().Num();
				++LayerIndex)
			{
				const FUeAgentCanonicalValue& Layer =
					Requested.GetArrayItems()[LayerIndex].Get();
				const FUeAgentCanonicalValue* NameValue =
					FindObjectField(Layer, TEXT("name"));
				const FUeAgentCanonicalValue* ThicknessValue =
					FindObjectField(
						Layer,
						TEXT("collision_thickness_override"));
				const FUeAgentCanonicalValue* OffsetValue =
					FindObjectField(
						Layer,
						TEXT("collision_offset_override"));
				const FUeAgentCanonicalValue* ColorValue =
					FindObjectField(Layer, TEXT("color"));
				const FUeAgentCanonicalValue* CellsValue =
					FindObjectField(Layer, TEXT("cells"));
				FString Name;
				bool bVisibleInEditor = false;
				bool bVisibleInGame = false;
				bool bCollides = false;
				FLinearColor Color;
                const FString Field = TEXT("layers/") + FString::FromInt(LayerIndex);
                if (Layer.GetKind() != EUeAgentValueKind::Object || Layer.GetObjectItems().Num() != 8)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_layer_value_invalid"),
                        TEXT("A layer must contain exactly its eight supported fields."), Field, &Layer,
                        TEXT("object with name, collision_thickness_override, collision_offset_override, color, cells, visible_in_editor, visible_in_game and collides")));
                if (!NameValue)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_layer_value_invalid"),
                        TEXT("The layer name is missing."), Field + TEXT("/name"), nullptr, TEXT("nonblank string")));
                if (!TryReadString(*NameValue, Name, Field + TEXT("/name"), OutError)) return false;
                if (Name.TrimStartAndEnd().IsEmpty())
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_layer_value_invalid"),
                        TEXT("The layer name must not contain only whitespace."), Field + TEXT("/name"), NameValue, TEXT("nonblank string")));
                if (!ThicknessValue || !OffsetValue || !ColorValue)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_layer_value_invalid"),
                        TEXT("A required layer field is missing."), Field + (!ThicknessValue ? TEXT("/collision_thickness_override")
                            : !OffsetValue ? TEXT("/collision_offset_override") : TEXT("/color")),
                        nullptr, !ThicknessValue || !OffsetValue ? TEXT("finite float32 number or null") : TEXT("RGBA color object")));
                if (!TryReadLinearColor(*ColorValue, Color, Field + TEXT("/color"), OutError)) return false;
                if (!CellsValue || CellsValue->GetKind() != EUeAgentValueKind::Array)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_layer_value_invalid"),
                        TEXT("The layer requires a cells array."), Field + TEXT("/cells"), CellsValue, TEXT("array")));
                if (!TryReadBooleanField(Layer, TEXT("visible_in_editor"), bVisibleInEditor, Field, OutError)
                    || !TryReadBooleanField(Layer, TEXT("visible_in_game"), bVisibleInGame, Field, OutError)
                    || !TryReadBooleanField(Layer, TEXT("collides"), bCollides, Field, OutError)) return false;
				const FString CanonicalName =
					Name.TrimStartAndEnd().ToLower();
				if (SeenNames.Contains(CanonicalName))
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_layer_name_duplicate"),
                        TEXT("Another layer has the same name after trimming whitespace and ignoring case."),
                        Field + TEXT("/name"), NameValue, TEXT("unique layer name")));
				}
				SeenNames.Add(CanonicalName);

				TSet<uint64> SeenCells;
				TArray<FUeAgentCanonicalValueRef> Cells;
				Cells.Reserve(CellsValue->GetArrayItems().Num());
                for (int32 CellIndex = 0; CellIndex < CellsValue->GetArrayItems().Num(); ++CellIndex)
                {
                    const FUeAgentCanonicalValueRef& Cell = CellsValue->GetArrayItems()[CellIndex];
					FUeAgentCanonicalValuePtr CanonicalCell;
					if (!TryBuildExpectedPaperTileMapCell(
							Resolver,
							Cell.Get(),
							MapSize,
							SeenCells,
							Resources,
							CanonicalCell,
                            OutError, Field + TEXT("/cells/") + FString::FromInt(CellIndex)))
					{
						return false;
					}
					Cells.Add(CanonicalCell.ToSharedRef());
				}

				FUeAgentCanonicalValuePtr CanonicalNameValue;
				FUeAgentCanonicalValuePtr VisibleInEditor;
				FUeAgentCanonicalValuePtr VisibleInGame;
				FUeAgentCanonicalValuePtr Collides;
				FUeAgentCanonicalValuePtr Thickness;
				FUeAgentCanonicalValuePtr Offset;
				FUeAgentCanonicalValuePtr CanonicalColor;
				FUeAgentCanonicalValuePtr CanonicalCells;
				FUeAgentCanonicalValuePtr CanonicalLayer;
				if (!TryBuildString(
						Name,
						Resources,
						CanonicalNameValue,
						OutError)
					|| !TryBuildBoolean(
						bVisibleInEditor,
						Resources,
						VisibleInEditor,
						OutError)
					|| !TryBuildBoolean(
						bVisibleInGame,
						Resources,
						VisibleInGame,
						OutError)
					|| !TryBuildBoolean(
						bCollides,
						Resources,
						Collides,
						OutError)
					|| !TryBuildRequestedOptionalFloat(
						*ThicknessValue,
						true,
						Resources,
						Thickness,
						OutError, Field + TEXT("/collision_thickness_override"))
					|| !TryBuildRequestedOptionalFloat(
						*OffsetValue,
						false,
						Resources,
						Offset,
						OutError, Field + TEXT("/collision_offset_override"))
					|| !TryBuildLinearColor(
						Color,
						Resources,
						CanonicalColor,
						OutError, Field + TEXT("/color"))
					|| !TryBuildArray(
						MoveTemp(Cells),
						Resources,
						CanonicalCells,
						OutError)
					|| !TryBuildObject(
						{
							{TEXT("cells"), CanonicalCells.ToSharedRef()},
							{TEXT("collides"), Collides.ToSharedRef()},
							{TEXT("collision_offset_override"), Offset.ToSharedRef()},
							{TEXT("collision_thickness_override"), Thickness.ToSharedRef()},
							{TEXT("color"), CanonicalColor.ToSharedRef()},
							{TEXT("name"), CanonicalNameValue.ToSharedRef()},
							{TEXT("visible_in_editor"), VisibleInEditor.ToSharedRef()},
							{TEXT("visible_in_game"), VisibleInGame.ToSharedRef()},
						},
						Resources,
						CanonicalLayer,
						OutError))
				{
					return false;
				}
				Layers.Add(CanonicalLayer.ToSharedRef());
			}
			return TryBuildArray(
				MoveTemp(Layers),
				Resources,
				OutExpected,
				OutError);
		}

		bool TryBuildExpectedPaperTileSetTerrain(
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			if (Requested.GetKind() != EUeAgentValueKind::Array)
			{
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_terrain_invalid"),
                    TEXT("Tile set terrain must be an array."), TEXT("terrain"), &Requested, TEXT("array")));
			}
            if (Requested.GetArrayItems().Num() > 254)
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tileset_terrain_invalid"),
                    TEXT("A tile set supports at most 254 terrains."), TEXT("terrain"), NumberValue(Requested.GetArrayItems().Num()),
                    MakeShared<FJsonValueString>(TEXT("at most 254 entries"))));
			TSet<FString> SeenNames;
			TArray<FUeAgentCanonicalValueRef> Terrains;
			Terrains.Reserve(Requested.GetArrayItems().Num());
			for (int32 Index = 0;
				Index < Requested.GetArrayItems().Num();
				++Index)
			{
				const FUeAgentCanonicalValue& Terrain =
					Requested.GetArrayItems()[Index].Get();
				const FUeAgentCanonicalValue* NameValue =
					FindObjectField(Terrain, TEXT("name"));
				const FUeAgentCanonicalValue* CenterValue =
					FindObjectField(Terrain, TEXT("center_tile_index"));
				FString Name;
				int32 Center = 0;
                const FString Field = TEXT("terrain/") + FString::FromInt(Index);
                if (Terrain.GetKind() != EUeAgentValueKind::Object || Terrain.GetObjectItems().Num() != 2)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_terrain_value_invalid"),
                        TEXT("A terrain must contain exactly name and center_tile_index."), Field, &Terrain,
                        TEXT("object with name and center_tile_index")));
                if (!NameValue)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_terrain_value_invalid"),
                        TEXT("The terrain name is missing."), Field + TEXT("/name"), nullptr, TEXT("nonblank string")));
                if (!TryReadString(*NameValue, Name, Field + TEXT("/name"), OutError)) return false;
                if (Name.TrimStartAndEnd().IsEmpty())
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_terrain_value_invalid"),
                        TEXT("The terrain name must not contain only whitespace."), Field + TEXT("/name"), NameValue, TEXT("nonblank string")));
                if (!CenterValue)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_terrain_value_invalid"),
                        TEXT("The terrain center tile index is missing."), Field + TEXT("/center_tile_index"), nullptr, TEXT("integer >= 0")));
                if (!TryReadInt32(*CenterValue, Center, Field + TEXT("/center_tile_index"), OutError)) return false;
                if (Center < 0)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_terrain_value_invalid"),
                        TEXT("The terrain center tile index must be nonnegative."), Field + TEXT("/center_tile_index"), CenterValue, TEXT("integer >= 0")));
				const FString CanonicalName =
					Name.TrimStartAndEnd().ToLower();
				if (SeenNames.Contains(CanonicalName))
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_terrain_name_duplicate"),
                        TEXT("Another terrain has the same name after trimming whitespace and ignoring case."),
                        Field + TEXT("/name"), NameValue, TEXT("unique terrain name")));
				}
				SeenNames.Add(CanonicalName);
				FUeAgentCanonicalValuePtr CanonicalNameValue;
				FUeAgentCanonicalValuePtr CanonicalCenter;
				FUeAgentCanonicalValuePtr CanonicalTerrain;
				if (!TryBuildString(
						Name,
						Resources,
						CanonicalNameValue,
						OutError)
					|| !TryBuildInteger(
						Center,
						Resources,
						CanonicalCenter,
						OutError)
					|| !TryBuildObject(
						{
							{TEXT("center_tile_index"), CanonicalCenter.ToSharedRef()},
							{TEXT("name"), CanonicalNameValue.ToSharedRef()},
						},
						Resources,
						CanonicalTerrain,
						OutError))
				{
					return false;
				}
				Terrains.Add(CanonicalTerrain.ToSharedRef());
			}
			return TryBuildArray(
				MoveTemp(Terrains),
				Resources,
				OutExpected,
				OutError);
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

		bool TryResolveExpectedTileMapSize(
			const TMap<FString, const FUeAgentCanonicalValue*>& Changes,
			const FUeAgentCanonicalValue& ProjectionContext,
			FIntPoint& OutMapSize,
			FExecutionMutationError& OutError)
		{
			if (const FUeAgentCanonicalValue* const* MapSizeChange =
				Changes.Find(TEXT("map_size")))
			{
				FString Presence;
				const FUeAgentCanonicalValue* Requested = nullptr;
                if (!TryReadPresenceAndValue(**MapSizeChange, Presence, Requested, TEXT("map_size"), OutError)) return false;
                if (Presence != TEXT("replace"))
                    return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tilemap_map_size_invalid"),
                        TEXT("Map size requires the replace operation."), TEXT("map_size/presence"), MakeShared<FJsonValueString>(Presence),
                        MakeShared<FJsonValueString>(TEXT("replace"))));
                if (!Requested)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_map_size_invalid"),
                        TEXT("The replacement map size is missing."), TEXT("map_size"), nullptr, TEXT("extent with x and y in [1, 1024]")));
                return TryReadExtent2i(*Requested, 1, 1024, OutMapSize, TEXT("map_size"), OutError);
			}
			const FUeAgentCanonicalValue* LayerContext =
				FindObjectField(ProjectionContext, TEXT("layers"));
			const FUeAgentCanonicalValue* MapSize = LayerContext
				? FindObjectField(*LayerContext, TEXT("map_size"))
				: nullptr;
            if (!MapSize)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_projection_context_invalid"),
                    TEXT("The captured layer context has no map size."), TEXT("layers/map_size"), LayerContext,
                    TEXT("context containing map_size"), TEXT("native_result")));
            return TryReadExtent2i(*MapSize, 1, 1024, OutMapSize, TEXT("layers/map_size"), OutError, TEXT("native_result"));
		}
	}

	bool CanProjectPaperFieldState(
		const FString& NativeMechanicId)
	{
		return NativeMechanicId == PaperFlipbookMechanicId
			|| NativeMechanicId == PaperSpriteMechanicId
			|| NativeMechanicId == PaperTileMapMechanicId
			|| NativeMechanicId == PaperTileSetMechanicId;
	}

	bool TryProjectPaperCurrentFieldState(
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
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty() && Target)
            OutError.TargetPath = Target->GetPathName(); };
        if (!CanProjectPaperFieldState(NativeMechanicId))
        {
            OutError = JsonFailure(TEXT("asset_data_paper_field_projection_unsupported"),
                TEXT("No Paper field projection is registered for this operation."), FString(), ObjectClassValue(Target),
                MakeShared<FJsonValueString>(TEXT("PaperFlipbook, PaperSprite, PaperTileMap or PaperTileSet projection")), TEXT("native_result"));
            OutError.InternalContext = NativeMechanicId;
            return false;
        }
        const UClass* ExpectedClass = NativeMechanicId == PaperFlipbookMechanicId ? UPaperFlipbook::StaticClass()
            : NativeMechanicId == PaperSpriteMechanicId ? UPaperSprite::StaticClass()
            : NativeMechanicId == PaperTileMapMechanicId ? UPaperTileMap::StaticClass() : UPaperTileSet::StaticClass();
        if (!Target || !Target->IsA(ExpectedClass))
            return Fail(OutError, JsonFailure(TEXT("asset_data_paper_field_projection_unsupported"),
                TEXT("The projection target is unavailable or has an incompatible class."), FString(),
                ObjectClassValue(Target), MakeShared<FJsonValueString>(ExpectedClass->GetPathName()), TEXT("native_result")));

		TMap<FString, FUeAgentCanonicalValueRef> CurrentValues;
		TMap<FString, FUeAgentCanonicalValueRef> DefaultValues;
		if (NativeMechanicId == PaperFlipbookMechanicId)
		{
			if (!TryBuildPaperFlipbookProjectionMap(
					Cast<UPaperFlipbook>(Target),
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildPaperFlipbookProjectionMap(
					GetDefault<UPaperFlipbook>(),
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
		}
		else if (NativeMechanicId == PaperSpriteMechanicId)
		{
			if (!TryBuildPaperSpriteProjectionMap(
					Cast<UPaperSprite>(Target),
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildPaperSpriteProjectionMap(
					GetDefault<UPaperSprite>(),
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
		}
		else if (NativeMechanicId == PaperTileMapMechanicId)
		{
			if (!TryBuildPaperTileMapProjectionMap(
					Cast<UPaperTileMap>(Target),
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildPaperTileMapProjectionMap(
					GetDefault<UPaperTileMap>(),
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
		}
		else if (!TryBuildPaperTileSetProjectionMap(
				Cast<UPaperTileSet>(Target),
				Resources,
				CurrentValues,
				OutError)
			|| !TryBuildPaperTileSetProjectionMap(
				GetDefault<UPaperTileSet>(),
				Resources,
				DefaultValues,
				OutError))
		{
			return false;
		}

		return TrySelectFields(
			NativeMechanicId,
			Target,
			SelectedFieldIds,
			CurrentValues,
			DefaultValues,
			Resources,
			OutCurrentFields,
			OutDefaultFields,
			OutProjectionContext,
			OutError);
	}

	bool TryProjectPaperExpectedFieldState(
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
		if (!CanProjectPaperFieldState(NativeMechanicId))
		{
            OutError = JsonFailure(TEXT("asset_data_paper_field_projection_unsupported"),
                TEXT("No Paper expected-state projection is registered for this operation."), FString(), nullptr,
                MakeShared<FJsonValueString>(TEXT("PaperFlipbook, PaperSprite, PaperTileMap or PaperTileSet projection")), TEXT("native_result"));
            OutError.InternalContext = NativeMechanicId;
            return false;
		}
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

		FIntPoint EffectiveTileMapSize = FIntPoint::ZeroValue;
		if (NativeMechanicId == PaperTileMapMechanicId
			&& Changes.Contains(TEXT("layers"))
			&& !TryResolveExpectedTileMapSize(
				Changes,
				ProjectionContext,
				EffectiveTileMapSize,
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
			const FUeAgentCanonicalValueRef* Default =
				FindObjectFieldRef(DefaultFields, Current.Key);
			const FUeAgentCanonicalValue* Context =
				FindObjectField(ProjectionContext, Current.Key);
			const TCHAR* ExpectedContext =
				FindContextTag(NativeMechanicId, Current.Key);
			FString ContextTag;
            if (!Change)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_expected_context_invalid"),
                    TEXT("The selected field has no typed change."), Current.Key, nullptr, TEXT("typed change for the selected field")));
            if (!Default || !Context || !ExpectedContext)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_expected_context_invalid"),
                    !Default ? TEXT("The selected field has no captured default.")
                        : !Context ? TEXT("The selected field has no captured projection context.") : TEXT("The selected field has no registered projection context."),
                    Current.Key, Context, TEXT("default and registered projection context for the selected field"), TEXT("native_result")));
            if (!TryReadContextTag(*Context, ContextTag, Current.Key, OutError)) return false;
            if (ContextTag != ExpectedContext)
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_expected_context_invalid"),
                    TEXT("The captured projection context tag does not match the selected field."), Current.Key,
                    MakeShared<FJsonValueString>(ContextTag), MakeShared<FJsonValueString>(ExpectedContext), TEXT("native_result")));

			FString Presence;
			const FUeAgentCanonicalValue* Requested = nullptr;
            if (!TryReadPresenceAndValue(**Change, Presence, Requested, Current.Key, OutError)) return false;

			if (NativeMechanicId == PaperSpriteMechanicId
				&& Presence == TEXT("clear"))
			{
				if (Requested
					|| (Current.Key != TEXT("source_texture_ref")
						&& Current.Key != TEXT("source_region")
						&& Current.Key != TEXT("pivot")))
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_sprite_clear_invalid"),
                        Requested ? TEXT("The clear operation must not include a value.") : TEXT("This sprite field does not support clear."),
                        Current.Key, Requested, Requested ? TEXT("no value for clear") : TEXT("source_texture_ref, source_region or pivot")));
				}
				ExpectedItems.Emplace(Current.Key, *Default);
				continue;
			}

			FUeAgentCanonicalValuePtr Expected;
			if (NativeMechanicId == PaperFlipbookMechanicId)
			{
				if (Presence != TEXT("replace") || !Requested)
				{
                    return Fail(OutError, Presence != TEXT("replace")
                        ? JsonFailure(TEXT("asset_data_paper_flipbook_expected_invalid"), TEXT("This field requires the replace operation."),
                            Current.Key + TEXT("/presence"), MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("replace")))
                        : ValueFailure(TEXT("asset_data_paper_flipbook_expected_invalid"), TEXT("The replacement value is missing."),
                            Current.Key, nullptr, TEXT("replacement value")));
				}
				if (Current.Key == TEXT("frames"))
				{
					if (!TryBuildExpectedFlipbookFrames(
							Resolver,
							*Requested,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (Current.Key == TEXT("frames_per_second"))
				{
					double FramesPerSecond = 0.0;
                    if (!TryReadNumber(*Requested, FramesPerSecond, Current.Key, OutError)) return false;
                    if (FramesPerSecond < 0.0)
                        return Fail(OutError, ValueFailure(TEXT("asset_data_paper_flipbook_fps_invalid"),
                            TEXT("Frames per second must be nonnegative."), Current.Key, Requested, TEXT("finite float32 number >= 0")));
                    const float NativeFramesPerSecond = static_cast<float>(FramesPerSecond);
                    if (!FMath::IsFinite(NativeFramesPerSecond))
                        return Fail(OutError, ValueFailure(TEXT("asset_data_paper_flipbook_fps_invalid"),
                            TEXT("Frames per second is outside the finite float32 range."), Current.Key, Requested, TEXT("finite float32 number >= 0")));
                    if (!TryBuildDecimal(NativeFramesPerSecond, Resources, Expected, OutError, Current.Key)) return false;
				}
				else
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_paper_flipbook_expected_field_invalid"),
                        TEXT("This field is not supported by the asset type."), Current.Key, MakeShared<FJsonValueString>(Current.Key),
                        MakeShared<FJsonValueString>(TEXT("frames, frames_per_second")), TEXT("native_result")));
				}
			}
			else if (NativeMechanicId == PaperSpriteMechanicId)
			{
				if (Current.Key == TEXT("source_texture_ref"))
				{
					if (Presence == TEXT("null") && !Requested)
					{
						if (!TryBuildNull(Resources, Expected, OutError))
						{
							return false;
						}
					}
					else if (Presence == TEXT("set") && Requested)
					{
						if (!TryBuildExpectedTextureReference(
								Resolver,
								*Requested,
								Resources,
								Expected,
								OutError))
						{
							return false;
						}
					}
					else
					{
                        auto Actual = MakeShared<FJsonObject>();
                        Actual->SetStringField(TEXT("presence"), Presence);
                        Actual->SetBoolField(TEXT("value_present"), Requested != nullptr);
                        return Fail(OutError, JsonFailure(TEXT("asset_data_paper_sprite_source_expected_invalid"),
                            TEXT("The sprite source requires null without a value, or set with a texture reference."), Current.Key,
                            MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("null without value, or set with value"))));
					}
				}
				else if (Current.Key == TEXT("source_region"))
				{
					FIntPoint Offset;
					FIntPoint Size;
                    if (Presence != TEXT("set") || !Requested)
                        return Fail(OutError, Presence != TEXT("set")
                            ? JsonFailure(TEXT("asset_data_paper_sprite_region_expected_invalid"), TEXT("The sprite region requires the set operation."),
                                Current.Key + TEXT("/presence"), MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("set")))
                            : ValueFailure(TEXT("asset_data_paper_sprite_region_expected_invalid"), TEXT("The sprite region value is missing."),
                                Current.Key, nullptr, TEXT("object with x, y, width and height")));
                    if (!TryReadRect2d(*Requested, Offset, Size, Current.Key, OutError)
                        || !TryBuildRect2d(Offset, Size, Resources, Expected, OutError)) return false;
				}
				else if (Current.Key == TEXT("pivot"))
				{
                    if (Presence != TEXT("set") || !Requested)
                        return Fail(OutError, Presence != TEXT("set")
                            ? JsonFailure(TEXT("asset_data_paper_sprite_pivot_expected_invalid"), TEXT("The sprite pivot requires the set operation."),
                                Current.Key + TEXT("/presence"), MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("set")))
                            : ValueFailure(TEXT("asset_data_paper_sprite_pivot_expected_invalid"), TEXT("The sprite pivot value is missing."),
                                Current.Key, nullptr, TEXT("object with x and y")));
                    if (!TryBuildExpectedSpritePivot(*Requested, Resources, Expected, OutError)) return false;
				}
				else
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_paper_sprite_expected_field_invalid"),
                        TEXT("This field is not supported by the asset type."), Current.Key, MakeShared<FJsonValueString>(Current.Key),
                        MakeShared<FJsonValueString>(TEXT("source_texture_ref, source_region, pivot")), TEXT("native_result")));
				}
			}
			else if (NativeMechanicId == PaperTileMapMechanicId)
			{
				if (Presence != TEXT("replace") || !Requested)
				{
                    return Fail(OutError, Presence != TEXT("replace")
                        ? JsonFailure(TEXT("asset_data_paper_tilemap_expected_invalid"), TEXT("This field requires the replace operation."),
                            Current.Key + TEXT("/presence"), MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("replace")))
                        : ValueFailure(TEXT("asset_data_paper_tilemap_expected_invalid"), TEXT("The replacement value is missing."),
                            Current.Key, nullptr, TEXT("replacement value")));
				}
				if (Current.Key == TEXT("map_size")
					|| Current.Key == TEXT("tile_size"))
				{
					FIntPoint Extent;
					const int32 Maximum = Current.Key == TEXT("map_size")
						? 1024
						: MAX_int32;
                    if (!TryReadExtent2i(*Requested, 1, Maximum, Extent, Current.Key, OutError)
                        || !TryBuildExtent2i(Extent, Resources, Expected, OutError)) return false;
				}
				else if (Current.Key == TEXT("layers"))
				{
					if (!TryBuildExpectedPaperTileMapLayers(
							Resolver,
							*Requested,
							EffectiveTileMapSize,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tilemap_expected_field_invalid"),
                        TEXT("This field is not supported by the asset type."), Current.Key, MakeShared<FJsonValueString>(Current.Key),
                        MakeShared<FJsonValueString>(TEXT("map_size, tile_size, layers")), TEXT("native_result")));
				}
			}
			else
			{
				if (Presence != TEXT("replace") || !Requested)
				{
                    return Fail(OutError, Presence != TEXT("replace")
                        ? JsonFailure(TEXT("asset_data_paper_tileset_expected_invalid"), TEXT("This field requires the replace operation."),
                            Current.Key + TEXT("/presence"), MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("replace")))
                        : ValueFailure(TEXT("asset_data_paper_tileset_expected_invalid"), TEXT("The replacement value is missing."),
                            Current.Key, nullptr, TEXT("replacement value")));
				}
				if (Current.Key == TEXT("source_texture_ref"))
				{
					if (!TryBuildExpectedTextureReference(
							Resolver,
							*Requested,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (Current.Key == TEXT("tile_size"))
				{
					FIntPoint Extent;
                    if (!TryReadExtent2i(*Requested, 1, MAX_int32, Extent, Current.Key, OutError)
                        || !TryBuildExtent2i(Extent, Resources, Expected, OutError)) return false;
				}
				else if (Current.Key == TEXT("margin"))
				{
					if (!TryBuildUniformMarginState(
							*Requested,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (Current.Key == TEXT("spacing"))
				{
					if (!TryBuildUniformSpacingState(
							*Requested,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (Current.Key == TEXT("terrain"))
				{
					if (!TryBuildExpectedPaperTileSetTerrain(
							*Requested,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tileset_expected_field_invalid"),
                        TEXT("This field is not supported by the asset type."), Current.Key, MakeShared<FJsonValueString>(Current.Key),
                        MakeShared<FJsonValueString>(TEXT("source_texture_ref, tile_size, margin, spacing, terrain")), TEXT("native_result")));
				}
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
