// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"

#include "Containers/AnsiString.h"
#include "Containers/Utf8String.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "Misc/PackageName.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/AnsiStrProperty.h"
#include "UObject/Class.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/PropertyOptional.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "UObject/Utf8StrProperty.h"
#include "Utilities/UeAgentCurveJson.h"

#include <limits>

namespace UeAgentPropertyValueCodec {
static FString GetValueKindInternal(const FProperty *Property,
                                    bool bIgnoreFixedArray);

namespace {
double CanonicalNumber(const double Value) {
  if (Value == 0.0)
    return 0.0;
  if (!FMath::IsFinite(Value))
    return Value;

  const double Magnitude = FMath::Max(1.0, FMath::Abs(Value));
  const double NoiseTolerance =
      8.0 * std::numeric_limits<double>::epsilon() * Magnitude;
  double DecimalScale = 1.0;
  for (int32 DecimalPlaces = 0; DecimalPlaces <= 12; ++DecimalPlaces) {
    if (FMath::Abs(Value) >
        std::numeric_limits<double>::max() / DecimalScale) {
      break;
    }
    const double Candidate =
        FMath::RoundToDouble(Value * DecimalScale) / DecimalScale;
    if (FMath::Abs(Value - Candidate) <= NoiseTolerance)
      return Candidate == 0.0 ? 0.0 : Candidate;
    DecimalScale *= 10.0;
  }
  return Value;
}

constexpr const TCHAR *LiteralField = TEXT("literal");

bool IsCanonicalSoftReferencePath(const FSoftObjectPath &Path) {
  return Path.IsNull() || (Path.IsValid() && FPackageName::IsValidObjectPath(
                                                 Path.GetAssetPathString()));
}

TSharedPtr<FJsonObject> MakeKindObject(const FString &Kind) {
  TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("kind"), Kind);
  return Object;
}

TSharedPtr<FJsonValue> MakeObjectValue(const TSharedPtr<FJsonObject> &Object) {
  return MakeShared<FJsonValueObject>(Object);
}

TSharedPtr<FJsonValue> MakeLiteral(const TSharedPtr<FJsonValue> &Literal) {
  TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetField(LiteralField, Literal);
  return MakeObjectValue(Object);
}

FString BoundString(const FString &Value, const FOptions &Options) {
  return Value;
}

bool Fail(FString &OutError, const FString &Error) {
  OutError = Error;
  return false;
}

UObject *ResolveLoadedRedirector(UObject *Object) {
  TSet<const UObjectRedirector *> Visited;
  while (UObjectRedirector *Redirector = Cast<UObjectRedirector>(Object)) {
    if (Visited.Contains(Redirector)) {
      return nullptr;
    }
    Visited.Add(Redirector);
    Object = Redirector->DestinationObject;
  }
  return Object;
}

bool GetObject(const TSharedPtr<FJsonValue> &Value,
               TSharedPtr<FJsonObject> &OutObject, FString &OutError) {
  if (!Value.IsValid() || Value->Type != EJson::Object) {
    return Fail(OutError, TEXT("expected_object"));
  }
  OutObject = Value->AsObject();
  return OutObject.IsValid() || Fail(OutError, TEXT("invalid_object"));
}

bool GetLiteral(const TSharedPtr<FJsonValue> &Value,
                TSharedPtr<FJsonValue> &OutLiteral, FString &OutError) {
  if (!Value.IsValid())
    return Fail(OutError, TEXT("invalid_literal"));
  if (Value->Type != EJson::Object) {
    OutLiteral = Value;
    return true;
  }
  TSharedPtr<FJsonObject> Object;
  if (!GetObject(Value, Object, OutError)) {
    return false;
  }
  OutLiteral = Object->TryGetField(LiteralField);
  if (!OutLiteral.IsValid())
    return Fail(OutError, TEXT("missing_literal"));
  return Object->Values.Num() == 1 ||
         Fail(OutError, TEXT("unexpected_literal_fields"));
}

bool GetReferencePath(const TSharedPtr<FJsonValue> &Value,
                      TSharedPtr<FJsonValue> &OutPath, FString &OutError) {
  if (!Value.IsValid())
    return Fail(OutError, TEXT("invalid_reference_path"));
  if (Value->Type == EJson::String || Value->Type == EJson::Null) {
    OutPath = Value;
    return true;
  }
  TSharedPtr<FJsonObject> Object;
  if (!GetObject(Value, Object, OutError))
    return false;
  OutPath = Object->TryGetField(TEXT("path"));
  if (!OutPath.IsValid())
    return Fail(OutError, TEXT("missing_reference_path"));
  return Object->Values.Num() == 1 ||
         Fail(OutError, TEXT("unexpected_reference_fields"));
}

bool GetComponents(const TSharedPtr<FJsonValue> &Value, int32 ExpectedCount,
                   const TArray<TSharedPtr<FJsonValue>> *&OutComponents,
                   FString &OutError) {
  TSharedPtr<FJsonObject> Object;
  if (!GetObject(Value, Object, OutError) ||
      !Object->TryGetArrayField(TEXT("components"), OutComponents) ||
      !OutComponents) {
    return Fail(OutError, TEXT("missing_components"));
  }
  if (Object->Values.Num() != 1)
    return Fail(OutError, TEXT("unexpected_component_fields"));
  return OutComponents->Num() == ExpectedCount ||
         Fail(OutError,
              FString::Printf(TEXT("component_count_mismatch_expected_%d"),
                              ExpectedCount));
}

bool JsonNumber(const TSharedPtr<FJsonValue> &Value, double &OutNumber,
                FString &OutError) {
  if (!Value.IsValid() || Value->Type != EJson::Number) {
    return Fail(OutError, TEXT("expected_number"));
  }
  OutNumber = Value->AsNumber();
  return FMath::IsFinite(OutNumber) ||
         Fail(OutError, TEXT("number_not_finite"));
}

bool JsonInteger(const TSharedPtr<FJsonValue> &Value, int64 Min, int64 Max,
                 int64 &OutNumber, FString &OutError) {
  double Number = 0.0;
  if (!JsonNumber(Value, Number, OutError) ||
      FMath::TruncToDouble(Number) != Number ||
      Number < static_cast<double>(Min) || Number > static_cast<double>(Max)) {
    return Fail(OutError, TEXT("integer_out_of_range"));
  }
  OutNumber = static_cast<int64>(Number);
  return true;
}

bool JsonUnsignedInteger(const TSharedPtr<FJsonValue> &Value, uint64 Max,
                         uint64 &OutNumber, FString &OutError) {
  double Number = 0.0;
  if (!JsonNumber(Value, Number, OutError) ||
      FMath::TruncToDouble(Number) != Number || Number < 0.0 ||
      Number > static_cast<double>(Max)) {
    return Fail(OutError, TEXT("unsigned_integer_out_of_range"));
  }
  OutNumber = static_cast<uint64>(Number);
  return true;
}

bool ValidateNumericConstraints(const FProperty *Property, double Number,
                                FString &OutError) {
  if (!FMath::IsFinite(Number))
    return Fail(OutError, TEXT("number_not_finite"));
  auto ValidateBound = [Property, Number, &OutError](const TCHAR *Key,
                                                     bool bMinimum) {
    const FString Text = Property ? Property->GetMetaData(Key) : FString();
    if (Text.IsEmpty())
      return true;
    double Bound = 0.0;
    if (!LexTryParseString(Bound, *Text) || !FMath::IsFinite(Bound))
      return Fail(OutError, FString::Printf(TEXT("invalid_%s_metadata"), Key));
    if ((bMinimum && Number < Bound) || (!bMinimum && Number > Bound))
      return Fail(OutError, FString::Printf(TEXT("violates_%s"), Key));
    return true;
  };
  return ValidateBound(TEXT("ClampMin"), true) &&
         ValidateBound(TEXT("ClampMax"), false);
}

bool ValidateSignedIntegerConstraints(const FProperty *Property, int64 Number,
                                      FString &OutError) {
  auto ValidateBound = [Property, Number, &OutError](const TCHAR *Key,
                                                     bool bMinimum) {
    const FString Text = Property ? Property->GetMetaData(Key) : FString();
    if (Text.IsEmpty())
      return true;
    int64 Bound = 0;
    if (LexTryParseString(Bound, *Text)) {
      return (bMinimum ? Number >= Bound : Number <= Bound) ||
             Fail(OutError,
                  bMinimum ? TEXT("below_clamp_min") : TEXT("above_clamp_max"));
    }
    return ValidateNumericConstraints(Property, static_cast<double>(Number),
                                      OutError);
  };
  return ValidateBound(TEXT("ClampMin"), true) &&
         ValidateBound(TEXT("ClampMax"), false);
}

bool ValidateUnsignedIntegerConstraints(const FProperty *Property,
                                        uint64 Number, FString &OutError) {
  auto ValidateBound = [Property, Number, &OutError](const TCHAR *Key,
                                                     bool bMinimum) {
    const FString Text = Property ? Property->GetMetaData(Key) : FString();
    if (Text.IsEmpty())
      return true;
    uint64 Bound = 0;
    if (LexTryParseString(Bound, *Text)) {
      return (bMinimum ? Number >= Bound : Number <= Bound) ||
             Fail(OutError,
                  bMinimum ? TEXT("below_clamp_min") : TEXT("above_clamp_max"));
    }
    return ValidateNumericConstraints(Property, static_cast<double>(Number),
                                      OutError);
  };
  return ValidateBound(TEXT("ClampMin"), true) &&
         ValidateBound(TEXT("ClampMax"), false);
}

bool IsBitmaskProperty(const FProperty *Property) {
  return UeAgentAuthoringTypeDescriptors::IsBitmaskProperty(Property);
}

bool IsUnsignedNumericProperty(const FNumericProperty *Property) {
  return CastField<FByteProperty>(Property) ||
         CastField<FUInt16Property>(Property) ||
         CastField<FUInt32Property>(Property) ||
         CastField<FUInt64Property>(Property);
}

const UEnum *GetPropertyEnum(const FProperty *Property) {
  return UeAgentAuthoringTypeDescriptors::GetPropertyEnum(Property);
}

bool UsesEnumValuesAsMaskValues(const FProperty *Property, const UEnum *Enum) {
  return (Property && Property->GetBoolMetaData(
                          TEXT("UseEnumValuesAsMaskValuesInEditor"))) ||
         (Enum &&
          Enum->GetBoolMetaData(TEXT("UseEnumValuesAsMaskValuesInEditor")));
}

FString StructKind(const UScriptStruct *Struct) {
  return UeAgentAuthoringTypeDescriptors::GetStructKind(Struct);
}

FProperty *FindStructField(const UScriptStruct *Struct, const TCHAR *Name) {
  return Struct ? FindFProperty<FProperty>(Struct, Name) : nullptr;
}

bool ReadStructNumber(const UScriptStruct *Struct, const void *ValuePtr,
                      const TCHAR *FieldName, double &OutValue) {
  FProperty *Field = FindStructField(Struct, FieldName);
  const FNumericProperty *Numeric = CastField<FNumericProperty>(Field);
  if (!Numeric) {
    return false;
  }
  const void *FieldPtr = Field->ContainerPtrToValuePtr<void>(ValuePtr);
  OutValue =
      Numeric->IsFloatingPoint()
          ? Numeric->GetFloatingPointPropertyValue(FieldPtr)
      : IsUnsignedNumericProperty(Numeric)
          ? static_cast<double>(Numeric->GetUnsignedIntPropertyValue(FieldPtr))
          : static_cast<double>(Numeric->GetSignedIntPropertyValue(FieldPtr));
  return true;
}

bool WriteStructNumber(const UScriptStruct *Struct, void *ValuePtr,
                       const TCHAR *FieldName, double Value,
                       FString &OutError) {
  FProperty *Field = FindStructField(Struct, FieldName);
  FNumericProperty *Numeric = CastField<FNumericProperty>(Field);
  if (!Numeric) {
    return Fail(
        OutError,
        FString::Printf(TEXT("missing_numeric_struct_field_%s"), FieldName));
  }
  void *FieldPtr = Field->ContainerPtrToValuePtr<void>(ValuePtr);
  if (Numeric->IsFloatingPoint())
    Numeric->SetFloatingPointPropertyValue(FieldPtr, Value);
  else if (IsUnsignedNumericProperty(Numeric))
    Numeric->SetIntPropertyValue(FieldPtr, static_cast<uint64>(Value));
  else
    Numeric->SetIntPropertyValue(FieldPtr, static_cast<int64>(Value));
  return true;
}

TSharedPtr<FJsonValue> EncodeComponents(const FStructProperty *StructProperty,
                                        const void *ValuePtr,
                                        const TArray<const TCHAR *> &Names) {
  TArray<TSharedPtr<FJsonValue>> Components;
  for (const TCHAR *Name : Names) {
    double Number = 0.0;
    if (!ReadStructNumber(StructProperty ? StructProperty->Struct : nullptr,
                          ValuePtr, Name, Number))
      return MakeShared<FJsonValueNull>();
    Components.Add(MakeShared<FJsonValueNumber>(CanonicalNumber(Number)));
  }
  TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetArrayField(TEXT("components"), Components);
  return MakeObjectValue(Object);
}

bool DecodeComponents(const UScriptStruct *Struct,
                      const FProperty *ConstraintsProperty,
                      const TSharedPtr<FJsonValue> &Value, void *OutValuePtr,
                      const TArray<const TCHAR *> &Names, FString &OutError) {
  const TArray<TSharedPtr<FJsonValue>> *Components = nullptr;
  if (!GetComponents(Value, Names.Num(), Components, OutError))
    return false;
  for (int32 Index = 0; Index < Names.Num(); ++Index) {
    double Number = 0.0;
    if (!JsonNumber((*Components)[Index], Number, OutError) ||
        (ConstraintsProperty &&
         !ValidateNumericConstraints(ConstraintsProperty, Number, OutError)) ||
        !WriteStructNumber(Struct, OutValuePtr, Names[Index], Number, OutError))
      return false;
  }
  return true;
}

bool DecodeKnownStructValue(UScriptStruct *Struct,
                            const FProperty *ConstraintsProperty,
                            const TSharedPtr<FJsonValue> &Value,
                            void *OutValuePtr, FString &OutError,
                            bool &bOutHandled) {
  bOutHandled = true;
  const FString Kind = StructKind(Struct);
  if (Kind == TEXT("vector2"))
    return DecodeComponents(Struct, ConstraintsProperty, Value, OutValuePtr,
                            {TEXT("X"), TEXT("Y")}, OutError);
  if (Kind == TEXT("vector3"))
    return DecodeComponents(Struct, ConstraintsProperty, Value, OutValuePtr,
                            {TEXT("X"), TEXT("Y"), TEXT("Z")}, OutError);
  if (Kind == TEXT("vector4"))
    return DecodeComponents(Struct, ConstraintsProperty, Value, OutValuePtr,
                            {TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W")},
                            OutError);
  if (Kind == TEXT("rotator"))
    return DecodeComponents(Struct, ConstraintsProperty, Value, OutValuePtr,
                            {TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll")},
                            OutError);
  if (Kind == TEXT("linear_color"))
    return DecodeComponents(Struct, ConstraintsProperty, Value, OutValuePtr,
                            {TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")},
                            OutError);
  if (Kind == TEXT("quat")) {
    if (!DecodeComponents(Struct, ConstraintsProperty, Value, OutValuePtr,
                          {TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W")},
                          OutError))
      return false;
    if (Struct->GetName() == TEXT("Quat4f")) {
      FQuat4f *Quat = static_cast<FQuat4f *>(OutValuePtr);
      if (Quat->SizeSquared() <= UE_SMALL_NUMBER)
        return Fail(OutError, TEXT("zero_quaternion"));
      Quat->Normalize();
    } else {
      FQuat *Quat = static_cast<FQuat *>(OutValuePtr);
      if (Quat->SizeSquared() <= UE_SMALL_NUMBER)
        return Fail(OutError, TEXT("zero_quaternion"));
      Quat->Normalize();
    }
    return true;
  }
  if (Kind == TEXT("color8")) {
    const TArray<TSharedPtr<FJsonValue>> *Components = nullptr;
    if (!GetComponents(Value, 4, Components, OutError))
      return false;
    const TArray<const TCHAR *> Names = {TEXT("R"), TEXT("G"), TEXT("B"),
                                         TEXT("A")};
    for (int32 Index = 0; Index < 4; ++Index) {
      uint64 Number = 0;
      if (!JsonUnsignedInteger((*Components)[Index], MAX_uint8, Number,
                               OutError) ||
          !WriteStructNumber(Struct, OutValuePtr, Names[Index],
                             static_cast<double>(Number), OutError))
        return false;
    }
    return true;
  }
  if (Kind == TEXT("transform")) {
    TSharedPtr<FJsonObject> Object;
    const TArray<TSharedPtr<FJsonValue>> *Translation = nullptr;
    const TArray<TSharedPtr<FJsonValue>> *Rotation = nullptr;
    const TArray<TSharedPtr<FJsonValue>> *Scale = nullptr;
    if (!GetObject(Value, Object, OutError) || Object->Values.Num() != 3 ||
        !Object->TryGetArrayField(TEXT("translation"), Translation) ||
        !Translation || Translation->Num() != 3 ||
        !Object->TryGetArrayField(TEXT("rotation"), Rotation) || !Rotation ||
        Rotation->Num() != 3 ||
        !Object->TryGetArrayField(TEXT("scale"), Scale) || !Scale ||
        Scale->Num() != 3) {
      return Fail(OutError, TEXT("invalid_transform_shape"));
    }
    double T[3], R[3], S[3];
    for (int32 Index = 0; Index < 3; ++Index) {
      if (!JsonNumber((*Translation)[Index], T[Index], OutError) ||
          !JsonNumber((*Rotation)[Index], R[Index], OutError) ||
          !JsonNumber((*Scale)[Index], S[Index], OutError))
        return false;
    }
    const FTransform Transform(FRotator(R[0], R[1], R[2]),
                               FVector(T[0], T[1], T[2]),
                               FVector(S[0], S[1], S[2]));
    if (Struct->GetName() == TEXT("Transform3f"))
      *static_cast<FTransform3f *>(OutValuePtr) = FTransform3f(Transform);
    else
      *static_cast<FTransform *>(OutValuePtr) = Transform;
    return true;
  }
  if (Kind == TEXT("guid")) {
    TSharedPtr<FJsonValue> Literal;
    return GetLiteral(Value, Literal, OutError) &&
                   Literal->Type == EJson::String &&
                   FGuid::Parse(Literal->AsString(),
                                *static_cast<FGuid *>(OutValuePtr))
               ? true
               : Fail(OutError, TEXT("invalid_guid"));
  }
  if (Kind == TEXT("datetime") || Kind == TEXT("timespan")) {
    TSharedPtr<FJsonObject> Object;
    FString TicksText;
    int64 Ticks = 0;
    if (!GetObject(Value, Object, OutError) || Object->Values.Num() != 1 ||
        !Object->TryGetStringField(TEXT("ticks"), TicksText) ||
        !LexTryParseString(Ticks, *TicksText)) {
      return Fail(OutError, TEXT("invalid_ticks"));
    }
    if (Kind == TEXT("datetime"))
      *static_cast<FDateTime *>(OutValuePtr) = FDateTime(Ticks);
    else
      *static_cast<FTimespan *>(OutValuePtr) = FTimespan(Ticks);
    return true;
  }

  bOutHandled = false;
  return true;
}

TSharedPtr<FJsonObject>
BuildTypeDescriptorInternal(const FProperty *Property,
                            bool bIgnoreFixedArray = false) {
  return UeAgentAuthoringTypeDescriptors::BuildJson(Property,
                                                    bIgnoreFixedArray);
}

bool ValidateDepth(const FOptions &Options, int32 Depth, FString &OutError) {
  return (Options.MaxDepth < 0 || Depth <= Options.MaxDepth) ||
         Fail(OutError, TEXT("max_depth_exceeded"));
}

bool ValidateContainerCount(const FOptions &Options, int32 Count,
                            FString &OutError) {
  return (Options.MaxContainerItems <= 0 ||
          Count <= Options.MaxContainerItems) ||
         Fail(OutError, TEXT("max_container_items_exceeded"));
}

bool IsTruncatedValue(const TSharedPtr<FJsonValue> &Value) {
  if (!Value.IsValid() || Value->Type == EJson::None)
    return true;
  if (Value->Type == EJson::Null)
    return false;
  if (Value->Type == EJson::Array) {
    for (const TSharedPtr<FJsonValue> &Item : Value->AsArray())
      if (IsTruncatedValue(Item))
        return true;
    return false;
  }
  if (Value->Type != EJson::Object)
    return false;
  const TSharedPtr<FJsonObject> Object = Value->AsObject();
  bool bTruncated = false;
  if (!Object.IsValid() ||
      (Object->TryGetBoolField(TEXT("truncated"), bTruncated) && bTruncated))
    return true;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : Object->Values)
    if (IsTruncatedValue(Pair.Value))
      return true;
  return false;
}

bool RejectTruncatedObject(const TSharedPtr<FJsonObject> &Object,
                           FString &OutError) {
  bool bTruncated = false;
  return !(Object.IsValid() &&
           Object->TryGetBoolField(TEXT("truncated"), bTruncated) &&
           bTruncated) ||
         Fail(OutError, TEXT("truncated_value_not_authorable"));
}

bool JsonEquivalent(const TSharedPtr<FJsonValue> &Left,
                    const TSharedPtr<FJsonValue> &Right) {
  if (!Left.IsValid() || !Right.IsValid() || Left->Type != Right->Type)
    return false;
  switch (Left->Type) {
  case EJson::Null:
    return true;
  case EJson::String:
    return Left->AsString() == Right->AsString();
  case EJson::Number:
    return Left->AsNumber() == Right->AsNumber();
  case EJson::Boolean:
    return Left->AsBool() == Right->AsBool();
  case EJson::Array: {
    const TArray<TSharedPtr<FJsonValue>> &A = Left->AsArray();
    const TArray<TSharedPtr<FJsonValue>> &B = Right->AsArray();
    if (A.Num() != B.Num())
      return false;
    for (int32 Index = 0; Index < A.Num(); ++Index)
      if (!JsonEquivalent(A[Index], B[Index]))
        return false;
    return true;
  }
  case EJson::Object: {
    const TSharedPtr<FJsonObject> A = Left->AsObject();
    const TSharedPtr<FJsonObject> B = Right->AsObject();
    if (!A.IsValid() || !B.IsValid() || A->Values.Num() != B->Values.Num())
      return false;
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : A->Values) {
      if (!JsonEquivalent(Pair.Value, B->TryGetField(Pair.Key)))
        return false;
    }
    return true;
  }
  default:
    return false;
  }
}

bool IsStructMemberAuthoringEditable(const FProperty *Property) {
  // EditCondition is value-dependent availability, not a limitation of the
  // struct member's canonical codec or recursive authoring shape.
  return Property && Property->HasAnyPropertyFlags(CPF_Edit) &&
         !Property->HasAnyPropertyFlags(
             CPF_EditConst | CPF_Transient | CPF_Deprecated |
             CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient |
             CPF_TextExportTransient);
}

bool ValidateAuthoringType(const FProperty *Property, const void *ValuePtr,
                           bool bIgnoreFixedArray, bool bRequireEditFlag,
                           FString &OutReason) {
  if (!Property) {
    return Fail(OutReason, TEXT("invalid_property"));
  }
  if (bRequireEditFlag && !IsStructMemberAuthoringEditable(Property)) {
    return Fail(OutReason,
                FString::Printf(TEXT("struct_member_not_editable_%s"),
                                *Property->GetName()));
  }
  if (!bIgnoreFixedArray && Property->ArrayDim > 1) {
    if (!ValidateAuthoringType(Property, nullptr, true, false, OutReason))
      return false;
    if (ValuePtr) {
      for (int32 Index = 0; Index < Property->ArrayDim; ++Index) {
        const void *Element = static_cast<const uint8 *>(ValuePtr) +
                              Index * Property->GetElementSize();
        if (!ValidateAuthoringType(Property, Element, true, false, OutReason))
          return false;
      }
    }
    return true;
  }
  if (GetValueKindInternal(Property, bIgnoreFixedArray) ==
      TEXT("unavailable")) {
    return Fail(OutReason, TEXT("unsupported_property_type"));
  }
  if (const FArrayProperty *Array = CastField<FArrayProperty>(Property)) {
    if (!ValidateAuthoringType(Array->Inner, nullptr, false, false, OutReason))
      return false;
    if (ValuePtr) {
      FScriptArrayHelper Helper(Array, ValuePtr);
      for (int32 Index = 0; Index < Helper.Num(); ++Index) {
        if (!ValidateAuthoringType(Array->Inner, Helper.GetRawPtr(Index), false,
                                   false, OutReason))
          return false;
      }
    }
    return true;
  }
  if (const FSetProperty *Set = CastField<FSetProperty>(Property)) {
    if (!ValidateAuthoringType(Set->ElementProp, nullptr, false, false,
                               OutReason))
      return false;
    if (ValuePtr) {
      FScriptSetHelper Helper(Set, ValuePtr);
      for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index) {
        if (Helper.IsValidIndex(Index) &&
            !ValidateAuthoringType(Set->ElementProp,
                                   Helper.GetElementPtr(Index), false, false,
                                   OutReason))
          return false;
      }
    }
    return true;
  }
  if (const FMapProperty *Map = CastField<FMapProperty>(Property)) {
    if (!Map->KeyProp ||
        !Map->KeyProp->HasAllPropertyFlags(CPF_HasGetValueTypeHash)) {
      return Fail(OutReason, TEXT("unstable_map_key_type"));
    }
    if (!ValidateAuthoringType(Map->KeyProp, nullptr, false, false,
                               OutReason) ||
        !ValidateAuthoringType(Map->ValueProp, nullptr, false, false,
                               OutReason))
      return false;
    if (ValuePtr) {
      FScriptMapHelper Helper(Map, ValuePtr);
      for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index) {
        if (!Helper.IsValidIndex(Index))
          continue;
        if (!ValidateAuthoringType(Map->KeyProp, Helper.GetKeyPtr(Index), false,
                                   false, OutReason) ||
            !ValidateAuthoringType(Map->ValueProp, Helper.GetValuePtr(Index),
                                   false, false, OutReason))
          return false;
      }
    }
    return true;
  }
  if (const FOptionalProperty *Optional =
          CastField<FOptionalProperty>(Property)) {
    if (!ValidateAuthoringType(Optional->GetValueProperty(), nullptr, false,
                               false, OutReason))
      return false;
    return !ValuePtr || !Optional->IsSet(ValuePtr) ||
           ValidateAuthoringType(Optional->GetValueProperty(),
                                 Optional->GetValuePointerForRead(ValuePtr),
                                 false, false, OutReason);
  }
  if (const FStructProperty *Struct = CastField<FStructProperty>(Property)) {
    if (!Struct->Struct)
      return Fail(OutReason, TEXT("missing_struct_type"));
    const FString Kind = StructKind(Struct->Struct);
    if (Kind == TEXT("instanced_struct")) {
      if (!ValuePtr)
        return true;
      const FInstancedStruct &Instanced =
          *static_cast<const FInstancedStruct *>(ValuePtr);
      const UScriptStruct *DynamicStruct = Instanced.GetScriptStruct();
      if (!DynamicStruct || !Instanced.GetMemory())
        return true;
      for (TFieldIterator<FProperty> It(DynamicStruct,
                                        EFieldIterationFlags::IncludeSuper);
           It; ++It) {
        FProperty *Field = *It;
        if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
          continue;
        if (!ValidateAuthoringType(
                Field,
                Field->ContainerPtrToValuePtr<void>(Instanced.GetMemory()),
                false, true, OutReason))
          return false;
      }
      return true;
    }
    if (Kind != TEXT("struct"))
      return true;
    for (TFieldIterator<FProperty> It(Struct->Struct,
                                      EFieldIterationFlags::IncludeSuper);
         It; ++It) {
      FProperty *Field = *It;
      if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
        continue;
      const void *FieldValue =
          ValuePtr ? Field->ContainerPtrToValuePtr<void>(ValuePtr) : nullptr;
      if (!ValidateAuthoringType(Field, FieldValue, false, true, OutReason))
        return false;
    }
  }
  return true;
}
} // namespace

