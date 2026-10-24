#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UeAgentAuthoringJsonFields
{
    namespace {
        const TCHAR* TypeName(EJson Type) {
            switch (Type) {
            case EJson::Object: return TEXT("object");
            case EJson::Array: return TEXT("array");
            case EJson::String: return TEXT("string");
            case EJson::Number: return TEXT("finite number");
            case EJson::Boolean: return TEXT("boolean");
            case EJson::Null: return TEXT("null");
            default: return TEXT("valid JSON value");
            }
        }
    }
    bool ValidateOptionalFields(const FJsonObject& Object, std::initializer_list<FFieldType> Fields,
        FDiagnostic& OutError, const FString& Prefix)
    {
        UeAgentAuthoringDiagnostics::FDiagnostic Failures;
        for (const auto& Field : Fields)
        {
            if (!Object.HasField(Field.Name)) continue;
            const auto Value = Object.TryGetField(Field.Name);
            FDiagnostic Cause;
            bool bValid = Value.IsValid() && Value->Type == Field.Type;
            if (Field.bReferencePath)
            {
                TOptional<FString> Path;
                bValid = UeAgentPropertyValueCodec::DecodeReferencePath(Value, Path, Cause);
            }
            else if (bValid && Field.Type == EJson::Number)
            {
                FString Lexeme;
                double Number = 0;
                bValid = UE::UAI::StateSemantic::CanonicalJson::TryGetNumberLexeme(*Value, Lexeme)
                    && UE::UAI::StateSemantic::TryParseFiniteDecimal(Lexeme, Number);
            }
            else if (bValid && Field.Type == EJson::Object) bValid = Value->AsObject().IsValid();
            if (bValid) continue;
            UeAgentAuthoringDiagnostics::FDiagnostic Failure;
            Failure.Code = TEXT("hjson_document_field_type_invalid");
            Failure.Stage = TEXT("input_decode");
            Failure.PropertyPath = Field.Name;
            Failure.PublicMessage = TEXT("The field has an incompatible value type.");
            Failure.ExpectedValue = MakeShared<FJsonValueString>(Field.bReferencePath ? TEXT("a reference path or null") : TypeName(Field.Type));
            Failure.ActualValue = Value;
            Failure.ActualValuePresent = true;
            Failure.SemanticPointer = Prefix + TEXT("/") + FString(Field.Name).Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
            Failure.CauseCode = Cause.Code;
            if (!Cause.IsEmpty()) Failure.PublicCause = MakeShared<FDiagnostic>(MoveTemp(Cause));
            UeAgentAuthoringDiagnostics::LocateAuthoringSource(Failure, Object, Field.Name);
            Failures.Append(Failure);
        }
        if (Failures.Code.IsEmpty()) return true;
        OutError = MoveTemp(Failures);
        return false;
    }

    bool ValidateDeclaredFields(const FJsonObject& Object, std::initializer_list<FFieldType> Fields,
        FDiagnostic& OutError, const FString& Prefix)
    {
        UeAgentAuthoringDiagnostics::FDiagnostic Failures;
        FDiagnostic TypeError;
        if (!ValidateOptionalFields(Object, Fields, TypeError, Prefix)) Failures.Append(TypeError);
        TSet<FString> Declared;
        for (const auto& Field : Fields) Declared.Add(Field.Name);
        for (const auto& Pair : Object.Values)
        {
            if (Declared.Contains(Pair.Key)) continue;
            UeAgentAuthoringDiagnostics::FDiagnostic Failure;
            Failure.Code = TEXT("hjson_document_field_unknown");
            Failure.Stage = TEXT("input_decode");
            Failure.PropertyPath = Pair.Key;
            Failure.PublicMessage = TEXT("This field is not declared by the native record.");
            TArray<TSharedPtr<FJsonValue>> Allowed;
            for (const auto& Field : Fields) Allowed.Add(MakeShared<FJsonValueString>(Field.Name));
            Failure.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Allowed));
            Failure.ActualValue = MakeShared<FJsonValueString>(Pair.Key);
            Failure.SemanticPointer = Prefix + TEXT("/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
            UeAgentAuthoringDiagnostics::LocateAuthoringSource(Failure, Object, *Pair.Key);
            Failures.Append(Failure);
        }
        if (Failures.Code.IsEmpty()) return true;
        OutError = MoveTemp(Failures);
        return false;
    }

    bool ReadReferenceField(const FJsonObject& Object, const TCHAR* Field, FString& OutPath, FDiagnostic& OutError)
    {
        if (!Object.HasField(Field)) return true;
        TOptional<FString> Path;
        FDiagnostic Cause;
        if (!UeAgentPropertyValueCodec::DecodeReferencePath(Object.TryGetField(Field), Path, Cause))
        {
            OutError = MoveTemp(Cause);
            UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError,
                TEXT("/") + FString(Field).Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
            UeAgentAuthoringDiagnostics::LocateFailure(OutError, Object, TEXT("input_decode"), FString(), Field);
            return false;
        }
        OutPath = Path.IsSet() ? Path.GetValue() : FString();
        return true;
    }

    bool ReadInt32Field(const FJsonObject& Object, const TCHAR* Field, int32& OutValue, FDiagnostic& OutError)
    {
        if (!Object.HasField(Field)) return true;
        const auto Value = Object.TryGetField(Field);
        FString Lexeme;
        int64 Number = 0;
        if (!Value || !UE::UAI::StateSemantic::CanonicalJson::TryGetNumberLexeme(*Value, Lexeme)
            || !UE::UAI::StateSemantic::TryParseInt64Number(Lexeme, Number) || Number < MIN_int32 || Number > MAX_int32)
        {
            UeAgentAuthoringDiagnostics::FDiagnostic Failure;
            Failure.Code = TEXT("hjson_document_integer_out_of_range");
            Failure.Stage = TEXT("input_decode");
            Failure.PropertyPath = Field;
            Failure.PublicMessage = TEXT("The field requires an exact signed 32-bit integer.");
            Failure.ExpectedValue = MakeShared<FJsonValueString>(TEXT("integer in [-2147483648, 2147483647]"));
            Failure.ActualValue = Value;
            UeAgentAuthoringDiagnostics::LocateAuthoringSource(Failure, Object, Field);
            OutError = MoveTemp(Failure);
            return false;
        }
        OutValue = static_cast<int32>(Number);
        return true;
    }

    bool ReadFloat64Field(const FJsonObject& Object, const TCHAR* Field, double& OutValue, FDiagnostic& OutError)
    {
        if (!Object.HasField(Field)) return true;
        const auto Value = Object.TryGetField(Field);
        FString Lexeme;
        if (Value && UE::UAI::StateSemantic::CanonicalJson::TryGetNumberLexeme(*Value, Lexeme)
            && UE::UAI::StateSemantic::TryParseFiniteDecimal(Lexeme, OutValue)) return true;
        UeAgentAuthoringDiagnostics::FDiagnostic Failure;
        Failure.Code = TEXT("hjson_document_float64_invalid");
        Failure.Stage = TEXT("input_decode");
        Failure.PropertyPath = Field;
        Failure.PublicMessage = TEXT("The field requires a finite 64-bit number.");
        Failure.ExpectedValue = MakeShared<FJsonValueString>(TEXT("finite float64"));
        Failure.ActualValue = Value;
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(Failure, Object, Field);
        OutError = MoveTemp(Failure);
        return false;
    }

    bool ReadFloat32Field(const FJsonObject& Object, const TCHAR* Field, float& OutValue, FDiagnostic& OutError)
    {
        if (!Object.HasField(Field)) return true;
        const auto Value = Object.TryGetField(Field);
        FString Lexeme;
        FDiagnostic Cause;
        double Number = 0;
        if (Value && UE::UAI::StateSemantic::CanonicalJson::TryGetNumberLexeme(*Value, Lexeme)
            && UE::UAI::StateSemantic::TryParseFiniteDecimal(Lexeme, Number)
            && UeAgentPropertyValueCodec::TryConvertFloat32(Number, OutValue, Cause)) return true;
        UeAgentAuthoringDiagnostics::FDiagnostic Failure;
        Failure.Code = TEXT("hjson_document_float32_invalid");
        Failure.Stage = TEXT("input_decode");
        Failure.PropertyPath = Field;
        Failure.PublicMessage = TEXT("The field requires a finite 32-bit number.");
        Failure.ExpectedValue = MakeShared<FJsonValueString>(TEXT("finite float32"));
        Failure.ActualValue = Value;
        Failure.CauseCode = Cause.Code;
            if (!Cause.IsEmpty()) Failure.PublicCause = MakeShared<FDiagnostic>(MoveTemp(Cause));
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(Failure, Object, Field);
        OutError = MoveTemp(Failure);
        return false;
    }
}
