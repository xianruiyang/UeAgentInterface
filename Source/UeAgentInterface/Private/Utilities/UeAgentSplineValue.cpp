#include "Utilities/UeAgentSplineValue.h"
#include "Utilities/UeAgentNativeNumberFormat.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Misc/ScopeExit.h"

#include "Components/SplineComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UeAgentSplineValue
{
namespace
{
template<typename T>
TArray<TSharedPtr<FJsonValue>> Numbers(std::initializer_list<T> Values)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    for (T V : Values) Out.Add(UeAgentNumericFormatting::MakeNativeValue(V));
    return Out;
}
FString ModeName(EInterpCurveMode Mode)
{
    switch (Mode)
    {
    case CIM_Linear: return TEXT("Linear");
    case CIM_Constant: return TEXT("Constant");
    case CIM_CurveAutoClamped: return TEXT("CurveClamped");
    case CIM_CurveUser: case CIM_CurveBreak: return TEXT("CurveCustomTangent");
    default: return TEXT("Curve");
    }
}
bool ReadNumbers(const FJsonObject& Row, const TCHAR* Key, int32 Count, double* Out, FDiagnostic& Error)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Row.TryGetArrayField(Key, Values) || Values->Num() != Count)
    { Error = FDiagnostic::Failure(FString::Printf(TEXT("spline_%s_requires_%d_numbers"), Key, Count),
        FString::Printf(TEXT("Spline field '%s' requires %d numbers."), Key, Count), TEXT("spline_value_decode"));
        Error.PropertyPath = Key; Error.Source.JsonPath = TEXT("/") + FString(Key);
        Error.ExpectedValue = MakeShared<FJsonValueNumber>(Count);
        if (Values) Error.ActualValue = MakeShared<FJsonValueNumber>(Values->Num());
        else Error.ActualValue = MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Row.Values.FindRef(Key)));
        return false; }
    for (int32 I = 0; I < Count; ++I)
        if (!(*Values)[I] || !(*Values)[I]->TryGetNumber(Out[I]) || !FMath::IsFinite(Out[I]))
        {
            Error = FDiagnostic::Failure(TEXT("spline_non_finite_number"), TEXT("The spline component must be a finite number."), TEXT("spline_value_decode"));
            Error.PropertyPath = FString::Printf(TEXT("%s[%d]"), Key, I);
            Error.Source.JsonPath = FString::Printf(TEXT("/%s/%d"), Key, I);
            Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("finite number"));
            Error.ActualValue = MakeShared<FJsonValueString>((*Values)[I] && (*Values)[I]->Type == EJson::Number
                ? TEXT("non-finite number") : UeAgentJsonDiagnostics::JsonValueTypeToString((*Values)[I]));
            return false;
        }
    return true;
}
}

TSharedPtr<FJsonObject> Encode(const FSplineCurves& Curves)
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("schema"), TEXT("uai.spline.points/1"));
    TArray<TSharedPtr<FJsonValue>> Points;
    for (const auto& P : Curves.Position.Points)
    {
        auto Row = MakeShared<FJsonObject>();
        Row->SetField(TEXT("input_key"), UeAgentNumericFormatting::MakeNativeValue(P.InVal));
        Row->SetStringField(TEXT("type"), ModeName(P.InterpMode));
        Row->SetArrayField(TEXT("position"), Numbers({P.OutVal.X, P.OutVal.Y, P.OutVal.Z}));
        const FQuat R = Curves.Rotation.Eval(P.InVal, FQuat::Identity);
        const FVector S = Curves.Scale.Eval(P.InVal, FVector::OneVector);
        Row->SetArrayField(TEXT("rotation"), Numbers({R.X, R.Y, R.Z, R.W}));
        Row->SetArrayField(TEXT("scale"), Numbers({S.X, S.Y, S.Z}));
        if (P.InterpMode == CIM_CurveUser || P.InterpMode == CIM_CurveBreak)
        {
            Row->SetArrayField(TEXT("arrive_tangent"), Numbers({P.ArriveTangent.X, P.ArriveTangent.Y, P.ArriveTangent.Z}));
            Row->SetArrayField(TEXT("leave_tangent"), Numbers({P.LeaveTangent.X, P.LeaveTangent.Y, P.LeaveTangent.Z}));
        }
        Points.Add(MakeShared<FJsonValueObject>(Row));
    }
    Result->SetArrayField(TEXT("points"), MoveTemp(Points));
    return Result;
}