static FString GetValueKindInternal(const FProperty *Property,
                                    bool bIgnoreFixedArray) {
  return UeAgentAuthoringTypeDescriptors::GetValueKind(Property,
                                                       bIgnoreFixedArray);
}

FString GetValueKind(const FProperty *Property) {
  return GetValueKindInternal(Property, false);
}

bool IsSupported(const FProperty *Property, FString &OutReason) {
  TFunction<bool(const FProperty *, bool)> Validate =
      [&](const FProperty *Candidate, bool bIgnoreFixedArray) {
        if (!Candidate)
          return Fail(OutReason, TEXT("invalid_property"));
        if (!bIgnoreFixedArray && Candidate->ArrayDim > 1)
          return Validate(Candidate, true);
        if (GetValueKindInternal(Candidate, bIgnoreFixedArray) ==
            TEXT("unavailable"))
          return Fail(OutReason, TEXT("unsupported_property_type"));
        if (const FArrayProperty *Array = CastField<FArrayProperty>(Candidate))
          return Validate(Array->Inner, false);
        if (const FSetProperty *Set = CastField<FSetProperty>(Candidate))
          return Validate(Set->ElementProp, false);
        if (const FMapProperty *Map = CastField<FMapProperty>(Candidate)) {
          if (!Map->KeyProp ||
              !Map->KeyProp->HasAllPropertyFlags(CPF_HasGetValueTypeHash)) {
            return Fail(OutReason, TEXT("unstable_map_key_type"));
          }
          return Validate(Map->KeyProp, false) &&
                 Validate(Map->ValueProp, false);
        }
        if (const FOptionalProperty *Optional =
                CastField<FOptionalProperty>(Candidate))
          return Validate(Optional->GetValueProperty(), false);
        if (const FStructProperty *Struct =
                CastField<FStructProperty>(Candidate)) {
          if (!Struct->Struct)
            return Fail(OutReason, TEXT("missing_struct_type"));
          if (StructKind(Struct->Struct) != TEXT("struct"))
            return true;
          for (TFieldIterator<FProperty> It(Struct->Struct,
                                            EFieldIterationFlags::IncludeSuper);
               It; ++It) {
            if ((*It)->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
              continue;
            if (!Validate(*It, false))
              return false;
          }
        }
        return true;
      };
  if (!Validate(Property, false))
    return false;
  OutReason.Reset();
  return true;
}

bool IsAuthoringSupported(const FProperty *Property, const void *ValuePtr,
                          FString &OutReason) {
  if (!IsSupported(Property, OutReason) ||
      !ValidateAuthoringType(Property, ValuePtr, false, false, OutReason)) {
    return false;
  }
  OutReason.Reset();
  return true;
}

TSharedPtr<FJsonObject> BuildTypeDescriptor(const FProperty *Property) {
  return BuildTypeDescriptorInternal(Property);
}

void AddTypeMetadata(const FProperty *Property,
                     const TSharedPtr<FJsonObject> &Target) {
  if (!Target.IsValid())
    return;
  const TSharedPtr<FJsonObject> Type = BuildTypeDescriptorInternal(Property);
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : Type->Values) {
    if (Pair.Key != TEXT("kind"))
      Target->SetField(Pair.Key, Pair.Value);
  }
}

