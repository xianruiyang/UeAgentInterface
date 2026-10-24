#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTextureFieldMechanics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Misc/ScopeExit.h"

#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCollection.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Particles/SubUVAnimation.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		const TCHAR* RenderTargetMechanicId =
			TEXT("uai.asset_data.mechanic:update_render_target:v1");
		const TCHAR* SubUVMechanicId =
			TEXT("uai.asset_data.mechanic:update_subuv_animation:v1");
		const TCHAR* TextureCollectionMechanicId =
			TEXT("uai.asset_data.mechanic:update_texture_collection:v1");

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
						? TEXT("asset_data_texture_projection_string_invalid")
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
						? TEXT("asset_data_texture_projection_boolean_invalid")
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
						? TEXT("asset_data_texture_projection_integer_invalid")
						: StateError.Code, StateError, TEXT("execution")));
		}

        bool TryBuildDecimal(const float Value, FUeAgentResourceTracker& Resources,
            FUeAgentCanonicalValuePtr& OutValue, FExecutionMutationError& OutError,
            const FString& Field = FString())
        {
            if (!FMath::IsFinite(Value))
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_projection_decimal_invalid"),
                    TEXT("The native value cannot be represented as a finite decimal."), Field,
                    NumberValue(Value), MakeShared<FJsonValueString>(TEXT("finite number")), TEXT("native_result")));
            FUeAgentStateSemanticError StateError;
            if (!FUeAgentCanonicalValue::TryDecimal(FString::SanitizeFloat(Value), Resources, OutValue, StateError))
            {
                OutError = Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
                    ? TEXT("asset_data_texture_projection_decimal_invalid") : StateError.Code, StateError, TEXT("execution"));
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
						? TEXT("asset_data_texture_projection_null_invalid")
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
						? TEXT("asset_data_texture_projection_array_invalid")
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
						? TEXT("asset_data_texture_projection_object_invalid")
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
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_string_invalid"),
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
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_number_invalid"),
                    TEXT("A numeric value is required."), Field, &Value, TEXT("finite number"), SourceKind));
            if (!LexTryParseString(OutNumber, *Value.GetLexeme()) || !FMath::IsFinite(OutNumber))
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_number_invalid"),
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
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_int32_invalid"),
                    TEXT("The value must be an integer representable as int32."), Field, &Value,
                    TEXT("integer in [-2147483648, 2147483647]"), SourceKind));
            OutValue = static_cast<int32>(Number);
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

        bool TryReadExtent2i(const FUeAgentCanonicalValue& Value, FIntPoint& OutExtent,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::Object || Value.GetObjectItems().Num() != 2)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_extent_invalid"),
                    TEXT("The extent must contain exactly x and y."), Field, &Value, TEXT("object with x and y")));
            const FUeAgentCanonicalValue* X = FindObjectField(Value, TEXT("x"));
            const FUeAgentCanonicalValue* Y = FindObjectField(Value, TEXT("y"));
            if (!X || !Y)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_extent_invalid"),
                    TEXT("A required extent component is missing."), Field + (!X ? TEXT("/x") : TEXT("/y")), nullptr, TEXT("positive int32 component")));
            if (!TryReadInt32(*X, OutExtent.X, Field + TEXT("/x"), OutError)
                || !TryReadInt32(*Y, OutExtent.Y, Field + TEXT("/y"), OutError)) return false;
            if (OutExtent.X <= 0 || OutExtent.Y <= 0)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_extent_invalid"),
                    TEXT("The extent component must be positive."), Field + (OutExtent.X <= 0 ? TEXT("/x") : TEXT("/y")),
                    OutExtent.X <= 0 ? X : Y, TEXT("integer >= 1")));
            return true;
        }

        bool TryBuildLinearColor(const FLinearColor& Color, FUeAgentResourceTracker& Resources,
            FUeAgentCanonicalValuePtr& OutValue, FExecutionMutationError& OutError, const FString& Field)
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
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_color_invalid"),
                    TEXT("A color must contain exactly r, g, b and a."), Field, &Value, TEXT("object with r, g, b and a")));
            const TCHAR* Names[] = {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")};
            const FUeAgentCanonicalValue* Values[] = {FindObjectField(Value, Names[0]), FindObjectField(Value, Names[1]),
                FindObjectField(Value, Names[2]), FindObjectField(Value, Names[3])};
            double Components[4] = {};
            for (int32 Index = 0; Index < 4; ++Index)
                if (!Values[Index])
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_color_invalid"),
                        TEXT("A required color component is missing."), Field + TEXT("/") + Names[Index], nullptr, TEXT("finite float32 component")));
            for (int32 Index = 0; Index < 4; ++Index)
                if (!TryReadNumber(*Values[Index], Components[Index], Field + TEXT("/") + Names[Index], OutError)) return false;
            OutColor = FLinearColor(static_cast<float>(Components[0]), static_cast<float>(Components[1]),
                static_cast<float>(Components[2]), static_cast<float>(Components[3]));
            const float Converted[] = {OutColor.R, OutColor.G, OutColor.B, OutColor.A};
            for (int32 Index = 0; Index < 4; ++Index)
                if (!FMath::IsFinite(Converted[Index]))
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_color_invalid"),
                        TEXT("The color component cannot be represented as a finite float32 value."),
                        Field + TEXT("/") + Names[Index], Values[Index], TEXT("finite float32 component")));
            return true;
        }

        bool TryGetRenderTargetFormatName(
            const ETextureRenderTargetFormat Format, FString& OutName, FExecutionMutationError& OutError)
        {
			switch (Format)
			{
			case RTF_R8: OutName = TEXT("r8"); return true;
			case RTF_RG8: OutName = TEXT("rg8"); return true;
			case RTF_RGBA8: OutName = TEXT("rgba8"); return true;
			case RTF_RGBA8_SRGB: OutName = TEXT("rgba8_srgb"); return true;
			case RTF_R16f: OutName = TEXT("r16f"); return true;
			case RTF_RG16f: OutName = TEXT("rg16f"); return true;
			case RTF_RGBA16f: OutName = TEXT("rgba16f"); return true;
			case RTF_R32f: OutName = TEXT("r32f"); return true;
			case RTF_RG32f: OutName = TEXT("rg32f"); return true;
			case RTF_RGBA32f: OutName = TEXT("rgba32f"); return true;
			case RTF_RGB10A2: OutName = TEXT("rgb10a2"); return true;
            default:
                OutName.Reset();
                return Fail(OutError, JsonFailure(TEXT("asset_data_render_target_format_invalid"),
                    TEXT("The native render target format is not supported."), TEXT("format"),
                    MakeShared<FJsonValueNumber>(static_cast<int32>(Format)),
                    MakeShared<FJsonValueString>(TEXT("RTF_R8, RTF_RG8, RTF_RGBA8, RTF_RGBA8_SRGB, RTF_R16f, RTF_RG16f, RTF_RGBA16f, RTF_R32f, RTF_RG32f, RTF_RGBA32f or RTF_RGB10A2")),
                    TEXT("native_result")));
			}
		}

        bool TryNormalizeRenderTargetFormat(const FUeAgentCanonicalValue& Value,
            FString& OutName, FExecutionMutationError& OutError)
        {
            if (!TryReadString(Value, OutName, TEXT("format"), OutError)) return false;
            OutName.TrimStartAndEndInline();
            OutName.ToLowerInline();
            if (OutName == TEXT("r8") || OutName == TEXT("rg8") || OutName == TEXT("rgba8")
                || OutName == TEXT("rgba8_srgb") || OutName == TEXT("r16f") || OutName == TEXT("rg16f")
                || OutName == TEXT("rgba16f") || OutName == TEXT("r32f") || OutName == TEXT("rg32f")
                || OutName == TEXT("rgba32f") || OutName == TEXT("rgb10a2")) return true;
            return Fail(OutError, ValueFailure(TEXT("asset_data_render_target_format_expected_invalid"),
                TEXT("The render target format name is not supported."), TEXT("format"), &Value,
                TEXT("r8, rg8, rgba8, rgba8_srgb, r16f, rg16f, rgba16f, r32f, rg32f, rgba32f or rgb10a2")));
        }

        bool TryGetSubUVBoundingName(
            const ESubUVBoundingVertexCount Mode, FString& OutName, FExecutionMutationError& OutError)
        {
			if (Mode == BVC_FourVertices)
			{
				OutName = TEXT("four_vertices");
				return true;
			}
			if (Mode == BVC_EightVertices)
			{
				OutName = TEXT("eight_vertices");
				return true;
			}
            OutName.Reset();
            return Fail(OutError, JsonFailure(TEXT("asset_data_subuv_bounding_invalid"),
                TEXT("The native SubUV bounding geometry mode is not supported."), TEXT("bounding_geometry"),
                MakeShared<FJsonValueNumber>(static_cast<int32>(Mode)),
                MakeShared<FJsonValueString>(TEXT("BVC_FourVertices or BVC_EightVertices")), TEXT("native_result")));
		}

        bool TryNormalizeSubUVBounding(const FUeAgentCanonicalValue& Value, FString& OutName, FExecutionMutationError& OutError)
        {
            if (!TryReadString(Value, OutName, TEXT("bounding_geometry"), OutError)) return false;
            OutName.TrimStartAndEndInline();
            OutName.ToLowerInline();
            if (OutName == TEXT("four_vertices") || OutName == TEXT("eight_vertices")) return true;
            return Fail(OutError, ValueFailure(TEXT("asset_data_subuv_bounding_expected_invalid"),
                TEXT("The SubUV bounding geometry name is not supported."), TEXT("bounding_geometry"), &Value,
                TEXT("four_vertices or eight_vertices")));
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

        bool TryReadStringArray(const FUeAgentCanonicalValue& Value, TArray<FString>& OutStrings,
            const FString& Field, FExecutionMutationError& OutError)
        {
            OutStrings.Reset();
            if (Value.GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_collection_current_invalid"),
                    TEXT("The captured texture reference list must be an array."), Field, &Value, TEXT("array"), TEXT("native_result")));
            TSet<FString> Seen;
            for (int32 Index = 0; Index < Value.GetArrayItems().Num(); ++Index)
            {
                const FUeAgentCanonicalValue& Item = Value.GetArrayItems()[Index].Get();
                const FString Path = Field + TEXT("/") + FString::FromInt(Index);
                FString Text;
                if (!TryReadString(Item, Text, Path, OutError, TEXT("native_result")))
                {
                    OutStrings.Reset();
                    return false;
                }
                if (Seen.Contains(Text))
                {
                    OutStrings.Reset();
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_collection_current_invalid"),
                        TEXT("The captured texture reference list contains a duplicate."), Path, &Item, TEXT("unique texture reference"), TEXT("native_result")));
                }
                Seen.Add(Text);
                OutStrings.Add(MoveTemp(Text));
            }
            return true;
        }

		bool TryBuildRenderTargetProjectionMap(
			const UTextureRenderTarget2D* Target,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
            OutValues.Reset();
            ON_SCOPE_EXIT { if (!OutError.IsEmpty()) { OutError.SourceKind = TEXT("native_result");
                if (OutError.TargetPath.IsEmpty() && Target) OutError.TargetPath = Target->GetPathName(); } };
            FString FormatName;
			FUeAgentCanonicalValuePtr Size;
			FUeAgentCanonicalValuePtr Format;
			FUeAgentCanonicalValuePtr ClearColor;
			FUeAgentCanonicalValuePtr AutoGenerateMips;
            if (!Target)
                return Fail(OutError, JsonFailure(TEXT("asset_data_render_target_projection_invalid"),
                    TEXT("The native render target is unavailable."), TEXT("target"), ObjectClassValue(Target),
                    MakeShared<FJsonValueString>(UTextureRenderTarget2D::StaticClass()->GetPathName()), TEXT("native_result")));
            if (Target->SizeX < 1 || Target->SizeY < 1)
                return Fail(OutError, JsonFailure(TEXT("asset_data_render_target_projection_invalid"),
                    TEXT("The native render target size must be positive."), Target->SizeX < 1 ? TEXT("size/x") : TEXT("size/y"),
                    MakeShared<FJsonValueNumber>(Target->SizeX < 1 ? Target->SizeX : Target->SizeY),
                    MakeShared<FJsonValueString>(TEXT("integer >= 1")), TEXT("native_result")));
            if (!TryGetRenderTargetFormatName(Target->RenderTargetFormat, FormatName, OutError)
                || !TryBuildExtent2i(FIntPoint(Target->SizeX, Target->SizeY), Resources, Size, OutError)
                || !TryBuildString(FormatName, Resources, Format, OutError)
                || !TryBuildLinearColor(Target->ClearColor, Resources, ClearColor, OutError, TEXT("clear_color"))
                || !TryBuildBoolean(Target->bAutoGenerateMips, Resources, AutoGenerateMips, OutError)) return false;
			OutValues.Add(TEXT("size"), Size.ToSharedRef());
			OutValues.Add(TEXT("format"), Format.ToSharedRef());
			OutValues.Add(TEXT("clear_color"), ClearColor.ToSharedRef());
			OutValues.Add(
				TEXT("auto_generate_mips"),
				AutoGenerateMips.ToSharedRef());
			return true;
		}

		bool TryBuildSubUVProjectionMap(
			const USubUVAnimation* SubUV,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
            OutValues.Reset();
            ON_SCOPE_EXIT { if (!OutError.IsEmpty()) { OutError.SourceKind = TEXT("native_result");
                if (OutError.TargetPath.IsEmpty() && SubUV) OutError.TargetPath = SubUV->GetPathName(); } };
            FString BoundingName;
			FUeAgentCanonicalValuePtr Source;
			FUeAgentCanonicalValuePtr Grid;
			FUeAgentCanonicalValuePtr Bounding;
            if (!SubUV)
                return Fail(OutError, JsonFailure(TEXT("asset_data_subuv_projection_invalid"),
                    TEXT("The native SubUV animation is unavailable."), TEXT("target"), ObjectClassValue(SubUV),
                    MakeShared<FJsonValueString>(USubUVAnimation::StaticClass()->GetPathName()), TEXT("native_result")));
            if (SubUV->SubImages_Horizontal < 1 || SubUV->SubImages_Vertical < 1)
                return Fail(OutError, JsonFailure(TEXT("asset_data_subuv_projection_invalid"),
                    TEXT("The native SubUV grid dimension must be positive."),
                    SubUV->SubImages_Horizontal < 1 ? TEXT("grid/x") : TEXT("grid/y"),
                    MakeShared<FJsonValueNumber>(SubUV->SubImages_Horizontal < 1 ? SubUV->SubImages_Horizontal : SubUV->SubImages_Vertical),
                    MakeShared<FJsonValueString>(TEXT("integer >= 1")), TEXT("native_result")));
            if (!TryGetSubUVBoundingName(SubUV->BoundingMode, BoundingName, OutError)
                || !TryBuildReference(SubUV->SubUVTexture.Get(), Resources, Source, OutError)
                || !TryBuildExtent2i(FIntPoint(SubUV->SubImages_Horizontal, SubUV->SubImages_Vertical), Resources, Grid, OutError)
                || !TryBuildString(BoundingName, Resources, Bounding, OutError)) return false;
			OutValues.Add(TEXT("source_texture_ref"), Source.ToSharedRef());
			OutValues.Add(TEXT("grid"), Grid.ToSharedRef());
			OutValues.Add(TEXT("bounding_geometry"), Bounding.ToSharedRef());
			return true;
		}

		bool TryBuildTextureCollectionProjectionMap(
			const UTextureCollection* Collection,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
            OutValues.Reset();
            ON_SCOPE_EXIT { if (!OutError.IsEmpty()) { OutError.SourceKind = TEXT("native_result");
                if (OutError.TargetPath.IsEmpty() && Collection) OutError.TargetPath = Collection->GetPathName(); } };
            if (!Collection)
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_collection_projection_invalid"),
                    TEXT("The native texture collection is unavailable."), TEXT("target"), ObjectClassValue(Collection),
                    MakeShared<FJsonValueString>(UTextureCollection::StaticClass()->GetPathName()), TEXT("native_result")));
			TArray<FString> Paths;
			TSet<const UTexture*> Seen;
			Paths.Reserve(Collection->Textures.Num());
            for (int32 Index = 0; Index < Collection->Textures.Num(); ++Index)
            {
                const UTexture* Texture = Collection->Textures[Index];
                if (!Texture || Seen.Contains(Texture))
                    return Fail(OutError, JsonFailure(TEXT("asset_data_texture_collection_projection_member_invalid"),
                        !Texture ? TEXT("The native texture collection contains an unavailable texture.") : TEXT("The native texture collection contains a duplicate texture."),
                        TEXT("texture_refs/") + FString::FromInt(Index),
                        Texture ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Texture->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                        MakeShared<FJsonValueString>(TEXT("unique live texture")), TEXT("native_result")));
				Seen.Add(Texture);
				Paths.Add(Texture->GetPathName());
			}
			FUeAgentCanonicalValuePtr Refs;
			if (!TryBuildStringArray(Paths, Resources, Refs, OutError))
			{
				return false;
			}
			OutValues.Add(TEXT("texture_refs"), Refs.ToSharedRef());
			return true;
		}

		const TCHAR* FindContextTag(
			const FString& NativeMechanicId,
			const FString& FieldId)
		{
			if (NativeMechanicId == RenderTargetMechanicId)
			{
				if (FieldId == TEXT("size"))
				{
					return TEXT("render_target_extent");
				}
				if (FieldId == TEXT("format"))
				{
					return TEXT("render_target_format");
				}
				if (FieldId == TEXT("clear_color"))
				{
					return TEXT("texture_linear_color");
				}
				if (FieldId == TEXT("auto_generate_mips"))
				{
					return TEXT("texture_boolean");
				}
			}
			if (NativeMechanicId == SubUVMechanicId)
			{
				if (FieldId == TEXT("source_texture_ref"))
				{
					return TEXT("texture_2d_asset_ref");
				}
				if (FieldId == TEXT("grid"))
				{
					return TEXT("subuv_grid_extent");
				}
				if (FieldId == TEXT("bounding_geometry"))
				{
					return TEXT("subuv_bounding_geometry");
				}
			}
			if (NativeMechanicId == TextureCollectionMechanicId
				&& FieldId == TEXT("texture_refs"))
			{
				return TEXT("ordered_texture_asset_refs");
			}
			return nullptr;
		}

		bool TrySelectFields(
			const FString& NativeMechanicId,
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
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_selected_fields_missing"),
                    TEXT("At least one texture field must be selected."), TEXT("selected_field_ids"), MakeShared<FJsonValueNumber>(0),
                    MakeShared<FJsonValueString>(TEXT("nonempty field list"))));
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
				const TCHAR* ContextTag =
					FindContextTag(NativeMechanicId, FieldId);
				FUeAgentCanonicalValuePtr Context;
				if (Seen.Contains(FieldId)
					|| !Current
					|| !Default
					|| !ContextTag)
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_texture_selected_field_invalid"),
                        Seen.Contains(FieldId) ? TEXT("The field is selected more than once.") : !Current
                            ? TEXT("The selected field has no current projection.") : !Default ? TEXT("The selected field has no default projection.")
                            : TEXT("The selected field has no registered projection context."),
                        TEXT("selected_field_ids/") + FString::FromInt(Index), MakeShared<FJsonValueString>(FieldId),
                        MakeShared<FJsonValueString>(TEXT("unique registered field with current and default projections"))));
				}
				if (!TryBuildString(ContextTag, Resources, Context, OutError)) return false;
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
            const TCHAR* Names[] = {TEXT("current_fields"), TEXT("default_fields"), TEXT("projection_context")};
            for (int32 Index = 0; Index < 3; ++Index)
                if (States[Index]->GetKind() != EUeAgentValueKind::Object)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_expected_state_invalid"),
                        TEXT("The captured texture field state must be an object."), Names[Index], States[Index], TEXT("object"), TEXT("native_result")));
            for (int32 Index = 1; Index < 3; ++Index)
                if (CurrentFields.GetObjectItems().Num() != States[Index]->GetObjectItems().Num())
                    return Fail(OutError, JsonFailure(TEXT("asset_data_texture_expected_state_invalid"),
                        TEXT("The captured field state count does not match the current field count."), Names[Index],
                        MakeShared<FJsonValueNumber>(States[Index]->GetObjectItems().Num()),
                        MakeShared<FJsonValueNumber>(CurrentFields.GetObjectItems().Num()), TEXT("native_result")));
            if (!TypedChanges || TypedChanges->GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_expected_state_invalid"),
                    TEXT("The proposed state requires a typed_changes array."), TEXT("typed_changes"), TypedChanges, TEXT("array")));
            if (TypedChanges->GetArrayItems().Num() != CurrentFields.GetObjectItems().Num())
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_expected_state_invalid"),
                    TEXT("The typed change count does not match the selected field count."), TEXT("typed_changes"),
                    MakeShared<FJsonValueNumber>(TypedChanges->GetArrayItems().Num()), MakeShared<FJsonValueNumber>(CurrentFields.GetObjectItems().Num())));
            for (int32 Index = 0; Index < TypedChanges->GetArrayItems().Num(); ++Index)
            {
                const FUeAgentCanonicalValue& Change = TypedChanges->GetArrayItems()[Index].Get();
                const FUeAgentCanonicalValue* Field = FindObjectField(Change, TEXT("field_id"));
                const FString Path = TEXT("typed_changes/") + FString::FromInt(Index) + TEXT("/field_id");
                FString FieldId;
                if (!Field)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_expected_change_invalid"),
                        TEXT("The typed change has no field_id."), Path, nullptr, TEXT("selected field_id")));
                if (!TryReadString(*Field, FieldId, Path, OutError)) return false;
                if (OutChanges.Contains(FieldId) || !FindObjectField(CurrentFields, FieldId))
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_expected_change_invalid"),
                        OutChanges.Contains(FieldId) ? TEXT("The field has more than one typed change.") : TEXT("The typed change refers to an unselected field."),
                        Path, Field, TEXT("unique selected field_id")));
                OutChanges.Add(FieldId, &Change);
            }
			return true;
		}

        bool TryReadPresenceAndValue(
            const FUeAgentCanonicalValue& Change, FString& OutPresence,
            const FUeAgentCanonicalValue*& OutRequested, const FString& Field, FExecutionMutationError& OutError)
        {
            OutPresence.Reset();
            OutRequested = nullptr;
            const FUeAgentCanonicalValue* Presence = FindObjectField(Change, TEXT("presence"));
            if (!Presence)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_expected_presence_invalid"),
                    TEXT("The field change has no presence value."), Field + TEXT("/presence"), nullptr, TEXT("nonempty operation name")));
            if (!TryReadString(*Presence, OutPresence, Field + TEXT("/presence"), OutError)) return false;
			const FUeAgentCanonicalValue* TypedValue =
				FindObjectField(Change, TEXT("value"));
			OutRequested = TypedValue
				? FindObjectField(*TypedValue, TEXT("canonical_value"))
				: nullptr;
			return true;
		}

		bool TryResolveTexturePaths(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Requested,
			TArray<FString>& OutPaths,
			FExecutionMutationError& OutError)
		{
			OutPaths.Reset();
			if (Requested.GetKind() != EUeAgentValueKind::Array)
			{
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_refs_expected_invalid"),
                    TEXT("Texture references must be an array."), TEXT("texture_refs"), &Requested, TEXT("array")));
			}
			TSet<const UTexture*> Seen;
            for (int32 Index = 0; Index < Requested.GetArrayItems().Num(); ++Index)
            {
                const FUeAgentCanonicalValue& Item = Requested.GetArrayItems()[Index].Get();
                const FString Field = TEXT("texture_refs/") + FString::FromInt(Index);
                FString CanonicalRef;
                UObject* Resolved = nullptr;
                if (!TryReadString(Item, CanonicalRef, Field, OutError)) return false;
                if (!Resolver.TryResolveExisting(CanonicalRef, Resolved, OutError))
                {
                    if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Field;
                    return false;
                }
				const UTexture* Texture = Cast<UTexture>(Resolved);
				if (!Texture || Seen.Contains(Texture))
				{
                    auto Actual = MakeShared<FJsonObject>();
                    Actual->SetStringField(TEXT("reference"), CanonicalRef);
                    Actual->SetField(TEXT("resolved_class"), ObjectClassValue(Resolved));
                    return Fail(OutError, JsonFailure(TEXT("asset_data_texture_ref_expected_invalid"),
                        !Texture ? TEXT("The reference does not resolve to a texture.") : TEXT("The texture is already present in this list."),
                        Field, MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("unique texture asset reference"))));
				}
				Seen.Add(Texture);
				OutPaths.Add(Texture->GetPathName());
			}
			return true;
		}
	}

	bool CanProjectTextureFieldState(
		const FString& NativeMechanicId)
	{
		return NativeMechanicId == RenderTargetMechanicId
			|| NativeMechanicId == SubUVMechanicId
			|| NativeMechanicId == TextureCollectionMechanicId;
	}

	bool TryProjectTextureCurrentFieldState(
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
        if (!CanProjectTextureFieldState(NativeMechanicId))
        {
            OutError = JsonFailure(TEXT("asset_data_texture_field_projection_unsupported"),
                TEXT("No texture field projection is registered for this operation."), FString(), ObjectClassValue(Target),
                MakeShared<FJsonValueString>(TEXT("RenderTarget2D, SubUVAnimation or TextureCollection projection")), TEXT("native_result"));
            OutError.InternalContext = NativeMechanicId;
            return false;
        }
        const UClass* ExpectedClass = NativeMechanicId == RenderTargetMechanicId ? UTextureRenderTarget2D::StaticClass()
            : NativeMechanicId == SubUVMechanicId ? USubUVAnimation::StaticClass() : UTextureCollection::StaticClass();
        if (!Target || !Target->IsA(ExpectedClass))
            return Fail(OutError, JsonFailure(TEXT("asset_data_texture_field_projection_unsupported"),
                TEXT("The projection target is unavailable or has an incompatible class."), TEXT("target"),
                ObjectClassValue(Target), MakeShared<FJsonValueString>(ExpectedClass->GetPathName()), TEXT("native_result")));

		TMap<FString, FUeAgentCanonicalValueRef> CurrentValues;
		TMap<FString, FUeAgentCanonicalValueRef> DefaultValues;
		if (NativeMechanicId == RenderTargetMechanicId)
		{
			if (!TryBuildRenderTargetProjectionMap(
					Cast<UTextureRenderTarget2D>(Target),
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildRenderTargetProjectionMap(
					GetDefault<UTextureRenderTarget2D>(),
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
		}
		else if (NativeMechanicId == SubUVMechanicId)
		{
			if (!TryBuildSubUVProjectionMap(
					Cast<USubUVAnimation>(Target),
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildSubUVProjectionMap(
					GetDefault<USubUVAnimation>(),
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
		}
		else if (!TryBuildTextureCollectionProjectionMap(
				Cast<UTextureCollection>(Target),
				Resources,
				CurrentValues,
				OutError)
			|| !TryBuildTextureCollectionProjectionMap(
				GetDefault<UTextureCollection>(),
				Resources,
				DefaultValues,
				OutError))
		{
			return false;
		}

		return TrySelectFields(
			NativeMechanicId,
			SelectedFieldIds,
			CurrentValues,
			DefaultValues,
			Resources,
			OutCurrentFields,
			OutDefaultFields,
			OutProjectionContext,
			OutError);
	}

	bool TryProjectTextureExpectedFieldState(
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
		if (!CanProjectTextureFieldState(NativeMechanicId))
		{
            OutError = JsonFailure(TEXT("asset_data_texture_field_projection_unsupported"),
                TEXT("No expected texture field projection is registered for this operation."), FString(), nullptr,
                MakeShared<FJsonValueString>(TEXT("RenderTarget2D, SubUVAnimation or TextureCollection projection")), TEXT("native_result"));
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
			FString ContextText;
            if (!Change)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_expected_context_invalid"),
                    TEXT("The selected field has no typed change."), Current.Key, nullptr, TEXT("typed change for the selected field")));
            if (!Default || !Context || !ExpectedContext)
                return Fail(OutError, ValueFailure(TEXT("asset_data_texture_expected_context_invalid"),
                    !Default ? TEXT("The selected field has no captured default.") : !Context
                        ? TEXT("The selected field has no captured projection context.") : TEXT("The selected field has no registered projection context."),
                    Current.Key, Context, TEXT("default and registered projection context for the selected field"), TEXT("native_result")));
            if (!TryReadString(*Context, ContextText, Current.Key, OutError, TEXT("native_result"))) return false;
            if (ContextText != ExpectedContext)
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_expected_context_invalid"),
                    TEXT("The captured context tag does not match the selected texture field."), Current.Key,
                    MakeShared<FJsonValueString>(ContextText), MakeShared<FJsonValueString>(ExpectedContext), TEXT("native_result")));

			FString Presence;
			const FUeAgentCanonicalValue* Requested = nullptr;
            if (!TryReadPresenceAndValue(**Change, Presence, Requested, Current.Key, OutError)) return false;
			if (Presence == TEXT("clear"))
			{
				if (Requested)
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_clear_value_invalid"),
                        TEXT("A clear operation must omit its value."), Current.Key, Requested, TEXT("no value for clear")));
				}
				if (NativeMechanicId == TextureCollectionMechanicId)
				{
					FUeAgentCanonicalValuePtr Empty;
					if (!TryBuildStringArray({}, Resources, Empty, OutError))
					{
						return false;
					}
					ExpectedItems.Emplace(
						Current.Key,
						Empty.ToSharedRef());
				}
				else
				{
					ExpectedItems.Emplace(Current.Key, *Default);
				}
				continue;
			}

			FUeAgentCanonicalValuePtr Expected;
			if (NativeMechanicId == RenderTargetMechanicId)
			{
				if (Presence != TEXT("set") || !Requested)
				{
                    return Fail(OutError, Presence != TEXT("set")
                        ? JsonFailure(TEXT("asset_data_render_target_expected_invalid"), TEXT("This render target field requires the set operation."),
                            Current.Key + TEXT("/presence"), MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("set")))
                        : ValueFailure(TEXT("asset_data_render_target_expected_invalid"), TEXT("The requested field value is missing."), Current.Key, nullptr, TEXT("field value")));
				}
				if (Current.Key == TEXT("size"))
				{
					FIntPoint Extent;
                    if (!TryReadExtent2i(*Requested, Extent, Current.Key, OutError)) return false;
                    if (Extent.X > GTextureRenderTarget2DMaxSizeX || Extent.Y > GTextureRenderTarget2DMaxSizeY)
                    {
                        const bool bX = Extent.X > GTextureRenderTarget2DMaxSizeX;
                        return Fail(OutError, JsonFailure(TEXT("asset_data_render_target_size_expected_invalid"),
                            TEXT("The requested render target dimension exceeds the supported maximum."), Current.Key + (bX ? TEXT("/x") : TEXT("/y")),
                            MakeShared<FJsonValueNumber>(bX ? Extent.X : Extent.Y),
                            MakeShared<FJsonValueNumber>(bX ? GTextureRenderTarget2DMaxSizeX : GTextureRenderTarget2DMaxSizeY)));
                    }
					if (!TryBuildExtent2i(Extent, Resources, Expected, OutError)) return false;
				}
				else if (Current.Key == TEXT("format"))
				{
					FString Name;
                    if (!TryNormalizeRenderTargetFormat(*Requested, Name, OutError)) return false;
					if (!TryBuildString(Name, Resources, Expected, OutError)) return false;
				}
				else if (Current.Key == TEXT("clear_color"))
				{
					FLinearColor Color;
                    if (!TryReadLinearColor(*Requested, Color, Current.Key, OutError)) return false;
                    if (!TryBuildLinearColor(Color, Resources, Expected, OutError, Current.Key)) return false;
				}
                else if (Current.Key == TEXT("auto_generate_mips"))
                {
                    if (Requested->GetKind() != EUeAgentValueKind::Boolean)
                        return Fail(OutError, ValueFailure(TEXT("asset_data_render_target_expected_field_invalid"),
                            TEXT("Automatic mip generation requires a boolean value."), Current.Key, Requested, TEXT("boolean")));
					if (!TryBuildBoolean(
							Requested->GetBoolean(),
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_render_target_expected_field_invalid"),
                        TEXT("This field is not supported by the render target."), Current.Key, MakeShared<FJsonValueString>(Current.Key),
                        MakeShared<FJsonValueString>(TEXT("size, format, clear_color or auto_generate_mips")), TEXT("native_result")));
				}
			}
			else if (NativeMechanicId == SubUVMechanicId)
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
					else
					{
						FString CanonicalRef;
						UObject* Resolved = nullptr;
                        if (Presence != TEXT("set") || !Requested)
                        {
                            auto Actual = MakeShared<FJsonObject>();
                            Actual->SetStringField(TEXT("presence"), Presence);
                            Actual->SetBoolField(TEXT("value_present"), Requested != nullptr);
                            return Fail(OutError, JsonFailure(TEXT("asset_data_subuv_texture_expected_invalid"),
                                TEXT("The SubUV source requires set with a texture reference, or null without a value."),
                                Current.Key, MakeShared<FJsonValueObject>(Actual),
                                MakeShared<FJsonValueString>(TEXT("set with Texture2D reference, or null without value"))));
                        }
                        if (!TryReadString(*Requested, CanonicalRef, Current.Key, OutError)) return false;
                        if (!Resolver.TryResolveExisting(CanonicalRef, Resolved, OutError))
                        {
                            if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Current.Key;
                            return false;
                        }
                        if (!Cast<UTexture2D>(Resolved))
                        {
                            auto Actual = MakeShared<FJsonObject>();
                            Actual->SetStringField(TEXT("reference"), CanonicalRef);
                            Actual->SetField(TEXT("resolved_class"), ObjectClassValue(Resolved));
                            return Fail(OutError, JsonFailure(TEXT("asset_data_subuv_texture_expected_invalid"),
                                TEXT("The referenced asset is not a Texture2D."), Current.Key, MakeShared<FJsonValueObject>(Actual),
                                MakeShared<FJsonValueString>(UTexture2D::StaticClass()->GetPathName())));
                        }
						if (!TryBuildReference(Resolved, Resources, Expected, OutError)) return false;
					}
				}
				else if (Current.Key == TEXT("grid"))
				{
					FIntPoint Grid;
                    if (Presence != TEXT("set") || !Requested)
                        return Fail(OutError, Presence != TEXT("set")
                            ? JsonFailure(TEXT("asset_data_subuv_grid_expected_invalid"), TEXT("The SubUV grid requires the set operation."),
                                Current.Key + TEXT("/presence"), MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("set")))
                            : ValueFailure(TEXT("asset_data_subuv_grid_expected_invalid"), TEXT("The SubUV grid value is missing."),
                                Current.Key, nullptr, TEXT("positive integer x and y")));
                    if (!TryReadExtent2i(*Requested, Grid, Current.Key, OutError)) return false;
                    const int64 CellCount = static_cast<int64>(Grid.X) * Grid.Y;
                    if (CellCount > MAX_int32 / 8)
                    {
                        auto Actual = MakeShared<FJsonObject>();
                        Actual->SetNumberField(TEXT("x"), Grid.X);
                        Actual->SetNumberField(TEXT("y"), Grid.Y);
                        Actual->SetField(TEXT("cell_count"), MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), CellCount)));
                        return Fail(OutError, JsonFailure(TEXT("asset_data_subuv_grid_expected_invalid"),
                            TEXT("The SubUV grid cell count exceeds the supported maximum."), Current.Key, MakeShared<FJsonValueObject>(Actual),
                            MakeShared<FJsonValueNumber>(MAX_int32 / 8)));
                    }
					if (!TryBuildExtent2i(Grid, Resources, Expected, OutError)) return false;
				}
				else if (Current.Key == TEXT("bounding_geometry"))
				{
					FString Name;
                    if (Presence != TEXT("set") || !Requested)
                        return Fail(OutError, Presence != TEXT("set")
                            ? JsonFailure(TEXT("asset_data_subuv_bounding_expected_invalid"), TEXT("The SubUV bounding geometry requires the set operation."),
                                Current.Key + TEXT("/presence"), MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("set")))
                            : ValueFailure(TEXT("asset_data_subuv_bounding_expected_invalid"), TEXT("The SubUV bounding geometry value is missing."),
                                Current.Key, nullptr, TEXT("four_vertices or eight_vertices")));
                    if (!TryNormalizeSubUVBounding(*Requested, Name, OutError)) return false;
					if (!TryBuildString(Name, Resources, Expected, OutError)) return false;
				}
				else
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_subuv_expected_field_invalid"),
                        TEXT("This field is not supported by the SubUV animation."), Current.Key, MakeShared<FJsonValueString>(Current.Key),
                        MakeShared<FJsonValueString>(TEXT("source_texture_ref, grid or bounding_geometry")), TEXT("native_result")));
				}
			}
			else if (Current.Key == TEXT("texture_refs"))
			{
				TArray<FString> CurrentPaths;
				TArray<FString> RequestedPaths;
                if (!Requested)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_texture_collection_expected_invalid"),
                        TEXT("The requested texture reference list is missing."), Current.Key, nullptr, TEXT("texture reference array")));
                if (!TryReadStringArray(Current.Value.Get(), CurrentPaths, Current.Key, OutError)) return false;
				if (!TryResolveTexturePaths(Resolver, *Requested, RequestedPaths, OutError)) return false;
				if (Presence == TEXT("set"))
				{
					for (const FString& Path : RequestedPaths)
					{
						CurrentPaths.AddUnique(Path);
					}
				}
				else if (Presence == TEXT("remove"))
				{
					const TSet<FString> Removed(RequestedPaths);
					CurrentPaths.RemoveAll(
						[&Removed](const FString& Path)
						{
							return Removed.Contains(Path);
						});
				}
				else if (Presence == TEXT("replace"))
				{
					CurrentPaths = MoveTemp(RequestedPaths);
				}
				else
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_texture_collection_presence_invalid"),
                        TEXT("This texture collection operation is not supported."), Current.Key + TEXT("/presence"),
                        MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("set, remove, replace or clear"))));
				}
				if (!TryBuildStringArray(
						CurrentPaths,
						Resources,
						Expected,
						OutError))
				{
					return false;
				}
			}
			else
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_texture_expected_field_invalid"),
                    TEXT("This field is not supported by the texture collection."), Current.Key, MakeShared<FJsonValueString>(Current.Key),
                    MakeShared<FJsonValueString>(TEXT("texture_refs")), TEXT("native_result")));
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