bool Decode(const TSharedPtr<FJsonValue>& Value, FSplineCurves& Curves, FDiagnostic& Error)
{
    Error.Reset();
    const auto Object = Value && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
    FString Schema;
    if (!Object)
    {
        Error = FDiagnostic::Failure(TEXT("spline_points_schema_required"), TEXT("The spline value must be an object."), TEXT("spline_value_decode"));
        Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
        Error.ActualValue = MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Value));
        return false;
    }
    if (!Object->TryGetStringField(TEXT("schema"), Schema) || Schema != TEXT("uai.spline.points/1"))
    {
        Error = FDiagnostic::Failure(TEXT("spline_points_schema_required"), TEXT("The spline schema is missing or unsupported."), TEXT("spline_value_decode"));
        Error.PropertyPath = TEXT("schema"); Error.Source.JsonPath = TEXT("/schema");
        Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("uai.spline.points/1"));
        Error.ActualValue = Object->Values.FindRef(TEXT("schema"));
        Error.ActualValuePresent = Object->HasField(TEXT("schema"));
        return false;
    }
    if (!Object->TryGetArrayField(TEXT("points"), Points))
    {
        Error = FDiagnostic::Failure(TEXT("spline_points_schema_required"), TEXT("Spline points must be an array."), TEXT("spline_value_decode"));
        Error.PropertyPath = TEXT("points"); Error.Source.JsonPath = TEXT("/points");
        Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("array"));
        Error.ActualValue = MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Object->Values.FindRef(TEXT("points"))));
        return false;
    }
    FSplineCurves Desired = Curves;
    Desired.Position.Points.Reset(); Desired.Rotation.Points.Reset(); Desired.Scale.Points.Reset();
    float Previous = -TNumericLimits<float>::Max();
    for (int32 Index = 0; Index < Points->Num(); ++Index)
    {
        ON_SCOPE_EXIT {
            if (!Error.IsEmpty()) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(
                Error, FString::Printf(TEXT("/points/%d"), Index));
        };
        const auto Row = (*Points)[Index] && (*Points)[Index]->Type == EJson::Object ? (*Points)[Index]->AsObject() : nullptr;
        double Key = 0;
        FString Type;
        double P[3], R[4], S[3];
        const FString InvalidPointCode = FString::Printf(TEXT("spline_point_%d_invalid_key_or_type"), Index);
        if (!Row)
        {
            Error = FDiagnostic::Failure(InvalidPointCode, TEXT("The spline point must be an object."), TEXT("spline_value_decode"));
            Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
            Error.ActualValue = MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString((*Points)[Index]));
            return false;
        }
        if (!Row->TryGetNumberField(TEXT("input_key"), Key) || !FMath::IsFinite(Key)
            || !FMath::IsFinite(static_cast<float>(Key)))
        {
            Error = FDiagnostic::Failure(InvalidPointCode, TEXT("The spline input key must be a finite number representable as a native float."), TEXT("spline_value_decode"));
            Error.PropertyPath = TEXT("input_key"); Error.Source.JsonPath = TEXT("/input_key");
            Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("finite float"));
            const auto Actual = Row->Values.FindRef(TEXT("input_key"));
            if (Actual && Actual->Type == EJson::Number && FMath::IsFinite(Key)) Error.ActualValue = Actual;
            else Error.ActualValue = MakeShared<FJsonValueString>(Actual && Actual->Type == EJson::Number
                ? TEXT("non-finite number") : UeAgentJsonDiagnostics::JsonValueTypeToString(Actual));
            return false;
        }
        if (Index && static_cast<float>(Key) <= Previous)
        {
            Error = FDiagnostic::Failure(InvalidPointCode, TEXT("Spline input keys must remain strictly increasing after conversion to native floats."), TEXT("spline_value_decode"));
            Error.PropertyPath = TEXT("input_key"); Error.Source.JsonPath = TEXT("/input_key");
            const auto Expected = MakeShared<FJsonObject>();
            Expected->SetNumberField(TEXT("greater_than"), Previous);
            Error.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
            const auto Actual = MakeShared<FJsonObject>();
            Actual->SetNumberField(TEXT("input_key"), Key);
            Actual->SetNumberField(TEXT("native_key"), static_cast<float>(Key));
            Error.ActualValue = MakeShared<FJsonValueObject>(Actual);
            return false;
        }
        if (!Row->TryGetStringField(TEXT("type"), Type))
        {
            Error = FDiagnostic::Failure(InvalidPointCode, TEXT("The spline point type must be a string."), TEXT("spline_value_decode"));
            Error.PropertyPath = TEXT("type"); Error.Source.JsonPath = TEXT("/type");
            Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("string"));
            Error.ActualValue = MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Row->Values.FindRef(TEXT("type"))));
            return false;
        }
        if (!ReadNumbers(*Row, TEXT("position"), 3, P, Error)
            || !ReadNumbers(*Row, TEXT("rotation"), 4, R, Error)
            || !ReadNumbers(*Row, TEXT("scale"), 3, S, Error)) return false;
        const float NativeKey = static_cast<float>(Key);
        EInterpCurveMode Mode;
        if (Type == TEXT("Linear")) Mode = CIM_Linear;
        else if (Type == TEXT("Constant")) Mode = CIM_Constant;
        else if (Type == TEXT("Curve")) Mode = CIM_CurveAuto;
        else if (Type == TEXT("CurveClamped")) Mode = CIM_CurveAutoClamped;
        else if (Type == TEXT("CurveCustomTangent")) Mode = CIM_CurveUser;
        else {
            Error = FDiagnostic::Failure(TEXT("spline_point_type_unknown"), TEXT("The spline point interpolation type is unsupported."), TEXT("spline_value_decode"));
            Error.PropertyPath = TEXT("type"); Error.Source.JsonPath = TEXT("/type");
            Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("Linear, Constant, Curve, CurveClamped, CurveCustomTangent"));
            Error.ActualValue = MakeShared<FJsonValueString>(Type);
            return false;
        }
        const auto* OldP = Curves.Position.Points.FindByPredicate([NativeKey](const auto& V) { return V.InVal == NativeKey; });
        if (OldP && ModeName(OldP->InterpMode) == Type) Mode = OldP->InterpMode;
        FInterpCurvePointVector Position = OldP ? *OldP : FInterpCurvePointVector(NativeKey, FVector::ZeroVector);
        Position.OutVal = FVector(P[0], P[1], P[2]);
        Position.InterpMode = Mode;
        if (Mode == CIM_CurveUser || Mode == CIM_CurveBreak)
        {
            double A[3], L[3];
            if (!ReadNumbers(*Row, TEXT("arrive_tangent"), 3, A, Error)
                || !ReadNumbers(*Row, TEXT("leave_tangent"), 3, L, Error)) return false;
            Position.ArriveTangent = FVector(A[0], A[1], A[2]);
            Position.LeaveTangent = FVector(L[0], L[1], L[2]);
        }
        FQuat Rotation(R[0], R[1], R[2], R[3]);
        if (!UeAgentPropertyValueCodec::TryNormalizeQuaternion(Rotation, Error))
        { auto Cause = MoveTemp(Error);
          Error = FDiagnostic::Failure(TEXT("spline_") + Cause.Code, TEXT("The spline point rotation is invalid."), TEXT("spline_value_decode"));
          Error.PropertyPath = TEXT("rotation"); Error.Source.JsonPath = TEXT("/rotation");
          Error.PublicCause = MakeShared<FDiagnostic>(MoveTemp(Cause)); return false; }
        const auto* OldR = Curves.Rotation.Points.FindByPredicate([NativeKey](const auto& V) { return V.InVal == NativeKey; });
        const auto* OldS = Curves.Scale.Points.FindByPredicate([NativeKey](const auto& V) { return V.InVal == NativeKey; });
        FInterpCurvePointQuat RotationPoint = OldR ? *OldR : FInterpCurvePointQuat(NativeKey, Rotation);
        FInterpCurvePointVector ScalePoint = OldS ? *OldS : FInterpCurvePointVector(NativeKey, FVector::OneVector);
        RotationPoint.OutVal = Rotation; ScalePoint.OutVal = FVector(S[0], S[1], S[2]);
        Desired.Position.Points.Add(Position); Desired.Rotation.Points.Add(RotationPoint); Desired.Scale.Points.Add(ScalePoint);
        Previous = NativeKey;
    }
    Curves = MoveTemp(Desired);
    return true;
}
}