bool ValidateTypeMetadata(const FProperty *Property,
                          const TSharedPtr<FJsonObject> &Descriptor,
                          FString &OutError) {
  return UeAgentAuthoringTypeDescriptors::ValidateMetadata(Property, Descriptor,
                                                           OutError);
}

static const FInstancedStruct *
GetPropertyBagStorage(const FInstancedPropertyBag *Bag) {
  const FStructProperty *ValueProperty = FindFProperty<FStructProperty>(
      FInstancedPropertyBag::StaticStruct(), TEXT("Value"));
  return Bag && ValueProperty
             ? ValueProperty->ContainerPtrToValuePtr<FInstancedStruct>(Bag)
             : nullptr;
}

static FInstancedStruct *
GetMutablePropertyBagStorage(FInstancedPropertyBag *Bag) {
  FStructProperty *ValueProperty = FindFProperty<FStructProperty>(
      FInstancedPropertyBag::StaticStruct(), TEXT("Value"));
  return Bag && ValueProperty
             ? ValueProperty->ContainerPtrToValuePtr<FInstancedStruct>(Bag)
             : nullptr;
}

static TSharedPtr<FJsonObject>
BuildPropertyBagSchemaEntry(const FPropertyBagPropertyDesc &Desc) {
  TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
  Entry->SetStringField(TEXT("name"), Desc.Name.ToString());
  const TSharedPtr<FJsonObject> Type =
      BuildTypeDescriptorInternal(Desc.CachedProperty);
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : Type->Values) {
    Entry->SetField(Pair.Key == TEXT("kind") ? TEXT("value_kind") : Pair.Key,
                    Pair.Value);
  }
  if (Desc.ValueTypeObject) {
    Entry->SetStringField(TEXT("type_path"),
                          Desc.ValueTypeObject->GetPathName());
  }
  return Entry;
}

static bool ResolvePropertyBagTerminalType(const FString &Kind,
                                           const TSharedPtr<FJsonObject> &Type,
                                           EPropertyBagPropertyType &OutType,
                                           UObject *&OutTypeObject,
                                           FString &OutError) {
  OutTypeObject = nullptr;
  if (Kind == TEXT("bool"))
    OutType = EPropertyBagPropertyType::Bool;
  else if (Kind == TEXT("uint8"))
    OutType = EPropertyBagPropertyType::Byte;
  else if (Kind == TEXT("int32"))
    OutType = EPropertyBagPropertyType::Int32;
  else if (Kind == TEXT("int64"))
    OutType = EPropertyBagPropertyType::Int64;
  else if (Kind == TEXT("uint32"))
    OutType = EPropertyBagPropertyType::UInt32;
  else if (Kind == TEXT("uint64"))
    OutType = EPropertyBagPropertyType::UInt64;
  else if (Kind == TEXT("float"))
    OutType = EPropertyBagPropertyType::Float;
  else if (Kind == TEXT("double"))
    OutType = EPropertyBagPropertyType::Double;
  else if (Kind == TEXT("name"))
    OutType = EPropertyBagPropertyType::Name;
  else if (Kind == TEXT("string"))
    OutType = EPropertyBagPropertyType::String;
  else if (Kind == TEXT("text"))
    OutType = EPropertyBagPropertyType::Text;
  else if (Kind == TEXT("enum"))
    OutType = EPropertyBagPropertyType::Enum;
  else if (Kind == TEXT("object_ref"))
    OutType = EPropertyBagPropertyType::Object;
  else if (Kind == TEXT("soft_object_ref"))
    OutType = EPropertyBagPropertyType::SoftObject;
  else if (Kind == TEXT("class_ref"))
    OutType = EPropertyBagPropertyType::Class;
  else if (Kind == TEXT("soft_class_ref"))
    OutType = EPropertyBagPropertyType::SoftClass;
  else if (Kind == TEXT("struct") || Kind == TEXT("vector2") ||
           Kind == TEXT("vector3") || Kind == TEXT("vector4") ||
           Kind == TEXT("rotator") || Kind == TEXT("quat") ||
           Kind == TEXT("color8") || Kind == TEXT("linear_color") ||
           Kind == TEXT("transform") || Kind == TEXT("guid") ||
           Kind == TEXT("datetime") || Kind == TEXT("timespan") ||
           Kind == TEXT("gameplay_tag") || Kind == TEXT("gameplay_tags") ||
           Kind == TEXT("curve") || Kind == TEXT("instanced_struct") ||
           Kind == TEXT("property_bag")) {
    OutType = EPropertyBagPropertyType::Struct;
  } else {
    return Fail(
        OutError,
        FString::Printf(TEXT("unsupported_property_bag_value_kind_%s"), *Kind));
  }

  FString TypePath;
  if (!Type->TryGetStringField(TEXT("type_path"), TypePath)) {
    if (!Type->TryGetStringField(TEXT("enum_path"), TypePath) &&
        !Type->TryGetStringField(TEXT("struct_path"), TypePath)) {
      Type->TryGetStringField(TEXT("allowed_class"), TypePath);
    }
  }
  if (!TypePath.IsEmpty()) {
    OutTypeObject = LoadObject<UObject>(nullptr, *TypePath);
    if (!OutTypeObject) {
      return Fail(
          OutError,
          FString::Printf(TEXT("property_bag_type_not_found_%s"), *TypePath));
    }
  }
  if ((OutType == EPropertyBagPropertyType::Enum ||
       OutType == EPropertyBagPropertyType::Struct ||
       OutType == EPropertyBagPropertyType::Object ||
       OutType == EPropertyBagPropertyType::SoftObject ||
       OutType == EPropertyBagPropertyType::Class ||
       OutType == EPropertyBagPropertyType::SoftClass) &&
      !OutTypeObject) {
    return Fail(OutError, TEXT("property_bag_type_path_required"));
  }
  return true;
}

static bool ParsePropertyBagSchemaEntry(const TSharedPtr<FJsonObject> &Entry,
                                        FPropertyBagPropertyDesc &OutDesc,
                                        FString &OutError) {
  FString Name;
  FString Kind;
  if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("name"), Name) ||
      Name.IsEmpty() || !Entry->TryGetStringField(TEXT("value_kind"), Kind) ||
      Kind.IsEmpty()) {
    return Fail(OutError, TEXT("invalid_property_bag_schema_entry"));
  }

  FPropertyBagContainerTypes Containers;
  TSharedPtr<FJsonObject> Current = Entry;
  FString CurrentKind = Kind;
  while (CurrentKind == TEXT("array") || CurrentKind == TEXT("set")) {
    if (!Containers.Add(CurrentKind == TEXT("array")
                            ? EPropertyBagContainerType::Array
                            : EPropertyBagContainerType::Set)) {
      return Fail(OutError, TEXT("property_bag_container_depth_exceeded"));
    }
    const TSharedPtr<FJsonObject> *ElementType = nullptr;
    if (!Current->TryGetObjectField(TEXT("element_type"), ElementType) ||
        !ElementType || !ElementType->IsValid() ||
        !(*ElementType)->TryGetStringField(TEXT("kind"), CurrentKind)) {
      return Fail(OutError, TEXT("property_bag_element_type_missing"));
    }
    Current = *ElementType;
  }

  EPropertyBagPropertyType ValueType = EPropertyBagPropertyType::None;
  UObject *TypeObject = nullptr;
  TSharedPtr<FJsonObject> TerminalType = Current;
  FString TopLevelTypePath;
  if (Current != Entry &&
      Entry->TryGetStringField(TEXT("type_path"), TopLevelTypePath) &&
      !TopLevelTypePath.IsEmpty()) {
    TerminalType = MakeShared<FJsonObject>();
    TerminalType->Values = Current->Values;
    TerminalType->SetStringField(TEXT("type_path"), TopLevelTypePath);
  }
  if (!ResolvePropertyBagTerminalType(CurrentKind, TerminalType, ValueType,
                                      TypeObject, OutError)) {
    return false;
  }
  OutDesc = FPropertyBagPropertyDesc(FName(*Name), Containers, ValueType,
                                     TypeObject, CPF_Edit);
  return true;
}

static TSharedPtr<FJsonValue>
EncodeStructValueInternal(UScriptStruct *Struct, const void *ValuePtr,
                          UObject *Owner, const FOptions &Options, int32 Depth);

static TSharedPtr<FJsonValue>
EncodeInternal(FProperty *Property, const void *ValuePtr, UObject *Owner,
               const FOptions &Options, int32 Depth, bool bIgnoreFixedArray) {
  if (!Property || !ValuePtr ||
      (Options.MaxDepth >= 0 && Depth > Options.MaxDepth))
    return MakeShared<FJsonValueNull>();
  if (!bIgnoreFixedArray && Property->ArrayDim > 1) {
    TArray<TSharedPtr<FJsonValue>> Items;
    for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
      Items.Add(EncodeInternal(Property,
                               static_cast<const uint8 *>(ValuePtr) +
                                   Index * Property->GetElementSize(),
                               Owner, Options, Depth + 1, true));
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetArrayField(TEXT("items"), Items);
    return MakeObjectValue(Object);
  }
  if (const FBoolProperty *Bool = CastField<FBoolProperty>(Property))
    return MakeLiteral(
        MakeShared<FJsonValueBoolean>(Bool->GetPropertyValue(ValuePtr)));
  if (const UEnum *Enum = GetPropertyEnum(Property);
      Enum && !IsBitmaskProperty(Property)) {
    int64 Raw = 0;
    if (const FEnumProperty *EnumProperty = CastField<FEnumProperty>(Property))
      Raw = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(
          ValuePtr);
    else
      Raw =
          CastFieldChecked<FByteProperty>(Property)->GetPropertyValue(ValuePtr);
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("selected"), Enum->GetNameStringByValue(Raw));
    return MakeObjectValue(Object);
  }
  if (const UEnum *Enum = GetPropertyEnum(Property);
      IsBitmaskProperty(Property) && Enum) {
    const FNumericProperty *Numeric = CastField<FNumericProperty>(Property);
    const uint64 Raw =
        Numeric && IsUnsignedNumericProperty(Numeric)
            ? Numeric->GetUnsignedIntPropertyValue(ValuePtr)
            : static_cast<uint64>(
                  Numeric ? Numeric->GetSignedIntPropertyValue(ValuePtr) : 0);
    TArray<TSharedPtr<FJsonValue>> Selected;
    for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index) {
      const int64 EnumValue = Enum->GetValueByIndex(Index);
      const uint64 Mask =
          UsesEnumValuesAsMaskValues(Property, Enum)
              ? static_cast<uint64>(EnumValue)
              : (EnumValue >= 0 && EnumValue < 64 ? (uint64(1) << EnumValue)
                                                  : 0);
      if (Mask && (Raw & Mask) == Mask)
        Selected.Add(
            MakeShared<FJsonValueString>(Enum->GetNameStringByIndex(Index)));
    }
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetArrayField(TEXT("selected"), Selected);
    return MakeObjectValue(Object);
  }
  if (const FNumericProperty *Numeric = CastField<FNumericProperty>(Property)) {
    if (Numeric->IsFloatingPoint())
      return MakeLiteral(MakeShared<FJsonValueNumber>(
          CanonicalNumber(Numeric->GetFloatingPointPropertyValue(ValuePtr))));
    if (GetValueKindInternal(Property, bIgnoreFixedArray) == TEXT("int64"))
      return MakeLiteral(MakeShared<FJsonValueString>(
          LexToString(Numeric->GetSignedIntPropertyValue(ValuePtr))));
    if (GetValueKindInternal(Property, bIgnoreFixedArray) == TEXT("uint64"))
      return MakeLiteral(MakeShared<FJsonValueString>(
          LexToString(Numeric->GetUnsignedIntPropertyValue(ValuePtr))));
    return MakeLiteral(MakeShared<FJsonValueNumber>(
        IsUnsignedNumericProperty(Numeric)
            ? static_cast<double>(
                  Numeric->GetUnsignedIntPropertyValue(ValuePtr))
            : static_cast<double>(
                  Numeric->GetSignedIntPropertyValue(ValuePtr))));
  }
  if (const FStrProperty *String = CastField<FStrProperty>(Property))
    return MakeLiteral(MakeShared<FJsonValueString>(
        BoundString(String->GetPropertyValue(ValuePtr), Options)));
  if (const FAnsiStrProperty *String = CastField<FAnsiStrProperty>(Property)) {
    const FAnsiString &Source = String->GetPropertyValue(ValuePtr);
    const auto Converted = StringCast<TCHAR>(*Source, Source.Len());
    return MakeLiteral(MakeShared<FJsonValueString>(
        FString(Converted.Length(), Converted.Get())));
  }
  if (const FUtf8StrProperty *String = CastField<FUtf8StrProperty>(Property)) {
    const FUtf8String &Source = String->GetPropertyValue(ValuePtr);
    const auto Converted = StringCast<TCHAR>(*Source, Source.Len());
    return MakeLiteral(MakeShared<FJsonValueString>(
        FString(Converted.Length(), Converted.Get())));
  }
  if (const FNameProperty *Name = CastField<FNameProperty>(Property))
    return MakeLiteral(MakeShared<FJsonValueString>(
        BoundString(Name->GetPropertyValue(ValuePtr).ToString(), Options)));
  if (const FTextProperty *Text = CastField<FTextProperty>(Property)) {
    const FText TextValue = Text->GetPropertyValue(ValuePtr);
    const TOptional<FString> Namespace =
        FTextInspector::GetNamespace(TextValue);
    const TOptional<FString> Key = FTextInspector::GetKey(TextValue);
    const FString *Source = FTextInspector::GetSourceString(TextValue);
    if (Namespace.IsSet() && Key.IsSet() && Source) {
      TSharedPtr<FJsonObject> Localized = MakeShared<FJsonObject>();
      Localized->SetStringField(TEXT("source"), BoundString(*Source, Options));
      Localized->SetStringField(TEXT("namespace"), *Namespace);
      Localized->SetStringField(TEXT("key"), *Key);
      TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
      Object->SetObjectField(TEXT("localized"), Localized);
      return MakeObjectValue(Object);
    }
    return MakeLiteral(MakeShared<FJsonValueString>(
        BoundString(Source ? *Source : TextValue.ToString(), Options)));
  }
  if (const FSoftObjectProperty *Soft =
          CastField<FSoftObjectProperty>(Property)) {
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    const FSoftObjectPath Path =
        Soft->GetPropertyValue(ValuePtr).ToSoftObjectPath();
    if (Path.IsNull())
      Object->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
    else
      Object->SetStringField(TEXT("path"), Path.ToString());
    return MakeObjectValue(Object);
  }
  if (const FObjectPropertyBase *ObjectProperty =
          CastField<FObjectPropertyBase>(Property)) {
    TSharedPtr<FJsonValue> CustomValue;
    if (Options.ObjectReferenceEncoder &&
        Options.ObjectReferenceEncoder(ObjectProperty, ValuePtr, Owner, Depth,
                                       CustomValue)) {
      return CustomValue.IsValid() ? CustomValue : MakeShared<FJsonValueNull>();
    }
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    if (UObject *Referenced = ObjectProperty->GetObjectPropertyValue(ValuePtr))
      Object->SetStringField(TEXT("path"), Referenced->GetPathName());
    else
      Object->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
    return MakeObjectValue(Object);
  }
  if (const FInterfaceProperty *Interface =
          CastField<FInterfaceProperty>(Property)) {
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    if (UObject *Referenced = Interface->GetPropertyValue(ValuePtr).GetObject())
      Object->SetStringField(TEXT("path"), Referenced->GetPathName());
    else
      Object->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
    return MakeObjectValue(Object);
  }
  if (const FStructProperty *Struct = CastField<FStructProperty>(Property)) {
    const FString Kind = StructKind(Struct->Struct);
    if (Kind == TEXT("instanced_struct")) {
      const FInstancedStruct &Instanced =
          *static_cast<const FInstancedStruct *>(ValuePtr);
      TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
      TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
      const UScriptStruct *DynamicStruct = Instanced.GetScriptStruct();
      if (DynamicStruct && Instanced.GetMemory()) {
        Object->SetStringField(TEXT("struct_path"),
                               DynamicStruct->GetPathName());
        for (TFieldIterator<FProperty> It(DynamicStruct,
                                          EFieldIterationFlags::IncludeSuper);
             It; ++It) {
          FProperty *Field = *It;
          if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            continue;
          if (Options.StructFieldFilter &&
              !Options.StructFieldFilter(DynamicStruct, Field))
            continue;
          Fields->SetField(Field->GetName(),
                           EncodeInternal(Field,
                                          Field->ContainerPtrToValuePtr<void>(
                                              Instanced.GetMemory()),
                                          Owner, Options, Depth + 1, false));
        }
      } else {
        Object->SetField(TEXT("struct_path"), MakeShared<FJsonValueNull>());
      }
      Object->SetObjectField(TEXT("fields"), Fields);
      return MakeObjectValue(Object);
    }
    if (Kind == TEXT("property_bag")) {
      return EncodePropertyBag(
          *static_cast<const FInstancedPropertyBag *>(ValuePtr), Owner, Options,
          Depth);
    }
    if (Kind == TEXT("soft_object_ref")) {
      TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
      const FSoftObjectPath &Path =
          *static_cast<const FSoftObjectPath *>(ValuePtr);
      if (Path.IsNull())
        Object->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
      else
        Object->SetStringField(TEXT("path"), Path.ToString());
      return MakeObjectValue(Object);
    }
    if (Kind == TEXT("soft_class_ref")) {
      TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
      const FSoftClassPath &Path =
          *static_cast<const FSoftClassPath *>(ValuePtr);
      if (Path.IsNull())
        Object->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
      else
        Object->SetStringField(TEXT("path"), Path.ToString());
      return MakeObjectValue(Object);
    }
    if (Kind == TEXT("gameplay_tag")) {
      const FGameplayTag &Tag = *static_cast<const FGameplayTag *>(ValuePtr);
      return MakeLiteral(MakeShared<FJsonValueString>(
          Tag.IsValid() ? Tag.ToString() : FString()));
    }
    if (Kind == TEXT("gameplay_tags")) {
      TArray<FGameplayTag> Tags;
      static_cast<const FGameplayTagContainer *>(ValuePtr)->GetGameplayTagArray(
          Tags);
      TArray<TSharedPtr<FJsonValue>> Items;
      for (const FGameplayTag &Tag : Tags)
        Items.Add(MakeShared<FJsonValueString>(Tag.ToString()));
      TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
      Object->SetArrayField(TEXT("items"), Items);
      return MakeObjectValue(Object);
    }
    if (Kind == TEXT("curve")) {
      TSharedPtr<FJsonObject> CurveJson;
      return UeAgentCurveJson::TryBuildPropertyCurveJson(Property, ValuePtr,
                                                         CurveJson) &&
                     CurveJson.IsValid()
                 ? MakeObjectValue(CurveJson)
                 : MakeShared<FJsonValueNull>();
    }
    return EncodeStructValueInternal(Struct->Struct, ValuePtr, Owner, Options,
                                     Depth);
  }
  if (const FArrayProperty *Array = CastField<FArrayProperty>(Property)) {
    FScriptArrayHelper Helper(Array, ValuePtr);
    TArray<TSharedPtr<FJsonValue>> Items;
    const int32 Limit =
        Options.MaxContainerItems > 0
            ? FMath::Min(Helper.Num(), Options.MaxContainerItems)
            : Helper.Num();
    for (int32 Index = 0; Index < Limit; ++Index)
      Items.Add(EncodeInternal(Array->Inner, Helper.GetRawPtr(Index), Owner,
                               Options, Depth + 1, false));
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetArrayField(TEXT("items"), Items);
    if (Limit < Helper.Num()) {
      Object->SetBoolField(TEXT("truncated"), true);
      Object->SetNumberField(TEXT("count"), Helper.Num());
    }
    return MakeObjectValue(Object);
  }
  if (const FSetProperty *Set = CastField<FSetProperty>(Property)) {
    FScriptSetHelper Helper(Set, ValuePtr);
    TArray<TSharedPtr<FJsonValue>> Items;
    for (int32 Index = 0; Index < Helper.GetMaxIndex() &&
                          (Options.MaxContainerItems <= 0 ||
                           Items.Num() < Options.MaxContainerItems);
         ++Index)
      if (Helper.IsValidIndex(Index))
        Items.Add(EncodeInternal(Set->ElementProp, Helper.GetElementPtr(Index),
                                 Owner, Options, Depth + 1, false));
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetArrayField(TEXT("items"), Items);
    if (Items.Num() < Helper.Num()) {
      Object->SetBoolField(TEXT("truncated"), true);
      Object->SetNumberField(TEXT("count"), Helper.Num());
    }
    return MakeObjectValue(Object);
  }
  if (const FMapProperty *Map = CastField<FMapProperty>(Property)) {
    FScriptMapHelper Helper(Map, ValuePtr);
    TArray<TSharedPtr<FJsonValue>> Entries;
    for (int32 Index = 0; Index < Helper.GetMaxIndex() &&
                          (Options.MaxContainerItems <= 0 ||
                           Entries.Num() < Options.MaxContainerItems);
         ++Index) {
      if (!Helper.IsValidIndex(Index))
        continue;
      TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
      Entry->SetField(TEXT("key"),
                      EncodeInternal(Map->KeyProp, Helper.GetKeyPtr(Index),
                                     Owner, Options, Depth + 1, false));
      Entry->SetField(TEXT("value"),
                      EncodeInternal(Map->ValueProp, Helper.GetValuePtr(Index),
                                     Owner, Options, Depth + 1, false));
      Entries.Add(MakeObjectValue(Entry));
    }
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetArrayField(TEXT("entries"), Entries);
    if (Entries.Num() < Helper.Num()) {
      Object->SetBoolField(TEXT("truncated"), true);
      Object->SetNumberField(TEXT("count"), Helper.Num());
    }
    return MakeObjectValue(Object);
  }
  if (const FOptionalProperty *Optional =
          CastField<FOptionalProperty>(Property)) {
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    const bool bSet = Optional->IsSet(ValuePtr);
    Object->SetBoolField(TEXT("is_set"), bSet);
    if (bSet)
      Object->SetField(
          TEXT("value"),
          EncodeInternal(Optional->GetValueProperty(),
                         Optional->GetValuePointerForRead(ValuePtr), Owner,
                         Options, Depth + 1, false));
    return MakeObjectValue(Object);
  }
  return MakeShared<FJsonValueNull>();
}

static TSharedPtr<FJsonValue> EncodeStructValueInternal(UScriptStruct *Struct,
                                                        const void *ValuePtr,
                                                        UObject *Owner,
                                                        const FOptions &Options,
                                                        int32 Depth) {
  if (!Struct || !ValuePtr ||
      (Options.MaxDepth >= 0 && Depth > Options.MaxDepth)) {
    return MakeShared<FJsonValueNull>();
  }

  const FString Kind = StructKind(Struct);
  auto EncodeNamedComponents = [Struct,
                                ValuePtr](const TArray<const TCHAR *> &Names) {
    TArray<TSharedPtr<FJsonValue>> Components;
    for (const TCHAR *Name : Names) {
      double Number = 0.0;
      if (!ReadStructNumber(Struct, ValuePtr, Name, Number)) {
        return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
      }
      Components.Add(MakeShared<FJsonValueNumber>(CanonicalNumber(Number)));
    }
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetArrayField(TEXT("components"), Components);
    return MakeObjectValue(Object);
  };
  if (Kind == TEXT("vector2"))
    return EncodeNamedComponents({TEXT("X"), TEXT("Y")});
  if (Kind == TEXT("vector3"))
    return EncodeNamedComponents({TEXT("X"), TEXT("Y"), TEXT("Z")});
  if (Kind == TEXT("vector4") || Kind == TEXT("quat"))
    return EncodeNamedComponents({TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W")});
  if (Kind == TEXT("rotator"))
    return EncodeNamedComponents({TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll")});
  if (Kind == TEXT("color8") || Kind == TEXT("linear_color"))
    return EncodeNamedComponents({TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")});
  if (Kind == TEXT("transform")) {
    const FTransform Transform =
        Struct->GetName() == TEXT("Transform3f")
            ? FTransform(*static_cast<const FTransform3f *>(ValuePtr))
            : *static_cast<const FTransform *>(ValuePtr);
    auto Numbers = [](std::initializer_list<double> Values) {
      TArray<TSharedPtr<FJsonValue>> Result;
      for (double Number : Values)
        Result.Add(MakeShared<FJsonValueNumber>(CanonicalNumber(Number)));
      return Result;
    };
    const FVector Translation = Transform.GetTranslation();
    const FRotator Rotation = Transform.Rotator();
    const FVector Scale = Transform.GetScale3D();
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetArrayField(
        TEXT("translation"),
        Numbers({Translation.X, Translation.Y, Translation.Z}));
    Object->SetArrayField(
        TEXT("rotation"),
        Numbers({Rotation.Pitch, Rotation.Yaw, Rotation.Roll}));
    Object->SetArrayField(TEXT("scale"), Numbers({Scale.X, Scale.Y, Scale.Z}));
    return MakeObjectValue(Object);
  }
  if (Kind == TEXT("guid"))
    return MakeLiteral(MakeShared<FJsonValueString>(
        static_cast<const FGuid *>(ValuePtr)->ToString(
            EGuidFormats::DigitsWithHyphens)));
  if (Kind == TEXT("datetime") || Kind == TEXT("timespan")) {
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    const int64 Ticks =
        Kind == TEXT("datetime")
            ? static_cast<const FDateTime *>(ValuePtr)->GetTicks()
            : static_cast<const FTimespan *>(ValuePtr)->GetTicks();
    Object->SetStringField(TEXT("ticks"), LexToString(Ticks));
    return MakeObjectValue(Object);
  }

  TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
  for (TFieldIterator<FProperty> It(Struct, EFieldIterationFlags::IncludeSuper);
       It; ++It) {
    FProperty *Field = *It;
    if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
      continue;
    if (Options.StructFieldFilter && !Options.StructFieldFilter(Struct, Field))
      continue;
    Fields->SetField(
        Field->GetName(),
        EncodeInternal(Field, Field->ContainerPtrToValuePtr<void>(ValuePtr),
                       Owner, Options, Depth + 1, false));
  }
  TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetObjectField(TEXT("fields"), Fields);
  return MakeObjectValue(Object);
}

TSharedPtr<FJsonValue> Encode(FProperty *Property, const void *ValuePtr,
                              UObject *Owner, const FOptions &Options,
                              int32 Depth) {
  return EncodeInternal(Property, ValuePtr, Owner, Options, Depth, false);
}

TSharedPtr<FJsonValue> EncodeStructValue(UScriptStruct *Struct,
                                         const void *ValuePtr, UObject *Owner,
                                         const FOptions &Options, int32 Depth) {
  return EncodeStructValueInternal(Struct, ValuePtr, Owner, Options, Depth);
}

static bool DecodeStructValueInternal(UScriptStruct *Struct,
                                      const FProperty *ConstraintsProperty,
                                      const TSharedPtr<FJsonValue> &Value,
                                      void *OutValuePtr, UObject *Owner,
                                      const FOptions &Options,
                                      FString &OutError, int32 Depth);

static bool DecodeInternal(FProperty *Property,
                           const TSharedPtr<FJsonValue> &Value,
                           void *OutValuePtr, UObject *Owner,
                           const FOptions &Options, FString &OutError,
                           int32 Depth, bool bIgnoreFixedArray) {
  if (!Property || !OutValuePtr)
    return Fail(OutError, TEXT("invalid_property_or_value_pointer"));
  if (!ValidateDepth(Options, Depth, OutError))
    return false;
  if (!bIgnoreFixedArray && Property->ArrayDim > 1) {
    TSharedPtr<FJsonObject> Object;
    const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
    if (!GetObject(Value, Object, OutError) ||
        !Object->TryGetArrayField(TEXT("items"), Items) || !Items ||
        Items->Num() != Property->ArrayDim)
      return Fail(OutError, TEXT("fixed_array_length_mismatch"));
    if (Object->Values.Num() != 1)
      return Fail(OutError, TEXT("unexpected_fixed_array_fields"));
    for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
      if (!DecodeInternal(Property, (*Items)[Index],
                          static_cast<uint8 *>(OutValuePtr) +
                              Index * Property->GetElementSize(),
                          Owner, Options, OutError, Depth + 1, true))
        return false;
    return true;
  }
  if (const FBoolProperty *Bool = CastField<FBoolProperty>(Property)) {
    TSharedPtr<FJsonValue> Literal;
    if (!GetLiteral(Value, Literal, OutError) ||
        Literal->Type != EJson::Boolean)
      return Fail(OutError, TEXT("expected_boolean_literal"));
    Bool->SetPropertyValue(OutValuePtr, Literal->AsBool());
    return true;
  }
  if (const UEnum *Enum = GetPropertyEnum(Property);
      Enum && !IsBitmaskProperty(Property)) {
    FString Selected;
    if (Value.IsValid() && Value->Type == EJson::String) {
      Selected = Value->AsString();
    } else {
      TSharedPtr<FJsonObject> Object;
      if (!GetObject(Value, Object, OutError) ||
          !Object->TryGetStringField(TEXT("selected"), Selected))
        return Fail(OutError, TEXT("missing_enum_selected"));
      if (Object->Values.Num() != 1)
        return Fail(OutError, TEXT("unexpected_enum_fields"));
    }
    const int64 Raw = Enum->GetValueByNameString(Selected);
    if (Raw == INDEX_NONE)
      return Fail(OutError, TEXT("unknown_enum_value"));
    if (const FEnumProperty *EnumProperty = CastField<FEnumProperty>(Property))
      EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(OutValuePtr,
                                                                 Raw);
    else
      CastFieldChecked<FByteProperty>(Property)->SetPropertyValue(
          OutValuePtr, static_cast<uint8>(Raw));
    return true;
  }
  if (const UEnum *Enum = GetPropertyEnum(Property);
      IsBitmaskProperty(Property) && Enum) {
    TSharedPtr<FJsonObject> Object;
    const TArray<TSharedPtr<FJsonValue>> *Selected = nullptr;
    if (!GetObject(Value, Object, OutError) ||
        !Object->TryGetArrayField(TEXT("selected"), Selected) || !Selected)
      return Fail(OutError, TEXT("missing_bitmask_selected"));
    if (Object->Values.Num() != 1)
      return Fail(OutError, TEXT("unexpected_bitmask_fields"));
    uint64 Raw = 0;
    for (const TSharedPtr<FJsonValue> &Item : *Selected) {
      if (!Item.IsValid() || Item->Type != EJson::String)
        return Fail(OutError, TEXT("invalid_bitmask_item"));
      const int64 EnumValue = Enum->GetValueByNameString(Item->AsString());
      if (EnumValue == INDEX_NONE)
        return Fail(OutError, TEXT("unknown_bitmask_value"));
      Raw |= UsesEnumValuesAsMaskValues(Property, Enum)
                 ? static_cast<uint64>(EnumValue)
                 : (EnumValue >= 0 && EnumValue < 64 ? (uint64(1) << EnumValue)
                                                     : 0);
    }
    FNumericProperty *Numeric = CastFieldChecked<FNumericProperty>(Property);
    Numeric->SetIntPropertyValue(OutValuePtr, Raw);
    return true;
  }
  if (FNumericProperty *Numeric = CastField<FNumericProperty>(Property)) {
    TSharedPtr<FJsonValue> Literal;
    if (!GetLiteral(Value, Literal, OutError))
      return false;
    const FString Kind = GetValueKindInternal(Property, bIgnoreFixedArray);
    if (Numeric->IsFloatingPoint()) {
      double Number = 0.0;
      if (!JsonNumber(Literal, Number, OutError) ||
          !ValidateNumericConstraints(Property, Number, OutError))
        return false;
      if (Kind == TEXT("float") && FMath::Abs(Number) > MAX_flt)
        return Fail(OutError, TEXT("float_out_of_range"));
      Numeric->SetFloatingPointPropertyValue(OutValuePtr, Number);
      return true;
    }
    if (Kind == TEXT("int64")) {
      if (Literal->Type != EJson::String)
        return Fail(OutError, TEXT("int64_literal_must_be_decimal_string"));
      int64 Number = 0;
      if (!LexTryParseString(Number, *Literal->AsString()) ||
          !ValidateSignedIntegerConstraints(Property, Number, OutError))
        return Fail(OutError, OutError.IsEmpty() ? TEXT("invalid_int64_literal")
                                                 : OutError);
      Numeric->SetIntPropertyValue(OutValuePtr, Number);
      return true;
    }
    if (Kind == TEXT("uint64")) {
      if (Literal->Type != EJson::String)
        return Fail(OutError, TEXT("uint64_literal_must_be_decimal_string"));
      uint64 Number = 0;
      if (!LexTryParseString(Number, *Literal->AsString()) ||
          !ValidateUnsignedIntegerConstraints(Property, Number, OutError))
        return Fail(OutError, OutError.IsEmpty()
                                  ? TEXT("invalid_uint64_literal")
                                  : OutError);
      Numeric->SetIntPropertyValue(OutValuePtr, Number);
      return true;
    }
    if (IsUnsignedNumericProperty(Numeric)) {
      uint64 Number = 0;
      const uint64 Max = Kind == TEXT("uint8")    ? MAX_uint8
                         : Kind == TEXT("uint16") ? MAX_uint16
                                                  : MAX_uint32;
      if (!JsonUnsignedInteger(Literal, Max, Number, OutError) ||
          !ValidateNumericConstraints(Property, static_cast<double>(Number),
                                      OutError))
        return false;
      Numeric->SetIntPropertyValue(OutValuePtr, Number);
      return true;
    }
    int64 Number = 0;
    const int64 Min = Kind == TEXT("int8")    ? MIN_int8
                      : Kind == TEXT("int16") ? MIN_int16
                                              : MIN_int32;
    const int64 Max = Kind == TEXT("int8")    ? MAX_int8
                      : Kind == TEXT("int16") ? MAX_int16
                                              : MAX_int32;
    if (!JsonInteger(Literal, Min, Max, Number, OutError) ||
        !ValidateNumericConstraints(Property, static_cast<double>(Number),
                                    OutError))
      return false;
    Numeric->SetIntPropertyValue(OutValuePtr, Number);
    return true;
  }
  if (FStrProperty *String = CastField<FStrProperty>(Property)) {
    TSharedPtr<FJsonValue> Literal;
    if (!GetLiteral(Value, Literal, OutError) || Literal->Type != EJson::String)
      return Fail(OutError, TEXT("expected_string_literal"));
    if (Options.MaxStringChars > 0 &&
        Literal->AsString().Len() > Options.MaxStringChars)
      return Fail(OutError, TEXT("string_too_long"));
    String->SetPropertyValue(OutValuePtr, Literal->AsString());
    return true;
  }
  if (FAnsiStrProperty *String = CastField<FAnsiStrProperty>(Property)) {
    TSharedPtr<FJsonValue> Literal;
    if (!GetLiteral(Value, Literal, OutError) || Literal->Type != EJson::String)
      return Fail(OutError, TEXT("expected_ansi_string_literal"));
    const auto Converted = StringCast<ANSICHAR>(*Literal->AsString());
    String->SetPropertyValue(
        OutValuePtr,
        FAnsiString::ConstructFromPtrSize(Converted.Get(), Converted.Length()));
    return true;
  }
  if (FUtf8StrProperty *String = CastField<FUtf8StrProperty>(Property)) {
    TSharedPtr<FJsonValue> Literal;
    if (!GetLiteral(Value, Literal, OutError) || Literal->Type != EJson::String)
      return Fail(OutError, TEXT("expected_utf8_string_literal"));
    const auto Converted = StringCast<UTF8CHAR>(*Literal->AsString());
    String->SetPropertyValue(
        OutValuePtr,
        FUtf8String::ConstructFromPtrSize(Converted.Get(), Converted.Length()));
    return true;
  }
  if (FNameProperty *Name = CastField<FNameProperty>(Property)) {
    TSharedPtr<FJsonValue> Literal;
    if (!GetLiteral(Value, Literal, OutError) || Literal->Type != EJson::String)
      return Fail(OutError, TEXT("expected_name_literal"));
    Name->SetPropertyValue(OutValuePtr, FName(*Literal->AsString()));
    return true;
  }
  if (FTextProperty *Text = CastField<FTextProperty>(Property)) {
    if (Value.IsValid() && Value->Type == EJson::String) {
      Text->SetPropertyValue(OutValuePtr, FText::FromString(Value->AsString()));
      return true;
    }
    TSharedPtr<FJsonObject> Object;
    if (!GetObject(Value, Object, OutError))
      return false;
    const TSharedPtr<FJsonValue> Literal = Object->TryGetField(TEXT("literal"));
    const TSharedPtr<FJsonObject> *Localized = nullptr;
    if (Literal.IsValid()) {
      if (Object->Values.Num() != 1 || Literal->Type != EJson::String)
        return Fail(OutError, TEXT("expected_text_literal"));
      Text->SetPropertyValue(OutValuePtr,
                             FText::FromString(Literal->AsString()));
      return true;
    }
    if (!Object->TryGetObjectField(TEXT("localized"), Localized) ||
        !Localized || !Localized->IsValid() || Object->Values.Num() != 1 ||
        (*Localized)->Values.Num() != 3) {
      return Fail(OutError, TEXT("expected_text_literal_or_localized"));
    }
    FString Source;
    FString Namespace;
    FString Key;
    if (!(*Localized)->TryGetStringField(TEXT("source"), Source) ||
        !(*Localized)->TryGetStringField(TEXT("namespace"), Namespace) ||
        !(*Localized)->TryGetStringField(TEXT("key"), Key)) {
      return Fail(OutError, TEXT("invalid_localized_text"));
    }
    Text->SetPropertyValue(
        OutValuePtr,
        FText::ChangeKey(Namespace, Key, FText::FromString(Source)));
    return true;
  }
  if (FSoftObjectProperty *Soft = CastField<FSoftObjectProperty>(Property)) {
    TSharedPtr<FJsonValue> PathValue;
    if (!GetReferencePath(Value, PathValue, OutError))
      return false;
    if (PathValue->Type == EJson::Null)
      Soft->SetPropertyValue(OutValuePtr, FSoftObjectPtr());
    else if (PathValue->Type == EJson::String) {
      const FSoftObjectPath Path(PathValue->AsString());
      if (!IsCanonicalSoftReferencePath(Path))
        return Fail(OutError, TEXT("invalid_soft_reference_path"));
      UObject *Referenced = Path.ResolveObject();
      if (Referenced && !Referenced->IsA(Soft->PropertyClass))
        return Fail(OutError, TEXT("reference_wrong_class"));
      if (const FSoftClassProperty *SoftClass =
              CastField<FSoftClassProperty>(Property)) {
        const UClass *ReferencedClass = Cast<UClass>(Referenced);
        if (Referenced && (!ReferencedClass ||
                           (SoftClass->MetaClass &&
                            !ReferencedClass->IsChildOf(SoftClass->MetaClass))))
          return Fail(OutError,
                      TEXT("class_reference_not_child_of_allowed_class"));
      }
      Soft->SetPropertyValue(OutValuePtr, FSoftObjectPtr(Path));
    } else
      return Fail(OutError, TEXT("invalid_reference_path"));
    return true;
  }
  if (FInterfaceProperty *Interface = CastField<FInterfaceProperty>(Property)) {
    TSharedPtr<FJsonValue> PathValue;
    if (!GetReferencePath(Value, PathValue, OutError))
      return false;
    FScriptInterface Result;
    if (PathValue->Type == EJson::String) {
      UObject *Referenced = StaticLoadObject(UObject::StaticClass(), nullptr,
                                             *PathValue->AsString());
      if (!Referenced || !Referenced->GetClass()->ImplementsInterface(
                             Interface->InterfaceClass))
        return Fail(OutError, TEXT("invalid_interface_reference"));
      Result.SetObject(Referenced);
      Result.SetInterface(
          Referenced->GetInterfaceAddress(Interface->InterfaceClass));
    } else if (PathValue->Type != EJson::Null)
      return Fail(OutError, TEXT("invalid_reference_path"));
    Interface->SetPropertyValue(OutValuePtr, Result);
    return true;
  }
  if (FObjectPropertyBase *ObjectProperty =
          CastField<FObjectPropertyBase>(Property)) {
    TSharedPtr<FJsonValue> PathValue;
    if (!GetReferencePath(Value, PathValue, OutError))
      return false;
    UObject *Referenced = nullptr;
    if (PathValue->Type == EJson::String) {
      Referenced = FindObject<UObject>(nullptr, *PathValue->AsString());
      if (!Referenced)
        Referenced = StaticLoadObject(ObjectProperty->PropertyClass, nullptr,
                                      *PathValue->AsString());
      Referenced = ResolveLoadedRedirector(Referenced);
      if (!Referenced || !Referenced->IsA(ObjectProperty->PropertyClass))
        return Fail(OutError, TEXT("reference_not_found_or_wrong_class"));
    } else if (PathValue->Type != EJson::Null)
      return Fail(OutError, TEXT("invalid_reference_path"));
    if (const FClassProperty *ClassProperty =
            CastField<FClassProperty>(Property)) {
      const UClass *ReferencedClass = Cast<UClass>(Referenced);
      if (ReferencedClass && ClassProperty->MetaClass &&
          !ReferencedClass->IsChildOf(ClassProperty->MetaClass))
        return Fail(OutError,
                    TEXT("class_reference_not_child_of_allowed_class"));
    }
    ObjectProperty->SetObjectPropertyValue(OutValuePtr, Referenced);
    return true;
  }
  if (FStructProperty *Struct = CastField<FStructProperty>(Property)) {
    const FString Kind = StructKind(Struct->Struct);
    if (Kind == TEXT("instanced_struct")) {
      TSharedPtr<FJsonObject> Object;
      const TSharedPtr<FJsonObject> *Fields = nullptr;
      if (!GetObject(Value, Object, OutError) || Object->Values.Num() != 2 ||
          !Object->TryGetObjectField(TEXT("fields"), Fields) || !Fields ||
          !Fields->IsValid()) {
        return Fail(OutError, TEXT("invalid_instanced_struct_shape"));
      }
      const TSharedPtr<FJsonValue> StructPathValue =
          Object->TryGetField(TEXT("struct_path"));
      if (!StructPathValue.IsValid())
        return Fail(OutError, TEXT("missing_instanced_struct_path"));
      FInstancedStruct &Instanced =
          *static_cast<FInstancedStruct *>(OutValuePtr);
      if (StructPathValue->Type == EJson::Null) {
        if (!(*Fields)->Values.IsEmpty())
          return Fail(OutError, TEXT("unset_instanced_struct_has_fields"));
        Instanced.Reset();
        return true;
      }
      if (StructPathValue->Type != EJson::String)
        return Fail(OutError, TEXT("invalid_instanced_struct_path"));
      UScriptStruct *DynamicStruct =
          LoadObject<UScriptStruct>(nullptr, *StructPathValue->AsString());
      if (!DynamicStruct)
        return Fail(OutError, TEXT("instanced_struct_type_not_found"));
      for (TFieldIterator<FProperty> It(DynamicStruct,
                                        EFieldIterationFlags::IncludeSuper);
           It; ++It) {
        FProperty *Field = *It;
        if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
          continue;
        if (!IsSupported(Field, OutError))
          return false;
      }
      FInstancedStruct Decoded;
      Decoded.InitializeAs(DynamicStruct);
      TSet<FString> Expected;
      for (TFieldIterator<FProperty> It(DynamicStruct,
                                        EFieldIterationFlags::IncludeSuper);
           It; ++It) {
        FProperty *Field = *It;
        if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
          continue;
        Expected.Add(Field->GetName());
        const TSharedPtr<FJsonValue> FieldValue =
            (*Fields)->TryGetField(Field->GetName());
        if (!FieldValue.IsValid())
          return Fail(OutError,
                      FString::Printf(TEXT("missing_instanced_struct_field_%s"),
                                      *Field->GetName()));
        if (!DecodeInternal(
                Field, FieldValue,
                Field->ContainerPtrToValuePtr<void>(Decoded.GetMutableMemory()),
                Owner, Options, OutError, Depth + 1, false)) {
          OutError = Field->GetName() + TEXT(":") + OutError;
          return false;
        }
      }
      for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
           (*Fields)->Values) {
        if (!Expected.Contains(Pair.Key))
          return Fail(OutError,
                      FString::Printf(TEXT("unknown_instanced_struct_field_%s"),
                                      *Pair.Key));
      }
      Instanced = MoveTemp(Decoded);
      return true;
    }
    if (Kind == TEXT("property_bag")) {
      return DecodePropertyBag(
          Value, *static_cast<FInstancedPropertyBag *>(OutValuePtr), Owner,
          Options, OutError, Depth);
    }
    if (Kind == TEXT("soft_object_ref") || Kind == TEXT("soft_class_ref")) {
      TSharedPtr<FJsonObject> Object;
      if (!GetObject(Value, Object, OutError) || Object->Values.Num() != 1)
        return Fail(OutError, TEXT("invalid_soft_reference_shape"));
      const TSharedPtr<FJsonValue> PathValue =
          Object->TryGetField(TEXT("path"));
      if (!PathValue.IsValid())
        return Fail(OutError, TEXT("missing_reference_path"));
      FString Path;
      if (PathValue->Type == EJson::String) {
        Path = PathValue->AsString();
        if (!IsCanonicalSoftReferencePath(FSoftObjectPath(Path)))
          return Fail(OutError, TEXT("invalid_soft_reference_path"));
      } else if (PathValue->Type != EJson::Null)
        return Fail(OutError, TEXT("invalid_reference_path"));
      if (Kind == TEXT("soft_class_ref"))
        *static_cast<FSoftClassPath *>(OutValuePtr) = FSoftClassPath(Path);
      else
        *static_cast<FSoftObjectPath *>(OutValuePtr) = FSoftObjectPath(Path);
      return true;
    }
    if (Kind == TEXT("gameplay_tag")) {
      TSharedPtr<FJsonValue> Literal;
      if (!GetLiteral(Value, Literal, OutError) ||
          Literal->Type != EJson::String)
        return Fail(OutError, TEXT("expected_gameplay_tag_literal"));
      const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(
          FName(*Literal->AsString()), false);
      if (!Tag.IsValid() && !Literal->AsString().IsEmpty())
        return Fail(OutError, TEXT("unknown_gameplay_tag"));
      *static_cast<FGameplayTag *>(OutValuePtr) = Tag;
      return true;
    }
    if (Kind == TEXT("gameplay_tags")) {
      TSharedPtr<FJsonObject> Object;
      const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
      if (!GetObject(Value, Object, OutError) || Object->Values.Num() != 1 ||
          !Object->TryGetArrayField(TEXT("items"), Items) || !Items ||
          !ValidateContainerCount(Options, Items->Num(), OutError))
        return Fail(OutError, TEXT("invalid_gameplay_tags_shape"));
      FGameplayTagContainer Result;
      for (const TSharedPtr<FJsonValue> &Item : *Items) {
        if (!Item.IsValid() || Item->Type != EJson::String)
          return Fail(OutError, TEXT("invalid_gameplay_tag_item"));
        const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(
            FName(*Item->AsString()), false);
        if (!Tag.IsValid())
          return Fail(OutError, TEXT("unknown_gameplay_tag"));
        Result.AddTag(Tag);
      }
      if (Result.Num() != Items->Num())
        return Fail(OutError, TEXT("duplicate_gameplay_tag"));
      *static_cast<FGameplayTagContainer *>(OutValuePtr) = MoveTemp(Result);
      return true;
    }
    if (Kind == TEXT("curve")) {
      TSharedPtr<FJsonObject> Object;
      if (!GetObject(Value, Object, OutError))
        return false;
      TArray<TSharedPtr<FJsonValue>> Issues;
      if (!UeAgentCurveJson::TryApplyPropertyCurveJson(
              Property, OutValuePtr, Object, TEXT("value"), Issues) ||
          UeAgentCurveJson::HasError(Issues))
        return Fail(OutError, TEXT("invalid_curve_value"));
      return true;
    }
    return DecodeStructValueInternal(Struct->Struct, Struct, Value, OutValuePtr,
                                     Owner, Options, OutError, Depth);
  }
  if (FArrayProperty *Array = CastField<FArrayProperty>(Property)) {
    TSharedPtr<FJsonObject> Object;
    const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
    if (!GetObject(Value, Object, OutError) ||
        !RejectTruncatedObject(Object, OutError) ||
        !Object->TryGetArrayField(TEXT("items"), Items) || !Items ||
        !ValidateContainerCount(Options, Items->Num(), OutError))
      return false;
    if (Object->Values.Num() != 1)
      return Fail(OutError, TEXT("unexpected_array_fields"));
    FScriptArrayHelper Helper(Array, OutValuePtr);
    Helper.EmptyAndAddValues(Items->Num());
    for (int32 Index = 0; Index < Items->Num(); ++Index)
      if (!DecodeInternal(Array->Inner, (*Items)[Index],
                          Helper.GetRawPtr(Index), Owner, Options, OutError,
                          Depth + 1, false)) {
        OutError = FString::Printf(TEXT("item_%d:"), Index) + OutError;
        return false;
      }
    return true;
  }
  if (FSetProperty *Set = CastField<FSetProperty>(Property)) {
    TSharedPtr<FJsonObject> Object;
    const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
    if (!GetObject(Value, Object, OutError) ||
        !RejectTruncatedObject(Object, OutError) ||
        !Object->TryGetArrayField(TEXT("items"), Items) || !Items ||
        !ValidateContainerCount(Options, Items->Num(), OutError))
      return false;
    if (Object->Values.Num() != 1)
      return Fail(OutError, TEXT("unexpected_set_fields"));
    FScriptSetHelper Helper(Set, OutValuePtr);
    Helper.EmptyElements(Items->Num());
    for (int32 Index = 0; Index < Items->Num(); ++Index) {
      const int32 Internal = Helper.AddDefaultValue_Invalid_NeedsRehash();
      void *Element = Helper.GetElementPtr(Internal);
      if (!DecodeInternal(Set->ElementProp, (*Items)[Index], Element, Owner,
                          Options, OutError, Depth + 1, false)) {
        OutError = FString::Printf(TEXT("item_%d:"), Index) + OutError;
        return false;
      }
      for (int32 Existing = 0; Existing < Internal; ++Existing) {
        if (Helper.IsValidIndex(Existing) &&
            Set->ElementProp->Identical(Helper.GetElementPtr(Existing), Element,
                                        PPF_None)) {
          return Fail(OutError, TEXT("duplicate_set_item"));
        }
      }
    }
    Helper.Rehash();
    return true;
  }
  if (FMapProperty *Map = CastField<FMapProperty>(Property)) {
    TSharedPtr<FJsonObject> Object;
    const TArray<TSharedPtr<FJsonValue>> *Entries = nullptr;
    if (!GetObject(Value, Object, OutError) ||
        !RejectTruncatedObject(Object, OutError) ||
        !Object->TryGetArrayField(TEXT("entries"), Entries) || !Entries ||
        !ValidateContainerCount(Options, Entries->Num(), OutError))
      return false;
    if (Object->Values.Num() != 1)
      return Fail(OutError, TEXT("unexpected_map_fields"));
    FScriptMapHelper Helper(Map, OutValuePtr);
    Helper.EmptyValues(Entries->Num());
    for (int32 Index = 0; Index < Entries->Num(); ++Index) {
      TSharedPtr<FJsonObject> Entry;
      if (!GetObject((*Entries)[Index], Entry, OutError) ||
          Entry->Values.Num() != 2) {
        return Fail(OutError, TEXT("invalid_map_entry"));
      }
      const TSharedPtr<FJsonValue> Key = Entry->TryGetField(TEXT("key"));
      const TSharedPtr<FJsonValue> Mapped = Entry->TryGetField(TEXT("value"));
      if (!Key.IsValid() || !Mapped.IsValid()) {
        return Fail(OutError, TEXT("invalid_map_entry"));
      }
      const int32 Internal = Helper.AddDefaultValue_Invalid_NeedsRehash();
      void *KeyPtr = Helper.GetKeyPtr(Internal);
      if (!DecodeInternal(Map->KeyProp, Key, KeyPtr, Owner, Options, OutError,
                          Depth + 1, false) ||
          !DecodeInternal(Map->ValueProp, Mapped, Helper.GetValuePtr(Internal),
                          Owner, Options, OutError, Depth + 1, false)) {
        OutError = FString::Printf(TEXT("entry_%d:"), Index) + OutError;
        return false;
      }
      for (int32 Existing = 0; Existing < Internal; ++Existing) {
        if (Helper.IsValidIndex(Existing) &&
            Map->KeyProp->Identical(Helper.GetKeyPtr(Existing), KeyPtr,
                                    PPF_None)) {
          return Fail(OutError, TEXT("duplicate_map_key"));
        }
      }
    }
    Helper.Rehash();
    return true;
  }
  if (FOptionalProperty *Optional = CastField<FOptionalProperty>(Property)) {
    TSharedPtr<FJsonObject> Object;
    bool bSet = false;
    if (!GetObject(Value, Object, OutError) ||
        !Object->TryGetBoolField(TEXT("is_set"), bSet))
      return Fail(OutError, TEXT("missing_optional_is_set"));
    if (!bSet) {
      if (Object->Values.Num() != 1)
        return Fail(OutError, TEXT("unexpected_optional_fields"));
      Optional->MarkUnset(OutValuePtr);
      return true;
    }
    if (Object->Values.Num() != 2)
      return Fail(OutError, TEXT("unexpected_optional_fields"));
    const TSharedPtr<FJsonValue> Inner = Object->TryGetField(TEXT("value"));
    if (!Inner.IsValid())
      return Fail(OutError, TEXT("missing_optional_value"));
    return DecodeInternal(
        Optional->GetValueProperty(), Inner,
        Optional->MarkSetAndGetInitializedValuePointerToReplace(OutValuePtr),
        Owner, Options, OutError, Depth + 1, false);
  }
  return Fail(OutError, TEXT("unsupported_property_type"));
}

static bool DecodeStructValueInternal(UScriptStruct *Struct,
                                      const FProperty *ConstraintsProperty,
                                      const TSharedPtr<FJsonValue> &Value,
                                      void *OutValuePtr, UObject *Owner,
                                      const FOptions &Options,
                                      FString &OutError, int32 Depth) {
  if (!Struct || !OutValuePtr)
    return Fail(OutError, TEXT("invalid_struct_target"));
  if (!ValidateDepth(Options, Depth, OutError))
    return false;

  bool bHandled = false;
  if (!DecodeKnownStructValue(Struct, ConstraintsProperty, Value, OutValuePtr,
                              OutError, bHandled))
    return false;
  if (bHandled)
    return true;

  TSharedPtr<FJsonObject> Object;
  const TSharedPtr<FJsonObject> *Fields = nullptr;
  if (!GetObject(Value, Object, OutError) || Object->Values.Num() != 1 ||
      !Object->TryGetObjectField(TEXT("fields"), Fields) || !Fields ||
      !Fields->IsValid()) {
    return Fail(OutError, TEXT("missing_struct_fields"));
  }

  TSet<FString> Expected;
  for (TFieldIterator<FProperty> It(Struct, EFieldIterationFlags::IncludeSuper);
       It; ++It) {
    FProperty *Field = *It;
    if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
      continue;
    if (!IsSupported(Field, OutError)) {
      OutError = Field->GetName() + TEXT(":") + OutError;
      return false;
    }
    Expected.Add(Field->GetName());
    const TSharedPtr<FJsonValue> FieldValue =
        (*Fields)->TryGetField(Field->GetName());
    if (!FieldValue.IsValid()) {
      if (Options.bAllowPartialStructFields)
        continue;
      return Fail(OutError, FString::Printf(TEXT("missing_struct_field_%s"),
                                            *Field->GetName()));
    }
    if (!DecodeInternal(Field, FieldValue,
                        Field->ContainerPtrToValuePtr<void>(OutValuePtr), Owner,
                        Options, OutError, Depth + 1, false)) {
      OutError = Field->GetName() + TEXT(":") + OutError;
      return false;
    }
  }
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : (*Fields)->Values) {
    if (!Expected.Contains(Pair.Key))
      return Fail(OutError,
                  FString::Printf(TEXT("unknown_struct_field_%s"), *Pair.Key));
  }
  return true;
}

bool Decode(FProperty *Property, const TSharedPtr<FJsonValue> &Value,
            void *OutValuePtr, UObject *Owner, const FOptions &Options,
            FString &OutError, int32 Depth) {
  return DecodeInternal(Property, Value, OutValuePtr, Owner, Options, OutError,
                        Depth, false);
}

bool AreSemanticallyEquivalent(FProperty *Property, const void *LeftValuePtr,
                               const void *RightValuePtr, UObject *Owner,
                               const FOptions &Options) {
  if (!Property || !LeftValuePtr || !RightValuePtr)
    return false;
  if (Property->Identical(LeftValuePtr, RightValuePtr, PPF_None))
    return true;
  if (Property->ArrayDim == 1) {
    if (const FFloatProperty *Float = CastField<FFloatProperty>(Property)) {
      return FMath::IsNearlyEqual(Float->GetPropertyValue(LeftValuePtr),
                                  Float->GetPropertyValue(RightValuePtr));
    }
    if (const FDoubleProperty *Double = CastField<FDoubleProperty>(Property)) {
      return FMath::IsNearlyEqual(Double->GetPropertyValue(LeftValuePtr),
                                  Double->GetPropertyValue(RightValuePtr));
    }
    if (const FStructProperty *Struct = CastField<FStructProperty>(Property)) {
      const FString Name =
          Struct->Struct ? Struct->Struct->GetName() : FString();
      if (Name == TEXT("Vector")) {
        return static_cast<const FVector *>(LeftValuePtr)
            ->Equals(*static_cast<const FVector *>(RightValuePtr));
      }
      if (Name == TEXT("Vector3f")) {
        return static_cast<const FVector3f *>(LeftValuePtr)
            ->Equals(*static_cast<const FVector3f *>(RightValuePtr));
      }
      if (Name == TEXT("Rotator")) {
        const FQuat Left =
            static_cast<const FRotator *>(LeftValuePtr)->Quaternion();
        const FQuat Right =
            static_cast<const FRotator *>(RightValuePtr)->Quaternion();
        return FMath::Abs(Left | Right) >= 1.0 - KINDA_SMALL_NUMBER;
      }
      if (Name == TEXT("Rotator3f")) {
        const FQuat4f Left =
            static_cast<const FRotator3f *>(LeftValuePtr)->Quaternion();
        const FQuat4f Right =
            static_cast<const FRotator3f *>(RightValuePtr)->Quaternion();
        return FMath::Abs(Left | Right) >= 1.0f - KINDA_SMALL_NUMBER;
      }
      if (Name == TEXT("Quat")) {
        return FMath::Abs(*static_cast<const FQuat *>(LeftValuePtr) |
                          *static_cast<const FQuat *>(RightValuePtr)) >=
               1.0 - KINDA_SMALL_NUMBER;
      }
      if (Name == TEXT("Quat4f")) {
        return FMath::Abs(*static_cast<const FQuat4f *>(LeftValuePtr) |
                          *static_cast<const FQuat4f *>(RightValuePtr)) >=
               1.0f - KINDA_SMALL_NUMBER;
      }
      if (Name == TEXT("Transform")) {
        const FTransform &Left = *static_cast<const FTransform *>(LeftValuePtr);
        const FTransform &Right =
            *static_cast<const FTransform *>(RightValuePtr);
        return Left.GetTranslation().Equals(Right.GetTranslation()) &&
               Left.GetScale3D().Equals(Right.GetScale3D()) &&
               FMath::Abs(Left.GetRotation() | Right.GetRotation()) >=
                   1.0 - KINDA_SMALL_NUMBER;
      }
      if (Name == TEXT("Transform3f")) {
        const FTransform3f &Left =
            *static_cast<const FTransform3f *>(LeftValuePtr);
        const FTransform3f &Right =
            *static_cast<const FTransform3f *>(RightValuePtr);
        return Left.GetTranslation().Equals(Right.GetTranslation()) &&
               Left.GetScale3D().Equals(Right.GetScale3D()) &&
               FMath::Abs(Left.GetRotation() | Right.GetRotation()) >=
                   1.0f - KINDA_SMALL_NUMBER;
      }
    }
  }
  const TSharedPtr<FJsonValue> Left =
      Encode(Property, LeftValuePtr, Owner, Options);
  const TSharedPtr<FJsonValue> Right =
      Encode(Property, RightValuePtr, Owner, Options);
  return IsReliableCanonicalValue(Property, Left) &&
         IsReliableCanonicalValue(Property, Right) &&
         JsonEquivalent(Left, Right);
}

bool DecodeStructValue(UScriptStruct *Struct,
                       const TSharedPtr<FJsonValue> &Value, void *OutValuePtr,
                       FString &OutError) {
  return DecodeStructValueInternal(Struct, nullptr, Value, OutValuePtr, nullptr,
                                   FOptions(), OutError, 0);
}

bool DecodeStructValue(UScriptStruct *Struct,
                       const TSharedPtr<FJsonValue> &Value, void *OutValuePtr,
                       UObject *Owner, const FOptions &Options,
                       FString &OutError, int32 Depth) {
  return DecodeStructValueInternal(Struct, nullptr, Value, OutValuePtr, Owner,
                                   Options, OutError, Depth);
}

TSharedPtr<FJsonValue> EncodePropertyBag(const FInstancedPropertyBag &Bag,
                                         UObject *Owner,
                                         const FOptions &Options, int32 Depth) {
  TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> Schema;
  TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
  const FInstancedStruct *Storage = GetPropertyBagStorage(&Bag);
  if (Bag.IsValid() && Bag.GetPropertyBagStruct() && Storage &&
      Storage->GetMemory()) {
    for (const FPropertyBagPropertyDesc &Desc :
         Bag.GetPropertyBagStruct()->GetPropertyDescs()) {
      if (!Desc.CachedProperty)
        continue;
      Schema.Add(MakeObjectValue(BuildPropertyBagSchemaEntry(Desc)));
      FProperty *Field = const_cast<FProperty *>(Desc.CachedProperty);
      Fields->SetField(
          Desc.Name.ToString(),
          EncodeInternal(
              Field, Field->ContainerPtrToValuePtr<void>(Storage->GetMemory()),
              Owner, Options, Depth + 1, false));
    }
  }
  Object->SetArrayField(TEXT("schema"), Schema);
  Object->SetObjectField(TEXT("fields"), Fields);
  return MakeObjectValue(Object);
}

bool DecodePropertyBag(const TSharedPtr<FJsonValue> &Value,
                       FInstancedPropertyBag &OutBag, UObject *Owner,
                       const FOptions &Options, FString &OutError,
                       int32 Depth) {
  TSharedPtr<FJsonObject> Object;
  const TArray<TSharedPtr<FJsonValue>> *Schema = nullptr;
  const TSharedPtr<FJsonObject> *Fields = nullptr;
  if (!ValidateDepth(Options, Depth, OutError) ||
      !GetObject(Value, Object, OutError) || Object->Values.Num() != 2 ||
      !Object->TryGetArrayField(TEXT("schema"), Schema) || !Schema ||
      !Object->TryGetObjectField(TEXT("fields"), Fields) || !Fields ||
      !Fields->IsValid() ||
      !ValidateContainerCount(Options, Schema->Num(), OutError)) {
    return Fail(OutError, TEXT("invalid_property_bag_shape"));
  }
  TArray<FPropertyBagPropertyDesc> Descs;
  TSet<FName> Names;
  for (const TSharedPtr<FJsonValue> &SchemaValue : *Schema) {
    TSharedPtr<FJsonObject> Entry;
    if (!GetObject(SchemaValue, Entry, OutError))
      return false;
    FPropertyBagPropertyDesc Desc;
    if (!ParsePropertyBagSchemaEntry(Entry, Desc, OutError))
      return false;
    if (Names.Contains(Desc.Name))
      return Fail(OutError, TEXT("duplicate_property_bag_field"));
    Names.Add(Desc.Name);
    Descs.Add(MoveTemp(Desc));
  }
  if ((*Fields)->Values.Num() != Names.Num())
    return Fail(OutError, TEXT("property_bag_schema_fields_mismatch"));
  FInstancedPropertyBag Decoded;
  if (!Descs.IsEmpty() &&
      Decoded.AddProperties(Descs) != EPropertyBagAlterationResult::Success) {
    return Fail(OutError, TEXT("property_bag_schema_creation_failed"));
  }
  FInstancedStruct *Storage = GetMutablePropertyBagStorage(&Decoded);
  if (!Descs.IsEmpty() && (!Decoded.GetPropertyBagStruct() || !Storage ||
                           !Storage->GetMutableMemory())) {
    return Fail(OutError, TEXT("property_bag_storage_unavailable"));
  }
  if (Decoded.GetPropertyBagStruct()) {
    for (const FPropertyBagPropertyDesc &Desc :
         Decoded.GetPropertyBagStruct()->GetPropertyDescs()) {
      const TSharedPtr<FJsonValue> FieldValue =
          (*Fields)->TryGetField(Desc.Name.ToString());
      if (!FieldValue.IsValid())
        return Fail(OutError,
                    FString::Printf(TEXT("missing_property_bag_field_%s"),
                                    *Desc.Name.ToString()));
      FProperty *Field = const_cast<FProperty *>(Desc.CachedProperty);
      if (!Field ||
          !DecodeInternal(
              Field, FieldValue,
              Field->ContainerPtrToValuePtr<void>(Storage->GetMutableMemory()),
              Owner, Options, OutError, Depth + 1, false)) {
        OutError = Desc.Name.ToString() + TEXT(":") + OutError;
        return false;
      }
    }
  }
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : (*Fields)->Values) {
    if (!Names.Contains(FName(*Pair.Key)))
      return Fail(
          OutError,
          FString::Printf(TEXT("unknown_property_bag_field_%s"), *Pair.Key));
  }
  OutBag = MoveTemp(Decoded);
  return true;
}

bool IsReliableCanonicalValue(const FProperty *Property,
                              const TSharedPtr<FJsonValue> &Value,
                              FString *OutReason) {
  FString Error;
  if (!Property || !Value.IsValid() || Value->Type == EJson::Null ||
      GetValueKind(Property) == TEXT("unavailable"))
    Error = TEXT("canonical_value_unavailable");
  else if (Value->Type != EJson::Object)
    Error = TEXT("canonical_value_must_be_object");
  else if (IsTruncatedValue(Value))
    Error = TEXT("canonical_value_truncated_or_incomplete");
  if (OutReason)
    *OutReason = Error;
  return Error.IsEmpty();
}
} // namespace UeAgentPropertyValueCodec
